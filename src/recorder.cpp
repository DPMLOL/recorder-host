#include "recorder.h"

#include "audio_apps.h"
#include "protocol.h"
#include "wall_clock.h"

#include <util/platform.h>
#include <util/windows/window-helpers.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>

namespace {

constexpr char kLeagueWindow[] = "League of Legends (TM) Client:RiotWindowClass:League of Legends.exe";
constexpr uint32_t kDefaultWidth = 1920;
constexpr uint32_t kDefaultHeight = 1080;
constexpr uint32_t kDefaultFps = 60;
constexpr long long kDefaultKeyframeSec = 2;
constexpr long long kKeyframesPerAnchorCheck = 30;
constexpr auto kStopTimeout = std::chrono::seconds(30);
// Closing stdin: the parent is gone, so the vod is finalised quickly or not at all.
constexpr auto kShutdownStopTimeout = std::chrono::seconds(5);
constexpr long long kProtocol = 2;

// Outputs signal their stop from their own threads; stop() waits for all of them here.
std::mutex stop_mutex;
std::condition_variable stop_cv;

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

void set_db(obs_data_t *event, const char *key, double db) {
  if (std::isfinite(db)) {
    obs_data_set_double(event, key, db);
  }
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
  for (const char *module :
       {"win-capture", "win-wasapi", "obs-ffmpeg", "obs-outputs", "obs-nvenc", "obs-qsv11", "obs-x264"}) {
    obs_add_safe_module(module);
  }
  obs_load_all_modules();
  obs_log_loaded_modules();
  obs_post_load_modules();
  return true;
}

void Recorder::shutdown() {
  // A normal stop, as `stop` does, waited for: a forced one finalises the vod only if a packet still arrives
  // before the teardown below, and loses what libobs had buffered.
  if (recording()) {
    stop_outputs(kShutdownStopTimeout);
  }
  release_session();
  audio_.release();
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
  } else if (strcmp(cmd, "monitor") == 0) {
    monitor(id, command);
  } else if (strcmp(cmd, "set_volume") == 0) {
    set_volume(id, command);
  } else if (strcmp(cmd, "loudness") == 0) {
    loudness(id, command);
  } else if (strcmp(cmd, "audio_apps") == 0) {
    audio_apps(id);
  } else {
    protocol::send_error(cmd, id, "unknown command");
  }
}

void Recorder::info(long long id) {
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "info");
  obs_data_set_int(event, "id", id);
  obs_data_set_int(event, "protocol", kProtocol);
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

  protocol::send(event, "features", {"vod-only", "progress", "encoder-settings"});
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
  // The OBS frontend sets these (its defaults); libobs leaves them at 0, which turns an HDR monitor black: its
  // capture is tonemapped to SDR with a multiplier of 80 / sdr_white_level.
  obs_set_video_levels(300.f, 1000.f);
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

bool Recorder::recording() const {
  return segments_.running || vod_.running;
}

bool Recorder::start_output(OutputSlot &slot, const char *type, obs_data_t *settings, std::string &error) {
  slot.output = obs_output_create(type, slot.name, settings, nullptr);
  if (!slot.output) {
    error = std::string("cannot create ") + type;
    return false;
  }
  obs_output_set_video_encoder(slot.output, video_encoder_);
  const auto &encoders = audio_.encoders();
  for (size_t track = 0; track < encoders.size(); track++) {
    obs_output_set_audio_encoder(slot.output, encoders[track], track);
  }
  slot.start_signal.Connect(obs_output_get_signal_handler(slot.output), "start", on_output_start, &slot);
  slot.stop_signal.Connect(obs_output_get_signal_handler(slot.output), "stop", on_output_stop, &slot);
  // Before the start, so the first keyframe is never missed.
  obs_output_add_packet_callback(slot.output, on_packet, &slot);
  {
    std::lock_guard lock(stop_mutex);
    slot.running = true;
  }
  if (!obs_output_start(slot.output)) {
    const char *last = obs_output_get_last_error(slot.output);
    error = std::string(slot.name) + " output start failed: " + (last ? last : "unknown");
    std::lock_guard lock(stop_mutex);
    slot.running = false;
    return false;
  }
  return true;
}

