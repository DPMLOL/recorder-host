#include "preview.h"

#include "jpeg.h"
#include "protocol.h"
#include "wall_clock.h"

#include <graphics/vec4.h>
#include <util/platform.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace {

constexpr uint32_t kDefaultThumbnailWidth = 480;
constexpr uint32_t kDefaultThumbnailHeight = 270;
constexpr uint32_t kMaxSheetSide = 16384;
// Frames between staging a copy and mapping it: libobs flushes every frame, so by the next one the GPU has
// finished the copy and the map does not wait.
constexpr int kReadbackDelayFrames = 1;
// How long stop() lets the graphics thread finish the readbacks already asked for.
constexpr auto kDrainTimeout = std::chrono::milliseconds(500);
// Images waiting for the worker; past this a job is dropped (its cell stays black) rather than growing memory.
constexpr size_t kMaxJobs = 16;
constexpr float kSheetQuality = 0.8f;
constexpr float kThumbnailQuality = 0.9f;

uint32_t get_uint(obs_data_t *data, const char *key) {
  const long long value = obs_data_get_int(data, key);
  return value > 0 && value <= UINT32_MAX ? (uint32_t)value : 0;
}

void send_warning(const std::string &message) {
  blog(LOG_WARNING, "%s", message.c_str());
  OBSDataAutoRelease warning = obs_data_create();
  obs_data_set_string(warning, "event", "warning");
  obs_data_set_string(warning, "message", message.c_str());
  protocol::send(warning);
}

double ms_since(uint64_t start_ns) {
  return (double)(os_gettime_ns() - start_ns) / 1e6;
}

}  // namespace

Preview::~Preview() {
  stop();
}

bool Preview::parse_preview(obs_data_t *params, PreviewSettings &out, bool &present, std::string &error) {
  OBSDataAutoRelease preview = obs_data_get_obj(params, "preview");
  present = preview != nullptr;
  if (!present) {
    return true;
  }
  out.directory = obs_data_get_string(preview, "directory");
  out.prefix = obs_data_get_string(preview, "prefix");
  out.tile_width = get_uint(preview, "tileWidth");
  out.tile_height = get_uint(preview, "tileHeight");
  out.interval_ms = get_uint(preview, "intervalMs");
  out.columns = get_uint(preview, "columns");
  out.rows = get_uint(preview, "rows");
  out.flush_tiles = obs_data_has_user_value(preview, "flushTiles") ? get_uint(preview, "flushTiles") : 5;
  if (out.directory.empty() || out.prefix.empty() || !out.tile_width || !out.tile_height || !out.interval_ms ||
      !out.columns || !out.rows || !out.flush_tiles) {
    error = "preview needs directory, prefix, tileWidth, tileHeight, intervalMs, columns and rows";
    return false;
  }
  if ((uint64_t)out.tile_width * out.columns > kMaxSheetSide || (uint64_t)out.tile_height * out.rows > kMaxSheetSide) {
    error = "preview sheets cannot exceed 16384 pixels a side";
    return false;
  }
  return true;
}

bool Preview::parse_thumbnail(obs_data_t *params, ThumbnailSettings &out, bool &present, std::string &error) {
  OBSDataAutoRelease thumbnail = obs_data_get_obj(params, "thumbnail");
  present = thumbnail != nullptr;
  if (!present) {
    return true;
  }
  out.path = obs_data_get_string(thumbnail, "path");
  out.width = obs_data_has_user_value(thumbnail, "width") ? get_uint(thumbnail, "width") : kDefaultThumbnailWidth;
  out.height = obs_data_has_user_value(thumbnail, "height") ? get_uint(thumbnail, "height") : kDefaultThumbnailHeight;
  if (out.path.empty() || !out.width || !out.height || out.width > kMaxSheetSide || out.height > kMaxSheetSide) {
    error = "thumbnail needs a path, and a width and height up to 16384";
    return false;
  }
  return true;
}

