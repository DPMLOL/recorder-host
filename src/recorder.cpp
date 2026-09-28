#include "recorder.h"

#include "protocol.h"
#include "wall_clock.h"

#include <util/windows/window-helpers.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {

constexpr char kLeagueWindow[] = "League of Legends (TM) Client:RiotWindowClass:League of Legends.exe";
constexpr uint32_t kDefaultWidth = 1920;
constexpr uint32_t kDefaultHeight = 1080;
constexpr uint32_t kDefaultFps = 60;
constexpr long long kKeyframesPerAnchorCheck = 30;
// Channel 0 is the scene; audio sources follow.
constexpr uint32_t kFirstAudioChannel = 1;

std::mutex stop_mutex;
std::condition_variable stop_cv;
bool output_stopped = true;

bool encoder_registered(const char *id) {
  const char *type = nullptr;
  for (size_t i = 0; obs_enum_encoder_types(i, &type); i++) {
    if (strcmp(type, id) == 0) {
      return true;
    }
  }
  return false;
}

// Reads the first value of a list property, e.g. the first monitor of monitor_capture.
std::string first_list_value(const char *source_id, const char *property) {
  obs_properties_t *props = obs_get_source_properties(source_id);
  std::string value;
  if (obs_property_t *p = obs_properties_get(props, property); p && obs_property_list_item_count(p) > 0) {
    value = obs_property_list_item_string(p, 0);
  }
  obs_properties_destroy(props);
  return value;
}

void append_devices(obs_data_t *target, const char *key, const char *source_id) {
  OBSDataArrayAutoRelease devices = obs_data_array_create();
  obs_properties_t *props = obs_get_source_properties(source_id);
  if (obs_property_t *p = obs_properties_get(props, "device_id")) {
    for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
      OBSDataAutoRelease device = obs_data_create();
      obs_data_set_string(device, "id", obs_property_list_item_string(p, i));
      obs_data_set_string(device, "name", obs_property_list_item_name(p, i));
      obs_data_array_push_back(devices, device);
    }
  }
  obs_properties_destroy(props);
  obs_data_set_array(target, key, devices);
}

std::string base_name(const std::string &path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

bool Recorder::init() {
  if (!obs_startup("en-US", nullptr, nullptr)) {
    blog(LOG_ERROR, "obs_startup failed");
    return false;
  }

  obs_audio_info audio{};
  audio.samples_per_sec = 48000;
  audio.speakers = SPEAKERS_STEREO;
  if (!obs_reset_audio(&audio)) {
    blog(LOG_ERROR, "obs_reset_audio failed");
    return false;
  }

  if (!reset_video(kDefaultWidth, kDefaultHeight, kDefaultFps)) {
    return false;
  }

  // Allowlist: UI plugins (frontend-tools, decklink-output-ui) abort without a Qt frontend.
  for (const char *module : {"win-capture", "win-wasapi", "obs-ffmpeg", "obs-nvenc", "obs-qsv11", "obs-x264"}) {
    obs_add_safe_module(module);
  }
  obs_load_all_modules();
  obs_log_loaded_modules();
  obs_post_load_modules();
  return true;
}

void Recorder::shutdown() {
  if (output_ && obs_output_active(output_)) {
    obs_output_force_stop(output_);
  }
  release_session();
  // Sources are destroyed on a background queue; shutting down before it drains reports them as leaked.
  obs_wait_for_destroy_queue();
  obs_shutdown();
}

void Recorder::handle(obs_data_t *command) {
  const char *cmd = obs_data_get_string(command, "cmd");
  const long long id = obs_data_get_int(command, "id");

  if (strcmp(cmd, "info") == 0) {
    info(id);
  } else if (strcmp(cmd, "start") == 0) {
    start(id, command);
  } else if (strcmp(cmd, "stop") == 0) {
    stop(id);
  } else {
    protocol::send_error(cmd, id, "unknown command");
  }
}

void Recorder::info(long long id) {
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "info");
  obs_data_set_int(event, "id", id);
  obs_data_set_string(event, "obsVersion", obs_get_version_string());
  append_devices(event, "inputs", "wasapi_input_capture");
  append_devices(event, "outputs", "wasapi_output_capture");

  OBSDataArrayAutoRelease encoders = obs_data_array_create();
  const char *type = nullptr;
  for (size_t i = 0; obs_enum_encoder_types(i, &type); i++) {
    if (obs_get_encoder_type(type) != OBS_ENCODER_VIDEO) {
      continue;
    }
    OBSDataAutoRelease encoder = obs_data_create();
    obs_data_set_string(encoder, "id", type);
    obs_data_set_string(encoder, "name", obs_encoder_get_display_name(type));
    obs_data_array_push_back(encoders, encoder);
  }
  obs_data_set_array(event, "encoders", encoders);
  obs_data_set_string(event, "defaultEncoder", pick_video_encoder(nullptr));

  protocol::send(event);
}

