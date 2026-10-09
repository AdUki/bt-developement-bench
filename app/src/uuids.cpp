#include "uuids.h"

#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "util/strings.h"

namespace btb {

namespace {

constexpr const char* kBaseSuffix = "-0000-1000-8000-00805f9b34fb";

bool all_hex(const std::string& s) {
  for (char c : s)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  return !s.empty();
}

}  // namespace

std::string uuid_full(const std::string& in) {
  std::string s = lower(trim(in));
  if (starts_with(s, "0x")) s = s.substr(2);
  if (s.size() == 36) {
    for (size_t i = 0; i < s.size(); ++i) {
      const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
      if (dash ? s[i] != '-' : !std::isxdigit(static_cast<unsigned char>(s[i]))) return {};
    }
    return s;
  }
  if (s.size() == 32 && all_hex(s)) {
    return s.substr(0, 8) + "-" + s.substr(8, 4) + "-" + s.substr(12, 4) + "-" + s.substr(16, 4) + "-" +
           s.substr(20);
  }
  if ((s.size() == 4 || s.size() == 8) && all_hex(s))
    return std::string(8 - s.size(), '0') + s + kBaseSuffix;
  return {};
}

std::string uuid_short(const std::string& in) {
  const std::string f = uuid_full(in);
  if (f.empty()) return lower(in);
  if (!ends_with(f, kBaseSuffix)) return f;
  if (starts_with(f, "0000")) return f.substr(4, 4);
  return f.substr(0, 8);
}

bool UuidNames::load_text(const std::string& text, std::string* err) {
  std::map<std::string, std::string> n;
  try {
    const nlohmann::json j = nlohmann::json::parse(text);
    for (const char* sec : {"uuid16", "uuid128"}) {
      if (!j.contains(sec)) continue;
      for (auto it = j[sec].begin(); it != j[sec].end(); ++it) {
        const std::string u = uuid_full(it.key());
        if (!u.empty() && it->is_string()) n[u] = it->get<std::string>();
      }
    }
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }
  std::lock_guard<std::mutex> lk(m_);
  names_ = std::move(n);
  return true;
}

bool UuidNames::load(const std::string& path, std::string* err) {
  std::ifstream f(path);
  if (!f) {
    if (err) *err = "cannot read " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return load_text(ss.str(), err);
}

std::string UuidNames::name(const std::string& uuid) const {
  const std::string u = uuid_full(uuid);
  std::lock_guard<std::mutex> lk(m_);
  const auto it = names_.find(u);
  return it == names_.end() ? std::string{} : it->second;
}

size_t UuidNames::size() const {
  std::lock_guard<std::mutex> lk(m_);
  return names_.size();
}

UuidNames& uuid_names() {
  static UuidNames n;
  return n;
}

}  // namespace btb
