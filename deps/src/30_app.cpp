// (Julia) pumps the loop via gmtvtk_process_events so the REPL stays interactive.
#include "_app_icon.h"               // embedded iGMT icon bytes (kAppIconPng / kAppIconPngLen)
static QApplication *g_app = nullptr;
static int           g_openWindows = 0;
static vtkRenderWindow *g_lastRW = nullptr;   // most-recent window, for gmtvtk_save_png
static Scene *g_lastScene = nullptr;   // most-recent scene, for gmtvtk_add_overlay
static QProgressDialog *g_progress = nullptr;  // progress dialog for long operations (Okada multi-patch)

// The ONE place that constructs this app's persistent settings store (prefs/dirMRU, Recent Files):
// ~/.gmt/iGMT.ini — the SAME ~/.gmt directory GMT.jl already reads/writes. NEVER the Windows registry
// (QSettings(organization, application), the 2-arg convenience constructor, is hardcoded to
// QSettings::NativeFormat — registry on Windows — so it must never be used anywhere in this codebase;
// always go through this explicit-fileName constructor instead).
static QSettings igmtSettings() {
	const QString dir = QDir::homePath() + "/.gmt";
	QDir().mkpath(dir);
	return QSettings(dir + "/iGMT.ini", QSettings::IniFormat);
}

// ============================================================================================
// Default directory (Preferences > Default directory). Every file-open / file-save dialog starts
// here, so a session keeps working out of the user's chosen folder. The value is the head of an
// MRU list (prefs/dirMRU in QSettings) so recently used directories persist and are offered in the
// Preferences combo. prefStartDir() seeds a dialog's start path; rememberStartDir() pushes the
// directory of a just-chosen file to the front of the MRU after a successful pick.
// ============================================================================================
static QStringList prefDirMRU() {
	QSettings st = igmtSettings();
	QStringList l = st.value("prefs/dirMRU").toStringList();
	if (l.isEmpty()) {                                   // migrate the single-value default dir
		QString d = st.value("prefs/defaultDir").toString().trimmed();
		if (!d.isEmpty()) l << d;
	}
	return l;
}
// Push `dir` to the front of the directory MRU (dedup, capped). Keep prefs/defaultDir in sync with
// the head so the two views of "the default directory" never diverge.
static void prefPushDir(const QString &dir) {
	if (dir.isEmpty()) return;
	QSettings st = igmtSettings();
	QStringList l = st.value("prefs/dirMRU").toStringList();
	l.removeAll(dir);
	l.prepend(dir);
	while (l.size() > 12) l.removeLast();
	st.setValue("prefs/dirMRU", l);
	st.setValue("prefs/defaultDir", dir);
}
// Start path for a file dialog. Prefer an explicit per-call seed (e.g. a default file name); when
// that is empty, fall back to the saved default directory. `seedName`, if given, is appended so a
// Save dialog opens with a suggested file name inside the default directory.
static QString prefStartDir(const QString &seedName = QString()) {
	QStringList l = prefDirMRU();
	QString dir = l.isEmpty() ? QString() : l.first();
	if (dir.isEmpty()) return seedName;            // nothing saved -> let Qt pick (cwd)
	if (seedName.isEmpty()) return dir;
	return QDir(dir).filePath(seedName);           // dir + "/" + suggested name
}
// After a dialog returns `path`, remember its directory as the new default (front of the MRU).
static void rememberStartDir(const QString &path) {
	if (path.isEmpty()) return;
	prefPushDir(QFileInfo(path).absolutePath());
}

// STANDING RULE: a list of short values (degrees, sizes, counts) must show its rows TIGHT — one line
// of text and nothing more — the way Mirone's listboxes do. Qt's default row height is far taller
// than the text, and setSpacing(0) alone does NOT fix it: the padding lives in each item's own
// sizeHint, so it has to be overridden per item. A stylesheet is the WRONG tool here — applying one
// switches Qt's style engine to a path that pads rows MORE, not less (learned the hard way on the
// RTP dialog's lists), which is why this is done through sizeHint and the widget font only.
// Call it after populating (and after every repopulate).
static void tightenListRows(QListWidget *lw) {
	if (!lw) return;
	lw->setSpacing(0);
	lw->setIconSize(QSize(0, 0));                  // no reserved icon column
	lw->setUniformItemSizes(true);                 // lets the view trust one height for every row
	const int h = QFontMetrics(lw->font()).height();
	for (int i = 0; i < lw->count(); ++i)
		lw->item(i)->setSizeHint(QSize(0, h));     // 0 width = keep the view's own width
}

// STANDING RULE for every dialog in this app (existing and future): a QLineEdit that expects a FILE
// must also bring up the system file chooser on a DOUBLE-CLICK inside it, not only through its "..."
// button. Implemented once, here, and by CLICKING THAT BUTTON — the button's own handler already
// knows the box's caption, filter and rememberStartDir bookkeeping, so a box keeps exactly ONE
// file-picking code path (same operation, same function) and the two entry points can never drift.
// A disabled button (a grayed-out option) means the double-click does nothing, as it should.
static void fileBoxDoubleClick(QLineEdit *edit, QAbstractButton *browseBtn) {
	if (!edit || !browseBtn) return;
	struct DblClickOpensChooser : QObject {
		QAbstractButton *btn;
		DblClickOpensChooser(QObject *parent, QAbstractButton *b) : QObject(parent), btn(b) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::MouseButtonDblClick && btn && btn->isEnabled()) {
				btn->click();
				return true;
			}
			return QObject::eventFilter(o, e);
		}
	};
	edit->installEventFilter(new DblClickOpensChooser(edit, browseBtn));
}

// ---- Preferences scalar settings (File > Preferences). Defined here (early) so every fragment can
//      read them; the editor dialog lives in 70_window.cpp. Defaults match the combos' first item.
static QString prefMeasureUnits()  { return igmtSettings().value("prefs/measureUnits",  "meters").toString(); }
static QString prefDistAzimType()  { return igmtSettings().value("prefs/distAzimType",  "Ellipsoidal").toString(); }
static QString prefAzimDir()       { return igmtSettings().value("prefs/azimDir",       "Forward").toString(); }
static QString prefLineThickness() { return igmtSettings().value("prefs/lineThickness", "2 pt").toString(); }
static QString prefLineColor()     { return igmtSettings().value("prefs/lineColor",     "Orange").toString(); }
static QString prefCoastColor()    { return igmtSettings().value("prefs/coastColor",    "Black").toString(); }
// "NaN fill colour": the solid colour grid NaN cells are painted (default white). Stored as a
// #rrggbb hex string so any colour is selectable via QColorDialog in Preferences.
static QString prefNanColor()      { return igmtSettings().value("prefs/nanColor",      "#ffffff").toString(); }
static void prefNanColorRGB(double &r, double &g, double &b) {
	QColor c(prefNanColor().trimmed());
	if (!c.isValid()) c = QColor(Qt::white);
	r = c.redF(); g = c.greenF(); b = c.blueF();
}

// "Background color": solid override for the 3-D scene's render background. Empty string (default)
// keeps the program's built-in dark-slate/steel-blue gradient; any #rrggbb hex picked in
// Preferences replaces it with a SOLID colour. ONE function applies it — scene creation
// (buildAndShow, 70_window.cpp) and the Preferences OK handler both call this, never inline
// SetBackground/GradientBackgroundOn elsewhere (SACRED_LAW: one operation, one function).
static QString prefBackgroundColor() { return igmtSettings().value("prefs/backgroundColor", "").toString(); }
static void applyBackgroundPref(vtkRenderer *ren) {
	QColor c(prefBackgroundColor().trimmed());
	if (!c.isValid()) {
		ren->GradientBackgroundOn();
		ren->SetBackground(0.16, 0.18, 0.22);    // bottom (dark slate)
		ren->SetBackground2(0.36, 0.42, 0.52);   // top
	} else {
		ren->GradientBackgroundOff();
		ren->SetBackground(c.redF(), c.greenF(), c.blueF());
	}
}

// Map the "Default line color" name to RGB (0..1). "Orange" (1.0,0.55,0.0) is the program's original
// unnamed default line colour, kept FIRST in the combo so the familiar look stays the default. Any
// unknown name falls back to that same orange (never a surprise black). Other colours are still
// freely settable per-line via Line Properties — the combo only seeds NEW lines/polygons.
static void prefLineColorRGB(double &r, double &g, double &b) {
	const QString c = prefLineColor().trimmed().toLower();
	if      (c == "black")   { r = 0.0; g = 0.0; b = 0.0; }
	else if (c == "red")     { r = 1.0; g = 0.0; b = 0.0; }
	else if (c == "magenta") { r = 1.0; g = 0.0; b = 1.0; }
	else if (c == "cyan")    { r = 0.0; g = 1.0; b = 1.0; }
	else if (c == "white")   { r = 1.0; g = 1.0; b = 1.0; }
	else if (c == "green")   { r = 0.0; g = 1.0; b = 0.0; }
	else if (c == "blue")    { r = 0.0; g = 0.0; b = 1.0; }
	else if (c == "yellow")  { r = 1.0; g = 1.0; b = 0.0; }
	else                     { r = 1.0; g = 0.55; b = 0.0; }   // "Orange" / unknown -> the original default
}
// "Default line thickness" combo ("N pt") -> VTK line width in px, using the SAME real-DPI pt<->px
// conversion every other line-width consumer already uses (55_lineprops.cpp's Width(px)/Width(points)
// two-way sync; gmtvtk_add_overlay_ex_h/_ex2_h's linewidth-in-points conversion) -- NOT a second,
// independently-guessed factor (SACRED_LAW.md: one quantity, one function). A stale fixed 1.25 px/pt
// (a 90 dpi assumption) here made a "1 pt" default round-trip back as "0.56 pt" in the Line
// Properties dialog on a real ~161 dpi screen -- confirmed live 2026-07-24, fixed at this ONE source.
static double prefLineWidthPx(Scene *s) {
	double dpi = 72.0;
	if (s && s->widget && s->widget->renderWindow() && s->widget->renderWindow()->GetDPI() > 0)
		dpi = s->widget->renderWindow()->GetDPI();
	const double pxPerPt = dpi / 72.0;
	bool ok = false;
	const int pt = prefLineThickness().section(' ', 0, 0).toInt(&ok);
	return (ok && pt > 0) ? pt * pxPerPt : 2.0 * pxPerPt;
}

