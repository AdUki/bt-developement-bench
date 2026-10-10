#include "bench_player.h"

#include <errno.h>
#include <string.h>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <chrono>
#include <fstream>

#include "audio/engine.h"
#include "dbus_util.h"
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

// ---- D-Bus trampolines ------------------------------------------------------------------------

namespace {

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

// The items under <player>/item/: one vtable for all of them, found by path.
int item_find(sd_bus*, const char* path, const char*, void* d, void** found, sd_bus_error*) {
  int id = 0;
  if (!self(d)->item_exists(path, &id)) return 0;
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
  } catch (const std::exception&) {
    // A broken file is replaced by the next change.
  }
}

void BenchPlayer::save_state() const {
  if (state_file_.empty()) return;
  std::ofstream f(state_file_ + ".tmp");
  f << json{{"enabled", enabled_}}.dump() << "\n";
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
  const std::vector<int> entries = folder_entries_locked(folder_);
  // Start and End are offsets, End included, as AVRCP's GetFolderItems has them.
  const long long n = static_cast<long long>(entries.size());
  const long long start = num_of(filter, "Start", 0);
  const long long end = std::min(num_of(filter, "End", n - 1), n - 1);
  r = sd_bus_message_open_container(reply, 'a', "{oa{sv}}");
  for (long long i = start; r >= 0 && i <= end && i < n; ++i) {
    const LibItem& it = lib_.items()[entries[i]];
    const std::string p = item_path(it.id);
    r = sd_bus_message_open_container(reply, 'e', "oa{sv}");
    if (r >= 0) r = sd_bus_message_append(reply, "o", p.c_str());
    if (r >= 0) {
      append_item_props(reply, it);
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

// The track's metadata: MediaItem1's keys, or MPRIS's (xesam:*, mpris:length in µs) for the player.
void BenchPlayer::append_metadata(sd_bus_message* m, int id, bool mpris) const {
  int r = sd_bus_message_open_container(m, 'a', "{sv}");
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
  if (iface == kItemIface) {
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
      return sd_bus_message_append(reply, "u", static_cast<uint32_t>(folder_entries_locked(folder_).size()));
    if (prop == "Name") {
      const std::string n = folder_ == kNowPlaying ? "/NowPlaying"
                            : folder_ == kSearch   ? "/Search"
                                                   : lib_.folder_name(folder_);
      return sd_bus_message_append(reply, "s", n.c_str());
    }
  } else {
    if (prop == "PlaybackStatus") return sd_bus_message_append(reply, "s", status_name(static_cast<int>(status_)));
    if (prop == "Metadata") {
      append_metadata(reply, pos_ >= 0 && pos_ < static_cast<int>(queue_.size()) ? queue_[pos_] : -1, true);
      return 1;
    }
    if (prop == "Playlist") return sd_bus_message_append(reply, "o", kNowPlayingPath);
    if (prop == "Position") return sd_bus_message_append(reply, "x", static_cast<int64_t>(0));
    if (prop == "Identity") return sd_bus_message_append(reply, "s", "btbench");
    if (prop == "CanSeek") return sd_bus_message_append(reply, "b", 0);
    return sd_bus_message_append(reply, "b", 1);  // Browsable, Searchable, CanControl, CanPlay, ...
  }
  return sd_bus_error_set(e, SD_BUS_ERROR_UNKNOWN_PROPERTY, prop.c_str());
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
    if (r >= 0 && pos_ >= 0) {
      r = sd_bus_message_open_container(m, 'e', "sv");
      if (r >= 0) r = sd_bus_message_append(m, "s", "Metadata");
      if (r >= 0) r = sd_bus_message_open_container(m, 'v', "a{sv}");
      if (r >= 0) append_metadata(m, queue_[pos_], true);
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
void BenchPlayer::changed(bool track, bool status, bool playlist) {
  {
    std::lock_guard<std::mutex> lk(m_);
    pending_track_ = pending_track_ || track;
    pending_status_ = pending_status_ || status;
    pending_playlist_ = pending_playlist_ || playlist;
  }
  bt_.post([](sd_bus*) {});  // wakes the bus loop
}

void BenchPlayer::emit_changes(sd_bus* bus) {
  bool track, status, playlist;
  {
    std::lock_guard<std::mutex> lk(m_);
    track = pending_track_;
    status = pending_status_;
    playlist = pending_playlist_;
    pending_track_ = pending_status_ = pending_playlist_ = false;
  }
  if (!slot_player_ || (!track && !status && !playlist)) return;
  // BlueZ takes a new Metadata as a new track (TRACK_CHANGED to the remote), and a Playlist that
  // is sent again as a changed now playing list (NOW_PLAYING_CONTENT_CHANGED).
  std::vector<const char*> props;
  if (track) props.push_back("Metadata");
  if (status) props.push_back("PlaybackStatus");
  if (playlist) props.push_back("Playlist");
  props.push_back(nullptr);
  sd_bus_emit_properties_changed_strv(bus, kPath, kMprisIface, const_cast<char**>(props.data()));
}

// ---- playing ----------------------------------------------------------------------------------

bool BenchPlayer::action(const std::string& what, std::string* err) {
  std::unique_lock<std::mutex> lk(m_);
  if (what == "play") {
    if (queue_.empty()) {
      // Nothing chosen yet: the test signals, from the first.
      queue_ = lib_.tracks(lib_.root().front());
      pos_ = 0;
      lk.unlock();
      changed(true, false, true);
    } else {
      lk.unlock();
    }
    queue_cmd(Cmd::play);
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
    pos_ = (pos_ + (what == "next" ? 1 : n - 1)) % n;  // wraps: a bench has no end of an album
    icy_title_.clear();
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
  icy_title_.clear();
  lk.unlock();
  changed(true, false, true);
  queue_cmd(Cmd::play);
  return true;
}

void BenchPlayer::queue_cmd(Cmd::Kind k) {
  {
    std::lock_guard<std::mutex> lk(wm_);
    cmds_.push_back(Cmd{k});
  }
  wcv_.notify_one();
}

// The Bluetooth sink to play into: PipeWire's default sink when it is a Bluetooth one, else the
// first Bluetooth sink, else the default sink (a speaker over the loopback card, say). Without
// PipeWire (audio mode "none") there is no Bluetooth sink to reach: the loopback card, so the
// player still plays and changes tracks for whoever tests AVRCP alone.
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
  const json sink = pick_sink();
  json body{{"source", source}, {"sink", sink}, {"label", "player: " + name}};
  json out;
  std::string err;
  int status = 0;
  const bool ok = audio_.create(body, &out, &err, &status);
  {
    std::lock_guard<std::mutex> lk(m_);
    if (ok) {
      stream_ = out.value("id", 0);
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

void BenchPlayer::run_worker() {
  for (;;) {
    Cmd c{Cmd::poll};
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
      case Cmd::stop:
      case Cmd::pause: {
        stop_stream();
        {
          std::lock_guard<std::mutex> lk(m_);
          // Stopped forgets where it was in the track (a tone has no "where"); either way the
          // next play starts the track again.
          status_ = c.kind == Cmd::pause ? Status::paused : Status::stopped;
        }
        changed(false, true, false);
        break;
      }
      case Cmd::poll: {
        int id;
        {
          std::lock_guard<std::mutex> lk(m_);
          id = stream_;
        }
        if (!id) break;
        const json s = audio_.get(id);
        const std::string st = s.is_null() ? "gone" : s.value("state", std::string{});
        if (st == "running" || st == "starting") {
          const std::string title = s.is_null() ? "" : s["meta"].value("title", std::string{});
          bool track = false;
          {
            std::lock_guard<std::mutex> lk(m_);
            if (title != icy_title_) {
              icy_title_ = title;
              track = true;
            }
          }
          if (track) changed(true, false, false);
          // Playing to something other than a Bluetooth sink (the one it had went away, or none
          // was there at play): every 5 s, look again, and move to one that has come (back), as a
          // source does when its speaker reconnects. Not more often: the look runs pw-dump.
          bool look;
          {
            std::lock_guard<std::mutex> lk(m_);
            look = sink_.compare(0, 13, "bluez_output.") != 0 && ++sink_polls_ % 5 == 0;
          }
          if (look && pick_sink().value("target", std::string{}).compare(0, 13, "bluez_output.") == 0) {
            LOG_INFO("player: a Bluetooth sink is back, moving to it");
            start_stream();
          }
          break;
        }
        // It ended by itself (a sweep's end, a radio gone): the next track, as a player at the
        // end of a song; or stopped, with why, if it failed.
        bool next = false;
        {
          std::lock_guard<std::mutex> lk(m_);
          stream_ = 0;
          if (st == "ended" && pos_ >= 0 && pos_ + 1 < static_cast<int>(queue_.size())) {
            ++pos_;
            icy_title_.clear();
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
              {"sink", sink_},
              {"library", lib}};
}

}  // namespace btb
