#include "audio/engine.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "audio/endpoints.h"
#include "audio/pcm.h"
#include "util/exec.h"
#include "util/log.h"
#include "util/prio.h"
#include "util/strings.h"

using json = nlohmann::json;

namespace btb::audio {

namespace {

constexpr int kChunkMs = 20;
constexpr int kLevelMs = 250;   // a meter reading every this much audio
constexpr int kPollMs = 200;    // how long the pump waits on a pipe before looking at stop again
// A stream that plays nowhere and that nobody has listened to for this long ends by itself: the
// browser that asked for it went away (a closed tab), and on one core it would be pure waste.
constexpr int64_t kUnheardMs = 30000;

int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t mono_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string out;
  for (const std::string& x : v) {
    if (!out.empty()) out += sep;
    out += x;
  }
  return out;
}

bool file_exists(const std::string& p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0;
}

double num(const json& j, const char* k, double fallback) {
  if (!j.contains(k) || j[k].is_null()) return fallback;
  if (!j[k].is_number()) throw std::invalid_argument(std::string(k) + " must be a number");
  return j[k].get<double>();
}

std::string str(const json& j, const char* k, const std::string& fallback) {
  if (!j.contains(k) || j[k].is_null()) return fallback;
  if (!j[k].is_string()) throw std::invalid_argument(std::string(k) + " must be a string");
  return j[k].get<std::string>();
}

bool url_ok(const std::string& u) {
  const std::string l = lower(u);
  if (!starts_with(l, "http://") && !starts_with(l, "https://")) return false;
  if (u.size() > 2048) return false;
  for (const unsigned char c : u)
    if (c <= 0x20 || c == 0x7f) return false;
  return true;
}

// Text that goes into a PipeWire property string on pw-cat's command line: no quotes, braces or
// backslashes (which SPA-JSON would read as structure), printable only.
std::string prop_text(const std::string& s) {
  std::string out;
  for (const unsigned char c : s) {
    if (c < 0x20 || c == '"' || c == '\\' || c == '{' || c == '}' || c == '=') continue;
    out.push_back(static_cast<char>(c));
  }
  return out.substr(0, 120);
}

std::string fmt_hz(double f) {
  char b[32];
  if (f >= 1000 && std::fmod(f, 100) == 0) std::snprintf(b, sizeof(b), "%g kHz", f / 1000);
  else std::snprintf(b, sizeof(b), "%g Hz", f);
  return b;
}

// What a URL's extension says it is: "mp3", "other" or "" (none, or one that says nothing).
std::string url_audio_kind(const std::string& url) {
  std::string path = lower(url);
  const size_t q = path.find_first_of("?#");
  if (q != std::string::npos) path.resize(q);
  if (ends_with(path, ".mp3")) return "mp3";
  static const char* const kNotMp3[] = {".aac", ".m4a", ".mp4", ".ogg", ".oga", ".opus",
                                        ".flac", ".wav", ".m3u8", ".wma", ".aiff"};
  for (const char* e : kNotMp3)
    if (ends_with(path, e)) return "other";
  return {};
}

// Which decoder a URL goes to with decoder "auto": mpg123 for what is (most likely) MP3 — most
// internet radio, and mpg123 is light and reports ICY titles — ffmpeg for anything else it can
// tell (AAC, which the Bluetooth side plays as well, Ogg, FLAC), or when there is no mpg123.
// `kind` is what the stream itself turned out to be (sniff_audio), when it was looked at.
std::string pick_decoder(const SourceSpec& s, const std::string& url, const Tools& t, const std::string& kind) {
  if (s.decoder != "auto") return s.decoder;
  const std::string k = !kind.empty() ? kind : url_audio_kind(url);
  if (!t.mpg123.empty() && !(k == "other" && !t.ffmpeg.empty())) return "mpg123";
  if (!t.ffmpeg.empty()) return "ffmpeg";
  return "mpg123";
}

}  // namespace

// ---- Tools --------------------------------------------------------------------------------------

Tools Tools::find() {
  Tools t;
  t.pw_cat = find_exec("pw-cat");
  t.pw_dump = find_exec("pw-dump");
  t.wpctl = find_exec("wpctl");
  t.aplay = find_exec("aplay");
  t.arecord = find_exec("arecord");
  t.mpg123 = find_exec("mpg123");
  t.ffmpeg = find_exec("ffmpeg");
  t.curl = find_exec("curl");
  return t;
}

json Tools::to_json() const {
  return nlohmann::json{{"pw-cat", !pw_cat.empty()},   {"pw-dump", !pw_dump.empty()},
                        {"wpctl", !wpctl.empty()},
                        {"aplay", !aplay.empty()},     {"arecord", !arecord.empty()},
                        {"mpg123", !mpg123.empty()},   {"ffmpeg", !ffmpeg.empty()},
                        {"curl", !curl.empty()}};
}

// ---- specs --------------------------------------------------------------------------------------

namespace {

constexpr size_t kMaxPlaylist = 500;

PlaylistItem titled(const std::string& url, const std::string& title) {
  PlaylistItem p;
  p.url = url;
  p.title = title;
  return p;
}

PlaylistItem playlist_item(const json& it) {
  if (!it.is_object()) throw std::invalid_argument("a playlist item is {url, title, artist, album, duration_ms, art}");
  PlaylistItem p = titled(trim(str(it, "url", "")), str(it, "title", "").substr(0, 120));
  if (!url_ok(p.url)) throw std::invalid_argument("a playlist url must be an http:// or https:// URL");
  p.artist = str(it, "artist", "").substr(0, 120);
  p.album = str(it, "album", "").substr(0, 120);
  // Up to a day: a media server's length of a recording, never a radio's (no end).
  p.duration_ms = static_cast<uint32_t>(std::clamp(num(it, "duration_ms", 0), 0.0, 86400000.0));
  // The cover goes on to a Bluetooth remote (the board's player); one that is no URL is dropped.
  p.art = trim(str(it, "art", ""));
  if (!url_ok(p.art)) p.art.clear();
  return p;
}

json playlist_item_json(const PlaylistItem& it) {
  return json{{"url", it.url}, {"title", it.title}, {"artist", it.artist}, {"album", it.album},
              {"duration_ms", it.duration_ms ? json(it.duration_ms) : json(nullptr)}, {"art", it.art}};
}

std::string repeat_mode(const json& j, const std::string& def) {
  const std::string r = str(j, "repeat", def);
  if (r != "off" && r != "track" && r != "all") throw std::invalid_argument("repeat is off, track or all");
  return r;
}

}  // namespace

// ---- the play order ---------------------------------------------------------------------------

void PlayOrder::reset(int n, int current, bool shuffle) {
  order_.clear();
  for (int i = 0; i < n; ++i) order_.push_back(i);
  at_ = 0;
  if (n <= 0) return;
  current = std::clamp(current, 0, n - 1);
  if (shuffle) {
    // The one playing stays first; the rest follow in a random order.
    std::swap(order_[0], order_[static_cast<size_t>(current)]);
    std::shuffle(order_.begin() + 1, order_.end(), rng_);
  } else {
    at_ = static_cast<size_t>(current);
  }
}

void PlayOrder::grow(int n, bool shuffle) {
  for (int i = static_cast<int>(order_.size()); i < n; ++i) {
    if (!shuffle || at_ + 1 >= order_.size()) {
      order_.push_back(i);
    } else {
      std::uniform_int_distribution<size_t> d(at_ + 1, order_.size());
      order_.insert(order_.begin() + static_cast<std::ptrdiff_t>(d(rng_)), i);
    }
  }
}

int PlayOrder::next(const std::string& repeat, bool asked, bool shuffle) {
  if (order_.empty()) return -1;
  if (!asked && repeat == "track") return order_[at_];
  if (at_ + 1 < order_.size()) return order_[++at_];
  if (repeat == "all") {
    // Round again; shuffled, in a new order that does not start with the track just played.
    const int last = order_[at_];
    if (shuffle && order_.size() > 1) {
      std::shuffle(order_.begin(), order_.end(), rng_);
      if (order_[0] == last) std::swap(order_[0], order_[order_.size() - 1]);
    }
    at_ = 0;
    return order_[0];
  }
  return asked ? order_[at_] : -1;
}

int PlayOrder::previous(const std::string& repeat) {
  if (order_.empty()) return -1;
  if (at_ > 0) return order_[--at_];
  if (repeat == "all") at_ = order_.size() - 1;
  return order_[at_];
}

void PlayOrder::go(int track) {
  const auto it = std::find(order_.begin(), order_.end(), track);
  if (it != order_.end()) at_ = static_cast<size_t>(it - order_.begin());
}

// A stream's "sink": null (listeners only) or {type, target}. Throws std::invalid_argument.
SinkSpec sink_spec(const json& kj) {
  SinkSpec k;
  if (kj.is_null()) return k;
  if (!kj.is_object()) throw std::invalid_argument("sink must be an object or null");
  k.type = str(kj, "type", "pipewire");
  k.target = str(kj, "target", "");
  if (k.type != "none" && k.type != "pipewire" && k.type != "alsa")
    throw std::invalid_argument("sink type is pipewire, alsa or none");
  if (!k.target.empty() && !endpoint_id_ok(k.target)) throw std::invalid_argument("bad sink target");
  if (k.type == "alsa" && k.target.empty()) throw std::invalid_argument("an ALSA sink needs a target device");
  if (k.type == "none") k.target.clear();
  return k;
}

namespace {

bool same_sink(const SinkSpec& a, const SinkSpec& b) { return a.type == b.type && a.target == b.target; }

// A stream's outputs: "sinks", an array of sinks (each once; none left out), or the one "sink" of
// before. Throws std::invalid_argument.
std::vector<SinkSpec> sinks_spec(const json& j) {
  std::vector<SinkSpec> out;
  auto add = [&out](const SinkSpec& k) {
    if (k.type == "none") return;
    for (const SinkSpec& x : out)
      if (same_sink(x, k)) return;
    if (out.size() >= AudioEngine::kMaxSinks)
      throw std::invalid_argument("a stream plays to at most " + std::to_string(AudioEngine::kMaxSinks) + " sinks");
    out.push_back(k);
  };
  if (j.contains("sinks")) {
    if (!j["sinks"].is_array()) throw std::invalid_argument("sinks is an array of {type, target}");
    for (const json& k : j["sinks"]) {
      if (k.is_null()) throw std::invalid_argument("a sink in sinks is an object");
      add(sink_spec(k));
    }
  } else if (j.contains("sink")) {
    add(sink_spec(j["sink"]));
  }
  return out;
}

json sink_json(const SinkSpec& k) { return json{{"type", k.type}, {"target", k.target}}; }

}  // namespace

