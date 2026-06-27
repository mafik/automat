#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkPath.h>
#include <include/core/SkPoint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSize.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../build/generated/wayland_generated.hpp"  // IWYU pragma: export
#include "colony.hpp"
#include "fd.hpp"
#include "int.hpp"
#include "mortal.hpp"
#include "mux_epoll.hpp"
#include "mux_timer.hpp"
#include "optional.hpp"
#include "path.hpp"
#include "ptr.hpp"
#include "span.hpp"
#include "str.hpp"
#include "vec.hpp"

// Provides utilities for working with the Wayland protocol:
//
// 1. A set of well-documented classes that directly map to Wayland interfaces
// 2. Automat's built-in Wayland Compositor
//
// Automat's implementation of Wayland interfaces turns each interface into a lightweight C++ class.
//
// *Requests* are methods that start with 'On*'. Server must implement each of them. The client may
// call them whenever they wish.
//
// *Events* are methods that do not start with 'On'. Server may call them whenever it wishes to.
// They are buffered internally and will be sent over to the client at the next 'server.FlushAll()'.
//
// *Lifetime* of the interfaces is entirely up to clients. Clients may allocate up to ~4.3 billion
// objects and have no obligation to ever free them. Turns out DoS is a core feature of the Wayland
// protocol... Anyway - because Wayland interfaces are so ephemeral (at least on the Server side) -
// they should really be treated more like references to some more permanent objects, potentially
// shared across clients. For Automat those are various Object-derivatives, tracked in a thread-safe
// way through Ptr.
//
// Current implementation is O(1) wherever possible, uses compact array-based indexing, global
// Colony-based allocation etc. The efficiency is currently balanced with ergonomy of use. A more
// efficient implementation is possible - one that would not store any class (kind + id +
// client-ptr + extension data) and not even their index but instead - just 'kind' for each id (this
// is essentially 1B / object). This would however force the user to store the extra data
// out-of-band (in some kind of hashmap keyed by 'client' + 'id'). This alternative design may be
// considered in the future.

namespace automat::library {
struct WaylandSurface;
struct WaylandWindow;
}  // namespace automat::library

namespace automat::wayland {

struct Common {
  Kind kind;  // LLVM-style RTTI
  U32 id;
  Client& client;  // Saves one parameter from every method call
  MortalCoil mortal_coil;
  Common(Kind kind, U32 id, Client& client);
  ~Common();
  void GenericColonyDestroy();                                       // generated
  void GenericDispatch(U32 opcode, const char* p, const char* end);  // generated
};

struct CutoutGeometry {
  SkRect src_crop = SkRect::MakeEmpty();
  SkISize dst_size = {};
};
struct SurfaceCutout : CutoutGeometry {
  sk_sp<SkImage> image;
};

struct InputRegion {
  bool infinite = true;  // by default the whole surface is reactive
  SkPath path;
};

struct DmabufPlane {
  FD fd;
  U32 offset = 0;
  U32 stride = 0;
};

// clang-format off: generated CursorShapeManagerV1 from cursor-shape-v1.xml

// Cursor shape manager
//
// This global offers an alternative, optional way to set cursor images. This
// new way uses enumerated cursors instead of a wl_surface like
// wl_pointer.set_cursor does.
// 
// Warning! The protocol described in this file is currently in the testing
// phase. Backward compatible changes may be added together with the
// corresponding interface version bump. Backward incompatible changes can
// only be done by creating a new major version of the extension.
struct CursorShapeManagerV1 : Common {
  static constexpr int Version = 2;
  static constexpr Kind Kind = KindCursorShapeManagerV1;

  static Colony<CursorShapeManagerV1> colony;

  // Do not use directly. Instead use `ColonyMake`
  CursorShapeManagerV1(U32 id, Client& client) : Common(Kind, id, client) {}

  static CursorShapeManagerV1& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Destroy the manager
  //
  // Destroy the cursor shape manager.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Manage the cursor shape of a pointer device
  //
  // Obtain a wp_cursor_shape_device_v1 for a wl_pointer object.
  // 
  // When the pointer capability is removed from the wl_seat, the
  // wp_cursor_shape_device_v1 object becomes inert.
  void OnGetPointer(CursorShapeDeviceV1& cursor_shape_device, Pointer& pointer);

  // Manage the cursor shape of a tablet tool device
  //
  // Obtain a wp_cursor_shape_device_v1 for a zwp_tablet_tool_v2 object.
  // 
  // When the zwp_tablet_tool_v2 is removed, the wp_cursor_shape_device_v1
  // object becomes inert.
  void OnGetTabletToolV2(CursorShapeDeviceV1& cursor_shape_device, U32 tablet_tool);
  // clang-format on: generated CursorShapeManagerV1 from cursor-shape-v1.xml
};

// clang-format off: generated CursorShapeDeviceV1 from cursor-shape-v1.xml

// Cursor shape for a device
//
// This interface allows clients to set the cursor shape.
struct CursorShapeDeviceV1 : Common {
  static constexpr int Version = 2;
  static constexpr Kind Kind = KindCursorShapeDeviceV1;

  static Colony<CursorShapeDeviceV1> colony;

  // Do not use directly. Instead use `ColonyMake`
  CursorShapeDeviceV1(U32 id, Client& client) : Common(Kind, id, client) {}

  static CursorShapeDeviceV1& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Cursor shapes
  //
  // This enum describes cursor shapes.
  // 
  // The names are taken from the CSS W3C specification:
  // https://w3c.github.io/csswg-drafts/css-ui/#cursor
  // with a few additions.
  // 
  // Note that there are some groups of cursor shapes that are related:
  // The first group is drag-and-drop cursors which are used to indicate
  // the selected action during dnd operations. The second group is resize
  // cursors which are used to indicate resizing and moving possibilities
  // on window borders. It is recommended that the shapes in these groups
  // should use visually compatible images and metaphors.
  enum Shape : U32 {
    ShapeDefault = 1, // Default cursor
    ShapeContextMenu = 2, // A context menu is available for the object under the cursor
    ShapeHelp = 3, // Help is available for the object under the cursor
    ShapePointer = 4, // Pointer that indicates a link or another interactive element
    ShapeProgress = 5, // Progress indicator
    ShapeWait = 6, // Program is busy, user should wait
    ShapeCell = 7, // A cell or set of cells may be selected
    ShapeCrosshair = 8, // Simple crosshair
    ShapeText = 9, // Text may be selected
    ShapeVerticalText = 10, // Vertical text may be selected
    ShapeAlias = 11, // Drag-and-drop: alias of/shortcut to something is to be created
    ShapeCopy = 12, // Drag-and-drop: something is to be copied
    ShapeMove = 13, // Drag-and-drop: something is to be moved
    ShapeNoDrop = 14, // Drag-and-drop: the dragged item cannot be dropped at the current cursor location
    ShapeNotAllowed = 15, // Drag-and-drop: the requested action will not be carried out
    ShapeGrab = 16, // Drag-and-drop: something can be grabbed
    ShapeGrabbing = 17, // Drag-and-drop: something is being grabbed
    ShapeEResize = 18, // Resizing: the east border is to be moved
    ShapeNResize = 19, // Resizing: the north border is to be moved
    ShapeNeResize = 20, // Resizing: the north-east corner is to be moved
    ShapeNwResize = 21, // Resizing: the north-west corner is to be moved
    ShapeSResize = 22, // Resizing: the south border is to be moved
    ShapeSeResize = 23, // Resizing: the south-east corner is to be moved
    ShapeSwResize = 24, // Resizing: the south-west corner is to be moved
    ShapeWResize = 25, // Resizing: the west border is to be moved
    ShapeEwResize = 26, // Resizing: the east and west borders are to be moved
    ShapeNsResize = 27, // Resizing: the north and south borders are to be moved
    ShapeNeswResize = 28, // Resizing: the north-east and south-west corners are to be moved
    ShapeNwseResize = 29, // Resizing: the north-west and south-east corners are to be moved
    ShapeColResize = 30, // Resizing: that the item/column can be resized horizontally
    ShapeRowResize = 31, // Resizing: that the item/row can be resized vertically
    ShapeAllScroll = 32, // Something can be scrolled in any direction
    ShapeZoomIn = 33, // Something can be zoomed in
    ShapeZoomOut = 34, // Something can be zoomed out
    ShapeDndAsk = 35, // Drag-and-drop: the user will select which action will be carried out (non-css value)
    ShapeAllResize = 36, // Resizing: something can be moved or resized in any direction (non-css value)
  };

  static StrView ShapeToStr(U32 value) {
    switch (value) {
    case 1: return "Default"sv;
    case 2: return "ContextMenu"sv;
    case 3: return "Help"sv;
    case 4: return "Pointer"sv;
    case 5: return "Progress"sv;
    case 6: return "Wait"sv;
    case 7: return "Cell"sv;
    case 8: return "Crosshair"sv;
    case 9: return "Text"sv;
    case 10: return "VerticalText"sv;
    case 11: return "Alias"sv;
    case 12: return "Copy"sv;
    case 13: return "Move"sv;
    case 14: return "NoDrop"sv;
    case 15: return "NotAllowed"sv;
    case 16: return "Grab"sv;
    case 17: return "Grabbing"sv;
    case 18: return "EResize"sv;
    case 19: return "NResize"sv;
    case 20: return "NeResize"sv;
    case 21: return "NwResize"sv;
    case 22: return "SResize"sv;
    case 23: return "SeResize"sv;
    case 24: return "SwResize"sv;
    case 25: return "WResize"sv;
    case 26: return "EwResize"sv;
    case 27: return "NsResize"sv;
    case 28: return "NeswResize"sv;
    case 29: return "NwseResize"sv;
    case 30: return "ColResize"sv;
    case 31: return "RowResize"sv;
    case 32: return "AllScroll"sv;
    case 33: return "ZoomIn"sv;
    case 34: return "ZoomOut"sv;
    case 35: return "DndAsk"sv;
    case 36: return "AllResize"sv;
    default: return "UnknownShape"sv;
    }
  }

  enum Error : U32 {
    ErrorInvalidShape = 1, // The specified shape value is invalid
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 1: return "InvalidShape"sv;
    default: return "UnknownError"sv;
    }
  }

  // Destroy the cursor shape device
  //
  // Destroy the cursor shape device.
  // 
  // The device cursor shape remains unchanged.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Set device cursor to the shape
  //
  // Sets the device cursor to the specified shape. The compositor will
  // change the cursor image based on the specified shape.
  // 
  // The cursor actually changes only if the input device focus is one of
  // the requesting client's surfaces. If any, the previous cursor image
  // (surface or shape) is replaced.
  // 
  // The "shape" argument must be a valid enum entry, otherwise the
  // invalid_shape protocol error is raised.
  // 
  // This is similar to the wl_pointer.set_cursor and
  // zwp_tablet_tool_v2.set_cursor requests, but this request accepts a
  // shape instead of contents in the form of a surface. Clients can mix
  // set_cursor and set_shape requests.
  // 
  // The serial parameter must match the latest wl_pointer.enter or
  // zwp_tablet_tool_v2.proximity_in serial number sent to the client.
  // Otherwise the request will be ignored.
  void OnSetShape(U32 serial, enum Shape shape);
  // clang-format on: generated CursorShapeDeviceV1 from cursor-shape-v1.xml
};

// clang-format off: generated LinuxDmabufV1 from linux-dmabuf-v1.xml

// Factory for creating dmabuf-based wl_buffers
//
// This interface offers ways to create generic dmabuf-based wl_buffers.
// 
// For more information about dmabuf, see:
// https://www.kernel.org/doc/html/next/userspace-api/dma-buf-alloc-exchange.html
// 
// Clients can use the get_surface_feedback request to get dmabuf feedback
// for a particular surface. If the client wants to retrieve feedback not
// tied to a surface, they can use the get_default_feedback request.
// 
// The following are required from clients:
// 
// - Clients must ensure that either all data in the dma-buf is
//   coherent for all subsequent read access or that coherency is
//   correctly handled by the underlying kernel-side dma-buf
//   implementation.
// 
// - Don't make any more attachments after sending the buffer to the
//   compositor. Making more attachments later increases the risk of
//   the compositor not being able to use (re-import) an existing
//   dmabuf-based wl_buffer.
// 
// The underlying graphics stack must ensure the following:
// 
// - The dmabuf file descriptors relayed to the server will stay valid
//   for the whole lifetime of the wl_buffer. This means the server may
//   at any time use those fds to import the dmabuf into any kernel
//   sub-system that might accept it.
// 
// However, when the underlying graphics stack fails to deliver the
// promise, because of e.g. a device hot-unplug which raises internal
// errors, after the wl_buffer has been successfully created the
// compositor must not raise protocol errors to the client when dmabuf
// import later fails.
// 
// To create a wl_buffer from one or more dmabufs, a client creates a
// zwp_linux_dmabuf_params_v1 object with a zwp_linux_dmabuf_v1.create_params
// request. All planes required by the intended format are added with
// the 'add' request. Finally, a 'create' or 'create_immed' request is
// issued, which has the following outcome depending on the import success.
// 
// The 'create' request,
// - on success, triggers a 'created' event which provides the final
//   wl_buffer to the client.
// - on failure, triggers a 'failed' event to convey that the server
//   cannot use the dmabufs received from the client.
// 
// For the 'create_immed' request,
// - on success, the server immediately imports the added dmabufs to
//   create a wl_buffer. No event is sent from the server in this case.
// - on failure, the server can choose to either:
//   - terminate the client by raising a fatal error.
//   - mark the wl_buffer as failed, and send a 'failed' event to the
//     client. If the client uses a failed wl_buffer as an argument to any
//     request, the behaviour is compositor implementation-defined.
// 
// For all DRM formats and unless specified in another protocol extension,
// pre-multiplied alpha is used for pixel values.
// 
// Unless specified otherwise in another protocol extension, implicit
// synchronization is used. In other words, compositors and clients must
// wait and signal fences implicitly passed via the DMA-BUF's reservation
// mechanism.
struct LinuxDmabufV1 : Common {
  static constexpr int Version = 5;
  static constexpr Kind Kind = KindLinuxDmabufV1;

  static Colony<LinuxDmabufV1> colony;

  // Do not use directly. Instead use `ColonyMake`
  LinuxDmabufV1(U32 id, Client& client) : Common(Kind, id, client) {}

  static LinuxDmabufV1& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Unbind the factory
  //
  // Objects created through this interface, especially wl_buffers, will
  // remain valid.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Create a temporary object for buffer parameters
  //
  // This temporary object is used to collect multiple dmabuf handles into
  // a single batch to create a wl_buffer. It can only be used once and
  // should be destroyed after a 'created' or 'failed' event has been
  // received.
  void OnCreateParams(LinuxBufferParamsV1& params_id);

  // Get default feedback
  //
  // This request creates a new wp_linux_dmabuf_feedback object not bound
  // to a particular surface. This object will deliver feedback about dmabuf
  // parameters to use if the client doesn't support per-surface feedback
  // (see get_surface_feedback).
  void OnGetDefaultFeedback(LinuxDmabufFeedbackV1& id);

  // Get feedback for a surface
  //
  // This request creates a new wp_linux_dmabuf_feedback object for the
  // specified wl_surface. This object will deliver feedback about dmabuf
  // parameters to use for buffers attached to this surface.
  // 
  // If the surface is destroyed before the wp_linux_dmabuf_feedback object,
  // the feedback object becomes inert.
  void OnGetSurfaceFeedback(LinuxDmabufFeedbackV1& id, Surface& surface);

  // Supported buffer format
  //
  // This event advertises one buffer format that the server supports.
  // All the supported formats are advertised once when the client
  // binds to this interface. A roundtrip after binding guarantees
  // that the client has received all supported formats.
  // 
  // For the definition of the format codes, see the
  // zwp_linux_buffer_params_v1::create request.
  // 
  // Starting version 4, the format event is deprecated and must not be
  // sent by compositors. Instead, use get_default_feedback or
  // get_surface_feedback.
  void Format(U32 format);

  // Supported buffer format modifier
  //
  // This event advertises the formats that the server supports, along with
  // the modifiers supported for each format. All the supported modifiers
  // for all the supported formats are advertised once when the client
  // binds to this interface. A roundtrip after binding guarantees that
  // the client has received all supported format-modifier pairs.
  // 
  // For legacy support, DRM_FORMAT_MOD_INVALID (that is, modifier_hi ==
  // 0x00ffffff and modifier_lo == 0xffffffff) is allowed in this event.
  // It indicates that the server can support the format with an implicit
  // modifier. When a plane has DRM_FORMAT_MOD_INVALID as its modifier, it
  // is as if no explicit modifier is specified. The effective modifier
  // will be derived from the dmabuf.
  // 
  // A compositor that sends valid modifiers and DRM_FORMAT_MOD_INVALID for
  // a given format supports both explicit modifiers and implicit modifiers.
  // 
  // For the definition of the format and modifier codes, see the
  // zwp_linux_buffer_params_v1::create and zwp_linux_buffer_params_v1::add
  // requests.
  // 
  // Starting version 4, the modifier event is deprecated and must not be
  // sent by compositors. Instead, use get_default_feedback or
  // get_surface_feedback.
  void Modifier(U32 format, U32 modifier_hi, U32 modifier_lo);
  // clang-format on: generated LinuxDmabufV1 from linux-dmabuf-v1.xml
};

// clang-format off: generated LinuxBufferParamsV1 from linux-dmabuf-v1.xml

// Parameters for creating a dmabuf-based wl_buffer
//
// This temporary object is a collection of dmabufs and other
// parameters that together form a single logical buffer. The temporary
// object may eventually create one wl_buffer unless cancelled by
// destroying it before requesting 'create'.
// 
// Single-planar formats only require one dmabuf, however
// multi-planar formats may require more than one dmabuf. For all
// formats, an 'add' request must be called once per plane (even if the
// underlying dmabuf fd is identical).
// 
// You must use consecutive plane indices ('plane_idx' argument for 'add')
// from zero to the number of planes used by the drm_fourcc format code.
// All planes required by the format must be given exactly once, but can
// be given in any order. Each plane index can only be set once; subsequent
// calls with a plane index which has already been set will result in a
// plane_set error being generated.
struct LinuxBufferParamsV1 : Common {
  static constexpr int Version = 5;
  static constexpr Kind Kind = KindLinuxBufferParamsV1;

  static Colony<LinuxBufferParamsV1> colony;

  // Do not use directly. Instead use `ColonyMake`
  LinuxBufferParamsV1(U32 id, Client& client) : Common(Kind, id, client) {}

  static LinuxBufferParamsV1& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorAlreadyUsed = 0, // The dmabuf_batch object has already been used to create a wl_buffer
    ErrorPlaneIdx = 1, // Plane index out of bounds
    ErrorPlaneSet = 2, // The plane index was already set
    ErrorIncomplete = 3, // Missing or too many planes to create a buffer
    ErrorInvalidFormat = 4, // Format not supported
    ErrorInvalidDimensions = 5, // Invalid width or height
    ErrorOutOfBounds = 6, // Offset + stride * height goes out of dmabuf bounds
    ErrorInvalidWlBuffer = 7, // Invalid wl_buffer resulted from importing dmabufs via                the create_immed request on given buffer_params
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "AlreadyUsed"sv;
    case 1: return "PlaneIdx"sv;
    case 2: return "PlaneSet"sv;
    case 3: return "Incomplete"sv;
    case 4: return "InvalidFormat"sv;
    case 5: return "InvalidDimensions"sv;
    case 6: return "OutOfBounds"sv;
    case 7: return "InvalidWlBuffer"sv;
    default: return "UnknownError"sv;
    }
  }

  enum Flags : U32 {
    FlagsYInvert = 1, // Contents are y-inverted
    FlagsInterlaced = 2, // Content is interlaced
    FlagsBottomFirst = 4, // Bottom field first
  };

  static StrView FlagsToStr(U32 value) {
    switch (value) {
    case 1: return "YInvert"sv;
    case 2: return "Interlaced"sv;
    case 4: return "BottomFirst"sv;
    default: return "UnknownFlags"sv;
    }
  }

  // Delete this object, used or not
  //
  // Cleans up the temporary data sent to the server for dmabuf-based
  // wl_buffer creation.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Add a dmabuf to the temporary set
  //
  // This request adds one dmabuf to the set in this
  // zwp_linux_buffer_params_v1.
  // 
  // The 64-bit unsigned value combined from modifier_hi and modifier_lo
  // is the dmabuf layout modifier. DRM AddFB2 ioctl calls this the
  // fb modifier, which is defined in drm_mode.h of Linux UAPI.
  // This is an opaque token. Drivers use this token to express tiling,
  // compression, etc. driver-specific modifications to the base format
  // defined by the DRM fourcc code.
  // 
  // Starting from version 4, the invalid_format protocol error is sent if
  // the format + modifier pair was not advertised as supported.
  // 
  // Starting from version 5, the invalid_format protocol error is sent if
  // all planes don't use the same modifier.
  // 
  // This request raises the PLANE_IDX error if plane_idx is too large.
  // The error PLANE_SET is raised if attempting to set a plane that
  // was already set.
  void OnAdd(FD&& fd, U32 plane_idx, U32 offset, U32 stride, U32 modifier_hi, U32 modifier_lo);

  // Create a wl_buffer from the given dmabufs
  //
  // This asks for creation of a wl_buffer from the added dmabuf
  // buffers. The wl_buffer is not created immediately but returned via
  // the 'created' event if the dmabuf sharing succeeds. The sharing
  // may fail at runtime for reasons a client cannot predict, in
  // which case the 'failed' event is triggered.
  // 
  // The 'format' argument is a DRM_FORMAT code, as defined by the
  // libdrm's drm_fourcc.h. The Linux kernel's DRM sub-system is the
  // authoritative source on how the format codes should work.
  // 
  // The 'flags' is a bitfield of the flags defined in enum "flags".
  // 'y_invert' means the that the image needs to be y-flipped.
  // 
  // Flag 'interlaced' means that the frame in the buffer is not
  // progressive as usual, but interlaced. An interlaced buffer as
  // supported here must always contain both top and bottom fields.
  // The top field always begins on the first pixel row. The temporal
  // ordering between the two fields is top field first, unless
  // 'bottom_first' is specified. It is undefined whether 'bottom_first'
  // is ignored if 'interlaced' is not set.
  // 
  // This protocol does not convey any information about field rate,
  // duration, or timing, other than the relative ordering between the
  // two fields in one buffer. A compositor may have to estimate the
  // intended field rate from the incoming buffer rate. It is undefined
  // whether the time of receiving wl_surface.commit with a new buffer
  // attached, applying the wl_surface state, wl_surface.frame callback
  // trigger, presentation, or any other point in the compositor cycle
  // is used to measure the frame or field times. There is no support
  // for detecting missed or late frames/fields/buffers either, and
  // there is no support whatsoever for cooperating with interlaced
  // compositor output.
  // 
  // The composited image quality resulting from the use of interlaced
  // buffers is explicitly undefined. A compositor may use elaborate
  // hardware features or software to deinterlace and create progressive
  // output frames from a sequence of interlaced input buffers, or it
  // may produce substandard image quality. However, compositors that
  // cannot guarantee reasonable image quality in all cases are recommended
  // to just reject all interlaced buffers.
  // 
  // Any argument errors, including non-positive width or height,
  // mismatch between the number of planes and the format, bad
  // format, bad offset or stride, may be indicated by fatal protocol
  // errors: INCOMPLETE, INVALID_FORMAT, INVALID_DIMENSIONS,
  // OUT_OF_BOUNDS.
  // 
  // Dmabuf import errors in the server that are not obvious client
  // bugs are returned via the 'failed' event as non-fatal. This
  // allows attempting dmabuf sharing and falling back in the client
  // if it fails.
  // 
  // This request can be sent only once in the object's lifetime, after
  // which the only legal request is destroy. This object should be
  // destroyed after issuing a 'create' request. Attempting to use this
  // object after issuing 'create' raises ALREADY_USED protocol error.
  // 
  // It is not mandatory to issue 'create'. If a client wants to
  // cancel the buffer creation, it can just destroy this object.
  void OnCreate(I32 width, I32 height, U32 format, enum Flags flags);

  // Immediately create a wl_buffer from the given                      dmabufs
  //
  // This asks for immediate creation of a wl_buffer by importing the
  // added dmabufs.
  // 
  // In case of import success, no event is sent from the server, and the
  // wl_buffer is ready to be used by the client.
  // 
  // Upon import failure, either of the following may happen, as seen fit
  // by the implementation:
  // - the client is terminated with one of the following fatal protocol
  //   errors:
  //   - INCOMPLETE, INVALID_FORMAT, INVALID_DIMENSIONS, OUT_OF_BOUNDS,
  //     in case of argument errors such as mismatch between the number
  //     of planes and the format, bad format, non-positive width or
  //     height, or bad offset or stride.
  //   - INVALID_WL_BUFFER, in case the cause for failure is unknown or
  //     platform specific.
  // - the server creates an invalid wl_buffer, marks it as failed and
  //   sends a 'failed' event to the client. The result of using this
  //   invalid wl_buffer as an argument in any request by the client is
  //   defined by the compositor implementation.
  // 
  // This takes the same arguments as a 'create' request, and obeys the
  // same restrictions.
  void OnCreateImmed(Buffer& buffer_id, I32 width, I32 height, U32 format, enum Flags flags);

  // Buffer creation succeeded
  //
  // This event indicates that the attempted buffer creation was
  // successful. It provides the new wl_buffer referencing the dmabuf(s).
  // 
  // Upon receiving this event, the client should destroy the
  // zwp_linux_buffer_params_v1 object.
  void Created(Buffer& buffer);

  // Buffer creation failed
  //
  // This event indicates that the attempted buffer creation has
  // failed. It usually means that one of the dmabuf constraints
  // has not been fulfilled.
  // 
  // Upon receiving this event, the client should destroy the
  // zwp_linux_buffer_params_v1 object.
  void Failed();
  // clang-format on: generated LinuxBufferParamsV1 from linux-dmabuf-v1.xml

  DmabufPlane planes[4];
  int plane_count = 0;
  U64 modifier = 0;
  bool used = false;
};

// clang-format off: generated LinuxDmabufFeedbackV1 from linux-dmabuf-v1.xml

// Dmabuf feedback
//
// This object advertises dmabuf parameters feedback. This includes the
// preferred devices and the supported formats/modifiers.
// 
// The parameters are sent once when this object is created and whenever they
// change. The done event is always sent once after all parameters have been
// sent. When a single parameter changes, all parameters are re-sent by the
// compositor.
// 
// Compositors can re-send the parameters when the current client buffer
// allocations are sub-optimal. Compositors should not re-send the
// parameters if re-allocating the buffers would not result in a more optimal
// configuration. In particular, compositors should avoid sending the exact
// same parameters multiple times in a row.
// 
// The tranche_target_device and tranche_formats events are grouped by
// tranches of preference. For each tranche, a tranche_target_device, one
// tranche_flags and one or more tranche_formats events are sent, followed
// by a tranche_done event finishing the list. The tranches are sent in
// descending order of preference. All formats and modifiers in the same
// tranche have the same preference.
// 
// To send parameters, the compositor sends one main_device event, tranches
// (each consisting of one tranche_target_device event, one tranche_flags
// event, tranche_formats events and then a tranche_done event), then one
// done event.
struct LinuxDmabufFeedbackV1 : Common {
  static constexpr int Version = 5;
  static constexpr Kind Kind = KindLinuxDmabufFeedbackV1;

  static Colony<LinuxDmabufFeedbackV1> colony;

  // Do not use directly. Instead use `ColonyMake`
  LinuxDmabufFeedbackV1(U32 id, Client& client) : Common(Kind, id, client) {}

  static LinuxDmabufFeedbackV1& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum TrancheFlags : U32 {
    TrancheFlagsScanout = 1, // Direct scan-out tranche
  };

  static StrView TrancheFlagsToStr(U32 value) {
    switch (value) {
    case 1: return "Scanout"sv;
    default: return "UnknownTrancheFlags"sv;
    }
  }

  // Destroy the feedback object
  //
  // Using this request a client can tell the server that it is not going to
  // use the wp_linux_dmabuf_feedback object anymore.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // All feedback has been sent
  //
  // This event is sent after all parameters of a wp_linux_dmabuf_feedback
  // object have been sent.
  // 
  // This allows changes to the wp_linux_dmabuf_feedback parameters to be
  // seen as atomic, even if they happen via multiple events.
  void Done();

  // Format and modifier table
  //
  // This event provides a file descriptor which can be memory-mapped to
  // access the format and modifier table.
  // 
  // The table contains a tightly packed array of consecutive format +
  // modifier pairs. Each pair is 16 bytes wide. It contains a format as a
  // 32-bit unsigned integer, followed by 4 bytes of unused padding, and a
  // modifier as a 64-bit unsigned integer. The native endianness is used.
  // 
  // The client must map the file descriptor in read-only private mode.
  // 
  // Compositors are not allowed to mutate the table file contents once this
  // event has been sent. Instead, compositors must create a new, separate
  // table file and re-send feedback parameters. Compositors are allowed to
  // store duplicate format + modifier pairs in the table.
  void FormatTable(FD&& fd, U32 size);

