// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_ffmpeg.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "format.hpp"
#include "text_field.hpp"
#include "ui_beta.hpp"
#include "units.hpp"

namespace automat::library {

using ui::beta::Hash2;

static Str AvErrWord(int err) {
  if (err == 0) return "OK";
  if (err == AVERROR(EAGAIN)) return "EAGAIN";
  if (err == AVERROR_EOF) return "EOF";
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(err, buf, sizeof(buf));
  return buf;
}

// ============================================================================
// MediaFile
// ============================================================================

void MediaFile::InitPorts() {
  for (int i = 0; i < kMaxStreams; ++i) {
    auto& p = stream_ports[i];
    p.Init(f("#{}", i), int(offsetof(MediaFile, stream_ports) + i * sizeof(StreamOutSlot) +
                            offsetof(StreamOutSlot, state)));
    p.table.on_connect = [](Argument self, Interface end) {
      static_cast<MediaFile*>(self.object_ptr)->OnOutStreamConnect(cast<StreamArgument>(self), end);
    };
    p.table.format = [](StreamArgument self) {
      auto* file = static_cast<MediaFile*>(self.object_ptr);
      return file->PacketFormat(file->SlotOf(self.table_ptr));
    };
    p.table.stats = [](StreamArgument self) {
      auto* file = static_cast<MediaFile*>(self.object_ptr);
      return file->PacketStats(file->SlotOf(self.table_ptr));
    };
  }
}

int MediaFile::SlotOf(const Interface::Table* table) const {
  for (int i = 0; i < kMaxStreams; ++i) {
    if (table == &stream_ports[i].table) return i;
  }
  return -1;
}

MediaFile::MediaFile() { InitPorts(); }

MediaFile::MediaFile(const MediaFile& o) : Object(o), out_stream(o.out_stream) {
  InitPorts();
  // Reopen rather than copy the handle: the clone probes its own streams
  // and activates its own ports.
  if (!o.path.empty()) SetPath(o.path);
}

MediaFile::~MediaFile() {
  auto lock = std::lock_guard(mutex);
  CloseLocked();
}

void MediaFile::Interfaces(const std::function<LoopControl(Interface)>& cb) {
  if (cb(out_stream.Bind()) == LoopControl::Break) return;
  for (int i = 0; i < n_streams; ++i) {
    if (cb(Interface(*this, stream_ports[i].table)) == LoopControl::Break) return;
  }
}

void MediaFile::CloseLocked() {
  if (fmt) {
    auto* ctx = (AVFormatContext*)fmt;
    avformat_close_input(&ctx);
    fmt = nullptr;
  }
  video_stream = -1;
  n_streams = 0;
  container.clear();
  duration_s = 0;
  rows.clear();
  position_s = 0;
  for (int i = 0; i < kMaxStreams; ++i) {
    for (void* pkt : queues[i]) {
      auto* p = (AVPacket*)pkt;
      av_packet_free(&p);
    }
    queues[i].clear();
    packets[i] = 0;
    packet_bytes[i] = 0;
  }
}

// One printed row per stream, in FFmpeg's own words: codec, resolution or
// sample rate, bit rate.
static Str StreamRow(AVFormatContext* fmt, int i) {
  AVStream* stream = fmt->streams[i];
  AVCodecParameters* par = stream->codecpar;
  Str row = f("#{} {}", i, avcodec_get_name(par->codec_id));
  if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
    row += f(" {}x{}", par->width, par->height);
    if (double fps = av_q2d(stream->avg_frame_rate); fps > 0) row += f(" {:.3g} fps", fps);
  } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
    row += f(" {} Hz {}ch", par->sample_rate, par->ch_layout.nb_channels);
  }
  if (par->bit_rate > 0) row += f(" {} kb/s", par->bit_rate / 1000);
  return row;
}

