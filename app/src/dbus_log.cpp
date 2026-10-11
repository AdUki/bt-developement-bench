#include "dbus_log.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>

#include "dbus_util.h"
#include "util/log.h"

using json = nlohmann::json;

namespace btb {

namespace {

constexpr const char* kRules[] = {"sender='org.bluez'", "destination='org.bluez'", "sender='org.bluez.obex'",
                                  "destination='org.bluez.obex'"};
constexpr size_t kMaxCalls = 512;           // calls waiting for their reply, tracked for latency
constexpr uint64_t kBatchUs = 250 * 1000;   // the topic carries a batch at most this often

uint64_t now_us() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

uint64_t mono_us() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

std::string str(const char* s) { return s ? s : ""; }

std::string who(const json& e, const char* name_key, const char* key) {
  const std::string n = e.value(name_key, std::string{});
  const std::string a = e.value(key, std::string{});
  if (n.empty()) return a;
  return a.empty() || a == n ? n : n + "(" + a + ")";
}

}  // namespace

std::string dbus_args_text(const json& args, size_t max, bool* cut) {
  std::string t = args.dump(-1, ' ', false, json::error_handler_t::replace);
  *cut = t.size() > max;
  if (*cut) {
    // Cut on a character boundary, not inside a UTF-8 sequence.
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xc0) == 0x80) --n;
    t = t.substr(0, n) + "… (" + std::to_string(t.size()) + " bytes)";
  }
  return t;
}

std::string dbus_log_line(const json& e) {
  char tbuf[32] = "";
  const uint64_t ms = e.value("ts", uint64_t{0});
  const time_t s = static_cast<time_t>(ms / 1000);
  struct tm tm{};
  localtime_r(&s, &tm);
  const size_t n = strftime(tbuf, sizeof tbuf, "%H:%M:%S", &tm);
  snprintf(tbuf + n, sizeof tbuf - n, ".%03u", static_cast<unsigned>(ms % 1000));

  const std::string type = e.value("type", std::string{});
  std::string line = std::string(tbuf) + " " + type + std::string(type.size() < 6 ? 6 - type.size() : 0, ' ') + " " +
                     who(e, "sender_name", "sender");
  const std::string dest = who(e, "dest_name", "dest");
  if (!dest.empty()) line += " → " + dest;
  const std::string args = e.contains("args") ? (e["args"].is_string() ? e["args"].get<std::string>() : e["args"].dump())
                                              : std::string{};
  if (type == "call" || type == "signal") {
    line += "  " + e.value("path", std::string{}) + " " + e.value("iface", std::string{}) + "." +
            e.value("member", std::string{});
    if (!args.empty() && args != "[]") line += " " + args;
  } else {
    const std::string call = e.value("call", std::string{});
    if (!call.empty()) line += "  " + call;
    if (e.contains("latency_ms") && e["latency_ms"].is_number()) line += " " + std::to_string(e["latency_ms"].get<int64_t>()) + " ms";
    if (type == "error") {
      line += ": " + e.value("error_name", std::string{});
      const std::string msg = e.value("error", std::string{});
      if (!msg.empty()) line += ": " + msg;
    } else if (!args.empty() && args != "[]") {
      line += ": " + args;
    }
  }
  return line;
}

DbusLog::DbusLog(Options opt, HasSubscribers has, Publish pub)
    : opt_(std::move(opt)), has_(std::move(has)), pub_(std::move(pub)) {}

DbusLog::~DbusLog() { stop(); }

void DbusLog::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void DbusLog::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

json DbusLog::entries(uint64_t since, size_t limit) const {
  std::lock_guard<std::mutex> lk(m_);
  json out = json::array();
  size_t first = ring_.size();
  while (first > 0 && ring_[first - 1]["seq"].get<uint64_t>() > since && ring_.size() - (first - 1) <= limit) --first;
  for (size_t i = first; i < ring_.size(); ++i) out.push_back(ring_[i]);
  return json{{"running", monitoring_}, {"error", error_}, {"seq", seq_}, {"file", opt_.file}, {"entries", out}};
}

