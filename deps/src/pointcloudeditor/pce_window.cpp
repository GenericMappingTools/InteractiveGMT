// ============================================================================
//  pce_window.cpp -- MB-System's pointCloudEditor (src/pointCloudEditor/: PointCloudEditor.cpp,
//  PointsSelectInteractorStyle.cpp, ZScaleCallback.cpp; src/qt-guilib/: RadioButtonGroup.cpp and the
//  grid-to-surface step of TopoDataReader.cpp), ported into the viewer.
//
//  The VTK scene is the original's, class for class: the surface coloured by each point's data
//  quality (green good, red bad) through a two-entry lookup table, the rubber-band pick style ('r'
//  toggles select mode) that marks the points inside the band BAD (ERASE) or GOOD (RESTORE), the
//  vertical exaggeration slider widget, the ERASE / RESTORE textured radio buttons, and -elev's
//  elevation profile (the band's two ends picked on the data, a vertical plane cut through it, red
//  pins and line on the surface, a 2-D chart below). It lives in a Qt window (QVTKOpenGLNativeWidget)
//  instead of a bare vtkRenderWindow, with File > Open for the command line's file and Options >
//  Elevation profile for -elev.
//
//  Departures from the original, each where the original is broken rather than a choice:
//    - visualize() no longer calls vtkRenderWindowInteractor::Start(): every call (slider, each
//      selection) started one more nested event loop. Qt runs the one loop; visualize() redraws.
//    - the quality array is marked Modified() after an edit, without which the mapper never
//      recoloured the erased points;
//    - the edit mode starts as ERASE, with its button on (the original left it uninitialized);
//    - the selection tests the points as DISPLAYED (z times the vertical exaggeration): the original
//      extracted from the unexaggerated data with a frustum taken on the exaggerated view, so at any
//      exaggeration but 1 it selected the wrong points;
//    - the selection keeps vtkExtractPolyDataGeometry's rule (a triangle whose three points are all
//      inside the frustum; its points are the selection) but evaluates it here, which keeps a VTK
//      module the viewer does not otherwise ship out of the runtime;
//    - each elevation profile replaces the chart instead of stacking one more renderer per profile;
//    - SwathData::getXYZ took x from the northing minimum and y from the easting minimum
//      (mbev_grid.boundsutm is xmin, xmax, ymin, ymax); here x and y come from their own bounds.
//    - the swath file is gridded in mbeditviz's browse mode, so loading it opens no edit save file.
// ============================================================================

#include "pce_window.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QUiLoader>
#include <QVBoxLayout>

#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkAreaPicker.h>
#include <vtkAxis.h>
#include <vtkButtonRepresentation.h>
#include <vtkButtonWidget.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkChartXY.h>
#include <vtkCommand.h>
#include <vtkContextActor.h>
#include <vtkContextScene.h>
#include <vtkCutter.h>
#include <vtkFloatArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkIntArray.h>
#include <vtkInteractorStyleRubberBandPick.h>
#include <vtkLookupTable.h>
#include <vtkMath.h>
#include <vtkNamedColors.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPNGWriter.h>
#include <vtkPlane.h>
#include <vtkPlanes.h>
#include <vtkPlot.h>
#include <vtkPointData.h>
#include <vtkPointPicker.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkSliderRepresentation2D.h>
#include <vtkSliderWidget.h>
#include <vtkSmartPointer.h>
#include <vtkSphereSource.h>
#include <vtkTable.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkTexturedButtonRepresentation2D.h>
#include <vtkWindowToImageFilter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "../mbeditviz/mbeditviz.h"
#include "../mbeditviz/mbeditviz_window.h"
extern "C" {
#include "../mbgrdviz/mbgrdviz.h"
}

namespace {

#define VTKISRBP_ORIENT 0
#define VTKISRBP_SELECT 1

// Quality flag values
#define DATA_QUALITY_NAME "dataQuality"
#define GOOD 1
#define BAD 0

// TopoData::NoData
const double kNoData = -10000000.;

// Edit modes
typedef enum {
	EraseMode,
	RestoreMode
} EditMode;

struct Pce;
Pce *g_pce = nullptr;
void pceVisualize(Pce *m);

// ---- PointsSelectInteractorStyle ---------------------------------------------------------------
class PointsSelectInteractorStyle : public vtkInteractorStyleRubberBandPick {
public:
	static PointsSelectInteractorStyle *New();
	vtkTypeMacro(PointsSelectInteractorStyle, vtkInteractorStyleRubberBandPick);

	enum class SelectionMode {
		Points,
		ElevSlice
	};

	void setSelectMode(SelectionMode mode) {
		selectMode_ = mode;
	}

	void OnLeftButtonUp() override;
	void OnChar() override;

	// the host's test driver: a band from (x0,y0) to (x1,y1), VTK display pixels, released in select mode
	void rubberBand(vtkRenderer *ren, int x0, int y0, int x1, int y1) {
		CurrentMode = VTKISRBP_SELECT;
		SetCurrentRenderer(ren);
		StartPosition[0] = x0;
		StartPosition[1] = y0;
		EndPosition[0] = x1;
		EndPosition[1] = y1;
		Moving = 1;
		OnLeftButtonUp();
	}
	bool selecting() const {
		return CurrentMode == VTKISRBP_SELECT;
	}