// Julia console callback. The viewer lives IN-PROCESS in the Julia session, so a console
// dock can hand a typed command straight back to Julia to eval in Main. `scene` is the
// window's own Scene* (so the callback can bind `fig` to it); `cmd` is the line typed;
// the result text is written into `outbuf` (capacity `outcap`); returns its length (or -1
// if no callback registered). Set from Julia via gmtvtk_set_julia_eval.
typedef int (*JuliaEvalFn)(void *scene, const char *cmd, char *outbuf, int outcap);
static JuliaEvalFn g_juliaEval = nullptr;

// File drag-and-drop: a window receives a dropped file and hands its local path to Julia
// (g_juliaDrop), which reads it (gmtread) and views it in a NEW window. Set via
// gmtvtk_set_drop_callback. nullptr -> drops ignored.
typedef void (*JuliaDropFn)(void *scene, const char *path);
static JuliaDropFn g_juliaDrop = nullptr;

// World Topo Tiles basemap picker (port of Mirone's bg_map.m). The "Base Map" menubar button opens
// a tile picker; a clicked tile's geographic region ("W/E/S/N/wrap") is handed to Julia (g_juliaBaseMap),
// which crops data/etopo4.jpg and adds it as a referenced flat image. g_basemapLogo is the path to
// the world logo image painted in the picker, pushed from Julia via gmtvtk_set_basemap_logo.
typedef void (*JuliaBaseMapFn)(void *scene, const char *region);
static JuliaBaseMapFn g_juliaBaseMap = nullptr;
static QString        g_basemapLogo;
static QString        g_basemapIcon;   // path to the Base Map toolbar-button icon (data/basemap_icon.png)

// Tiles Tool (Tools menu, port of Mirone's tiles_tool.m MINUS the url2image download/mosaic, which is
// replaced by GMT.jl's `mosaic`). The interactive picker hands "op;..." requests to Julia (g_juliaTiles):
// op "go" builds the final mosaic for the bracketed bbox (GMT.mosaic, two zoom levels coarser) and opens
// it in a new viewer; op "bg" (Phase 2) fetches a coarser mosaic for the current view and pushes it back.
// `dlg` is the picker (TilesPicker*) so Julia can call back into it. g_tilesWorld is the path to the
// equirectangular world image (data/etopo4.jpg) the picker crops/zooms as its base, pushed from Julia.
typedef void (*JuliaTilesFn)(void *scene, void *dlg, const char *params);
static JuliaTilesFn g_juliaTiles = nullptr;
static QString      g_tilesWorld;

// Background region (File > Background region, port of Mirone's empty-figure-with-limits). A small
// dialog asks for W/E/S/N + "Is Geographic?"; the result "W/E/S/N/geographic" is handed to Julia
// (g_juliaBgRegion), which opens a fresh window framed to those limits as a blank white 2-D map
// (axes only, ready for coastlines/overlays). Set via gmtvtk_set_bgregion_callback; nullptr -> the
// menu entry reports "callback not registered".
typedef void (*JuliaBgRegionFn)(void *scene, const char *region);
static JuliaBgRegionFn g_juliaBgRegion = nullptr;

// New Window (File > New Window). Opens a fresh empty iGMT launcher. Routed through Julia
// (g_juliaNewWindow) rather than calling gmtvtk_open_empty directly so the new window is tracked
// in the Julia figure registry — the basis for the (future) inter-window data exchange. nullptr ->
// the menu entry reports "callback not registered". `scene` is the window the menu was clicked in.
typedef void (*JuliaNewWindowFn)(void *scene);
static JuliaNewWindowFn g_juliaNewWindow = nullptr;

// Geography menu (Plot coastline / political boundaries / rivers). A leaf action computes the
// CURRENT visible geographic region (i.e. honouring the zoom level) and hands the request
// "<kind>/<res>/W/E/S/N" to Julia (g_juliaGeo), which runs GMT.coast and adds the resulting
// GMTdataset as a line overlay. kind = "coast" (others reserved); res = l/i/h/f. Set via
// gmtvtk_set_geography_callback. nullptr -> the leaf falls back to a "not implemented" status.
typedef void (*JuliaGeoFn)(void *scene, const char *req);
static JuliaGeoFn g_juliaGeo = nullptr;

// 3-D Bodies toolbar flyout. Each entry hands a GMT solid NAME ("cube"/"sphere"/"torus"/"cylinder"/
// "tetrahedron"/… — the SOLIDS catalogue keys in fv.jl) to Julia (g_juliaSolid), which builds the
// named GMTfv and opens it with view_fv. Set via gmtvtk_set_solid_callback; nullptr -> the buttons
// silently do nothing (feature reads as "not wired" until the DLL is rebuilt + Julia restarted).
typedef void (*JuliaSolidFn)(void *scene, const char *name);
static JuliaSolidFn g_juliaSolid = nullptr;

// grdsample tool (GMT menu). Hands "input;output;I;R;n;r;T" to Julia:
// input=input grid, output=output filename, I=inc, R=W/E/S/N, n=interp (+c for clipping),
// r=registration (g/p), T=toggle (1/0). Julia runs GMT.grdsample and views the result.
typedef void (*JuliaGrdsampleFn)(void *scene, const char *params);
static JuliaGrdsampleFn g_juliaGrdsample = nullptr;

// NSWING tsunami modelling tool (Geophysics menu). Port of Mirone's swan_options.m driving the nswing
// executable. The dialog (NswingDialog, 70_window.cpp) hands a newline-separated "key=value" block to
// Julia (g_juliaNswing), which assembles the nswing command line (-G/-Z/-A/-n, -M, -X, -N, -t, …) and
// launches it. nullptr -> the RUN button reports "callback not registered".
typedef void (*JuliaNswingFn)(void *scene, const char *params);
static JuliaNswingFn g_juliaNswing = nullptr;

// Save / Load Session (File menu). g_juliaSaveSession(scene, path) writes THIS window's state to a
// `.igmtz` (session.jl _on_save_session); g_juliaLoadSession(path) rebuilds a window from one
// (_on_load_session, opens its own window, so no scene arg). nullptr -> "callback not registered".
typedef void (*JuliaSaveSessionFn)(void *scene, const char *path);
typedef void (*JuliaLoadSessionFn)(void *scene, const char *path);
static JuliaSaveSessionFn g_juliaSaveSession = nullptr;
static JuliaLoadSessionFn g_juliaLoadSession = nullptr;

// IGRF Calculator (Geophysics > Magnetics). Port of Mirone's igrf_options.m, using GMT.jl's magref
// (mgd77magref) instead of the igrf_m MEX. Two callbacks:
//   g_juliaIgrfPoint(state): state = "lon/lat/elev_m/date_dec" -> "F/H/X/Y/Z/D/I" (nT x5, deg x2),
//     or "" on failure. Same Julia-owned-buffer convention as JuliaGridMetaFn/JuliaDimFunFn — C++
//     copies immediately, never frees. Driven by every Lat/Lon/Elev/Date edit + the map click.
//   g_juliaIgrfGrid(scene, params): params = "W/E/S/N/xinc/yinc/elev_m/date_dec/fieldcode"
//     (fieldcode one of T|H|X|Y|Z|D|I). Always opens a NEW viewer window (there is no existing
//     grid being resampled here, unlike grdsample). nullptr -> the Compute buttons report
//     "callback not registered".
typedef const char* (*JuliaIgrfPointFn)(const char *state);
static JuliaIgrfPointFn g_juliaIgrfPoint = nullptr;
typedef void (*JuliaIgrfGridFn)(void *scene, const char *params);
static JuliaIgrfGridFn g_juliaIgrfGrid = nullptr;
// Input Mag File "Compute" button: params = "infile;outfile;nHeaders;elev_m;date_dec". MVP scope
// (matches the dialog's own help text, which states 2 columns lon/lat is the MINIMUM it needs):
// reads the first two whitespace-separated numeric columns of every non-header line, computes
// Total Field for all of them at the shared Elevation/Date box values, and writes
// "lon\tlat\tfield" to outfile. Mirone's fuller version (per-row elevation/date columns via an
// interactive column selector, optional anomaly column) is NOT ported — see docs/GRDSAMPLE_TODO.md
// for the project's convention of flagging scoped-down ports instead of overclaiming.
typedef void (*JuliaIgrfFileFn)(void *scene, const char *params);
static JuliaIgrfFileFn g_juliaIgrfFile = nullptr;

