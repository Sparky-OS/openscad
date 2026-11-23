#pragma once

#include <QString>
#include <QTimer>
#include <QtGlobal>
#include <array>
#include <set>
#include <string>

#include "gui/StereoDepthLimits.h"

class QGLView;
class QSurfaceFormat;

/*
 * The stereo 3D view. With "Enable 3D" on and an OpenGL context with stereo buffers, QOpenGLWidget
 * calls QGLView::paintGL() once for the left buffer and once for the right buffer, and this class
 * sets the camera of each eye through GLView::stereoEyeOffset: two parallel cameras, an off-axis
 * frustum in perspective and a shear in orthographic, meeting at the point looked at.
 *
 * The separation is not a setting.  The person sets the depth limits once, in the desktop's 3D
 * settings; the view reads them through libstereo-declare and, before the left eye, draws the scene
 * once from the camera between the eyes and reads its depth.  The separation is then the largest
 * for which every pixel of the view stays inside the limits.  Without limits both eyes are the same
 * picture, and the console says why, once.
 *
 * It needs QOpenGLWidget::currentTargetBuffer(), which came with Qt 6.5. With older Qt this class
 * is a stub: it never asks for stereo buffers and never moves the eye, so the eye offset stays 0
 * and the 3D view is the normal single view. This class is the only place where the stereo 3D view
 * depends on the Qt version.
 */
class Stereo3dView
{
public:
  explicit Stereo3dView(QGLView& view);

  // Asks for stereo buffers in the 3D view's own format when "Enable 3D" is on
  static void requestStereoBuffers(QSurfaceFormat& format);
  // Whether this desktop gives OpenGL contexts stereo buffers, found out once per session
  static bool isAvailable();
  // What the person is told when they enable 3D where it is not available
  static QString unavailableReason();

  // From QGLView::initializeGL()
  void initialize();
  // Around GLView::paintGL(): before the left eye, the scene is drawn and measured from the camera
  // between the eyes; then the offset of the eye drawn now is set, and after it the camera between
  // the eyes again, which picking uses
  void beginEye();
  void endEye();
  // From QGLView::sceneDrawn(): the depth of the scene, in the measuring pass only
  void sceneDrawn();
  // Around reading the depth buffer for a double click: the view drawn from the camera between the
  // eyes, so the point found is the one a single view finds
  void beginDepthRead();
  void endDepthRead();

private:
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  [[nodiscard]] bool drawsEyes() const;
  void pollLimits();
  void tellOnce(const std::string& sentence);
  [[nodiscard]] double fittedHalfOffset() const;

  QGLView& view;
  bool stereo;            // "Enable 3D", as it was when the view was created
  bool measuring = false;
  bool depthRead = false;     // the measuring pass read the depth of the scene
  bool depthUnreadable = false;
  double nearest = 0.0;   // distances from the camera between the eyes, along its axis
  double farthest = 0.0;
  bool sceneSeen = false;  // at least one pixel of the scene in the view
  std::array<double, 16> projection{};
  int bufferWidth = 0;
  double halfOffset = 0.0;  // half the distance between the two cameras
  StereoDepthLimits::Result limits;
  int pollsWithoutLimits = 0;
  QTimer limitsTimer;
  std::set<std::string> told;
#endif
};
