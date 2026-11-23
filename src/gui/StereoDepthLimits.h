#pragma once

/*
 * The person's depth limits for the stereo 3D view, read through libstereo-declare.
 *
 * A stereo desktop (Sparky Stereo OS's KWin) sets the limits in one place, its 3D depth settings,
 * and hands them to every window: on Wayland for the top-level window's surface, on X11 as a
 * property of the declared stereo child window.  The library converts them to the pixels of the
 * view's buffer.  This file keeps the library's X11 header out of the Qt sources.
 */
namespace StereoDepthLimits {

enum class State {
  Ok,
  NotOffered,    // this desktop sends no depth limits
  NotDelivered,  // offered, not received yet
  Unreadable,    // any other answer of the library
};

struct Result {
  State status = State::NotOffered;
  double sunk = 0.0;    // farthest disparity allowed behind the screen, in buffer pixels
  double popped = 0.0;  // nearest disparity allowed in front of the screen, in buffer pixels
};

// display: the program's wl_display; surface: the top-level window's wl_surface
Result wayland(void *display, void *surface, double bufferWidth, double shownWidth);
// display: the program's X11 Display; window: the view's declared stereo window
Result x11(void *display, unsigned long window, double bufferWidth, double shownWidth);

}  // namespace StereoDepthLimits