void MediaFile::SetPath(StrView new_path) {
  Str error;
  {
    auto lock = std::lock_guard(mutex);
    CloseLocked();
    path = new_path;
    if (!path.empty()) {
      AVFormatContext* ctx = nullptr;
      int err = avformat_open_input(&ctx, path.c_str(), nullptr, nullptr);
      if (err == 0) err = avformat_find_stream_info(ctx, nullptr);
      if (err < 0) {
        error = f("{}: {}", path, AvErrWord(err));
        if (ctx) avformat_close_input(&ctx);
      } else {
        fmt = ctx;
        container = ctx->iformat->long_name ? ctx->iformat->long_name : ctx->iformat->name;
        if (ctx->duration > 0) duration_s = (double)ctx->duration / AV_TIME_BASE;
        for (int i = 0; i < (int)ctx->nb_streams; ++i) rows.push_back(StreamRow(ctx, i));
        video_stream = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        n_streams = std::min<int>((int)ctx->nb_streams, kMaxStreams);
      }
    }
  }
  if (!error.empty()) {
    ReportError(error);
  } else {
    ClearOwnError();
  }
  WakeToys();
}

void MediaFile::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  if (path.empty()) return;
  writer.Key("path");
  writer.String(path.data(), path.size());
}

bool MediaFile::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key != "path") return false;
  Status status;
  Str new_path;
  d.Get(new_path, status);
  if (OK(status)) SetPath(new_path);
  return true;
}

int MediaFile::BestVideoStream() {
  auto lock = std::lock_guard(mutex);
  return video_stream;
}

int MediaFile::StreamIndexFeeding(const Object* consumer) {
  auto video_target = out_stream.target.Lock();
  if (video_target.Owner<Object>() == consumer) {
    auto lock = std::lock_guard(mutex);
    return video_stream;
  }
  for (int i = 0; i < kMaxStreams; ++i) {
    auto target = stream_ports[i].state.target.Lock();
    if (target.Owner<Object>() == consumer) return i;
  }
  return -1;
}

// Whether any port hands out stream `index`'s packets; such packets park in
// the stream's queue instead of being dropped. Callers hold `mutex`.
bool MediaFile::StreamConsumedLocked(int index) {
  if (index == video_stream && !out_stream.target.IsExpired()) return true;
  if (index >= 0 && index < kMaxStreams && !stream_ports[index].state.target.IsExpired())
    return true;
  return false;
}

int MediaFile::ReadPacketFor(int index, void* av_packet) {
  auto lock = std::lock_guard(mutex);
  if (!fmt || index < 0) return AVERROR_EOF;
  auto* ctx = (AVFormatContext*)fmt;
  auto* pkt = (AVPacket*)av_packet;
  auto hand_out = [&](AVPacket* out) {
    if (index < kMaxStreams) {
      packets[index] += 1;
      packet_bytes[index] += out->size;
    }
    if (out->pts != AV_NOPTS_VALUE && index < (int)ctx->nb_streams) {
      position_s = out->pts * av_q2d(ctx->streams[index]->time_base);
    }
    WakeToys();
  };
  if (index < kMaxStreams && !queues[index].empty()) {
    auto* queued = (AVPacket*)queues[index][0];
    queues[index].erase(queues[index].begin());
    av_packet_move_ref(pkt, queued);
    av_packet_free(&queued);
    hand_out(pkt);
    return 0;
  }
  for (;;) {
    int err = av_read_frame(ctx, pkt);
    if (err < 0) return err;
    if (pkt->stream_index == index) {
      hand_out(pkt);
      return 0;
    }
    int other = pkt->stream_index;
    if (other < kMaxStreams && StreamConsumedLocked(other)) {
      auto& queue = queues[other];
      if ((int)queue.size() >= kQueueCap) {
        auto* oldest = (AVPacket*)queue[0];
        queue.erase(queue.begin());
        av_packet_free(&oldest);
      }
      AVPacket* parked = av_packet_alloc();
      av_packet_move_ref(parked, pkt);
      queue.push_back(parked);
    } else {
      av_packet_unref(pkt);
    }
  }
}