bool Recorder::reset_video(uint32_t width, uint32_t height, uint32_t fps) {
  if (width == width_ && height == height_ && fps == fps_) {
    return true;
  }

  obs_video_info video{};
  video.graphics_module = "libobs-d3d11.dll";
  video.fps_num = fps;
  video.fps_den = 1;
  video.base_width = video.output_width = width;
  video.base_height = video.output_height = height;
  video.output_format = VIDEO_FORMAT_NV12;
  video.colorspace = VIDEO_CS_709;
  video.range = VIDEO_RANGE_PARTIAL;
  video.adapter = 0;
  video.gpu_conversion = true;
  video.scale_type = OBS_SCALE_BICUBIC;

  const int result = obs_reset_video(&video);
  if (result != OBS_VIDEO_SUCCESS) {
    blog(LOG_ERROR, "obs_reset_video failed: %d", result);
    return false;
  }
  width_ = width;
  height_ = height;
  fps_ = fps;
  return true;
}

const char *Recorder::pick_video_encoder(const char *requested) {
  if (requested && *requested && encoder_registered(requested)) {
    return requested;
  }
  // Hardware first; a registered hardware encoder already passed OBS's own probe (obs-nvenc-test etc.).
  for (const char *id : {"obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2", "obs_x264"}) {
    if (encoder_registered(id)) {
      return id;
    }
  }
  return "obs_x264";
}

obs_source_t *Recorder::create_video_source(obs_data_t *video, std::string &error) {
  const char *kind = obs_data_get_string(video, "source");

  if (strcmp(kind, "monitor") == 0) {
    const std::string monitor = first_list_value("monitor_capture", "monitor_id");
    if (monitor.empty()) {
      error = "no monitor found";
      return nullptr;
    }
    OBSDataAutoRelease settings = obs_data_create();
    obs_data_set_string(settings, "monitor_id", monitor.c_str());
    obs_data_set_bool(settings, "capture_cursor", false);
    if (obs_data_has_user_value(video, "method")) {
      obs_data_set_int(settings, "method", obs_data_get_int(video, "method"));
    }
    return obs_source_create("monitor_capture", "monitor", settings, nullptr);
  }

  const char *window = obs_data_get_string(video, "window");
  OBSDataAutoRelease settings = obs_data_create();
  obs_data_set_string(settings, "capture_mode", "window");
  obs_data_set_string(settings, "window", *window ? window : kLeagueWindow);
  obs_data_set_int(settings, "priority", WINDOW_PRIORITY_EXE);
  obs_data_set_bool(settings, "anti_cheat_hook", true);
  obs_data_set_bool(settings, "capture_cursor", false);
  obs_data_set_bool(settings, "capture_overlays", false);
  return obs_source_create("game_capture", "game", settings, nullptr);
}

