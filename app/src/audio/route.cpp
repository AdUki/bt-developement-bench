#include "audio/route.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

#include "bluez_model.h"
#include "util/log.h"
#include "util/strings.h"

using json = nlohmann::json;

namespace btb::audio {

namespace {

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

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string str(const json& j, const char* k) {
  return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string{};
}

bool live(const json& s) {
  const std::string st = str(s, "state");
  return st == "starting" || st == "running";
}

// LE Audio's audio locations (Bluetooth Assigned Numbers 6.12.1): a part is the left or the right
// channel when it carries front left or front right and not both.
constexpr uint32_t kFrontLeft = 0x1, kFrontRight = 0x2;

const char* kKinds[] = {"a2dp", "bap", "broadcast", "hfp"};

const nlohmann::json kNoSources = nlohmann::json::array();
const nlohmann::json kNoTransports = nlohmann::json::array();

}  // namespace

std::string route_kind(const json& e) {
  if (str(e, "kind") != "bluetooth") return {};
  if (e.contains("transport") && e["transport"].is_string()) return "broadcast";
  const std::string p = str(e, "profile");
  if (starts_with(p, "a2dp")) return "a2dp";
  if (starts_with(p, "bap")) return "bap";
  if (starts_with(p, "headset") || starts_with(p, "audio-gateway") || starts_with(p, "hfp") || starts_with(p, "hsp"))
    return "hfp";
  // A node PipeWire made without a profile name (an older PipeWire): by its codec.
  const std::string c = str(e, "codec");
  if (c == "lc3") return "bap";
  if (c == "cvsd" || c == "msbc" || c == "lc3_swb") return "hfp";
  return "a2dp";
}

bool route_input_from_json(const json& j, std::string* type, std::vector<RouteDevice>* devices, std::string* err) {
  if (!j.is_object()) {
    *err = "input is {\"type\": \"none|local|bluetooth\", \"devices\": [{address, kind}]}";
    return false;
  }
  *type = str(j, "type");
  devices->clear();
  if (*type == "none" || *type == "local") return true;
  if (*type != "bluetooth") {
    *err = "input type is none, local or bluetooth";
    return false;
  }
  if (!j.contains("devices") || !j["devices"].is_array() || j["devices"].empty()) {
    *err = "a Bluetooth input names its devices: [{\"address\": \"AA:BB:…\", \"kind\": \"a2dp\"}]";
    return false;
  }
  for (const json& d : j["devices"]) {
    RouteDevice rd;
    rd.address = upper(d.is_string() ? d.get<std::string>() : str(d, "address"));
    rd.kind = d.is_object() ? str(d, "kind") : std::string{};
    if (!bt_address_ok(rd.address)) {
      *err = "not a Bluetooth address: " + rd.address;
      return false;
    }
    if (!rd.kind.empty() && std::find(std::begin(kKinds), std::end(kKinds), rd.kind) == std::end(kKinds)) {
      *err = "kind is a2dp, bap, broadcast or hfp";
      return false;
    }
    for (const RouteDevice& o : *devices)
      if (o.address == rd.address) {
        *err = rd.address + " is named twice";
        return false;
      }
    devices->push_back(rd);
  }
  // One input is heard at a time; two parts only when they are the two channels of one: a left
  // and a right broadcast.
  if (devices->size() > 2) {
    *err = "one input at a time: two devices only as the left and the right channel of one";
    return false;
  }
  if (devices->size() == 2)
    for (const RouteDevice& d : *devices)
      if (d.kind != "broadcast") {
        *err = "one input at a time: two devices only as two single-channel broadcasts, left and right";
        return false;
      }
  return true;
}

CaptureFind route_capture(const std::vector<RouteDevice>& devices, const json& sources, const json& transports,
                          SourceSpec* out, int* rate, std::string* err) {
  const json& tlist = transports.is_array() ? transports : kNoTransports;
  const auto transport_location = [&tlist](const std::string& tp) -> int64_t {
    for (const json& t : tlist)
      if (str(t, "path") == tp && t.contains("location") && t["location"].is_number()) return t["location"].get<int64_t>();
    return -1;
  };
  struct Part {
    const json* e;
    int64_t loc;
  };
  std::vector<Part> parts;
  // The parts point into the list: it has to be the caller's, never a copy.
  const json& list = sources.is_array() ? sources : kNoSources;
  for (const RouteDevice& d : devices) {
    std::vector<const json*> found;
    for (const json& e : list) {
      const std::string k = route_kind(e);
      if (k.empty() || (!d.kind.empty() && k != d.kind)) continue;
      const bool member = e.contains("set") && e["set"].is_string() && !e.value("set_leader", false);
      bool mine = upper(str(e, "address")) == d.address;
      // A coordinated set (a pair of earbuds) is captured from the node PipeWire makes of the set,
      // whichever of its members was picked; a member's own node is part of it.
      if (e.value("set_leader", false) && e.contains("members") && e["members"].is_array())
        for (const json& m : e["members"])
          if (m.is_string() && upper(m.get<std::string>()) == d.address) mine = true;
      if (mine && !member) found.push_back(&e);
    }
    // A broadcaster: its BISes being received (selected, syncing or synced) are one input, a node
    // each — they come one by one, and until every one is there a left and a right one would be
    // taken as the left one alone. A BIS's node names its transport, but for a broadcaster PipeWire
    // also knows as a device it puts the first BIS (fd0) on the device's own card, where the node
    // names none: a received stream there (not the Audio/Source of a unicast link the board set
    // up) is the BIS no other node claims.
    if (d.kind.empty() || d.kind == "broadcast") {
      std::vector<std::string> receiving;
      for (const json& t : tlist) {
        const std::string st = str(t, "state");
        if (str(t, "uuid") == "1851" && upper(str(t, "address")) == d.address &&
            (st == "broadcasting" || st == "pending" || st == "active"))
          receiving.push_back(str(t, "path"));
      }
      if (!receiving.empty()) {
        size_t have = 0;
        std::vector<std::string> unclaimed;
        for (const std::string& tp : receiving) {
          const auto n = std::find_if(found.begin(), found.end(), [&tp](const json* e) { return str(*e, "transport") == tp; });
          if (n != found.end()) {
            parts.push_back({*n, transport_location(tp)});
            ++have;
          } else {
            unclaimed.push_back(tp);
          }
        }
        if (unclaimed.size() == 1)
          for (const json& e : list)
            if (upper(str(e, "address")) == d.address && !(e.contains("transport") && e["transport"].is_string()) &&
                e.value("stream", false) && starts_with(str(e, "profile"), "bap")) {
              parts.push_back({&e, transport_location(unclaimed[0])});
              ++have;
              break;
            }
        if (have < receiving.size()) {
          *err = d.address + ": " + std::to_string(have) + " of its " + std::to_string(receiving.size()) +
                 " broadcast streams synced";
          return CaptureFind::kMissing;
        }
        continue;
      }
    }
    if (found.empty()) {
      *err = d.address + " sends the board no audio yet";
      return CaptureFind::kMissing;
    }
    // A device with several sources and no kind asked for (a phone's music and its call): the
    // music, then LE Audio, a call.
    if (found.size() > 1) {
      for (const char* k : kKinds) {
        const auto it = std::find_if(found.begin(), found.end(), [k](const json* e) { return route_kind(*e) == k; });
        if (it != found.end()) {
          found = {*it};
          break;
        }
      }
    }
    parts.push_back({found[0], transport_location(str(*found[0], "transport"))});
  }
  const auto side = [](int64_t loc) {
    if (loc < 0) return 0;
    const bool l = loc & kFrontLeft, r = loc & kFrontRight;
    return l && !r ? -1 : r && !l ? 1 : 0;
  };
  SourceSpec s;
  s.type = "capture";
  s.backend = "pipewire";
  if (parts.size() == 1) {
    s.target = str(*parts[0].e, "id");
  } else {
    const auto l = std::find_if(parts.begin(), parts.end(), [&side](const Part& p) { return side(p.loc) < 0; });
    const auto r = std::find_if(parts.begin(), parts.end(), [&side](const Part& p) { return side(p.loc) > 0; });
    if (parts.size() > 2 || l == parts.end() || r == parts.end()) {
      // A broadcaster with several BISes that are not a left and a right one (two stereo BISes,
      // two languages): the first of them.
      if (devices.size() == 1) {
        s.target = str(*parts[0].e, "id");
      } else {
        *err = "two broadcasts are one input only as a left and a right channel";
        return CaptureFind::kInvalid;
      }
    } else {
      s.target = str(*l->e, "id");
      s.target_right = str(*r->e, "id");
    }
  }
  s.title = str(*parts[0].e, "label");
  const json& e0 = *parts[0].e;
  *rate = e0.contains("rate") && e0["rate"].is_number() && e0["rate"].get<int>() > 0 ? e0["rate"].get<int>() : 0;
  // A node that does not say its rate records at its link's (a phone's A2DP is mostly 44.1 kHz):
  // recording it at another makes PipeWire resample, which on a Zero W costs more than the codec.
  // The link of the same kind: a device can have A2DP and LE Audio transports at once.
  const std::string k0 = route_kind(e0);
  const auto same_kind = [&k0](const json& t) {
    const std::string u = str(t, "uuid");
    return k0 == "a2dp" ? u == "110a" || u == "110b" : k0 == "broadcast" ? u == "1851" : k0 == "bap" ? starts_with(u, "2bc") : false;
  };
  if (!*rate && transports.is_array())
    for (const json& t : transports)
      if (upper(str(t, "address")) == devices[0].address && same_kind(t) && t.contains("codec") && t["codec"].is_object() &&
          t["codec"].contains("rate") && t["codec"]["rate"].is_number_integer() && t["codec"]["rate"].get<int>() > 0) {
        *rate = t["codec"]["rate"].get<int>();
        break;
      }
  if (!*rate) *rate = 48000;
  *out = s;
  return CaptureFind::kFound;
}

AudioRoute::AudioRoute(AudioEngine& engine, Publish publish, Transports transports, std::string state_file)
    : engine_(engine), publish_(std::move(publish)), transports_(std::move(transports)), state_file_(std::move(state_file)) {
  load();
  thread_ = std::thread([this] { run(); });
}

AudioRoute::~AudioRoute() {
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void AudioRoute::load() {
  std::ifstream f(state_file_);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  const json j = json::parse(ss.str(), nullptr, false);
  if (!j.is_object() || !j.contains("outputs")) return;
  // Checked as a request would be: a file edited by hand with a bad sink is not played into.
  StreamSpec spec;
  std::string err;
  if (stream_spec_from_json(json{{"source", {{"type", "silence"}}}, {"sinks", j["outputs"]}}, &spec, &err))
    outputs_ = stream_spec_json(spec)["sinks"];
  else
    LOG_WARN("route: {}: {}", state_file_, err);
}

void AudioRoute::save() const {
  const std::string tmp = state_file_ + ".tmp";
  {
    std::ofstream f(tmp);
    if (!f) return;
    f << json{{"outputs", outputs_}}.dump(2) << "\n";
  }
  std::rename(tmp.c_str(), state_file_.c_str());
}

bool AudioRoute::stream_alive(int id) const {
  if (!id) return false;
  const json s = engine_.get(id);
  return !s.is_null() && live(s);
}

std::string AudioRoute::who_locked(const json& ts) const {
  std::string who;
  for (const RouteDevice& d : devices_) {
    std::string name = d.address;
    for (const json& t : ts.is_array() ? ts : json::array())
      if (upper(str(t, "address")) == d.address && !str(t, "device_name").empty()) name = str(t, "device_name");
    who += (who.empty() ? "" : " and ") + name;
  }
  return who;
}

int AudioRoute::local_stream() const {
  if (stream_alive(paused_)) return paused_;
  const json l = engine_.list();
  for (const json& s : l["streams"])  // newest first
    if (live(s) && str(s["source"], "type") != "capture") return s.value("id", 0);
  return 0;
}

void AudioRoute::set_input_state(const std::string& state, const std::string& error) {
  if (state == state_ && error == error_) return;
  state_ = state;
  error_ = error;
  since_ms_ = wall_ms();
}

json AudioRoute::json_locked() const {
  json devs = json::array();
  for (const RouteDevice& d : devices_) devs.push_back({{"address", d.address}, {"kind", d.kind}});
  const int local = local_stream();
  const int stream = type_ == "bluetooth" ? (stream_alive(capture_) ? capture_ : 0) : type_ == "local" ? local : 0;
  // A board input with nothing loaded is idle rather than playing.
  const std::string state = type_ == "local" ? (local ? "playing" : "idle") : state_;
  return json{{"input", {{"type", type_}, {"devices", devs}, {"state", state}, {"error", error_}, {"since_ms", since_ms_}}},
              {"outputs", outputs_},
              {"stream", stream ? json(stream) : json(nullptr)},
              {"local", local ? json(local) : json(nullptr)}};
}

void AudioRoute::publish_locked() {
  if (publish_) publish_("audio.route", json_locked());
}

json AudioRoute::get() {
  std::lock_guard<std::mutex> lk(m_);
  return json_locked();
}

bool AudioRoute::bind_locked() {
  SourceSpec src;
  int rate = 48000;
  std::string err;
  const json ep = engine_.endpoints(false);
  const CaptureFind f = route_capture(devices_, ep.value("sources", json::array()), transports_ ? transports_() : json::array(),
                                      &src, &rate, &err);
  if (f == CaptureFind::kMissing) return false;
  if (f == CaptureFind::kInvalid) {
    set_input_state("failed", err);
    wait_until_ = 0;
    return true;
  }
  json body{{"source", {{"type", "capture"}, {"backend", src.backend}, {"target", src.target}, {"title", src.title}}},
            {"sinks", outputs_}, {"rate", rate}, {"channels", 2}};
  if (!src.target_right.empty()) body["source"]["target_right"] = src.target_right;
  json out;
  int status = 400;
  wait_until_ = 0;
  if (!engine_.create(body, &out, &err, &status)) {
    set_input_state("failed", err);
    return true;
  }
  capture_ = out.value("id", 0);
  LOG_INFO("route: input {} as stream {}", src.target + (src.target_right.empty() ? "" : " + " + src.target_right), capture_);
  set_input_state("playing", "");
  return true;
}

void AudioRoute::set_input_locked(const std::string& type, const std::vector<RouteDevice>& devices, json* updated,
                                  std::string* err, int* status) {
  // The input that was: the route's capture goes, and the board's own stream steps aside — a
  // playlist paused off its outputs (its queue kept, for when the board is the input again), a
  // test signal or a station stopped.
  if (capture_) engine_.remove(capture_);
  capture_ = 0;
  const int local = local_stream();
  type_ = type;
  devices_ = devices;
  wait_until_ = 0;
  if (type == "local") {
    paused_ = 0;
    if (local) {
      json body2{{"sinks", outputs_}};
      if (str(engine_.get(local)["source"], "type") == "url") body2["paused"] = false;
      engine_.update(local, body2, updated, err, status);
    }
    set_input_state(local ? "playing" : "idle", "");
    return;
  }
  if (local) {
    const json s = engine_.get(local);
    if (str(s["source"], "type") == "url") {
      json ignored;
      std::string e2;
      int st2 = 0;
      engine_.update(local, json{{"paused", true}, {"sinks", json::array()}}, &ignored, &e2, &st2);
      paused_ = local;
    } else {
      engine_.remove(local);
      paused_ = 0;
    }
  }
  if (type == "none") {
    set_input_state("idle", "");
  } else {
    set_input_state("waiting", "");
    wait_until_ = mono_ms() + kWaitMs;
    bind_locked();
  }
}

bool AudioRoute::put(const json& body, json* out, std::string* err, int* status) {
  *status = 400;
  if (!body.is_object() || (!body.contains("input") && !body.contains("outputs"))) {
    *err = "PUT {\"input\": {...}} and/or {\"outputs\": [sinks]}";
    return false;
  }
  std::string type;
  std::vector<RouteDevice> devices;
  if (body.contains("input") && !route_input_from_json(body["input"], &type, &devices, err)) return false;
  json outputs;
  if (body.contains("outputs")) {
    StreamSpec spec;
    if (!stream_spec_from_json(json{{"source", {{"type", "silence"}}}, {"sinks", body["outputs"]}}, &spec, err)) return false;
    outputs = stream_spec_json(spec)["sinks"];
  }
  std::lock_guard<std::mutex> lk(m_);
  json updated;
  if (!outputs.is_null()) {
    outputs_ = outputs;
    unused_ = true;  // an output taken off may be one to end (end_unused_)
    save();
    // What plays now plays on them at once: its kept outputs untouched, new ones opened.
    const int active = type_ == "bluetooth" ? (stream_alive(capture_) ? capture_ : 0) : type_ == "local" ? local_stream() : 0;
    if (active && !body.contains("input") && !engine_.update(active, json{{"sinks", outputs_}}, &updated, err, status)) return false;
  }
  if (body.contains("input")) set_input_locked(type, devices, &updated, err, status);
  *out = json_locked();
  if (!updated.is_null()) (*out)["updated"] = updated;
  publish_locked();
  cv_.notify_all();
  return true;
}

void AudioRoute::on_unused_outputs(EndUnused f) {
  std::lock_guard<std::mutex> lk(m_);
  end_unused_ = std::move(f);
}

bool AudioRoute::play(const json& body, json* out, std::string* err, int* status) {
  *status = 400;
  if (!body.is_object() || !body.contains("source") || str(body["source"], "type") == "capture") {
    *err = "the board's own input: a tone, sweep, noise, silence or url source (a device is picked with PUT input)";
    return false;
  }
  std::lock_guard<std::mutex> lk(m_);
  if (capture_) engine_.remove(capture_);
  capture_ = 0;
  // The board plays one thing of its own: every other one goes, the one paused for a device too.
  const json l = engine_.list();
  for (const json& s : l["streams"])
    if (live(s) && str(s["source"], "type") != "capture") engine_.remove(s.value("id", 0));
  paused_ = 0;
  json b = body;
  b.erase("sink");
  b["sinks"] = outputs_;
  if (!engine_.create(b, out, err, status)) return false;
  type_ = "local";
  devices_.clear();
  wait_until_ = 0;
  set_input_state("playing", "");
  publish_locked();
  return true;
}

bool AudioRoute::playing_locked() const {
  if (type_ == "bluetooth") return state_ == "playing" && stream_alive(capture_);
  if (type_ != "local") return false;
  const int local = local_stream();
  return local && !engine_.get(local).value("paused", false);
}

void AudioRoute::transports_changed_locked(const json& ts) {
  const auto started = [](const std::string& st) { return st == "pending" || st == "active"; };
  json now = json::object();
  for (const json& t : ts.is_array() ? ts : json::array()) now[str(t, "path")] = str(t, "state");
  if (seen_transports_.is_null()) {
    seen_transports_ = now;
    return;
  }
  for (const json& t : ts.is_array() ? ts : json::array()) {
    const std::string path = str(t, "path"), st = str(t, "state"), u = str(t, "uuid"), addr = upper(str(t, "address"));
    const bool had = seen_transports_.contains(path);
    const std::string before = had ? seen_transports_[path].get<std::string>() : std::string{};
    // The transport's UUID is the board's own role: A2DP source or a source PAC sends.
    if (!had && (u == "110a" || u == "2bcb")) sink_looks_[addr] = mono_ms() + kSinkLookMs;
    // A device that starts sending: A2DP's source starts its stream, a broadcast's BIS is synced.
    // LE Audio unicast is the board's own to start (the central enables it when it records), so
    // a receiving stream configured is the device being ready to send.
    const std::string kind = u == "110b" ? "a2dp" : u == "2bc9" ? "bap" : u == "1851" ? "broadcast" : "";
    if (kind.empty() || playing_locked()) continue;
    if (kind == "bap" ? had : started(before) || !started(st)) continue;
    LOG_INFO("route: {} sends while nothing plays: it is the input", addr);
    json ignored;
    std::string err;
    int status = 0;
    set_input_locked("bluetooth", {RouteDevice{addr, kind}}, &ignored, &err, &status);
  }
  seen_transports_ = now;
}

void AudioRoute::look_for_sinks_locked() {
  const int64_t now = mono_ms();
  const json ep = engine_.endpoints(false);
  const int active = type_ == "bluetooth" ? (stream_alive(capture_) ? capture_ : 0) : type_ == "local" ? local_stream() : 0;
  for (auto it = sink_looks_.begin(); it != sink_looks_.end();) {
    const std::string addr = it->first;
    std::string sink;
    for (const json& e : ep.value("sinks", json::array())) {
      const std::string k = route_kind(e);
      if ((k == "a2dp" || k == "bap") && upper(str(e, "address")) == addr) {
        sink = str(e, "id");
        break;
      }
    }
    if (sink.empty() && now < it->second) {
      ++it;
      continue;
    }
    it = sink_looks_.erase(it);
    if (sink.empty() || !active || !playing_locked()) continue;
    // Never the input's own device: what it sends would go back to it.
    bool input = false;
    for (const RouteDevice& d : devices_) input = input || (type_ == "bluetooth" && d.address == addr);
    if (input) continue;
    bool plays = false;
    const json s = engine_.get(active);
    for (const json& k : s.value("sinks_status", json::array())) {
      const std::string st = str(k, "state");
      plays = plays || st == "starting" || st == "playing" || st == "stalled";
    }
    if (plays) continue;
    bool listed = false;
    for (const json& o : outputs_) listed = listed || str(o, "target") == sink;
    if (!listed) outputs_.push_back(json{{"type", "pipewire"}, {"target", sink}});
    save();
    json updated;
    std::string err;
    int status = 0;
    if (engine_.update(active, json{{"sinks", outputs_}}, &updated, &err, &status))
      LOG_INFO("route: {} came while the input played to no output: it is one now", sink);
    else
      LOG_WARN("route: {} came, but the input cannot play to it: {}", sink, err);
  }
}

void AudioRoute::poke() {
  {
    std::lock_guard<std::mutex> lk(m_);
    // Every change may have left a broadcast unused (end_unused_); only a wait needs the binding.
    unused_ = true;
    bt_changed_ = true;
    if (wait_until_) poked_ = true;
  }
  cv_.notify_all();
}

void AudioRoute::run() {
  std::unique_lock<std::mutex> lk(m_);
  while (!stop_) {
    cv_.wait_for(lk, std::chrono::seconds(1), [this] { return stop_ || poked_ || unused_; });
    if (stop_) break;
    const bool unused = unused_, bt_changed = bt_changed_;
    poked_ = unused_ = bt_changed_ = false;
    const json before = json_locked();
    if (bt_changed && transports_) transports_changed_locked(transports_());
    if (!sink_looks_.empty()) look_for_sinks_locked();
    if (type_ == "bluetooth" && wait_until_) {
      if (!bind_locked()) {
        const json ts = transports_ ? transports_() : json::array();
        if (wait_until_ == kUntilBack) {
          // Gone from BlueZ altogether: said, and nothing reconnects it.
          bool there = false;
          for (const RouteDevice& d : devices_)
            for (const json& t : ts.is_array() ? ts : json::array()) there = there || upper(str(t, "address")) == d.address;
          if (!there) {
            wait_until_ = 0;
            // A broadcast has no link to connect: it ended, or the board lost its sync.
            const bool bcast = !devices_.empty() && devices_[0].kind == "broadcast";
            set_input_state("failed", who_locked(ts) + (bcast ? " stopped broadcasting: pick it again once it is back"
                                                              : " disconnected: connect it, then pick it again"));
          }
        } else if (mono_ms() > wait_until_) {
          wait_until_ = 0;
          set_input_state("failed", who_locked(ts) + " sent the board no audio within " + std::to_string(kWaitMs / 1000) +
                                        " s: start playing on it, then pick it again");
        }
      }
    } else if (type_ == "bluetooth" && capture_ && engine_.get(capture_).is_null()) {
      // The capture was removed by hand (Audio → Streams' Stop, DELETE /api/audio/streams/{id}):
      // that is the input stopped, not the device going quiet, so it is not waited for and bound
      // again — it would come right back.
      capture_ = 0;
      devices_.clear();
      type_ = "none";
      LOG_INFO("route: the input's stream was stopped; the route has no input");
      set_input_state("idle", "");
    } else if (type_ == "bluetooth" && capture_ && !stream_alive(capture_)) {
      // Its audio went away: the device paused or stopped (its node goes with the stream), remade
      // its node for another codec, or disconnected. The input stays that device, waited for while
      // it is connected: its play button (here or on it) brings the audio back without a pick.
      engine_.remove(capture_);
      capture_ = 0;
      LOG_INFO("route: input stopped sending; waiting for it");
      set_input_state("waiting", who_locked(transports_ ? transports_() : json::array()) + " stopped sending audio");
      wait_until_ = kUntilBack;
    }
    if (json_locked() != before) publish_locked();
    if (end_unused_ && unused) {
      const json outputs = outputs_;
      const EndUnused end = end_unused_;
      lk.unlock();
      end(outputs);
      lk.lock();
    }
  }
}

}  // namespace btb::audio
