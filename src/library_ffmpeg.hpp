#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkImage.h>

#include <mutex>

#include "base.hpp"
#include "image_provider.hpp"
#include "status.hpp"
#include "str.hpp"
#include "stream.hpp"
#include "vec.hpp"

namespace automat::library {

// The FFmpeg blocks are Automat-driven: they do nothing until called, per
// docs/parrots/Pipeline Language.md. The media file opens the moment its
// path is set and prints one row per stream; its packet port produces the
// best video stream's packets. The decoder performs one send/receive round
// per Run and prints the library's literal return word.

// libavformat demuxer. Opens `path` immediately (avformat_open_input,
// avformat_find_stream_info); shows container, duration, and stream rows.
// The "video" port hands out the best video stream's packets to a connected
// decoder, which pulls them through ReadVideoPacket.
struct MediaFile : Object {
  mutable std::mutex mutex;  // guards path and the runtime state below

  Str path;  // recipe data; empty = closed

  // Runtime, guarded by `mutex`. `fmt` is an AVFormatContext*; the type is
  // erased so FFmpeg headers stay out of this header.
  void* fmt = nullptr;
  int video_stream = -1;  // index of the stream behind the "video" port
  Str container;          // demuxer long name, FFmpeg's own words
  double duration_s = 0;
  Vec<Str> rows;          // one printed row per stream
  double position_s = 0;  // pts of the last packet handed out
  uint64_t packets = 0;
  uint64_t packet_bytes = 0;

  DEF_INTERFACE(MediaFile, StreamArgument, out_stream, "video")
  Str OnFormat() { return obj->PacketFormat(); }
  StreamStats OnStats() { return obj->PacketStats(); }
  void OnConnect(Interface end) { obj->OnOutStreamConnect(*this, end); }
  DEF_END(out_stream);

  INTERFACES(out_stream);

  MediaFile() = default;
  MediaFile(const MediaFile& o) : Object(o), path(o.path), out_stream(o.out_stream) {}
  ~MediaFile() override;

  StrView Name() const override { return "avformat"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(MediaFile, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  // Closes any open file, then opens and probes `new_path`. An empty path
  // just closes. Errors land on this object.
  void SetPath(StrView new_path);

  // out_stream's OnConnect: a decoder that changes its packet source must
  // reopen its codec, so both ends of the change reset.
  void OnOutStreamConnect(StreamArgument self, Interface end);

  // Reads the next packet of the video stream into `av_packet` (AVPacket*),
  // skipping other streams. Returns 0 or a libav error (AVERROR_EOF at the
  // end). Called by the connected decoder on a worker thread.
  int ReadVideoPacket(void* av_packet);
  // Copies the video stream's codec parameters into `av_params`
  // (AVCodecParameters*). False when no file or no video stream is open.
  bool CopyVideoCodecParams(void* av_params);
  // The video stream's time base, in seconds per pts unit.
  double VideoTimeBase();

  Str PacketFormat();
  StreamStats PacketStats();

 private:
  void CloseLocked();
};

// libavcodec decoder. Each Run performs one receive, feeding the codec one
// packet from the connected media file when it asks (EAGAIN); the face
// prints the literal return word (OK, EAGAIN, EOF) because EAGAIN is the
// library's own word for "feed me first". The decoded frame is held as an
// image for the rest of Automat (ImageProvider).
struct FfmpegDecoder : Object {
  mutable std::mutex mutex;  // guards the runtime state below

  // Runtime, guarded by `mutex`. Erased libav types: AVCodecContext*,
  // SwsContext*, AVFrame*, AVPacket*.
  void* ctx = nullptr;
  void* sws = nullptr;
  void* frame = nullptr;
  void* packet = nullptr;
  bool eof_sent = false;
  Str codec_name;  // resolved when the codec opens ("h264")
  Str ret_word;    // the last return, literally: OK / EAGAIN / EOF / an error
  double time_base = 0;
  double position_s = 0;
  sk_sp<SkImage> held;  // the last decoded frame
  uint64_t frames = 0;
  uint64_t frame_bytes = 0;

  DEF_INTERFACE(FfmpegDecoder, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->Step(); }
  DEF_END(run);

  DEF_INTERFACE(FfmpegDecoder, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(FfmpegDecoder, StreamInput, in_stream, "packets")
  DEF_END(in_stream);

  DEF_INTERFACE(FfmpegDecoder, StreamArgument, out_stream, "frames")
  Str OnFormat() { return obj->FrameFormat(); }
  StreamStats OnStats() { return obj->FrameStats(); }
  DEF_END(out_stream);

  DEF_INTERFACE(FfmpegDecoder, ImageProvider, image_provider, "Image")
  sk_sp<SkImage> GetImage() { return obj->Held(); }
  DEF_END(image_provider);

  INTERFACES(run, next, in_stream, out_stream, image_provider);

  FfmpegDecoder() = default;
  FfmpegDecoder(const FfmpegDecoder& o)
      : Object(o), run(o.run), next(o.next), out_stream(o.out_stream) {}
  ~FfmpegDecoder() override;

  StrView Name() const override { return "avcodec"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(FfmpegDecoder, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  // One send/receive round against the codec.
  void Step();
  // Drops the open codec so the next Step reopens it from the connected
  // stream. Called when the packet input reconnects.
  void ResetCodec();

  sk_sp<SkImage> Held() const;
  Str FrameFormat();
  StreamStats FrameStats();
};

}  // namespace automat::library
