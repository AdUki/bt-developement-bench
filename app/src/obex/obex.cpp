#include "obex/obex.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <chrono>
#include <future>

#include "dbus_util.h"
#include "util/exec.h"
#include "util/log.h"
#include "util/strings.h"

using json = nlohmann::json;

namespace btb::obex {

namespace {

constexpr const char* kObex = "org.bluez.obex";
constexpr const char* kClientPath = "/org/bluez/obex";
constexpr const char* kAgentPath = "/org/btbench/obexagent";
constexpr int kCallMs = 30000;          // an OBEX connect pages the device: allow for it
constexpr uint64_t kRequestNs = 55ull * 1000000000ull;  // obexd's own agent timeout is 60 s

uint64_t mono_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void mkdirs(const std::string& p) {
  std::string acc;
  for (const std::string& part : split(p, '/')) {
    acc += "/" + part;
    mkdir(acc.c_str(), 0755);
  }
}

json dir_listing(const std::string& dir) {
  json out = json::array();
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.empty() || n[0] == '.') continue;
    struct stat st{};
    if (stat((dir + "/" + n).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    out.push_back(json{{"name", n}, {"size", static_cast<long long>(st.st_size)},
                       {"mtime", static_cast<long long>(st.st_mtime) * 1000}});
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const json& a, const json& b) { return a["mtime"] > b["mtime"]; });
  return out;
}

// The target names the API uses, and the UUIDs obexd reports them by in Session1.Target.
const std::pair<const char*, const char*> kTargets[] = {
    {"opp", "00001105-0000-1000-8000-00805f9b34fb"},
    {"ftp", "f9ec7bc4-953c-11d2-984e-525400dc9e09"},
    {"pbap", "796135f0-f0c5-11d8-0966-0800200c9a66"},
    {"map", "bb582b40-420c-11db-b0de-0800200c9a66"},
    {"bip-avrcp", "7163dd54-4a7e-11e2-b47c-0050c2490048"},
};

// obexd reports a session's Target as the profile's service class (FTP 0x1106, PBAP's PSE 0x112f,
// MAP's MAS 0x1132, OPP 0x1105), the fake as the OBEX target UUID: both name the same profile.
const std::pair<const char*, const char*> kServiceClasses[] = {
    {"opp", "00001105-0000-1000-8000-00805f9b34fb"},
    {"ftp", "00001106-0000-1000-8000-00805f9b34fb"},
    {"pbap", "0000112f-0000-1000-8000-00805f9b34fb"},
    {"map", "00001132-0000-1000-8000-00805f9b34fb"},
    {"bip-avrcp", "0000111b-0000-1000-8000-00805f9b34fb"},
};

std::string target_name(const std::string& uuid_or_name) {
  const std::string l = lower(uuid_or_name);
  for (const auto& [n, u] : kTargets)
    if (l == n || l == u) return n;
  for (const auto& [n, u] : kServiceClasses)
    if (l == u) return n;
  return l;
}

std::string str(const json& j, const char* k, const std::string& def = "") {
  return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : def;
}

// A file name a remote device chose, made safe for in/: the base name, no leading dot, nothing
// but printable characters — never a path out of the folder.
std::string safe_name(const std::string& raw) {
  std::string n = raw.substr(raw.find_last_of('/') == std::string::npos ? 0 : raw.find_last_of('/') + 1);
  std::string out;
  for (const unsigned char c : n)
    if (c >= 0x20 && c != 0x7f && c != '\\') out.push_back(static_cast<char>(c));
  while (!out.empty() && out[0] == '.') out.erase(0, 1);
  if (out.empty()) out = "received";
  return out.substr(0, 200);
}

}  // namespace

// ---- pure helpers -------------------------------------------------------------------------------

bool target_ok(const std::string& t) {
  for (const auto& [n, u] : kTargets)
    if (t == n) return true;
  return false;
}

bool file_name_ok(const std::string& n) {
  if (n.empty() || n.size() > 200 || n[0] == '.') return false;
  for (const unsigned char c : n)
    if (c < 0x20 || c == '/' || c == '\\' || c == 0x7f) return false;
  return true;
}

std::string transfer_id(const std::string& path) {
  const std::string pre = "/org/bluez/obex/";
  std::string id = starts_with(path, pre) ? path.substr(pre.size()) : path;
  if (starts_with(id, "client/")) id = id.substr(7);
  return id;
}

json transfer_json(const std::string& path, const json& p) {
  const std::string session = str(p, "Session");
  const bool server = path.find("/server/") != std::string::npos;
  return json{{"id", transfer_id(path)},
              {"path", path},
              {"session", transfer_id(session)},
              {"status", str(p, "Status")},
              {"name", str(p, "Name")},
              {"type", str(p, "Type")},
              {"filename", str(p, "Filename")},
              {"size", p.contains("Size") ? p["Size"] : json(nullptr)},
              {"transferred", p.contains("Transferred") ? p["Transferred"] : json(0)},
              {"direction", server ? "in" : "client"}};
}

json folder_entries(const json& reply) {
  json out = json::array();
  const json& list = reply.is_array() && !reply.empty() ? reply[0] : json::array();
  if (!list.is_array()) return out;
  for (const json& e : list) {
    if (!e.is_object()) continue;
    json x{{"name", str(e, "Name")}, {"type", str(e, "Type")}};
    for (const char* k : {"Size", "Modified", "Created", "Accessed", "Permission"})
      if (e.contains(k)) x[lower(k)] = e[k];
    out.push_back(std::move(x));
  }
  return out;
}

