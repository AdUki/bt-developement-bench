#include "bench_player.h"

#include <errno.h>
#include <string.h>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <chrono>
#include <fstream>

#include "audio/engine.h"
#include "audio/route.h"
#include "dbus_util.h"
#include "obex/cover_art.h"
#include "util/log.h"
#include "util/strings.h"

namespace btb {

using json = nlohmann::json;

namespace {

constexpr const char* kMediaIface = "org.bluez.Media1";
constexpr const char* kMprisIface = "org.mpris.MediaPlayer2.Player";
constexpr const char* kFolderIface = "org.bluez.MediaFolder1";
constexpr const char* kItemIface = "org.bluez.MediaItem1";
constexpr const char* kItemPrefix = "/org/btbench/player/item";
constexpr const char* kNowPlayingPath = "/org/btbench/player/NowPlaying";
constexpr const char* kSearchPath = "/org/btbench/player/Search";
// MediaFolder1's current folder when it is not a library folder.
constexpr int kRoot = -1;
constexpr int kNowPlaying = -2;
constexpr int kSearch = -3;
// append_metadata's id for the track of another stream (BenchPlayer::mirror_).
constexpr int kMirrorTrack = -10;

// How far the remote's count of the position may drift from the track's before it is told.
constexpr int64_t kSeekDriftMs = 1500;

int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// MPRIS LoopStatus ↔ the engine's repeat.
const char* loop_status(const std::string& repeat) {
  return repeat == "track" ? "Track" : repeat == "all" ? "Playlist" : "None";
}

json tone(double freq, double level_db = -12, double right = 0) {
  json s{{"type", "tone"}, {"freq", freq}, {"level_db", level_db}};
  if (right > 0) s["freq_right"] = right;
  return s;
}

std::string level_name(int db) { return db == 0 ? "0 dBFS" : std::to_string(db) + " dBFS"; }

}  // namespace

// ---- the library ------------------------------------------------------------------------------

int BenchLibrary::add(LibItem it) {
  it.id = static_cast<int>(items_.size());
  if (it.parent < 0) root_.push_back(it.id);
  else items_[it.parent].children.push_back(it.id);
  items_.push_back(std::move(it));
  return items_.back().id;
}

void BenchLibrary::build(const json& radio) {
  items_.clear();
  root_.clear();

  // Folders carry no source; a playable one plays its tracks, the first first.
  auto folder = [this](int parent, std::string name, std::string type) {
    LibItem f;
    f.parent = parent;
    f.name = std::move(name);
    f.folder = true;
    f.folder_type = std::move(type);
    f.playable = true;
    return add(std::move(f));
  };
  auto track = [this](int parent, std::string name, json source, const std::string& album,
                      const std::string& genre, uint32_t duration_ms = 0) {
    LibItem t;
    t.parent = parent;
    t.name = std::move(name);
    t.source = std::move(source);
    t.artist = "btbench";
    t.album = album;
    t.genre = genre;
    t.duration_ms = duration_ms;
    add(std::move(t));
  };

  const int sig = folder(-1, "Test signals", "titles");
  track(sig, "Sine 1 kHz", tone(1000), "Test signals", "Test signal");
  track(sig, "Sine 440 Hz", tone(440), "Test signals", "Test signal");
  track(sig, "Sine 100 Hz", tone(100), "Test signals", "Test signal");
  track(sig, "Sine 10 kHz", tone(10000), "Test signals", "Test signal");
  track(sig, "Left 1 kHz, right 2 kHz", tone(1000, -12, 2000), "Test signals", "Test signal");
  // A sweep that ends, so a remote sees a track end and the next one start.
  track(sig, "Sweep 20 Hz to 20 kHz",
        json{{"type", "sweep"}, {"from", 20}, {"to", 20000}, {"seconds", 10}, {"log", true},
             {"repeat", false}, {"level_db", -12}},
        "Test signals", "Test signal", 10000);
  track(sig, "Pink noise", json{{"type", "noise"}, {"color", "pink"}, {"level_db", -20}}, "Test signals",
        "Test signal");
  track(sig, "White noise", json{{"type", "noise"}, {"color", "white"}, {"level_db", -20}}, "Test signals",
        "Test signal");
  track(sig, "Silence", json{{"type", "silence"}}, "Test signals", "Test signal");
  // A folder in a folder: what a remote's "up" and "down" are tried on.
  const int lv = folder(sig, "Levels", "titles");
  for (int db : {0, -6, -12, -20, -40, -60})
    track(lv, "Sine 1 kHz at " + level_name(db), tone(1000, db), "Levels", "Test signal");

  const int rad = folder(-1, "Radio", "playlists");
  if (radio.contains("stations") && radio["stations"].is_array()) {
    for (const json& s : radio["stations"]) {
      const std::string url = s.value("url", std::string{});
      if (url.empty()) continue;
      const std::string name = s.value("name", url);
      track(rad, name, json{{"type", "url"}, {"url", url}, {"title", name}}, name, "Radio");
    }
  }
}

const LibItem* BenchLibrary::find(int id) const {
  if (id < 0 || id >= static_cast<int>(items_.size())) return nullptr;
  return &items_[id];
}

const std::vector<int>& BenchLibrary::children(int folder) const {
  static const std::vector<int> none;
  if (folder < 0) return root_;
  const LibItem* f = find(folder);
  return f && f->folder ? f->children : none;
}

std::vector<int> BenchLibrary::tracks(int folder) const {
  std::vector<int> out;
  for (int id : children(folder))
    if (!items_[id].folder) out.push_back(id);
  return out;
}

std::vector<int> BenchLibrary::search(const std::string& q) const {
  const std::vector<std::string> words = split(lower(q), ' ');
  std::vector<int> out;
  if (words.empty()) return out;
  for (const LibItem& it : items_) {
    if (it.folder) continue;
    const std::string name = lower(it.name);
    bool all = true;
    for (const std::string& w : words) all = all && name.find(w) != std::string::npos;
    if (all) out.push_back(it.id);
  }
  return out;
}

std::string BenchLibrary::folder_name(int folder) const {
  std::string path;
  for (const LibItem* f = find(folder); f; f = find(f->parent)) path = "/" + f->name + path;
  return "/Filesystem" + path;
}

json lib_item_metadata(const BenchLibrary& lib, const LibItem& it) {
  json md{{"Title", it.name}, {"Artist", it.artist}, {"Album", it.album}, {"Genre", it.genre}};
  const std::vector<int> siblings = lib.tracks(it.parent);
  const auto at = std::find(siblings.begin(), siblings.end(), it.id);
  if (at != siblings.end()) {
    md["Number"] = static_cast<uint32_t>(at - siblings.begin() + 1);
    md["NumberOfTracks"] = static_cast<uint32_t>(siblings.size());
  }
  if (it.duration_ms) md["Duration"] = it.duration_ms;
  return md;
}

json stream_track(const json& s) {
  static const json none = json::object();
  const json& src = s.contains("source") && s["source"].is_object() ? s["source"] : none;
  const std::string type = src.value("type", std::string{});
  const json meta = s.contains("meta") && s["meta"].is_object() ? s["meta"] : json::object();
  // No made-up artist: "btbench" is the player's name (Identity), and a remote that shows the
  // artist line shows nothing better than an empty one.
  json md = json::object();
  if (type == "url") {
    const json pl = src.contains("playlist") && src["playlist"].is_array() ? src["playlist"] : json::array();
    const int n = static_cast<int>(pl.size()), t = s.value("track", 0);
    const json item = t >= 0 && t < n && pl[t].is_object() ? pl[t] : json::object();
    const std::string icy = meta.value("title", std::string{});
    const std::string station = src.value("title", std::string{});
    if (n > 1) {
      // A playlist (a media server's folder): its item as the server described it, numbered. The
      // stream's own title is only its first track's, so it is no album.
      const std::string title = item.value("title", std::string{});
      md["Title"] = !title.empty() ? title : icy;
      md["Artist"] = item.value("artist", std::string{});
      md["Album"] = item.value("album", std::string{});
      md["Number"] = static_cast<uint32_t>(t + 1);
      md["NumberOfTracks"] = static_cast<uint32_t>(n);
      if (item.contains("duration_ms") && item["duration_ms"].is_number() && item["duration_ms"].get<double>() > 0)
        md["Duration"] = static_cast<uint32_t>(item["duration_ms"].get<double>());
      const std::string art = item.value("art", std::string{});
      if (!art.empty()) md["Art"] = art;
    } else {
      // A radio: the song it says it plays, the station as the album. Stations send ICY titles as
      // "Artist - Title" by convention; split there, else the whole of it is the title.
      const size_t dash = icy.find(" - ");
      if (dash != std::string::npos && dash > 0 && dash + 3 < icy.size()) {
        md["Artist"] = icy.substr(0, dash);
        md["Title"] = icy.substr(dash + 3);
      } else {
        md["Title"] = !icy.empty() ? icy : !station.empty() ? station : meta.value("name", src.value("url", std::string{}));
      }
      md["Album"] = !station.empty() ? station : meta.value("name", std::string{});
      md["Genre"] = "Radio";
      // The cover the station sends with its title, for the cover art responder.
      const std::string art = meta.value("art", std::string{});
      if (!art.empty()) md["Art"] = art;
    }
  } else if (type == "capture") {
    md["Title"] = src.value("title", std::string("What a device plays"));
    md["Album"] = "Relayed";
    md["Genre"] = "Relay";
  } else {
    // A test signal: its label without where it goes ("tone 1 kHz → bluez_output...").
    std::string label = s.value("label", type);
    const size_t arrow = label.rfind(" → ");  // a sweep's own "20 Hz → 20 kHz" comes first
    if (arrow != std::string::npos) label = label.substr(0, arrow);
    if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    md["Title"] = label;
    md["Album"] = "Test signals";
    md["Genre"] = "Test signal";
    if (type == "sweep" && !src.value("repeat", true))
      md["Duration"] = static_cast<uint32_t>(std::lround(src.value("seconds", 0.0) * 1000));
  }
  return md;
}

namespace {

// A stream's sinks: "sinks", or the one "sink" of a stream from before there were several.
json stream_sinks(const json& s) {
  if (s.contains("sinks") && s["sinks"].is_array()) return s["sinks"];
  if (s.contains("sink") && s["sink"].is_object()) return json::array({s["sink"]});
  return json::array();
}

// What remotes are told of a device the board relays (the route's input): its player's track when
// it has one with a title (a phone), else the devices by name, a broadcast as Auracast.
json relay_track(const json& objs, const json& devices, const json& player) {
  if (!player.is_null() && player.contains("track")) {
    const json& t = player["track"];
    if (!t.value("title", std::string{}).empty()) {
      json md{{"Title", t.value("title", std::string{})},
              {"Artist", t.value("artist", std::string{})},
              {"Album", t.value("album", std::string{})},
              {"Genre", t.value("genre", std::string{})}};
      if (t["track_number"].is_number()) md["Number"] = t["track_number"].get<uint32_t>();
      if (t["number_of_tracks"].is_number()) md["NumberOfTracks"] = t["number_of_tracks"].get<uint32_t>();
      if (t.value("duration_ms", 0) > 0) md["Duration"] = t["duration_ms"].get<uint32_t>();
      return md;
    }
  }
  std::string names;
  bool broadcast = false;
  for (const json& dv : devices) {
    const std::string addr = dv.value("address", std::string{});
    broadcast = broadcast || dv.value("kind", std::string{}) == "broadcast";
    std::string name = addr;
    for (auto it = objs.begin(); it != objs.end(); ++it) {
      const auto d = it->find("org.bluez.Device1");
      if (d == it->end() || upper(str_of(*d, "Address")) != upper(addr)) continue;
      if (!str_of(*d, "Alias").empty()) name = str_of(*d, "Alias");
      break;
    }
    if (names.find(name) == std::string::npos) names += (names.empty() ? "" : " + ") + name;
  }
  return json{{"Title", names}, {"Album", broadcast ? "Auracast" : "Relayed"}, {"Genre", "Relay"}};
}

}  // namespace

bool stream_to_bluetooth(const json& s, const std::string& default_bt) {
  for (const json& sink : stream_sinks(s)) {
    if (sink.value("type", std::string{}) != "pipewire") continue;
    const std::string t = sink.value("target", std::string{});
    if (t.empty() ? !default_bt.empty() : t.compare(0, 13, "bluez_output.") == 0) return true;
  }
  return false;
}

// ---- D-Bus trampolines ------------------------------------------------------------------------

namespace {

// <player>/item/s<stream>_<n>: the stream's id and the track's number.
bool parse_stream_item(const std::string& path, int* stream, int* track) {
  const std::string pre = std::string(kItemPrefix) + "/s";
  if (path.compare(0, pre.size(), pre) != 0) return false;
  const size_t us = path.find('_', pre.size());
  if (us == std::string::npos) return false;
  const std::string a = path.substr(pre.size(), us - pre.size()), b = path.substr(us + 1);
  for (const std::string* x : {&a, &b})
    if (x->empty() || x->size() > 9 || x->find_first_not_of("0123456789") != std::string::npos) return false;
  *stream = std::stoi(a);
  *track = std::stoi(b);
  return true;
}

BenchPlayer* self(void* data) { return static_cast<BenchPlayer*>(data); }

int m_play(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_mpris(m, "Play", e); }
int m_pause(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_mpris(m, "Pause", e); }
int m_playpause(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_mpris(m, "PlayPause", e); }
int m_stop(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_mpris(m, "Stop", e); }
int m_next(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_mpris(m, "Next", e); }
int m_previous(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_mpris(m, "Previous", e); }
int m_seek(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_mpris(m, "Seek", e); }
int m_release(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, ""); }
int m_change_folder(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_change_folder(m, e); }
int m_list_items(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_list_items(m, e); }
int m_search(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_search(m, e); }
int m_item_play(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_item(m, "Play", e); }
int m_item_add(sd_bus_message* m, void* d, sd_bus_error* e) { return self(d)->on_item(m, "AddtoNowPlaying", e); }

int p_get(sd_bus*, const char* path, const char* iface, const char* prop, sd_bus_message* reply, void* d,
          sd_bus_error* e) {
  return self(d)->get_prop(reply, iface, prop, path, e);
}

int p_set(sd_bus*, const char*, const char*, const char* prop, sd_bus_message* value, void* d, sd_bus_error* e) {
  return self(d)->set_prop(value, prop, e);
}

// The items under <player>/item/: one vtable for all of them, found by path.
int item_find(sd_bus*, const char* path, const char*, void* d, void** found, sd_bus_error*) {
  int id = 0;
  if (!self(d)->item_exists(path, &id) && !self(d)->stream_item_exists(path, &id)) return 0;
  *found = d;
  return 1;
}

// The items, for introspection (busctl tree): BlueZ never asks.
int item_enum(sd_bus*, const char*, void* d, char*** nodes, sd_bus_error*) {
  const size_t n = self(d)->item_count();
  char** v = static_cast<char**>(calloc(n + 1, sizeof(char*)));
  if (!v) return -ENOMEM;
  for (size_t i = 0; i < n; ++i) v[i] = strdup((std::string(kItemPrefix) + "/" + std::to_string(i)).c_str());
  *nodes = v;
  return 1;
}

const sd_bus_vtable kPlayerVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Play", "", "", m_play, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Pause", "", "", m_pause, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PlayPause", "", "", m_playpause, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Stop", "", "", m_stop, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Next", "", "", m_next, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Previous", "", "", m_previous, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Seek", "x", "", m_seek, SD_BUS_VTABLE_UNPRIVILEGED),
    // BlueZ calls it when it lets go of the player (bluetoothd stopping).
    SD_BUS_METHOD("Release", "", "", m_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("PlaybackStatus", "s", p_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Metadata", "a{sv}", p_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Playlist", "o", p_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Position", "x", p_get, 0, 0),
    // BlueZ sets them when the remote changes AVRCP's repeat or shuffle setting, and offers the
    // settings at all only when the registration had them.
    SD_BUS_WRITABLE_PROPERTY("LoopStatus", "s", p_get, p_set, 0,
                             SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE | SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_WRITABLE_PROPERTY("Shuffle", "b", p_get, p_set, 0,
                             SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE | SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Identity", "s", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Browsable", "b", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Searchable", "b", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanControl", "b", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanPlay", "b", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanPause", "b", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanGoNext", "b", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanGoPrevious", "b", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanSeek", "b", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END,
};

const sd_bus_vtable kFolderVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("ChangeFolder", "o", "", m_change_folder, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ListItems", "a{sv}", "a{oa{sv}}", m_list_items, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Search", "sa{sv}", "o", m_search, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("NumberOfItems", "u", p_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Name", "s", p_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END,
};

const sd_bus_vtable kItemVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Play", "", "", m_item_play, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AddtoNowPlaying", "", "", m_item_add, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Player", "o", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Name", "s", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Type", "s", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("FolderType", "s", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Playable", "b", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Metadata", "a{sv}", p_get, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END,
};

// One a{sv} entry with a basic value. sd_bus_message_append_basic takes a string itself, and a
// pointer to any other value.
inline const void* basic_ptr(const char* const& v) { return v; }
template <typename T>
const void* basic_ptr(const T& v) { return &v; }

template <typename T>
int dict_entry(sd_bus_message* m, const char* key, char type, T v) {
  const char sig[2] = {type, 0};
  int r = sd_bus_message_open_container(m, 'e', "sv");
  if (r >= 0) r = sd_bus_message_append(m, "s", key);
  if (r >= 0) r = sd_bus_message_open_container(m, 'v', sig);
  if (r >= 0) r = sd_bus_message_append_basic(m, type, basic_ptr(v));
  if (r >= 0) r = sd_bus_message_close_container(m);
  if (r >= 0) r = sd_bus_message_close_container(m);
  return r;
}

int dict_str(sd_bus_message* m, const char* key, const std::string& v) {
  return dict_entry(m, key, 's', v.c_str());
}

int dict_strv(sd_bus_message* m, const char* key, const std::string& v) {
  int r = sd_bus_message_open_container(m, 'e', "sv");
  if (r >= 0) r = sd_bus_message_append(m, "s", key);
  if (r >= 0) r = sd_bus_message_open_container(m, 'v', "as");
  if (r >= 0) r = sd_bus_message_append(m, "as", 1, v.c_str());
  if (r >= 0) r = sd_bus_message_close_container(m);
  if (r >= 0) r = sd_bus_message_close_container(m);
  return r;
}

const char* status_name(int s) {
  return s == 1 ? "Playing" : s == 2 ? "Paused" : "Stopped";
}

}  // namespace

// ---- the player -------------------------------------------------------------------------------

BenchPlayer::BenchPlayer(BtManager& bt, audio::AudioEngine& audio, std::string state_file)
    : bt_(bt), audio_(audio), state_file_(std::move(state_file)) {
  lib_.build(audio_.radio());
  load_state();
  worker_ = std::thread([this] { run_worker(); });
}

BenchPlayer::~BenchPlayer() {
  {
    std::lock_guard<std::mutex> lk(wm_);
    quit_ = true;
  }
  wcv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void BenchPlayer::load_state() {
  if (state_file_.empty()) return;
  std::ifstream f(state_file_);
  if (!f) return;
  try {
    const json j = json::parse(f);
    enabled_ = j.value("enabled", true);
    const std::string repeat = j.value("repeat", std::string("off"));
    if (repeat == "off" || repeat == "track" || repeat == "all") repeat_ = repeat;
    shuffle_ = j.value("shuffle", false);
  } catch (const std::exception&) {
    // A broken file is replaced by the next change.
  }
}

void BenchPlayer::save_state() const {
  if (state_file_.empty()) return;
  std::ofstream f(state_file_ + ".tmp");
  f << json{{"enabled", enabled_}, {"repeat", repeat_}, {"shuffle", shuffle_}}.dump() << "\n";
  f.close();
  if (f) std::rename((state_file_ + ".tmp").c_str(), state_file_.c_str());
}

std::string BenchPlayer::item_path(int id) const { return std::string(kItemPrefix) + "/" + std::to_string(id); }

bool BenchPlayer::item_exists(const std::string& path, int* id) const {
  const std::string pre = std::string(kItemPrefix) + "/";
  if (path.compare(0, pre.size(), pre) != 0) return false;
  const std::string n = path.substr(pre.size());
  if (n.empty() || n.size() > 6 || n.find_first_not_of("0123456789") != std::string::npos) return false;
  std::lock_guard<std::mutex> lk(m_);
  const LibItem* it = lib_.find(std::stoi(n));
  if (!it) return false;
  *id = it->id;
  return true;
}

bool BenchPlayer::stream_item_exists(const std::string& path, int* track) const {
  int sid = 0, t = 0;
  if (!parse_stream_item(path, &sid, &t)) return false;
  std::lock_guard<std::mutex> lk(m_);
  if (!mirroring_locked() || sid != mirror_ || t >= static_cast<int>(mirror_list_.size())) return false;
  *track = t;
  return true;
}

std::string BenchPlayer::stream_item_path_locked(int track) const {
  return std::string(kItemPrefix) + "/s" + std::to_string(mirror_) + "_" + std::to_string(track);
}

size_t BenchPlayer::item_count() const {
  std::lock_guard<std::mutex> lk(m_);
  return lib_.items().size();
}

bool BenchPlayer::folder_of(const std::string& path, int* folder) const {
  if (path == kPath) {
    *folder = kRoot;
    return true;
  }
  if (path == kNowPlayingPath) {
    *folder = kNowPlaying;
    return true;
  }
  if (path == kSearchPath) {
    *folder = kSearch;
    return true;
  }
  int id = 0;
  if (!item_exists(path, &id)) return false;
  std::lock_guard<std::mutex> lk(m_);
  if (!lib_.find(id)->folder) return false;
  *folder = id;
  return true;
}

std::vector<int> BenchPlayer::folder_entries_locked(int folder) const {
  if (folder == kNowPlaying) return queue_;
  if (folder == kSearch) return search_;
  return lib_.children(folder);
}

// ---- D-Bus methods (bus thread) ---------------------------------------------------------------

int BenchPlayer::on_mpris(sd_bus_message* m, const std::string& method, sd_bus_error* e) {
  std::string err;
  if (method == "Seek") return sd_bus_reply_method_return(m, "");  // CanSeek is false
  std::string what = method == "PlayPause" ? "" : lower(method);
  if (what.empty()) {
    std::lock_guard<std::mutex> lk(m_);
    what = status_ == Status::playing ? "pause" : "play";
  }
  LOG_INFO("player: {} from {}", what, sd_bus_message_get_sender(m) ? sd_bus_message_get_sender(m) : "?");
  if (!action(what, &err)) return sd_bus_error_set(e, "org.bluez.Error.Failed", err.c_str());
  return sd_bus_reply_method_return(m, "");
}

int BenchPlayer::on_change_folder(sd_bus_message* m, sd_bus_error* e) {
  const char* path = nullptr;
  int r = sd_bus_message_read(m, "o", &path);
  if (r < 0) return r;
  int folder = 0;
  if (!folder_of(path, &folder)) return sd_bus_error_set(e, "org.bluez.Error.DoesNotExist", "no such folder");
  {
    std::lock_guard<std::mutex> lk(m_);
    folder_ = folder;
  }
  return sd_bus_reply_method_return(m, "");
}

int BenchPlayer::on_list_items(sd_bus_message* m, sd_bus_error* e) {
  json filter;
  int r = read_json(m, &filter);
  if (r < 0) return sd_bus_error_set(e, "org.bluez.Error.InvalidArguments", "a{sv} filter");
  sd_bus_message* reply = nullptr;
  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) return r;
  std::lock_guard<std::mutex> lk(m_);
  // Telling another stream: its playlist is what plays now, not the player's own list.
  const bool stream = folder_ == kNowPlaying && mirroring_locked();
  const std::vector<int> entries = stream ? std::vector<int>{} : folder_entries_locked(folder_);
  // Start and End are offsets, End included, as AVRCP's GetFolderItems has them.
  const long long n = static_cast<long long>(stream ? mirror_list_.size() : entries.size());
  const long long start = num_of(filter, "Start", 0);
  const long long end = std::min(num_of(filter, "End", n - 1), n - 1);
  r = sd_bus_message_open_container(reply, 'a', "{oa{sv}}");
  for (long long i = start; r >= 0 && i <= end && i < n; ++i) {
    const std::string p = stream ? stream_item_path_locked(static_cast<int>(i)) : item_path(entries[i]);
    r = sd_bus_message_open_container(reply, 'e', "oa{sv}");
    if (r >= 0) r = sd_bus_message_append(reply, "o", p.c_str());
    if (r >= 0) {
      if (stream) append_stream_item_props(reply, static_cast<int>(i));
      else append_item_props(reply, lib_.items()[entries[i]]);
      r = sd_bus_message_close_container(reply);
    }
  }
  if (r >= 0) r = sd_bus_message_close_container(reply);
  if (r >= 0) r = sd_bus_send(nullptr, reply, nullptr);
  sd_bus_message_unref(reply);
  return r < 0 ? r : 1;
}

int BenchPlayer::on_search(sd_bus_message* m, sd_bus_error* e) {
  const char* value = nullptr;
  int r = sd_bus_message_read(m, "s", &value);
  if (r < 0) return r;
  {
    std::lock_guard<std::mutex> lk(m_);
    search_ = lib_.search(value);
    LOG_INFO("player: search \"{}\": {} found", value, search_.size());
    if (search_.empty()) return sd_bus_error_set(e, "org.bluez.Error.DoesNotExist", "nothing found");
  }
  return sd_bus_reply_method_return(m, "o", kSearchPath);
}

int BenchPlayer::on_item(sd_bus_message* m, const std::string& method, sd_bus_error* e) {
  int id = 0;
  if (stream_item_exists(sd_bus_message_get_path(m), &id)) {
    if (method == "AddtoNowPlaying")
      return sd_bus_error_set(e, "org.bluez.Error.NotSupported", "already in the now playing list");
    int stream;
    {
      std::lock_guard<std::mutex> lk(m_);
      stream = mirror_;
    }
    LOG_INFO("player: track {} of stream {} from {}", id + 1, stream,
             sd_bus_message_get_sender(m) ? sd_bus_message_get_sender(m) : "?");
    {
      std::lock_guard<std::mutex> wl(wm_);
      cmds_.push_back(Cmd{Cmd::mirror, json{{"op", "track"}, {"stream", stream}, {"track", id}}});
    }
    wcv_.notify_one();
    return sd_bus_reply_method_return(m, "");
  }
  if (!item_exists(sd_bus_message_get_path(m), &id))
    return sd_bus_error_set(e, "org.bluez.Error.DoesNotExist", "no such item");
  std::string err;
  if (!play_item(id, method == "AddtoNowPlaying", &err)) return sd_bus_error_set(e, "org.bluez.Error.Failed", err.c_str());
  return sd_bus_reply_method_return(m, "");
}

void BenchPlayer::append_item_props(sd_bus_message* m, const LibItem& it) const {
  int r = sd_bus_message_open_container(m, 'a', "{sv}");
  if (r >= 0) r = dict_entry(m, "Player", 'o', kPath);
  if (r >= 0) r = dict_str(m, "Name", it.name);
  if (r >= 0) r = dict_str(m, "Type", it.folder ? "folder" : "audio");
  if (it.folder) {
    if (r >= 0) r = dict_str(m, "FolderType", it.folder_type);
    if (r >= 0) r = dict_entry(m, "Playable", 'b', static_cast<int>(it.playable));
  } else {
    if (r >= 0) r = sd_bus_message_open_container(m, 'e', "sv");
    if (r >= 0) r = sd_bus_message_append(m, "s", "Metadata");
    if (r >= 0) r = sd_bus_message_open_container(m, 'v', "a{sv}");
    if (r >= 0) append_metadata(m, it.id, false);
    if (r >= 0) r = sd_bus_message_close_container(m);
    if (r >= 0) r = sd_bus_message_close_container(m);
  }
  if (r >= 0) sd_bus_message_close_container(m);
}

// MediaItem1's Metadata of a track of the told stream's playlist.
void BenchPlayer::append_stream_metadata(sd_bus_message* m, int track) const {
  const json& it = mirror_list_[track];
  const std::string title = it.value("title", std::string{});
  int r = sd_bus_message_open_container(m, 'a', "{sv}");
  if (r >= 0) r = dict_str(m, "Title", !title.empty() ? title : it.value("url", std::string{}));
  if (r >= 0) r = dict_str(m, "Artist", it.value("artist", std::string{}));
  if (r >= 0) r = dict_str(m, "Album", it.value("album", std::string{}));
  if (r >= 0) r = dict_entry(m, "Number", 'u', static_cast<uint32_t>(track + 1));
  if (r >= 0) r = dict_entry(m, "NumberOfTracks", 'u', static_cast<uint32_t>(mirror_list_.size()));
  if (r >= 0 && it.contains("duration_ms") && it["duration_ms"].is_number() && it["duration_ms"].get<double>() > 0)
    r = dict_entry(m, "Duration", 'u', static_cast<uint32_t>(it["duration_ms"].get<double>()));
  if (r >= 0) sd_bus_message_close_container(m);
}

void BenchPlayer::append_stream_item_props(sd_bus_message* m, int track) const {
  const std::string title = mirror_list_[track].value("title", std::string{});
  int r = sd_bus_message_open_container(m, 'a', "{sv}");
  if (r >= 0) r = dict_entry(m, "Player", 'o', kPath);
  if (r >= 0) r = dict_str(m, "Name", !title.empty() ? title : mirror_list_[track].value("url", std::string{}));
  if (r >= 0) r = dict_str(m, "Type", "audio");
  if (r >= 0) r = sd_bus_message_open_container(m, 'e', "sv");
  if (r >= 0) r = sd_bus_message_append(m, "s", "Metadata");
  if (r >= 0) r = sd_bus_message_open_container(m, 'v', "a{sv}");
  if (r >= 0) append_stream_metadata(m, track);
  if (r >= 0) r = sd_bus_message_close_container(m);
  if (r >= 0) r = sd_bus_message_close_container(m);
  if (r >= 0) sd_bus_message_close_container(m);
}

// The cover of the track, as BlueZ's target passes it on: an image handle at the player's
// ObexPort, sent only while a controller is connected there (AVRCP 1.6: a handle means nothing
// to one that is not).
int BenchPlayer::append_img_handle(sd_bus_message* m, const std::string& art) const {
  if (art.empty() || !cover_ || !cover_->psm() || !cover_->connected()) return 0;
  return dict_str(m, "bluez:ImgHandle", cover_->handle_for(art));
}

void BenchPlayer::set_cover_art(CoverArtServer* cover) {
  cover_ = cover;
  if (cover_) cover_->on_connected_change([this] { changed(true, false, false); });
}

// The track's metadata: MediaItem1's keys, or MPRIS's (xesam:*, mpris:length in µs) for the player.
void BenchPlayer::append_metadata(sd_bus_message* m, int id, bool mpris) const {
  int r = sd_bus_message_open_container(m, 'a', "{sv}");
  if (id == kMirrorTrack && r >= 0) {
    // Another stream's track: no library item behind it. One of a playlist is its item in the now
    // playing list (the trackid a remote finds there); any other has no trackid.
    const json& md = mirror_md_;
    const std::string title = md.value("Title", std::string{}), album = md.value("Album", std::string{});
    const std::string artist = md.value("Artist", std::string{}), genre = md.value("Genre", std::string{});
    if (mpris) {
      if (mirror_track_ >= 0 && mirror_track_ < static_cast<int>(mirror_list_.size()))
        r = dict_entry(m, "mpris:trackid", 'o', stream_item_path_locked(mirror_track_).c_str());
      if (r >= 0) r = dict_str(m, "xesam:title", title);
      if (r >= 0) r = dict_strv(m, "xesam:artist", artist);
      if (r >= 0) r = dict_str(m, "xesam:album", album);
      if (r >= 0) r = dict_strv(m, "xesam:genre", genre);
      if (r >= 0 && md.contains("Number")) r = dict_entry(m, "xesam:trackNumber", 'i', md["Number"].get<int32_t>());
      // The length makes the remote's progress bar: without it, AVRCP says "not known".
      if (r >= 0 && md.contains("Duration"))
        r = dict_entry(m, "mpris:length", 'x', static_cast<int64_t>(md["Duration"].get<uint32_t>()) * 1000);
      if (r >= 0) r = append_img_handle(m, md.value("Art", std::string{}));
    } else {
      r = dict_str(m, "Title", title);
      if (r >= 0) r = dict_str(m, "Album", album);
    }
    if (r >= 0) sd_bus_message_close_container(m);
    return;
  }
  const LibItem* it = lib_.find(id);
  if (it && r >= 0) {
    const json md = lib_item_metadata(lib_, *it);
    // A radio's ICY title is the track; the station is the album.
    const std::string title = mpris && !icy_title_.empty() ? icy_title_ : it->name;
    if (mpris) {
      const std::string trackid = item_path(id);
      r = dict_entry(m, "mpris:trackid", 'o', trackid.c_str());
      if (r >= 0) r = dict_str(m, "xesam:title", title);
      if (r >= 0) r = dict_strv(m, "xesam:artist", it->artist);
      if (r >= 0) r = dict_str(m, "xesam:album", it->album);
      if (r >= 0) r = dict_strv(m, "xesam:genre", it->genre);
      if (r >= 0 && md.contains("Number")) r = dict_entry(m, "xesam:trackNumber", 'i', md["Number"].get<int32_t>());
      if (r >= 0 && it->duration_ms)
        r = dict_entry(m, "mpris:length", 'x', static_cast<int64_t>(it->duration_ms) * 1000);
      if (r >= 0) r = append_img_handle(m, art_);
    } else {
      r = dict_str(m, "Title", title);
      if (r >= 0) r = dict_str(m, "Artist", it->artist);
      if (r >= 0) r = dict_str(m, "Album", it->album);
      if (r >= 0) r = dict_str(m, "Genre", it->genre);
      if (r >= 0 && md.contains("Number")) r = dict_entry(m, "Number", 'u', md["Number"].get<uint32_t>());
      if (r >= 0 && md.contains("NumberOfTracks"))
        r = dict_entry(m, "NumberOfTracks", 'u', md["NumberOfTracks"].get<uint32_t>());
      if (r >= 0 && it->duration_ms) r = dict_entry(m, "Duration", 'u', it->duration_ms);
    }
  }
  if (r >= 0) sd_bus_message_close_container(m);
}

int BenchPlayer::get_prop(sd_bus_message* reply, const std::string& iface, const std::string& prop,
                          const std::string& path, sd_bus_error* e) {
  std::lock_guard<std::mutex> lk(m_);
  int sid = 0, track = 0;
  if (iface == kItemIface && parse_stream_item(path, &sid, &track)) {
    if (!mirroring_locked() || sid != mirror_ || track >= static_cast<int>(mirror_list_.size()))
      return sd_bus_error_set(e, "org.bluez.Error.DoesNotExist", "no such item");
    const std::string title = mirror_list_[track].value("title", std::string{});
    if (prop == "Player") return sd_bus_message_append(reply, "o", kPath);
    if (prop == "Name") return sd_bus_message_append(reply, "s", title.c_str());
    if (prop == "Type") return sd_bus_message_append(reply, "s", "audio");
    if (prop == "FolderType") return sd_bus_message_append(reply, "s", "");
    if (prop == "Playable") return sd_bus_message_append(reply, "b", 1);
    if (prop == "Metadata") {
      append_stream_metadata(reply, track);
      return 1;
    }
  } else if (iface == kItemIface) {
    const std::string pre = std::string(kItemPrefix) + "/";
    const LibItem* it = path.size() > pre.size() ? lib_.find(std::atoi(path.c_str() + pre.size())) : nullptr;
    if (!it) return sd_bus_error_set(e, "org.bluez.Error.DoesNotExist", "no such item");
    if (prop == "Player") return sd_bus_message_append(reply, "o", kPath);
    if (prop == "Name") return sd_bus_message_append(reply, "s", it->name.c_str());
    if (prop == "Type") return sd_bus_message_append(reply, "s", it->folder ? "folder" : "audio");
    if (prop == "FolderType") return sd_bus_message_append(reply, "s", it->folder ? it->folder_type.c_str() : "");
    if (prop == "Playable") return sd_bus_message_append(reply, "b", static_cast<int>(it->folder ? it->playable : true));
    if (prop == "Metadata") {
      append_metadata(reply, it->id, false);
      return 1;
    }
  } else if (iface == kFolderIface) {
    if (prop == "NumberOfItems")
      return sd_bus_message_append(reply, "u", static_cast<uint32_t>(folder_ == kNowPlaying && mirroring_locked()
                                                                         ? mirror_list_.size()
                                                                         : folder_entries_locked(folder_).size()));
    if (prop == "Name") {
      const std::string n = folder_ == kNowPlaying ? "/NowPlaying"
                            : folder_ == kSearch   ? "/Search"
                                                   : lib_.folder_name(folder_);
      return sd_bus_message_append(reply, "s", n.c_str());
    }
  } else {
    if (prop == "PlaybackStatus") return sd_bus_message_append(reply, "s", status_name(static_cast<int>(status_)));
    if (prop == "Metadata") {
      append_metadata(reply, mirroring_locked() ? kMirrorTrack
                             : pos_ >= 0 && pos_ < static_cast<int>(queue_.size()) ? queue_[pos_] : -1,
                      true);
      return 1;
    }
    if (prop == "Playlist") return sd_bus_message_append(reply, "o", kNowPlayingPath);
    if (prop == "Position") return sd_bus_message_append(reply, "x", position_locked(steady_ms()) * 1000);
    if (prop == "LoopStatus") return sd_bus_message_append(reply, "s", loop_status(mirroring_locked() ? mirror_repeat_ : repeat_));
    if (prop == "Shuffle") return sd_bus_message_append(reply, "b", static_cast<int>(mirroring_locked() ? mirror_shuffle_ : shuffle_));
    if (prop == "Identity") return sd_bus_message_append(reply, "s", "btbench");
    if (prop == "CanSeek") return sd_bus_message_append(reply, "b", 0);
    return sd_bus_message_append(reply, "b", 1);  // Browsable, Searchable, CanControl, CanPlay, ...
  }
  return sd_bus_error_set(e, SD_BUS_ERROR_UNKNOWN_PROPERTY, prop.c_str());
}

int BenchPlayer::set_prop(sd_bus_message* value, const std::string& prop, sd_bus_error* e) {
  std::string err;
  bool ok;
  if (prop == "LoopStatus") {
    const char* v = nullptr;
    const int r = sd_bus_message_read(value, "s", &v);
    if (r < 0) return r;
    const std::string l = v ? v : "";
    if (l != "None" && l != "Track" && l != "Playlist")
      return sd_bus_error_set(e, SD_BUS_ERROR_INVALID_ARGS, "LoopStatus is None, Track or Playlist");
    ok = set_settings(l == "Track" ? "track" : l == "Playlist" ? "all" : "off", nullptr, &err);
  } else {
    int b = 0;
    const int r = sd_bus_message_read(value, "b", &b);
    if (r < 0) return r;
    const bool on = b != 0;
    ok = set_settings("", &on, &err);
  }
  LOG_INFO("player: {} set by {}", prop, sd_bus_message_get_sender(value) ? sd_bus_message_get_sender(value) : "?");
  return ok ? 1 : sd_bus_error_set(e, "org.bluez.Error.Failed", err.c_str());
}

// ---- registration (bus thread) ----------------------------------------------------------------

void BenchPlayer::bus_opened(sd_bus* bus) {
  bus_ = bus;
  int r = sd_bus_add_object_vtable(bus, &slot_player_, kPath, kMprisIface, kPlayerVtable, this);
  if (r >= 0) r = sd_bus_add_object_vtable(bus, &slot_folder_, kPath, kFolderIface, kFolderVtable, this);
  if (r >= 0) r = sd_bus_add_fallback_vtable(bus, &slot_items_, kItemPrefix, kItemIface, kItemVtable, item_find, this);
  if (r >= 0) r = sd_bus_add_node_enumerator(bus, &slot_enum_, kItemPrefix, item_enum, this);
  if (r < 0) {
    LOG_WARN("player: cannot export {}: {}", kPath, strerror(-r));
    std::lock_guard<std::mutex> lk(m_);
    error_ = std::string("cannot export the player: ") + strerror(-r);
  }
}

void BenchPlayer::bus_closing() {
  for (sd_bus_slot** s : {&slot_player_, &slot_folder_, &slot_items_, &slot_enum_})
    if (*s) *s = sd_bus_slot_unref(*s);
  registered_.clear();
  registering_.clear();
  bus_ = nullptr;
}

void BenchPlayer::bluez_ready(sd_bus*, const std::string&) {
  bluez_ = true;
  next_check_ns_ = 0;  // look for adapters at the next tick
}

void BenchPlayer::bluez_gone() {
  bluez_ = false;
  registered_.clear();
  registering_.clear();
}

namespace {

struct RegOp {
  std::string adapter;
  std::set<std::string>* registered;
  std::set<std::string>* registering;
  std::mutex* m;
  std::string* error;
};

int registered_cb(sd_bus_message* m, void* data, sd_bus_error*) {
  auto* op = static_cast<RegOp*>(data);
  op->registering->erase(op->adapter);
  const sd_bus_error* e = sd_bus_message_get_error(m);
  // BlueZ has it already: a second registration that raced the first (BlueZ coming back while one
  // was out). Registered either way.
  if (e && is_error(e, "org.bluez.Error.AlreadyExists")) e = nullptr;
  if (e) {
    LOG_WARN("player: {} refused it: {}", op->adapter, friendly(e));
    std::lock_guard<std::mutex> lk(*op->m);
    *op->error = op->adapter + ": " + friendly(e);
  } else {
    LOG_INFO("player: registered with {}", op->adapter);
    op->registered->insert(op->adapter);
    std::lock_guard<std::mutex> lk(*op->m);
    // A refusal of an earlier try (BlueZ still starting) is over.
    if (op->error->compare(0, op->adapter.size() + 1, op->adapter + ":") == 0) op->error->clear();
  }
  return 0;
}

}  // namespace

void BenchPlayer::register_on(sd_bus* bus, const std::string& adapter) {
  sd_bus_message* m = nullptr;
  int r = sd_bus_message_new_method_call(bus, &m, "org.bluez", adapter.c_str(), kMediaIface, "RegisterPlayer");
  if (r >= 0) r = sd_bus_message_append(m, "o", kPath);
  if (r >= 0) {
    // The properties BlueZ takes at registration (it never reads them back; later changes are
    // PropertiesChanged signals).
    std::lock_guard<std::mutex> lk(m_);
    r = sd_bus_message_open_container(m, 'a', "{sv}");
    if (r >= 0) r = dict_str(m, "PlaybackStatus", status_name(static_cast<int>(status_)));
    if (r >= 0) r = dict_str(m, "Identity", "btbench");
    for (const char* f : {"CanControl", "CanPlay", "CanPause", "CanGoNext", "CanGoPrevious", "Browsable", "Searchable"})
      if (r >= 0) r = dict_entry(m, f, 'b', 1);
    if (r >= 0) r = dict_entry(m, "CanSeek", 'b', 0);
    if (r >= 0) r = dict_entry(m, "Playlist", 'o', kNowPlayingPath);
    // Cover art at this PSM: BlueZ's target record offers it (the bench's BlueZ series).
    if (r >= 0 && cover_ && cover_->psm()) r = dict_entry(m, "ObexPort", 'q', cover_->psm());
    const bool mir = mirroring_locked();
    if (r >= 0) r = dict_str(m, "LoopStatus", loop_status(mir ? mirror_repeat_ : repeat_));
    if (r >= 0) r = dict_entry(m, "Shuffle", 'b', static_cast<int>(mir ? mirror_shuffle_ : shuffle_));
    // BlueZ starts its count of the position here (and at 0 for each new track after).
    const int64_t now = steady_ms();
    told_ms_ = position_locked(now);
    told_at_ms_ = now;
    told_playing_ = status_ == Status::playing;
    if (r >= 0) r = dict_entry(m, "Position", 'x', told_ms_ * 1000);
    if (r >= 0 && (pos_ >= 0 || mirroring_locked())) {
      r = sd_bus_message_open_container(m, 'e', "sv");
      if (r >= 0) r = sd_bus_message_append(m, "s", "Metadata");
      if (r >= 0) r = sd_bus_message_open_container(m, 'v', "a{sv}");
      if (r >= 0) append_metadata(m, mirroring_locked() ? kMirrorTrack : queue_[pos_], true);
      if (r >= 0) r = sd_bus_message_close_container(m);
      if (r >= 0) r = sd_bus_message_close_container(m);
    }
    if (r >= 0) r = sd_bus_message_close_container(m);
  }
  auto* op = new RegOp{adapter, &registered_, &registering_, &m_, &error_};
  sd_bus_slot* slot = nullptr;
  if (r >= 0) r = sd_bus_call_async(bus, &slot, m, registered_cb, op, 10 * 1000000ull);
  sd_bus_message_unref(m);
  if (r < 0) {
    delete op;
    LOG_WARN("player: RegisterPlayer on {}: {}", adapter, strerror(-r));
    return;
  }
  registering_.insert(adapter);
  sd_bus_slot_set_destroy_callback(slot, [](void* p) { delete static_cast<RegOp*>(p); });
  sd_bus_slot_set_floating(slot, 1);
  sd_bus_slot_unref(slot);
}

void BenchPlayer::unregister_all(sd_bus* bus) {
  for (const std::string& a : registered_) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    sd_bus_call_method(bus, "org.bluez", a.c_str(), kMediaIface, "UnregisterPlayer", &e, nullptr, "o", kPath);
    sd_bus_error_free(&e);
  }
  registered_.clear();
}

void BenchPlayer::tick(sd_bus* bus, uint64_t now_ns) {
  emit_changes(bus);
  if (!bluez_ || !slot_player_ || now_ns < next_check_ns_) return;
  next_check_ns_ = now_ns + 2000000000ull;  // adapters come and go rarely: every 2 s is plenty
  bool on;
  {
    std::lock_guard<std::mutex> lk(m_);
    on = enabled_;
  }
  if (!on) {
    if (!registered_.empty()) unregister_all(bus);
  } else {
    // Every adapter that has Media1 (a powered-off one too: BlueZ keeps the player for when it
    // is powered), and none that went away.
    const auto objs = bt_.objects();
    std::set<std::string> have;
    for (auto it = objs->begin(); it != objs->end(); ++it)
      if (it.value().contains(kMediaIface)) have.insert(it.key());
    for (auto it = registered_.begin(); it != registered_.end();)
      it = have.count(*it) ? std::next(it) : registered_.erase(it);
    for (const std::string& a : have)
      if (!registered_.count(a) && !registering_.count(a)) register_on(bus, a);
  }
  std::string reg = json(std::vector<std::string>(registered_.begin(), registered_.end())).dump();
  std::lock_guard<std::mutex> lk(m_);
  registered_json_ = std::move(reg);
}

// What changed reaches BlueZ as PropertiesChanged, sent from the bus thread at its next pass.
void BenchPlayer::changed(bool track, bool status, bool playlist, bool settings) {
  {
    std::lock_guard<std::mutex> lk(m_);
    pending_track_ = pending_track_ || track;
    pending_status_ = pending_status_ || status;
    pending_playlist_ = pending_playlist_ || playlist;
    pending_settings_ = pending_settings_ || settings;
    // A new track starts at 0 until a poll measures it (a stream joined midway is further on).
    if (track) note_position_locked(0);
  }
  bt_.post([](sd_bus*) {});  // wakes the bus loop
}

bool BenchPlayer::note_position_locked(int64_t ms) {
  const int64_t now = steady_ms();
  pos_ms_ = ms;
  pos_at_ms_ = now;
  const int64_t told = told_ms_ + (told_playing_ ? now - told_at_ms_ : 0);
  if (std::llabs(told - ms) <= kSeekDriftMs || pending_seek_) return false;
  pending_seek_ = true;
  return true;
}

int64_t BenchPlayer::position_locked(int64_t now_ms) const {
  return pos_ms_ + (status_ == Status::playing ? now_ms - pos_at_ms_ : 0);
}

void BenchPlayer::emit_changes(sd_bus* bus) {
  bool track, status, playlist, settings, seek;
  int64_t seek_us = 0;
  {
    std::lock_guard<std::mutex> lk(m_);
    track = pending_track_;
    status = pending_status_;
    playlist = pending_playlist_;
    settings = pending_settings_;
    seek = pending_seek_;
    pending_track_ = pending_status_ = pending_playlist_ = pending_settings_ = pending_seek_ = false;
    if (!slot_player_ || (!track && !status && !playlist && !settings && !seek)) return;
    // What BlueZ will count from once it has these: a new track from 0; a new status from where
    // its clock had got to; and the track's real position, if that is somewhere else.
    const int64_t now = steady_ms();
    if (track) {
      told_ms_ = 0;
      told_at_ms_ = now;
    }
    if (status) {
      told_ms_ += told_playing_ ? now - told_at_ms_ : 0;
      told_at_ms_ = now;
      told_playing_ = status_ == Status::playing;
    }
    const int64_t at = position_locked(now);
    if (std::llabs(told_ms_ + (told_playing_ ? now - told_at_ms_ : 0) - at) > kSeekDriftMs) {
      seek = true;
      seek_us = at * 1000;
      told_ms_ = at;
      told_at_ms_ = now;
    } else {
      seek = false;
    }
  }
  // BlueZ takes a new Metadata as a new track (TRACK_CHANGED to the remote), and a Playlist that
  // is sent again as a changed now playing list (NOW_PLAYING_CONTENT_CHANGED).
  std::vector<const char*> props;
  if (track) props.push_back("Metadata");
  if (status) props.push_back("PlaybackStatus");
  if (playlist) props.push_back("Playlist");
  if (settings) {
    props.push_back("LoopStatus");
    props.push_back("Shuffle");
  }
  if (props.size()) {
    props.push_back(nullptr);
    sd_bus_emit_properties_changed_strv(bus, kPath, kMprisIface, const_cast<char**>(props.data()));
  }
  // MPRIS's Seeked: BlueZ sets its position from it (and tells a remote that asked for
  // PLAYBACK_POS_CHANGED).
  if (seek) sd_bus_emit_signal(bus, kPath, kMprisIface, "Seeked", "x", seek_us);
}

// ---- playing ----------------------------------------------------------------------------------

bool BenchPlayer::action(const std::string& what, std::string* err) {
  std::unique_lock<std::mutex> lk(m_);
  // Stopped stays stopped: there is nothing to pause.
  if (what == "pause" && status_ == Status::stopped) return true;
  // Telling another stream's track: the remote's buttons are for that stream.
  if (mirroring_locked() && (what == "play" || what == "pause" || what == "stop" || what == "next" ||
                             what == "previous")) {
    lk.unlock();
    {
      std::lock_guard<std::mutex> wl(wm_);
      cmds_.push_back(Cmd{Cmd::mirror, json{{"op", what}}});
    }
    wcv_.notify_one();
    return true;
  }
  if (what == "play") {
    if (queue_.empty()) {
      // Nothing chosen yet: the test signals, from the first.
      queue_ = lib_.tracks(lib_.root().front());
      pos_ = 0;
      if (shuffle_) shuffle_queue_locked(true);
      lk.unlock();
      changed(true, false, true);
      queue_cmd(Cmd::play);
      return true;
    }
    // Its stream plays on where it was (playing, nothing changes), as Listen's play; else the
    // track starts.
    const bool resume = stream_ != 0;
    lk.unlock();
    queue_cmd(resume ? Cmd::resume : Cmd::play);
    return true;
  }
  if (what == "pause" || what == "stop") {
    lk.unlock();
    queue_cmd(what == "pause" ? Cmd::pause : Cmd::stop);
    return true;
  }
  if (what == "next" || what == "previous") {
    if (queue_.empty()) {
      *err = "nothing to play: pick something from the library";
      return false;
    }
    const int n = static_cast<int>(queue_.size());
    // Wraps: a bench has no end of an album.
    pos_ = what == "next" ? next_pos_locked(true) : (pos_ + n - 1) % n;
    icy_title_.clear();
    art_.clear();
    const bool was = status_ == Status::playing;
    lk.unlock();
    changed(true, false, false);
    if (was) queue_cmd(Cmd::play);
    return true;
  }
  *err = "action is play, pause, stop, next or previous";
  return false;
}

bool BenchPlayer::play_item(int id, bool enqueue, std::string* err) {
  std::unique_lock<std::mutex> lk(m_);
  const LibItem* it = lib_.find(id);
  if (!it) {
    *err = "no such item";
    return false;
  }
  std::vector<int> tracks = it->folder ? lib_.tracks(id) : std::vector<int>{id};
  if (tracks.empty()) {
    *err = it->name + " has nothing to play";
    return false;
  }
  if (enqueue) {
    queue_.insert(queue_.end(), tracks.begin(), tracks.end());
    if (shuffle_) listed_.insert(listed_.end(), tracks.begin(), tracks.end());
    if (pos_ < 0) pos_ = 0;
    lk.unlock();
    changed(false, false, true);
    return true;
  }
  if (it->folder) {
    queue_ = std::move(tracks);
    pos_ = 0;
  } else {
    // A track plays in its folder: next and previous go through its siblings.
    queue_ = lib_.tracks(it->parent);
    pos_ = static_cast<int>(std::find(queue_.begin(), queue_.end(), id) - queue_.begin());
  }
  listed_.clear();
  if (shuffle_) shuffle_queue_locked(true);
  icy_title_.clear();
  art_.clear();
  lk.unlock();
  changed(true, false, true);
  queue_cmd(Cmd::play);
  return true;
}

int BenchPlayer::next_pos_locked(bool asked) {
  const int n = static_cast<int>(queue_.size());
  if (pos_ < 0 || n == 0) return -1;
  if (!asked && repeat_ == "track") return pos_;
  if (pos_ + 1 < n) return pos_ + 1;
  if (!asked && repeat_ != "all") return -1;
  // Round again; shuffled, in a new order that does not start with the track just played.
  if (shuffle_ && n > 1) {
    const int last = queue_[static_cast<size_t>(pos_)];
    std::shuffle(queue_.begin(), queue_.end(), rng_);
    if (queue_.front() == last) std::swap(queue_.front(), queue_.back());
    pending_playlist_ = true;
  }
  return 0;
}

void BenchPlayer::shuffle_queue_locked(bool on) {
  if (on) {
    listed_ = queue_;
    if (queue_.empty()) return;
    const auto playing = queue_.begin() + std::clamp(pos_, 0, static_cast<int>(queue_.size()) - 1);
    std::rotate(queue_.begin(), playing, playing + 1);
    std::shuffle(queue_.begin() + 1, queue_.end(), rng_);
    if (pos_ >= 0) pos_ = 0;
    return;
  }
  if (listed_.size() == queue_.size() && !queue_.empty()) {
    const int playing = pos_ >= 0 && pos_ < static_cast<int>(queue_.size()) ? queue_[static_cast<size_t>(pos_)] : -1;
    queue_ = listed_;
    if (playing >= 0) pos_ = static_cast<int>(std::find(queue_.begin(), queue_.end(), playing) - queue_.begin());
  }
  listed_.clear();
}

bool BenchPlayer::set_settings(const std::string& repeat, const bool* shuffle, std::string* err) {
  if (!repeat.empty() && repeat != "off" && repeat != "track" && repeat != "all") {
    *err = "repeat is off, track or all";
    return false;
  }
  std::unique_lock<std::mutex> lk(m_);
  if (mirroring_locked()) {
    // The told stream's own: the engine keeps them; the next poll reads them back.
    if (!mirror_url_) {
      *err = relay_ ? "the board relays a device: it has no playlist here" : "a test signal has no playlist to repeat or shuffle";
      return false;
    }
    json body{{"op", "settings"}};
    if (!repeat.empty()) body["repeat"] = repeat;
    if (shuffle) body["shuffle"] = *shuffle;
    lk.unlock();
    {
      std::lock_guard<std::mutex> wl(wm_);
      cmds_.push_back(Cmd{Cmd::mirror, body});
    }
    wcv_.notify_one();
    return true;
  }
  if (!repeat.empty()) repeat_ = repeat;
  const bool reorder = shuffle && *shuffle != shuffle_;
  if (reorder) {
    shuffle_ = *shuffle;
    shuffle_queue_locked(shuffle_);
  }
  save_state();
  lk.unlock();
  changed(false, false, reorder, true);
  return true;
}

void BenchPlayer::queue_cmd(Cmd::Kind k) {
  {
    std::lock_guard<std::mutex> lk(wm_);
    cmds_.push_back(Cmd{k, {}});
  }
  wcv_.notify_one();
}

// The Bluetooth sink to play into: PipeWire's default sink when it is a Bluetooth one, else the
// first Bluetooth sink, else the default sink (a speaker over the loopback card, say). With
// PipeWire stopped (for someone's own endpoint code) there is no Bluetooth sink to reach: the
// loopback card, so the player still plays and changes tracks for whoever tests AVRCP alone.
json BenchPlayer::pick_sink() {
  const json ep = audio_.endpoints(false);
  if (ep.value("backend", std::string{}) != "pipewire") {
    std::string alsa;
    if (ep.contains("sinks") && !ep["sinks"].empty()) alsa = ep["sinks"][0].value("id", std::string{});
    return json{{"type", "alsa"}, {"target", alsa.empty() ? "hw:Loopback,0,0" : alsa}};
  }
  std::string first;
  if (ep.contains("sinks"))
    for (const json& s : ep["sinks"]) {
      if (s.value("kind", std::string{}) != "bluetooth") continue;
      if (s.value("default", false)) return json{{"type", "pipewire"}, {"target", s.value("id", std::string{})}};
      if (first.empty()) first = s.value("id", std::string{});
    }
  return json{{"type", "pipewire"}, {"target", first}};
}

// Stopped: nothing playing, the same whoever stopped it (a remote, Listen): no track and no now
// playing list, so a remote shows nothing.
void BenchPlayer::stopped() {
  {
    std::lock_guard<std::mutex> lk(m_);
    status_ = Status::stopped;
    queue_.clear();
    listed_.clear();
    pos_ = -1;
    icy_title_.clear();
    art_.clear();
  }
  changed(true, true, true);
}

void BenchPlayer::stop_stream() {
  int id;
  {
    std::lock_guard<std::mutex> lk(m_);
    id = stream_;
    stream_ = 0;
  }
  if (id) audio_.remove(id);
}

void BenchPlayer::start_stream() {
  stop_stream();
  json source;
  std::string name;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (pos_ < 0 || pos_ >= static_cast<int>(queue_.size())) return;
    const LibItem& it = lib_.items()[queue_[pos_]];
    source = it.source;
    name = it.name;
  }
  json out;
  std::string err;
  int status = 0;
  bool ok;
  json sink;
  audio::AudioRoute* route = route_.load();
  if (route) {
    // The board's input, on the outputs picked in Listen. With none on, the remote that pressed
    // play still hears it: the Bluetooth sink the player would pick is turned on, shown in Play to
    // like any other output.
    json outputs = route->get()["outputs"];
    if (outputs.empty()) {
      outputs = json::array({pick_sink()});
      json r;
      int st = 0;
      if (!route->put(json{{"outputs", outputs}}, &r, &err, &st)) LOG_WARN("player: outputs: {}", err);
      err.clear();
    }
    sink = outputs[0];
    ok = route->play(json{{"source", source}, {"label", "player: " + name}}, &out, &err, &status);
  } else {
    sink = pick_sink();
    ok = audio_.create(json{{"source", source}, {"sink", sink}, {"label", "player: " + name}}, &out, &err, &status);
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    if (ok) {
      stream_ = out.value("id", 0);
      mirror_ = 0;
      mirror_md_ = nullptr;
      mirror_list_ = json::array();
      sink_ = sink.value("target", std::string{});
      status_ = Status::playing;
      error_.clear();
    } else {
      LOG_WARN("player: cannot play {}: {}", name, err);
      error_ = err;
      status_ = Status::stopped;
    }
  }
  changed(false, true, false);
}

// Once a second while the player plays nothing itself: the newest stream that plays to a Bluetooth
// sink becomes the track the remote is told about, its state the play state. The streams are a
// lock and a copy away (AudioEngine::list); whether PipeWire's default sink is a Bluetooth one (for
// a stream with no target) is asked only when such a stream is there.
void BenchPlayer::mirror_poll() {
  {
    // Nobody to tell while the player is not on the bus.
    std::lock_guard<std::mutex> lk(m_);
    if (!enabled_ || (!mirror_ && registered_json_.size() <= 2)) return;  // "[]"
  }
  const json all = audio_.list();
  std::vector<json> running, live;  // live: those that play to PipeWire
  int newest_default = 0;  // the newest stream with no target, if any
  for (const json& st : all.value("streams", json::array())) {
    const std::string state = st.value("state", std::string{});
    if (state != "running" && state != "starting") continue;
    if (st.value("label", std::string{}).compare(0, 8, "player: ") == 0) continue;  // its own
    running.push_back(st);
    bool pw = false;
    for (const json& sink : stream_sinks(st)) {
      if (sink.value("type", std::string{}) != "pipewire") continue;
      pw = true;
      if (sink.value("target", std::string{}).empty() && !newest_default) newest_default = st.value("id", 0);
    }
    if (pw) live.push_back(st);
  }
  // With the route, the remote is told about the board's input, wherever it plays, even nowhere.
  // A device the board relays is told as its own player tells it (a phone's track), or by its name
  // (a broadcast), and is playing while the board records it: a remote that presses play on it
  // then gets what it hears, not the library started in its place.
  audio::AudioRoute* route = route_.load();
  int route_local = 0, route_relay = 0;
  json relay_player = nullptr, relay_md = nullptr;
  if (route) {
    const json r = route->get();
    const std::string type = r["input"].value("type", std::string{});
    if (type == "local" && r["local"].is_number()) route_local = r["local"].get<int>();
    if (type == "bluetooth") {
      route_relay = r["stream"].is_number() && r["stream"].get<int>() > 0 ? r["stream"].get<int>() : kRelayIdle;
      const auto objs = bt_.objects();
      const json devices = r["input"].value("devices", json::array());
      for (const json& dv : devices) {
        if (objs) relay_player = model_device_player(*objs, dv.value("address", std::string{}));
        if (!relay_player.is_null()) break;
      }
      relay_md = relay_track(objs ? *objs : json::object(), devices, relay_player);
    }
  }
  // pw-dump is the costly part on a Zero W: asked when a new stream plays to the default sink, then
  // every 30 s while one does (in case the default moves).
  if (!route && newest_default && (newest_default != mirror_default_for_ || ++mirror_polls_ % 30 == 0)) {
    mirror_default_for_ = newest_default;
    std::string def_bt;
    const json ep = audio_.endpoints(false);
    for (const json& k : ep.value("sinks", json::array()))
      if (k.value("default", false) && k.value("kind", std::string{}) == "bluetooth") def_bt = k.value("id", std::string{});
    std::lock_guard<std::mutex> lk(m_);
    mirror_default_bt_ = def_bt;
  }
  bool track = false, status = false, playlist = false, settings = false;
  int64_t at = -1;  // where the stream is in its track, measured after the change is noted
  {
    std::lock_guard<std::mutex> lk(m_);
    if (stream_) return;  // its own play started meanwhile
    json pick = nullptr;  // the list is newest first
    const int want = route_local ? route_local : route_relay;
    for (const json& st : route ? running : live)
      if (route ? st.value("id", 0) == want : stream_to_bluetooth(st, mirror_default_bt_)) {
        pick = st;
        break;
      }
    const int id = route_relay ? route_relay : pick.is_null() ? 0 : pick.value("id", 0);
    const json md = route_relay ? relay_md : pick.is_null() ? json(nullptr) : stream_track(pick);
    const bool paused = !pick.is_null() && pick.value("paused", false);
    if (id != mirror_ || md != mirror_md_) {
      track = true;
      if (id > 0 && id != mirror_) LOG_INFO("player: telling remotes about stream {}", id);
    }
    Status st = !id ? (mirror_ ? Status::stopped : status_) : paused ? Status::paused : Status::playing;
    if (route_relay) {
      // The device's player knows best whether it plays; without one, the recording says.
      const std::string ps = relay_player.is_null() ? "" : relay_player.value("status", std::string{});
      st = ps == "playing" ? Status::playing : ps == "paused" ? Status::paused : ps == "stopped" ? Status::stopped
           : pick.is_null() ? Status::stopped : Status::playing;
    }
    if (!id && mirror_) {
      // The told stream ended (a stop, here or in Listen): nothing plays, so nothing is said to,
      // not the library's queue from before, which is not what stopped.
      queue_.clear();
      listed_.clear();
      pos_ = -1;
      track = playlist = true;
    }
    if (st != status_) status = true;
    const json& src = !pick.is_null() && pick.contains("source") && pick["source"].is_object() ? pick["source"] : json::object();
    const bool url = src.value("type", std::string{}) == "url";
    const std::string repeat = url ? src.value("repeat", std::string("off")) : "off";
    const bool shuffle = url && src.value("shuffle", false);
    if (repeat != mirror_repeat_ || shuffle != mirror_shuffle_) settings = true;
    const json list = url && src.contains("playlist") && src["playlist"].is_array() ? src["playlist"] : json::array();
    if (id != mirror_ || list != mirror_list_) playlist = true;
    mirror_list_ = list;
    mirror_track_ = pick.is_null() ? 0 : pick.value("track", 0);
    mirror_ = id;
    mirror_md_ = md;
    mirror_paused_ = paused;
    mirror_url_ = url;
    mirror_repeat_ = repeat;
    mirror_shuffle_ = shuffle;
    relay_ = route_relay != 0;
    relay_player_ = relay_player.is_null() ? "" : relay_player.value("path", std::string{});
    status_ = st;
    if (!relay_player.is_null()) at = relay_player.value("position_ms", int64_t{0});
    else if (id && !pick.is_null()) at = static_cast<int64_t>(pick.value("track_seconds", 0.0) * 1000);
  }
  if (track || status || playlist || settings) changed(track, status, playlist, settings);
  if (at >= 0) {
    bool wake;
    {
      std::lock_guard<std::mutex> lk(m_);
      wake = note_position_locked(at);
    }
    if (wake) bt_.post([](sd_bus*) {});
  }
}

// A remote's button on another stream, as Listen's: pause and play pause it in place (a radio, a
// playlist, a test signal), stop ends it, and next and previous move through a playlist.
void BenchPlayer::mirror_cmd(const json& body) {
  int id;
  bool relay;
  std::string player;
  {
    std::lock_guard<std::mutex> lk(m_);
    id = mirror_;
    relay = relay_;
    player = relay_player_;
  }
  if (!id) return;
  if (relay) {
    // A relayed device's buttons are its player's, as in Listen. A broadcast has none: it plays on.
    std::string method;
    int key = -1;
    if (!player.empty() && player_method(body.value("op", std::string{}), &method, &key))
      bt_.call(player, "org.bluez.MediaPlayer1", method, nullptr, 3000);
    mirror_poll();
    return;
  }
  // A track picked from a list the remote read while another stream played: not this one's.
  if (body.contains("stream") && body.value("stream", 0) != id) return;
  const std::string op = body.value("op", std::string{});
  const json s = audio_.get(id);
  if (s.is_null()) return;
  const bool url = s.contains("source") && s["source"].is_object() && s["source"].value("type", std::string{}) == "url";
  json out;
  std::string err;
  int status = 0;
  if (op == "stop") {
    audio_.remove(id);
  } else if (op == "pause" || op == "play") {
    audio_.update(id, json{{"paused", op == "pause"}}, &out, &err, &status);
  } else if (op == "track" && url) {
    audio_.update(id, json{{"track", body.value("track", 0)}, {"paused", false}}, &out, &err, &status);
  } else if ((op == "next" || op == "previous") && url) {
    audio_.update(id, json{{op, true}}, &out, &err, &status);
  } else if (op == "settings" && url) {
    json b = json::object();
    if (body.contains("repeat")) b["repeat"] = body["repeat"];
    if (body.contains("shuffle")) b["shuffle"] = body["shuffle"];
    audio_.update(id, b, &out, &err, &status);
  }
  if (!err.empty()) LOG_WARN("player: {} on stream {}: {}", op, id, err);
  mirror_poll();
}

void BenchPlayer::run_worker() {
  for (;;) {
    Cmd c{Cmd::poll, {}};
    {
      std::unique_lock<std::mutex> lk(wm_);
      // Polled once a second while something plays: the stream ending, a radio's title.
      wcv_.wait_for(lk, std::chrono::seconds(1), [this] { return quit_ || !cmds_.empty(); });
      if (quit_) break;
      if (!cmds_.empty()) {
        c = cmds_.front();
        cmds_.pop_front();
      }
    }
    switch (c.kind) {
      case Cmd::play:
        start_stream();
        break;
      case Cmd::resume:
      case Cmd::pause: {
        // As Listen's buttons: its stream pauses and plays on in place; resumed with none, the
        // track starts.
        int id;
        {
          std::lock_guard<std::mutex> lk(m_);
          id = stream_;
        }
        json out;
        std::string err;
        int st = 0;
        const bool pause = c.kind == Cmd::pause;
        if (id && audio_.update(id, json{{"paused", pause}}, &out, &err, &st)) {
          {
            std::lock_guard<std::mutex> lk(m_);
            status_ = pause ? Status::paused : Status::playing;
          }
          changed(false, true, false);
        } else if (!pause) {
          start_stream();
        }
        break;
      }
      case Cmd::stop:
        stop_stream();
        stopped();
        break;
      case Cmd::mirror:
        mirror_cmd(c.body);
        break;
      case Cmd::poll: {
        int id;
        {
          std::lock_guard<std::mutex> lk(m_);
          id = stream_;
        }
        if (!id) {
          mirror_poll();
          break;
        }
        const json s = audio_.get(id);
        const std::string st = s.is_null() ? "gone" : s.value("state", std::string{});
        if (st == "running" || st == "starting") {
          // Paused or played on from Listen: said as the remote's own buttons would.
          const Status now = s.value("paused", false) ? Status::paused : Status::playing;
          bool moved = false;
          {
            std::lock_guard<std::mutex> lk(m_);
            moved = now != status_;
            status_ = now;
          }
          if (moved) changed(false, true, false);
          const std::string title = s.is_null() ? "" : s["meta"].value("title", std::string{});
          const std::string art = s.is_null() ? "" : s["meta"].value("art", std::string{});
          bool track = false;
          {
            std::lock_guard<std::mutex> lk(m_);
            if (title != icy_title_ || art != art_) {
              icy_title_ = title;
              art_ = art;
              track = true;
            }
          }
          if (track) changed(true, false, false);
          bool wake;
          {
            std::lock_guard<std::mutex> lk(m_);
            wake = note_position_locked(static_cast<int64_t>(s.value("track_seconds", 0.0) * 1000));
          }
          if (wake) bt_.post([](sd_bus*) {});
          // Playing to something other than a Bluetooth sink (the one it had went away, or none
          // was there at play): every 5 s, look again, and move to one that has come (back), as a
          // source does when its speaker reconnects. Not more often: the look runs pw-dump.
          bool look;
          {
            std::lock_guard<std::mutex> lk(m_);
            // With the route, where it plays is what Listen's Play to says.
            look = !route_.load() && sink_.compare(0, 13, "bluez_output.") != 0 && ++sink_polls_ % 5 == 0;
          }
          if (look && pick_sink().value("target", std::string{}).compare(0, 13, "bluez_output.") == 0) {
            LOG_INFO("player: a Bluetooth sink is back, moving to it");
            start_stream();
          }
          break;
        }
        if (st == "gone") {
          // Stopped from Listen (or Audio → Streams): nothing playing, as after its own stop.
          {
            std::lock_guard<std::mutex> lk(m_);
            stream_ = 0;
          }
          stopped();
          break;
        }
        // It ended by itself (a sweep's end, a radio gone): the next track, as a player at the
        // end of a song; or stopped, with why, if it failed.
        bool next = false;
        {
          std::lock_guard<std::mutex> lk(m_);
          stream_ = 0;
          const int to = st == "ended" ? next_pos_locked(false) : -1;
          if (to >= 0) {
            pos_ = to;
            icy_title_.clear();
            art_.clear();
            next = true;
          } else {
            status_ = Status::stopped;
            if (!s.is_null() && !s.value("error", std::string{}).empty()) error_ = s.value("error", std::string{});
          }
        }
        if (next) {
          changed(true, false, false);
          start_stream();
        } else {
          changed(false, true, false);
        }
        break;
      }
    }
  }
  stop_stream();
}

// ---- the API ----------------------------------------------------------------------------------

bool BenchPlayer::set_enabled(bool on, std::string*) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (enabled_ == on) return true;
    enabled_ = on;
    save_state();
  }
  if (!on) queue_cmd(Cmd::stop);
  // The bus thread registers or unregisters at its next check.
  bt_.post([this](sd_bus*) { next_check_ns_ = 0; });
  return true;
}

