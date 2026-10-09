#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "btsnoop.h"
#include "capture.h"
#include "decoder.h"
#include "hci_monitor.h"
#include "monitor_impl.h"
#include "pcap.h"
#include "wire.h"

namespace btb::hci {

using nlohmann::json;

namespace {

int64_t realtime_us() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

int64_t mono_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// struct hci_dev_info (lib/bluetooth/hci.h), for HCIGETDEVINFO. Natural alignment, as the
// kernel's copy is declared.
struct HciDevInfo {
  uint16_t dev_id;
  char name[8];
  uint8_t bdaddr[6];
  uint32_t flags;
  uint8_t type;
  uint8_t features[8];
  uint32_t pkt_type;
  uint32_t link_policy;
  uint32_t link_mode;
  uint16_t acl_mtu;
  uint16_t acl_pkts;
  uint16_t sco_mtu;
  uint16_t sco_pkts;
  uint32_t stat[10];
};
constexpr unsigned long kHciGetDevInfo = _IOR('H', 211, int);

int open_hci_channel(uint16_t channel, std::string* err) {
  const int fd = socket(kAfBluetooth, SOCK_RAW | SOCK_CLOEXEC, kBtProtoHci);
  if (fd < 0) {
    *err = std::string("Bluetooth socket: ") + std::strerror(errno);
    return -1;
  }
  SockaddrHci addr{};
  addr.hci_family = kAfBluetooth;
  addr.hci_dev = kHciDevNone;
  addr.hci_channel = channel;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    const int e = errno;
    *err = std::string(channel == kHciChannelMonitor ? "HCI monitor channel: "
                                                     : "HCI logging channel: ") +
           std::strerror(e);
    if (e == EPERM || e == EACCES) *err += " (needs CAP_NET_RAW/CAP_NET_ADMIN: run as root)";
    close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

Monitor::Impl::Impl(const Options& o, Publish p)
    : opts(o),
      publish(std::move(p)),
      feed(std::make_shared<PcapFeed>()),
      capture(std::make_shared<Capture>(o.capture_dir, o.capture_unit, o.systemctl, o.btmon)),
      packets(std::make_shared<PacketStore>(o.capture_dir)) {}

Monitor::Impl::~Impl() { stop(); }

void Monitor::Impl::stop() {
  {
    std::lock_guard<std::mutex> lk(stop_mu);
    if (stopping) return;
    stopping = true;
  }
  stop_cv.notify_all();
  if (wake_fd[1] >= 0) {
    const char c = 1;
    (void)!write(wake_fd[1], &c, 1);
  }
  if (thread.joinable()) thread.join();
  feed->close_all();
  for (int* fd : {&mon_fd, &wake_fd[0], &wake_fd[1]}) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
  }
  std::lock_guard<std::mutex> lk(log_mu);
  if (log_fd >= 0) close(log_fd);
  log_fd = -1;
}

bool Monitor::Impl::start_source() {
  if (!opts.replay_file.empty()) {
    replay = true;
    std::string err;
    if (!reader.open(opts.replay_file, &err)) {
      error = err;
      return false;
    }
    spdlog::info("HCI monitor: replaying {} (datalink {}){}", opts.replay_file,
                 reader.datalink(), opts.replay_fast ? ", fast" : "");
    thread = std::thread([this] { run_replay(); });
    return true;
  }

  std::string err;
  mon_fd = open_hci_channel(kHciChannelMonitor, &err);
  if (mon_fd < 0) {
    error = err;
    return false;
  }
  // Kernel receive timestamps: latency must not include our own scheduling delay (one ARM11
  // core shared with bluetoothd and PipeWire). SO_RXQ_OVFL reports what the kernel dropped
  // when this socket's buffer overflowed.
  const int one = 1;
  if (setsockopt(mon_fd, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof(one)) < 0) {
    spdlog::warn("HCI monitor: SO_TIMESTAMP: {}", std::strerror(errno));
  }
  setsockopt(mon_fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one));
  const int rcvbuf = 1024 * 1024;
  if (setsockopt(mon_fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) < 0) {
    setsockopt(mon_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }
  if (pipe2(wake_fd, O_CLOEXEC | O_NONBLOCK) < 0) {
    error = std::string("pipe: ") + std::strerror(errno);
    close(mon_fd);
    mon_fd = -1;
    return false;
  }
  spdlog::info("HCI monitor: reading the kernel's HCI monitor channel");
  thread = std::thread([this] { run_live(); });
  return true;
}

// Publishing happens outside the stats lock: the callback serializes and fans out to WebSocket
// clients, and the HTTP handlers must not wait behind that.
void Monitor::Impl::publish_pending(bool stats_closed) {
  std::vector<Event> evs;
  json stats_copy;
  {
    std::lock_guard<std::mutex> lk(mu);
    evs = dec.take_new_events();
    if (stats_closed) stats_copy = stats_locked();
  }
  if (!publish) return;
  for (const auto& e : evs) {
    json j{{"seq", e.seq},
           {"ts", e.ts_ms},
           {"index", e.index},
           {"handle", e.handle < 0 ? json(nullptr) : json(e.handle)},
           {"kind", e.kind},
           {"text", e.text}};
    if (!e.data.is_null()) j["data"] = e.data;
    publish("hci.event", j);
  }
  if (stats_closed) publish("hci.stats", stats_copy);
}

json Monitor::Impl::stats_locked() const {
  json s = dec.stats();
  s["source"] = json{{"kind", replay ? "replay" : "live"},
                     {"available", error.empty()},
                     {"packets", dec.packets()},
                     {"kernel_drops", kernel_drops}};
  if (replay) {
    s["source"]["file"] = opts.replay_file;
    s["source"]["done"] = replay_done;
  }
  return s;
}

void Monitor::Impl::query_devinfo(uint16_t index) {
  // The controller was initialised before we opened the socket, so its Read Buffer Size went by
  // unseen. The kernel keeps the BR/EDR numbers; HCIGETDEVINFO needs no privilege. (LE and ISO
  // buffer counts have no such ioctl: they stay unknown until the controller is re-initialised.)
  const int fd = socket(kAfBluetooth, SOCK_RAW | SOCK_CLOEXEC, kBtProtoHci);
  if (fd < 0) return;
  HciDevInfo di{};
  di.dev_id = index;
  if (ioctl(fd, kHciGetDevInfo, &di) == 0 && di.acl_pkts) {
    std::lock_guard<std::mutex> lk(mu);
    dec.set_acl_buffers(index, di.acl_mtu, di.acl_pkts, di.sco_mtu, di.sco_pkts);
  }
  close(fd);
}

void Monitor::Impl::run_live() {
  std::vector<uint8_t> buf(kMonHdrSize + 65536);
  alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(timeval)) + CMSG_SPACE(sizeof(uint32_t)) + 64];
  std::vector<uint16_t> new_indexes;

