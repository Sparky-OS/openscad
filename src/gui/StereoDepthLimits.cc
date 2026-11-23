#include "gui/StereoDepthLimits.h"

#include <cerrno>

#include <stereo-declare.h>

namespace StereoDepthLimits {

namespace {

Result result(int r, const stereo_depth_limits& limits)
{
  Result res;
  if (r == 0) {
    res.status = State::Ok;
    res.sunk = limits.sunk;
    res.popped = limits.popped;
  } else if (r == -ENOTSUP) {
    res.status = State::NotOffered;
  } else if (r == -ENODATA) {
    res.status = State::NotDelivered;
  } else {
    res.status = State::Unreadable;
  }
  return res;
}

}  // namespace

Result wayland(void *display, void *surface, double bufferWidth, double shownWidth)
{
  static void *initialized = nullptr;
  if (!display || !surface) return {};
  if (initialized != display) {
    // The objects the library makes go on the program's default queue, which Qt dispatches; the
    // first answer can be "not offered" until the compositor's globals have been dispatched.
    if (stereo_declare_wayland_init(static_cast<wl_display *>(display), nullptr) != 0) return {};
    initialized = display;
  }
  stereo_depth_limits limits{};
  const int r = stereo_depth_wayland_get_buffer_limits(static_cast<wl_surface *>(surface), bufferWidth,
                                                       shownWidth, &limits);
  return result(r, limits);
}

Result x11(void *display, unsigned long window, double bufferWidth, double shownWidth)
{
  if (!display || !window) return {};
  stereo_depth_limits limits{};
  const int r = stereo_depth_x11_get_buffer_limits(static_cast<Display *>(display), static_cast<Window>(window),
                                                   bufferWidth, shownWidth, &limits);
  return result(r, limits);
}

}  // namespace StereoDepthLimits
