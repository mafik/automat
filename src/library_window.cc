// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_window.hh"

#include <include/core/SkColorType.h>
#include <include/core/SkImage.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>
#include <leptonica/allheaders.h>
#include <leptonica/pix.h>

#include "argument.hh"
#include "embedded.hh"
#include "font.hh"
#include "gui_constants.hh"
#include "gui_shape_widget.hh"
#include "key.hh"
#include "pointer.hh"
#include "root_widget.hh"
#include "str.hh"
#include "svg.hh"
#include "theme_xp.hh"

#ifdef __linux__
#include <sys/shm.h>
#include <xcb/shm.h>

#include <ranges>

#include "control_flow.hh"
#include "textures.hh"
#include "xcb.hh"
#endif

using namespace std;

namespace automat::library {

constexpr bool kDebugWindowPicking = false;

Window::Window() {
  auto eng_traineddata = embedded::assets_eng_traineddata.content;
  if (tesseract.Init(eng_traineddata.data(), eng_traineddata.size(), "eng",
                     tesseract::OEM_LSTM_ONLY, nullptr, 0, nullptr, nullptr, true, nullptr)) {
    LOG << "Tesseract init failed";
  }
}

std::string_view Window::Name() const { return "Window"; }

Ptr<Object> Window::Clone() const {
  auto ret = MakePtr<Window>();
  ret->x_min_ratio = x_min_ratio;
  ret->x_max_ratio = x_max_ratio;
  ret->y_min_ratio = y_min_ratio;
  ret->y_max_ratio = y_max_ratio;
#ifdef __linux__
  ret->xcb_window = xcb_window;
#endif
  return ret;
}

const SkMatrix kCenterPickIcon = SkMatrix::Translate(-1.4_mm, -0.2_mm).preScale(0.9, 0.9);

constexpr static float kBorderWidth = theme::xp::kBorderWidth;  // width of the border
constexpr static float kContentMargin =
    theme::xp::kBorderWidth;  // margin between the border and the content
constexpr static float kTitleHeight = theme::xp::kTitleBarHeight;  // height of the title bar
constexpr static float kTitleButtonSize = kTitleHeight - 2 * kContentMargin;

struct WindowWidget;

struct PickButton : theme::xp::TitleButton {
  std::function<void(gui::Pointer&)> on_activate;
  PickButton()
      : theme::xp::TitleButton(gui::MakeShapeWidget(kPickSVG, "#000000"_color, &kCenterPickIcon)) {
    child->local_to_parent.preTranslate(-0.2_mm, -0.2_mm);
  }
  // float Height() const override { return kTitleButtonSize; }
  SkRRect RRect() const override {
    return RRect::MakeSimple(
               Rect::MakeAtZero<::LeftX, ::BottomY>({kTitleButtonSize, kTitleButtonSize}), 2_mm)
        .sk;
  }