bool stream_spec_from_json(const json& j, StreamSpec* out, std::string* err) {
  try {
    if (!j.is_object()) throw std::invalid_argument("the body must be an object");
    StreamSpec s;
    static const int kRates[] = {8000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000};
    s.rate = static_cast<int>(num(j, "rate", 48000));
    if (std::find(std::begin(kRates), std::end(kRates), s.rate) == std::end(kRates))
      throw std::invalid_argument("rate is one of 8000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000");
    s.channels = static_cast<int>(num(j, "channels", 2));
    if (s.channels != 1 && s.channels != 2) throw std::invalid_argument("channels is 1 or 2");
    s.gain_db = std::clamp(num(j, "gain_db", 0), -60.0, 20.0);
    s.latency_ms = static_cast<int>(std::clamp(num(j, "latency_ms", 60), 10.0, 1000.0));
    s.label = str(j, "label", "").substr(0, 120);
    const double nyquist = s.rate / 2.0;

    if (!j.contains("source") || !j["source"].is_object()) throw std::invalid_argument("source is required");
    const json& sj = j["source"];
    SourceSpec& src = s.source;
    src.type = str(sj, "type", "");
    if (src.type == "tone") {
      src.freq = std::clamp(num(sj, "freq", 1000), 1.0, nyquist);
      src.freq_right = sj.contains("freq_right") && !sj["freq_right"].is_null()
                           ? std::clamp(num(sj, "freq_right", 0), 1.0, nyquist) : 0.0;
      src.level_db = std::clamp(num(sj, "level_db", -12), -96.0, 0.0);
    } else if (src.type == "sweep") {
      src.from = std::clamp(num(sj, "from", 20), 1.0, nyquist);
      src.to = std::clamp(num(sj, "to", std::min(20000.0, nyquist)), 1.0, nyquist);
      src.seconds = std::clamp(num(sj, "seconds", 10), 0.5, 600.0);
      src.log = sj.value("log", true);
      src.repeat = sj.value("repeat", true);
      src.level_db = std::clamp(num(sj, "level_db", -12), -96.0, 0.0);
    } else if (src.type == "noise") {
      src.color = str(sj, "color", "pink");
      NoiseGen::Color c;
      if (!noise_color(src.color, &c)) throw std::invalid_argument("color is white, pink or brown");
      src.level_db = std::clamp(num(sj, "level_db", -20), -96.0, 0.0);
    } else if (src.type == "silence") {
    } else if (src.type == "url") {
      src.url = trim(str(sj, "url", ""));
      if (!src.url.empty() && !url_ok(src.url)) throw std::invalid_argument("url must be an http:// or https:// URL");
      src.decoder = str(sj, "decoder", "auto");
      if (src.decoder != "auto" && src.decoder != "mpg123" && src.decoder != "ffmpeg")
        throw std::invalid_argument("decoder is auto, mpg123 or ffmpeg");
      src.title = str(sj, "title", "").substr(0, 120);
      // A playlist: its first item is the stream's URL (given or not).
      if (sj.contains("playlist")) {
        if (!sj["playlist"].is_array() || sj["playlist"].empty() || sj["playlist"].size() > kMaxPlaylist)
          throw std::invalid_argument("playlist is an array of 1.." + std::to_string(kMaxPlaylist) + " {url, title}");
        for (const json& it : sj["playlist"]) src.playlist.push_back(playlist_item(it));
        if (src.url.empty()) src.url = src.playlist.front().url;
        else if (src.playlist.front().url != src.url) src.playlist.insert(src.playlist.begin(), titled(src.url, src.title));
        if (src.title.empty()) src.title = src.playlist.front().title;
      }
      if (!url_ok(src.url)) throw std::invalid_argument("url must be an http:// or https:// URL");
      if (src.playlist.empty()) src.playlist.push_back(titled(src.url, src.title));
      src.repeat_mode = repeat_mode(sj, "off");
      if (sj.contains("shuffle")) {
        if (!sj["shuffle"].is_boolean()) throw std::invalid_argument("shuffle is true or false");
        src.shuffle = sj["shuffle"].get<bool>();
      }
    } else if (src.type == "capture") {
      src.backend = str(sj, "backend", "pipewire");
      if (src.backend != "pipewire" && src.backend != "alsa") throw std::invalid_argument("source backend is pipewire or alsa");
      src.target = str(sj, "target", "");
      if (!src.target.empty() && !endpoint_id_ok(src.target)) throw std::invalid_argument("bad source target");
      if (src.backend == "alsa" && src.target.empty()) throw std::invalid_argument("an ALSA capture needs a target device");
      src.monitor = sj.value("monitor", false);
      if (src.monitor && (src.backend != "pipewire" || src.target.empty()))
        throw std::invalid_argument("monitor captures what goes to a PipeWire sink: give its target");
      src.title = str(sj, "title", "").substr(0, 120);
    } else {
      throw std::invalid_argument("source type is tone, sweep, noise, silence, url or capture");
    }

    s.sinks = sinks_spec(j);
    *out = std::move(s);
    return true;
  } catch (const std::exception& e) {
    *err = e.what();
    return false;
  }
}

json stream_spec_json(const StreamSpec& s) {
  const SourceSpec& src = s.source;
  json sj{{"type", src.type}};
  if (src.type == "tone") {
    sj["freq"] = src.freq;
    sj["freq_right"] = src.freq_right > 0 ? json(src.freq_right) : json(nullptr);
    sj["level_db"] = src.level_db;
  } else if (src.type == "sweep") {
    sj.update(json{{"from", src.from}, {"to", src.to}, {"seconds", src.seconds}, {"log", src.log},
                   {"repeat", src.repeat}, {"level_db", src.level_db}});
  } else if (src.type == "noise") {
    sj.update(json{{"color", src.color}, {"level_db", src.level_db}});
  } else if (src.type == "url") {
    json pl = json::array();
    for (const PlaylistItem& it : src.playlist) pl.push_back(playlist_item_json(it));
    sj.update(json{{"url", src.url}, {"decoder", src.decoder}, {"title", src.title}, {"playlist", pl},
                   {"repeat", src.repeat_mode}, {"shuffle", src.shuffle}});
  } else if (src.type == "capture") {
    sj.update(json{{"backend", src.backend}, {"target", src.target}, {"monitor", src.monitor},
                   {"title", src.title}});
  }
  json sinks = json::array();
  for (const SinkSpec& k : s.sinks) sinks.push_back(sink_json(k));
  // "sink": the first of them, for what reads one (the CLI, the board's player).
  return json{{"source", sj},
              {"sinks", sinks},
              {"sink", s.sinks.empty() ? json(nullptr) : sinks[0]},
              {"rate", s.rate},
              {"channels", s.channels},
              {"gain_db", s.gain_db},
              {"latency_ms", s.latency_ms}};
}

std::string default_label(const StreamSpec& s) {
  const SourceSpec& src = s.source;
  std::string what;
  if (src.type == "tone") {
    what = "tone " + fmt_hz(src.freq);
    if (src.freq_right > 0 && src.freq_right != src.freq) what += " / " + fmt_hz(src.freq_right);
  } else if (src.type == "sweep") {
    what = "sweep " + fmt_hz(src.from) + " → " + fmt_hz(src.to);
  } else if (src.type == "noise") {
    what = src.color + " noise";
  } else if (src.type == "silence") {
    what = "silence";
  } else if (src.type == "url") {
    what = !src.title.empty() ? src.title : src.url;
  } else {
    what = !src.title.empty() ? src.title : (src.monitor ? "monitor of " : "capture ") + (src.target.empty() ? "default" : src.target);
  }
  std::string to;
  for (const SinkSpec& k : s.sinks) to += (to.empty() ? "" : " + ") + (k.target.empty() ? std::string("default sink") : k.target);
  return what + " → " + (to.empty() ? "listeners" : to);
}

bool decoder_argv(const SourceSpec& s, const std::string& url, int rate, int channels,
                  const Tools& t, std::vector<std::string>* argv, std::string* err, const std::string& kind) {
  const std::string dec = pick_decoder(s, url, t, kind);
  if (dec == "mpg123") {
    if (t.mpg123.empty()) {
      *err = "playing a URL needs mpg123 (MP3) or ffmpeg on the board; neither is installed";
      return false;
    }
    // -s: raw samples on stdout; -e/-r/--stereo|--mono: exactly the stream's format (mpg123
    // resamples with its own NtoM, cheaper than anything after it). Not -q: the ICY titles are in
    // what it prints on stderr. https goes through mpg123's curl helper.
    *argv = {t.mpg123, "-s", "-e", "s16", "-r", std::to_string(rate), channels == 1 ? "--mono" : "--stereo",
             "--timeout", "15", url};
    return true;
  }
  if (t.ffmpeg.empty()) {
    *err = "this URL needs ffmpeg, which is not installed (mpg123 plays only MP3)";
    return false;
  }
  const std::string out = " -vn -f s16le -ac " + std::to_string(channels) + " -ar " + std::to_string(rate) + " pipe:1";
  // https through curl, as mpg123 does it: the image's ffmpeg is built without TLS. One process
  // group, so stopping the stream stops both; the URL goes in as an argument, never into the
  // script.
  if (starts_with(lower(url), "https://") && !t.curl.empty()) {
    *argv = {"/bin/sh", "-c", "\"$1\" -sSL -A btbenchd/1.0 \"$2\" | exec \"$3\" -nostdin -hide_banner -loglevel info -i pipe:0" + out,
             "sh", t.curl, url, t.ffmpeg};
    return true;
  }
  *argv = {t.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "info", "-reconnect", "1",
           "-reconnect_streamed", "1", "-reconnect_delay_max", "5", "-i", url, "-vn", "-f", "s16le",
           "-ac", std::to_string(channels), "-ar", std::to_string(rate), "pipe:1"};
  return true;
}

