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

int main() {
  test_names();
  test_run_command();
  test_plot_errors();
  return report("test_hci_capture");
}
