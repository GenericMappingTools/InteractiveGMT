// ============================================================================
//  mbeditviz_window.cpp -- mbeditviz's file list window: the port of MB-System's
//  src/mbeditviz/mbeditviz_callbacks.c (the Motif interface of mbeditviz) onto Qt, plus the glue
//  between the engine and the InteractiveGMT survey map that stands in for libmbview.
//
//  A translation unit of its own (see mbeditviz_window.h). The engine (mbeditviz.c) holds the
//  files, the grid and the selection and does every load, grid, select, edit and bias step; this
//  file turns widgets into the same engine calls the Motif callbacks made, in the same order.
//
//  What libmbview did, and what stands in for it:
//    - the survey viewer (mbview_init/open/setprimarygrid/addnav): an InteractiveGMT window with the
//      grid (projected, metres) and every loaded file's navigation, through MbEditVizHost;
//    - the region / area / navigation picks: a shape drawn on that map with the viewer's own Draw
//      tools (Rectangle, Line, Polygon...), chosen by name in this window's "Survey map" box. Region
//      takes the shape's bounding box, Area the line from its first to its last vertex with a width of
//      `aspect` x its length (mbview's areaaspect, 0.5), Nav the pings whose fix lies inside it. The
//      engine reads them from the mbview stand-ins of mbeditviz.h, filled here;
//    - the action buttons (Update Bathymetry Grid, Enable/Disable Secondary Picks): this window;
//    - mbview_plothigh / mbview_update*grid: the map's grid is replaced in place;
//    - mbview_colorvalue_instance: the colour the map's colour bar gives that height;
//    - the dismiss notify: the map window closing (seen by the same one-second timer that keeps the
//      file list's lock and esf marks current).
// ============================================================================

#include "mbeditviz_window.h"
#include "mbeditviz.h"
#include "mb3dsoundings_window.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QUiLoader>

#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *const mbev_grid_algorithm_label[] = {"Simple Mean", "Footprint", "Shoal Bias"};

// mbview's default map colours (MBV_COLORTABLE_HAXBY, normal: index 0 at the maximum)
const double kHaxby[11][3] = {{0.950, 0.950, 0.950}, {1.000, 0.729, 0.522}, {1.000, 0.631, 0.267}, {1.000, 0.741, 0.341},
                              {0.941, 0.925, 0.475}, {0.804, 1.000, 0.635}, {0.541, 0.925, 0.682}, {0.416, 0.922, 1.000},
                              {0.196, 0.745, 1.000}, {0.157, 0.498, 0.984}, {0.145, 0.224, 0.686}};

// ---- mbeditviz_callbacks.c's globals --------------------------------------------------------
struct MbEditViz {
	MbParking *parking = nullptr;        // X / minimise park it in Scene Objects (mbParkable)
	MbEditVizHost host;
	QMainWindow *win = nullptr;

	QListWidget *fileList = nullptr;
	QLabel *labelStatus = nullptr;
	QPushButton *viewAllButton = nullptr, *viewSelectedButton = nullptr, *removeButton = nullptr;
	QRadioButton *modeEdit = nullptr, *modeBrowse = nullptr;
	QAction *actOpen = nullptr, *actNewGrid = nullptr;
	QGroupBox *mapGroup = nullptr;
	QComboBox *shapeCombo = nullptr;
	QString lastSelectedLine;                // the line selected on the map when the list was last filled
	QStringList lastShapes;                  // the map's shapes then (shapeNames)
	QDoubleSpinBox *areaAspect = nullptr;
	QCheckBox *secondaryCheck = nullptr;

	// dialogs, built on first use and kept
	QDialog *gridDlg = nullptr, *openDlg = nullptr;
	QLabel *labelCurrent = nullptr, *labelImplied = nullptr, *cellSizeValue = nullptr;
	QSlider *cellSizeSlider = nullptr;
	QRadioButton *algButton[3] = {};
	QLineEdit *interpolationEdit = nullptr;
	QLineEdit *openFile = nullptr;
	QSpinBox *openFormat = nullptr;
	QRadioButton *openEdit = nullptr, *openBrowse = nullptr;

	int mformat = -1;
	QTimer *timer = nullptr;

	// the survey map (libmbview's instance 0)
	void *map = nullptr;
	std::vector<double> cz, crgb;            // its colour nodes, fixed when it opened (mbview kept min/max)
	bool mapDirty = false;
	// the map is in the WINDOW's coordinates: lon/lat when that window is a geographic map (the engine's
	// grid stays UTM; mapLattice / mapShapes convert at the boundary, as mbview's display projection did)
	bool mapGeo = false;

	// the mbview selections the engine reads
	mbview_struct view{};
	mbview_shareddata_struct shared{};
	std::vector<mbview_nav_struct> navs;
	std::vector<std::vector<mbview_navpointw_struct>> navpts;
};
MbEditViz *g_mv = nullptr;

void doUpdateGui();
void doUpdateFilelist();
int doMbviewDismissNotify(size_t instance);

int doMessageOn(char *message) {
	mbev_message_on = true;
	if (g_mv && g_mv->host.base.busyText)
		g_mv->host.base.busyText(message);
	return 1;
}

int doMessageOff() {
	mbev_message_on = false;
	if (g_mv && g_mv->host.base.busyOff)
		g_mv->host.base.busyOff();
	return 1;
}

int doErrorDialog(char *s1, char *s2, char *s3) {
	QApplication::beep();
	QMessageBox::warning(g_mv ? g_mv->win : nullptr, "MBeditviz",
	                     QString::fromUtf8(s1) + "\n" + QString::fromUtf8(s2) + "\n" + QString::fromUtf8(s3));
	return 1;
}

int msgOn(const QString &s) {
	QByteArray b = s.toUtf8();
	return doMessageOn(b.data());
}

// ---- the survey map ---------------------------------------------------------------------------
bool mapAlive() {
	return g_mv && g_mv->map && g_mv->host.mapAlive && g_mv->host.mapAlive(g_mv->map);
}

// mbev_grid.val with the no data value as NaN, the way the viewer's grids carry it
std::vector<float> mapGridValues() {
	const size_t n = (size_t)mbev_grid.n_columns * (size_t)mbev_grid.n_rows;
	std::vector<float> z(n);
	for (size_t k = 0; k < n; k++)
		z[k] = (mbev_grid.val[k] == mbev_grid.nodatavalue) ? NAN : mbev_grid.val[k];
	return z;
}

// The grid as the map shows it, in the window's coordinates. A UTM window (or a new one): the engine's
// own grid, untouched. A geographic window: the same number of nodes laid over the grid's lon/lat
// bounds, each taking the value of the UTM node it falls on (nearest node: no value is invented or
// smoothed, a hole stays a hole).
struct MapLattice {
	std::vector<float> z;
	int nx = 0, ny = 0;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};
MapLattice mapLattice() {
	MapLattice L;
	const int nc = mbev_grid.n_columns, nr = mbev_grid.n_rows;
	const double ux0 = mbev_grid.boundsutm[0], uy0 = mbev_grid.boundsutm[2];
	if (!g_mv->mapGeo) {
		L.z = mapGridValues();
		L.nx = nc;
		L.ny = nr;
		L.x0 = ux0;
		L.x1 = ux0 + (nc - 1) * mbev_grid.dx;
		L.y0 = uy0;
		L.y1 = uy0 + (nr - 1) * mbev_grid.dy;
		return L;
	}
	L.nx = nc;
	L.ny = nr;
	L.x0 = mbev_grid.bounds[0];
	L.x1 = mbev_grid.bounds[1];
	L.y0 = mbev_grid.bounds[2];
	L.y1 = mbev_grid.bounds[3];
	L.z.assign((size_t)nc * (size_t)nr, NAN);
	for (int i = 0; i < nc; i++) {
		const double lon = L.x0 + i * (L.x1 - L.x0) / (nc - 1);
		for (int j = 0; j < nr; j++) {
			const double lat = L.y0 + j * (L.y1 - L.y0) / (nr - 1);
			double x, y;
			int err = MB_ERROR_NO_ERROR;
			mb_proj_forward(mbev_verbose, mbev_grid.pjptr, lon, lat, &x, &y, &err);
			const long ii = std::lround((x - ux0) / mbev_grid.dx), jj = std::lround((y - uy0) / mbev_grid.dy);
			if (ii < 0 || ii >= nc || jj < 0 || jj >= nr)
				continue;
			const float v = mbev_grid.val[(size_t)ii * nr + (size_t)jj];
			L.z[(size_t)i * nr + (size_t)j] = (v == mbev_grid.nodatavalue) ? NAN : v;
		}
	}
	return L;
}

