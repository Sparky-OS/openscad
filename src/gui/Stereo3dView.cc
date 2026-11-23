#include "gui/Stereo3dView.h"

#include <QString>
#include <QtGlobal>

#include "gui/qtgettext.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)

// QGLView.h first: the OpenGL loader must come before the Qt OpenGL headers
#include "gui/QGLView.h"
#include "core/Settings.h"
#include "utils/printutils.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

// The fit of each stereo frame, for a check against a prediction: QT_LOGGING_RULES="openscad.stereo.debug=true"
Q_LOGGING_CATEGORY(lcStereo, "openscad.stereo", QtWarningMsg)

namespace {

// Whether the 3D view gets stereo buffers, once it is known
std::optional<bool> stereoBuffers;

// Four times a second the limits are read again: a change in the desktop's 3D settings reaches an
// open view within a quarter of a second, without reopening it
constexpr int limitsPollMs = 250;
// The limits may come a moment after the first frame; the person is told after two seconds
constexpr int pollsBeforeTelling = 8;

}  // namespace

Stereo3dView::Stereo3dView(QGLView& view) : view(view), stereo(Settings::Settings::stereo3dView.value())
{
  limitsTimer.setInterval(limitsPollMs);
  QObject::connect(&limitsTimer, &QTimer::timeout, &view, [this]() { pollLimits(); });
}

void Stereo3dView::requestStereoBuffers(QSurfaceFormat& format)
{
  // QOpenGLWidget draws for each eye only if its own format asks for stereo buffers, and that must
  // be known before its window is created. Only the 3D view asks: the rest of the window stays one
  // picture, the same in both eyes.
  if (Settings::Settings::stereo3dView.value()) {
    format.setOption(QSurfaceFormat::StereoBuffers);
  }
}

bool Stereo3dView::isAvailable()
{
  if (!stereoBuffers) {
    // Qt picks the framebuffer configuration when it creates the context, and QOpenGLWidget draws
    // each eye only when its own context's format is stereo, so a context with the view's format
    // gives the same answer the view gets.
    auto format = QGLView::compatibleWidgetFormat();
    format.setOption(QSurfaceFormat::StereoBuffers);
    stereoBuffers = createdContextFormat(format).stereo();
  }
  return *stereoBuffers;
}

QString Stereo3dView::unavailableReason()
{
  return _("This desktop offers no stereo buffers, so the 3D view stays 2D.");
}

void Stereo3dView::initialize()
{
  if (!stereo) return;
  // A view that asked for stereo buffers knows from its own format whether it got them
  const bool granted = view.format().stereo();
  if (!granted && isAvailable()) {
    tellOnce(_("3D: this view got no stereo buffers, so it is shown in 2D"));
  }
  if (!stereoBuffers) stereoBuffers = granted;
  if (granted) {
    pollLimits();
    limitsTimer.start();
  }
}

bool Stereo3dView::drawsEyes() const
{
  return stereo && view.format().stereo();
}

void Stereo3dView::tellOnce(const std::string& sentence)
{
  if (told.insert(sentence).second) LOG(message_group::UI_Warning, "%1$s", sentence);
}

void Stereo3dView::pollLimits()
{
  if (!drawsEyes()) return;
  const double width = std::round(view.width() * view.devicePixelRatioF());
  if (width <= 0) return;
  StereoDepthLimits::Result r;
  auto *native = QGuiApplication::platformNativeInterface();
  const QString platform = QGuiApplication::platformName();
  if (platform.startsWith(QLatin1String("wayland"))) {
    QWindow *top = view.window() ? view.window()->windowHandle() : nullptr;
    void *display = native ? native->nativeResourceForIntegration("wl_display") : nullptr;
    void *surface = native && top ? native->nativeResourceForWindow("surface", top) : nullptr;
    // A stereo desktop gives the limits for the top-level window's surface, not for the view's
    // own subsurface
    r = StereoDepthLimits::wayland(display, surface, width, width);
  } else if (platform == QLatin1String("xcb")) {
    void *display = native ? native->nativeResourceForIntegration("display") : nullptr;
    // The view's own window is the declared stereo child window that carries the limits
    r = StereoDepthLimits::x11(display, view.winId(), width, width);
  } else {
    r.status = StereoDepthLimits::State::Unreadable;
  }

  const bool changed = r.status != limits.status || r.sunk != limits.sunk || r.popped != limits.popped;
  limits = r;
  if (changed) {
    qCDebug(lcStereo).nospace() << "limits platform=" << platform << " status=" << static_cast<int>(r.status)
                                << " sunk=" << r.sunk << " popped=" << r.popped << " width=" << width;
  }
  if (r.status == StereoDepthLimits::State::Ok) {
    pollsWithoutLimits = 0;
  } else if (++pollsWithoutLimits == pollsBeforeTelling) {
    switch (r.status) {
    case StereoDepthLimits::State::NotOffered:
      tellOnce(_("3D depth: this desktop offers no depth limits, so both eyes show the same picture"));
      break;
    case StereoDepthLimits::State::NotDelivered:
      tellOnce(
        _("3D depth: the desktop has not delivered the depth limits yet, both eyes show the same picture for now"));
      break;
    default:
      tellOnce(_("3D depth: this window cannot read the desktop's depth limits, so both eyes show the same picture"));
      break;
    }
  }
  if (changed) view.update();
}

