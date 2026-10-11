#pragma once

// Audio streams: something to play (a test signal, an internet radio or UPnP URL, or what a
// Bluetooth device sends the board) pumped into somewhere to play it (a Bluetooth speaker or
// headset through PipeWire, the loopback card) and/or to the browsers listening in
// (GET /api/audio/streams/{id}/listen). See docs/api.md "Audio streams".
//
// Each stream is one pump thread moving 20 ms chunks of S16LE from its source to its sink:
//
//   generator ─┐                                   ┌─► sink process (pw-cat --playback, aplay)
//   decoder  ──┼─► gain ─► level meter ─► chunk ───┤
//   capture  ──┘   (mpg123/ffmpeg, pw-cat --record,└─► listeners (browsers, curl | aplay)
//                   arecord: child processes)
//
// The tools do the stack-specific work — PipeWire's graph, ALSA devices, HTTP and codecs — so the
// daemon links none of them, and a stream is the same whether PipeWire runs or not (stopped, only
// the plain ALSA devices, i.e. the loopback card, are there to play into).
// What paces a stream is whatever is real-time in it: the sink consuming, a capture producing,
// or, for a generator or a decoder heard only by listeners, the pump's own clock.
//
// The Zero W has one core: at most kMaxStreams run, a stream nobody plays or hears still costs its
// pump, and nothing is computed for the "audio.streams" topic while nobody is subscribed.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audio/upnp.h"

namespace btb::audio {

// The programs the streams run, found once on PATH. Empty when missing.
struct Tools {
  std::string pw_cat, pw_dump, wpctl, aplay, arecord, mpg123, ffmpeg, curl;
  static Tools find();
  nlohmann::json to_json() const;
};

// A track of a URL stream's playlist. What a media server said about it besides its URL rides
// along for the remotes (the board's player tells AVRCP the artist, album and length).
struct PlaylistItem {
  std::string url;
  std::string title;
  std::string artist, album;
  uint32_t duration_ms = 0;  // 0: unknown
};

// The order a URL stream's playlist plays in: as listed, or shuffled (each track once, the one
// playing first); and what comes next at a track's end or on next/previous, with repeat off, of
// the one track, or of all. Pure: the pump asks it, the tests drive it with a fixed seed.
class PlayOrder {
 public:
  explicit PlayOrder(uint32_t seed = 1) : rng_(seed) {}
  // Starts over with `n` tracks, `current` playing.
  void reset(int n, int current, bool shuffle);
  // Tracks appended up to `n`: at the end as listed, at random places after the current shuffled.
  void grow(int n, bool shuffle);
  // The track after the current one, made current: at its end (`asked` false) or on next. -1 at
  // the end of the list with repeat off (on next: the current track again).
  int next(const std::string& repeat, bool asked, bool shuffle);
  int previous(const std::string& repeat);
  void go(int track);  // a track picked by its number
  const std::vector<int>& order() const { return order_; }

 private:
  std::vector<int> order_;
  size_t at_ = 0;
  std::mt19937 rng_;
};

struct SourceSpec {
  std::string type = "tone";  // tone | sweep | noise | silence | url | capture
  // tone
  double freq = 1000, freq_right = 0;  // 0: the right channel as the left
  double level_db = -12;               // tone/sweep: peak dBFS; noise: RMS dBFS
  // sweep
  double from = 20, to = 20000, seconds = 10;
  bool log = true, repeat = true;
  // noise
  std::string color = "pink";
  // url: an internet radio stream (or its .pls/.m3u playlist), or a UPnP media item
  std::string url;
  std::string decoder = "auto";  // auto | mpg123 | ffmpeg
  std::string title;             // what the console calls it (a station's name), for the label
  // url: the tracks to play one after another (UPnP files, a list of URLs); the first is `url`.
  // One item for a radio. It can grow while the stream runs (PUT {"enqueue": [...]}).
  std::vector<PlaylistItem> playlist;
  std::string repeat_mode = "off";  // off | track | all ("repeat" in the JSON; a sweep's is a bool)
  bool shuffle = false;
  // capture: an endpoint id (GET /api/audio/endpoints), "" for PipeWire's default source
  std::string backend = "pipewire";  // pipewire | alsa
  std::string target;
  bool monitor = false;  // PipeWire: capture what goes *to* sink `target` (its monitor)
};

struct SinkSpec {
  std::string type = "none";  // none | pipewire | alsa
  std::string target;         // endpoint id; "" for PipeWire's default sink
};

struct StreamSpec {
  SourceSpec source;
  SinkSpec sink;
  int rate = 48000;
  int channels = 2;
  double gain_db = 0;
  int latency_ms = 60;  // what the sink process is asked to buffer
  std::string label;    // "" → made up from the source and the sink
};

// The body of POST /api/audio/streams, checked: everything that reaches a command line is
// validated here (URL scheme and characters, endpoint ids), and numbers are clamped to what makes
// sense for the rate. False with a reason for the 400.
bool stream_spec_from_json(const nlohmann::json& j, StreamSpec* out, std::string* err);
nlohmann::json stream_spec_json(const StreamSpec& s);
std::string default_label(const StreamSpec& s);

// The command lines (pure, tested). `node_name` names the PipeWire stream ("btbench-stream-3"), so
// it can be told apart in wpctl / pw-top.
bool decoder_argv(const SourceSpec& s, const std::string& url, int rate, int channels,
                  const Tools& t, std::vector<std::string>* argv, std::string* err);
bool capture_argv(const SourceSpec& s, int rate, int channels, int latency_ms,
                  const std::string& node_name, const Tools& t, std::vector<std::string>* argv,
                  std::string* err);
bool sink_argv(const SinkSpec& s, int rate, int channels, int latency_ms,
               const std::string& node_name, const std::string& description, const Tools& t,
               std::vector<std::string>* argv, std::string* err);

class AudioEngine {
 public:
  using Publish = std::function<void(const std::string& topic, const nlohmann::json& data)>;
  using HasSubscribers = std::function<bool(const std::string& topic)>;
  AudioEngine(Publish publish, HasSubscribers has_subscribers, std::string data_dir);
  ~AudioEngine();
  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  static constexpr size_t kMaxStreams = 4;
  static constexpr size_t kKeepEnded = 6;
  static constexpr size_t kMaxListeners = 3;  // per stream; each holds an HTTP worker