  void Activate(gui::Pointer& p) override {
    WakeAnimation();
    on_activate(p);
  }
};

struct TextArgument : Argument {
  TextDrawable icon;
  TextArgument() : Argument("text", kRequiresObject), icon("T", gui::kLetterSize, gui::GetFont()) {
    requirements.push_back([](Location* location, Object* object, std::string& error) {
      return;  // noop for now
    });
  }
  PaintDrawable& Icon() override { return icon; }
};

TextArgument text_arg;

std::string Window::RunOCR() {
#ifdef __linux__
  if (!capture.has_value()) return "";
  if (capture->width == 0 || capture->height == 0) return "";
  std::string utf8_text = "";
  auto pix = pixCreate(capture->width, capture->height, 32);
  uint32_t* pix_data = pixGetData(pix);
  int n = capture->width * capture->height;
  auto data = capture->data;
  for (int i = 0; i < n; ++i) {
    pix_data[i] =
        (data[i * 4] << 8) | (data[i * 4 + 1] << 16) | (data[i * 4 + 2] << 24) | (data[i * 4 + 3]);
  }

  int ocr_left = 0, ocr_top = 0, ocr_width = 0, ocr_height = 0;
  ocr_left = x_min_ratio * capture->width;
  ocr_top = (1 - y_max_ratio) * capture->height;
  ocr_width = capture->width * (x_max_ratio - x_min_ratio);
  ocr_height = capture->height * (y_max_ratio - y_min_ratio);

  if (ocr_width > 0 && ocr_height > 0) {
    tesseract.SetImage(pix);
    tesseract.SetRectangle(ocr_left, ocr_top, ocr_width,
                           ocr_height);  // SetRectangle must come after SetImage
    int recognize_status = tesseract.Recognize(nullptr);
    if (recognize_status) {
      LOG << "Tesseract recognize failed: " << recognize_status;
    }
    utf8_text = tesseract.GetUTF8Text();
    StripTrailingWhitespace(utf8_text);
  }

  pixDestroy(&pix);
  return utf8_text;
#else
  return "";
#endif
}

#ifdef __linux__
using WindowVisitor = std::function<ControlFlow(xcb_window_t window, xcb_window_t parent)>;
static bool HasWMState(xcb_window_t window) {
  auto property_reply = xcb::get_property(window, xcb::atom::WM_STATE, XCB_ATOM_ANY, 0, 0);
  return property_reply->type != XCB_ATOM_NONE;
}
static void SearchWindows(xcb_window_t start, WindowVisitor visitor) {
  std::deque<std::pair<xcb_window_t, xcb_window_t>> search_list;
  search_list.push_back({start, XCB_WINDOW_NONE});
  while (!search_list.empty()) {
    // Pick the front window, search its children and if none of them has WM_STATE, add them to
    // the queue.
    auto curr = search_list.front();
    search_list.pop_front();
    if (curr.first == XCB_WINDOW_NONE) {
      continue;
    }
    auto control_flow = visitor(curr.first, curr.second);
    if (control_flow == ControlFlow::StopSearching) {
      break;
    } else if (control_flow == ControlFlow::SkipChildren) {
      continue;
    }
    auto query_tree_reply = xcb::query_tree(curr.first);
    for (auto child : std::ranges::reverse_view(xcb::query_tree_children(*query_tree_reply))) {
      search_list.push_back({child, curr.first});
    }
  }
}
#endif

struct WindowWidget : Object::FallbackWidget, gui::PointerGrabber, gui::KeyGrabber {
  constexpr static float kWidth = 5_cm;
  constexpr static float kCornerRadius = 1_mm;
  constexpr static float kHeight = 5_cm;

  gui::PointerGrab* pointer_grab = nullptr;
  gui::KeyGrab* key_grab = nullptr;

  Ptr<PickButton> pick_button;
  std::string window_name;

  std::string tesseract_text;

  Ptr<Window> LockWindow() const { return LockObject<Window>(); }

  WindowWidget(WeakPtr<Object> window) {
    object = window;
    pick_button = MakePtr<PickButton>();
    pick_button->on_activate = [this](gui::Pointer& p) {
      p.EndAllActions();
      pointer_grab = &p.RequestGlobalGrab(*this);
      if (p.keyboard) {
        key_grab = &p.keyboard->RequestKeyGrab(*this, gui::AnsiKey::Escape, false, false, false,
                                               false, [this](Status& status) {
                                                 if (pointer_grab) pointer_grab->Release();
                                                 if (key_grab) key_grab->Release();
                                               });
      }
    };
    auto content_bounds = kCoarseBounds.Outset(-kBorderWidth - kContentMargin);
    auto title_bounds = Rect(kCoarseBounds.rect.left, kCoarseBounds.rect.top - kTitleHeight,
                             kCoarseBounds.rect.right, kCoarseBounds.rect.top);

    auto pos = title_bounds.RightCenter();
    pos.x -= kTitleButtonSize + kContentMargin;
    pos.y -= kTitleButtonSize / 2;
    pick_button->local_to_parent = SkM44::Translate(pos.x, pos.y);
  }

  constexpr static RRect kCoarseBounds =
      RRect::MakeSimple(Rect::MakeAtZero({kWidth, kHeight}), kCornerRadius);
  constexpr static RRect kBorderInner = kCoarseBounds.Outset(-kBorderWidth);
  constexpr static float kRegionStrokeWidth = 1_mm;

  RRect CoarseBounds() const override { return kCoarseBounds; }

  SkPath Shape() const override { return SkPath::RRect(CoarseBounds().sk); }

