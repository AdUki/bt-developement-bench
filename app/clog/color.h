#pragma once

#include <map>
#include <stack>
#include <string>

namespace Color {

typedef std::map<std::string::const_iterator, std::deque<std::string>> Map;
typedef std::stack<std::string> Stack;
const std::string Reset = "\x1b[0m";
const std::string Reset00 = "\x1b[00m";
}

