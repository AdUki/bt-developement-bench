#include "obex/cover_art.h"

#include <endian.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "util/exec.h"
#include "util/log.h"

namespace btb {

namespace {

// The kernel's Bluetooth socket ABI (include/net/bluetooth), spelled out rather than taken from
// libbluetooth's headers, which the build does not otherwise need.
constexpr int kAfBluetooth = 31;
constexpr int kBtprotoL2cap = 0;
constexpr int kSolL2cap = 6;
constexpr int kL2capOptions = 0x01;
constexpr uint8_t kL2capModeErtm = 0x03;
struct SockaddrL2 {
  sa_family_t family;
  uint16_t psm;  // little endian
  uint8_t bdaddr[6];
  uint16_t cid;
  uint8_t bdaddr_type;
};
struct L2capOptions {
  uint16_t omtu;
  uint16_t imtu;
  uint16_t flush_to;
  uint8_t mode;
  uint8_t fcs;
  uint8_t max_tx;
  uint16_t txwin_size;
};

// OBEX over L2CAP: one packet per SDU, as large as both sides take.
constexpr uint16_t kMtu = 32767;

// BIP's Cover Art target (AVRCP 1.6, 5.14.2.1): 7163DD54-4A7E-11E2-B47C-0050C2490048.
const std::string kCoverArtTarget("\x71\x63\xDD\x54\x4A\x7E\x11\xE2\xB4\x7C\x00\x50\xC2\x49\x00\x48", 16);

// OBEX opcodes, response codes and header ids (IrOBEX 1.5, BIP 1.2).
constexpr uint8_t kConnect = 0x80, kDisconnect = 0x81, kGet = 0x03, kFinal = 0x80, kAbort = 0xFF;
constexpr uint8_t kContinue = 0x90, kOk = 0xA0, kBadRequest = 0xC0, kForbidden = 0xC3, kNotFound = 0xC4,
                  kNotImplemented = 0xD1, kUnavailable = 0xD3;
constexpr uint8_t kHdrType = 0x42, kHdrTarget = 0x46, kHdrWho = 0x4A, kHdrConnId = 0xCB, kHdrBody = 0x48,
                  kHdrEndOfBody = 0x49, kHdrLength = 0xC3, kHdrImgHandle = 0x30;

void put16(std::string* s, uint16_t v) {
  s->push_back(static_cast<char>(v >> 8));
  s->push_back(static_cast<char>(v & 0xff));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

std::string utf16be_to_utf8(const uint8_t* p, size_t n) {
  std::string out;
  for (size_t i = 0; i + 1 < n; i += 2) {
    const uint16_t c = get16(p + i);
    if (c == 0) break;
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xC0 | c >> 6));
      out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xE0 | c >> 12));
      out.push_back(static_cast<char>(0x80 | (c >> 6 & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
  }
  return out;
}

// ASCII and the Basic Multilingual Plane: what an image handle (digits) and a type need.
std::string utf8_to_utf16be(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    uint16_t c = b;
    if (b >= 0xE0 && i + 2 < s.size()) {
      c = static_cast<uint16_t>((b & 0x0f) << 12 | (s[i + 1] & 0x3f) << 6 | (s[i + 2] & 0x3f));
      i += 3;
    } else if (b >= 0xC0 && i + 1 < s.size()) {
      c = static_cast<uint16_t>((b & 0x1f) << 6 | (s[i + 1] & 0x3f));
      i += 2;
    } else {
      i++;
    }
    put16(&out, c);
  }
  put16(&out, 0);
  return out;
}

bool read_file(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return !out->empty();
}

bool exists(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

// Receives one OBEX packet: SDUs until its length is there. False at the end or on an error.
bool recv_packet(int fd, std::string* pkt) {
  pkt->clear();
  char buf[kMtu + 16];
  for (;;) {
    const ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return false;
    pkt->append(buf, static_cast<size_t>(n));
    if (pkt->size() >= 3) {
      const uint16_t len = get16(reinterpret_cast<const uint8_t*>(pkt->data()) + 1);
      if (len < 3) return false;
      if (pkt->size() >= len) {
        pkt->resize(len);
        return true;
      }
    }
  }
}

bool send_all(int fd, const std::string& pkt) {
  return send(fd, pkt.data(), pkt.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(pkt.size());
}

}  // namespace

namespace obex {

bool parse_headers(const uint8_t* p, size_t n, std::vector<Header>* out) {
  out->clear();
  size_t i = 0;
  while (i < n) {
    Header h;
    h.id = p[i];
    switch (h.id >> 6) {
      case 0:    // Unicode text, length-prefixed
      case 1: {  // bytes, length-prefixed
        if (i + 3 > n) return false;
        const uint16_t len = get16(p + i + 1);
        if (len < 3 || i + len > n) return false;
        h.bytes = h.id >> 6 == 0 ? utf16be_to_utf8(p + i + 3, len - 3u)
                                 : std::string(reinterpret_cast<const char*>(p + i + 3), len - 3u);
        i += len;
        break;
      }
      case 2:  // one byte
        if (i + 2 > n) return false;
        h.num = p[i + 1];
        i += 2;
        break;
      default:  // four bytes
        if (i + 5 > n) return false;
        h.num = static_cast<uint32_t>(p[i + 1]) << 24 | static_cast<uint32_t>(p[i + 2]) << 16 |
                static_cast<uint32_t>(p[i + 3]) << 8 | p[i + 4];
        i += 5;
        break;
    }
    out->push_back(std::move(h));
  }
  return true;
}

std::string packet(uint8_t opcode, const std::string& fixed, const std::vector<Header>& headers) {
  std::string body = fixed;
  for (const Header& h : headers) {
    body.push_back(static_cast<char>(h.id));
    switch (h.id >> 6) {
      case 0:
      case 1: {
        const std::string v = h.id >> 6 == 0 ? utf8_to_utf16be(h.bytes) : h.bytes;
        put16(&body, static_cast<uint16_t>(v.size() + 3));
        body += v;
        break;
      }
      case 2:
        body.push_back(static_cast<char>(h.num));
        break;
      default:
        for (int s = 24; s >= 0; s -= 8) body.push_back(static_cast<char>(h.num >> s & 0xff));
        break;
    }
  }
  std::string out(1, static_cast<char>(opcode));
  put16(&out, static_cast<uint16_t>(body.size() + 3));
  return out + body;
}

}  // namespace obex

bool jpeg_size(const std::string& d, int* w, int* h) {
  const auto* p = reinterpret_cast<const uint8_t*>(d.data());
  const size_t n = d.size();
  if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 9 < n) {
    if (p[i] != 0xFF) return false;
    const uint8_t marker = p[i + 1];
    if (marker == 0xFF) {  // fill
      i++;
      continue;
    }
    const uint16_t len = get16(p + i + 2);
    // Start of frame: baseline, progressive and the rest, but DHT (C4), JPG (C8) and DAC (CC).
    if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
      *h = get16(p + i + 5);
      *w = get16(p + i + 7);
      return *w > 0 && *h > 0;
    }
    i += 2 + len;
  }
  return false;
}

