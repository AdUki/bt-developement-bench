// Codec capability/configuration decoding: what a developer would otherwise do by hand from btmon.
#include "a2dp_codec.h"
#include "check.h"

using namespace btb;
using json = nlohmann::json;

namespace {

void test_sbc_config() {
  // 48 kHz joint stereo, 16 blocks, 8 subbands, loudness, bitpool 2..53: the common SBC config.
  const json j = decode_codec(0x00, {0x11, 0x15, 0x02, 0x35}, false);
  CHECK_EQ(j["name"].get<std::string>(), std::string("SBC"));
  CHECK_EQ(j["rate"].get<unsigned>(), 48000u);
  CHECK_EQ(j["channel_mode"].get<std::string>(), std::string("joint stereo"));
  CHECK_EQ(j["blocks"].get<unsigned>(), 16u);
  CHECK_EQ(j["subbands"].get<unsigned>(), 8u);
  CHECK_EQ(j["allocation"].get<std::string>(), std::string("loudness"));
  CHECK_EQ(j["min_bitpool"].get<unsigned>(), 2u);
  CHECK_EQ(j["max_bitpool"].get<unsigned>(), 53u);
  CHECK_EQ(j["max_bitrate"].get<unsigned>(), 357000u);
  CHECK(j["summary"].get<std::string>().find("48000 Hz, joint stereo") == 0);
}

void test_sbc_caps() {
  const json j = decode_codec(0x00, {0xff, 0xff, 0x02, 0x35}, true);
  CHECK_EQ(j["rate"].size(), size_t(4));
  CHECK_EQ(j["channel_mode"].size(), size_t(4));
  CHECK_EQ(j["blocks"].size(), size_t(4));
  CHECK(!j.contains("max_bitrate"));  // not one configuration
  CHECK(decode_codec(0x00, {0x11}, false).contains("error"));
}

void test_sbc_bitrate() {
  // 44.1 kHz joint stereo at bitpool 53: the well-known 328 kbit/s "high quality".
  CHECK_EQ(sbc_bitrate(44100, "joint stereo", 16, 8, 53), 327993u);
  CHECK_EQ(sbc_bitrate(48000, "mono", 16, 8, 31), 8u * (4 + 4 + 62) * 48000 / 128);
  CHECK_EQ(sbc_bitrate(48000, "nonsense", 16, 8, 31), 0u);
}

void test_aac() {
  const json j = decode_codec(0x02, {0x80, 0x01, 0x04, 0x84, 0xe2, 0x00}, false);
  CHECK_EQ(j["name"].get<std::string>(), std::string("AAC"));
  CHECK_EQ(j["object_type"].get<std::string>(), std::string("MPEG-2 AAC LC"));
  CHECK_EQ(j["rate"].get<unsigned>(), 44100u);
  CHECK_EQ(j["channels"].get<unsigned>(), 2u);
  CHECK(j["vbr"].get<bool>());
  CHECK_EQ(j["bitrate"].get<unsigned>(), 320000u);
}

void test_vendor() {
  json j = decode_codec(0xff, {0x4f, 0, 0, 0, 0x01, 0, 0x22}, false);
  CHECK_EQ(j["name"].get<std::string>(), std::string("aptX"));
  CHECK_EQ(j["rate"].get<unsigned>(), 44100u);
  CHECK_EQ(j["channel_mode"].get<std::string>(), std::string("stereo"));
  j = decode_codec(0xff, {0x2d, 0x01, 0, 0, 0xaa, 0, 0x10, 0x01}, false);
  CHECK_EQ(j["name"].get<std::string>(), std::string("LDAC"));
  CHECK_EQ(j["rate"].get<unsigned>(), 48000u);
  j = decode_codec(0xff, {0x78, 0x56, 0x34, 0x12, 0x02, 0x01}, false);
  CHECK_EQ(j["name"].get<std::string>(), std::string("vendor 0x12345678:0x0102"));
  CHECK_EQ(codec_name(0xff, {0xd7, 0, 0, 0, 0x24, 0}), std::string("aptX HD"));
  CHECK(decode_codec(0xff, {0x4f}, false).contains("error"));
}

void test_lc3() {
  const json j = decode_codec(0x06, {0x02, 0x01, 0x08, 0x02, 0x02, 0x01, 0x05, 0x03, 0x03, 0x00, 0x00, 0x00,
                                     0x03, 0x04, 0x78, 0x00, 0x02, 0x05, 0x01},
                              false);
  CHECK_EQ(j["name"].get<std::string>(), std::string("LC3"));
  CHECK_EQ(j["rate"].get<unsigned>(), 48000u);
  CHECK_EQ(j["frame_duration"].get<std::string>(), std::string("10 ms"));
  CHECK_EQ(j["octets_per_frame"].get<unsigned>(), 120u);
  CHECK_EQ(j["channel_allocation"].get<std::string>(), std::string("0x00000003"));
  CHECK_EQ(j["bitrate"].get<unsigned>(), 96000u);
  CHECK(decode_codec(0x06, {0x05, 0x01}, false).contains("error"));  // a truncated LTV
}

void test_unknown() {
  const json j = decode_codec(0x42, {}, false);
  CHECK_EQ(j["name"].get<std::string>(), std::string("codec 0x42"));
  CHECK(j.contains("summary"));
}

}  // namespace

int main() {
  test_sbc_config();
  test_sbc_caps();
  test_sbc_bitrate();
  test_aac();
  test_vendor();
  test_lc3();
  test_unknown();
  return report("test_codec");
}