void Recorder::start(long long id, obs_data_t *params) {
  if (recording()) {
    protocol::send_error("start", id, "already recording");
    return;
  }
  release_session();

  // The ring (fixed-length MPEG-TS segments named {prefix}_{000000}.ts, which clips are cut from), the vod, or
  // both. The vod is the replay, written as the game plays: Hybrid MP4 stays readable if anything crashes, and
  // needs no remux at the end.
  OBSDataAutoRelease out = obs_data_get_obj(params, "output");
  OBSDataAutoRelease segments = out ? obs_data_get_obj(out, "segments") : nullptr;
  vod_path_ = out ? obs_data_get_string(out, "vod") : "";
  if (!segments && vod_path_.empty()) {
    protocol::send_error("start", id, "nothing to record");
    return;
  }
  std::string first_segment;
  uint32_t segment_seconds = 0;
  OBSDataAutoRelease segment_settings;
  if (segments) {
    const std::string directory = obs_data_get_string(segments, "directory");
    const std::string prefix = obs_data_get_string(segments, "prefix");
    segment_seconds = (uint32_t)obs_data_get_int(segments, "seconds");
    if (directory.empty() || prefix.empty() || segment_seconds == 0) {
      protocol::send_error("start", id, "segments needs directory, prefix and seconds");
      return;
    }
    first_segment = directory + "/" + prefix + "_000000.ts";
    segment_settings = obs_data_create();
    obs_data_set_string(segment_settings, "path", first_segment.c_str());
    obs_data_set_bool(segment_settings, "split_file", true);
    obs_data_set_int(segment_settings, "max_time_sec", segment_seconds);
    obs_data_set_int(segment_settings, "max_size_mb", 0);
    obs_data_set_bool(segment_settings, "allow_overwrite", true);
    obs_data_set_string(segment_settings, "directory", directory.c_str());
    obs_data_set_string(segment_settings, "format", (prefix + "_{seq}").c_str());
    obs_data_set_string(segment_settings, "extension", "ts");
  }

  OBSDataAutoRelease video = obs_data_get_obj(params, "video");
  if (!video) {
    video = obs_data_create();
  }
  obs_data_set_default_int(video, "width", kDefaultWidth);
  obs_data_set_default_int(video, "height", kDefaultHeight);
  obs_data_set_default_int(video, "fps", kDefaultFps);
  obs_data_set_default_int(video, "bitrateKbps", 20000);
  obs_data_set_default_int(video, "keyframeSec", kDefaultKeyframeSec);
  // Segments split on keyframes, so with a ring the GOP is the segment length.
  const long long keyframe_sec = segments ? segment_seconds : obs_data_get_int(video, "keyframeSec");
  if (keyframe_sec <= 0) {
    protocol::send_error("start", id, "video.keyframeSec must be positive");
    return;
  }

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

  // Recording replaces any mixer monitoring: the same sources would be opened twice.
  OBSDataArrayAutoRelease tracks = obs_data_get_array(params, "audio");
  if (!audio_.create(tracks, error) || !audio_.create_encoders(error)) {
    protocol::send_error("start", id, error);
    release_session();
    return;
  }

  const char *encoder_id = pick_video_encoder(obs_data_get_string(video, "encoder"));
  OBSDataAutoRelease encoder_settings = obs_data_create();
  obs_data_set_string(encoder_settings, "rate_control", "CBR");
  obs_data_set_int(encoder_settings, "bitrate", obs_data_get_int(video, "bitrateKbps"));
  obs_data_set_int(encoder_settings, "keyint_sec", keyframe_sec);
  // Preset and rate control, translated per encoder by DPM: applied as given, over the three above.
  OBSDataAutoRelease extra_settings = obs_data_get_obj(video, "encoderSettings");
  if (extra_settings) {
    obs_data_apply(encoder_settings, extra_settings);
  }
  video_encoder_ = obs_video_encoder_create(encoder_id, "video", encoder_settings, nullptr);
  if (!video_encoder_) {
    protocol::send_error("start", id, std::string("video encoder creation failed: ") + encoder_id);
    release_session();
    return;
  }
  obs_encoder_set_video(video_encoder_, obs_get_video());

  {
    std::lock_guard lock(clock_mutex_);
    keyframes_ = 0;
    anchored_ = false;
    last_keyframe_wall_ms_ = 0;
    last_video_wall_ms_ = 0;
    stop_ns_ = 0;
    file_start_wall_ms_ = 0;
    first_dts_sec_ = 0;
    last_dts_sec_ = 0;
    segment_file_ = segments ? base_name(first_segment) : "";
    segment_start_wall_ms_ = 0;
    // Anchors map the file the app plays: the vod's own time when there is one.
    clock_ = vod_path_.empty() ? &segments_ : &vod_;
  }
  audio_.start_loudness();

  if (segments) {
    if (!start_output(segments_, "ffmpeg_muxer", segment_settings, error)) {
      protocol::send_error("start", id, error);
      release_session();
      return;
    }
    file_changed_.Connect(obs_output_get_signal_handler(segments_.output), "file_changed", on_file_changed, this);
  }

  if (!vod_path_.empty()) {
    OBSDataAutoRelease vod_settings = obs_data_create();
    obs_data_set_string(vod_settings, "path", vod_path_.c_str());
    if (!start_output(vod_, "mp4_output", vod_settings, error)) {
      if (!segments) {
        // Nothing else would record.
        protocol::send_error("start", id, error);
        release_session();
        return;
      }
      // The ring still records: clips work, and DPM can assemble the replay from it.
      blog(LOG_WARNING, "%s", error.c_str());
      OBSDataAutoRelease warning = obs_data_create();
      obs_data_set_string(warning, "event", "warning");
      obs_data_set_string(warning, "message", error.c_str());
      protocol::send(warning);
      // Disconnected before the output is released, while its signal handler still exists.
      vod_.start_signal.Disconnect();
      vod_.stop_signal.Disconnect();
      vod_.output = nullptr;
      vod_path_.clear();
      std::lock_guard lock(clock_mutex_);
      clock_ = &segments_;
    }
  }

  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "starting");
  obs_data_set_int(event, "id", id);
  obs_data_set_string(event, "encoder", encoder_id);
  obs_data_set_string(event, "path", first_segment.c_str());
  obs_data_set_string(event, "vod", vod_path_.c_str());
  obs_data_set_int(event, "width", width_);
  obs_data_set_int(event, "height", height_);
  obs_data_set_int(event, "fps", fps_);
  OBSDataArrayAutoRelease ids = obs_data_array_create();
  for (const std::string &track : audio_.track_ids()) {
    OBSDataAutoRelease entry = obs_data_create();
    obs_data_set_string(entry, "id", track.c_str());
    obs_data_array_push_back(ids, entry);
  }
  obs_data_set_array(event, "tracks", ids);
  protocol::send(event);
}