std::string image_properties_xml(const std::string& handle, int w, int h, size_t size) {
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "<image-properties version=\"1.0\" handle=\"%s\">\r\n"
                "<native encoding=\"JPEG\" pixel=\"%d*%d\" size=\"%zu\"/>\r\n"
                "<variant encoding=\"JPEG\" pixel=\"200*200\"/>\r\n"
                "</image-properties>\r\n",
                handle.c_str(), w, h, size);
  return buf;
}

CoverArtServer::CoverArtServer(std::string dir) : dir_(std::move(dir)) {}

CoverArtServer::~CoverArtServer() { stop(); }

bool CoverArtServer::start(std::string* err) {
  mkdir(dir_.c_str(), 0755);
  const int fd = socket(kAfBluetooth, SOCK_SEQPACKET | SOCK_CLOEXEC, kBtprotoL2cap);
  if (fd < 0) {
    *err = std::string("L2CAP socket: ") + strerror(errno);
    return false;
  }
  // ERTM, as GOEP 2.0 has OBEX over L2CAP (obexd's client asks for nothing else).
  L2capOptions opts{};
  socklen_t olen = sizeof opts;
  if (getsockopt(fd, kSolL2cap, kL2capOptions, &opts, &olen) == 0) {
    opts.mode = kL2capModeErtm;
    opts.imtu = kMtu;
    opts.omtu = kMtu;
    if (setsockopt(fd, kSolL2cap, kL2capOptions, &opts, sizeof opts) < 0) {
      *err = std::string("L2CAP ERTM: ") + strerror(errno);
      close(fd);
      return false;
    }
  }
  SockaddrL2 a{};
  a.family = kAfBluetooth;
  a.psm = 0;  // the kernel picks a free dynamic PSM
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || listen(fd, 4) < 0) {
    *err = std::string("L2CAP listen: ") + strerror(errno);
    close(fd);
    return false;
  }
  socklen_t alen = sizeof a;
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&a), &alen) < 0 || !a.psm) {
    *err = "no PSM assigned";
    close(fd);
    return false;
  }
  psm_ = le16toh(a.psm);
  listen_fd_ = fd;
  stopping_ = false;
  accept_ = std::thread([this] { accept_loop(); });
  LOG_INFO("cover art: BIP responder on PSM 0x{:04x}", psm_);
  return true;
}

