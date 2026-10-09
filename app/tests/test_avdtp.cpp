// AVDTP browsing's message handling: the commands it sends, how it reads the device's answers,
// and the decoding of DISCOVER's SEP list and of the service capabilities.
#include "avdtp/probe.h"
#include "check.h"

using json = nlohmann::json;
using namespace btb::avdtp;

namespace {

void test_commands() {
  CHECK_EQ(command(3, kDiscover), (std::vector<uint8_t>{0x30, 0x01}));
  CHECK_EQ(command(1, kGetAllCapabilities, 2), (std::vector<uint8_t>{0x10, 0x0c, 0x08}));
}

void test_replies() {
  Reply r = parse_reply({0x32, 0x01, 0x04, 0x08}, 3, kDiscover);
  CHECK(r.ok);
  CHECK_EQ(r.payload.size(), size_t(2));
  r = parse_reply({0x33, 0x02, 0x12}, 3, kGetCapabilities);
  CHECK(r.rejected);
  CHECK_EQ(r.error_code, uint8_t(0x12));
  CHECK(!parse_reply({0x42, 0x01}, 3, kDiscover).ok);         // another label
  CHECK(parse_reply({0x31, 0x0c}, 3, kGetAllCapabilities).rejected);  // general reject
  CHECK(!parse_reply({0x30}, 3, kDiscover).ok);
}

void test_discover() {
  // SEP 1: audio source not in use; SEP 2: audio sink in use.
  const json s = parse_discover({0x04, 0x00, 0x0a, 0x08});
  CHECK_EQ(s.size(), size_t(2));
  CHECK_EQ(s[0]["seid"].get<int>(), 1);
  CHECK_EQ(s[0]["role"].get<std::string>(), std::string("source"));
  CHECK(!s[0]["in_use"].get<bool>());
  CHECK_EQ(s[1]["seid"].get<int>(), 2);
  CHECK(s[1]["in_use"].get<bool>());
  CHECK_EQ(s[1]["role"].get<std::string>(), std::string("sink"));
  CHECK_EQ(s[1]["media_type"].get<std::string>(), std::string("audio"));
}

void test_capabilities() {
  // Media transport, SBC (every option, bitpool 2..53), SCMS-T content protection, delay reporting.
  const json c = parse_capabilities({0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0xff, 0xff, 0x02, 0x35,
                                     0x04, 0x02, 0x03, 0x00, 0x08, 0x00});
  CHECK_EQ(c.size(), size_t(4));
  CHECK_EQ(c[0]["name"].get<std::string>(), std::string("media transport"));
  CHECK_EQ(c[1]["codec"]["name"].get<std::string>(), std::string("SBC"));
  CHECK_EQ(c[2]["type"].get<std::string>(), std::string("SCMS-T"));
  CHECK_EQ(c[3]["name"].get<std::string>(), std::string("delay reporting"));
  const json t = parse_capabilities({0x07, 0x09, 0x00});
  CHECK_EQ(t[0]["error"].get<std::string>(), std::string("truncated"));
}

}  // namespace

int main() {
  test_commands();
  test_replies();
  test_discover();
  test_capabilities();
  return report("test_avdtp");
}