void mapRefresh() {
	MbEditViz *m = g_mv;
	if (!mapAlive() || mbev_grid.val == nullptr || !m->host.mapUpdate)
		return;
	const MapLattice L = mapLattice();
	m->host.mapUpdate(m->map, L.z.data(), L.nx, L.ny, L.x0, L.x1, L.y0, L.y1, m->mapGeo ? 1 : 0, m->cz.data(),
	                  m->crgb.data(), (int)m->cz.size());
	m->mapDirty = false;
}

// the map's own handle goes first, so that closing it from here is not taken for the user's dismiss
void mapClose() {
	MbEditViz *m = g_mv;
	void *map = m->map;
	m->map = nullptr;
	if (map && m->host.mapClose)
		m->host.mapClose(map);   // lets go of the handle; the grid (if still there) stays in its window
}

// grid height at (x, y) of the map, or 0 (mbview_getzdata, nearest node)
double mapZAt(double x, double y) {
	if (mbev_grid.val == nullptr || mbev_grid.dx <= 0.0 || mbev_grid.dy <= 0.0)
		return 0.0;
	const int i = (int)lround((x - mbev_grid.boundsutm[0]) / mbev_grid.dx);
	const int j = (int)lround((y - mbev_grid.boundsutm[2]) / mbev_grid.dy);
	if (i < 0 || i >= mbev_grid.n_columns || j < 0 || j >= mbev_grid.n_rows)
		return 0.0;
	const float v = mbev_grid.val[i * mbev_grid.n_rows + j];
	return (v == mbev_grid.nodatavalue) ? 0.0 : v;
}

// ---- do_mbeditviz_update_gui / _update_filelist ----------------------------------------------
void doUpdateGui() {
	MbEditViz *m = g_mv;
	if (!m)
		return;

	/* set status text */
	mbev_num_files_loaded = 0;
	mbev_num_pings_loaded = 0;
	mbev_num_soundings_loaded = 0;
	mbev_num_soundings_secondary = 0;
	for (int i = 0; i < mbev_num_files; i++) {
		struct mbev_file_struct *file = &(mbev_files[i]);
		if (file->load_status) {
			mbev_num_files_loaded++;
			mbev_num_pings_loaded += file->num_pings;
			for (int j = 0; j < file->num_pings; j++) {
				struct mbev_ping_struct *ping = &(file->pings[j]);
				for (int k = 0; k < ping->beams_bath; k++) {
					if (!mb_beam_check_flag_unusable(ping->beamflag[k]))
						mbev_num_soundings_loaded++;
					if (mb_beam_check_flag_multipick(ping->beamflag[k]))
						mbev_num_soundings_secondary++;
				}
			}
		}
	}

	char string[MB_PATH_MAXLINE];
	if (mbev_grid.status == MBEV_GRID_NONE)
		snprintf(string, sizeof(string), "Available Files: %d\nLoaded Files: %d\nGrid Not Generated", mbev_num_files,
		         mbev_num_files_loaded);
	else
		snprintf(string, sizeof(string),
		         "Available Files: %d\nLoaded Files: %d\nGrid:\n  Lon: %f %f\n  Lat: %f %f\n  Cell Size: %f m\n  Algorithm: %d\n"
		         "  Interpolation: %d\n  Dimensions: %d %d",
		         mbev_num_files, mbev_num_files_loaded, mbev_grid.bounds[0], mbev_grid.bounds[1], mbev_grid.bounds[2],
		         mbev_grid.bounds[3], mbev_grid.dx, mbev_grid_algorithm, mbev_grid_interpolation, mbev_grid.n_columns,
		         mbev_grid.n_rows);
	m->labelStatus->setText(QString::fromUtf8(string));

	/* build available file list */
	doUpdateFilelist();

	/*set sensitivity */
	m->actOpen->setEnabled(mbev_grid.status == MBEV_GRID_NONE);
	const bool idle = (mbev_num_files > 0 && mbev_grid.status == MBEV_GRID_NONE);
	m->removeButton->setEnabled(idle);
	m->viewSelectedButton->setEnabled(idle);
	m->viewAllButton->setEnabled(idle);
	m->modeEdit->setEnabled(idle);
	m->modeBrowse->setEnabled(idle);
	m->actNewGrid->setEnabled(!idle);

	/* the survey map box: what mbview's own controls did */
	m->mapGroup->setEnabled(mbev_grid.status == MBEV_GRID_VIEWED);
	m->secondaryCheck->setEnabled(mbev_num_soundings_secondary > 0);
}

void doUpdateFilelist() {
	MbEditViz *m = g_mv;
	if (!m)
		return;
	/* check to see if anything has changed */
	bool update_filelist = (m->fileList->count() != mbev_num_files);

	/* check for change in load status, lock status, or esf status */
	for (int i = 0; i < mbev_num_files; i++) {
		struct mbev_file_struct *file = &(mbev_files[i]);

		/* check load status */
		if (file->load_status != file->load_status_shown) {
			file->load_status_shown = file->load_status;
			update_filelist = true;
		}

		/* swath file locking variables */
		int lock_error = MB_ERROR_NO_ERROR;
		bool locked = false;
		int lock_purpose;
		mb_path lock_program;
		mb_path lock_cpu;
		mb_path lock_user;
		char lock_date[25];

		/* check for locks */
		mb_pr_lockinfo(mbev_verbose, mbev_files[i].path, &locked, &lock_purpose, lock_program, lock_user, lock_cpu, lock_date,
		               &lock_error);
		if (locked != file->locked) {
			file->locked = locked;
			update_filelist = true;
		}

		/* check for edit save file */
		char save_file[MB_PATH_MAXLINE + 40];
		snprintf(save_file, sizeof(save_file), "%s.esf", mbev_files[i].path);
		struct stat file_status;
		const bool esf_exists = (stat(save_file, &file_status) == 0 && (file_status.st_mode & S_IFMT) != S_IFDIR);
		if (esf_exists != file->esf_exists) {
			file->esf_exists = esf_exists;
			update_filelist = true;
		}
	}

	/* only update the filelist if necessary */
	if (update_filelist) {
		/* get the current selection, if any, from the list */
		const int item_count = m->fileList->count();
		std::vector<int> position_list_save;
		for (int i = 0; i < item_count; i++)
			if (m->fileList->item(i)->isSelected())
				position_list_save.push_back(i);

		/* delete existing file list */
		QSignalBlocker b(m->fileList);
		m->fileList->clear();

		/* build available file list */
		for (int i = 0; i < mbev_num_files; i++) {
			// the file and its format only: no Motif status prefixes (<loaded>/<Locked>/<esf>/HSA)
			char string[MB_PATH_MAXLINE + 40];
			snprintf(string, sizeof(string), "%s %d", mbev_files[i].name, mbev_files[i].format);
			m->fileList->addItem(QString::fromUtf8(string));
		}

		/* reinstate selection if the number of items is the same as before */
		if (item_count == mbev_num_files)
			for (int i : position_list_save)
				if (i < m->fileList->count())
					m->fileList->item(i)->setSelected(true);
	}
}

// the positions selected in the file list (XmNselectedPositions, 0-based here)
std::vector<int> selectedPositions() {
	std::vector<int> v;
	for (int i = 0; i < g_mv->fileList->count(); i++)
		if (g_mv->fileList->item(i)->isSelected())
			v.push_back(i);
	return v;
}

