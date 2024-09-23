#include "audio.hh"

#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <cassert>
#include <thread>

#include "concurrentqueue.hh"
#include "log.hh"
#include "span.hh"
#include "str.hh"

#pragma maf add link argument "-latomic"

using namespace maf;
using namespace std;

namespace automat::audio {

jthread pw_loop_thread;

constexpr int kDefaultRate = 48'000;
constexpr int kDefaultChannels = 1;

struct Frame {
  I16 mono;
};

struct Clip {
  Span<Frame> remaining;
  Span<Frame> all;
  atomic<shared_ptr<Clip>> next = nullptr;
  Clip(Span<Frame> frames) : remaining(frames), all(frames) {}
};

Vec<shared_ptr<Clip>> playing;

moodycamel::ConcurrentQueue<shared_ptr<Clip>> to_play;

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

  for (auto& clip : playing) {
    int dst_pos = 0;
    while (true) {
      if (dst_pos >= n_frames) {
        break;
      }
      if (clip->remaining.empty()) {
        clip = clip->next;
        if (clip == nullptr) {
          break;
        }
        clip->remaining = clip->all;
      }
      if (__builtin_add_overflow(dst[dst_pos].mono, clip->remaining.front().mono,
                                 &dst[dst_pos].mono)) {
        if (dst[dst_pos].mono >= 0) {
          dst[dst_pos].mono = SHRT_MAX;
        } else {
          dst[dst_pos].mono = SHRT_MIN;
        }
      }
      clip->remaining.RemovePrefix(1);
      ++dst_pos;
    }
  }

  for (int i = 0; i < playing.size(); ++i) {
    if (playing[i] == nullptr) {
      swap(playing[i], playing.back());
      playing.pop_back();
      --i;
    }
  }

  buf.chunk->offset = 0;
  buf.chunk->stride = sizeof(Frame);
  buf.chunk->size = dst.size_bytes();

  pw_stream_queue_buffer(data->stream, b);

  shared_ptr<Clip> to_play_clip = nullptr;
  while (to_play.try_dequeue(to_play_clip)) {
    playing.emplace_back(std::move(to_play_clip));
  }
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

void Init(int* argc, char*** argv) {
  pw_init(argc, argv);

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

shared_ptr<Clip> MakeClipFromWAV(maf::fs::VFile& file) {
  Span<> content = file.content;
  WAV_Header& header = content.Consume<WAV_Header>();

  if (content.size_bytes() > header.data_size) {
    ERROR << file.path << " contains extra data at the end. Run `./run.py optimize_sfx` to fix.";
    content.Resize(header.data_size);
  }
  assert(StrView(header.riff, 4) == "RIFF");
  assert(StrView(header.wave, 4) == "WAVE");
  assert(StrView(header.fmt, 4) == "fmt ");
  assert(StrView(header.data, 4) == "data");
  assert(header.bits_per_sample == 16);
  assert(header.format == 1);
  assert(header.rate == kDefaultRate);
  assert(header.channels == kDefaultChannels);
  assert(content.size_bytes() == header.data_size);
  return make_shared<Clip>(content.AsSpanOf<Frame>());
}

void ScheduleClip(shared_ptr<Clip> clip) { to_play.enqueue(std::move(clip)); }

void Play(maf::fs::VFile& file) { ScheduleClip(MakeClipFromWAV(file)); }

struct BeginLoopEndEffect : Effect {
  shared_ptr<Clip> loop;
  shared_ptr<Clip> end;

  BeginLoopEndEffect(maf::fs::VFile& begin_file, maf::fs::VFile& loop_file,
                     maf::fs::VFile& end_file)
      : loop(MakeClipFromWAV(loop_file)), end(MakeClipFromWAV(end_file)) {
    auto begin = MakeClipFromWAV(begin_file);
    begin->next = loop;
    loop->next = loop;
    ScheduleClip(begin);
  }

  ~BeginLoopEndEffect() override { loop->next = end; }
};

std::unique_ptr<Effect> MakeBeginLoopEndEffect(maf::fs::VFile& begin_file,
                                               maf::fs::VFile& loop_file,
                                               maf::fs::VFile& end_file) {
  return make_unique<BeginLoopEndEffect>(begin_file, loop_file, end_file);
}

}  // namespace automat::audio