// Reduction to the Pole / Total field to Components (Geophysics > Magnetics), port of Mirone's
// parker_stuff.m ('redPole'/'component' cases) + utils/mboard.m (FFT padding), via GMT.jl's own
// 1-D FFT (src/rtp3d.jl). ONE dialog (Rtp3DDialog) and ONE callback serve both menu entries; scene
// is the receiving window (promoted if empty, else the result is added as an extra surface).
// params = "fieldFile;fieldDip;fieldDec;magDip;magDec;component;newRows;newCols;mirror"
// (component: 0=RTP forced by the dialog regardless of its radio group, 1/2/3=North/East/Vert;
// newRows/newCols>grid size triggers Mirone-style FFT-edge padding; mirror "1"/"0"). nullptr to detach.
// Returns 1 on success, 0 on failure (parse error / grid-add rejected / exception) — unlike most of
// the fire-and-forget callbacks here, Rtp3DDialog needs a REAL yes/no to tell the user something
// happened, since the alternative feedback (scene's own Errors console) lives in a DIFFERENT window
// the user may not be looking at (see the "Compute does nothing, no error" investigation, 2026-07-20).
typedef int (*JuliaRtp3DFn)(void *scene, const char *params);
static JuliaRtp3DFn g_juliaRtp3D = nullptr;

// Gravity/Magnetic anomaly of a 3-D body (Geophysics > Magnetics > gmtgravmag3d) — GMT's gmtgravmag3d
// (Okabe 1979) through GMT.jl's `gravmag3d` (src/gravmag3d.jl). The dialog (GravMag3DDialog,
// 70_window.cpp, loads deps/ui/gravmag3d_dialog.ui) hands a NEWLINE-separated "key=value" block:
//   bodykind=geom|file|memfv, bodies=shape,params|shape,params (geom), file=+filekind=raw|index|stl,
//   onebased, noswap, mode=grav|mag, density= or magparams=f_dec/f_dip/m_int/m_dec/m_dip,
//   region=w/e/s/n, inc=xinc[/yinc], geog, zobs, level, thickness, radius, track, outfile
// (every key but bodykind/mode optional — an absent key means "don't pass that option to GMT").
// The result grid is added to `scene` as a new derived variable; a `track` run produces a table
// window instead. Returns 1 on success, 0 on failure — same real yes/no contract as g_juliaRtp3D,
// since the dialog, not the parent viewer's Errors console, is what the user is looking at.
// nullptr to detach.
typedef int (*JuliaGravMag3DFn)(void *scene, const char *params);
static JuliaGravMag3DFn g_juliaGravMag3D = nullptr;

// Same anomaly, body described by GRIDS instead of triangles (Geophysics > Magnetics >
// grdgravmag3d) — GMT's grdgravmag3d through GMT.jl (src/grdgravmag3d.jl). The dialog
// (GrdGravMag3DDialog, 70_window.cpp, loads deps/ui/grdgravmag3d_dialog.ui) hands a NEWLINE-separated
// "key=value" block: top=path|selected, bottom=path (two-grid mode), mode=grav|mag, density= or
// magparams=f_dec/f_dip/m_int/m_dec/m_dip (+ component=t|x|y|z|h, igrf=+i|+n, maggrid=path — GMT
// takes several -H, Julia assembles them), zlevel=bottom|top, region=w/e/s/n, inc=xinc[/yinc], geog,
// thickness, pad, zobs, radius, threads, track, outfile. Absent key = don't pass that option.
// Returns 1 on success, 0 on failure, same contract as g_juliaGravMag3D. nullptr to detach.
typedef int (*JuliaGrdGravMag3DFn)(void *scene, const char *params);
static JuliaGrdGravMag3DFn g_juliaGrdGravMag3D = nullptr;

// Continuous Reduction To the Pole / differential RTP (Geophysics > Magnetics > grdredpol) — GMT's
// grdredpol supplement, driven by src/grdredpol.jl in MONOLITHIC mode (GMT.jl has no verbose wrapper
// for it). The dialog (GrdRedPolDialog, 70_window.cpp, loads deps/ui/grdredpol_dialog.ui) hands a
// NEWLINE-separated "key=value" block: input=path|selected, year= (IGRF mode) or constdec=/constdip=
// (constant mode), incgrid=/decgrid= (Ei/Ed, either mode), filter=m/n, window=, boundary=m|r,
// notaylor=1, region=w/e/s/n, outfile=, filterfile=. Absent key = don't pass that option.
// Returns 1 on success, 0 on failure, same contract as the two gravmag callbacks. nullptr to detach.
typedef int (*JuliaGrdRedPolFn)(void *scene, const char *params);
static JuliaGrdRedPolFn g_juliaGrdRedPol = nullptr;

// "Open full manual page" — the little green ? disk every module dialog carries in its lower-left
// corner (addManualButton, 70_window.cpp). `name` is the GMT module name; Julia opens that module's
// page under https://www.generic-mapping-tools.org/GMTjl_doc/. Returns 1 if the page was opened.
// nullptr to detach.
typedef int (*JuliaOpenManualFn)(const char *name);
static JuliaOpenManualFn g_juliaOpenManual = nullptr;

// grdgradient (GMT menu) — directional derivative / slope / aspect of the window's grid, via GMT.jl's
// `grdgradient` (src/grdgradient.jl). The dialog (GrdGradientDialog, 70_window.cpp, loads
// deps/ui/grdgradient_dialog.ui) hands a NEWLINE-separated "key=value" block: azim=/azim2= (plain
// directional derivative) or finddir=<flags>[+slope=1] (slope/aspect mode), boundary=g|p|n,
// norm=linear|laplace|cauchy with amp=/sigma=/offset=. Absent key = don't pass that option. The
// result is added to `scene` as a new derived grid, named for what was computed. Returns 1 on
// success, 0 on failure. nullptr to detach.
typedef int (*JuliaGrdGradientFn)(void *scene, const char *params);
static JuliaGrdGradientFn g_juliaGrdGradient = nullptr;

// grdseamount (GMT menu) — synthetic seamounts from a table of parameters, via GMT.jl's
// `grdseamount` (src/grdseamount.jl). The dialog (GrdSeamountDialog, 70_window.cpp, loads
// deps/ui/grdseamount_dialog.ui) hands a NEWLINE-separated "key=value" block: table=path or
// record=lon/lat/[azimuth/semimajor/semiminor|radius]/height, region, inc, shape, elliptical,
// flattening or flatcol, unit, level, levelnan, normalize, mask+maskout/maskin/maskscale,
// liststats, time+buildmode+list, densities+densify/denspower/densitygrid/densityout, outgrid.
// Absent key = don't pass that option. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGrdSeamountFn)(void *scene, const char *params);
static JuliaGrdSeamountFn g_juliaGrdSeamount = nullptr;

// Import *.gmt/*.nc cruise track file(s) (Geophysics > Magnetics), port of Mirone's
// GeophysicsImportGmtFile_CB (mirone.m) — plots the navigation (lon/lat) of MGD77+ netCDF cruise
// files. `path` is either the single file picked, or a list-file (one path per line, "#"-prefixed
// lines are comments) when `isList` is nonzero. Julia resolves each file, reads it via GMT.jl's own
// mgd77list, and adds it as a line overlay; a list producing more than one track groups them under
// one master handle named for the list file (SACRED_LAW master-handle-per-file). nullptr -> the menu
// entries report "callback not registered".
typedef void (*JuliaImportGmtFn)(void *scene, const char *path, int isList);
static JuliaImportGmtFn g_juliaImportGmt = nullptr;

// Clip Grid (Grid Tools), port of Mirone's src_figs/ml_clip.m. The dialog (ClipGridDialog,
// 70_window.cpp, loads deps/ui/clipp_grid.ui) hands "below;above;belowVal;aboveVal;inBetween;stretch"
// to Julia (_on_clipgrid, src/clipgrid.jl): grid nodes < below get belowVal, > above get aboveVal
// (empty value = that side not clipped, "NaN" is a valid replacement); inBetween replaces the
// [below,above] band by belowVal instead; stretch contrast-clamps both sides to [below,above]. The
// clipped result is added to `scene` as a NEW derived grid. Returns 1 on success, 0 on failure
// (same real yes/no contract as g_juliaRtp3D — the dialog reports it, the parent's Errors console
// may be on a window the user isn't looking at). nullptr to detach.
typedef int (*JuliaClipGridFn)(void *scene, const char *params);
static JuliaClipGridFn g_juliaClipGrid = nullptr;

// Grid calculator (Grid Tools), port of Mirone's src_figs/grid_calculator.m. The dialog
// (GridCalculatorDialog, 70_window.cpp, loads deps/ui/grid_calculator.ui) hands a newline-separated
// "key=value" block to Julia (_on_gridcalc, src/gridcalc.jl):
//   expr=<the expression, grid names written &name or &{name with blanks}>
//   base=<Scene Objects label of the window's base grid, so &thatname resolves>
//   file<i>=<path>       (one per grid added with "Load Grid"; matched by file NAME in the expr)
// The result is added to `scene` as a NEW derived grid (SACRED_LAW derived-variable display law).
// Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGridCalcFn)(void *scene, const char *params);
static JuliaGridCalcFn g_juliaGridCalc = nullptr;

// grdtrend (GMT menu), dialog laid out after Mirone's src_figs/grdtrend_mir.m. GrdTrendDialog
// (70_window.cpp, loads deps/ui/grdtrend_dialog.ui) hands a newline-separated "key=value" block to
// Julia (_on_grdtrend, src/grdtrend.jl): what=trend|diff|weights, model=<n>, robust, protectnans,
// axis=|x|y, region, wfile (+ sigma), outfile, grid (the DISPLAYED layer's label). The result is
// added to `scene` as a NEW derived grid. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGrdTrendFn)(void *scene, const char *params);
static JuliaGrdTrendFn g_juliaGrdTrend = nullptr;