bool capture_argv(const SourceSpec& s, int rate, int channels, int latency_ms,
                  const std::string& node_name, const Tools& t, std::vector<std::string>* argv,
                  std::string* err) {
  if (s.backend == "alsa") {
    if (t.arecord.empty()) {
      *err = "arecord is not installed";
      return false;
    }
    *argv = {t.arecord, "-q", "-D", s.target, "-t", "raw", "-f", "S16_LE", "-r", std::to_string(rate),
             "-c", std::to_string(channels), "--buffer-time=" + std::to_string(latency_ms * 1000), "-"};
    return true;
  }
  if (t.pw_cat.empty()) {
    *err = "pw-cat is not installed";
    return false;
  }
  std::string props = "{ node.name = \"" + node_name + "\" node.description = \"btbench capture\"";
  // A sink's monitor: the same node name as target, captured from its output side.
  if (s.monitor) props += " stream.capture.sink = true";
  // A source named is that source or none, as a sink is (sink_argv): when it goes away (a codec
  // change makes a Bluetooth source's node anew) WirePlumber would link the capture to the default
  // source (the loopback card) and leave it there. pw-cat ends instead, and the stream with it.
  if (!s.target.empty()) props += " node.dont-fallback = true node.dont-reconnect = true";
  props += " }";
  *argv = {t.pw_cat, "--record", "--rate", std::to_string(rate), "--channels", std::to_string(channels),
           "--format", "s16", "--latency", std::to_string(latency_ms) + "ms", "-P", props};
  if (!s.target.empty()) argv->insert(argv->end(), {"--target", s.target});
  argv->push_back("-");
  return true;
}

bool sink_argv(const SinkSpec& s, int rate, int channels, int latency_ms,
               const std::string& node_name, const std::string& description, const Tools& t,
               std::vector<std::string>* argv, std::string* err) {
  if (s.type == "alsa") {
    if (t.aplay.empty()) {
      *err = "aplay is not installed";
      return false;
    }
    *argv = {t.aplay, "-q", "-D", s.target, "-t", "raw", "-f", "S16_LE", "-r", std::to_string(rate),
             "-c", std::to_string(channels), "--buffer-time=" + std::to_string(latency_ms * 1000), "-"};
    return true;
  }
  if (t.pw_cat.empty()) {
    *err = "pw-cat is not installed";
    return false;
  }
  // pw-cat reads a WAV from stdin through libsndfile (the pump writes the header first), so the
  // format needs no flags — and no --raw, which older pw-cat lacks.
  // A sink named is that sink or none: WirePlumber would otherwise play into the default output
  // (the loopback card) when it is not there, or has gone (a speaker disconnected), without a word.
  // pw-cat exits instead, "defined target not found", which is then the sink's reason.
  const std::string only = s.target.empty() ? "" : " node.dont-fallback = true node.dont-reconnect = true";
  *argv = {t.pw_cat, "--playback", "--latency", std::to_string(latency_ms) + "ms", "--media-role", "Music",
           "-P", "{ node.name = \"" + node_name + "\" node.description = \"" + prop_text(description) +
                     "\" media.name = \"" + prop_text(description) + "\"" + only + " }"};
  if (!s.target.empty()) argv->insert(argv->end(), {"--target", s.target});
  argv->push_back("-");
  (void)rate;
  (void)channels;
  return true;
}

// ---- a child process ----------------------------------------------------------------------------

namespace {

// A decoder, capture or sink process: its stdin and/or stdout belong to the pump; its stderr is
// drained by a thread of its own, line by line (the ICY titles, and the last line as the reason
// when it fails).
class Child {
 public:
  using LineFn = std::function<void(const std::string&)>;
  ~Child() { stop(); }

  bool start(const std::vector<std::string>& argv, bool stdin_pipe, LineFn on_line, std::string* err) {
    if (!spawn_io(argv, stdin_pipe, &p_, err)) return false;
    name_ = argv.empty() ? "" : argv[0].substr(argv[0].rfind('/') + 1);
    LOG_DEBUG("audio: started {} (pid {})", join(argv, " "), p_.pid);
    on_line_ = std::move(on_line);
    drainer_ = std::thread([this] { drain(); });
    return true;
  }

  void close_stdin() {
    if (p_.in_fd >= 0) close(p_.in_fd);
    p_.in_fd = -1;
  }

  int pid() const { return p_.pid; }
  int in_fd() const { return p_.in_fd; }
  int out_fd() const { return p_.out_fd; }
  const std::string& name() const { return name_; }

  std::string last_line() const {
    std::lock_guard<std::mutex> lk(m_);
    return last_;
  }
  // Why it stopped, said best: what its last "...error: why" line says (pw-cat ends with a line
  // about its node after the error itself), else its last line.
  std::string reason() const {
    std::lock_guard<std::mutex> lk(m_);
    if (error_.empty()) return last_;
    const size_t c = error_.rfind(": ");
    return c == std::string::npos ? error_ : error_.substr(c + 2);
  }

  // Ends it: closes our ends, SIGTERM to its group (SIGKILL after 300 ms), reaps it. Returns its
  // exit status (128+N for a signal), -1 if it never ran.
  int stop() {
    if (p_.pid <= 0) return status_;
    if (p_.in_fd >= 0) close(p_.in_fd);
    if (p_.out_fd >= 0) close(p_.out_fd);
    p_.in_fd = p_.out_fd = -1;
    // Give a process that is exiting by itself (EOF on its stdin) a moment to do so cleanly.
    int st = 0;
    pid_t w = 0;
    for (int i = 0; i < 5 && (w = waitpid(p_.pid, &st, WNOHANG)) == 0; ++i) usleep(10000);
    if (w == 0) {
      kill_group(p_.pid, 300);
      w = waitpid(p_.pid, &st, 0);
    }
    if (w == p_.pid) status_ = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
    p_.pid = -1;
    if (drainer_.joinable()) drainer_.join();
    if (p_.err_fd >= 0) close(p_.err_fd);
    p_.err_fd = -1;
    return status_;
  }

 private:
  void drain() {
    std::string acc;
    char buf[1024];
    for (;;) {
      const ssize_t n = read(p_.err_fd, buf, sizeof(buf));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      acc.append(buf, static_cast<size_t>(n));
      // Lines end in \n, and progress output (ffmpeg's size=..., mpg123's frame counter) in \r.
      size_t pos;
      while ((pos = acc.find_first_of("\r\n")) != std::string::npos) {
        const std::string line = trim(acc.substr(0, pos));
        acc.erase(0, pos + 1);
        if (line.empty()) continue;
        {
          std::lock_guard<std::mutex> lk(m_);
          last_ = line;
          if (line.find("error: ") != std::string::npos) error_ = line;
        }
        if (on_line_) on_line_(line);
      }
      if (acc.size() > 4096) acc.clear();
    }
  }

