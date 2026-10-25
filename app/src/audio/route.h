#pragma once

// The board's route: one input playing to a set of outputs (docs/api.md "The route", Listen in
// the console). The input is the board's own music (a station, a media server's tracks, a test
// signal: one at a time) or what a Bluetooth device sends it (a phone over A2DP, an LE Audio
// source, an Auracast broadcast, a call over HFP). Any number of devices can be connected, but one
// input is heard at a time: the others play nowhere (WirePlumber links no bluez_input by itself on
// the bench, 50-btbench.conf) and are left alone, never paused or disconnected. The exception is an
// input whose parts carry different channels: two single-channel broadcasts, left and right, are
// one stereo input (a capture with target_right).
//
// It is a thin layer over AudioEngine: the input is a stream there (the board's own one, or a
// capture the route makes of the device), and the outputs are that stream's sinks, changed in
// place so a toggle never interrupts the music. Every browser and `curl` see the same route; only
// whether a browser listens is the browser's.
//
// Nothing is retried by itself: a device that sends no audio within 15 s of being picked, or whose
// audio goes away, leaves the input failed with the reason until it is picked again.

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "audio/engine.h"

namespace btb::audio {

// One part of a Bluetooth input: a device, and which of its audio (a phone has A2DP music and HFP
// call audio as two sources).
struct RouteDevice {
  std::string address;  // AA:BB:CC:DD:EE:FF
  std::string kind;     // a2dp | hfp | bap | broadcast | "" (whichever the device sends)
};

// What a source endpoint (GET /api/audio/endpoints) carries, in the route's words: a2dp, hfp,
// bap or broadcast; "" for one that is not Bluetooth.
std::string route_kind(const nlohmann::json& endpoint);

// Parses {"type": "none|local|bluetooth", "devices": [{address, kind}]}. More than one device
// only as a channel pair of broadcasts; the channels themselves are checked once the nodes are
// known (route_capture).
bool route_input_from_json(const nlohmann::json& j, std::string* type, std::vector<RouteDevice>* devices,
                           std::string* err);

enum class CaptureFind { kFound, kMissing, kInvalid };
// The capture of `devices` among `sources` (the endpoints' sources) and `transports` (the media
// model's, for each broadcast BIS's audio location): one target, or a left one and a right one.
// kMissing while a device's node is not there yet (it has not started sending), kInvalid with the
// reason for parts that do not make one input (two broadcasts on the same channel).
CaptureFind route_capture(const std::vector<RouteDevice>& devices, const nlohmann::json& sources,
                          const nlohmann::json& transports, SourceSpec* out, int* rate, std::string* err);

class AudioRoute {
 public:
  using Publish = std::function<void(const std::string& topic, const nlohmann::json& data)>;
  // The media model's transports (bluez_model's model_media), read when a capture is looked for.
  using Transports = std::function<nlohmann::json()>;
  AudioRoute(AudioEngine& engine, Publish publish, Transports transports, std::string state_file);
  ~AudioRoute();
  AudioRoute(const AudioRoute&) = delete;
  AudioRoute& operator=(const AudioRoute&) = delete;

  static constexpr int kWaitMs = 15000;
  // Ends what exists only to be one of the outputs and is not one (the board's own Auracast
  // broadcast): called from the route's thread, without its lock, with the outputs, when Bluetooth
  // changed (poke: a broadcast going off air) or the outputs did.
  using EndUnused = std::function<void(const nlohmann::json& outputs)>;
  void on_unused_outputs(EndUnused f);

  // GET /api/audio/route.
  nlohmann::json get();
  // PUT /api/audio/route: {"input": {...}} and/or {"outputs": [sinks]}.
  bool put(const nlohmann::json& body, nlohmann::json* out, std::string* err, int* status);
  // POST /api/audio/route/play: the board's own input, a POST /api/audio/streams body without
  // its sinks (the route's outputs are). It takes the place of every other stream of the board's
  // own and of the route's capture.
  bool play(const nlohmann::json& body, nlohmann::json* out, std::string* err, int* status);
  // Bluetooth changed (a transport came or went): a device waited for may have its node now.
  void poke();

 private:
  void run();
  void load();
  void save() const;
  // The board's own stream: the newest one that is not a capture and has not ended.
  int local_stream() const;
  bool bind_locked();
  void set_input_state(const std::string& state, const std::string& error);
  nlohmann::json json_locked() const;
  void publish_locked();
  bool stream_alive(int id) const;
  // The devices as the devices list names them, from BlueZ's transports ts.
  std::string who_locked(const nlohmann::json& ts) const;

  AudioEngine& engine_;
  Publish publish_;
  Transports transports_;
  std::string state_file_;

  mutable std::mutex m_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool poked_ = false;
  std::thread thread_;

  nlohmann::json outputs_ = nlohmann::json::array();  // the sinks, as stream_spec_json has them
  std::string type_ = "none";
  std::vector<RouteDevice> devices_;
  std::string state_ = "idle";  // idle | waiting | playing | failed
  std::string error_;
  int64_t since_ms_ = 0;
  int64_t wait_until_ = 0;  // steady ms; 0 while not waiting, kUntilBack with no deadline
  // A device that stopped sending is waited for as long as it stays connected.
  static constexpr int64_t kUntilBack = INT64_MAX;
  int capture_ = 0;         // the capture the route made for a Bluetooth input
  int paused_ = 0;          // the board's stream paused for a Bluetooth input, resumed for local
  EndUnused end_unused_;
};

}  // namespace btb::audio
