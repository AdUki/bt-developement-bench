# clog

The log colorizer its author wrote for StreamSDK (`src/helpers/clog` there), copied here so the
bench builds it for the board and the PC. `clog FILE`, `clog -t FILE` (follow), `cmd | clog`:
the colour group comes from the file name (`clog.conf`: `FileRegexp`), so btbench-logd names the
files in `/tmp/logs` after the groups (`bluez.log`, `pipewire.log`, `wireplumber.log`,
`wpasupplicant.log`; the rest in the default `nsdk` format). `clog -h` lists the options.

Differences from the StreamSDK copy, all in the build:
- std::regex instead of Qt's (the board has no Qt); `std::match_results` has no `clear()`, so the
  one call is an assignment.
- `TERM` and `HOME` may be unset (a command over ssh, a service): read as empty / `/` instead of
  constructing a string from a null pointer.
- The `#include "helpers/clog/color.h"` path is local, and the config is compiled in (as the
  StreamSDK PC build does) besides being installed to `/usr/share/clog/clog.conf`.