void Preview::start(uint32_t width, uint32_t height, const PreviewSettings *preview,
                    const ThumbnailSettings *thumbnail) {
  stop();
  canvas_width_ = width;
  canvas_height_ = height;
  preview_on_ = preview != nullptr;
  thumbnail_on_ = thumbnail != nullptr;
  preview_ = preview ? *preview : PreviewSettings{};
  thumbnail_ = thumbnail ? *thumbnail : ThumbnailSettings{};

  first_ns_ = 0;
  next_tile_ = 0;
  first_wall_ms_ = 0;
  grab_count_ = grab_ns_total_ = grab_ns_max_ = 0;
  map_count_ = map_ns_total_ = map_ns_max_ = 0;
  sheet_index_ = 0;
  sheet_dirty_ = false;
  tiles_ = 0;
  sheet_first_wall_ms_ = 0;
  sheet_writes_ = thumbnail_writes_ = 0;
  sheet_ms_total_ = sheet_ms_max_ = thumbnail_ms_total_ = thumbnail_ms_max_ = 0;
  if (preview_on_) {
    sheet_.assign((size_t)preview_.columns * preview_.tile_width * preview_.rows * preview_.tile_height * 3, 0);
    // The core creates the folder; this only covers a script or a folder removed since.
    os_mkdirs(preview_.directory.c_str());
  }
  {
    std::lock_guard lock(mutex_);
    grabbing_ = true;
    auto_done_ = false;
    requests_.clear();
    in_flight_ = 0;
  }
  jobs_.clear();
  worker_ = std::thread(&Preview::work, this);
  obs_add_main_rendered_callback(on_rendered, this);
  running_ = true;
}

void Preview::auto_thumbnail() {
  std::lock_guard lock(mutex_);
  if (!grabbing_ || !thumbnail_on_ || auto_done_) {
    return;
  }
  auto_done_ = true;
  requests_.push_back({-1, thumbnail_});
}

bool Preview::thumbnail(long long id, obs_data_t *params, std::string &error) {
  Request request{id, thumbnail_};
  if (const char *path = obs_data_get_string(params, "path"); *path) {
    request.settings.path = path;
  }
  if (obs_data_has_user_value(params, "width")) {
    request.settings.width = get_uint(params, "width");
  }
  if (obs_data_has_user_value(params, "height")) {
    request.settings.height = get_uint(params, "height");
  }
  if (!request.settings.width && !obs_data_has_user_value(params, "width")) {
    request.settings.width = kDefaultThumbnailWidth;
  }
  if (!request.settings.height && !obs_data_has_user_value(params, "height")) {
    request.settings.height = kDefaultThumbnailHeight;
  }
  if (request.settings.path.empty()) {
    error = "missing path";
    return false;
  }
  if (!request.settings.width || !request.settings.height || request.settings.width > kMaxSheetSide ||
      request.settings.height > kMaxSheetSide) {
    error = "width and height must be between 1 and 16384";
    return false;
  }
  std::lock_guard lock(mutex_);
  if (!running_ || !grabbing_) {
    error = "not recording";
    return false;
  }
  requests_.push_back(std::move(request));
  return true;
}

