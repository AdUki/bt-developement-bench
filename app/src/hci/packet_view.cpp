// The packet view of the capture ring: index, filter, list, detail and graph (packet_view.h).

#include "packet_view.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>

#include "btsnoop.h"
#include "capture.h"
#include "histogram.h"
#include "wire.h"

namespace btb::hci {

using nlohmann::json;

namespace {

int64_t mono_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr size_t kRecHdr = 24;
constexpr uint32_t kMaxRecord = 65536 + 16;  // what BtsnoopReader accepts; beyond is corruption
constexpr size_t kReadChunk = 256 * 1024;
// A file far beyond the ring's 16 MiB (someone copied a long capture into /data/btsnoop) is
// indexed up to here and reported as truncated: 32 MB of index is all the Zero W can spare.
constexpr size_t kMaxEntries = 1000000;
constexpr size_t kMaxQueries = 4;  // cached filter results per file
constexpr size_t kTxFifo = 128;    // as the live decoder: TX timestamps awaiting NOCP per link
constexpr size_t kMaxChans = 32;

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

const char* mon_name(uint8_t op) {
  switch (op) {
    case kMonNewIndex: return "New Index";
    case kMonDelIndex: return "Delete Index";
    case kMonCommand: return "HCI Command";
    case kMonEvent: return "HCI Event";
    case kMonAclTx: return "ACL TX";
    case kMonAclRx: return "ACL RX";
    case kMonScoTx: return "SCO TX";
    case kMonScoRx: return "SCO RX";
    case kMonOpenIndex: return "Open Index";
    case kMonCloseIndex: return "Close Index";
    case kMonIndexInfo: return "Index Info";
    case kMonVendorDiag: return "Vendor Diagnostic";
    case kMonSystemNote: return "System Note";
    case kMonUserLogging: return "User Logging";
    case kMonCtrlOpen: return "Control Open";
    case kMonCtrlClose: return "Control Close";
    case kMonCtrlCommand: return "Control Command";
    case kMonCtrlEvent: return "Control Event";
    case kMonIsoTx: return "ISO TX";
    case kMonIsoRx: return "ISO RX";
    default: return "?";
  }
}

uint8_t proto_for_psm(uint16_t psm) {
  switch (psm) {
    case kPsmSdp: return kProtoSdp;
    case kPsmRfcomm: return kProtoRfcomm;
    case kPsmBnep: return kProtoBnep;
    case kPsmHidCtrl:
    case kPsmHidIntr: return kProtoHid;
    case kPsmAvctp:
    case kPsmAvctpBrowsing: return kProtoAvctp;
    case kPsmAvdtp: return kProtoAvdtp;
    case kPsmAtt:
    case kPsmEatt: return kProtoAtt;
    default: return kProtoL2cap;
  }
}

uint8_t proto_for_fixed_cid(uint16_t cid) {
  switch (cid) {
    case kCidAtt: return kProtoAtt;
    case kCidSmp:
    case kCidSmpBredr: return kProtoSmp;
    default: return kProtoL2cap;
  }
}

// Events whose parameters start with status and handle, and those that start with the handle.
bool evt_status_handle(uint8_t code) {
  switch (code) {
    case 0x03: case 0x05: case 0x06: case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c:
    case 0x0d: case 0x14: case 0x1c: case 0x1d: case 0x21: case 0x23: case 0x2c: case 0x2d:
    case 0x2e: case 0x30: case 0x59:
      return true;
    default:
      return false;
  }
}
bool evt_handle_first(uint8_t code) {
  switch (code) {
    case 0x11: case 0x1b: case 0x1e: case 0x38: case 0x39: case 0x57:
      return true;
    default:
      return false;
  }
}
bool le_status_handle(uint8_t sub) {
  switch (sub) {
    case 0x01: case 0x03: case 0x04: case 0x0a: case 0x0c: case 0x19: case 0x29: case 0x2a:
      return true;
    default:
      return false;
  }
}
bool le_handle_first(uint8_t sub) {
  switch (sub) {
    case 0x05: case 0x06: case 0x07: case 0x14: case 0x1a:
      return true;
    default:
      return false;
  }
}

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Names

const char* pkt_type_name(uint8_t t) {
  switch (t) {
    case kTypeCmd: return "cmd";
    case kTypeEvt: return "evt";
    case kTypeAcl: return "acl";
    case kTypeSco: return "sco";
    case kTypeIso: return "iso";
    case kTypeIndex: return "index";
    case kTypeLog: return "log";
    case kTypeMgmt: return "mgmt";
    case kTypeDiag: return "diag";
    default: return "other";
  }
}

const char* proto_name(uint8_t p) {
  switch (p) {
    case kProtoHci: return "hci";
    case kProtoL2cap: return "l2cap";
    case kProtoAtt: return "att";
    case kProtoSmp: return "smp";
    case kProtoSdp: return "sdp";
    case kProtoRfcomm: return "rfcomm";
    case kProtoAvdtp: return "avdtp";
    case kProtoRtp: return "rtp";
    case kProtoAvctp: return "avctp";
    case kProtoBnep: return "bnep";
    case kProtoHid: return "hid";
    case kProtoSco: return "sco";
    case kProtoIso: return "iso";
    default: return nullptr;
  }
}

uint8_t pkt_type_of(uint16_t op) {
  switch (op) {
    case kMonCommand: return kTypeCmd;
    case kMonEvent: return kTypeEvt;
    case kMonAclTx:
    case kMonAclRx: return kTypeAcl;
    case kMonScoTx:
    case kMonScoRx: return kTypeSco;
    case kMonIsoTx:
    case kMonIsoRx: return kTypeIso;
    case kMonNewIndex:
    case kMonDelIndex:
    case kMonOpenIndex:
    case kMonCloseIndex:
    case kMonIndexInfo: return kTypeIndex;
    case kMonSystemNote:
    case kMonUserLogging: return kTypeLog;
    case kMonCtrlOpen:
    case kMonCtrlClose:
    case kMonCtrlCommand:
    case kMonCtrlEvent: return kTypeMgmt;
    case kMonVendorDiag: return kTypeDiag;
    default: return kTypeOther;
  }
}

// ---------------------------------------------------------------------------------------------
// Filter

namespace {

bool parse_num(const std::string& s, double* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    const unsigned long v = std::strtoul(s.c_str(), &end, 16);
    if (*end) return false;
    *out = static_cast<double>(v);
    return true;
  }
  const double v = std::strtod(s.c_str(), &end);
  if (*end || std::isnan(v)) return false;
  *out = v;
  return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    const size_t b = s.find(sep, a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

// Whitespace-separated, with "double quotes" around a phrase (the quotes are dropped).
std::vector<std::string> tokenize(const std::string& src) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false, any = false;
  for (char c : src) {
    if (c == '"') {
      quoted = !quoted;
      any = true;
      continue;
    }
    if (!quoted && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
      if (any) out.push_back(cur);
      cur.clear();
      any = false;
      continue;
    }
    cur.push_back(c);
    any = true;
  }
  if (any) out.push_back(cur);
  return out;
}

}  // namespace

bool Filter::parse(const std::string& src, Filter* out, std::string* err) {
  *out = Filter{};
  std::vector<std::string> norm;
  for (std::string tok : tokenize(src)) {
    Term t;
    std::string shown = tok;
    if (!tok.empty() && tok[0] == '!') {
      t.neg = true;
      tok.erase(0, 1);
    }
    if (tok.empty()) continue;

    // Comparisons: len>100, t>=12.5, n<500, lat>20 (ms).
    static const struct {
      const char* name;
      char field;
    } cmps[] = {{"len", 'l'}, {"lat", 'L'}, {"t", 't'}, {"n", 'n'}};
    bool is_cmp = false;
    for (const auto& c : cmps) {
      const size_t nl = std::strlen(c.name);
      if (tok.size() <= nl || tok.compare(0, nl, c.name) != 0) continue;
      const char a = tok[nl], b = tok.size() > nl + 1 ? tok[nl + 1] : 0;
      int op = 0;
      size_t vat = nl + 1;
      if (a == '>' && b == '=') op = 'g', vat = nl + 2;
      else if (a == '<' && b == '=') op = 'l', vat = nl + 2;
      else if (a == '>') op = '>';
      else if (a == '<') op = '<';
      else if (a == '=') op = '=';
      else if (a == ':' && c.field != 't') op = '=';
      if (!op) continue;
      t.kind = Term::kCmp;
      t.field = c.field;
      t.op = op;
      if (!parse_num(tok.substr(vat), &t.num)) {
        *err = "bad number in '" + shown + "'";
        return false;
      }
      is_cmp = true;
      if (c.field == 't' && !t.neg) {
        if (op == 'g' || op == '>') out->t_lo = std::max(out->t_lo, t.num);
        if (op == 'l' || op == '<') out->t_hi = std::min(out->t_hi, t.num);
        if (op == '=') out->t_lo = std::max(out->t_lo, t.num), out->t_hi = std::min(out->t_hi, t.num);
      }
      break;
    }

    if (!is_cmp) {
      const size_t colon = tok.find(':');
      const std::string field = colon == std::string::npos ? "" : lower(tok.substr(0, colon));
      const std::string value = colon == std::string::npos ? "" : lower(tok.substr(colon + 1));
      static const struct {
        const char* name;
        Term::Kind kind;
      } fields[] = {{"type", Term::kType},     {"dir", Term::kDir},       {"proto", Term::kProto},
                    {"handle", Term::kHandle}, {"cid", Term::kCid},       {"psm", Term::kPsm},
                    {"index", Term::kIndex},   {"opcode", Term::kOpcode}, {"evt", Term::kEvt},
                    {"subevt", Term::kSub},    {"is", Term::kIs}};
      bool known = false;
      for (const auto& f : fields) {
        if (field == f.name) {
          t.kind = f.kind;
          known = true;
        }
      }
      if (!known) {
        // Anything else is text — including words with a colon, like a BD_ADDR.
        t.kind = Term::kText;
        t.text = lower(tok);
        out->has_text = true;
      } else {
        if (value.empty()) {
          *err = "'" + shown + "' has no value";
          return false;
        }
        uint32_t mask = 0;
        for (const std::string& v : split(value, '|')) {
          bool ok = true;
          switch (t.kind) {
            case Term::kType: {
              int found = -1;
              for (int i = 0; i <= kTypeOther; ++i) {
                if (v == pkt_type_name(static_cast<uint8_t>(i))) found = i;
              }
              ok = found >= 0;
              if (ok) mask |= 1u << found;
              break;
            }
            case Term::kDir:
              ok = v == "tx" || v == "rx";
              if (ok) mask |= v == "tx" ? kFlagTx : kFlagRx;
              break;
            case Term::kProto: {
              int found = -1;
              for (int i = 1; i < kProtoCount; ++i) {
                if (v == proto_name(static_cast<uint8_t>(i))) found = i;
              }
              ok = found >= 0;
              if (ok) mask |= 1u << found;
              break;
            }
            case Term::kIs:
              if (v == "err" || v == "error") mask |= 1;
              else if (v == "mark") mask |= 2;
              else if (v == "cont") mask |= 4;
              else if (v == "lat") mask |= 8;
              else ok = false;
              break;
            default: {
              double num = 0;
              ok = parse_num(v, &num) && num >= 0 && num <= 0xffff;
              if (ok) t.values.push_back(static_cast<uint32_t>(num));
              break;
            }
          }
          if (!ok) {
            static const char* hints[] = {
                "cmd|evt|acl|sco|iso|index|log|mgmt|diag", "tx|rx",
                "hci|l2cap|att|smp|sdp|rfcomm|avdtp|rtp|avctp|bnep|hid|sco|iso"};
            std::string hint = "a number";
            if (t.kind == Term::kType) hint = hints[0];
            if (t.kind == Term::kDir) hint = hints[1];
            if (t.kind == Term::kProto) hint = hints[2];
            if (t.kind == Term::kIs) hint = "err|mark|cont|lat";
            *err = "unknown " + field + " '" + v + "' (" + hint + ")";
            return false;
          }
        }
        if (t.kind == Term::kType || t.kind == Term::kDir || t.kind == Term::kProto ||
            t.kind == Term::kIs) {
          t.values.assign(1, mask);
        }
      }
    }
    norm.push_back(shown);
    out->terms.push_back(std::move(t));
  }
  // Index terms first, text last: the cheap test runs before a decode.
  std::stable_partition(out->terms.begin(), out->terms.end(),
                        [](const Term& t) { return t.kind != Term::kText; });
  for (size_t i = 0; i < norm.size(); ++i) out->key += (i ? " " : "") + norm[i];
  return true;
}

bool Filter::match_index(const IndexEntry& e, double t, uint32_t n) const {
  auto in = [](const std::vector<uint32_t>& v, uint32_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
  };
  for (const Term& term : terms) {
    bool m = false;
    switch (term.kind) {
      case Term::kText: continue;
      case Term::kType: m = (term.values[0] >> pkt_type_of(e.mon)) & 1; break;
      case Term::kDir: m = e.flags & term.values[0]; break;
      case Term::kProto: m = (term.values[0] >> e.proto) & 1; break;
      case Term::kHandle: m = e.handle != kNoHandle && in(term.values, e.handle); break;
      case Term::kCid: m = e.cid && in(term.values, e.cid); break;
      case Term::kPsm: m = e.psm && in(term.values, e.psm); break;
      case Term::kIndex: m = e.index != kNoIndex && in(term.values, e.index); break;
      case Term::kOpcode: m = e.opcode && in(term.values, e.opcode); break;
      case Term::kEvt: m = e.mon == kMonEvent && in(term.values, e.evt); break;
      case Term::kSub: m = e.mon == kMonEvent && e.evt == kEvtLeMeta && in(term.values, e.sub); break;
      case Term::kIs: {
        const uint32_t mask = term.values[0];
        m = ((mask & 1) && (e.flags & kFlagErr)) || ((mask & 2) && (e.flags & kFlagMark)) ||
            ((mask & 4) && (e.flags & kFlagCont)) || ((mask & 8) && e.lat_us != kNoLat);
        break;
      }
      case Term::kCmp: {
        double v;
        if (term.field == 'l') v = e.len;
        else if (term.field == 't') v = t;
        else if (term.field == 'n') v = n;
        else if (e.lat_us == kNoLat) {
          // A packet without a latency (not TX data, or never completed) matches no lat term.
          if (!term.neg) return false;
          continue;
        } else v = e.lat_us / 1000.0;
        switch (term.op) {
          case '<': m = v < term.num; break;
          case '>': m = v > term.num; break;
          case 'l': m = v <= term.num; break;
          case 'g': m = v >= term.num; break;
          default: m = term.field == 't' ? std::fabs(v - term.num) < 5e-7 : v == term.num; break;
        }
        break;
      }
    }
    if (m == term.neg) return false;
  }
  return true;
}

bool Filter::match_text(const std::string& s) const {
  for (const Term& term : terms) {
    if (term.kind != Term::kText) continue;
    const bool m = s.find(term.text) != std::string::npos;
    if (m == term.neg) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Tracking links and L2CAP channels while indexing, so a dynamic CID maps to its PSM and every TX
// data packet gets its NOCP latency — the same bookkeeping as the live Decoder, but kept to what
// the index needs.

struct PacketIndex::Tracker {
  struct Chan {
    uint16_t lcid = 0;  // ours: what RX packets carry
    uint16_t rcid = 0;  // the peer's: what TX packets carry
    uint16_t psm = 0;
    uint8_t proto = kProtoL2cap;
    bool open = false;
  };
  struct Frag {
    uint16_t cid = 0, psm = 0;
    uint8_t proto = kProtoNone;
    uint32_t remaining = 0;
    // Signalling SDUs split over ACL fragments are collected, so channel setup is never missed.
    std::vector<uint8_t> sig;
    uint32_t want = 0;
  };
  struct Pend {  // LE / enhanced credit based requests, matched to their response by ident
    bool valid = false;
    bool tx = false;
    uint8_t ident = 0;
    uint8_t n = 0;
    uint16_t cids[5] = {};
  };
  struct Link {
    uint16_t index = 0, handle = 0;
    bool le = false;
    std::vector<Chan> chans;
    Frag frag[2];
    std::deque<uint32_t> fifo;  // entry indices awaiting NOCP
    bool avdtp_sig = false;     // an AVDTP signalling channel is open: the next one is media
    uint16_t avdtp_sig_lcid = 0;
    Pend pend[8];
    size_t pend_next = 0;
    size_t info = 0;  // conn_info_ slot
  };
  std::vector<std::unique_ptr<Link>> links;

  Link* find(uint16_t index, uint16_t handle) {
    for (auto& l : links) {
      if (l->index == index && l->handle == handle) return l.get();
    }
    return nullptr;
  }
  void drop(uint16_t index, uint16_t handle) {
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const std::unique_ptr<Link>& l) {
                                 return l->index == index && l->handle == handle;
                               }),
                links.end());
  }
};

namespace {

using Link = PacketIndex::Tracker::Link;
using Chan = PacketIndex::Tracker::Chan;
using Pend = PacketIndex::Tracker::Pend;

void chan_opened(Link* l, Chan* ch) {
  ch->open = true;
  ch->proto = proto_for_psm(ch->psm);
  if (ch->psm == kPsmAvdtp && !l->le) {
    // As the live decoder: the first AVDTP channel is signalling, every later one a transport.
    if (!l->avdtp_sig) {
      l->avdtp_sig = true;
      l->avdtp_sig_lcid = ch->lcid;
    } else {
      ch->proto = kProtoRtp;
    }
  }
}

void chan_closed(Link* l, size_t i) {
  if (l->chans[i].proto == kProtoAvdtp && l->chans[i].lcid == l->avdtp_sig_lcid) l->avdtp_sig = false;
  l->chans.erase(l->chans.begin() + static_cast<long>(i));
}

Chan* add_chan(Link* l, uint16_t lcid, uint16_t rcid, uint16_t psm) {
  if (l->chans.size() >= kMaxChans) l->chans.erase(l->chans.begin());
  l->chans.push_back(Chan{lcid, rcid, psm, kProtoL2cap, false});
  return &l->chans.back();
}

// The signalling commands that open and close channels; CIDs from the sender's point of view (see
// Decoder::l2cap_sig).
void track_sig(Link* l, bool tx, const uint8_t* d, size_t len) {
  auto pending = [&](bool by_lcid, uint16_t cid) -> Chan* {
    for (auto& ch : l->chans) {
      if (ch.open) continue;
      if (by_lcid ? (ch.lcid == cid && !ch.rcid) : (ch.rcid == cid && !ch.lcid)) return &ch;
    }
    return nullptr;
  };
  auto set_peer = [&](Chan* ch, uint16_t cid) {
    if (tx) ch->lcid = cid;
    else ch->rcid = cid;
  };
  while (len >= 4) {
    const uint8_t code = d[0], ident = d[1];
    const size_t clen = le16(d + 2);
    const uint8_t* p = d + 4;
    if (clen > len - 4) break;
    switch (code) {
      case kSigConnReq:
        if (clen >= 4) add_chan(l, tx ? le16(p + 2) : 0, tx ? 0 : le16(p + 2), le16(p));
        break;
      case kSigConnRsp:
        if (clen >= 6) {
          const uint16_t dcid = le16(p), scid = le16(p + 2), result = le16(p + 4);
          Chan* ch = pending(!tx, scid);
          if (!ch) break;
          if (result == 0) {
            set_peer(ch, dcid);
            chan_opened(l, ch);
          } else if (result != 1) {
            chan_closed(l, static_cast<size_t>(ch - l->chans.data()));
          }
        }
        break;
      case kSigDisconnRsp:
        if (clen >= 4) {
          const uint16_t dcid = le16(p), scid = le16(p + 2);
          const uint16_t lcid = tx ? dcid : scid, rcid = tx ? scid : dcid;
          for (size_t i = 0; i < l->chans.size(); ++i) {
            if (l->chans[i].lcid == lcid && l->chans[i].rcid == rcid) {
              chan_closed(l, i);
              break;
            }
          }
        }
        break;
      case kSigLeConnReq:
      case kSigEcredConnReq: {
        const bool ecred = code == kSigEcredConnReq;
        if (clen < 10) break;
        Pend& pd = l->pend[l->pend_next];
        l->pend_next = (l->pend_next + 1) % 8;
        pd = Pend{};
        pd.valid = true;
        pd.tx = tx;
        pd.ident = ident;
        const uint16_t psm = le16(p);
        if (!ecred) {
          pd.cids[pd.n++] = le16(p + 2);
        } else {
          for (size_t off = 8; off + 2 <= clen && pd.n < 5; off += 2) pd.cids[pd.n++] = le16(p + off);
        }
        for (uint8_t i = 0; i < pd.n; ++i) add_chan(l, tx ? pd.cids[i] : 0, tx ? 0 : pd.cids[i], psm);
        break;
      }
      case kSigLeConnRsp:
      case kSigEcredConnRsp: {
        const bool ecred = code == kSigEcredConnRsp;
        if (clen < (ecred ? 8u : 10u)) break;
        Pend* pd = nullptr;
        for (auto& x : l->pend) {
          if (x.valid && x.ident == ident && x.tx != tx) pd = &x;
        }
        if (!pd) break;
        pd->valid = false;
        for (uint8_t i = 0; i < pd->n; ++i) {
          Chan* ch = pending(!tx, pd->cids[i]);
          if (!ch) continue;
          uint16_t dcid = 0;
          if (!ecred) {
            dcid = le16(p + 8) == 0 ? le16(p) : 0;
          } else if (8 + 2u * i + 2 <= clen) {
            dcid = le16(p + 8 + 2 * i);
          }
          if (dcid) {
            set_peer(ch, dcid);
            chan_opened(l, ch);
          } else {
            chan_closed(l, static_cast<size_t>(ch - l->chans.data()));
          }
        }
        break;
      }
      default:
        break;
    }
    d += 4 + clen;
    len -= 4 + clen;
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// PacketIndex

PacketIndex::PacketIndex() : tracker_(std::make_unique<Tracker>()) {}

PacketIndex::~PacketIndex() {
  if (fd_ >= 0) close(fd_);
}

bool PacketIndex::open(const std::string& path, std::string* err) {
  path_ = path;
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd_ < 0) {
    *err = std::string("cannot open: ") + std::strerror(errno);
    return false;
  }
  struct stat st {};
  if (fstat(fd_, &st) != 0) {
    *err = std::string("stat: ") + std::strerror(errno);
    return false;
  }
  ino_ = st.st_ino;
  dev_ = st.st_dev;
  file_size_ = static_cast<uint64_t>(st.st_size);
  uint8_t hdr[16];
  if (pread(fd_, hdr, sizeof(hdr), 0) != static_cast<ssize_t>(sizeof(hdr)) ||
      std::memcmp(hdr, "btsnoop\0", 8) != 0) {
    *err = "not a btsnoop file";
    return false;
  }
  datalink_ = be32(hdr + 12);
  if (datalink_ != kBtsnoopMonitor && datalink_ != kBtsnoopUart && datalink_ != kBtsnoopHci) {
    *err = "unsupported btsnoop datalink " + std::to_string(datalink_);
    return false;
  }
  return true;
}

bool PacketIndex::still_valid() const {
  struct stat st {};
  if (stat(path_.c_str(), &st) != 0) return false;
  // btmon only appends; a different inode is a new file under the old name, a shorter one was
  // rewritten. Either way the index describes something else.
  return static_cast<uint64_t>(st.st_ino) == ino_ && static_cast<uint64_t>(st.st_dev) == dev_ &&
         static_cast<uint64_t>(st.st_size) >= next_off_;
}

bool PacketIndex::fill(uint64_t off, size_t n) {
  if (off >= buf_off_ && off + n <= buf_off_ + buf_len_) return true;
  const size_t want = std::max(n, kReadChunk);
  if (buf_.size() < want) buf_.resize(want);
  size_t got = 0;
  while (got < want) {
    const ssize_t r = pread(fd_, buf_.data() + got, want - got, static_cast<off_t>(off + got));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    got += static_cast<size_t>(r);
  }
  buf_off_ = off;
  buf_len_ = got;
  return got >= n;
}

const uint8_t* PacketIndex::read(size_t i, size_t* len) {
  if (i >= entries_.size()) return nullptr;
  const uint64_t off = entries_[i].off;
  if (!fill(off, kRecHdr)) return nullptr;
  const uint32_t incl = be32(buf_.data() + (off - buf_off_) + 4);
  if (incl > kMaxRecord || !fill(off, kRecHdr + incl)) return nullptr;
  const uint8_t* p = buf_.data() + (off - buf_off_) + kRecHdr;
  if (datalink_ == kBtsnoopUart) {
    *len = incl ? incl - 1 : 0;
    return p + 1;
  }
  *len = incl;
  return p;
}

std::string PacketIndex::summary(size_t i) {
  size_t len = 0;
  const uint8_t* d = read(i, &len);
  if (!d) return "(unreadable)";
  Dissection ds;
  dissect(entries_[i], d, len, false, &ds);
  return ds.summary;
}

bool PacketIndex::extend(int64_t deadline_us) {
  struct stat st {};
  if (fstat(fd_, &st) == 0) {
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    if (size != file_size_) {
      file_size_ = size;
      at_end_ = false;
    }
  }
  if (at_end_) return true;
  // A file being written: what was read past the old end may have been a partial record.
  if (buf_off_ + buf_len_ > next_off_) buf_len_ = 0;

  Tracker& tr = *tracker_;
  size_t done = 0;
  for (;;) {
    if ((++done & 255) == 0 && mono_us() > deadline_us) return false;
    if (entries_.size() >= kMaxEntries || !fill(next_off_, kRecHdr)) break;
    const uint8_t* rec = buf_.data() + (next_off_ - buf_off_);
    const uint32_t incl = be32(rec + 4);
    if (incl > kMaxRecord) break;  // corrupt: nothing after it can be trusted
    if (!fill(next_off_, kRecHdr + incl)) break;  // the last record is still being written
    rec = buf_.data() + (next_off_ - buf_off_);
    const uint32_t flags = be32(rec + 8);
    const uint64_t raw_ts = (static_cast<uint64_t>(be32(rec + 16)) << 32) | be32(rec + 20);
    const uint8_t* d = rec + kRecHdr;
    size_t len = incl;
    uint16_t index = 0, mon = 0;
    if (datalink_ == kBtsnoopMonitor) {
      index = static_cast<uint16_t>(flags >> 16);
      mon = static_cast<uint16_t>(flags & 0xffff);
    } else if (datalink_ == kBtsnoopUart) {
      mon = incl ? btsnoop_hci_opcode(d[0], flags) : 0xffff;
      ++d;
      len = incl ? incl - 1 : 0;
    } else {
      mon = btsnoop_hci_opcode(0xff, flags);
    }
    const uint64_t rec_off = next_off_;
    next_off_ += kRecHdr + incl;
    if (mon == 0xffff) continue;

    IndexEntry e{};
    e.off = static_cast<uint32_t>(rec_off);
    e.lat_us = kNoLat;
    e.ts_us = static_cast<int64_t>(raw_ts) - kBtsnoopEpochDelta;
    e.len = static_cast<uint16_t>(std::min<size_t>(len, 0xffff));
    e.handle = kNoHandle;
    e.index = index == kHciDevNone ? kNoIndex : static_cast<uint8_t>(std::min<uint16_t>(index, 0xfe));
    e.mon = static_cast<uint8_t>(mon);
    const uint32_t me = static_cast<uint32_t>(entries_.size());

    auto info_for = [&](uint16_t handle, const char* type, bool create_new) -> size_t {
      for (size_t i = 0; i < conn_info_.size(); ++i) {
        if (conn_info_[i].index == index && conn_info_[i].handle == handle) {
          if (create_new && type) conn_info_[i].type = type;
          return i;
        }
      }
      ConnInfo ci;
      ci.index = index;
      ci.handle = handle;
      ci.type = type ? type : "acl";
      ci.first_us = e.ts_us;
      conn_info_.push_back(ci);
      return conn_info_.size() - 1;
    };
    auto link_for = [&](uint16_t handle, const char* type) -> Link* {
      Link* l = tr.find(index, handle);
      if (!l) {
        if (tr.links.size() >= 64) tr.links.erase(tr.links.begin());
        auto nl = std::make_unique<Link>();
        nl->index = index;
        nl->handle = handle;
        nl->le = type && std::strcmp(type, "le") == 0;
        nl->info = info_for(handle, type, false);
        tr.links.push_back(std::move(nl));
        l = tr.links.back().get();
      }
      return l;
    };
    auto connected = [&](uint16_t handle, const char* type, const uint8_t* peer) {
      tr.drop(index, handle);  // a reused handle starts afresh
      Link* l = link_for(handle, type);
      l->le = std::strcmp(type, "le") == 0;
      ConnInfo& ci = conn_info_[l->info];
      ci.type = type;
      if (peer) ci.peer = bdaddr_str(peer);
    };
    auto data = [&](uint16_t handle, bool tx, uint32_t payload, const char* type) -> Link* {
      Link* l = link_for(handle, type);
      ConnInfo& ci = conn_info_[l->info];
      ci.last_us = e.ts_us;
      if (tx) {
        ci.tx_bytes += payload;
        ++ci.tx_pkts;
        if (l->fifo.size() >= kTxFifo) l->fifo.pop_front();
        l->fifo.push_back(me);
      } else {
        ci.rx_bytes += payload;
        ++ci.rx_pkts;
      }
      return l;
    };

    switch (mon) {
      case kMonCommand:
        e.proto = kProtoHci;
        e.flags |= kFlagTx;
        if (len >= 3) {
          e.opcode = le16(d);
          if (hci_cmd_has_handle(e.opcode) && len >= 5) e.handle = le16(d + 3) & 0x0fff;
        }
        break;
      case kMonEvent: {
        e.proto = kProtoHci;
        e.flags |= kFlagRx;
        if (len < 2) break;
        e.evt = d[0];
        const uint8_t* p = d + 2;
        const size_t n = std::min<size_t>(d[1], len - 2);
        if (e.evt == kEvtCmdComplete && n >= 3) {
          e.opcode = le16(p + 1);
          if (n >= 4 && p[3]) e.flags |= kFlagErr;
          if (hci_cmd_has_handle(e.opcode) && n >= 6) e.handle = le16(p + 4) & 0x0fff;
        } else if (e.evt == 0x0f && n >= 4) {
          e.opcode = le16(p + 2);
          if (p[0]) e.flags |= kFlagErr;
        } else if (e.evt == kEvtNumCompletedPackets && n >= 1) {
          for (size_t i = 0; i < p[0] && 1 + 4 * (i + 1) <= n; ++i) {
            const uint16_t h = le16(p + 1 + 4 * i) & 0x0fff;
            const uint16_t count = le16(p + 3 + 4 * i);
            if (i == 0) e.handle = h;
            Link* l = tr.find(index, h);
            if (!l) continue;
            ConnInfo& ci = conn_info_[l->info];
            for (uint16_t j = 0; j < count && !l->fifo.empty(); ++j) {
              IndexEntry& sent = entries_[l->fifo.front()];
              l->fifo.pop_front();
              const int64_t lat = e.ts_us - sent.ts_us;
              if (lat >= 0) {
                sent.lat_us = static_cast<uint32_t>(std::min<int64_t>(lat, 0xfffffffe));
                ++ci.lat_n;
              }
            }
          }
        } else if (e.evt == kEvtLeMeta && n >= 1) {
          e.sub = p[0];
          const uint8_t* q = p + 1;
          const size_t m = n - 1;
          if (le_status_handle(e.sub) && m >= 3) {
            e.handle = le16(q + 1) & 0x0fff;
            if (q[0]) e.flags |= kFlagErr;
          } else if (le_handle_first(e.sub) && m >= 2) {
            e.handle = le16(q) & 0x0fff;
          } else if ((e.sub == 0x1b || e.sub == 0x1d || e.sub == 0x08 || e.sub == 0x09 ||
                      e.sub == 0x0e || e.sub == 0x12) && m >= 1 && q[0]) {
            e.flags |= kFlagErr;
          }
          if ((e.sub == kLeConnComplete || e.sub == kLeEnhConnComplete ||
               e.sub == kLeEnhConnCompleteV2) && m >= 11 && q[0] == 0) {
            connected(e.handle, "le", q + 5);
          } else if ((e.sub == kLeCisEstablished || e.sub == kLeCisEstablishedV2) && m >= 3 && q[0] == 0) {
            connected(e.handle, "cis", nullptr);
          } else if ((e.sub == kLeBigComplete || e.sub == kLeBigSyncEstablished) && q[0] == 0) {
            const size_t num_at = e.sub == kLeBigComplete ? 17 : 13;
            for (size_t i = 0; num_at < m && i < q[num_at] && num_at + 1 + 2 * (i + 1) <= m; ++i) {
              connected(le16(q + num_at + 1 + 2 * i) & 0x0fff, "bis", nullptr);
            }
          }
        } else if (evt_status_handle(e.evt) && n >= 3) {
          e.handle = le16(p + 1) & 0x0fff;
          if (p[0]) e.flags |= kFlagErr;
          if (p[0] == 0 && e.evt == kEvtConnComplete && n >= 10) {
            connected(e.handle, p[9] == 0x01 ? "acl" : "sco", p + 3);
          } else if (p[0] == 0 && e.evt == kEvtSyncConnComplete && n >= 10) {
            connected(e.handle, p[9] == 0x02 ? "esco" : "sco", p + 3);
          } else if (p[0] == 0 && e.evt == kEvtDisconnComplete) {
            tr.drop(index, e.handle);
          }
        } else if (evt_handle_first(e.evt) && n >= 2) {
          e.handle = le16(p) & 0x0fff;
        } else if ((e.evt == 0x01 || e.evt == 0x07 || e.evt == 0x12 || e.evt == 0x36) && n >= 1 && p[0]) {
          e.flags |= kFlagErr;
        } else if (e.evt == 0x10 || e.evt == 0x1a) {
          e.flags |= kFlagErr;  // Hardware Error, Data Buffer Overflow
        }
        break;
      }
      case kMonAclTx:
      case kMonAclRx: {
        const bool tx = mon == kMonAclTx;
        e.flags |= tx ? kFlagTx : kFlagRx;
        e.proto = kProtoL2cap;
        if (len < 4) break;
        const uint16_t hf = le16(d);
        e.handle = hf & 0x0fff;
        const uint8_t pb = (hf >> 12) & 3;
        const size_t n = std::min<size_t>(len - 4, le16(d + 2));
        const uint8_t* p = d + 4;
        Link* l = data(e.handle, tx, static_cast<uint32_t>(n), nullptr);
        Tracker::Frag& f = l->frag[tx ? 0 : 1];
        if (pb == 0x01) {
          e.flags |= kFlagCont;
          e.cid = f.cid;
          e.psm = f.psm;
          e.proto = f.proto == kProtoNone ? static_cast<uint8_t>(kProtoL2cap) : f.proto;
          if (f.want && f.sig.size() < f.want) {
            const size_t take = std::min<size_t>(n, f.want - f.sig.size());
            f.sig.insert(f.sig.end(), p, p + take);
            if (f.sig.size() == f.want) {
              track_sig(l, tx, f.sig.data(), f.sig.size());
              if (l2cap_payload_is_error(kProtoL2cap, f.cid, f.sig.data(), f.sig.size())) e.flags |= kFlagErr;
              f.want = 0;
              f.sig.clear();
            }
          }
          f.remaining = f.remaining > n ? static_cast<uint32_t>(f.remaining - n) : 0;
          if (!f.remaining) f = Tracker::Frag{};
          break;
        }
        f = Tracker::Frag{};
        if (n < 4) break;
        const uint16_t l2len = le16(p), cid = le16(p + 2);
        e.cid = cid;
        if (cid < kCidDynamicStart) {
          e.proto = proto_for_fixed_cid(cid);
          if (cid == kCidAtt || cid == kCidLeSignaling || cid == kCidSmp) {
            l->le = true;
            ConnInfo& ci = conn_info_[l->info];
            if (std::strcmp(ci.type, "acl") == 0 && ci.peer.empty()) ci.type = "le";
          }
        } else {
          for (const auto& ch : l->chans) {
            if (ch.open && (tx ? ch.rcid : ch.lcid) == cid) {
              e.psm = ch.psm;
              e.proto = ch.proto;
              break;
            }
          }
        }
        const uint8_t* q = p + 4;
        const size_t got = n - 4;
        const bool sig = cid == kCidSignaling || cid == kCidLeSignaling;
        if (got >= l2len) {
          if (sig) track_sig(l, tx, q, l2len);
          if (l2cap_payload_is_error(e.proto, cid, q, l2len)) e.flags |= kFlagErr;
        } else {
          f.cid = cid;
          f.psm = e.psm;
          f.proto = e.proto;
          f.remaining = static_cast<uint32_t>(l2len - got);
          if (sig && l2len <= 4096) {
            f.want = l2len;
            f.sig.assign(q, q + got);
          } else if (l2cap_payload_is_error(e.proto, cid, q, got)) {
            e.flags |= kFlagErr;
          }
        }
        break;
      }
      case kMonScoTx:
      case kMonScoRx:
      case kMonIsoTx:
      case kMonIsoRx: {
        const bool tx = mon == kMonScoTx || mon == kMonIsoTx;
        const bool iso = mon == kMonIsoTx || mon == kMonIsoRx;
        e.flags |= tx ? kFlagTx : kFlagRx;
        e.proto = iso ? kProtoIso : kProtoSco;
        const size_t hdr = iso ? 4 : 3;
        if (len < hdr) break;
        e.handle = le16(d) & 0x0fff;
        data(e.handle, tx, static_cast<uint32_t>(len - hdr), iso ? "cis" : "sco");
        break;
      }
      case kMonUserLogging:
        if (len >= 2 + 7u && d[1] >= 7 && std::strncmp(reinterpret_cast<const char*>(d + 2), "btbench", 7) == 0) {
          e.flags |= kFlagMark;
        }
        break;
      default:
        break;
    }
    entries_.push_back(e);
  }
  at_end_ = true;
  return true;
}

PacketIndex::Query* PacketIndex::query(const Filter& f, int64_t deadline_us) {
  auto it = std::find_if(queries_.begin(), queries_.end(),
                         [&](const Query& q) { return q.key == f.key; });
  if (it == queries_.end()) {
    queries_.push_front(Query{f.key, f, {}, 0});
    if (queries_.size() > kMaxQueries) queries_.pop_back();
  } else if (it != queries_.begin()) {
    queries_.splice(queries_.begin(), queries_, it);
  }
  Query& q = queries_.front();
  const int64_t t0 = t0_us();
  const size_t start = q.scanned;
  size_t decoded = 0;
  size_t i = q.scanned;
  for (; i < entries_.size(); ++i) {
    // Text terms decode: check the clock every few packets, but always make some progress so a
    // client polling with a tiny budget still gets to the end.
    if (f.has_text && (decoded & 31) == 31 && i - start > 512 && mono_us() > deadline_us) break;
    const IndexEntry& e = entries_[i];
    const double t = static_cast<double>(e.ts_us - t0) / 1e6;
    if (!f.match_index(e, t, static_cast<uint32_t>(i + 1))) continue;
    if (f.has_text) {
      ++decoded;
      if (!f.match_text(lower(summary(i)))) continue;
    }
    q.matches.push_back(static_cast<uint32_t>(i));
  }
  q.scanned = i;
  return &q;
}

// ---------------------------------------------------------------------------------------------
// PacketStore

std::shared_ptr<PacketIndex> PacketStore::get(const std::string& name, int* status,
                                              std::string* err) {
  if (!Capture::valid_name(name)) {
    *status = 400;
    *err = "invalid capture name";
    return nullptr;
  }
  const std::string path = dir_ + "/" + name;
  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    *status = 404;
    *err = "no such capture";
    return nullptr;
  }
  std::lock_guard<std::mutex> lk(mu_);
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    if (it->first != name) continue;
    std::shared_ptr<PacketIndex> idx = it->second;
    bool ok;
    {
      std::lock_guard<std::mutex> ilk(idx->mu);
      ok = idx->still_valid();
    }
    lru_.erase(it);
    if (!ok) break;  // rebuilt below
    lru_.emplace_front(name, idx);
    return idx;
  }
  auto idx = std::make_shared<PacketIndex>();
  if (!idx->open(path, err)) {
    *status = *err == "not a btsnoop file" || err->rfind("unsupported", 0) == 0 ? 415 : 500;
    return nullptr;
  }
  lru_.emplace_front(name, idx);
  while (lru_.size() > kMaxIndexes) lru_.pop_back();
  return idx;
}

namespace {

json entry_json(PacketIndex& idx, size_t i, const std::string& summary) {
  const IndexEntry& e = idx.at(i);
  const double t = static_cast<double>(e.ts_us - idx.t0_us()) / 1e6;
  const char* proto = proto_name(e.proto);
  json j{{"n", i + 1},
         {"ts_ms", static_cast<double>(e.ts_us) / 1000.0},
         {"t", std::round(t * 1e6) / 1e6},
         {"index", e.index == kNoIndex ? json(nullptr) : json(e.index)},
         {"dir", e.flags & kFlagTx ? "tx" : e.flags & kFlagRx ? "rx" : ""},
         {"type", pkt_type_name(pkt_type_of(e.mon))},
         {"proto", proto ? json(proto) : json(nullptr)},
         {"handle", e.handle == kNoHandle ? json(nullptr) : json(e.handle)},
         {"cid", e.cid ? json(e.cid) : json(nullptr)},
         {"psm", e.psm ? json(e.psm) : json(nullptr)},
         {"len", e.len},
         {"lat_ms", e.lat_us == kNoLat ? json(nullptr) : json(round3(e.lat_us / 1000.0))},
         {"summary", summary}};
  if (e.flags & kFlagErr) j["err"] = true;
  if (e.flags & kFlagMark) j["mark"] = true;
  if (e.flags & kFlagCont) j["cont"] = true;
  return j;
}

// Progress fields every answer carries, so a client knows whether to ask again.
void progress(PacketIndex& idx, const PacketIndex::Query* q, json* out) {
  const bool complete = idx.at_end() && (!q || q->scanned >= idx.size());
  (*out)["packets_in_file"] = idx.size();
  (*out)["indexed_bytes"] = idx.indexed_bytes();
  (*out)["file_size"] = idx.file_size();
  (*out)["datalink"] = idx.datalink();
  (*out)["t0_ms"] = idx.size() ? json(static_cast<double>(idx.t0_us()) / 1000.0) : json(nullptr);
  (*out)["complete"] = complete;
  if (q) {
    (*out)["scanned"] = q->scanned;
    (*out)["next"] = complete ? json(nullptr) : json(q->scanned + 1);
  }
}

// Bucket widths a person reads easily.
int64_t nice_bucket_ms(double min_ms) {
  static const int64_t steps[] = {1,      2,      5,       10,      20,      50,       100,
                                  200,    250,    500,     1000,    2000,    5000,     10000,
                                  15000,  30000,  60000,   120000,  300000,  600000,   900000,
                                  1800000, 3600000, 7200000, 21600000, 43200000, 86400000};
  for (int64_t s : steps) {
    if (s >= min_ms) return s;
  }
  return static_cast<int64_t>(std::ceil(min_ms / 86400000.0)) * 86400000;
}

}  // namespace

bool PacketStore::list(const std::string& name, const ListParams& p, json* out, int* status,
                       std::string* err) {
  Filter f;
  if (!Filter::parse(p.filter, &f, err)) {
    *status = 400;
    return false;
  }
  std::shared_ptr<PacketIndex> idx = get(name, status, err);
  if (!idx) return false;
  std::lock_guard<std::mutex> lk(idx->mu);
  const int64_t deadline = mono_us() + int64_t{budget_ms_} * 1000;
  idx->extend(deadline);
  PacketIndex::Query* q = idx->query(f, deadline);

  size_t start = p.start;
  const auto& m = q->matches;
  if (p.at_n >= 1) {
    start = static_cast<size_t>(std::lower_bound(m.begin(), m.end(), static_cast<uint32_t>(p.at_n - 1)) - m.begin());
  } else if (p.at_t >= 0) {
    // Matches are in file order, and timestamps nearly so: the first one at or after at_t.
    const int64_t want = idx->t0_us() + static_cast<int64_t>(p.at_t * 1e6);
    start = m.size();
    for (size_t i = 0; i < m.size(); ++i) {
      if (idx->at(m[i]).ts_us >= want) {
        start = i;
        break;
      }
    }
  }
  const size_t count = std::min<size_t>(p.count, 1000);
  json pkts = json::array();
  for (size_t i = start; i < m.size() && i < start + count; ++i) {
    pkts.push_back(entry_json(*idx, m[i], idx->summary(m[i])));
  }
  *out = json{{"name", name}, {"filter", f.key}, {"total", m.size()}, {"start", start},
              {"packets", std::move(pkts)}};
  progress(*idx, q, out);
  return true;
}

bool PacketStore::packet(const std::string& name, uint32_t n, json* out, int* status,
                         std::string* err) {
  std::shared_ptr<PacketIndex> idx = get(name, status, err);
  if (!idx) return false;
  std::lock_guard<std::mutex> lk(idx->mu);
  // The frame may lie beyond what has been indexed so far; index up to it if time allows.
  const int64_t deadline = mono_us() + int64_t{budget_ms_} * 1000;
  if (n > idx->size()) idx->extend(deadline);
  if (n < 1 || n > idx->size()) {
    *status = 404;
    *err = n > idx->size() && !idx->at_end() ? "not indexed yet, try again" : "no such packet";
    return false;
  }
  const size_t i = n - 1;
  size_t len = 0;
  const uint8_t* d = idx->read(i, &len);
  if (!d) {
    *status = 500;
    *err = "cannot read the packet";
    return false;
  }
  const IndexEntry e = idx->at(i);
  const std::vector<uint8_t> bytes(d, d + len);
  Dissection ds;
  dissect(e, bytes.data(), bytes.size(), true, &ds);

  json frame{{"name", "Frame"}, {"value", std::to_string(n)}, {"children", json::array()}};
  auto& fc = frame["children"];
  const time_t secs = static_cast<time_t>(e.ts_us / 1000000);
  struct tm tmv {};
  localtime_r(&secs, &tmv);
  char tbuf[64];
  std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv);
  char ubuf[96];
  std::snprintf(ubuf, sizeof(ubuf), "%s.%06lld", tbuf, static_cast<long long>(e.ts_us % 1000000));
  fc.push_back({{"name", "Time"}, {"value", ubuf}});
  fc.push_back({{"name", "Since first packet"},
                {"value", std::to_string(static_cast<double>(e.ts_us - idx->t0_us()) / 1e6) + " s"}});
  fc.push_back({{"name", "Controller"},
                {"value", e.index == kNoIndex ? std::string("(none)") : "hci" + std::to_string(e.index)}});
  fc.push_back({{"name", "Monitor opcode"}, {"value", std::to_string(e.mon) + " (" + mon_name(e.mon) + ")"}});
  fc.push_back({{"name", "Length"}, {"value", std::to_string(len) + " bytes"}});
  if (e.lat_us != kNoLat) {
    char lb[48];
    std::snprintf(lb, sizeof(lb), "%.3f ms", e.lat_us / 1000.0);
    fc.push_back({{"name", "TX latency (HCI → NOCP)"}, {"value", lb}});
  }
  if (e.psm) fc.push_back({{"name", "Channel PSM (from its setup)"}, {"value", std::to_string(e.psm)}});
  json fields = json::array();
  fields.push_back(std::move(frame));
  for (auto& x : ds.fields) fields.push_back(std::move(x));