  Spawned p_;
  std::string name_;
  LineFn on_line_;
  std::thread drainer_;
  mutable std::mutex m_;
  std::string last_, error_;
  int status_ = -1;
};

// Writes all of [p, p+n) to a non-blocking fd, waiting as the reader drains it. False on a broken
// pipe (the reader exited) or when `stop` (or `also`) was raised. `waiting` hears how long it has
// waited, at each poll.
bool write_all(int fd, const char* p, size_t n, const std::atomic<bool>& stop, const std::atomic<bool>* also = nullptr,
               const std::function<void(int64_t)>& waiting = nullptr) {
  const int64_t since = mono_ms();
  while (n > 0) {
    const ssize_t w = write(fd, p, n);
    if (w > 0) {
      p += w;
      n -= static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && errno != EAGAIN) return false;
    pollfd pf{fd, POLLOUT, 0};
    const int r = poll(&pf, 1, kPollMs);
    if (stop.load() || (also && also->load())) return false;
    if (r > 0 && (pf.revents & (POLLERR | POLLHUP)) && !(pf.revents & POLLOUT)) return false;
    if (waiting) waiting(mono_ms() - since);
  }
  return true;
}

// Writes what a non-blocking fd takes now of *buf, from its front, and drops it from *buf (*took
// bytes). False on a broken pipe (the reader exited).
bool write_some(int fd, std::string* buf, size_t* took) {
  *took = 0;
  while (!buf->empty()) {
    const ssize_t w = write(fd, buf->data(), buf->size());
    if (w > 0) {
      buf->erase(0, static_cast<size_t>(w));
      *took += static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    return w < 0 && errno == EAGAIN;
  }
  return true;
}

// Reads until *out holds `want` bytes, the fd hits EOF (returns false; *out keeps what came), or
// `stop` is raised (false). Waits in kPollMs steps.
bool read_some(int fd, std::string* out, size_t want, const std::atomic<bool>& stop, bool* eof) {
  char buf[8192];
  while (out->size() < want) {
    pollfd pf{fd, POLLIN, 0};
    const int r = poll(&pf, 1, kPollMs);
    if (stop.load()) return false;
    if (r <= 0) continue;
    const ssize_t n = read(fd, buf, std::min(sizeof(buf), want - out->size() + 4096));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (n <= 0) {
      *eof = true;
      return false;
    }
    out->append(buf, static_cast<size_t>(n));
  }
  return true;
}

// The current metadata block of an ICY stream (StreamTitle, StreamUrl), and the response's headers
// (icy-description), read on a connection of its own: for a stream ffmpeg decodes, which does not
// report title changes. Reads one metadata block's worth (tens of KB) and hangs up. False when the
// server sends no ICY metadata.
bool fetch_icy(const std::string& curl, const std::string& url, const std::atomic<bool>& stop, std::string* meta,
               std::string* headers) {
  Spawned p;
  std::string err;
  if (!spawn({curl, "-sSL", "-A", "btbenchd/1.0", "--max-time", "10", "-H", "Icy-MetaData: 1", "-D", "/dev/stderr", url}, &p, &err))
    return false;
  std::string body;
  bool got = false;
  size_t need = 1;
  const int64_t until = mono_ms() + 10000;
  bool out_open = true, err_open = true;
  char buf[8192];
  while ((out_open || err_open) && !stop.load() && mono_ms() < until) {
    pollfd pf[2] = {{out_open ? p.out_fd : -1, POLLIN, 0}, {err_open ? p.err_fd : -1, POLLIN, 0}};
    if (poll(pf, 2, kPollMs) <= 0) continue;
    for (int i = 0; i < 2; ++i) {
      if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const ssize_t n = read(pf[i].fd, buf, sizeof(buf));
      if (n <= 0) {
        (i == 0 ? out_open : err_open) = false;
        continue;
      }
      (i == 0 ? body : *headers).append(buf, static_cast<size_t>(n));
    }
    if (body.size() >= need) {
      if ((got = icy_meta_from(*headers, body, meta, &need)) || need == 0) break;
    }
  }
  close(p.out_fd);
  close(p.err_fd);
  kill_group(p.pid, 200);
  waitpid(p.pid, nullptr, 0);
  return got;
}

// What a stream is (sniff_audio: "mp3", "other", ""), from its headers and first few KB, read on a
// connection of its own and hung up. Only asked when the choice matters: decoder "auto", both
// decoders there, and an extension that does not say (a station's "/aac/96", "/live").
std::string probe_audio_kind(const SourceSpec& s, const std::string& url, const Tools& t, const std::atomic<bool>& stop) {
  if (s.decoder != "auto" || t.curl.empty() || t.mpg123.empty() || t.ffmpeg.empty() || !url_audio_kind(url).empty())
    return {};
  Spawned p;
  std::string err;
  if (!spawn({t.curl, "-sSL", "-A", "btbenchd/1.0", "--max-time", "5", "-D", "/dev/stderr", url}, &p, &err)) return {};
  std::string headers, body;
  const int64_t until = mono_ms() + 5000;
  bool out_open = true, err_open = true;
  char buf[8192];
  while ((out_open || err_open) && body.size() < 4096 && !stop.load() && mono_ms() < until) {
    pollfd pf[2] = {{out_open ? p.out_fd : -1, POLLIN, 0}, {err_open ? p.err_fd : -1, POLLIN, 0}};
    if (poll(pf, 2, kPollMs) <= 0) continue;
    for (int i = 0; i < 2; ++i) {
      if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const ssize_t n = read(pf[i].fd, buf, sizeof(buf));
      if (n <= 0) {
        (i == 0 ? out_open : err_open) = false;
        continue;
      }
      (i == 0 ? body : headers).append(buf, static_cast<size_t>(n));
    }
  }
  close(p.out_fd);
  close(p.err_fd);
  kill_group(p.pid, 200);
  waitpid(p.pid, nullptr, 0);
  const std::string kind = sniff_audio(http_header(headers, "content-type"), body);
  LOG_DEBUG("audio: {} sniffed as {}", url, kind.empty() ? "unknown" : kind);
  return kind;
}

// The stream a .pls/.m3u playlist URL names first; any other URL as it is.
std::string playlist_stream(const std::string& curl, const std::string& url) {
  if (!looks_like_playlist(url) || curl.empty()) return url;
  const ExecResult r = run_cmd({curl, "-sSL", "-A", "btbenchd/1.0", "--max-time", "8", "--max-filesize", "65536", url}, 10000, 65536);
  if (!r.ok()) return url;
  const std::string first = playlist_first_url(r.out);
  return first.empty() || !url_ok(first) ? url : first;
}

// A station added by its URL alone: the name its stream gives itself (icy-name), else the URL's
// host.
std::string station_name(const std::string& curl, const std::string& url) {
  if (!curl.empty()) {
    const std::atomic<bool> stop{false};
    std::string meta, headers;
    fetch_icy(curl, playlist_stream(curl, url), stop, &meta, &headers);
    const std::string n = http_header(headers, "icy-name");
    if (!n.empty() && lower(n) != "unspecified name") return n;
  }
  const size_t a = url.find("://");
  std::string host = url.substr(a == std::string::npos ? 0 : a + 3);
  host = host.substr(0, host.find_first_of(":/?#"));
  return starts_with(host, "www.") ? host.substr(4) : host;
}

// The cover a station's StreamUrl leads to (pcm.h image_url_in_json): the URL itself, or the image
// its now-playing JSON names. "" for none.
std::string resolve_art(const std::string& curl, const std::string& stream_url) {
  if (stream_url.empty() || url_is_image(stream_url)) return stream_url;
  if (!url_ok(stream_url)) return {};
  const ExecResult r = run_cmd({curl, "-sSL", "-A", "btbenchd/1.0", "--max-time", "5", "--max-filesize", "65536", stream_url}, 7000, 65536);
  return r.ok() ? image_url_in_json(r.out) : std::string();
}

}  // namespace

// ---- a stream -----------------------------------------------------------------------------------

struct AudioEngine::Stream {
  int id = 0;
  StreamSpec spec;  // under m (gain and the live source parameters change)
  std::string label;
  std::string node_name;
  const Tools* tools = nullptr;
  std::atomic<bool> stop{false};
  std::thread thread;
  std::thread titles;  // the ICY title poller of a URL
  std::atomic<int64_t> decoder_title_ms{-1000000};  // when the decoder last reported a title

  mutable std::mutex m;
  std::string state = "starting";  // starting | running | ended | failed | stopped
  std::string error;
  std::string resolved_url;  // a playlist's stream
  std::string meta_name, meta_title;
  std::string meta_art, meta_description;  // a station's cover (or logo) and what it says about itself
  std::string art_src;                     // its StreamUrl, resolved into meta_art by the title poller
  int64_t started_ms = 0, ended_ms = 0;
  uint64_t frames = 0;
  bool params_dirty = false;
  int track = 0;     // a URL stream's playlist position
  PlayOrder order{static_cast<uint32_t>(std::random_device{}())};  // which track follows which
  uint64_t track_frames = 0;  // frames played of the current track
  bool paused = false;  // a URL stream: silence goes out, and the decoder waits on its pipe
  int jump_to = -1;  // a track asked for (next, previous, a row), taken by the pump
  // PUT {"sinks"}: the pump closes the sink processes no longer in spec.sinks and opens the new
  // ones, the others, the source and the listeners carrying on. Raised, it also frees the pump
  // from sinks that take nothing.
  std::atomic<bool> sink_switch{false};
  // Each sink as the pump has it: starting (opened, not yet taking audio), playing (has taken half
  // a second), stalled (takes nothing), or failed (it would not open, or stopped:
  // `error`; the stream goes on without it). The stream's sinks in order, then the ones it lost
  // since the last PUT {"sinks"}.
  struct SinkStatus {
    SinkSpec sink;
    std::string state, error;
    int64_t since_ms = 0;
  };
  std::vector<SinkStatus> sinks_status;
  int sink_req = 0;  // PUT {"sinks"} requests so far; `sink_seq` the one the sinks' states answer
  int sink_seq = 0;
  LevelMeter::Reading level;
  std::vector<ListenerPtr> listeners;

  json to_json() const {
    std::lock_guard<std::mutex> lk(m);
    json lv{{"peak_db", json::array()}, {"rms_db", json::array()}, {"clipped", level.clipped}};
    for (int c = 0; c < level.channels; ++c) {
      lv["peak_db"].push_back(std::round(level.peak_db[c] * 10) / 10);
      lv["rms_db"].push_back(std::round(level.rms_db[c] * 10) / 10);
    }
    json ss = json::array();
    for (const SinkStatus& k : sinks_status)
      ss.push_back(json{{"type", k.sink.type}, {"target", k.sink.target}, {"state", k.state}, {"error", k.error},
                        {"since_ms", k.since_ms}});
    // The first sink's, as it was when a stream had one.
    json first = ss.empty() ? json{{"type", "none"}, {"target", ""}, {"state", "none"}, {"error", ""}, {"since_ms", 0}}
                            : ss[0];
    first["seq"] = sink_seq;
    json j = stream_spec_json(spec);
    j.update(json{{"id", id},
                  {"label", label},
                  {"node", node_name},
                  {"state", state},
                  {"error", error},
                  {"started_ms", started_ms},
                  {"ended_ms", ended_ms ? json(ended_ms) : json(nullptr)},
                  {"seconds", std::round(static_cast<double>(frames) / spec.rate * 10) / 10},
                  {"listeners", listeners.size()},
                  {"track", track},
                  {"track_seconds", std::round(static_cast<double>(track_frames) / spec.rate * 10) / 10},
                  {"paused", paused},
                  {"sink_req", sink_req},
                  {"sink_seq", sink_seq},
                  {"sinks_status", ss},
                  {"sink_status", first},
                  {"level", lv},
                  {"meta", json{{"name", meta_name}, {"title", meta_title}, {"url", resolved_url}, {"art", meta_art},
                                {"description", meta_description}}}});
    return j;
  }

  void set_state(const std::string& s, const std::string& e = "") {
    std::lock_guard<std::mutex> lk(m);
    if (state == "failed" || state == "stopped" || state == "ended") return;
    state = s;
    if (!e.empty()) error = e;
    if (s != "running" && s != "starting") ended_ms = wall_ms();
  }

  void set_sink(const SinkSpec& k, const std::string& state, const std::string& why = "") {
    std::lock_guard<std::mutex> lk(m);
    auto it = std::find_if(sinks_status.begin(), sinks_status.end(), [&](const SinkStatus& x) { return same_sink(x.sink, k); });
    if (it == sinks_status.end()) it = sinks_status.insert(sinks_status.end(), SinkStatus{k, "", "", 0});
    it->state = state;
    it->error = why;
    it->since_ms = wall_ms();
  }

  void fan_out(const int16_t* s, size_t frames_n, int channels) {
    std::vector<ListenerPtr> ls;
    {
      std::lock_guard<std::mutex> lk(m);
      if (listeners.empty()) return;
      ls = listeners;
    }
    const std::string stereo(reinterpret_cast<const char*>(s), frames_n * channels * sizeof(int16_t));
    std::string mono;
    for (const ListenerPtr& l : ls) {
      const std::string* chunk = &stereo;
      if (l->mono && channels == 2) {
        if (mono.empty()) {
          std::vector<int16_t> tmp(s, s + frames_n * 2);
          downmix(tmp.data(), frames_n);
          mono.assign(reinterpret_cast<const char*>(tmp.data()), frames_n * sizeof(int16_t));
        }
        chunk = &mono;
      }
      std::lock_guard<std::mutex> lk(l->m);
      if (l->closed) continue;
      l->q.push_back(*chunk);
      l->queued += chunk->size();
      while (l->queued > l->max_queued && l->q.size() > 1) {
        l->queued -= l->q.front().size();
        l->q.pop_front();
        ++l->dropped;
      }
      l->cv.notify_one();
    }
  }

  void close_listeners() {
    std::lock_guard<std::mutex> lk(m);
    for (const ListenerPtr& l : listeners) {
      std::lock_guard<std::mutex> lk2(l->m);
      l->closed = true;
      l->cv.notify_all();
    }
  }