void Preview::stop() {
  if (!running_) {
    return;
  }
  // No new tile is grabbed from here: the sheets end before the video does. Readbacks already staged finish.
  {
    std::unique_lock lock(mutex_);
    grabbing_ = false;
    idle_cv_.wait_for(lock, kDrainTimeout, [this] { return requests_.empty() && in_flight_ == 0; });
  }
  // Waits for a running callback: libobs calls them under the lock this takes.
  obs_remove_main_rendered_callback(on_rendered, this);

  std::deque<Request> unanswered;
  {
    std::lock_guard lock(mutex_);
    unanswered.swap(requests_);
  }
  for (Readback *readback : {&tile_readbacks_[0], &tile_readbacks_[1], &thumbnail_readback_}) {
    if (readback->busy && !readback->tile) {
      unanswered.push_back(readback->request);
    }
    readback->busy = false;
  }
  for (const Request &request : unanswered) {
    if (request.id >= 0) {
      protocol::send_error("thumbnail", request.id, "not recording");
    }
  }

  obs_enter_graphics();
  destroy_chain(tile_chain_);
  destroy_chain(thumbnail_chain_);
  for (Readback *readback : {&tile_readbacks_[0], &tile_readbacks_[1], &thumbnail_readback_}) {
    gs_stagesurface_destroy(readback->surface);
    *readback = Readback{};
  }
  obs_leave_graphics();

  {
    std::lock_guard lock(jobs_mutex_);
    jobs_.push_back(Job{});  // Finish: the last sheet, after every image queued before it
    jobs_cv_.notify_one();
  }
  worker_.join();
  sheet_.clear();
  sheet_.shrink_to_fit();
  running_ = false;

  // CPU time on the graphics thread (the GPU work itself is asynchronous) and on the worker.
  blog(LOG_INFO,
       "preview: %u tiles; grab %llu x avg %.3f ms max %.3f ms; readback %llu x avg %.3f ms max %.3f ms; "
       "sheet %llu x avg %.1f ms max %.1f ms; thumbnail %llu x avg %.1f ms max %.1f ms",
       tiles_, (unsigned long long)grab_count_, grab_count_ ? grab_ns_total_ / 1e6 / grab_count_ : 0.0,
       grab_ns_max_ / 1e6, (unsigned long long)map_count_, map_count_ ? map_ns_total_ / 1e6 / map_count_ : 0.0,
       map_ns_max_ / 1e6, (unsigned long long)sheet_writes_, sheet_writes_ ? sheet_ms_total_ / sheet_writes_ : 0.0,
       sheet_ms_max_, (unsigned long long)thumbnail_writes_,
       thumbnail_writes_ ? thumbnail_ms_total_ / thumbnail_writes_ : 0.0, thumbnail_ms_max_);
}

// ---- Graphics thread ----

void Preview::on_rendered(void *param) {
  static_cast<Preview *>(param)->render();
}

void Preview::render() {
  // Readbacks staged kReadbackDelayFrames ago: the copy is done on the GPU, the map returns at once.
  for (Readback *readback : {&tile_readbacks_[0], &tile_readbacks_[1], &thumbnail_readback_}) {
    if (readback->busy && ++readback->frames >= kReadbackDelayFrames) {
      const uint64_t begin = os_gettime_ns();
      read_back(*readback);
      const uint64_t spent = os_gettime_ns() - begin;
      map_count_++;
      map_ns_total_ += spent;
      map_ns_max_ = std::max(map_ns_max_, spent);
    }
  }

  // The frame being composited: the timestamp its video packet carries (anchor.wallMs's clock).
  const uint64_t frame_ns = obs_get_video_frame_time();
  std::unique_lock lock(mutex_);
  if (!grabbing_) {
    if (requests_.empty() && in_flight_ == 0) {
      idle_cv_.notify_all();
    }
  }

  if (grabbing_ && preview_on_) {
    if (!first_ns_) {
      first_ns_ = frame_ns;
      first_wall_ms_ = obs_ns_to_wall_ms(frame_ns);
    }
    const uint64_t interval_ns = (uint64_t)preview_.interval_ms * 1000000;
    if (frame_ns >= first_ns_ + next_tile_ * interval_ns) {
      Readback *slot = !tile_readbacks_[0].busy ? &tile_readbacks_[0] : !tile_readbacks_[1].busy ? &tile_readbacks_[1] : nullptr;
      if (slot) {
        // Every tile whose time has come: after lagged frames this one frame stands for several, so tile k stays
        // at firstWallMs + k * intervalMs.
        const uint32_t due = (uint32_t)((frame_ns - first_ns_) / interval_ns) + 1;
        const uint64_t begin = os_gettime_ns();
        if (stage(*slot, tile_chain_, preview_.tile_width, preview_.tile_height)) {
          slot->tile = true;
          slot->first_tile = next_tile_;
          slot->count = due - next_tile_;
          slot->wall_ms = first_wall_ms_;
          in_flight_++;
        }
        next_tile_ = due;
        const uint64_t spent = os_gettime_ns() - begin;
        grab_count_++;
        grab_ns_total_ += spent;
        grab_ns_max_ = std::max(grab_ns_max_, spent);
      }
    }
  }

  if (!requests_.empty() && !thumbnail_readback_.busy) {
    Request request = std::move(requests_.front());
    requests_.pop_front();
    const uint64_t begin = os_gettime_ns();
    if (stage(thumbnail_readback_, thumbnail_chain_, request.settings.width, request.settings.height)) {
      thumbnail_readback_.tile = false;
      thumbnail_readback_.request = std::move(request);
      thumbnail_readback_.wall_ms = obs_ns_to_wall_ms(frame_ns);
      in_flight_++;
    } else if (request.id >= 0) {
      protocol::send_error("thumbnail", request.id, "the picture could not be rendered");
    } else {
      send_warning("thumbnail: the picture could not be rendered");
    }
    const uint64_t spent = os_gettime_ns() - begin;
    grab_count_++;
    grab_ns_total_ += spent;
    grab_ns_max_ = std::max(grab_ns_max_, spent);
  }
}

