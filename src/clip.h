#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// The `clip` command: [from_sec, to_sec] of an MP4, in its file time.
struct ClipJob {
  long long id = 0;
  std::string source;
  double from_sec = 0;
  double to_sec = 0;
  std::string path;
};

struct ClipResult {
  bool ok = false;
  // Why it failed, when !ok.
  std::string message;
  // The keyframe the Clip starts on, in the source's file time: at or before the asked from_sec.
  double from_sec = 0;
  long long duration_ms = 0;
};

// Stream-copies [job.from_sec, job.to_sec] of job.source into a new MP4 at job.path, every video and audio stream,
// starting on the keyframe at or before from_sec. No decoding, no encoding.
//
// The source may be a Hybrid MP4 still being written (mp4_output: ftyp, free, a moov without samples, then one
// moof + mdat per GOP): only the fragments already complete on disk are read, and the reading starts at the
// fragment holding from_sec instead of the top of the file. The Clip goes to `<path>.part`, then is renamed over
// `path`, so a reader never sees half a file. Checks `cancel` between packets.
ClipResult cut_clip(const ClipJob &job, const std::atomic<bool> &cancel);

// Cuts Clips one at a time on a thread of its own, in background mode (lowest CPU and I/O priority), so cutting
// never takes time or disk bandwidth from the outputs. `done` is called on that thread for every job pushed, once.
class Clipper {
 public:
  using Done = std::function<void(const ClipJob &, const ClipResult &)>;

  explicit Clipper(Done done);
  ~Clipper();

  void push(ClipJob job);
  // Cancels the Clip being cut, fails the queued ones and joins the thread. push() after stop() fails the job.
  void stop();

 private:
  void run();

  Done done_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<ClipJob> queue_;
  bool stopping_ = false;
  std::atomic<bool> cancel_{false};
  std::thread thread_;
};
