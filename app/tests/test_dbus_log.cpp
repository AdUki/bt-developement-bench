// The D-Bus log's text: the line a message becomes in bluez-dbus.log, and how long arguments are
// cut. The monitoring itself is bus traffic, exercised on the board and against tools/fake-bluez.
#include <cstdlib>

#include "check.h"
#include "dbus_log.h"

using namespace btb;
using json = nlohmann::json;

namespace {

void test_line() {
  // 2026-10-11 10:45:14.331 UTC.
  const uint64_t ts = 1791715514331ull;
  const json call = {{"ts", ts}, {"type", "call"}, {"sender", ":1.112"}, {"sender_name", "btbenchd"},
                     {"dest", "org.bluez"}, {"dest_name", "bluetoothd"}, {"path", "/org/bluez/hci0/dev_50_1E_2D_95_16_34"},
                     {"iface", "org.bluez.Device1"}, {"member", "ConnectProfile"},
                     {"args", json::array({"0000110b-0000-1000-8000-00805f9b34fb"})}};
  CHECK_EQ(dbus_log_line(call), std::string("10:45:14.331 call   btbenchd(:1.112) → bluetoothd(org.bluez)  "
                                            "/org/bluez/hci0/dev_50_1E_2D_95_16_34 org.bluez.Device1.ConnectProfile "
                                            "[\"0000110b-0000-1000-8000-00805f9b34fb\"]"));
  // A reply names the call it answers and how long it took; no arguments, nothing after it.
  const json ret = {{"ts", ts}, {"type", "return"}, {"sender", ":1.113"}, {"sender_name", "bluetoothd"},
                    {"dest", ":1.112"}, {"dest_name", "btbenchd"}, {"call", "GetManagedObjects"}, {"latency_ms", 23},
                    {"args", "42 objects"}};
  CHECK_EQ(dbus_log_line(ret), std::string("10:45:14.331 return bluetoothd(:1.113) → btbenchd(:1.112)  "
                                           "GetManagedObjects 23 ms: 42 objects"));
  const json err = {{"ts", ts}, {"type", "error"}, {"sender", ":1.113"}, {"dest", ":1.112"}, {"call", "Connect"},
                    {"latency_ms", 30001}, {"error_name", "org.bluez.Error.Failed"}, {"error", "br-connection-page-timeout"}};
  CHECK_EQ(dbus_log_line(err), std::string("10:45:14.331 error  :1.113 → :1.112  Connect 30001 ms: "
                                           "org.bluez.Error.Failed: br-connection-page-timeout"));
  // A signal has no destination.
  const json sig = {{"ts", ts}, {"type", "signal"}, {"sender", ":1.113"}, {"sender_name", "bluetoothd"},
                    {"path", "/org/bluez/hci0"}, {"iface", "org.freedesktop.DBus.Properties"},
                    {"member", "PropertiesChanged"}, {"args", json::array({"org.bluez.Adapter1", {{"Pairable", true}}, json::array()})}};
  CHECK_EQ(dbus_log_line(sig), std::string("10:45:14.331 signal bluetoothd(:1.113)  /org/bluez/hci0 "
                                           "org.freedesktop.DBus.Properties.PropertiesChanged "
                                           "[\"org.bluez.Adapter1\",{\"Pairable\":true},[]]"));
}

void test_args_text() {
  bool cut = true;
  CHECK_EQ(dbus_args_text(json::array({1, "a"}), 100, &cut), std::string("[1,\"a\"]"));
  CHECK(!cut);
  const std::string t = dbus_args_text(json::array({std::string(50, 'x')}), 10, &cut);
  CHECK(cut);
  CHECK_EQ(t, std::string("[\"xxxxxxxx… (54 bytes)"));
  // Never inside a UTF-8 sequence: "é" is two bytes, the cut lands before it.
  CHECK_EQ(dbus_args_text(json::array({"aé"}), 4, &cut), std::string("[\"a… (7 bytes)"));
}

}  // namespace

int main() {
  setenv("TZ", "UTC", 1);
  tzset();
  test_line();
  test_args_text();
  return report("test_dbus_log");
}