void Recorder::start(long long id, obs_data_t *params) {
  if (output_ && obs_output_active(output_)) {
    protocol::send_error("start", id, "already recording");
    return;
  }
  release_session();

  // Output: one file (path), or a ring of fixed-length MPEG-TS segments named {prefix}_{000000}.ts.
  OBSDataAutoRelease out = obs_data_get_obj(params, "output");
  OBSDataAutoRelease segments = out ? obs_data_get_obj(out, "segments") : nullptr;
  std::string path = out ? obs_data_get_string(out, "path") : "";
  uint32_t segment_seconds = 0;
  OBSDataAutoRelease output_settings = obs_data_create();
  if (segments) {
    const std::string directory = obs_data_get_string(segments, "directory");
    const std::string prefix = obs_data_get_string(segments, "prefix");
    segment_seconds = (uint32_t)obs_data_get_int(segments, "seconds");
    if (directory.empty() || prefix.empty() || segment_seconds == 0) {
      protocol::send_error("start", id, "segments needs directory, prefix and seconds");
      return;
    }
    path = directory + "/" + prefix + "_000000.ts";
    obs_data_set_bool(output_settings, "split_file", true);
    obs_data_set_int(output_settings, "max_time_sec", segment_seconds);
    obs_data_set_int(output_settings, "max_size_mb", 0);
    obs_data_set_bool(output_settings, "allow_overwrite", true);
    obs_data_set_string(output_settings, "directory", directory.c_str());
    obs_data_set_string(output_settings, "format", (prefix + "_{seq}").c_str());
    obs_data_set_string(output_settings, "extension", "ts");
  }
  if (path.empty()) {
    protocol::send_error("start", id, "missing output.path or output.segments");
    return;
  }
  obs_data_set_string(output_settings, "path", path.c_str());

  OBSDataAutoRelease video = obs_data_get_obj(params, "video");
  if (!video) {
    video = obs_data_create();
  }
  obs_data_set_default_int(video, "width", kDefaultWidth);
  obs_data_set_default_int(video, "height", kDefaultHeight);
  obs_data_set_default_int(video, "fps", kDefaultFps);
  obs_data_set_default_int(video, "bitrateKbps", 20000);

  if (!reset_video((uint32_t)obs_data_get_int(video, "width"), (uint32_t)obs_data_get_int(video, "height"),
                   (uint32_t)obs_data_get_int(video, "fps"))) {
    protocol::send_error("start", id, "video reset failed");
    return;
  }

  std::string error;
  video_source_ = create_video_source(video, error);
  if (!video_source_) {
    protocol::send_error("start", id, error.empty() ? "video source creation failed" : error);
    return;
  }
  if (strcmp(obs_source_get_id(video_source_), "game_capture") == 0) {
    signal_handler_t *sh = obs_source_get_signal_handler(video_source_);
    hooked_.Connect(sh, "hooked", on_hooked, this);
    unhooked_.Connect(sh, "unhooked", on_unhooked, this);
  }

  scene_ = obs_scene_create_private("dpm");
  obs_sceneitem_t *item = obs_scene_add(scene_, video_source_);
  vec2 bounds;
  vec2_set(&bounds, (float)width_, (float)height_);
  obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
  obs_sceneitem_set_bounds(item, &bounds);
  obs_set_output_source(0, obs_scene_get_source(scene_));

  OBSDataArrayAutoRelease tracks = obs_data_get_array(params, "audio");
  if (!create_audio_tracks(tracks, error)) {
    protocol::send_error("start", id, error);
    release_session();
    return;
  }

  const char *encoder_id = pick_video_encoder(obs_data_get_string(video, "encoder"));
  OBSDataAutoRelease encoder_settings = obs_data_create();
  obs_data_set_string(encoder_settings, "rate_control", "CBR");
  obs_data_set_int(encoder_settings, "bitrate", obs_data_get_int(video, "bitrateKbps"));
  // Segments split on keyframes, so the GOP is the segment length.
  obs_data_set_int(encoder_settings, "keyint_sec", segment_seconds ? segment_seconds : 2);
  video_encoder_ = obs_video_encoder_create(encoder_id, "video", encoder_settings, nullptr);
  if (!video_encoder_) {
    protocol::send_error("start", id, std::string("video encoder creation failed: ") + encoder_id);
    release_session();
    return;
  }
  obs_encoder_set_video(video_encoder_, obs_get_video());

  output_ = obs_output_create("ffmpeg_muxer", "file", output_settings, nullptr);
  obs_output_set_video_encoder(output_, video_encoder_);
  for (size_t track = 0; track < audio_encoders_.size(); track++) {
    obs_output_set_audio_encoder(output_, audio_encoders_[track], track);
  }

  signal_handler_t *sh = obs_output_get_signal_handler(output_);
  output_start_.Connect(sh, "start", on_output_start, this);
  output_stop_.Connect(sh, "stop", on_output_stop, this);
  file_changed_.Connect(sh, "file_changed", on_file_changed, this);
  obs_output_add_packet_callback(output_, on_packet, this);

  {
    std::lock_guard lock(clock_mutex_);
    keyframes_ = 0;
    anchored_ = false;
    last_keyframe_wall_ms_ = 0;
    last_video_wall_ms_ = 0;
    segmented_ = segments != nullptr;
    segment_file_ = segmented_ ? base_name(path) : "";
    segment_start_wall_ms_ = 0;
  }
  {
    std::lock_guard lock(stop_mutex);
    output_stopped = false;
  }
  if (!obs_output_start(output_)) {
    const char *last = obs_output_get_last_error(output_);
    protocol::send_error("start", id, std::string("output start failed: ") + (last ? last : "unknown"));
    {
      std::lock_guard lock(stop_mutex);
      output_stopped = true;
    }
    release_session();
    return;
  }

  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "starting");
  obs_data_set_int(event, "id", id);
  obs_data_set_string(event, "encoder", encoder_id);
  obs_data_set_string(event, "path", path.c_str());
  obs_data_set_int(event, "width", width_);
  obs_data_set_int(event, "height", height_);
  obs_data_set_int(event, "fps", fps_);
  protocol::send(event);
}