// ---- do_mbeditviz_viewgrid ---------------------------------------------------------------------
void doViewGrid() {
	MbEditViz *m = g_mv;

	/* display grid */
	if (mbev_status == MB_SUCCESS && mbev_grid.status == MBEV_GRID_NOTVIEWED) {
		/* the primary colour table: Haxby, normal, over the grid's min..max */
		m->cz.clear();
		m->crgb.clear();
		for (int k = 0; k <= 10; k++) {
			m->cz.push_back(mbev_grid.min + k * (mbev_grid.max - mbev_grid.min) / 10.0);
			for (int c = 0; c < 3; c++)
				m->crgb.push_back(kHaxby[10 - k][c]);
		}

		/* open up the survey viewer: into the window mbeditviz was opened from, as a new element named
		   for what it is, in THAT window's coordinates */
		m->mapGeo = m->host.mapWindowGeographic && m->host.mapWindowGeographic(m->host.base.parkScene);
		const MapLattice L = mapLattice();
		const QByteArray title = QString("mbeditviz bathymetry (%1)").arg(mbev_grid.projection_id).toUtf8();
		m->map = m->host.mapOpen ? m->host.mapOpen(m->host.base.parkScene, title.constData(), L.z.data(), L.nx, L.ny, L.x0,
		                                           L.x1, L.y0, L.y1, m->mapGeo ? 1 : 0, m->cz.data(), m->crgb.data(),
		                                           (int)m->cz.size())
		                         : nullptr;
		mbev_instance = 0;

		/* set grid status */
		if (m->map != nullptr) {
			mbev_grid.status = MBEV_GRID_VIEWED;
			// the tool belongs to the window its map is in: it parks there, and goes when that window goes
			if (m->host.mapScene)
				mbParkRebind(m->parking, m->host.mapScene(m->map));
		}
		else
			mbev_status = MB_FAILURE;

		/* add navigation to view */
		if (mbev_status == MB_SUCCESS)
			for (int ifile = 0; ifile < mbev_num_files; ifile++) {
				struct mbev_file_struct *file = &(mbev_files[ifile]);
				if (file->load_status && file->num_pings > 0) {
					/* set message */
					msgOn(QString("Loading nav %1 of %2...").arg(ifile + 1).arg(mbev_num_files));
					std::vector<double> xyz;
					xyz.reserve(3 * (size_t)file->num_pings);
					for (int iping = 0; iping < file->num_pings; iping++) {
						struct mbev_ping_struct *ping = &(file->pings[iping]);
						double x = ping->navlon, y = ping->navlat;      // a geographic window: as recorded
						if (!m->mapGeo)
							mb_proj_forward(mbev_verbose, mbev_grid.pjptr, ping->navlon, ping->navlat, &x, &y, &mbev_error);
						xyz.push_back(x);
						xyz.push_back(y);
						xyz.push_back(0.0);
					}
					const int segoff[2] = {0, file->num_pings};
					if (m->host.mapAddLines)
						m->host.mapAddLines(m->map, xyz.data(), file->num_pings, segoff, 1, 0.0, 0.0, 0.0, 2.0, file->name,
						                    "MBeditviz navigation");
				}
			}
		doMessageOff();

		/* the secondary picks state (MBV_STATEMASK_20 / _21): off */
		m->view.state21 = 0;
		{
			QSignalBlocker b(m->secondaryCheck);
			m->secondaryCheck->setChecked(false);
		}
		// NO raise of the map's window here (mbview popped its own survey window to the front). The grid
		// now lands in the iGMT window mbeditviz was opened from; raising THAT window buried mbeditviz and
		// every dialog it owns behind it. The tool stays in front, the window shows its new element.
	}

	/* reset the gui */
	doUpdateGui();
}

// ---- do_mbeditviz_mbview_dismiss_notify -------------------------------------------------------
int doMbviewDismissNotify(size_t instance) {
	(void)instance;

	/* destroy any mb3dsoundings window */
	mb3dsdgEnd();
	mbeditviz_mb3dsoundings_dismiss();

	/* destroy the grid */
	if (mbev_grid.status != MBEV_GRID_NONE)
		mbeditviz_destroy_grid();

	/* reset the gui */
	doUpdateGui();

	return (mbev_status);
}

// ---- do_mbeditviz_updategrid / _gridparameters / _changecellsize ---------------------------------
void doUpdateGrid() {
	/* the previous survey map goes with its grid */
	mapClose();
	doMbviewDismissNotify(0);

	/* loop over all files to be sure all files are loaded */
	int loadcount = 0;
	for (int ifile = 0; ifile < mbev_num_files; ifile++) {
		struct mbev_file_struct *file = &(mbev_files[ifile]);
		if (file->load_status)
			loadcount++;
	}

	/* make the grid and display grid */
	if (mbev_status == MB_SUCCESS && loadcount > 0) {
		/* make the grid */
		msgOn("Making grid...");
		mbev_status = mbeditviz_setup_grid();
		mbeditviz_project_soundings();
		mbev_status = mbeditviz_make_grid();

		/* display grid */
		doViewGrid();

		doMessageOff();
	}
	else {
		doMessageOff();
		QApplication::beep();
	}

	/* reset the gui */
	doUpdateGui();

	/* reset status */
	mbev_status = MB_SUCCESS;
	mbev_error = MB_ERROR_NO_ERROR;
}

void doSetLabelImplied() {
	MbEditViz *m = g_mv;
	char string[MB_PATH_MAXLINE];
	snprintf(string, sizeof(string),
	         "Selected Grid Parameters:\n    Cell Size: %.2f m\n    Dimensions: %d %d\n    Algorithm: %s\n    Interpolation: %d cell gaps",
	         mbev_grid_cellsize, mbev_grid_n_columns, mbev_grid_n_rows, mbev_grid_algorithm_label[mbev_grid_algorithm],
	         mbev_grid_interpolation);
	m->labelImplied->setText(QString::fromUtf8(string));
}

void doChangeCellSize(int icellsize) {
	MbEditViz *m = g_mv;
	mbev_grid_cellsize = 0.001 * icellsize;
	m->cellSizeValue->setText(QString::number(mbev_grid_cellsize, 'f', 3));

	/* reset the scale maximum */
	int iscalemax = m->cellSizeSlider->maximum();
	if (icellsize <= 1) {
		iscalemax /= 2;
		QSignalBlocker b(m->cellSizeSlider);
		m->cellSizeSlider->setMaximum(MAX(2, iscalemax));
	}
	else if (icellsize == iscalemax) {
		iscalemax *= 2;
		QSignalBlocker b(m->cellSizeSlider);
		m->cellSizeSlider->setMaximum(iscalemax);
	}

	/* get updated grid dimensions */
	mbev_grid_n_columns = (mbev_grid_boundsutm[1] - mbev_grid_boundsutm[0]) / mbev_grid_cellsize + 1;
	mbev_grid_n_rows = (mbev_grid_boundsutm[3] - mbev_grid_boundsutm[2]) / mbev_grid_cellsize + 1;

	doSetLabelImplied();
}

void doGridAlgorithmChange() {
	MbEditViz *m = g_mv;
	/* get grid definition values from the dismissed dialog */
	if (m->algButton[0]->isChecked())
		mbev_grid_algorithm = MBEV_GRID_ALGORITHM_SIMPLEMEAN;
	else if (m->algButton[1]->isChecked())
		mbev_grid_algorithm = MBEV_GRID_ALGORITHM_FOOTPRINT;
	else
		mbev_grid_algorithm = MBEV_GRID_ALGORITHM_SHOALBIAS;

	/* get interpolation scale */
	sscanf(m->interpolationEdit->text().toLatin1().constData(), "%d", &mbev_grid_interpolation);

	doSetLabelImplied();
}

QWidget *loadUi(const char *file, QWidget *parent) {
	QFile f(QDir(g_mv->host.base.uiDir).filePath(QString::fromLatin1(file)));
	if (!f.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, "MBeditviz", QString("Cannot open %1").arg(f.fileName()));
		return nullptr;
	}
	QUiLoader loader;
	QWidget *w = loader.load(&f, parent);
	if (!w)
		QMessageBox::warning(parent, "MBeditviz", QString("Cannot load %1").arg(f.fileName()));
	return w;
}

template <typename T>
T *child(QWidget *root, const char *name, QStringList &missing) {
	T *w = root->findChild<T *>(QString::fromLatin1(name));
	if (!w)
		missing << QString::fromLatin1(name);
	return w;
}

// The tool's own dialogs (Grid Parameters, Open) are windows of their own: whatever takes the tool or
// its grid away -- park, minimise, Quit, a re-open on new data, the grid's Apply, the map going --
// takes them with it, gone from the screen NOW (painted away before any long work starts), never left
// standing as ghosts with nothing behind them.
void hideToolDialogs(bool paintNow = true) {
	MbEditViz *m = g_mv;
	if (!m)
		return;
	bool hid = false;
	for (QDialog *d : {m->gridDlg, m->openDlg})
		if (d && d->isVisible()) {
			d->hide();
			hid = true;
		}
	if (hid && paintNow)
		QApplication::processEvents();
}