	vtkSmartPointer<vtkActor> selectedActor_;
	vtkSmartPointer<vtkPolyDataMapper> selectedMapper_;

protected:
	PointsSelectInteractorStyle() {
		selectedMapper_ = vtkSmartPointer<vtkPolyDataMapper>::New();
		selectedActor_ = vtkSmartPointer<vtkActor>::New();
		selectedActor_->SetMapper(selectedMapper_);
	}
	void computeElevationProfile();
	SelectionMode selectMode_ = SelectionMode::Points;
};
vtkStandardNewMacro(PointsSelectInteractorStyle);

// ---- RadioButtonGroup / EditModeGroup ----------------------------------------------------------
class EditModeGroup : public vtkCommand {
public:
	static EditModeGroup *New() {
		return new EditModeGroup;
	}
	void Execute(vtkObject *caller, unsigned long event, void *calldata) override;
	bool processAction(int selectedIndex);
	void addButton(vtkButtonWidget *button) {
		buttons_.push_back(button);
	}
	void setInteractor(vtkRenderWindowInteractor *interactor) {
		interactor_ = interactor;
	}
	void select(int index);   // the state Execute leaves: `index` on, every other button off
	std::vector<vtkSmartPointer<vtkButtonWidget>> buttons_;
	vtkSmartPointer<vtkRenderWindowInteractor> interactor_;
};

// ---- ZScaleCallback ----------------------------------------------------------------------------
class ZScaleCallback : public vtkCallbackCommand {
public:
	static ZScaleCallback *New() {
		return new ZScaleCallback;
	}
	void Execute(vtkObject *caller, unsigned long, void *) override;
};

// ---- the editor --------------------------------------------------------------------------------
struct Pce {
	MbParking *parking = nullptr;        // X / minimise park it in Scene Objects (mbParkable)
	PceHost host;
	QMainWindow *win = nullptr;
	QVTKOpenGLNativeWidget *canvas = nullptr;
	QAction *actElev = nullptr;
	vtkSmartPointer<vtkGenericOpenGLRenderWindow> renderWindow_;
	vtkRenderWindowInteractor *renderWindowInteractor_ = nullptr;

