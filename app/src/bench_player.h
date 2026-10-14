#pragma once

// The board's own media player, for what it plays to (a car, a speaker, headphones: the board as
// A2DP source): registered with org.bluez.Media1 on every adapter, so BlueZ's AVRCP target shows
// it to the remote controller with its track and play state, takes play/pause/next/previous from
// it, and lets it browse a media library (docs/audio.md "The board's player"). Browsing needs the
// BlueZ series in yocto/meta-btbench/recipes-connectivity/bluez5/bluez5/ (upstream BlueZ answers
// the target's browsing commands with "invalid scope"); without it the player still shows and
// plays.
//
// The library: test signals (tones, a sweep, noise, silence, and a folder of levels) and the radio
// stations of the Listen page. Playing an item starts an audio stream (audio/engine.h) into the
// Bluetooth sink; the folder it came from, or what was added to it, is the now playing list that
// next and previous move through. A radio stream's ICY title becomes the track's title.
//
// The D-Bus side follows BlueZ's target role documents: the player object implements
// org.mpris.MediaPlayer2.Player and org.bluez.MediaFolder1 (the root of the library), each item an
// org.bluez.MediaItem1 under <player>/item/<n>. Everything that touches the bus runs on the bus
// thread; starting and stopping streams (which runs pw-dump and spawns processes) runs on a worker
// of its own, so a slow PipeWire never stalls the pairing agent.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "bluetooth.h"
#include "dbus_export.h"

namespace btb {

namespace audio {
class AudioEngine;
}
class CoverArtServer;

// One entry of the library: a folder or something to play.
struct LibItem {
  int id = 0;          // its index in the library; the object is <player>/item/<id>
  int parent = -1;     // -1: in the root
  std::string name;
  bool folder = false;
  std::string folder_type = "mixed";  // MediaItem1 FolderType
  bool playable = false;              // a folder: plays its tracks
  nlohmann::json source;              // what to play: a POST /api/audio/streams "source"
  std::string artist, album, genre;
  uint32_t duration_ms = 0;  // 0: no end (a tone, a radio)
  std::vector<int> children;
};

// The library and the now playing list, without any bus: what the D-Bus methods and the API
// compute from (tested in tests/test_bench_player.cpp).
class BenchLibrary {
 public:
  // The test signals and the radio stations ({"stations": [{name, url}]} as AudioEngine::radio()).
  void build(const nlohmann::json& radio);
  const std::vector<LibItem>& items() const { return items_; }
  const LibItem* find(int id) const;
  const std::vector<int>& root() const { return root_; }
  // The folder's entries (root: -1).
  const std::vector<int>& children(int folder) const;
  // The tracks a folder plays: its entries that are not folders, in order.
  std::vector<int> tracks(int folder) const;
  // Tracks whose name has every word of `q` (case-insensitive), in library order.
  std::vector<int> search(const std::string& q) const;
  // "/Filesystem/Test signals/Levels"
  std::string folder_name(int folder) const;

 private:
  int add(LibItem it);
  std::vector<LibItem> items_;
  std::vector<int> root_;
};

// MediaItem1's Metadata of a track: Title, Artist, Album, Genre, Number, NumberOfTracks, Duration.
nlohmann::json lib_item_metadata(const BenchLibrary& lib, const LibItem& it);

// The track an audio stream plays (one of GET /api/audio/streams), as the player tells a remote,
// in lib_item_metadata's keys: a radio's ICY title (its station the album), a playlist's current
// item (its number of all), a test signal by name. For streams the player did not start itself.
nlohmann::json stream_track(const nlohmann::json& stream);
// Whether a stream plays to a Bluetooth sink: its target is one, or it plays to PipeWire's default
// sink (target "") and `default_bt` is that default's id ("" when the default is not Bluetooth).
bool stream_to_bluetooth(const nlohmann::json& stream, const std::string& default_bt);

class BenchPlayer : public BusUser {
 public:
  static constexpr const char* kPath = "/org/btbench/player";

  // `state_file` keeps whether the player is offered at all (on by default).
  BenchPlayer(BtManager& bt, audio::AudioEngine& audio, std::string state_file = "");
  ~BenchPlayer() override;
  BenchPlayer(const BenchPlayer&) = delete;
  BenchPlayer& operator=(const BenchPlayer&) = delete;

  // GET /api/media/local-player: {enabled, registered:[adapter paths], error, status, track,
  // position:{index, count}, queue:[{id, name}], stream, sink, library:[...tree...]}
  nlohmann::json state() const;
  bool set_enabled(bool on, std::string* err);
  // The console's controls, the same as the remote's: play, pause, stop, next, previous; and
  // playing (or queueing) a library entry by its id. False with *err for an unknown one.
  bool action(const std::string& what, std::string* err);
  bool play_item(int id, bool enqueue, std::string* err);
  // Repeat (off | track | all) and shuffle, as a remote sets them (MPRIS LoopStatus and Shuffle):
  // of the stream it tells about when there is one, else of its own now playing list. "" or a
  // null `shuffle` leaves that one as it is.
  bool set_settings(const std::string& repeat, const bool* shuffle, std::string* err);
  // The cover art responder (obex/cover_art.h): registered as the player's ObexPort, and the
  // track's cover goes out as an image handle while a controller is connected to it. Set before
  // the bus opens; the track is told again whenever that connection comes or goes.
  void set_cover_art(CoverArtServer* cover);

  // BusUser
  void bus_opened(sd_bus* bus) override;
  void bus_closing() override;
  void bluez_ready(sd_bus* bus, const std::string& adapter_path) override;
  void bluez_gone() override;
  void tick(sd_bus* bus, uint64_t now_ns) override;