  animation::Phase Tick(time::Timer&) override {
#ifdef __linux__
    tesseract_text.clear();
    auto window = LockWindow();
    auto lock = std::lock_guard(window->mutex);
    if (window_name != window->title) {
      window_name = window->title;
    }
    if (window->xcb_window == XCB_WINDOW_NONE) {
      window->capture.reset();
      return animation::Finished;
    }

    if (!window->capture.has_value()) {
      window->capture.emplace();
    }

    window->capture->Capture(window->xcb_window);
    tesseract_text = window->RunOCR();

    return animation::Animating;
#else
    return animation::Finished;
#endif
  }

  struct LayoutData {
    RRect contents_rrect;
    Rect title_rect;
    Rect full_region_rect;
    Rect region_rect;
    SkMatrix image_matrix;
  };

  LayoutData Layout() const {
    LayoutData l = {};
    l.title_rect = Rect(kCoarseBounds.rect.left, kCoarseBounds.rect.top - kTitleHeight,
                        kCoarseBounds.rect.right, kCoarseBounds.rect.top);
    l.contents_rrect = kBorderInner;
    l.contents_rrect.rect.top = l.title_rect.bottom;
    if (auto window = LockWindow()) {
#ifdef __linux__
      if (window->capture.has_value() &&
          (window->capture->height > 0 || window->capture->width > 0)) {
        SkRect image_rect = SkRect::Make(SkISize{window->capture->width, window->capture->height});
        l.image_matrix =
            SkMatrix::RectToRect(image_rect, l.contents_rrect.rect, SkMatrix::kCenter_ScaleToFit);
        l.image_matrix.preTranslate(0, window->capture->height / 2.f);
        l.image_matrix.preScale(1, -1);
        l.image_matrix.preTranslate(0, -window->capture->height / 2.f);
        l.full_region_rect = image_rect;
        l.image_matrix.mapRect(&l.full_region_rect.sk);
      } else {
        l.full_region_rect = l.contents_rrect.rect;
      }
      l.region_rect =
          Rect(lerp(l.full_region_rect.left, l.full_region_rect.right, window->x_min_ratio),
               lerp(l.full_region_rect.bottom, l.full_region_rect.top, window->y_min_ratio),
               lerp(l.full_region_rect.left, l.full_region_rect.right, window->x_max_ratio),
               lerp(l.full_region_rect.bottom, l.full_region_rect.top, window->y_max_ratio));
#endif
    }
    l.region_rect = l.region_rect.Outset(kRegionStrokeWidth / 2);
    return l;
  }

  void Draw(SkCanvas& canvas) const override {
    auto layout = Layout();

    auto vertices = theme::xp::WindowBorder(kCoarseBounds.rect);
    canvas.drawVertices(vertices, SkBlendMode::kDst, SkPaint());

    auto& font = gui::GetFont();
    SkPaint title_text_paint;
    title_text_paint.setColor("#ffffff"_color);
    canvas.save();
    auto title_text_pos = layout.title_rect.LeftCenter();
    title_text_pos.x += kContentMargin;
    title_text_pos.y -= font.letter_height / 2;
    canvas.translate(title_text_pos.x, title_text_pos.y);
    font.DrawText(canvas, window_name, title_text_paint);
    canvas.restore();

#ifdef __linux__
    if (auto window = LockWindow()) {
      if (window->capture.has_value()) {
        auto& capture = *window->capture;
        auto image_info =
            SkImageInfo::Make(window->capture->width, window->capture->height,
                              kBGRA_8888_SkColorType, SkAlphaType::kPremul_SkAlphaType);
        SkPixmap pixmap(image_info, capture.data.data(), window->capture->width * 4);
        auto image = SkImages::RasterFromPixmap(
            pixmap, [](const void* pixels, SkImages::ReleaseContext){}, nullptr);
        canvas.save();
        canvas.concat(layout.image_matrix);
        canvas.drawImage(image, 0, 0, kFastSamplingOptions, nullptr);
        canvas.restore();
      }
    }
#endif

    SkPaint region_paint;
    region_paint.setColor("#ff0000"_color);
    region_paint.setStyle(SkPaint::kStroke_Style);
    region_paint.setStrokeWidth(kRegionStrokeWidth);
    region_paint.setAntiAlias(true);
    region_paint.setAlphaf(0.5f);

    canvas.drawRect(layout.region_rect.sk, region_paint);

    float text_width = font.MeasureText(tesseract_text);
    float scale_x = 1;
    if (text_width > layout.contents_rrect.rect.Width()) {
      scale_x = layout.contents_rrect.rect.Width() / text_width;
    }
    canvas.save();
    auto text_pos = layout.contents_rrect.rect.TopLeftCorner();
    text_pos.x += kContentMargin;
    text_pos.y -= kContentMargin + font.letter_height;
    canvas.translate(text_pos.x, text_pos.y);
    if (scale_x != 1) {
      canvas.scale(scale_x, 1);
    }

    SkPaint outline_paint;
    outline_paint.setColor("#000000"_color);
    outline_paint.setStyle(SkPaint::kStroke_Style);
    outline_paint.setStrokeWidth(1_mm / font.font_scale);
    font.DrawText(canvas, tesseract_text, outline_paint);
    font.DrawText(canvas, tesseract_text, title_text_paint);

    canvas.restore();

    DrawChildren(canvas);
  }