// grdlandmask (GMT menu), dialog laid out after Mirone's grdlandmask window. GrdLandmaskDialog
// (70_window.cpp, loads deps/ui/grdlandmask_dialog.ui) hands a newline-separated "key=value" block to
// Julia (_on_grdlandmask, src/grdlandmask.jl): region, inc, res, area, maskvalues, border, pixel,
// verbose, clip (mask the window's own grid instead of making a bare mask), outfile, grid. The result
// is added to `scene` as a NEW derived grid. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGrdLandmaskFn)(void *scene, const char *params);
static JuliaGrdLandmaskFn g_juliaGrdLandmask = nullptr;

// grdfilter (GMT menu), dialog laid out after Mirone's Grdfilter window. GrdFilterDialog
// (70_window.cpp, loads deps/ui/grdfilter_dialog.ui) hands a newline-separated "key=value" block to
// Julia (_on_grdfilter, src/grdfilter.jl): filter (the whole -F string, code + width + modifiers),
// distance, region, inc, nans, toggle, outfile, grid (the DISPLAYED layer's label). The filtered grid
// is added to `scene` as a NEW derived grid. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGrdFilterFn)(void *scene, const char *params);
static JuliaGrdFilterFn g_juliaGrdFilter = nullptr;

// Plot seismicity (Geophysics > Seismology). Port of Mirone's earthquakes.m. The dialog
// (PlotSeismicityDialog, 70_window.cpp) hands a newline-separated "key=value" block to Julia
// (g_juliaSeismicity), which reads the catalog (USGS web query / ISF / plain-column layouts /
// Posit), filters by date/magnitude/depth/visible region and stamps the events as screen-constant
// symbol layers. nullptr -> the menu entry reports "callback not registered".
typedef void (*JuliaSeismicityFn)(void *scene, const char *params);
static JuliaSeismicityFn g_juliaSeismicity = nullptr;

// Vertical elastic deformation (Geophysics menu). Port of Mirone's Okada (1985) surface-deformation
// tool. The dialog (ElasticDialog, 70_window.cpp) hands a semicolon-separated parameter string
//   "action;coord;len;wid;strike;dip;depth;depthTop;rake;slip;hide;scc;N;q;mu;R;I"
// to Julia (g_juliaElastic) on Compute / Save fault. The compute side is not implemented yet;
// nullptr -> the menu reports "callback not registered".
typedef void (*JuliaElasticFn)(void *scene, const char *params);
static JuliaElasticFn g_juliaElastic = nullptr;

// Fault-trace endpoint recompute (Vertical elastic deformation dialog). Port of Mirone's
// edit_FaultStrike_CB / edit_FaultLength_CB (deform_mansinha.m): when the user edits the Strike or
// Length box, the fault line's end vertex must move so the drawn trace matches the typed geometry.
// Mirone solves this with vreckon (direct geodesic); we hand the fixed start point + strike + length
// (km) to Julia, which calls GMT.geod and returns "lon2/lat2" (Julia-owned buffer, copied at once).
// Geographic faults only — cartesian faults move the endpoint with plain trig C++-side, no round
// trip. Set via gmtvtk_set_faultgeom_callback; nullptr -> geographic edits leave the trace unchanged.
typedef const char* (*JuliaFaultGeomFn)(double lon1, double lat1, double strike, double len_km);
static JuliaFaultGeomFn g_juliaFaultGeom = nullptr;

// Import Trace Fault (Geophysics > Seismology > Elastic deformation). Port of Mirone's
// fault_models.m `subfault`: read a sub-fault-format file and lay its surface fault traces into the
// scene as Draw-Fault lines (isFault polylines that carry the Vertical elastic deformation dialog).
// The host menu opens a QFileDialog and hands the chosen path to Julia (g_juliaImportFault), which
// parses the file, rebuilds each downdip row's up-dip trace with GMT.geod and calls
// gmtvtk_add_fault_h back per trace. nullptr -> the menu reports "callback not registered".
typedef void (*JuliaImportFaultFn)(void *scene, const char *path);
static JuliaImportFaultFn g_juliaImportFault = nullptr;

// Import Model Slip (Geophysics > Seismology > Elastic deformation). Port of Mirone's
// fault_models.m `subfault`, full slip model: read a sub-fault-format file and lay EVERY sub-fault
// patch into the scene as a filled polygon coloured by its slip (the surface projection of each
// patch — NOT the dipping 3-D planes). The host menu opens a QFileDialog and hands the chosen path
// to Julia (g_juliaModelSlip), which parses the file, builds each patch's surface-projection quad
// with the ported circ_geo spherical forward step and calls gmtvtk_add_slip_patches_h with the whole
// batch. nullptr -> the menu reports "callback not registered".
typedef void (*JuliaModelSlipFn)(void *scene, const char *path);
static JuliaModelSlipFn g_juliaModelSlip = nullptr;

// Focal mechanisms (Geophysics > Seismology). Port of Mirone's focal_meca.m. The dialog
// (FocalMechanismsDialog, 70_window.cpp) hands a newline-separated "key=value" block to Julia
// (g_juliaFocal), which reads the chosen catalog (ISF / Aki & Richards / Harvard CMT / CMT
// .ndk), filters by magnitude/depth, computes each event's compressive/dilatational "beachball"
// patches (patch_meca.m's equal-area nodal-plane projection) and adds them via
// gmtvtk_add_meca_h. nullptr -> the menu entry reports "callback not registered".
typedef void (*JuliaFocalFn)(void *scene, const char *params);
static JuliaFocalFn g_juliaFocal = nullptr;

// Focal-mechanism GROUP properties (Scene Objects group row, left-click -> mecaGroupPropsDialog,
// 50_scene.cpp): (scene, groupName, "key=value\n…") for compcolor/dilatcolor/rimcolor/rimwidth.
// Julia removes the old batch (gmtvtk_remove_meca_group_h) and re-plots it from the ORIGINAL
// catalog params (stashed at the first plot) with these overrides merged in.
typedef void (*JuliaMecaPropsFn)(void *scene, const char *groupName, const char *params);
static JuliaMecaPropsFn g_juliaMecaProps = nullptr;

// grdsample "OR Ref grid" picker (and grid-metadata prefill). Given a grid/image path, Julia
// gmtreads its header and returns "W/E/S/N/xinc/yinc/nx/ny" (empty string on failure) so the
// dialog can fill the Griding Line Geometry boxes. The returned pointer is owned by Julia (a
// module-global buffer rooted until the next call); C++ copies it immediately, never frees it.
typedef const char* (*JuliaGridMetaFn)(const char *path);
static JuliaGridMetaFn g_juliaGridMeta = nullptr;

// grdsample Region box cross-field recompute (port of Mirone's dim_funs.m, implemented in Julia).
// Called as fn(which, state) when one of the 8 geometry boxes is edited:
//   which = "xMin|xMax|yMin|yMax|xInc|yInc|nCols|nRows"
//   state = "xMin/xMax/yMin/yMax/xInc/yInc/nCols/nRows/oneOrZero/xMinOr/xMaxOr/yMinOr/yMaxOr/dms"
// Returns the 8 recomputed fields "xMin/xMax/yMin/yMax/xInc/yInc/nCols/nRows" (same Julia-owned
// buffer convention as JuliaGridMetaFn — C++ copies immediately, never frees).
typedef const char* (*JuliaDimFunFn)(const char *which, const char *state);
static JuliaDimFunFn g_juliaDimFun = nullptr;

// 3D Cube layer selector dialog callback. Called when the user selects a layer from a 3D cube, or
// toggles the "Scale color to global min/max" checkbox (useGlobalScale != 0 -> colour by the
// whole cube's z-range instead of this slice's own). The dialog is non-modal, so the user can
// switch between layers while keeping the dialog open.
typedef void (*JuliaCubeLayerFn)(void *scene, int layerIndex, int useGlobalScale);
static JuliaCubeLayerFn g_juliaCubeLayer = nullptr;

// "Load all in RAM" button in the cube layer dock: load the whole cube into memory so subsequent
// layer switches are instant (memory slice) instead of a per-layer disk read. Returns a status:
// 0 = loaded OK, 1 = not enough free RAM (nothing loaded), 2 = error. Julia does the RAM check.
typedef int (*JuliaCubeLoadAllFn)(void *scene);
static JuliaCubeLoadAllFn g_juliaCubeLoadAll = nullptr;

// "Cube layers…" item in a cube element's Scene Objects menu. A window can hold several cubes (each a
// separate surface); this asks Julia to make the NAMED cube the active one and (re)open the slider
// dock bound to it. The single per-Scene dock is retargeted, not duplicated.
typedef void (*JuliaCubeSliderFn)(void *scene, const char *name);
static JuliaCubeSliderFn g_juliaCubeSlider = nullptr;