void Recorder::stop(long long id) {
  if (!recording()) {
    release_session();
    protocol::send_error("stop", id, "not recording");
    return;
  }

  stop_outputs(kStopTimeout);

  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "stopped");
  obs_data_set_int(event, "id", id);
  obs_data_set_string(event, "vod", vod_path_.c_str());
  {
    std::lock_guard lock(clock_mutex_);
    if (clock_ == &vod_ && anchored_) {
      // mp4_output never writes a track's last packet (its duration is the gap to the next one), so the vod's
      // video lasts the decode times it spans, from file time 0.
      obs_data_set_double(event, "endWallMs", file_start_wall_ms_ + (last_dts_sec_ - first_dts_sec_) * 1000);
    } else if (last_video_wall_ms_ > 0) {
      // The ring writes every packet: it ends one frame after its last picture.
      obs_data_set_double(event, "endWallMs", last_video_wall_ms_ + (fps_ ? 1000.0 / fps_ : 0));
    }
  }
  // The whole recording's mix, measured as it played: the replay's normalisation gain needs no second pass.
  set_db(event, "loudnessLufs", audio_.loudness_lufs(0));
  set_db(event, "peakDb", audio_.peak_dbfs(0));
  release_session();
  protocol::send(event);
}

void Recorder::stop_outputs(std::chrono::seconds timeout) {
  {
    // Taken just before obs_output_stop takes its own, so a packet is never counted that the file does not hold.
    std::lock_guard lock(clock_mutex_);
    stop_ns_ = os_gettime_ns();
  }
  for (OutputSlot *slot : {&segments_, &vod_}) {
    if (slot->output && slot->running) {
      obs_output_stop(slot->output);
    }
  }
  // Muxers flush asynchronously; sources are only released once every file is closed.
  std::unique_lock lock(stop_mutex);
  if (!stop_cv.wait_for(lock, timeout, [this] { return !recording(); })) {
    lock.unlock();
    blog(LOG_WARNING, "outputs did not stop within %lld s, forcing", (long long)timeout.count());
    for (OutputSlot *slot : {&segments_, &vod_}) {
      if (slot->output && obs_output_active(slot->output)) {
        obs_output_force_stop(slot->output);
      }
    }
  }
}