  // Preferred main device
  //
  // This event advertises the main device that the server prefers to use
  // when direct scan-out to the target device isn't possible. The
  // advertised main device may be different for each
  // wp_linux_dmabuf_feedback object, and may change over time.
  // 
  // There is exactly one main device. The compositor must send at least
  // one preference tranche with tranche_target_device equal to main_device.
  // 
  // Clients need to create buffers that the main device can import and
  // read from, otherwise creating the dmabuf wl_buffer will fail (see the
  // wp_linux_buffer_params.create and create_immed requests for details).
  // The main device will also likely be kept active by the compositor,
  // so clients can use it instead of waking up another device for power
  // savings.
  // 
  // In general the device is a DRM node. The DRM node type (primary vs.
  // render) is unspecified. Clients must not rely on the compositor sending
  // a particular node type. Clients cannot check two devices for equality
  // by comparing the dev_t value.
  // 
  // If explicit modifiers are not supported and the client performs buffer
  // allocations on a different device than the main device, then the client
  // must force the buffer to have a linear layout.
  void MainDevice(Span<> device);

  // A preference tranche has been sent
  //
  // This event splits tranche_target_device and tranche_formats events in
  // preference tranches. It is sent after a set of tranche_target_device
  // and tranche_formats events; it represents the end of a tranche. The
  // next tranche will have a lower preference.
  void TrancheDone();

  // Target device
  //
  // This event advertises the target device that the server prefers to use
  // for a buffer created given this tranche. The advertised target device
  // may be different for each preference tranche, and may change over time.
  // 
  // There is exactly one target device per tranche.
  // 
  // The target device may be a scan-out device, for example if the
  // compositor prefers to directly scan-out a buffer created given this
  // tranche. The target device may be a rendering device, for example if
  // the compositor prefers to texture from said buffer.
  // 
  // The client can use this hint to allocate the buffer in a way that makes
  // it accessible from the target device, ideally directly. The buffer must
  // still be accessible from the main device, either through direct import
  // or through a potentially more expensive fallback path. If the buffer
  // can't be directly imported from the main device then clients must be
  // prepared for the compositor changing the tranche priority or making
  // wl_buffer creation fail (see the wp_linux_buffer_params.create and
  // create_immed requests for details).
  // 
  // If the device is a DRM node, the DRM node type (primary vs. render) is
  // unspecified. Clients must not rely on the compositor sending a
  // particular node type. Clients cannot check two devices for equality by
  // comparing the dev_t value.
  // 
  // This event is tied to a preference tranche, see the tranche_done event.
  void TrancheTargetDevice(Span<> device);

  // Supported buffer format modifier
  //
  // This event advertises the format + modifier combinations that the
  // compositor supports.
  // 
  // It carries an array of indices, each referring to a format + modifier
  // pair in the last received format table (see the format_table event).
  // Each index is a 16-bit unsigned integer in native endianness.
  // 
  // For legacy support, DRM_FORMAT_MOD_INVALID is an allowed modifier.
  // It indicates that the server can support the format with an implicit
  // modifier. When a buffer has DRM_FORMAT_MOD_INVALID as its modifier, it
  // is as if no explicit modifier is specified. The effective modifier
  // will be derived from the dmabuf.
  // 
  // A compositor that sends valid modifiers and DRM_FORMAT_MOD_INVALID for
  // a given format supports both explicit modifiers and implicit modifiers.
  // 
  // Compositors must not send duplicate format + modifier pairs within the
  // same tranche or across two different tranches with the same target
  // device and flags.
  // 
  // This event is tied to a preference tranche, see the tranche_done event.
  // 
  // For the definition of the format and modifier codes, see the
  // wp_linux_buffer_params.create request.
  void TrancheFormats(Span<> indices);

  // Tranche flags
  //
  // This event sets tranche-specific flags.
  // 
  // The scanout flag is a hint that direct scan-out may be attempted by the
  // compositor on the target device if the client appropriately allocates a
  // buffer. How to allocate a buffer that can be scanned out on the target
  // device is implementation-defined.
  // 
  // This event is tied to a preference tranche, see the tranche_done event.
  void TrancheFlags(enum TrancheFlags flags);
  // clang-format on: generated LinuxDmabufFeedbackV1 from linux-dmabuf-v1.xml
};

// clang-format off: generated Viewporter from viewporter.xml

// Surface cropping and scaling
//
// The global interface exposing surface cropping and scaling
// capabilities is used to instantiate an interface extension for a
// wl_surface object. This extended interface will then allow
// cropping and scaling the surface contents, effectively
// disconnecting the direct relationship between the buffer and the
// surface size.
struct Viewporter : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindViewporter;

  static Colony<Viewporter> colony;

  // Do not use directly. Instead use `ColonyMake`
  Viewporter(U32 id, Client& client) : Common(Kind, id, client) {}

  static Viewporter& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorViewportExists = 0, // The surface already has a viewport object associated
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "ViewportExists"sv;
    default: return "UnknownError"sv;
    }
  }

  // Unbind from the cropping and scaling interface
  //
  // Informs the server that the client will not be using this
  // protocol object anymore. This does not affect any other objects,
  // wp_viewport objects included.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Extend surface interface for crop and scale
  //
  // Instantiate an interface extension for the given wl_surface to
  // crop and scale its content. If the given wl_surface already has
  // a wp_viewport object associated, the viewport_exists
  // protocol error is raised.
  void OnGetViewport(Viewport& id, Surface& surface);
  // clang-format on: generated Viewporter from viewporter.xml
};

// clang-format off: generated Viewport from viewporter.xml

// Crop and scale interface to a wl_surface
//
// An additional interface to a wl_surface object, which allows the
// client to specify the cropping and scaling of the surface
// contents.
// 
// This interface works with two concepts: the source rectangle (src_x,
// src_y, src_width, src_height), and the destination size (dst_width,
// dst_height). The contents of the source rectangle are scaled to the
// destination size, and content outside the source rectangle is ignored.
// This state is double-buffered, see wl_surface.commit.
// 
// The two parts of crop and scale state are independent: the source
// rectangle, and the destination size. Initially both are unset, that
// is, no scaling is applied. The whole of the current wl_buffer is
// used as the source, and the surface size is as defined in
// wl_surface.attach.
// 
// If the destination size is set, it causes the surface size to become
// dst_width, dst_height. The source (rectangle) is scaled to exactly
// this size. This overrides whatever the attached wl_buffer size is,
// unless the wl_buffer is NULL. If the wl_buffer is NULL, the surface
// has no content and therefore no size. Otherwise, the size is always
// at least 1x1 in surface local coordinates.
// 
// If the source rectangle is set, it defines what area of the wl_buffer is
// taken as the source. If the source rectangle is set and the destination
// size is not set, then src_width and src_height must be integers, and the
// surface size becomes the source rectangle size. This results in cropping
// without scaling. If src_width or src_height are not integers and
// destination size is not set, the bad_size protocol error is raised when
// the surface state is applied.
// 
// The coordinate transformations from buffer pixel coordinates up to
// the surface-local coordinates happen in the following order:
//   1. buffer_transform (wl_surface.set_buffer_transform)
//   2. buffer_scale (wl_surface.set_buffer_scale)
//   3. crop and scale (wp_viewport.set*)
// This means, that the source rectangle coordinates of crop and scale
// are given in the coordinates after the buffer transform and scale,
// i.e. in the coordinates that would be the surface-local coordinates
// if the crop and scale was not applied.
// 
// If src_x or src_y are negative, the bad_value protocol error is raised.
// Otherwise, if the source rectangle is partially or completely outside of
// the non-NULL wl_buffer, then the out_of_buffer protocol error is raised
// when the surface state is applied. A NULL wl_buffer does not raise the
// out_of_buffer error.
// 
// If the wl_surface associated with the wp_viewport is destroyed,
// all wp_viewport requests except 'destroy' raise the protocol error
// no_surface.
// 
// If the wp_viewport object is destroyed, the crop and scale
// state is removed from the wl_surface. The change will be applied
// on the next wl_surface.commit.
struct Viewport : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindViewport;

  static Colony<Viewport> colony;

  // Do not use directly. Instead use `ColonyMake`
  Viewport(U32 id, Client& client) : Common(Kind, id, client) {}

  static Viewport& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorBadValue = 0, // Negative or zero values in width or height
    ErrorBadSize = 1, // Destination size is not integer
    ErrorOutOfBuffer = 2, // Source rectangle extends outside of the content area
    ErrorNoSurface = 3, // The wl_surface was destroyed
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "BadValue"sv;
    case 1: return "BadSize"sv;
    case 2: return "OutOfBuffer"sv;
    case 3: return "NoSurface"sv;
    default: return "UnknownError"sv;
    }
  }

  // Remove scaling and cropping from the surface
  //
  // The associated wl_surface's crop and scale state is removed.
  // The change is applied on the next wl_surface.commit.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Set the source rectangle for cropping
  //
  // Set the source rectangle of the associated wl_surface. See
  // wp_viewport for the description, and relation to the wl_buffer
  // size.
  // 
  // If all of x, y, width and height are -1.0, the source rectangle is
  // unset instead. Any other set of values where width or height are zero
  // or negative, or x or y are negative, raise the bad_value protocol
  // error.
  // 
  // The crop and scale state is double-buffered, see wl_surface.commit.
  void OnSetSource(float x, float y, float width, float height);

  // Set the surface size for scaling
  //
  // Set the destination size of the associated wl_surface. See
  // wp_viewport for the description, and relation to the wl_buffer
  // size.
  // 
  // If width is -1 and height is -1, the destination size is unset
  // instead. Any other pair of values for width and height that
  // contains zero or negative values raises the bad_value protocol
  // error.
  // 
  // The crop and scale state is double-buffered, see wl_surface.commit.
  void OnSetDestination(I32 width, I32 height);
  // clang-format on: generated Viewport from viewporter.xml

  MortalPtr<Surface> surface;
  float src_x = -1, src_y = -1, src_w = -1, src_h = -1;
  SkISize dst_size = {-1, -1};
};

// clang-format off: generated Display from wayland.xml

// Core global object
//
// The core global object.  This is a special singleton object.  It
// is used for internal Wayland protocol features.
struct Display : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindDisplay;

  static Colony<Display> colony;

  // Do not use directly. Instead use `ColonyMake`
  Display(U32 id, Client& client) : Common(Kind, id, client) {}

  static Display& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Global error values
  //
  // These errors are global and can be emitted in response to any
  // server request.
  enum Error : U32 {
    ErrorInvalidObject = 0, // Server couldn't find object
    ErrorInvalidMethod = 1, // Method doesn't exist on the specified interface or malformed request
    ErrorNoMemory = 2, // Server is out of memory
    ErrorImplementation = 3, // Implementation error in compositor
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "InvalidObject"sv;
    case 1: return "InvalidMethod"sv;
    case 2: return "NoMemory"sv;
    case 3: return "Implementation"sv;
    default: return "UnknownError"sv;
    }
  }

  // Asynchronous roundtrip
  //
  // The sync request asks the server to emit the 'done' event
  // on the returned wl_callback object.  Since requests are
  // handled in-order and events are delivered in-order, this can
  // be used as a barrier to ensure all previous requests and the
  // resulting events have been handled.
  // 
  // The object returned by this request will be destroyed by the
  // compositor after the callback is fired and as such the client must not
  // attempt to use it after that point.
  // 
  // The callback_data passed in the callback is undefined and should be ignored.
  void OnSync(Callback& callback);

  // Get global registry object
  //
  // This request creates a registry object that allows the client
  // to list and bind the global objects available from the
  // compositor.
  // 
  // It should be noted that the server side resources consumed in
  // response to a get_registry request can only be released when the
  // client disconnects, not when the client side proxy is destroyed.
  // Therefore, clients should invoke get_registry as infrequently as
  // possible to avoid wasting memory.
  void OnGetRegistry(Registry& registry);

  // Fatal error event
  //
  // The error event is sent out when a fatal (non-recoverable)
  // error has occurred.  The object_id argument is the object
  // where the error occurred, most often in response to a request
  // to that object.  The code identifies the error and is defined
  // by the object interface.  As such, each interface defines its
  // own set of error codes.  The message is a brief description
  // of the error, for (debugging) convenience.
  void Error(U32 object_id, U32 code, StrView message);

  // Acknowledge object id deletion
  //
  // This event is used internally by the object ID management
  // logic. When a client deletes an object that it had created,
  // the server will send this event to acknowledge that it has
  // seen the delete request. When the client receives this event,
  // it will know that it can safely reuse the object ID.
  void DeleteId(U32 id);
  // clang-format on: generated Display from wayland.xml
};

// clang-format off: generated Registry from wayland.xml

// Global registry object
//
// The singleton global registry object.  The server has a number of
// global objects that are available to all clients.  These objects
// typically represent an actual object in the server (for example,
// an input device) or they are singleton objects that provide
// extension functionality.
// 
// When a client creates a registry object, the registry object
// will emit a global event for each global currently in the
// registry.  Globals come and go as a result of device or
// monitor hotplugs, reconfiguration or other events, and the
// registry will send out global and global_remove events to
// keep the client up to date with the changes.  To mark the end
// of the initial burst of events, the client can use the
// wl_display.sync request immediately after calling
// wl_display.get_registry.
// 
// A client can bind to a global object by using the bind
// request.  This creates a client-side handle that lets the object
// emit events to the client and lets the client invoke requests on
// the object.
struct Registry : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindRegistry;

  static Colony<Registry> colony;

  // Do not use directly. Instead use `ColonyMake`
  Registry(U32 id, Client& client) : Common(Kind, id, client) {}

  static Registry& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Bind an object to the display
  //
  // Binds a new, client-created object to the server using the
  // specified name as the identifier.
  void OnBind(U32 name, StrView id_interface, U32 id_version, U32 id);

  // Announce global object
  //
  // Notify the client of global objects.
  // 
  // The event notifies the client that a global object with
  // the given name is now available, and it implements the
  // given version of the given interface.
  void Global(U32 name, StrView interface, U32 version);

  // Announce removal of global object
  //
  // Notify the client of removed global objects.
  // 
  // This event notifies the client that the global identified
  // by name is no longer available.  If the client bound to
  // the global using the bind request, the client should now
  // destroy that object.
  // 
  // The object remains valid and requests to the object will be
  // ignored until the client destroys it, to avoid races between
  // the global going away and a client sending a request to it.
  void GlobalRemove(U32 name);
  // clang-format on: generated Registry from wayland.xml
};

// clang-format off: generated Callback from wayland.xml

// Callback object
//
// Clients can handle the 'done' event to get notified when
// the related request is done.
// 
// Note, because wl_callback objects are created from multiple independent
// factory interfaces, the wl_callback interface is frozen at version 1.
struct Callback : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindCallback;

  static Colony<Callback> colony;

  // Do not use directly. Instead use `ColonyMake`
  Callback(U32 id, Client& client) : Common(Kind, id, client) {}

  static Callback& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Done event
  //
  // Notify the client when the related request is done.
  void Done(U32 callback_data);
  // clang-format on: generated Callback from wayland.xml
};

// clang-format off: generated Compositor from wayland.xml

// The compositor singleton
//
// A compositor.  This object is a singleton global.  The
// compositor is in charge of combining the contents of multiple
// surfaces into one displayable output.
struct Compositor : Common {
  static constexpr int Version = 6;
  static constexpr Kind Kind = KindCompositor;

  static Colony<Compositor> colony;

  // Do not use directly. Instead use `ColonyMake`
  Compositor(U32 id, Client& client) : Common(Kind, id, client) {}

  static Compositor& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Create new surface
  //
  // sk the compositor to create a new surface.
  void OnCreateSurface(Surface& id);

  // Create new region
  //
  // sk the compositor to create a new region.
  void OnCreateRegion(Region& id);
  // clang-format on: generated Compositor from wayland.xml
};

// clang-format off: generated Shm from wayland.xml

// Shared memory support
//
// A singleton global object that provides support for shared
// memory.
// 
// Clients can create wl_shm_pool objects using the create_pool
// request.
// 
// On binding the wl_shm object one or more format events
// are emitted to inform clients about the valid pixel formats
// that can be used for buffers.
struct Shm : Common {
  static constexpr int Version = 2;
  static constexpr Kind Kind = KindShm;

  static Colony<Shm> colony;

  // Do not use directly. Instead use `ColonyMake`
  Shm(U32 id, Client& client) : Common(Kind, id, client) {}

  static Shm& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Wl_shm error values
  //
  // These errors can be emitted in response to wl_shm requests.
  enum Error : U32 {
    ErrorInvalidFormat = 0, // Buffer format is not known
    ErrorInvalidStride = 1, // Invalid size or stride during pool or buffer creation
    ErrorInvalidFd = 2, // Mmapping the file descriptor failed
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "InvalidFormat"sv;
    case 1: return "InvalidStride"sv;
    case 2: return "InvalidFd"sv;
    default: return "UnknownError"sv;
    }
  }

  // Pixel formats
  //
  // This describes the memory layout of an individual pixel.
  // 
  // All renderers should support argb8888 and xrgb8888 but any other
  // formats are optional and may not be supported by the particular
  // renderer in use.
  // 
  // The drm format codes match the macros defined in drm_fourcc.h, except
  // argb8888 and xrgb8888. The formats actually supported by the compositor
  // will be reported by the format event.
  // 
  // For all wl_shm formats and unless specified in another protocol
  // extension, pre-multiplied alpha is used for pixel values.
  enum Format : U32 {
    FormatArgb8888 = 0, // 32-bit argb format, [31:0] a:r:g:b 8:8:8:8 little endian
    FormatXrgb8888 = 1, // 32-bit rgb format, [31:0] x:r:g:b 8:8:8:8 little endian
    FormatC8 = 0x20203843, // 8-bit color index format, [7:0] c
    FormatRgb332 = 0x38424752, // 8-bit rgb format, [7:0] r:g:b 3:3:2
    FormatBgr233 = 0x38524742, // 8-bit bgr format, [7:0] b:g:r 2:3:3
    FormatXrgb4444 = 0x32315258, // 16-bit xrgb format, [15:0] x:r:g:b 4:4:4:4 little endian
    FormatXbgr4444 = 0x32314258, // 16-bit xbgr format, [15:0] x:b:g:r 4:4:4:4 little endian
    FormatRgbx4444 = 0x32315852, // 16-bit rgbx format, [15:0] r:g:b:x 4:4:4:4 little endian
    FormatBgrx4444 = 0x32315842, // 16-bit bgrx format, [15:0] b:g:r:x 4:4:4:4 little endian
    FormatArgb4444 = 0x32315241, // 16-bit argb format, [15:0] a:r:g:b 4:4:4:4 little endian
    FormatAbgr4444 = 0x32314241, // 16-bit abgr format, [15:0] a:b:g:r 4:4:4:4 little endian
    FormatRgba4444 = 0x32314152, // 16-bit rbga format, [15:0] r:g:b:a 4:4:4:4 little endian
    FormatBgra4444 = 0x32314142, // 16-bit bgra format, [15:0] b:g:r:a 4:4:4:4 little endian
    FormatXrgb1555 = 0x35315258, // 16-bit xrgb format, [15:0] x:r:g:b 1:5:5:5 little endian
    FormatXbgr1555 = 0x35314258, // 16-bit xbgr 1555 format, [15:0] x:b:g:r 1:5:5:5 little endian
    FormatRgbx5551 = 0x35315852, // 16-bit rgbx 5551 format, [15:0] r:g:b:x 5:5:5:1 little endian
    FormatBgrx5551 = 0x35315842, // 16-bit bgrx 5551 format, [15:0] b:g:r:x 5:5:5:1 little endian
    FormatArgb1555 = 0x35315241, // 16-bit argb 1555 format, [15:0] a:r:g:b 1:5:5:5 little endian
    FormatAbgr1555 = 0x35314241, // 16-bit abgr 1555 format, [15:0] a:b:g:r 1:5:5:5 little endian
    FormatRgba5551 = 0x35314152, // 16-bit rgba 5551 format, [15:0] r:g:b:a 5:5:5:1 little endian
    FormatBgra5551 = 0x35314142, // 16-bit bgra 5551 format, [15:0] b:g:r:a 5:5:5:1 little endian
    FormatRgb565 = 0x36314752, // 16-bit rgb 565 format, [15:0] r:g:b 5:6:5 little endian
    FormatBgr565 = 0x36314742, // 16-bit bgr 565 format, [15:0] b:g:r 5:6:5 little endian
    FormatRgb888 = 0x34324752, // 24-bit rgb format, [23:0] r:g:b little endian
    FormatBgr888 = 0x34324742, // 24-bit bgr format, [23:0] b:g:r little endian
    FormatXbgr8888 = 0x34324258, // 32-bit xbgr format, [31:0] x:b:g:r 8:8:8:8 little endian
    FormatRgbx8888 = 0x34325852, // 32-bit rgbx format, [31:0] r:g:b:x 8:8:8:8 little endian
    FormatBgrx8888 = 0x34325842, // 32-bit bgrx format, [31:0] b:g:r:x 8:8:8:8 little endian
    FormatAbgr8888 = 0x34324241, // 32-bit abgr format, [31:0] a:b:g:r 8:8:8:8 little endian
    FormatRgba8888 = 0x34324152, // 32-bit rgba format, [31:0] r:g:b:a 8:8:8:8 little endian
    FormatBgra8888 = 0x34324142, // 32-bit bgra format, [31:0] b:g:r:a 8:8:8:8 little endian
    FormatXrgb2101010 = 0x30335258, // 32-bit xrgb format, [31:0] x:r:g:b 2:10:10:10 little endian
    FormatXbgr2101010 = 0x30334258, // 32-bit xbgr format, [31:0] x:b:g:r 2:10:10:10 little endian
    FormatRgbx1010102 = 0x30335852, // 32-bit rgbx format, [31:0] r:g:b:x 10:10:10:2 little endian
    FormatBgrx1010102 = 0x30335842, // 32-bit bgrx format, [31:0] b:g:r:x 10:10:10:2 little endian
    FormatArgb2101010 = 0x30335241, // 32-bit argb format, [31:0] a:r:g:b 2:10:10:10 little endian
    FormatAbgr2101010 = 0x30334241, // 32-bit abgr format, [31:0] a:b:g:r 2:10:10:10 little endian
    FormatRgba1010102 = 0x30334152, // 32-bit rgba format, [31:0] r:g:b:a 10:10:10:2 little endian
    FormatBgra1010102 = 0x30334142, // 32-bit bgra format, [31:0] b:g:r:a 10:10:10:2 little endian
    FormatYuyv = 0x56595559, // Packed ycbcr format, [31:0] cr0:y1:cb0:y0 8:8:8:8 little endian
    FormatYvyu = 0x55595659, // Packed ycbcr format, [31:0] cb0:y1:cr0:y0 8:8:8:8 little endian
    FormatUyvy = 0x59565955, // Packed ycbcr format, [31:0] y1:cr0:y0:cb0 8:8:8:8 little endian
    FormatVyuy = 0x59555956, // Packed ycbcr format, [31:0] y1:cb0:y0:cr0 8:8:8:8 little endian
    FormatAyuv = 0x56555941, // Packed aycbcr format, [31:0] a:y:cb:cr 8:8:8:8 little endian
    FormatNv12 = 0x3231564e, // 2 plane ycbcr cr:cb format, 2x2 subsampled cr:cb plane
    FormatNv21 = 0x3132564e, // 2 plane ycbcr cb:cr format, 2x2 subsampled cb:cr plane
    FormatNv16 = 0x3631564e, // 2 plane ycbcr cr:cb format, 2x1 subsampled cr:cb plane
    FormatNv61 = 0x3136564e, // 2 plane ycbcr cb:cr format, 2x1 subsampled cb:cr plane
    FormatYuv410 = 0x39565559, // 3 plane ycbcr format, 4x4 subsampled cb (1) and cr (2) planes
    FormatYvu410 = 0x39555659, // 3 plane ycbcr format, 4x4 subsampled cr (1) and cb (2) planes
    FormatYuv411 = 0x31315559, // 3 plane ycbcr format, 4x1 subsampled cb (1) and cr (2) planes
    FormatYvu411 = 0x31315659, // 3 plane ycbcr format, 4x1 subsampled cr (1) and cb (2) planes
    FormatYuv420 = 0x32315559, // 3 plane ycbcr format, 2x2 subsampled cb (1) and cr (2) planes
    FormatYvu420 = 0x32315659, // 3 plane ycbcr format, 2x2 subsampled cr (1) and cb (2) planes
    FormatYuv422 = 0x36315559, // 3 plane ycbcr format, 2x1 subsampled cb (1) and cr (2) planes
    FormatYvu422 = 0x36315659, // 3 plane ycbcr format, 2x1 subsampled cr (1) and cb (2) planes
    FormatYuv444 = 0x34325559, // 3 plane ycbcr format, non-subsampled cb (1) and cr (2) planes
    FormatYvu444 = 0x34325659, // 3 plane ycbcr format, non-subsampled cr (1) and cb (2) planes
    FormatR8 = 0x20203852, // [7:0] r
    FormatR16 = 0x20363152, // [15:0] r little endian
    FormatRg88 = 0x38384752, // [15:0] r:g 8:8 little endian
    FormatGr88 = 0x38385247, // [15:0] g:r 8:8 little endian
    FormatRg1616 = 0x32334752, // [31:0] r:g 16:16 little endian
    FormatGr1616 = 0x32335247, // [31:0] g:r 16:16 little endian
    FormatXrgb16161616f = 0x48345258, // [63:0] x:r:g:b 16:16:16:16 little endian
    FormatXbgr16161616f = 0x48344258, // [63:0] x:b:g:r 16:16:16:16 little endian
    FormatArgb16161616f = 0x48345241, // [63:0] a:r:g:b 16:16:16:16 little endian
    FormatAbgr16161616f = 0x48344241, // [63:0] a:b:g:r 16:16:16:16 little endian
    FormatXyuv8888 = 0x56555958, // [31:0] x:y:cb:cr 8:8:8:8 little endian
    FormatVuy888 = 0x34325556, // [23:0] cr:cb:y 8:8:8 little endian
    FormatVuy101010 = 0x30335556, // Y followed by u then v, 10:10:10. non-linear modifier only
    FormatY210 = 0x30313259, // [63:0] cr0:0:y1:0:cb0:0:y0:0 10:6:10:6:10:6:10:6 little endian per 2 y pixels
    FormatY212 = 0x32313259, // [63:0] cr0:0:y1:0:cb0:0:y0:0 12:4:12:4:12:4:12:4 little endian per 2 y pixels
    FormatY216 = 0x36313259, // [63:0] cr0:y1:cb0:y0 16:16:16:16 little endian per 2 y pixels
    FormatY410 = 0x30313459, // [31:0] a:cr:y:cb 2:10:10:10 little endian
    FormatY412 = 0x32313459, // [63:0] a:0:cr:0:y:0:cb:0 12:4:12:4:12:4:12:4 little endian
    FormatY416 = 0x36313459, // [63:0] a:cr:y:cb 16:16:16:16 little endian
    FormatXvyu2101010 = 0x30335658, // [31:0] x:cr:y:cb 2:10:10:10 little endian
    FormatXvyu1216161616 = 0x36335658, // [63:0] x:0:cr:0:y:0:cb:0 12:4:12:4:12:4:12:4 little endian
    FormatXvyu16161616 = 0x38345658, // [63:0] x:cr:y:cb 16:16:16:16 little endian
    FormatY0l0 = 0x304c3059, // [63:0]   a3:a2:y3:0:cr0:0:y2:0:a1:a0:y1:0:cb0:0:y0:0  1:1:8:2:8:2:8:2:1:1:8:2:8:2:8:2 little endian
    FormatX0l0 = 0x304c3058, // [63:0]   x3:x2:y3:0:cr0:0:y2:0:x1:x0:y1:0:cb0:0:y0:0  1:1:8:2:8:2:8:2:1:1:8:2:8:2:8:2 little endian
    FormatY0l2 = 0x324c3059, // [63:0]   a3:a2:y3:cr0:y2:a1:a0:y1:cb0:y0  1:1:10:10:10:1:1:10:10:10 little endian
    FormatX0l2 = 0x324c3058, // [63:0]   x3:x2:y3:cr0:y2:x1:x0:y1:cb0:y0  1:1:10:10:10:1:1:10:10:10 little endian
    FormatYuv4208bit = 0x38305559,
    FormatYuv42010bit = 0x30315559,
    FormatXrgb8888A8 = 0x38415258,
    FormatXbgr8888A8 = 0x38414258,
    FormatRgbx8888A8 = 0x38415852,
    FormatBgrx8888A8 = 0x38415842,
    FormatRgb888A8 = 0x38413852,
    FormatBgr888A8 = 0x38413842,
    FormatRgb565A8 = 0x38413552,
    FormatBgr565A8 = 0x38413542,
    FormatNv24 = 0x3432564e, // Non-subsampled cr:cb plane
    FormatNv42 = 0x3234564e, // Non-subsampled cb:cr plane
    FormatP210 = 0x30313250, // 2x1 subsampled cr:cb plane, 10 bit per channel
    FormatP010 = 0x30313050, // 2x2 subsampled cr:cb plane 10 bits per channel
    FormatP012 = 0x32313050, // 2x2 subsampled cr:cb plane 12 bits per channel
    FormatP016 = 0x36313050, // 2x2 subsampled cr:cb plane 16 bits per channel
    FormatAxbxgxrx106106106106 = 0x30314241, // [63:0] a:x:b:x:g:x:r:x 10:6:10:6:10:6:10:6 little endian
    FormatNv15 = 0x3531564e, // 2x2 subsampled cr:cb plane
    FormatQ410 = 0x30313451,
    FormatQ401 = 0x31303451,
    FormatXrgb16161616 = 0x38345258, // [63:0] x:r:g:b 16:16:16:16 little endian
    FormatXbgr16161616 = 0x38344258, // [63:0] x:b:g:r 16:16:16:16 little endian
    FormatArgb16161616 = 0x38345241, // [63:0] a:r:g:b 16:16:16:16 little endian
    FormatAbgr16161616 = 0x38344241, // [63:0] a:b:g:r 16:16:16:16 little endian
    FormatC1 = 0x20203143, // [7:0] c0:c1:c2:c3:c4:c5:c6:c7 1:1:1:1:1:1:1:1 eight pixels/byte
    FormatC2 = 0x20203243, // [7:0] c0:c1:c2:c3 2:2:2:2 four pixels/byte
    FormatC4 = 0x20203443, // [7:0] c0:c1 4:4 two pixels/byte
    FormatD1 = 0x20203144, // [7:0] d0:d1:d2:d3:d4:d5:d6:d7 1:1:1:1:1:1:1:1 eight pixels/byte
    FormatD2 = 0x20203244, // [7:0] d0:d1:d2:d3 2:2:2:2 four pixels/byte
    FormatD4 = 0x20203444, // [7:0] d0:d1 4:4 two pixels/byte
    FormatD8 = 0x20203844, // [7:0] d
    FormatR1 = 0x20203152, // [7:0] r0:r1:r2:r3:r4:r5:r6:r7 1:1:1:1:1:1:1:1 eight pixels/byte
    FormatR2 = 0x20203252, // [7:0] r0:r1:r2:r3 2:2:2:2 four pixels/byte
    FormatR4 = 0x20203452, // [7:0] r0:r1 4:4 two pixels/byte
    FormatR10 = 0x20303152, // [15:0] x:r 6:10 little endian
    FormatR12 = 0x20323152, // [15:0] x:r 4:12 little endian
    FormatAvuy8888 = 0x59555641, // [31:0] a:cr:cb:y 8:8:8:8 little endian
    FormatXvuy8888 = 0x59555658, // [31:0] x:cr:cb:y 8:8:8:8 little endian
    FormatP030 = 0x30333050, // 2x2 subsampled cr:cb plane 10 bits per channel packed
  };

  static StrView FormatToStr(U32 value) {
    switch (value) {
    case 0: return "Argb8888"sv;
    case 1: return "Xrgb8888"sv;
    case 0x20203843: return "C8"sv;
    case 0x38424752: return "Rgb332"sv;
    case 0x38524742: return "Bgr233"sv;
    case 0x32315258: return "Xrgb4444"sv;
    case 0x32314258: return "Xbgr4444"sv;
    case 0x32315852: return "Rgbx4444"sv;
    case 0x32315842: return "Bgrx4444"sv;
    case 0x32315241: return "Argb4444"sv;
    case 0x32314241: return "Abgr4444"sv;
    case 0x32314152: return "Rgba4444"sv;
    case 0x32314142: return "Bgra4444"sv;
    case 0x35315258: return "Xrgb1555"sv;
    case 0x35314258: return "Xbgr1555"sv;
    case 0x35315852: return "Rgbx5551"sv;
    case 0x35315842: return "Bgrx5551"sv;
    case 0x35315241: return "Argb1555"sv;
    case 0x35314241: return "Abgr1555"sv;
    case 0x35314152: return "Rgba5551"sv;
    case 0x35314142: return "Bgra5551"sv;
    case 0x36314752: return "Rgb565"sv;
    case 0x36314742: return "Bgr565"sv;
    case 0x34324752: return "Rgb888"sv;
    case 0x34324742: return "Bgr888"sv;
    case 0x34324258: return "Xbgr8888"sv;
    case 0x34325852: return "Rgbx8888"sv;
    case 0x34325842: return "Bgrx8888"sv;
    case 0x34324241: return "Abgr8888"sv;
    case 0x34324152: return "Rgba8888"sv;
    case 0x34324142: return "Bgra8888"sv;
    case 0x30335258: return "Xrgb2101010"sv;
    case 0x30334258: return "Xbgr2101010"sv;
    case 0x30335852: return "Rgbx1010102"sv;
    case 0x30335842: return "Bgrx1010102"sv;
    case 0x30335241: return "Argb2101010"sv;
    case 0x30334241: return "Abgr2101010"sv;
    case 0x30334152: return "Rgba1010102"sv;
    case 0x30334142: return "Bgra1010102"sv;
    case 0x56595559: return "Yuyv"sv;
    case 0x55595659: return "Yvyu"sv;
    case 0x59565955: return "Uyvy"sv;
    case 0x59555956: return "Vyuy"sv;
    case 0x56555941: return "Ayuv"sv;
    case 0x3231564e: return "Nv12"sv;
    case 0x3132564e: return "Nv21"sv;
    case 0x3631564e: return "Nv16"sv;
    case 0x3136564e: return "Nv61"sv;
    case 0x39565559: return "Yuv410"sv;
    case 0x39555659: return "Yvu410"sv;
    case 0x31315559: return "Yuv411"sv;
    case 0x31315659: return "Yvu411"sv;
    case 0x32315559: return "Yuv420"sv;
    case 0x32315659: return "Yvu420"sv;
    case 0x36315559: return "Yuv422"sv;
    case 0x36315659: return "Yvu422"sv;
    case 0x34325559: return "Yuv444"sv;
    case 0x34325659: return "Yvu444"sv;
    case 0x20203852: return "R8"sv;
    case 0x20363152: return "R16"sv;
    case 0x38384752: return "Rg88"sv;
    case 0x38385247: return "Gr88"sv;
    case 0x32334752: return "Rg1616"sv;
    case 0x32335247: return "Gr1616"sv;
    case 0x48345258: return "Xrgb16161616f"sv;
    case 0x48344258: return "Xbgr16161616f"sv;
    case 0x48345241: return "Argb16161616f"sv;
    case 0x48344241: return "Abgr16161616f"sv;
    case 0x56555958: return "Xyuv8888"sv;
    case 0x34325556: return "Vuy888"sv;
    case 0x30335556: return "Vuy101010"sv;
    case 0x30313259: return "Y210"sv;
    case 0x32313259: return "Y212"sv;
    case 0x36313259: return "Y216"sv;
    case 0x30313459: return "Y410"sv;
    case 0x32313459: return "Y412"sv;
    case 0x36313459: return "Y416"sv;
    case 0x30335658: return "Xvyu2101010"sv;
    case 0x36335658: return "Xvyu1216161616"sv;
    case 0x38345658: return "Xvyu16161616"sv;
    case 0x304c3059: return "Y0l0"sv;
    case 0x304c3058: return "X0l0"sv;
    case 0x324c3059: return "Y0l2"sv;
    case 0x324c3058: return "X0l2"sv;
    case 0x38305559: return "Yuv4208bit"sv;
    case 0x30315559: return "Yuv42010bit"sv;
    case 0x38415258: return "Xrgb8888A8"sv;
    case 0x38414258: return "Xbgr8888A8"sv;
    case 0x38415852: return "Rgbx8888A8"sv;
    case 0x38415842: return "Bgrx8888A8"sv;
    case 0x38413852: return "Rgb888A8"sv;
    case 0x38413842: return "Bgr888A8"sv;
    case 0x38413552: return "Rgb565A8"sv;
    case 0x38413542: return "Bgr565A8"sv;
    case 0x3432564e: return "Nv24"sv;
    case 0x3234564e: return "Nv42"sv;
    case 0x30313250: return "P210"sv;
    case 0x30313050: return "P010"sv;
    case 0x32313050: return "P012"sv;
    case 0x36313050: return "P016"sv;
    case 0x30314241: return "Axbxgxrx106106106106"sv;
    case 0x3531564e: return "Nv15"sv;
    case 0x30313451: return "Q410"sv;
    case 0x31303451: return "Q401"sv;
    case 0x38345258: return "Xrgb16161616"sv;
    case 0x38344258: return "Xbgr16161616"sv;
    case 0x38345241: return "Argb16161616"sv;
    case 0x38344241: return "Abgr16161616"sv;
    case 0x20203143: return "C1"sv;
    case 0x20203243: return "C2"sv;
    case 0x20203443: return "C4"sv;
    case 0x20203144: return "D1"sv;
    case 0x20203244: return "D2"sv;
    case 0x20203444: return "D4"sv;
    case 0x20203844: return "D8"sv;
    case 0x20203152: return "R1"sv;
    case 0x20203252: return "R2"sv;
    case 0x20203452: return "R4"sv;
    case 0x20303152: return "R10"sv;
    case 0x20323152: return "R12"sv;
    case 0x59555641: return "Avuy8888"sv;
    case 0x59555658: return "Xvuy8888"sv;
    case 0x30333050: return "P030"sv;
    default: return "UnknownFormat"sv;
    }
  }

  // Create a shm pool
  //
  // Create a new wl_shm_pool object.
  // 
  // The pool can be used to create shared memory based buffer
  // objects.  The server will mmap size bytes of the passed file
  // descriptor, to use as backing memory for the pool.
  void OnCreatePool(ShmPool& id, FD&& fd, I32 size);

  // Release the shm object
  //
  // Using this request a client can tell the server that it is not going to
  // use the shm object anymore.
  // 
  // Objects created via this interface remain unaffected.
  //
  // [destructor] After this method returns, this object will be released
  void OnRelease();

  // Pixel format description
  //
  // Informs the client about a valid pixel format that
  // can be used for buffers. Known formats include
  // argb8888 and xrgb8888.
  void Format(enum Format format);
  // clang-format on: generated Shm from wayland.xml
};

