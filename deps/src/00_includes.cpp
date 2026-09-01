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
#include <QClipboard>
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
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QComboBox>
#include <QStandardItemModel>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QWindow>
#include <QString>
#include <QRegularExpression>
#include <QBuffer>
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
#include <QIntValidator>
#include <algorithm>
#include <string>
#include <cstring>
#include <cmath>
#if !defined(_WIN32)
#include <dlfcn.h>
#define __declspec(x)
#define __stdcall
#endif
#if !defined(_WIN32)
#include <dlfcn.h>
#define __declspec(x)
#define __stdcall
#endif
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <QSlider>
#include <QScrollBar>
#include <QStyleFactory>
#include <QScreen>
#include <QLabel>
#include <QCheckBox>
#include <QFrame>
#include <QGroupBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QActionGroup>
#include <QWidgetAction>
#include <QShortcut>
#include <functional>
#include <algorithm>
#include <map>
#include <QToolTip>
#include <QColorDialog>
#include <QTimer>
#include <QPoint>
#include <QCursor>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QFontMetrics>
#include <functional>
#include <QFile>
#include <QUiLoader>
#include <QTextStream>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QLineEdit>
#include <QFont>
#include <QTabWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QListWidget>
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
#include <QDirIterator>
#include <QFileInfo>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QCalendarWidget>       // the Ocean Color date box's popup — its clicked/activated IS the commit
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QStringList>
#include <QSettings>
#include <QMimeData>
#include <QImage>
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
#include <vtkGlyph3DMapper.h>
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
#include <vtkPlane.h>              // Swipe tool: the camera-aligned cut plane (57_swipe.cpp)
#include <vtkPlaneCollection.h>    // ... and the mapper's plane list, to stamp it idempotently
#include <vtkCellPicker.h>
#include <vtkPointPicker.h>
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

// --- VTK's own file formats (87_vtkio.cpp) -----------------------------------
// The generic readers sniff the concrete dataset type themselves, so ONE reader covers the whole
// XML family and one the whole legacy family — no per-extension reader table to keep in sync.
#include <vtkDataObject.h>
#include <vtkDataSet.h>
#include <vtkImageData.h>
#include <vtkRectilinearGrid.h>
#include <vtkStructuredGrid.h>
#include <vtkUnstructuredGrid.h>
#include <vtkMultiBlockDataSet.h>
#include <vtkCompositeDataIterator.h>
#include <vtkDataArray.h>
#include <vtkDoubleArray.h>
#include <vtkXMLGenericDataObjectReader.h>
#include <vtkGenericDataObjectReader.h>
#include <vtkXMLMultiBlockDataReader.h>
#include <vtkHDFReader.h>
#include <vtkXMLImageDataWriter.h>
#include <vtkXMLPolyDataWriter.h>
#include <vtkXMLStructuredGridWriter.h>
#include <vtkDataSetWriter.h>
#include <vtkPolyDataWriter.h>
#include <vtkDataSetSurfaceFilter.h>
#include <vtkCellArrayIterator.h>
#include <vtkTriangleFilter.h>

// --- gizmo (Fledermaus-style scale/tilt/azimuth handle) ----------------------
#include <vtkConeSource.h>
#include <vtkCylinderSource.h>
#include <vtkSphereSource.h>
#include <vtkCubeSource.h>
#include <vtkPlaneSource.h>       // the flat unlit NaN-hole backdrop (nanPlaneUpdate)
#include <vtkLineSource.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkTextActor3D.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
// Globe (geographic orthographic) view mode: lon/lat/z -> a real sphere, seen through the parallel
// camera. vtkGeneralTransform is the only VTK transform that can concatenate a NON-LINEAR one, and
// vtkSphericalTransform is the (r,phi,theta) -> (x,y,z) half. See sceneGlobeTransform (10_geometry).
#include <vtkGeneralTransform.h>
#include <vtkSphericalTransform.h>
// …and the CUBE body of that same mode (PROJ's +proj=qsc). vtkWarpTransform is VTK's base for a
// non-linear transform written in C++ — it supplies the point/normal/vector plumbing every
// vtkTransformPolyDataFilter needs, leaving only the forward/inverse point maps to fill in.
#include <vtkWarpTransform.h>
#include <vtkLight.h>
#include <vtkMath.h>
// vtkSMPTools pulls in TBB's profiling.h, whose `void emit()` method collides with Qt's `emit`
// keyword macro (-> `void ()` -> syntax error). Undef around the include, then restore Qt's macro.
#pragma push_macro("emit")
#undef emit
#include <vtkSMPTools.h>            // parallel-for (TBB backend) for the hillshade / PBR bakes
#pragma pop_macro("emit")
#include <vtkMatrix4x4.h>
#include <vtkPolyDataAlgorithm.h>
#include <vtkAlgorithmOutput.h>
#include <vtkPolyDataNormals.h>
#include <vtkTriangleFilter.h>
#include <vtkContourTriangulator.h>
#include <vtkAppendPolyData.h>
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
#include <vtkChartLegend.h>
#include <vtkPlot.h>
#include <vtkPlotPoints.h>
#include <vtkPlotLine.h>
#include <vtkContext2D.h>
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
#include <vtkTextRenderer.h>          // pixel extent of a string in the billboards' own text engine
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

