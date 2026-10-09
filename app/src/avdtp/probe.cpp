#include "avdtp/probe.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>

#include "a2dp_codec.h"
#include "util/strings.h"

using json = nlohmann::json;

namespace btb::avdtp {

namespace {

// <bluetooth/l2cap.h> without the header: libbluetooth's headers are on the board's SDK but not
// necessarily on a developer PC, and these few definitions are all this needs.
constexpr int kAfBluetooth = 31;
constexpr int kBtProtoL2cap = 0;
constexpr int kSolBluetooth = 274;
constexpr int kBtSecurity = 4;
constexpr uint16_t kPsmAvdtp = 0x0019;

struct SockaddrL2 {
  sa_family_t l2_family;
  uint16_t l2_psm;
  uint8_t l2_bdaddr[6];
  uint16_t l2_cid;
  uint8_t l2_bdaddr_type;
};
struct BtSecurity {
  uint8_t level;
  uint8_t key_size;
};

std::string hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s.push_back(d[p[i] >> 4]);
    s.push_back(d[p[i] & 15]);
  }
  return s;
}

bool parse_addr(const std::string& a, uint8_t out[6]) {
  unsigned v[6];
  if (std::sscanf(a.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(v[5 - i]);  // little-endian on the air
  return true;
}

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

const char* category_name(uint8_t c) {
  switch (c) {
    case 1: return "media transport";
    case 2: return "reporting";
    case 3: return "recovery";
    case 4: return "content protection";
    case 5: return "header compression";
    case 6: return "multiplexing";
    case 7: return "media codec";
    case 8: return "delay reporting";
    default: return "unknown";
  }
}

}  // namespace

std::vector<uint8_t> command(uint8_t label, uint8_t signal, int acp_seid) {
  // Byte 0: label (4 bits) | packet type single (00) | message type command (00).
  std::vector<uint8_t> c{static_cast<uint8_t>((label & 0x0f) << 4), static_cast<uint8_t>(signal & 0x3f)};
  if (acp_seid >= 0) c.push_back(static_cast<uint8_t>((acp_seid & 0x3f) << 2));
  return c;
}

Reply parse_reply(const std::vector<uint8_t>& pkt, uint8_t label, uint8_t signal) {
  Reply r;
  if (pkt.size() < 2) return r;
  const uint8_t l = pkt[0] >> 4, type = pkt[0] & 0x03, sig = pkt[1] & 0x3f;
  if (l != (label & 0x0f)) return r;
  if (type == 0x01) {  // general reject: the signal is not known at all
    r.rejected = true;
    return r;
  }
  if (sig != signal) return r;
  if (type == 0x02) {
    r.ok = true;
    r.payload.assign(pkt.begin() + 2, pkt.end());
  } else if (type == 0x03) {
    r.rejected = true;
    // DISCOVER's and GET_CAPABILITIES' rejects carry just the error code.
    r.error_code = pkt.size() > 2 ? pkt.back() : 0;
  }
  return r;
}

const char* error_name(uint8_t code) {
  switch (code) {
    case 0x01: return "BAD_HEADER_FORMAT";
    case 0x11: return "BAD_LENGTH";
    case 0x12: return "BAD_ACP_SEID";
    case 0x13: return "SEP_IN_USE";
    case 0x14: return "SEP_NOT_IN_USE";
    case 0x17: return "BAD_SERV_CATEGORY";
    case 0x18: return "BAD_PAYLOAD_FORMAT";
    case 0x19: return "NOT_SUPPORTED_COMMAND";
    case 0x1a: return "INVALID_CAPABILITIES";
    case 0x31: return "BAD_STATE";
    default: return "error";
  }
}

json parse_discover(const std::vector<uint8_t>& p) {
  json out = json::array();
  for (size_t i = 0; i + 1 < p.size(); i += 2) {
    const uint8_t mt = p[i + 1] >> 4;
    out.push_back(json{{"seid", p[i] >> 2},
                       {"in_use", (p[i] & 0x02) != 0},
                       {"media_type", mt == 0 ? json("audio") : mt == 1 ? json("video") : mt == 2 ? json("multimedia") : json(mt)},
                       {"role", (p[i + 1] & 0x08) ? "sink" : "source"}});
  }
  return out;
}

json parse_capabilities(const std::vector<uint8_t>& p) {
  json caps = json::array();
  for (size_t i = 0; i + 1 < p.size();) {
    const uint8_t cat = p[i], len = p[i + 1];
    if (i + 2 + len > p.size()) {
      caps.push_back(json{{"category", cat}, {"name", category_name(cat)}, {"error", "truncated"}});
      break;
    }
    const uint8_t* v = p.data() + i + 2;
    json c{{"category", cat}, {"name", category_name(cat)}, {"hex", hex(v, len)}};
    if (cat == 4 && len >= 2) {
      const unsigned type = v[0] | (v[1] << 8);
      c["type"] = type == 0x0002 ? "DTCP" : type == 0x0003 ? "SCMS-T" : "0x" + hex(v, 2);
    } else if (cat == 7 && len >= 2) {
      // Media type (high nibble), codec type, then the codec information element.
      c["media_type"] = v[0] >> 4;
      c["codec_type"] = v[1];
      c["codec"] = decode_codec(v[1], std::vector<uint8_t>(v + 2, v + len), true);
    }
    caps.push_back(std::move(c));
    i += 2 + len;
  }
  return caps;
}

namespace {

// One command and its reply on a connected signalling socket, skipping anything else the device
// sends meanwhile (its own commands, as it starts to configure us).
bool exchange(int fd, uint8_t label, uint8_t signal, int seid, int64_t deadline, Reply* r, std::string* err) {
  const std::vector<uint8_t> c = command(label, signal, seid);
  if (send(fd, c.data(), c.size(), 0) < 0) {
    *err = std::string("send: ") + strerror(errno);
    return false;
  }
  uint8_t buf[1024];
  for (;;) {
    const int64_t left = deadline - now_ms();
    if (left <= 0) {
      *err = "no answer in time";
      return false;
    }
    pollfd pf{fd, POLLIN, 0};
    if (poll(&pf, 1, static_cast<int>(left)) <= 0) continue;
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      *err = n == 0 ? "the device closed the channel" : std::string("recv: ") + strerror(errno);
      return false;
    }
    *r = parse_reply(std::vector<uint8_t>(buf, buf + n), label, signal);
    if (r->ok || r->rejected) return true;
    // A command from the device: answer it with a general reject so it does not wait on us.
    if ((buf[0] & 0x03) == 0x00 && n >= 2) {
      const uint8_t rej[2] = {static_cast<uint8_t>((buf[0] & 0xf0) | 0x01), buf[1]};
      send(fd, rej, sizeof(rej), 0);
    }
  }
}

}  // namespace