void Preview::ensure_chain(Chain &chain, uint32_t width, uint32_t height) {
  if (chain.width == width && chain.height == height && !chain.passes.empty()) {
    return;
  }
  destroy_chain(chain);
  chain.width = width;
  chain.height = height;
  // The canvas keeps its aspect inside the target: 16:9 fills a 16:9 tile, anything else gets black bars.
  const double scale = std::min((double)width / canvas_width_, (double)height / canvas_height_);
  chain.fit_width = std::max(1u, (uint32_t)(canvas_width_ * scale + 0.5));
  chain.fit_height = std::max(1u, (uint32_t)(canvas_height_ * scale + 0.5));
  chain.fit_x = (int)(width - chain.fit_width) / 2;
  chain.fit_y = (int)(height - chain.fit_height) / 2;
  // A 12x reduction in one bilinear tap would read 4 of every 144 pixels and alias. Each halving averages 2x2.
  uint32_t w = canvas_width_;
  uint32_t h = canvas_height_;
  while (w / 2 >= chain.fit_width && h / 2 >= chain.fit_height) {
    w /= 2;
    h /= 2;
    chain.pass_width.push_back(w);
    chain.pass_height.push_back(h);
  }
  chain.pass_width.push_back(width);
  chain.pass_height.push_back(height);
  for (size_t i = 0; i < chain.pass_width.size(); i++) {
    chain.passes.push_back(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
  }
}

gs_texture_t *Preview::downscale(Chain &chain) {
  gs_texture_t *source = nullptr;
  uint32_t source_width = canvas_width_;
  uint32_t source_height = canvas_height_;
  vec4 black;
  vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
  for (size_t i = 0; i < chain.passes.size(); i++) {
    gs_texrender_t *pass = chain.passes[i];
    const bool last = i + 1 == chain.passes.size();
    gs_texrender_reset(pass);
    if (!pass || !gs_texrender_begin_with_color_space(pass, chain.pass_width[i], chain.pass_height[i], GS_CS_SRGB)) {
      return nullptr;
    }
    gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
    gs_ortho(0.0f, (float)source_width, 0.0f, (float)source_height, -100.0f, 100.0f);
    if (last) {
      gs_set_viewport(chain.fit_x, chain.fit_y, (int)chain.fit_width, (int)chain.fit_height);
    }
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    if (!source) {
      // The canvas as the encoders get it, before the NV12 conversion: sRGB RGBA.
      obs_render_main_texture_src_color_only();
    } else {
      // Sampled and written through sRGB views, as libobs draws the canvas: filtering happens in linear light.
      const bool previous = gs_framebuffer_srgb_enabled();
      gs_enable_framebuffer_srgb(true);
      gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
      gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), source);
      while (gs_effect_loop(effect, "Draw")) {
        gs_draw_sprite(source, 0, 0, 0);
      }
      gs_enable_framebuffer_srgb(previous);
    }
    gs_blend_state_pop();
    gs_texrender_end(pass);
    source = gs_texrender_get_texture(pass);
    source_width = chain.pass_width[i];
    source_height = chain.pass_height[i];
  }
  return source;
}