bool buildGridDialog() {
	MbEditViz *m = g_mv;
	if (m->gridDlg)
		return true;
	QWidget *w = loadUi("mbeditviz_grid.ui", m->win);
	auto *d = qobject_cast<QDialog *>(w);
	if (!d) {
		delete w;
		return false;
	}
	QStringList missing;
	m->labelCurrent = child<QLabel>(d, "labelCurrent", missing);
	m->labelImplied = child<QLabel>(d, "labelImplied", missing);
	m->cellSizeSlider = child<QSlider>(d, "cellSizeSlider", missing);
	m->cellSizeValue = child<QLabel>(d, "cellSizeSliderValue", missing);
	m->algButton[0] = child<QRadioButton>(d, "algSimpleMean", missing);
	m->algButton[1] = child<QRadioButton>(d, "algFootprint", missing);
	m->algButton[2] = child<QRadioButton>(d, "algShoalBias", missing);
	m->interpolationEdit = child<QLineEdit>(d, "interpolationEdit", missing);
	auto *apply = child<QPushButton>(d, "applyButton", missing);
	auto *dismiss = child<QPushButton>(d, "dismissButton", missing);
	if (!missing.isEmpty()) {
		QMessageBox::warning(m->win, "MBeditviz", "mbeditviz_grid.ui lacks: " + missing.join(", "));
		delete d;
		return false;
	}
	m->gridDlg = d;
	d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
	auto *g = new QButtonGroup(d);
	for (int i = 0; i < 3; i++) {
		g->addButton(m->algButton[i], i);
		QObject::connect(m->algButton[i], &QRadioButton::toggled, d, [](bool on) {
			if (on)
				doGridAlgorithmChange();
		});
	}
	QObject::connect(m->interpolationEdit, &QLineEdit::editingFinished, d, []() { doGridAlgorithmChange(); });
	QObject::connect(m->cellSizeSlider, &QSlider::valueChanged, d, [](int v) {
		g_mv->cellSizeValue->setText(QString::number(0.001 * v, 'f', 3));
		if (!g_mv->cellSizeSlider->isSliderDown())
			doChangeCellSize(v);
	});
	QObject::connect(m->cellSizeSlider, &QSlider::sliderReleased, d, []() { doChangeCellSize(g_mv->cellSizeSlider->value()); });
	// pushButton_gridparameters_apply: dismiss the dialog, then do_mbeditviz_updategrid
	QObject::connect(apply, &QPushButton::clicked, d, []() {
		doGridAlgorithmChange();
		hideToolDialogs();
		doUpdateGrid();
	});
	QObject::connect(dismiss, &QPushButton::clicked, d, []() { hideToolDialogs(); });
	return true;
}

void doGridParameters() {
	MbEditViz *m = g_mv;
	if (!buildGridDialog())
		return;

	/* get calculated grid parameters */
	mbeditviz_get_grid_bounds();

	/* set the widgets */
	const int icellsize = (int)(1000 * mbev_grid_cellsize);
	{
		QSignalBlocker b(m->cellSizeSlider);
		m->cellSizeSlider->setRange(1, MAX(2, 5 * icellsize));
		m->cellSizeSlider->setValue(icellsize);
	}
	m->cellSizeValue->setText(QString::number(mbev_grid_cellsize, 'f', 3));
	for (int i = 0; i < 3; i++) {
		QSignalBlocker b(m->algButton[i]);
		m->algButton[i]->setChecked(i == (int)mbev_grid_algorithm);
	}
	m->interpolationEdit->setText(QString::number(mbev_grid_interpolation));

	const double xx = (mbev_grid_boundsutm[1] - mbev_grid_boundsutm[0]);
	const double yy = (mbev_grid_boundsutm[3] - mbev_grid_boundsutm[2]);
	char string[MB_PATH_MAXLINE];
	snprintf(string, sizeof(string),
	         "Grid Bounds:\n    Longitude: %10.5f %10.5f  | %6.3f km  | %9.3f m\n    Latitude: %9.5f %9.5f | %6.3f km  | %9.3f m\n"
	         "Suggested Grid Parameters:\n    Cell Size: %.2f m\n    Dimensions: %d %d",
	         mbev_grid_bounds[0], mbev_grid_bounds[1], 0.001 * xx, xx, mbev_grid_bounds[2], mbev_grid_bounds[3], 0.001 * yy, yy,
	         mbev_grid_cellsize, mbev_grid_n_columns, mbev_grid_n_rows);
	m->labelCurrent->setText(QString::fromUtf8(string));

	doSetLabelImplied();
	m->gridDlg->show();
	m->gridDlg->raise();
	m->gridDlg->activateWindow();
}

// ---- do_mbeditviz_viewall / _viewselected / _regrid / _deleteselected / _quit --------------------
// the survey map and its sounding editor go before the grid is rebuilt (mbview_destroy)
void destroyViews() {
	if (mbev_grid.status == MBEV_GRID_VIEWED) {
		/* destroy any mb3dsoundings window */
		mb3dsdgEnd();
		mbeditviz_mb3dsoundings_dismiss();
		mapClose();
		mbev_grid.status = MBEV_GRID_NOTVIEWED;
	}

	/* destroy old grid */
	if (mbev_grid.status != MBEV_GRID_NONE)
		mbeditviz_destroy_grid();
}

bool doViewAll(bool showDialog) {
	destroyViews();

	/* loop over all files to be sure all files are loaded */
	msgOn("Loading files...");
	int loadcount = 0;
	for (int ifile = 0; ifile < mbev_num_files; ifile++) {
		struct mbev_file_struct *file = &(mbev_files[ifile]);
		if (!file->load_status) {
			msgOn(QString("Loading file %1 of %2...").arg(ifile + 1).arg(mbev_num_files));
			// Load file, asserting lock
			mbeditviz_load_file(ifile, true);
		}
		loadcount++;
	}
	doMessageOff();

	/* put up dialog on grid parameters */
	const bool ok = (mbev_status == MB_SUCCESS && loadcount > 0);
	if (ok) {
		if (showDialog)
			doGridParameters();
		else
			mbeditviz_get_grid_bounds();
	}
	else
		QApplication::beep();

	/* reset the gui */
	doUpdateGui();
	return ok;
}

void doViewSelected() {
	destroyViews();

	/* get positions of selected list items */
	const std::vector<int> position_list = selectedPositions();

	/* loop over all files to be sure selected files are loaded */
	msgOn("Loading files...");
	int loadcount = 0;
	for (int ifile = 0; ifile < mbev_num_files; ifile++) {
		/* find out if file is in selected list */
		bool selected = false;
		for (int p : position_list)
			if (ifile == p)
				selected = true;

		/* load unloaded selected files, unload loaded unselected files */
		struct mbev_file_struct *file = &(mbev_files[ifile]);
		if (selected && !file->load_status) {
			loadcount++;
			msgOn(QString("Loading file %1 of %2...").arg(loadcount).arg(position_list.size()));
			// Load file, asserting lock
			mbeditviz_load_file(ifile, true);
		}
		else if (selected && file->load_status) {
			loadcount++;
		}
		else if (!selected && file->load_status) {
			// Unload file, asserting unlock
			mbeditviz_unload_file(ifile, true);
		}
	}
	doMessageOff();

	/* put up dialog on grid parameters */
	if (mbev_status == MB_SUCCESS && loadcount > 0)
		doGridParameters();
	else
		QApplication::beep();

	/* reset the gui */
	doUpdateGui();

	/* reset status */
	mbev_status = MB_SUCCESS;
	mbev_error = MB_ERROR_NO_ERROR;
}

void doRegrid() {
	destroyViews();

	/* loop over all files to be count loaded files */
	int loadcount = 0;
	for (int ifile = 0; ifile < mbev_num_files; ifile++) {
		struct mbev_file_struct *file = &(mbev_files[ifile]);
		if (file->load_status)
			loadcount++;
	}

	/* put up dialog on grid parameters */
	if (mbev_status == MB_SUCCESS && loadcount > 0)
		doGridParameters();
	else
		QApplication::beep();

	/* reset the gui */
	doUpdateGui();

	/* reset status */
	mbev_status = MB_SUCCESS;
	mbev_error = MB_ERROR_NO_ERROR;
}

void doDeleteSelected() {
	/* get positions of selected list items */
	const std::vector<int> position_list = selectedPositions();

	/* delete the selected files */
	for (int i = (int)position_list.size() - 1; i >= 0; i--)
		mbeditviz_delete_file(position_list[i]);

	/* reset the gui */
	doUpdateGui();
}

