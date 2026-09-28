#include "loudness.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kAbsoluteGateLufs = -70.0;
constexpr double kRelativeGateLu = -10.0;
constexpr size_t kHopsPerBlock = 4;

double to_lufs(double energy) {
  return energy > 0 ? -0.691 + 10.0 * std::log10(energy) : -std::numeric_limits<double>::infinity();
}

}  // namespace

double LoudnessMeter::Biquad::process(double x) {
  const double y = b0 * x + z1;
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  return y;
}

void LoudnessMeter::reset(uint32_t sample_rate, size_t channels) {
  std::lock_guard lock(mutex_);
  sample_rate_ = sample_rate;
  channels_ = channels;
  hop_frames_ = sample_rate / 10;

  // K-weighting, BS.1770-4 Annex 1 (coefficients given for 48 kHz, which is what libobs runs at).
  shelf_.assign(channels, Biquad{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
                                 0.73248077421585});
  highpass_.assign(channels, Biquad{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621});
  hop_energy_.assign(channels, 0.0);
  hop_filled_ = 0;
  recent_hops_.clear();
  block_energy_.clear();
  hop_peak_.clear();
  current_peak_ = 0;
}

void LoudnessMeter::add(const float *const *planes, size_t channels, size_t frames) {
  std::lock_guard lock(mutex_);
  channels = std::min(channels, channels_);
  for (size_t frame = 0; frame < frames; frame++) {
    for (size_t ch = 0; ch < channels; ch++) {
      const double sample = planes[ch][frame];
      current_peak_ = std::max(current_peak_, std::abs(sample));
      const double weighted = highpass_[ch].process(shelf_[ch].process(sample));
      hop_energy_[ch] += weighted * weighted;
    }
    if (++hop_filled_ < hop_frames_) {
      continue;
    }
    // Stereo channels all weigh 1 in BS.1770; surround is out of scope here.
    double hop = 0;
    for (size_t ch = 0; ch < channels_; ch++) {
      hop += hop_energy_[ch];
      hop_energy_[ch] = 0;
    }
    hop_filled_ = 0;
    hop_peak_.push_back(float(current_peak_));
    current_peak_ = 0;
    recent_hops_.push_back(hop);
    if (recent_hops_.size() > kHopsPerBlock) {
      recent_hops_.erase(recent_hops_.begin());
    }
    if (recent_hops_.size() == kHopsPerBlock) {
      double block = 0;
      for (double value : recent_hops_) {
        block += value;
      }
      block_energy_.push_back(block / double(hop_frames_ * kHopsPerBlock));
    }
  }
}

double LoudnessMeter::integrated_lufs(double from_ms) const {
  std::lock_guard lock(mutex_);
  // Block i covers [i * 100 ms, i * 100 ms + 400 ms).
  const size_t first = from_ms > 0 ? size_t(from_ms / 100.0) : 0;
  double sum = 0;
  size_t count = 0;
  for (size_t i = first; i < block_energy_.size(); i++) {
    if (to_lufs(block_energy_[i]) > kAbsoluteGateLufs) {
      sum += block_energy_[i];
      count++;
    }
  }
  if (count == 0) {
    return -std::numeric_limits<double>::infinity();
  }
  const double relative_gate = to_lufs(sum / count) + kRelativeGateLu;
  sum = 0;
  count = 0;
  for (size_t i = first; i < block_energy_.size(); i++) {
    const double lufs = to_lufs(block_energy_[i]);
    if (lufs > kAbsoluteGateLufs && lufs > relative_gate) {
      sum += block_energy_[i];
      count++;
    }
  }
  return count ? to_lufs(sum / count) : -std::numeric_limits<double>::infinity();
}

double LoudnessMeter::peak_dbfs(double from_ms) const {
  std::lock_guard lock(mutex_);
  const size_t first = from_ms > 0 ? size_t(from_ms / 100.0) : 0;
  double peak = current_peak_;
  for (size_t i = first; i < hop_peak_.size(); i++) {
    peak = std::max(peak, double(hop_peak_[i]));
  }
  return peak > 0 ? 20.0 * std::log10(peak) : -std::numeric_limits<double>::infinity();
}