bool probe(const std::string& address, int timeout_ms, json* out, std::string* err) {
  SockaddrL2 sa{};
  sa.l2_family = kAfBluetooth;
  sa.l2_psm = kPsmAvdtp;  // little-endian host
  if (!parse_addr(address, sa.l2_bdaddr)) {
    *err = "address is AA:BB:CC:DD:EE:FF";
    return false;
  }
  const int64_t t0 = now_ms(), deadline = t0 + timeout_ms;
  const int fd = socket(kAfBluetooth, SOCK_SEQPACKET | SOCK_CLOEXEC, kBtProtoL2cap);
  if (fd < 0) {
    *err = std::string("no L2CAP sockets: ") + strerror(errno);
    return false;
  }
  // Low security: discovery and capabilities need no encryption, and asking for it would make
  // an unpaired device fail where it could answer.
  BtSecurity sec{1, 0};
  setsockopt(fd, kSolBluetooth, kBtSecurity, &sec, sizeof(sec));
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
    const int e = errno;
    close(fd);
    *err = std::string("cannot open an AVDTP channel: ") + strerror(e) +
           (e == ECONNREFUSED ? " (the device has one open already — A2DP is connected? — or no AVDTP)" :
            e == EHOSTDOWN || e == ETIMEDOUT ? " (the device does not answer pages)" : "");
    return false;
  }
  uint8_t label = 0;
  Reply r;
  if (!exchange(fd, label++, kDiscover, -1, deadline, &r, err)) {
    close(fd);
    return false;
  }
  if (!r.ok) {
    close(fd);
    *err = std::string("DISCOVER rejected: ") + error_name(r.error_code);
    return false;
  }
  json seps = parse_discover(r.payload);
  for (json& s : seps) {
    const int seid = s["seid"].get<int>();
    std::string e;
    bool got = exchange(fd, label++ & 0x0f, kGetAllCapabilities, seid, deadline, &r, &e);
    // An AVDTP 1.0 device does not know GET_ALL_CAPABILITIES: ask the 1.0 way.
    if (got && r.rejected && (r.error_code == 0 || r.error_code == 0x19))
      got = exchange(fd, label++ & 0x0f, kGetCapabilities, seid, deadline, &r, &e);
    if (!got) {
      s["error"] = e;
      continue;
    }
    if (!r.ok) {
      s["error"] = error_name(r.error_code);
      continue;
    }
    s["capabilities"] = parse_capabilities(r.payload);
    s["codec"] = nullptr;
    for (const json& c : s["capabilities"])
      if (c.contains("codec")) s["codec"] = c["codec"];
  }
  close(fd);
  *out = json{{"address", address}, {"seps", seps}, {"took_ms", now_ms() - t0}};
  return true;
}

}  // namespace btb::avdtp