void Stereo3dView::beginEye()
{
  // One eye per call: the left buffer first, then the right one
  if (!drawsEyes()) return;
  if (view.currentTargetBuffer() == QOpenGLWidget::LeftBuffer) {
    // The measuring pass: the whole view from the camera between the eyes, into the left buffer,
    // which the left eye then draws over
    view.stereoEyeOffset = 0.0;
    measuring = true;
    depthRead = false;
    view.GLView::paintGL();
    measuring = false;
    halfOffset = depthRead ? fittedHalfOffset() : 0.0;
    if (depthUnreadable) {
      tellOnce(_("3D depth: the depth of this view could not be read, so both eyes show the same picture"));
    }
  }
  view.stereoEyeOffset = view.currentTargetBuffer() == QOpenGLWidget::RightBuffer ? halfOffset : -halfOffset;
}

void Stereo3dView::endEye()
{
  if (!drawsEyes()) return;
  view.stereoEyeOffset = 0.0;
  view.setupCamera();
}

void Stereo3dView::sceneDrawn()
{
  if (!measuring) return;
  // The scene is drawn and nothing of the 2D items over it yet: its depth buffer holds the nearest
  // pixel of the scene everywhere
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  const int w = viewport[2];
  const int h = viewport[3];
  if (w <= 0 || h <= 0) return;
  std::vector<GLfloat> depth(static_cast<size_t>(w) * h);
  while (glGetError() != GL_NO_ERROR) {
  }
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(viewport[0], viewport[1], w, h, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
  if (glGetError() != GL_NO_ERROR) {
    depthUnreadable = true;
    return;
  }
  GLfloat dmin = 1.0f;
  GLfloat dmax = 0.0f;
  for (const GLfloat d : depth) {
    if (d >= 1.0f) continue;  // the cleared depth: background, no scene there
    dmin = std::min(dmin, d);
    dmax = std::max(dmax, d);
  }
  depthUnreadable = false;
  depthRead = true;
  bufferWidth = w;
  sceneSeen = dmin <= dmax;
  std::copy(std::begin(view.projection), std::end(view.projection), projection.begin());
  if (!sceneSeen) return;
  // Window depth to the distance in front of the camera, with the projection of this pass
  const double a = projection[10];
  const double b = projection[14];
  const bool perspective = view.cam.projection == Camera::ProjectionType::PERSPECTIVE;
  auto distance = [&](double d) {
    const double z = 2.0 * d - 1.0;
    return perspective ? b / (z + a) : (b - z) / a;
  };
  nearest = distance(dmin);
  farthest = distance(dmax);
}

double Stereo3dView::fittedHalfOffset() const
{
  if (limits.status != StereoDepthLimits::State::Ok) return 0.0;
  // k: pixels per unit of the image plane, half the buffer's width times the projection's x scale;
  // c: the distance of the point looked at, where both eyes meet.  With the separation t as a
  // fraction of c, a point at distance d is shifted between the eyes by k t (1 - c/d) in
  // perspective and by k t (d - c) in orthographic: the separation is the largest that keeps the
  // nearest pixel inside the popped limit and the farthest inside the sunk limit.
  const double k = bufferWidth / 2.0 * projection[0];
  const double c = view.cam.zoomValue();
  if (k <= 0 || c <= 0) return 0.0;
  const bool perspective = view.cam.projection == Camera::ProjectionType::PERSPECTIVE;
  constexpr double none = std::numeric_limits<double>::infinity();
  double t = none;
  if (sceneSeen) {
    const double front = perspective ? c / nearest - 1.0 : c - nearest;
    const double behind = perspective ? 1.0 - c / farthest : farthest - c;
    if (front > 0) t = std::min(t, limits.popped / (k * front));
    if (behind > 0) t = std::min(t, limits.sunk / (k * behind));
  }
  if (t == none) {
    // Nothing off the plane of the point looked at: in perspective, infinity stands at the sunk
    // limit; an orthographic view has no depth to show
    t = perspective ? limits.sunk / k : 0.0;
  }
  if (lcStereo().isDebugEnabled()) {
    const double nearPx = sceneSeen ? (perspective ? k * t * (1.0 - c / nearest) : k * t * (nearest - c)) : 0.0;
    const double farPx = sceneSeen ? (perspective ? k * t * (1.0 - c / farthest) : k * t * (farthest - c)) : 0.0;
    qCDebug(lcStereo).nospace() << "fit projection=" << (perspective ? "perspective" : "orthographic")
                                << " limits sunk=" << limits.sunk << " popped=" << limits.popped
                                << " width=" << bufferWidth << " k=" << k << " c=" << c << " nearest=" << nearest
                                << " farthest=" << farthest << " separation=" << t << " offset=" << t * c
                                << " disparity nearest=" << nearPx << " farthest=" << farPx;
  }
  return t * c / 2.0;
}

void Stereo3dView::beginDepthRead()
{
  if (!drawsEyes()) return;
  view.stereoEyeOffset = 0.0;
  view.GLView::paintGL();
}

void Stereo3dView::endDepthRead()
{
  if (!drawsEyes()) return;
  view.stereoEyeOffset = 0.0;
  view.setupCamera();
  view.update();
}

#else  // Qt before 6.5: the stereo 3D view is not available

Stereo3dView::Stereo3dView(QGLView& /*view*/)
{
}

void Stereo3dView::requestStereoBuffers(QSurfaceFormat& /*format*/)
{
}

bool Stereo3dView::isAvailable()
{
  return false;
}

QString Stereo3dView::unavailableReason()
{
  return _("The stereo 3D view needs OpenSCAD built with Qt 6.5 or later.");
}

void Stereo3dView::initialize()
{
}

void Stereo3dView::beginEye()
{
}

void Stereo3dView::endEye()
{
}

void Stereo3dView::sceneDrawn()
{
}

void Stereo3dView::beginDepthRead()
{
}

void Stereo3dView::endDepthRead()
{
}

#endif  // QT_VERSION
