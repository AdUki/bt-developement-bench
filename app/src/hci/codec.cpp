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

// A2DP 1.4 added HE-AAC, HE-AACv2, AAC-ELDv2 and DRC, and 6 and 8 channels.
const Bit kAacObject[] = {{7, "MPEG-2 AAC LC", 0}, {6, "MPEG-4 AAC LC", 0},
                          {5, "MPEG-4 AAC LTP", 0}, {4, "MPEG-4 AAC scalable", 0},
                          {3, "MPEG-4 HE-AAC", 0}, {2, "MPEG-4 HE-AACv2", 0},
                          {1, "MPEG-4 AAC-ELDv2", 0}, {0, "DRC", 0}, {0, nullptr, 0}};
const Bit kAacFreq[] = {{15, "8000", 8000},   {14, "11025", 11025}, {13, "12000", 12000},
                        {12, "16000", 16000}, {11, "22050", 22050}, {10, "24000", 24000},
                        {9, "32000", 32000},  {8, "44100", 44100},  {7, "48000", 48000},
                        {6, "64000", 64000},  {5, "88200", 88200},  {4, "96000", 96000},
                        {0, nullptr, 0}};
const Bit kAacChannels[] = {{3, "1 ch", 0}, {2, "2 ch", 0}, {1, "6 ch", 0}, {0, "8 ch", 0},
                            {0, nullptr, 0}};

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

// PipeWire's Opus (a2dp-codec-caps.h, a2dp_opus_05_t): frame durations of each direction.
const Bit kOpus05Duration[] = {{0, "2.5", 0}, {1, "5", 0}, {2, "10", 0}, {3, "20", 0},
                               {4, "40", 0}, {0, nullptr, 0}};

const Bit kMpegLayer[] = {{7, "I", 0}, {6, "II", 0}, {5, "III", 0}, {0, nullptr, 0}};
const Bit kMpegFreq[] = {{5, "16000", 16000}, {4, "22050", 22050}, {3, "24000", 24000},
                         {2, "32000", 32000}, {1, "44100", 44100}, {0, "48000", 48000},
                         {0, nullptr, 0}};

const Bit kFastStreamDir[] = {{0, "sink (music)", 0}, {1, "source (voice)", 0}, {0, nullptr, 0}};
const Bit kFastStreamSink[] = {{1, "44100", 44100}, {0, "48000", 48000}, {0, nullptr, 0}};
const Bit kFastStreamSource[] = {{5, "16000", 0}, {0, nullptr, 0}};

const Bit kLc3plusDuration[] = {{6, "10 ms", 0}, {5, "5 ms", 0}, {4, "2.5 ms", 0}, {0, nullptr, 0}};
const Bit kLc3plusChannels[] = {{7, "mono", 0}, {6, "stereo", 0}, {0, nullptr, 0}};
const Bit kLc3plusFreq[] = {{8, "48000", 48000}, {7, "96000", 96000}, {0, nullptr, 0}};

std::string sf(const char* fmt, unsigned v) {
  char b[48];
  std::snprintf(b, sizeof(b), fmt, v);
  return b;
}

std::string hexs(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; ++i) s += sf("%02x", p[i]);
  return s;
}

void field(CodecInfo* c, const char* name, const std::string& value) { c->fields.emplace_back(name, value); }

// The SBC frame length the encoder produces (A2DP 12.9), as a bitrate: the ceiling a configuration
// allows at its maximum bitpool. 0 unless exactly one option of each field is set.
unsigned sbc_max_bitrate(const uint8_t* p) {
  static const unsigned rates[] = {48000, 44100, 32000, 16000};  // bits 4..7
  unsigned rate = 0, blocks = 0, subbands = 0, nr = 0, nb = 0;
  for (int b = 0; b < 4; ++b) {
    if (p[0] & (0x10 << b)) rate = rates[b], ++nr;
    if (p[1] & (0x10 << b)) blocks = 16 - 4 * b, ++nb;
  }
  if (nr != 1 || nb != 1) return 0;
  if ((p[1] & 0x0c) == 0x08) subbands = 4;
  if ((p[1] & 0x0c) == 0x04) subbands = 8;
  const uint8_t mode = p[0] & 0x0f;
  if (!subbands || (mode & (mode - 1)) || !mode) return 0;
  const unsigned ch = mode == 0x08 ? 1 : 2, bitpool = p[3];
  unsigned len = 4 + (4 * subbands * ch) / 8;
  if (mode == 0x08 || mode == 0x04) len += (blocks * ch * bitpool + 7) / 8;  // mono, dual
  else if (mode == 0x02) len += (blocks * bitpool + 7) / 8;                   // stereo
  else len += (subbands + blocks * bitpool + 7) / 8;                         // joint stereo
  return static_cast<unsigned>(8ull * len * rate / (subbands * blocks));
}

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
  field(c, "Sampling frequency", bits(p[0] & 0xf0, kSbcFreq) + " Hz");
  field(c, "Channel mode", bits(p[0] & 0x0f, kSbcMode));
  field(c, "Block length", bits(p[1] & 0xf0, kSbcBlocks));
  field(c, "Subbands", bits(p[1] & 0x0c, kSbcSubbands));
  field(c, "Allocation method", bits(p[1] & 0x03, kSbcAlloc));
  field(c, "Minimum bitpool", std::to_string(p[2]));
  field(c, "Maximum bitpool", std::to_string(p[3]));
  if (const unsigned br = sbc_max_bitrate(p)) field(c, "Bitrate at the maximum bitpool", sf("%u bit/s", br));
}

