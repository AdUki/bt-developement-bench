// Capture-file name validation and the fork/exec runner behind systemctl and btmon -a.

#include <chrono>
#include <string>

#include "../check.h"
#include "capture.h"

using btb::hci::Capture;
using btb::hci::run_command;

namespace {

void test_names() {
  CHECK(Capture::valid_name("hci-20261009-120000.btsnoop"));
  CHECK(Capture::valid_name("x.btsnoop"));
  CHECK(!Capture::valid_name(".btsnoop"));
  CHECK(!Capture::valid_name(".hidden.btsnoop"));
  CHECK(!Capture::valid_name("../etc/passwd.btsnoop"));
  CHECK(!Capture::valid_name("a/b.btsnoop"));
  CHECK(!Capture::valid_name("hci.btsnoop.txt"));
  CHECK(!Capture::valid_name("hci.log"));
  CHECK(!Capture::valid_name(""));
  CHECK(!Capture::valid_name(std::string("a\0b.btsnoop", 11)));
}

void test_run_command() {
  auto r = run_command({"sh", "-c", "echo out; echo err >&2; exit 3"}, 5000, 1024);
  CHECK(r.started);
  CHECK(!r.timed_out);
  CHECK_EQ(r.exit_code, 3);
  CHECK(r.output.find("out\n") != std::string::npos);
  CHECK(r.output.find("err\n") != std::string::npos);

  // The cap truncates but keeps draining, so the child is not blocked on a full pipe.
  r = run_command({"sh", "-c", "head -c 200000 /dev/zero; echo done >&2"}, 5000, 1000);
  CHECK_EQ(r.exit_code, 0);
  CHECK(r.truncated);
  CHECK_EQ(r.output.size(), 1000u);

  const auto t0 = std::chrono::steady_clock::now();
  r = run_command({"sleep", "10"}, 300, 1024);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  CHECK(r.timed_out);
  CHECK(ms < 3000);

  r = run_command({"/nonexistent/btmon", "-a", "x"}, 5000, 1024);
  CHECK_EQ(r.exit_code, 127);
  CHECK(r.error.find("not found") != std::string::npos);

  // Our descriptors do not leak into the child.
  r = run_command({"sh", "-c", "ls /proc/self/fd | wc -l"}, 5000, 64);
  CHECK(r.exit_code == 0 && std::stoi(r.output) <= 5);
}

void test_plot_errors() {
  // btmon -a's summary, with the shell's complaint about the missing plotter between sections.
  const std::string out = "Bluetooth monitor ver 5.87\nFound BR-ACL connection with handle 11\n"
                          "sh: line 1: gnuplot: command not found\n        Address: AA:BB\n"
                          "sh: gnuplot: not found\n        TX packets: 12";
  CHECK_EQ(btb::hci::drop_plot_errors(out),
           std::string("Bluetooth monitor ver 5.87\nFound BR-ACL connection with handle 11\n        Address: AA:BB\n        TX packets: 12"));
  CHECK_EQ(btb::hci::drop_plot_errors(""), std::string(""));
}

}  // namespace

// btmon -r's output for two context records and a frame split over two fragments: the last two
// records, renumbered with the capture's frame numbers, the columns kept.
void test_btmon_pick() {
  const std::string out =
      "Bluetooth monitor ver 5.72\n"
      "= New Index: B8:27:EB:8F:D1:56 (Primary,UART,hci0)                [hci0] 08:03:41.476906\n"
      "> HCI Event: Connect Complete (0x03) plen 11                    #1 [hci0] 08:03:41.500000\n"
      "        Status: Success (0x00)\n"
      "< ACL Data TX: Handle 12 flags 0x00 dlen 48                     #2 [hci0] 08:03:41.680094\n"
      "< ACL Data TX: Handle 12 flags 0x01 dlen 8                      #3 [hci0] 08:03:41.680100\n"
      "      AVRCP: GetFolderItems\n";
  size_t found = 0;
  const std::string t = btb::hci::btmon_pick(out, {4711, 4712}, &found);
  CHECK_EQ(found, 2u);
  CHECK_EQ(t,
           "< ACL Data TX: Handle 12 flags 0x00 dlen 48                  #4711 [hci0] 08:03:41.680094\n"
           "< ACL Data TX: Handle 12 flags 0x01 dlen 8                   #4712 [hci0] 08:03:41.680100\n"
           "      AVRCP: GetFolderItems\n");
  CHECK_EQ(btb::hci::btmon_pick("Bluetooth monitor ver 5.72\n", {1}, &found), std::string(""));
  CHECK_EQ(found, 0u);
}

int main() {
  test_names();
  test_run_command();
  test_plot_errors();
  test_btmon_pick();
  return report("test_hci_capture");
}
