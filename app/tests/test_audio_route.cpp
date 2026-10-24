// The route's decisions: which Bluetooth node is a device's input, and when two parts make one
// stereo input (audio/route.h). The streams themselves are AudioEngine's, exercised on a board.
#include "audio/route.h"
#include "check.h"

using namespace btb::audio;
using json = nlohmann::json;

namespace {

json bt_source(const std::string& id, const std::string& address, const std::string& profile,
               const json& transport = nullptr) {
  return json{{"id", id}, {"kind", "bluetooth"}, {"direction", "source"}, {"address", address}, {"profile", profile},
              {"codec", ""}, {"transport", transport}, {"label", id}, {"rate", nullptr}, {"set", nullptr},
              {"set_leader", false}};
}

std::vector<RouteDevice> devs(std::initializer_list<RouteDevice> l) { return l; }

void test_kind() {
  CHECK_EQ(route_kind(bt_source("a", "AA:BB:CC:DD:EE:01", "a2dp-sink")), std::string("a2dp"));
  CHECK_EQ(route_kind(bt_source("a", "AA:BB:CC:DD:EE:01", "headset-head-unit")), std::string("hfp"));
  CHECK_EQ(route_kind(bt_source("a", "AA:BB:CC:DD:EE:01", "bap-duplex")), std::string("bap"));
  CHECK_EQ(route_kind(bt_source("a", "AA:BB:CC:DD:EE:01", "", "/org/bluez/hci0/dev_AA/sid0/bis1/fd0")),
           std::string("broadcast"));
  CHECK_EQ(route_kind(json{{"id", "alsa_input.x"}, {"kind", "loopback"}}), std::string(""));
}

void test_input_json() {
  std::string type, err;
  std::vector<RouteDevice> d;
  CHECK(route_input_from_json(json{{"type", "local"}}, &type, &d, &err));
  CHECK_EQ(type, std::string("local"));
  CHECK(route_input_from_json(json{{"type", "bluetooth"}, {"devices", {{{"address", "aa:bb:cc:dd:ee:01"}, {"kind", "a2dp"}}}}},
                              &type, &d, &err));
  CHECK_EQ(d.size(), size_t(1));
  CHECK_EQ(d[0].address, std::string("AA:BB:CC:DD:EE:01"));
  // One input at a time: two phones are not one.
  CHECK(!route_input_from_json(json{{"type", "bluetooth"}, {"devices", {{{"address", "AA:BB:CC:DD:EE:01"}, {"kind", "a2dp"}},
                                                                       {{"address", "AA:BB:CC:DD:EE:02"}, {"kind", "a2dp"}}}}},
                               &type, &d, &err));
  CHECK(route_input_from_json(json{{"type", "bluetooth"}, {"devices", {{{"address", "AA:BB:CC:DD:EE:01"}, {"kind", "broadcast"}},
                                                                      {{"address", "AA:BB:CC:DD:EE:02"}, {"kind", "broadcast"}}}}},
                              &type, &d, &err));
  CHECK(!route_input_from_json(json{{"type", "bluetooth"}, {"devices", json::array()}}, &type, &d, &err));
  CHECK(!route_input_from_json(json{{"type", "bluetooth"}, {"devices", {{{"address", "nope"}}}}}, &type, &d, &err));
  CHECK(!route_input_from_json(json{{"type", "bluetooth"}, {"devices", {{{"address", "AA:BB:CC:DD:EE:01"}, {"kind", "x"}}}}},
                               &type, &d, &err));
  CHECK(!route_input_from_json(json{{"type", "radio"}}, &type, &d, &err));
}

void test_capture() {
  SourceSpec s;
  int rate = 0;
  std::string err;
  // A phone: its music, unless its call is asked for.
  const json phone = json::array({bt_source("bluez_input.AA_BB_CC_DD_EE_01.2", "AA:BB:CC:DD:EE:01", "a2dp-sink"),
                                  bt_source("bluez_input.AA_BB_CC_DD_EE_01.0", "AA:BB:CC:DD:EE:01", "headset-head-unit")});
  CHECK(route_capture(devs({{"AA:BB:CC:DD:EE:01", ""}}), phone, json::array(), &s, &rate, &err) == CaptureFind::kFound);
  CHECK_EQ(s.target, std::string("bluez_input.AA_BB_CC_DD_EE_01.2"));
  CHECK(s.target_right.empty());
  CHECK_EQ(rate, 48000);
  CHECK(route_capture(devs({{"AA:BB:CC:DD:EE:01", "hfp"}}), phone, json::array(), &s, &rate, &err) == CaptureFind::kFound);
  CHECK_EQ(s.target, std::string("bluez_input.AA_BB_CC_DD_EE_01.0"));
  // Its link's rate when the node says none.
  CHECK(route_capture(devs({{"AA:BB:CC:DD:EE:01", ""}}), phone,
                      json::array({{{"address", "AA:BB:CC:DD:EE:01"}, {"uuid", "2bcb"}, {"codec", {{"rate", 32000}}}},
                                   {{"address", "AA:BB:CC:DD:EE:01"}, {"uuid", "110b"}, {"codec", {{"rate", 44100}}}}}), &s, &rate, &err) ==
        CaptureFind::kFound);
  CHECK_EQ(rate, 44100);
  // Not streaming yet: waited for.
  CHECK(route_capture(devs({{"AA:BB:CC:DD:EE:09", ""}}), phone, json::array(), &s, &rate, &err) == CaptureFind::kMissing);

  // A broadcaster's two BISes, right first in the list: left and right by their audio location.
  const std::string t1 = "/org/bluez/hci1/dev_50_1E_2D_9E_E7_92/sid0/bis1/fd0";
  const std::string t2 = "/org/bluez/hci1/dev_50_1E_2D_9E_E7_92/sid0/bis2/fd1";
  const json bis = json::array({bt_source("bis2", "50:1E:2D:9E:E7:92", "", t2), bt_source("bis1", "50:1E:2D:9E:E7:92", "", t1)});
  const auto rx = [](const std::string& path, int loc, const std::string& addr, const std::string& state) {
    return json{{"path", path}, {"location", loc}, {"uuid", "1851"}, {"address", addr}, {"state", state}};
  };
  const json tr = json::array({rx(t1, 1, "50:1E:2D:9E:E7:92", "active"), rx(t2, 2, "50:1E:2D:9E:E7:92", "active")});
  CHECK(route_capture(devs({{"50:1E:2D:9E:E7:92", "broadcast"}}), bis, tr, &s, &rate, &err) == CaptureFind::kFound);
  CHECK_EQ(s.target, std::string("bis1"));
  CHECK_EQ(s.target_right, std::string("bis2"));
  // Both received, the right one's node not there yet: waited for.
  const json tr_rx = json::array({{{"path", t1}, {"location", 1}, {"uuid", "1851"}, {"address", "50:1E:2D:9E:E7:92"}, {"state", "active"}},
                                  {{"path", t2}, {"location", 2}, {"uuid", "1851"}, {"address", "50:1E:2D:9E:E7:92"}, {"state", "broadcasting"}}});
  CHECK(route_capture(devs({{"50:1E:2D:9E:E7:92", "broadcast"}}), json::array({bis[1]}), tr_rx, &s, &rate, &err) ==
        CaptureFind::kMissing);
  CHECK(route_capture(devs({{"50:1E:2D:9E:E7:92", "broadcast"}}), bis, tr_rx, &s, &rate, &err) == CaptureFind::kFound);
  CHECK_EQ(s.target_right, std::string("bis2"));
  // The first BIS on the broadcaster's own card, a stream with no transport: the BIS left over.
  json card = bt_source("bluez_input.50_1E_2D_9E_E7_92.0", "50:1E:2D:9E:E7:92", "bap-source");
  card["stream"] = true;
  json bis2only = bt_source("bis2", "50:1E:2D:9E:E7:92", "bap-source", t2);
  const json tr_both = json::array({{{"path", t1}, {"location", 1}, {"uuid", "1851"}, {"address", "50:1E:2D:9E:E7:92"}, {"state", "active"}},
                                    {{"path", t2}, {"location", 2}, {"uuid", "1851"}, {"address", "50:1E:2D:9E:E7:92"}, {"state", "active"}}});
  CHECK(route_capture(devs({{"50:1E:2D:9E:E7:92", "broadcast"}}), json::array({bis2only, card}), tr_both, &s, &rate, &err) ==
        CaptureFind::kFound);
  CHECK_EQ(s.target, std::string("bluez_input.50_1E_2D_9E_E7_92.0"));
  CHECK_EQ(s.target_right, std::string("bis2"));
  // A unicast link's source (a device, not a stream) is not taken for it.
  card["stream"] = false;
  CHECK(route_capture(devs({{"50:1E:2D:9E:E7:92", "broadcast"}}), json::array({bis2only, card}), tr_both, &s, &rate, &err) ==
        CaptureFind::kMissing);
  // One stereo BIS: one target.
  const json one = json::array({bt_source("bis1", "50:1E:2D:9E:E7:92", "", t1)});
  CHECK(route_capture(devs({{"50:1E:2D:9E:E7:92", "broadcast"}}), one, json::array({rx(t1, 3, "50:1E:2D:9E:E7:92", "active")}), &s,
                      &rate, &err) == CaptureFind::kFound);
  CHECK_EQ(s.target, std::string("bis1"));
  CHECK(s.target_right.empty());

  // Two broadcasters, one channel each.
  const std::string ta = "/org/bluez/hci1/dev_AA_AA_AA_AA_AA_AA/sid0/bis1/fd0";
  const std::string tb = "/org/bluez/hci1/dev_BB_BB_BB_BB_BB_BB/sid0/bis1/fd1";
  const json two = json::array({bt_source("a", "AA:AA:AA:AA:AA:AA", "", ta), bt_source("b", "BB:BB:BB:BB:BB:BB", "", tb)});
  CHECK(route_capture(devs({{"AA:AA:AA:AA:AA:AA", "broadcast"}, {"BB:BB:BB:BB:BB:BB", "broadcast"}}), two,
                      json::array({rx(ta, 2, "AA:AA:AA:AA:AA:AA", "active"), rx(tb, 1, "BB:BB:BB:BB:BB:BB", "active")}), &s, &rate,
                      &err) == CaptureFind::kFound);
  CHECK_EQ(s.target, std::string("b"));
  CHECK_EQ(s.target_right, std::string("a"));
  // Both on the left: not one input.
  CHECK(route_capture(devs({{"AA:AA:AA:AA:AA:AA", "broadcast"}, {"BB:BB:BB:BB:BB:BB", "broadcast"}}), two,
                      json::array({rx(ta, 1, "AA:AA:AA:AA:AA:AA", "active"), rx(tb, 1, "BB:BB:BB:BB:BB:BB", "active")}), &s, &rate,
                      &err) == CaptureFind::kInvalid);

  // A pair of earbuds: the set's node, whichever member was picked; the members' own are not.
  json leader = bt_source("bluez_input.set_1", "", "bap-duplex");
  leader["address"] = nullptr;
  leader["set"] = "/org/bluez/hci1/set_1";
  leader["set_leader"] = true;
  leader["members"] = {"CC:CC:CC:CC:CC:01", "CC:CC:CC:CC:CC:02"};
  leader["rate"] = 32000;
  json member = bt_source("bluez_input.CC_CC_CC_CC_CC_02.0", "CC:CC:CC:CC:CC:02", "bap-duplex");
  member["set"] = "/org/bluez/hci1/set_1";
  CHECK(route_capture(devs({{"CC:CC:CC:CC:CC:02", ""}}), json::array({member, leader}), json::array(), &s, &rate, &err) ==
        CaptureFind::kFound);
  CHECK_EQ(s.target, std::string("bluez_input.set_1"));
  CHECK_EQ(rate, 32000);
}

}  // namespace

int main() {
  test_kind();
  test_input_json();
  test_capture();
  return report("audio_route");
}