// clang-format off: generated Buffer from wayland.xml

// Content for a wl_surface
//
// A buffer provides the content for a wl_surface. Buffers are
// created through factory interfaces such as wl_shm, wp_linux_buffer_params
// (from the linux-dmabuf protocol extension) or similar. It has a width and
// a height and can be attached to a wl_surface, but the mechanism by which a
// client provides and updates the contents is defined by the buffer factory
// interface.
// 
// Color channels are assumed to be electrical rather than optical (in other
// words, encoded with a transfer function) unless otherwise specified. If
// the buffer uses a format that has an alpha channel, the alpha channel is
// assumed to be premultiplied into the electrical color channel values
// (after transfer function encoding) unless otherwise specified.
// 
// Note, because wl_buffer objects are created from multiple independent
// factory interfaces, the wl_buffer interface is frozen at version 1.
struct Buffer : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindBuffer;

  static Colony<Buffer> colony;

  // Do not use directly. Instead use `ColonyMake`
  Buffer(U32 id, Client& client) : Common(Kind, id, client) {}

  static Buffer& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Destroy a buffer
  //
  // Destroy a buffer. If and how you need to release the backing
  // storage is defined by the buffer factory interface.
  // 
  // For possible side-effects to a surface, see wl_surface.attach.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Compositor releases buffer
  //
  // Sent when this wl_buffer is no longer used by the compositor.
  // 
  // For more information on when release events may or may not be sent,
  // and what consequences it has, please see the description of
  // wl_surface.attach.
  // 
  // If a client receives a release event before the frame callback
  // requested in the same wl_surface.commit that attaches this
  // wl_buffer to a surface, then the client is immediately free to
  // reuse the buffer and its backing storage, and does not need a
  // second buffer for the next surface content update. Typically
  // this is possible, when the compositor maintains a copy of the
  // wl_surface contents, e.g. as a GL texture. This is an important
  // optimization for GL(ES) compositors with wl_shm clients.
  void Release();
  // clang-format on: generated Buffer from wayland.xml

  void* data = nullptr;  // shm: read-only mmap of the pool
  size_t map_size = 0;
  int32_t offset = 0, width = 0, height = 0, stride = 0;
  U32 format = 0;  // shm: wl_shm format
  DmabufPlane dmabuf_planes[4];
  int dmabuf_plane_count = 0;
  U32 drm_format = 0;  // dmabuf: DRM fourcc
  U64 dmabuf_modifier = 0;
  bool y_invert = false;
  ~Buffer();
};

// clang-format off: generated DataDevice from wayland.xml

// Data transfer device
//
// There is one wl_data_device per seat which can be obtained
// from the global wl_data_device_manager singleton.
// 
// A wl_data_device provides access to inter-client data transfer
// mechanisms such as copy-and-paste and drag-and-drop.
struct DataDevice : Common {
  static constexpr int Version = 3;
  static constexpr Kind Kind = KindDataDevice;

  static Colony<DataDevice> colony;

  // Do not use directly. Instead use `ColonyMake`
  DataDevice(U32 id, Client& client) : Common(Kind, id, client) {}

  static DataDevice& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorRole = 0, // Given wl_surface has another role
    ErrorUsedSource = 1, // Source has already been used
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "Role"sv;
    case 1: return "UsedSource"sv;
    default: return "UnknownError"sv;
    }
  }

  // Start drag-and-drop operation
  //
  // This request asks the compositor to start a drag-and-drop
  // operation on behalf of the client.
  // 
  // The source argument is the data source that provides the data
  // for the eventual data transfer. If source is NULL, enter, leave
  // and motion events are sent only to the client that initiated the
  // drag and the client is expected to handle the data passing
  // internally. If source is destroyed, the drag-and-drop session will be
  // cancelled.
  // 
  // The origin surface is the surface where the drag originates and
  // the client must have an active implicit grab that matches the
  // serial.
  // 
  // The icon surface is an optional (can be NULL) surface that
  // provides an icon to be moved around with the cursor.  Initially,
  // the top-left corner of the icon surface is placed at the cursor
  // hotspot, but subsequent wl_surface.offset requests can move the
  // relative position. Attach requests must be confirmed with
  // wl_surface.commit as usual. The icon surface is given the role of
  // a drag-and-drop icon. If the icon surface already has another role,
  // it raises a protocol error.
  // 
  // The input region is ignored for wl_surfaces with the role of a
  // drag-and-drop icon.
  // 
  // The given source may not be used in any further set_selection or
  // start_drag requests. Attempting to reuse a previously-used source
  // may send a used_source error.
  void OnStartDrag(DataSource* source, Surface& origin, Surface* icon, U32 serial);

  // Copy data to the selection
  //
  // This request asks the compositor to set the selection
  // to the data from the source on behalf of the client.
  // 
  // To unset the selection, set the source to NULL.
  // 
  // The given source may not be used in any further set_selection or
  // start_drag requests. Attempting to reuse a previously-used source
  // may send a used_source error.
  void OnSetSelection(DataSource* source, U32 serial);

  // Destroy data device
  //
  // This request destroys the data device.
  //
  // [destructor] After this method returns, this object will be released
  void OnRelease();

  // Introduce a new wl_data_offer
  //
  // The data_offer event introduces a new wl_data_offer object,
  // which will subsequently be used in either the
  // data_device.enter event (for drag-and-drop) or the
  // data_device.selection event (for selections).  Immediately
  // following the data_device.data_offer event, the new data_offer
  // object will send out data_offer.offer events to describe the
  // mime types it offers.
  void DataOffer(struct DataOffer& id);

  // Initiate drag-and-drop session
  //
  // This event is sent when an active drag-and-drop pointer enters
  // a surface owned by the client.  The position of the pointer at
  // enter time is provided by the x and y arguments, in surface-local
  // coordinates.
  void Enter(U32 serial, Surface& surface, float x, float y, struct DataOffer* id);

  // End drag-and-drop session
  //
  // This event is sent when the drag-and-drop pointer leaves the
  // surface and the session ends.  The client must destroy the
  // wl_data_offer introduced at enter time at this point.
  void Leave();

  // Drag-and-drop session motion
  //
  // This event is sent when the drag-and-drop pointer moves within
  // the currently focused surface. The new position of the pointer
  // is provided by the x and y arguments, in surface-local
  // coordinates.
  void Motion(U32 time, float x, float y);

  // End drag-and-drop session successfully
  //
  // The event is sent when a drag-and-drop operation is ended
  // because the implicit grab is removed.
  // 
  // The drag-and-drop destination is expected to honor the last action
  // received through wl_data_offer.action, if the resulting action is
  // "copy" or "move", the destination can still perform
  // wl_data_offer.receive requests, and is expected to end all
  // transfers with a wl_data_offer.finish request.
  // 
  // If the resulting action is "ask", the action will not be considered
  // final. The drag-and-drop destination is expected to perform one last
  // wl_data_offer.set_actions request, or wl_data_offer.destroy in order
  // to cancel the operation.
  void Drop();

  // Advertise new selection
  //
  // The selection event is sent out to notify the client of a new
  // wl_data_offer for the selection for this device.  The
  // data_device.data_offer and the data_offer.offer events are
  // sent out immediately before this event to introduce the data
  // offer object.  The selection event is sent to a client
  // immediately before receiving keyboard focus and when a new
  // selection is set while the client has keyboard focus.  The
  // data_offer is valid until a new data_offer or NULL is received
  // or until the client loses keyboard focus.  Switching surface with
  // keyboard focus within the same client doesn't mean a new selection
  // will be sent.  The client must destroy the previous selection
  // data_offer, if any, upon receiving this event.
  void Selection(struct DataOffer* id);
  // clang-format on: generated DataDevice from wayland.xml
};

// clang-format off: generated DataDeviceManager from wayland.xml

// Data transfer interface
//
// The wl_data_device_manager is a singleton global object that
// provides access to inter-client data transfer mechanisms such as
// copy-and-paste and drag-and-drop.  These mechanisms are tied to
// a wl_seat and this interface lets a client get a wl_data_device
// corresponding to a wl_seat.
// 
// Depending on the version bound, the objects created from the bound
// wl_data_device_manager object will have different requirements for
// functioning properly. See wl_data_source.set_actions,
// wl_data_offer.accept and wl_data_offer.finish for details.
struct DataDeviceManager : Common {
  static constexpr int Version = 3;
  static constexpr Kind Kind = KindDataDeviceManager;

  static Colony<DataDeviceManager> colony;

  // Do not use directly. Instead use `ColonyMake`
  DataDeviceManager(U32 id, Client& client) : Common(Kind, id, client) {}

  static DataDeviceManager& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Drag and drop actions
  //
  // This is a bitmask of the available/preferred actions in a
  // drag-and-drop operation.
  // 
  // In the compositor, the selected action is a result of matching the
  // actions offered by the source and destination sides.  "action" events
  // with a "none" action will be sent to both source and destination if
  // there is no match. All further checks will effectively happen on
  // (source actions ∩ destination actions).
  // 
  // In addition, compositors may also pick different actions in
  // reaction to key modifiers being pressed. One common design that
  // is used in major toolkits (and the behavior recommended for
  // compositors) is:
  // 
  // - If no modifiers are pressed, the first match (in bit order)
  //   will be used.
  // - Pressing Shift selects "move", if enabled in the mask.
  // - Pressing Control selects "copy", if enabled in the mask.
  // 
  // Behavior beyond that is considered implementation-dependent.
  // Compositors may for example bind other modifiers (like Alt/Meta)
  // or drags initiated with other buttons than BTN_LEFT to specific
  // actions (e.g. "ask").
  enum DndAction : U32 {
    DndActionNone = 0, // No action
    DndActionCopy = 1, // Copy action
    DndActionMove = 2, // Move action
    DndActionAsk = 4, // Ask action
  };

  static StrView DndActionToStr(U32 value) {
    switch (value) {
    case 0: return "None"sv;
    case 1: return "Copy"sv;
    case 2: return "Move"sv;
    case 4: return "Ask"sv;
    default: return "UnknownDndAction"sv;
    }
  }

  // Create a new data source
  //
  // reate a new data source.
  void OnCreateDataSource(DataSource& id);

  // Create a new data device
  //
  // reate a new data device for a given seat.
  void OnGetDataDevice(DataDevice& id, Seat& seat);
  // clang-format on: generated DataDeviceManager from wayland.xml
};

// clang-format off: generated Shell from wayland.xml

// Create desktop-style surfaces
//
// This interface is implemented by servers that provide
// desktop-style user interfaces.
// 
// It allows clients to associate a wl_shell_surface with
// a basic surface.
// 
// Note! This protocol is deprecated and not intended for production use.
// For desktop-style user interfaces, use xdg_shell. Compositors and clients
// should not implement this interface.
struct Shell : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindShell;

  static Colony<Shell> colony;

  // Do not use directly. Instead use `ColonyMake`
  Shell(U32 id, Client& client) : Common(Kind, id, client) {}

  static Shell& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorRole = 0, // Given wl_surface has another role
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "Role"sv;
    default: return "UnknownError"sv;
    }
  }

  // Create a shell surface from a surface
  //
  // Create a shell surface for an existing surface. This gives
  // the wl_surface the role of a shell surface. If the wl_surface
  // already has another role, it raises a protocol error.
  // 
  // Only one shell surface can be associated with a given surface.
  void OnGetShellSurface(ShellSurface& id, Surface& surface);
  // clang-format on: generated Shell from wayland.xml
};

// clang-format off: generated ShellSurface from wayland.xml

// Desktop-style metadata interface
//
// An interface that may be implemented by a wl_surface, for
// implementations that provide a desktop-style user interface.
// 
// It provides requests to treat surfaces like toplevel, fullscreen
// or popup windows, move, resize or maximize them, associate
// metadata like title and class, etc.
// 
// On the server side the object is automatically destroyed when
// the related wl_surface is destroyed. On the client side,
// wl_shell_surface_destroy() must be called before destroying
// the wl_surface object.
struct ShellSurface : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindShellSurface;

  static Colony<ShellSurface> colony;

  // Do not use directly. Instead use `ColonyMake`
  ShellSurface(U32 id, Client& client) : Common(Kind, id, client) {}

  static ShellSurface& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Edge values for resizing
  //
  // These values are used to indicate which edge of a surface
  // is being dragged in a resize operation. The server may
  // use this information to adapt its behavior, e.g. choose
  // an appropriate cursor image.
  enum Resize : U32 {
    ResizeNone = 0, // No edge
    ResizeTop = 1, // Top edge
    ResizeBottom = 2, // Bottom edge
    ResizeLeft = 4, // Left edge
    ResizeTopLeft = 5, // Top and left edges
    ResizeBottomLeft = 6, // Bottom and left edges
    ResizeRight = 8, // Right edge
    ResizeTopRight = 9, // Top and right edges
    ResizeBottomRight = 10, // Bottom and right edges
  };

  static StrView ResizeToStr(U32 value) {
    switch (value) {
    case 0: return "None"sv;
    case 1: return "Top"sv;
    case 2: return "Bottom"sv;
    case 4: return "Left"sv;
    case 5: return "TopLeft"sv;
    case 6: return "BottomLeft"sv;
    case 8: return "Right"sv;
    case 9: return "TopRight"sv;
    case 10: return "BottomRight"sv;
    default: return "UnknownResize"sv;
    }
  }

  // Details of transient behaviour
  //
  // These flags specify details of the expected behaviour
  // of transient surfaces. Used in the set_transient request.
  enum Transient : U32 {
    TransientInactive = 0x1, // Do not set keyboard focus
  };

  static StrView TransientToStr(U32 value) {
    switch (value) {
    case 0x1: return "Inactive"sv;
    default: return "UnknownTransient"sv;
    }
  }

  // Different method to set the surface fullscreen
  //
  // Hints to indicate to the compositor how to deal with a conflict
  // between the dimensions of the surface and the dimensions of the
  // output. The compositor is free to ignore this parameter.
  enum FullscreenMethod : U32 {
    FullscreenMethodDefault = 0, // No preference, apply default policy
    FullscreenMethodScale = 1, // Scale, preserve the surface's aspect ratio and center on output
    FullscreenMethodDriver = 2, // Switch output mode to the smallest mode that can fit the surface, add black borders to compensate size mismatch
    FullscreenMethodFill = 3, // No upscaling, center on output and add black borders to compensate size mismatch
  };

  static StrView FullscreenMethodToStr(U32 value) {
    switch (value) {
    case 0: return "Default"sv;
    case 1: return "Scale"sv;
    case 2: return "Driver"sv;
    case 3: return "Fill"sv;
    default: return "UnknownFullscreenMethod"sv;
    }
  }

  // Respond to a ping event
  //
  // A client must respond to a ping event with a pong request or
  // the client may be deemed unresponsive.
  void OnPong(U32 serial);

  // Start an interactive move
  //
  // Start a pointer-driven move of the surface.
  // 
  // This request must be used in response to a button press event.
  // The server may ignore move requests depending on the state of
  // the surface (e.g. fullscreen or maximized).
  void OnMove(Seat& seat, U32 serial);

  // Start an interactive resize
  //
  // Start a pointer-driven resizing of the surface.
  // 
  // This request must be used in response to a button press event.
  // The server may ignore resize requests depending on the state of
  // the surface (e.g. fullscreen or maximized).
  void OnResize(Seat& seat, U32 serial, enum Resize edges);

  // Make the surface a toplevel surface
  //
  // Map the surface as a toplevel surface.
  // 
  // A toplevel surface is not fullscreen, maximized or transient.
  void OnSetToplevel();

  // Make the surface a transient surface
  //
  // Map the surface relative to an existing surface.
  // 
  // The x and y arguments specify the location of the upper left
  // corner of the surface relative to the upper left corner of the
  // parent surface, in surface-local coordinates.
  // 
  // The flags argument controls details of the transient behaviour.
  void OnSetTransient(Surface& parent, I32 x, I32 y, enum Transient flags);

  // Make the surface a fullscreen surface
  //
  // Map the surface as a fullscreen surface.
  // 
  // If an output parameter is given then the surface will be made
  // fullscreen on that output. If the client does not specify the
  // output then the compositor will apply its policy - usually
  // choosing the output on which the surface has the biggest surface
  // area.
  // 
  // The client may specify a method to resolve a size conflict
  // between the output size and the surface size - this is provided
  // through the method parameter.
  // 
  // The framerate parameter is used only when the method is set
  // to "driver", to indicate the preferred framerate. A value of 0
  // indicates that the client does not care about framerate.  The
  // framerate is specified in mHz, that is framerate of 60000 is 60Hz.
  // 
  // A method of "scale" or "driver" implies a scaling operation of
  // the surface, either via a direct scaling operation or a change of
  // the output mode. This will override any kind of output scaling, so
  // that mapping a surface with a buffer size equal to the mode can
  // fill the screen independent of buffer_scale.
  // 
  // A method of "fill" means we don't scale up the buffer, however
  // any output scale is applied. This means that you may run into
  // an edge case where the application maps a buffer with the same
  // size of the output mode but buffer_scale 1 (thus making a
  // surface larger than the output). In this case it is allowed to
  // downscale the results to fit the screen.
  // 
  // The compositor must reply to this request with a configure event
  // with the dimensions for the output on which the surface will
  // be made fullscreen.
  void OnSetFullscreen(enum FullscreenMethod method, U32 framerate, Output* output);

  // Make the surface a popup surface
  //
  // Map the surface as a popup.
  // 
  // A popup surface is a transient surface with an added pointer
  // grab.
  // 
  // An existing implicit grab will be changed to owner-events mode,
  // and the popup grab will continue after the implicit grab ends
  // (i.e. releasing the mouse button does not cause the popup to
  // be unmapped).
  // 
  // The popup grab continues until the window is destroyed or a
  // mouse button is pressed in any other client's window. A click
  // in any of the client's surfaces is reported as normal, however,
  // clicks in other clients' surfaces will be discarded and trigger
  // the callback.
  // 
  // The x and y arguments specify the location of the upper left
  // corner of the surface relative to the upper left corner of the
  // parent surface, in surface-local coordinates.
  void OnSetPopup(Seat& seat, U32 serial, Surface& parent, I32 x, I32 y, enum Transient flags);

  // Make the surface a maximized surface
  //
  // Map the surface as a maximized surface.
  // 
  // If an output parameter is given then the surface will be
  // maximized on that output. If the client does not specify the
  // output then the compositor will apply its policy - usually
  // choosing the output on which the surface has the biggest surface
  // area.
  // 
  // The compositor will reply with a configure event telling
  // the expected new surface size. The operation is completed
  // on the next buffer attach to this surface.
  // 
  // A maximized surface typically fills the entire output it is
  // bound to, except for desktop elements such as panels. This is
  // the main difference between a maximized shell surface and a
  // fullscreen shell surface.
  // 
  // The details depend on the compositor implementation.
  void OnSetMaximized(Output* output);

  // Set surface title
  //
  // Set a short title for the surface.
  // 
  // This string may be used to identify the surface in a task bar,
  // window list, or other user interface elements provided by the
  // compositor.
  // 
  // The string must be encoded in UTF-8.
  void OnSetTitle(StrView title);

  // Set surface class
  //
  // Set a class for the surface.
  // 
  // The surface class identifies the general class of applications
  // to which the surface belongs. A common convention is to use the
  // file name (or the full path if it is a non-standard location) of
  // the application's .desktop file as the class.
  void OnSetClass(StrView class_);

  // Ping client
  //
  // Ping a client to check if it is receiving events and sending
  // requests. A client is expected to reply with a pong request.
  void Ping(U32 serial);

  // Suggest resize
  //
  // The configure event asks the client to resize its surface.
  // 
  // The size is a hint, in the sense that the client is free to
  // ignore it if it doesn't resize, pick a smaller size (to
  // satisfy aspect ratio or resize in steps of NxM pixels).
  // 
  // The edges parameter provides a hint about how the surface
  // was resized. The client may use this information to decide
  // how to adjust its content to the new size (e.g. a scrolling
  // area might adjust its content position to leave the viewable
  // content unmoved).
  // 
  // The client is free to dismiss all but the last configure
  // event it received.
  // 
  // The width and height arguments specify the size of the window
  // in surface-local coordinates.
  void Configure(enum Resize edges, I32 width, I32 height);

  // Popup interaction is done
  //
  // The popup_done event is sent out when a popup grab is broken,
  // that is, when the user clicks a surface that doesn't belong
  // to the client owning the popup surface.
  void PopupDone();
  // clang-format on: generated ShellSurface from wayland.xml
};

