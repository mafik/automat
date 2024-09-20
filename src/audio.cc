#include "audio.hh"

#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <cassert>
#include <thread>

#include "span.hh"
#include "str.hh"

#pragma maf add link argument "-latomic"

using namespace maf;
using namespace std;

namespace automat::audio {

jthread pw_loop_thread;

constexpr int kDefaultRate = 48'000;
constexpr int kDefaultChannels = 2;

struct Frame {
  I16 l, r;
};

Vec<Span<Frame>> playing;

atomic<Span<Frame>*> to_play;

struct data {
  struct pw_main_loop* loop;
  struct pw_stream* stream;
};

static void on_process(void* userdata) {
  struct data* data = (struct data*)userdata;
  struct pw_buffer* b;

  if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
    pw_log_warn("out of buffers: %m");
    return;
  }

  auto& buf = b->buffer->datas[0];

  int n_frames = buf.maxsize / sizeof(Frame);
  if (b->requested) n_frames = SPA_MIN(b->requested, n_frames);

  Span<Frame> dst = Span<Frame>((Frame*)buf.data, n_frames);

  dst.Zero();

  for (auto& span : playing) {
    auto end = min<int>(span.size(), n_frames);
    for (int i = 0; i < end; i++) {
      if (__builtin_add_overflow(dst[i].l, span[i].l, &dst[i].l)) {
        if (dst[i].l >= 0) {
          dst[i].l = SHRT_MAX;
        } else {
          dst[i].l = SHRT_MIN;
        }
      }
      if (__builtin_add_overflow(dst[i].r, span[i].r, &dst[i].r)) {
        if (dst[i].r >= 0) {
          dst[i].r = SHRT_MAX;
        } else {
          dst[i].r = SHRT_MIN;
        }
      }
    }
    span.RemovePrefix(end);
  }

  for (auto& span : playing) {
    if (span.empty()) {
      span = playing.back();
      playing.pop_back();
    }
  }

  buf.chunk->offset = 0;
  buf.chunk->stride = sizeof(Frame);
  buf.chunk->size = dst.size_bytes();

  pw_stream_queue_buffer(data->stream, b);

  while (true) {
    auto new_span = to_play.exchange(nullptr);
    if (new_span == nullptr) {
      break;
    }
    to_play.notify_one();
    playing.Append(*new_span);
    delete new_span;
  }
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

void Init(int* argc, char*** argv) {
  pw_init(argc, argv);

  to_play = nullptr;

  pw_loop_thread = std::jthread([] {
    struct data data = {
        0,
    };
    const struct spa_pod* params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    data.loop = pw_main_loop_new(NULL);

    data.stream =
        pw_stream_new_simple(pw_main_loop_get_loop(data.loop), "Automat",
                             pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                                               "Playback", PW_KEY_MEDIA_ROLE, "Game", NULL),
                             &stream_events, &data);

    auto info = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16, .rate = kDefaultRate,
                                        .channels = kDefaultChannels);
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    pw_stream_connect(data.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                        PW_STREAM_FLAG_RT_PROCESS),
                      params, 1);
    pw_main_loop_run(data.loop);
    pw_stream_destroy(data.stream);
    pw_main_loop_destroy(data.loop);
  });
}

struct WAV_Header {
  char riff[4];
  I32 size;
  char wave[4];
  char fmt[4];
  I32 fmt_size;
  I16 format;
  I16 channels;
  I32 rate;
  I32 bytes_per_second;
  I16 block_align;
  I16 bits_per_sample;
  char data[4];
  I32 data_size;
};

void Play(maf::fs::VFile& file) {
  Span<> content = file.content;
  WAV_Header& header = content.Consume<WAV_Header>();
  assert(StrView(header.riff, 4) == "RIFF");
  assert(StrView(header.wave, 4) == "WAVE");
  assert(StrView(header.fmt, 4) == "fmt ");
  assert(StrView(header.data, 4) == "data");
  assert(content.size_bytes() == header.data_size);
  assert(to_play.is_lock_free());

  auto data = new Span<Frame>(content.AsSpanOf<Frame>());
  Span<Frame>* expected_null = nullptr;
  while (!to_play.compare_exchange_strong(expected_null, data)) {
    auto current = to_play.load();
    if (current == nullptr) {
      continue;
    }
    to_play.wait(current);
  }
}

}  // namespace automat::audio