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

// Which decoder a URL goes to with decoder "auto": mpg123 for what is (most likely) MP3 — most
// internet radio, and mpg123 is light and reports ICY titles — ffmpeg for anything else it can
// tell, or when there is no mpg123.
std::string pick_decoder(const SourceSpec& s, const std::string& url, const Tools& t) {
  if (s.decoder != "auto") return s.decoder;
  std::string path = lower(url);
  const size_t q = path.find_first_of("?#");
  if (q != std::string::npos) path.resize(q);
  static const char* const kNotMp3[] = {".aac", ".m4a", ".mp4", ".ogg", ".oga", ".opus",
                                        ".flac", ".wav", ".m3u8", ".wma", ".aiff"};
  bool not_mp3 = false;
  for (const char* e : kNotMp3)
    if (ends_with(path, e)) not_mp3 = true;
  if (!t.mpg123.empty() && !(not_mp3 && !t.ffmpeg.empty())) return "mpg123";
  if (!t.ffmpeg.empty()) return "ffmpeg";
  return "mpg123";
}

}  // namespace

// ---- Tools --------------------------------------------------------------------------------------

Tools Tools::find() {
  Tools t;
  t.pw_cat = find_exec("pw-cat");
  t.pw_dump = find_exec("pw-dump");
  t.aplay = find_exec("aplay");
  t.arecord = find_exec("arecord");
  t.mpg123 = find_exec("mpg123");
  t.ffmpeg = find_exec("ffmpeg");
  t.curl = find_exec("curl");
  return t;
}

json Tools::to_json() const {
  return nlohmann::json{{"pw-cat", !pw_cat.empty()},   {"pw-dump", !pw_dump.empty()},
                        {"aplay", !aplay.empty()},     {"arecord", !arecord.empty()},
                        {"mpg123", !mpg123.empty()},   {"ffmpeg", !ffmpeg.empty()},
                        {"curl", !curl.empty()}};
}

// ---- specs --------------------------------------------------------------------------------------

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
      if (!url_ok(src.url)) throw std::invalid_argument("url must be an http:// or https:// URL");
      src.decoder = str(sj, "decoder", "auto");
      if (src.decoder != "auto" && src.decoder != "mpg123" && src.decoder != "ffmpeg")
        throw std::invalid_argument("decoder is auto, mpg123 or ffmpeg");
      src.title = str(sj, "title", "").substr(0, 120);
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

    if (j.contains("sink") && !j["sink"].is_null()) {
      if (!j["sink"].is_object()) throw std::invalid_argument("sink must be an object or null");
      const json& kj = j["sink"];
      s.sink.type = str(kj, "type", "pipewire");
      s.sink.target = str(kj, "target", "");
      if (s.sink.type != "none" && s.sink.type != "pipewire" && s.sink.type != "alsa")
        throw std::invalid_argument("sink type is pipewire, alsa or none");
      if (!s.sink.target.empty() && !endpoint_id_ok(s.sink.target)) throw std::invalid_argument("bad sink target");
      if (s.sink.type == "alsa" && s.sink.target.empty()) throw std::invalid_argument("an ALSA sink needs a target device");
    }
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
    sj.update(json{{"url", src.url}, {"decoder", src.decoder}, {"title", src.title}});
  } else if (src.type == "capture") {
    sj.update(json{{"backend", src.backend}, {"target", src.target}, {"monitor", src.monitor},
                   {"title", src.title}});
  }
  return json{{"source", sj},
              {"sink", s.sink.type == "none" ? json(nullptr)
                                             : json{{"type", s.sink.type}, {"target", s.sink.target}}},
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
  const std::string to = s.sink.type == "none" ? "listeners" : (s.sink.target.empty() ? "default sink" : s.sink.target);
  return what + " → " + to;
}

