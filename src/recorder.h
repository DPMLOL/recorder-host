#pragma once

#include <obs.hpp>

#include <string>

class Recorder {
 public:
  bool init();
  void shutdown();

  void handle(obs_data_t *command);

 private:
  void info(long long id);
  void start(long long id, obs_data_t *params);
  void stop(long long id);
  void release_session();

  bool reset_video(uint32_t width, uint32_t height, uint32_t fps);
  obs_source_t *create_video_source(obs_data_t *video, std::string &error);
  static const char *pick_video_encoder(const char *requested);

  static void on_packet(obs_output_t *output, encoder_packet *pkt, encoder_packet_time *pkt_time, void *param);
  static void on_output_start(void *param, calldata_t *cd);
  static void on_output_stop(void *param, calldata_t *cd);
  static void on_hooked(void *param, calldata_t *cd);
  static void on_unhooked(void *param, calldata_t *cd);

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t fps_ = 0;

  OBSSceneAutoRelease scene_;
  OBSSourceAutoRelease video_source_;
  OBSSourceAutoRelease game_audio_;
  OBSSourceAutoRelease mic_;
  OBSEncoderAutoRelease video_encoder_;
  OBSEncoderAutoRelease audio_encoders_[2];
  OBSOutputAutoRelease output_;

  // Declared after what they watch so they disconnect first.
  OBSSignal output_start_;
  OBSSignal output_stop_;
  OBSSignal hooked_;
  OBSSignal unhooked_;

  long long keyframes_ = 0;
  bool anchored_ = false;
};