  // The source's stderr: titles from the decoder.
  void on_source_line(const std::string& line) {
    std::string k, v;
    if (!parse_icy_line(line, &k, &v)) return;
    std::lock_guard<std::mutex> lk(m);
    if (k == "name") {
      meta_name = v;
    } else {
      meta_title = v;
      decoder_title_ms.store(mono_ms());
      // mpg123 prints the whole block: the cover's URL with the title.
      if (line.find("StreamUrl='") != std::string::npos) art_src = icy_field(line, "StreamUrl");
    }
  }

  std::string resolve_playlist(const std::string& url) { return playlist_stream(tools->curl, url); }

  // A start that failed: the reason, and nothing left running (the title poller included).
  void abandon(const std::string& why) {
    set_state("failed", why);
    stop.store(true);
    if (titles.joinable()) titles.join();
    close_listeners();
  }

  ~Stream() {
    stop.store(true);
    if (titles.joinable()) titles.join();
  }

  void run();
};

void AudioEngine::Stream::run() {
  // Before anything starts: the decoder and the sink inherit this (util/prio.h).
  set_thread_nice(kAudioNice);
  StreamSpec sp;
  {
    std::lock_guard<std::mutex> lk(m);
    sp = spec;
  }
  const int rate = sp.rate, ch = sp.channels;
  const size_t frames_per_chunk = static_cast<size_t>(rate * kChunkMs / 1000);
  const size_t chunk_bytes = frames_per_chunk * ch * sizeof(int16_t);
  const size_t level_frames = static_cast<size_t>(rate * kLevelMs / 1000);
  std::vector<int16_t> buf(frames_per_chunk * ch);
  std::string err;

  // The source.
  std::unique_ptr<Generator> gen;
  SweepGen* sweep = nullptr;
  Child src;
  WavStripper strip;
  std::string pending;  // source bytes not yet a whole chunk
  const std::string& type = sp.source.type;
  if (type == "tone") {
    gen = std::make_unique<ToneGen>(rate, ch, sp.source.freq, sp.source.freq_right, sp.source.level_db);
  } else if (type == "sweep") {
    auto g = std::make_unique<SweepGen>(rate, ch, sp.source.from, sp.source.to, sp.source.seconds, sp.source.log,
                                        sp.source.repeat, sp.source.level_db);
    sweep = g.get();
    gen = std::move(g);
  } else if (type == "noise") {
    NoiseGen::Color c = NoiseGen::Color::Pink;
    noise_color(sp.source.color, &c);
    gen = std::make_unique<NoiseGen>(ch, c, sp.source.level_db, static_cast<uint32_t>(0x9e3779b9u * id));
  } else if (type == "silence") {
    gen = std::make_unique<SilenceGen>(ch);
  } else {
    std::vector<std::string> argv;
    bool ok;
    if (type == "url") {
      const std::string url = resolve_playlist(sp.source.url);
      {
        std::lock_guard<std::mutex> lk(m);
        resolved_url = url;
        if (sp.source.playlist.size() > 1) meta_title = sp.source.playlist.front().title;
      }
      ok = decoder_argv(sp.source, url, rate, ch, *tools, &argv, &err, probe_audio_kind(sp.source, url, *tools, stop));
      // ffmpeg does not report title changes, and mpg123 may not (its https helper): ask the
      // server every 15 s, unless the decoder has reported a title within the last 30 s. The first
      // time regardless: the station's description is in the headers only. A playlist's tracks
      // are files with their titles known: no polling — nor for one such track (a length says it
      // is a file, not a station). The same thread looks up the cover a new StreamUrl leads to.
      const bool file = sp.source.playlist.size() > 1 ||
                        (sp.source.playlist.size() == 1 && sp.source.playlist.front().duration_ms);
      if (ok && !tools->curl.empty() && !file && lower(url).find(".m3u8") == std::string::npos) {
        titles = std::thread([this, url] {
          // Track titles can wait; the pump is what this thread was forked from.
          set_thread_nice(kBackgroundNice);
          std::string art_for;
          bool first = true;
          for (int64_t next = mono_ms() + 2000; !stop.load(); std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs))) {
            std::string want;
            {
              std::lock_guard<std::mutex> lk(m);
              want = art_src;
            }
            if (want != art_for) {
              art_for = want;
              const std::string art = resolve_art(tools->curl, want);
              std::lock_guard<std::mutex> lk(m);
              if (art_src == want) meta_art = art;
            }
            if (mono_ms() < next) continue;
            next = mono_ms() + 15000;
            if (!first && mono_ms() - decoder_title_ms.load() < 30000) continue;
            first = false;
            std::string meta, headers;
            if (!fetch_icy(tools->curl, url, stop, &meta, &headers)) continue;
            std::lock_guard<std::mutex> lk(m);
            const std::string t = icy_field(meta, "StreamTitle");
            if (!t.empty()) meta_title = t;
            if (meta.find("StreamUrl='") != std::string::npos) art_src = icy_field(meta, "StreamUrl");
            const std::string d = http_header(headers, "icy-description");
            if (!d.empty() && lower(d) != "unspecified description") meta_description = d;
            // A stream ffmpeg reads through curl (https) has its station's name only from here.
            const std::string n = http_header(headers, "icy-name");
            if (meta_name.empty() && !n.empty() && lower(n) != "unspecified name") meta_name = n;
          }
        });
      }
    } else {
      ok = capture_argv(sp.source, rate, ch, sp.latency_ms, node_name + "-in", *tools, &argv, &err);
    }
    if (!ok || !src.start(argv, false, [this](const std::string& l) { on_source_line(l); }, &err)) {
      abandon(err);
      return;
    }
    // A capture keeps up with the graph; a decoder only has to stay ahead, and stays below audio.
    if (type == "capture") set_process_fifo(src.pid(), kAudioRtPrio);
  }

  // The sinks: the ones the stream started with, then whatever PUT {"sinks"} made of them. Each
  // is given every chunk, and the one that takes it first is the stream's clock: a sink behind
  // (still opening, a Bluetooth link that stopped streaming, one slower) gets what it has room for
  // and loses the rest, so it holds neither the others nor the listeners.
  struct Out {
    SinkSpec sink;
    std::unique_ptr<Child> proc;
    std::string pending;          // what it was given and has not taken yet
    uint64_t taken = 0;           // bytes it has taken since it opened
    int64_t took_ms = 0;          // when it last took any
    bool stalled = false;
  };
  std::vector<std::unique_ptr<Out>> outs;
  const size_t frame_bytes = static_cast<size_t>(ch) * sizeof(int16_t);
  const uint64_t half_s = static_cast<uint64_t>(rate / 2) * frame_bytes;  // taken this much: it plays
  // What a sink that plays may have waiting beyond its pipe before chunks are lost to it: one that
  // takes its audio in bigger periods than the others.
  const size_t backlog = static_cast<size_t>(rate / 5) * frame_bytes;
  bool switched = false;  // the sinks were changed: losing them leaves the stream without one
  int opened = 0;         // sink processes started, which tells their nodes apart
  auto open_out = [&](Out& o, std::string* e) -> bool {
    std::string desc;
    {
      std::lock_guard<std::mutex> lk(m);
      desc = label;
    }
    // The first sink's node has the stream's name, the others a number after it.
    const std::string node = opened++ ? node_name + "-" + std::to_string(opened) : node_name;
    std::vector<std::string> argv;
    auto c = std::make_unique<Child>();
    if (!sink_argv(o.sink, rate, ch, sp.latency_ms, node, desc, *tools, &argv, e) || !c->start(argv, true, nullptr, e))
      return false;
    set_process_fifo(c->pid(), kAudioRtPrio);
    // A small pipe: what sits in it is latency between a change (a gain, a tone's frequency) and
    // the speaker. 16 KiB is ~85 ms of 48 kHz stereo; the sink's own buffer does the rest.
    fcntl(c->in_fd(), F_SETPIPE_SZ, 16384);
    fcntl(c->in_fd(), F_SETFL, fcntl(c->in_fd(), F_GETFL) | O_NONBLOCK);
    if (o.sink.type == "pipewire") {
      const std::string h = wav_header(rate, ch);
      if (!write_all(c->in_fd(), h.data(), h.size(), stop)) {
        const int st = c->stop();
        *e = "the sink did not start: " + (c->reason().empty() ? "exit " + std::to_string(st) : c->reason());
        return false;
      }
    }
    o.proc = std::move(c);
    o.pending.clear();
    o.taken = 0;
    o.took_ms = mono_ms();
    o.stalled = false;
    set_sink(o.sink, "starting");
    return true;
  };
  const auto sink_name = [](const SinkSpec& k) { return k.target.empty() ? std::string("the default sink") : k.target; };
  const auto any_open = [&] {
    return std::any_of(outs.begin(), outs.end(), [](const std::unique_ptr<Out>& o) { return !!o->proc; });
  };
  // spec.sinks as the pump has them: a sink given up on leaves them.
  const auto sync_spec = [&] {
    std::lock_guard<std::mutex> lk(m);
    spec.sinks.clear();
    for (const auto& o : outs) spec.sinks.push_back(o->sink);
  };
  // A new sink, opened; one that would not open is said to have failed.
  const auto add_out = [&](const SinkSpec& k, std::string* e) {
    auto o = std::make_unique<Out>();
    o->sink = k;
    if (open_out(*o, e)) {
      outs.push_back(std::move(o));
      return;
    }
    LOG_WARN("audio: stream {} could not open {}: {}", id, sink_name(k), *e);
    set_sink(k, "failed", *e);
  };
  {
    std::string first_err;
    for (const SinkSpec& k : sp.sinks) {
      std::string e;
      add_out(k, &e);
      if (first_err.empty()) first_err = e;
    }
    // None of the sinks it was started with would open: the stream has failed. Some: it plays on those.
    if (!sp.sinks.empty() && outs.empty()) {
      src.stop();
      abandon(first_err);
      return;
    }
    if (outs.size() != sp.sinks.size()) sync_spec();
  }

  set_state("running");
  LevelMeter meter;
  size_t meter_frames = 0;
  double gain = db_to_gain(sp.gain_db);
  int64_t next_ms = mono_ms();
  int64_t heard_ms = mono_ms();
  bool ended = false;
  std::string fail;



  // A sink gone. The only one left of those the stream was started with, and nobody else
  // listening: the stream has failed (true). Otherwise it goes alone (a speaker turned off), and
  // the others play on.
  auto give_up = [&](size_t i, const std::string& why, const std::string& lost) -> bool {
    Out& o = *outs[i];
    bool heard;
    {
      std::lock_guard<std::mutex> lk(m);
      heard = !listeners.empty();
    }
    set_sink(o.sink, "failed", why.empty() ? lost : why);
    if (!switched && !heard && outs.size() == 1) {
      fail = lost;
      return true;
    }
    LOG_WARN("audio: stream {} lost its sink {}: {}", id, sink_name(o.sink), lost);
    if (o.proc) o.proc->stop();
    outs.erase(outs.begin() + static_cast<std::ptrdiff_t>(i));
    sync_spec();
    if (!any_open()) next_ms = heard_ms = mono_ms();
    return false;
  };

  // A sink whose process ended (a speaker gone, a codec change that made its node anew): given up
  // on, with what pw-cat said. Nothing reopens it: it is picked again by hand. True: the stream
  // has failed.
  auto lose = [&](size_t i) -> bool {
    Out& o = *outs[i];
    const int st = o.proc->stop();
    const std::string why = o.proc->reason();
    const std::string lost = o.proc->name() + " (the sink) ended" + (why.empty() ? " (exit " + std::to_string(st) + ")" : ": " + why);
    return give_up(i, why, lost);
  };

  // A URL stream's next decoder: track `to` of the playlist (which may have grown since the
  // start), the sink and the listeners carrying on. False past the end; *fail set if it did not
  // start.
  auto start_track = [&](int to, std::string* failed) -> bool {
    PlaylistItem it;
    {
      std::lock_guard<std::mutex> lk(m);
      if (to < 0 || to >= static_cast<int>(spec.source.playlist.size())) return false;
      it = spec.source.playlist[static_cast<size_t>(to)];
    }
    src.stop();
    pending.clear();
    strip = WavStripper{};
    const std::string url = resolve_playlist(it.url);
    std::vector<std::string> argv;
    std::string e;
    if (!decoder_argv(sp.source, url, rate, ch, *tools, &argv, &e, probe_audio_kind(sp.source, url, *tools, stop)) ||
        !src.start(argv, false, [this](const std::string& l) { on_source_line(l); }, &e)) {
      *failed = "track " + std::to_string(to + 1) + ": " + e;
      return true;
    }
    std::lock_guard<std::mutex> lk(m);
    track = to;
    track_frames = 0;
    resolved_url = url;
    meta_title = it.title;
    meta_name.clear();
    LOG_INFO("audio: stream {} track {} {}", id, to + 1, it.title);
    return true;
  };

  while (!stop.load()) {
    if (type == "url") {
      int to = -1;
      {
        std::lock_guard<std::mutex> lk(m);
        std::swap(to, jump_to);
      }
      if (to >= 0 && !start_track(to, &fail)) fail = "no track " + std::to_string(to + 1);
      if (!fail.empty()) break;
    }
    // Other sinks: the ones no longer wanted end, the new ones start, the others play on.
    if (sink_switch.exchange(false)) {
      std::vector<SinkSpec> want;
      const auto wanted = [&want](const SinkSpec& k) {
        return std::any_of(want.begin(), want.end(), [&k](const SinkSpec& w) { return same_sink(w, k); });
      };
      {
        std::lock_guard<std::mutex> lk(m);
        want = spec.sinks;
        sink_seq = sink_req;
        // The sinks lost before are forgotten with a new choice.
        sinks_status.erase(std::remove_if(sinks_status.begin(), sinks_status.end(),
                                          [&](const SinkStatus& x) { return !wanted(x.sink); }),
                           sinks_status.end());
      }
      switched = true;
      std::vector<std::unique_ptr<Out>> had;
      had.swap(outs);
      for (const SinkSpec& k : want) {
        const auto it = std::find_if(had.begin(), had.end(), [&k](const auto& o) { return o && same_sink(o->sink, k); });
        std::string e;
        if (it != had.end()) outs.push_back(std::move(*it));
        else add_out(k, &e);
      }
      for (const auto& o : had)
        if (o && o->proc) o->proc->stop();
      sync_spec();
      if (!any_open()) next_ms = heard_ms = mono_ms();
      std::string to;
      for (const auto& o : outs) to += (to.empty() ? "" : ", ") + sink_name(o->sink);
      LOG_INFO("audio: stream {} now plays to {}", id, to.empty() ? "its listeners only" : to);
    }
    // Live changes: the gain, a tone's frequencies and level, a noise's level.
    {
      std::lock_guard<std::mutex> lk(m);
      if (params_dirty) {
        params_dirty = false;
        gain = db_to_gain(spec.gain_db);
        if (auto* t = dynamic_cast<ToneGen*>(gen.get()))
          t->set(spec.source.freq, spec.source.freq_right, spec.source.level_db);
        if (auto* nz = dynamic_cast<NoiseGen*>(gen.get())) nz->set_level(spec.source.level_db);
      }
    }
    size_t got_frames = frames_per_chunk;
    bool hold;
    {
      std::lock_guard<std::mutex> lk(m);
      hold = paused;
    }
    if (hold) {
      // Paused: silence keeps the sink (a Bluetooth speaker) open and the clock running; the
      // decoder, not read, blocks on its full pipe and resumes where it was.
      std::fill(buf.begin(), buf.end(), 0);
    } else if (gen) {
      gen->fill(buf.data(), frames_per_chunk);
      if (sweep && sweep->done()) ended = true;
    } else {
      bool eof = false;
      std::string raw;
      while (pending.size() < chunk_bytes && !stop.load() && !eof) {
        raw.clear();
        read_some(src.out_fd(), &raw, chunk_bytes - pending.size(), stop, &eof);
        strip.feed(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), &pending);
      }
      if (stop.load()) break;
      const size_t usable = std::min(pending.size(), chunk_bytes) / (ch * sizeof(int16_t)) * (ch * sizeof(int16_t));
      std::memcpy(buf.data(), pending.data(), usable);
      pending.erase(0, usable);
      got_frames = usable / (ch * sizeof(int16_t));
      if (eof) {
        const int st = src.stop();
        const std::string why = src.last_line();
        if (type == "url" && st == 0) {
          // The end of a track: the next one, or the end of the stream.
          int next;
          {
            std::lock_guard<std::mutex> lk(m);
            next = order.next(spec.source.repeat_mode, false, spec.source.shuffle);
          }
          if (!start_track(next, &fail)) ended = true;
        } else {
          fail = src.name() + (type == "url" ? " stopped" : " ended") +
                 (why.empty() ? " (exit " + std::to_string(st) + ")" : ": " + why);
        }
      }
      if (got_frames == 0) {
        if (ended || !fail.empty() || src.out_fd() < 0) break;
        continue;  // the next track's decoder is starting
      }
    }

    apply_gain(buf.data(), got_frames * ch, gain);
    meter.add(buf.data(), got_frames, ch);
    meter_frames += got_frames;
    if (meter_frames >= level_frames) {
      meter_frames = 0;
      const LevelMeter::Reading r = meter.take();
      std::lock_guard<std::mutex> lk(m);
      level = r;
    }
    fan_out(buf.data(), got_frames, ch);
    {
      std::lock_guard<std::mutex> lk(m);
      frames += got_frames;
      if (!hold) track_frames += got_frames;
    }

    // Clocked by the pump when nothing else is real-time: no sink (or none that takes audio), and
    // a source that is not a capture (a generator would run flat out, a radio stream drain its
    // server's burst).
    bool held = false;  // every sink stalled
    if (any_open()) {
      const size_t n = got_frames * frame_bytes;
      // Each sink is given the chunk: one that plays may have some waiting (backlog); one opening or
      // stalled only when it took the last, so it starts with what plays then, not with old audio.
      for (const auto& o : outs) {
        if (!o->proc) continue;
        const bool plays = o->taken >= half_s && !o->stalled;
        if (o->pending.size() <= (plays ? backlog : 0)) o->pending.append(reinterpret_cast<const char*>(buf.data()), n);
      }
      // Written as each sink takes it, until one has taken all it was given. One that takes nothing
      // for 2 s (a Bluetooth link that stopped streaming) has stalled: it no longer holds the pump.
      std::vector<size_t> broken;
      for (;;) {
        bool done = false, waiting = false;
        std::vector<pollfd> pfs;
        const int64_t now = mono_ms();
        for (size_t k = 0; k < outs.size(); ++k) {
          Out& o = *outs[k];
          if (!o.proc) continue;
          size_t took = 0;
          if (!write_some(o.proc->in_fd(), &o.pending, &took)) {
            broken.push_back(k);
            continue;
          }
          if (took) {
            const bool was = o.taken >= half_s;
            o.taken += took;
            o.took_ms = now;
            if (o.stalled) {
              o.stalled = false;
              LOG_INFO("audio: stream {}: {} takes audio again", id, sink_name(o.sink));
              set_sink(o.sink, "playing");
            } else if (!was && o.taken >= half_s) {
              set_sink(o.sink, "playing");
            }
          } else if (!o.pending.empty() && !o.stalled && now - o.took_ms >= 2000) {
            o.stalled = true;
            LOG_WARN("audio: stream {}: {} has taken nothing for 2 s", id, sink_name(o.sink));
            set_sink(o.sink, "stalled", "it takes no audio");
          }
          if (o.pending.empty()) {
            done = true;
          } else {
            pfs.push_back(pollfd{o.proc->in_fd(), POLLOUT, 0});
            waiting = waiting || !o.stalled;
          }
        }
        if (done || !broken.empty()) break;
        if (!waiting) {
          held = true;
          break;
        }
        poll(pfs.data(), pfs.size(), kPollMs);
        if (stop.load() || sink_switch.load()) break;
      }
      if (stop.load()) break;
      for (size_t k = broken.size(); k-- > 0;)
        if (lose(broken[k])) break;
      if (!fail.empty()) break;
    }
    const bool clocked = (!any_open() || held) && type != "capture";
    if (clocked) {
      next_ms += static_cast<int64_t>(got_frames) * 1000 / rate;
      const int64_t now = mono_ms();
      // Behind by more than a few chunks (the board was busy): catch up by skipping, not racing.
      if (now - next_ms > 200) next_ms = now;
      else if (next_ms > now) std::this_thread::sleep_for(std::chrono::milliseconds(next_ms - now));
    }
    if (!any_open()) {
      std::lock_guard<std::mutex> lk(m);
      const int64_t now = mono_ms();
      if (!listeners.empty()) heard_ms = now;
      else if (now - heard_ms > kUnheardMs) {
        error = "ended: nobody listened for " + std::to_string(kUnheardMs / 1000) + " s";
        ended = true;
      }
    }
    if (ended || !fail.empty()) break;
  }

  // Sinks fed to the end get to play what they hold: EOF on their stdin, then they exit by themselves.
  if (ended)
    for (const auto& o : outs)
      if (o->proc) o->proc->close_stdin();
  src.stop();
  for (const auto& o : outs)
    if (o->proc) o->proc->stop();
  const bool stopped = stop.load();
  stop.store(true);  // ends the title poller too
  if (titles.joinable()) titles.join();
  if (stopped) set_state("stopped");
  else if (!fail.empty()) set_state("failed", fail);
  else set_state("ended");
  close_listeners();
  LOG_INFO("audio: stream {} ({}) {}", id, label, fail.empty() ? "ended" : fail);
}