void Preview::destroy_chain(Chain &chain) {
  for (gs_texrender_t *pass : chain.passes) {
    gs_texrender_destroy(pass);
  }
  chain = Chain{};
}

bool Preview::stage(Readback &readback, Chain &chain, uint32_t width, uint32_t height) {
  ensure_chain(chain, width, height);
  if (!readback.surface || readback.width != width || readback.height != height) {
    gs_stagesurface_destroy(readback.surface);
    readback.surface = gs_stagesurface_create(width, height, GS_RGBA);
    readback.width = width;
    readback.height = height;
  }
  gs_texture_t *texture = readback.surface ? downscale(chain) : nullptr;
  if (!texture) {
    return false;
  }
  gs_stage_texture(readback.surface, texture);
  readback.busy = true;
  readback.frames = 0;
  return true;
}

// Caller does not hold mutex_.
void Preview::read_back(Readback &readback) {
  Job job;
  job.kind = readback.tile ? Job::Kind::Tiles : Job::Kind::Thumbnail;
  job.width = readback.width;
  job.height = readback.height;
  job.first_tile = readback.first_tile;
  job.count = readback.count;
  job.request = std::move(readback.request);
  job.wall_ms = readback.wall_ms;
  uint8_t *data = nullptr;
  uint32_t linesize = 0;
  if (gs_stagesurface_map(readback.surface, &data, &linesize)) {
    const size_t row = (size_t)readback.width * 4;
    job.rgba.resize(row * readback.height);
    for (uint32_t y = 0; y < readback.height; y++) {
      memcpy(job.rgba.data() + y * row, data + (size_t)y * linesize, row);
    }
    gs_stagesurface_unmap(readback.surface);
  }
  readback.busy = false;
  push(std::move(job));
  std::lock_guard lock(mutex_);
  in_flight_--;
  if (!grabbing_ && requests_.empty() && in_flight_ == 0) {
    idle_cv_.notify_all();
  }
}

void Preview::push(Job &&job) {
  std::lock_guard lock(jobs_mutex_);
  if (jobs_.size() >= kMaxJobs) {
    // The worker is far behind (a stalled disk): this image is lost rather than memory growing.
    blog(LOG_WARNING, "preview: worker queue full, image dropped");
    job.rgba.clear();
  }
  jobs_.push_back(std::move(job));
  jobs_cv_.notify_one();
}

// ---- Worker ----

void Preview::work() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  while (true) {
    Job job;
    {
      std::unique_lock lock(jobs_mutex_);
      jobs_cv_.wait(lock, [this] { return !jobs_.empty(); });
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    if (job.kind == Job::Kind::Tiles) {
      place_tiles(job);
    } else if (job.kind == Job::Kind::Thumbnail) {
      write_thumbnail(job);
    } else {
      // Once more before `stopped`, so the last sheet holds every tile.
      if (sheet_dirty_) {
        write_sheet();
      }
      break;
    }
  }
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
}