json vcard_list(const json& reply) {
  json out = json::array();
  const json& list = reply.is_array() && !reply.empty() ? reply[0] : json::array();
  if (!list.is_array()) return out;
  for (const json& e : list)
    if (e.is_array() && e.size() >= 2) out.push_back(json{{"vcard", e[0]}, {"name", e[1]}});
  return out;
}

json message_list(const json& reply) {
  json out = json::array();
  const json& list = reply.is_array() && !reply.empty() ? reply[0] : json::object();
  if (!list.is_object()) return out;
  for (auto it = list.begin(); it != list.end(); ++it) {
    const json& m = it.value();
    json x{{"path", it.key()}, {"id", it.key().substr(it.key().rfind('/') + 1)}};
    for (const char* k : {"Subject", "Timestamp", "Sender", "SenderAddress", "Recipient",
                          "RecipientAddress", "Type", "Size", "Status", "Read", "Sent", "Protected",
                          "Priority", "Folder"})
      if (m.contains(k)) x[lower(k)] = m[k];
    out.push_back(std::move(x));
  }
  return out;
}

json image_properties(const json& reply) {
  json out = json::array();
  const json& list = reply.is_array() && !reply.empty() ? reply[0] : json::array();
  if (!list.is_array()) return out;
  for (const json& e : list) {
    if (!e.is_object()) continue;
    json x = json::object();
    for (auto it = e.begin(); it != e.end(); ++it) x[lower(it.key())] = it.value();
    out.push_back(std::move(x));
  }
  return out;
}

// ---- the bus thread's callbacks -----------------------------------------------------------------

namespace {

struct CallOp {
  std::shared_ptr<std::promise<CallResult>> done;
};

int call_reply(sd_bus_message* m, void* userdata, sd_bus_error*) {
  auto* op = static_cast<CallOp*>(userdata);
  CallResult r;
  if (sd_bus_message_is_method_error(m, nullptr)) {
    const sd_bus_error* e = sd_bus_message_get_error(m);
    r.error = e && e->message ? e->message : "error";
    r.error_name = e && e->name ? e->name : "";
    if (r.error_name == "org.freedesktop.DBus.Error.NoReply") r.timed_out = true;
  } else {
    r.ok = true;
    json v;
    while (read_json(m, &v) > 0) r.reply.push_back(v);
  }
  if (op->done) {
    op->done->set_value(std::move(r));
    op->done.reset();
  }
  return 0;
}

}  // namespace

struct ObexManager::Bus {
  static int signal(sd_bus_message* m, void* userdata, sd_bus_error*) {
    static_cast<ObexManager*>(userdata)->on_signal(m);
    return 0;
  }
  static int release(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, ""); }
  static int authorize(sd_bus_message* m, void* userdata, sd_bus_error*) {
    return static_cast<ObexManager*>(userdata)->agent_call(m, "AuthorizePush");
  }
  static int cancel(sd_bus_message* m, void* userdata, sd_bus_error*) {
    return static_cast<ObexManager*>(userdata)->agent_call(m, "Cancel");
  }
  static const sd_bus_vtable* agent_vtable() {
    static const sd_bus_vtable v[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Release", "", "", release, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("AuthorizePush", "o", "s", authorize, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Cancel", "", "", cancel, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END};
    return v;
  }
};

// ---- lifecycle ----------------------------------------------------------------------------------

ObexManager::ObexManager(Options opt, Publish publish, HasSubscribers has_subscribers)
    : opt_(std::move(opt)), publish_(std::move(publish)), has_subscribers_(std::move(has_subscribers)) {
  for (const char* d : {"in", "out", "shared", "in/cover"}) mkdirs(opt_.root + "/" + d);
  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  running_.store(true);
  thread_ = std::thread([this] { run(); });
}

ObexManager::~ObexManager() {
  stop();
  running_.store(false);
  if (wake_fd_ >= 0) eventfd_write(wake_fd_, 1);
  if (thread_.joinable()) thread_.join();
  if (wake_fd_ >= 0) close(wake_fd_);
}

void ObexManager::post(std::function<void(sd_bus*)> fn) {
  {
    std::lock_guard<std::mutex> lk(m_);
    commands_.push_back(std::move(fn));
  }
  eventfd_write(wake_fd_, 1);
}

void ObexManager::run() {
  while (running_.load()) {
    if (!bus_ && !open_bus()) {
      for (int i = 0; i < 50 && running_.load(); ++i) usleep(100000);
      continue;
    }
    std::deque<std::function<void(sd_bus*)>> cmds;
    {
      std::lock_guard<std::mutex> lk(m_);
      cmds.swap(commands_);
    }
    for (auto& c : cmds) c(bus_);
    for (;;) {
      const int r = sd_bus_process(bus_, nullptr);
      if (r < 0) {
        LOG_WARN("obex: lost the system bus: {}", strerror(-r));
        close_bus();
        break;
      }
      if (r == 0) break;
    }
    if (!bus_) continue;
    tick();
    pollfd pfd[2] = {{sd_bus_get_fd(bus_), static_cast<short>(sd_bus_get_events(bus_)), 0},
                     {wake_fd_, POLLIN, 0}};
    if (poll(pfd, 2, 200) > 0 && (pfd[1].revents & POLLIN)) {
      eventfd_t v;
      eventfd_read(wake_fd_, &v);
    }
  }
  close_bus();
}

