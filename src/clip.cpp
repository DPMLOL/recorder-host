#include "clip.h"

#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace {

// Larger boxes are not ours to read whole: an mp4_output moov is a few KB while recording, a moof a few hundred bytes.
constexpr uint64_t kMaxHeaderBox = 64ull << 20;
constexpr int kIoBufferSize = 256 << 10;

std::wstring widen(const std::string &utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
  std::wstring wide(len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), wide.data(), len);
  return wide;
}

std::string av_error(int code) {
  if (code == AVERROR(ENOSPC)) {
    return "disk full";
  }
  char text[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, text, sizeof(text));
  return text;
}

uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint64_t be64(const uint8_t *p) {
  return (uint64_t)be32(p) << 32 | be32(p + 4);
}

void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

// The source, opened so the output still writing it is never in the way (and never sees us).
class SourceFile {
 public:
  ~SourceFile() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

  bool open(const std::string &path) {
    handle_ = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    return handle_ != INVALID_HANDLE_VALUE;
  }

  int64_t size() const {
    LARGE_INTEGER size{};
    return GetFileSizeEx(handle_, &size) ? size.QuadPart : -1;
  }

  // Fewer bytes than asked only at the end of the file; -1 on a read error.
  int64_t read_at(int64_t offset, void *buffer, size_t bytes) const {
    size_t total = 0;
    while (total < bytes) {
      OVERLAPPED at{};
      at.Offset = (DWORD)(offset + total);
      at.OffsetHigh = (DWORD)((uint64_t)(offset + total) >> 32);
      const DWORD chunk = (DWORD)std::min<size_t>(bytes - total, 1u << 30);
      DWORD got = 0;
      if (!ReadFile(handle_, (uint8_t *)buffer + total, chunk, &got, &at)) {
        return GetLastError() == ERROR_HANDLE_EOF ? (int64_t)total : -1;
      }
      if (got == 0) {
        break;
      }
      total += got;
    }
    return (int64_t)total;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

struct Box {
  uint64_t offset = 0;
  uint64_t size = 0;
  uint32_t header = 0;
  char type[5] = {};
};

// The box at `offset` of a buffer of `end` bytes; false when it does not fit.
bool box_at(const uint8_t *data, uint64_t offset, uint64_t end, Box &box) {
  if (offset + 8 > end) {
    return false;
  }
  box.offset = offset;
  box.size = be32(data + offset);
  box.header = 8;
  memcpy(box.type, data + offset + 4, 4);
  if (box.size == 1) {
    if (offset + 16 > end) {
      return false;
    }
    box.size = be64(data + offset + 8);
    box.header = 16;
  } else if (box.size == 0) {
    box.size = end - offset;
  }
  return box.size >= box.header && box.size <= end - offset;
}

template <typename Visit>
void for_each_child(const uint8_t *data, const Box &parent, uint32_t skip, Visit visit) {
  const uint64_t end = parent.offset + parent.size;
  Box child;
  for (uint64_t at = parent.offset + parent.header + skip; box_at(data, at, end, child); at += child.size) {
    visit(child);
  }
}

bool is(const Box &box, const char *type) {
  return memcmp(box.type, type, 4) == 0;
}

struct Fragment {
  int64_t offset = 0;
  // Of the video track, in its timescale: baseMediaDecodeTime (tfdt), and when the fragment's first sample (its
  // keyframe) shows: tfdt plus that sample's composition offset (trun, negative ones too: mp4_output writes
  // version 1 runs, offsets relative to the first frame's).
  uint64_t video_dts = 0;
  int64_t video_first_pts = 0;
};

// Where the parts of the source are, from its top-level boxes. A moof counts once its mdat is entirely on disk.
struct Layout {
  bool fragmented = false;
  int64_t moov_end = 0;
  // How far the source may be read: the end of the last complete fragment, or the whole file when not fragmented.
  int64_t end = 0;
  uint32_t video_track = 0;
  uint32_t video_timescale = 0;
  std::vector<Fragment> fragments;
};

// moov: the video track (tkhd track_ID, mdhd timescale) and whether samples come in fragments (mvex).
void parse_moov(const uint8_t *data, const Box &moov, Layout &layout) {
  for_each_child(data, moov, 0, [&](const Box &child) {
    if (is(child, "mvex")) {
      layout.fragmented = true;
    }
    if (!is(child, "trak")) {
      return;
    }
    uint32_t track_id = 0;
    uint32_t timescale = 0;
    bool video = false;
    for_each_child(data, child, 0, [&](const Box &trak_child) {
      const uint8_t *p = data + trak_child.offset + trak_child.header;
      const uint64_t body = trak_child.size - trak_child.header;
      if (is(trak_child, "tkhd") && body >= 24) {
        track_id = p[0] == 1 ? be32(p + 20) : be32(p + 12);
      } else if (is(trak_child, "mdia")) {
        for_each_child(data, trak_child, 0, [&](const Box &mdia_child) {
          const uint8_t *q = data + mdia_child.offset + mdia_child.header;
          const uint64_t mdia_body = mdia_child.size - mdia_child.header;
          if (is(mdia_child, "mdhd") && mdia_body >= 24) {
            timescale = q[0] == 1 ? be32(q + 20) : be32(q + 12);
          } else if (is(mdia_child, "hdlr") && mdia_body >= 12) {
            video = memcmp(q + 8, "vide", 4) == 0;
          }
        });
      }
    });
    if (video && !layout.video_track) {
      layout.video_track = track_id;
      layout.video_timescale = timescale;
    }
  });
}

// trun: the composition offset of the run's first sample, 0 without offsets.
int64_t first_composition_offset(const uint8_t *p, uint64_t body) {
  if (body < 8) {
    return 0;
  }
  const bool v1 = p[0] == 1;
  const uint32_t flags = (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
  if (!(flags & 0x800) || be32(p + 4) == 0) {
    return 0;
  }
  // sample_count, then data_offset (0x1) and first_sample_flags (0x4), then the first sample's duration (0x100),
  // size (0x200) and flags (0x400) before its composition offset.
  uint64_t at = 8;
  for (const uint32_t field : {0x1u, 0x4u, 0x100u, 0x200u, 0x400u}) {
    at += (flags & field) ? 4 : 0;
  }
  if (at + 4 > body) {
    return 0;
  }
  return v1 ? (int64_t)(int32_t)be32(p + at) : (int64_t)be32(p + at);
}

// moof: tfdt and first sample of the video track's traf.
bool parse_moof(const uint8_t *data, const Box &moof, uint32_t video_track, Fragment &fragment) {
  bool found = false;
  for_each_child(data, moof, 0, [&](const Box &traf) {
    if (!is(traf, "traf")) {
      return;
    }
    uint32_t track_id = 0;
    uint64_t dts = 0;
    int64_t offset = 0;
    bool has_dts = false;
    bool has_run = false;
    for_each_child(data, traf, 0, [&](const Box &child) {
      const uint8_t *p = data + child.offset + child.header;
      const uint64_t body = child.size - child.header;
      if (is(child, "tfhd") && body >= 8) {
        track_id = be32(p + 4);
      } else if (is(child, "tfdt") && body >= 8) {
        has_dts = true;
        dts = p[0] == 1 && body >= 12 ? be64(p + 4) : be32(p + 4);
      } else if (is(child, "trun") && !has_run) {
        has_run = true;
        offset = first_composition_offset(p, body);
      }
    });
    if (track_id == video_track && has_dts) {
      fragment.video_dts = dts;
      fragment.video_first_pts = (int64_t)dts + offset;
      found = true;
    }
  });
  return found;
}

bool read_box(const SourceFile &file, int64_t offset, uint64_t size, std::vector<uint8_t> &data) {
  if (size > kMaxHeaderBox) {
    return false;
  }
  data.resize((size_t)size);
  return file.read_at(offset, data.data(), data.size()) == (int64_t)size;
}

// Walks the top-level boxes, skipping over mdat payloads: a few reads per fragment, nothing of the media.
bool scan(const SourceFile &file, Layout &layout, std::string &error) {
  const int64_t file_size = file.size();
  if (file_size < 0) {
    error = "source unreadable";
    return false;
  }
  bool has_moov = false;
  Fragment pending{-1, 0, 0};
  std::vector<uint8_t> data;
  uint8_t header[16];
  int64_t at = 0;
  while (at + 8 <= file_size) {
    const int64_t got = file.read_at(at, header, (size_t)std::min<int64_t>(16, file_size - at));
    if (got < 8) {
      break;
    }
    Box box;
    // Offsets relative to `header`: the size field may point past the 16 bytes read.
    if (!box_at(header, 0, UINT64_MAX, box) || box.size == 0) {
      break;
    }
    if (be32(header) == 0) {
      box.size = (uint64_t)(file_size - at);
    }
    // A box still being written: everything before it is what can be read.
    if ((uint64_t)(file_size - at) < box.size) {
      break;
    }
    // The first moov only: while mp4_output finalises, the full moov is appended after the fragments before the
    // header is rewritten, and the fragmented one still describes the file.
    if (is(box, "moov") && !has_moov) {
      if (!read_box(file, at, box.size, data)) {
        error = "source unreadable";
        return false;
      }
      Box moov;
      box_at(data.data(), 0, data.size(), moov);
      parse_moov(data.data(), moov, layout);
      layout.moov_end = at + (int64_t)box.size;
      has_moov = true;
    } else if (is(box, "moof") && layout.fragmented && layout.video_track) {
      pending = {at, 0, 0};
      Box moof;
      if (!read_box(file, at, box.size, data) || !box_at(data.data(), 0, data.size(), moof) ||
          !parse_moof(data.data(), moof, layout.video_track, pending)) {
        pending.offset = -1;
      }
    } else if (is(box, "mdat") && pending.offset >= 0) {
      layout.fragments.push_back(pending);
      layout.end = at + (int64_t)box.size;
      pending.offset = -1;
    }
    at += (int64_t)box.size;
  }

  if (!has_moov) {
    error = "source unreadable: no moov";
    return false;
  }
  if (!layout.video_track) {
    error = "source has no video";
    return false;
  }
  if (!layout.fragmented) {
    layout.end = file_size;
  } else if (layout.fragments.empty()) {
    error = "source has no complete fragment yet";
    return false;
  }
  return true;
}

// What the demuxer reads: the source as it is on disk, up to `end`, with `head` served in its place from offset 0.
// `head` is the init part (ftyp, free, moov) copied at scan time, then, to start in the middle of a fragmented
// source, a `free` box that covers every fragment before the first one needed: the demuxer skips it, and the
// fragments keep their real offsets (each tfhd holds its moof's absolute offset as base_data_offset).
struct VirtualSource {
  const SourceFile *file = nullptr;
  std::vector<uint8_t> head;
  int64_t end = 0;
  int64_t pos = 0;

  static int read(void *opaque, uint8_t *buffer, int size) {
    auto *self = static_cast<VirtualSource *>(opaque);
    const int64_t want = std::min<int64_t>(size, self->end - self->pos);
    if (want <= 0) {
      return AVERROR_EOF;
    }
    int64_t done = 0;
    if (self->pos < (int64_t)self->head.size()) {
      done = std::min<int64_t>(want, (int64_t)self->head.size() - self->pos);
      memcpy(buffer, self->head.data() + self->pos, (size_t)done);
    }
    if (done < want) {
      const int64_t got = self->file->read_at(self->pos + done, buffer + done, (size_t)(want - done));
      if (got < 0) {
        return done ? (int)done : AVERROR(EIO);
      }
      done += got;
    }
    self->pos += done;
    return done ? (int)done : AVERROR_EOF;
  }

  static int64_t seek(void *opaque, int64_t offset, int whence) {
    auto *self = static_cast<VirtualSource *>(opaque);
    switch (whence & ~AVSEEK_FORCE) {
      case AVSEEK_SIZE:
        return self->end;
      case SEEK_SET:
        break;
      case SEEK_CUR:
        offset += self->pos;
        break;
      case SEEK_END:
        offset += self->end;
        break;
      default:
        return AVERROR(EINVAL);
    }
    if (offset < 0) {
      return AVERROR(EINVAL);
    }
    self->pos = offset;
    return offset;
  }
};

// The first fragment to read for a cut from `from_sec`. Fragments start on a video keyframe (mp4_output flushes one
// per GOP), so the one holding the keyframe at or before from_sec is the last that shows at or before it. One more
// is read before it: the audio of the keyframe's first instant may come before its video in the file, and the
// packets read before the first video one are dropped (see calibration in cut_clip).
size_t first_fragment(const Layout &layout, double from_sec) {
  const double from_ticks = from_sec * layout.video_timescale;
  size_t index = 0;
  for (size_t i = 0; i < layout.fragments.size(); i++) {
    if ((double)layout.fragments[i].video_first_pts <= from_ticks) {
      index = i;
    }
  }
  return index > 0 ? index - 1 : 0;
}

struct InputCloser {
  AVFormatContext *ctx = nullptr;
  AVIOContext *io = nullptr;
  ~InputCloser() {
    avformat_close_input(&ctx);
    if (io) {
      av_freep(&io->buffer);
      avio_context_free(&io);
    }
  }
};

struct OutputCloser {
  AVFormatContext *ctx = nullptr;
  // The `.part`, deleted unless renamed (cleared then).
  std::wstring path;
  ~OutputCloser() {
    if (ctx) {
      if (ctx->pb) {
        avio_closep(&ctx->pb);
      }
      avformat_free_context(ctx);
    }
    if (!path.empty()) {
      DeleteFileW(path.c_str());
    }
  }
};

struct PacketList {
  std::vector<AVPacket *> packets;
  ~PacketList() { clear(); }
  void clear() {
    for (AVPacket *pkt : packets) {
      av_packet_free(&pkt);
    }
    packets.clear();
  }
};

double seconds(int64_t ts, AVRational tb) {
  return ts == AV_NOPTS_VALUE ? NAN : ts * av_q2d(tb);
}

}  // namespace

ClipResult cut_clip(const ClipJob &job, const std::atomic<bool> &cancel) {
  ClipResult result;
  auto fail = [&](std::string message) {
    result.ok = false;
    result.message = std::move(message);
    return result;
  };

  if (!(job.to_sec > job.from_sec)) {
    return fail("empty range");
  }
  const double from_sec = std::max(0.0, job.from_sec);

  SourceFile file;
  if (!file.open(job.source)) {
    return fail("source unreadable: cannot open " + job.source);
  }
  Layout layout;
  std::string error;
  if (!scan(file, layout, error)) {
    return fail(error);
  }

  auto *source = new VirtualSource();
  std::unique_ptr<VirtualSource> source_owner(source);
  source->file = &file;
  source->end = layout.end;
  // A live source: where reading starts, and when its first video sample shows.
  const Fragment *start_fragment = nullptr;
  if (layout.fragmented) {
    source->head.resize((size_t)layout.moov_end);
    if (file.read_at(0, source->head.data(), source->head.size()) != layout.moov_end) {
      return fail("source unreadable");
    }
    start_fragment = &layout.fragments[first_fragment(layout, from_sec)];
    const int64_t start = start_fragment->offset;
    if (start - layout.moov_end >= 16) {
      uint8_t free_box[16];
      put_be32(free_box, 1);
      memcpy(free_box + 4, "free", 4);
      put_be32(free_box + 8, (uint32_t)((uint64_t)(start - layout.moov_end) >> 32));
      put_be32(free_box + 12, (uint32_t)(start - layout.moov_end));
      source->head.insert(source->head.end(), free_box, free_box + 16);
    }
  }

  InputCloser in;
  auto *io_buffer = static_cast<uint8_t *>(av_malloc(kIoBufferSize));
  in.io = avio_alloc_context(io_buffer, kIoBufferSize, 0, source, &VirtualSource::read, nullptr, &VirtualSource::seek);
  if (!in.io) {
    av_free(io_buffer);
    return fail("out of memory");
  }
  in.ctx = avformat_alloc_context();
  in.ctx->pb = in.io;
  in.ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
  AVDictionary *options = nullptr;
  av_dict_set(&options, "use_tfdt", "1", 0);
  int ret = avformat_open_input(&in.ctx, nullptr, av_find_input_format("mov"), &options);
  av_dict_free(&options);
  if (ret < 0) {
    return fail("source unreadable: " + av_error(ret));
  }
  // No avformat_find_stream_info: the moov's sample descriptions are all a stream copy needs, and probing would
  // read seconds of media first.
  const int video = av_find_best_stream(in.ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video < 0) {
    return fail("source has no video");
  }
  const AVRational video_tb = in.ctx->streams[video]->time_base;
  // On fragments, libavformat shifts timestamps by the reordering delay it has seen in negative composition offsets
  // (one frame with B-frames), where a finalised file reads from its full moov does not: a live vod would read late.
  // Measured on the first video packet, the start fragment's first sample, against what its moof says, and taken
  // off every stream, so a live and a finalised vod read the same file time: the one the `anchor` events use.
  bool calibrated = start_fragment == nullptr;
  int64_t video_shift = 0;

  // Output: every video and audio stream, in the source's order.
  const std::filesystem::path out_path(reinterpret_cast<const char8_t *>(job.path.c_str()));
  std::error_code fs_error;
  if (out_path.has_parent_path()) {
    std::filesystem::create_directories(out_path.parent_path(), fs_error);
  }
  const std::string part = job.path + ".part";
  OutputCloser out;
  out.path = widen(part);
  if ((ret = avformat_alloc_output_context2(&out.ctx, nullptr, "mp4", part.c_str())) < 0) {
    return fail("cannot create " + job.path + ": " + av_error(ret));
  }
  std::vector<int> map(in.ctx->nb_streams, -1);
  int mapped = 0;
  for (unsigned i = 0; i < in.ctx->nb_streams; i++) {
    AVStream *in_stream = in.ctx->streams[i];
    const AVMediaType type = in_stream->codecpar->codec_type;
    if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
      in_stream->discard = AVDISCARD_ALL;
      continue;
    }
    AVStream *out_stream = avformat_new_stream(out.ctx, nullptr);
    if (!out_stream || avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar) < 0) {
      return fail("out of memory");
    }
    out_stream->codecpar->codec_tag = 0;
    out_stream->time_base = in_stream->time_base;
    out_stream->disposition = in_stream->disposition;
    av_dict_copy(&out_stream->metadata, in_stream->metadata, 0);
    map[i] = mapped++;
  }
  if ((ret = avio_open(&out.ctx->pb, part.c_str(), AVIO_FLAG_WRITE)) < 0) {
    return fail("cannot create " + job.path + ": " + av_error(ret));
  }
  AVDictionary *mux_options = nullptr;
  // The moov up front: the Clip plays as soon as it starts loading.
  av_dict_set(&mux_options, "movflags", "+faststart", 0);
  ret = avformat_write_header(out.ctx, &mux_options);
  av_dict_free(&mux_options);
  if (ret < 0) {
    return fail("cannot write " + job.path + ": " + av_error(ret));
  }