bool DbusLog::open(std::string* err) {
  const char* env = getenv("DBUS_SYSTEM_BUS_ADDRESS");
  const std::string address = env && *env ? env : "unix:path=/run/dbus/system_bus_socket";
  auto connect = [&](sd_bus** b, bool monitor) {
    int r = sd_bus_new(b);
    if (r >= 0) r = sd_bus_set_address(*b, address.c_str());
    if (r >= 0 && monitor) r = sd_bus_set_monitor(*b, 1);
    if (r >= 0) r = sd_bus_set_bus_client(*b, 1);
    if (r >= 0) r = sd_bus_start(*b);
    return r;
  };
  int r = connect(&names_, false);
  if (r >= 0) r = connect(&bus_, true);
  if (r < 0) {
    *err = std::string("cannot reach the system bus: ") + strerror(-r);
    return false;
  }
  sd_bus_message* m = nullptr;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  r = sd_bus_message_new_method_call(bus_, &m, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                     "org.freedesktop.DBus.Monitoring", "BecomeMonitor");
  if (r >= 0) r = sd_bus_message_open_container(m, 'a', "s");
  for (const char* rule : kRules)
    if (r >= 0) r = sd_bus_message_append(m, "s", rule);
  if (r >= 0) r = sd_bus_message_close_container(m);
  if (r >= 0) r = sd_bus_message_append(m, "u", 0u);
  if (r >= 0) r = sd_bus_call(bus_, m, 0, &e, nullptr);
  sd_bus_message_unref(m);
  if (r < 0) {
    *err = std::string("BecomeMonitor: ") + (e.message ? e.message : strerror(-r));
    sd_bus_error_free(&e);
    return false;
  }
  sd_bus_error_free(&e);
  return true;
}

void DbusLog::close_bus() {
  bus_ = sd_bus_flush_close_unref(bus_);
  names_ = sd_bus_flush_close_unref(names_);
  labels_.clear();
  calls_.clear();
  call_order_.clear();
}