void decode_aac(const uint8_t* p, size_t len, CodecInfo* c) {
  c->name = "AAC";
  if (len < 6) return;
  const uint32_t freq = (static_cast<uint32_t>(p[1]) << 8 | p[2]) & 0xfff0;
  const uint32_t bitrate = (static_cast<uint32_t>(p[3] & 0x7f) << 16) | (p[4] << 8) | p[5];
  const std::string br = bitrate ? std::to_string(bitrate / 1000) + " kbps" : "any bitrate";
  c->config = bits(p[0], kAacObject) + ", " + bits(freq, kAacFreq, &c->rate) + " Hz, " +
              bits(p[2] & 0x0f, kAacChannels) + ", " + br + ((p[3] & 0x80) ? " VBR" : "");
  field(c, "Object type", sf("0x%02x: ", p[0]) + bits(p[0], kAacObject));
  field(c, "Sampling frequency", bits(freq, kAacFreq) + " Hz");
  field(c, "Channels", bits(p[2] & 0x0f, kAacChannels));
  field(c, "VBR", (p[3] & 0x80) ? "supported" : "no");
  field(c, "Bitrate", bitrate ? std::to_string(bitrate) + " bit/s" : "0 (not specified)");
}

void decode_mpeg(const uint8_t* p, size_t len, CodecInfo* c) {
  c->name = "MPEG-1,2 Audio";
  if (len < 4) return;
  c->config = "layer " + bits(p[0] & 0xe0, kMpegLayer) + ", " + bits(p[1] & 0x3f, kMpegFreq, &c->rate) +
              " Hz, " + bits(p[0] & 0x0f, kSbcMode);
  field(c, "Layer", bits(p[0] & 0xe0, kMpegLayer));
  field(c, "CRC protection", (p[0] & 0x10) ? "yes" : "no");
  field(c, "Channel mode", bits(p[0] & 0x0f, kSbcMode));
  field(c, "Sampling frequency", bits(p[1] & 0x3f, kMpegFreq) + " Hz");
  field(c, "VBR", (p[2] & 0x80) ? "supported" : "no");
  field(c, "Bitrate index", sf("0x%04x", be16(p + 2) & 0x7fff));
}

const char* vendor_company(uint32_t v) { return v <= 0xffff ? company_name(static_cast<uint16_t>(v)) : nullptr; }

