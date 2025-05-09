// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <tesseract/baseapi.h>

#include "base.hh"
#include "str.hh"

#ifdef __linux__
#include <sys/shm.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#endif

namespace automat::library {

struct Window : public Object, Runnable {
  std::mutex mutex;
  maf::Str title = "";

#ifdef __linux__
  xcb_window_t xcb_window = XCB_WINDOW_NONE;

  struct XSHMCapture {
    xcb_shm_seg_t shmseg = -1;
    int shmid = -1;
    std::span<char> data;
    int width = 0;
    int height = 0;

    XSHMCapture();
    ~XSHMCapture();
    void Capture(xcb_window_t xcb_window);
  };

  std::optional<XSHMCapture> capture;
#endif

  tesseract::TessBaseAPI tesseract;

  float x_min_ratio = 0.25f;
  float x_max_ratio = 0.75f;
  float y_min_ratio = 0.25f;
  float y_max_ratio = 0.75f;

  Window();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  Ptr<gui::Widget> MakeWidget() override;

  // Run OCR on the currently captured window
  std::string RunOCR();

  void Args(std::function<void(Argument&)> cb) override;
  void OnRun(Location& here) override;

  // Called after deserialization. Makes the window object attach its native handle to the window
  // with the current title.
  void AttachToTitle();

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library