void doQuit() {
	hideToolDialogs();
	msgOn("Shutting down...");

	/* destroy any mbview window */
	if (mbev_grid.status == MBEV_GRID_VIEWED) {
		/* destroy any mb3dsoundings window */
		mb3dsdgEnd();
		mbeditviz_mb3dsoundings_dismiss();
		mapClose();
		mbev_grid.status = MBEV_GRID_NOTVIEWED;
	}

	/* destroy the grid */
	if (mbev_grid.status != MBEV_GRID_NONE)
		mbeditviz_destroy_grid();

	/* loop over all files to be sure all files are unloaded */
	for (int ifile = 0; ifile < mbev_num_files; ifile++) {
		struct mbev_file_struct *file = &(mbev_files[ifile]);
		if (file->load_status) {
			// Unload file, asserting unlock
			mbeditviz_unload_file(ifile, true);
		}
	}

	/* reset the gui */
	doUpdateGui();
	doMessageOff();
}

// A datalist (format -1), read HERE, line by line, with MB-System's datalist rules: `path format
// [weight]` per line; '#' comments and '$' directives skipped; an "R:" (raw) / "P:" (processed) status
// prefix dropped; a path not absolute is relative to the datalist's own folder; a nested datalist
// (format -1) followed. Each swath file goes to the engine exactly as a single file does
// (mbeditviz_import_file). Read here because the MBIO library's own datalist reader can hand back no
// entry at all (seen with the 5.8 build), and a datalist is a legitimate input: it must open.
int importDatalist(const QString &path, int depth) {
	QFile f(path);
	if (depth > 16 || !f.open(QIODevice::ReadOnly | QIODevice::Text))
		return 0;
	const QString dir = QFileInfo(path).absolutePath();
	int n = 0;
	while (!f.atEnd()) {
		QString line = QString::fromUtf8(f.readLine()).trimmed();
		if (line.isEmpty() || line.startsWith('#') || line.startsWith('$'))
			continue;
		// "R:" / "P:" status prefix -- never a drive letter, whose colon is followed by a path separator
		if (line.size() > 2 && (line[0] == 'R' || line[0] == 'P') && line[1] == ':' && line[2] != '/' && line[2] != '\\')
			line = line.mid(2);
		const QStringList tok = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
		if (tok.isEmpty())
			continue;
		QString file = QDir::fromNativeSeparators(tok[0]);
		const bool absolute = file.startsWith('/') || (file.size() > 1 && file[1] == ':');
		if (!absolute)
			file = dir + '/' + file;
		int format = tok.size() > 1 ? tok[1].toInt() : 0;
		QByteArray fb = file.toUtf8();
		if (format == 0)
			mbeditviz_get_format(fb.data(), &format);
		if (format == -1)
			n += importDatalist(file, depth + 1);
		else if (format > 0 && QFileInfo::exists(file) && mbeditviz_import_file(fb.data(), format) == MB_SUCCESS)
			n++;
	}
	return n;
}

// do_mbeditviz_opendata
int doOpenData(const QByteArray &input_file, int format) {
	msgOn("Reading data list...");
	QByteArray f = input_file;
	if (format == -1) {
		mbev_status = importDatalist(QString::fromUtf8(input_file), 0) > 0 ? MB_SUCCESS : MB_FAILURE;
		doUpdateGui();
	}
	else
		mbeditviz_open_data(f.data(), format);
	doMessageOff();
	return (mbev_status);
}

// ---- the selections (do_mbeditviz_pickregion/_pickarea/_picknav_notify) ---------------------------
void openSoundingEditor() {
	MbEditViz *m = g_mv;
	Mb3dsdgNotify n;
	n.dismiss = &mbeditviz_mb3dsoundings_dismiss;
	n.edit = &mbeditviz_mb3dsoundings_edit;
	n.info = &mbeditviz_mb3dsoundings_info;
	n.bias = &mbeditviz_mb3dsoundings_bias;
	n.biasapply = &mbeditviz_mb3dsoundings_biasapply;
	n.flagsparsevoxels = &mbeditviz_mb3dsoundings_flagsparsevoxels;
	n.colorsoundings = &mbeditviz_mb3dsoundings_colorsoundings;
	n.optimizebiasvalues = &mbeditviz_mb3dsoundings_optimizebiasvalues;
	const bool ok = mb3dsdgOpen(m->win, m->host.base, &mbev_selected, n);
	if (ok)
		mbev_selected.displayed = true;
}

// mbview's region pick: the bounding box of the shape
void setRegion(double xmin, double xmax, double ymin, double ymax) {
	MbEditViz *m = g_mv;
	m->view.region_type = MBV_REGION_QUAD;
	const double cx[4] = {xmin, xmax, xmax, xmin};
	const double cy[4] = {ymin, ymin, ymax, ymax};
	for (int i = 0; i < 4; i++) {
		m->view.region.cornerpoints[i].xgrid = cx[i];
		m->view.region.cornerpoints[i].ygrid = cy[i];
		m->view.region.cornerpoints[i].zdata = mapZAt(cx[i], cy[i]);
	}
}

// mbview's area pick: endpoints, width = areaaspect x length, bearing from the north
void setArea(double x0, double y0, double x1, double y1, double aspect) {
	MbEditViz *m = g_mv;
	m->view.area_type = MBV_AREA_QUAD;
	mbview_area_struct &a = m->view.area;
	a.endpoints[0].xgrid = x0;
	a.endpoints[0].ygrid = y0;
	a.endpoints[0].zdata = mapZAt(x0, y0);
	a.endpoints[1].xgrid = x1;
	a.endpoints[1].ygrid = y1;
	a.endpoints[1].zdata = mapZAt(x1, y1);
	const double dx = x1 - x0;
	const double dy = y1 - y0;
	const double dxuse = 0.5 * aspect * dy;
	const double dyuse = 0.5 * aspect * dx;
	a.cornerpoints[0].xgrid = x0 - dxuse;
	a.cornerpoints[0].ygrid = y0 + dyuse;
	a.cornerpoints[1].xgrid = x0 + dxuse;
	a.cornerpoints[1].ygrid = y0 - dyuse;
	a.cornerpoints[2].xgrid = x1 + dxuse;
	a.cornerpoints[2].ygrid = y1 - dyuse;
	a.cornerpoints[3].xgrid = x1 - dxuse;
	a.cornerpoints[3].ygrid = y1 + dyuse;
	for (int i = 0; i < 4; i++)
		a.cornerpoints[i].zdata = mapZAt(a.cornerpoints[i].xgrid, a.cornerpoints[i].ygrid);
	a.length = sqrt(dx * dx + dy * dy);
	a.width = aspect * a.length;
	a.bearing = RTD * atan2(dx, dy);
	if (a.bearing < 0.0)
		a.bearing += 360.0;
	if (a.bearing > 360.0)
		a.bearing -= 360.0;
}

bool pointInPolygon(double x, double y, const std::vector<std::array<double, 2>> &v) {
	bool in = false;
	const size_t n = v.size();
	for (size_t i = 0, j = n - 1; i < n; j = i++) {
		if (((v[i][1] > y) != (v[j][1] > y)) &&
		    (x < (v[j][0] - v[i][0]) * (y - v[i][1]) / (v[j][1] - v[i][1]) + v[i][0]))
			in = !in;
	}
	return in;
}

// mbview's nav pick: every ping of every loaded file whose fix is inside the shape
int setNav(const std::vector<std::array<double, 2>> &poly) {
	MbEditViz *m = g_mv;
	m->navs.clear();
	m->navpts.clear();
	int nselected = 0;
	for (int ifile = 0; ifile < mbev_num_files; ifile++) {
		struct mbev_file_struct *file = &mbev_files[ifile];
		if (!file->load_status)
			continue;
		std::vector<mbview_navpointw_struct> pts((size_t)MAX(1, file->num_pings));
		for (int iping = 0; iping < file->num_pings; iping++) {
			double x, y;
			mb_proj_forward(mbev_verbose, mbev_grid.pjptr, file->pings[iping].navlon, file->pings[iping].navlat, &x, &y,
			                &mbev_error);
			pts[iping].selected = pointInPolygon(x, y, poly);
			if (pts[iping].selected)
				nselected++;
		}
		m->navpts.push_back(std::move(pts));
	}
	for (auto &p : m->navpts) {
		mbview_nav_struct n;
		n.navpts = p.data();
		m->navs.push_back(n);
	}
	m->shared.navs = m->navs.data();
	return nselected;
}