// One direction of PipeWire's Opus: channels, coupled streams, audio location, frame durations,
// maximum bitrate in units of 1024 bit/s (0: any).
std::string opus05_direction(const uint8_t* v, CodecInfo* c, const char* prefix) {
  const uint32_t loc = le32(v + 2);
  const unsigned br = le16(v + 7);
  const std::string p = prefix;
  field(c, (p + "Channels").c_str(), std::to_string(v[0]));
  field(c, (p + "Coupled streams").c_str(), std::to_string(v[1]));
  field(c, (p + "Audio location").c_str(),
        sf("0x%08x", loc) + (loc == 0x0fffffff ? " (any)" : loc == 3 ? " (front left, front right)" : loc == 1 ? " (front left)" : ""));
  field(c, (p + "Frame duration").c_str(), bits(v[6] & 0x1f, kOpus05Duration) + " ms");
  field(c, (p + "Maximum bitrate").c_str(), br ? sf("%u bit/s", br * 1024) : std::string("any"));
  return sf("%u ch, ", v[0]) + bits(v[6] & 0x1f, kOpus05Duration) + " ms, " +
         (br ? sf("%u kbit/s", (br * 1024 + 500) / 1000) : std::string("any bitrate"));
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
  const char* company = vendor_company(vendor);
  field(c, "Vendor ID", sf("0x%08x", vendor) + (company ? std::string(" (") + company + ")" : ""));
  field(c, "Vendor codec ID", sf("0x%04x", codec));

  if (vendor == 0x4f && codec == 0x0001) {
    c->name = "aptX";
    c->rtp = false;
  } else if (vendor == 0xd7 && codec == 0x0024) {
    c->name = "aptX HD";
  } else if (vendor == 0x0a && codec == 0x0001) {
    c->name = "FastStream";
    c->rtp = false;
    if (vlen >= 2) {
      field(c, "Direction", bits(v[0] & 0x03, kFastStreamDir));
      field(c, "Sink frequency", bits(v[1] & 0x03, kFastStreamSink, &c->rate) + " Hz");
      field(c, "Source frequency", bits(v[1] & 0x20, kFastStreamSource) + " Hz");
      c->config = bits(v[0] & 0x03, kFastStreamDir) + ", " + bits(v[1] & 0x03, kFastStreamSink) + " Hz";
      if (v[0] & 0x02) c->config += ", voice " + bits(v[1] & 0x20, kFastStreamSource) + " Hz";
    }
    return;
  } else if (vendor == 0x0a && codec == 0x0002) {
    c->name = "aptX Low Latency";
    c->rtp = false;
  } else if (vendor == 0xd7 && codec == 0x00ad) {
    c->name = "aptX Adaptive";
    if (vlen) field(c, "Codec information", hexs(v, vlen));
    return;
  } else if (vendor == 0x12d && codec == 0x00aa) {
    c->name = "LDAC";
    if (vlen >= 2) {
      c->config = bits(v[0] & 0x3f, kLdacFreq, &c->rate) + " Hz, " + bits(v[1] & 0x07, kLdacMode);
      field(c, "Sampling frequency", bits(v[0] & 0x3f, kLdacFreq) + " Hz");
      field(c, "Channel mode", bits(v[1] & 0x07, kLdacMode));
    }
    return;
  } else if (vendor == 0xe0 && codec == 0x0001) {
    c->name = "Opus (Google)";
    if (vlen >= 1) {
      c->config = bits(v[0] & 0x80, kOpusFreq, &c->rate) + " Hz, " +
                  bits(v[0] & 0x07, kOpusChannels) + ", " + bits(v[0] & 0x18, kOpusDuration);
      field(c, "Sampling frequency", bits(v[0] & 0x80, kOpusFreq) + " Hz");
      field(c, "Channel mode", bits(v[0] & 0x07, kOpusChannels));
      field(c, "Frame duration", bits(v[0] & 0x18, kOpusDuration));
    }
    return;
  } else if (vendor == 0x05f1 && codec == 0x1005) {
    // a2dp_opus_05_t: the main direction, then the bidirectional (return) one; no channels in
    // the return direction means it is not used. Always 48 kHz.
    c->name = "Opus (PipeWire)";
    c->rate = 48000;
    if (vlen >= 9) {
      c->config = opus05_direction(v, c, "");
      if (vlen >= 18 && v[9]) c->config += ", return " + opus05_direction(v + 9, c, "Return: ");
    }
    return;
  } else if (vendor == 0x08a9 && codec == 0x0001) {
    c->name = "LC3plus HR";
    if (vlen >= 4) {
      const uint16_t freq = be16(v + 2);
      c->config = bits(freq, kLc3plusFreq, &c->rate) + " Hz, " + bits(v[1], kLc3plusChannels) + ", " +
                  bits(v[0], kLc3plusDuration);
      field(c, "Frame duration", bits(v[0], kLc3plusDuration));
      field(c, "Channels", bits(v[1], kLc3plusChannels));
      field(c, "Sampling frequency", bits(freq, kLc3plusFreq) + " Hz");
    }
    return;
  } else {
    char s[64];
    std::snprintf(s, sizeof(s), "vendor 0x%08x:0x%04x", vendor, codec);
    c->name = s;
    if (vlen) field(c, "Codec information", hexs(v, vlen));
    return;
  }

  // The aptX family shares one layout: frequency in the high nibble, channel mode in the low.
  if (vlen >= 1) {
    c->config = bits(v[0] & 0xf0, kAptxFreq, &c->rate) + " Hz, " + bits(v[0] & 0x0f, kAptxMode);
    field(c, "Sampling frequency", bits(v[0] & 0xf0, kAptxFreq) + " Hz");
    field(c, "Channel mode", bits(v[0] & 0x0f, kAptxMode));
  }
  if (vlen > 1) field(c, "Codec information", hexs(v + 1, vlen - 1));
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
      decode_mpeg(p, len, out);
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

// The companies whose IDs (Assigned Numbers 7.1) show up on the bench: controller makers, the
// phones and earbuds it talks to, and A2DP vendor codecs' owners. Not the whole list (thousands).
const char* company_name(uint16_t c) {
  switch (c) {
    case 0x0000: return "Ericsson";
    case 0x0001: return "Nokia";
    case 0x0002: return "Intel";
    case 0x0006: return "Microsoft";
    case 0x000a: return "Qualcomm (CSR)";
    case 0x000d: return "Texas Instruments";
    case 0x000f: return "Broadcom";
    case 0x001d: return "Qualcomm";
    case 0x0030: return "STMicroelectronics";
    case 0x0046: return "MediaTek";
    case 0x004c: return "Apple";
    case 0x004f: return "APT Licensing";
    case 0x0059: return "Nordic Semiconductor";
    case 0x005d: return "Realtek";
    case 0x0075: return "Samsung";
    case 0x0087: return "Garmin";
    case 0x009e: return "Bose";
    case 0x00d7: return "Qualcomm";
    case 0x00e0: return "Google";
    case 0x012d: return "Sony";
    case 0x0131: return "Cypress";
    case 0x0171: return "Amazon";
    case 0x027d: return "Huawei";
    case 0x02e5: return "Espressif";
    case 0x038f: return "Xiaomi";
    case 0x05f1: return "The Linux Foundation";
    case 0x08a9: return "Fraunhofer IIS";
    default: return nullptr;
  }
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

const char* avdtp_error_name(uint8_t e) {
  switch (e) {
    case 0x01: return "BAD_HEADER_FORMAT";
    case 0x11: return "BAD_LENGTH";
    case 0x12: return "BAD_ACP_SEID";
    case 0x13: return "SEP_IN_USE";
    case 0x14: return "SEP_NOT_IN_USE";
    case 0x17: return "BAD_SERV_CATEGORY";
    case 0x18: return "BAD_PAYLOAD_FORMAT";
    case 0x19: return "NOT_SUPPORTED_COMMAND";
    case 0x1a: return "INVALID_CAPABILITIES";
    case 0x22: return "BAD_RECOVERY_TYPE";
    case 0x23: return "BAD_MEDIA_TRANSPORT_FORMAT";
    case 0x25: return "BAD_RECOVERY_FORMAT";
    case 0x26: return "BAD_ROHC_FORMAT";
    case 0x27: return "BAD_CP_FORMAT";
    case 0x28: return "BAD_MULTIPLEXING_FORMAT";
    case 0x29: return "UNSUPPORTED_CONFIGURATION";
    case 0x31: return "BAD_STATE";
    case 0xc1: return "INVALID_CODEC_TYPE";
    case 0xc2: return "NOT_SUPPORTED_CODEC_TYPE";
    case 0xc3: return "INVALID_SAMPLING_FREQUENCY";
    case 0xc4: return "NOT_SUPPORTED_SAMPLING_FREQUENCY";
    case 0xc5: return "INVALID_CHANNEL_MODE";
    case 0xc6: return "NOT_SUPPORTED_CHANNEL_MODE";
    case 0xc7: return "INVALID_SUBBANDS";
    case 0xc8: return "NOT_SUPPORTED_SUBBANDS";
    case 0xc9: return "INVALID_ALLOCATION_METHOD";
    case 0xca: return "NOT_SUPPORTED_ALLOCATION_METHOD";
    case 0xcb: return "INVALID_MINIMUM_BITPOOL_VALUE";
    case 0xcc: return "NOT_SUPPORTED_MINIMUM_BITPOOL_VALUE";
    case 0xcd: return "INVALID_MAXIMUM_BITPOOL_VALUE";
    case 0xce: return "NOT_SUPPORTED_MAXIMUM_BITPOOL_VALUE";
    case 0xcf: return "INVALID_LAYER";
    case 0xd0: return "NOT_SUPPORTED_LAYER";
    case 0xd1: return "NOT_SUPPORTED_CRC";
    case 0xd2: return "NOT_SUPPORTED_MPF";
    case 0xd3: return "NOT_SUPPORTED_VBR";
    case 0xd4: return "INVALID_BIT_RATE";
    case 0xd5: return "NOT_SUPPORTED_BIT_RATE";
    case 0xd6: return "INVALID_OBJECT_TYPE";
    case 0xd7: return "NOT_SUPPORTED_OBJECT_TYPE";
    case 0xd8: return "INVALID_CHANNELS";
    case 0xd9: return "NOT_SUPPORTED_CHANNELS";
    case 0xda: return "INVALID_VERSION";
    case 0xdb: return "NOT_SUPPORTED_VERSION";
    case 0xdc: return "NOT_SUPPORTED_MAXIMUM_SUL";
    case 0xdd: return "INVALID_BLOCK_LENGTH";
    case 0xe0: return "INVALID_CP_TYPE";
    case 0xe1: return "INVALID_CP_FORMAT";
    case 0xe2: return "INVALID_CODEC_PARAMETER";
    case 0xe3: return "NOT_SUPPORTED_CODEC_PARAMETER";
    default: return nullptr;
  }
}

}  // namespace btb::hci
