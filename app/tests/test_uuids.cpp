// UUID forms and the name table the console and the API share (www/uuids.json).
#include "check.h"
#include "uuids.h"

using namespace btb;

namespace {

void test_forms() {
  const std::string hrs = "0000180d-0000-1000-8000-00805f9b34fb";
  CHECK_EQ(uuid_full("180d"), hrs);
  CHECK_EQ(uuid_full("0x180D"), hrs);
  CHECK_EQ(uuid_full("0000180d"), hrs);
  CHECK_EQ(uuid_full("0000180D-0000-1000-8000-00805F9B34FB"), hrs);
  CHECK_EQ(uuid_full("0000180d00001000800000805f9b34fb"), hrs);
  CHECK_EQ(uuid_full("18"), std::string());
  CHECK_EQ(uuid_full("xyz1"), std::string());
  CHECK_EQ(uuid_full("0000180d-0000-1000-8000-00805f9b34f"), std::string());
  CHECK_EQ(uuid_short(hrs), std::string("180d"));
  CHECK_EQ(uuid_short("12345678-0000-1000-8000-00805f9b34fb"), std::string("12345678"));
  CHECK_EQ(uuid_short("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"),
           std::string("6e400001-b5a3-f393-e0a9-e50e24dcca9e"));
}

void test_names() {
  UuidNames n;
  std::string err;
  CHECK(n.load(std::string(BTB_WWW_DIR) + "/uuids.json", &err));
  CHECK(err.empty());
  CHECK(n.size() > 400);
  CHECK_EQ(n.name("180d"), std::string("Heart Rate"));
  CHECK_EQ(n.name("0000110b-0000-1000-8000-00805f9b34fb"), std::string("Audio Sink"));
  CHECK_EQ(n.name("2A37"), std::string("Heart Rate Measurement"));
  CHECK_EQ(n.name("6e400001-b5a3-f393-e0a9-e50e24dcca9e"), std::string("Nordic UART Service"));
  CHECK_EQ(n.name("1234"), std::string());
  CHECK(!n.load_text("{not json", &err));
  CHECK(!err.empty());
  CHECK_EQ(n.name("180d"), std::string("Heart Rate"));  // a failed load keeps what was there
}

}  // namespace

int main() {
  test_forms();
  test_names();
  return report("test_uuids");
}