// The Aquamoto control window (75_aquamoto.cpp) closes to HIDDEN, never destroyed; its viewer scene's
// surface handle offers "Aquamoto viewer…" to re-show it. surfaceObjectMenu (50_scene.cpp) is compiled
// before 75_aquamoto.cpp, so it reaches the window through these hooks, which 75 installs at load.
// `has` reports whether `scene` owns an Aquamoto window (gates the menu entry); `reopen` re-shows it.
struct Scene;
static bool (*g_aquamotoHasWindow)(Scene *scene) = nullptr;
static void (*g_aquamotoReopen)(Scene *scene) = nullptr;
static void (*g_aquamotoSetVisible)(Scene *scene, int on) = nullptr;   // Scene Objects handle checkbox: show/hide the window
static bool (*g_aquamotoIsVisible)(Scene *scene) = nullptr;            // current window visibility (checkbox initial state)
static void (*g_aquamotoDestroy)(Scene *scene) = nullptr;             // destroy the window (lifetime-tied to its nc cube surface)
static void (*g_aquamotoSetCmap)(Scene *scene, int side, const char *cmap) = nullptr;   // side 0=water,1=land; re-renders the current slice

// File > Save Grid / Save Image. The host File menu opens a QFileDialog (format picked via the
// filter) and hands "<kind>;<fmt>;<path>" to Julia (g_juliaSave): kind = "grid" | "image"; fmt a
// short format code (nc/surfer/gtiff/jp2/erdas/envi for grids; those + jpg/png/tif/bmp for images);
// path = the chosen file. Julia writes the window's primary GMTgrid/GMTimage via GMT.gmtwrite
// (netCDF/Surfer) or GMT.gdalwrite (the rest). Set via gmtvtk_set_save_callback; nullptr -> the menu
// entry reports "callback not registered".
typedef void (*JuliaSaveFn)(void *scene, const char *req);
static JuliaSaveFn g_juliaSave = nullptr;

// File > Save Screenshot GeoTIFF. Passes the captured RGB pixels straight to Julia in memory (no
// temp file, no PNG encode/decode/re-read) — `rgb` is a packed row-major buffer (top row first,
// like a standard image file), w*h*3 bytes, owned by the caller and only valid for the duration of
// this call. Julia wraps it directly into a GMTimage (mat2img) and writes the real GeoTIFF via
// GDAL. Set via gmtvtk_set_save_geotiff_callback; nullptr -> the menu entry reports "callback not
// registered".
typedef void (*JuliaSaveGeoTiffFn)(void *scene, const unsigned char *rgb, int w, int h,
                                    const char *path, double x0, double x1, double y0, double y1,
                                    const char *proj4, const char *wkt);
static JuliaSaveGeoTiffFn g_juliaSaveGeoTiff = nullptr;

// Scene Objects > "Move to new window" (grid rows). The row menu calls fn(scene, "<kind>;<name>")
// (kind = "grid"); Julia looks up the live GMTgrid and opens it in a NEW iGMT window via view_grid,
// returning 1 on success. The source window then removes the grid (= a MOVE, not a copy). Set via
// gmtvtk_set_move_callback; nullptr -> a status-bar notice and the grid stays put.
typedef int (*JuliaMoveFn)(void *scene, const char *req);
static JuliaMoveFn g_juliaMove = nullptr;

// Scene Objects image row > "Auto histogram stretch". Calls fn(scene, "image;<name>"); Julia looks up
// the image's full-precision source (a 16-bit satellite band shown as a fast min-max 8-bit preview),
// runs GMT's percentile histogram stretch, and adds the result as a NEW image row in this window.
// Set via gmtvtk_set_img_stretch_callback; nullptr -> a status-bar notice.
typedef void (*JuliaImgStretchFn)(void *scene, const char *req);
static JuliaImgStretchFn g_juliaImgStretch = nullptr;

// One selectable output format for the Save dialog: a human label, the short code handed to Julia,
// the QFileDialog filter, and the canonical extension (used to seed/auto-suffix the file name).
struct SaveFmt { const char *label; const char *code; const char *filter; const char *ext; };
// Grids: netCDF + Surfer 6 go through GMT.gmtwrite; the rest through GMT.gdalwrite (driver by ext).
static const SaveFmt kGridFmts[] = {
	{ "netCDF grid",   "nc",     "netCDF grid (*.nc *.grd)", ".nc"  },
	{ "GeoTIFF",       "gtiff",  "GeoTIFF (*.tif *.tiff)",   ".tif" },
	{ "JPEG2000",      "jp2",    "JPEG2000 (*.jp2)",         ".jp2" },
	{ "Erdas Imagine", "erdas",  "Erdas Imagine (*.img)",    ".img" },
	{ "Surfer 6 grid", "surfer", "Surfer 6 grid (*.grd)",    ".grd" },
	{ "ENVI",          "envi",   "ENVI (*.hdr)",             ".hdr" },
};
// Images always go through GMT.gdalwrite (driver by extension).
static const SaveFmt kImageFmts[] = {
	{ "GeoTIFF",       "gtiff", "GeoTIFF (*.tif *.tiff)", ".tif" },
	{ "JPEG2000",      "jp2",   "JPEG2000 (*.jp2)",       ".jp2" },
	{ "Erdas Imagine", "erdas", "Erdas Imagine (*.img)",  ".img" },
	{ "ENVI",          "envi",  "ENVI (*.hdr)",           ".hdr" },
	{ "JPEG",          "jpg",   "JPEG (*.jpg *.jpeg)",    ".jpg" },
	{ "PNG",           "png",   "PNG (*.png)",            ".png" },
	{ "TIFF",          "tif",   "TIFF (*.tif)",           ".tif" },
	{ "BMP",           "bmp",   "BMP (*.bmp)",            ".bmp" },
};

// The "little window" the user asked for: pick an output format from a combo, then a file (Browse
// runs the native save dialog filtered to that format; changing the format re-suffixes the path).
// On accept, `code` + `path` carry the choice. isGrid selects the grid vs image format list.
struct SaveFormatDialog : QDialog {
	const SaveFmt *fmts; int nfmt;
	QComboBox *combo; QLineEdit *pathEdit; QPushButton *okBtn;
	QString code, path;

	static QString sanitize(const QString &n) {                 // object label -> safe file stem
		QString r; for (QChar c : n) r += (c.isLetterOrNumber() || c == '_' || c == '-') ? c : QChar('_');
		return r;
	}
	void swapExt() {                                            // keep the path's extension == the format
		QString p = pathEdit->text().trimmed();
		if (p.isEmpty()) return;
		int dot = p.lastIndexOf('.');
		int sep = std::max(p.lastIndexOf('/'), p.lastIndexOf('\\'));
		if (dot > sep) p = p.left(dot);
		pathEdit->setText(p + fmts[combo->currentIndex()].ext);
	}
	SaveFormatDialog(QWidget *parent, bool isGrid, const QString &objName) : QDialog(parent) {
		fmts = isGrid ? kGridFmts : kImageFmts;
		nfmt = isGrid ? (int)(sizeof(kGridFmts) / sizeof(kGridFmts[0]))
		              : (int)(sizeof(kImageFmts) / sizeof(kImageFmts[0]));
		setWindowTitle(isGrid ? "Save grid" : "Save image");
		QVBoxLayout *v = new QVBoxLayout(this);
		if (!objName.isEmpty()) v->addWidget(new QLabel("Object:  " + objName, this));

		QHBoxLayout *fr = new QHBoxLayout();
		fr->addWidget(new QLabel("Format:", this));
		combo = new QComboBox(this);
		for (int i = 0; i < nfmt; ++i) combo->addItem(fmts[i].label);
		fr->addWidget(combo, 1);
		v->addLayout(fr);

		QHBoxLayout *pr = new QHBoxLayout();
		pr->addWidget(new QLabel("File:", this));
		pathEdit = new QLineEdit(this); pathEdit->setMinimumWidth(300);
		pr->addWidget(pathEdit, 1);
		QPushButton *browse = new QPushButton("Browse…", this);
		pr->addWidget(browse, 0);
		v->addLayout(pr);

		QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
		v->addWidget(bb);
		okBtn = bb->button(QDialogButtonBox::Save);
		okBtn->setEnabled(false);

		const QString seed = objName.isEmpty() ? QString() : sanitize(objName);
		QObject::connect(browse, &QPushButton::clicked, this, [this, seed]() {
			const SaveFmt &f = fmts[combo->currentIndex()];
			QString start = pathEdit->text().trimmed();
			if (start.isEmpty()) start = prefStartDir(seed.isEmpty() ? QString() : seed + f.ext);
			QString fn = QFileDialog::getSaveFileName(this, "Save as", start, f.filter);
			if (!fn.isEmpty()) { pathEdit->setText(fn); rememberStartDir(fn); }
		});
		QObject::connect(pathEdit, &QLineEdit::textChanged, this,
		                 [this](const QString &t) { okBtn->setEnabled(!t.trimmed().isEmpty()); });
		QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		                 [this](int) { swapExt(); });
		QObject::connect(bb, &QDialogButtonBox::accepted, this, [this]() {
			code = fmts[combo->currentIndex()].code;
			path = pathEdit->text().trimmed();
			accept();
		});
		QObject::connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
	}
};

// Open the Save dialog for a scene object and hand the choice to Julia (g_juliaSave) as
// "<kind>;<fmt>;<path>;<name>". kind = "grid" | "image"; name identifies which object (empty for the
// File-menu "save the window's grid/image"). nullptr callback -> a status-bar notice.
static void saveObjectDialog(Scene *s, const char *kind, const QString &name) {
	if (!g_juliaSave) {
		if (s && s->win) s->win->statusBar()->showMessage("Save: callback not registered", 3000);
		return;
	}
	const bool isGrid = (QString(kind) == QLatin1String("grid"));
	SaveFormatDialog dlg(s ? s->win : nullptr, isGrid, name);
	if (dlg.exec() != QDialog::Accepted || dlg.path.isEmpty()) return;
	const QString req = QString("%1;%2;%3;%4").arg(QString(kind)).arg(dlg.code).arg(dlg.path).arg(name);
	g_juliaSave(s, req.toUtf8().constData());
}

