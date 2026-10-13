#include "watchdog.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>

#include <spdlog/spdlog.h>

#include "wire.h"

namespace btb::hci {

namespace {

constexpr uint16_t kHciChannelRaw = 0;
constexpr int kSolHci = 0;
constexpr int kHciFilter = 2;
constexpr uint8_t kHciCommandPkt = 0x01, kHciEventPkt = 0x04;
constexpr uint16_t kCmdReadLocalVersion = 0x1001;

struct HciFilter {
  uint32_t type_mask;
  uint32_t event_mask[2];
  uint16_t opcode;
};

int64_t mono_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string base_name(const std::string& p) {
  const size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

std::string real_path(const std::string& p) {
  char buf[PATH_MAX];
  return realpath(p.c_str(), buf) ? std::string(buf) : std::string{};
}

bool write_sysfs(const std::string& path, const std::string& value, std::string* err) {
  std::ofstream f(path);
  if (f) f << value;
  if (f) f.flush();
  if (!f) {
    *err = path + ": " + std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace

bool probe_controller(uint16_t index, int timeout_ms, std::string* err) {
  const int fd = socket(kAfBluetooth, SOCK_RAW | SOCK_CLOEXEC, kBtProtoHci);
  if (fd < 0) {
    *err = std::string("HCI socket: ") + std::strerror(errno);
    return false;
  }
  HciFilter f{};
  f.type_mask = 1u << kHciEventPkt;
  f.event_mask[0] = (1u << kEvtCmdComplete) | (1u << 0x0f);  // Command Complete, Command Status
  SockaddrHci a{static_cast<unsigned short>(kAfBluetooth), index, kHciChannelRaw};
  if (setsockopt(fd, kSolHci, kHciFilter, &f, sizeof(f)) < 0 ||
      bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
    *err = std::string("HCI socket: ") + std::strerror(errno);
    close(fd);
    return false;
  }
  const uint8_t cmd[] = {kHciCommandPkt, kCmdReadLocalVersion & 0xff, kCmdReadLocalVersion >> 8, 0};
  if (write(fd, cmd, sizeof(cmd)) != static_cast<ssize_t>(sizeof(cmd))) {
    *err = std::string("HCI command: ") + std::strerror(errno);
    close(fd);
    return false;
  }
  const int64_t until = mono_ms() + timeout_ms;
  for (int64_t left; (left = until - mono_ms()) > 0;) {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, static_cast<int>(left)) <= 0) continue;
    uint8_t buf[260];
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) break;
    // Command Complete: type, event, len, ncmd, opcode. Command Status: type, event, len,
    // status, ncmd, opcode. Either for this command answers it.
    const bool cc = n >= 6 && buf[1] == kEvtCmdComplete && (buf[4] | buf[5] << 8) == kCmdReadLocalVersion;
    const bool cs = n >= 7 && buf[1] == 0x0f && (buf[5] | buf[6] << 8) == kCmdReadLocalVersion;
    if (cc || cs) {
      close(fd);
      return true;
    }
  }
  close(fd);
  *err = "no answer in " + std::to_string(timeout_ms) + " ms";
  return false;
}

bool rebind_controller(uint16_t index, std::string* what, std::string* err) {
  const std::string hci = "/sys/class/bluetooth/hci" + std::to_string(index);
  const std::string dev = real_path(hci + "/device");
  const std::string drv = dev.empty() ? std::string{} : real_path(dev + "/driver");
  if (dev.empty() || drv.empty()) {
    *err = "hci" + std::to_string(index) + " has no device with a driver in sysfs";
    return false;
  }
  const std::string name = base_name(dev);
  *what = base_name(drv) + " " + name;
  if (!write_sysfs(drv + "/unbind", name, err)) return false;
  // The driver's remove powers the chip down; a moment off makes sure it starts from reset.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  return write_sysfs(drv + "/bind", name, err);
}

void ControllerWatchdog::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void ControllerWatchdog::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void ControllerWatchdog::run() {
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    for (const uint16_t index : silent_(kSilentMs)) {
      if (!running_.load()) return;
      const int64_t now = mono_ms();
      const auto at = reset_at_.find(index);
      if (at != reset_at_.end() && now - at->second < kResetGapMs) continue;
      if (resets_[index] >= kMaxResets) continue;
      std::string err;
      if (probe_controller(index, kProbeMs, &err)) continue;  // it answers: a peer is slow, not it
      const std::string hci = "hci" + std::to_string(index);
      if (err.rfind("no answer", 0) != 0) {
        spdlog::warn("controller watchdog: {} could not be asked: {}", hci, err);
        reset_at_[index] = now;  // not again at once
        continue;
      }
      std::string what;
      const std::string why = hci + " stopped answering (nothing for " + std::to_string(kSilentMs / 1000) +
                              " s, and " + err + " to Read Local Version)";
      spdlog::warn("controller watchdog: {}: resetting it", why);
      // Said before the reset: the mark goes out through the controller's index while it is there.
      if (note_) note_(why + ": resetting it");
      reset_at_[index] = now;
      if (++resets_[index] == kMaxResets)
        spdlog::warn("controller watchdog: {} reset {} times: not again until btbenchd restarts", hci, kMaxResets);
      if (rebind_controller(index, &what, &err))
        spdlog::warn("controller watchdog: {} reset ({} rebound)", hci, what);
      else
        spdlog::error("controller watchdog: {} could not be reset: {}", hci, err);
    }
  }
}

}  // namespace btb::hci
