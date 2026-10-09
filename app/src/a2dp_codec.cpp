#include "a2dp_codec.h"

#include <cstdio>

#include "util/strings.h"

using json = nlohmann::json;

namespace btb {

namespace {

struct Bit {
  uint8_t mask;
  unsigned value;
};

struct NamedBit {
  uint8_t mask;
  const char* name;
};

std::vector<unsigned> bits_u(uint8_t v, std::initializer_list<Bit> table) {
  std::vector<unsigned> out;
  for (const Bit& b : table)
    if (v & b.mask) out.push_back(b.value);
  return out;
}

std::vector<std::string> bits_s(uint8_t v, std::initializer_list<NamedBit> table) {
  std::vector<std::string> out;
  for (const NamedBit& b : table)
    if (v & b.mask) out.push_back(b.name);
  return out;
}

std::string join(const std::vector<std::string>& v, const char* sep = "/") {
  std::string s;
  for (const std::string& x : v) s += (s.empty() ? "" : sep) + x;
  return s;
}

std::string join_u(const std::vector<unsigned>& v, const char* sep = "/") {
  std::vector<std::string> s;
  for (unsigned x : v) s.push_back(std::to_string(x));
  return join(s, sep);
}

uint32_t le32(const std::vector<uint8_t>& d, size_t at) {
  return d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}

uint16_t le16(const std::vector<uint8_t>& d, size_t at) {
  return static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
}

std::string hex32(uint32_t v) {
  char b[16];
  snprintf(b, sizeof(b), "0x%08x", v);
  return b;
}

std::string hex16(uint16_t v) {
  char b[8];
  snprintf(b, sizeof(b), "0x%04x", v);
  return b;
}

// The value when exactly one option is set, else the list: a configuration reads as numbers, a
// capability as the set it offers.
json one_or_list(const std::vector<unsigned>& v, bool caps) {
  if (!caps && v.size() == 1) return v[0];
  return v;
}

json one_or_list_s(const std::vector<std::string>& v, bool caps) {
  if (!caps && v.size() == 1) return v[0];
  return v;
}

json decode_sbc(const std::vector<uint8_t>& d, bool caps) {
  json j{{"name", "SBC"}};
  if (d.size() < 4) {
    j["error"] = "SBC element is 4 bytes";
    return j;
  }
  const auto rates = bits_u(d[0] >> 4, {{0x8, 16000}, {0x4, 32000}, {0x2, 44100}, {0x1, 48000}});
  const auto modes = bits_s(d[0] & 0xf, {{0x8, "mono"}, {0x4, "dual channel"}, {0x2, "stereo"},
                                         {0x1, "joint stereo"}});
  const auto blocks = bits_u(d[1] >> 4, {{0x8, 4}, {0x4, 8}, {0x2, 12}, {0x1, 16}});
  const auto subbands = bits_u((d[1] >> 2) & 0x3, {{0x2, 4}, {0x1, 8}});
  const auto alloc = bits_s(d[1] & 0x3, {{0x2, "SNR"}, {0x1, "loudness"}});
  j["rate"] = one_or_list(rates, caps);
  j["channel_mode"] = one_or_list_s(modes, caps);
  j["blocks"] = one_or_list(blocks, caps);
  j["subbands"] = one_or_list(subbands, caps);
  j["allocation"] = one_or_list_s(alloc, caps);
  j["min_bitpool"] = d[2];
  j["max_bitpool"] = d[3];
  std::string sum = join_u(rates) + " Hz, " + join(modes) + ", " + join_u(blocks) + " blocks, " +
                    join_u(subbands) + " subbands, " + join(alloc) + ", bitpool " +
                    std::to_string(d[2]) + "-" + std::to_string(d[3]);
  if (!caps && rates.size() == 1 && modes.size() == 1 && blocks.size() == 1 && subbands.size() == 1) {
    // The bitpool actually used is chosen by the source within [min, max], usually the max: the
    // bitrate quoted is the ceiling the configuration allows.
    const unsigned br = sbc_bitrate(rates[0], modes[0], blocks[0], subbands[0], d[3]);
    if (br) {
      j["max_bitrate"] = br;
      sum += ", ≤" + std::to_string((br + 500) / 1000) + " kbit/s";
    }
  }
  j["summary"] = sum;
  return j;
}

json decode_aac(const std::vector<uint8_t>& d, bool caps) {
  json j{{"name", "AAC"}};
  if (d.size() < 6) {
    j["error"] = "AAC element is 6 bytes";
    return j;
  }
  const auto types = bits_s(d[0], {{0x80, "MPEG-2 AAC LC"}, {0x40, "MPEG-4 AAC LC"},
                                   {0x20, "MPEG-4 AAC LTP"}, {0x10, "MPEG-4 AAC scalable"},
                                   {0x08, "MPEG-4 HE-AAC"}, {0x04, "MPEG-4 HE-AACv2"},
                                   {0x02, "MPEG-4 AAC-ELDv2"}});
  std::vector<unsigned> rates = bits_u(d[1], {{0x80, 8000}, {0x40, 11025}, {0x20, 12000},
                                              {0x10, 16000}, {0x08, 22050}, {0x04, 24000},
                                              {0x02, 32000}, {0x01, 44100}});
  for (unsigned r : bits_u(d[2] >> 4, {{0x8, 48000}, {0x4, 64000}, {0x2, 88200}, {0x1, 96000}}))
    rates.push_back(r);
  const auto ch = bits_u((d[2] >> 2) & 0x3, {{0x2, 1}, {0x1, 2}});
  const bool vbr = d[3] & 0x80;
  const unsigned bitrate = ((d[3] & 0x7f) << 16) | (d[4] << 8) | d[5];
  j["object_type"] = one_or_list_s(types, caps);
  j["rate"] = one_or_list(rates, caps);
  j["channels"] = one_or_list(ch, caps);
  j["vbr"] = vbr;
  j["bitrate"] = bitrate;
  j["summary"] = join(types) + ", " + join_u(rates) + " Hz, " + join_u(ch) + " ch, " +
                 (bitrate ? std::to_string(bitrate / 1000) + " kbit/s" : std::string("any bitrate")) +
                 (vbr ? ", VBR" : "");
  return j;
}

struct Vendor {
  uint32_t vendor;
  uint16_t codec;
  const char* name;
};

// a2dp-codecs.h and PipeWire's a2dp-codec-caps.h.
constexpr Vendor kVendors[] = {
    {0x0000004f, 0x0001, "aptX"},
    {0x000000d7, 0x0024, "aptX HD"},
    {0x0000000a, 0x0002, "aptX LL"},
    {0x0000000a, 0x0001, "FastStream"},
    {0x0000012d, 0x00aa, "LDAC"},
    {0x000000e0, 0x0001, "Opus (Google)"},
    {0x000005f1, 0x1005, "Opus (PipeWire)"},
    {0x000008a9, 0x0001, "LC3plus HR"},
    {0x0000003a, 0x0001, "aptX Adaptive"},
    {0x000000d7, 0x00ad, "aptX Adaptive"},
};

const Vendor* find_vendor(uint32_t v, uint16_t c) {
  for (const Vendor& x : kVendors)
    if (x.vendor == v && x.codec == c) return &x;
  return nullptr;
}

json decode_vendor(const std::vector<uint8_t>& d, bool caps) {
  if (d.size() < 6) return json{{"name", "vendor"}, {"error", "vendor element is at least 6 bytes"},
                                {"summary", "vendor (short element)"}};
  const uint32_t vid = le32(d, 0);
  const uint16_t cid = le16(d, 4);
  const Vendor* v = find_vendor(vid, cid);
  json j{{"name", v ? v->name : "vendor " + hex32(vid) + ":" + hex16(cid)},
         {"vendor_id", hex32(vid)},
         {"codec_id", hex16(cid)}};
  const std::string name = j["name"];
  if (v && (name == "aptX" || name == "aptX HD") && d.size() >= 7) {
    const auto rates = bits_u(d[6] >> 4, {{0x8, 16000}, {0x4, 32000}, {0x2, 44100}, {0x1, 48000}});
    const auto modes = bits_s(d[6] & 0xf, {{0x1, "mono"}, {0x2, "stereo"}});
    j["rate"] = one_or_list(rates, caps);
    j["channel_mode"] = one_or_list_s(modes, caps);
    j["summary"] = name + ", " + join_u(rates) + " Hz, " + join(modes);
    return j;
  }
  if (v && name == "LDAC" && d.size() >= 8) {
    const auto rates = bits_u(d[6], {{0x20, 44100}, {0x10, 48000}, {0x08, 88200}, {0x04, 96000},
                                     {0x02, 176400}, {0x01, 192000}});
    const auto modes = bits_s(d[7], {{0x04, "mono"}, {0x02, "dual channel"}, {0x01, "stereo"}});
    j["rate"] = one_or_list(rates, caps);
    j["channel_mode"] = one_or_list_s(modes, caps);
    j["summary"] = name + ", " + join_u(rates) + " Hz, " + join(modes);
    return j;
  }
  j["summary"] = name;
  return j;
}

// LC3 codec-specific configuration, as BAP carries it: length-type-value entries.
json decode_lc3(const std::vector<uint8_t>& d, bool caps) {
  json j{{"name", "LC3"}};
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < d.size()) {
    const uint8_t len = d[i];
    if (len == 0 || i + 1 + len > d.size()) {
      j["error"] = "truncated LTV";
      break;
    }
    const uint8_t type = d[i + 1];
    const std::vector<uint8_t> v(d.begin() + static_cast<long>(i) + 2,
                                 d.begin() + static_cast<long>(i) + 1 + len);
    if (caps) {
      // Capability LTVs are bitmasks and ranges; worth naming but not worth a second decoder here.
      static const char* const kCapNames[] = {"", "frequencies", "durations", "channel_counts",
                                              "octets_per_frame", "max_frames_per_sdu"};
      j[type < 6 ? kCapNames[type] : ("type_" + std::to_string(type)).c_str()] = to_hex(v);
    } else if (type == 0x01 && v.size() == 1) {
      static const unsigned kRates[] = {0, 8000, 11025, 16000, 22050, 24000, 32000,
                                        44100, 48000, 88200, 96000, 176400, 192000, 384000};
      const unsigned r = v[0] < 14 ? kRates[v[0]] : 0;
      j["rate"] = r;
      parts.push_back(std::to_string(r) + " Hz");
    } else if (type == 0x02 && v.size() == 1) {
      const char* dur = v[0] == 0 ? "7.5 ms" : v[0] == 1 ? "10 ms" : "?";
      j["frame_duration"] = dur;
      parts.push_back(dur);
    } else if (type == 0x03 && v.size() == 4) {
      j["channel_allocation"] = hex32(le32(v, 0));
      parts.push_back("allocation " + hex32(le32(v, 0)));
    } else if (type == 0x04 && v.size() == 2) {
      j["octets_per_frame"] = le16(v, 0);
      parts.push_back(std::to_string(le16(v, 0)) + " octets/frame");
    } else if (type == 0x05 && v.size() == 1) {
      j["frames_per_sdu"] = v[0];
      parts.push_back(std::to_string(v[0]) + " frames/SDU");
    } else {
      j["type_" + std::to_string(type)] = to_hex(v);
    }
    i += 1 + len;
  }
  if (!caps && j.contains("rate") && j.contains("frame_duration") && j.contains("octets_per_frame")) {
    const double frame_ms = j["frame_duration"] == "7.5 ms" ? 7.5 : 10.0;
    const unsigned br = static_cast<unsigned>(8.0 * j["octets_per_frame"].get<unsigned>() * 1000.0 / frame_ms);
    j["bitrate"] = br;
    parts.push_back(std::to_string(br / 1000) + " kbit/s per channel");
  }
  j["summary"] = parts.empty() ? std::string("LC3") : "LC3, " + join(parts, ", ");
  return j;
}

}  // namespace