bool ObexManager::open_bus() {
  if (sd_bus_open_system(&bus_) < 0) {
    bus_ = nullptr;
    return false;
  }
  sd_bus_add_object_vtable(bus_, &agent_slot_, kAgentPath, "org.bluez.obex.Agent1", Bus::agent_vtable(), this);
  auto match = [this](const char* sender, const char* path, const char* iface, const char* member) {
    sd_bus_slot* s = nullptr;
    if (sd_bus_match_signal(bus_, &s, sender, path, iface, member, Bus::signal, this) >= 0)
      match_slots_.push_back(s);
  };
  match(kObex, nullptr, nullptr, nullptr);
  match("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged");
  // Somebody may own it already (a desktop's obexd, the fake, one we started before a restart).
  sd_bus_message* reply = nullptr;
  if (sd_bus_call_method(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                         "GetNameOwner", nullptr, &reply, "s", kObex) >= 0) {
    const char* o = nullptr;
    if (sd_bus_message_read(reply, "s", &o) >= 0 && o) {
      std::lock_guard<std::mutex> lk(m_);
      owner_ = o;
    }
    sd_bus_message_unref(reply);
  }
  bool have = false;
  {
    std::lock_guard<std::mutex> lk(m_);
    have = !owner_.empty();
  }
  if (have) {
    refresh_objects(bus_);
    register_agent();
  }
  return true;
}

void ObexManager::close_bus() {
  for (sd_bus_slot* s : match_slots_) sd_bus_slot_unref(s);
  match_slots_.clear();
  if (agent_slot_) agent_slot_ = sd_bus_slot_unref(agent_slot_);
  if (bus_) bus_ = sd_bus_flush_close_unref(bus_);
  std::lock_guard<std::mutex> lk(m_);
  if (request_ && request_->msg) request_->msg = sd_bus_message_unref(request_->msg);
  request_.reset();
  agent_registered_ = false;
}

bool ObexManager::wait_for_owner(int timeout_ms) {
  std::unique_lock<std::mutex> lk(m_);
  return owner_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return !owner_.empty(); });
}

bool ObexManager::start(std::string* err) { return ensure_running(err); }

bool ObexManager::ensure_running(std::string* err) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!owner_.empty()) return true;
  }
  if (wait_for_owner(300)) return true;  // the bus thread may not have looked yet
  const std::string exe = find_exec(opt_.obexd);
  if (exe.empty()) {
    *err = "obexd is not installed (" + opt_.obexd + ")";
    return false;
  }
  // -n: stay in the foreground (btbenchd owns it); -s: the API on the system bus; -r: the root.
  // obexd writes nothing outside its root — not even where its own agent says to put a push — so
  // the root is the whole OBEX folder: pushes land in in/, and a device browsing the board over
  // FTP sees in/, out/ and shared/.
  Spawned p;
  std::string e;
  if (!spawn({exe, "-n", "-s", "-r", opt_.root}, &p, &e)) {
    *err = "cannot start obexd: " + e;
    return false;
  }
  // Its output is only logged; drained so it can never block on a full pipe.
  std::thread([p] {
    char buf[512];
    for (int fd : {p.out_fd, p.err_fd}) fcntl(fd, F_SETFL, O_NONBLOCK);
    pollfd pf[2] = {{p.out_fd, POLLIN, 0}, {p.err_fd, POLLIN, 0}};
    int open_fds = 2;
    while (open_fds > 0 && poll(pf, 2, -1) > 0) {
      for (pollfd& f : pf) {
        if (f.fd < 0 || !(f.revents & (POLLIN | POLLHUP | POLLERR))) continue;
        const ssize_t n = read(f.fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
          close(f.fd);
          f.fd = -1;
          --open_fds;
          continue;
        }
        buf[n] = 0;
        LOG_DEBUG("obexd: {}", trim(buf));
      }
    }
  }).detach();
  {
    std::lock_guard<std::mutex> lk(m_);
    pid_ = p.pid;
    managed_ = true;
  }
  LOG_INFO("obex: started obexd (pid {})", p.pid);
  if (wait_for_owner(5000)) return true;
  int st = 0;
  if (waitpid(p.pid, &st, WNOHANG) == p.pid) {
    std::lock_guard<std::mutex> lk(m_);
    pid_ = -1;
    managed_ = false;
    *err = "obexd exited at once (status " + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st)) +
           "): an obexd without --system-bus, or the bus policy refused it";
    return false;
  }
  *err = "obexd did not appear on the system bus within 5 s";
  return false;
}

void ObexManager::stop() {
  pid_t pid;
  {
    std::lock_guard<std::mutex> lk(m_);
    pid = managed_ ? pid_ : -1;
    pid_ = -1;
    managed_ = false;
  }
  if (pid > 0) {
    kill_group(pid, 1000);
    waitpid(pid, nullptr, 0);
    LOG_INFO("obex: stopped obexd");
  }
}

// ---- calls, objects, signals --------------------------------------------------------------------

CallResult ObexManager::call(const std::string& path, const std::string& iface, const std::string& method,
                             std::function<int(sd_bus_message*)> append, int timeout_ms) {
  auto done = std::make_shared<std::promise<CallResult>>();
  std::future<CallResult> fut = done->get_future();
  post([path, iface, method, append, timeout_ms, done](sd_bus* bus) {
    CallResult r;
    if (!bus) {
      r.error = "no D-Bus connection";
      done->set_value(std::move(r));
      return;
    }
    sd_bus_message* m = nullptr;
    int rc = sd_bus_message_new_method_call(bus, &m, kObex, path.c_str(), iface.c_str(), method.c_str());
    if (rc >= 0 && append) rc = append(m);
    auto* op = new CallOp{done};
    sd_bus_slot* slot = nullptr;
    if (rc >= 0) rc = sd_bus_call_async(bus, &slot, m, call_reply, op, static_cast<uint64_t>(timeout_ms) * 1000);
    sd_bus_message_unref(m);
    if (rc < 0) {
      delete op;
      r.error = std::string("cannot call ") + method + ": " + strerror(-rc);
      done->set_value(std::move(r));
      return;
    }
    sd_bus_slot_set_destroy_callback(slot, [](void* p) {
      auto* o = static_cast<CallOp*>(p);
      if (o->done) {
        CallResult x;
        x.error = "the D-Bus connection closed";
        o->done->set_value(std::move(x));
      }
      delete o;
    });
    sd_bus_slot_set_floating(slot, 1);
    sd_bus_slot_unref(slot);
  });
  if (fut.wait_for(std::chrono::milliseconds(timeout_ms + 2000)) != std::future_status::ready) {
    CallResult r;
    r.timed_out = true;
    r.error = "obexd did not answer in time";
    return r;
  }
  return fut.get();
}

