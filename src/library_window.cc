// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_window.hh"

#include <include/core/SkColorType.h>
#include <include/core/SkImage.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>
#include <leptonica/allheaders.h>
#include <leptonica/pix.h>
#include <tesseract/baseapi.h>

#include <ranges>

#include "embedded.hh"
#include "font.hh"
#include "gui_shape_widget.hh"
#include "key.hh"
#include "pointer.hh"
#include "root_widget.hh"
#include "str.hh"
#include "svg.hh"
#include "textures.hh"
#include "theme_xp.hh"

#ifdef __linux__
#include <sys/shm.h>
#include <xcb/shm.h>

#include "xcb.hh"

#pragma comment(lib, "xcb-shm")
#endif

using namespace maf;

namespace automat::library {

constexpr bool kDebugWindowPicking = false;

Window::Window() {}

std::string_view Window::Name() const { return "Window"; }

std::shared_ptr<Object> Window::Clone() const { return std::make_shared<Window>(*this); }

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

struct XSHMCapture {
  xcb_shm_seg_t shmseg = -1;
  int shmid = -1;
  std::span<char> data;

  XSHMCapture() {
    shmseg = xcb_generate_id(xcb::connection);
    int w = xcb::screen->width_in_pixels;
    int h = xcb::screen->height_in_pixels;
    int size = w * h * 4;
    shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0777);

    xcb_shm_attach(xcb::connection, shmseg, shmid, false);
    data = std::span<char>(static_cast<char*>(shmat(shmid, nullptr, 0)), size);
  }

  ~XSHMCapture() {
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
};

struct WindowWidget : gui::Widget, gui::PointerGrabber, gui::KeyGrabber {
  std::weak_ptr<Window> window_weak;

  constexpr static float kWidth = 5_cm;
  constexpr static float kCornerRadius = 1_mm;
  constexpr static float kHeight = 5_cm;

  gui::PointerGrab* pointer_grab = nullptr;
  gui::KeyGrab* key_grab = nullptr;

  std::shared_ptr<PickButton> pick_button;
  std::string window_name;
  std::optional<XSHMCapture> shm_capture;
  int capture_width = 0;
  int capture_height = 0;

  std::unique_ptr<tesseract::TessBaseAPI> tesseract;
  std::string tesseract_text;

  WindowWidget(std::weak_ptr<Window>&& window) : window_weak(std::move(window)) {
    pick_button = std::make_shared<PickButton>();
    pick_button->on_activate = [this](gui::Pointer& p) {
      p.EndAction();
      pointer_grab = &p.RequestGlobalGrab(*this);
      key_grab = &p.keyboard->RequestKeyGrab(*this, gui::AnsiKey::Escape, false, false, false,
                                             false, [this](maf::Status& status) {
                                               if (pointer_grab) pointer_grab->Release();
                                               if (key_grab) key_grab->Release();
                                             });
    };
    auto content_bounds = kCoarseBounds.Outset(-kBorderWidth - kContentMargin);
    auto title_bounds = Rect(kCoarseBounds.rect.left, kCoarseBounds.rect.top - kTitleHeight,
                             kCoarseBounds.rect.right, kCoarseBounds.rect.top);

    auto pos = title_bounds.RightCenter();
    pos.x -= kTitleButtonSize + kContentMargin;
    pos.y -= kTitleButtonSize / 2;
    pick_button->local_to_parent = SkM44::Translate(pos.x, pos.y);

    tesseract = std::make_unique<tesseract::TessBaseAPI>();
    auto eng_traineddata = embedded::assets_eng_traineddata.content;
    if (tesseract->Init(eng_traineddata.data(), eng_traineddata.size(), "eng",
                        tesseract::OEM_LSTM_ONLY, nullptr, 0, nullptr, nullptr, true, nullptr)) {
      LOG << "Tesseract init failed";
    }
  }

  constexpr static RRect kCoarseBounds =
      RRect::MakeSimple(Rect::MakeAtZero({kWidth, kHeight}), kCornerRadius);
  constexpr static RRect kBorderInner = kCoarseBounds.Outset(-kBorderWidth);
  constexpr static float kRegionStrokeWidth = 1_mm;

