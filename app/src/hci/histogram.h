#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace btb::hci {

// Latency distribution in fixed 1 ms buckets, 0..kBuckets ms, plus one overflow bucket.
//
// Percentiles come from the bucket counts rather than from sorted samples: at A2DP rates there are
// a few hundred completions a second per link, and keeping/sorting them on a 1 GHz ARM11 that also
// runs bluetoothd and PipeWire would be wasted work for a number shown with 1 ms resolution. min,
// max and the mean are tracked exactly, and a percentile is clamped into [min, max], so a window
// whose samples all fall in one bucket reports a value that was actually observed.
class LatencyHist {
 public:
  static constexpr int kBuckets = 500;  // [0,1) .. [499,500) ms; >= 500 ms is overflow

  void add_us(int64_t us) {
    if (us < 0) us = 0;
    const int64_t b = us / 1000;
    ++counts_[static_cast<size_t>(b >= kBuckets ? kBuckets : b)];
    if (n_ == 0 || us < min_us_) min_us_ = us;
    if (n_ == 0 || us > max_us_) max_us_ = us;
    sum_us_ += us;
    ++n_;
  }

  void clear() {
    if (n_ == 0) return;  // the common idle case: skip touching 2 KB
    counts_.fill(0);
    n_ = 0;
    sum_us_ = 0;
    min_us_ = max_us_ = 0;
  }

  uint32_t count() const { return n_; }
  double min_ms() const { return static_cast<double>(min_us_) / 1000.0; }
  double max_ms() const { return static_cast<double>(max_us_) / 1000.0; }
  double avg_ms() const {
    return n_ ? static_cast<double>(sum_us_) / static_cast<double>(n_) / 1000.0 : 0.0;
  }

  // Nearest-rank percentile (p in 0..1): the bucket holding the ceil(p*n)-th sample, reported at
  // its midpoint. The overflow bucket has no midpoint; it reports the exact max.
  double percentile_ms(double p) const {
    if (n_ == 0) return 0.0;
    uint64_t rank = static_cast<uint64_t>(p * static_cast<double>(n_) + 0.999999);
    rank = std::max<uint64_t>(1, std::min<uint64_t>(rank, n_));
    uint64_t cum = 0;
    for (int b = 0; b <= kBuckets; ++b) {
      cum += counts_[static_cast<size_t>(b)];
      if (cum < rank) continue;
      if (b == kBuckets) return max_ms();
      const double mid = b + 0.5;
      return std::min(std::max(mid, min_ms()), max_ms());
    }
    return max_ms();
  }

  const std::array<uint32_t, kBuckets + 1>& counts() const { return counts_; }

 private:
  std::array<uint32_t, kBuckets + 1> counts_{};
  uint32_t n_ = 0;
  int64_t sum_us_ = 0;
  int64_t min_us_ = 0;
  int64_t max_us_ = 0;
};

}  // namespace btb::hci