void ObexManager::refresh_objects(sd_bus* bus) {
  sd_bus_message* reply = nullptr;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  if (sd_bus_call_method(bus, kObex, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &e,
                         &reply, "") < 0) {
    sd_bus_error_free(&e);
    return;
  }
  json objs;
  read_json(reply, &objs);
  sd_bus_message_unref(reply);
  std::lock_guard<std::mutex> lk(m_);
  sessions_.clear();
  std::map<std::string, json> t;
  if (objs.is_object()) {
    for (auto it = objs.begin(); it != objs.end(); ++it) {
      if (it->contains("org.bluez.obex.Session1")) sessions_[it.key()] = (*it)["org.bluez.obex.Session1"];
      if (it->contains("org.bluez.obex.Transfer1")) t[it.key()] = (*it)["org.bluez.obex.Transfer1"];
    }
  }
  // A transfer that finished is removed by obexd at once; keep the last state of the ones we saw
  // (bounded), so the console can show how it ended.
  for (auto& [p, props] : transfers_) {
    if (t.count(p)) continue;
    t[p] = props;
    // Gone before it finished: obexd dropped it (refused, cancelled, the link went).
    const std::string st = str(props, "Status");
    if (st != "complete" && st != "error") t[p]["Status"] = "error";
  }
  while (t.size() > 50) t.erase(t.begin());
  transfers_.swap(t);
  for (auto it = session_meta_.begin(); it != session_meta_.end();)
    it = sessions_.count(it->first) ? std::next(it) : session_meta_.erase(it);
  publish_pending_ = true;
}

void ObexManager::on_signal(sd_bus_message* m) {
  const char* member = sd_bus_message_get_member(m);
  const char* path = sd_bus_message_get_path(m);
  if (!member) return;
  if (strcmp(member, "NameOwnerChanged") == 0) {
    const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 || !name || strcmp(name, kObex) != 0) return;
    {
      std::lock_guard<std::mutex> lk(m_);
      owner_ = new_owner ? new_owner : "";
      agent_registered_ = false;
      if (owner_.empty()) {
        sessions_.clear();
        session_meta_.clear();
        if (request_ && request_->msg) request_->msg = sd_bus_message_unref(request_->msg);
        request_.reset();
      }
      publish_pending_ = true;
    }
    owner_cv_.notify_all();
    if (new_owner && *new_owner) {
      refresh_objects(bus_);
      register_agent();
    }
    return;
  }
  if (strcmp(member, "PropertiesChanged") == 0 && path) {
    const char* iface = nullptr;
    if (sd_bus_message_read(m, "s", &iface) < 0 || !iface) return;
    json changed;
    read_json(m, &changed);
    std::lock_guard<std::mutex> lk(m_);
    if (strcmp(iface, "org.bluez.obex.Transfer1") == 0 && changed.is_object()) {
      json& t = transfers_[path];
      if (!t.is_object()) t = json::object();
      t.update(changed);
    }
    publish_pending_ = true;
    return;
  }
  if (strcmp(member, "InterfacesAdded") == 0 || strcmp(member, "InterfacesRemoved") == 0) refresh_objects(bus_);
}

void ObexManager::register_agent() {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  const int r = sd_bus_call_method(bus_, kObex, kClientPath, "org.bluez.obex.AgentManager1", "RegisterAgent", &e,
                                   nullptr, "o", kAgentPath);
  std::lock_guard<std::mutex> lk(m_);
  agent_registered_ = r >= 0 || sd_bus_error_has_name(&e, "org.bluez.obex.Error.AlreadyExists");
  if (!agent_registered_) LOG_WARN("obex: cannot register the agent: {}", e.message ? e.message : strerror(-r));
  sd_bus_error_free(&e);
  publish_pending_ = true;
}

