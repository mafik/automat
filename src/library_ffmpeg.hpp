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
// Every stream in the file is a port: "#0", "#1", ... in the file's own
// order, plus the "video" port for the best video stream
// (av_find_best_stream) so a decoder connects without knowing indices.
// Connected decoders pull packets through ReadPacketFor; packets that
// belong to another connected stream wait in that stream's bounded queue,
// whose fill is the port's meter.
struct MediaFile : Object {
  static constexpr int kMaxStreams = 6;
  static constexpr int kQueueCap = 256;  // packets per stream; oldest drop first

  mutable std::mutex mutex;  // guards path and the runtime state below

  Str path;  // recipe data; empty = closed

  // Per-stream ports, named "#0".."#5" at construction; the open file sets
  // how many are active.
  StreamOutSlot stream_ports[kMaxStreams];
  int n_streams = 0;  // active ports; min(file streams, kMaxStreams)

  // Runtime, guarded by `mutex`. `fmt` is an AVFormatContext*; queued
  // packets are AVPacket*. The types are erased so FFmpeg headers stay out
  // of this header.
  void* fmt = nullptr;
  int video_stream = -1;  // index of the stream behind the "video" port
  Str container;          // demuxer long name, FFmpeg's own words
  double duration_s = 0;
  Vec<Str> rows;          // one printed row per stream
  double position_s = 0;  // pts of the last packet handed out
  Vec<void*> queues[kMaxStreams];
  uint64_t packets[kMaxStreams] = {};  // handed-out totals per stream
  uint64_t packet_bytes[kMaxStreams] = {};

  DEF_INTERFACE(MediaFile, StreamArgument, out_stream, "video")
  Str OnFormat() { return obj->PacketFormat(obj->BestVideoStream()); }
  StreamStats OnStats() { return obj->PacketStats(obj->BestVideoStream()); }
  void OnConnect(Interface end) { obj->OnOutStreamConnect(*this, end); }
  DEF_END(out_stream);

  void Interfaces(const std::function<LoopControl(Interface)>& cb) override;

  MediaFile();
  MediaFile(const MediaFile& o);
  ~MediaFile() override;

  StrView Name() const override { return "avformat"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(MediaFile, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  // Closes any open file, then opens and probes `new_path`. An empty path
  // just closes. Errors land on this object.
  void SetPath(StrView new_path);

  // Every out port's OnConnect: a decoder that changes its packet source
  // must reopen its codec, so both ends of the change reset.
  void OnOutStreamConnect(StreamArgument self, Interface end);

  // The stream index feeding `consumer`, or -1. The "video" port counts as
  // the best video stream; a consumer on several ports gets the first.
  int StreamIndexFeeding(const Object* consumer);
  int BestVideoStream();

  // Reads the next packet of stream `index` into `av_packet` (AVPacket*):
  // from the stream's queue when one waits, else forward through the file,
  // parking packets that belong to other connected streams. Returns 0 or a
  // libav error (AVERROR_EOF at the end). Called by connected decoders on
  // worker threads.
  int ReadPacketFor(int index, void* av_packet);
  // Copies stream `index`'s codec parameters into `av_params`
  // (AVCodecParameters*). False when no file or no such stream.
  bool CopyCodecParams(int index, void* av_params);
  // Stream `index`'s time base, in seconds per pts unit.
  double TimeBase(int index);

  Str PacketFormat(int index);
  StreamStats PacketStats(int index);

  // The port slot owning `table`, or -1 for the "video" port's table.
  int SlotOf(const Interface::Table* table) const;

 private:
  bool StreamConsumedLocked(int index);
  void InitPorts();
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