// mbview's nav pick on a navigation TRACK selected on the map: every ping of the loaded file that track
// is the navigation of. A track is known by its file's name (mbeditviz's own tracks carry it verbatim,
// mbgrdviz's may carry a path, or " (swath bounds)" on the bounds line). 0 = no loaded file is that track.
int setNavTrack(const std::string &nav) {
	MbEditViz *m = g_mv;
	QString want = QString::fromStdString(nav);
	if (want.endsWith(" (swath bounds)"))
		want.chop(int(strlen(" (swath bounds)")));
	const QString wantBase = QFileInfo(QDir::fromNativeSeparators(want)).fileName();
	m->navs.clear();
	m->navpts.clear();
	int nselected = 0;
	for (int ifile = 0; ifile < mbev_num_files; ifile++) {
		struct mbev_file_struct *file = &mbev_files[ifile];
		if (!file->load_status)
			continue;
		const QString name = QString::fromUtf8(file->name);
		const bool hit = name == want || QFileInfo(QDir::fromNativeSeparators(name)).fileName() == wantBase;
		std::vector<mbview_navpointw_struct> pts((size_t)MAX(1, file->num_pings));
		for (int iping = 0; iping < file->num_pings; iping++) {
			pts[iping].selected = hit;
			if (hit)
				nselected++;
		}
		m->navpts.push_back(std::move(pts));
	}
	for (auto &p : m->navpts) {
		mbview_nav_struct n;
		n.navpts = p.data();
		m->navs.push_back(n);
	}
	m->shared.navs = m->navs.data();
	return nselected;
}

// the shapes drawn on the map, in the ENGINE's coordinates (UTM metres): on a geographic window they
// were drawn in lon/lat and are carried over here, the one place the selections read them
std::vector<MbEditVizShape> mapShapes() {
	if (!mapAlive() || !g_mv->host.mapShapes)
		return {};
	std::vector<MbEditVizShape> shapes = g_mv->host.mapShapes(g_mv->map);
	if (g_mv->mapGeo)
		for (MbEditVizShape &s : shapes)
			for (auto &p : s.v) {
				double x, y;
				int err = MB_ERROR_NO_ERROR;
				mb_proj_forward(mbev_verbose, mbev_grid.pjptr, p[0], p[1], &x, &y, &err);
				p = {x, y};
			}
	return shapes;
}

void refillShapes() {
	MbEditViz *m = g_mv;
	QString keep = m->shapeCombo->currentText();
	QSignalBlocker b(m->shapeCombo);
	m->shapeCombo->clear();
	QString sel;
	for (const MbEditVizShape &s : mapShapes()) {
		const QString nm = QString::fromStdString(s.name);
		m->shapeCombo->addItem(nm);
		if (s.selected)
			sel = nm;
	}
	if (!sel.isEmpty() && sel != m->lastSelectedLine)
		keep = sel;                              // a line just selected on the map is the one meant
	m->lastSelectedLine = sel;
	const int i = m->shapeCombo->findText(keep);
	if (i >= 0)
		m->shapeCombo->setCurrentIndex(i);
}

// the shape names as the map has them now (the timer re-fills the list when they change)
QStringList shapeNames(const std::vector<MbEditVizShape> &shapes) {
	QStringList out;
	for (const MbEditVizShape &s : shapes)
		out << QString::fromStdString(s.name) + (s.selected ? "\n*" : "");
	return out;
}

bool selectWith(int what, const QString &name) {
	MbEditViz *m = g_mv;
	if (mbev_grid.status != MBEV_GRID_VIEWED)
		return false;
	const MbEditVizShape *shape = nullptr;
	const std::vector<MbEditVizShape> shapes = mapShapes();
	for (const MbEditVizShape &s : shapes)
		if (QString::fromStdString(s.name) == name)
			shape = &s;
	if (!shape || shape->v.empty()) {
		QMessageBox::information(m->win, "MBeditviz",
		                         "Draw a shape on the survey map first (Draw tools: Rectangle, Line, Polygon), or "
		                         "double-click a navigation track on it, then choose it here.");
		return false;
	}
	// Nav on a navigation TRACK selected on the map: the pings of that track (mbview's nav pick on a
	// nav line), whatever its shape
	if (what == 2 && !shape->nav.empty() && !shape->closed) {
		if (setNavTrack(shape->nav) == 0) {
			QMessageBox::information(m->win, "MBeditviz",
			                         QString("No file loaded in MBeditviz is the track \"%1\".")
			                             .arg(QString::fromStdString(shape->nav)));
			return false;
		}
		mbeditviz_selectnav(0);
		openSoundingEditor();
		return true;
	}
	std::vector<std::array<double, 2>> v = shape->v;
	if (v.size() > 1 && v.front() == v.back())
		v.pop_back();
	if (what == 0) {
		double xmin = v[0][0], xmax = v[0][0], ymin = v[0][1], ymax = v[0][1];
		for (const auto &p : v) {
			xmin = MIN(xmin, p[0]);
			xmax = MAX(xmax, p[0]);
			ymin = MIN(ymin, p[1]);
			ymax = MAX(ymax, p[1]);
		}
		if (!(xmax > xmin && ymax > ymin)) {
			QApplication::beep();
			return false;
		}
		setRegion(xmin, xmax, ymin, ymax);
		mbeditviz_selectregion(0);
	}
	else if (what == 1) {
		if (v.size() < 2) {
			QApplication::beep();
			return false;
		}
		setArea(v.front()[0], v.front()[1], v.back()[0], v.back()[1], m->areaAspect->value());
		mbeditviz_selectarea(0);
	}
	else {
		if (v.size() < 3) {
			QMessageBox::information(m->win, "MBeditviz", "Selecting navigation needs a closed shape (a rectangle or a polygon) or a navigation track.");
			return false;
		}
		if (setNav(v) == 0) {
			QApplication::beep();
			return false;
		}
		mbeditviz_selectnav(0);
	}
	openSoundingEditor();
	return true;
}

// ---- the window ----------------------------------------------------------------------------
class MvCloseFilter : public QObject {
public:
	explicit MvCloseFilter(QObject *parent) : QObject(parent) {}

protected:
	// do_mbeditviz_quit
	bool eventFilter(QObject *o, QEvent *e) override {
		// the window going off screen by any route (park, minimise, close) takes its dialogs along
		if (g_mv && o == g_mv->win &&
		    (e->type() == QEvent::Hide ||
		     (e->type() == QEvent::WindowStateChange && g_mv->win->isMinimized())))
			hideToolDialogs(/*paintNow=*/false);   // inside an event: no nested event loop
		if (e->type() == QEvent::Close && g_mv && o == g_mv->win) {
			if (g_mv->timer)
				g_mv->timer->stop();
			doQuit();
		}
		return QObject::eventFilter(o, e);
	}
};

void openDialog() {
	MbEditViz *m = g_mv;
	if (!m->openDlg) {
		QWidget *w = loadUi("mbeditviz_open.ui", m->win);
		auto *d = qobject_cast<QDialog *>(w);
		if (!d) {
			delete w;
			return;
		}
		QStringList missing;
		m->openFile = child<QLineEdit>(d, "fileEdit", missing);
		m->openFormat = child<QSpinBox>(d, "formatBox", missing);
		m->openEdit = child<QRadioButton>(d, "outputEdit", missing);
		m->openBrowse = child<QRadioButton>(d, "outputBrowse", missing);
		auto *browse = child<QPushButton>(d, "browseButton", missing);
		auto *ok = child<QPushButton>(d, "okButton", missing);
		auto *cancel = child<QPushButton>(d, "cancelButton", missing);
		if (!missing.isEmpty()) {
			QMessageBox::warning(m->win, "MBeditviz", "mbeditviz_open.ui lacks: " + missing.join(", "));
			delete d;
			m->openFile = nullptr;
			return;
		}
		m->openDlg = d;
		d->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
		// do_mbeditviz_fileselection_list: a chosen file gets its format guessed from its name
		auto guess = [m]() {
			QByteArray path = QDir::fromNativeSeparators(m->openFile->text().trimmed()).toUtf8();
			if (path.isEmpty())
				return;
			int form = m->mformat;
			if (mbeditviz_get_format(path.data(), &form) == MB_SUCCESS) {
				m->mformat = form;
				m->openFormat->setValue(m->mformat);
			}
		};
		QObject::connect(browse, &QPushButton::clicked, d, [m, d, guess]() {
			const QString fn = QFileDialog::getOpenFileName(d, "Open Swath Data", m->host.base.startDir(),
			                                                "Swath data (*.mb*);;All Files (*)");
			if (fn.isEmpty())
				return;
			m->host.base.rememberDir(fn);
			m->openFile->setText(QDir::toNativeSeparators(fn));
			guess();
		});
		QObject::connect(m->openFile, &QLineEdit::editingFinished, d, guess);
		QObject::connect(cancel, &QPushButton::clicked, d, &QDialog::hide);
		// do_mbeditviz_openfile
		QObject::connect(ok, &QPushButton::clicked, d, [m, d]() {
			const QByteArray file = QDir::fromNativeSeparators(m->openFile->text().trimmed()).toUtf8();
			if (file.isEmpty()) {
				QApplication::beep();
				return;
			}
			d->hide();
			mbev_mode_output = m->openBrowse->isChecked() ? MBEV_OUTPUT_MODE_BROWSE : MBEV_OUTPUT_MODE_EDIT;
			{
				QSignalBlocker b1(m->modeEdit), b2(m->modeBrowse);
				m->modeEdit->setChecked(mbev_mode_output == MBEV_OUTPUT_MODE_EDIT);
				m->modeBrowse->setChecked(mbev_mode_output == MBEV_OUTPUT_MODE_BROWSE);
			}
			mbev_status = doOpenData(file, m->openFormat->value());
		});
	}
	m->openFormat->setValue(m->mformat);
	{
		QSignalBlocker b1(m->openEdit), b2(m->openBrowse);
		m->openEdit->setChecked(mbev_mode_output == MBEV_OUTPUT_MODE_EDIT);
		m->openBrowse->setChecked(mbev_mode_output == MBEV_OUTPUT_MODE_BROWSE);
	}
	m->openDlg->show();
	m->openDlg->raise();
	m->openDlg->activateWindow();
}