bool MediaFile::CopyCodecParams(int index, void* av_params) {
  auto lock = std::lock_guard(mutex);
  if (!fmt || index < 0) return false;
  auto* ctx = (AVFormatContext*)fmt;
  if (index >= (int)ctx->nb_streams) return false;
  return avcodec_parameters_copy((AVCodecParameters*)av_params, ctx->streams[index]->codecpar) >= 0;
}

double MediaFile::TimeBase(int index) {
  auto lock = std::lock_guard(mutex);
  if (!fmt || index < 0) return 0;
  auto* ctx = (AVFormatContext*)fmt;
  if (index >= (int)ctx->nb_streams) return 0;
  return av_q2d(ctx->streams[index]->time_base);
}

Str MediaFile::PacketFormat(int index) {
  auto lock = std::lock_guard(mutex);
  if (!fmt || index < 0) return "";
  auto* ctx = (AVFormatContext*)fmt;
  if (index >= (int)ctx->nb_streams) return "";
  AVStream* stream = ctx->streams[index];
  AVCodecParameters* par = stream->codecpar;
  // The packet stream in FFmpeg's words: codec, size or rate, and the time
  // base the timestamps live in.
  Str format = avcodec_get_name(par->codec_id);
  if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
    format += f(" {}x{}", par->width, par->height);
  } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
    format += f(" {} Hz {}ch", par->sample_rate, par->ch_layout.nb_channels);
  }
  format += f(" tb {}/{}", stream->time_base.num, stream->time_base.den);
  return format;
}

StreamStats MediaFile::PacketStats(int index) {
  auto lock = std::lock_guard(mutex);
  if (index < 0 || index >= kMaxStreams) return {};
  return {.bytes = packet_bytes[index],
          .units = packets[index],
          .fill = queues[index].size(),
          .capacity = kQueueCap,
          .fill_unit = "packets"};
}

void MediaFile::OnOutStreamConnect(StreamArgument self, Interface end) {
  Ptr<FfmpegDecoder> old_decoder;
  if (auto old = self.state->target.Lock()) {
    if (auto* o = dynamic_cast<FfmpegDecoder*>(old.Owner<Object>())) old_decoder = o->AcquirePtr();
  }
  StreamArgument::Table::StreamOnConnect(self, end);
  if (old_decoder) old_decoder->ResetCodec();
  if (auto* new_decoder = dynamic_cast<FfmpegDecoder*>(end.object_ptr)) {
    new_decoder->ResetCodec();
  }
}

// ============================================================================
// FfmpegDecoder
// ============================================================================

FfmpegDecoder::~FfmpegDecoder() {
  auto lock = std::lock_guard(mutex);
  if (sws) sws_freeContext((SwsContext*)sws);
  if (frame) av_frame_free((AVFrame**)&frame);
  if (packet) av_packet_free((AVPacket**)&packet);
  if (ctx) avcodec_free_context((AVCodecContext**)&ctx);
}

void FfmpegDecoder::ResetCodec() {
  auto lock = std::lock_guard(mutex);
  if (ctx) avcodec_free_context((AVCodecContext**)&ctx);
  eof_sent = false;
  codec_name.clear();
  ret_word.clear();
  time_base = 0;
  position_s = 0;
}

sk_sp<SkImage> FfmpegDecoder::Held() const {
  auto lock = std::lock_guard(mutex);
  return held;
}