void CoverArtServer::stop() {
  if (listen_fd_ < 0) return;
  stopping_ = true;
  shutdown(listen_fd_, SHUT_RDWR);
  if (accept_.joinable()) accept_.join();
  close(listen_fd_);
  listen_fd_ = -1;
  std::vector<std::thread> ts;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (int c : conns_) shutdown(c, SHUT_RDWR);
    ts.swap(threads_);
  }
  for (std::thread& t : ts)
    if (t.joinable()) t.join();
}

std::string CoverArtServer::handle_for(const std::string& url) {
  if (url.empty()) return "";
  std::lock_guard<std::mutex> lk(m_);
  const auto it = handles_.find(url);
  if (it != handles_.end()) return it->second;
  char h[16];
  std::snprintf(h, sizeof h, "%07u", next_++ % 10000000u);
  handles_[url] = h;
  urls_[h] = url;
  return h;
}

void CoverArtServer::accept_loop() {
  while (!stopping_) {
    pollfd p{listen_fd_, POLLIN, 0};
    if (poll(&p, 1, 500) <= 0) continue;
    SockaddrL2 peer{};
    socklen_t plen = sizeof peer;
    const int fd = accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen, SOCK_CLOEXEC);
    if (fd < 0) continue;
    char addr[18];
    const uint8_t* b = peer.bdaddr;
    std::snprintf(addr, sizeof addr, "%02X:%02X:%02X:%02X:%02X:%02X", b[5], b[4], b[3], b[2], b[1], b[0]);
    std::lock_guard<std::mutex> lk(m_);
    conns_.insert(fd);
    threads_.emplace_back([this, fd, a = std::string(addr)] { serve(fd, a); });
  }
}

void CoverArtServer::serve(int fd, std::string peer) {
  // What one packet may be: the client's maximum (CONNECT), and what the link carries.
  L2capOptions opts{};
  socklen_t olen = sizeof opts;
  uint16_t link = kMtu;
  if (getsockopt(fd, kSolL2cap, kL2capOptions, &opts, &olen) == 0 && opts.omtu) link = opts.omtu;
  uint16_t max = 255;  // OBEX's minimum, until CONNECT says
  bool connected = false;
  const uint32_t conn_id = 1;
  // A GET: its headers (they may come over several packets), then the answer in pieces.
  std::string type, handle, data;
  size_t sent = 0;
  bool answering = false;
  auto reset = [&] {
    type.clear();
    handle.clear();
    data.clear();
    sent = 0;
    answering = false;
  };
  std::string pkt;
  while (!stopping_ && recv_packet(fd, &pkt)) {
    const auto* p = reinterpret_cast<const uint8_t*>(pkt.data());
    const uint8_t op = p[0];
    std::vector<obex::Header> hs;
    std::string reply;
    if (op == kConnect) {
      if (pkt.size() < 7 || !obex::parse_headers(p + 7, pkt.size() - 7, &hs)) {
        reply = obex::packet(kBadRequest, "", {});
      } else {
        const auto t = std::find_if(hs.begin(), hs.end(), [](const obex::Header& h) { return h.id == kHdrTarget; });
        if (t == hs.end() || t->bytes != kCoverArtTarget) {
          reply = obex::packet(kUnavailable, "", {});
        } else {
          max = std::min<uint16_t>({get16(p + 5), link, kMtu});
          std::string fixed("\x10\x00", 2);  // OBEX 1.0, no flags
          put16(&fixed, max);
          reply = obex::packet(kOk, fixed, {{kHdrConnId, conn_id, ""}, {kHdrWho, 0, kCoverArtTarget}});
          if (!connected) {
            connected = true;
            sessions_++;
            LOG_INFO("cover art: {} connected (packets up to {} bytes)", peer, max);
            if (on_change_) on_change_();
          }
        }
      }
    } else if (op == kDisconnect) {
      send_all(fd, obex::packet(kOk, "", {}));
      break;
    } else if (op == kAbort) {
      reset();
      reply = obex::packet(kOk, "", {});
    } else if ((op & 0x7F) == kGet) {
      if (!connected) {
        reply = obex::packet(kForbidden, "", {});
      } else if (!obex::parse_headers(p + 3, pkt.size() - 3, &hs)) {
        reset();
        reply = obex::packet(kBadRequest, "", {});
      } else {
        for (const obex::Header& h : hs) {
          if (h.id == kHdrType) type = std::string(h.bytes.c_str());  // up to its NUL
          if (h.id == kHdrImgHandle) handle = h.bytes;
        }
        if (!(op & kFinal) && !answering) {
          reply = obex::packet(kContinue, "", {});  // more headers to come
        } else {
          if (!answering) {
            std::string err;
            bool ok = false;
            if (type == "x-bt/img-img" || type == "x-bt/img-thm") {
              ok = image(handle, type == "x-bt/img-thm", &data, &err);
            } else if (type == "x-bt/img-properties") {
              std::string img;
              int w = 0, h = 0;
              ok = image(handle, false, &img, &err);
              if (ok && !jpeg_size(img, &w, &h)) w = h = 0;
              if (ok) data = image_properties_xml(handle, w, h, img.size());
            } else {
              err = "type " + type + " not served";
            }
            if (!ok) {
              LOG_INFO("cover art: {} asked for {} {}: {}", peer, type, handle, err);
              reply = obex::packet(type.rfind("x-bt/img-", 0) == 0 ? kNotFound : kNotImplemented, "", {});
              reset();
            } else {
              answering = true;
              LOG_INFO("cover art: {} gets {} {} ({} bytes)", peer, type, handle, data.size());
            }
          }
          if (answering) {
            // A piece of the body per GET: the packet's room less its own and the body header's
            // three bytes (and the first one's Length).
            const size_t room = max - 3u - 3u - (sent == 0 ? 5u : 0u);
            const size_t n = std::min(room, data.size() - sent);
            std::vector<obex::Header> out;
            if (sent == 0) out.push_back({kHdrLength, static_cast<uint32_t>(data.size()), ""});
            const bool last = sent + n >= data.size();
            out.push_back({last ? kHdrEndOfBody : kHdrBody, 0, data.substr(sent, n)});
            sent += n;
            reply = obex::packet(last ? kOk : kContinue, "", out);
            if (last) reset();
          }
        }
      }
    } else {
      reply = obex::packet(kNotImplemented, "", {});  // PUT, SETPATH: a responder only answers
    }
    if (!reply.empty() && !send_all(fd, reply)) break;
  }
  close(fd);
  {
    std::lock_guard<std::mutex> lk(m_);
    conns_.erase(fd);
  }
  if (connected) {
    sessions_--;
    LOG_INFO("cover art: {} disconnected", peer);
    if (on_change_ && !stopping_) on_change_();
  }
}

