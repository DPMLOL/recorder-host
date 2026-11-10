#pragma once

#include <obs.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct PreviewSettings {
  std::string directory;
  std::string prefix;
  uint32_t tile_width = 0;
  uint32_t tile_height = 0;
  uint32_t interval_ms = 0;
  uint32_t columns = 0;
  uint32_t rows = 0;
  uint32_t flush_tiles = 5;
};

struct ThumbnailSettings {
  std::string path;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Preview tiles packed into Sprite sheets, and Thumbnails, written while the game records so nothing is decoded
// afterwards. The composited picture (the canvas before NV12 conversion) is downscaled on the GPU from libobs's
// main-rendered callback and read back a few frames later, so the render thread never waits on the GPU. JPEGs are
// encoded and written on a worker thread.
class Preview {
 public:
  ~Preview();

  // start.preview and start.thumbnail: false with an error when present but invalid; `present` tells if given.
  static bool parse_preview(obs_data_t *params, PreviewSettings &out, bool &present, std::string &error);
  static bool parse_thumbnail(obs_data_t *params, ThumbnailSettings &out, bool &present, std::string &error);

  // Canvas size; null settings turn that part off. Thumbnails on command work either way.
  void start(uint32_t width, uint32_t height, const PreviewSettings *preview, const ThumbnailSettings *thumbnail);
  // The automatic Thumbnail of start.thumbnail, at the next composited frame (no-op without start.thumbnail).
  void auto_thumbnail();
  // The `thumbnail` command. The worker replies `thumbnail {id, path, wallMs}` once the file is written.
  bool thumbnail(long long id, obs_data_t *params, std::string &error);
  // Stops grabbing and writes the last sheet; every `preview` and `thumbnail` event is sent when it returns.
  void stop();

  bool running() const { return running_; }
  bool preview_active() const { return running_ && preview_on_; }
  bool thumbnail_active() const { return running_ && thumbnail_on_; }
  // Tiles grabbed since start, valid once stop() returned.
  uint32_t tiles() const { return tiles_; }

 private:
  // Downscale passes from the canvas to one image: halvings (each a 2x2 box filter in one bilinear tap), then one
  // pass to the target, letterboxed when the canvas aspect differs.
  struct Chain {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<gs_texrender_t *> passes;
    std::vector<uint32_t> pass_width;
    std::vector<uint32_t> pass_height;
    int fit_x = 0;
    int fit_y = 0;
    uint32_t fit_width = 0;
    uint32_t fit_height = 0;
  };

  struct Request {
    long long id = -1;  // -1: the automatic Thumbnail
    ThumbnailSettings settings;
  };

  // A staged image waiting a few frames for the GPU before it is mapped.
  struct Readback {
    gs_stagesurf_t *surface = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool busy = false;
    int frames = 0;
    bool tile = false;
    uint32_t first_tile = 0;
    uint32_t count = 0;
    Request request;
    double wall_ms = 0;
  };

  struct Job {
    enum class Kind { Tiles, Thumbnail, Finish } kind = Kind::Finish;
    std::vector<uint8_t> rgba;  // packed, width * 4 bytes per row; empty if the readback failed
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t first_tile = 0;
    uint32_t count = 0;
    Request request;
    double wall_ms = 0;
  };

  static void on_rendered(void *param);
  void render();
  void ensure_chain(Chain &chain, uint32_t width, uint32_t height);
  gs_texture_t *downscale(Chain &chain);
  void destroy_chain(Chain &chain);
  bool stage(Readback &readback, Chain &chain, uint32_t width, uint32_t height);
  void read_back(Readback &readback);
  void push(Job &&job);

  void work();
  void place_tiles(const Job &job);
  void write_sheet();
  void write_thumbnail(const Job &job);

  bool running_ = false;
  bool preview_on_ = false;
  bool thumbnail_on_ = false;
  uint32_t canvas_width_ = 0;
  uint32_t canvas_height_ = 0;
  PreviewSettings preview_;
  ThumbnailSettings thumbnail_;

  // Shared between the command thread, the graphics thread and signal threads.
  std::mutex mutex_;
  std::condition_variable idle_cv_;
  bool grabbing_ = false;
  bool auto_done_ = false;
  std::deque<Request> requests_;
  int in_flight_ = 0;

  // Graphics thread only (and stop(), once the callback is removed).
  Chain tile_chain_;
  Chain thumbnail_chain_;
  Readback tile_readbacks_[2];
  Readback thumbnail_readback_;
  uint64_t first_ns_ = 0;
  uint32_t next_tile_ = 0;
  double first_wall_ms_ = 0;
  uint64_t grab_count_ = 0, grab_ns_total_ = 0, grab_ns_max_ = 0;
  uint64_t map_count_ = 0, map_ns_total_ = 0, map_ns_max_ = 0;

  // Worker.
  std::thread worker_;
  std::mutex jobs_mutex_;
  std::condition_variable jobs_cv_;
  std::deque<Job> jobs_;
  std::vector<uint8_t> sheet_;  // BGR, columns * tile_width wide
  uint32_t sheet_index_ = 0;
  bool sheet_dirty_ = false;
  uint32_t tiles_ = 0;
  double sheet_first_wall_ms_ = 0;
  uint64_t sheet_writes_ = 0;
  double sheet_ms_total_ = 0, sheet_ms_max_ = 0;
  uint64_t thumbnail_writes_ = 0;
  double thumbnail_ms_total_ = 0, thumbnail_ms_max_ = 0;
};