bool decoder_argv(const SourceSpec& s, const std::string& url, int rate, int channels,
                  const Tools& t, std::vector<std::string>* argv, std::string* err) {
  const std::string dec = pick_decoder(s, url, t);
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
  *argv = {t.pw_cat, "--playback", "--latency", std::to_string(latency_ms) + "ms", "--media-role", "Music",
           "-P", "{ node.name = \"" + node_name + "\" node.description = \"" + prop_text(description) +
                     "\" media.name = \"" + prop_text(description) + "\" }"};
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

  int in_fd() const { return p_.in_fd; }
  int out_fd() const { return p_.out_fd; }
  const std::string& name() const { return name_; }

  std::string last_line() const {
    std::lock_guard<std::mutex> lk(m_);
    return last_;
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
  std::string last_;
  int status_ = -1;
};

// Writes all of [p, p+n) to a non-blocking fd, waiting as the reader drains it. False on a broken
// pipe (the reader exited) or when `stop` was raised.
bool write_all(int fd, const char* p, size_t n, const std::atomic<bool>& stop) {
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
    if (stop.load()) return false;
    if (r > 0 && (pf.revents & (POLLERR | POLLHUP)) && !(pf.revents & POLLOUT)) return false;
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

// The current StreamTitle of an ICY stream, read on a connection of its own: for a stream ffmpeg
// decodes, which does not report title changes. Reads one metadata block's worth (tens of KB) and
// hangs up. "" when the server sends no ICY metadata.
std::string fetch_icy_title(const std::string& curl, const std::string& url, const std::atomic<bool>& stop) {
  Spawned p;
  std::string err;
  if (!spawn({curl, "-sSL", "-A", "btbenchd/1.0", "--max-time", "10", "-H", "Icy-MetaData: 1", "-D", "/dev/stderr", url}, &p, &err))
    return {};
  std::string headers, body, title;
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
      (i == 0 ? body : headers).append(buf, static_cast<size_t>(n));
    }
    if (body.size() >= need) {
      if (icy_title_from(headers, body, &title, &need) || need == 0) break;
    }
  }
  close(p.out_fd);
  close(p.err_fd);
  kill_group(p.pid, 200);
  waitpid(p.pid, nullptr, 0);
  return title;
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
  int64_t started_ms = 0, ended_ms = 0;
  uint64_t frames = 0;
  bool params_dirty = false;
  LevelMeter::Reading level;
  std::vector<ListenerPtr> listeners;

  json to_json() const {
    std::lock_guard<std::mutex> lk(m);
    json lv{{"peak_db", json::array()}, {"rms_db", json::array()}, {"clipped", level.clipped}};
    for (int c = 0; c < level.channels; ++c) {
      lv["peak_db"].push_back(std::round(level.peak_db[c] * 10) / 10);
      lv["rms_db"].push_back(std::round(level.rms_db[c] * 10) / 10);
    }
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
                  {"level", lv},
                  {"meta", json{{"name", meta_name}, {"title", meta_title}, {"url", resolved_url}}}});
    return j;
  }

  void set_state(const std::string& s, const std::string& e = "") {
    std::lock_guard<std::mutex> lk(m);
    if (state == "failed" || state == "stopped" || state == "ended") return;
    state = s;
    if (!e.empty()) error = e;
    if (s != "running" && s != "starting") ended_ms = wall_ms();
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
    }
  }

  std::string resolve_playlist(const std::string& url) {
    if (!looks_like_playlist(url)) return url;
    if (tools->curl.empty()) return url;
    const ExecResult r = run_cmd({tools->curl, "-sSL", "-A", "btbenchd/1.0", "--max-time", "8", "--max-filesize", "65536", url}, 10000, 65536);
    if (!r.ok()) return url;
    const std::string first = playlist_first_url(r.out);
    return first.empty() || !url_ok(first) ? url : first;
  }

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
      }
      ok = decoder_argv(sp.source, url, rate, ch, *tools, &argv, &err);
      // ffmpeg does not report title changes, and mpg123 may not (its https helper): ask the
      // server every 15 s, unless the decoder has reported a title within the last 30 s.
      if (ok && !tools->curl.empty() && lower(url).find(".m3u8") == std::string::npos) {
        titles = std::thread([this, url] {
          for (int64_t next = mono_ms() + 2000; !stop.load(); std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs))) {
            if (mono_ms() < next) continue;
            next = mono_ms() + 15000;
            if (mono_ms() - decoder_title_ms.load() < 30000) continue;
            const std::string t = fetch_icy_title(tools->curl, url, stop);
            if (!t.empty()) {
              std::lock_guard<std::mutex> lk(m);
              meta_title = t;
            }
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
  }

  // The sink.
  Child snk;
  if (sp.sink.type != "none") {
    std::vector<std::string> argv;
    if (!sink_argv(sp.sink, rate, ch, sp.latency_ms, node_name, label, *tools, &argv, &err) ||
        !snk.start(argv, true, nullptr, &err)) {
      src.stop();
      abandon(err);
      return;
    }
    // A small pipe: what sits in it is latency between a change (a gain, a tone's frequency) and
    // the speaker. 16 KiB is ~85 ms of 48 kHz stereo; the sink's own buffer does the rest.
    fcntl(snk.in_fd(), F_SETPIPE_SZ, 16384);
    fcntl(snk.in_fd(), F_SETFL, fcntl(snk.in_fd(), F_GETFL) | O_NONBLOCK);
    if (sp.sink.type == "pipewire") {
      const std::string h = wav_header(rate, ch);
      if (!write_all(snk.in_fd(), h.data(), h.size(), stop)) {
        src.stop();
        const int st = snk.stop();
        abandon("the sink did not start: " + (snk.last_line().empty() ? "exit " + std::to_string(st) : snk.last_line()));
        return;
      }
    }
  }

  // Clocked by the pump when nothing else is real-time: no sink, and a source that is not a
  // capture (a generator would run flat out, a radio stream drain its server's burst).
  const bool clocked = sp.sink.type == "none" && type != "capture";
  set_state("running");
  LevelMeter meter;
  size_t meter_frames = 0;
  double gain = db_to_gain(sp.gain_db);
  int64_t next_ms = mono_ms();
  int64_t heard_ms = mono_ms();
  bool ended = false;
  std::string fail;

  while (!stop.load()) {
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
    if (gen) {
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
          ended = true;
        } else {
          fail = src.name() + (type == "url" ? " stopped" : " ended") +
                 (why.empty() ? " (exit " + std::to_string(st) + ")" : ": " + why);
        }
      }
      if (got_frames == 0) break;
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
    }

    if (sp.sink.type != "none") {
      if (!write_all(snk.in_fd(), reinterpret_cast<const char*>(buf.data()), got_frames * ch * sizeof(int16_t), stop)) {
        if (stop.load()) break;
        const int st = snk.stop();
        const std::string why = snk.last_line();
        fail = snk.name() + " (the sink) ended" + (why.empty() ? " (exit " + std::to_string(st) + ")" : ": " + why);
        break;
      }
    } else if (clocked) {
      next_ms += static_cast<int64_t>(got_frames) * 1000 / rate;
      const int64_t now = mono_ms();
      // Behind by more than a few chunks (the board was busy): catch up by skipping, not racing.
      if (now - next_ms > 200) next_ms = now;
      else if (next_ms > now) std::this_thread::sleep_for(std::chrono::milliseconds(next_ms - now));
    }
    if (sp.sink.type == "none") {
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

  // A sink fed to the end gets to play what it holds: EOF on its stdin, then it exits by itself.
  if (ended && sp.sink.type != "none" && snk.in_fd() >= 0) {
    close(snk.in_fd());
  }
  src.stop();
  snk.stop();
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

AudioEngine::AudioEngine(Publish publish, HasSubscribers has_subscribers, std::string data_dir,
                         std::function<json()> media)
    : publish_(std::move(publish)),
      has_subscribers_(std::move(has_subscribers)),
      data_dir_(std::move(data_dir)),
      media_(std::move(media)),
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
    const ExecResult r = run_cmd({tools_.pw_dump}, 5000, 8 << 20);
    if (r.ok()) {
      try {
        out = endpoints_from_pw_dump(json::parse(r.out));
        have_pw = true;
      } catch (const std::exception& e) {
        error = std::string("pw-dump: ") + e.what();
      }
    } else {
      error = "PipeWire is not running (" + r.reason() + ")";
    }
  }
  if (!have_pw) out = endpoints_from_alsa(media_ ? media_() : json::object(), file_exists("/proc/asound/Loopback"));
  out["backend"] = have_pw ? "pipewire" : (out["sinks"].empty() && out["sources"].empty() ? "none" : "alsa");
  out["error"] = have_pw ? "" : error;
  out["tools"] = tools_.to_json();
  ep_cache_ = out;
  ep_at_ms_ = now;
  return out;
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
    clean.push_back(json{{"name", s.value("name", url).substr(0, 120)}, {"url", url}});
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
