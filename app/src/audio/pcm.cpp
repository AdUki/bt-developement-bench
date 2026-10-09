#include "audio/pcm.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "util/strings.h"

namespace btb::audio {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kFloorDb = -120.0;

inline int16_t clip16(double v) {
  if (v >= 32767.0) return 32767;
  if (v <= -32768.0) return -32768;
  return static_cast<int16_t>(std::lrint(v));
}

void put_le16(std::string* s, uint16_t v) {
  s->push_back(static_cast<char>(v & 0xff));
  s->push_back(static_cast<char>(v >> 8));
}
void put_le32(std::string* s, uint32_t v) {
  put_le16(s, static_cast<uint16_t>(v & 0xffff));
  put_le16(s, static_cast<uint16_t>(v >> 16));
}

}  // namespace

double db_to_gain(double db) { return db <= kFloorDb ? 0.0 : std::pow(10.0, db / 20.0); }
double gain_to_db(double g) { return g <= 1e-6 ? kFloorDb : std::max(kFloorDb, 20.0 * std::log10(g)); }

// ---- tone ---------------------------------------------------------------------------------------

ToneGen::ToneGen(int rate, int channels, double freq, double freq_right, double level_db)
    : rate_(rate), ch_(std::clamp(channels, 1, kMaxChannels)) {
  set(freq, freq_right, level_db);
}

void ToneGen::set(double freq, double freq_right, double level_db) {
  const double f[kMaxChannels] = {freq, freq_right > 0 ? freq_right : freq};
  for (int c = 0; c < kMaxChannels; ++c) {
    const double w = kTwoPi * f[c] / rate_;
    cos_[c] = std::cos(w);
    sin_[c] = std::sin(w);
  }
  amp_ = 32767.0 * db_to_gain(level_db);
}

void ToneGen::fill(int16_t* out, size_t frames) {
  for (size_t i = 0; i < frames; ++i) {
    for (int c = 0; c < ch_; ++c) {
      *out++ = clip16(amp_ * im_[c]);
      const double re = re_[c] * cos_[c] - im_[c] * sin_[c];
      im_[c] = re_[c] * sin_[c] + im_[c] * cos_[c];
      re_[c] = re;
    }
  }
  // Rounding makes the phasor's length drift a little every step; pull it back to 1 once a chunk.
  for (int c = 0; c < ch_; ++c) {
    const double m = std::sqrt(re_[c] * re_[c] + im_[c] * im_[c]);
    if (m > 0) {
      re_[c] /= m;
      im_[c] /= m;
    }
  }
}

// ---- sweep --------------------------------------------------------------------------------------

SweepGen::SweepGen(int rate, int channels, double from, double to, double seconds, bool log,
                   bool repeat, double level_db)
    : rate_(rate),
      ch_(std::clamp(channels, 1, kMaxChannels)),
      from_(from),
      to_(to),
      seconds_(std::max(0.1, seconds)),
      log_(log),
      repeat_(repeat),
      amp_(32767.0 * db_to_gain(level_db)) {}

void SweepGen::fill(int16_t* out, size_t frames) {
  const uint64_t len = static_cast<uint64_t>(seconds_ * rate_);
  for (size_t i = 0; i < frames; ++i) {
    if (n_ >= len) {
      if (!repeat_) {
        done_ = true;
        std::fill(out, out + static_cast<size_t>(ch_) * (frames - i), int16_t{0});
        return;
      }
      n_ = 0;
    }
    const double x = static_cast<double>(n_) / static_cast<double>(len);
    const double f = log_ ? from_ * std::pow(to_ / from_, x) : from_ + (to_ - from_) * x;
    // The phase is accumulated from the instantaneous frequency, so it is continuous however the
    // frequency moves (and across the restart of a repeat).
    phase_ += kTwoPi * f / rate_;
    if (phase_ > kTwoPi) phase_ -= kTwoPi;
    const int16_t v = clip16(amp_ * std::sin(phase_));
    for (int c = 0; c < ch_; ++c) *out++ = v;
    ++n_;
  }
}

// ---- noise --------------------------------------------------------------------------------------

bool noise_color(const std::string& name, NoiseGen::Color* out) {
  if (name == "white") *out = NoiseGen::Color::White;
  else if (name == "pink") *out = NoiseGen::Color::Pink;
  else if (name == "brown" || name == "red") *out = NoiseGen::Color::Brown;
  else return false;
  return true;
}

NoiseGen::NoiseGen(int channels, Color color, double level_db, uint32_t seed)
    : ch_(std::clamp(channels, 1, kMaxChannels)), color_(color), rng_(seed ? seed : 1) {
  set_level(level_db);
}