  enum DragRegionPart {
    kDragRegionPart_XMin = 1 << 0,
    kDragRegionPart_XMax = 1 << 1,
    kDragRegionPart_YMin = 1 << 2,
    kDragRegionPart_YMax = 1 << 3,
    kDragRegionPart_X = kDragRegionPart_XMin | kDragRegionPart_XMax,
    kDragRegionPart_Y = kDragRegionPart_YMin | kDragRegionPart_YMax,
  };

  // Drags the whole region around
  struct DragRegionAction : Action {
    Ptr<WindowWidget> widget;
    Vec2 contact_point;
    uint32_t drag_region_mask = 0;
    DragRegionAction(gui::Pointer& pointer, Ptr<WindowWidget>&& widget_arg)
        : Action(pointer), widget(std::move(widget_arg)) {
      contact_point = pointer.PositionWithin(*widget);
      auto layout = widget->Layout();
      auto inner_region_rect = layout.region_rect.Outset(-kRegionStrokeWidth / 2);
      if (contact_point.x < inner_region_rect.right) {
        drag_region_mask |= kDragRegionPart_XMin;
      }
      if (contact_point.x > inner_region_rect.left) {
        drag_region_mask |= kDragRegionPart_XMax;
      }
      if (contact_point.y < inner_region_rect.top) {
        drag_region_mask |= kDragRegionPart_YMin;
      }
      if (contact_point.y > inner_region_rect.bottom) {
        drag_region_mask |= kDragRegionPart_YMax;
      }
      // Dragging on the border produces a weird mask that affects three coordinates.
      // When this happens, we disable the drag on the axis where both are enabled and turn this
      // into a resize.
      if (std::popcount(drag_region_mask) == 3) {
        if ((drag_region_mask & kDragRegionPart_X) == kDragRegionPart_X) {
          drag_region_mask &= ~kDragRegionPart_X;
        }
        if ((drag_region_mask & kDragRegionPart_Y) == kDragRegionPart_Y) {
          drag_region_mask &= ~kDragRegionPart_Y;
        }
      }
    }
    void Update() override {
      Vec2 new_position = pointer.PositionWithin(*widget);
      Vec2 d = new_position - contact_point;
      contact_point = new_position;
      auto layout = widget->Layout();
      d /= layout.full_region_rect.Size();

      if (auto window = widget->LockWindow()) {
        // Shift X
        if (drag_region_mask & kDragRegionPart_XMin) {
          window->x_min_ratio += d.x;
        }
        if (drag_region_mask & kDragRegionPart_XMax) {
          window->x_max_ratio += d.x;
        }
        // Enforce x_min <= x_max
        if (window->x_min_ratio > window->x_max_ratio) {
          if (drag_region_mask & kDragRegionPart_XMin) {
            window->x_min_ratio = window->x_max_ratio;
          } else {
            window->x_max_ratio = window->x_min_ratio;
          }
        }
        // Enforce x_max <= 1
        if (window->x_max_ratio > 1) {
          if (drag_region_mask & kDragRegionPart_XMin) {
            window->x_min_ratio -= window->x_max_ratio - 1;
          }
          window->x_max_ratio = 1;
        }
        // Enforce x_min >= 0
        if (window->x_min_ratio < 0) {
          if (drag_region_mask & kDragRegionPart_XMax) {
            window->x_max_ratio -= window->x_min_ratio;
          }
          window->x_min_ratio = 0;
        }

        // Shift Y
        if (drag_region_mask & kDragRegionPart_YMin) {
          window->y_min_ratio += d.y;
        }
        if (drag_region_mask & kDragRegionPart_YMax) {
          window->y_max_ratio += d.y;
        }
        // Enforce y_min <= y_max
        if (window->y_min_ratio > window->y_max_ratio) {
          if (drag_region_mask & kDragRegionPart_YMin) {
            window->y_min_ratio = window->y_max_ratio;
          } else {
            window->y_max_ratio = window->y_min_ratio;
          }
        }
        // Enforce y_max <= 1
        if (window->y_max_ratio > 1) {
          if (drag_region_mask & kDragRegionPart_YMin) {
            window->y_min_ratio -= window->y_max_ratio - 1;
          }
          window->y_max_ratio = 1;
        }
        // Enforce y_min >= 0
        if (window->y_min_ratio < 0) {
          if (drag_region_mask & kDragRegionPart_YMax) {
            window->y_max_ratio -= window->y_min_ratio;
          }
          window->y_min_ratio = 0;
        }
      }
      widget->WakeAnimation();
    }
  };