int ObexManager::agent_call(sd_bus_message* m, const char* member) {
  if (strcmp(member, "Cancel") == 0) {
    std::lock_guard<std::mutex> lk(m_);
    if (request_ && request_->msg) {
      sd_bus_reply_method_errorf(request_->msg, "org.bluez.obex.Error.Canceled", "Canceled");
      request_->msg = sd_bus_message_unref(request_->msg);
    }
    request_.reset();
    publish_pending_ = true;
    return sd_bus_reply_method_return(m, "");
  }
  const char* tpath = nullptr;
  if (sd_bus_message_read(m, "o", &tpath) < 0 || !tpath) return sd_bus_reply_method_errorf(m, "org.bluez.obex.Error.Rejected", "bad call");
  // What is being pushed: the transfer's properties (and its session's, for the sender).
  json props = json::object(), sprops = json::object();
  {
    sd_bus_message* reply = nullptr;
    if (sd_bus_call_method(bus_, kObex, tpath, "org.freedesktop.DBus.Properties", "GetAll", nullptr, &reply, "s",
                           "org.bluez.obex.Transfer1") >= 0) {
      read_json(reply, &props);
      sd_bus_message_unref(reply);
    }
    const std::string session = str(props, "Session");
    if (!session.empty() &&
        sd_bus_call_method(bus_, kObex, session.c_str(), "org.freedesktop.DBus.Properties", "GetAll", nullptr, &reply,
                           "s", "org.bluez.obex.Session1") >= 0) {
      read_json(reply, &sprops);
      sd_bus_message_unref(reply);
    }
  }
  const std::string name = safe_name(str(props, "Name", "received"));
  std::lock_guard<std::mutex> lk(m_);
  transfers_[tpath] = props;
  publish_pending_ = true;
  LOG_INFO("obex: {} pushes {} ({} bytes), policy {}", str(sprops, "Destination", "a device"), name,
           props.contains("Size") ? props["Size"].dump() : std::string("?"), policy_);
  if (policy_ == "reject") return sd_bus_reply_method_errorf(m, "org.bluez.obex.Error.Rejected", "Rejected by the bench");
  if (policy_ == "auto") {
    const std::string file = opt_.root + "/in/" + name;
    return sd_bus_reply_method_return(m, "s", file.c_str());
  }
  // ask: held until the console answers (or the deadline passes).
  if (request_ && request_->msg) {
    sd_bus_reply_method_errorf(request_->msg, "org.bluez.obex.Error.Rejected", "Superseded");
    sd_bus_message_unref(request_->msg);
  }
  request_ = std::make_unique<Request>();
  request_->id = next_request_id_++;
  request_->transfer = tpath;
  request_->name = name;
  request_->size = props.contains("Size") && props["Size"].is_number() ? props["Size"].get<long long>() : 0;
  request_->device = str(sprops, "Destination");
  request_->msg = sd_bus_message_ref(m);
  request_->deadline_ns = mono_ns() + kRequestNs;
  return 1;  // answered later
}

bool ObexManager::answer(uint64_t id, bool accept, const std::string& name, std::string* err) {
  if (!name.empty() && !file_name_ok(name)) {
    *err = "not a plain file name";
    return false;
  }
  auto done = std::make_shared<std::promise<std::string>>();
  auto fut = done->get_future();
  post([this, id, accept, name, done](sd_bus*) {
    std::lock_guard<std::mutex> lk(m_);
    if (!request_ || request_->id != id || !request_->msg) {
      done->set_value("no such request (answered, cancelled or expired)");
      return;
    }
    if (accept) {
      const std::string file = opt_.root + "/in/" + (name.empty() ? request_->name : name);
      sd_bus_reply_method_return(request_->msg, "s", file.c_str());
    } else {
      sd_bus_reply_method_errorf(request_->msg, "org.bluez.obex.Error.Rejected", "Rejected by the operator");
    }
    request_->msg = sd_bus_message_unref(request_->msg);
    request_.reset();
    publish_pending_ = true;
    done->set_value("");
  });
  *err = fut.get();
  return err->empty();
}

bool ObexManager::set_policy(const std::string& policy, std::string* err) {
  if (policy != "auto" && policy != "ask" && policy != "reject") {
    *err = "policy is auto, ask or reject";
    return false;
  }
  std::lock_guard<std::mutex> lk(m_);
  policy_ = policy;
  publish_pending_ = true;
  return true;
}

void ObexManager::tick() {
  std::unique_lock<std::mutex> lk(m_);
  if (request_ && request_->msg && mono_ns() > request_->deadline_ns) {
    sd_bus_reply_method_errorf(request_->msg, "org.bluez.obex.Error.Rejected", "Nobody answered");
    request_->msg = sd_bus_message_unref(request_->msg);
    request_.reset();
    publish_pending_ = true;
  }
  if (!publish_pending_ || mono_ns() - last_publish_ns_ < 250000000ull) return;
  publish_pending_ = false;
  last_publish_ns_ = mono_ns();
  lk.unlock();
  if (has_subscribers_ && has_subscribers_("obex")) publish_("obex", status());
}

// ---- status, sessions, operations ---------------------------------------------------------------

json ObexManager::files_json() const {
  return json{{"in", dir_listing(opt_.root + "/in")},
              {"out", dir_listing(opt_.root + "/out")},
              {"shared", dir_listing(opt_.root + "/shared")}};
}

json ObexManager::status() {
  std::lock_guard<std::mutex> lk(m_);
  json sessions = json::array(), transfers = json::array();
  for (const auto& [path, p] : sessions_) {
    json s{{"id", transfer_id(path)},
           {"path", path},
           {"destination", str(p, "Destination")},
           {"source", str(p, "Source")},
           {"target", target_name(str(p, "Target"))},
           {"channel", p.contains("Channel") ? p["Channel"] : json(nullptr)},
           {"psm", p.contains("PSM") ? p["PSM"] : json(nullptr)},
           {"root", str(p, "Root")}};
    if (session_meta_.count(path)) s.update(session_meta_.at(path));
    sessions.push_back(std::move(s));
  }
  for (auto it = transfers_.rbegin(); it != transfers_.rend(); ++it) transfers.push_back(transfer_json(it->first, it->second));
  json req = nullptr;
  if (request_ && request_->msg)
    req = json{{"id", request_->id}, {"name", request_->name}, {"size", request_->size}, {"device", request_->device},
               {"expires_s", static_cast<int>((request_->deadline_ns - std::min(request_->deadline_ns, mono_ns())) / 1000000000ull)}};
  return json{{"running", !owner_.empty()},
              {"managed", managed_},
              {"owner", owner_},
              {"error", error_},
              {"root", opt_.root},
              {"agent", json{{"policy", policy_}, {"registered", agent_registered_}}},
              {"request", req},
              {"sessions", sessions},
              {"transfers", transfers},
              {"files", files_json()}};
}