  static const char* digits = "0123456789abcdef";
  std::string hex;
  hex.reserve(len * 2);
  for (uint8_t b : bytes) {
    hex.push_back(digits[b >> 4]);
    hex.push_back(digits[b & 15]);
  }
  *out = entry_json(*idx, i, ds.summary);
  (*out)["fields"] = std::move(fields);
  (*out)["hex"] = std::move(hex);
  (*out)["packets_in_file"] = idx->size();
  return true;
}

bool PacketStore::graph(const std::string& name, const GraphParams& p, json* out, int* status,
                        std::string* err) {
  Filter f;
  if (!Filter::parse(p.filter, &f, err)) {
    *status = 400;
    return false;
  }
  std::shared_ptr<PacketIndex> idx = get(name, status, err);
  if (!idx) return false;
  std::lock_guard<std::mutex> lk(idx->mu);
  const int64_t deadline = mono_us() + int64_t{budget_ms_} * 1000;
  idx->extend(deadline);
  PacketIndex::Query* q = idx->query(f, deadline);

  const int64_t t0 = idx->t0_us();
  double t_last = 0;
  for (size_t i = idx->size(); i-- > 0 && i + 64 > idx->size();) {
    t_last = std::max(t_last, static_cast<double>(idx->at(i).ts_us - t0) / 1e6);
  }
  double from = p.from >= 0 ? p.from : std::max(0.0, f.t_lo);
  double to = p.to >= 0 ? p.to : std::min(t_last, f.t_hi);
  if (!(to > from)) to = from + 1;
  const double span_ms = (to - from) * 1000.0;
  const size_t points = std::max<size_t>(10, std::min(p.points, kMaxPoints));
  int64_t bucket = nice_bucket_ms(span_ms / static_cast<double>(points));
  if (p.bucket_ms > 0) bucket = std::max<int64_t>(p.bucket_ms, bucket);
  const size_t nb = std::min<size_t>(points,
                                     std::max<size_t>(1, static_cast<size_t>(std::ceil(span_ms / static_cast<double>(bucket)))));
  // The end is inclusive: by default it is the last packet's time, which must be in the graph.
  // (A filter's t<X has already excluded what lies at X.)
  auto bucket_of = [&](int64_t ts) -> long {
    const double t = static_cast<double>(ts - t0) / 1e6;
    if (t < from || t > to) return -1;
    const long b = static_cast<long>((t - from) * 1000.0 / static_cast<double>(bucket));
    return std::min(b, static_cast<long>(nb) - 1);
  };

  std::vector<uint32_t> pk(nb, 0);
  std::vector<uint64_t> txb(nb, 0), rxb(nb, 0);
  size_t in_range = 0;
  for (uint32_t i : q->matches) {
    const IndexEntry& e = idx->at(i);
    const long b = bucket_of(e.ts_us);
    if (b < 0) continue;
    ++in_range;
    ++pk[static_cast<size_t>(b)];
    if (e.flags & kFlagTx) txb[static_cast<size_t>(b)] += e.len;
    if (e.flags & kFlagRx) rxb[static_cast<size_t>(b)] += e.len;
  }

  // The connections, busiest first; and the one whose throughput and latency are graphed.
  std::vector<const PacketIndex::ConnInfo*> cs;
  for (const auto& c : idx->conns()) cs.push_back(&c);
  std::sort(cs.begin(), cs.end(), [](const auto* a, const auto* b) {
    return a->tx_bytes + a->rx_bytes > b->tx_bytes + b->rx_bytes;
  });
  json conns = json::array();
  for (size_t i = 0; i < cs.size() && i < 32; ++i) {
    const auto* c = cs[i];
    conns.push_back(json{{"index", c->index},
                         {"handle", c->handle},
                         {"type", c->type},
                         {"peer", c->peer},
                         {"tx_bytes", c->tx_bytes},
                         {"rx_bytes", c->rx_bytes},
                         {"tx_pkts", c->tx_pkts},
                         {"rx_pkts", c->rx_pkts},
                         {"lat_n", c->lat_n},
                         {"first_t", round3(static_cast<double>(c->first_us - t0) / 1e6)},
                         {"last_t", round3(static_cast<double>(c->last_us - t0) / 1e6)}});
  }
  int ci = p.conn_index, ch = p.conn_handle;
  if (ch < 0 && !cs.empty()) {
    ci = cs[0]->index;
    ch = cs[0]->handle;
  }
  if (ch >= 0 && ci < 0) ci = 0;

  json conn = nullptr;
  if (ch >= 0) {
    std::vector<uint64_t> ctx(nb, 0), crx(nb, 0);
    std::vector<std::pair<uint32_t, uint32_t>> lats;  // (bucket, µs)
    for (size_t i = 0; i < idx->size(); ++i) {
      const IndexEntry& e = idx->at(i);
      if (e.handle != ch || e.index != ci) continue;
      const uint8_t type = pkt_type_of(e.mon);
      if (type != kTypeAcl && type != kTypeSco && type != kTypeIso) continue;
      const long b = bucket_of(e.ts_us);
      if (b < 0) continue;
      const uint32_t hdr = type == kTypeSco ? 3 : 4;
      const uint32_t payload = e.len > hdr ? e.len - hdr : 0;
      if (e.flags & kFlagTx) {
        ctx[static_cast<size_t>(b)] += payload;
        if (e.lat_us != kNoLat) lats.emplace_back(static_cast<uint32_t>(b), e.lat_us);
      } else {
        crx[static_cast<size_t>(b)] += payload;
      }
    }
    std::stable_sort(lats.begin(), lats.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    const double per_s = 1000.0 / static_cast<double>(bucket);
    json tx = json::array(), rx = json::array(), p50 = json::array(), p95 = json::array(),
         mx = json::array();
    LatencyHist h;
    size_t li = 0;
    for (size_t b = 0; b < nb; ++b) {
      tx.push_back(static_cast<uint64_t>(static_cast<double>(ctx[b]) * 8 * per_s));
      rx.push_back(static_cast<uint64_t>(static_cast<double>(crx[b]) * 8 * per_s));
      h.clear();
      while (li < lats.size() && lats[li].first == b) h.add_us(lats[li++].second);
      if (h.count()) {
        p50.push_back(std::round(h.percentile_ms(0.50) * 100) / 100);
        p95.push_back(std::round(h.percentile_ms(0.95) * 100) / 100);
        mx.push_back(std::round(h.max_ms() * 100) / 100);
      } else {
        p50.push_back(nullptr);
        p95.push_back(nullptr);
        mx.push_back(nullptr);
      }
    }
    conn = json{{"index", ci},         {"handle", ch},         {"tx_bps", std::move(tx)},
                {"rx_bps", std::move(rx)}, {"lat_p50", std::move(p50)}, {"lat_p95", std::move(p95)},
                {"lat_max", std::move(mx)}};
  }

  json jpk = json::array(), jtx = json::array(), jrx = json::array();
  for (size_t b = 0; b < nb; ++b) {
    jpk.push_back(pk[b]);
    jtx.push_back(txb[b]);
    jrx.push_back(rxb[b]);
  }
  *out = json{{"name", name},
              {"filter", f.key},
              {"from", round3(from)},
              {"to", round3(to)},
              {"bucket_ms", bucket},
              {"buckets", nb},
              {"matches", in_range},
              {"total", q->matches.size()},
              {"filtered", json{{"pkts", std::move(jpk)}, {"tx_bytes", std::move(jtx)}, {"rx_bytes", std::move(jrx)}}},
              {"conns", std::move(conns)},
              {"conn", std::move(conn)}};
  progress(*idx, q, out);
  return true;
}

}  // namespace btb::hci