  std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override {
    if (btn == gui::PointerButton::Left) {
      auto contact_point = p.PositionWithin(*this);

      auto layout = Layout();
      auto outer_region_rect = layout.region_rect.Outset(kRegionStrokeWidth / 2);
      if (outer_region_rect.Contains(contact_point)) {
        return std::make_unique<DragRegionAction>(p, AcquirePtr());
      }
    }
    return FallbackWidget::FindAction(p, btn);
  }

  void FillChildren(Vec<Ptr<Widget>>& children) override { children.push_back(pick_button); }

  void ReleaseGrab(gui::PointerGrab&) override { pointer_grab = nullptr; }
  void ReleaseKeyGrab(gui::KeyGrab&) override { key_grab = nullptr; }
  void KeyGrabberKeyDown(gui::KeyGrab&) override {
    if (pointer_grab) pointer_grab->Release();
    if (key_grab) key_grab->Release();
  }
  void PointerGrabberButtonDown(gui::PointerGrab& grab, gui::PointerButton) override {
#ifdef __linux__
    grab.Release();

    xcb_window_t picked_window = XCB_WINDOW_NONE;
    {
      auto query_pointer_reply = xcb::query_pointer();
      picked_window = query_pointer_reply->child;
    }

    if (kDebugWindowPicking) {
      LOG << "Picked window: " << f("%x", picked_window);
      LOG_Indent();
    }

    // Find the first window that has WM_STATE
    xcb_window_t found_window = XCB_WINDOW_NONE;
    SearchWindows(picked_window, [&](xcb_window_t window, xcb_window_t parent) {
      if (kDebugWindowPicking) {
        LOG << "Checking for WM_STATE: " << f("%x", window);
      }
      if (HasWMState(window)) {
        if (kDebugWindowPicking) {
          LOG << "Found!";
        }
        found_window = window;
        return ControlFlow::StopSearching;
      }
      return ControlFlow::VisitChildren;
    });
    if (kDebugWindowPicking) {
      if (found_window != XCB_WINDOW_NONE) {
        auto name = xcb::GetPropertyString(found_window, XCB_ATOM_WM_NAME);
        LOG << "Found window: " << f("%x", found_window) << " " << name;
      } else {
        LOG << "No window found";
      }
      LOG_Unindent();
    }
    window_name = xcb::GetPropertyString(found_window, XCB_ATOM_WM_NAME);
    WakeAnimation();
    if (auto window = LockWindow()) {
      window->xcb_window = found_window;
      window->title = window_name;
    }
#else
    grab.Release();
#endif
  }
  Vec2AndDir ArgStart(const Argument& arg) override {
    if (&arg == &text_arg) {
      return Vec2AndDir{kCoarseBounds.rect.LeftCenter(), 180_deg};
    }
    return FallbackWidget::ArgStart(arg);
  }
};

Ptr<gui::Widget> Window::MakeWidget() { return MakePtr<WindowWidget>(AcquireWeakPtr<Object>()); }

void Window::Args(std::function<void(Argument&)> cb) {
  cb(text_arg);
  cb(next_arg);
}

void Window::OnRun(Location& here) {
  auto out = text_arg.FindObject(here, {});
  if (!out) {
    here.ReportError("Needs to be connected to a text output");
    return;
  }
  auto lock = std::lock_guard(mutex);
#ifdef __linux__
  if (xcb_window == XCB_WINDOW_NONE) {
    here.ReportError("No window selected");
    return;
  }
  if (!capture.has_value()) {
    capture.emplace();
  }
  capture->Capture(xcb_window);
#endif
  std::string utf8_text = RunOCR();
  out->SetText(here, utf8_text);
}

#ifdef __linux__
Window::XSHMCapture::XSHMCapture() {
  shmseg = xcb_generate_id(xcb::connection);
  int w = xcb::screen->width_in_pixels;
  int h = xcb::screen->height_in_pixels;
  int size = w * h * 4;
  shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0777);