// clang-format off: generated Seat from wayland.xml

// Group of input devices
//
// A seat is a group of keyboards, pointer and touch devices. This
// object is published as a global during start up, or when such a
// device is hot plugged.  A seat typically has a pointer and
// maintains a keyboard focus and a pointer focus.
struct Seat : Common {
  static constexpr int Version = 10;
  static constexpr Kind Kind = KindSeat;

  static Colony<Seat> colony;

  // Do not use directly. Instead use `ColonyMake`
  Seat(U32 id, Client& client) : Common(Kind, id, client) {}

  static Seat& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Seat capability bitmask
  //
  // This is a bitmask of capabilities this seat has; if a member is
  // set, then it is present on the seat.
  enum Capability : U32 {
    CapabilityPointer = 1, // The seat has pointer devices
    CapabilityKeyboard = 2, // The seat has one or more keyboards
    CapabilityTouch = 4, // The seat has touch devices
  };

  static StrView CapabilityToStr(U32 value) {
    switch (value) {
    case 1: return "Pointer"sv;
    case 2: return "Keyboard"sv;
    case 4: return "Touch"sv;
    default: return "UnknownCapability"sv;
    }
  }

  // Wl_seat error values
  //
  // These errors can be emitted in response to wl_seat requests.
  enum Error : U32 {
    ErrorMissingCapability = 0, // Get_pointer, get_keyboard or get_touch called on seat without the matching capability
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "MissingCapability"sv;
    default: return "UnknownError"sv;
    }
  }

  // Return pointer object
  //
  // The ID provided will be initialized to the wl_pointer interface
  // for this seat.
  // 
  // This request only takes effect if the seat has the pointer
  // capability, or has had the pointer capability in the past.
  // It is a protocol violation to issue this request on a seat that has
  // never had the pointer capability. The missing_capability error will
  // be sent in this case.
  void OnGetPointer(Pointer& id);

  // Return keyboard object
  //
  // The ID provided will be initialized to the wl_keyboard interface
  // for this seat.
  // 
  // This request only takes effect if the seat has the keyboard
  // capability, or has had the keyboard capability in the past.
  // It is a protocol violation to issue this request on a seat that has
  // never had the keyboard capability. The missing_capability error will
  // be sent in this case.
  void OnGetKeyboard(Keyboard& id);

  // Return touch object
  //
  // The ID provided will be initialized to the wl_touch interface
  // for this seat.
  // 
  // This request only takes effect if the seat has the touch
  // capability, or has had the touch capability in the past.
  // It is a protocol violation to issue this request on a seat that has
  // never had the touch capability. The missing_capability error will
  // be sent in this case.
  void OnGetTouch(Touch& id);

  // Release the seat object
  //
  // Using this request a client can tell the server that it is not going to
  // use the seat object anymore.
  //
  // [destructor] After this method returns, this object will be released
  void OnRelease();

  // Seat capabilities changed
  //
  // This is sent on binding to the seat global or whenever a seat gains
  // or loses the pointer, keyboard or touch capabilities.
  // The argument is a capability enum containing the complete set of
  // capabilities this seat has.
  // 
  // When the pointer capability is added, a client may create a
  // wl_pointer object using the wl_seat.get_pointer request. This object
  // will receive pointer events until the capability is removed in the
  // future.
  // 
  // When the pointer capability is removed, a client should destroy the
  // wl_pointer objects associated with the seat where the capability was
  // removed, using the wl_pointer.release request. No further pointer
  // events will be received on these objects.
  // 
  // In some compositors, if a seat regains the pointer capability and a
  // client has a previously obtained wl_pointer object of version 4 or
  // less, that object may start sending pointer events again. This
  // behavior is considered a misinterpretation of the intended behavior
  // and must not be relied upon by the client. wl_pointer objects of
  // version 5 or later must not send events if created before the most
  // recent event notifying the client of an added pointer capability.
  // 
  // The above behavior also applies to wl_keyboard and wl_touch with the
  // keyboard and touch capabilities, respectively.
  void Capabilities(enum Capability capabilities);

  // Unique identifier for this seat
  //
  // In a multi-seat configuration the seat name can be used by clients to
  // help identify which physical devices the seat represents.
  // 
  // The seat name is a UTF-8 string with no convention defined for its
  // contents. Each name is unique among all wl_seat globals. The name is
  // only guaranteed to be unique for the current compositor instance.
  // 
  // The same seat names are used for all clients. Thus, the name can be
  // shared across processes to refer to a specific wl_seat global.
  // 
  // The name event is sent after binding to the seat global, and should be sent
  // before announcing capabilities. This event only sent once per seat object,
  // and the name does not change over the lifetime of the wl_seat global.
  // 
  // Compositors may re-use the same seat name if the wl_seat global is
  // destroyed and re-created later.
  void Name(StrView name);
  // clang-format on: generated Seat from wayland.xml

  U32 version = 1;
};

// clang-format off: generated Pointer from wayland.xml

// Pointer input device
//
// The wl_pointer interface represents one or more input devices,
// such as mice, which control the pointer location and pointer_focus
// of a seat.
// 
// The wl_pointer interface generates motion, enter and leave
// events for the surfaces that the pointer is located over,
// and button and axis events for button presses, button releases
// and scrolling.
struct Pointer : Common {
  static constexpr int Version = 10;
  static constexpr Kind Kind = KindPointer;

  static Colony<Pointer> colony;

  // Do not use directly. Instead use `ColonyMake`
  Pointer(U32 id, Client& client) : Common(Kind, id, client) {}

  static Pointer& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorRole = 0, // Given wl_surface has another role
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "Role"sv;
    default: return "UnknownError"sv;
    }
  }

  // Physical button state
  //
  // Describes the physical state of a button that produced the button
  // event.
  enum ButtonState : U32 {
    ButtonStateReleased = 0, // The button is not pressed
    ButtonStatePressed = 1, // The button is pressed
  };

  static StrView ButtonStateToStr(U32 value) {
    switch (value) {
    case 0: return "Released"sv;
    case 1: return "Pressed"sv;
    default: return "UnknownButtonState"sv;
    }
  }

  // Axis types
  //
  // Describes the axis types of scroll events.
  enum Axis : U32 {
    AxisVerticalScroll = 0, // Vertical axis
    AxisHorizontalScroll = 1, // Horizontal axis
  };

  static StrView AxisToStr(U32 value) {
    switch (value) {
    case 0: return "VerticalScroll"sv;
    case 1: return "HorizontalScroll"sv;
    default: return "UnknownAxis"sv;
    }
  }

  // Axis source types
  //
  // Describes the source types for axis events. This indicates to the
  // client how an axis event was physically generated; a client may
  // adjust the user interface accordingly. For example, scroll events
  // from a "finger" source may be in a smooth coordinate space with
  // kinetic scrolling whereas a "wheel" source may be in discrete steps
  // of a number of lines.
  // 
  // The "continuous" axis source is a device generating events in a
  // continuous coordinate space, but using something other than a
  // finger. One example for this source is button-based scrolling where
  // the vertical motion of a device is converted to scroll events while
  // a button is held down.
  // 
  // The "wheel tilt" axis source indicates that the actual device is a
  // wheel but the scroll event is not caused by a rotation but a
  // (usually sideways) tilt of the wheel.
  enum AxisSource : U32 {
    AxisSourceWheel = 0, // A physical wheel rotation
    AxisSourceFinger = 1, // Finger on a touch surface
    AxisSourceContinuous = 2, // Continuous coordinate space
    AxisSourceWheelTilt = 3, // A physical wheel tilt
  };

  static StrView AxisSourceToStr(U32 value) {
    switch (value) {
    case 0: return "Wheel"sv;
    case 1: return "Finger"sv;
    case 2: return "Continuous"sv;
    case 3: return "WheelTilt"sv;
    default: return "UnknownAxisSource"sv;
    }
  }

  // Axis relative direction
  //
  // This specifies the direction of the physical motion that caused a
  // wl_pointer.axis event, relative to the wl_pointer.axis direction.
  enum AxisRelativeDirection : U32 {
    AxisRelativeDirectionIdentical = 0, // Physical motion matches axis direction
    AxisRelativeDirectionInverted = 1, // Physical motion is the inverse of the axis direction
  };

  static StrView AxisRelativeDirectionToStr(U32 value) {
    switch (value) {
    case 0: return "Identical"sv;
    case 1: return "Inverted"sv;
    default: return "UnknownAxisRelativeDirection"sv;
    }
  }

  // Set the pointer surface
  //
  // Set the pointer surface, i.e., the surface that contains the
  // pointer image (cursor). This request gives the surface the role
  // of a cursor. If the surface already has another role, it raises
  // a protocol error.
  // 
  // The cursor actually changes only if the pointer
  // focus for this device is one of the requesting client's surfaces
  // or the surface parameter is the current pointer surface. If
  // there was a previous surface set with this request it is
  // replaced. If surface is NULL, the pointer image is hidden.
  // 
  // The parameters hotspot_x and hotspot_y define the position of
  // the pointer surface relative to the pointer location. Its
  // top-left corner is always at (x, y) - (hotspot_x, hotspot_y),
  // where (x, y) are the coordinates of the pointer location, in
  // surface-local coordinates.
  // 
  // On wl_surface.offset requests to the pointer surface, hotspot_x
  // and hotspot_y are decremented by the x and y parameters
  // passed to the request. The offset must be applied by
  // wl_surface.commit as usual.
  // 
  // The hotspot can also be updated by passing the currently set
  // pointer surface to this request with new values for hotspot_x
  // and hotspot_y.
  // 
  // The input region is ignored for wl_surfaces with the role of
  // a cursor. When the use as a cursor ends, the wl_surface is
  // unmapped.
  // 
  // The serial parameter must match the latest wl_pointer.enter
  // serial number sent to the client. Otherwise the request will be
  // ignored.
  void OnSetCursor(U32 serial, Surface* surface, I32 hotspot_x, I32 hotspot_y);

  // Release the pointer object
  //
  // Using this request a client can tell the server that it is not going to
  // use the pointer object anymore.
  // 
  // This request destroys the pointer proxy object, so clients must not call
  // wl_pointer_destroy() after using this request.
  //
  // [destructor] After this method returns, this object will be released
  void OnRelease();

  // Enter event
  //
  // Notification that this seat's pointer is focused on a certain
  // surface.
  // 
  // When a seat's focus enters a surface, the pointer image
  // is undefined and a client should respond to this event by setting
  // an appropriate pointer image with the set_cursor request.
  void Enter(U32 serial, Surface& surface, float surface_x, float surface_y);

  // Leave event
  //
  // Notification that this seat's pointer is no longer focused on
  // a certain surface.
  // 
  // The leave notification is sent before the enter notification
  // for the new focus.
  void Leave(U32 serial, Surface& surface);

  // Pointer motion event
  //
  // Notification of pointer location change. The arguments
  // surface_x and surface_y are the location relative to the
  // focused surface.
  void Motion(U32 time, float surface_x, float surface_y);

  // Pointer button event
  //
  // Mouse button click and release notifications.
  // 
  // The location of the click is given by the last motion or
  // enter event.
  // The time argument is a timestamp with millisecond
  // granularity, with an undefined base.
  // 
  // The button is a button code as defined in the Linux kernel's
  // linux/input-event-codes.h header file, e.g. BTN_LEFT.
  // 
  // Any 16-bit button code value is reserved for future additions to the
  // kernel's event code list. All other button codes above 0xFFFF are
  // currently undefined but may be used in future versions of this
  // protocol.
  void Button(U32 serial, U32 time, U32 button, enum ButtonState state);

  // Axis event
  //
  // Scroll and other axis notifications.
  // 
  // For scroll events (vertical and horizontal scroll axes), the
  // value parameter is the length of a vector along the specified
  // axis in a coordinate space identical to those of motion events,
  // representing a relative movement along the specified axis.
  // 
  // For devices that support movements non-parallel to axes multiple
  // axis events will be emitted.
  // 
  // When applicable, for example for touch pads, the server can
  // choose to emit scroll events where the motion vector is
  // equivalent to a motion event vector.
  // 
  // When applicable, a client can transform its content relative to the
  // scroll distance.
  void Axis(U32 time, enum Axis axis, float value);

  // End of a pointer event sequence
  //
  // Indicates the end of a set of events that logically belong together.
  // A client is expected to accumulate the data in all events within the
  // frame before proceeding.
  // 
  // All wl_pointer events before a wl_pointer.frame event belong
  // logically together. For example, in a diagonal scroll motion the
  // compositor will send an optional wl_pointer.axis_source event, two
  // wl_pointer.axis events (horizontal and vertical) and finally a
  // wl_pointer.frame event. The client may use this information to
  // calculate a diagonal vector for scrolling.
  // 
  // When multiple wl_pointer.axis events occur within the same frame,
  // the motion vector is the combined motion of all events.
  // When a wl_pointer.axis and a wl_pointer.axis_stop event occur within
  // the same frame, this indicates that axis movement in one axis has
  // stopped but continues in the other axis.
  // When multiple wl_pointer.axis_stop events occur within the same
  // frame, this indicates that these axes stopped in the same instance.
  // 
  // A wl_pointer.frame event is sent for every logical event group,
  // even if the group only contains a single wl_pointer event.
  // Specifically, a client may get a sequence: motion, frame, button,
  // frame, axis, frame, axis_stop, frame.
  // 
  // The wl_pointer.enter and wl_pointer.leave events are logical events
  // generated by the compositor and not the hardware. These events are
  // also grouped by a wl_pointer.frame. When a pointer moves from one
  // surface to another, a compositor should group the
  // wl_pointer.leave event within the same wl_pointer.frame.
  // However, a client must not rely on wl_pointer.leave and
  // wl_pointer.enter being in the same wl_pointer.frame.
  // Compositor-specific policies may require the wl_pointer.leave and
  // wl_pointer.enter event being split across multiple wl_pointer.frame
  // groups.
  void Frame();

  // Axis source event
  //
  // Source information for scroll and other axes.
  // 
  // This event does not occur on its own. It is sent before a
  // wl_pointer.frame event and carries the source information for
  // all events within that frame.
  // 
  // The source specifies how this event was generated. If the source is
  // wl_pointer.axis_source.finger, a wl_pointer.axis_stop event will be
  // sent when the user lifts the finger off the device.
  // 
  // If the source is wl_pointer.axis_source.wheel,
  // wl_pointer.axis_source.wheel_tilt or
  // wl_pointer.axis_source.continuous, a wl_pointer.axis_stop event may
  // or may not be sent. Whether a compositor sends an axis_stop event
  // for these sources is hardware-specific and implementation-dependent;
  // clients must not rely on receiving an axis_stop event for these
  // scroll sources and should treat scroll sequences from these scroll
  // sources as unterminated by default.
  // 
  // This event is optional. If the source is unknown for a particular
  // axis event sequence, no event is sent.
  // Only one wl_pointer.axis_source event is permitted per frame.
  // 
  // The order of wl_pointer.axis_discrete and wl_pointer.axis_source is
  // not guaranteed.
  void AxisSource(enum AxisSource axis_source);

  // Axis stop event
  //
  // Stop notification for scroll and other axes.
  // 
  // For some wl_pointer.axis_source types, a wl_pointer.axis_stop event
  // is sent to notify a client that the axis sequence has terminated.
  // This enables the client to implement kinetic scrolling.
  // See the wl_pointer.axis_source documentation for information on when
  // this event may be generated.
  // 
  // Any wl_pointer.axis events with the same axis_source after this
  // event should be considered as the start of a new axis motion.
  // 
  // The timestamp is to be interpreted identical to the timestamp in the
  // wl_pointer.axis event. The timestamp value may be the same as a
  // preceding wl_pointer.axis event.
  void AxisStop(U32 time, enum Axis axis);

  // Axis click event
  //
  // Discrete step information for scroll and other axes.
  // 
  // This event carries the axis value of the wl_pointer.axis event in
  // discrete steps (e.g. mouse wheel clicks).
  // 
  // This event is deprecated with wl_pointer version 8 - this event is not
  // sent to clients supporting version 8 or later.
  // 
  // This event does not occur on its own, it is coupled with a
  // wl_pointer.axis event that represents this axis value on a
  // continuous scale. The protocol guarantees that each axis_discrete
  // event is always followed by exactly one axis event with the same
  // axis number within the same wl_pointer.frame. Note that the protocol
  // allows for other events to occur between the axis_discrete and
  // its coupled axis event, including other axis_discrete or axis
  // events. A wl_pointer.frame must not contain more than one axis_discrete
  // event per axis type.
  // 
  // This event is optional; continuous scrolling devices
  // like two-finger scrolling on touchpads do not have discrete
  // steps and do not generate this event.
  // 
  // The discrete value carries the directional information. e.g. a value
  // of -2 is two steps towards the negative direction of this axis.
  // 
  // The axis number is identical to the axis number in the associated
  // axis event.
  // 
  // The order of wl_pointer.axis_discrete and wl_pointer.axis_source is
  // not guaranteed.
  void AxisDiscrete(enum Axis axis, I32 discrete);

  // Axis high-resolution scroll event
  //
  // Discrete high-resolution scroll information.
  // 
  // This event carries high-resolution wheel scroll information,
  // with each multiple of 120 representing one logical scroll step
  // (a wheel detent). For example, an axis_value120 of 30 is one quarter of
  // a logical scroll step in the positive direction, a value120 of
  // -240 are two logical scroll steps in the negative direction within the
  // same hardware event.
  // Clients that rely on discrete scrolling should accumulate the
  // value120 to multiples of 120 before processing the event.
  // 
  // The value120 must not be zero.
  // 
  // This event replaces the wl_pointer.axis_discrete event in clients
  // supporting wl_pointer version 8 or later.
  // 
  // Where a wl_pointer.axis_source event occurs in the same
  // wl_pointer.frame, the axis source applies to this event.
  // 
  // The order of wl_pointer.axis_value120 and wl_pointer.axis_source is
  // not guaranteed.
  void AxisValue120(enum Axis axis, I32 value120);

  // Axis relative physical direction event
  //
  // Relative directional information of the entity causing the axis
  // motion.
  // 
  // For a wl_pointer.axis event, the wl_pointer.axis_relative_direction
  // event specifies the movement direction of the entity causing the
  // wl_pointer.axis event. For example:
  // - if a user's fingers on a touchpad move down and this
  //   causes a wl_pointer.axis vertical_scroll down event, the physical
  //   direction is 'identical'
  // - if a user's fingers on a touchpad move down and this causes a
  //   wl_pointer.axis vertical_scroll up scroll up event ('natural
  //   scrolling'), the physical direction is 'inverted'.
  // 
  // A client may use this information to adjust scroll motion of
  // components. Specifically, enabling natural scrolling causes the
  // content to change direction compared to traditional scrolling.
  // Some widgets like volume control sliders should usually match the
  // physical direction regardless of whether natural scrolling is
  // active. This event enables clients to match the scroll direction of
  // a widget to the physical direction.
  // 
  // This event does not occur on its own, it is coupled with a
  // wl_pointer.axis event that represents this axis value.
  // The protocol guarantees that each axis_relative_direction event is
  // always followed by exactly one axis event with the same
  // axis number within the same wl_pointer.frame. Note that the protocol
  // allows for other events to occur between the axis_relative_direction
  // and its coupled axis event.
  // 
  // The axis number is identical to the axis number in the associated
  // axis event.
  // 
  // The order of wl_pointer.axis_relative_direction,
  // wl_pointer.axis_discrete and wl_pointer.axis_source is not
  // guaranteed.
  void AxisRelativeDirection(enum Axis axis, enum AxisRelativeDirection direction);
  // clang-format on: generated Pointer from wayland.xml

  U32 version = 1;
};

// clang-format off: generated Keyboard from wayland.xml

// Keyboard input device
//
// The wl_keyboard interface represents one or more keyboards
// associated with a seat.
// 
// Each wl_keyboard has the following logical state:
// 
// - an active surface (possibly null),
// - the keys currently logically down,
// - the active modifiers,
// - the active group.
// 
// By default, the active surface is null, the keys currently logically down
// are empty, the active modifiers and the active group are 0.
struct Keyboard : Common {
  static constexpr int Version = 10;
  static constexpr Kind Kind = KindKeyboard;

  static Colony<Keyboard> colony;

  // Do not use directly. Instead use `ColonyMake`
  Keyboard(U32 id, Client& client) : Common(Kind, id, client) {}

  static Keyboard& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Keyboard mapping format
  //
  // This specifies the format of the keymap provided to the
  // client with the wl_keyboard.keymap event.
  enum KeymapFormat : U32 {
    KeymapFormatNoKeymap = 0, // No keymap; client must understand how to interpret the raw keycode
    KeymapFormatXkbV1 = 1, // Libxkbcommon compatible, null-terminated string; to determine the xkb keycode, clients must add 8 to the key event keycode
  };

  static StrView KeymapFormatToStr(U32 value) {
    switch (value) {
    case 0: return "NoKeymap"sv;
    case 1: return "XkbV1"sv;
    default: return "UnknownKeymapFormat"sv;
    }
  }

  // Physical key state
  //
  // Describes the physical state of a key that produced the key event.
  // 
  // Since version 10, the key can be in a "repeated" pseudo-state which
  // means the same as "pressed", but is used to signal repetition in the
  // key event.
  // 
  // The key may only enter the repeated state after entering the pressed
  // state and before entering the released state. This event may be
  // generated multiple times while the key is down.
  enum KeyState : U32 {
    KeyStateReleased = 0, // Key is not pressed
    KeyStatePressed = 1, // Key is pressed
    KeyStateRepeated = 2, // Key was repeated
  };

  static StrView KeyStateToStr(U32 value) {
    switch (value) {
    case 0: return "Released"sv;
    case 1: return "Pressed"sv;
    case 2: return "Repeated"sv;
    default: return "UnknownKeyState"sv;
    }
  }

  // Release the keyboard object
  //
  // [destructor] After this method returns, this object will be released
  void OnRelease();

  // Keyboard mapping
  //
  // This event provides a file descriptor to the client which can be
  // memory-mapped in read-only mode to provide a keyboard mapping
  // description.
  // 
  // From version 7 onwards, the fd must be mapped with MAP_PRIVATE by
  // the recipient, as MAP_SHARED may fail.
  void Keymap(enum KeymapFormat format, FD&& fd, U32 size);

  // Enter event
  //
  // Notification that this seat's keyboard focus is on a certain
  // surface.
  // 
  // The compositor must send the wl_keyboard.modifiers event after this
  // event.
  // 
  // In the wl_keyboard logical state, this event sets the active surface to
  // the surface argument and the keys currently logically down to the keys
  // in the keys argument. The compositor must not send this event if the
  // wl_keyboard already had an active surface immediately before this event.
  // 
  // Clients should not use the list of pressed keys to emulate key-press
  // events. The order of keys in the list is unspecified.
  void Enter(U32 serial, Surface& surface, Span<> keys);

  // Leave event
  //
  // Notification that this seat's keyboard focus is no longer on
  // a certain surface.
  // 
  // The leave notification is sent before the enter notification
  // for the new focus.
  // 
  // In the wl_keyboard logical state, this event resets all values to their
  // defaults. The compositor must not send this event if the active surface
  // of the wl_keyboard was not equal to the surface argument immediately
  // before this event.
  void Leave(U32 serial, Surface& surface);

  // Key event
  //
  // A key was pressed or released.
  // The time argument is a timestamp with millisecond
  // granularity, with an undefined base.
  // 
  // The key is a platform-specific key code that can be interpreted
  // by feeding it to the keyboard mapping (see the keymap event).
  // 
  // If this event produces a change in modifiers, then the resulting
  // wl_keyboard.modifiers event must be sent after this event.
  // 
  // In the wl_keyboard logical state, this event adds the key to the keys
  // currently logically down (if the state argument is pressed) or removes
  // the key from the keys currently logically down (if the state argument is
  // released). The compositor must not send this event if the wl_keyboard
  // did not have an active surface immediately before this event. The
  // compositor must not send this event if state is pressed (resp. released)
  // and the key was already logically down (resp. was not logically down)
  // immediately before this event.
  // 
  // Since version 10, compositors may send key events with the "repeated"
  // key state when a wl_keyboard.repeat_info event with a rate argument of
  // 0 has been received. This allows the compositor to take over the
  // responsibility of key repetition.
  void Key(U32 serial, U32 time, U32 key, enum KeyState state);

  // Modifier and group state
  //
  // Notifies clients that the modifier and/or group state has
  // changed, and it should update its local state.
  // 
  // The compositor may send this event without a surface of the client
  // having keyboard focus, for example to tie modifier information to
  // pointer focus instead. If a modifier event with pressed modifiers is sent
  // without a prior enter event, the client can assume the modifier state is
  // valid until it receives the next wl_keyboard.modifiers event. In order to
  // reset the modifier state again, the compositor can send a
  // wl_keyboard.modifiers event with no pressed modifiers.
  // 
  // In the wl_keyboard logical state, this event updates the modifiers and
  // group.
  void Modifiers(U32 serial, U32 mods_depressed, U32 mods_latched, U32 mods_locked, U32 group);

  // Repeat rate and delay
  //
  // Informs the client about the keyboard's repeat rate and delay.
  // 
  // This event is sent as soon as the wl_keyboard object has been created,
  // and is guaranteed to be received by the client before any key press
  // event.
  // 
  // Negative values for either rate or delay are illegal. A rate of zero
  // will disable any repeating (regardless of the value of delay).
  // 
  // This event can be sent later on as well with a new value if necessary,
  // so clients should continue listening for the event past the creation
  // of wl_keyboard.
  void RepeatInfo(I32 rate, I32 delay);
  // clang-format on: generated Keyboard from wayland.xml

  U32 version = 1;
};

// clang-format off: generated Touch from wayland.xml

// Touchscreen input device
//
// The wl_touch interface represents a touchscreen
// associated with a seat.
// 
// Touch interactions can consist of one or more contacts.
// For each contact, a series of events is generated, starting
// with a down event, followed by zero or more motion events,
// and ending with an up event. Events relating to the same
// contact point can be identified by the ID of the sequence.
struct Touch : Common {
  static constexpr int Version = 10;
  static constexpr Kind Kind = KindTouch;

  static Colony<Touch> colony;

  // Do not use directly. Instead use `ColonyMake`
  Touch(U32 id, Client& client) : Common(Kind, id, client) {}

  static Touch& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Release the touch object
  //
  // [destructor] After this method returns, this object will be released
  void OnRelease();

  // Touch down event and beginning of a touch sequence
  //
  // A new touch point has appeared on the surface. This touch point is
  // assigned a unique ID. Future events from this touch point reference
  // this ID. The ID ceases to be valid after a touch up event and may be
  // reused in the future.
  void Down(U32 serial, U32 time, Surface& surface, I32 id, float x, float y);