void about() {
	QMessageBox::about(g_mv->win, "About MBeditviz",
		QString("<b>MBeditviz</b><br>Bathymetry Editor and Patch Test Tool<br><br>"
		        "One Component of the <b>MB-System</b> Open Source Software Package<br>"
		        "for Processing and Display of Swath Sonar Data<br><br>"
		        "Created by: David W. Caress and Dale N. Chayes<br>"
		        "Monterey Bay Aquarium Research Institute &nbsp; / &nbsp; Lamont-Doherty Earth Observatory<br><br>"
		        "MB-System library in use: %1<br><br>"
		        "Ported to InteractiveGMT (Qt + VTK) from MB-System's mbeditviz; the survey map is an "
		        "InteractiveGMT window.")
		    .arg(QString::fromLatin1(mbedit_mbio_version())));
}

MbEditViz *build(QWidget *parent, const MbEditVizHost &host) {
	auto *m = new MbEditViz;
	m->host = host;
	g_mv = m;                                // loadUi reads the host's ui folder through it
	QWidget *root = loadUi("mbeditviz.ui", nullptr);
	auto *win = qobject_cast<QMainWindow *>(root);
	if (!win) {
		delete root;
		g_mv = nullptr;
		delete m;
		return nullptr;
	}
	QStringList missing;
	m->fileList = child<QListWidget>(win, "fileList", missing);
	m->labelStatus = child<QLabel>(win, "labelStatus", missing);
	m->viewAllButton = child<QPushButton>(win, "viewAllButton", missing);
	m->viewSelectedButton = child<QPushButton>(win, "viewSelectedButton", missing);
	m->removeButton = child<QPushButton>(win, "removeButton", missing);
	m->modeEdit = child<QRadioButton>(win, "modeEdit", missing);
	m->modeBrowse = child<QRadioButton>(win, "modeBrowse", missing);
	m->actOpen = child<QAction>(win, "actionOpen", missing);
	m->actNewGrid = child<QAction>(win, "actionNewGrid", missing);
	auto *actQuit = child<QAction>(win, "actionQuit", missing);
	auto *actAbout = child<QAction>(win, "actionAbout", missing);
	m->mapGroup = child<QGroupBox>(win, "mapGroup", missing);
	m->shapeCombo = child<QComboBox>(win, "shapeCombo", missing);
	m->areaAspect = child<QDoubleSpinBox>(win, "areaAspectBox", missing);
	m->secondaryCheck = child<QCheckBox>(win, "secondaryCheck", missing);
	auto *refreshButton = child<QPushButton>(win, "refreshShapesButton", missing);
	auto *regionButton = child<QPushButton>(win, "regionButton", missing);
	auto *areaButton = child<QPushButton>(win, "areaButton", missing);
	auto *navButton = child<QPushButton>(win, "navButton", missing);
	auto *updateGridButton = child<QPushButton>(win, "updateGridButton", missing);
	if (!missing.isEmpty()) {
		QMessageBox::warning(parent, "MBeditviz", "mbeditviz.ui lacks: " + missing.join(", "));
		delete win;
		g_mv = nullptr;
		delete m;
		return nullptr;
	}
	m->win = win;
	win->setAttribute(Qt::WA_DeleteOnClose);
	if (!host.base.icon.isNull())
		win->setWindowIcon(host.base.icon);

	// do_mbeditviz_mode_change / _changeoutputmode
	auto *gMode = new QButtonGroup(win);
	gMode->addButton(m->modeEdit);
	gMode->addButton(m->modeBrowse);
	QObject::connect(m->modeEdit, &QRadioButton::toggled, win, [](bool on) {
		mbev_mode_output = on ? MBEV_OUTPUT_MODE_EDIT : MBEV_OUTPUT_MODE_BROWSE;
	});

	QObject::connect(m->actOpen, &QAction::triggered, win, []() { openDialog(); });
	QObject::connect(actQuit, &QAction::triggered, win, [m]() { mbParkQuit(m->parking); });
	QObject::connect(m->actNewGrid, &QAction::triggered, win, []() { doRegrid(); });
	QObject::connect(actAbout, &QAction::triggered, win, []() { about(); });
	QObject::connect(m->viewAllButton, &QPushButton::clicked, win, []() { doViewAll(true); });
	QObject::connect(m->viewSelectedButton, &QPushButton::clicked, win, []() { doViewSelected(); });
	QObject::connect(m->removeButton, &QPushButton::clicked, win, []() { doDeleteSelected(); });

	// the survey map box
	QObject::connect(refreshButton, &QPushButton::clicked, win, []() { refillShapes(); });
	QObject::connect(regionButton, &QPushButton::clicked, win, []() { selectWith(0, g_mv->shapeCombo->currentText()); });
	QObject::connect(areaButton, &QPushButton::clicked, win, []() { selectWith(1, g_mv->shapeCombo->currentText()); });
	QObject::connect(navButton, &QPushButton::clicked, win, []() { selectWith(2, g_mv->shapeCombo->currentText()); });
	// do_mbeditviz_regrid_notify: "Update Bathymetry Grid" with the editor's current bias values
	QObject::connect(updateGridButton, &QPushButton::clicked, win, []() {
		double rollbias, pitchbias, headingbias, timelag, snell;
		mb3dsdgGetBiasValues(&rollbias, &pitchbias, &headingbias, &timelag, &snell);
		mbeditviz_mb3dsoundings_biasapply(rollbias, pitchbias, headingbias, timelag, snell);
		doUpdateGui();
	});
	// do_mbeditviz_enable/disablesecondarypicks_notify (MBV_STATEMASK_20 / _21)
	QObject::connect(m->secondaryCheck, &QCheckBox::toggled, win, [](bool on) { g_mv->view.state21 = on ? 1 : 0; });

	// do_mbeditviz_settimer / _workfunction: keep the file list current, and see the map go
	m->timer = new QTimer(win);
	m->timer->setInterval(1000);
	QObject::connect(m->timer, &QTimer::timeout, win, []() {
		MbEditViz *mm = g_mv;
		if (!mm)
			return;
		if (mm->map && !mapAlive()) {           // its grid was removed from the window: do_mbeditviz_mbview_dismiss_notify
			hideToolDialogs();
			mapClose();
			doMbviewDismissNotify(0);
		}
		if (mbev_num_files > 0 && !mbev_message_on)
			doUpdateFilelist();
		if (mapAlive() && mm->mapGroup->isEnabled()) {
			const QStringList now = shapeNames(mapShapes());   // a line selected / let go of counts too
			if (now != mm->lastShapes) {
				mm->lastShapes = now;
				refillShapes();
			}
		}
	});
	m->timer->start();

	win->installEventFilter(new MvCloseFilter(win));
	m->parking = mbParkable(win, host.base, "mbeditviz");   // after the close filter: a park comes first
	return m;
}

} // namespace