  // POST /api/audio/streams: 201 with the stream, or *status and *err.
  bool create(const nlohmann::json& body, nlohmann::json* out, std::string* err, int* status);
  // {streams:[...], max}
  nlohmann::json list() const;
  nlohmann::json get(int id) const;  // null when there is no such stream
  // PUT: gain_db at any time; a tone's freq/freq_right/level_db and a noise's level_db live; for a
  // URL stream, its playlist: {"next": true}, {"previous": true}, {"track": n}, {"enqueue": [...]},
  // {"repeat": "off|track|all"}, {"shuffle": bool}.
  bool update(int id, const nlohmann::json& body, nlohmann::json* out, std::string* err, int* status);
  bool remove(int id);  // stops it if it runs, and forgets it
  void stop_all();

  // A browser (or curl) listening to a stream: a WAV header, then the stream's samples as they
  // are pumped. A listener that falls behind loses its oldest chunks (a second's worth is queued).
  struct Listener {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> q;
    size_t queued = 0;
    size_t max_queued = 0;
    bool mono = false;
    bool closed = false;
    uint64_t dropped = 0;
  };
  using ListenerPtr = std::shared_ptr<Listener>;
  ListenerPtr listen(int id, bool mono, std::string* err, int* status);
  void unlisten(int id, const ListenerPtr& l);
  // Waits up to timeout_ms; moves what is queued into *out (empty on timeout). False once the
  // stream ended and the queue is drained.
  static bool take(Listener& l, std::string* out, int timeout_ms);

  // GET /api/audio/endpoints: {backend, sinks, sources, tools, error}. pw-dump is run at most every
  // two seconds; `fresh` skips that cache.
  nlohmann::json endpoints(bool fresh);
  // The codec picker (GET/PUT /api/audio/devices): PipeWire's Bluetooth devices and their
  // profiles, and switching one to another profile — which is how PipeWire picks a codec.
  // {backend, devices:[...cards_from_pw_dump...], error}.
  nlohmann::json devices();
  // `profile` is a profile's index or name, of that device. Answers with the device as it is
  // after the switch.
  bool set_profile(int device_id, const nlohmann::json& profile, nlohmann::json* out,
                   std::string* err, int* status);

  const Tools& tools() const { return tools_; }
  UpnpBrowser& upnp() { return upnp_; }

  // The radio station list (/data/btbench/radio.json; a starter list until one is saved).
  nlohmann::json radio() const;
  bool set_radio(const nlohmann::json& body, std::string* err);

  struct Stream;

 private:
  void run_publisher();
  void prune_locked();

  const Publish publish_;
  const HasSubscribers has_subscribers_;
  const std::string data_dir_;
  const Tools tools_;
  UpnpBrowser upnp_;

  mutable std::mutex m_;
  std::map<int, std::shared_ptr<Stream>> streams_;
  int next_id_ = 1;

  std::mutex ep_m_;
  nlohmann::json ep_cache_;
  int64_t ep_at_ms_ = 0;
  nlohmann::json devices_uncached();
  std::mutex dev_m_;
  nlohmann::json dev_cache_;
  int64_t dev_at_ms_ = 0;

  std::atomic<bool> running_{true};
  std::mutex pub_m_;
  std::condition_variable pub_cv_;
  std::thread publisher_;
};

}  // namespace btb::audio
