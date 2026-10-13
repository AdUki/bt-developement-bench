#include "util/spa_json.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

using json = nlohmann::json;

namespace btb {

namespace {

class Parser {
 public:
  explicit Parser(const std::string& t) : t_(t) {}

  json document() {
    json o = members('\0');
    skip();
    if (i_ < t_.size()) fail();
    return o;
  }

 private:
  [[noreturn]] void fail() { throw std::runtime_error("SPA-JSON"); }

  // Blanks, commas and comments: none of them mean anything between tokens.
  void skip() {
    while (i_ < t_.size()) {
      const char c = t_[i_];
      if (c == '#') {
        while (i_ < t_.size() && t_[i_] != '\n') ++i_;
      } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
        ++i_;
      } else {
        break;
      }
    }
  }

  static bool bare_char(char c) { return !std::strchr(" \t\r\n,#{}[]=:\"", c); }

  std::string quoted() {
    ++i_;  // the opening quote
    std::string s;
    while (i_ < t_.size() && t_[i_] != '"') {
      char c = t_[i_++];
      if (c == '\\' && i_ < t_.size()) {
        c = t_[i_++];
        switch (c) {
          case 'n': c = '\n'; break;
          case 't': c = '\t'; break;
          case 'r': c = '\r'; break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          default: break;  // \" \\ \/ and anything else: the character itself
        }
      }
      s += c;
    }
    if (i_ >= t_.size()) fail();
    ++i_;  // the closing quote
    return s;
  }

  std::string word() {
    const size_t start = i_;
    while (i_ < t_.size() && bare_char(t_[i_])) ++i_;
    if (i_ == start) fail();
    return t_.substr(start, i_ - start);
  }

  static json bare_value(const std::string& w) {
    if (w == "true") return true;
    if (w == "false") return false;
    if (w == "null") return nullptr;
    char* end = nullptr;
    if (w.find_first_of(".eE") == std::string::npos) {
      const long long v = std::strtoll(w.c_str(), &end, 0);
      if (*end == '\0') return v;
    }
    const double d = std::strtod(w.c_str(), &end);
    if (*end == '\0') return d;
    return w;
  }

  // key = value pairs up to `close` ('\0': the end of the text).
  json members(char close) {
    json o = json::object();
    for (;;) {
      skip();
      if (i_ >= t_.size()) {
        if (close) fail();
        return o;
      }
      if (t_[i_] == close) {
        ++i_;
        return o;
      }
      const std::string key = t_[i_] == '"' ? quoted() : word();
      skip();
      if (i_ < t_.size() && (t_[i_] == '=' || t_[i_] == ':')) ++i_;
      o[key] = value();
    }
  }

  json value() {
    skip();
    if (i_ >= t_.size()) fail();
    const char c = t_[i_];
    if (c == '{') {
      ++i_;
      return members('}');
    }
    if (c == '[') {
      ++i_;
      json a = json::array();
      for (;;) {
        skip();
        if (i_ >= t_.size()) fail();
        if (t_[i_] == ']') {
          ++i_;
          return a;
        }
        a.push_back(value());
      }
    }
    if (c == '"') return quoted();
    return bare_value(word());
  }

  const std::string& t_;
  size_t i_ = 0;
};

}  // namespace

json spa_json_parse(const std::string& text) {
  try {
    return Parser(text).document();
  } catch (const std::exception&) {
    return json(json::value_t::discarded);
  }
}

}  // namespace btb
