#include "mgmt.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

using json = nlohmann::json;

namespace btb {

namespace {

// include/net/bluetooth/hci_sock.h, mgmt.h
constexpr int kAfBluetooth = 31;
constexpr int kBtProtoHci = 1;
constexpr uint16_t kHciDevNone = 0xffff;
constexpr uint16_t kHciChannelControl = 3;
constexpr uint16_t kIndexNone = 0xffff;
constexpr uint16_t kOpReadIndexList = 0x0003;
constexpr uint16_t kOpReadInfo = 0x0004;
constexpr uint16_t kOpReadExpFeatures = 0x0049;
constexpr uint16_t kEvCmdComplete = 0x0001;
constexpr uint16_t kEvCmdStatus = 0x0002;

struct SockaddrHci {
  unsigned short hci_family;
  unsigned short hci_dev;
  unsigned short hci_channel;
};

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

class Mgmt {
 public:
  explicit Mgmt(int timeout_ms) : timeout_ms_(timeout_ms) {}
  ~Mgmt() {
    if (fd_ >= 0) close(fd_);
  }
  bool open(std::string* err) {
    fd_ = socket(kAfBluetooth, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, kBtProtoHci);
    if (fd_ < 0) {
      *err = std::string("Bluetooth socket: ") + std::strerror(errno);
      return false;
    }
    SockaddrHci a{};
    a.hci_family = kAfBluetooth;
    a.hci_dev = kHciDevNone;
    a.hci_channel = kHciChannelControl;
    if (bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
      *err = std::string("mgmt socket: ") + std::strerror(errno);
      return false;
    }
    return true;
  }
  // One command and its Command Complete; events the kernel sends meanwhile (an adapter added,
  // a setting changed) are skipped.
  bool call(uint16_t op, uint16_t index, std::vector<uint8_t>* reply, std::string* err) {
    const uint8_t cmd[6] = {static_cast<uint8_t>(op), static_cast<uint8_t>(op >> 8), static_cast<uint8_t>(index),
                            static_cast<uint8_t>(index >> 8), 0, 0};
    if (send(fd_, cmd, sizeof cmd, 0) != static_cast<ssize_t>(sizeof cmd)) {
      *err = std::string("mgmt send: ") + std::strerror(errno);
      return false;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
    uint8_t buf[2048];
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
      if (left <= 0) {
        *err = "mgmt: no answer";
        return false;
      }
      pollfd p{fd_, POLLIN, 0};
      if (poll(&p, 1, static_cast<int>(left)) <= 0) continue;
      const ssize_t n = recv(fd_, buf, sizeof buf, 0);
      if (n < 9) continue;
      const uint16_t ev = le16(buf), idx = le16(buf + 2), len = le16(buf + 4);
      if ((ev != kEvCmdComplete && ev != kEvCmdStatus) || idx != index || le16(buf + 6) != op) continue;
      const uint8_t status = buf[8];
      if (status != 0) {
        char m[48];
        std::snprintf(m, sizeof m, "mgmt status 0x%02x", status);
        *err = m;
        return false;
      }
      const size_t end = std::min<size_t>(static_cast<size_t>(n), 6u + len);
      reply->assign(buf + 9, buf + std::max<size_t>(end, 9));
      return true;
    }
  }

 private:
  int fd_ = -1;
  const int timeout_ms_;
};

json exp_features(const std::vector<uint8_t>& r) {
  json out = json::array();
  if (r.size() < 2) return out;
  const size_t n = le16(r.data());
  for (size_t i = 0; i < n && 2 + (i + 1) * 20 <= r.size(); ++i) {
    const uint8_t* e = r.data() + 2 + i * 20;
    out.push_back(json{{"uuid", mgmt_uuid_str(e)}, {"flags", le32(e + 16)}});
  }
  return out;
}

}  // namespace

std::string mgmt_uuid_str(const uint8_t* le) {
  char s[37];
  int o = 0;
  for (int i = 15; i >= 0; --i) {
    o += std::snprintf(s + o, sizeof s - o, "%02x", le[i]);
    if (i == 12 || i == 10 || i == 8 || i == 6) s[o++] = '-';
  }
  return std::string(s, 36);
}

std::vector<std::string> mgmt_settings_names(uint32_t bits) {
  // btmgmt's names (tools/btmgmt.c settings_str), in bit order.
  static const char* const names[] = {"powered", "connectable", "fast-connectable", "discoverable", "bondable",
                                      "link-security", "ssp", "br/edr", "hs", "le", "advertising", "secure-conn",
                                      "debug-keys", "privacy", "configuration", "static-addr", "phy-configuration",
                                      "wide-band-speech", "cis-central", "cis-peripheral", "iso-broadcaster",
                                      "sync-receiver", "ll-privacy", "past-sender", "past-receiver", "sci"};
  std::vector<std::string> out;
  for (unsigned i = 0; i < 32; ++i)
    if (bits & (1u << i)) out.push_back(i < sizeof names / sizeof *names ? names[i] : "bit" + std::to_string(i));
  return out;
}

bool mgmt_read_controllers(json* out, std::string* err, int timeout_ms) {
  Mgmt m(timeout_ms);
  if (!m.open(err)) return false;
  std::vector<uint8_t> r;
  if (!m.call(kOpReadIndexList, kIndexNone, &r, err)) return false;
  json controllers = json::array();
  const size_t n = r.size() >= 2 ? le16(r.data()) : 0;
  for (size_t i = 0; i < n && 2 + (i + 1) * 2 <= r.size(); ++i) {
    const uint16_t index = le16(r.data() + 2 + i * 2);
    json c{{"index", index}};
    std::vector<uint8_t> info, exp;
    std::string e;
    if (m.call(kOpReadInfo, index, &info, &e) && info.size() >= 20) {
      char a[18];
      std::snprintf(a, sizeof a, "%02X:%02X:%02X:%02X:%02X:%02X", info[5], info[4], info[3], info[2], info[1], info[0]);
      c["address"] = a;
      c["version"] = info[6];
      c["manufacturer"] = le16(info.data() + 7);
      c["supported"] = mgmt_settings_names(le32(info.data() + 9));
      c["current"] = mgmt_settings_names(le32(info.data() + 13));
    }
    c["experimental"] = m.call(kOpReadExpFeatures, index, &exp, &e) ? exp_features(exp) : json::array();
    controllers.push_back(std::move(c));
  }
  std::vector<uint8_t> global;
  std::string e;
  *out = json{{"controllers", controllers},
              {"experimental", m.call(kOpReadExpFeatures, kIndexNone, &global, &e) ? exp_features(global) : json::array()}};
  return true;
}

}  // namespace btb