  // The D-Bus handlers (static trampolines in the .cpp reach these).
  int on_mpris(sd_bus_message* m, const std::string& method, sd_bus_error* e);
  int on_change_folder(sd_bus_message* m, sd_bus_error* e);
  int on_list_items(sd_bus_message* m, sd_bus_error* e);
  int on_search(sd_bus_message* m, sd_bus_error* e);
  int on_item(sd_bus_message* m, const std::string& method, sd_bus_error* e);
  int get_prop(sd_bus_message* reply, const std::string& iface, const std::string& prop,
               const std::string& path, sd_bus_error* e);
  int set_prop(sd_bus_message* value, const std::string& prop, sd_bus_error* e);
  bool item_exists(const std::string& path, int* id) const;
  size_t item_count() const;

 private:
  enum class Status { stopped, playing, paused };
  struct Cmd {
    enum Kind { play, stop, pause, poll, mirror } kind;
    nlohmann::json body;  // mirror: {op: pause|play|stop|next|previous|settings, repeat, shuffle}
  };

  void load_state();
  void save_state() const;
  // Folder object path → folder id (-1 root), or false.
  bool folder_of(const std::string& path, int* folder) const;
  std::vector<int> folder_entries_locked(int folder) const;
  std::string item_path(int id) const;
  void append_metadata(sd_bus_message* m, int id, bool mpris) const;
  int append_img_handle(sd_bus_message* m, const std::string& art) const;
  void append_item_props(sd_bus_message* m, const LibItem& it) const;
  void register_on(sd_bus* bus, const std::string& adapter);
  void unregister_all(sd_bus* bus);
  void changed(bool track, bool status, bool playlist, bool settings = false);
  // Where the track is now, as measured by a poll (the stream's track_seconds); m_ held.
  // True when the bus loop has a Seeked to send (the caller wakes it, with m_ released).
  bool note_position_locked(int64_t ms);
  // The now playing list's next position: at a track's end (`asked` false) or on next, with
  // repeat and shuffle; -1 at the end. m_ held.
  int next_pos_locked(bool asked);
  int64_t position_locked(int64_t now_ms) const;
  void emit_changes(sd_bus* bus);
  void queue_cmd(Cmd::Kind k);
  void run_worker();
  void start_stream();
  void mirror_poll();
  void mirror_cmd(const nlohmann::json& body);
  bool mirroring_locked() const { return stream_ == 0 && mirror_ != 0; }
  void stop_stream();
  nlohmann::json pick_sink();

  BtManager& bt_;
  audio::AudioEngine& audio_;
  const std::string state_file_;

  mutable std::mutex m_;
  bool enabled_ = true;
  BenchLibrary lib_;
  std::vector<int> queue_;  // the now playing list: track ids
  int pos_ = -1;            // in queue_
  Status status_ = Status::stopped;
  int stream_ = 0;          // the stream that plays it (0: none)
  // Not playing itself, the player tells what another stream plays to a Bluetooth sink (one
  // started from the console's Play and listen, a radio, a media server): its id, its track.
  int mirror_ = 0;
  nlohmann::json mirror_md_;
  bool mirror_paused_ = false;
  unsigned mirror_polls_ = 0;
  std::string mirror_default_bt_;  // PipeWire's default sink when it is a Bluetooth one
  int mirror_default_for_ = 0;     // the stream it was asked for
  // Repeat and shuffle: its own (for its now playing list), and the told stream's (from its
  // spec, read at each poll).
  std::string repeat_ = "off";
  bool shuffle_ = false;
  std::mt19937 rng_{std::random_device{}()};
  std::string mirror_repeat_ = "off";
  bool mirror_shuffle_ = false;
  bool mirror_url_ = false;  // the told stream is a URL stream: it has a playlist to repeat
  // Where the track is: pos_ms_ measured at pos_at_ms_ (steady clock), moving while it plays.
  // BlueZ keeps its own count for the remote (a new track is 0, a clock runs while Playing):
  // told_* is that count, so a Seeked goes out only when the two drift apart (a stream joined
  // midway, a pause the remote missed, a stall).
  int64_t pos_ms_ = 0, pos_at_ms_ = 0;
  int64_t told_ms_ = 0, told_at_ms_ = 0;
  bool told_playing_ = false;
  std::string sink_;        // where it plays
  unsigned sink_polls_ = 0;  // polls since it last looked for a Bluetooth sink
  std::string icy_title_;   // a radio's current title
  std::string art_;         // its cover (ICY StreamUrl): an image URL, "" for none
  CoverArtServer* cover_ = nullptr;
  std::string error_;
  std::vector<int> search_;
  int folder_ = -1;  // MediaFolder1's current folder (the root, an item, or kNowPlaying/kSearch)
  bool pending_track_ = false, pending_status_ = false, pending_playlist_ = false;
  bool pending_settings_ = false, pending_seek_ = false;

  // Bus thread only.
  sd_bus* bus_ = nullptr;
  sd_bus_slot* slot_player_ = nullptr;
  sd_bus_slot* slot_folder_ = nullptr;
  sd_bus_slot* slot_items_ = nullptr;
  sd_bus_slot* slot_enum_ = nullptr;
  std::set<std::string> registered_;   // adapters it is registered with
  std::set<std::string> registering_;  // asked, no answer yet
  uint64_t next_check_ns_ = 0;
  bool bluez_ = false;
  std::string registered_json_;

  std::thread worker_;
  std::mutex wm_;
  std::condition_variable wcv_;
  std::deque<Cmd> cmds_;
  bool quit_ = false;
};

}  // namespace btb