  RRect CoarseBounds() const override { return kCoarseBounds; }

  SkPath Shape() const override { return SkPath::RRect(CoarseBounds().sk); }

  animation::Phase Tick(time::Timer&) override {
    xcb_window_t xcb_window = XCB_WINDOW_NONE;
    tesseract_text.clear();
    if (auto window = window_weak.lock()) {
      xcb_window = window->xcb_window;
    }
    if (xcb_window == XCB_WINDOW_NONE) {
      shm_capture.reset();
      return animation::Finished;
    }

    if (!shm_capture.has_value()) {
      shm_capture.emplace();
    }

    auto geometry_reply = xcb::get_geometry(xcb_window);
    if (!geometry_reply) {
      return animation::Finished;
    }

    I16 x = 0;
    I16 y = 0;
    capture_width = geometry_reply->width;
    capture_height = geometry_reply->height;

    auto gtk_frame_extents_reply =
        xcb::get_property(xcb_window, xcb::atom::_GTK_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 0, 4);
    if (gtk_frame_extents_reply->value_len == 4) {
      auto extents = (U32*)xcb_get_property_value(gtk_frame_extents_reply.get());
      x += extents[0];
      y += extents[2];
      capture_width -= extents[0] + extents[1];
      capture_height -= extents[2] + extents[3];
    }

    auto cookie =
        xcb_shm_get_image(xcb::connection, xcb_window, x, y, capture_width, capture_height, ~0,
                          XCB_IMAGE_FORMAT_Z_PIXMAP, shm_capture->shmseg, 0);

    std::unique_ptr<xcb_shm_get_image_reply_t, xcb::FreeDeleter> reply(
        xcb_shm_get_image_reply(xcb::connection, cookie, nullptr));

    bool center_pixel_transparent =
        shm_capture->data[(capture_height / 2 * capture_width + capture_width / 2) * 4 + 3] == 0;

    if (center_pixel_transparent) {
      int n = capture_width * capture_height;
      for (int i = 0; i < n; ++i) {
        shm_capture->data[i * 4 + 3] = 0xff;
      }
    }

    if (capture_height > 0 && capture_width > 0) {
      auto pix = pixCreate(capture_width, capture_height, 32);
      uint32_t* data = pixGetData(pix);
      int n = capture_width * capture_height;
      for (int i = 0; i < n; ++i) {
        data[i] = (shm_capture->data[i * 4] << 8) | (shm_capture->data[i * 4 + 1] << 16) |
                  (shm_capture->data[i * 4 + 2] << 24) | (shm_capture->data[i * 4 + 3]);
      }

      int left = 0, top = 0, width = 0, height = 0;
      if (auto window = window_weak.lock()) {
        left = window->x_min_ratio * capture_width;
        top = (1 - window->y_max_ratio) * capture_height;
        width = capture_width * (window->x_max_ratio - window->x_min_ratio);
        height = capture_height * (window->y_max_ratio - window->y_min_ratio);
      }

      if (width > 0 && height > 0) {
        tesseract->SetImage(pix);
        tesseract->SetRectangle(left, top, width, height);  // SetRectangle must come after SetImage
        int recognize_status = tesseract->Recognize(nullptr);
        if (recognize_status) {
          LOG << "Tesseract recognize failed: " << recognize_status;
        }
        tesseract_text = tesseract->GetUTF8Text();
        StripTrailingWhitespace(tesseract_text);

        if constexpr (false) {  // write to "out.bmp"
          std::unique_ptr<uint8_t[], xcb::FreeDeleter> bmp;
          size_t size;
          pixWriteMemBmp(std::out_ptr(bmp), &size, pix);

          FILE* f = fopen("out.bmp", "wb");
          fwrite(bmp.get(), 1, size, f);
          fclose(f);
        }
      }

      pixDestroy(&pix);
    }

    return animation::Animating;
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
    if (capture_height > 0 || capture_width > 0) {
      SkRect image_rect = SkRect::Make(SkISize{capture_width, capture_height});
      l.image_matrix =
          SkMatrix::RectToRect(image_rect, l.contents_rrect.rect, SkMatrix::kCenter_ScaleToFit);
      l.image_matrix.preTranslate(0, capture_height / 2.f);
      l.image_matrix.preScale(1, -1);
      l.image_matrix.preTranslate(0, -capture_height / 2.f);
      l.full_region_rect = image_rect;
      l.image_matrix.mapRect(&l.full_region_rect.sk);
    } else {
      l.full_region_rect = l.contents_rrect.rect;
    }
    if (auto window = window_weak.lock()) {
      l.region_rect =
          Rect(lerp(l.full_region_rect.left, l.full_region_rect.right, window->x_min_ratio),
               lerp(l.full_region_rect.bottom, l.full_region_rect.top, window->y_min_ratio),
               lerp(l.full_region_rect.left, l.full_region_rect.right, window->x_max_ratio),
               lerp(l.full_region_rect.bottom, l.full_region_rect.top, window->y_max_ratio));
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

    if (shm_capture.has_value()) {
      auto& capture = shm_capture.value();
      auto image_info = SkImageInfo::Make(capture_width, capture_height, kBGRA_8888_SkColorType,
                                          SkAlphaType::kPremul_SkAlphaType);
      SkPixmap pixmap(image_info, capture.data.data(), capture_width * 4);
      auto image = SkImages::RasterFromPixmap(
          pixmap, [](const void* pixels, SkImages::ReleaseContext) {}, nullptr);
      canvas.save();
      canvas.concat(layout.image_matrix);
      canvas.drawImage(image, 0, 0, kFastSamplingOptions, nullptr);
      canvas.restore();
    }

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
    std::shared_ptr<WindowWidget> widget;
    Vec2 contact_point;
    uint32_t drag_region_mask = 0;
    DragRegionAction(gui::Pointer& pointer, std::shared_ptr<WindowWidget>&& widget_arg)
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

      if (auto window = widget->window_weak.lock()) {
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
    gui::Widget* Widget() override { return nullptr; }
  };

  std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override {
    if (btn == gui::PointerButton::Left) {
      auto contact_point = p.PositionWithin(*this);

      auto layout = Layout();
      auto outer_region_rect = layout.region_rect.Outset(kRegionStrokeWidth / 2);
      if (outer_region_rect.Contains(contact_point)) {
        return std::make_unique<DragRegionAction>(p, SharedPtr());
      }

      auto* location = Closest<Location>(*p.hover);
      auto* machine = Closest<Machine>(*p.hover);
      if (location && machine) {
        return std::make_unique<DragLocationAction>(p, machine->Extract(*location), contact_point);
      }
    }
    return nullptr;
  }

  void FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) override {
    children.push_back(pick_button);
  }

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
    std::deque<xcb_window_t> search_list;
    search_list.push_back(picked_window);
    while (!search_list.empty()) {
      // Pick the front window, search its children and if none of them has WM_STATE, add them to
      // the queue.
      auto curr = search_list.front();
      search_list.pop_front();
      if (curr == XCB_WINDOW_NONE) {
        continue;
      }

      if (kDebugWindowPicking) {
        LOG << "Checking for WM_STATE: " << f("%x", curr);
      }
      auto property_reply = xcb::get_property(curr, xcb::atom::WM_STATE, XCB_ATOM_ANY, 0, 0);
      if (property_reply->type != XCB_ATOM_NONE) {
        found_window = curr;
        if (kDebugWindowPicking) {
          LOG << "Found!";
        }
        break;
      }
      auto query_tree_reply = xcb::query_tree(curr);
      if (kDebugWindowPicking) {
        LOG << "Contains " << query_tree_reply->children_len << " children";
        LOG_Indent();
      }
      for (auto child : std::ranges::reverse_view(xcb::query_tree_children(*query_tree_reply))) {
        search_list.push_back(child);
      }
      if (kDebugWindowPicking) {
        LOG_Unindent();
      }
    }
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
    if (auto window = window_weak.lock()) {
      window->xcb_window = found_window;
    }
#else
    grab.Release();
#endif
  }
};

std::shared_ptr<gui::Widget> Window::MakeWidget() {
  return std::make_shared<WindowWidget>(WeakPtr());
}

}  // namespace automat::library