// The mixer's live view outside a game: the same sources as a recording, with meters, and nothing written.
void Recorder::monitor(long long id, obs_data_t *params) {
  if (recording()) {
    protocol::send_error("monitor", id, "recording");
    return;
  }
  OBSDataArrayAutoRelease tracks = obs_data_get_array(params, "audio");
  std::string error;
  if (!audio_.create(tracks, error)) {
    protocol::send_error("monitor", id, error);
    return;
  }
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "monitoring");
  obs_data_set_int(event, "id", id);
  protocol::send(event);
}

void Recorder::set_volume(long long id, obs_data_t *params) {
  if (!audio_.set_volume(obs_data_get_string(params, "track"), obs_data_get_double(params, "volume"))) {
    protocol::send_error("set_volume", id, "no such track");
    return;
  }
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "ok");
  obs_data_set_int(event, "id", id);
  protocol::send(event);
}

// Loudness and peak of the mix since a wall-clock instant: a clip's window, without decoding it again.
void Recorder::loudness(long long id, obs_data_t *params) {
  const double since = obs_data_get_double(params, "sinceWallMs");
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "loudness");
  obs_data_set_int(event, "id", id);
  set_db(event, "loudnessLufs", audio_.loudness_lufs(since));
  set_db(event, "peakDb", audio_.peak_dbfs(since));
  protocol::send(event);
}

void Recorder::audio_apps(long long id) {
  OBSDataArrayAutoRelease apps = obs_data_array_create();
  list_audio_apps(apps);
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "audio_apps");
  obs_data_set_int(event, "id", id);
  obs_data_set_array(event, "apps", apps);
  protocol::send(event);
}