  // End of a touch event sequence
  //
  // The touch point has disappeared. No further events will be sent for
  // this touch point and the touch point's ID is released and may be
  // reused in a future touch down event.
  void Up(U32 serial, U32 time, I32 id);

  // Update of touch point coordinates
  //
  //  touch point has changed coordinates.
  void Motion(U32 time, I32 id, float x, float y);

  // End of touch frame event
  //
  // Indicates the end of a set of events that logically belong together.
  // A client is expected to accumulate the data in all events within the
  // frame before proceeding.
  // 
  // A wl_touch.frame terminates at least one event but otherwise no
  // guarantee is provided about the set of events within a frame. A client
  // must assume that any state not updated in a frame is unchanged from the
  // previously known state.
  void Frame();

  // Touch session cancelled
  //
  // Sent if the compositor decides the touch stream is a global
  // gesture. No further events are sent to the clients from that
  // particular gesture. Touch cancellation applies to all touch points
  // currently active on this client's surface. The client is
  // responsible for finalizing the touch points, future touch points on
  // this surface may reuse the touch point ID.
  // 
  // No frame event is required after the cancel event.
  void Cancel();

  // Update shape of touch point
  //
  // Sent when a touchpoint has changed its shape.
  // 
  // This event does not occur on its own. It is sent before a
  // wl_touch.frame event and carries the new shape information for
  // any previously reported, or new touch points of that frame.
  // 
  // Other events describing the touch point such as wl_touch.down,
  // wl_touch.motion or wl_touch.orientation may be sent within the
  // same wl_touch.frame. A client should treat these events as a single
  // logical touch point update. The order of wl_touch.shape,
  // wl_touch.orientation and wl_touch.motion is not guaranteed.
  // A wl_touch.down event is guaranteed to occur before the first
  // wl_touch.shape event for this touch ID but both events may occur within
  // the same wl_touch.frame.
  // 
  // A touchpoint shape is approximated by an ellipse through the major and
  // minor axis length. The major axis length describes the longer diameter
  // of the ellipse, while the minor axis length describes the shorter
  // diameter. Major and minor are orthogonal and both are specified in
  // surface-local coordinates. The center of the ellipse is always at the
  // touchpoint location as reported by wl_touch.down or wl_touch.move.
  // 
  // This event is only sent by the compositor if the touch device supports
  // shape reports. The client has to make reasonable assumptions about the
  // shape if it did not receive this event.
  void Shape(I32 id, float major, float minor);

  // Update orientation of touch point
  //
  // Sent when a touchpoint has changed its orientation.
  // 
  // This event does not occur on its own. It is sent before a
  // wl_touch.frame event and carries the new shape information for
  // any previously reported, or new touch points of that frame.
  // 
  // Other events describing the touch point such as wl_touch.down,
  // wl_touch.motion or wl_touch.shape may be sent within the
  // same wl_touch.frame. A client should treat these events as a single
  // logical touch point update. The order of wl_touch.shape,
  // wl_touch.orientation and wl_touch.motion is not guaranteed.
  // A wl_touch.down event is guaranteed to occur before the first
  // wl_touch.orientation event for this touch ID but both events may occur
  // within the same wl_touch.frame.
  // 
  // The orientation describes the clockwise angle of a touchpoint's major
  // axis to the positive surface y-axis and is normalized to the -180 to
  // +180 degree range. The granularity of orientation depends on the touch
  // device, some devices only support binary rotation values between 0 and
  // 90 degrees.
  // 
  // This event is only sent by the compositor if the touch device supports
  // orientation reports.
  void Orientation(I32 id, float orientation);
  // clang-format on: generated Touch from wayland.xml
};

// clang-format off: generated Output from wayland.xml

// Compositor output region
//
// An output describes part of the compositor geometry.  The
// compositor works in the 'compositor coordinate system' and an
// output corresponds to a rectangular area in that space that is
// actually visible.  This typically corresponds to a monitor that
// displays part of the compositor space.  This object is published
// as global during start up, or when a monitor is hotplugged.
struct Output : Common {
  static constexpr int Version = 4;
  static constexpr Kind Kind = KindOutput;

  static Colony<Output> colony;

  // Do not use directly. Instead use `ColonyMake`
  Output(U32 id, Client& client) : Common(Kind, id, client) {}

  static Output& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Subpixel geometry information
  //
  // This enumeration describes how the physical
  // pixels on an output are laid out.
  enum Subpixel : U32 {
    SubpixelUnknown = 0, // Unknown geometry
    SubpixelNone = 1, // No geometry
    SubpixelHorizontalRgb = 2, // Horizontal rgb
    SubpixelHorizontalBgr = 3, // Horizontal bgr
    SubpixelVerticalRgb = 4, // Vertical rgb
    SubpixelVerticalBgr = 5, // Vertical bgr
  };

  static StrView SubpixelToStr(U32 value) {
    switch (value) {
    case 0: return "Unknown"sv;
    case 1: return "None"sv;
    case 2: return "HorizontalRgb"sv;
    case 3: return "HorizontalBgr"sv;
    case 4: return "VerticalRgb"sv;
    case 5: return "VerticalBgr"sv;
    default: return "UnknownSubpixel"sv;
    }
  }

  // Transformation applied to buffer contents
  //
  // This describes transformations that clients and compositors apply to
  // buffer contents.
  // 
  // The flipped values correspond to an initial flip around a
  // vertical axis followed by rotation.
  // 
  // The purpose is mainly to allow clients to render accordingly and
  // tell the compositor, so that for fullscreen surfaces, the
  // compositor will still be able to scan out directly from client
  // surfaces.
  enum Transform : U32 {
    TransformNormal = 0, // No transform
    Transform90 = 1, // 90 degrees counter-clockwise
    Transform180 = 2, // 180 degrees counter-clockwise
    Transform270 = 3, // 270 degrees counter-clockwise
    TransformFlipped = 4, // 180 degree flip around a vertical axis
    TransformFlipped90 = 5, // Flip and rotate 90 degrees counter-clockwise
    TransformFlipped180 = 6, // Flip and rotate 180 degrees counter-clockwise
    TransformFlipped270 = 7, // Flip and rotate 270 degrees counter-clockwise
  };

  static StrView TransformToStr(U32 value) {
    switch (value) {
    case 0: return "Normal"sv;
    case 1: return "90"sv;
    case 2: return "180"sv;
    case 3: return "270"sv;
    case 4: return "Flipped"sv;
    case 5: return "Flipped90"sv;
    case 6: return "Flipped180"sv;
    case 7: return "Flipped270"sv;
    default: return "UnknownTransform"sv;
    }
  }

  // Mode information
  //
  // These flags describe properties of an output mode.
  // They are used in the flags bitfield of the mode event.
  enum Mode : U32 {
    ModeCurrent = 0x1, // Indicates this is the current mode
    ModePreferred = 0x2, // Indicates this is the preferred mode
  };

  static StrView ModeToStr(U32 value) {
    switch (value) {
    case 0x1: return "Current"sv;
    case 0x2: return "Preferred"sv;
    default: return "UnknownMode"sv;
    }
  }

  // Release the output object
  //
  // Using this request a client can tell the server that it is not going to
  // use the output object anymore.
  //
  // [destructor] After this method returns, this object will be released
  void OnRelease();

  // Properties of the output
  //
  // The geometry event describes geometric properties of the output.
  // The event is sent when binding to the output object and whenever
  // any of the properties change.
  // 
  // The physical size can be set to zero if it doesn't make sense for this
  // output (e.g. for projectors or virtual outputs).
  // 
  // The geometry event will be followed by a done event (starting from
  // version 2).
  // 
  // Clients should use wl_surface.preferred_buffer_transform instead of the
  // transform advertised by this event to find the preferred buffer
  // transform to use for a surface.
  // 
  // Note: wl_output only advertises partial information about the output
  // position and identification. Some compositors, for instance those not
  // implementing a desktop-style output layout or those exposing virtual
  // outputs, might fake this information. Instead of using x and y, clients
  // should use xdg_output.logical_position. Instead of using make and model,
  // clients should use name and description.
  void Geometry(I32 x, I32 y, I32 physical_width, I32 physical_height, enum Subpixel subpixel, StrView make, StrView model, enum Transform transform);

  // Advertise available modes for the output
  //
  // The mode event describes an available mode for the output.
  // 
  // The event is sent when binding to the output object and there
  // will always be one mode, the current mode.  The event is sent
  // again if an output changes mode, for the mode that is now
  // current.  In other words, the current mode is always the last
  // mode that was received with the current flag set.
  // 
  // Non-current modes are deprecated. A compositor can decide to only
  // advertise the current mode and never send other modes. Clients
  // should not rely on non-current modes.
  // 
  // The size of a mode is given in physical hardware units of
  // the output device. This is not necessarily the same as
  // the output size in the global compositor space. For instance,
  // the output may be scaled, as described in wl_output.scale,
  // or transformed, as described in wl_output.transform. Clients
  // willing to retrieve the output size in the global compositor
  // space should use xdg_output.logical_size instead.
  // 
  // The vertical refresh rate can be set to zero if it doesn't make
  // sense for this output (e.g. for virtual outputs).
  // 
  // The mode event will be followed by a done event (starting from
  // version 2).
  // 
  // Clients should not use the refresh rate to schedule frames. Instead,
  // they should use the wl_surface.frame event or the presentation-time
  // protocol.
  // 
  // Note: this information is not always meaningful for all outputs. Some
  // compositors, such as those exposing virtual outputs, might fake the
  // refresh rate or the size.
  void Mode(enum Mode flags, I32 width, I32 height, I32 refresh);

  // Sent all information about output
  //
  // This event is sent after all other properties have been
  // sent after binding to the output object and after any
  // other property changes done after that. This allows
  // changes to the output properties to be seen as
  // atomic, even if they happen via multiple events.
  void Done();

  // Output scaling properties
  //
  // This event contains scaling geometry information
  // that is not in the geometry event. It may be sent after
  // binding the output object or if the output scale changes
  // later. The compositor will emit a non-zero, positive
  // value for scale. If it is not sent, the client should
  // assume a scale of 1.
  // 
  // A scale larger than 1 means that the compositor will
  // automatically scale surface buffers by this amount
  // when rendering. This is used for very high resolution
  // displays where applications rendering at the native
  // resolution would be too small to be legible.
  // 
  // Clients should use wl_surface.preferred_buffer_scale
  // instead of this event to find the preferred buffer
  // scale to use for a surface.
  // 
  // The scale event will be followed by a done event.
  void Scale(I32 factor);

  // Name of this output
  //
  // Many compositors will assign user-friendly names to their outputs, show
  // them to the user, allow the user to refer to an output, etc. The client
  // may wish to know this name as well to offer the user similar behaviors.
  // 
  // The name is a UTF-8 string with no convention defined for its contents.
  // Each name is unique among all wl_output globals. The name is only
  // guaranteed to be unique for the compositor instance.
  // 
  // The same output name is used for all clients for a given wl_output
  // global. Thus, the name can be shared across processes to refer to a
  // specific wl_output global.
  // 
  // The name is not guaranteed to be persistent across sessions, thus cannot
  // be used to reliably identify an output in e.g. configuration files.
  // 
  // Examples of names include 'HDMI-A-1', 'WL-1', 'X11-1', etc. However, do
  // not assume that the name is a reflection of an underlying DRM connector,
  // X11 connection, etc.
  // 
  // The name event is sent after binding the output object. This event is
  // only sent once per output object, and the name does not change over the
  // lifetime of the wl_output global.
  // 
  // Compositors may re-use the same output name if the wl_output global is
  // destroyed and re-created later. Compositors should avoid re-using the
  // same name if possible.
  // 
  // The name event will be followed by a done event.
  void Name(StrView name);

  // Human-readable description of this output
  //
  // Many compositors can produce human-readable descriptions of their
  // outputs. The client may wish to know this description as well, e.g. for
  // output selection purposes.
  // 
  // The description is a UTF-8 string with no convention defined for its
  // contents. The description is not guaranteed to be unique among all
  // wl_output globals. Examples might include 'Foocorp 11" Display' or
  // 'Virtual X11 output via :1'.
  // 
  // The description event is sent after binding the output object and
  // whenever the description changes. The description is optional, and may
  // not be sent at all.
  // 
  // The description event will be followed by a done event.
  void Description(StrView description);
  // clang-format on: generated Output from wayland.xml
};

// clang-format off: generated Region from wayland.xml

// Region interface
//
// A region object describes an area.
// 
// Region objects are used to describe the opaque and input
// regions of a surface.
struct Region : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindRegion;

  static Colony<Region> colony;

  // Do not use directly. Instead use `ColonyMake`
  Region(U32 id, Client& client) : Common(Kind, id, client) {}

  static Region& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Destroy region
  //
  // Destroy the region.  This will invalidate the object ID.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Add rectangle to region
  //
  // dd the specified rectangle to the region.
  void OnAdd(I32 x, I32 y, I32 width, I32 height);

  // Subtract rectangle from region
  //
  // Subtract the specified rectangle from the region.
  void OnSubtract(I32 x, I32 y, I32 width, I32 height);
  // clang-format on: generated Region from wayland.xml

  SkPath path;
};

// clang-format off: generated Subcompositor from wayland.xml

// Sub-surface compositing
//
// The global interface exposing sub-surface compositing capabilities.
// A wl_surface, that has sub-surfaces associated, is called the
// parent surface. Sub-surfaces can be arbitrarily nested and create
// a tree of sub-surfaces.
// 
// The root surface in a tree of sub-surfaces is the main
// surface. The main surface cannot be a sub-surface, because
// sub-surfaces must always have a parent.
// 
// A main surface with its sub-surfaces forms a (compound) window.
// For window management purposes, this set of wl_surface objects is
// to be considered as a single window, and it should also behave as
// such.
// 
// The aim of sub-surfaces is to offload some of the compositing work
// within a window from clients to the compositor. A prime example is
// a video player with decorations and video in separate wl_surface
// objects. This should allow the compositor to pass YUV video buffer
// processing to dedicated overlay hardware when possible.
struct Subcompositor : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindSubcompositor;

  static Colony<Subcompositor> colony;

  // Do not use directly. Instead use `ColonyMake`
  Subcompositor(U32 id, Client& client) : Common(Kind, id, client) {}

  static Subcompositor& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorBadSurface = 0, // The to-be sub-surface is invalid
    ErrorBadParent = 1, // The to-be sub-surface parent is invalid
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "BadSurface"sv;
    case 1: return "BadParent"sv;
    default: return "UnknownError"sv;
    }
  }

  // Unbind from the subcompositor interface
  //
  // Informs the server that the client will not be using this
  // protocol object anymore. This does not affect any other
  // objects, wl_subsurface objects included.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Give a surface the role sub-surface
  //
  // Create a sub-surface interface for the given surface, and
  // associate it with the given parent surface. This turns a
  // plain wl_surface into a sub-surface.
  // 
  // The to-be sub-surface must not already have another role, and it
  // must not have an existing wl_subsurface object. Otherwise the
  // bad_surface protocol error is raised.
  // 
  // Adding sub-surfaces to a parent is a double-buffered operation on the
  // parent (see wl_surface.commit). The effect of adding a sub-surface
  // becomes visible on the next time the state of the parent surface is
  // applied.
  // 
  // The parent surface must not be one of the child surface's descendants,
  // and the parent must be different from the child surface, otherwise the
  // bad_parent protocol error is raised.
  // 
  // This request modifies the behaviour of wl_surface.commit request on
  // the sub-surface, see the documentation on wl_subsurface interface.
  void OnGetSubsurface(Subsurface& id, Surface& surface, Surface& parent);
  // clang-format on: generated Subcompositor from wayland.xml
};

// clang-format off: generated Subsurface from wayland.xml

// Sub-surface interface to a wl_surface
//
// An additional interface to a wl_surface object, which has been
// made a sub-surface. A sub-surface has one parent surface. A
// sub-surface's size and position are not limited to that of the parent.
// Particularly, a sub-surface is not automatically clipped to its
// parent's area.
// 
// A sub-surface becomes mapped, when a non-NULL wl_buffer is applied
// and the parent surface is mapped. The order of which one happens
// first is irrelevant. A sub-surface is hidden if the parent becomes
// hidden, or if a NULL wl_buffer is applied. These rules apply
// recursively through the tree of surfaces.
// 
// The behaviour of a wl_surface.commit request on a sub-surface
// depends on the sub-surface's mode. The possible modes are
// synchronized and desynchronized, see methods
// wl_subsurface.set_sync and wl_subsurface.set_desync. Synchronized
// mode caches the wl_surface state to be applied when the parent's
// state gets applied, and desynchronized mode applies the pending
// wl_surface state directly. A sub-surface is initially in the
// synchronized mode.
// 
// Sub-surfaces also have another kind of state, which is managed by
// wl_subsurface requests, as opposed to wl_surface requests. This
// state includes the sub-surface position relative to the parent
// surface (wl_subsurface.set_position), and the stacking order of
// the parent and its sub-surfaces (wl_subsurface.place_above and
// .place_below). This state is applied when the parent surface's
// wl_surface state is applied, regardless of the sub-surface's mode.
// As the exception, set_sync and set_desync are effective immediately.
// 
// The main surface can be thought to be always in desynchronized mode,
// since it does not have a parent in the sub-surfaces sense.
// 
// Even if a sub-surface is in desynchronized mode, it will behave as
// in synchronized mode, if its parent surface behaves as in
// synchronized mode. This rule is applied recursively throughout the
// tree of surfaces. This means, that one can set a sub-surface into
// synchronized mode, and then assume that all its child and grand-child
// sub-surfaces are synchronized, too, without explicitly setting them.
// 
// Destroying a sub-surface takes effect immediately. If you need to
// synchronize the removal of a sub-surface to the parent surface update,
// unmap the sub-surface first by attaching a NULL wl_buffer, update parent,
// and then destroy the sub-surface.
// 
// If the parent wl_surface object is destroyed, the sub-surface is
// unmapped.
// 
// A sub-surface never has the keyboard focus of any seat.
// 
// The wl_surface.offset request is ignored: clients must use set_position
// instead to move the sub-surface.
struct Subsurface : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindSubsurface;

  static Colony<Subsurface> colony;

  // Do not use directly. Instead use `ColonyMake`
  Subsurface(U32 id, Client& client) : Common(Kind, id, client) {}

  static Subsurface& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorBadSurface = 0, // Wl_surface is not a sibling or the parent
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "BadSurface"sv;
    default: return "UnknownError"sv;
    }
  }

  // Remove sub-surface interface
  //
  // The sub-surface interface is removed from the wl_surface object
  // that was turned into a sub-surface with a
  // wl_subcompositor.get_subsurface request. The wl_surface's association
  // to the parent is deleted. The wl_surface is unmapped immediately.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Reposition the sub-surface
  //
  // This schedules a sub-surface position change.
  // The sub-surface will be moved so that its origin (top left
  // corner pixel) will be at the location x, y of the parent surface
  // coordinate system. The coordinates are not restricted to the parent
  // surface area. Negative values are allowed.
  // 
  // The scheduled coordinates will take effect whenever the state of the
  // parent surface is applied.
  // 
  // If more than one set_position request is invoked by the client before
  // the commit of the parent surface, the position of a new request always
  // replaces the scheduled position from any previous request.
  // 
  // The initial position is 0, 0.
  void OnSetPosition(I32 x, I32 y);

  // Restack the sub-surface
  //
  // This sub-surface is taken from the stack, and put back just
  // above the reference surface, changing the z-order of the sub-surfaces.
  // The reference surface must be one of the sibling surfaces, or the
  // parent surface. Using any other surface, including this sub-surface,
  // will cause a protocol error.
  // 
  // The z-order is double-buffered. Requests are handled in order and
  // applied immediately to a pending state. The final pending state is
  // copied to the active state the next time the state of the parent
  // surface is applied.
  // 
  // A new sub-surface is initially added as the top-most in the stack
  // of its siblings and parent.
  void OnPlaceAbove(Surface& sibling);

  // Restack the sub-surface
  //
  // The sub-surface is placed just below the reference surface.
  // See wl_subsurface.place_above.
  void OnPlaceBelow(Surface& sibling);

  // Set sub-surface to synchronized mode
  //
  // Change the commit behaviour of the sub-surface to synchronized
  // mode, also described as the parent dependent mode.
  // 
  // In synchronized mode, wl_surface.commit on a sub-surface will
  // accumulate the committed state in a cache, but the state will
  // not be applied and hence will not change the compositor output.
  // The cached state is applied to the sub-surface immediately after
  // the parent surface's state is applied. This ensures atomic
  // updates of the parent and all its synchronized sub-surfaces.
  // Applying the cached state will invalidate the cache, so further
  // parent surface commits do not (re-)apply old state.
  // 
  // See wl_subsurface for the recursive effect of this mode.
  void OnSetSync();

  // Set sub-surface to desynchronized mode
  //
  // Change the commit behaviour of the sub-surface to desynchronized
  // mode, also described as independent or freely running mode.
  // 
  // In desynchronized mode, wl_surface.commit on a sub-surface will
  // apply the pending state directly, without caching, as happens
  // normally with a wl_surface. Calling wl_surface.commit on the
  // parent surface has no effect on the sub-surface's wl_surface
  // state. This mode allows a sub-surface to be updated on its own.
  // 
  // If cached state exists when wl_surface.commit is called in
  // desynchronized mode, the pending state is added to the cached
  // state, and applied as a whole. This invalidates the cache.
  // 
  // Note: even if a sub-surface is set to desynchronized, a parent
  // sub-surface may override it to behave as synchronized. For details,
  // see wl_subsurface.
  // 
  // If a surface's parent surface behaves as desynchronized, then
  // the cached state is applied on set_desync.
  void OnSetDesync();
  // clang-format on: generated Subsurface from wayland.xml

  Surface* surface = nullptr;  // the child surface (holds the subsurface role)
  MortalPtr<Surface> parent;
  SkIPoint pos = {}, pending_pos = {};
  bool position_dirty = false;
  bool sync = true;
  Optional<SurfaceCutout> cache;
  ~Subsurface();
};

// clang-format off: generated Fixes from wayland.xml

// Wayland protocol fixes
//
// This global fixes problems with other core-protocol interfaces that
// cannot be fixed in these interfaces themselves.
struct Fixes : Common {
  static constexpr int Version = 1;
  static constexpr Kind Kind = KindFixes;

  static Colony<Fixes> colony;

  // Do not use directly. Instead use `ColonyMake`
  Fixes(U32 id, Client& client) : Common(Kind, id, client) {}

  static Fixes& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Destroys this object
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Destroy a wl_registry
  //
  // This request destroys a wl_registry object.
  // 
  // The client should no longer use the wl_registry after making this
  // request.
  // 
  // The compositor will emit a wl_display.delete_id event with the object ID
  // of the registry and will no longer emit any events on the registry. The
  // client should re-use the object ID once it receives the
  // wl_display.delete_id event.
  void OnDestroyRegistry(Registry& registry);
  // clang-format on: generated Fixes from wayland.xml
};

// clang-format off: generated ZxdgDecorationManagerV1 from xdg-decoration-unstable-v1.xml

// Window decoration manager
//
// This interface allows a compositor to announce support for server-side
// decorations.
// 
// A window decoration is a set of window controls as deemed appropriate by
// the party managing them, such as user interface components used to move,
// resize and change a window's state.
// 
// A client can use this protocol to request being decorated by a supporting
// compositor.
// 
// If compositor and client do not negotiate the use of a server-side
// decoration using this protocol, clients continue to self-decorate as they
// see fit.
// 
// Warning! The protocol described in this file is experimental and
// backward incompatible changes may be made. Backward compatible changes
// may be added together with the corresponding interface version bump.
// Backward incompatible changes are done by bumping the version number in
// the protocol and interface names and resetting the interface version.
// Once the protocol is to be declared stable, the 'z' prefix and the
// version number in the protocol and interface names are removed and the
// interface version number is reset.
struct ZxdgDecorationManagerV1 : Common {
  static constexpr int Version = 2;
  static constexpr Kind Kind = KindZxdgDecorationManagerV1;

  static Colony<ZxdgDecorationManagerV1> colony;

  // Do not use directly. Instead use `ColonyMake`
  ZxdgDecorationManagerV1(U32 id, Client& client) : Common(Kind, id, client) {}

  static ZxdgDecorationManagerV1& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Destroy the decoration manager object
  //
  // Destroy the decoration manager. This doesn't destroy objects created
  // with the manager.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Create a new toplevel decoration object
  //
  // Create a new decoration object associated with the given toplevel.
  // 
  // For objects of version 1, creating an xdg_toplevel_decoration from an
  // xdg_toplevel which has a buffer attached or committed is a client
  // error, and any attempts by a client to attach or manipulate a buffer
  // prior to the first xdg_toplevel_decoration.configure event must also be
  // treated as errors.
  // 
  // For objects of version 2 or newer, creating an xdg_toplevel_decoration
  // from an xdg_toplevel which has a buffer attached or committed is
  // allowed. The initial decoration mode of the surface if a buffer is
  // already attached depends on whether a xdg_toplevel_decoration object
  // has been associated with the surface or not prior to this request.
  // 
  // If an xdg_toplevel_decoration was associated with the surface, then
  // destroyed without a surface commit, the previous decoration mode is
  // retained.
  // 
  // If no xdg_toplevel_decoration was associated with the surface prior to
  // this request, or if a surface commit has been performed after a previous
  // xdg_toplevel_decoration object associated with the surface was
  // destroyed, the decoration mode is assumed to be client-side.
  void OnGetToplevelDecoration(ZxdgToplevelDecorationV1& id, XdgToplevel& toplevel);
  // clang-format on: generated ZxdgDecorationManagerV1 from xdg-decoration-unstable-v1.xml
};

// clang-format off: generated ZxdgToplevelDecorationV1 from xdg-decoration-unstable-v1.xml

// Decoration object for a toplevel surface
//
// The decoration object allows the compositor to toggle server-side window
// decorations for a toplevel surface. The client can request to switch to
// another mode.
// 
// The xdg_toplevel_decoration object must be destroyed before its
// xdg_toplevel.
struct ZxdgToplevelDecorationV1 : Common {
  static constexpr int Version = 2;
  static constexpr Kind Kind = KindZxdgToplevelDecorationV1;

  static Colony<ZxdgToplevelDecorationV1> colony;

  // Do not use directly. Instead use `ColonyMake`
  ZxdgToplevelDecorationV1(U32 id, Client& client) : Common(Kind, id, client) {}

  static ZxdgToplevelDecorationV1& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorUnconfiguredBuffer = 0, // Xdg_toplevel has a buffer attached before configure
    ErrorAlreadyConstructed = 1, // Xdg_toplevel already has a decoration object
    ErrorOrphaned = 2, // Xdg_toplevel destroyed before the decoration object
    ErrorInvalidMode = 3, // Invalid mode
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "UnconfiguredBuffer"sv;
    case 1: return "AlreadyConstructed"sv;
    case 2: return "Orphaned"sv;
    case 3: return "InvalidMode"sv;
    default: return "UnknownError"sv;
    }
  }

  // Window decoration modes
  //
  // These values describe window decoration modes.
  enum Mode : U32 {
    ModeClientSide = 1, // No server-side window decoration
    ModeServerSide = 2, // Server-side window decoration
  };

  static StrView ModeToStr(U32 value) {
    switch (value) {
    case 1: return "ClientSide"sv;
    case 2: return "ServerSide"sv;
    default: return "UnknownMode"sv;
    }
  }

  // Destroy the decoration object
  //
  // Switch back to a mode without any server-side decorations at the next
  // commit, unless a new xdg_toplevel_decoration is created for the surface
  // first.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Set the decoration mode
  //
  // Set the toplevel surface decoration mode. This informs the compositor
  // that the client prefers the provided decoration mode.
  // 
  // After requesting a decoration mode, the compositor will respond by
  // emitting an xdg_surface.configure event. The client should then update
  // its content, drawing it without decorations if the received mode is
  // server-side decorations. The client must also acknowledge the configure
  // when committing the new content (see xdg_surface.ack_configure).
  // 
  // The compositor can decide not to use the client's mode and enforce a
  // different mode instead.
  // 
  // Clients whose decoration mode depend on the xdg_toplevel state may send
  // a set_mode request in response to an xdg_surface.configure event and wait
  // for the next xdg_surface.configure event to prevent unwanted state.
  // Such clients are responsible for preventing configure loops and must
  // make sure not to send multiple successive set_mode requests with the
  // same decoration mode.
  // 
  // If an invalid mode is supplied by the client, the invalid_mode protocol
  // error is raised by the compositor.
  void OnSetMode(enum Mode mode);

  // Unset the decoration mode
  //
  // Unset the toplevel surface decoration mode. This informs the compositor
  // that the client doesn't prefer a particular decoration mode.
  // 
  // This request has the same semantics as set_mode.
  void OnUnsetMode();

  // Notify a decoration mode change
  //
  // The configure event configures the effective decoration mode. The
  // configured state should not be applied immediately. Clients must send an
  // ack_configure in response to this event. See xdg_surface.configure and
  // xdg_surface.ack_configure for details.
  // 
  // A configure event can be sent at any time. The specified mode must be
  // obeyed by the client.
  void Configure(enum Mode mode);
  // clang-format on: generated ZxdgToplevelDecorationV1 from xdg-decoration-unstable-v1.xml

  MortalPtr<XdgToplevel> toplevel;
  U32 client_mode = 0;
};