// One track per entry, in order: {id, kind: "process", executable | window} or {id, kind: "input", deviceId}.
// The id becomes the encoder name, which the muxer writes as the stream title.
bool Recorder::create_audio_tracks(obs_data_array_t *tracks, std::string &error) {
  const size_t count = tracks ? obs_data_array_count(tracks) : 0;
  if (count > MAX_AUDIO_MIXES) {
    error = "at most " + std::to_string(MAX_AUDIO_MIXES) + " audio tracks";
    return false;
  }

  OBSDataAutoRelease aac_settings = obs_data_create();
  obs_data_set_int(aac_settings, "bitrate", 160);
  for (size_t track = 0; track < count; track++) {
    OBSDataAutoRelease entry = obs_data_array_item(tracks, track);
    const std::string track_id = obs_data_get_string(entry, "id");
    const char *kind = obs_data_get_string(entry, "kind");
    if (track_id.empty()) {
      error = "audio track without id";
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
      error = "audio track " + track_id + ": unknown kind";
      return false;
    }

    obs_source_t *source = obs_source_create(source_id, ("audio:" + track_id).c_str(), settings, nullptr);
    // A mic on input 1 of a stereo interface (Focusrite "Analogue 1 + 2") only fills the left channel.
    obs_data_set_default_bool(entry, "mono", strcmp(kind, "input") == 0);
    if (obs_data_get_bool(entry, "mono")) {
      obs_source_set_flags(source, obs_source_get_flags(source) | OBS_SOURCE_FLAG_FORCE_MONO);
    }
    obs_source_set_audio_mixers(source, 1u << track);
    obs_set_output_source((uint32_t)(kFirstAudioChannel + track), source);
    audio_sources_.emplace_back(source);

    obs_encoder_t *encoder = obs_audio_encoder_create("ffmpeg_aac", track_id.c_str(), aac_settings, track, nullptr);
    obs_encoder_set_audio(encoder, obs_get_audio());
    audio_encoders_.emplace_back(encoder);
  }
  return true;
}

void Recorder::stop(long long id) {
  if (!output_ || !obs_output_active(output_)) {
    release_session();
    protocol::send_error("stop", id, "not recording");
    return;
  }

  obs_output_stop(output_);
  // The muxer flushes asynchronously; sources are only released once the file is closed.
  std::unique_lock lock(stop_mutex);
  if (!stop_cv.wait_for(lock, std::chrono::seconds(30), [] { return output_stopped; })) {
    lock.unlock();
    blog(LOG_WARNING, "output did not stop within 30 s, forcing");
    obs_output_force_stop(output_);
  } else {
    lock.unlock();
  }
  release_session();
}

void Recorder::release_session() {
  output_start_.Disconnect();
  output_stop_.Disconnect();
  file_changed_.Disconnect();
  hooked_.Disconnect();
  unhooked_.Disconnect();
  if (output_) {
    obs_output_remove_packet_callback(output_, on_packet, this);
  }
  obs_set_output_source(0, nullptr);
  for (size_t track = 0; track < audio_sources_.size(); track++) {
    obs_set_output_source((uint32_t)(kFirstAudioChannel + track), nullptr);
  }
  output_ = nullptr;
  video_encoder_ = nullptr;
  audio_encoders_.clear();
  audio_sources_.clear();
  scene_ = nullptr;
  video_source_ = nullptr;
}

