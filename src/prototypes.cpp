// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "prototypes.hpp"

#include "library_assembler.hpp"
#include "library_beta_shelf.hpp"
#include "library_command.hpp"
#include "library_ffmpeg.hpp"
#include "library_flip_flop.hpp"
#include "library_gegl.hpp"
#include "library_gstreamer.hpp"
#include "library_hotkey.hpp"
#include "library_instruction_library.hpp"
#include "library_key_presser.hpp"
#include "library_leptonica.hpp"
#include "library_macro_recorder.hpp"
#include "library_mouse.hpp"
#include "library_number.hpp"
#include "library_pipewire.hpp"
#include "library_sources.hpp"
#include "library_tensorflow.hpp"
#include "library_tesseract_ocr.hpp"
#include "library_timeline.hpp"
#include "library_timer.hpp"
#include "library_window.hpp"
#include "object.hpp"
#include "sync.hpp"
#include "wayland.hpp"
#include "x11.hpp"

using namespace automat::library;

namespace automat {

Optional<PrototypeLibrary> prototypes;

enum ToolbarVisibility {
  ShowInToolbar,
  HideInToolbar,
};

struct IndexHelper {
  PrototypeLibrary& lib;
  IndexHelper(PrototypeLibrary& lib) : lib(lib) {}
  template <typename T, ToolbarVisibility show_in_toolbar = ShowInToolbar, typename... Args>
  void Register(Args&&... args) {
    auto proto = MAKE_PTR(T, std::forward<Args>(args)...);
    lib.type_index[typeid(T)] = proto;
    lib.name_index[Str(proto->Name())] = proto;
    if (show_in_toolbar == ShowInToolbar) {
      lib.default_toolbar.push_back(proto);
    }
  }
};

PrototypeLibrary::PrototypeLibrary() {
  IndexHelper index(*this);

  index.Register<FlipFlop>();
  index.Register<Command, HideInToolbar>();
  index.Register<MacroRecorder>();
  index.Register<Timer>();
  index.Register<HotKey>();
  index.Register<KeyPresser>();
  index.Register<Mouse>();
  index.Register<MouseMove, HideInToolbar>();
  index.Register<MouseScrollY, HideInToolbar>();
  index.Register<MouseScrollX, HideInToolbar>();
  index.Register<MouseButtonEvent, HideInToolbar>(ui::PointerButton::Unknown, false);
  index.Register<MouseButtonPresser, HideInToolbar>();
  index.Register<Number>();
  index.Register<Timeline>();
  index.Register<InstructionLibrary>();
  index.Register<Instruction, HideInToolbar>();
  index.Register<Register, HideInToolbar>(nullptr, 0);
  index.Register<Assembler>();
  index.Register<Window>();
  index.Register<TesseractOCR>();
  index.Register<Sources>();
  index.Register<Gear, HideInToolbar>();
  index.Register<WaylandWindow, HideInToolbar>();
  index.Register<X11Window, HideInToolbar>();
  index.Register<BetaShelf>();
#if !defined(_WIN32)
  index.Register<GStreamerElement, HideInToolbar>("videotestsrc", "pattern");
  index.Register<GStreamerElement, HideInToolbar>("videoflip", "video-direction");
  index.Register<GStreamerElement, HideInToolbar>("videoconvert");
  index.Register<GStreamerElement, HideInToolbar>("audiotestsrc", "wave");
  index.Register<GStreamerElement, HideInToolbar>("audioconvert");
  index.Register<GStreamerElement, HideInToolbar>("level");
  index.Register<AppSinkBoundary, HideInToolbar>();
  index.Register<AppSrcBoundary, HideInToolbar>();
  index.Register<MediaFile, HideInToolbar>();
  index.Register<FfmpegDecoder, HideInToolbar>();
  index.Register<GeglBlur, HideInToolbar>();
  index.Register<PipeWireNode, HideInToolbar>();
  index.Register<TfTensor, HideInToolbar>();
  index.Register<TfOp, HideInToolbar>("Square");
#endif
  {  // The Leptonica tools are reached through the shelf, not the toolbar.
    index.Register<LeptonicaShelf, HideInToolbar>();
    index.Register<LeptonicaImage, HideInToolbar>();
    index.Register<Generate, HideInToolbar>();
    index.Register<Threshold, HideInToolbar>();
    index.Register<Morphology, HideInToolbar>();
    index.Register<Tone, HideInToolbar>();
    index.Register<Geometry, HideInToolbar>();
    index.Register<Channel, HideInToolbar>();
    index.Register<Convolve, HideInToolbar>();
    index.Register<Blend, HideInToolbar>();
    index.Register<Quantize, HideInToolbar>();
    index.Register<Flatten, HideInToolbar>();
    index.Register<Posterize, HideInToolbar>();
    index.Register<Dither, HideInToolbar>();
    index.Register<Deskew, HideInToolbar>();
    index.Register<Seedfill, HideInToolbar>();
    index.Register<CropRegion, HideInToolbar>();
    index.Register<FindLevel, HideInToolbar>();
    index.Register<Count, HideInToolbar>();
    index.Register<Color, HideInToolbar>();
    index.Register<Warp, HideInToolbar>();
    index.Register<Measure, HideInToolbar>();
    index.Register<Select, HideInToolbar>();
    index.Register<Fade, HideInToolbar>();
    index.Register<Reduce, HideInToolbar>();
  }
}

Object* PrototypeLibrary::Find(const std::type_info& type) {
  auto it = type_index.find(type);
  if (it != type_index.end()) {
    return it->second.get();
  }
  return nullptr;
}

Object* PrototypeLibrary::Find(StrView name) {
  auto it = name_index.find(name);
  if (it != name_index.end()) {
    return it->second.get();
  }
  return nullptr;
}

}  // namespace automat