// ---- the engine's mbview hooks (declared in mbeditviz.h) ------------------------------------
extern "C" {

int mbview_getdataptr(int verbose, size_t instance, struct mbview_struct **datahandle, int *error) {
	(void)verbose;
	(void)instance;
	if (!g_mv) {
		*error = MB_ERROR_BAD_PARAMETER;
		return MB_FAILURE;
	}
	*datahandle = &g_mv->view;
	*error = MB_ERROR_NO_ERROR;
	return MB_SUCCESS;
}

int mbview_getsharedptr(int verbose, struct mbview_shareddata_struct **sharedhandle, int *error) {
	(void)verbose;
	if (!g_mv) {
		*error = MB_ERROR_BAD_PARAMETER;
		return MB_FAILURE;
	}
	*sharedhandle = &g_mv->shared;
	*error = MB_ERROR_NO_ERROR;
	return MB_SUCCESS;
}

int mbview_colorvalue_instance(size_t instance, double value, float *r, float *g, float *b) {
	(void)instance;
	double rgb[3] = {0.0, 0.0, 0.0};
	if (mapAlive() && g_mv->host.mapColor && g_mv->host.mapColor(g_mv->map, value, rgb)) {
		*r = (float)rgb[0];
		*g = (float)rgb[1];
		*b = (float)rgb[2];
		return MB_SUCCESS;
	}
	*r = *g = *b = 0.0f;
	return MB_SUCCESS;
}

int mbview_updateprimarygridcell(int verbose, size_t instance, int primary_ix, int primary_jy, float value, int *error) {
	(void)verbose;
	(void)instance;
	(void)primary_ix;
	(void)primary_jy;
	(void)value;
	if (g_mv)
		g_mv->mapDirty = true;               // the engine wrote mbev_grid.val itself; shown at plothigh
	*error = MB_ERROR_NO_ERROR;
	return MB_SUCCESS;
}

int mbview_updateprimarygrid(int verbose, size_t instance, int primary_n_columns, int primary_n_rows, float *primary_data,
                             int *error) {
	(void)verbose;
	(void)instance;
	(void)primary_n_columns;
	(void)primary_n_rows;
	(void)primary_data;
	if (g_mv)
		g_mv->mapDirty = true;
	*error = MB_ERROR_NO_ERROR;
	return MB_SUCCESS;
}

int mbview_updatesecondarygrid(int verbose, size_t instance, int secondary_n_columns, int secondary_n_rows,
                               float *secondary_data, int *error) {
	(void)verbose;
	(void)instance;
	(void)secondary_n_columns;
	(void)secondary_n_rows;
	(void)secondary_data;
	*error = MB_ERROR_NO_ERROR;
	return MB_SUCCESS;
}

int mbview_plothigh(size_t instance) {
	(void)instance;
	if (g_mv && g_mv->mapDirty)
		mapRefresh();
	return MB_SUCCESS;
}

} // extern "C"

// ---- entry points ----------------------------------------------------------------------------
bool mbeditvizOpenWindow(QWidget *parent, const MbEditVizHost &host, const QString &file, int format, int outputMode,
                         bool replace) {
	if (g_mv && replace) {
		doQuit();                                        // the map, the grid, every loaded file: let go
		for (int i = mbev_num_files - 1; i >= 0; i--)    // and the list itself: only what is opened now
			mbeditviz_delete_file(i);
		doUpdateGui();
	}
	if (!g_mv) {
		if (!mbeditLoadMbio(parent, host.base))
			return false;
		char msg[2048] = "";
		if (!mbeditviz_mbio_open(msg, int(sizeof(msg)))) {
			QMessageBox::warning(parent, "MBeditviz", QString::fromUtf8(msg));
			return false;
		}
		MbEditViz *m = build(parent, host);
		if (!m)
			return false;

		// do_mbeditviz_init
		static char program[] = "MBeditviz";
		static char help[] = "MBeditviz is a bathymetry editor and patch test tool.";
		static char usage[] = "mbeditviz [-I file -F format -G -H -V]";
		mbeditviz_init(program, help, usage, &doMessageOn, &doMessageOff, &doUpdateGui, &doErrorDialog);
		m->mformat = -1;
		{
			QSignalBlocker b1(m->modeEdit), b2(m->modeBrowse);
			m->modeEdit->setChecked(mbev_mode_output == MBEV_OUTPUT_MODE_EDIT);
			m->modeBrowse->setChecked(mbev_mode_output == MBEV_OUTPUT_MODE_BROWSE);
		}
		doUpdateGui();

		if (host.base.windowOpened)
			host.base.windowOpened();
		QObject::connect(m->win, &QObject::destroyed, [m]() {
			if (m->host.base.windowClosed)
				m->host.base.windowClosed();
			if (g_mv == m)
				g_mv = nullptr;
			delete m;
		});
		m->win->show();
		mbPlaceRight(m->win, parent);
		QApplication::processEvents();
	}
	MbEditViz *m = g_mv;
	if (host.base.parkScene)
		m->host.base.parkScene = host.base.parkScene;    // its 3-D sounding editor parks there too
	mbParkRebind(m->parking, host.base.parkScene);
	mbParkShow(m->parking);                              // a parked one comes back off its handle
	if (!file.isEmpty()) {
		mbev_mode_output = (outputMode == 1) ? MBEV_OUTPUT_MODE_BROWSE : MBEV_OUTPUT_MODE_EDIT;
		{
			QSignalBlocker b1(m->modeEdit), b2(m->modeBrowse);
			m->modeEdit->setChecked(mbev_mode_output == MBEV_OUTPUT_MODE_EDIT);
			m->modeBrowse->setChecked(mbev_mode_output == MBEV_OUTPUT_MODE_BROWSE);
		}
		QByteArray f = QDir::fromNativeSeparators(file).toUtf8();
		int form = format;
		if (form == 0)
			mbeditviz_get_format(f.data(), &form);
		doOpenData(f, form);
		return mbev_num_files > 0;
	}
	return true;
}

int mbeditvizState(int *out, int n) {
	const bool open = (g_mv != nullptr);
	const int v[9] = {open ? 1 : 0,
	                  open ? mbev_num_files : 0,
	                  open ? mbev_num_files_loaded : 0,
	                  open ? mbev_grid.status : 0,
	                  open ? mbev_grid.n_columns : 0,
	                  open ? mbev_grid.n_rows : 0,
	                  open ? mbev_selected.num_soundings : 0,
	                  open ? mbev_selected.num_soundings_flagged : 0,
	                  mb3dsdgIsOpen() ? 1 : 0};
	const int k = n < 9 ? n : 9;
	for (int i = 0; i < k; i++)
		out[i] = v[i];
	return k;
}

bool mbeditvizViewAll(double cellsize) {
	if (!g_mv || !doViewAll(false))
		return false;
	if (cellsize > 0.0) {
		mbev_grid_cellsize = cellsize;
		mbev_grid_n_columns = (mbev_grid_boundsutm[1] - mbev_grid_boundsutm[0]) / mbev_grid_cellsize + 1;
		mbev_grid_n_rows = (mbev_grid_boundsutm[3] - mbev_grid_boundsutm[2]) / mbev_grid_cellsize + 1;
	}
	doUpdateGrid();
	return mbev_grid.status == MBEV_GRID_VIEWED;
}

bool mbeditvizSelect(int what, const QString &shape) {
	return g_mv && selectWith(what, shape);
}

bool mbeditvizSelectBox(double x0, double x1, double y0, double y1) {
	if (!g_mv || mbev_grid.status != MBEV_GRID_VIEWED)
		return false;
	setRegion(MIN(x0, x1), MAX(x0, x1), MIN(y0, y1), MAX(y0, y1));
	mbeditviz_selectregion(0);
	openSoundingEditor();
	return mb3dsdgIsOpen();
}

bool mbeditvizEditorKey(int ch) {
	return mb3dsdgKey(ch);
}

bool mbeditvizEditorMode(int mode) {
	return mb3dsdgSetEditMode(mode);
}

bool mbeditvizEditorClickSounding(int i) {
	int x = 0, y = 0;
	if (!mb3dsdgSoundingPixel(i, &x, &y))
		return false;
	return mb3dsdgMouseEdit(x, y, x, y);
}

bool mbeditvizEditorSavePng(const QString &path) {
	return mb3dsdgSavePng(path);
}

bool mbeditvizCloseEditor() {
	if (!mb3dsdgIsOpen())
		return false;
	mb3dsdgEnd();
	mbeditviz_mb3dsoundings_dismiss();
	return true;
}

bool mbeditvizCloseMap() {
	if (!g_mv || !g_mv->map)
		return false;
	mapClose();
	doMbviewDismissNotify(0);
	return true;
}

bool mbeditvizClose() {
	if (!g_mv)
		return false;
	mbParkQuit(g_mv->parking);
	return true;
}