// Caller holds clock_mutex_. Mirrors a line of ffmpeg's segment list: sent once the segment is closed.
void Recorder::close_segment(double end_wall_ms) {
  if (segment_file_.empty() || segment_start_wall_ms_ == 0) {
    return;
  }
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "segment");
  obs_data_set_string(event, "file", segment_file_.c_str());
  obs_data_set_double(event, "startWallMs", segment_start_wall_ms_);
  obs_data_set_double(event, "endWallMs", end_wall_ms);
  protocol::send(event);
}

// Runs on the output thread. Maps file time to wall clock so DPM can place game events on the video.
void Recorder::on_packet(obs_output_t *, encoder_packet *pkt, encoder_packet_time *pkt_time, void *param) {
  auto *self = static_cast<Recorder *>(param);
  if (pkt->type != OBS_ENCODER_VIDEO) {
    return;
  }
  // Composition time: when libobs rendered this frame, i.e. the moment it shows.
  const double wall_ms =
      pkt_time ? obs_ns_to_wall_ms(pkt_time->cts) : obs_ns_to_wall_ms((uint64_t)pkt->sys_dts_usec * 1000);

  std::lock_guard lock(self->clock_mutex_);
  self->last_video_wall_ms_ = std::max(self->last_video_wall_ms_, wall_ms);
  if (!pkt->keyframe) {
    return;
  }
  self->last_keyframe_wall_ms_ = wall_ms;
  if (self->segmented_ && self->segment_start_wall_ms_ == 0) {
    self->segment_start_wall_ms_ = wall_ms;
  }

  const bool first = !self->anchored_;
  if (!first && ++self->keyframes_ % kKeyframesPerAnchorCheck != 0) {
    return;
  }
  self->anchored_ = true;

  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "anchor");
  obs_data_set_string(event, "kind", first ? "first" : "check");
  obs_data_set_int(event, "pts", pkt->pts);
  obs_data_set_int(event, "dts", pkt->dts);
  obs_data_set_double(event, "ptsSec", (double)pkt->pts * pkt->timebase_num / pkt->timebase_den);
  obs_data_set_double(event, "dtsWallMs", obs_ns_to_wall_ms((uint64_t)pkt->sys_dts_usec * 1000));
  obs_data_set_double(event, "wallMs", wall_ms);
  protocol::send(event);
}

// A split happens on the keyframe the packet callback saw last, so that keyframe opens the new file.
void Recorder::on_file_changed(void *param, calldata_t *cd) {
  auto *self = static_cast<Recorder *>(param);
  const char *next = calldata_string(cd, "next_file");
  std::lock_guard lock(self->clock_mutex_);
  self->close_segment(self->last_keyframe_wall_ms_);
  self->segment_file_ = next ? base_name(next) : "";
  self->segment_start_wall_ms_ = self->last_keyframe_wall_ms_;
}

void Recorder::on_output_start(void *, calldata_t *) {
  protocol::send_event("started");
}

void Recorder::on_output_stop(void *param, calldata_t *cd) {
  auto *self = static_cast<Recorder *>(param);
  {
    // The last segment ends one frame after its last picture, not at a nominal segment length.
    std::lock_guard lock(self->clock_mutex_);
    if (self->segmented_) {
      self->close_segment(self->last_video_wall_ms_ + (self->fps_ ? 1000.0 / self->fps_ : 0));
      self->segment_file_.clear();
    }
  }

  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "stopped");
  obs_data_set_int(event, "code", calldata_int(cd, "code"));
  if (const char *last = calldata_string(cd, "last_error")) {
    obs_data_set_string(event, "error", last);
  }
  protocol::send(event);

  std::lock_guard lock(stop_mutex);
  output_stopped = true;
  stop_cv.notify_all();
}

void Recorder::on_hooked(void *, calldata_t *cd) {
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "hooked");
  obs_data_set_string(event, "title", calldata_string(cd, "title"));
  obs_data_set_string(event, "class", calldata_string(cd, "class"));
  obs_data_set_string(event, "executable", calldata_string(cd, "executable"));
  protocol::send(event);
}

void Recorder::on_unhooked(void *, calldata_t *) {
  protocol::send_event("unhooked");
}
