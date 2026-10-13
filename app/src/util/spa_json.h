#pragma once

// SPA-JSON, PipeWire's and WirePlumber's config syntax, read into JSON: keys need no quotes, `=`
// or `:` (or nothing) between a key and its value, commas are optional, `#` starts a comment, and a
// bare word is a string unless it reads as a number, true, false or null. The text itself is an
// object without its braces.

#include <nlohmann/json.hpp>
#include <string>

namespace btb {

// Discarded (is_discarded()) when the text is not SPA-JSON.
nlohmann::json spa_json_parse(const std::string& text);

}  // namespace btb