// Ask Julia to open the named scene grid in a NEW window (g_juliaMove, req = "<kind>;<name>").
// Returns true ONLY if Julia reported success — the caller then removes the grid from this window
// (= a move). A nullptr callback or a Julia failure leaves the source window untouched.
static bool moveObjectToNewWindow(Scene *s, const char *kind, const QString &name) {
	if (!g_juliaMove) {
		if (s && s->win) s->win->statusBar()->showMessage("Move to new window: callback not registered", 3000);
		return false;
	}
	const QString req = QString("%1;%2").arg(QString(kind)).arg(name);
	return g_juliaMove(s, req.toUtf8().constData()) != 0;
}

// Ask Julia to build a histogram-stretched 8-bit copy of the named image as a NEW row (g_juliaImgStretch,
// req = "image;<name>"). A nullptr callback just posts a status-bar notice.
static void stretchImageObject(Scene *s, const QString &name) {
	if (!g_juliaImgStretch) {
		if (s && s->win) s->win->statusBar()->showMessage("Histogram stretch: callback not registered", 3000);
		return;
	}
	const QString req = QString("image;%1").arg(name);
	g_juliaImgStretch(s, req.toUtf8().constData());
}

// Does the window hold a saveable grid / image? Used to enable/disable the File>Save entries and to
// decide which per-object "Save…" to show. A grid = the primary relief surface (not a bare image,
// not the empty launcher) or any non-image extra. An image = the bare primary image or any image
// extra (drops / basemap / tiles / iview_image_obj).
static bool sceneHasGrid(Scene *s) {
	if (!s) return false;
	if (s->surf && !s->emptyStart && !s->imageOnly) return true;
	for (auto &ex : s->extras) if (!ex.isImage) return true;
	return false;
}
static bool sceneHasImage(Scene *s) {
	if (!s) return false;
	if (s->drape && s->imageOnly) return true;            // bare image opened by view_image
	for (auto &ex : s->extras) if (ex.isImage) return true;
	return false;
}

// Tide-station download menu. A right-click on a "Tide Stations" star adds two entries —
// "Download Mareg (2 days)" / "Download Mareg (Calendar)" — that hand (mode, station) to Julia,
// which opens the Mareg download window. mode = "2days" | "calendar"; station = the clicked star's
// "Name:/Code:/Country:" hover block. Set via gmtvtk_set_tides_callback; nullptr -> entries hidden.
typedef void (*JuliaTidesFn)(void *scene, const char *mode, const char *station);
static JuliaTidesFn g_juliaTides = nullptr;

// Tide-prediction menu. A right-click on a "Tide Prediction Stations" triangle adds "Plot tides
// (now)" / "Plot tides (calendar)", which hand (mode, station) to Julia -- mode is "now" or
// "calendar/<startISO>/<endISO>"; station is the clicked triangle's hover text (= the exact
// xtide.mat station name, no parsing needed). Julia harmonic-synthesizes the prediction
// (GMT.xtide_predict) over the requested window and opens it in a standalone X,Y window. Set via
// gmtvtk_set_tidemodel_callback; nullptr -> hidden.
typedef void (*JuliaTideModelFn)(void *scene, const char *mode, const char *station);
static JuliaTideModelFn g_juliaTideModel = nullptr;

// Earth-tides dialog (Geography > Earth Tides, port of Mirone's earth_tides). The dialog hands
// "<mode>/<startISO>/<endISO>/<lon>/<lat>/<comp>/<W>/<E>/<S>/<N>" to Julia (g_juliaEarthTide):
// mode = "series" | "grid"; comp = subset of "VEN" (Vertical/East/North). Julia runs
// GMT.earthtide and either opens an X,Y window (time series) or adds a grid to the scene. Set via
// gmtvtk_set_earthtide_callback; nullptr -> the menu entry reports "callback not registered".
typedef void (*JuliaEarthTideFn)(void *scene, const char *req);
static JuliaEarthTideFn g_juliaEarthTide = nullptr;

// Live scenes, keyed by the Scene *returned to the host as an opaque figure handle.
// A handle is valid only while its window is open; the window-destroyed lambda erases
// it here so a stale handle from a closed figure is rejected instead of dereferenced.
static std::unordered_set<Scene*> g_scenes;
static bool sceneAlive(Scene *s) { return s && g_scenes.count(s) != 0; }

// Two kernel32 calls, hand-declared instead of #include <windows.h>: that header unconditionally
// drags in wingdi.h (WIN32_LEAN_AND_MEAN does NOT gate it — tried, reverted), whose GDI
// `Polygon()` function collides with this codebase's own `Polygon` struct (10_geometry.cpp)
// since it's all one translation unit — `Polygon &pg` then binds to the wrong (function)
// declaration and fails to parse. This sidesteps the whole header instead of chasing every bare
// `Polygon` usage into an elaborated `struct Polygon`.
extern "C" {
	__declspec(dllimport) int __stdcall GetModuleHandleExA(unsigned long dwFlags, const char *lpModuleName, void **phModule);
	__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *hModule, char *lpFilename, unsigned long nSize);
}

// Absolute path to the directory gmtvtk.dll itself was loaded from (deps/build for both a dev
// build and an NSIS-installed tree — see deps/CMakeLists.txt). Resolved via the module handle of
// an address inside this DLL, NOT argv[0]/applicationDirPath() — when hosted by Julia, argv[0] is
// a fabricated dummy (see ensureApp) and the real process exe is julia.exe, nowhere near this
// DLL. Returns empty string if the lookup fails (should not happen for a loaded DLL).
static QString gmtvtkModuleDir() {
	const unsigned long GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 0x4;
	const unsigned long GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 0x2;
	void *hm = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<const char *>(&gmtvtkModuleDir), &hm);
	char buf[260];
	unsigned long n = hm ? GetModuleFileNameA(hm, buf, sizeof(buf)) : 0;
	if (n == 0) return QString();
	return QFileInfo(QString::fromLocal8Bit(buf, (int)n)).absolutePath();
}

// Absolute path to deps/ui/ (the runtime-loaded .ui files, see FocalMechanismsDialog in
// 70_window.cpp): <module dir>/../ui. GMTVTK_UI_DIR is kept only as a last-resort fallback
// (module-dir lookup failed).
static QString gmtvtkUiDir() {
	QString modDir = gmtvtkModuleDir();
	if (modDir.isEmpty()) return QString(GMTVTK_UI_DIR);
	QDir dir(modDir);
	dir.cdUp();                     // deps/build -> deps
	return dir.filePath("ui");      // deps/ui
}

// Absolute path to the package's data/ dir (data files like Cande_Kent_95.dat, shared with the
// Julia side's data/ — see hydrothermal_vents.dat, meteoritos.dat). NOT deps/assets: that was an
// earlier, wrong copy of Cande_Kent_95.dat (stale M-sequence ages); the real one lives here.
static QString gmtvtkDataDir() {
	QString modDir = gmtvtkModuleDir();
	if (modDir.isEmpty()) return QString(GMTVTK_DATA_DIR);
	QDir dir(modDir);
	dir.cdUp();                     // deps/build -> deps
	dir.cdUp();                     // deps -> package root
	return dir.filePath("data");    // <package root>/data
}

// iGMT application/window icon, decoded once from the embedded PNG (see _app_icon.h).
static QIcon appIcon() {
	static QIcon ic = []{
		QPixmap pm;
		pm.loadFromData(kAppIconPng, kAppIconPngLen, "PNG");
		return QIcon(pm);
	}();
	return ic;
}

// App-wide rule: pressing Enter/Return in ANY QLineEdit drops keyboard focus, which commits the edit
// through the normal editingFinished path (so live-update callbacks fire). Installed once on the
// QApplication so every box in every dialog behaves the same — no per-widget wiring. The event is not
// consumed, so returnPressed / default-button handlers still run after the defocus.
class EnterDefocusFilter : public QObject {
public:
	using QObject::QObject;
	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (ev->type() == QEvent::KeyPress) {
			const int key = static_cast<QKeyEvent*>(ev)->key();
			if (key == Qt::Key_Return || key == Qt::Key_Enter)
				if (auto *le = qobject_cast<QLineEdit*>(obj)) le->clearFocus();
		}
		return QObject::eventFilter(obj, ev);
	}
};

static void ensureApp() {
	if (g_app) return;
	// QApplication needs argc/argv that outlive it; there is none when driven from
	// a host, so fabricate a persistent dummy argv.
	static int   s_argc = 1;
	static char  s_arg0[] = "gmtvtk";
	static char *s_argv[] = { s_arg0, nullptr };
	// Qt hunts its platform plugin (platforms/qwindows.dll) relative to argv[0]'s directory at
	// QApplication construction time. argv[0] above is a fabricated dummy, so the real lookup
	// would land next to the HOST exe (julia.exe) — nowhere near the windeployqt-staged plugins
	// beside gmtvtk.dll (deps/build, see deps/CMakeLists.txt GMTVTK_PACKAGE) on a box without a
	// Qt install. Must run BEFORE the QApplication ctor below.
	QString modDir = gmtvtkModuleDir();
	if (!modDir.isEmpty()) QCoreApplication::addLibraryPath(modDir);
	QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
	g_app = new QApplication(s_argc, s_argv);
	g_app->setWindowIcon(appIcon());   // taskbar / app-wide default icon
	g_app->installEventFilter(new EnterDefocusFilter(g_app));   // Enter defocuses any QLineEdit (app-wide)
}