unsigned sbc_bitrate(unsigned rate, const std::string& mode, unsigned blocks, unsigned subbands,
                     unsigned bitpool) {
  if (!rate || !blocks || !subbands) return 0;
  const unsigned channels = mode == "mono" ? 1 : 2;
  unsigned len = 4 + (4 * subbands * channels) / 8;
  if (mode == "mono" || mode == "dual channel") {
    len += (blocks * channels * bitpool + 7) / 8;
  } else if (mode == "stereo") {
    len += (blocks * bitpool + 7) / 8;
  } else if (mode == "joint stereo") {
    len += (subbands + blocks * bitpool + 7) / 8;
  } else {
    return 0;
  }
  return static_cast<unsigned>(8ull * len * rate / (subbands * blocks));
}

json decode_codec(uint8_t codec, const std::vector<uint8_t>& data, bool caps) {
  json j;
  switch (codec) {
    case 0x00: j = decode_sbc(data, caps); break;
    case 0x01: j = json{{"name", "MPEG-1,2 Audio"}, {"summary", "MPEG-1,2 Audio"}}; break;
    case 0x02: j = decode_aac(data, caps); break;
    case 0x04: j = json{{"name", "ATRAC"}, {"summary", "ATRAC"}}; break;
    case 0x06: j = decode_lc3(data, caps); break;
    case 0xff: j = decode_vendor(data, caps); break;
    default: {
      char b[8];
      snprintf(b, sizeof(b), "0x%02x", codec);
      j = json{{"name", std::string("codec ") + b}, {"summary", std::string("codec ") + b}};
    }
  }
  if (!j.contains("summary")) j["summary"] = j.value("name", std::string("?"));
  return j;
}

std::string codec_name(uint8_t codec, const std::vector<uint8_t>& data) {
  return decode_codec(codec, data, true).value("name", std::string("?"));
}

}  // namespace btb