  for (;;) {
    const int64_t now = realtime_us();
    // Wake just after the next whole second, so windows close on time with no traffic.
    const int timeout_ms = static_cast<int>((kWindowUs - now % kWindowUs) / 1000) + 2;
    pollfd p[2] = {{mon_fd, POLLIN, 0}, {wake_fd[0], POLLIN, 0}};
    const int rc = poll(p, 2, timeout_ms);
    if (rc < 0 && errno != EINTR) {
      spdlog::error("HCI monitor: poll: {}", std::strerror(errno));
      break;
    }
    if (rc > 0 && (p[1].revents & POLLIN)) break;
    if (rc > 0 && (p[0].revents & (POLLERR | POLLHUP))) {
      spdlog::error("HCI monitor: socket error, reader stops");
      break;
    }

    bool closed = false;
    if (rc > 0 && (p[0].revents & POLLIN)) {
      std::lock_guard<std::mutex> lk(mu);
      // Drain what is queued, bounded so the HTTP handlers get the lock now and then.
      for (int i = 0; i < 64; ++i) {
        iovec iov{buf.data(), buf.size()};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);
        const ssize_t n = recvmsg(mon_fd, &msg, MSG_DONTWAIT);
        if (n < 0) break;
        if (n < static_cast<ssize_t>(kMonHdrSize)) continue;

        int64_t ts = 0;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
          if (c->cmsg_level != SOL_SOCKET) continue;
          if (c->cmsg_type == SCM_TIMESTAMP) {
            timeval tv{};
            std::memcpy(&tv, CMSG_DATA(c), sizeof(tv));
            ts = static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
          } else if (c->cmsg_type == SO_RXQ_OVFL) {
            uint32_t drops = 0;
            std::memcpy(&drops, CMSG_DATA(c), sizeof(drops));
            kernel_drops = drops;
          }
        }
        if (!ts) ts = realtime_us();

        const uint16_t opcode = le16(buf.data());
        const uint16_t index = le16(buf.data() + 2);
        const size_t len = std::min<size_t>(le16(buf.data() + 4), n - kMonHdrSize);
        const uint8_t* data = buf.data() + kMonHdrSize;
        dec.packet(ts, index, opcode, data, len);
        if (feed->active()) feed->push(ts, index, opcode, data, len);
        if (opcode == kMonNewIndex || opcode == kMonOpenIndex) new_indexes.push_back(index);
      }
      closed = dec.advance(realtime_us()) > 0;
    } else {
      std::lock_guard<std::mutex> lk(mu);
      closed = dec.advance(realtime_us()) > 0;
    }

    for (uint16_t idx : new_indexes) query_devinfo(idx);
    new_indexes.clear();
    publish_pending(closed);
  }
}