Str FfmpegDecoder::FrameFormat() {
  auto lock = std::lock_guard(mutex);
  if (!ctx) return "";
  auto* c = (AVCodecContext*)ctx;
  if (c->codec_type == AVMEDIA_TYPE_AUDIO) {
    if (c->sample_fmt == AV_SAMPLE_FMT_NONE) return "";
    const char* name = av_get_sample_fmt_name(c->sample_fmt);
    return f("{} {} Hz {}ch", name ? name : "?", c->sample_rate, c->ch_layout.nb_channels);
  }
  if (c->pix_fmt == AV_PIX_FMT_NONE) return "";
  const char* name = av_get_pix_fmt_name(c->pix_fmt);
  return f("{} {}x{}", name ? name : "?", c->width, c->height);
}

StreamStats FfmpegDecoder::FrameStats() {
  auto lock = std::lock_guard(mutex);
  return {.bytes = frame_bytes, .units = frames};
}

static sk_sp<SkImage> FrameToImage(void*& sws, AVFrame* frame) {
  int width = frame->width;
  int height = frame->height;
  sws = sws_getCachedContext((SwsContext*)sws, width, height, (AVPixelFormat)frame->format, width,
                             height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws) return nullptr;
  sk_sp<SkData> data = SkData::MakeUninitialized((size_t)width * height * 4);
  uint8_t* dst[4] = {(uint8_t*)data->writable_data()};
  int dst_stride[4] = {width * 4};
  sws_scale((SwsContext*)sws, frame->data, frame->linesize, 0, height, dst, dst_stride);
  auto info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  return SkImages::RasterFromData(info, std::move(data), (size_t)width * 4);
}

void FfmpegDecoder::Step() {
  Ptr<Object> producer = in_stream->Producer();
  auto* file = dynamic_cast<MediaFile*>(producer.get());
  Str error;
  {
    auto lock = std::lock_guard(mutex);
    if (!file) {
      ret_word = "no packets connected";
      WakeToys();
      return;
    }
    if (!frame) frame = av_frame_alloc();
    if (!packet) packet = av_packet_alloc();
    int stream_index = file->StreamIndexFeeding(this);
    if (!ctx) {
      AVCodecParameters* par = avcodec_parameters_alloc();
      const AVCodec* codec = nullptr;
      AVCodecContext* c = nullptr;
      int err = 0;
      if (!file->CopyCodecParams(stream_index, par)) {
        error = "avcodec: the connected port has no stream";
      } else if (!(codec = avcodec_find_decoder(par->codec_id))) {
        error = f("avcodec: no decoder for {}", avcodec_get_name(par->codec_id));
      } else if (!(c = avcodec_alloc_context3(codec))) {
        error = "avcodec: could not allocate the codec context";
      } else if ((err = avcodec_parameters_to_context(c, par)) < 0 ||
                 (err = avcodec_open2(c, codec, nullptr)) < 0) {
        error = f("avcodec: {}", AvErrWord(err));
        avcodec_free_context(&c);
      } else {
        ctx = c;
        codec_name = codec->name;
        eof_sent = false;
        time_base = file->TimeBase(stream_index);
      }
      avcodec_parameters_free(&par);
      if (!error.empty()) {
        ret_word = "error";
      }
    }
    if (error.empty()) {
      // One receive; when the codec wants data, one packet and one more
      // receive. The word on the face is exactly what the library returned.
      auto* c = (AVCodecContext*)ctx;
      auto* fr = (AVFrame*)frame;
      auto* pkt = (AVPacket*)packet;
      int ret = avcodec_receive_frame(c, fr);
      if (ret == AVERROR(EAGAIN)) {
        int read = file->ReadPacketFor(stream_index, pkt);
        if (read == 0) {
          avcodec_send_packet(c, pkt);
          av_packet_unref(pkt);
        } else if (read == AVERROR_EOF && !eof_sent) {
          avcodec_send_packet(c, nullptr);
          eof_sent = true;
        } else if (read != AVERROR_EOF) {
          error = f("avformat: {}", AvErrWord(read));
        }
        if (error.empty()) ret = avcodec_receive_frame(c, fr);
      }
      if (error.empty()) {
        ret_word = AvErrWord(ret);
        if (ret == 0) {
          if (fr->width > 0) {
            if (auto image = FrameToImage(sws, fr)) {
              held = std::move(image);
              frame_bytes += (uint64_t)fr->width * fr->height * 4;
            }
          } else if (fr->nb_samples > 0) {
            frame_bytes += (uint64_t)fr->nb_samples * fr->ch_layout.nb_channels *
                           av_get_bytes_per_sample((AVSampleFormat)fr->format);
          }
          frames += 1;
          if (fr->pts != AV_NOPTS_VALUE && time_base > 0) position_s = fr->pts * time_base;
          av_frame_unref(fr);
        }
      }
    }
  }
  if (!error.empty()) ReportError(error);
  WakeToys();
}