std::string ObexManager::session_path(const std::string& id) const {
  if (id.empty() || id.find("..") != std::string::npos) return {};
  for (const char c : id)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '/') return {};
  return "/org/bluez/obex/client/" + id;
}

bool ObexManager::create_session(const std::string& address, const std::string& target, const json& opts,
                                 json* out, std::string* err, int* status) {
  if (!target_ok(target)) {
    *status = 400;
    *err = "target is opp, ftp, pbap, map or bip-avrcp";
    return false;
  }
  if (!ensure_running(err)) {
    *status = 503;
    return false;
  }
  DDict d{{"Target", DVar::str(target)}};
  if (opts.contains("channel") && opts["channel"].is_number_unsigned())
    d.push_back({"Channel", DVar{'y', "", opts["channel"].get<int>(), {}, {}}});
  if (opts.contains("psm") && opts["psm"].is_number_unsigned()) d.push_back({"PSM", DVar::u16(opts["psm"].get<uint16_t>())});
  const CallResult r = call(kClientPath, "org.bluez.obex.Client1", "CreateSession", [address, d](sd_bus_message* m) {
    int rc = sd_bus_message_append(m, "s", address.c_str());
    if (rc >= 0) rc = append_dict(m, d);
    return rc;
  }, kCallMs);
  if (!r.ok) {
    *status = r.timed_out ? 504 : 502;
    *err = "CreateSession: " + r.error;
    return false;
  }
  const std::string path = r.reply.empty() ? "" : r.reply[0].get<std::string>();
  {
    std::lock_guard<std::mutex> lk(m_);
    session_meta_[path] = json{{"target", target}, {"created_ms", wall_ms()}};
  }
  post([this](sd_bus* bus) { if (bus) refresh_objects(bus); });
  *out = json{{"id", transfer_id(path)}, {"path", path}, {"target", target}, {"destination", address}};
  LOG_INFO("obex: {} session with {}: {}", target, address, path);
  return true;
}

bool ObexManager::remove_session(const std::string& id, std::string* err, int* status) {
  const std::string path = session_path(id);
  if (path.empty()) {
    *status = 404;
    *err = "no such session";
    return false;
  }
  const CallResult r = call(kClientPath, "org.bluez.obex.Client1", "RemoveSession",
                            [path](sd_bus_message* m) { return sd_bus_message_append(m, "o", path.c_str()); }, 5000);
  if (!r.ok) {
    *status = 404;
    *err = r.error;
    return false;
  }
  post([this](sd_bus* bus) { if (bus) refresh_objects(bus); });
  return true;
}

bool ObexManager::folder_ok(const std::string& f) const { return f == "in" || f == "out" || f == "shared"; }

std::string ObexManager::path_of(const std::string& folder, const std::string& name) const {
  if (!folder_ok(folder) || !file_name_ok(name)) return {};
  return opt_.root + "/" + folder + "/" + name;
}