// clang-format off: generated XdgWmBase from xdg-shell.xml

// Create desktop-style surfaces
//
// The xdg_wm_base interface is exposed as a global object enabling clients
// to turn their wl_surfaces into windows in a desktop environment. It
// defines the basic functionality needed for clients and the compositor to
// create windows that can be dragged, resized, maximized, etc, as well as
// creating transient windows such as popup menus.
struct XdgWmBase : Common {
  static constexpr int Version = 7;
  static constexpr Kind Kind = KindXdgWmBase;

  static Colony<XdgWmBase> colony;

  // Do not use directly. Instead use `ColonyMake`
  XdgWmBase(U32 id, Client& client) : Common(Kind, id, client) {}

  static XdgWmBase& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorRole = 0, // Given wl_surface has another role
    ErrorDefunctSurfaces = 1, // Xdg_wm_base was destroyed before children
    ErrorNotTheTopmostPopup = 2, // The client tried to map or destroy a non-topmost popup
    ErrorInvalidPopupParent = 3, // The client specified an invalid popup parent surface
    ErrorInvalidSurfaceState = 4, // The client provided an invalid surface state
    ErrorInvalidPositioner = 5, // The client provided an invalid positioner
    ErrorUnresponsive = 6, // The client didn’t respond to a ping event in time
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "Role"sv;
    case 1: return "DefunctSurfaces"sv;
    case 2: return "NotTheTopmostPopup"sv;
    case 3: return "InvalidPopupParent"sv;
    case 4: return "InvalidSurfaceState"sv;
    case 5: return "InvalidPositioner"sv;
    case 6: return "Unresponsive"sv;
    default: return "UnknownError"sv;
    }
  }

  // Destroy xdg_wm_base
  //
  // Destroy this xdg_wm_base object.
  // 
  // Destroying a bound xdg_wm_base object while there are surfaces
  // still alive created by this xdg_wm_base object instance is illegal
  // and will result in a defunct_surfaces error.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Create a positioner object
  //
  // Create a positioner object. A positioner object is used to position
  // surfaces relative to some parent surface. See the interface description
  // and xdg_surface.get_popup for details.
  void OnCreatePositioner(XdgPositioner& id);

  // Create a shell surface from a surface
  //
  // This creates an xdg_surface for the given surface. While xdg_surface
  // itself is not a role, the corresponding surface may only be assigned
  // a role extending xdg_surface, such as xdg_toplevel or xdg_popup. It is
  // illegal to create an xdg_surface for a wl_surface which already has an
  // assigned role and this will result in a role error.
  // 
  // This creates an xdg_surface for the given surface. An xdg_surface is
  // used as basis to define a role to a given surface, such as xdg_toplevel
  // or xdg_popup. It also manages functionality shared between xdg_surface
  // based surface roles.
  // 
  // See the documentation of xdg_surface for more details about what an
  // xdg_surface is and how it is used.
  void OnGetXdgSurface(XdgSurface& id, Surface& surface);

  // Respond to a ping event
  //
  // A client must respond to a ping event with a pong request or
  // the client may be deemed unresponsive. See xdg_wm_base.ping
  // and xdg_wm_base.error.unresponsive.
  void OnPong(U32 serial);

  // Check if the client is alive
  //
  // The ping event asks the client if it's still alive. Pass the
  // serial specified in the event back to the compositor by sending
  // a "pong" request back with the specified serial. See xdg_wm_base.pong.
  // 
  // Compositors can use this to determine if the client is still
  // alive. It's unspecified what will happen if the client doesn't
  // respond to the ping request, or in what timeframe. Clients should
  // try to respond in a reasonable amount of time. The “unresponsive”
  // error is provided for compositors that wish to disconnect unresponsive
  // clients.
  // 
  // A compositor is free to ping in any way it wants, but a client must
  // always respond to any xdg_wm_base object it created.
  void Ping(U32 serial);
  // clang-format on: generated XdgWmBase from xdg-shell.xml

  U32 version = 1;
};

// clang-format off: generated XdgPositioner from xdg-shell.xml

// Child surface positioner
//
// The xdg_positioner provides a collection of rules for the placement of a
// child surface relative to a parent surface. Rules can be defined to ensure
// the child surface remains within the visible area's borders, and to
// specify how the child surface changes its position, such as sliding along
// an axis, or flipping around a rectangle. These positioner-created rules are
// constrained by the requirement that a child surface must intersect with or
// be at least partially adjacent to its parent surface.
// 
// See the various requests for details about possible rules.
// 
// At the time of the request, the compositor makes a copy of the rules
// specified by the xdg_positioner. Thus, after the request is complete the
// xdg_positioner object can be destroyed or reused; further changes to the
// object will have no effect on previous usages.
// 
// For an xdg_positioner object to be considered complete, it must have a
// non-zero size set by set_size, and a non-zero anchor rectangle set by
// set_anchor_rect. Passing an incomplete xdg_positioner object when
// positioning a surface raises an invalid_positioner error.
struct XdgPositioner : Common {
  static constexpr int Version = 7;
  static constexpr Kind Kind = KindXdgPositioner;

  static Colony<XdgPositioner> colony;

  // Do not use directly. Instead use `ColonyMake`
  XdgPositioner(U32 id, Client& client) : Common(Kind, id, client) {}

  static XdgPositioner& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorInvalidInput = 0, // Invalid input provided
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "InvalidInput"sv;
    default: return "UnknownError"sv;
    }
  }

  enum Anchor : U32 {
    AnchorNone = 0,
    AnchorTop = 1,
    AnchorBottom = 2,
    AnchorLeft = 3,
    AnchorRight = 4,
    AnchorTopLeft = 5,
    AnchorBottomLeft = 6,
    AnchorTopRight = 7,
    AnchorBottomRight = 8,
  };

  static StrView AnchorToStr(U32 value) {
    switch (value) {
    case 0: return "None"sv;
    case 1: return "Top"sv;
    case 2: return "Bottom"sv;
    case 3: return "Left"sv;
    case 4: return "Right"sv;
    case 5: return "TopLeft"sv;
    case 6: return "BottomLeft"sv;
    case 7: return "TopRight"sv;
    case 8: return "BottomRight"sv;
    default: return "UnknownAnchor"sv;
    }
  }

  enum Gravity : U32 {
    GravityNone = 0,
    GravityTop = 1,
    GravityBottom = 2,
    GravityLeft = 3,
    GravityRight = 4,
    GravityTopLeft = 5,
    GravityBottomLeft = 6,
    GravityTopRight = 7,
    GravityBottomRight = 8,
  };

  static StrView GravityToStr(U32 value) {
    switch (value) {
    case 0: return "None"sv;
    case 1: return "Top"sv;
    case 2: return "Bottom"sv;
    case 3: return "Left"sv;
    case 4: return "Right"sv;
    case 5: return "TopLeft"sv;
    case 6: return "BottomLeft"sv;
    case 7: return "TopRight"sv;
    case 8: return "BottomRight"sv;
    default: return "UnknownGravity"sv;
    }
  }

  // Constraint adjustments
  //
  // The constraint adjustment value define ways the compositor will adjust
  // the position of the surface, if the unadjusted position would result
  // in the surface being partly constrained.
  // 
  // Whether a surface is considered 'constrained' is left to the compositor
  // to determine. For example, the surface may be partly outside the
  // compositor's defined 'work area', thus necessitating the child surface's
  // position be adjusted until it is entirely inside the work area.
  // 
  // The adjustments can be combined, according to a defined precedence: 1)
  // Flip, 2) Slide, 3) Resize.
  enum ConstraintAdjustment : U32 {
    ConstraintAdjustmentNone = 0,
    ConstraintAdjustmentSlideX = 1,
    ConstraintAdjustmentSlideY = 2,
    ConstraintAdjustmentFlipX = 4,
    ConstraintAdjustmentFlipY = 8,
    ConstraintAdjustmentResizeX = 16,
    ConstraintAdjustmentResizeY = 32,
  };

  static StrView ConstraintAdjustmentToStr(U32 value) {
    switch (value) {
    case 0: return "None"sv;
    case 1: return "SlideX"sv;
    case 2: return "SlideY"sv;
    case 4: return "FlipX"sv;
    case 8: return "FlipY"sv;
    case 16: return "ResizeX"sv;
    case 32: return "ResizeY"sv;
    default: return "UnknownConstraintAdjustment"sv;
    }
  }

  // Destroy the xdg_positioner object
  //
  // Notify the compositor that the xdg_positioner will no longer be used.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Set the size of the to-be positioned rectangle
  //
  // Set the size of the surface that is to be positioned with the positioner
  // object. The size is in surface-local coordinates and corresponds to the
  // window geometry. See xdg_surface.set_window_geometry.
  // 
  // If a zero or negative size is set the invalid_input error is raised.
  void OnSetSize(I32 width, I32 height);

  // Set the anchor rectangle within the parent surface
  //
  // Specify the anchor rectangle within the parent surface that the child
  // surface will be placed relative to. The rectangle is relative to the
  // window geometry as defined by xdg_surface.set_window_geometry of the
  // parent surface.
  // 
  // When the xdg_positioner object is used to position a child surface, the
  // anchor rectangle may not extend outside the window geometry of the
  // positioned child's parent surface.
  // 
  // If a negative size is set the invalid_input error is raised.
  void OnSetAnchorRect(I32 x, I32 y, I32 width, I32 height);

  // Set anchor rectangle anchor
  //
  // Defines the anchor point for the anchor rectangle. The specified anchor
  // is used derive an anchor point that the child surface will be
  // positioned relative to. If a corner anchor is set (e.g. 'top_left' or
  // 'bottom_right'), the anchor point will be at the specified corner;
  // otherwise, the derived anchor point will be centered on the specified
  // edge, or in the center of the anchor rectangle if no edge is specified.
  void OnSetAnchor(enum Anchor anchor);

  // Set child surface gravity
  //
  // Defines in what direction a surface should be positioned, relative to
  // the anchor point of the parent surface. If a corner gravity is
  // specified (e.g. 'bottom_right' or 'top_left'), then the child surface
  // will be placed towards the specified gravity; otherwise, the child
  // surface will be centered over the anchor point on any axis that had no
  // gravity specified. If the gravity is not in the ‘gravity’ enum, an
  // invalid_input error is raised.
  void OnSetGravity(enum Gravity gravity);

  // Set the adjustment to be done when constrained
  //
  // Specify how the window should be positioned if the originally intended
  // position caused the surface to be constrained, meaning at least
  // partially outside positioning boundaries set by the compositor. The
  // adjustment is set by constructing a bitmask describing the adjustment to
  // be made when the surface is constrained on that axis.
  // 
  // If no bit for one axis is set, the compositor will assume that the child
  // surface should not change its position on that axis when constrained.
  // 
  // If more than one bit for one axis is set, the order of how adjustments
  // are applied is specified in the corresponding adjustment descriptions.
  // 
  // The default adjustment is none.
  void OnSetConstraintAdjustment(enum ConstraintAdjustment constraint_adjustment);

  // Set surface position offset
  //
  // Specify the surface position offset relative to the position of the
  // anchor on the anchor rectangle and the anchor on the surface. For
  // example if the anchor of the anchor rectangle is at (x, y), the surface
  // has the gravity bottom|right, and the offset is (ox, oy), the calculated
  // surface position will be (x + ox, y + oy). The offset position of the
  // surface is the one used for constraint testing. See
  // set_constraint_adjustment.
  // 
  // An example use case is placing a popup menu on top of a user interface
  // element, while aligning the user interface element of the parent surface
  // with some user interface element placed somewhere in the popup surface.
  void OnSetOffset(I32 x, I32 y);

  // Continuously reconstrain the surface
  //
  // When set reactive, the surface is reconstrained if the conditions used
  // for constraining changed, e.g. the parent window moved.
  // 
  // If the conditions changed and the popup was reconstrained, an
  // xdg_popup.configure event is sent with updated geometry, followed by an
  // xdg_surface.configure event.
  void OnSetReactive();

  // 
  //
  // Set the parent window geometry the compositor should use when
  // positioning the popup. The compositor may use this information to
  // determine the future state the popup should be constrained using. If
  // this doesn't match the dimension of the parent the popup is eventually
  // positioned against, the behavior is undefined.
  // 
  // The arguments are given in the surface-local coordinate space.
  void OnSetParentSize(I32 parent_width, I32 parent_height);

  // Set parent configure this is a response to
  //
  // Set the serial of an xdg_surface.configure event this positioner will be
  // used in response to. The compositor may use this information together
  // with set_parent_size to determine what future state the popup should be
  // constrained using.
  void OnSetParentConfigure(U32 serial);
  // clang-format on: generated XdgPositioner from xdg-shell.xml

  SkISize size = {};
  SkIRect anchor_rect = SkIRect::MakeEmpty();
  U32 anchor = 0;
  U32 gravity = 0;
  SkIPoint offset = {};
  U32 constraint_adjustment = 0;
};

// clang-format off: generated XdgSurface from xdg-shell.xml

// Desktop user interface surface base interface
//
//     An interface that may be implemented by a wl_surface, for
//     implementations that provide a desktop-style user interface.
// 
//     It provides a base set of functionality required to construct user
//     interface elements requiring management by the compositor, such as
//     toplevel windows, menus, etc. The types of functionality are split into
//     xdg_surface roles.
// 
//     Creating an xdg_surface does not set the role for a wl_surface. In order
//     to map an xdg_surface, the client must create a role-specific object
//     using, e.g., get_toplevel, get_popup. The wl_surface for any given
//     xdg_surface can have at most one role, and may not be assigned any role
//     not based on xdg_surface.
// 
//     A role must be assigned before any other requests are made to the
//     xdg_surface object.
// 
//     The client must call wl_surface.commit on the corresponding wl_surface
//     for the xdg_surface state to take effect.
// 
//     Creating an xdg_surface from a wl_surface which has a buffer attached or
//     committed is a client error, and any attempts by a client to attach or
//     manipulate a buffer prior to the first xdg_surface.configure call must
//     also be treated as errors.
// 
//     After creating a role-specific object and setting it up (e.g. by sending
//     the title, app ID, size constraints, parent, etc), the client must
//     perform an initial commit without any buffer attached. The compositor
//     will reply with initial wl_surface state such as
//     wl_surface.preferred_buffer_scale followed by an xdg_surface.configure
//     event. The client must acknowledge it and is then allowed to attach a
//     buffer to map the surface.
// 
//     Mapping an xdg_surface-based role surface is defined as making it
//     possible for the surface to be shown by the compositor. Note that
//     a mapped surface is not guaranteed to be visible once it is mapped.
// 
//     For an xdg_surface to be mapped by the compositor, the following
//     conditions must be met:
//     (1) the client has assigned an xdg_surface-based role to the surface
//     (2) the client has set and committed the xdg_surface state and the
//  role-dependent state to the surface
//     (3) the client has committed a buffer to the surface
// 
//     A newly-unmapped surface is considered to have met condition (1) out
//     of the 3 required conditions for mapping a surface if its role surface
//     has not been destroyed, i.e. the client must perform the initial commit
//     again before attaching a buffer.
struct XdgSurface : Common {
  static constexpr int Version = 7;
  static constexpr Kind Kind = KindXdgSurface;

  static Colony<XdgSurface> colony;

  // Do not use directly. Instead use `ColonyMake`
  XdgSurface(U32 id, Client& client) : Common(Kind, id, client) {}

  static XdgSurface& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorNotConstructed = 1, // Surface was not fully constructed
    ErrorAlreadyConstructed = 2, // Surface was already constructed
    ErrorUnconfiguredBuffer = 3, // Attaching a buffer to an unconfigured surface
    ErrorInvalidSerial = 4, // Invalid serial number when acking a configure event
    ErrorInvalidSize = 5, // Width or height was zero or negative
    ErrorDefunctRoleObject = 6, // Surface was destroyed before its role object
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 1: return "NotConstructed"sv;
    case 2: return "AlreadyConstructed"sv;
    case 3: return "UnconfiguredBuffer"sv;
    case 4: return "InvalidSerial"sv;
    case 5: return "InvalidSize"sv;
    case 6: return "DefunctRoleObject"sv;
    default: return "UnknownError"sv;
    }
  }

  // Destroy the xdg_surface
  //
  // Destroy the xdg_surface object. An xdg_surface must only be destroyed
  // after its role object has been destroyed, otherwise
  // a defunct_role_object error is raised.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Assign the xdg_toplevel surface role
  //
  // This creates an xdg_toplevel object for the given xdg_surface and gives
  // the associated wl_surface the xdg_toplevel role.
  // 
  // See the documentation of xdg_toplevel for more details about what an
  // xdg_toplevel is and how it is used.
  void OnGetToplevel(XdgToplevel& id);

  // Assign the xdg_popup surface role
  //
  // This creates an xdg_popup object for the given xdg_surface and gives
  // the associated wl_surface the xdg_popup role.
  // 
  // If null is passed as a parent, a parent surface must be specified using
  // some other protocol, before committing the initial state.
  // 
  // See the documentation of xdg_popup for more details about what an
  // xdg_popup is and how it is used.
  void OnGetPopup(XdgPopup& id, XdgSurface* parent, XdgPositioner& positioner);

  // Set the new window geometry
  //
  // The window geometry of a surface is its "visible bounds" from the
  // user's perspective. Client-side decorations often have invisible
  // portions like drop-shadows which should be ignored for the
  // purposes of aligning, placing and constraining windows. Note that
  // in some situations, compositors may clip rendering to the window
  // geometry, so the client should avoid putting functional elements
  // outside of it.
  // 
  // The window geometry is double-buffered state, see wl_surface.commit.
  // 
  // When maintaining a position, the compositor should treat the (x, y)
  // coordinate of the window geometry as the top left corner of the window.
  // A client changing the (x, y) window geometry coordinate should in
  // general not alter the position of the window.
  // 
  // Once the window geometry of the surface is set, it is not possible to
  // unset it, and it will remain the same until set_window_geometry is
  // called again, even if a new subsurface or buffer is attached.
  // 
  // If never set, the value is the full bounds of the surface,
  // including any subsurfaces. This updates dynamically on every
  // commit. This unset is meant for extremely simple clients.
  // 
  // The arguments are given in the surface-local coordinate space of
  // the wl_surface associated with this xdg_surface, and may extend outside
  // of the wl_surface itself to mark parts of the subsurface tree as part of
  // the window geometry.
  // 
  // When applied, the effective window geometry will be the set window
  // geometry clamped to the bounding rectangle of the combined
  // geometry of the surface of the xdg_surface and the associated
  // subsurfaces.
  // 
  // The effective geometry will not be recalculated unless a new call to
  // set_window_geometry is done and the new pending surface state is
  // subsequently applied.
  // 
  // The width and height of the effective window geometry must be
  // greater than zero. Setting an invalid size will raise an
  // invalid_size error.
  void OnSetWindowGeometry(I32 x, I32 y, I32 width, I32 height);

  // Ack a configure event
  //
  // When a configure event is received, if a client commits the
  // surface in response to the configure event, then the client
  // must make an ack_configure request sometime before the commit
  // request, passing along the serial of the configure event.
  // 
  // For instance, for toplevel surfaces the compositor might use this
  // information to move a surface to the top left only when the client has
  // drawn itself for the maximized or fullscreen state.
  // 
  // If the client receives multiple configure events before it
  // can respond to one, it only has to ack the last configure event.
  // Acking a configure event that was never sent raises an invalid_serial
  // error.
  // 
  // A client is not required to commit immediately after sending
  // an ack_configure request - it may even ack_configure several times
  // before its next surface commit.
  // 
  // A client may send multiple ack_configure requests before committing, but
  // only the last request sent before a commit indicates which configure
  // event the client really is responding to.
  // 
  // Sending an ack_configure request consumes the serial number sent with
  // the request, as well as serial numbers sent by all configure events
  // sent on this xdg_surface prior to the configure event referenced by
  // the committed serial.
  // 
  // It is an error to issue multiple ack_configure requests referencing a
  // serial from the same configure event, or to issue an ack_configure
  // request referencing a serial from a configure event issued before the
  // event identified by the last ack_configure request for the same
  // xdg_surface. Doing so will raise an invalid_serial error.
  void OnAckConfigure(U32 serial);

  // Suggest a surface change
  //
  // The configure event marks the end of a configure sequence. A configure
  // sequence is a set of one or more events configuring the state of the
  // xdg_surface, including the final xdg_surface.configure event.
  // 
  // Where applicable, xdg_surface surface roles will during a configure
  // sequence extend this event as a latched state sent as events before the
  // xdg_surface.configure event. Such events should be considered to make up
  // a set of atomically applied configuration states, where the
  // xdg_surface.configure commits the accumulated state.
  // 
  // Clients should arrange their surface for the new states, and then send
  // an ack_configure request with the serial sent in this configure event at
  // some point before committing the new surface.
  // 
  // If the client receives multiple configure events before it can respond
  // to one, it is free to discard all but the last event it received.
  void Configure(U32 serial);
  // clang-format on: generated XdgSurface from xdg-shell.xml

  MortalPtr<Surface> surface;
  MortalPtr<XdgToplevel> toplevel;
  MortalPtr<XdgPopup> popup;
  Vec<XdgPopup*> child_popups;  // oldest first
  SkIRect geo = SkIRect::MakeEmpty();
  U32 last_configure_serial = 0;
  U32 version = 1;  // inherited from the xdg_wm_base, propagated on to the toplevel
  bool initial_configure_sent = false;
};

// clang-format off: generated XdgToplevel from xdg-shell.xml

// Toplevel surface
//
// This interface defines an xdg_surface role which allows a surface to,
// among other things, set window-like properties such as maximize,
// fullscreen, and minimize, set application-specific metadata like title and
// id, and well as trigger user interactive operations such as interactive
// resize and move.
// 
// A xdg_toplevel by default is responsible for providing the full intended
// visual representation of the toplevel, which depending on the window
// state, may mean things like a title bar, window controls and drop shadow.
// 
// Unmapping an xdg_toplevel means that the surface cannot be shown
// by the compositor until it is explicitly mapped again.
// All active operations (e.g., move, resize) are canceled and all
// attributes (e.g. title, state, stacking, ...) are discarded for
// an xdg_toplevel surface when it is unmapped. The xdg_toplevel returns to
// the state it had right after xdg_surface.get_toplevel. The client
// can re-map the toplevel by performing a commit without any buffer
// attached, waiting for a configure event and handling it as usual (see
// xdg_surface description).
// 
// Attaching a null buffer to a toplevel unmaps the surface.
struct XdgToplevel : Common {
  static constexpr int Version = 7;
  static constexpr Kind Kind = KindXdgToplevel;

  static Colony<XdgToplevel> colony;

  // Do not use directly. Instead use `ColonyMake`
  XdgToplevel(U32 id, Client& client) : Common(Kind, id, client) {}