void DbusLog::run() {
  std::string logged;
  while (running_.load()) {
    std::string err;
    if (!open(&err)) {
      close_bus();
      if (err != logged) LOG_WARN("dbus log: {}", err);
      logged = err;
      {
        std::lock_guard<std::mutex> lk(m_);
        error_ = err;
        monitoring_ = false;
      }
      for (int i = 0; i < 50 && running_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    LOG_INFO("dbus log: watching org.bluez and org.bluez.obex{}", opt_.file.empty() ? "" : " → " + opt_.file);
    logged.clear();
    {
      std::lock_guard<std::mutex> lk(m_);
      error_.clear();
      monitoring_ = true;
    }
    while (running_.load()) {
      sd_bus_message* m = nullptr;
      const int r = sd_bus_process(bus_, &m);
      if (r < 0) {
        LOG_WARN("dbus log: lost the bus: {}", strerror(-r));
        break;
      }
      if (m) {
        const bool gone = sd_bus_message_is_signal(m, "org.freedesktop.DBus.Local", "Disconnected") > 0;
        if (!gone) handle(m);
        sd_bus_message_unref(m);
        if (gone) break;
        continue;
      }
      if (r > 0) continue;
      flush(false);
      sd_bus_wait(bus_, kBatchUs);
    }
    flush(true);
    close_bus();
    std::lock_guard<std::mutex> lk(m_);
    monitoring_ = false;
  }
  if (file_) fclose(file_);
  file_ = nullptr;
}

// A unique name as the program behind it (bluetoothd, wireplumber, btbenchd), looked up once:
// the log is read by people, who know programs, not :1.113.
std::string DbusLog::label(const std::string& name) {
  if (name.empty() || name == "org.freedesktop.DBus") return {};
  const auto it = labels_.find(name);
  if (it != labels_.end()) return it->second;
  std::string out;
  uint32_t pid = 0;
  sd_bus_message* reply = nullptr;
  if (names_ && sd_bus_call_method(names_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                   "GetConnectionUnixProcessID", nullptr, &reply, "s", name.c_str()) >= 0 &&
      sd_bus_message_read(reply, "u", &pid) >= 0 && pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    std::getline(f, out);
  }
  sd_bus_message_unref(reply);
  // A well-known name whose owner left is looked up again next time; unique names never return.
  if (!out.empty() || name[0] == ':') {
    if (labels_.size() > 256) labels_.clear();
    labels_[name] = out;
  }
  return out;
}

void DbusLog::handle(sd_bus_message* m) {
  uint8_t t = 0;
  if (sd_bus_message_get_type(m, &t) < 0) return;
  const std::string sender = str(sd_bus_message_get_sender(m));
  const std::string dest = str(sd_bus_message_get_destination(m));
  // The bus's own messages (NameAcquired, NameLost to this monitor), and BlueZ's housekeeping with
  // the bus (AddMatch, GetNameOwner for each client it watches): about the bus, not about Bluetooth.
  if (sender == "org.freedesktop.DBus" || dest == "org.freedesktop.DBus") return;

  json e;
  e["ts"] = now_us() / 1000;
  e["sender"] = sender;
  e["sender_name"] = label(sender);
  if (!dest.empty()) {
    e["dest"] = dest;
    e["dest_name"] = label(dest);
  }
  const char* sig = sd_bus_message_get_signature(m, 1);
  e["sig"] = str(sig);
  uint64_t cookie = 0, reply_cookie = 0;
  sd_bus_message_get_cookie(m, &cookie);
  bool count_objects = false;

  if (t == SD_BUS_MESSAGE_METHOD_CALL || t == SD_BUS_MESSAGE_SIGNAL) {
    e["type"] = t == SD_BUS_MESSAGE_SIGNAL ? "signal" : "call";
    e["path"] = str(sd_bus_message_get_path(m));
    e["iface"] = str(sd_bus_message_get_interface(m));
    e["member"] = str(sd_bus_message_get_member(m));
    if (t == SD_BUS_MESSAGE_METHOD_CALL) {
      const std::string key = sender + "/" + std::to_string(cookie);
      calls_[key] = Call{mono_us(), e["member"].get<std::string>()};
      call_order_.push_back(key);
      while (call_order_.size() > kMaxCalls) {
        calls_.erase(call_order_.front());
        call_order_.pop_front();
      }
    }
  } else {
    e["type"] = t == SD_BUS_MESSAGE_METHOD_ERROR ? "error" : "return";
    sd_bus_message_get_reply_cookie(m, &reply_cookie);
    // A reply goes to the unique name of whoever called.
    const auto it = calls_.find(dest + "/" + std::to_string(reply_cookie));
    if (it != calls_.end()) {
      e["call"] = it->second.member;
      e["latency_ms"] = (mono_us() - it->second.at_us) / 1000;
      count_objects = it->second.member == "GetManagedObjects";
      calls_.erase(it);
    }
    if (t == SD_BUS_MESSAGE_METHOD_ERROR) {
      const sd_bus_error* err = sd_bus_message_get_error(m);
      e["error_name"] = err ? str(err->name) : "";
      e["error"] = err ? str(err->message) : "";
    }
  }

  if (t != SD_BUS_MESSAGE_METHOD_ERROR) {
    json args = json::array();
    if (count_objects && sig && strcmp(sig, "a{oa{sa{sv}}}") == 0) {
      // The whole object tree, every few seconds: counted, not decoded.
      size_t n = 0;
      if (sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}") > 0)
        while (sd_bus_message_skip(m, "{oa{sa{sv}}}") > 0) ++n;
      args = std::to_string(n) + " objects";
    } else {
      for (;;) {
        json v;
        const int r = read_json(m, &v);
        if (r <= 0) break;
        args.push_back(std::move(v));
      }
    }
    bool cut = false;
    if (args.is_string()) {
      e["args"] = args;
    } else {
      const std::string text = dbus_args_text(args, opt_.max_args, &cut);
      if (cut) e["args"] = text;
      else e["args"] = std::move(args);
    }
    e["cut"] = cut;
  }

  write_line(dbus_log_line(e));
  {
    std::lock_guard<std::mutex> lk(m_);
    e["seq"] = ++seq_;
    ring_.push_back(e);
    while (ring_.size() > opt_.ring) ring_.pop_front();
  }
  if (has_ && has_()) batch_.push_back(std::move(e));
  flush(false);
}

void DbusLog::write_line(const std::string& line) {
  if (opt_.file.empty()) return;
  if (file_ && file_bytes_ + line.size() + 1 > opt_.max_file_bytes) {
    fclose(file_);
    file_ = nullptr;
    rename(opt_.file.c_str(), (opt_.file + ".1").c_str());
  }
  if (!file_) {
    file_ = fopen(opt_.file.c_str(), "a");
    if (!file_) return;
    struct stat st{};
    file_bytes_ = fstat(fileno(file_), &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
  }
  fputs(line.c_str(), file_);
  fputc('\n', file_);
  // Flushed per line: the file is in RAM (tmpfs), and a reader following it wants the line now.
  fflush(file_);
  file_bytes_ += line.size() + 1;
}

void DbusLog::flush(bool force) {
  if (batch_.empty()) return;
  const uint64_t now = mono_us();
  if (!force && now - last_flush_us_ < kBatchUs) return;
  last_flush_us_ = now;
  json out = json{{"entries", std::move(batch_)}};
  batch_ = json::array();
  if (pub_) pub_(out);
}

}  // namespace btb