	bool displayElevProfile_ = false;
	vtkNew<vtkAreaPicker> areaPicker_;
	vtkNew<vtkLookupTable> qualityLUT_;
	vtkNew<vtkNamedColors> colors_;
	vtkNew<vtkPolyDataMapper> mapper_;
	vtkNew<vtkActor> actor_;
	std::vector<vtkSmartPointer<vtkActor>> addedActors_;   // pins and profile lines
	vtkNew<vtkRenderer> renderer3D_;
	vtkSmartPointer<vtkRenderer> renderer2D_;              // the elevation profile chart
	vtkNew<PointsSelectInteractorStyle> style_;
	vtkSmartPointer<vtkPolyData> polyData_;
	vtkNew<vtkSliderRepresentation2D> sliderRep_;
	vtkNew<vtkSliderWidget> sliderWidget_;
	vtkNew<EditModeGroup> editModeGroup_;
	std::vector<vtkSmartPointer<vtkTextActor>> buttonLabels_;
	vtkSmartPointer<vtkIntArray> quality_ = vtkSmartPointer<vtkIntArray>::New();
	EditMode editMode_ = EraseMode;
	double verticalExagg_ = 1.;
	bool firstRender_ = true;
	int nProfile_ = 0;
};

bool setSurfaceOpacity(Pce *m, float opacity) {
	if (opacity < 0. || opacity > 1.0)
		return false;
	m->actor_->GetProperty()->SetOpacity(opacity);
	return true;
}

// PointCloudEditor::createColorImage
vtkSmartPointer<vtkImageData> createColorImage(const std::string &color) {
	vtkNew<vtkNamedColors> colors;
	std::array<unsigned char, 3> dc{0, 0, 0};
	auto c = colors->GetColor3ub(color).GetData();
	for (auto i = 0; i < 3; ++i)
		dc[i] = c[i];
	vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
	image->SetDimensions(10, 10, 1);
	image->AllocateScalars(VTK_UNSIGNED_CHAR, 3);
	auto dims = image->GetDimensions();
	for (int y = 0; y < dims[1]; y++) {
		for (int x = 0; x < dims[0]; x++) {
			unsigned char *pixel = static_cast<unsigned char *>(image->GetScalarPointer(x, y, 0));
			for (int i = 0; i < 3; ++i)
				pixel[i] = dc[i];
		}
	}
	return image;
}

// PointCloudEditor::buildWidgets
void buildWidgets(Pce *m) {
	m->sliderRep_->SetMinimumValue(1.0);
	m->sliderRep_->SetMaximumValue(20.0);
	m->sliderRep_->SetValue(m->verticalExagg_);
	m->sliderRep_->SetTitleText("vertical exaggeration");
	m->sliderRep_->GetSliderProperty()->SetColor(m->colors_->GetColor3d("Green").GetData());
	m->sliderRep_->GetTitleProperty()->SetColor(m->colors_->GetColor3d("AliceBlue").GetData());
	m->sliderRep_->GetLabelProperty()->SetColor(m->colors_->GetColor3d("AliceBlue").GetData());
	m->sliderRep_->GetSelectedProperty()->SetColor(m->colors_->GetColor3d("DeepPink").GetData());
	m->sliderRep_->GetTubeProperty()->SetColor(m->colors_->GetColor3d("MistyRose").GetData());
	m->sliderRep_->GetCapProperty()->SetColor(m->colors_->GetColor3d("Yellow").GetData());
	m->sliderRep_->SetSliderLength(0.05);
	m->sliderRep_->SetSliderWidth(0.025);
	m->sliderRep_->SetEndCapLength(0.02);
	m->sliderRep_->GetPoint1Coordinate()->SetCoordinateSystemToNormalizedDisplay();
	m->sliderRep_->GetPoint1Coordinate()->SetValue(0.2, 0.1);
	m->sliderRep_->GetPoint2Coordinate()->SetCoordinateSystemToNormalizedDisplay();
	m->sliderRep_->GetPoint2Coordinate()->SetValue(0.8, 0.1);
	m->sliderWidget_->SetInteractor(m->renderWindowInteractor_);
	m->sliderWidget_->SetRepresentation(m->sliderRep_);
	m->sliderWidget_->SetAnimationModeToAnimate();
	m->sliderWidget_->EnabledOn();
	vtkNew<ZScaleCallback> callback;
	m->sliderWidget_->AddObserver(vtkCommand::EndInteractionEvent, callback);

	std::array<std::string, 3> onColors = {"Gray", "Gray", "Gray"};
	std::array<std::string, 3> offColors = {"Silver", "Silver", "Silver"};
	m->editModeGroup_->setInteractor(m->renderWindowInteractor_);
	const int nRadioButtons = 2;
	for (int i = 0; i < nRadioButtons; ++i) {
		vtkNew<vtkTexturedButtonRepresentation2D> buttonRepresentation;
		vtkSmartPointer<vtkTextActor> textActor = vtkSmartPointer<vtkTextActor>::New();
		if (i == 0)
			textActor->SetInput("ERASE");
		else if (i == 1)
			textActor->SetInput("RESTORE");
		textActor->GetTextProperty()->SetColor(0., 0., 0.);
		buttonRepresentation->SetNumberOfStates(2);                             // Two states: on and off.
		buttonRepresentation->SetButtonTexture(0, createColorImage(offColors[i]));   // State 0: off.
		buttonRepresentation->SetButtonTexture(1, createColorImage(onColors[i]));    // State 1: on.
		std::array<double, 6> bounds{0.0, 0.0, 100.0, 250.0, 0.0, 0.0};
		bounds[0] = 215.0 + i * 60.0;   // Adjust for spacing.
		bounds[1] = bounds[0] + 50.0;
		buttonRepresentation->PlaceWidget(bounds.data());
		textActor->SetDisplayPosition(int(bounds[0] + 12), int(bounds[2] + 20));
		m->renderer3D_->AddViewProp(textActor);	// AddActor2D: gone in VTK 9.7 (macOS CI)
		m->buttonLabels_.push_back(textActor);
		vtkNew<vtkButtonWidget> buttonWidget;
		buttonWidget->SetInteractor(m->renderWindowInteractor_);
		buttonWidget->SetRepresentation(buttonRepresentation);
		buttonWidget->EnabledOn();
		m->editModeGroup_->addButton(buttonWidget);
		buttonWidget->AddObserver(vtkCommand::StateChangedEvent, m->editModeGroup_);
	}
	m->editModeGroup_->select(m->editMode_ == EraseMode ? 0 : 1);
}

// PointCloudEditor::visualize (without the event loop the original started on every call)
void pceVisualize(Pce *m) {
	if (!m->polyData_)
		return;
	const float zScale = float(m->verticalExagg_);
	m->mapper_->SetInputData(m->polyData_);
	m->mapper_->SetLookupTable(m->qualityLUT_);
	m->polyData_->GetPointData()->SetActiveScalars(DATA_QUALITY_NAME);
	m->mapper_->SetScalarModeToUsePointData();
	m->mapper_->SetColorModeToMapScalars();
	m->mapper_->SetScalarRange(0, 1);
	m->actor_->GetProperty()->SetPointSize(5);
	m->actor_->SetMapper(m->mapper_);
	m->actor_->SetScale(1., 1., zScale);
	m->renderer3D_->UseHiddenLineRemovalOn();
	if (m->displayElevProfile_)
		m->renderer3D_->SetViewport(0., 0., 0.5, 1.0);   // viewport for the elevation profile 2D graph
	else
		m->renderer3D_->SetViewport(0., 0., 1.0, 1.0);
	m->renderWindow_->AddRenderer(m->renderer3D_);
	m->renderWindowInteractor_->SetPicker(m->areaPicker_);
	m->renderer3D_->AddActor(m->actor_);
	for (vtkActor *actor : m->addedActors_)
		m->renderer3D_->AddActor(actor);
	m->renderer3D_->SetBackground(m->colors_->GetColor3d("Tan").GetData());
	if (m->firstRender_) {
		buildWidgets(m);
		m->firstRender_ = false;
		m->renderer3D_->ResetCamera();
	}
	m->renderWindowInteractor_->SetInteractorStyle(m->style_);
	// a new exaggeration moves the surface along z: the clipping range (which also bounds the pick
	// frustum) follows it, or the surface leaves the view
	m->renderer3D_->ResetCameraClippingRange();
	m->renderWindow_->Render();
}

void EditModeGroup::select(int index) {
	for (size_t i = 0; i < buttons_.size(); ++i) {
		auto *rep = vtkButtonRepresentation::SafeDownCast(buttons_[i]->GetRepresentation());
		if (rep)
			rep->SetState(int(i) == index ? 1 : 0);
	}
}

// RadioButtonGroup::Execute
void EditModeGroup::Execute(vtkObject *caller, unsigned long event, void *) {
	if (event == vtkCommand::StateChangedEvent) {
		vtkButtonWidget *pressedButton = static_cast<vtkButtonWidget *>(caller);
		int pressedButtonIndex = -1;
		for (size_t i = 0; i < buttons_.size(); ++i) {
			if (buttons_[i] == pressedButton) {
				pressedButtonIndex = int(i);
				break;
			}
		}
		select(pressedButtonIndex);
		if (interactor_)
			interactor_->GetRenderWindow()->Render();
		processAction(pressedButtonIndex);
	}
}

// EditModeGroup::processAction
bool EditModeGroup::processAction(int selectedIndex) {
	if (!g_pce)
		return false;
	if (selectedIndex == 0)
		g_pce->editMode_ = EraseMode;
	else
		g_pce->editMode_ = RestoreMode;
	return true;
}

// ZScaleCallback::Execute
void ZScaleCallback::Execute(vtkObject *caller, unsigned long, void *) {
	vtkSliderWidget *sliderWidget = reinterpret_cast<vtkSliderWidget *>(caller);
	const double value = static_cast<vtkSliderRepresentation *>(sliderWidget->GetRepresentation())->GetValue();
	if (!g_pce)
		return;
	g_pce->verticalExagg_ = value;
	pceVisualize(g_pce);
}

// PointsSelectInteractorStyle::OnChar
void PointsSelectInteractorStyle::OnChar() {
	const int startingMode = CurrentMode;
	vtkInteractorStyleRubberBandPick::OnChar();
	if (startingMode == VTKISRBP_SELECT && CurrentMode != VTKISRBP_SELECT && g_pce) {
		// Just left select mode: remove the selected area actor
		g_pce->renderer3D_->RemoveActor(selectedActor_);
		pceVisualize(g_pce);
	}
}

// PointsSelectInteractorStyle::OnLeftButtonUp
void PointsSelectInteractorStyle::OnLeftButtonUp() {
	// Forward events
	vtkInteractorStyleRubberBandPick::OnLeftButtonUp();
	if (CurrentMode != VTKISRBP_SELECT || !g_pce || !g_pce->polyData_)
		return;
	Pce *m = g_pce;
	if (selectMode_ == SelectionMode::ElevSlice) {
		computeElevationProfile();
		return;
	}
	vtkNew<vtkNamedColors> colors;
	vtkPlanes *frustum = static_cast<vtkAreaPicker *>(GetInteractor()->GetPicker())->GetFrustum();

	// vtkExtractPolyDataGeometry, ExtractInside on, boundary cells off: a cell is extracted when every
	// one of its points is inside the frustum (implicit function < 0); the points of the extracted
	// cells are the selection. Points are taken as displayed (z times the exaggeration).
	vtkPoints *pts = m->polyData_->GetPoints();
	const vtkIdType npts = pts->GetNumberOfPoints();
	std::vector<signed char> inside(size_t(npts), 0);
	for (vtkIdType i = 0; i < npts; i++) {
		double p[3];
		pts->GetPoint(i, p);
		p[2] *= m->verticalExagg_;
		inside[size_t(i)] = frustum->EvaluateFunction(p) < 0.0 ? 1 : 0;
	}
	std::vector<char> selected(size_t(npts), 0);
	vtkCellArray *polys = m->polyData_->GetPolys();
	vtkIdType ncellpts;
	const vtkIdType *cellpts;
	for (polys->InitTraversal(); polys->GetNextCell(ncellpts, cellpts);) {
		bool all = true;
		for (vtkIdType k = 0; k < ncellpts && all; k++)
			all = inside[size_t(cellpts[k])] != 0;
		if (all)
			for (vtkIdType k = 0; k < ncellpts; k++)
				selected[size_t(cellpts[k])] = 1;
	}
	vtkNew<vtkPoints> selPts;
	vtkNew<vtkCellArray> selVerts;
	for (vtkIdType i = 0; i < npts; i++) {
		if (!selected[size_t(i)])
			continue;
		double p[3];
		pts->GetPoint(i, p);
		const vtkIdType id = selPts->InsertNextPoint(p);
		selVerts->InsertNextCell(1, &id);
		// Set selected point data quality
		m->quality_->SetValue(i, m->editMode_ == EraseMode ? BAD : GOOD);
	}
	m->quality_->Modified();
	vtkNew<vtkPolyData> extractedData;
	extractedData->SetPoints(selPts);
	extractedData->SetVerts(selVerts);

	// Set mapper input to extracted cells (Color is not controlled by scalar)
	selectedMapper_->SetInputData(extractedData);
	selectedMapper_->ScalarVisibilityOff();
	selectedActor_->GetProperty()->SetColor(colors->GetColor3d("Black").GetData());
	selectedActor_->GetProperty()->SetPointSize(1);
	selectedActor_->GetProperty()->SetRepresentationToPoints();
	selectedActor_->SetScale(1., 1., m->verticalExagg_);
	m->renderer3D_->AddActor(selectedActor_);
	GetInteractor()->GetRenderWindow()->Render();

	// Highlight the selected area
	HighlightProp(nullptr);
	pceVisualize(m);
}

// PointsSelectInteractorStyle::computeElevationProfile
void PointsSelectInteractorStyle::computeElevationProfile() {
	Pce *m = g_pce;
	vtkRenderer *renderer = m->renderer3D_;

	// Find world coordinates of start and end points
	vtkNew<vtkPointPicker> picker;
	double startPoint[3], endPoint[3];
	double *p;
	if (picker->Pick(static_cast<double>(StartPosition[0]), static_cast<double>(StartPosition[1]), 0, renderer)) {
		p = picker->GetPickPosition();
		std::copy(p, p + 3, startPoint);
	}
	else {
		return;   // Could not pick StartPosition
	}
	if (picker->Pick(static_cast<double>(EndPosition[0]), static_cast<double>(EndPosition[1]), 0, renderer)) {
		p = picker->GetPickPosition();
		std::copy(p, p + 3, endPoint);
	}
	else {
		return;   // Could not pick EndPosition
	}

	// Put a little sphere ("pin") at start and end points
	for (const double *c : {startPoint, endPoint}) {
		vtkNew<vtkSphereSource> pin;
		pin->SetCenter(c[0], c[1], c[2]);
		pin->SetRadius(50.);
		pin->SetPhiResolution(50);
		pin->SetThetaResolution(50);
		vtkNew<vtkPolyDataMapper> pinMapper;
		pinMapper->SetInputConnection(pin->GetOutputPort());
		vtkSmartPointer<vtkActor> pinActor = vtkSmartPointer<vtkActor>::New();
		pinActor->SetMapper(pinMapper);
		pinActor->GetProperty()->SetColor(1., 0., 0.);
		pinActor->GetProperty()->SetLineWidth(3.);
		m->addedActors_.push_back(pinActor);
	}

	// Compute normal to elevation profile plane; elevation profile plane is vertical,
	// so normal to plane is horizontal
	double normal[3];
	normal[0] = -(endPoint[1] - startPoint[1]);
	normal[1] = endPoint[0] - startPoint[0];
	normal[2] = 0.0;   // normal to z-axis is horizontal
	vtkMath::Normalize(normal);

	// Create the elevation profile plane
	vtkNew<vtkPlane> plane;
	plane->SetOrigin(endPoint);
	plane->SetNormal(normal);

	// Create the cutter filter
	vtkNew<vtkCutter> cutter;
	cutter->SetInputData(m->polyData_);
	cutter->SetCutFunction(plane);
	cutter->Update();

	// Display profile on main 3D surface
	vtkNew<vtkPolyDataMapper> profileMapper;
	profileMapper->SetInputConnection(cutter->GetOutputPort());
	vtkSmartPointer<vtkActor> profileActor = vtkSmartPointer<vtkActor>::New();
	profileActor->SetMapper(profileMapper);
	profileActor->GetProperty()->SetColor(1., 0., 0.);
	profileActor->GetProperty()->SetLineWidth(3.);
	m->addedActors_.push_back(profileActor);
	setSurfaceOpacity(m, 0.3);

	// Extract elev profile data for display in 2D graph
	vtkPolyData *profilePolyData = cutter->GetOutput();
	vtkPoints *points = profilePolyData->GetPoints();
	if (!points || points->GetNumberOfPoints() == 0) {
		m->nProfile_ = 0;
		pceVisualize(m);
		return;   // No elevation profile intersection found
	}

	// Get profile direction vector
	double direction[3];
	direction[0] = endPoint[0] - startPoint[0];
	direction[1] = endPoint[1] - startPoint[1];
	direction[2] = endPoint[2] - startPoint[2];
	vtkMath::Normalize(direction);

	// Fill profileData with sorted (x,y) data for plotting; x: distance along profile, y: elevation
	std::vector<std::pair<double, double>> profileData;
	for (vtkIdType i = 0; i < points->GetNumberOfPoints(); i++) {
		double point[3];
		points->GetPoint(i, point);
		if ((point[0] >= startPoint[0] && point[0] <= endPoint[0]) ||
		    (point[0] >= endPoint[0] && point[0] <= startPoint[0])) {
			// This point lies on line between startPoint and endPoint; add it to profileData
			double vec[3];
			vec[0] = point[0] - startPoint[0];
			vec[1] = point[1] - startPoint[1];
			vec[2] = point[2] - startPoint[2];
			const double distAlongProfile = vtkMath::Dot(vec, direction);
			const double elevation = point[2];
			profileData.push_back({distAlongProfile, elevation});
		}
	}
	std::sort(profileData.begin(), profileData.end());
	m->nProfile_ = int(profileData.size());

	// Create table for chart
	vtkNew<vtkTable> table;
	vtkNew<vtkFloatArray> xArray;
	xArray->SetName("Distance");
	table->AddColumn(xArray);
	vtkNew<vtkFloatArray> yArray;
	yArray->SetName("Elevation (m)");
	table->AddColumn(yArray);
	table->SetNumberOfRows(vtkIdType(profileData.size()));
	for (size_t i = 0; i < profileData.size(); i++) {
		table->SetValue(vtkIdType(i), 0, profileData[i].first);    // distance
		table->SetValue(vtkIdType(i), 1, profileData[i].second);   // elevation
	}

	// the chart's renderer: one, its content replaced by every profile
	if (m->renderer2D_)
		m->renderWindow_->RemoveRenderer(m->renderer2D_);
	m->renderer2D_ = vtkSmartPointer<vtkRenderer>::New();
	m->renderer2D_->SetViewport(0., 0., 1.0, 0.25);
	m->renderer2D_->SetBackground(1., 1., 1.);
	m->renderWindow_->AddRenderer(m->renderer2D_);
	vtkNew<vtkChartXY> chart;
	vtkNew<vtkContextScene> scene;
	vtkNew<vtkContextActor> actor;
	scene->AddItem(chart);
	actor->SetScene(scene);
	m->renderer2D_->AddActor(actor);
	vtkPlot *line = chart->AddPlot(vtkChart::LINE);
	line->SetInputData(table, 0, 1);
	line->SetColor(0, 0, 255, 255);   // blue
	line->SetWidth(2.0);
	chart->SetShowLegend(false);
	chart->GetAxis(vtkAxis::BOTTOM)->SetTitle("Distance");
	chart->GetAxis(vtkAxis::BOTTOM)->GetTitleProperties()->SetFontSize(20);
	chart->GetAxis(vtkAxis::BOTTOM)->GetLabelProperties()->SetFontSize(20);
	chart->GetAxis(vtkAxis::LEFT)->SetTitle("Elevation (m)");
	chart->GetAxis(vtkAxis::LEFT)->GetTitleProperties()->SetFontSize(20);
	chart->GetAxis(vtkAxis::LEFT)->GetLabelProperties()->SetFontSize(20);
	pceVisualize(m);
}

// ---- reading: TopoDataReader's grid-to-surface step --------------------------------------------
// nRows x nCols nodes, node (row, col) is point col + row * nCols (TopoDataReader::gridOffset); two
// triangles per cell, none touching a node without data
void pceSetData(Pce *m, int nRows, int nColumns, const std::vector<double> &x, const std::vector<double> &y,
                const std::vector<double> &z) {
	vtkNew<vtkPoints> topoDataPoints;
	topoDataPoints->SetDataTypeToDouble();
	topoDataPoints->Allocate(vtkIdType(nRows) * nColumns);
	bool gridMissingZValues = false;
	for (int row = 0; row < nRows; row++) {
		for (int col = 0; col < nColumns; col++) {
			const size_t k = size_t(col) + size_t(row) * size_t(nColumns);
			double zz = z[k];
			if (std::isnan(zz) || zz == kNoData) {
				gridMissingZValues = true;
				zz = kNoData;
			}
			topoDataPoints->InsertNextPoint(x[k], y[k], zz);
		}
	}
	auto missing = [&](const vtkIdType *v) {
		for (int i = 0; i < 3; i++)
			if (topoDataPoints->GetPoint(v[i])[2] == kNoData)
				return true;
		return false;
	};
	vtkNew<vtkCellArray> topoDataPolygons;
	vtkIdType tv[3];
	for (int row = 0; row < nRows - 1; row++) {
		for (int col = 0; col < nColumns - 1; col++) {
			tv[0] = col + vtkIdType(row) * nColumns;
			tv[1] = (col + 1) + vtkIdType(row) * nColumns;
			tv[2] = (col + 1) + vtkIdType(row + 1) * nColumns;
			if (!gridMissingZValues || !missing(tv))
				topoDataPolygons->InsertNextCell(3, tv);
			tv[0] = col + vtkIdType(row) * nColumns;
			tv[1] = (col + 1) + vtkIdType(row + 1) * nColumns;
			tv[2] = col + vtkIdType(row + 1) * nColumns;
			if (!gridMissingZValues || !missing(tv))
				topoDataPolygons->InsertNextCell(3, tv);
		}
	}
	m->polyData_ = vtkSmartPointer<vtkPolyData>::New();
	m->polyData_->SetPoints(topoDataPoints);
	m->polyData_->SetPolys(topoDataPolygons);

	// PointCloudEditor::readPolyData: first assume all points are good
	m->quality_ = vtkSmartPointer<vtkIntArray>::New();
	m->quality_->SetName(DATA_QUALITY_NAME);
	m->quality_->SetNumberOfTuples(m->polyData_->GetNumberOfPoints());
	for (vtkIdType i = 0; i < m->polyData_->GetNumberOfPoints(); i++)
		m->quality_->SetValue(i, GOOD);
	m->polyData_->GetPointData()->AddArray(m->quality_);

	// a new data set: the previous one's selection, pins and profile go
	m->renderer3D_->RemoveActor(m->style_->selectedActor_);
	for (vtkActor *a : m->addedActors_)
		m->renderer3D_->RemoveActor(a);
	m->addedActors_.clear();
	if (m->renderer2D_) {
		m->renderWindow_->RemoveRenderer(m->renderer2D_);
		m->renderer2D_ = nullptr;
	}
	m->nProfile_ = 0;
	setSurfaceOpacity(m, 1.0);
	m->renderer3D_->ResetCamera();
}

// a grid of an InteractiveGMT window: geographic ones go to UTM, as TopoDataReader converts them
bool pceLoadGrid(Pce *m, const MbGrdVizGrid &g) {
	if (g.nx < 2 || g.ny < 2)
		return false;
	const int nRows = g.ny, nColumns = g.nx;
	std::vector<double> x(size_t(nRows) * nColumns), y(x.size()), z(x.size());
	void *pj = nullptr;
	if (g.geographic) {
		// TopoDataReader: "+proj=utm +zone=%d +datum=WGS84" with zone (xMin + 180) / 6 + 0.5
		const int utmZone = int((g.x0 + 180) / 6 + 0.5);
		char id[32];
		snprintf(id, sizeof(id), "UTM%02dN", utmZone);
		if (!mbgrdviz_proj_init(id, &pj)) {
			QMessageBox::warning(m->win, "pointCloudEditor", QString("Unable to set up the projection %1").arg(id));
			return false;
		}
	}
	for (int row = 0; row < nRows; row++) {
		for (int col = 0; col < nColumns; col++) {
			const size_t k = size_t(col) + size_t(row) * size_t(nColumns);
			double xx = g.x0 + col * (g.x1 - g.x0) / (nColumns - 1);
			double yy = g.y0 + row * (g.y1 - g.y0) / (nRows - 1);
			if (pj) {
				double e = 0, n = 0;
				mbgrdviz_proj_forward(pj, xx, yy, &e, &n);
				xx = e;
				yy = n;
			}
			x[k] = xx;
			y[k] = yy;
			z[k] = g.z[size_t(col) * size_t(nRows) + size_t(row)];   // column-major, row 0 = south
		}
	}
	if (pj)
		mbgrdviz_proj_free(&pj);
	pceSetData(m, nRows, nColumns, x, y, z);
	return true;
}

// swath data: SwathData::readDatafile, gridded by the ported mbeditviz engine
int pceShowMessage(char *message) {
	if (g_pce && g_pce->host.base.busyText)
		g_pce->host.base.busyText(message);
	return 1;
}
int pceHideMessage(void) {
	return 1;
}
void pceUpdateGui(void) {
	QApplication::processEvents();
}
int pceShowErrorDialog(char *s1, char *s2, char *s3) {
	QMessageBox::warning(g_pce ? g_pce->win : nullptr, "pointCloudEditor",
	                     QString::fromUtf8(s1) + "\n" + QString::fromUtf8(s2) + "\n" + QString::fromUtf8(s3));
	return 1;
}

bool pceLoadSwath(Pce *m, const QString &file) {
	int st[1] = {0};
	if (mbeditvizState(st, 1) > 0 && st[0]) {
		QMessageBox::information(m->win, "pointCloudEditor",
		                         "mbeditviz is open: a swath file is gridded by mbeditviz's engine, which serves one "
		                         "tool at a time. Close mbeditviz first.");
		return false;
	}
	char msg[2048] = "";
	if (!mbeditviz_mbio_open(msg, int(sizeof(msg)))) {
		QMessageBox::warning(m->win, "pointCloudEditor", QString::fromUtf8(msg));
		return false;
	}
	static char programName[] = "pointCloudEditor";
	static char helpMsg[] = "pointCloudEditor: grid a swath file and edit its points";
	static char usageMsg[] = "pointCloudEditor [-elev] <swath-or-gridFile>";
	mbeditviz_init(programName, helpMsg, usageMsg, pceShowMessage, pceHideMessage, pceUpdateGui, pceShowErrorDialog);
	mbev_mode_output = MBEV_OUTPUT_MODE_BROWSE;   // nothing is edited: no edit save file is opened

	QByteArray f = QDir::fromNativeSeparators(file).toUtf8();
	int sonarFormat = 0;
	if (mbeditviz_get_format(f.data(), &sonarFormat) != MB_SUCCESS) {
		QMessageBox::warning(m->win, "pointCloudEditor", "Couldn't determine sonar format of " + file);
		return false;
	}
	if (m->host.base.busyText)
		m->host.base.busyText("Gridding swath data...");
	bool ok = mbeditviz_import_file(f.data(), sonarFormat) == MB_SUCCESS && mbev_num_files > 0 &&
	          mbeditviz_load_file(0, false) == MB_SUCCESS;
	if (ok) {
		mbeditviz_get_grid_bounds();
		mbeditviz_setup_grid();
		mbeditviz_project_soundings();
		mbeditviz_make_grid();
		ok = mbev_grid.status != MBEV_GRID_NONE && mbev_grid.n_rows > 1 && mbev_grid.n_columns > 1 && mbev_grid.val;
	}
	if (ok) {
		const int nRows = mbev_grid.n_rows, nColumns = mbev_grid.n_columns;
		std::vector<double> x(size_t(nRows) * nColumns), y(x.size()), z(x.size());
		for (int row = 0; row < nRows; row++) {
			for (int col = 0; col < nColumns; col++) {
				const size_t k = size_t(col) + size_t(row) * size_t(nColumns);
				x[k] = mbev_grid.boundsutm[0] + col * mbev_grid.dx;
				y[k] = mbev_grid.boundsutm[2] + row * mbev_grid.dy;
				const float v = mbev_grid.val[size_t(col) * size_t(nRows) + size_t(row)];
				z[k] = (v == mbev_grid.nodatavalue) ? kNoData : double(v);
			}
		}
		pceSetData(m, nRows, nColumns, x, y, z);
	}
	// the engine back to empty: grid freed, file unloaded and dropped
	mbeditviz_destroy_grid();
	for (int i = mbev_num_files - 1; i >= 0; i--) {
		mbeditviz_unload_file(i, false);
		mbeditviz_delete_file(i);
	}
	if (m->host.base.busyOff)
		m->host.base.busyOff();
	if (!ok)
		QMessageBox::warning(m->win, "pointCloudEditor", "Couldn't load data from " + file);
	return ok;
}

bool pceOpenFile(Pce *m, const QString &file) {
	const QString ext = QFileInfo(file).suffix().toLower();
	bool ok = false;
	if (ext.startsWith("mb"))
		ok = pceLoadSwath(m, file);
	else {
		// a grid: opened through the viewer's file door, in a window of its own, then read off it
		void *w = m->host.openFile ? m->host.openFile(nullptr, QDir::toNativeSeparators(file).toUtf8().constData()) : nullptr;
		MbGrdVizGrid g;
		ok = w && m->host.grid && m->host.grid(w, g) && pceLoadGrid(m, g);
		if (!ok)
			QMessageBox::warning(m->win, "pointCloudEditor", "Couldn't read a grid from " + file);
	}
	if (ok) {
		m->win->setWindowTitle("pointCloudEditor: " + QFileInfo(file).fileName());
		pceVisualize(m);
	}
	return ok;
}

void pceSetElevMode(Pce *m, bool on) {
	m->displayElevProfile_ = on;
	m->style_->setSelectMode(on ? PointsSelectInteractorStyle::SelectionMode::ElevSlice
	                            : PointsSelectInteractorStyle::SelectionMode::Points);
	if (!on && m->renderer2D_) {
		m->renderWindow_->RemoveRenderer(m->renderer2D_);
		m->renderer2D_ = nullptr;
	}
	if (m->actElev && m->actElev->isChecked() != on) {
		const QSignalBlocker b(m->actElev);
		m->actElev->setChecked(on);
	}
	pceVisualize(m);
}

Pce *pceBuild(QWidget *parent, const PceHost &host) {
	auto *m = new Pce;
	m->host = host;
	QFile f(QDir(host.base.uiDir).filePath("pointcloudeditor.ui"));
	if (!f.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, "pointCloudEditor", QString("Cannot open %1").arg(f.fileName()));
		delete m;
		return nullptr;
	}
	QUiLoader loader;
	auto *win = qobject_cast<QMainWindow *>(loader.load(&f, parent));
	QWidget *hostW = win ? win->findChild<QWidget *>("canvasHost") : nullptr;
	QAction *actOpen = win ? win->findChild<QAction *>("actionOpen") : nullptr;
	QAction *actQuit = win ? win->findChild<QAction *>("actionQuit") : nullptr;
	m->actElev = win ? win->findChild<QAction *>("actionElevProfile") : nullptr;
	if (!win || !hostW || !actOpen || !actQuit || !m->actElev) {
		QMessageBox::warning(parent, "pointCloudEditor", "pointcloudeditor.ui is incomplete");
		delete win;
		delete m;
		return nullptr;
	}
	m->win = win;
	win->setAttribute(Qt::WA_DeleteOnClose);
	if (!host.base.icon.isNull())
		win->setWindowIcon(host.base.icon);
	auto *lay = new QVBoxLayout(hostW);
	lay->setContentsMargins(0, 0, 0, 0);
	m->canvas = new QVTKOpenGLNativeWidget(hostW);
	m->canvas->setFocusPolicy(Qt::StrongFocus);
	lay->addWidget(m->canvas);
	m->renderWindow_ = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
	m->canvas->setRenderWindow(m->renderWindow_);
	m->renderWindowInteractor_ = m->renderWindow_->GetInteractor();

