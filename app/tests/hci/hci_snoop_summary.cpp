// hci_snoop_summary FILE.btsnoop — run a capture through the decoder as fast as it reads and
// print what the Monitor tab would have shown: events, and per link the busiest second.
// A development tool for checking the decoder against real captures; not a test.

#include <chrono>
#include <cstdio>
#include <map>
#include <string>

#include "btsnoop.h"
#include "decoder.h"

using nlohmann::json;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s FILE.btsnoop [max-events]\n", argv[0]);
    return 2;
  }
  const size_t max_events = argc > 2 ? std::stoul(argv[2]) : 60;
  btb::hci::BtsnoopReader r;
  std::string err;
  if (!r.open(argv[1], &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }

  struct Peak {
    std::string desc;
    double tx_bps = 0, rx_bps = 0, p95 = 0, max = 0;
    int in_flight = 0, credits_min = -1, lat_n = 0;
    uint64_t rtp_lost = 0;
    double jitter = 0;
    std::string media;
  };
  std::map<std::string, Peak> peaks;
  std::map<std::string, size_t> kinds;
  size_t printed = 0;

  btb::hci::Decoder d;
  btb::hci::SnoopPacket p;
  uint64_t last_closed = 0;
  int64_t last_ts = 0;
  const auto t0 = std::chrono::steady_clock::now();

  auto collect = [&]() {
    for (const auto& c : d.stats()["conns"]) {
      const std::string key = std::to_string(c["index"].get<int>()) + "/" +
                              std::to_string(c["handle"].get<int>()) + " " +
                              c["type"].get<std::string>() + " " + c["peer"].get<std::string>();
      Peak& pk = peaks[key];
      pk.tx_bps = std::max(pk.tx_bps, c["tx_bps"].get<double>());
      pk.rx_bps = std::max(pk.rx_bps, c["rx_bps"].get<double>());
      pk.in_flight = std::max(pk.in_flight, c["in_flight_max"].get<int>());
      if (c["credits_min"].is_number()) {
        const int cm = c["credits_min"].get<int>();
        pk.credits_min = pk.credits_min < 0 ? cm : std::min(pk.credits_min, cm);
      }
      pk.lat_n += c["lat_ms"]["n"].get<int>();
      pk.p95 = std::max(pk.p95, c["lat_ms"]["p95"].get<double>());
      pk.max = std::max(pk.max, c["lat_ms"]["max"].get<double>());
      for (const auto& ch : c["channels"]) {
        if (!ch.contains("avdtp") || ch["avdtp"]["role"] != "media") continue;
        pk.rtp_lost += ch["avdtp"]["rtp_lost"].get<uint64_t>();
        if (ch["avdtp"]["rtp_jitter_ms"].is_number()) {
          pk.jitter = std::max(pk.jitter, ch["avdtp"]["rtp_jitter_ms"].get<double>());
        }
        pk.media = ch["avdtp"]["codec"].get<std::string>() + " " +
                   ch["avdtp"]["config"].get<std::string>() + " (" +
                   ch["avdtp"]["state"].get<std::string>() + ")";
      }
    }
  };
  auto drain_events = [&]() {
    for (const auto& e : d.take_new_events()) {
      ++kinds[e.kind];
      if (printed++ < max_events) {
        std::printf("  %lld.%03lld  [%s] %s%s\n", static_cast<long long>(e.ts_ms / 1000),
                    static_cast<long long>(e.ts_ms % 1000), e.kind.c_str(),
                    e.handle >= 0 ? ("h" + std::to_string(e.handle) + " ").c_str() : "",
                    e.text.c_str());
      }
    }
  };

  std::printf("events (first %zu):\n", max_events);
  while (r.read(&p)) {
    d.packet(p.ts_us, p.index, p.opcode, p.data, p.len);
    last_ts = p.ts_us;
    if (d.windows_closed() != last_closed) {
      last_closed = d.windows_closed();
      collect();
    }
    drain_events();
  }
  d.flush(last_ts + 1);
  collect();
  drain_events();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  std::printf("\n%llu packets, %llu windows, decoded in %.2f s\nevents:",
              static_cast<unsigned long long>(d.packets()),
              static_cast<unsigned long long>(d.windows_closed()), secs);
  for (const auto& [k, n] : kinds) std::printf(" %s=%zu", k.c_str(), n);
  std::printf("\n\nper link, peak second:\n");
  for (const auto& [k, pk] : peaks) {
    std::printf("  %-36s tx %7.0f kbit/s rx %7.0f kbit/s  lat n=%d p95<=%.1f max %.1f ms  "
                "in-flight<=%d credits>=%d",
                k.c_str(), pk.tx_bps / 1000, pk.rx_bps / 1000, pk.lat_n, pk.p95, pk.max,
                pk.in_flight, pk.credits_min);
    if (!pk.media.empty()) {
      std::printf("\n  %-36s media %s, rtp lost %llu, jitter<=%.2f ms", "", pk.media.c_str(),
                  static_cast<unsigned long long>(pk.rtp_lost), pk.jitter);
    }
    std::printf("\n");
  }
  std::printf("\nfinal stats:\n%s\n", d.stats().dump(1).c_str());
  return 0;
}
