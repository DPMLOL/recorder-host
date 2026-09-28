#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// Integrated loudness (ITU-R BS.1770-4 / EBU R128) and sample peak of a stream fed live, so a
// recording knows its normalisation gain the moment it stops instead of decoding it again.
class LoudnessMeter {
 public:
  void reset(uint32_t sample_rate, size_t channels);
  // Planar float samples, as libobs hands them out.
  void add(const float *const *planes, size_t channels, size_t frames);

  // Loudness of what was fed since `from_ms` of stream time (0 = everything); -inf when silent.
  double integrated_lufs(double from_ms = 0) const;
  // Sample peak since `from_ms` of stream time.
  double peak_dbfs(double from_ms = 0) const;

 private:
  struct Biquad {
    double b0, b1, b2, a1, a2;
    double z1 = 0, z2 = 0;
    double process(double x);
  };

  mutable std::mutex mutex_;
  uint32_t sample_rate_ = 48000;
  size_t channels_ = 2;
  size_t hop_frames_ = 4800;  // 100 ms
  std::vector<Biquad> shelf_, highpass_;
  std::vector<double> hop_energy_;    // per channel, current 100 ms hop
  size_t hop_filled_ = 0;
  std::vector<double> recent_hops_;   // summed channel energy of the last hops (a block is 4 of them)
  std::vector<double> block_energy_;  // mean-square energy of every 400 ms block, 75 % overlap
  std::vector<float> hop_peak_;      // sample peak of every 100 ms hop
  double current_peak_ = 0;
};
