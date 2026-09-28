#pragma once

#include "loudness.h"

#include <obs.hpp>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The audio side of a session: one OBS source per requested track, a live mix of all of them on
// track 0 (what clips and replays play), the individual tracks after it (kept for re-mixing),
// volume meters, and the loudness of the mix.
class AudioTracks {
 public:
  ~AudioTracks();

  // `specs`: [{id, kind: "process", executable | window} | {id, kind: "input", deviceId}, volume?, mono?].
  bool create(obs_data_array_t *specs, std::string &error);
  // AAC encoders for track 0 (mix) and every source, in stream order.
  bool create_encoders(std::string &error);
  void release();

  bool set_volume(const std::string &id, double volume);
  const std::vector<OBSEncoderAutoRelease> &encoders() const { return encoders_; }
  // Stream order: "mix", then the source ids.
  std::vector<std::string> track_ids() const;
  bool empty() const { return tracks_.empty(); }

  // Loudness of the mix since a wall-clock instant (0 = since recording started).
  void start_loudness();
  void stop_loudness();
  double loudness_lufs(double since_wall_ms) const;
  double peak_dbfs(double since_wall_ms) const;

 private:
  struct Track {
    std::string id;
    OBSSourceAutoRelease source;
    obs_volmeter_t *volmeter = nullptr;
    std::atomic<float> peak_db{-INFINITY};
  };

  static void on_volume(void *param, const float magnitude[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
                        const float input_peak[MAX_AUDIO_CHANNELS]);
  static void on_mix_audio(void *param, size_t mix_idx, struct audio_data *data);
  void levels_loop();
  double stream_ms(double wall_ms) const;

  std::vector<std::unique_ptr<Track>> tracks_;
  std::vector<OBSEncoderAutoRelease> encoders_;

  std::mutex levels_mutex_;
  std::thread levels_thread_;
  std::atomic<bool> levels_running_{false};

  LoudnessMeter loudness_;
  bool loudness_attached_ = false;
  std::atomic<double> stream_start_wall_ms_{0};
};
