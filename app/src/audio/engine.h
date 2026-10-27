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
  std::string pw_cat, pw_dump, pw_metadata, wpctl, aplay, arecord, mpg123, ffmpeg, curl;
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
  std::string art;           // its cover (a media server's albumArtURI), "" for none
  uint32_t id = 0;           // the stream numbers its tracks as they come (PlayOrder)
};

// A URL stream's playlist as it plays. Shuffled, the tracks themselves are put in a random order
// (the one playing first), so the list says what plays next; shuffle off puts them back as they
// were listed, the one playing where it was. Each track is numbered as it comes (`id`): a change
// asked from a list on screen finds its track wherever it has moved since. Pure: the stream calls
// it with its playlist under its lock, the tests with a fixed seed.
class PlayOrder {
 public:
  explicit PlayOrder(uint32_t seed = 1) : rng_(seed) {}
  // A new list, its first track playing; shuffled, the others after it in a random order.
  void reset(std::vector<PlaylistItem>& list, bool shuffle);
  // Shuffle switched on or off; nothing when it already is.
  void shuffle(std::vector<PlaylistItem>& list, bool on);
  // Tracks added right after the one playing (`next`), or at the end.
  void insert(std::vector<PlaylistItem>& list, std::vector<PlaylistItem> add, bool next);
  // Track `id` taken out. False for no such track, or the one playing.
  bool remove(std::vector<PlaylistItem>& list, uint32_t id);
  // The track after the one playing, made the one playing: at its end (`asked` false) or on
  // next. -1 at the end of the list with repeat off (on next: the one playing again). Repeating
  // all, shuffled, a new round puts the list in a new order that does not start with the track
  // just played.
  int next(std::vector<PlaylistItem>& list, const std::string& repeat, bool asked);
  int previous(const std::vector<PlaylistItem>& list, const std::string& repeat);
  void go(const std::vector<PlaylistItem>& list, int track);  // a track picked by its place
  // The one playing: its id, and its place in `list` (0 when it is not there).
  uint32_t current() const { return cur_; }
  int at(const std::vector<PlaylistItem>& list) const;

 private:
  bool shuffled_ = false;
  uint32_t cur_ = 0;
  std::vector<uint32_t> listed_;  // shuffled: the tracks' ids as listed
  uint32_t next_id_ = 1;
  std::mt19937 rng_;
};

// The place of track `id` in `list`, -1 for none.
int playlist_index(const std::vector<PlaylistItem>& list, uint32_t id);

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
  // One item for a radio. It changes while the stream runs (PUT enqueue, remove, shuffle), in the
  // order it plays.
  std::vector<PlaylistItem> playlist;
  std::string repeat_mode = "off";  // off | track | all ("repeat" in the JSON; a sweep's is a bool)
  bool shuffle = false;
  // capture: an endpoint id (GET /api/audio/endpoints), "" for PipeWire's default source
  std::string backend = "pipewire";  // pipewire | alsa
  std::string target;
  bool monitor = false;  // PipeWire: capture what goes *to* sink `target` (its monitor)
  // PipeWire, "" for none: a second source, the right channel to `target`'s left. LE Audio hands
  // a stereo broadcast over as two nodes (one per BIS, or two one-channel broadcasts); each is
  // captured mono and the pump makes them one stereo stream (pcm.h interleave_pair).
  std::string target_right;
};

struct SinkSpec {
  std::string type = "none";  // none | pipewire | alsa
  std::string target;         // endpoint id; "" for PipeWire's default sink
  // "left" or "right": the sink takes that channel of a stereo stream alone, as mono (one BIS of
  // a broadcast that sends left and right as two); "" every channel.
  std::string channel;
};

