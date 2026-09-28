#include "audio_tracks.h"

#include "protocol.h"
#include "wall_clock.h"

#include <util/windows/window-helpers.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

// Channel 0 is the video scene; audio sources follow.
constexpr uint32_t kFirstAudioChannel = 1;
constexpr size_t kMixTrack = 0;
constexpr auto kLevelsInterval = std::chrono::milliseconds(250);

}  // namespace

AudioTracks::~AudioTracks() {
  release();
}

bool AudioTracks::create(obs_data_array_t *specs, std::string &error) {
  release();
  const size_t count = specs ? obs_data_array_count(specs) : 0;
  // Track 0 is the mix, so one of OBS's six mixes is taken.
  if (count > MAX_AUDIO_MIXES - 1) {
    error = "at most " + std::to_string(MAX_AUDIO_MIXES - 1) + " audio tracks";
    return false;
  }

  for (size_t index = 0; index < count; index++) {
    OBSDataAutoRelease entry = obs_data_array_item(specs, index);
    const std::string id = obs_data_get_string(entry, "id");
    const char *kind = obs_data_get_string(entry, "kind");
    if (id.empty() || id == "mix") {
      error = "audio track needs an id other than \"mix\"";
      release();
      return false;
    }

    OBSDataAutoRelease settings = obs_data_create();
    const char *source_id = nullptr;
    if (strcmp(kind, "process") == 0) {
      // title:class:exe; with exe priority an empty title and class match any window of the executable.
      std::string window = obs_data_get_string(entry, "window");
      if (window.empty()) {
        window = std::string("::") + obs_data_get_string(entry, "executable");
      }
      obs_data_set_string(settings, "window", window.c_str());
      obs_data_set_int(settings, "priority", WINDOW_PRIORITY_EXE);
      source_id = "wasapi_process_output_capture";
    } else if (strcmp(kind, "input") == 0) {
      const char *device = obs_data_get_string(entry, "deviceId");
      obs_data_set_string(settings, "device_id", *device ? device : "default");
      source_id = "wasapi_input_capture";
    } else {
      error = "audio track " + id + ": unknown kind";
      release();
      return false;
    }

    auto track = std::make_unique<Track>();
    track->id = id;
    track->source = obs_source_create(source_id, ("audio:" + id).c_str(), settings, nullptr);
    // A mic on input 1 of a stereo interface (Focusrite "Analogue 1 + 2") only fills the left channel.
    obs_data_set_default_bool(entry, "mono", strcmp(kind, "input") == 0);
    if (obs_data_get_bool(entry, "mono")) {
      obs_source_set_flags(track->source, obs_source_get_flags(track->source) | OBS_SOURCE_FLAG_FORCE_MONO);
    }
    obs_data_set_default_double(entry, "volume", 1.0);
    obs_source_set_volume(track->source, (float)std::clamp(obs_data_get_double(entry, "volume"), 0.0, 2.0));
    // Into the mix, and onto its own track.
    obs_source_set_audio_mixers(track->source, (1u << kMixTrack) | (1u << (index + 1)));
    obs_set_output_source((uint32_t)(kFirstAudioChannel + index), track->source);

    track->volmeter = obs_volmeter_create(OBS_FADER_LOG);
    obs_volmeter_attach_source(track->volmeter, track->source);
    obs_volmeter_add_callback(track->volmeter, on_volume, track.get());
    tracks_.push_back(std::move(track));
  }

  if (!tracks_.empty()) {
    levels_running_ = true;
    levels_thread_ = std::thread(&AudioTracks::levels_loop, this);
  }
  return true;
}

bool AudioTracks::create_encoders(std::string &error) {
  encoders_.clear();
  if (tracks_.empty()) {
    return true;
  }
  OBSDataAutoRelease aac = obs_data_create();
  obs_data_set_int(aac, "bitrate", 160);
  const std::vector<std::string> ids = track_ids();
  for (size_t mixer = 0; mixer < ids.size(); mixer++) {
    // The encoder name becomes the stream title in formats that carry one.
    obs_encoder_t *encoder = obs_audio_encoder_create("ffmpeg_aac", ids[mixer].c_str(), aac, mixer, nullptr);
    if (!encoder) {
      error = "audio encoder creation failed";
      encoders_.clear();
      return false;
    }
    obs_encoder_set_audio(encoder, obs_get_audio());
    encoders_.emplace_back(encoder);
  }
  return true;
}