  // On a finalised source the index is complete and this lands on the keyframe. A live one starts at its start
  // fragment's first sample already (the free box), and must: calibration reads it.
  if (!start_fragment) {
    av_seek_frame(in.ctx, video, (int64_t)(from_sec / av_q2d(video_tb)), AVSEEK_FLAG_BACKWARD);
  }

  // The keyframe the Clip starts on is the last one at or before from_sec, known only once a later one (or the end)
  // is read: packets since the latest candidate wait here.
  PacketList waiting;
  bool started = false;
  bool decided = false;
  int64_t key_dts = AV_NOPTS_VALUE;
  double key_sec = 0;
  double end_sec = 0;
  std::vector<bool> done(in.ctx->nb_streams, false);
  int remaining = mapped;
  int written = 0;

  auto write = [&](AVPacket *pkt) -> int {
    AVStream *in_stream = in.ctx->streams[pkt->stream_index];
    const bool is_video = pkt->stream_index == video;
    const double pts = seconds(pkt->pts, in_stream->time_base);
    const double dts = seconds(pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts, in_stream->time_base);
    // Audio from the keyframe on; no packet past to_sec (decode order for video, so every frame kept decodes).
    if (!is_video && pts < key_sec) {
      return 0;
    }
    if ((is_video ? dts : pts) >= job.to_sec) {
      if (!done[pkt->stream_index]) {
        done[pkt->stream_index] = true;
        remaining--;
      }
      return 0;
    }
    end_sec = std::max(end_sec, pts + seconds(pkt->duration, in_stream->time_base));
    // File time 0 of the Clip is the keyframe's decode time, so no timestamp is negative.
    const int64_t origin = av_rescale_q(key_dts, video_tb, in_stream->time_base);
    if (pkt->pts != AV_NOPTS_VALUE) {
      pkt->pts -= origin;
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
      pkt->dts -= origin;
    }
    AVStream *out_stream = out.ctx->streams[map[pkt->stream_index]];
    av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
    pkt->stream_index = out_stream->index;
    pkt->pos = -1;
    written++;
    return av_interleaved_write_frame(out.ctx, pkt);
  };
  auto decide = [&]() -> int {
    decided = true;
    for (AVPacket *pkt : waiting.packets) {
      if (const int write_ret = write(pkt); write_ret < 0) {
        return write_ret;
      }
    }
    waiting.clear();
    return 0;
  };