void Preview::place_tiles(const Job &job) {
  const uint32_t per_sheet = preview_.columns * preview_.rows;
  const size_t sheet_stride = (size_t)preview_.columns * preview_.tile_width * 3;
  sheet_first_wall_ms_ = job.wall_ms;
  for (uint32_t i = 0; i < job.count; i++) {
    const uint32_t tile = job.first_tile + i;
    const uint32_t sheet = tile / per_sheet;
    if (sheet != sheet_index_) {
      if (sheet_dirty_) {
        write_sheet();
      }
      sheet_index_ = sheet;
      std::fill(sheet_.begin(), sheet_.end(), 0);  // cells not grabbed yet are black
    }
    const uint32_t cell = tile % per_sheet;
    if (!job.rgba.empty()) {
      uint8_t *origin =
          sheet_.data() + (size_t)(cell / preview_.columns) * preview_.tile_height * sheet_stride +
          (size_t)(cell % preview_.columns) * preview_.tile_width * 3;
      for (uint32_t y = 0; y < preview_.tile_height; y++) {
        const uint8_t *src = job.rgba.data() + (size_t)y * job.width * 4;
        uint8_t *dst = origin + y * sheet_stride;
        for (uint32_t x = 0; x < preview_.tile_width; x++) {
          dst[x * 3 + 0] = src[x * 4 + 2];
          dst[x * 3 + 1] = src[x * 4 + 1];
          dst[x * 3 + 2] = src[x * 4 + 0];
        }
      }
    }
    tiles_ = tile + 1;
    sheet_dirty_ = true;
    if (cell == per_sheet - 1 || tiles_ % preview_.flush_tiles == 0) {
      write_sheet();
    }
  }
}

void Preview::write_sheet() {
  char name[32];
  snprintf(name, sizeof(name), "_%06u.jpg", sheet_index_);
  const std::string file = preview_.directory + "/" + preview_.prefix + name;
  const uint32_t width = preview_.columns * preview_.tile_width;
  const uint32_t height = preview_.rows * preview_.tile_height;
  const uint64_t begin = os_gettime_ns();
  std::string error;
  if (!write_jpeg(file, sheet_.data(), width, height, width * 3, kSheetQuality, error)) {
    // Kept dirty: the next flush writes it again.
    send_warning("preview: " + error);
    return;
  }
  const double spent = ms_since(begin);
  sheet_writes_++;
  sheet_ms_total_ += spent;
  sheet_ms_max_ = std::max(sheet_ms_max_, spent);
  sheet_dirty_ = false;

  OBSDataAutoRelease event = obs_data_create();
  obs_data_set_string(event, "event", "preview");
  obs_data_set_string(event, "file", file.c_str());
  obs_data_set_int(event, "sheet", sheet_index_);
  obs_data_set_int(event, "tiles", tiles_);
  obs_data_set_double(event, "firstWallMs", sheet_first_wall_ms_);
  obs_data_set_int(event, "intervalMs", preview_.interval_ms);
  protocol::send(event);
}

void Preview::write_thumbnail(const Job &job) {
  const Request &request = job.request;
  std::string error;
  if (job.rgba.empty()) {
    error = request.settings.path + ": the picture could not be read back";
  } else {
    const uint64_t begin = os_gettime_ns();
    std::vector<uint8_t> bgr((size_t)job.width * job.height * 3);
    for (size_t i = 0, n = (size_t)job.width * job.height; i < n; i++) {
      bgr[i * 3 + 0] = job.rgba[i * 4 + 2];
      bgr[i * 3 + 1] = job.rgba[i * 4 + 1];
      bgr[i * 3 + 2] = job.rgba[i * 4 + 0];
    }
    if (write_jpeg(request.settings.path, bgr.data(), job.width, job.height, job.width * 3, kThumbnailQuality,
                   error)) {
      const double spent = ms_since(begin);
      thumbnail_writes_++;
      thumbnail_ms_total_ += spent;
      thumbnail_ms_max_ = std::max(thumbnail_ms_max_, spent);
      OBSDataAutoRelease event = obs_data_create();
      obs_data_set_string(event, "event", "thumbnail");
      if (request.id >= 0) {
        obs_data_set_int(event, "id", request.id);
      }
      obs_data_set_string(event, "path", request.settings.path.c_str());
      obs_data_set_double(event, "wallMs", job.wall_ms);
      protocol::send(event);
      return;
    }
  }
  // The command waits for a reply; the automatic one only warns. Neither stops the recording.
  if (request.id >= 0) {
    blog(LOG_WARNING, "thumbnail: %s", error.c_str());
    protocol::send_error("thumbnail", request.id, error);
  } else {
    send_warning("thumbnail: " + error);
  }
}