bool ObexManager::op(const std::string& session_id, const json& body, json* out, std::string* err, int* status) {
  const std::string path = session_path(session_id);
  std::string target;
  {
    // What the session was opened for wins over what obexd reports for it.
    std::lock_guard<std::mutex> lk(m_);
    if (session_meta_.count(path)) target = str(session_meta_.at(path), "target");
    if (target.empty() && sessions_.count(path)) target = target_name(str(sessions_.at(path), "Target"));
  }
  if (path.empty() || target.empty()) {
    *status = 404;
    *err = "no such session";
    return false;
  }
  const std::string op = str(body, "op");
  auto arg = [&body](const char* k) { return str(body, k); };
  // A local file to write: in/<name>, the name given or one made from the operation and the time.
  auto in_file = [this, &body](const std::string& fallback) {
    std::string n = str(body, "file");
    if (n.empty() || !file_name_ok(n)) n = fallback;
    return opt_.root + "/in/" + n;
  };
  auto out_file = [this, &body, err, status](std::string* file) {
    const std::string n = str(body, "file");
    if (!file_name_ok(n)) {
      *status = 400;
      *err = "file is a name in out/ (upload it first: POST /api/obex/files/out)";
      return false;
    }
    *file = opt_.root + "/out/" + n;
    struct stat st{};
    if (stat(file->c_str(), &st) != 0) {
      *status = 404;
      *err = "no such file in out/: " + n;
      return false;
    }
    return true;
  };
  const std::string stamp = std::to_string(wall_ms());
  std::string iface, method;
  std::function<int(sd_bus_message*)> append;
  enum class Shape { None, Transfer, Folder, Vcards, Messages, Folders, Size, Images } shape = Shape::None;

  if (target == "opp") {
    iface = "org.bluez.obex.ObjectPush1";
    if (op == "send") {
      std::string f;
      if (!out_file(&f)) return false;
      method = "SendFile";
      append = [f](sd_bus_message* m) { return sd_bus_message_append(m, "s", f.c_str()); };
      shape = Shape::Transfer;
    } else if (op == "pull_card") {
      const std::string f = in_file("card-" + stamp + ".vcf");
      method = "PullBusinessCard";
      append = [f](sd_bus_message* m) { return sd_bus_message_append(m, "s", f.c_str()); };
      shape = Shape::Transfer;
    } else if (op == "exchange") {
      std::string f;
      if (!out_file(&f)) return false;
      const std::string t = opt_.root + "/in/card-" + stamp + ".vcf";
      method = "ExchangeBusinessCards";
      append = [f, t](sd_bus_message* m) { return sd_bus_message_append(m, "ss", f.c_str(), t.c_str()); };
      shape = Shape::Transfer;
    }
  } else if (target == "ftp") {
    iface = "org.bluez.obex.FileTransfer1";
    const std::string a = arg("name"), b = arg("to");
    if (op == "list") {
      method = "ListFolder";
      shape = Shape::Folder;
    } else if (op == "cd" || op == "mkdir" || op == "delete") {
      method = op == "cd" ? "ChangeFolder" : op == "mkdir" ? "CreateFolder" : "Delete";
      append = [a](sd_bus_message* m) { return sd_bus_message_append(m, "s", a.c_str()); };
    } else if (op == "get") {
      if (a.empty()) { *status = 400; *err = "name is the remote file"; return false; }
      const std::string f = in_file(safe_name(a));
      method = "GetFile";
      append = [f, a](sd_bus_message* m) { return sd_bus_message_append(m, "ss", f.c_str(), a.c_str()); };
      shape = Shape::Transfer;
    } else if (op == "put") {
      std::string f;
      if (!out_file(&f)) return false;
      const std::string remote = a.empty() ? str(body, "file") : a;
      method = "PutFile";
      append = [f, remote](sd_bus_message* m) { return sd_bus_message_append(m, "ss", f.c_str(), remote.c_str()); };
      shape = Shape::Transfer;
    } else if (op == "copy" || op == "move") {
      method = op == "copy" ? "CopyFile" : "MoveFile";
      append = [a, b](sd_bus_message* m) { return sd_bus_message_append(m, "ss", a.c_str(), b.c_str()); };
    }
  } else if (target == "pbap") {
    iface = "org.bluez.obex.PhonebookAccess1";
    DDict filters;
    if (body.contains("format")) filters.push_back({"Format", DVar::str(arg("format"))});
    if (body.contains("order")) filters.push_back({"Order", DVar::str(arg("order"))});
    if (body.contains("max") && body["max"].is_number_unsigned()) filters.push_back({"MaxCount", DVar::u16(body["max"].get<uint16_t>())});
    if (op == "select") {
      const std::string loc = arg("location").empty() ? "int" : arg("location");
      const std::string pb = arg("phonebook").empty() ? "pb" : arg("phonebook");
      method = "Select";
      append = [loc, pb](sd_bus_message* m) { return sd_bus_message_append(m, "ss", loc.c_str(), pb.c_str()); };
    } else if (op == "pull_all") {
      const std::string f = in_file("phonebook-" + stamp + ".vcf");
      method = "PullAll";
      append = [f, filters](sd_bus_message* m) {
        int rc = sd_bus_message_append(m, "s", f.c_str());
        return rc >= 0 ? append_dict(m, filters) : rc;
      };
      shape = Shape::Transfer;
    } else if (op == "list") {
      method = "List";
      append = [filters](sd_bus_message* m) { return append_dict(m, filters); };
      shape = Shape::Vcards;
    } else if (op == "pull") {
      const std::string v = arg("vcard");
      const std::string f = in_file("contact-" + safe_name(v));
      method = "Pull";
      append = [v, f, filters](sd_bus_message* m) {
        int rc = sd_bus_message_append(m, "ss", v.c_str(), f.c_str());
        return rc >= 0 ? append_dict(m, filters) : rc;
      };
      shape = Shape::Transfer;
    } else if (op == "search") {
      const std::string field = arg("field").empty() ? "name" : arg("field"), value = arg("value");
      method = "Search";
      append = [field, value, filters](sd_bus_message* m) {
        int rc = sd_bus_message_append(m, "ss", field.c_str(), value.c_str());
        return rc >= 0 ? append_dict(m, filters) : rc;
      };
      shape = Shape::Vcards;
    } else if (op == "size") {
      method = "GetSize";
      shape = Shape::Size;
    }
  } else if (target == "map") {
    iface = "org.bluez.obex.MessageAccess1";
    if (op == "folder") {
      const std::string f = arg("name");
      method = "SetFolder";
      append = [f](sd_bus_message* m) { return sd_bus_message_append(m, "s", f.c_str()); };
    } else if (op == "list_folders") {
      method = "ListFolders";
      append = [](sd_bus_message* m) { return append_dict(m, DDict{}); };
      shape = Shape::Folders;
    } else if (op == "list") {
      const std::string f = arg("name");
      method = "ListMessages";
      append = [f](sd_bus_message* m) {
        int rc = sd_bus_message_append(m, "s", f.c_str());
        return rc >= 0 ? append_dict(m, DDict{}) : rc;
      };
      shape = Shape::Messages;
    } else if (op == "get") {
      // Message1.Get on the message object a listing returned.
      const std::string msg = arg("message");
      if (!starts_with(msg, path + "/")) { *status = 400; *err = "message is a path from this session's listing"; return false; }
      const std::string f = in_file("message-" + safe_name(msg) + ".bmsg");
      const bool attach = body.value("attachment", false);
      const CallResult r = call(msg, "org.bluez.obex.Message1", "Get", [f, attach](sd_bus_message* m) {
        return sd_bus_message_append(m, "sb", f.c_str(), attach ? 1 : 0);
      }, kCallMs);
      if (!r.ok) { *status = r.timed_out ? 504 : 502; *err = r.error; return false; }
      *out = json{{"transfer", r.reply.empty() ? json(nullptr) : json(transfer_id(r.reply[0].get<std::string>()))},
                  {"file", f.substr(f.rfind('/') + 1)}};
      return true;
    }
  } else if (target == "bip-avrcp") {
    iface = "org.bluez.obex.Image1";
    const std::string h = arg("handle");
    if (op == "properties") {
      method = "Properties";
      append = [h](sd_bus_message* m) { return sd_bus_message_append(m, "s", h.c_str()); };
      shape = Shape::Images;
    } else if (op == "thumbnail" || op == "get") {
      const std::string f = in_file("image-" + safe_name(h) + (op == "thumbnail" ? "-thumb.jpg" : ".img"));
      if (op == "thumbnail") {
        method = "GetThumbnail";
        append = [f, h](sd_bus_message* m) { return sd_bus_message_append(m, "ss", f.c_str(), h.c_str()); };
      } else {
        method = "Get";
        append = [f, h](sd_bus_message* m) {
          int rc = sd_bus_message_append(m, "ss", f.c_str(), h.c_str());
          return rc >= 0 ? append_dict(m, DDict{}) : rc;
        };
      }
      shape = Shape::Transfer;
    }
  }
  if (method.empty()) {
    *status = 400;
    *err = "no operation '" + op + "' for a " + target + " session (docs/api.md lists them)";
    return false;
  }
  const CallResult r = call(path, iface, method, append, kCallMs);
  if (!r.ok) {
    *status = r.timed_out ? 504 : (r.error_name == "org.bluez.obex.Error.InvalidArguments" ? 400 : 502);
    *err = method + ": " + r.error;
    return false;
  }
  switch (shape) {
    case Shape::Transfer: {
      const std::string tpath = r.reply.size() >= 1 && r.reply[0].is_string() ? r.reply[0].get<std::string>() : "";
      const json props = r.reply.size() >= 2 ? r.reply[1] : json::object();
      {
        std::lock_guard<std::mutex> lk(m_);
        if (!tpath.empty()) transfers_[tpath] = props;
        publish_pending_ = true;
      }
      *out = transfer_json(tpath, props);
      break;
    }
    case Shape::Folder: *out = json{{"entries", folder_entries(r.reply)}}; break;
    case Shape::Vcards: *out = json{{"vcards", vcard_list(r.reply)}}; break;
    case Shape::Messages: *out = json{{"messages", message_list(r.reply)}}; break;
    case Shape::Folders: *out = json{{"folders", folder_entries(r.reply)}}; break;
    case Shape::Size: *out = json{{"size", r.reply.empty() ? json(nullptr) : r.reply[0]}}; break;
    case Shape::Images: *out = json{{"properties", image_properties(r.reply)}}; break;
    default: *out = json{{"ok", true}};
  }
  return true;
}

