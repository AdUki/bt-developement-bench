// Topic filtering in the WebSocket hub: who gets what, and that nobody watching costs nothing.
#include "check.h"
#include "ws_hub.h"

using namespace btb;

namespace {

void test_patterns() {
  CHECK(WsHub::topic_matches("*", "hci.stats"));
  CHECK(WsHub::topic_matches("hci.stats", "hci.stats"));
  CHECK(!WsHub::topic_matches("hci.stats", "hci.event"));
  CHECK(WsHub::topic_matches("job.*", "job.3"));
  CHECK(WsHub::topic_matches("hci.*", "hci.stats"));
  CHECK(!WsHub::topic_matches("job.*", "job"));
  CHECK(!WsHub::topic_matches("job.*", "jobs.1"));
  CHECK(!WsHub::topic_matches("bt", "bt.request"));
  CHECK(!WsHub::topic_matches("", "bt"));
}

void test_parse() {
  const auto t = WsHub::parse_topics(" bt, hci.stats,,job.* ");
  CHECK_EQ(t.size(), size_t(3));
  CHECK_EQ(t[0], std::string("bt"));
  CHECK_EQ(t[2], std::string("job.*"));
  CHECK(WsHub::parse_topics("").empty());
}

void test_fanout() {
  WsHub hub;
  CHECK(!hub.has_subscribers("bt"));
  CHECK(!hub.publish("bt", {{"x", 1}}));  // nobody: not even serialized
  auto a = hub.add({"bt"});
  auto b = hub.add({"hci.*"});
  CHECK(hub.has_subscribers("bt"));
  CHECK(hub.has_subscribers("hci.stats"));
  CHECK(!hub.has_subscribers("journal"));
  CHECK(hub.publish("bt", {{"x", 1}}));
  CHECK(hub.publish("hci.stats", {{"y", 2}}));
  WsMessagePtr m = hub.wait(a, 10);
  CHECK(m != nullptr);
  CHECK_EQ(*m, std::string("{\"data\":{\"x\":1},\"topic\":\"bt\"}"));
  CHECK(hub.wait(a, 10) == nullptr);  // a did not get hci.stats
  m = hub.wait(b, 10);
  CHECK(m != nullptr && m->find("hci.stats") != std::string::npos);

  // A subscription change takes effect for the next message.
  hub.set_topics(a, {"journal"});
  CHECK(!hub.has_subscribers("bt"));
  CHECK(hub.has_subscribers("journal"));

  // A slow client loses its oldest frames, never blocks the publisher.
  for (int i = 0; i < 40; ++i) hub.publish("journal", {{"i", i}});
  int got = 0, first = -1;
  while (WsMessagePtr x = hub.wait(a, 1)) {
    if (first < 0) first = nlohmann::json::parse(*x)["data"]["i"].get<int>();
    ++got;
  }
  CHECK_EQ(got, static_cast<int>(WsHub::kMaxQueued));
  CHECK_EQ(first, 40 - static_cast<int>(WsHub::kMaxQueued));

  hub.remove(a);
  CHECK(!hub.has_subscribers("journal"));
  CHECK_EQ(hub.clients(), size_t(1));
  hub.shutdown();
  CHECK(hub.wait(b, 10) == nullptr);
}

void test_bad_utf8() {
  WsHub hub;
  auto a = hub.add({"*"});
  // A device name is anyone's bytes; the publisher must not throw on them.
  hub.publish("bt", {{"name", std::string("\xff\xfe" "abc")}});
  CHECK(hub.wait(a, 10) != nullptr);
}

}  // namespace

int main() {
  test_patterns();
  test_parse();
  test_fanout();
  test_bad_utf8();
  return report("test_ws_hub");
}