bool CoverArtServer::image(const std::string& handle, bool thumbnail, std::string* data, std::string* err) {
  std::string url;
  {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = urls_.find(handle);
    if (it == urls_.end()) {
      *err = "no such handle";
      return false;
    }
    url = it->second;
  }
  std::lock_guard<std::mutex> lk(fetch_m_);
  const std::string native = dir_ + "/" + handle + ".jpg", thumb = dir_ + "/" + handle + "-thm.jpg";
  if (!exists(native)) {
    const std::string got = dir_ + "/" + handle + ".got";
    const ExecResult r = run_cmd({"curl", "-fsSL", "-m", "10", "-o", got, url}, 15000);
    if (!r.ok() || !exists(got)) {
      *err = "fetching " + url + ": " + r.reason();
      unlink(got.c_str());
      return false;
    }
    // BIP's images are JPEG: another format is turned into one, when ffmpeg is there.
    std::string img;
    int w = 0, h = 0;
    if (read_file(got, &img) && jpeg_size(img, &w, &h)) {
      rename(got.c_str(), native.c_str());
    } else {
      const ExecResult c = run_cmd({"ffmpeg", "-y", "-loglevel", "error", "-i", got, "-q:v", "3", native}, 15000);
      unlink(got.c_str());
      if (!c.ok() || !exists(native)) {
        *err = "not a JPEG, and no ffmpeg to make one";
        return false;
      }
    }
  }
  if (thumbnail && !exists(thumb)) {
    // 200×200, the image scaled into it whole (a square cover fills it).
    run_cmd({"ffmpeg", "-y", "-loglevel", "error", "-i", native, "-vf",
             "scale=200:200:force_original_aspect_ratio=decrease,pad=200:200:(ow-iw)/2:(oh-ih)/2", "-q:v", "4", thumb},
            15000);
  }
  // Without ffmpeg, the thumbnail is the image itself (a controller scales it).
  if (thumbnail && read_file(thumb, data)) return true;
  if (read_file(native, data)) return true;
  *err = "the image file went away";
  return false;
}

}  // namespace btb
