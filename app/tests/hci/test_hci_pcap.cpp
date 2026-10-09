// pcap encoding (LINUX_BT_MONITOR) and the bounded live-capture fan-out.

#include <cstring>
#include <string>
#include <thread>

#include "../check.h"
#include "pcap.h"

using btb::hci::PcapFeed;

namespace {

uint32_t host32(const std::string& s, size_t off) {
  uint32_t v;
  std::memcpy(&v, s.data() + off, 4);
  return v;
}

void test_header_and_record() {
  const std::string h = btb::hci::pcap_global_header();
  CHECK_EQ(h.size(), 24u);
  CHECK_EQ(host32(h, 0), 0xa1b2c3d4u);
  uint16_t major, minor;
  std::memcpy(&major, h.data() + 4, 2);
  std::memcpy(&minor, h.data() + 6, 2);
  CHECK_EQ(major, 2);
  CHECK_EQ(minor, 4);
  CHECK_EQ(host32(h, 16), 65539u);
  CHECK_EQ(host32(h, 20), 254u);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  const unsigned char want[8] = {0xd4, 0xc3, 0xb2, 0xa1, 0x02, 0x00, 0x04, 0x00};
  CHECK(std::memcmp(h.data(), want, 8) == 0);
#endif

  std::string rec;
  const uint8_t payload[3] = {0x0e, 0x01, 0x02};
  btb::hci::pcap_append_record(&rec, 1700000000LL * 1000000 + 123456, 1, 5, payload, 3);
  CHECK_EQ(rec.size(), 16u + 4u + 3u);
  CHECK_EQ(host32(rec, 0), 1700000000u);
  CHECK_EQ(host32(rec, 4), 123456u);
  CHECK_EQ(host32(rec, 8), 7u);
  CHECK_EQ(host32(rec, 12), 7u);
  // pseudo-header: adapter 1, opcode 5 (ACL RX), big-endian
  const unsigned char ph[7] = {0x00, 0x01, 0x00, 0x05, 0x0e, 0x01, 0x02};
  CHECK(std::memcmp(rec.data() + 16, ph, 7) == 0);
}

void test_feed() {
  PcapFeed feed;
  CHECK(!feed.active());
  auto a = feed.subscribe();
  auto b = feed.subscribe();
  CHECK(a && b);
  CHECK(feed.subscribe() == nullptr);  // two at most: each holds an HTTP worker
  CHECK(feed.active());

  const uint8_t d[4] = {1, 2, 3, 4};
  feed.push(1000000, 0, 4, d, 4);
  feed.push(2000000, 0, 5, d, 4);
  std::string out;
  CHECK(PcapFeed::take(*a, &out, 10));
  CHECK_EQ(out.size(), 24u + 2 * (16 + 4 + 4));
  CHECK_EQ(host32(out, 20), 254u);
  CHECK_EQ(host32(out, 24), 1u);  // first record's ts_sec
  CHECK_EQ(a->sent_records, 2u);

  // Nothing queued: take times out with an empty chunk, still open.
  CHECK(PcapFeed::take(*a, &out, 10));
  CHECK(out.empty());

  // b never reads: its queue stays bounded and drops the oldest chunks.
  std::vector<uint8_t> big(1000, 0x55);
  for (int i = 0; i < 3000; ++i) feed.push(3000000 + i, 0, 5, big.data(), big.size());
  CHECK(b->dropped_records > 0);
  CHECK(b->queued <= PcapFeed::kMaxQueued);
  CHECK(PcapFeed::take(*b, &out, 10));
  CHECK_EQ(host32(out, 0), 0xa1b2c3d4u);  // the header survives the drops
  CHECK_EQ(b->sent_records + b->dropped_records, 3002u);

  feed.unsubscribe(b);
  CHECK(feed.active());
  // Closing wakes a waiting reader, which then reports the end.
  std::thread closer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    feed.close_all();
  });
  PcapFeed::take(*a, &out, 5000);  // drains what is queued, or returns at close
  while (PcapFeed::take(*a, &out, 5000)) {
  }
  closer.join();
  feed.unsubscribe(a);
  CHECK(!feed.active());
}

}  // namespace

int main() {
  test_header_and_record();
  test_feed();
  return report("test_hci_pcap");
}