  static XdgToplevel& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorInvalidResizeEdge = 0, // Provided value is         not a valid variant of the resize_edge enum
    ErrorInvalidParent = 1, // Invalid parent toplevel
    ErrorInvalidSize = 2, // Client provided an invalid min or max size
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "InvalidResizeEdge"sv;
    case 1: return "InvalidParent"sv;
    case 2: return "InvalidSize"sv;
    default: return "UnknownError"sv;
    }
  }

  // Edge values for resizing
  //
  // These values are used to indicate which edge of a surface
  // is being dragged in a resize operation.
  enum ResizeEdge : U32 {
    ResizeEdgeNone = 0,
    ResizeEdgeTop = 1,
    ResizeEdgeBottom = 2,
    ResizeEdgeLeft = 4,
    ResizeEdgeTopLeft = 5,
    ResizeEdgeBottomLeft = 6,
    ResizeEdgeRight = 8,
    ResizeEdgeTopRight = 9,
    ResizeEdgeBottomRight = 10,
  };

  static StrView ResizeEdgeToStr(U32 value) {
    switch (value) {
    case 0: return "None"sv;
    case 1: return "Top"sv;
    case 2: return "Bottom"sv;
    case 4: return "Left"sv;
    case 5: return "TopLeft"sv;
    case 6: return "BottomLeft"sv;
    case 8: return "Right"sv;
    case 9: return "TopRight"sv;
    case 10: return "BottomRight"sv;
    default: return "UnknownResizeEdge"sv;
    }
  }

  // Types of state on the surface
  //
  // The different state values used on the surface. This is designed for
  // state values like maximized, fullscreen. It is paired with the
  // configure event to ensure that both the client and the compositor
  // setting the state can be synchronized.
  // 
  // States set in this way are double-buffered, see wl_surface.commit.
  enum State : U32 {
    StateMaximized = 1, // The surface is maximized
    StateFullscreen = 2, // The surface is fullscreen
    StateResizing = 3, // The surface is being resized
    StateActivated = 4, // The surface is now activated
    StateTiledLeft = 5,
    StateTiledRight = 6,
    StateTiledTop = 7,
    StateTiledBottom = 8,
    StateSuspended = 9,
    StateConstrainedLeft = 10,
    StateConstrainedRight = 11,
    StateConstrainedTop = 12,
    StateConstrainedBottom = 13,
  };

  static StrView StateToStr(U32 value) {
    switch (value) {
    case 1: return "Maximized"sv;
    case 2: return "Fullscreen"sv;
    case 3: return "Resizing"sv;
    case 4: return "Activated"sv;
    case 5: return "TiledLeft"sv;
    case 6: return "TiledRight"sv;
    case 7: return "TiledTop"sv;
    case 8: return "TiledBottom"sv;
    case 9: return "Suspended"sv;
    case 10: return "ConstrainedLeft"sv;
    case 11: return "ConstrainedRight"sv;
    case 12: return "ConstrainedTop"sv;
    case 13: return "ConstrainedBottom"sv;
    default: return "UnknownState"sv;
    }
  }

  enum WmCapabilities : U32 {
    WmCapabilitiesWindowMenu = 1, // Show_window_menu is available
    WmCapabilitiesMaximize = 2, // Set_maximized and unset_maximized are available
    WmCapabilitiesFullscreen = 3, // Set_fullscreen and unset_fullscreen are available
    WmCapabilitiesMinimize = 4, // Set_minimized is available
  };

  static StrView WmCapabilitiesToStr(U32 value) {
    switch (value) {
    case 1: return "WindowMenu"sv;
    case 2: return "Maximize"sv;
    case 3: return "Fullscreen"sv;
    case 4: return "Minimize"sv;
    default: return "UnknownWmCapabilities"sv;
    }
  }

  // Destroy the xdg_toplevel
  //
  // This request destroys the role surface and unmaps the surface;
  // see "Unmapping" behavior in interface section for details.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Set the parent of this surface
  //
  // Set the "parent" of this surface. This surface should be stacked
  // above the parent surface and all other ancestor surfaces.
  // 
  // Parent surfaces should be set on dialogs, toolboxes, or other
  // "auxiliary" surfaces, so that the parent is raised when the dialog
  // is raised.
  // 
  // Setting a null parent for a child surface unsets its parent. Setting
  // a null parent for a surface which currently has no parent is a no-op.
  // 
  // Only mapped surfaces can have child surfaces. Setting a parent which
  // is not mapped is equivalent to setting a null parent. If a surface
  // becomes unmapped, its children's parent is set to the parent of
  // the now-unmapped surface. If the now-unmapped surface has no parent,
  // its children's parent is unset. If the now-unmapped surface becomes
  // mapped again, its parent-child relationship is not restored.
  // 
  // The parent toplevel must not be one of the child toplevel's
  // descendants, and the parent must be different from the child toplevel,
  // otherwise the invalid_parent protocol error is raised.
  void OnSetParent(XdgToplevel* parent);

  // Set surface title
  //
  // Set a short title for the surface.
  // 
  // This string may be used to identify the surface in a task bar,
  // window list, or other user interface elements provided by the
  // compositor.
  // 
  // The string must be encoded in UTF-8.
  void OnSetTitle(StrView title);

  // Set application id
  //
  // Set an application identifier for the surface.
  // 
  // The app ID identifies the general class of applications to which
  // the surface belongs. The compositor can use this to group multiple
  // surfaces together, or to determine how to launch a new application.
  // 
  // For D-Bus activatable applications, the app ID is used as the D-Bus
  // service name.
  // 
  // The compositor shell will try to group application surfaces together
  // by their app ID. As a best practice, it is suggested to select app
  // ID's that match the basename of the application's .desktop file.
  // For example, "org.freedesktop.FooViewer" where the .desktop file is
  // "org.freedesktop.FooViewer.desktop".
  // 
  // Like other properties, a set_app_id request can be sent after the
  // xdg_toplevel has been mapped to update the property.
  // 
  // See the desktop-entry specification [0] for more details on
  // application identifiers and how they relate to well-known D-Bus
  // names and .desktop files.
  // 
  // [0] https://standards.freedesktop.org/desktop-entry-spec/
  void OnSetAppId(StrView app_id);

  // Show the window menu
  //
  // Clients implementing client-side decorations might want to show
  // a context menu when right-clicking on the decorations, giving the
  // user a menu that they can use to maximize or minimize the window.
  // 
  // This request asks the compositor to pop up such a window menu at
  // the given position, relative to the local surface coordinates of
  // the parent surface. There are no guarantees as to what menu items
  // the window menu contains, or even if a window menu will be drawn
  // at all.
  // 
  // This request must be used in response to some sort of user action
  // like a button press, key press, or touch down event.
  void OnShowWindowMenu(Seat& seat, U32 serial, I32 x, I32 y);

  // Start an interactive move
  //
  // Start an interactive, user-driven move of the surface.
  // 
  // This request must be used in response to some sort of user action
  // like a button press, key press, or touch down event. The passed
  // serial is used to determine the type of interactive move (touch,
  // pointer, etc).
  // 
  // The server may ignore move requests depending on the state of
  // the surface (e.g. fullscreen or maximized), or if the passed serial
  // is no longer valid.
  // 
  // If triggered, the surface will lose the focus of the device
  // (wl_pointer, wl_touch, etc) used for the move. It is up to the
  // compositor to visually indicate that the move is taking place, such as
  // updating a pointer cursor, during the move. There is no guarantee
  // that the device focus will return when the move is completed.
  void OnMove(Seat& seat, U32 serial);

  // Start an interactive resize
  //
  // Start a user-driven, interactive resize of the surface.
  // 
  // This request must be used in response to some sort of user action
  // like a button press, key press, or touch down event. The passed
  // serial is used to determine the type of interactive resize (touch,
  // pointer, etc).
  // 
  // The server may ignore resize requests depending on the state of
  // the surface (e.g. fullscreen or maximized).
  // 
  // If triggered, the client will receive configure events with the
  // "resize" state enum value and the expected sizes. See the "resize"
  // enum value for more details about what is required. The client
  // must also acknowledge configure events using "ack_configure". After
  // the resize is completed, the client will receive another "configure"
  // event without the resize state.
  // 
  // If triggered, the surface also will lose the focus of the device
  // (wl_pointer, wl_touch, etc) used for the resize. It is up to the
  // compositor to visually indicate that the resize is taking place,
  // such as updating a pointer cursor, during the resize. There is no
  // guarantee that the device focus will return when the resize is
  // completed.
  // 
  // The edges parameter specifies how the surface should be resized, and
  // is one of the values of the resize_edge enum. Values not matching
  // a variant of the enum will cause the invalid_resize_edge protocol error.
  // The compositor may use this information to update the surface position
  // for example when dragging the top left corner. The compositor may also
  // use this information to adapt its behavior, e.g. choose an appropriate
  // cursor image.
  void OnResize(Seat& seat, U32 serial, enum ResizeEdge edges);

  // Set the maximum size
  //
  // Set a maximum size for the window.
  // 
  // The client can specify a maximum size so that the compositor does
  // not try to configure the window beyond this size.
  // 
  // The width and height arguments are in window geometry coordinates.
  // See xdg_surface.set_window_geometry.
  // 
  // Values set in this way are double-buffered, see wl_surface.commit.
  // 
  // The compositor can use this information to allow or disallow
  // different states like maximize or fullscreen and draw accurate
  // animations.
  // 
  // Similarly, a tiling window manager may use this information to
  // place and resize client windows in a more effective way.
  // 
  // The client should not rely on the compositor to obey the maximum
  // size. The compositor may decide to ignore the values set by the
  // client and request a larger size.
  // 
  // If never set, or a value of zero in the request, means that the
  // client has no expected maximum size in the given dimension.
  // As a result, a client wishing to reset the maximum size
  // to an unspecified state can use zero for width and height in the
  // request.
  // 
  // Requesting a maximum size to be smaller than the minimum size of
  // a surface is illegal and will result in an invalid_size error.
  // 
  // The width and height must be greater than or equal to zero. Using
  // strictly negative values for width or height will result in a
  // invalid_size error.
  void OnSetMaxSize(I32 width, I32 height);

  // Set the minimum size
  //
  // Set a minimum size for the window.
  // 
  // The client can specify a minimum size so that the compositor does
  // not try to configure the window below this size.
  // 
  // The width and height arguments are in window geometry coordinates.
  // See xdg_surface.set_window_geometry.
  // 
  // Values set in this way are double-buffered, see wl_surface.commit.
  // 
  // The compositor can use this information to allow or disallow
  // different states like maximize or fullscreen and draw accurate
  // animations.
  // 
  // Similarly, a tiling window manager may use this information to
  // place and resize client windows in a more effective way.
  // 
  // The client should not rely on the compositor to obey the minimum
  // size. The compositor may decide to ignore the values set by the
  // client and request a smaller size.
  // 
  // If never set, or a value of zero in the request, means that the
  // client has no expected minimum size in the given dimension.
  // As a result, a client wishing to reset the minimum size
  // to an unspecified state can use zero for width and height in the
  // request.
  // 
  // Requesting a minimum size to be larger than the maximum size of
  // a surface is illegal and will result in an invalid_size error.
  // 
  // The width and height must be greater than or equal to zero. Using
  // strictly negative values for width and height will result in a
  // invalid_size error.
  void OnSetMinSize(I32 width, I32 height);

  // Maximize the window
  //
  // Maximize the surface.
  // 
  // After requesting that the surface should be maximized, the compositor
  // will respond by emitting a configure event. Whether this configure
  // actually sets the window maximized is subject to compositor policies.
  // The client must then update its content, drawing in the configured
  // state. The client must also acknowledge the configure when committing
  // the new content (see ack_configure).
  // 
  // It is up to the compositor to decide how and where to maximize the
  // surface, for example which output and what region of the screen should
  // be used.
  // 
  // If the surface was already maximized, the compositor will still emit
  // a configure event with the "maximized" state.
  // 
  // If the surface is in a fullscreen state, this request has no direct
  // effect. It may alter the state the surface is returned to when
  // unmaximized unless overridden by the compositor.
  void OnSetMaximized();

  // Unmaximize the window
  //
  // Unmaximize the surface.
  // 
  // After requesting that the surface should be unmaximized, the compositor
  // will respond by emitting a configure event. Whether this actually
  // un-maximizes the window is subject to compositor policies.
  // If available and applicable, the compositor will include the window
  // geometry dimensions the window had prior to being maximized in the
  // configure event. The client must then update its content, drawing it in
  // the configured state. The client must also acknowledge the configure
  // when committing the new content (see ack_configure).
  // 
  // It is up to the compositor to position the surface after it was
  // unmaximized; usually the position the surface had before maximizing, if
  // applicable.
  // 
  // If the surface was already not maximized, the compositor will still
  // emit a configure event without the "maximized" state.
  // 
  // If the surface is in a fullscreen state, this request has no direct
  // effect. It may alter the state the surface is returned to when
  // unmaximized unless overridden by the compositor.
  void OnUnsetMaximized();

  // Set the window as fullscreen on an output
  //
  // Make the surface fullscreen.
  // 
  // After requesting that the surface should be fullscreened, the
  // compositor will respond by emitting a configure event. Whether the
  // client is actually put into a fullscreen state is subject to compositor
  // policies. The client must also acknowledge the configure when
  // committing the new content (see ack_configure).
  // 
  // The output passed by the request indicates the client's preference as
  // to which display it should be set fullscreen on. If this value is NULL,
  // it's up to the compositor to choose which display will be used to map
  // this surface.
  // 
  // If the surface doesn't cover the whole output, the compositor will
  // position the surface in the center of the output and compensate with
  // with border fill covering the rest of the output. The content of the
  // border fill is undefined, but should be assumed to be in some way that
  // attempts to blend into the surrounding area (e.g. solid black).
  // 
  // If the fullscreened surface is not opaque, the compositor must make
  // sure that other screen content not part of the same surface tree (made
  // up of subsurfaces, popups or similarly coupled surfaces) are not
  // visible below the fullscreened surface.
  void OnSetFullscreen(Output* output);

  // Unset the window as fullscreen
  //
  // Make the surface no longer fullscreen.
  // 
  // After requesting that the surface should be unfullscreened, the
  // compositor will respond by emitting a configure event.
  // Whether this actually removes the fullscreen state of the client is
  // subject to compositor policies.
  // 
  // Making a surface unfullscreen sets states for the surface based on the following:
  // * the state(s) it may have had before becoming fullscreen
  // * any state(s) decided by the compositor
  // * any state(s) requested by the client while the surface was fullscreen
  // 
  // The compositor may include the previous window geometry dimensions in
  // the configure event, if applicable.
  // 
  // The client must also acknowledge the configure when committing the new
  // content (see ack_configure).
  void OnUnsetFullscreen();

  // Set the window as minimized
  //
  // Request that the compositor minimize your surface. There is no
  // way to know if the surface is currently minimized, nor is there
  // any way to unset minimization on this surface.
  // 
  // If you are looking to throttle redrawing when minimized, please
  // instead use the wl_surface.frame event for this, as this will
  // also work with live previews on windows in Alt-Tab, Expose or
  // similar compositor features.
  void OnSetMinimized();

  // Suggest a surface change
  //
  // This configure event asks the client to resize its toplevel surface or
  // to change its state. The configured state should not be applied
  // immediately. See xdg_surface.configure for details.
  // 
  // The width and height arguments specify a hint to the window
  // about how its surface should be resized in window geometry
  // coordinates. See set_window_geometry.
  // 
  // If the width or height arguments are zero, it means the client
  // should decide its own window dimension. This may happen when the
  // compositor needs to configure the state of the surface but doesn't
  // have any information about any previous or expected dimension.
  // 
  // The states listed in the event specify how the width/height
  // arguments should be interpreted, and possibly how it should be
  // drawn.
  // 
  // Clients must send an ack_configure in response to this event. See
  // xdg_surface.configure and xdg_surface.ack_configure for details.
  void Configure(I32 width, I32 height, Span<> states);

  // Surface wants to be closed
  //
  // The close event is sent by the compositor when the user
  // wants the surface to be closed. This should be equivalent to
  // the user clicking the close button in client-side decorations,
  // if your application has any.
  // 
  // This is only a request that the user intends to close the
  // window. The client may choose to ignore this request, or show
  // a dialog to ask the user to save their data, etc.
  void Close();

  // Recommended window geometry bounds
  //
  // The configure_bounds event may be sent prior to a xdg_toplevel.configure
  // event to communicate the bounds a window geometry size is recommended
  // to constrain to.
  // 
  // The passed width and height are in surface coordinate space. If width
  // and height are 0, it means bounds is unknown and equivalent to as if no
  // configure_bounds event was ever sent for this surface.
  // 
  // The bounds can for example correspond to the size of a monitor excluding
  // any panels or other shell components, so that a surface isn't created in
  // a way that it cannot fit.
  // 
  // The bounds may change at any point, and in such a case, a new
  // xdg_toplevel.configure_bounds will be sent, followed by
  // xdg_toplevel.configure and xdg_surface.configure.
  void ConfigureBounds(I32 width, I32 height);

  // Compositor capabilities
  //
  // This event advertises the capabilities supported by the compositor. If
  // a capability isn't supported, clients should hide or disable the UI
  // elements that expose this functionality. For instance, if the
  // compositor doesn't advertise support for minimized toplevels, a button
  // triggering the set_minimized request should not be displayed.
  // 
  // The compositor will ignore requests it doesn't support. For instance,
  // a compositor which doesn't advertise support for minimized will ignore
  // set_minimized requests.
  // 
  // Compositors must send this event once before the first
  // xdg_surface.configure event. When the capabilities change, compositors
  // must send this event again and then send an xdg_surface.configure
  // event.
  // 
  // The configured state should not be applied immediately. See
  // xdg_surface.configure for details.
  // 
  // The capabilities are sent as an array of 32-bit unsigned integers in
  // native endianness.
  void WmCapabilities(Span<> capabilities);
  // clang-format on: generated XdgToplevel from xdg-shell.xml

  MortalPtr<XdgSurface> xdg;
  Str title;
  Str app_id;
  bool mapped = false;
  U32 version = 1;  // inherited from the xdg_wm_base; gates wm_capabilities
  I64 pid = 0;
  WeakPtr<ReferenceCounted> window;  // a library::WaylandWindow
  MortalPtr<ZxdgToplevelDecorationV1> decoration;
  std::unique_ptr<mux::Timer> sigterm_timer;
  ~XdgToplevel();
};

// clang-format off: generated XdgPopup from xdg-shell.xml

// Short-lived, popup surfaces for menus
//
// A popup surface is a short-lived, temporary surface. It can be used to
// implement for example menus, popovers, tooltips and other similar user
// interface concepts.
// 
// A popup can be made to take an explicit grab. See xdg_popup.grab for
// details.
// 
// When the popup is dismissed, a popup_done event will be sent out, and at
// the same time the surface will be unmapped. See the xdg_popup.popup_done
// event for details.
// 
// Explicitly destroying the xdg_popup object will also dismiss the popup and
// unmap the surface. Clients that want to dismiss the popup when another
// surface of their own is clicked should dismiss the popup using the destroy
// request.
// 
// A newly created xdg_popup will be stacked on top of all previously created
// xdg_popup surfaces associated with the same xdg_toplevel.
// 
// The parent of an xdg_popup must be mapped (see the xdg_surface
// description) before the xdg_popup itself.
// 
// The client must call wl_surface.commit on the corresponding wl_surface
// for the xdg_popup state to take effect.
struct XdgPopup : Common {
  static constexpr int Version = 7;
  static constexpr Kind Kind = KindXdgPopup;

  static Colony<XdgPopup> colony;

  // Do not use directly. Instead use `ColonyMake`
  XdgPopup(U32 id, Client& client) : Common(Kind, id, client) {}

  static XdgPopup& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorInvalidGrab = 0, // Tried to grab after being mapped
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "InvalidGrab"sv;
    default: return "UnknownError"sv;
    }
  }

  // Remove xdg_popup interface
  //
  // This destroys the popup. Explicitly destroying the xdg_popup
  // object will also dismiss the popup, and unmap the surface.
  // 
  // If this xdg_popup is not the "topmost" popup, the
  // xdg_wm_base.not_the_topmost_popup protocol error will be sent.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Make the popup take an explicit grab
  //
  // This request makes the created popup take an explicit grab. An explicit
  // grab will be dismissed when the user dismisses the popup, or when the
  // client destroys the xdg_popup. This can be done by the user clicking
  // outside the surface, using the keyboard, or even locking the screen
  // through closing the lid or a timeout.
  // 
  // If the compositor denies the grab, the popup will be immediately
  // dismissed.
  // 
  // This request must be used in response to some sort of user action like a
  // button press, key press, or touch down event. The serial number of the
  // event should be passed as 'serial'.
  // 
  // The parent of a grabbing popup must either be an xdg_toplevel surface or
  // another xdg_popup with an explicit grab. If the parent is another
  // xdg_popup it means that the popups are nested, with this popup now being
  // the topmost popup.
  // 
  // Nested popups must be destroyed in the reverse order they were created
  // in, e.g. the only popup you are allowed to destroy at all times is the
  // topmost one.
  // 
  // When compositors choose to dismiss a popup, they may dismiss every
  // nested grabbing popup as well. When a compositor dismisses popups, it
  // will follow the same dismissing order as required from the client.
  // 
  // If the topmost grabbing popup is destroyed, the grab will be returned to
  // the parent of the popup, if that parent previously had an explicit grab.
  // 
  // If the parent is a grabbing popup which has already been dismissed, this
  // popup will be immediately dismissed. If the parent is a popup that did
  // not take an explicit grab, an error will be raised.
  // 
  // During a popup grab, the client owning the grab will receive pointer
  // and touch events for all their surfaces as normal (similar to an
  // "owner-events" grab in X11 parlance), while the top most grabbing popup
  // will always have keyboard focus.
  void OnGrab(Seat& seat, U32 serial);

  // Recalculate the popup's location
  //
  // Reposition an already-mapped popup. The popup will be placed given the
  // details in the passed xdg_positioner object, and a
  // xdg_popup.repositioned followed by xdg_popup.configure and
  // xdg_surface.configure will be emitted in response. Any parameters set
  // by the previous positioner will be discarded.
  // 
  // The passed token will be sent in the corresponding
  // xdg_popup.repositioned event. The new popup position will not take
  // effect until the corresponding configure event is acknowledged by the
  // client. See xdg_popup.repositioned for details. The token itself is
  // opaque, and has no other special meaning.
  // 
  // If multiple reposition requests are sent, the compositor may skip all
  // but the last one.
  // 
  // If the popup is repositioned in response to a configure event for its
  // parent, the client should send an xdg_positioner.set_parent_configure
  // and possibly an xdg_positioner.set_parent_size request to allow the
  // compositor to properly constrain the popup.
  // 
  // If the popup is repositioned together with a parent that is being
  // resized, but not in response to a configure event, the client should
  // send an xdg_positioner.set_parent_size request.
  void OnReposition(XdgPositioner& positioner, U32 token);

  // Configure the popup surface
  //
  // This event asks the popup surface to configure itself given the
  // configuration. The configured state should not be applied immediately.
  // See xdg_surface.configure for details.
  // 
  // The x and y arguments represent the position the popup was placed at
  // given the xdg_positioner rule, relative to the upper left corner of the
  // window geometry of the parent surface.
  // 
  // For version 2 or older, the configure event for an xdg_popup is only
  // ever sent once for the initial configuration. Starting with version 3,
  // it may be sent again if the popup is setup with an xdg_positioner with
  // set_reactive requested, or in response to xdg_popup.reposition requests.
  void Configure(I32 x, I32 y, I32 width, I32 height);

  // Popup interaction is done
  //
  // The popup_done event is sent out when a popup is dismissed by the
  // compositor. The client should destroy the xdg_popup object at this
  // point.
  void PopupDone();

  // Signal the completion of a repositioned request
  //
  // The repositioned event is sent as part of a popup configuration
  // sequence, together with xdg_popup.configure and lastly
  // xdg_surface.configure to notify the completion of a reposition request.
  // 
  // The repositioned event is to notify about the completion of a
  // xdg_popup.reposition request. The token argument is the token passed
  // in the xdg_popup.reposition request.
  // 
  // Immediately after this event is emitted, xdg_popup.configure and
  // xdg_surface.configure will be sent with the updated size and position,
  // as well as a new configure serial.
  // 
  // The client should optionally update the content of the popup, but must
  // acknowledge the new popup configuration for the new position to take
  // effect. See xdg_surface.ack_configure for details.
  void Repositioned(U32 token);
  // clang-format on: generated XdgPopup from xdg-shell.xml

  MortalPtr<XdgSurface> xdg;
  MortalPtr<XdgSurface> parent;
  SkIRect geo = SkIRect::MakeEmpty();
  SkIPoint flipped = {};
  bool flip_x = false, flip_y = false, slide_x = false, slide_y = false;
  ~XdgPopup();
};

// clang-format off: generated ShmPool from wayland.xml

// A shared memory pool
//
// The wl_shm_pool object encapsulates a piece of memory shared
// between the compositor and client.  Through the wl_shm_pool
// object, the client can allocate shared memory wl_buffer objects.
// All objects created through the same pool share the same
// underlying mapped memory. Reusing the mapped memory avoids the
// setup/teardown overhead and is useful when interactively resizing
// a surface or for many small buffers.
struct ShmPool : Common {
  static constexpr int Version = 2;
  static constexpr Kind Kind = KindShmPool;

  static Colony<ShmPool> colony;

  // Do not use directly. Instead use `ColonyMake`
  ShmPool(U32 id, Client& client) : Common(Kind, id, client) {}

  static ShmPool& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Create a buffer from the pool
  //
  // Create a wl_buffer object from the pool.
  // 
  // The buffer is created offset bytes into the pool and has
  // width and height as specified.  The stride argument specifies
  // the number of bytes from the beginning of one row to the beginning
  // of the next.  The format is the pixel format of the buffer and
  // must be one of those advertised through the wl_shm.format event.
  // 
  // A buffer will keep a reference to the pool it was created from
  // so it is valid to destroy the pool immediately after creating
  // a buffer from it.
  void OnCreateBuffer(Buffer& id, I32 offset, I32 width, I32 height, I32 stride, enum Shm::Format format);

  // Destroy the pool
  //
  // Destroy the shared memory pool.
  // 
  // The mmapped memory will be released when all
  // buffers that have been created from this pool
  // are gone.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Change the size of the pool mapping
  //
  // This request will cause the server to remap the backing memory
  // for the pool from the file descriptor passed when the pool was
  // created, but using the new size.  This request can only be
  // used to make the pool bigger.
  // 
  // This request only changes the amount of bytes that are mmapped
  // by the server and does not touch the file corresponding to the
  // file descriptor passed at creation time. It is the client's
  // responsibility to ensure that the file is at least as big as
  // the new pool size.
  void OnResize(I32 size);
  // clang-format on: generated ShmPool from wayland.xml

  FD fd;
  size_t size = 0;
};

// clang-format off: generated DataOffer from wayland.xml

// Offer to transfer data
//
// A wl_data_offer represents a piece of data offered for transfer
// by another client (the source client).  It is used by the
// copy-and-paste and drag-and-drop mechanisms.  The offer
// describes the different mime types that the data can be
// converted to and provides the mechanism for transferring the
// data directly from the source client.
struct DataOffer : Common {
  static constexpr int Version = 3;
  static constexpr Kind Kind = KindDataOffer;

  static Colony<DataOffer> colony;

  // Do not use directly. Instead use `ColonyMake`
  DataOffer(U32 id, Client& client) : Common(Kind, id, client) {}

  static DataOffer& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorInvalidFinish = 0, // Finish request was called untimely
    ErrorInvalidActionMask = 1, // Action mask contains invalid values
    ErrorInvalidAction = 2, // Action argument has an invalid value
    ErrorInvalidOffer = 3, // Offer doesn't accept this request
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "InvalidFinish"sv;
    case 1: return "InvalidActionMask"sv;
    case 2: return "InvalidAction"sv;
    case 3: return "InvalidOffer"sv;
    default: return "UnknownError"sv;
    }
  }

  // Accept one of the offered mime types
  //
  // Indicate that the client can accept the given mime type, or
  // NULL for not accepted.
  // 
  // For objects of version 2 or older, this request is used by the
  // client to give feedback whether the client can receive the given
  // mime type, or NULL if none is accepted; the feedback does not
  // determine whether the drag-and-drop operation succeeds or not.
  // 
  // For objects of version 3 or newer, this request determines the
  // final result of the drag-and-drop operation. If the end result
  // is that no mime types were accepted, the drag-and-drop operation
  // will be cancelled and the corresponding drag source will receive
  // wl_data_source.cancelled. Clients may still use this event in
  // conjunction with wl_data_source.action for feedback.
  void OnAccept(U32 serial, StrView mime_type);

  // Request that the data is transferred
  //
  // To transfer the offered data, the client issues this request
  // and indicates the mime type it wants to receive.  The transfer
  // happens through the passed file descriptor (typically created
  // with the pipe system call).  The source client writes the data
  // in the mime type representation requested and then closes the
  // file descriptor.
  // 
  // The receiving client reads from the read end of the pipe until
  // EOF and then closes its end, at which point the transfer is
  // complete.
  // 
  // This request may happen multiple times for different mime types,
  // both before and after wl_data_device.drop. Drag-and-drop destination
  // clients may preemptively fetch data or examine it more closely to
  // determine acceptance.
  void OnReceive(StrView mime_type, FD&& fd);

  // Destroy data offer
  //
  // Destroy the data offer.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // The offer will no longer be used
  //
  // Notifies the compositor that the drag destination successfully
  // finished the drag-and-drop operation.
  // 
  // Upon receiving this request, the compositor will emit
  // wl_data_source.dnd_finished on the drag source client.
  // 
  // It is a client error to perform other requests than
  // wl_data_offer.destroy after this one. It is also an error to perform
  // this request after a NULL mime type has been set in
  // wl_data_offer.accept or no action was received through
  // wl_data_offer.action.
  // 
  // If wl_data_offer.finish request is received for a non drag and drop
  // operation, the invalid_finish protocol error is raised.
  void OnFinish();

  // Set the available/preferred drag-and-drop actions
  //
  // Sets the actions that the destination side client supports for
  // this operation. This request may trigger the emission of
  // wl_data_source.action and wl_data_offer.action events if the compositor
  // needs to change the selected action.
  // 
  // This request can be called multiple times throughout the
  // drag-and-drop operation, typically in response to wl_data_device.enter
  // or wl_data_device.motion events.
  // 
  // This request determines the final result of the drag-and-drop
  // operation. If the end result is that no action is accepted,
  // the drag source will receive wl_data_source.cancelled.
  // 
  // The dnd_actions argument must contain only values expressed in the
  // wl_data_device_manager.dnd_actions enum, and the preferred_action
  // argument must only contain one of those values set, otherwise it
  // will result in a protocol error.
  // 
  // While managing an "ask" action, the destination drag-and-drop client
  // may perform further wl_data_offer.receive requests, and is expected
  // to perform one last wl_data_offer.set_actions request with a preferred
  // action other than "ask" (and optionally wl_data_offer.accept) before
  // requesting wl_data_offer.finish, in order to convey the action selected
  // by the user. If the preferred action is not in the
  // wl_data_offer.source_actions mask, an error will be raised.
  // 
  // If the "ask" action is dismissed (e.g. user cancellation), the client
  // is expected to perform wl_data_offer.destroy right away.
  // 
  // This request can only be made on drag-and-drop offers, a protocol error
  // will be raised otherwise.
  void OnSetActions(enum DataDeviceManager::DndAction dnd_actions, enum DataDeviceManager::DndAction preferred_action);

  // Advertise offered mime type
  //
  // Sent immediately after creating the wl_data_offer object.  One
  // event per offered mime type.
  void Offer(StrView mime_type);

  // Notify the source-side available actions
  //
  // This event indicates the actions offered by the data source. It
  // will be sent immediately after creating the wl_data_offer object,
  // or anytime the source side changes its offered actions through
  // wl_data_source.set_actions.
  void SourceActions(enum DataDeviceManager::DndAction source_actions);

  // Notify the selected action
  //
  // This event indicates the action selected by the compositor after
  // matching the source/destination side actions. Only one action (or
  // none) will be offered here.
  // 
  // This event can be emitted multiple times during the drag-and-drop
  // operation in response to destination side action changes through
  // wl_data_offer.set_actions.
  // 
  // This event will no longer be emitted after wl_data_device.drop
  // happened on the drag-and-drop destination, the client must
  // honor the last action received, or the last preferred one set
  // through wl_data_offer.set_actions when handling an "ask" action.
  // 
  // Compositors may also change the selected action on the fly, mainly
  // in response to keyboard modifier changes during the drag-and-drop
  // operation.
  // 
  // The most recent action received is always the valid one. Prior to
  // receiving wl_data_device.drop, the chosen action may change (e.g.
  // due to keyboard modifiers being pressed). At the time of receiving
  // wl_data_device.drop the drag-and-drop destination must honor the
  // last action received.
  // 
  // Action changes may still happen after wl_data_device.drop,
  // especially on "ask" actions, where the drag-and-drop destination
  // may choose another action afterwards. Action changes happening
  // at this stage are always the result of inter-client negotiation, the
  // compositor shall no longer be able to induce a different action.
  // 
  // Upon "ask" actions, it is expected that the drag-and-drop destination
  // may potentially choose a different action and/or mime type,
  // based on wl_data_offer.source_actions and finally chosen by the
  // user (e.g. popping up a menu with the available options). The
  // final wl_data_offer.set_actions and wl_data_offer.accept requests
  // must happen before the call to wl_data_offer.finish.
  void Action(enum DataDeviceManager::DndAction dnd_action);
  // clang-format on: generated DataOffer from wayland.xml

  MortalPtr<DataSource> source;
};

// clang-format off: generated DataSource from wayland.xml

// Offer to transfer data
//
// The wl_data_source object is the source side of a wl_data_offer.
// It is created by the source client in a data transfer and
// provides a way to describe the offered data and a way to respond
// to requests to transfer the data.
struct DataSource : Common {
  static constexpr int Version = 3;
  static constexpr Kind Kind = KindDataSource;

  static Colony<DataSource> colony;

  // Do not use directly. Instead use `ColonyMake`
  DataSource(U32 id, Client& client) : Common(Kind, id, client) {}

