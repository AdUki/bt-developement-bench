// Running a tool with a deadline: the output, the exit status, a timeout, a missing tool.
#include <unistd.h>

#include "check.h"
#include "scripts.h"
#include "util/exec.h"

using namespace btb;

int main() {
  ExecResult r = run_cmd({"sh", "-c", "echo out; echo err >&2; exit 3"}, 5000);
  CHECK(r.started);
  CHECK(!r.timed_out);
  CHECK_EQ(r.status, 3);
  CHECK_EQ(r.out, std::string("out\n"));
  CHECK_EQ(r.reason(), std::string("err"));
  CHECK(!r.ok());

  r = run_cmd({"sh", "-c", "sleep 5"}, 300);
  CHECK(r.timed_out);
  CHECK_EQ(r.reason(), std::string("timed out"));

  r = run_cmd({"no-such-tool-btbench"}, 1000);
  CHECK(!r.started);
  CHECK(r.reason().find("not found") != std::string::npos);

  // Output past the cap is dropped, not buffered.
  r = run_cmd({"sh", "-c", "head -c 100000 /dev/zero"}, 5000, 1000);
  CHECK(r.ok());
  CHECK_EQ(r.out.size(), size_t(1000));

  CHECK(!find_exec("sh").empty());
  CHECK(find_exec("no-such-tool-btbench").empty());

  // A script that is not installed is a 503 with a reason, not a crash.
  TargetScripts s("/nonexistent");
  const auto res = s.json_cmd("btbench-no-such-script", {"status"}, 1000);
  CHECK_EQ(res.http, 503);
  CHECK(res.body["error"].get<std::string>().find("not installed") != std::string::npos);
  return report("test_exec");
}