  AVPacket *pkt = av_packet_alloc();
  int write_error = 0;
  while (remaining > 0 && !write_error) {
    if (cancel) {
      av_packet_free(&pkt);
      return fail("cancelled");
    }
    ret = av_read_frame(in.ctx, pkt);
    if (ret < 0) {
      // The end of what can be read: of the file, or of the complete fragments of a live one. Before the first
      // keyframe, a read error means the source is broken.
      if (ret != AVERROR_EOF && !started) {
        av_packet_free(&pkt);
        return fail("source unreadable: " + av_error(ret));
      }
      break;
    }
    if (pkt->stream_index < 0 || (unsigned)pkt->stream_index >= map.size() || map[pkt->stream_index] < 0) {
      av_packet_unref(pkt);
      continue;
    }
    if (!calibrated) {
      if (pkt->stream_index != video) {
        // Before the start fragment's first video sample: earlier than any keyframe the Clip can start on.
        av_packet_unref(pkt);
        continue;
      }
      calibrated = true;
      video_shift = pkt->pts - av_rescale_q(start_fragment->video_first_pts,
                                            AVRational{1, (int)layout.video_timescale}, video_tb);
    }
    if (video_shift) {
      const int64_t s = av_rescale_q(video_shift, video_tb, in.ctx->streams[pkt->stream_index]->time_base);
      if (pkt->pts != AV_NOPTS_VALUE) {
        pkt->pts -= s;
      }
      if (pkt->dts != AV_NOPTS_VALUE) {
        pkt->dts -= s;
      }
    }
    if (!decided) {
      if (pkt->stream_index == video) {
        const double pts = seconds(pkt->pts, video_tb);
        const bool key = pkt->flags & AV_PKT_FLAG_KEY;
        if (key && (pts <= from_sec || !started)) {
          // A later candidate: the one before is not the last at or before from_sec. With none before from_sec
          // (from_sec before the first keyframe read), the first one is the start.
          waiting.clear();
          started = true;
          key_dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
          key_sec = pts;
          decided = pts > from_sec;
        } else if (key) {
          write_error = decide();
        } else if (!started) {
          av_packet_unref(pkt);
          continue;
        }
      } else if (!started) {
        av_packet_unref(pkt);
        continue;
      }
      if (!decided && !write_error) {
        // Reaching to_sec before another keyframe decides too.
        const double dts = seconds(pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts,
                                   in.ctx->streams[pkt->stream_index]->time_base);
        if (dts < job.to_sec) {
          waiting.packets.push_back(av_packet_clone(pkt));
          av_packet_unref(pkt);
          continue;
        }
        write_error = decide();
      }
    }
    if (!write_error) {
      write_error = write(pkt);
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  if (!write_error && !decided && started) {
    write_error = decide();
  }
  if (write_error < 0) {
    return fail("cannot write " + job.path + ": " + av_error(write_error));
  }
  if (written == 0) {
    return fail("empty range");
  }
  if ((ret = av_interleaved_write_frame(out.ctx, nullptr)) < 0 || (ret = av_write_trailer(out.ctx)) < 0) {
    return fail("cannot write " + job.path + ": " + av_error(ret));
  }
  if ((ret = avio_closep(&out.ctx->pb)) < 0) {
    return fail("cannot write " + job.path + ": " + av_error(ret));
  }
  if (!MoveFileExW(out.path.c_str(), widen(job.path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
    return fail("cannot write " + job.path + ": rename failed (" + std::to_string(GetLastError()) + ")");
  }
  out.path.clear();

  result.ok = true;
  result.from_sec = key_sec;
  result.duration_ms = std::llround(std::max(0.0, end_sec - key_sec) * 1000);
  return result;
}

Clipper::Clipper(Done done) : done_(std::move(done)) {}

Clipper::~Clipper() {
  stop();
}

void Clipper::push(ClipJob job) {
  {
    std::lock_guard lock(mutex_);
    if (!stopping_) {
      queue_.push_back(std::move(job));
      if (!thread_.joinable()) {
        thread_ = std::thread(&Clipper::run, this);
      }
      cv_.notify_one();
      return;
    }
  }
  ClipResult result;
  result.message = "shutting down";
  done_(job, result);
}

void Clipper::stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
    cancel_ = true;
    cv_.notify_one();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Clipper::run() {
  SetThreadDescription(GetCurrentThread(), L"recorder-host clip");
  // Lowest CPU, I/O and memory priority: the recording outputs always go first.
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
  while (true) {
    ClipJob job;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return !queue_.empty() || stopping_; });
      if (stopping_) {
        break;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    done_(job, cut_clip(job, cancel_));
  }
  // Every job gets its reply, even the ones shutdown leaves uncut.
  std::deque<ClipJob> left;
  {
    std::lock_guard lock(mutex_);
    left.swap(queue_);
  }
  for (const ClipJob &job : left) {
    ClipResult result;
    result.message = "shutting down";
    done_(job, result);
  }
}