  static DataSource& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  enum Error : U32 {
    ErrorInvalidActionMask = 0, // Action mask contains invalid values
    ErrorInvalidSource = 1, // Source doesn't accept this request
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "InvalidActionMask"sv;
    case 1: return "InvalidSource"sv;
    default: return "UnknownError"sv;
    }
  }

  // Add an offered mime type
  //
  // This request adds a mime type to the set of mime types
  // advertised to targets.  Can be called several times to offer
  // multiple types.
  void OnOffer(StrView mime_type);

  // Destroy the data source
  //
  // Destroy the data source.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Set the available drag-and-drop actions
  //
  // Sets the actions that the source side client supports for this
  // operation. This request may trigger wl_data_source.action and
  // wl_data_offer.action events if the compositor needs to change the
  // selected action.
  // 
  // The dnd_actions argument must contain only values expressed in the
  // wl_data_device_manager.dnd_actions enum, otherwise it will result
  // in a protocol error.
  // 
  // This request must be made once only, and can only be made on sources
  // used in drag-and-drop, so it must be performed before
  // wl_data_device.start_drag. Attempting to use the source other than
  // for drag-and-drop will raise a protocol error.
  void OnSetActions(enum DataDeviceManager::DndAction dnd_actions);

  // A target accepts an offered mime type
  //
  // Sent when a target accepts pointer_focus or motion events.  If
  // a target does not accept any of the offered types, type is NULL.
  // 
  // Used for feedback during drag-and-drop.
  void Target(StrView mime_type);

  // Send the data
  //
  // Request for data from the client.  Send the data as the
  // specified mime type over the passed file descriptor, then
  // close it.
  void Send(StrView mime_type, FD&& fd);

  // Selection was cancelled
  //
  // This data source is no longer valid. There are several reasons why
  // this could happen:
  // 
  // - The data source has been replaced by another data source.
  // - The drag-and-drop operation was performed, but the drop destination
  //   did not accept any of the mime types offered through
  //   wl_data_source.target.
  // - The drag-and-drop operation was performed, but the drop destination
  //   did not select any of the actions present in the mask offered through
  //   wl_data_source.action.
  // - The drag-and-drop operation was performed but didn't happen over a
  //   surface.
  // - The compositor cancelled the drag-and-drop operation (e.g. compositor
  //   dependent timeouts to avoid stale drag-and-drop transfers).
  // 
  // The client should clean up and destroy this data source.
  // 
  // For objects of version 2 or older, wl_data_source.cancelled will
  // only be emitted if the data source was replaced by another data
  // source.
  void Cancelled();

  // The drag-and-drop operation physically finished
  //
  // The user performed the drop action. This event does not indicate
  // acceptance, wl_data_source.cancelled may still be emitted afterwards
  // if the drop destination does not accept any mime type.
  // 
  // However, this event might however not be received if the compositor
  // cancelled the drag-and-drop operation before this event could happen.
  // 
  // Note that the data_source may still be used in the future and should
  // not be destroyed here.
  void DndDropPerformed();

  // The drag-and-drop operation concluded
  //
  // The drop destination finished interoperating with this data
  // source, so the client is now free to destroy this data source and
  // free all associated data.
  // 
  // If the action used to perform the operation was "move", the
  // source can now delete the transferred data.
  void DndFinished();

  // Notify the selected action
  //
  // This event indicates the action selected by the compositor after
  // matching the source/destination side actions. Only one action (or
  // none) will be offered here.
  // 
  // This event can be emitted multiple times during the drag-and-drop
  // operation, mainly in response to destination side changes through
  // wl_data_offer.set_actions, and as the data device enters/leaves
  // surfaces.
  // 
  // It is only possible to receive this event after
  // wl_data_source.dnd_drop_performed if the drag-and-drop operation
  // ended in an "ask" action, in which case the final wl_data_source.action
  // event will happen immediately before wl_data_source.dnd_finished.
  // 
  // Compositors may also change the selected action on the fly, mainly
  // in response to keyboard modifier changes during the drag-and-drop
  // operation.
  // 
  // The most recent action received is always the valid one. The chosen
  // action may change alongside negotiation (e.g. an "ask" action can turn
  // into a "move" operation), so the effects of the final action must
  // always be applied in wl_data_offer.dnd_finished.
  // 
  // Clients can trigger cursor surface changes from this point, so
  // they reflect the current action.
  void Action(enum DataDeviceManager::DndAction dnd_action);
  // clang-format on: generated DataSource from wayland.xml

  Vec<Str> mimes;
};

// clang-format off: generated Surface from wayland.xml

// An onscreen surface
//
// A surface is a rectangular area that may be displayed on zero
// or more outputs, and shown any number of times at the compositor's
// discretion. They can present wl_buffers, receive user input, and
// define a local coordinate system.
// 
// The size of a surface (and relative positions on it) is described
// in surface-local coordinates, which may differ from the buffer
// coordinates of the pixel content, in case a buffer_transform
// or a buffer_scale is used.
// 
// A surface without a "role" is fairly useless: a compositor does
// not know where, when or how to present it. The role is the
// purpose of a wl_surface. Examples of roles are a cursor for a
// pointer (as set by wl_pointer.set_cursor), a drag icon
// (wl_data_device.start_drag), a sub-surface
// (wl_subcompositor.get_subsurface), and a window as defined by a
// shell protocol (e.g. wl_shell.get_shell_surface).
// 
// A surface can have only one role at a time. Initially a
// wl_surface does not have a role. Once a wl_surface is given a
// role, it is set permanently for the whole lifetime of the
// wl_surface object. Giving the current role again is allowed,
// unless explicitly forbidden by the relevant interface
// specification.
// 
// Surface roles are given by requests in other interfaces such as
// wl_pointer.set_cursor. The request should explicitly mention
// that this request gives a role to a wl_surface. Often, this
// request also creates a new protocol object that represents the
// role and adds additional functionality to wl_surface. When a
// client wants to destroy a wl_surface, they must destroy this role
// object before the wl_surface, otherwise a defunct_role_object error is
// sent.
// 
// Destroying the role object does not remove the role from the
// wl_surface, but it may stop the wl_surface from "playing the role".
// For instance, if a wl_subsurface object is destroyed, the wl_surface
// it was created for will be unmapped and forget its position and
// z-order. It is allowed to create a wl_subsurface for the same
// wl_surface again, but it is not allowed to use the wl_surface as
// a cursor (cursor is a different role than sub-surface, and role
// switching is not allowed).
struct Surface : Common {
  static constexpr int Version = 6;
  static constexpr Kind Kind = KindSurface;

  static Colony<Surface> colony;

  // Do not use directly. Instead use `ColonyMake`
  Surface(U32 id, Client& client) : Common(Kind, id, client) {}

  static Surface& ColonyMake(U32 id, Client& client) { return *colony.emplace(id, client); }
  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }

  // Wl_surface error values
  //
  // These errors can be emitted in response to wl_surface requests.
  enum Error : U32 {
    ErrorInvalidScale = 0, // Buffer scale value is invalid
    ErrorInvalidTransform = 1, // Buffer transform value is invalid
    ErrorInvalidSize = 2, // Buffer size is invalid
    ErrorInvalidOffset = 3, // Buffer offset is invalid
    ErrorDefunctRoleObject = 4, // Surface was destroyed before its role object
  };

  static StrView ErrorToStr(U32 value) {
    switch (value) {
    case 0: return "InvalidScale"sv;
    case 1: return "InvalidTransform"sv;
    case 2: return "InvalidSize"sv;
    case 3: return "InvalidOffset"sv;
    case 4: return "DefunctRoleObject"sv;
    default: return "UnknownError"sv;
    }
  }

  // Delete surface
  //
  // Deletes the surface and invalidates its object ID.
  //
  // [destructor] After this method returns, this object will be released
  void OnDestroy();

  // Set the surface contents
  //
  // Set a buffer as the content of this surface.
  // 
  // The new size of the surface is calculated based on the buffer
  // size transformed by the inverse buffer_transform and the
  // inverse buffer_scale. This means that at commit time the supplied
  // buffer size must be an integer multiple of the buffer_scale. If
  // that's not the case, an invalid_size error is sent.
  // 
  // The x and y arguments specify the location of the new pending
  // buffer's upper left corner, relative to the current buffer's upper
  // left corner, in surface-local coordinates. In other words, the
  // x and y, combined with the new surface size define in which
  // directions the surface's size changes. Setting anything other than 0
  // as x and y arguments is discouraged, and should instead be replaced
  // with using the separate wl_surface.offset request.
  // 
  // When the bound wl_surface version is 5 or higher, passing any
  // non-zero x or y is a protocol violation, and will result in an
  // 'invalid_offset' error being raised. The x and y arguments are ignored
  // and do not change the pending state. To achieve equivalent semantics,
  // use wl_surface.offset.
  // 
  // Surface contents are double-buffered state, see wl_surface.commit.
  // 
  // The initial surface contents are void; there is no content.
  // wl_surface.attach assigns the given wl_buffer as the pending
  // wl_buffer. wl_surface.commit makes the pending wl_buffer the new
  // surface contents, and the size of the surface becomes the size
  // calculated from the wl_buffer, as described above. After commit,
  // there is no pending buffer until the next attach.
  // 
  // Committing a pending wl_buffer allows the compositor to read the
  // pixels in the wl_buffer. The compositor may access the pixels at
  // any time after the wl_surface.commit request. When the compositor
  // will not access the pixels anymore, it will send the
  // wl_buffer.release event. Only after receiving wl_buffer.release,
  // the client may reuse the wl_buffer. A wl_buffer that has been
  // attached and then replaced by another attach instead of committed
  // will not receive a release event, and is not used by the
  // compositor.
  // 
  // If a pending wl_buffer has been committed to more than one wl_surface,
  // the delivery of wl_buffer.release events becomes undefined. A well
  // behaved client should not rely on wl_buffer.release events in this
  // case. Alternatively, a client could create multiple wl_buffer objects
  // from the same backing storage or use a protocol extension providing
  // per-commit release notifications.
  // 
  // Destroying the wl_buffer after wl_buffer.release does not change
  // the surface contents. Destroying the wl_buffer before wl_buffer.release
  // is allowed as long as the underlying buffer storage isn't re-used (this
  // can happen e.g. on client process termination). However, if the client
  // destroys the wl_buffer before receiving the wl_buffer.release event and
  // mutates the underlying buffer storage, the surface contents become
  // undefined immediately.
  // 
  // If wl_surface.attach is sent with a NULL wl_buffer, the
  // following wl_surface.commit will remove the surface content.
  // 
  // If a pending wl_buffer has been destroyed, the result is not specified.
  // Many compositors are known to remove the surface content on the following
  // wl_surface.commit, but this behaviour is not universal. Clients seeking to
  // maximise compatibility should not destroy pending buffers and should
  // ensure that they explicitly remove content from surfaces, even after
  // destroying buffers.
  void OnAttach(Buffer* buffer, I32 x, I32 y);

  // Mark part of the surface damaged
  //
  // This request is used to describe the regions where the pending
  // buffer is different from the current surface contents, and where
  // the surface therefore needs to be repainted. The compositor
  // ignores the parts of the damage that fall outside of the surface.
  // 
  // Damage is double-buffered state, see wl_surface.commit.
  // 
  // The damage rectangle is specified in surface-local coordinates,
  // where x and y specify the upper left corner of the damage rectangle.
  // 
  // The initial value for pending damage is empty: no damage.
  // wl_surface.damage adds pending damage: the new pending damage
  // is the union of old pending damage and the given rectangle.
  // 
  // wl_surface.commit assigns pending damage as the current damage,
  // and clears pending damage. The server will clear the current
  // damage as it repaints the surface.
  // 
  // Note! New clients should not use this request. Instead damage can be
  // posted with wl_surface.damage_buffer which uses buffer coordinates
  // instead of surface coordinates.
  void OnDamage(I32 x, I32 y, I32 width, I32 height);

  // Request a frame throttling hint
  //
  // Request a notification when it is a good time to start drawing a new
  // frame, by creating a frame callback. This is useful for throttling
  // redrawing operations, and driving animations.
  // 
  // When a client is animating on a wl_surface, it can use the 'frame'
  // request to get notified when it is a good time to draw and commit the
  // next frame of animation. If the client commits an update earlier than
  // that, it is likely that some updates will not make it to the display,
  // and the client is wasting resources by drawing too often.
  // 
  // The frame request will take effect on the next wl_surface.commit.
  // The notification will only be posted for one frame unless
  // requested again. For a wl_surface, the notifications are posted in
  // the order the frame requests were committed.
  // 
  // The server must send the notifications so that a client
  // will not send excessive updates, while still allowing
  // the highest possible update rate for clients that wait for the reply
  // before drawing again. The server should give some time for the client
  // to draw and commit after sending the frame callback events to let it
  // hit the next output refresh.
  // 
  // A server should avoid signaling the frame callbacks if the
  // surface is not visible in any way, e.g. the surface is off-screen,
  // or completely obscured by other opaque surfaces.
  // 
  // The object returned by this request will be destroyed by the
  // compositor after the callback is fired and as such the client must not
  // attempt to use it after that point.
  // 
  // The callback_data passed in the callback is the current time, in
  // milliseconds, with an undefined base.
  void OnFrame(Callback& callback);

  // Set opaque region
  //
  // This request sets the region of the surface that contains
  // opaque content.
  // 
  // The opaque region is an optimization hint for the compositor
  // that lets it optimize the redrawing of content behind opaque
  // regions.  Setting an opaque region is not required for correct
  // behaviour, but marking transparent content as opaque will result
  // in repaint artifacts.
  // 
  // The opaque region is specified in surface-local coordinates.
  // 
  // The compositor ignores the parts of the opaque region that fall
  // outside of the surface.
  // 
  // Opaque region is double-buffered state, see wl_surface.commit.
  // 
  // wl_surface.set_opaque_region changes the pending opaque region.
  // wl_surface.commit copies the pending region to the current region.
  // Otherwise, the pending and current regions are never changed.
  // 
  // The initial value for an opaque region is empty. Setting the pending
  // opaque region has copy semantics, and the wl_region object can be
  // destroyed immediately. A NULL wl_region causes the pending opaque
  // region to be set to empty.
  void OnSetOpaqueRegion(Region* region);

  // Set input region
  //
  // This request sets the region of the surface that can receive
  // pointer and touch events.
  // 
  // Input events happening outside of this region will try the next
  // surface in the server surface stack. The compositor ignores the
  // parts of the input region that fall outside of the surface.
  // 
  // The input region is specified in surface-local coordinates.
  // 
  // Input region is double-buffered state, see wl_surface.commit.
  // 
  // wl_surface.set_input_region changes the pending input region.
  // wl_surface.commit copies the pending region to the current region.
  // Otherwise the pending and current regions are never changed,
  // except cursor and icon surfaces are special cases, see
  // wl_pointer.set_cursor and wl_data_device.start_drag.
  // 
  // The initial value for an input region is infinite. That means the
  // whole surface will accept input. Setting the pending input region
  // has copy semantics, and the wl_region object can be destroyed
  // immediately. A NULL wl_region causes the input region to be set
  // to infinite.
  void OnSetInputRegion(Region* region);

  // Commit pending surface state
  //
  // Surface state (input, opaque, and damage regions, attached buffers,
  // etc.) is double-buffered. Protocol requests modify the pending state,
  // as opposed to the active state in use by the compositor.
  // 
  // A commit request atomically creates a content update from the pending
  // state, even if the pending state has not been touched. The content
  // update is placed in a queue until it becomes active. After commit, the
  // new pending state is as documented for each related request.
  // 
  // When the content update is applied, the wl_buffer is applied before all
  // other state. This means that all coordinates in double-buffered state
  // are relative to the newly attached wl_buffers, except for
  // wl_surface.attach itself. If there is no newly attached wl_buffer, the
  // coordinates are relative to the previous content update.
  // 
  // All requests that need a commit to become effective are documented
  // to affect double-buffered state.
  // 
  // Other interfaces may add further double-buffered surface state.
  void OnCommit();

  // Sets the buffer transformation
  //
  // This request sets the transformation that the client has already applied
  // to the content of the buffer. The accepted values for the transform
  // parameter are the values for wl_output.transform.
  // 
  // The compositor applies the inverse of this transformation whenever it
  // uses the buffer contents.
  // 
  // Buffer transform is double-buffered state, see wl_surface.commit.
  // 
  // A newly created surface has its buffer transformation set to normal.
  // 
  // wl_surface.set_buffer_transform changes the pending buffer
  // transformation. wl_surface.commit copies the pending buffer
  // transformation to the current one. Otherwise, the pending and current
  // values are never changed.
  // 
  // The purpose of this request is to allow clients to render content
  // according to the output transform, thus permitting the compositor to
  // use certain optimizations even if the display is rotated. Using
  // hardware overlays and scanning out a client buffer for fullscreen
  // surfaces are examples of such optimizations. Those optimizations are
  // highly dependent on the compositor implementation, so the use of this
  // request should be considered on a case-by-case basis.
  // 
  // Note that if the transform value includes 90 or 270 degree rotation,
  // the width of the buffer will become the surface height and the height
  // of the buffer will become the surface width.
  // 
  // If transform is not one of the values from the
  // wl_output.transform enum the invalid_transform protocol error
  // is raised.
  void OnSetBufferTransform(enum Output::Transform transform);

  // Sets the buffer scaling factor
  //
  // This request sets an optional scaling factor on how the compositor
  // interprets the contents of the buffer attached to the window.
  // 
  // Buffer scale is double-buffered state, see wl_surface.commit.
  // 
  // A newly created surface has its buffer scale set to 1.
  // 
  // wl_surface.set_buffer_scale changes the pending buffer scale.
  // wl_surface.commit copies the pending buffer scale to the current one.
  // Otherwise, the pending and current values are never changed.
  // 
  // The purpose of this request is to allow clients to supply higher
  // resolution buffer data for use on high resolution outputs. It is
  // intended that you pick the same buffer scale as the scale of the
  // output that the surface is displayed on. This means the compositor
  // can avoid scaling when rendering the surface on that output.
  // 
  // Note that if the scale is larger than 1, then you have to attach
  // a buffer that is larger (by a factor of scale in each dimension)
  // than the desired surface size.
  // 
  // If scale is not greater than 0 the invalid_scale protocol error is
  // raised.
  void OnSetBufferScale(I32 scale);

  // Mark part of the surface damaged using buffer coordinates
  //
  // This request is used to describe the regions where the pending
  // buffer is different from the current surface contents, and where
  // the surface therefore needs to be repainted. The compositor
  // ignores the parts of the damage that fall outside of the surface.
  // 
  // Damage is double-buffered state, see wl_surface.commit.
  // 
  // The damage rectangle is specified in buffer coordinates,
  // where x and y specify the upper left corner of the damage rectangle.
  // 
  // The initial value for pending damage is empty: no damage.
  // wl_surface.damage_buffer adds pending damage: the new pending
  // damage is the union of old pending damage and the given rectangle.
  // 
  // wl_surface.commit assigns pending damage as the current damage,
  // and clears pending damage. The server will clear the current
  // damage as it repaints the surface.
  // 
  // This request differs from wl_surface.damage in only one way - it
  // takes damage in buffer coordinates instead of surface-local
  // coordinates. While this generally is more intuitive than surface
  // coordinates, it is especially desirable when using wp_viewport
  // or when a drawing library (like EGL) is unaware of buffer scale
  // and buffer transform.
  // 
  // Note: Because buffer transformation changes and damage requests may
  // be interleaved in the protocol stream, it is impossible to determine
  // the actual mapping between surface and buffer damage until
  // wl_surface.commit time. Therefore, compositors wishing to take both
  // kinds of damage into account will have to accumulate damage from the
  // two requests separately and only transform from one to the other
  // after receiving the wl_surface.commit.
  void OnDamageBuffer(I32 x, I32 y, I32 width, I32 height);

  // Set the surface contents offset
  //
  // The x and y arguments specify the location of the new pending
  // buffer's upper left corner, relative to the current buffer's upper
  // left corner, in surface-local coordinates. In other words, the
  // x and y, combined with the new surface size define in which
  // directions the surface's size changes.
  // 
  // The exact semantics of wl_surface.offset are role-specific. Refer to
  // the documentation of specific roles for more information.
  // 
  // Surface location offset is double-buffered state, see
  // wl_surface.commit.
  // 
  // This request is semantically equivalent to and the replaces the x and y
  // arguments in the wl_surface.attach request in wl_surface versions prior
  // to 5. See wl_surface.attach for details.
  void OnOffset(I32 x, I32 y);

  // Surface enters an output
  //
  // This is emitted whenever a surface's creation, movement, or resizing
  // results in some part of it being within the scanout region of an
  // output.
  // 
  // Note that a surface may be overlapping with zero or more outputs.
  void Enter(Output& output);

  // Surface leaves an output
  //
  // This is emitted whenever a surface's creation, movement, or resizing
  // results in it no longer having any part of it within the scanout region
  // of an output.
  // 
  // Clients should not use the number of outputs the surface is on for frame
  // throttling purposes. The surface might be hidden even if no leave event
  // has been sent, and the compositor might expect new surface content
  // updates even if no enter event has been sent. The frame event should be
  // used instead.
  void Leave(Output& output);

  // Preferred buffer scale for the surface
  //
  // This event indicates the preferred buffer scale for this surface. It is
  // sent whenever the compositor's preference changes.
  // 
  // Before receiving this event the preferred buffer scale for this surface
  // is 1.
  // 
  // It is intended that scaling aware clients use this event to scale their
  // content and use wl_surface.set_buffer_scale to indicate the scale they
  // have rendered with. This allows clients to supply a higher detail
  // buffer.
  // 
  // The compositor shall emit a scale value greater than 0.
  void PreferredBufferScale(I32 factor);

  // Preferred buffer transform for the surface
  //
  // This event indicates the preferred buffer transform for this surface.
  // It is sent whenever the compositor's preference changes.
  // 
  // Before receiving this event the preferred buffer transform for this
  // surface is normal.
  // 
  // Applying this transformation to the surface buffer contents and using
  // wl_surface.set_buffer_transform might allow the compositor to use the
  // surface buffer more efficiently.
  void PreferredBufferTransform(enum Output::Transform transform);
  // clang-format on: generated Surface from wayland.xml

  MortalPtr<Buffer> pending_buffer;  // wl_buffer attached since the last commit (null = none)
  bool buffer_attached = false;      // an attach happened (a null buffer means unmap)
  Vec<U32> frame_callbacks;          // wl_callback ids from wl_surface.frame
  MortalPtr<XdgSurface> xdg;
  Subsurface* as_subsurface = nullptr;
  MortalPtr<Viewport> viewport;
  MortalPtr<Buffer> held_dmabuf;  // dmabuf wl_buffer held for zero-copy display
  SurfaceCutout content;
  WeakPtr<ReferenceCounted> object;  // a library::WaylandSurface mirroring this
  InputRegion input_region;
  Optional<InputRegion> pending_input_region;
  int buffer_scale = 1;
  int32_t buffer_transform = 0;
  Vec<sk_sp<SkData>> frame_pool;  // recycled shm-copy allocations
  Vec<Surface*> stack;            // back-to-front (this + subsurfaces)
  Vec<Surface*> pending_stack;    // accumulates until commit
  ~Surface();
};

template <U32 StartId = 0>
struct IdPool {
  Vec<U32> free_list{};
  U32 next_id{StartId};

  U32 Grab() {
    if (free_list.empty()) {
      return next_id++;
    } else {
      int id = free_list.back();
      free_list.pop_back();
      return id;
    }
  }

  void Return(U32 id) {
    // TODO: turn `free_list` into a heap & compact it on-line
    if (id == next_id - 1) {
      --next_id;
    } else {
      free_list.push_back(id);
    }
  }
};

struct Server : mux::Epoll::Listener {
  mux::Epoll& epoll;
  Path socket_path;  // full filesystem path; unlinked on clean shutdown
  FD lock_fd;  // exclusive flock on socket_path + ".lock"; released when the Server is destroyed

  // Compositor server-level state (touched on the mux::epoll thread unless noted).
  uint32_t serial = 1;  // monotonic serial source for protocol events
  std::unique_ptr<mux::Timer> frame_timer;
  bool frame_pending = false;
  std::mutex ui_mutex;                        // guards the two handoff vectors
  Vec<Ptr<ReferenceCounted>> ui_appeared;     // mapped windows awaiting board insert
  Vec<Ptr<ReferenceCounted>> ui_disappeared;  // unmapped windows awaiting board remove
  std::mutex adoption_mutex;
  Vec<std::pair<I64, WeakPtr<ReferenceCounted>>> adoptions;  // respawned clients, by pid

  // Input focus + keymap. Cleared in the surface destructor;
  // the keymap is the host X keymap, built once.
  MortalPtr<Surface> pointer_surface;
  MortalPtr<Surface> keyboard_surface;
  MortalPtr<XdgPopup> grabbing_popup;
  MortalPtr<DataSource> selection;  // current clipboard owner (cleared in its destructor)
  int keymap_fd = -1;
  U32 keymap_size = 0;
  U32 mod_shift = 0, mod_ctrl = 0, mod_alt = 0, mod_super = 0;

  // dmabuf version-4 feedback inputs, built lazily on the first feedback request.
  FD dmabuf_format_table_fd;
  U32 dmabuf_format_table_size = 0;
  dev_t dmabuf_main_device = 0;
  bool dmabuf_has_device = false;
  bool dmabuf_inited = false;

  Server(mux::Epoll& epoll) : epoll(epoll) {}
  ~Server();

  void NotifyRead(Status&) override;
  StrView Name() const override;

  void FlushAll();

  // Compositor public API (formerly wayland_compositor.hpp). Callers reach it through the
  // `wayland::server` global; all of them run on the UI/render thread except where noted.
  bool Running();
  void UIFrame();
  void NotifyWindowDestroyed(void* toplevel_handle);
  void SendPointerEnter(library::WaylandSurface&, float lx, float ly);
  void SendPointerMotion(library::WaylandSurface&, float lx, float ly);
  void SendPointerButton(library::WaylandSurface&, uint32_t button, bool pressed);
  void SendPointerAxis(library::WaylandSurface&, float notches_up);
  void SendPointerLeave(library::WaylandSurface&);
  void SendKeyboardEnter(library::WaylandWindow&);
  void SendKeyboardLeave(library::WaylandWindow&);
  void SendKey(library::WaylandWindow&, uint32_t evdev_keycode, bool pressed, bool ctrl, bool alt,
               bool shift, bool super);
  void SendDecorationPreference(library::WaylandWindow&);
};

struct Client : mux::Epoll::Listener {
  Server& server;
  std::string in;
  std::string out;
  std::deque<FD> recv_fds;
  Vec<FD> out_fds;
  Vec<Common*> client_ids;
  Vec<Common*> server_ids;
  IdPool<0xff000000> server_id_pool;
  U32 next_id = 2;
  bool errored = false;
  static Colony<Client> colony;

  Client(Server& s) : server(s) {}
  ~Client() {
    for (Common* o : client_ids)
      if (o) o->GenericColonyDestroy();
    for (Common* o : server_ids)  // e.g. data_offers we allocated on the client's behalf
      if (o) o->GenericColonyDestroy();
  }

  void NotifyRead(Status&) override;
  StrView Name() const override;

  void SetId(U32 id, Common* o) {
    bool server = id >= 0xff000000;
    auto& table = server ? server_ids : client_ids;
    U32 index = server ? id - 0xff000000 : id - 1;
    if (index >= table.size()) table.resize(index + 1, nullptr);
    table[index] = o;
  }
  Common* GetId(U32 id) {
    bool server = id >= 0xff000000;
    auto& table = server ? server_ids : client_ids;
    U32 index = server ? id - 0xff000000 : id - 1;
    return index < table.size() ? table[index] : nullptr;
  }
  bool CheckId(U32 id) {
    if (id == 0 || id > next_id) return false;
    if (id == next_id) {
      ++next_id;
      return true;
    }
    return GetId(id) == nullptr;
  }
  void ProtocolError(U32 object_id, U32 code, StrView message);
  void Disconnect();
};

void AdvertiseDmabufOnBind(LinuxDmabufV1&, U32 version);

// TODO: Make the API more RAII-like

// Starts a new Wayland Compositor a.k.a. Wayland Server a.k.a. Wayland Display.
//
// Picks an unused WAYLAND_DISPLAY.
//
// Also creates flock-ed ${WAYLAND_DISPLAY}.lock - so that socket may be reused in case of crash.
//
// I/O happens through the given epoll instance.
std::unique_ptr<Server> MakeServer(mux::Epoll&, Status&);

// The process-global compositor. Created in automat::Main via MakeServer; null until then. Every
// caller outside wayland.cpp reaches the compositor through this.
extern std::unique_ptr<Server> server;

}  // namespace automat::wayland
