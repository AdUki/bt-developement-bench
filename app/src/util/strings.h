#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace btb {

std::string lower(std::string s);
std::string upper(std::string s);
std::string trim(const std::string& s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);
std::vector<std::string> split(const std::string& s, char sep, bool skip_empty = true);

// Bytes as lowercase hex without separators ("0a1b"), the way the API carries every value.
std::string to_hex(const std::vector<uint8_t>& b);
// Accepts what people paste: "0a1b", "0A 1B", "0a:1b", "0x0a1b". False on an odd digit count or
// anything that is not hex.
bool from_hex(const std::string& s, std::vector<uint8_t>* out);
// The bytes as text when they are printable ASCII/UTF-8 (a device name, a firmware string) and
// at least two of them, else "".
std::string printable(const std::vector<uint8_t>& b);

// A number the way the CLI and the URL give it: decimal, or hex with 0x. False on anything else
// or on a value above `max`.
bool parse_uint(const std::string& s, uint64_t max, uint64_t* out);

// A shell-style env file (KEY=VALUE, KEY="VALUE", KEY='VALUE', # comments), as board.env,
// device.env and bluetoothd.env are written. No expansion: these files are data.
std::map<std::string, std::string> parse_env(const std::string& text);
// The inverse for one value: double-quoted, with ", \, $ and ` escaped, so a systemd
// EnvironmentFile= and a shell both read back exactly the string given.
std::string env_quote(const std::string& v);

// Whitespace-separated words, honouring "double" and 'single' quotes, as a shell would split
// bluetoothd's arguments or a job's command line. No expansion of any kind.
std::vector<std::string> split_args(const std::string& s);

}  // namespace btb