// Middle button, done by hand (not the default trackball, which the gizmo's left-drag
// observer + Qt make unreliable here):
//   - DRAG  -> pan: translate camera position + focal point so the world point under the
//              cursor (at the focal-plane depth) stays under the cursor.
//   - CLICK -> recenter: pick the SURFACE point (never the gizmo) and make it the new
//              centre of rotation, keeping the view direction + distance. The gizmo,
//              pinned to the focal point by PlaceCB, follows on the next render.
// Observed at priority 10; sets the abort flag per-event (so a plain mouse-move with the
// middle button up still reaches the coordinate readout + gizmo hover).
void MiddleCB(vtkObject *caller, unsigned long eid, void *clientData, void*) {
	Scene *s = static_cast<Scene*>(clientData);
	vtkRenderWindowInteractor *rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
	if (!s || !rwi) return;
	vtkRenderer *ren = s->ren;
	vtkCamera *cam = (ren && ren->GetActiveCamera()) ? ren->GetActiveCamera() : nullptr;
	if (!ren || !cam) return;
	bool handled = false;

	if (eid == vtkCommand::MiddleButtonPressEvent) {
		fprintf(stderr, "[mid] PRESS @ %d,%d\n", rwi->GetEventPosition()[0], rwi->GetEventPosition()[1]); fflush(stderr);
		s->midDown = true; s->midMoved = false;
		s->midX = rwi->GetEventPosition()[0];
		s->midY = rwi->GetEventPosition()[1];
		vtkNew<vtkCellPicker> pk;
		pk->SetTolerance(0.0005);
		pk->PickFromListOn(); pk->AddPickList(surfProp(s));   // recenter target = surface only
		s->midPicked = (pk->Pick(s->midX, s->midY, 0.0, ren) != 0);
		if (s->midPicked) pk->GetPickPosition(s->midPick);
		handled = true;
	}
	else if (eid == vtkCommand::MouseMoveEvent && s->midDown) {
		const int x = rwi->GetEventPosition()[0], y = rwi->GetEventPosition()[1];
		fprintf(stderr, "[mid] DRAG -> %d,%d\n", x, y); fflush(stderr);
		if (std::abs(x - s->midX) > 2 || std::abs(y - s->midY) > 2) s->midMoved = true;
		// Project the focal point to display to get its depth, then unproject the old and
		// new cursor positions at that depth; the difference is the world-space pan.
		double fp[3]; cam->GetFocalPoint(fp);
		ren->SetWorldPoint(fp[0], fp[1], fp[2], 1.0); ren->WorldToDisplay();
		const double depth = ren->GetDisplayPoint()[2];
		ren->SetDisplayPoint((double)x, (double)y, depth);            ren->DisplayToWorld();
		double np[4]; for (int i=0;i<4;++i) np[i]=ren->GetWorldPoint()[i];
		ren->SetDisplayPoint((double)s->midX, (double)s->midY, depth); ren->DisplayToWorld();
		double op[4]; for (int i=0;i<4;++i) op[i]=ren->GetWorldPoint()[i];
		if (np[3]!=0.0) { np[0]/=np[3]; np[1]/=np[3]; np[2]/=np[3]; }
		if (op[3]!=0.0) { op[0]/=op[3]; op[1]/=op[3]; op[2]/=op[3]; }
		const double mot[3] = { op[0]-np[0], op[1]-np[1], op[2]-np[2] };
		double pos[3]; cam->GetPosition(pos);
		cam->SetFocalPoint(fp[0]+mot[0], fp[1]+mot[1], fp[2]+mot[2]);
		cam->SetPosition (pos[0]+mot[0], pos[1]+mot[1], pos[2]+mot[2]);
		ren->ResetCameraClippingRange();
		s->midX = x; s->midY = y;
		rwi->Render();
		handled = true;
	}
	else if (eid == vtkCommand::MiddleButtonReleaseEvent) {
		if (s->midDown && !s->midMoved && s->midPicked) {  // click, no drag -> recenter
			double pos[3], fpc[3]; cam->GetPosition(pos); cam->GetFocalPoint(fpc);
			const double dir[3] = { pos[0]-fpc[0], pos[1]-fpc[1], pos[2]-fpc[2] };
			cam->SetFocalPoint(s->midPick);
			cam->SetPosition(s->midPick[0]+dir[0], s->midPick[1]+dir[1], s->midPick[2]+dir[2]);
			ren->ResetCameraClippingRange();
			rwi->Render();
		}
		s->midDown = false;
		handled = true;
	}

	if (s->midCmd) s->midCmd->SetAbortFlagOnExecute(handled ? 1 : 0);
}

// Middle button reaches the Qt widget (left/right do) but the VTK interactor adapter was
// NOT delivering MiddleButton events to our observers ([mid] PRESS never fired). So drive
// middle-button PAN (drag) and RECENTER (click, no drag) straight from a Qt event filter
// installed on the widget — the same event path left-drag/right-click already use.
class MidPanFilter : public QObject {
public:
	Scene *s = nullptr;
	bool   down = false, moved = false;
	double lastX = 0, lastY = 0, pressX = 0, pressY = 0;
	explicit MidPanFilter(Scene *sc, QObject *parent) : QObject(parent), s(sc) {}
protected:
	// VTK display coords are bottom-up device pixels; Qt gives top-down logical pixels.
	void devPos(QMouseEvent *me, double &dx, double &dy) {
		const double r = s->widget->devicePixelRatioF();
		const int    H = s->widget->renderWindow()->GetSize()[1];
		dx = me->position().x() * r;
		dy = H - me->position().y() * r;
	}
	void panTo(double ox, double oy, double nx, double ny) {
		vtkRenderer *ren = s->ren; vtkCamera *cam = ren->GetActiveCamera(); if (!cam) return;
		double fp[3]; cam->GetFocalPoint(fp);
		ren->SetWorldPoint(fp[0], fp[1], fp[2], 1.0); ren->WorldToDisplay();
		const double depth = ren->GetDisplayPoint()[2];
		ren->SetDisplayPoint(nx, ny, depth); ren->DisplayToWorld();
		double np[4]; for (int i=0;i<4;++i) np[i]=ren->GetWorldPoint()[i];
		ren->SetDisplayPoint(ox, oy, depth); ren->DisplayToWorld();
		double op[4]; for (int i=0;i<4;++i) op[i]=ren->GetWorldPoint()[i];
		if (np[3]!=0.0){ np[0]/=np[3]; np[1]/=np[3]; np[2]/=np[3]; }
		if (op[3]!=0.0){ op[0]/=op[3]; op[1]/=op[3]; op[2]/=op[3]; }
		const double m[3] = { op[0]-np[0], op[1]-np[1], op[2]-np[2] };
		double pos[3]; cam->GetPosition(pos);
		cam->SetFocalPoint(fp[0]+m[0], fp[1]+m[1], fp[2]+m[2]);
		cam->SetPosition (pos[0]+m[0], pos[1]+m[1], pos[2]+m[2]);
		ren->ResetCameraClippingRange();
		s->widget->renderWindow()->Render();
	}
	void recenter(double x, double y) {
		vtkRenderer *ren = s->ren; vtkCamera *cam = ren->GetActiveCamera(); if (!cam) return;
		vtkNew<vtkCellPicker> pk; pk->SetTolerance(0.0005);
		pk->PickFromListOn(); pk->AddPickList(surfProp(s));
		if (pk->Pick(x, y, 0.0, ren)) {
			double pick[3]; pk->GetPickPosition(pick);
			double pos[3], fp[3]; cam->GetPosition(pos); cam->GetFocalPoint(fp);
			const double d[3] = { pos[0]-fp[0], pos[1]-fp[1], pos[2]-fp[2] };
			cam->SetFocalPoint(pick);
			cam->SetPosition(pick[0]+d[0], pick[1]+d[1], pick[2]+d[2]);
			ren->ResetCameraClippingRange();
			s->widget->renderWindow()->Render();
		}
	}
	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (!s || !s->ren) return QObject::eventFilter(obj, ev);
		const QEvent::Type t = ev->type();
		if (t == QEvent::MouseButtonPress) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (me->button() == Qt::MiddleButton) {
				fprintf(stderr, "[mid] PRESS (qt filter)\n"); fflush(stderr);
				down = true; moved = false;
				devPos(me, pressX, pressY); lastX = pressX; lastY = pressY;
				return true;
			}
		}
		else if (t == QEvent::MouseMove && down) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			double x, y; devPos(me, x, y);
			if (std::abs(x - pressX) > 2 || std::abs(y - pressY) > 2) moved = true;
			panTo(lastX, lastY, x, y);
			lastX = x; lastY = y;
			return true;
		}
		else if (t == QEvent::MouseButtonRelease && down) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (me->button() == Qt::MiddleButton) {
				if (!moved) {
					double x, y; devPos(me, x, y);
					// A plain middle-click (no drag) ON a symbol opens ITS properties — the only
					// trigger for symbolLayerMenu now (left-click stays free for double-click-drag,
					// see 20_gizmo.cpp; right-click still opens it too, via the generic context menu).
					if (vtkActor *sym = pickSymbolAt(s, (int)x, (int)y))
						symbolLayerMenu(s, sym, s->widget->mapToGlobal(me->position().toPoint()));
					else
						recenter(x, y);
				}
				down = false;
				return true;
			}
		}
		return QObject::eventFilter(obj, ev);
	}
};

