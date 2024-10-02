// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "audio.hh"

#ifdef __linux__
#include <math.h>
#include <pipewire/main-loop.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#pragma maf add link argument "-latomic"
#else
// clang-format off
#undef NOGDI
#include <windows.h>
#undef ERROR
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
// clang-format on
#endif

#include <cassert>
#include <thread>

#include "concurrentqueue.hh"
#include "log.hh"
#include "span.hh"
#include "str.hh"

using namespace maf;
using namespace std;

namespace automat::audio {

jthread loop_thread;
atomic<bool> running = false;

constexpr int kDefaultRate = 48'000;
constexpr int kDefaultChannels = 1;

using Frame = I16;

struct Clip {
  Span<Frame> remaining;
  Span<Frame> all;
  atomic<shared_ptr<Clip>> next = {};
  Clip(Span<Frame> frames) : remaining(frames), all(frames) {}
};

Vec<shared_ptr<Clip>> playing;

moodycamel::ConcurrentQueue<shared_ptr<Clip>> to_play;

// Called from the audio thread to receive new clips from other threads.
static void ReceiveClips() {
  shared_ptr<Clip> to_play_clip = nullptr;
  while (to_play.try_dequeue(to_play_clip)) {
    playing.emplace_back(std::move(to_play_clip));
  }
}

template <typename T>
static void Add(T& out, I16 in);

template <>
void Add(I16& out, I16 in) {
  if (__builtin_add_overflow(out, in, &out)) {
    if (out >= 0) {
      out = SHRT_MAX;
    } else {
      out = SHRT_MIN;
    }
  }
}

template <>
void Add(float& out, I16 in) {
  out += in / 32768.0f;
  if (out > 1) {
    out = 1;
  } else if (out < -1) {
    out = -1;
  }
}

template <typename SAMPLE_T>
static void MixPlayingClips(char* buffer, int n_channels, int n_frames) {
  // TODO(performance, maintainability): mix clips into float buffer, then convert to SAMPLE_T
  auto dst = Span<SAMPLE_T>((SAMPLE_T*)buffer, n_frames * n_channels);
  dst.Zero();
  for (auto& clip : playing) {
    auto dst2 = dst;
    while (!dst2.empty()) {
      if (clip->remaining.empty()) {
        clip = clip->next;
        if (clip == nullptr) {
          break;
        }
        clip->remaining = clip->all;
      }
      for (int c = 0; c < n_channels; ++c) {
        Add(dst2[c], clip->remaining.front());
      }
      clip->remaining.RemovePrefix(1);
      dst2.RemovePrefix(n_channels);
    }
  }

  for (int i = 0; i < playing.size(); ++i) {
    if (playing[i] == nullptr) {
      swap(playing[i], playing.back());
      playing.pop_back();
      --i;
    }
  }
}

#pragma region Linux

#ifdef __linux__

struct data {
  struct pw_main_loop* loop;
  struct pw_stream* stream;
};

static void on_process(void* userdata) {
  struct data* data = (struct data*)userdata;

  if (!running) {
    pw_main_loop_quit(data->loop);
  }

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

  MixPlayingClips<I16>((char*)buf.data, kDefaultChannels, n_frames);

  buf.chunk->offset = 0;
  buf.chunk->stride = sizeof(Frame);
  buf.chunk->size = dst.size_bytes();

  pw_stream_queue_buffer(data->stream, b);

  ReceiveClips();
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

void Init(int* argc, char*** argv) {
  pw_init(argc, argv);
  running = true;

  loop_thread = std::jthread([] {
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
    running = false;
    pw_stream_destroy(data.stream);
    pw_main_loop_destroy(data.loop);
  });
}

void Stop() {
  running = false;
  loop_thread.join();
}

#endif
#pragma endregion

#pragma region Windows
#ifdef _WIN32

#define VERIFY(hr)                          \
  do {                                      \
    auto temp = (hr);                       \
    if (FAILED(temp)) {                     \
      ERROR << #hr << ": " << temp << "\n"; \
      goto error;                           \
    }                                       \
  } while (0)

static char* GetBuffer(IAudioRenderClient* client, int num_frames, Status& status) {
  BYTE* buffer;
  auto hr = client->GetBuffer(num_frames, &buffer);
  if (FAILED(hr)) {
    Str msg;
    switch (hr) {
      case AUDCLNT_E_BUFFER_ERROR:
        msg =
            "GetBuffer failed to retrieve a data buffer and *ppData points to NULL. For more "
            "information, see Remarks.";
        break;
      case AUDCLNT_E_BUFFER_TOO_LARGE:
        msg =
            "The NumFramesRequested value exceeds the available buffer space (buffer size "
            "minus padding size).";
        break;
      case AUDCLNT_E_BUFFER_SIZE_ERROR:
        msg =
            "The stream is exclusive mode and uses event-driven buffering, but the client "
            "attempted to get a packet that was not the size of the buffer.";
        break;
      case AUDCLNT_E_OUT_OF_ORDER:
        msg = "A previous IAudioRenderClient::GetBuffer call is still in effect.";
        break;
      case AUDCLNT_E_DEVICE_INVALIDATED:
        msg =
            "The audio endpoint device has been unplugged, or the audio hardware or "
            "associated hardware resources have been reconfigured, disabled, removed, or "
            "otherwise made unavailable for use.";
        break;
      case AUDCLNT_E_BUFFER_OPERATION_PENDING:
        msg = "Buffer cannot be accessed because a stream reset is in progress.";
        break;
      case AUDCLNT_E_SERVICE_NOT_RUNNING:
        msg = "The Windows audio service is not running.";
        break;
      case E_POINTER:
        msg = "Parameter ppData is NULL.";
        break;
      default:
        msg = std::to_string(hr);
        break;
    }
    AppendErrorMessage(status) += "GetBuffer failed: "s + msg;
    return {};
  }
  return (char*)buffer;
}

void Init() {
  running = true;
  loop_thread = std::jthread([] {
    Status status;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient3* client = nullptr;
    WAVEFORMATEX* format = nullptr;
    UINT32 n_frames;  // number of frames
    UINT32 bufferFrameCount;
    HANDLE event;
    AudioClientProperties props = {
        .cbSize = sizeof(AudioClientProperties),
        .bIsOffload = false,
        .eCategory = AudioCategory_GameEffects,
        .Options = AUDCLNT_STREAMOPTIONS_RAW | AUDCLNT_STREAMOPTIONS_MATCH_FORMAT,
    };
    IAudioRenderClient* render_client = nullptr;
    bool is_float;
    DWORD taskIndex = 0;
    HANDLE hTask;

    VERIFY(CoInitialize(nullptr));
    VERIFY(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                            __uuidof(IMMDeviceEnumerator), (void**)&enumerator));
    VERIFY(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device));
    VERIFY(device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)&client));
    VERIFY(client->GetCurrentSharedModeEnginePeriod(&format, &n_frames));
    VERIFY(client->SetClientProperties(&props));
    VERIFY(client->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, n_frames, format,
                                               nullptr));
    VERIFY(client->GetBufferSize(&bufferFrameCount));
    event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
      ERROR << "CreateEvent failed: " << GetLastError();
      goto error;
    }
    VERIFY(client->SetEventHandle(event));
    VERIFY(client->GetService(__uuidof(IAudioRenderClient), (void**)&render_client));

    // NOTE: change to "Pro Audio" for low latency
    hTask = AvSetMmThreadCharacteristics(TEXT("Audio"), &taskIndex);
    if (hTask == NULL) {
      ERROR << "AvSetMmThreadCharacteristics failed";
      // priority is not critical - continue
    }
    VERIFY(client->Start());

    is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
               (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                ((WAVEFORMATEXTENSIBLE*)format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    if (!is_float) {
      Str error_msg = "Unsupported audio format. ";
      if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        error_msg += dump_struct(*(WAVEFORMATEXTENSIBLE*)format);
      } else {
        error_msg += dump_struct(*format);
      }
      ERROR << error_msg;
      goto error;
    }

    while (true) {
      char* buffer = GetBuffer(render_client, n_frames, status);
      if (!OK(status)) {
        ERROR << status;
        goto error;
      }
      MixPlayingClips<float>(buffer, format->nChannels, n_frames);
      VERIFY(render_client->ReleaseBuffer(n_frames, 0));
      ReceiveClips();

      // Wait for next buffer event to be signaled.
      DWORD retval = WaitForSingleObject(event, 2000);
      if (retval != WAIT_OBJECT_0) {
        ERROR << "WaitForSingleObject failed: " << GetLastError();
        goto error;
      }
    }

  error:
    running = false;
    if (event) CloseHandle(event);
    if (render_client) render_client->Release();
    if (format) CoTaskMemFree(format);
    if (enumerator) enumerator->Release();
    if (device) device->Release();
    CoUninitialize();
  });
}

#endif
#pragma endregion

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

void ScheduleClip(shared_ptr<Clip> clip) {
  if (!running) {
    return;
  }
  to_play.enqueue(std::move(clip));
}

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