// ============================================================================
// MediaFile toy
// ============================================================================

namespace {

constexpr float kPlateW = 7_cm;
constexpr float kBand = ui::beta::kTitleSize + 2 * ui::beta::kPadS + 0.45_mm;
constexpr float kCreditRow = 2.0_mm;
constexpr float kSide = 2.0_mm;
constexpr float kPathRow = 6.0_mm;
constexpr float kInfoRow = 3.0_mm;
constexpr float kRowCount = 5;  // container line plus up to four stream rows
constexpr float kStatusRow = 5.0_mm;
constexpr float kBottomPad = 1.5_mm;
constexpr float kFilePlateH =
    kBand + kCreditRow + kPathRow + 1_mm + kInfoRow * (kRowCount + 1) + kStatusRow + kBottomPad;

constexpr uint32_t kFileSeed = 0xF4A;
constexpr uint32_t kDecoderSeed = 0xF4B;

}  // namespace

struct MediaFileToy;

// A plain one-line path editor writing straight into the toy's edit buffer;
// the toy opens the file once the path names one.
struct PathField : ui::TextField {
  PathField(ui::Widget* parent, std::string* text, float width)
      : ui::TextField(parent, text, width) {}
  StrView Name() const override { return "PathField"; }
};

struct MediaFileToy : ui::beta::ObjectToy {
  std::unique_ptr<PathField> field;
  std::string path_edit_;

  // Tick-cached object state (UI thread only):
  Str path_applied_;
  Str container_;
  double duration_s_ = 0;
  Vec<Str> rows_;
  double position_s_ = 0;
  bool open_ = false;
  int n_streams_ = 0;
  int video_stream_ = -1;
  Vec<const Interface::Table*> port_tables_;  // anchor identities; never dereferenced
  Vec<bool> port_connected_;

  MediaFileToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    if (auto file = LockObject<MediaFile>()) {
      {
        auto lock = std::lock_guard(file->mutex);
        path_edit_ = file->path;
        path_applied_ = file->path;
      }
      for (int i = 0; i < MediaFile::kMaxStreams; ++i) {
        port_tables_.push_back(&file->stream_ports[i].table);
      }
      port_connected_.resize(MediaFile::kMaxStreams);
    }
    field = std::make_unique<PathField>(this, &path_edit_, kPlateW - 2 * kSide);
    float field_bottom = kFilePlateH / 2 - (kBand + kCreditRow + kPathRow);
    field->local_to_parent =
        SkM44::Translate(-kPlateW / 2 + kSide, field_bottom) * SkM44::Scale(0.55f, 0.55f, 1);
    UpdateFromObject();
  }

  float PortX(int index) const { return -kPlateW / 2 + 19_mm + index * 8_mm; }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kPlateW, kFilePlateH), 3_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }
  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&decltype(MediaFile::out_stream)::tbl)) {
      return Vec2AndDir{.pos = Vec2(-kPlateW / 2 + 10_mm, -kFilePlateH / 2), .dir = -90_deg};
    }
    for (int i = 0; i < (int)port_tables_.size(); ++i) {
      if (&arg == port_tables_[i]) {
        return Vec2AndDir{.pos = Vec2(PortX(i), -kFilePlateH / 2), .dir = -90_deg};
      }
    }
    return ObjectToy::ArgStart(arg);
  }

  void UpdateFromObject() {
    if (auto file = LockObject<MediaFile>()) {
      // The file opens the moment its path names a readable file.
      if (Str(path_edit_) != path_applied_) {
#if defined(_WIN32)
        bool exists = !path_edit_.empty() && _access(path_edit_.c_str(), 4) == 0;
#else
        bool exists = !path_edit_.empty() && access(path_edit_.c_str(), R_OK) == 0;
#endif
        if (exists || path_edit_.empty()) {
          path_applied_ = path_edit_;
          file->SetPath(path_applied_);
        }
      }
      for (int i = 0; i < MediaFile::kMaxStreams; ++i) {
        port_connected_[i] = !file->stream_ports[i].state.target.IsExpired();
      }
      auto lock = std::lock_guard(file->mutex);
      open_ = file->fmt != nullptr;
      container_ = file->container;
      duration_s_ = file->duration_s;
      rows_ = file->rows;
      position_s_ = file->position_s;
      n_streams_ = file->n_streams;
      video_stream_ = file->video_stream;
    }
  }

  Tock Tick(time::Timer&) override {
    UpdateFromObject();
    return Tock::Draw;
  }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, kFilePlateH), "avformat",
                    ui::beta::kOrange, ui::beta::State::Default, Seed(kFileSeed), true);

    {  // credit
      StrView credit = "FFmpeg · libavformat";
      float w = ui::beta::TextWidth(credit, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit,
                         {kPlateW / 2 - kSide - w, kFilePlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kFileSeed));
    }

    {  // caption over the path field
      float cap_y = kFilePlateH / 2 - kBand - kCreditRow - 1.4_mm;
      ui::beta::DrawText(canvas, "path", {-kPlateW / 2 + kSide + 0.6_mm, cap_y},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kFileSeed));
    }

    {  // container, duration, and the stream rows
      float y = kFilePlateH / 2 - (kBand + kCreditRow + kPathRow + 1_mm) - kInfoRow;
      if (open_) {
        Str head = container_;
        if (duration_s_ > 0) head += f("  ·  {:.2f} s", duration_s_);
        ui::beta::DrawText(canvas, head, {-kPlateW / 2 + kSide, y}, ui::beta::kMicroSize,
                           ui::beta::kInk, false, Seed(kFileSeed));
        y -= kInfoRow;
        for (int i = 0; i < (int)rows_.size() && i < (int)kRowCount - 1; ++i) {
          ui::beta::DrawText(canvas, rows_[i], {-kPlateW / 2 + kSide + 1.5_mm, y},
                             ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kFileSeed));
          y -= kInfoRow;
        }
      } else {
        StrView hint =
            path_applied_.empty() ? StrView("type a media file path") : StrView("no such file");
        ui::beta::DrawText(canvas, hint, {-kPlateW / 2 + kSide, y}, ui::beta::kMicroSize,
                           ui::beta::kGray, false, Seed(kFileSeed));
      }
    }

    {  // position readout at the lower left
      float row_mid = -kFilePlateH / 2 + kBottomPad + kStatusRow * 0.5f;
      if (open_) {
        Str pos = f("at {:.2f} s", position_s_);
        ui::beta::DrawText(canvas, pos, {-kPlateW / 2 + kSide, row_mid - 0.7_mm},
                           ui::beta::kMicroSize + 0.3_mm, ui::beta::kInk, false, Seed(kFileSeed));
      }
    }

    {  // stream ports along the bottom edge: "video" plus one per stream
      ui::beta::DrawText(canvas, "video", {-kPlateW / 2 + 6.4_mm, -kFilePlateH / 2 + 0.8_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kFileSeed));
      for (int i = 0; i < n_streams_; ++i) {
        Vec2 c{PortX(i), -kFilePlateH / 2 + 1.4_mm};
        uint32_t ps = Seed(Hash2(kFileSeed, 0x90 + (uint32_t)i));
        ui::beta::Port(canvas, c, 1.4_mm, true, ui::beta::kBlue, port_connected_[i],
                       port_connected_[i] ? ui::beta::State::Default : ui::beta::State::Disabled,
                       ps);
        Str label = f("#{}", i);
        float w = ui::beta::TextWidth(label, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, label, {c.x - w / 2, c.y + 2.2_mm}, ui::beta::kMicroSize,
                           ui::beta::kInkSoft, false, ps);
      }
    }
    BakeChildren(canvas);
  }
};