void NoiseGen::set_level(double level_db) {
  // Uniform white noise in [-1, 1) has an RMS of 1/sqrt(3). The filtered colours are scaled by
  // their own measured RMS (the constants below), so `level_db` is the RMS for each of them.
  double unit_rms = 0.57735;
  if (color_ == Color::Pink) unit_rms = 0.1907;
  if (color_ == Color::Brown) unit_rms = 0.1764;
  rms_ = 32767.0 * db_to_gain(level_db) / unit_rms;
}

float NoiseGen::next_white() {
  // xorshift32: plenty for audio, and no locks or state shared with anything else.
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<float>(static_cast<int32_t>(rng_)) * (1.0f / 2147483648.0f);
}

void NoiseGen::fill(int16_t* out, size_t frames) {
  for (size_t i = 0; i < frames; ++i) {
    for (int c = 0; c < ch_; ++c) {
      const float w = next_white();
      float v = w;
      if (color_ == Color::Pink) {
        float* b = pink_[c];
        b[0] = 0.99886f * b[0] + w * 0.0555179f;
        b[1] = 0.99332f * b[1] + w * 0.0750759f;
        b[2] = 0.96900f * b[2] + w * 0.1538520f;
        b[3] = 0.86650f * b[3] + w * 0.3104856f;
        b[4] = 0.55000f * b[4] + w * 0.5329522f;
        b[5] = -0.7616f * b[5] - w * 0.0168980f;
        v = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f) * 0.11f;
        b[6] = w * 0.115926f;
      } else if (color_ == Color::Brown) {
        brown_[c] = 0.998f * brown_[c] + 0.02f * w;
        v = brown_[c];
      }
      *out++ = clip16(rms_ * v);
    }
  }
}

void SilenceGen::fill(int16_t* out, size_t frames) {
  std::memset(out, 0, frames * static_cast<size_t>(ch_) * sizeof(int16_t));
}

// ---- gain, downmix, meter -----------------------------------------------------------------------

void apply_gain(int16_t* s, size_t samples, double gain) {
  if (gain == 1.0) return;
  // Q16 fixed point: an integer multiply a sample.
  const int64_t g = static_cast<int64_t>(std::llround(gain * 65536.0));
  for (size_t i = 0; i < samples; ++i) {
    const int64_t v = (static_cast<int64_t>(s[i]) * g) >> 16;
    s[i] = static_cast<int16_t>(std::clamp<int64_t>(v, -32768, 32767));
  }
}

size_t downmix(int16_t* s, size_t frames) {
  for (size_t i = 0; i < frames; ++i)
    s[i] = static_cast<int16_t>((static_cast<int32_t>(s[2 * i]) + s[2 * i + 1]) / 2);
  return frames;
}

void LevelMeter::add(const int16_t* s, size_t frames, int channels) {
  ch_ = std::clamp(channels, 1, kMaxChannels);
  for (size_t i = 0; i < frames; ++i) {
    for (int c = 0; c < ch_; ++c) {
      const int32_t v = *s++;
      const int32_t a = v < 0 ? -v : v;
      if (a > peak_[c]) peak_[c] = a;
      if (a >= 32767) ++clipped_;
      sumsq_[c] += static_cast<double>(v) * v;
    }
  }
  frames_ += frames;
}

LevelMeter::Reading LevelMeter::take() {
  Reading r;
  r.channels = ch_;
  r.frames = frames_;
  r.clipped = clipped_;
  for (int c = 0; c < ch_ && frames_; ++c) {
    r.peak_db[c] = gain_to_db(peak_[c] / 32768.0);
    r.rms_db[c] = gain_to_db(std::sqrt(sumsq_[c] / static_cast<double>(frames_)) / 32768.0);
  }
  *this = LevelMeter{};
  ch_ = r.channels;
  return r;
}

// ---- WAV ----------------------------------------------------------------------------------------

std::string wav_header(int rate, int channels, uint32_t data_bytes) {
  std::string h = "RIFF";
  put_le32(&h, data_bytes == 0xffffffffu ? 0xffffffffu : data_bytes + 36);
  h += "WAVEfmt ";
  put_le32(&h, 16);
  put_le16(&h, 1);  // PCM
  put_le16(&h, static_cast<uint16_t>(channels));
  put_le32(&h, static_cast<uint32_t>(rate));
  put_le32(&h, static_cast<uint32_t>(rate * channels * 2));
  put_le16(&h, static_cast<uint16_t>(channels * 2));
  put_le16(&h, 16);
  h += "data";
  put_le32(&h, data_bytes);
  return h;
}