	// PointCloudEditor's constructor: the colour lookup table
	m->qualityLUT_->SetNumberOfTableValues(2);
	m->qualityLUT_->SetRange(0, 1);
	m->qualityLUT_->SetTableValue(BAD, 1.0, 0.0, 0.0, 1.0);
	m->qualityLUT_->SetTableValue(GOOD, 0.0, 1.0, 0.0, 1.0);
	m->qualityLUT_->Build();
	m->renderer3D_->SetBackground(m->colors_->GetColor3d("Tan").GetData());
	m->renderWindow_->AddRenderer(m->renderer3D_);
	m->renderWindowInteractor_->SetPicker(m->areaPicker_);
	m->renderWindowInteractor_->SetInteractorStyle(m->style_);

	QObject::connect(actOpen, &QAction::triggered, win, [m]() {
		const QString fn = QFileDialog::getOpenFileName(m->win, "Open grid or swath file", m->host.base.startDir(),
		                                                "Grids and swath files (*.grd *.nc *.tif *.mb*);;All Files (*)");
		if (fn.isEmpty())
			return;
		m->host.base.rememberDir(fn);
		pceOpenFile(m, fn);
	});
	QObject::connect(actQuit, &QAction::triggered, win, [m]() { mbParkQuit(m->parking); });
	QObject::connect(m->actElev, &QAction::toggled, win, [m](bool on) { pceSetElevMode(m, on); });
	m->parking = mbParkable(win, host.base, "pointCloudEditor");
	return m;
}

} // namespace