void Monitor::Impl::run_replay() {
  SnoopPacket pkt;
  bool first = true;
  int64_t ts0 = 0, mono0 = 0, last_ts = 0;

  auto stopped = [this] {
    std::lock_guard<std::mutex> lk(stop_mu);
    return stopping;
  };

  while (reader.read(&pkt)) {
    if (first) {
      ts0 = pkt.ts_us;
      mono0 = mono_us();
      first = false;
    }
    last_ts = pkt.ts_us;

    if (!opts.replay_fast) {
      // A long silence is replayed as 10 s, not waited out: vendor captures from boards without
      // an RTC jump by days when the clock gets set.
      constexpr int64_t kMaxGapUs = 10 * kWindowUs;
      const int64_t ahead = pkt.ts_us - (ts0 + (mono_us() - mono0));
      if (ahead > kMaxGapUs) {
        spdlog::info("HCI monitor: replay skips {} s of silence", (ahead - kMaxGapUs) / 1000000);
        ts0 += ahead - kMaxGapUs;
      }
      // Real-time pacing: sleep until the packet is due, closing windows on the way so a quiet
      // stretch of the capture shows up as quiet seconds.
      for (;;) {
        const int64_t virt = ts0 + (mono_us() - mono0);
        if (virt >= pkt.ts_us) break;
        const int64_t next_tick = virt - virt % kWindowUs + kWindowUs;
        const int64_t wait = std::min(pkt.ts_us, next_tick) - virt;
        {
          std::unique_lock<std::mutex> lk(stop_mu);
          if (stop_cv.wait_for(lk, std::chrono::microseconds(wait), [this] { return stopping; })) {
            return;
          }
        }
        bool closed;
        {
          std::lock_guard<std::mutex> lk(mu);
          closed = dec.advance(ts0 + (mono_us() - mono0)) > 0;
        }
        if (closed) publish_pending(true);
      }
    } else if (stopped()) {
      return;
    }

    int closed;
    {
      std::lock_guard<std::mutex> lk(mu);
      const uint64_t before = dec.windows_closed();
      dec.packet(pkt.ts_us, pkt.index, pkt.opcode, pkt.data, pkt.len);
      closed = static_cast<int>(dec.windows_closed() - before);
      if (feed->active()) feed->push(pkt.ts_us, pkt.index, pkt.opcode, pkt.data, pkt.len);
    }
    // In fast mode thousands of windows go by per second: publish events, but stats only paced.
    if (!opts.replay_fast || closed) publish_pending(closed > 0 && !opts.replay_fast);
  }

  {
    std::lock_guard<std::mutex> lk(mu);
    dec.flush(last_ts + 1);
    replay_done = true;
  }
  publish_pending(true);
  done_cv.notify_all();
  spdlog::info("HCI monitor: replay of {} finished", opts.replay_file);
}