void WavStripper::feed(const uint8_t* p, size_t n, std::string* out) {
  if (state_ == State::Pass) {
    out->append(reinterpret_cast<const char*>(p), n);
    return;
  }
  head_.append(reinterpret_cast<const char*>(p), n);
  if (state_ == State::Sniff) {
    if (head_.size() < 4) return;
    if (head_.compare(0, 4, "RIFF") != 0) {
      state_ = State::Pass;
      out->append(head_);
      head_.clear();
      return;
    }
    state_ = State::Header;
  }
  // Walk the chunks after "RIFF<len>WAVE" until "data"; its payload is the audio.
  size_t pos = 12;
  while (pos + 8 <= head_.size()) {
    const auto* b = reinterpret_cast<const uint8_t*>(head_.data()) + pos;
    const uint32_t len = static_cast<uint32_t>(b[4] | (b[5] << 8) | (b[6] << 16)) |
                         (static_cast<uint32_t>(b[7]) << 24);
    if (std::memcmp(b, "data", 4) == 0) {
      state_ = State::Pass;
      out->append(head_, pos + 8, std::string::npos);
      head_.clear();
      return;
    }
    pos += 8 + len + (len & 1);
  }
  // A header that never reaches "data" within 4 KiB is not one: give the bytes back as audio
  // rather than swallowing the stream.
  if (head_.size() > 4096) {
    state_ = State::Pass;
    out->append(head_);
    head_.clear();
  }
}

// ---- playlists and stream titles ----------------------------------------------------------------

bool looks_like_playlist(const std::string& s) {
  std::string l = lower(s);
  const size_t q = l.find_first_of("?#");
  const std::string path = q == std::string::npos ? l : l.substr(0, q);
  return ends_with(path, ".pls") || ends_with(path, ".m3u") ||
         l.find("audio/x-scpls") != std::string::npos || l.find("audio/x-mpegurl") != std::string::npos ||
         l.find("audio/mpegurl") != std::string::npos;
}

std::string playlist_first_url(const std::string& body) {
  if (body.find("#EXT-X-") != std::string::npos) return {};
  for (std::string line : split(body, '\n')) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == '[') continue;
    // .pls: File1=http://...
    const size_t eq = line.find('=');
    if (eq != std::string::npos && starts_with(lower(line), "file")) line = trim(line.substr(eq + 1));
    const std::string l = lower(line);
    if (starts_with(l, "http://") || starts_with(l, "https://")) return line;
  }
  return {};
}

bool parse_icy_line(const std::string& raw, std::string* key, std::string* value) {
  const std::string line = trim(raw);
  if (starts_with(line, "ICY-NAME:")) {
    *key = "name";
    *value = trim(line.substr(9));
    return !value->empty();
  }
  if (starts_with(line, "ICY-META:")) {
    const std::string m = line.substr(9);
    const size_t a = m.find("StreamTitle='");
    if (a == std::string::npos) return false;
    const size_t b = m.find("';", a + 13);
    *key = "title";
    *value = trim(m.substr(a + 13, b == std::string::npos ? std::string::npos : b - (a + 13)));
    return true;
  }
  // ffmpeg: "    icy-name        : Groove Salad" / "      StreamTitle     : Artist - Title"
  const size_t colon = line.find(" : ");
  if (colon != std::string::npos) {
    const std::string k = lower(trim(line.substr(0, colon)));
    const std::string v = trim(line.substr(colon + 3));
    if (k == "icy-name") *key = "name";
    else if (k == "streamtitle") *key = "title";
    else return false;
    *value = v;
    return !v.empty();
  }
  return false;
}

bool icy_title_from(const std::string& headers, const std::string& body, std::string* title,
                    size_t* need) {
  size_t metaint = 0;
  for (const std::string& line : split(headers, '\n')) {
    const size_t c = line.find(':');
    if (c != std::string::npos && lower(trim(line.substr(0, c))) == "icy-metaint")
      metaint = static_cast<size_t>(std::strtoul(trim(line.substr(c + 1)).c_str(), nullptr, 10));
  }
  *need = 0;
  if (metaint == 0 || metaint > (1u << 20)) return false;
  if (body.size() <= metaint) {
    *need = metaint + 1;
    return false;
  }
  const size_t len = static_cast<unsigned char>(body[metaint]) * 16u;
  if (body.size() < metaint + 1 + len) {
    *need = metaint + 1 + len;
    return false;
  }
  const std::string meta = body.substr(metaint + 1, len);
  const size_t a = meta.find("StreamTitle='");
  if (a == std::string::npos) return false;
  const size_t b = meta.find("';", a + 13);
  *title = trim(meta.substr(a + 13, b == std::string::npos ? std::string::npos : b - (a + 13)));
  return true;
}

}  // namespace btb::audio