// ---- entry point ---------------------------------------------------------------------------
bool pceOpenWindow(QWidget *parent, const PceHost &host, void *win, const QString &file, bool elevProfile) {
	if (!g_pce) {
		// MBIO: the projection to UTM and the swath reader both come from it
		if (!mbeditLoadMbio(parent, host.base))
			return false;
		char msg[2048] = "";
		if (!mbgrdviz_mbio_open(msg, int(sizeof(msg)))) {
			QMessageBox::warning(parent, "pointCloudEditor", QString::fromUtf8(msg));
			return false;
		}
		Pce *m = pceBuild(parent, host);
		if (!m)
			return false;
		g_pce = m;
		if (host.base.windowOpened)
			host.base.windowOpened();
		QObject::connect(m->win, &QObject::destroyed, [m]() {
			if (m->host.base.windowClosed)
				m->host.base.windowClosed();
			if (g_pce == m)
				g_pce = nullptr;
			delete m;
		});
		m->win->show();
		QApplication::processEvents();
	}
	Pce *m = g_pce;
	mbParkRebind(m->parking, win ? win : host.base.parkScene);   // parks in the window it edits
	mbParkShow(m->parking);                              // a parked one comes back off its handle
	if (elevProfile != m->displayElevProfile_)
		pceSetElevMode(m, elevProfile);
	bool ok = true;
	if (win) {
		MbGrdVizGrid g;
		ok = m->host.alive && m->host.alive(win) && m->host.grid && m->host.grid(win, g) && pceLoadGrid(m, g);
		if (!ok)
			QMessageBox::information(m->win, "pointCloudEditor", "That window has no grid to edit.");
		else {
			m->win->setWindowTitle("pointCloudEditor: " + QString::fromStdString(g.name));
			pceVisualize(m);
		}
	}
	if (!file.isEmpty())
		ok = pceOpenFile(m, file) && ok;
	return ok;
}

