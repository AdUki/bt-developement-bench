// pw-top's batch table → nodes → the API's JSON (audio/pwtop.h).
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "audio/pwtop.h"
#include "check.h"

using btb::audio::PwTopNode;

namespace {

// A row exactly as pw-top's print_node() prints it.
std::string row(const char* st, unsigned id, unsigned q, unsigned r, const char* wait, const char* busy,
                const char* wq, const char* bq, unsigned err, const char* fmt, bool follower, const char* name) {
  char b[512];
  std::snprintf(b, sizeof b, "%s %4.1u %6.1u %6.1u %s %s %s %s  %3.1u %16.16s %s%s", st, id, q, r, wait, busy, wq, bq,
                err, fmt, follower ? " + " : "", name);
  return b;
}

void test_lines() {
  PwTopNode n;
  CHECK(!btb::audio::parse_pwtop_line("S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME ", &n));
  CHECK(!btb::audio::parse_pwtop_line("", &n));
  // A driver: the rows exactly as print_node() formats them.
  CHECK(btb::audio::parse_pwtop_line(
      row("R", 82, 1024, 48000, " 43.5us", "312.0us", " 0.00", " 0.01", 3, "S16LE 2 48000", false,
          "bluez_output.A4_B1_C1_0B_28_E4.1"), &n));
  CHECK_EQ(n.state, 'R');
  CHECK_EQ(n.id, 82u);
  CHECK_EQ(n.quant, 1024u);
  CHECK_EQ(n.rate, 48000u);
  CHECK_NEAR(n.wait_us, 43.5, 1e-9);
  CHECK_NEAR(n.busy_us, 312.0, 1e-9);
  CHECK_NEAR(n.busy_q, 0.01, 1e-9);
  CHECK_EQ(n.err, 3u);
  CHECK_EQ(n.format, std::string("S16LE 2 48000"));
  CHECK_EQ(n.name, std::string("bluez_output.A4_B1_C1_0B_28_E4.1"));
  CHECK(!n.follower);
  // A follower, in milliseconds.
  CHECK(btb::audio::parse_pwtop_line(row("R", 95, 0, 0, "  1.2ms", "  2.5ms", " 0.05", " 0.12", 0, "F32P 2 48000", true, "pw-cat"), &n));
  CHECK(n.follower);
  CHECK_NEAR(n.wait_us, 1200.0, 1e-6);
  CHECK_EQ(n.name, std::string("pw-cat"));
  // pw-top 1.6 marks an asynchronous follower with " = ".
  CHECK(btb::audio::parse_pwtop_line(
      "R   53   2880  48000   1.6ms 416.0us  0.04  0.01    0    S16LE 2 48000  = btbench-stream-1", &n));
  CHECK(n.follower);
  CHECK_EQ(n.name, std::string("btbench-stream-1"));
  CHECK_EQ(n.quant, 2880u);
  // Not running: no times, no format.
  CHECK(btb::audio::parse_pwtop_line(row("S", 49, 0, 0, "   --- ", "   --- ", " --- ", " --- ", 0, "", false,
                                         "alsa_output.platform-snd_aloop.0.analog-stereo"), &n));
  CHECK_EQ(n.state, 'S');
  CHECK(n.wait_us < 0 && n.busy_q < 0);
  CHECK_EQ(n.format, std::string(""));
  CHECK_EQ(n.name, std::string("alsa_output.platform-snd_aloop.0.analog-stereo"));
}

void test_json() {
  std::vector<PwTopNode> rows(2);
  rows[0].state = 'R';
  rows[0].id = 82;
  rows[0].quant = 1024;
  rows[0].rate = 48000;
  rows[0].busy_q = 0.25;
  rows[0].err = 3;
  rows[0].name = "sink";
  rows[1].state = 'R';
  rows[1].id = 95;
  rows[1].quant = 512;
  rows[1].rate = 48000;
  rows[1].follower = true;
  rows[1].err = 1;
  rows[1].name = "pw-cat";
  std::map<uint32_t, uint32_t> prev;
  auto j = btb::audio::pwtop_json(rows, 1000, &prev);
  CHECK_EQ(j["drivers"].size(), 1u);
  CHECK_EQ(j["drivers"][0]["quantum"], 1024);
  CHECK_NEAR(j["drivers"][0]["period_ms"].get<double>(), 21.333, 1e-9);
  CHECK_EQ(j["drivers"][0]["followers"].size(), 1u);
  CHECK_NEAR(j["drivers"][0]["followers"][0]["latency_ms"].get<double>(), 10.667, 1e-9);
  CHECK_EQ(j["xruns"], 0);  // the first sighting is the baseline
  rows[0].err = 5;
  j = btb::audio::pwtop_json(rows, 2000, &prev);
  CHECK_EQ(j["drivers"][0]["xruns"], 2);
  CHECK_EQ(j["xruns"], 2);
}

}  // namespace

int main() {
  test_lines();
  test_json();
  return report("test_pwtop");
}
