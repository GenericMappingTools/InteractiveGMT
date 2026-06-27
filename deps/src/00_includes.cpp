// gmtvtk — self-contained Qt6 + VTK 9.6 3-D viewer for GMT data (native QMenu UI +
// VTK 3-D; see ../QTVTK_PLAN.md). No dependency on f3d.
//
// Builds as BOTH a shared library (C API `gmtvtk_view_grid` / `gmtvtk_view_demo`,
// ccall'd from Julia) and a standalone demo executable. A grid surface is a
// vtkPolyData (points + quad cells + a z scalar) wrapped in the polished chrome:
// native menubar, right-click context menu, cube axes, scalar bar, gradient
// background, live coordinate readout, an interaction gizmo, and vertical
// exaggeration. The demo build shows a synthetic MATLAB-peaks surface.

#include <QApplication>
#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QStatusBar>
#include <QAction>
#include <QInputDialog>
#include <QFileDialog>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QString>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QDockWidget>
#include <QWidget>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QDoubleValidator>
#include <string>
#include <QSlider>
#include <QScrollBar>
#include <QLabel>
#include <QCheckBox>
#include <QGroupBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QActionGroup>
#include <QWidgetAction>
#include <functional>
#include <algorithm>
#include <QToolTip>
#include <QColorDialog>
#include <QTimer>
#include <QPoint>
#include <QCursor>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QFontMetrics>
#include <functional>
#include <QFile>
#include <QTextStream>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QFont>
#include <QTabWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QToolButton>
#include <QToolBar>
#include <QStyle>
#include <QPixmap>
#include <QIcon>
#include <QPolygonF>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QStringList>
#include <QSettings>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QContextMenuEvent>
#include <QUrl>

#include <QVTKOpenGLNativeWidget.h>

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkActor.h>
#include <vtkLODActor.h>
#include <vtkProperty.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor2D.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProperty2D.h>
#include <vtkCoordinate.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkGlyph3D.h>
#include <vtkIdList.h>
#include <vtkFloatArray.h>
#include <vtkUnsignedCharArray.h>
#include <vtkPointData.h>
#include <vtkCellData.h>
#include <vtkLookupTable.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>
#include <vtkCubeAxesActor.h>
#include <vtkCamera.h>
#include <vtkCellPicker.h>
#include <vtkCellLocator.h>
#include <vtkPropPicker.h>
#include <vtkPropCollection.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkTextProperty.h>
#include <vtkWindowToImageFilter.h>
#include <vtkPNGWriter.h>
#include <vtkNamedColors.h>
#include <vtkSmartPointer.h>
#include <vtkNew.h>

// --- gizmo (Fledermaus-style scale/tilt/azimuth handle) ----------------------
#include <vtkConeSource.h>
#include <vtkCylinderSource.h>
#include <vtkLineSource.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkTextActor3D.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkLight.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyDataAlgorithm.h>
#include <vtkPolyDataNormals.h>
#include <vtkColorTransferFunction.h>
#include <vtkScalarsToColors.h>
#include <vtkImageData.h>
#include <vtkAssembly.h>
#include <vtkProp3D.h>
#include <vtkTexture.h>
#include <vtkImageReader2.h>
#include <vtkImageReader2Factory.h>
#include <vtkImageFlip.h>
#include <vtkRenderWindow.h>
#include <vtkInteractorStyleTrackballCamera.h>

// --- 2-D charts (the X,Y plot tool, 65_xyplot.cpp) ---------------------------
#include <vtkContextView.h>
#include <vtkContextScene.h>
#include <vtkChart.h>
#include <vtkChartXY.h>
#include <vtkPlot.h>
#include <vtkPlotPoints.h>
#include <vtkAxis.h>
#include <vtkTable.h>
#include <vtkPen.h>
#include <vtkStringArray.h>
#include <vtkTooltipItem.h>
#include <vtkContextMouseEvent.h>
#include <vtkObjectFactory.h>

// --- F3D-style shading (Tier 1: light rig + post passes, no assets) ----------
#include <vtkLightKit.h>
#include <vtkRenderStepsPass.h>
#include <vtkSSAOPass.h>
#include <vtkToneMappingPass.h>
#include <vtkOpenGLFXAAPass.h>
#include <vtkRenderPass.h>
// Cast-shadow pass chain (sun self-shadowing on terrain).
#include <vtkShadowMapPass.h>
#include <vtkShadowMapBakerPass.h>
#include <vtkSequencePass.h>
#include <vtkCameraPass.h>
#include <vtkLightsPass.h>
#include <vtkOpaquePass.h>
#include <vtkTranslucentPass.h>
#include <vtkVolumetricPass.h>
#include <vtkOverlayPass.h>
#include <vtkRenderPassCollection.h>
#include <vtkLightCollection.h>
#include <vtkVersion.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkTextProperty.h>
#include <vtkDoubleArray.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include <array>
#include <set>
#include <unordered_set>