// ---- the engine ---------------------------------------------------------------------------------

AudioEngine::AudioEngine(Publish publish, HasSubscribers has_subscribers, std::string data_dir)
    : publish_(std::move(publish)),
      has_subscribers_(std::move(has_subscribers)),
      data_dir_(std::move(data_dir)),
      tools_(Tools::find()) {
  publisher_ = std::thread([this] { run_publisher(); });
}

AudioEngine::~AudioEngine() {
  stop_all();
  running_.store(false);
  pub_cv_.notify_all();
  if (publisher_.joinable()) publisher_.join();
}

void AudioEngine::prune_locked() {
  std::vector<int> ended;
  for (const auto& [id, s] : streams_) {
    std::lock_guard<std::mutex> lk(s->m);
    if (s->state != "starting" && s->state != "running") ended.push_back(id);
  }
  while (ended.size() > kKeepEnded) {
    auto it = streams_.find(ended.front());
    if (it->second->thread.joinable()) it->second->thread.join();
    streams_.erase(it);
    ended.erase(ended.begin());
  }
}

bool AudioEngine::create(const json& body, json* out, std::string* err, int* status) {
  StreamSpec spec;
  if (!stream_spec_from_json(body, &spec, err)) {
    *status = 400;
    return false;
  }
  std::shared_ptr<Stream> s;
  // One local stream plays at a time: a new one takes the place of the one playing, which is
  // stopped first (its sinks free for the new one) and forgotten. Captures stay, any number.
  if (is_local(spec.source)) {
    std::vector<int> old;
    {
      std::lock_guard<std::mutex> lk(m_);
      for (const auto& [id, st] : streams_) {
        std::lock_guard<std::mutex> lk2(st->m);
        if ((st->state == "starting" || st->state == "running") && is_local(st->spec.source)) old.push_back(id);
      }
    }
    for (const int id : old) {
      LOG_INFO("audio: stream {} makes way for the new one", id);
      remove(id);
    }
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    prune_locked();
    size_t running = 0;
    for (const auto& [id, st] : streams_) {
      std::lock_guard<std::mutex> lk2(st->m);
      if (st->state == "starting" || st->state == "running") ++running;
    }
    if (running >= kMaxStreams) {
      *err = "at most " + std::to_string(kMaxStreams) + " streams run at once (the board has one core): stop one";
      *status = 409;
      return false;
    }
    s = std::make_shared<Stream>();
    s->id = next_id_++;
    s->spec = spec;
    s->order.reset(static_cast<int>(spec.source.playlist.size()), 0, spec.source.shuffle);
    s->label = spec.label.empty() ? default_label(spec) : spec.label;
    s->node_name = "btbench-stream-" + std::to_string(s->id);
    s->tools = &tools_;
    s->started_ms = wall_ms();
    streams_[s->id] = s;
  }
  Stream* raw = s.get();
  s->thread = std::thread([raw] { raw->run(); });
  LOG_INFO("audio: stream {}: {}", s->id, s->label);
  *out = s->to_json();
  pub_cv_.notify_all();
  return true;
}

