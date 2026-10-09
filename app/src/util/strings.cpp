#include "util/strings.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <sstream>

namespace btb {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(const std::string& s, char sep, bool skip_empty) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      if (!cur.empty() || !skip_empty) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty() || !skip_empty) out.push_back(cur);
  return out;
}

std::string to_hex(const std::vector<uint8_t>& b) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(b.size() * 2);
  for (uint8_t v : b) {
    s += d[v >> 4];
    s += d[v & 15];
  }
  return s;
}

bool from_hex(const std::string& in, std::vector<uint8_t>* out) {
  std::string s;
  size_t i = 0;
  if (in.size() >= 2 && in[0] == '0' && (in[1] == 'x' || in[1] == 'X')) i = 2;
  for (; i < in.size(); ++i) {
    const char c = in[i];
    if (c == ' ' || c == ':' || c == '-' || c == '\t') continue;
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    s += c;
  }
  if (s.size() % 2) return false;
  out->clear();
  for (size_t k = 0; k < s.size(); k += 2)
    out->push_back(static_cast<uint8_t>(std::strtoul(s.substr(k, 2).c_str(), nullptr, 16)));
  return true;
}

std::string printable(const std::vector<uint8_t>& b) {
  size_t n = b.size();
  while (n > 0 && b[n - 1] == 0) --n;  // C strings from firmware often carry their terminator
  // One byte that happens to be printable is a number far more often than a one-letter string
  // (a battery level of 90 % is "Z").
  if (n < 2) return {};
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = b[i];
    // Printable ASCII, or any byte of a UTF-8 sequence: a name in another script is still text.
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return {};
    if (c == 0x7f) return {};
  }
  return std::string(b.begin(), b.begin() + static_cast<long>(n));
}

bool parse_uint(const std::string& s, uint64_t max, uint64_t* out) {
  if (s.empty()) return false;
  int base = 10;
  size_t i = 0;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    i = 2;
  }
  for (size_t k = i; k < s.size(); ++k) {
    const unsigned char c = static_cast<unsigned char>(s[k]);
    if (base == 10 ? !std::isdigit(c) : !std::isxdigit(c)) return false;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s.c_str() + i, &end, base);
  if (errno || !end || *end || v > max) return false;
  *out = v;
  return true;
}

std::map<std::string, std::string> parse_env(const std::string& text) {
  std::map<std::string, std::string> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (starts_with(line, "export ")) line = trim(line.substr(7));
    const size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    const std::string key = trim(line.substr(0, eq));
    std::string v = trim(line.substr(eq + 1));
    std::string val;
    if (!v.empty() && (v[0] == '"' || v[0] == '\'')) {
      const char q = v[0];
      for (size_t i = 1; i < v.size(); ++i) {
        if (v[i] == q) break;
        if (q == '"' && v[i] == '\\' && i + 1 < v.size()) {
          val += v[++i];
          continue;
        }
        val += v[i];
      }
    } else {
      // Unquoted: up to a comment.
      const size_t hash = v.find(" #");
      val = trim(hash == std::string::npos ? v : v.substr(0, hash));
    }
    out[key] = val;
  }
  return out;
}

std::string env_quote(const std::string& v) {
  std::string s = "\"";
  for (char c : v) {
    if (c == '"' || c == '\\' || c == '$' || c == '`') s += '\\';
    s += c;
  }
  return s + "\"";
}

std::vector<std::string> split_args(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  bool have = false;
  char q = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (q) {
      if (c == q) q = 0;
      else cur += c;
      continue;
    }
    if (c == '"' || c == '\'') {
      q = c;
      have = true;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (have) out.push_back(cur);
      cur.clear();
      have = false;
    } else {
      cur += c;
      have = true;
    }
  }
  if (have) out.push_back(cur);
  return out;
}

}  // namespace btb