void AudioTracks::release() {
  stop_loudness();
  if (levels_running_.exchange(false) && levels_thread_.joinable()) {
    levels_thread_.join();
  }
  encoders_.clear();
  for (size_t index = 0; index < tracks_.size(); index++) {
    Track &track = *tracks_[index];
    if (track.volmeter) {
      obs_volmeter_remove_callback(track.volmeter, on_volume, &track);
      obs_volmeter_destroy(track.volmeter);
    }
    obs_set_output_source((uint32_t)(kFirstAudioChannel + index), nullptr);
  }
  tracks_.clear();
}

bool AudioTracks::set_volume(const std::string &id, double volume) {
  for (const auto &track : tracks_) {
    if (track->id == id) {
      obs_source_set_volume(track->source, (float)std::clamp(volume, 0.0, 2.0));
      return true;
    }
  }
  return false;
}

std::vector<std::string> AudioTracks::track_ids() const {
  std::vector<std::string> ids;
  if (tracks_.empty()) {
    return ids;
  }
  ids.emplace_back("mix");
  for (const auto &track : tracks_) {
    ids.push_back(track->id);
  }
  return ids;
}

void AudioTracks::start_loudness() {
  if (tracks_.empty() || loudness_attached_) {
    return;
  }
  const audio_output_info *info = audio_output_get_info(obs_get_audio());
  loudness_.reset(info->samples_per_sec, audio_output_get_channels(obs_get_audio()));
  stream_start_wall_ms_ = 0;
  obs_add_raw_audio_callback(kMixTrack, nullptr, on_mix_audio, this);
  loudness_attached_ = true;
}

void AudioTracks::stop_loudness() {
  if (!loudness_attached_) {
    return;
  }
  obs_remove_raw_audio_callback(kMixTrack, on_mix_audio, this);
  loudness_attached_ = false;
}

double AudioTracks::stream_ms(double wall_ms) const {
  const double start = stream_start_wall_ms_;
  return wall_ms > 0 && start > 0 ? std::max(0.0, wall_ms - start) : 0;
}

double AudioTracks::loudness_lufs(double since_wall_ms) const {
  return loudness_.integrated_lufs(stream_ms(since_wall_ms));
}

double AudioTracks::peak_dbfs(double since_wall_ms) const {
  return loudness_.peak_dbfs(stream_ms(since_wall_ms));
}

// Audio thread: libobs' mix, float planar at the output rate.
void AudioTracks::on_mix_audio(void *param, size_t, struct audio_data *data) {
  auto *self = static_cast<AudioTracks *>(param);
  if (self->stream_start_wall_ms_ == 0) {
    self->stream_start_wall_ms_ = obs_ns_to_wall_ms(data->timestamp);
  }
  const float *planes[MAX_AV_PLANES] = {};
  size_t channels = 0;
  for (; channels < MAX_AV_PLANES && data->data[channels]; channels++) {
    planes[channels] = reinterpret_cast<const float *>(data->data[channels]);
  }
  self->loudness_.add(planes, channels, data->frames);
}

void AudioTracks::on_volume(void *param, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
                            const float[MAX_AUDIO_CHANNELS]) {
  auto *track = static_cast<Track *>(param);
  float loudest = -INFINITY;
  for (size_t ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
    if (std::isfinite(peak[ch])) {
      loudest = std::max(loudest, peak[ch]);
    }
  }
  track->peak_db = std::max(track->peak_db.load(), loudest);
}

// The loudest peak of each track since the previous report, 4x per second.
void AudioTracks::levels_loop() {
  while (levels_running_) {
    std::this_thread::sleep_for(kLevelsInterval);
    OBSDataAutoRelease event = obs_data_create();
    obs_data_set_string(event, "event", "levels");
    OBSDataAutoRelease levels = obs_data_create();
    for (const auto &track : tracks_) {
      const float db = track->peak_db.exchange(-INFINITY);
      obs_data_set_double(levels, track->id.c_str(), std::isfinite(db) ? db : -100.0);
    }
    obs_data_set_obj(event, "levels", levels);
    protocol::send(event);
  }
}