int pceState(int *out, int n) {
	Pce *m = g_pce;
	int nbad = 0;
	if (m && m->polyData_)
		for (vtkIdType i = 0; i < m->quality_->GetNumberOfTuples(); i++)
			if (m->quality_->GetValue(i) == BAD)
				nbad++;
	const int v[8] = {m ? 1 : 0,
	                  (m && m->polyData_) ? int(m->polyData_->GetNumberOfPoints()) : 0,
	                  (m && m->polyData_) ? int(m->polyData_->GetNumberOfPolys()) : 0,
	                  nbad,
	                  m ? (m->editMode_ == EraseMode ? 0 : 1) : 0,
	                  (m && m->style_->selecting()) ? 1 : 0,
	                  (m && m->displayElevProfile_) ? 1 : 0,
	                  m ? m->nProfile_ : 0};
	const int k = n < 8 ? n : 8;
	for (int i = 0; i < k; i++)
		out[i] = v[i];
	return k;
}

bool pceSetEditMode(int mode) {
	if (!g_pce || g_pce->firstRender_)
		return false;
	const int index = mode == 0 ? 0 : 1;
	g_pce->editModeGroup_->select(index);
	g_pce->editModeGroup_->processAction(index);
	g_pce->renderWindow_->Render();
	return true;
}