// Accept dropped files on a window: on a URL drop, hand each LOCAL file path + THIS window's
// A modal "busy" dialog with an indeterminate bar, for an operation that blocks the UI thread.
// Lives HERE, not in 70_window.cpp, because the file-open path below has to raise it before it hands
// control to Julia — and 30_app.cpp comes first in the translation unit.
static void showBusyDialog(const char *title) {
	ensureApp();                          // the FIRST open happens before any window, hence before any
	if (!QApplication::instance()) return; // QApplication: create one rather than skip the dialog
	if (g_progress) delete g_progress;
	g_progress = new QProgressDialog();
	g_progress->setWindowTitle(title);
	g_progress->setRange(0, 0);
	g_progress->setCancelButton(nullptr);
	g_progress->setWindowModality(Qt::ApplicationModal);
	g_progress->show();
	g_progress->raise();
	QApplication::processEvents();        // paint it NOW: what follows never returns to the event loop
}
static void closeBusyDialog() {
	if (g_progress) { g_progress->close(); delete g_progress; g_progress = nullptr; }
	QApplication::processEvents();
}

// EVERY route that opens a data file goes through here: drag-and-drop, File > Open, Recent Files,
// the desktop-icon launch. The point is WHERE the dialog is raised — on this side of the call, before
// Julia is entered. Julia cannot do it for itself on the first open: its own `_load_dialog_begin` sits
// inside a method that Julia is still COMPILING, so the dialog only appeared once compilation had
// finished, i.e. at the same moment as the grid. Here it is on screen before any of that starts.
static void juliaOpenFile(Scene *s, const char *path) {
	if (!g_juliaDrop) return;
	const QString base = QFileInfo(QString::fromUtf8(path)).fileName();
	showBusyDialog(base.isEmpty() ? "Opening…" : QString("Opening %1…").arg(base).toUtf8().constData());
	g_juliaDrop(s, path);
	closeBusyDialog();
}

// Scene *to Julia (g_juliaDrop), which reads the file and adds it INTO this window. One filter
// per window so it knows which Scene received the drop.
struct DropFilter : QObject {
	Scene *s = nullptr;
	explicit DropFilter(Scene *sc) : s(sc) {}
protected:
	bool eventFilter(QObject *obj, QEvent *ev) override {
		const QEvent::Type t = ev->type();
		if (t == QEvent::DragEnter || t == QEvent::DragMove) {
			auto *de = static_cast<QDragMoveEvent*>(ev);
			if (de->mimeData() && de->mimeData()->hasUrls()) { de->acceptProposedAction(); return true; }
		} else if (t == QEvent::Drop) {
			auto *de = static_cast<QDropEvent*>(ev);
			if (de->mimeData() && de->mimeData()->hasUrls()) {
				for (const QUrl &u : de->mimeData()->urls()) {
					const QString f = u.toLocalFile();
					if (!f.isEmpty()) {
						const QByteArray utf8 = f.toUtf8();        // keep the buffer alive across the call
						juliaOpenFile(s, utf8.constData());        // busy dialog up before Julia is entered
					}
				}
				de->acceptProposedAction();
				return true;
			}
		}
		return QObject::eventFilter(obj, ev);
	}
};

// Wire file drag-and-drop into a window (called for EVERY viewer window). The widget must
// accept drops AND have the filter installed so QEvent::Drop is delivered + intercepted.
static void enableFileDrops(QMainWindow *win, QWidget *widget, Scene *s) {
	DropFilter *filt = new DropFilter(s);   // one per window (carries its Scene*); freed with the window
	filt->setParent(win);                   // parented to the window -> destroyed with it
	win->setAcceptDrops(true);
	widget->setAcceptDrops(true);
	win->installEventFilter(filt);
	widget->installEventFilter(filt);
}

// ============================================================================================
// "Open full manual page" — a small green disk with a white question mark, sitting in the LOWER-LEFT
// corner of a module dialog's button row. STANDING RULE: every module dialog carries one. ONE helper
// for all of them; the icon is painted here, not shipped as an asset, so it scales to whatever size
// a caller asks for and can never go missing from an install.
//
// Clicking it hands the MODULE NAME to Julia (g_juliaOpenManual -> _on_open_manual, src/manual.jl),
// which opens that module's page under https://www.generic-mapping-tools.org/GMTjl_doc/.
// Lives in this fragment (not 70_window.cpp) so the dialogs defined EARLIER there can use it too.
// ============================================================================================
static QIcon makeHelpDiskIcon(int px) {
	QPixmap pm(px, px);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(46, 160, 67));                     // green disk
	p.drawEllipse(0, 0, px - 1, px - 1);
	QFont fnt = p.font();
	fnt.setBold(true);
	fnt.setPixelSize(int(px * 0.72));
	p.setFont(fnt);
	p.setPen(Qt::white);                                 // white question mark
	p.drawText(QRect(0, 0, px, px), Qt::AlignCenter, "?");
	p.end();
	return QIcon(pm);
}

// Insert the button at the FRONT of `row` — the dialogs' bottom row starts with a stretch, so index 0
// puts it hard against the lower-left corner, opposite Compute/Close. This overload is for dialogs
// built in C++ (their button row has no object name to look up).
static void addManualButton(QDialog *dlg, QBoxLayout *row, const QString &moduleName) {
	if (!dlg || !row) return;
	const int px = 18;
	auto *btn = new QToolButton(dlg);
	btn->setIcon(makeHelpDiskIcon(px));
	btn->setIconSize(QSize(px, px));
	btn->setAutoRaise(true);                             // no frame: the disk IS the button
	btn->setCursor(Qt::PointingHandCursor);
	btn->setToolTip("Open full manual page");
	btn->setFocusPolicy(Qt::NoFocus);                    // never steals Enter/Tab from the real controls
	QObject::connect(btn, &QToolButton::clicked, dlg, [dlg, moduleName]() {
		if (!g_juliaOpenManual) {
			QMessageBox::warning(dlg, "Manual", "Manual: callback not registered (rebuild/restart needed?).");
			return;
		}
		if (!g_juliaOpenManual(moduleName.toUtf8().constData()))
			QMessageBox::warning(dlg, "Manual",
				QString("Could not open the manual page for %1.").arg(moduleName));
	});
	row->insertWidget(0, btn);
}

// Same thing for a dialog loaded from a .ui, where the bottom row is the one named
// "horizontalLayout_buttons" by convention.
static void addManualButton(QDialog *dlg, const QString &moduleName) {
	if (!dlg) return;
	addManualButton(dlg, dlg->findChild<QHBoxLayout *>("horizontalLayout_buttons"), moduleName);
}

// STANDING RULE: wherever a dialog shows a Region (xmin/xmax/ymin/ymax), an "OR Ref grid" row goes
// DIRECTLY BELOW it — pick a grid and its own region fills the boxes, exactly as the grdsample
// dialog's GeoGridGeometry does. Same helper for every such group so the row always looks and
// behaves the same; `regionGrid` must be a layout holding ONLY the region, so appending lands the
// row immediately under it. Pass the increment boxes too when the dialog has them: a reference grid
// then supplies its spacing as well (the meta string carries w/e/s/n/dx/dy/nx/ny).
//
// The grid is read ONLY by the "..." button (or a double-click in the box, which clicks it) — never
// from an edit box's own signal: see only-action-button-executes-dialog.
static void addRefGridRow(QDialog *dlg, QGridLayout *regionGrid,
                          QLineEdit *xmin, QLineEdit *xmax, QLineEdit *ymin, QLineEdit *ymax,
                          QLineEdit *xinc = nullptr, QLineEdit *yinc = nullptr) {
	if (!dlg || !regionGrid || !xmin || !xmax || !ymin || !ymax) return;
	const int row = regionGrid->rowCount();
	auto *lbl = new QLabel("OR Ref grid", dlg);
	auto *edit = new QLineEdit(dlg);
	edit->setToolTip("Pick a grid/image; its own region fills the boxes above.");
	auto *btn = new QToolButton(dlg);
	btn->setText("...");
	regionGrid->addWidget(lbl,  row, 0);
	regionGrid->addWidget(edit, row, 1, 1, 3);
	regionGrid->addWidget(btn,  row, 4);
	QObject::connect(btn, &QToolButton::clicked, dlg, [dlg, edit, xmin, xmax, ymin, ymax, xinc, yinc]() {
		QString f = QFileDialog::getOpenFileName(dlg, "Select reference grid", prefStartDir(),
		                                         "Grid/Image files (*.nc *.grd *.tif *.tiff);;All files (*)");
		if (f.isEmpty()) return;
		rememberStartDir(f);
		edit->setText(f);
		if (!g_juliaGridMeta) return;
		const char *m = g_juliaGridMeta(f.toUtf8().constData());
		if (!m) return;
		const QStringList meta = QString::fromUtf8(m).split('/');   // copy at once (Julia-owned buffer)
		if (meta.size() < 4) return;
		xmin->setText(meta[0]);  xmax->setText(meta[1]);
		ymin->setText(meta[2]);  ymax->setText(meta[3]);
		if (meta.size() >= 6) {
			if (xinc) xinc->setText(meta[4]);
			if (yinc) yinc->setText(meta[5]);
		}
	});
	fileBoxDoubleClick(edit, btn);
}

// Procedural HDR environment for image-based lighting. A flat azimuthal gradient
// looks dull (no directional light), so this bakes a studio-ish equirectangular
// sky: cool bright zenith -> warm horizon -> darker ground, PLUS a soft bright
// "sun" disk that gives PBR its directional specular pop. Values >1 are HDR.
