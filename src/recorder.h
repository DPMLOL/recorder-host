#pragma once

#include "audio_tracks.h"

#include <obs.hpp>

#include <mutex>
#include <string>

class Recorder {
 public:
  bool init();
  void shutdown();

  void handle(obs_data_t *command);

 private:
  struct OutputSlot {
    Recorder *owner = nullptr;
    const char *name = "";
    OBSOutputAutoRelease output;
    OBSSignal start_signal;
    OBSSignal stop_signal;
    bool running = false;
  };

  void info(long long id);
  void start(long long id, obs_data_t *params);
  void stop(long long id);
  void monitor(long long id, obs_data_t *params);
  void set_volume(long long id, obs_data_t *params);
  void loudness(long long id, obs_data_t *params);
  void audio_apps(long long id);
  void release_session();
  bool recording() const;

  bool reset_video(uint32_t width, uint32_t height, uint32_t fps);
  obs_source_t *create_video_source(obs_data_t *video, std::string &error);
  static const char *pick_video_encoder(const char *requested);
  bool start_output(OutputSlot &slot, const char *type, obs_data_t *settings, std::string &error);

  void close_segment(double end_wall_ms);

  static void on_packet(obs_output_t *output, encoder_packet *pkt, encoder_packet_time *pkt_time, void *param);
  static void on_output_start(void *param, calldata_t *cd);
  static void on_output_stop(void *param, calldata_t *cd);
  static void on_file_changed(void *param, calldata_t *cd);
  static void on_hooked(void *param, calldata_t *cd);
  static void on_unhooked(void *param, calldata_t *cd);

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t fps_ = 0;

  OBSSceneAutoRelease scene_;
  OBSSourceAutoRelease video_source_;
  AudioTracks audio_;
  OBSEncoderAutoRelease video_encoder_;
  // The ring clips are cut from, and the whole game written as it plays (the replay).
  OutputSlot segments_{this, "segments"};
  OutputSlot vod_{this, "vod"};
  std::string vod_path_;

  // Written on the output thread (packets, file changes), read at stop.
  std::mutex clock_mutex_;
  long long keyframes_ = 0;
  bool anchored_ = false;
  double last_keyframe_wall_ms_ = 0;
  double last_video_wall_ms_ = 0;
  std::string segment_file_;
  double segment_start_wall_ms_ = 0;

  // Declared after what they watch so they disconnect first.
  OBSSignal file_changed_;
  OBSSignal hooked_;
  OBSSignal unhooked_;
};