json AudioEngine::list() const {
  std::vector<std::shared_ptr<Stream>> ss;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& [id, s] : streams_) ss.push_back(s);
  }
  json a = json::array();
  // Newest first: the one just started is the one being looked at.
  for (auto it = ss.rbegin(); it != ss.rend(); ++it) a.push_back((*it)->to_json());
  return json{{"streams", a}, {"max", kMaxStreams}};
}

json AudioEngine::get(int id) const {
  std::lock_guard<std::mutex> lk(m_);
  const auto it = streams_.find(id);
  return it == streams_.end() ? json(nullptr) : it->second->to_json();
}

bool AudioEngine::update(int id, const json& body, json* out, std::string* err, int* status) {
  std::shared_ptr<Stream> s;
  {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = streams_.find(id);
    if (it == streams_.end()) {
      *status = 404;
      *err = "no such stream";
      return false;
    }
    s = it->second;
  }
  try {
    std::lock_guard<std::mutex> lk(s->m);
    StreamSpec sp = s->spec;
    if (body.contains("gain_db")) sp.gain_db = std::clamp(num(body, "gain_db", 0), -60.0, 20.0);
    const bool list_op = body.contains("next") || body.contains("previous") || body.contains("track") || body.contains("enqueue");
    if (list_op && sp.source.type != "url") throw std::invalid_argument("only a URL stream has a playlist");
    if (body.contains("enqueue")) {
      if (!body["enqueue"].is_array()) throw std::invalid_argument("enqueue is an array of {url, title}");
      for (const json& it : body["enqueue"]) {
        if (sp.source.playlist.size() >= kMaxPlaylist) throw std::invalid_argument("the playlist is full");
        sp.source.playlist.push_back(playlist_item(it));
      }
    }
    if (body.contains("repeat") || body.contains("shuffle")) {
      if (sp.source.type != "url") throw std::invalid_argument("only a URL stream repeats and shuffles its playlist");
      sp.source.repeat_mode = repeat_mode(body, sp.source.repeat_mode);
      if (body.contains("shuffle")) {
        if (!body["shuffle"].is_boolean()) throw std::invalid_argument("shuffle is true or false");
        sp.source.shuffle = body["shuffle"].get<bool>();
      }
    }
    const bool sinks = body.contains("sinks") || body.contains("sink");
    if (sinks) {
      sp.sinks = sinks_spec(body);
      if (s->state != "starting" && s->state != "running") throw std::invalid_argument("the stream has stopped");
    }
    if (body.contains("paused")) {
      if (sp.source.type != "url") throw std::invalid_argument("only a URL stream pauses");
      s->paused = body["paused"].get<bool>();
    }
    const int n = static_cast<int>(sp.source.playlist.size());
    if (sp.source.type == "url") {
      // Shuffle switched: a new order from the track playing. Tracks enqueued: into the order.
      if (sp.source.shuffle != s->spec.source.shuffle) s->order.reset(n, s->track, sp.source.shuffle);
      else s->order.grow(n, sp.source.shuffle);
    }
    if (body.value("next", false)) s->jump_to = s->order.next(sp.source.repeat_mode, true, sp.source.shuffle);
    if (body.value("previous", false)) s->jump_to = s->order.previous(sp.source.repeat_mode);
    if (body.contains("track")) {
      if (!body["track"].is_number_integer()) throw std::invalid_argument("track is a number");
      const int t = body["track"].get<int>();
      if (t < 0 || t >= n) throw std::invalid_argument("track is 0.." + std::to_string(n - 1));
      s->order.go(t);
      s->jump_to = t;
    }
    if (body.contains("source")) {
      const json& sj = body["source"];
      const double nyquist = sp.rate / 2.0;
      if (sp.source.type == "tone") {
        sp.source.freq = std::clamp(num(sj, "freq", sp.source.freq), 1.0, nyquist);
        if (sj.contains("freq_right"))
          sp.source.freq_right = sj["freq_right"].is_null() ? 0.0 : std::clamp(num(sj, "freq_right", 0), 1.0, nyquist);
        sp.source.level_db = std::clamp(num(sj, "level_db", sp.source.level_db), -96.0, 0.0);
      } else if (sp.source.type == "noise") {
        sp.source.level_db = std::clamp(num(sj, "level_db", sp.source.level_db), -96.0, 0.0);
      } else {
        throw std::invalid_argument("only a tone's freq/freq_right/level_db and a noise's level_db change live; "
                                    "start a new stream for anything else");
      }
    }
    const bool relabel = s->label == default_label(s->spec);
    s->spec = sp;
    if (relabel) s->label = default_label(sp);
    s->params_dirty = true;
    // The pump picks the new sinks up from the spec, as a whole request that was valid.
    if (sinks) {
      ++s->sink_req;
      s->sink_switch.store(true);
    }
  } catch (const std::exception& e) {
    *status = 400;
    *err = e.what();
    return false;
  }
  *out = s->to_json();
  return true;
}

bool AudioEngine::remove(int id) {
  std::shared_ptr<Stream> s;
  {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = streams_.find(id);
    if (it == streams_.end()) return false;
    s = it->second;
    streams_.erase(it);
  }
  s->stop.store(true);
  if (s->thread.joinable()) s->thread.join();
  pub_cv_.notify_all();
  return true;
}

void AudioEngine::stop_all() {
  std::map<int, std::shared_ptr<Stream>> all;
  {
    std::lock_guard<std::mutex> lk(m_);
    all.swap(streams_);
  }
  for (auto& [id, s] : all) s->stop.store(true);
  for (auto& [id, s] : all)
    if (s->thread.joinable()) s->thread.join();
}

AudioEngine::ListenerPtr AudioEngine::listen(int id, bool mono, std::string* err, int* status) {
  std::shared_ptr<Stream> s;
  {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = streams_.find(id);
    if (it == streams_.end()) {
      *status = 404;
      *err = "no such stream";
      return nullptr;
    }
    s = it->second;
  }
  std::lock_guard<std::mutex> lk(s->m);
  if (s->state != "starting" && s->state != "running") {
    *status = 409;
    *err = "the stream has " + s->state;
    return nullptr;
  }
  if (s->listeners.size() >= kMaxListeners) {
    *status = 503;
    *err = "at most " + std::to_string(kMaxListeners) + " listeners a stream (each holds an HTTP worker)";
    return nullptr;
  }
  auto l = std::make_shared<Listener>();
  l->mono = mono && s->spec.channels == 2;
  const int ch = l->mono ? 1 : s->spec.channels;
  l->max_queued = static_cast<size_t>(s->spec.rate) * ch * sizeof(int16_t);  // one second
  l->q.push_back(wav_header(s->spec.rate, ch));
  l->queued = l->q.back().size();
  s->listeners.push_back(l);
  return l;
}

