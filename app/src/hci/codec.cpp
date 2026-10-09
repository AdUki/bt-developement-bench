#include "codec.h"

#include <cstdio>

#include "wire.h"

namespace btb::hci {

namespace {

struct Bit {
  int bit;
  const char* str;
  uint32_t rate;  // for frequency tables: the rate this bit selects
};

// "44100/48000"; *rate is set only when exactly one rate bit is present (a configuration).
std::string bits(uint32_t value, const Bit* table, uint32_t* rate = nullptr) {
  std::string s;
  int n = 0;
  uint32_t r = 0;
  for (const Bit* b = table; b->str; ++b) {
    if (!(value & (1u << b->bit))) continue;
    if (!s.empty()) s += '/';
    s += b->str;
    r = b->rate;
    ++n;
  }
  if (rate) *rate = n == 1 ? r : 0;
  return s.empty() ? "?" : s;
}

const Bit kSbcFreq[] = {{7, "16000", 16000}, {6, "32000", 32000}, {5, "44100", 44100},
                        {4, "48000", 48000}, {0, nullptr, 0}};
const Bit kSbcMode[] = {{3, "mono", 0}, {2, "dual channel", 0}, {1, "stereo", 0},
                        {0, "joint stereo", 0}, {0, nullptr, 0}};
const Bit kSbcBlocks[] = {{7, "4", 0}, {6, "8", 0}, {5, "12", 0}, {4, "16", 0}, {0, nullptr, 0}};
const Bit kSbcSubbands[] = {{3, "4", 0}, {2, "8", 0}, {0, nullptr, 0}};
const Bit kSbcAlloc[] = {{1, "SNR", 0}, {0, "loudness", 0}, {0, nullptr, 0}};

const Bit kAacObject[] = {{7, "MPEG-2 AAC LC", 0}, {6, "MPEG-4 AAC LC", 0},
                          {5, "MPEG-4 AAC LTP", 0}, {4, "MPEG-4 AAC scalable", 0},
                          {0, nullptr, 0}};
const Bit kAacFreq[] = {{15, "8000", 8000},   {14, "11025", 11025}, {13, "12000", 12000},
                        {12, "16000", 16000}, {11, "22050", 22050}, {10, "24000", 24000},
                        {9, "32000", 32000},  {8, "44100", 44100},  {7, "48000", 48000},
                        {6, "64000", 64000},  {5, "88200", 88200},  {4, "96000", 96000},
                        {0, nullptr, 0}};
const Bit kAacChannels[] = {{3, "1 ch", 0}, {2, "2 ch", 0}, {0, nullptr, 0}};

const Bit kAptxFreq[] = {{7, "16000", 16000}, {6, "32000", 32000}, {5, "44100", 44100},
                         {4, "48000", 48000}, {0, nullptr, 0}};
const Bit kAptxMode[] = {{0, "mono", 0}, {1, "stereo", 0}, {0, nullptr, 0}};

const Bit kLdacFreq[] = {{5, "44100", 44100},   {4, "48000", 48000}, {3, "88200", 88200},
                         {2, "96000", 96000},   {1, "176400", 176400},
                         {0, "192000", 192000}, {0, nullptr, 0}};
const Bit kLdacMode[] = {{2, "mono", 0}, {1, "dual channel", 0}, {0, "stereo", 0},
                         {0, nullptr, 0}};

const Bit kOpusFreq[] = {{7, "48000", 48000}, {0, nullptr, 0}};
const Bit kOpusDuration[] = {{3, "10 ms", 0}, {4, "20 ms", 0}, {0, nullptr, 0}};
const Bit kOpusChannels[] = {{0, "mono", 0}, {1, "stereo", 0}, {2, "dual mono", 0},
                             {0, nullptr, 0}};

void decode_sbc(const uint8_t* p, size_t len, CodecInfo* c) {
  c->name = "SBC";
  c->sbc = true;
  if (len < 4) return;
  char bp[32];
  if (p[2] == p[3]) {
    std::snprintf(bp, sizeof(bp), "bitpool %u", p[3]);
  } else {
    std::snprintf(bp, sizeof(bp), "bitpool %u..%u", p[2], p[3]);
  }
  c->config = bits(p[0] & 0xf0, kSbcFreq, &c->rate) + " Hz, " + bits(p[0] & 0x0f, kSbcMode) +
              ", " + bits(p[1] & 0xf0, kSbcBlocks) + " blocks, " +
              bits(p[1] & 0x0c, kSbcSubbands) + " subbands, " + bits(p[1] & 0x03, kSbcAlloc) +
              ", " + bp;
}

void decode_aac(const uint8_t* p, size_t len, CodecInfo* c) {
  c->name = "AAC";
  if (len < 6) return;
  const uint32_t freq = (static_cast<uint32_t>(p[1]) << 8 | p[2]) & 0xfff0;
  const uint32_t bitrate = (static_cast<uint32_t>(p[3] & 0x7f) << 16) | (p[4] << 8) | p[5];
  c->config = bits(p[0], kAacObject) + ", " + bits(freq, kAacFreq, &c->rate) + " Hz, " +
              bits(p[2] & 0x0c, kAacChannels) + ", " + std::to_string(bitrate / 1000) +
              " kbps" + ((p[3] & 0x80) ? " VBR" : "");
}

void decode_vendor(const uint8_t* p, size_t len, CodecInfo* c) {
  if (len < 6) {
    c->name = "vendor";
    return;
  }
  const uint32_t vendor = le32(p);
  const uint16_t codec = le16(p + 4);
  const uint8_t* v = p + 6;
  const size_t vlen = len - 6;

  if (vendor == 0x4f && codec == 0x0001) {
    c->name = "aptX";
    c->rtp = false;
  } else if (vendor == 0xd7 && codec == 0x0024) {
    c->name = "aptX HD";
  } else if (vendor == 0x0a && codec == 0x0001) {
    c->name = "FastStream";
    c->rtp = false;
    return;
  } else if (vendor == 0x0a && codec == 0x0002) {
    c->name = "aptX Low Latency";
    c->rtp = false;
  } else if (vendor == 0x12d && codec == 0x00aa) {
    c->name = "LDAC";
    if (vlen >= 2) {
      c->config = bits(v[0] & 0x3f, kLdacFreq, &c->rate) + " Hz, " + bits(v[1] & 0x07, kLdacMode);
    }
    return;
  } else if (vendor == 0xe0 && codec == 0x0001) {
    c->name = "Opus (Google)";
    if (vlen >= 1) {
      c->config = bits(v[0] & 0x80, kOpusFreq, &c->rate) + " Hz, " +
                  bits(v[0] & 0x07, kOpusChannels) + ", " + bits(v[0] & 0x18, kOpusDuration);
    }
    return;
  } else if (vendor == 0x08a9 && codec == 0x0001) {
    c->name = "LC3plus";
    return;
  } else {
    char s[48];
    std::snprintf(s, sizeof(s), "vendor 0x%08x:0x%04x", vendor, codec);
    c->name = s;
    return;
  }

  // The aptX family shares one layout: frequency in the high nibble, channel mode in the low.
  if (vlen >= 1) {
    c->config = bits(v[0] & 0xf0, kAptxFreq, &c->rate) + " Hz, " + bits(v[0] & 0x0f, kAptxMode);
  }
}

}  // namespace

bool decode_media_codec(const uint8_t* p, size_t len, CodecInfo* out) {
  *out = CodecInfo{};
  if (len < 2) return false;
  const uint8_t type = p[1];
  p += 2;
  len -= 2;
  switch (type) {
    case 0x00:
      decode_sbc(p, len, out);
      break;
    case 0x01:
      out->name = "MPEG-1,2 Audio";
      break;
    case 0x02:
      decode_aac(p, len, out);
      break;
    case 0x04:
      out->name = "ATRAC";
      break;
    case 0xff:
      decode_vendor(p, len, out);
      break;
    default: {
      char s[24];
      std::snprintf(s, sizeof(s), "codec 0x%02x", type);
      out->name = s;
    }
  }
  return true;
}

const char* avdtp_signal_name(uint8_t id) {
  switch (id) {
    case 0x01: return "DISCOVER";
    case 0x02: return "GET_CAPABILITIES";
    case 0x03: return "SET_CONFIGURATION";
    case 0x04: return "GET_CONFIGURATION";
    case 0x05: return "RECONFIGURE";
    case 0x06: return "OPEN";
    case 0x07: return "START";
    case 0x08: return "CLOSE";
    case 0x09: return "SUSPEND";
    case 0x0a: return "ABORT";
    case 0x0b: return "SECURITY_CONTROL";
    case 0x0c: return "GET_ALL_CAPABILITIES";
    case 0x0d: return "DELAY_REPORT";
    default: return "UNKNOWN";
  }
}

}  // namespace btb::hci