json BenchPlayer::state() const {
  std::lock_guard<std::mutex> lk(m_);
  json lib = json::array();
  for (const LibItem& it : lib_.items()) {
    json j{{"id", it.id}, {"parent", it.parent < 0 ? json(nullptr) : json(it.parent)}, {"name", it.name},
           {"type", it.folder ? "folder" : "audio"}};
    if (it.folder) j["playable"] = it.playable;
    lib.push_back(std::move(j));
  }
  json queue = json::array();
  for (int id : queue_) queue.push_back(json{{"id", id}, {"name", lib_.items()[id].name}});
  json track = nullptr;
  if (pos_ >= 0 && pos_ < static_cast<int>(queue_.size())) {
    track = lib_item_metadata(lib_, lib_.items()[queue_[pos_]]);
    track["id"] = queue_[pos_];
    if (!icy_title_.empty()) track["Title"] = icy_title_;
  } else if (mirroring_locked()) {
    track = mirror_md_;
  }
  json reg = registered_json_.empty() ? json::array() : json::parse(registered_json_);
  return json{{"enabled", enabled_},
              {"path", kPath},
              {"registered", reg},
              {"error", error_},
              {"status", lower(status_name(static_cast<int>(status_)))},
              {"track", track},
              {"position", pos_ < 0 ? json(nullptr) : json{{"index", pos_}, {"count", queue_.size()}}},
              {"queue", queue},
              {"stream", stream_ ? json(stream_) : json(nullptr)},
              {"mirror", mirror_ > 0 ? json(mirror_) : json(nullptr)},
              {"repeat", mirroring_locked() ? mirror_repeat_ : repeat_},
              {"shuffle", mirroring_locked() ? mirror_shuffle_ : shuffle_},
              {"position_ms", position_locked(steady_ms())},
              {"sink", sink_},
              {"library", lib}};
}

}  // namespace btb
