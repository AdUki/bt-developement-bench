// The OBEX tester's decisions: what a session target and a file name may be, and the shapes of
// obexd's answers (folder listings, vCard lists, message lists, image properties, transfers) as
// the API serves them. The D-Bus side is exercised against tools/fake-bluez's fake obexd.
#include "check.h"
#include "obex/obex.h"

using json = nlohmann::json;
using namespace btb::obex;

namespace {

void test_names() {
  CHECK(target_ok("opp"));
  CHECK(target_ok("bip-avrcp"));
  CHECK(!target_ok("sync"));
  CHECK(!target_ok("OPP"));
  CHECK(file_name_ok("photo 1.jpg"));
  CHECK(!file_name_ok(""));
  CHECK(!file_name_ok(".hidden"));
  CHECK(!file_name_ok("../etc/passwd"));
  CHECK(!file_name_ok("a/b"));
  CHECK(!file_name_ok(std::string("a\nb")));
  CHECK_EQ(transfer_id("/org/bluez/obex/client/session3/transfer7"), std::string("session3/transfer7"));
  CHECK_EQ(transfer_id("/org/bluez/obex/server/session1/transfer1"), std::string("server/session1/transfer1"));
}

void test_shapes() {
  const json t = transfer_json("/org/bluez/obex/server/session1/transfer1",
                               json{{"Status", "active"}, {"Name", "a.jpg"}, {"Size", 100}, {"Transferred", 40},
                                    {"Session", "/org/bluez/obex/server/session1"}, {"Filename", "/data/in/a.jpg"}});
  CHECK_EQ(t["direction"].get<std::string>(), std::string("in"));
  CHECK_EQ(t["session"].get<std::string>(), std::string("server/session1"));
  CHECK_EQ(t["transferred"].get<int>(), 40);
  CHECK(transfer_json("/x", json::object())["size"].is_null());

  const json f = folder_entries(json::parse(R"([[{"Name": "DCIM", "Type": "folder"},
      {"Name": "a.txt", "Type": "file", "Size": 3, "Modified": "20261009T120000Z"}]])"));
  CHECK_EQ(f.size(), size_t(2));
  CHECK_EQ(f[1]["size"].get<int>(), 3);
  CHECK_EQ(f[1]["modified"].get<std::string>(), std::string("20261009T120000Z"));
  CHECK(folder_entries(json::array()).empty());

  const json v = vcard_list(json::parse(R"([[["0.vcf", "Owner"], ["1.vcf", "Alice"], ["bad"]]])"));
  CHECK_EQ(v.size(), size_t(2));
  CHECK_EQ(v[1]["name"].get<std::string>(), std::string("Alice"));

  const json m = message_list(json::parse(R"([{"/org/bluez/obex/client/session4/message1000":
      {"Subject": "Lunch?", "Sender": "Alice", "Read": false, "Size": 26}}])"));
  CHECK_EQ(m.size(), size_t(1));
  CHECK_EQ(m[0]["id"].get<std::string>(), std::string("message1000"));
  CHECK(!m[0]["read"].get<bool>());

  const json i = image_properties(json::parse(R"([[{"handle": "1000001"}, {"type": "native", "Encoding": "JPEG"}]])"));
  CHECK_EQ(i.size(), size_t(2));
  CHECK_EQ(i[1]["encoding"].get<std::string>(), std::string("JPEG"));  // keys lowercased
}

}  // namespace

int main() {
  test_names();
  test_shapes();
  return report("test_obex");
}