void Recorder::release_session() {
  file_changed_.Disconnect();
  hooked_.Disconnect();
  unhooked_.Disconnect();
  for (OutputSlot *slot : {&segments_, &vod_}) {
    if (slot->output) {
      obs_output_remove_packet_callback(slot->output, on_packet, slot);
    }
    slot->start_signal.Disconnect();
    slot->stop_signal.Disconnect();
    slot->output = nullptr;
    slot->running = false;
  }
  obs_set_output_source(0, nullptr);
  video_encoder_ = nullptr;
  audio_.release();
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

// Runs on the output's thread, for every output. Maps file time to wall clock so DPM can place game events on
// the video, and tells how far the file reaches.
void Recorder::on_packet(obs_output_t *, encoder_packet *pkt, encoder_packet_time *pkt_time, void *param) {
  auto *slot = static_cast<OutputSlot *>(param);
  Recorder *self = slot->owner;
  if (pkt->type != OBS_ENCODER_VIDEO) {
    return;
  }
  // Composition time: when libobs rendered this frame, i.e. the moment it shows.
  const double wall_ms =
      pkt_time ? obs_ns_to_wall_ms(pkt_time->cts) : obs_ns_to_wall_ms((uint64_t)pkt->sys_dts_usec * 1000);

  std::lock_guard lock(self->clock_mutex_);
  // The packet that ends the file still comes through here; with B-frames it shows after the last written picture.
  if (self->stop_ns_ && (uint64_t)pkt->sys_dts_usec * 1000 >= self->stop_ns_) {
    return;
  }
  self->last_video_wall_ms_ = std::max(self->last_video_wall_ms_, wall_ms);
  if (slot == self->clock_) {
    // An output's first packet is a keyframe, which anchors it.
    const double dts_sec = (double)pkt->dts * pkt->timebase_num / pkt->timebase_den;
    if (!self->anchored_) {
      self->first_dts_sec_ = dts_sec;
    }
    self->last_dts_sec_ = dts_sec;
  }
  if (!pkt->keyframe) {
    return;
  }
  if (slot == &self->segments_) {
    self->last_keyframe_wall_ms_ = wall_ms;
    if (self->segment_start_wall_ms_ == 0) {
      self->segment_start_wall_ms_ = wall_ms;
    }
  }
  if (slot != self->clock_) {
    return;
  }

  const bool first = !self->anchored_;
  if (!first) {
    // A keyframe closes the previous GOP: every picture before it is with the muxer, a whole fragment of the vod.
    OBSDataAutoRelease progress = obs_data_create();
    obs_data_set_string(progress, "event", "progress");
    obs_data_set_double(progress, "endWallMs", wall_ms);
    protocol::send(progress);
  }
  if (!first && ++self->keyframes_ % kKeyframesPerAnchorCheck != 0) {
    return;
  }
  self->anchored_ = true;

  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "anchor");
  obs_data_set_string(event, "kind", first ? "first" : "check");
  obs_data_set_int(event, "pts", pkt->pts);
  obs_data_set_int(event, "dts", pkt->dts);
  const double pts_sec = (double)pkt->pts * pkt->timebase_num / pkt->timebase_den;
  if (first) {
    self->file_start_wall_ms_ = wall_ms - pts_sec * 1000;
  }
  obs_data_set_double(event, "ptsSec", pts_sec);
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

// The first output writing means the recording is live: the ring when there is one (started first), else the vod.
void Recorder::on_output_start(void *param, calldata_t *) {
  auto *slot = static_cast<OutputSlot *>(param);
  Recorder *self = slot->owner;
  if (slot == (self->segments_.output ? &self->segments_ : &self->vod_)) {
    protocol::send_event("started");
  }
}

void Recorder::on_output_stop(void *param, calldata_t *cd) {
  auto *slot = static_cast<OutputSlot *>(param);
  Recorder *self = slot->owner;
  const long long code = calldata_int(cd, "code");
  if (slot == &self->segments_) {
    // The last segment ends one frame after its last picture, not at a nominal segment length.
    std::lock_guard lock(self->clock_mutex_);
    self->close_segment(self->last_video_wall_ms_ + (self->fps_ ? 1000.0 / self->fps_ : 0));
    self->segment_file_.clear();
  }

  // A non-zero code outside stop() means the output died on its own (disk full, encoder lost).
  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "output_stopped");
  obs_data_set_string(event, "output", slot->name);
  obs_data_set_int(event, "code", code);
  if (const char *last = calldata_string(cd, "last_error")) {
    obs_data_set_string(event, "error", last);
  }
  protocol::send(event);

  std::lock_guard lock(stop_mutex);
  slot->running = false;
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