bool pceSetVerticalExagg(double value) {
	if (!g_pce || value <= 0)
		return false;
	g_pce->verticalExagg_ = value;
	g_pce->sliderRep_->SetValue(value);
	pceVisualize(g_pce);
	return true;
}

bool pceRubberBand(int x0, int y0, int x1, int y1) {
	Pce *m = g_pce;
	if (!m || !m->polyData_)
		return false;
	// Qt's top-left logical pixels -> VTK's bottom-left device pixels
	const double dpr = m->canvas->devicePixelRatioF();
	const int h = m->renderWindow_->GetSize()[1];
	m->style_->rubberBand(m->renderer3D_, int(x0 * dpr), h - 1 - int(y0 * dpr), int(x1 * dpr), h - 1 - int(y1 * dpr));
	return true;
}

bool pceSetElevProfile(bool on) {
	if (!g_pce)
		return false;
	pceSetElevMode(g_pce, on);
	return true;
}

bool pceCanvasSize(int *w, int *h) {
	if (!g_pce)
		return false;
	*w = g_pce->canvas->width();
	*h = g_pce->canvas->height();
	return true;
}

bool pceSavePng(const QString &path) {
	if (!g_pce)
		return false;
	g_pce->renderWindow_->Render();
	vtkNew<vtkWindowToImageFilter> w2i;
	w2i->SetInput(g_pce->renderWindow_);
	w2i->ReadFrontBufferOff();
	w2i->Update();
	vtkNew<vtkPNGWriter> png;
	png->SetFileName(path.toUtf8().constData());
	png->SetInputConnection(w2i->GetOutputPort());
	png->Write();
	return QFile::exists(path);
}

bool pceClose() {
	if (!g_pce)
		return false;
	mbParkQuit(g_pce->parking);
	return true;
}
