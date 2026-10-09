#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace btb {

using WsMessagePtr = std::shared_ptr<const std::string>;

// Fan-out for the push-only WebSocket /api/ws?topics=a,b.
//
// Every message has a topic ("bt", "gatt.notify", "hci.stats", "job.3", ...) and goes only to the
// clients subscribed to it. It is serialized once, as {"topic":...,"data":...}, and dropped into
// each subscriber's own queue; the connection's own thread is the only thing that writes to its
// socket, so there is no cross-thread send race. A client that cannot keep up loses its oldest
// frames rather than stalling the publisher.
//
// has_subscribers() is what keeps the Zero W idle: a producer asks before it builds a message, and
// with nobody watching (the usual case — the board sits on a desk) it does no work at all.
class WsHub {
 public:
  struct Client {
    std::mutex m;
    std::condition_variable cv;
    std::deque<WsMessagePtr> q;
    std::vector<std::string> topics;  // patterns, see topic_matches()
    bool closed = false;
  };
  using ClientPtr = std::shared_ptr<Client>;

  static constexpr size_t kMaxQueued = 32;

  // "a,b,c" → {"a","b","c"}; empty items and surrounding spaces dropped. An absent parameter is
  // the caller's business (the server treats it as "*").
  static std::vector<std::string> parse_topics(const std::string& csv);
  // A pattern is a topic ("hci.stats"), a prefix wildcard ("job.*": job.1, job.2, ... but not
  // "job" itself), or "*" for everything.
  static bool topic_matches(const std::string& pattern, const std::string& topic);

  ClientPtr add(std::vector<std::string> topics);
  void remove(const ClientPtr& c);
  // Replaces a client's subscriptions (a message the client sends: {"topics":[...]}).
  void set_topics(const ClientPtr& c, std::vector<std::string> topics);

  bool has_subscribers(const std::string& topic) const;
  // Serialized only when someone is subscribed. Returns whether anyone was.
  bool publish(const std::string& topic, const nlohmann::json& data);
  void shutdown();

  size_t clients() const;

  // Blocks until a message is queued, the client is removed, or the timeout expires.
  // Returns nullptr on timeout or close.
  WsMessagePtr wait(const ClientPtr& c, int timeout_ms);

 private:
  static bool wants(const std::vector<std::string>& patterns, const std::string& topic);

  mutable std::mutex m_;
  std::vector<ClientPtr> clients_;
};

}  // namespace btb
