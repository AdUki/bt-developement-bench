#include "iso_stream.h"

#include <algorithm>
#include <cstdio>

#include "wire.h"

namespace btb::hci {

namespace {

constexpr uint16_t kNone = 0xffff;

std::string frame_from(const char* what, uint32_t n) {
  char b[64];
  if (n) std::snprintf(b, sizeof b, "%s, frame %u", what, n);
  else std::snprintf(b, sizeof b, "%s, the file's context", what);
  return b;
}

// LE Setup ISO Data Path's path and codec, as the detail shows them: "HCI, transparent".
std::string data_path(uint8_t id, const uint8_t* codec) {
  static const char* const formats[] = {"u-law", "A-law", "CVSD", "transparent", "linear PCM", "mSBC", "LC3", "G.729A"};
  char b[64];
  const char* f = codec[0] < 8 ? formats[codec[0]] : nullptr;
  if (codec[0] == 0xff) std::snprintf(b, sizeof b, "vendor 0x%04x:0x%04x", le16(codec + 1), le16(codec + 3));
  else if (f) std::snprintf(b, sizeof b, "%s", f);
  else std::snprintf(b, sizeof b, "coding format 0x%02x", codec[0]);
  if (id == 0) return std::string("HCI, ") + b;
  char p[96];
  std::snprintf(p, sizeof p, "vendor path 0x%02x, %s", id, b);
  return p;
}

// The codec configuration LTVs of BAP (Assigned Numbers 6.12.5): a later one overrides an
// earlier one, as a BIS's level 3 overrides its subgroup's level 2.
void apply_ltvs(const std::vector<uint8_t>& v, IsoStream* s) {
  for (size_t at = 0; at < v.size();) {
    const size_t l = v[at];
    if (!l) {
      ++at;
      continue;
    }
    if (at + 1 + l > v.size()) break;
    const uint8_t t = v[at + 1];
    const uint8_t* p = v.data() + at + 2;
    const size_t n = l - 1;
    if (t == 0x01 && n >= 1) s->freq = p[0];
    if (t == 0x02 && n >= 1) s->duration = p[0];
    if (t == 0x03 && n >= 4) s->have_alloc = true, s->alloc = le32(p);
    if (t == 0x04 && n >= 2) s->octets = le16(p);
    if (t == 0x05 && n >= 1) s->blocks = p[0] ? p[0] : 1;
    at += 1 + l;
  }
}

}  // namespace

unsigned IsoStream::channels() const {
  if (!have_alloc || !alloc) return 1;
  return static_cast<unsigned>(__builtin_popcount(alloc));
}

bool find_base(const uint8_t* ad, size_t n, const uint8_t** base, size_t* base_len) {
  for (size_t at = 0; at < n;) {
    const size_t l = ad[at];
    if (!l) break;
    if (at + 1 + l > n) break;
    // Service Data - 16-bit UUID, the Basic Audio Announcement Service (0x1851).
    if (ad[at + 1] == 0x16 && l >= 3 && le16(ad + at + 2) == 0x1851) {
      *base = ad + at + 4;
      *base_len = l - 3;
      return true;
    }
    at += 1 + l;
  }
  return false;
}

bool IsoTracker::setup_packet(uint16_t mon, uint8_t evt, uint8_t sub, uint16_t opcode) {
  if (mon == kMonCommand) {
    switch (opcode) {
      case 0x203f:  // LE Set Periodic Advertising Data: the BASE a broadcaster sends
      case 0x2062:  // LE Set CIG Parameters (and its test form): the CIS IDs
      case 0x2063:
      case 0x2064:  // LE Create CIS: each CIS's ACL link
      case 0x2068:  // LE Create BIG (and its test form): the BIG's advertising set
      case 0x2069:
      case 0x206b:  // LE BIG Create Sync: the sync handle and the BIS indices
      case 0x206e:  // LE Setup ISO Data Path
        return true;
      default:
        return false;
    }
  }
  if (mon != kMonEvent) return false;
  if (evt == kEvtCmdComplete) return opcode == 0x2062 || opcode == 0x2063;  // the CIS handles
  return evt == kEvtLeMeta && (sub == kLeCisRequest || sub == kLeBigComplete || sub == kLeBigSyncEstablished);
}

bool IsoTracker::report_packet(uint16_t mon, uint8_t evt, uint8_t sub) {
  return mon == kMonEvent && evt == kEvtLeMeta && (sub == 0x0f || sub == 0x25);
}

void IsoTracker::packet(uint16_t index, uint16_t mon, const uint8_t* d, size_t len, uint32_t n) {
  Controller& c = ctl_[index];
  if (mon == kMonCommand) {
    if (len < 3) return;
    const uint16_t op = le16(d);
    const uint8_t* p = d + 3;
    const size_t m = std::min<size_t>(d[2], len - 3);
    switch (op) {
      case 0x203f: {  // advertising handle, operation, length, data
        if (m < 3) return;
        const uint8_t adv = p[0], operation = p[1];
        const size_t dl = std::min<size_t>(p[2], m - 3);
        std::vector<uint8_t>& acc = c.adv_data[adv];
        if (operation == 1 || operation == 3) acc.clear();  // first fragment, complete data
        if (operation <= 3) acc.insert(acc.end(), p + 3, p + 3 + dl);
        if (operation == 2 || operation == 3) {
          const uint8_t* b = nullptr;
          size_t bl = 0;
          if (find_base(acc.data(), acc.size(), &b, &bl)) c.adv_base[adv] = Base{{b, b + bl}, frame_from("BASE", n)};
          acc.clear();
        }
        return;
      }
      case 0x2062:
      case 0x2063: {  // CIG ID, ..., CIS count at 14, then 9 (test: 14) bytes per CIS led by its ID
        if (m < 15) return;
        const size_t step = op == 0x2062 ? 9 : 14;
        std::vector<uint8_t>& ids = c.cig_cis[p[0]];
        ids.clear();
        for (size_t i = 0; i < p[14] && 15 + step * (i + 1) <= m; ++i) ids.push_back(p[15 + step * i]);
        return;
      }
      case 0x2064:  // count, then CIS handle and ACL handle each
        if (m < 1) return;
        for (size_t i = 0; i < p[0] && 1 + 4 * (i + 1) <= m; ++i) {
          c.links[le16(p + 1 + 4 * i) & 0x0fff].acl = le16(p + 3 + 4 * i) & 0x0fff;
        }
        return;
      case 0x2068:
      case 0x2069:  // BIG handle, advertising handle
        if (m >= 2) c.bigs[p[0]] = Big{p[1], kNone, {}};
        return;
      case 0x206b: {  // BIG handle, sync handle, encryption, code, MSE, timeout, count, BIS indices
        if (m < 24) return;
        Big b{kNone, static_cast<uint16_t>(le16(p + 1) & 0x0fff), {}};
        for (size_t i = 0; i < p[23] && 24 + i < m; ++i) b.bis.push_back(p[24 + i]);
        c.bigs[p[0]] = b;
        return;
      }
      case 0x206e:  // handle, direction, path ID, codec ID, ...
        if (m >= 9) c.links[le16(p) & 0x0fff].path[p[2] & 1] = data_path(p[3], p + 4);
        return;
      default:
        return;
    }
  }
  if (mon != kMonEvent || len < 2) return;
  const uint8_t* p = d + 2;
  const size_t m = std::min<size_t>(d[1], len - 2);
  if (d[0] == kEvtCmdComplete) {
    // ncmd, opcode, status, CIG ID, count, CIS handles: in the order of the command's CIS IDs.
    if (m < 6 || (le16(p + 1) != 0x2062 && le16(p + 1) != 0x2063) || p[3]) return;
    const uint8_t cig = p[4];
    const std::vector<uint8_t>& ids = c.cig_cis[cig];
    for (size_t i = 0; i < p[5] && 6 + 2 * (i + 1) <= m; ++i) {
      Link l;
      l.named = true;
      l.group = cig;
      l.member = i < ids.size() ? ids[i] : static_cast<uint8_t>(i);
      c.links[le16(p + 6 + 2 * i) & 0x0fff] = l;
    }
    return;
  }
  if (d[0] != kEvtLeMeta || m < 2) return;
  const uint8_t sub = p[0];
  const uint8_t* q = p + 1;
  const size_t k = m - 1;
  switch (sub) {
    case kLeCisRequest:  // ACL handle, CIS handle, CIG ID, CIS ID
      if (k >= 6) {
        Link l;
        l.named = true;
        l.group = q[4];
        l.member = q[5];
        l.acl = le16(q) & 0x0fff;
        c.links[le16(q + 2) & 0x0fff] = l;
      }
      return;
    case kLeBigComplete:
    case kLeBigSyncEstablished: {
      // status, BIG handle, ..., count at 17 (sync: 13), BIS handles: BIS 1, 2, ... of a created
      // BIG; of a synchronized one the indices BIG Create Sync asked for, in its order.
      const size_t num_at = sub == kLeBigComplete ? 17 : 13;
      if (k <= num_at || q[0]) return;
      const auto bit = c.bigs.find(q[1]);
      const Big big = bit == c.bigs.end() ? Big{} : bit->second;
      for (size_t i = 0; i < q[num_at] && num_at + 1 + 2 * (i + 1) <= k; ++i) {
        Link l;
        l.named = true;
        l.bis = true;
        l.group = q[1];
        l.member = sub == kLeBigSyncEstablished && i < big.bis.size() ? big.bis[i] : static_cast<uint8_t>(i + 1);
        l.adv = sub == kLeBigComplete ? big.adv : kNone;
        l.sync = sub == kLeBigSyncEstablished ? big.sync : kNone;
        c.links[le16(q + num_at + 1 + 2 * i) & 0x0fff] = l;
      }
      return;
    }
    case 0x0f:
    case 0x25:
      base_report(index, d, len, n);
      return;
    default:
      return;
  }
}

void IsoTracker::base_report(uint16_t index, const uint8_t* d, size_t len, uint32_t n) {
  if (len < 3 || d[0] != kEvtLeMeta || d[1] < 1) return;
  const uint8_t* q = d + 3;
  const size_t k = std::min<size_t>(d[1], len - 2) - 1;
  // Sync handle, TX power, RSSI, CTE type, (v2: event counter, subevent,) data status, length.
  const size_t at = d[2] == 0x25 ? 10 : 7;
  if (k < at || q[at - 2] != 0) return;  // incomplete data: the BASE may be cut
  const uint8_t* b = nullptr;
  size_t bl = 0;
  if (!find_base(q + at, std::min<size_t>(q[at - 1], k - at), &b, &bl)) return;
  ctl_[index].sync_base[le16(q) & 0x0fff] = Base{{b, b + bl}, frame_from("BASE", n)};
}

void IsoTracker::ase_state(uint16_t index, uint16_t acl, bool local_server, uint16_t uuid, const uint8_t* v, size_t n,
                           uint32_t frame) {
  if (n < 2) return;
  Ase& a = ctl_[index].ases[static_cast<uint32_t>(local_server) << 24 | static_cast<uint32_t>(acl) << 8 | v[0]];
  if (uuid) a.uuid = uuid;
  a.local_server = local_server;
  switch (v[1]) {
    case 1:  // Codec Configured: ..., codec ID at 19, configuration length at 24
      if (n >= 25 && n >= 25u + v[24]) {
        a.config.have = true;
        std::copy(v + 19, v + 24, a.config.codec);
        a.config.ltvs.assign(v + 25, v + 25 + v[24]);
        char what[32];
        std::snprintf(what, sizeof what, "ASE %u", v[0]);
        a.config.from = frame_from(what, frame);
      }
      a.cig = a.cis = -1;
      break;
    case 2:
    case 3:
    case 4:
    case 5:  // QoS Configured, Enabling, Streaming, Disabling: CIG ID, CIS ID
      if (n >= 4) a.cig = v[2], a.cis = v[3];
      break;
    default:
      break;
  }
}

bool IsoTracker::base_config(const std::vector<uint8_t>& base, uint8_t bis_index, Config* out) {
  const uint8_t* p = base.data();
  const size_t n = base.size();
  if (n < 4) return false;
  size_t at = 4;  // presentation delay, subgroup count
  for (unsigned g = 0; g < p[3]; ++g) {
    if (at + 7 > n) return false;
    const uint8_t k = p[at];
    const uint8_t* codec = p + at + 1;
    const size_t cl = p[at + 6];
    if (at + 7 + cl + 1 > n) return false;
    const size_t cfg = at + 7;
    at = cfg + cl;
    at += 1 + p[at];  // metadata
    for (unsigned b = 0; b < k; ++b) {
      if (at + 2 > n || at + 2 + p[at + 1] > n) return false;
      if (p[at] == bis_index) {
        out->have = true;
        std::copy(codec, codec + 5, out->codec);
        out->ltvs.assign(p + cfg, p + cfg + cl);
        out->ltvs.insert(out->ltvs.end(), p + at + 2, p + at + 2 + p[at + 1]);
        return true;
      }
      at += 2 + p[at + 1];
    }
  }
  return false;
}

uint16_t IsoTracker::acl_of(uint16_t index, uint16_t handle) const {
  const auto c = ctl_.find(index);
  if (c == ctl_.end()) return kNoAcl;
  const auto l = c->second.links.find(handle);
  return l == c->second.links.end() || l->second.bis ? kNoAcl : l->second.acl;
}

uint16_t IsoTracker::sync_of(uint16_t index, uint16_t handle) const {
  const auto c = ctl_.find(index);
  if (c == ctl_.end()) return kNone;
  const auto l = c->second.links.find(handle);
  return l == c->second.links.end() || !l->second.bis ? kNone : l->second.sync;
}

IsoStream IsoTracker::stream(uint16_t index, uint16_t handle, bool tx, size_t sdu_len) const {
  IsoStream s;
  const auto cit = ctl_.find(index);
  if (cit == ctl_.end()) return s;
  const Controller& c = cit->second;
  const auto lit = c.links.find(handle);
  if (lit == c.links.end()) return s;
  const Link& l = lit->second;
  // A link whose only setup seen is its data path or its ACL (the BIG or CIG was set up before the
  // file and its context) stays unnamed.
  if (l.named) {
    char b[64];
    std::snprintf(b, sizeof b, "%s %u of %s %u", l.bis ? "BIS" : "CIS", l.member, l.bis ? "BIG" : "CIG", l.group);
    s.link = b;
  }
  s.path = l.path[tx ? 0 : 1];

  Config cfg;
  if (l.bis) {
    const auto& bases = l.adv != kNone ? c.adv_base : c.sync_base;
    const auto bit = bases.find(l.adv != kNone ? l.adv : l.sync);
    if (bit != bases.end() && base_config(bit->second.bytes, l.member, &cfg)) cfg.from = bit->second.from;
  } else {
    // The ASEs on this CIS. The data this side sends goes to the peer's sink ASE, or comes from
    // this side's source ASE; one whose direction discovery did not name is matched by size.
    const Ase* pick = nullptr;
    int best = 0;
    for (const auto& [key, a] : c.ases) {
      if (!a.config.have || a.cig != l.group || a.cis != l.member) continue;
      if (l.acl != kNoAcl && ((key >> 8) & 0xffff) != l.acl) continue;
      int score = 1;
      if (a.uuid) {
        if ((a.uuid == 0x2bc4) != (tx != a.local_server)) continue;
        score = 3;
      } else {
        IsoStream t;
        apply_ltvs(a.config.ltvs, &t);
        if (t.sdu_bytes() == sdu_len) score = 2;
      }
      if (score > best) best = score, pick = &a;
    }
    if (pick) cfg = pick->config;
  }
  if (cfg.have) {
    s.have_codec = true;
    std::copy(cfg.codec, cfg.codec + 5, s.codec);
    s.config_from = cfg.from;
    apply_ltvs(cfg.ltvs, &s);
  }
  return s;
}

}  // namespace btb::hci