void AudioEngine::unlisten(int id, const ListenerPtr& l) {
  std::shared_ptr<Stream> s;
  {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = streams_.find(id);
    if (it != streams_.end()) s = it->second;
  }
  {
    std::lock_guard<std::mutex> lk(l->m);
    l->closed = true;
  }
  if (!s) return;
  std::lock_guard<std::mutex> lk(s->m);
  s->listeners.erase(std::remove(s->listeners.begin(), s->listeners.end(), l), s->listeners.end());
}

bool AudioEngine::take(Listener& l, std::string* out, int timeout_ms) {
  out->clear();
  std::unique_lock<std::mutex> lk(l.m);
  l.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&l] { return !l.q.empty() || l.closed; });
  while (!l.q.empty()) {
    *out += l.q.front();
    l.queued -= l.q.front().size();
    l.q.pop_front();
  }
  return !(l.closed && out->empty());
}

json AudioEngine::endpoints(bool fresh) {
  std::lock_guard<std::mutex> lk(ep_m_);
  const int64_t now = mono_ms();
  if (!fresh && !ep_cache_.is_null() && now - ep_at_ms_ < 2000) return ep_cache_;
  json out;
  std::string error;
  bool have_pw = false;
  if (!tools_.pw_dump.empty()) {
    // pw-dumps run below the audio (util/prio.h): one costs a Zero W about a second, and the
    // console asks for this on every visit to the player.
    const ExecResult r = run_cmd({"nice", "-n", std::to_string(kBackgroundNice), tools_.pw_dump}, 5000, 8 << 20);
    if (r.ok()) {
      try {
        out = endpoints_from_pw_dump(parse_pw_dump(r.out));
        have_pw = true;
      } catch (const std::exception& e) {
        error = std::string("pw-dump: ") + e.what();
      }
    } else {
      error = "PipeWire is not running (" + r.reason() + ")";
    }
  }
  if (!have_pw) out = endpoints_from_alsa(file_exists("/proc/asound/Loopback"));
  out["backend"] = have_pw ? "pipewire" : (out["sinks"].empty() && out["sources"].empty() ? "none" : "alsa");
  out["error"] = have_pw ? "" : error;
  out["tools"] = tools_.to_json();
  ep_cache_ = out;
  // As old as it is when it is ready: a busy Zero W takes seconds for a pw-dump, which would have
  // made every answer stale on arrival and every request another dump.
  ep_at_ms_ = mono_ms();
  return out;
}

// ---- codec picker -------------------------------------------------------------------------------

json AudioEngine::devices() {
  // One pw-dump at a time, and its answer reused for two seconds: every console on the Audio tab
  // asks on each media update, and on a Zero W a pw-dump per request piles up to the HTTP pool's
  // size in concurrent ones (seen: load 30, the board unreachable).
  std::lock_guard<std::mutex> lk(dev_m_);
  const int64_t now = mono_ms();
  if (!dev_cache_.is_null() && now - dev_at_ms_ < 2000) return dev_cache_;
  dev_cache_ = devices_uncached();
  dev_at_ms_ = mono_ms();
  return dev_cache_;
}

json AudioEngine::devices_uncached() {
  if (tools_.pw_dump.empty())
    return json{{"backend", "none"}, {"devices", json::array()},
                {"error", "codecs are picked through PipeWire, which is not installed"}};
  const ExecResult r = run_cmd({"nice", "-n", std::to_string(kBackgroundNice), tools_.pw_dump}, 5000, 8 << 20);
  if (!r.ok())
    return json{{"backend", "none"}, {"devices", json::array()},
                {"error", "PipeWire is not running (" + r.reason() + "): codecs are picked in pipewire mode"}};
  try {
    json out = cards_from_pw_dump(parse_pw_dump(r.out));
    out["backend"] = "pipewire";
    out["error"] = "";
    return out;
  } catch (const std::exception& e) {
    return json{{"backend", "pipewire"}, {"devices", json::array()}, {"error", std::string("pw-dump: ") + e.what()}};
  }
}

bool AudioEngine::set_profile(int device_id, const json& profile, json* out, std::string* err,
                              int* status) {
  if (tools_.wpctl.empty()) {
    *status = 503;
    *err = "wpctl is not installed";
    return false;
  }
  json all;
  {
    std::lock_guard<std::mutex> lk(dev_m_);
    all = dev_cache_ = devices_uncached();
    dev_at_ms_ = mono_ms();
  }
  const json* dev = nullptr;
  for (const json& d : all["devices"])
    if (d["id"] == device_id) dev = &d;
  if (!dev) {
    *status = 404;
    *err = all["error"].get<std::string>().empty() ? "no such Bluetooth device" : all["error"].get<std::string>();
    return false;
  }
  // By index or by name, but only one of this device's own: wpctl would take any number.
  const json* pick = nullptr;
  for (const json& p : (*dev)["profiles"]) {
    if ((profile.is_number_integer() && p["index"] == profile) || (profile.is_string() && p["name"] == profile))
      pick = &p;
  }
  if (!pick) {
    *status = 400;
    *err = "the device has no such profile (GET /api/audio/devices lists them)";
    return false;
  }
  const ExecResult r = run_cmd({tools_.wpctl, "set-profile", std::to_string(device_id),
                                std::to_string((*pick)["index"].get<long long>())}, 5000);
  if (!r.ok()) {
    *status = 502;
    *err = "wpctl set-profile: " + r.reason();
    return false;
  }
  LOG_INFO("audio: device {} ({}) → profile {}", device_id, (*dev)["label"].get<std::string>(),
           (*pick)["name"].get<std::string>());
  {
    std::lock_guard<std::mutex> lk(ep_m_);
    ep_cache_ = nullptr;  // the endpoints change with the profile
  }
  // The switch renegotiates the stream with the device (an AVDTP reconfiguration: a second or two
  // on a Zero W, six seen with a desktop as the peer). The answer is the device once its active
  // profile is the new one, or as it is after ~10 s when it never gets there (the device refused;
  // the old profile stays).
  const json want = (*pick)["name"];
  // Twenty tries: each pw-dump itself takes a third of a second on the board.
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Held by name: a range-for over devices()["devices"] would walk a destroyed temporary.
    json now;
    {
      std::lock_guard<std::mutex> lk(dev_m_);
      now = dev_cache_ = devices_uncached();
      dev_at_ms_ = mono_ms();
    }
    for (const json& d : now["devices"])
      if (d["id"] == device_id) *out = d;
    if (out->is_object() && (*out)["active"].is_object() && (*out)["active"]["name"] == want) return true;
  }
  if (out->is_null()) *out = *dev;
  (*out)["switch_error"] = "the device is still on " +
                           ((*out)["active"].is_object() ? (*out)["active"]["name"].get<std::string>() : std::string("no profile")) +
                           ": it did not accept " + want.get<std::string>() + " (yet)";
  return true;
}

// ---- radio stations -----------------------------------------------------------------------------

namespace {

// A starter list: long-running, listener-supported MP3 streams that mpg123 plays as they are.
json default_stations() {
  return json::array({
      json{{"name", "SomaFM Groove Salad"}, {"url", "https://ice1.somafm.com/groovesalad-128-mp3"}},
      json{{"name", "SomaFM Drone Zone"}, {"url", "https://ice1.somafm.com/dronezone-128-mp3"}},
      json{{"name", "SomaFM Secret Agent"}, {"url", "https://ice1.somafm.com/secretagent-128-mp3"}},
      json{{"name", "Radio Paradise (MP3 128k)"}, {"url", "http://stream.radioparadise.com/mp3-128"}},
  });
}

}  // namespace

json AudioEngine::radio() const {
  std::ifstream f(data_dir_ + "/radio.json");
  if (f) {
    try {
      json j = json::parse(f);
      if (j.contains("stations") && j["stations"].is_array()) return json{{"stations", j["stations"]}, {"saved", true}};
    } catch (const std::exception&) {
      // A broken file is replaced by the next save.
    }
  }
  return json{{"stations", default_stations()}, {"saved", false}};
}

bool AudioEngine::set_radio(const json& body, std::string* err) {
  if (!body.contains("stations") || !body["stations"].is_array()) {
    *err = "body must be {\"stations\": [{\"name\", \"url\"}]}";
    return false;
  }
  json clean = json::array();
  for (const json& s : body["stations"]) {
    if (!s.is_object() || !s.contains("url") || !s["url"].is_string()) {
      *err = "each station needs a url";
      return false;
    }
    const std::string url = trim(s["url"].get<std::string>());
    if (!url_ok(url)) {
      *err = "not an http(s) URL: " + url;
      return false;
    }
    std::string name = s.contains("name") && s["name"].is_string() ? trim(s["name"].get<std::string>()) : "";
    if (name.empty()) name = station_name(tools_.curl, url);
    clean.push_back(json{{"name", name.substr(0, 120)}, {"url", url}});
    if (clean.size() >= 100) break;
  }
  const std::string path = data_dir_ + "/radio.json";
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp);
    if (!f) {
      *err = "cannot write " + tmp;
      return false;
    }
    f << json{{"stations", clean}}.dump(1) << "\n";
  }
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    *err = "cannot write " + path + ": " + strerror(errno);
    return false;
  }
  return true;
}

// The "audio.streams" topic: the list (state, levels, titles) four times a second while anyone
// watches and anything runs; once more when the last stream ends, so a console sees it stop.
void AudioEngine::run_publisher() {
  bool was_active = false;
  while (running_.load()) {
    {
      std::unique_lock<std::mutex> lk(pub_m_);
      pub_cv_.wait_for(lk, std::chrono::milliseconds(kLevelMs));
    }
    if (!running_.load()) break;
    if (!has_subscribers_ || !has_subscribers_("audio.streams")) {
      was_active = false;
      continue;
    }
    bool active = false;
    {
      std::lock_guard<std::mutex> lk(m_);
      for (const auto& [id, s] : streams_) {
        std::lock_guard<std::mutex> lk2(s->m);
        if (s->state == "starting" || s->state == "running") active = true;
      }
    }
    if (active || was_active) publish_("audio.streams", list());
    was_active = active;
  }
}

}  // namespace btb::audio
