#include "ws_hub.h"

#include <algorithm>
#include <chrono>

#include "util/strings.h"

namespace btb {

std::vector<std::string> WsHub::parse_topics(const std::string& csv) {
  std::vector<std::string> out;
  for (const std::string& t : split(csv, ',')) {
    const std::string s = trim(t);
    if (!s.empty()) out.push_back(s);
  }
  return out;
}

bool WsHub::topic_matches(const std::string& pattern, const std::string& topic) {
  if (pattern == "*" || pattern == topic) return true;
  if (pattern.size() >= 2 && ends_with(pattern, ".*")) {
    const std::string prefix = pattern.substr(0, pattern.size() - 1);  // keeps the dot
    return topic.size() > prefix.size() && starts_with(topic, prefix);
  }
  return false;
}

bool WsHub::wants(const std::vector<std::string>& patterns, const std::string& topic) {
  for (const std::string& p : patterns)
    if (topic_matches(p, topic)) return true;
  return false;
}

WsHub::ClientPtr WsHub::add(std::vector<std::string> topics) {
  auto c = std::make_shared<Client>();
  c->topics = std::move(topics);
  std::lock_guard<std::mutex> lock(m_);
  clients_.push_back(c);
  return c;
}

void WsHub::remove(const ClientPtr& c) {
  {
    std::lock_guard<std::mutex> lock(m_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), c), clients_.end());
  }
  std::lock_guard<std::mutex> lock(c->m);
  c->closed = true;
  c->cv.notify_all();
}

void WsHub::set_topics(const ClientPtr& c, std::vector<std::string> topics) {
  // Under the hub lock as well: has_subscribers() reads every client's list under it alone.
  std::lock_guard<std::mutex> lock(m_);
  std::lock_guard<std::mutex> lk(c->m);
  c->topics = std::move(topics);
}

bool WsHub::has_subscribers(const std::string& topic) const {
  std::lock_guard<std::mutex> lock(m_);
  for (const ClientPtr& c : clients_) {
    std::lock_guard<std::mutex> lk(c->m);
    if (!c->closed && wants(c->topics, topic)) return true;
  }
  return false;
}

bool WsHub::publish(const std::string& topic, const nlohmann::json& data) {
  std::vector<ClientPtr> to;
  {
    std::lock_guard<std::mutex> lock(m_);
    for (const ClientPtr& c : clients_) {
      std::lock_guard<std::mutex> lk(c->m);
      if (!c->closed && wants(c->topics, topic)) to.push_back(c);
    }
  }
  if (to.empty()) return false;
  // dump() with error_handler::replace: a device name is attacker-controlled bytes, and invalid
  // UTF-8 in it must cost one replacement character, not an exception on the publisher's thread.
  const nlohmann::json msg{{"topic", topic}, {"data", data}};
  auto s = std::make_shared<const std::string>(
      msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  for (const ClientPtr& c : to) {
    std::lock_guard<std::mutex> lk(c->m);
    if (c->closed) continue;
    if (c->q.size() >= kMaxQueued) c->q.pop_front();
    c->q.push_back(s);
    c->cv.notify_one();
  }
  return true;
}

void WsHub::shutdown() {
  std::vector<ClientPtr> snapshot;
  {
    std::lock_guard<std::mutex> lock(m_);
    snapshot = clients_;
    clients_.clear();
  }
  for (const ClientPtr& c : snapshot) {
    std::lock_guard<std::mutex> lk(c->m);
    c->closed = true;
    c->cv.notify_all();
  }
}

size_t WsHub::clients() const {
  std::lock_guard<std::mutex> lock(m_);
  return clients_.size();
}

WsMessagePtr WsHub::wait(const ClientPtr& c, int timeout_ms) {
  std::unique_lock<std::mutex> lock(c->m);
  if (c->q.empty() && !c->closed) {
    c->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                   [&c] { return !c->q.empty() || c->closed; });
  }
  if (c->q.empty()) return nullptr;
  WsMessagePtr m = c->q.front();
  c->q.pop_front();
  return m;
}

}  // namespace btb