std::unique_ptr<ObjectToy> MediaFile::MakeToy(ui::Widget* parent) {
  return std::make_unique<MediaFileToy>(parent, *this);
}

// ============================================================================
// Decoder toy
// ============================================================================

namespace {

constexpr float kFrameW = kPlateW - 2 * kSide;
constexpr float kFrameH = kFrameW * 3 / 4;
constexpr float kDecoderPlateH = kBand + kCreditRow + kFrameH + 1_mm + kStatusRow + kBottomPad;

}  // namespace

struct FfmpegDecoderToy : ui::beta::ObjectToy {
  std::unique_ptr<ui::beta::RunButton> button;

  // Tick-cached object state (UI thread only):
  Str codec_name_;
  Str ret_word_;
  Str format_;
  double position_s_ = 0;
  uint64_t frames_ = 0;
  sk_sp<SkImage> held_;

  FfmpegDecoderToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    button = std::make_unique<ui::beta::RunButton>(this, [this] { OnButton(); }, Seed(0x5D));
    button->running = false;
    button->enabled = true;
    UpdateFromObject();
  }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kPlateW, kDecoderPlateH), 3_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }
  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&decltype(FfmpegDecoder::out_stream)::tbl)) {
      return Vec2AndDir{.pos = Vec2(-kPlateW / 2 + 10_mm, -kDecoderPlateH / 2), .dir = -90_deg};
    }
    return ui::beta::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  void UpdateFromObject() {
    if (auto decoder = LockObject<FfmpegDecoder>()) {
      format_ = decoder->FrameFormat();
      auto lock = std::lock_guard(decoder->mutex);
      codec_name_ = decoder->codec_name;
      ret_word_ = decoder->ret_word;
      position_s_ = decoder->position_s;
      frames_ = decoder->frames;
      held_ = decoder->held;
    }
  }

  Tock Tick(time::Timer&) override {
    UpdateFromObject();
    return Tock::Draw;
  }

  void OnButton() {
    if (auto decoder = LockObject<FfmpegDecoder>()) decoder->run->ScheduleRun();
    WakeAnimation();
  }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, kDecoderPlateH), "avcodec",
                    ui::beta::kOrange, ui::beta::State::Default, Seed(kDecoderSeed), true);

    {  // credit; the resolved codec joins it in the library's own word
      Str credit = "FFmpeg · libavcodec";
      if (!codec_name_.empty()) credit = f("FFmpeg · libavcodec · {}", codec_name_);
      float w = ui::beta::TextWidth(credit, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit,
                         {kPlateW / 2 - kSide - w, kDecoderPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kDecoderSeed));
    }

    // The held frame: the decoder's current data.
    Rect frame_rect =
        Rect::MakeCornerZero(kFrameW, kFrameH)
            .MoveBy({-kFrameW / 2, kDecoderPlateH / 2 - kBand - kCreditRow - kFrameH});
    {
      SkPaint bg;
      bg.setColor(ui::beta::kInk);
      canvas.drawRect(frame_rect.sk, bg);
      if (held_) {
        canvas.save();
        SkRect src = SkRect::Make(held_->dimensions());
        SkMatrix m = SkMatrix::RectToRect(src, frame_rect.sk, SkMatrix::kCenter_ScaleToFit);
        m.preTranslate(0, held_->height() / 2.f);
        m.preScale(1, -1);
        m.preTranslate(0, -held_->height() / 2.f);
        canvas.concat(m);
        canvas.drawImage(held_, 0, 0, SkSamplingOptions(SkFilterMode::kLinear), nullptr);
        canvas.restore();
      } else if (!format_.empty()) {
        // An audio decoder holds no image; its data face is the decoded
        // format in FFmpeg's own words.
        float w = ui::beta::TextWidth(format_, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, format_, {-w / 2, frame_rect.CenterY()}, ui::beta::kMicroSize,
                           ui::beta::kPaper, false, Seed(kDecoderSeed));
      } else {
        StrView hint = "Run decodes one frame";
        float w = ui::beta::TextWidth(hint, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, hint, {-w / 2, frame_rect.CenterY()}, ui::beta::kMicroSize,
                           ui::beta::kGray, false, Seed(kDecoderSeed));
      }
      SkPaint frame_stroke;
      frame_stroke.setStyle(SkPaint::kStroke_Style);
      frame_stroke.setStrokeWidth(ui::beta::kStroke);
      frame_stroke.setColor(ui::beta::kInk);
      canvas.drawRect(frame_rect.sk, frame_stroke);
    }

    {  // Status row: the literal return chip, position, and frame count.
      float row_mid = -kDecoderPlateH / 2 + kBottomPad + kStatusRow * 0.5f;
      if (!ret_word_.empty()) {
        SkColor color = ret_word_ == "OK"       ? ui::beta::kGreen
                        : ret_word_ == "EAGAIN" ? ui::beta::kGold
                        : ret_word_ == "EOF"    ? ui::beta::kGray
                                                : ui::beta::kRed;
        float w = ui::beta::TextWidth(ret_word_, ui::beta::kMicroSize + 0.3_mm) + 2.6_mm;
        float chip_left = -kPlateW / 2 + kSide;
        float chip_bottom = row_mid - 1.6_mm;
        Rect chip{chip_left, chip_bottom, chip_left + w, chip_bottom + 3.2_mm};
        uint32_t cs = Seed(Hash2(kDecoderSeed, 0xC3));
        SkPath path = ui::beta::WonkyRoundRect(chip, 1.2_mm, ui::beta::kWonk * 0.8f, cs);
        ui::beta::HandShadow(canvas, path, {0.3_mm, -0.3_mm}, ui::beta::kShadow, cs);
        ui::beta::MisregFill(canvas, path, color, cs);
        ui::beta::SketchyStroke(canvas, path, ui::beta::kInk, ui::beta::kStroke * 0.8f, cs, 1);
        ui::beta::DrawTextIn(canvas, ret_word_, chip, ui::beta::kMicroSize + 0.3_mm,
                             ui::beta::TextOn(color), ui::beta::TextAlign::Center, false, cs);
        if (frames_ > 0) {
          Str info = f("{} frames · at {:.2f} s", frames_, position_s_);
          ui::beta::DrawText(canvas, info, {chip.right + 2_mm, row_mid - 0.7_mm},
                             ui::beta::kMicroSize + 0.3_mm, ui::beta::kInk, false,
                             Seed(kDecoderSeed));
        }
      }
    }

    {  // stream port labels
      ui::beta::DrawText(canvas, "frames", {-kPlateW / 2 + 5.8_mm, -kDecoderPlateH / 2 + 0.8_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kDecoderSeed));
      StrView in_label = "packets";
      float in_w = ui::beta::TextWidth(in_label, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, in_label, {-in_w / 2, kDecoderPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kDecoderSeed));
    }
    BakeChildren(canvas);
  }
};

std::unique_ptr<ObjectToy> FfmpegDecoder::MakeToy(ui::Widget* parent) {
  return std::make_unique<FfmpegDecoderToy>(parent, *this);
}

}  // namespace automat::library
