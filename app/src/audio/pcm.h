#pragma once

// The sample-level pieces of the audio streams (audio/engine.h): test-signal generators, gain, a
// level meter, the WAV header the listen stream starts with, and the small parsers for what a
// decoder or a radio station hands over (playlists, ICY titles). All pure: no processes, no
// threads, unit-tested (tests/test_audio.cpp).
//
// The streams carry one format end to end: signed 16-bit little-endian, interleaved, 1 or 2
// channels, at the stream's rate. It is what every A2DP codec takes, what pw-cat, aplay and the
// decoders speak natively, and on an ARM11 without a usable FPU for doubles in a hot loop it keeps
// the per-sample work to integer adds and multiplies.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace btb::audio {

constexpr int kMaxChannels = 2;

// dBFS ↔ linear amplitude. Anything at or below -120 dB is silence.
double db_to_gain(double db);
double gain_to_db(double g);

// A test signal, written one chunk at a time. Not thread-safe: one stream's pump owns it, and a
// parameter change (PUT /api/audio/streams/{id}) takes effect from the next chunk.
class Generator {
 public:
  virtual ~Generator() = default;
  virtual void fill(int16_t* out, size_t frames) = 0;
};

// A sine per channel (the right one may have its own frequency: a channel-identification tone),
// phase-continuous across chunks and across a frequency change, so changing it live does not click.
// Each channel is a rotating phasor (one complex multiply a sample, renormalised every chunk)
// rather than a sin() call per sample.
class ToneGen : public Generator {
 public:
  ToneGen(int rate, int channels, double freq, double freq_right, double level_db);
  void set(double freq, double freq_right, double level_db);
  void fill(int16_t* out, size_t frames) override;

 private:
  int rate_, ch_;
  double re_[kMaxChannels] = {1, 1}, im_[kMaxChannels] = {0, 0};    // the phasor
  double cos_[kMaxChannels] = {1, 1}, sin_[kMaxChannels] = {0, 0};  // its step
  double amp_ = 0;
};

// A sweep from `from` to `to` Hz over `seconds`, logarithmic (equal time per octave: what a
// frequency response wants) or linear, then again from the start when `repeat`, else silence.
class SweepGen : public Generator {
 public:
  SweepGen(int rate, int channels, double from, double to, double seconds, bool log, bool repeat,
           double level_db);
  void fill(int16_t* out, size_t frames) override;
  bool done() const { return done_; }

 private:
  int rate_, ch_;
  double from_, to_, seconds_;
  bool log_, repeat_;
  double amp_;
  double phase_ = 0;
  uint64_t n_ = 0;
  bool done_ = false;
};

// White, pink (Paul Kellet's economy filter: -3 dB/octave within ±0.5 dB above 40 Hz) or brown
// (integrated white, leaky so it cannot wander off) noise, independent per channel. The level is
// the RMS in dBFS, so -20 dB of pink and -20 dB of white carry the same power.
class NoiseGen : public Generator {
 public:
  enum class Color { White, Pink, Brown };
  NoiseGen(int channels, Color color, double level_db, uint32_t seed = 0x2545f491);
  void set_level(double level_db);
  void fill(int16_t* out, size_t frames) override;

 private:
  float next_white();  // uniform in [-1, 1)
  int ch_;
  Color color_;
  double rms_;
  uint32_t rng_;
  float pink_[kMaxChannels][7] = {};
  float brown_[kMaxChannels] = {};
};

class SilenceGen : public Generator {
 public:
  explicit SilenceGen(int channels) : ch_(channels) {}
  void fill(int16_t* out, size_t frames) override;

 private:
  int ch_;
};

bool noise_color(const std::string& name, NoiseGen::Color* out);

// In place, saturating. A gain of exactly 1 is a no-op (the common case: no multiply at all).
void apply_gain(int16_t* s, size_t samples, double gain);
// Stereo → mono in place (the average of the two); returns the number of mono samples.
size_t downmix(int16_t* s, size_t frames);

// Peak and RMS per channel since the last take(), in dBFS (-120 for silence). The pump adds every
// chunk; the publisher takes a reading four times a second.
class LevelMeter {
 public:
  void add(const int16_t* s, size_t frames, int channels);
  struct Reading {
    int channels = 0;
    double peak_db[kMaxChannels] = {-120, -120};
    double rms_db[kMaxChannels] = {-120, -120};
    uint64_t frames = 0;
    uint64_t clipped = 0;  // samples at full scale
  };
  Reading take();

 private:
  int ch_ = 0;
  int32_t peak_[kMaxChannels] = {0, 0};
  double sumsq_[kMaxChannels] = {0, 0};
  uint64_t frames_ = 0;
  uint64_t clipped_ = 0;
};

// A 44-byte PCM WAV header. `data_bytes` 0xffffffff means "until the stream ends", which is what a
// live stream sends: aplay, ffplay, sox and browsers' WAV readers all accept it.
std::string wav_header(int rate, int channels, uint32_t data_bytes = 0xffffffffu);

// A capture tool's stdout may start with a WAV header (pw-cat writes one to a pipe in some
// versions, raw samples in others). Fed the stream's first bytes, it drops a RIFF header up to and
// including the "data" chunk's header, and passes everything else through untouched.
class WavStripper {
 public:
  // Appends the payload part of [p, p+n) to *out.
  void feed(const uint8_t* p, size_t n, std::string* out);

 private:
  enum class State { Sniff, Header, Pass } state_ = State::Sniff;
  std::string head_;
};

// The first stream URL of a playlist (.pls "FileN=", or .m3u/.m3u8 lines that are not comments),
// or "" when there is none. An HLS playlist (#EXT-X-) is not one of these: it is played as is.
std::string playlist_first_url(const std::string& body);
// Whether a URL or a Content-Type names a playlist we resolve ourselves.
bool looks_like_playlist(const std::string& url_or_type);

// A decoder's stderr lines about an ICY (Shoutcast/Icecast) stream: mpg123's "ICY-NAME: station"
// and "ICY-META: StreamTitle='Artist - Title';", and ffmpeg's metadata dump ("    icy-name : x",
// "    StreamTitle : y"). Returns true and sets *key ("name" or "title") and *value for those.
bool parse_icy_line(const std::string& line, std::string* key, std::string* value);

// An ICY stream read with "Icy-MetaData: 1": `headers` as curl dumps them (every response of a
// redirect chain), `body` the first bytes of the stream. The metadata block sits after
// icy-metaint bytes of audio: a length byte (×16), then "StreamTitle='...';...". Returns the
// size of the prefix that has to be read (metaint + 1 + the block) in *need when body is short.
bool icy_title_from(const std::string& headers, const std::string& body, std::string* title,
                    size_t* need);

}  // namespace btb::audio