json Monitor::Impl::do_mark(const std::string& text_in) {
  std::string text = text_in.substr(0, 256);
  for (char& ch : text) {
    if (ch == '\0' || ch == '\n' || ch == '\r') ch = ' ';
  }
  const int64_t ts = realtime_us();

  bool logged = false;
  std::string err;
  if (replay) {
    err = "replay mode: no kernel logging channel";
  } else {
    std::lock_guard<std::mutex> lk(log_mu);
    if (log_fd < 0) log_fd = open_hci_channel(kHciChannelLogging, &err);
    if (log_fd >= 0) {
      // bluez src/shared/log.c bt_log_sendmsg(): { opcode 0x0000, index, len } (le16), priority,
      // ident length, NUL-terminated ident, NUL-terminated message.
      static const char kIdent[] = "btbench";
      std::vector<uint8_t> f(6 + 2 + sizeof(kIdent) + text.size() + 1);
      put_le16(f.data(), 0x0000);
      put_le16(f.data() + 2, kHciDevNone);
      put_le16(f.data() + 4, static_cast<uint16_t>(f.size() - 6));
      f[6] = 6;  // LOG_INFO
      f[7] = sizeof(kIdent);
      std::memcpy(f.data() + 8, kIdent, sizeof(kIdent));
      std::memcpy(f.data() + 8 + sizeof(kIdent), text.c_str(), text.size() + 1);
      if (send(log_fd, f.data(), f.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(f.size())) {
        logged = true;
      } else {
        err = std::string("HCI logging channel: ") + std::strerror(errno);
        close(log_fd);
        log_fd = -1;
      }
    }
  }

  uint64_t seq;
  {
    std::lock_guard<std::mutex> lk(mu);
    dec.add_mark(ts, text);
    seq = dec.last_seq();
  }
  publish_pending(false);
  json out{{"ok", true}, {"seq", seq}, {"logged", logged}};
  if (!logged) out["error"] = err;
  return out;
}

// ---------------------------------------------------------------------------------------------

Monitor::Monitor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Monitor::~Monitor() { stop(); }

bool Monitor::available() const { return impl_->error.empty(); }
std::string Monitor::error() const { return impl_->error; }

json Monitor::stats() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->stats_locked();
}

bool Monitor::history(int index, int handle, int seconds, json* out) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->dec.history(index, handle, seconds, out);
}

bool Monitor::latency(int index, int handle, json* out) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->dec.latency(index, handle, out);
}

json Monitor::events(uint64_t since) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->dec.events(since);
}

json Monitor::mark(const std::string& text) { return impl_->do_mark(text); }

bool Monitor::wait_replay_done(int timeout_ms) const {
  std::unique_lock<std::mutex> lk(impl_->mu);
  return impl_->done_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                 [this] { return impl_->replay_done; });
}

void Monitor::stop() { impl_->stop(); }

std::unique_ptr<Monitor> start(const Options& opts, Publish publish) {
  auto impl = std::make_unique<Monitor::Impl>(opts, std::move(publish));
  if (!impl->start_source()) {
    spdlog::warn("HCI monitor unavailable: {}", impl->error);
  }
  return std::make_unique<Monitor>(std::move(impl));
}

}  // namespace btb::hci