// A stream plays to any number of sinks at once (two Bluetooth speakers, a speaker and the
// loopback card), each a sink process of its own fed the same chunks: none is listeners only.
struct StreamSpec {
  SourceSpec source;
  std::vector<SinkSpec> sinks;
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
// A local stream: what the board plays of its own (a test signal, a radio or UPnP URL), not a
// capture of what a device sends it. One plays at a time; captures, any number (kMaxStreams).
inline bool is_local(const SourceSpec& s) { return s.type != "capture"; }

// The command lines (pure, tested). `node_name` names the PipeWire stream ("btbench-stream-3"), so
// it can be told apart in wpctl / pw-top. capture_argv captures `target` and ignores
// `target_right`: a left/right pair is two of them, each mono, the right one with its target.
// `kind` is what the stream turned out to be ("mp3" or "other", pcm.h sniff_audio), "" for unknown:
// the URL's extension decides then.
bool decoder_argv(const SourceSpec& s, const std::string& url, int rate, int channels,
                  const Tools& t, std::vector<std::string>* argv, std::string* err, const std::string& kind = "");
bool capture_argv(const SourceSpec& s, int rate, int channels, int latency_ms,
                  const std::string& node_name, const Tools& t, std::vector<std::string>* argv,
                  std::string* err);
bool sink_argv(const SinkSpec& s, int rate, int channels, int latency_ms,
               const std::string& node_name, const std::string& description, const Tools& t,
               std::vector<std::string>* argv, std::string* err);

// PipeWire's controls (the PipeWire view): the one command each PUT runs, or false with the reason
// for a 400. volume_argv: {"volume": 0..1.5} on wpctl's scale; mute_argv: {"mute": true|false|
// "toggle"}; clock_argv: exactly one of the clock keys (docs/api.md "Clock"), null for Default,
// with `clock` the settings as they are (clock_from_pw_dump), which allowed_rates is checked against.
bool volume_argv(int node, const nlohmann::json& body, const Tools& t, std::vector<std::string>* argv,
                 std::string* err);
bool mute_argv(int node, const nlohmann::json& body, const Tools& t, std::vector<std::string>* argv,
               std::string* err);
bool clock_argv(const nlohmann::json& body, const nlohmann::json& clock, const Tools& t,
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
  static constexpr size_t kMaxSinks = 4;  // per stream: each is a pw-cat (or aplay) of its own
  static constexpr size_t kKeepEnded = 6;
  static constexpr size_t kMaxListeners = 3;  // per stream; each holds an HTTP worker

  // POST /api/audio/streams: 201 with the stream, or *status and *err. A local stream takes the
  // place of the one playing (which is removed, as DELETE would).
  bool create(const nlohmann::json& body, nlohmann::json* out, std::string* err, int* status);
  // {streams:[...], max}
  nlohmann::json list() const;
  nlohmann::json get(int id) const;  // null when there is no such stream
  // PUT: gain_db at any time; a tone's freq/freq_right/level_db and a noise's level_db live; for a
  // URL stream, its playlist: {"next": true}, {"previous": true}, {"track": n}, {"enqueue": [...]}
  // (with "at": "next" right after the track playing), {"remove": <track id>},
  // {"repeat": "off|track|all"}, {"shuffle": bool}. Any stream: {"sinks": [...]} (or one "sink"),
  // the outputs kept playing on, the new ones opened, the others closed.
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
  // A device removed from BlueZ: WirePlumber forgets the profile (codec) picked for it. It keeps
  // a profile picked by hand (set_profile) per device name, apart from BlueZ's pairing, and puts
  // it back whenever the device connects: a codec picked before would come back on a fresh pair
  // and switch the device's stream right after it started (its nodes are made again, and what
  // plays to them stops).
  void forget_device(const std::string& address);

  // The PipeWire view (GET /api/audio/pipewire/graph): graph_from_pw_dump with {backend, error,
  // age_ms, tools}. The setters each run one wpctl or pw-metadata — wpctl itself says whether the
  // node takes it — and answer with the graph as it is after.
  nlohmann::json graph(bool fresh);
  bool set_default_node(int node, nlohmann::json* out, std::string* err, int* status);
  bool set_node_volume(int node, const nlohmann::json& body, nlohmann::json* out, std::string* err, int* status);
  bool set_node_mute(int node, const nlohmann::json& body, nlohmann::json* out, std::string* err, int* status);
  bool set_clock(const nlohmann::json& body, nlohmann::json* out, std::string* err, int* status);

  // A sink a stream lost: its process ended (a speaker gone, its node made anew for another
  // codec). Called from the stream's thread, without the engine's locks. Set before the first
  // stream is made.
  using SinkLost = std::function<void(int stream, const SinkSpec& sink)>;
  void on_sink_lost(SinkLost f) { sink_lost_ = std::move(f); }

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
  SinkLost sink_lost_;
  const std::string data_dir_;
  const Tools tools_;
  UpnpBrowser upnp_;

  mutable std::mutex m_;
  std::map<int, std::shared_ptr<Stream>> streams_;
  int next_id_ = 1;

  // One pw-dump for everything that reads PipeWire's graph (the endpoints, the codec picker, the
  // PipeWire view): under nice, one at a time, its answer reused for two seconds. A Zero W takes
  // seconds for one, and each console view asks on every media update.
  struct PwDump {
    nlohmann::json objects = nlohmann::json::array();  // parse_pw_dump's
    std::string error;     // "" when objects is the graph
    bool running = false;  // PipeWire answered, or was only too slow to (kDumpBusy)
    int64_t started_ms = 0, at_ms = 0;
  };
  // `fresh`: one started after this call, not a cached one.
  std::shared_ptr<const PwDump> pw_dump(bool fresh);
  std::mutex dump_m_;
  std::shared_ptr<const PwDump> dump_;
  // endpoints() of the dump it was made from: the route and the board's player ask for it often.
  std::mutex ep_m_;
  std::shared_ptr<const PwDump> ep_from_;
  nlohmann::json ep_json_;
  nlohmann::json devices_from(const PwDump& d) const;
  bool control(const std::vector<std::string>& argv, const std::string& what, nlohmann::json* out,
               std::string* err, int* status);

  std::atomic<bool> running_{true};
  std::mutex pub_m_;
  std::condition_variable pub_cv_;
  std::thread publisher_;
};

}  // namespace btb::audio