bool ObexManager::transfer_action(const std::string& id, const std::string& action, std::string* err, int* status) {
  if (action != "cancel" && action != "suspend" && action != "resume") {
    *status = 400;
    *err = "action is cancel, suspend or resume";
    return false;
  }
  std::string path;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& [p, props] : transfers_)
      if (transfer_id(p) == id) path = p;
  }
  if (path.empty()) {
    *status = 404;
    *err = "no such transfer";
    return false;
  }
  const char* method = action == "cancel" ? "Cancel" : action == "suspend" ? "Suspend" : "Resume";
  const CallResult r = call(path, "org.bluez.obex.Transfer1", method, nullptr, 5000);
  if (!r.ok) {
    *status = 409;
    *err = r.error;
    return false;
  }
  return true;
}

// ---- cover art ----------------------------------------------------------------------------------

bool ObexManager::cover_art(const std::string& address, uint16_t psm, const std::string& handle, bool thumbnail,
                            std::string* file, std::string* err, int* status) {
  if (handle.empty() || handle.size() > 16 || handle.find_first_not_of("0123456789") != std::string::npos) {
    *status = 400;
    *err = "an image handle is seven digits";
    return false;
  }
  const std::string local = opt_.root + "/in/cover/" + address + "-" + handle + (thumbnail ? "-thumb" : "") + ".jpg";
  struct stat st{};
  if (stat(local.c_str(), &st) == 0 && st.st_size > 0) {
    *file = local;
    return true;
  }
  // One bip-avrcp session per device, kept while it works: a session per image would cost an
  // OBEX connect each time the track changes.
  std::lock_guard<std::mutex> guard(cover_m_);
  std::string sid;
  auto it = cover_sessions_.find(address);
  if (it != cover_sessions_.end()) {
    std::lock_guard<std::mutex> lk(m_);
    if (sessions_.count("/org/bluez/obex/client/" + it->second)) sid = it->second;
  }
  if (sid.empty()) {
    json s;
    if (!create_session(address, "bip-avrcp", json{{"psm", psm}}, &s, err, status)) return false;
    sid = s["id"].get<std::string>();
    cover_sessions_[address] = sid;
  }
  const std::string name = local.substr(local.rfind('/') + 1);
  json t;
  if (!op(sid, json{{"op", thumbnail ? "thumbnail" : "get"}, {"handle", handle}, {"file", "cover/" + name}}, &t, err, status)) {
    cover_sessions_.erase(address);
    return false;
  }
  // in_file() refused "cover/x" as a name (a separator) and used its fallback; find the file the
  // transfer writes from its Filename instead, and wait for the transfer to end.
  const std::string tid = t.value("id", std::string{});
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::string written = t.value("filename", std::string{});
  while (std::chrono::steady_clock::now() < until) {
    std::string st_s;
    {
      std::lock_guard<std::mutex> lk(m_);
      for (const auto& [p, props] : transfers_) {
        if (transfer_id(p) != tid) continue;
        st_s = str(props, "Status");
        if (!str(props, "Filename").empty()) written = str(props, "Filename");
      }
    }
    if (st_s == "complete") break;
    if (st_s == "error") {
      *status = 502;
      *err = "the image transfer failed";
      return false;
    }
    usleep(100000);
  }
  if (written.empty() || stat(written.c_str(), &st) != 0) {
    *status = 504;
    *err = "the image did not arrive";
    return false;
  }
  rename(written.c_str(), local.c_str());
  *file = local;
  return true;
}

}  // namespace btb::obex