  xcb_shm_attach(xcb::connection, shmseg, shmid, false);
  data = std::span<char>(static_cast<char*>(shmat(shmid, nullptr, 0)), size);
}

Window::XSHMCapture::~XSHMCapture() {
  if (shmseg != -1) {
    xcb_shm_detach(xcb::connection, shmseg);
    shmseg = -1;
  }
  if (data.data()) {
    shmdt(data.data());
    data = {};
  }
  if (shmid != -1) {
    shmctl(shmid, IPC_RMID, NULL);
    shmid = -1;
  }
}

void Window::XSHMCapture::Capture(xcb_window_t xcb_window) {
  auto geometry_reply = xcb::get_geometry(xcb_window);
  if (!geometry_reply) {
    return;
  }

  I16 x = 0;
  I16 y = 0;
  width = geometry_reply->width;
  height = geometry_reply->height;

  auto gtk_frame_extents_reply =
      xcb::get_property(xcb_window, xcb::atom::_GTK_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 0, 4);
  if (gtk_frame_extents_reply->value_len == 4) {
    auto extents = (U32*)xcb_get_property_value(gtk_frame_extents_reply.get());
    x += extents[0];
    y += extents[2];
    width -= extents[0] + extents[1];
    height -= extents[2] + extents[3];
  }

  auto cookie = xcb_shm_get_image(xcb::connection, xcb_window, x, y, width, height, ~0,
                                  XCB_IMAGE_FORMAT_Z_PIXMAP, shmseg, 0);

  std::unique_ptr<xcb_shm_get_image_reply_t, xcb::FreeDeleter> reply(
      xcb_shm_get_image_reply(xcb::connection, cookie, nullptr));

  bool center_pixel_transparent = data[(height / 2 * width + width / 2) * 4 + 3] == 0;

  int n = width * height;
  if (center_pixel_transparent) {
    for (int i = 0; i < n; ++i) {
      data[i * 4 + 3] = 0xff;
    }
  }
}
#endif

void Window::AttachToTitle() {
#ifdef __linux__
  SearchWindows(xcb::screen->root, [&](xcb_window_t window, xcb_window_t parent) {
    if (window == xcb::screen->root) {
      return ControlFlow::VisitChildren;
    }
    auto name = xcb::GetPropertyString(window, xcb::atom::WM_NAME);
    if (name != title) {
      return ControlFlow::SkipChildren;
    }
    if (HasWMState(window)) {
      xcb_window = window;
      return ControlFlow::StopSearching;
    }
    return ControlFlow::VisitChildren;
  });
#endif
}

void Window::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();

  writer.Key("title");
  writer.String(title.data(), title.size());
  writer.Key("x_min_ratio");
  writer.Double(x_min_ratio);
  writer.Key("x_max_ratio");
  writer.Double(x_max_ratio);
  writer.Key("y_min_ratio");
  writer.Double(y_min_ratio);
  writer.Key("y_max_ratio");
  writer.Double(y_max_ratio);

  writer.EndObject();
}

void Window::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto key : ObjectView(d, status)) {
    if (key == "title") {
      d.Get(title, status);
    } else if (key == "x_min_ratio") {
      d.Get(x_min_ratio, status);
    } else if (key == "x_max_ratio") {
      d.Get(x_max_ratio, status);
    } else if (key == "y_min_ratio") {
      d.Get(y_min_ratio, status);
    } else if (key == "y_max_ratio") {
      d.Get(y_max_ratio, status);
    }
  }
  if (!OK(status)) {
    l.ReportError(status.ToStr());
  }
  if (!title.empty()) {
    AttachToTitle();
  }
}

}  // namespace automat::library