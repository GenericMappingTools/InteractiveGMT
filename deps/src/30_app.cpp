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
// A path in a temp/scratch directory has no business being REMEMBERED — not in Recent Files, not as
// the default directory. It is gone (or meaningless) by the next session, and it evicts real entries
// from the MRU it lands in. Anything under the OS temp dir, %TMP%/%TEMP%, or a path segment named
// tmp / temp / scratchpad (C:\TMP\…, /tmp/…, a session scratchpad) counts. ONE test, used by every
// place that persists a path — `prefPushDir` here and `addRecentFile`/`loadRecent` (70_window.cpp).
static bool isTransientPath(const QString &p) {
	const QString n = QDir::fromNativeSeparators(QFileInfo(p).absoluteFilePath()).toLower();
	auto under = [&n](QString dir) {
		if (dir.isEmpty()) return false;
		dir = QDir::fromNativeSeparators(QDir::cleanPath(dir)).toLower();
		return !dir.isEmpty() && n.startsWith(dir + '/');
	};
	if (under(QDir::tempPath()) || under(qEnvironmentVariable("TMP")) || under(qEnvironmentVariable("TEMP")))
		return true;
	for (const QString &seg : n.split('/'))
		if (seg == "tmp" || seg == "temp" || seg == "scratchpad") return true;
	return false;
}

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
	if (dir.isEmpty() || isTransientPath(dir)) return;   // a scratch dir must never become the default
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

// Ctrl+V clipboard paste — the paste twin of the file drop above. What lands on the clipboard is
// handed to Julia (g_juliaPaste -> paste.jl `_on_paste`), which routes it through the SAME builders
// a dropped file goes through (SACRED_LAW.md: one operation, one function): an image becomes a
// GMTimage fed to `_drop_into`, a numeric table becomes a line/polygon overlay (or X,Y series).
// Exactly one of `text` / `rgb` carries the payload; `rgb` is w*h*nbands row-major bytes, top row
// first, valid only for the duration of the call. Set via gmtvtk_set_paste_callback.
typedef void (*JuliaPasteFn)(void *scene, const char *text, const unsigned char *rgb,
                             int w, int h, int nbands);
static JuliaPasteFn g_juliaPaste = nullptr;

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

// Ocean Color Data Browser (Geophysics menu). The dialog (OceanColorDialog, 70_window.cpp, loads
// deps/ui/oceancolor_browser.ui) hands a newline-separated "key=value" block to Julia
// (_on_oceancolor, src/oceancolor.jl), which knows the OB.DAAC catalogue and talks to the server:
//   req=latest   -- the two newest browse images that exist for inst/prod/period
//   req=step     -- the pair one composite EARLIER (dir=-1) or LATER (dir=1) than start=yyyymmdd
//   req=at       -- the pair ending at date=yyyymmdd
//   req=open     -- download the browse image url=<png url> and put it into `scene` georeferenced
//                   (the Extract button, and a double-click on a tile)
//   inst=1..4  prod=1..2  period=1..4      (1-based combo indices, order fixed by the .ui)
// The reply is not a return value: Julia pushes it back into the dialog with gmtvtk_oc_set_tile /
// gmtvtk_oc_status while the call is still on the stack, so `dlg` is always the live dialog that
// asked. Returns 1 when the request was served, 0 on failure. nullptr to detach.
typedef int (*JuliaOceanColorFn)(void *scene, void *dlg, const char *params);
static JuliaOceanColorFn g_juliaOceanColor = nullptr;

// LIDAR2011 PT (Tools menu, port of Mirone's cartas_militares.m in its "nikles" = LIDAR mosaic mode).
// The picker paints data/PTimg_lidar.jpg (mainland Portugal in the ETRS89/PT-TM06-ish metric frame the
// survey uses) under the survey's 1600x1000 m tile matrix, and hands "op;..." requests to Julia
// (g_juliaLidar): op "init" asks for the tile table (read with gmtread from data/lidarPT.dat and pushed
// back via gmtvtk_lidar_set_tiles), op "go" builds the mosaic of the selected cells' bounding box.
// `dlg` is the picker (LidarPicker*) so Julia can call back into it. g_lidarImg is the background
// image's path, pushed from Julia at __init__ via gmtvtk_set_lidar_image.
typedef void (*JuliaLidarFn)(void *scene, void *dlg, const char *params);
static JuliaLidarFn g_juliaLidar = nullptr;
static QString      g_lidarImg;

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

// Color Palettes (Image > Color Palettes, port of Mirone's color_palettes.m). ONE callback serves
// the whole dialog; `req` says what is wanted and the two buffers carry the payload both ways:
//   "list;<ML|GMT|CET|CAR|GIMP|T>"        -> txt = the family's palette names, one per line
//   "pal;<fam>;<name>"                    -> rgb = the palette rows (0..1), txt = "zlo zhi hinge log"
//   "readcpt;<master|zlevels>;<path>"     -> same as "pal", from a .cpt file on disk
//   "savecpt;<mode>;<path>;<zmin>;<zmax>" -> CONSUMES the rgb rows handed in, writes the file
//   "cie76;<w1w2w3>"                      -> CONSUMES the rgb rows, opens the ΔE* X,Y plot
// `nrows` is the rgb buffer's CAPACITY for the producing requests and the ROW COUNT present for the
// consuming ones. Returns the number of rows written (>=0), or a NEGATIVE count of error bytes left
// in `txt`. Julia side: src/palettes.jl `_on_palette`.
typedef int (*JuliaPaletteFn)(void *scene, const char *req, double *rgb, int nrows,
                              char *txt, int txtCap);
static JuliaPaletteFn g_juliaPalette = nullptr;

// Load Bands (Image > Load Bands, port of Mirone's bands_list.m). One callback, `req` says what:
//   "probe"           -> txt = "<n>\n<name1>\n<name2>…" for the multiband file this window came
//                        from; returns n, or 0 when the window has no such file (the menu entry
//                        greys itself out on that answer)
//   "gray;<k>"        -> load band k into the window (a Byte band as a picture, anything else as a
//                        grid — Mirone's own rule)
//   "rgb;<r>;<g>;<b>" -> load the three bands as one RGB picture
//   "pca"             -> compute the principal components and make THEM the band list
// Negative return = error, |ret| bytes of message in txt. Julia side: src/bandslist.jl `_on_bands`.
typedef int (*JuliaBandsFn)(void *scene, const char *req, char *txt, int txtCap);
static JuliaBandsFn g_juliaBands = nullptr;

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

// The FFT tool (Mag/Grav > FFT tool, Image > FFT Spectrum, Grid Tools > Spectrum). One request
// string does every operation: "op;grid1;grid2;newRows;newCols;coords;detrend;value" -- see
// _on_fftstuff (src/fftstuff.jl) for what each field means. Returns 1 on success, 0 on failure.
// `out`/`cap` carry a text answer back: the "size" request replies "<rows> <cols>" — the size of
// the raster the window is REALLY showing, which the host cannot know for an image (its scene
// grid layer is the flat placeholder the texture rides on, not the picture's pixel count).
typedef int (*JuliaFFTStuffFn)(void *scene, const char *params, char *out, int cap);
static JuliaFFTStuffFn g_juliaFFTStuff = nullptr;

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

// Illumination / Hillshade (View menu) — the port of Mirone's shading_params.m: pick a GMT
// illumination model and its light vector, get back a per-node REFLECTANCE which Julia pushes down
// with gmtvtk_set_shade_intensity_h (src/hillshade.jl). The dialog (HillshadeDialog, 70_window.cpp)
// hands a NEWLINE-separated "key=value" block: model=1..7 (1 grdgradient classic, 2 grdgradient
// Lambertian, 3 Lambertian with lighting, 4 ESRI hillshade, 5 false colour, 6 dynamic range
// compression, 7 remove), azim=, elev=, ambient=/diffuse=/specular=/shine= (model 3),
// azimR=/azimG=/azimB= + oldalgo=0|1 + amp= (model 5), wavelength= (model 6). Returns 1 on success,
// 0 on failure. nullptr to detach.
typedef int (*JuliaHillshadeFn)(void *scene, const char *params);
static JuliaHillshadeFn g_juliaHillshade = nullptr;

// JIT WARM-UP (src/warmup.jl). Julia compiles a tool's code the first time it RUNS, so the first
// press of a dialog's action button pays several seconds that have nothing to do with the maths.
// The fix is to start that compilation when the DIALOG OPENS, in a background task: the seconds the
// user spends picking a model and a light direction are seconds the compiler is already working.
// warmupTool() is the one call a menu action makes; everything else (which functions, and whether a
// second thread is available) is decided in Julia. It returns IMMEDIATELY — it only spawns a task —
// so it is safe to call straight from a menu handler, and it is a no-op when Julia has not
// registered a warm-up for that tool name, or has already run it once this session.
typedef void (*JuliaWarmupFn)(const char *tool);
static JuliaWarmupFn g_juliaWarmup = nullptr;
static void warmupTool(const char *tool) { if (g_juliaWarmup) g_juliaWarmup(tool); }

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

// Binarize (Image menu), port of Mirone's src_figs/thresholdit.m. The dialog (BinarizeDialog,
// 70_window.cpp, loads deps/ui/binarize.ui) hands "op;arg;arg" to Julia (_on_binarize,
// src/binarize.jl), which owns the grey image, the current mask and its one-level undo:
//   init                       grab this window's image, push back the histogram + first mask
//   method;<name>;<revert>     otsu|maxent|mince|isodata|triangle -> level, mask (level pushed back)
//   level;<level>;<revert>     single-line threshold (the histogram line was dragged)
//   window;<lo>;<hi>;<revert>  in-window band threshold (the histogram box was dragged/resized)
//   revert;<0|1>               complement the CURRENT mask (Mirone check_revert_CB)
//   dust;<size>   fill   label;<n>   undo
//   ok;<applyOrig>;<useAlpha>  commit: new mask element, or mask the source image (alpha or bg fill)
// `dlg` is the dialog (BinarizeDialog*) so Julia can push the histogram/preview back into it
// (gmtvtk_binarize_set_histogram / gmtvtk_binarize_set_preview), same shape as g_juliaLidar.
// Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaBinarizeFn)(void *scene, void *dlg, const char *params);
static JuliaBinarizeFn g_juliaBinarize = nullptr;

// "Image -> Show Histogram" (port of Mirone's src_figs/image_histo.m). The dialog (ImageHistoDialog,
// 70_window.cpp, deps/ui/image_histo.ui) extracts the pixels the window is DISPLAYING — the only
// part that must happen in C++, since a grid's rendered colours live in VTK — and hands them here.
// Julia counts them with `GMT.histogray`, the SAME single histogram function the Binarize dialog
// uses (SACRED_LAW: one quantity, one function — no C++ re-implementation), and pushes the 256 bins
// per band straight back with gmtvtk_histo_set_counts.
//   `px`   pixel-interleaved bytes, `nb` components each, `npix` pixels
//   `dlg`  the ImageHistoDialog* to push the counts into
// Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaImageHistoFn)(void *scene, void *dlg, const unsigned char *px, int npix, int nb);
static JuliaImageHistoFn g_juliaImageHisto = nullptr;

// "Image -> Image Enhance -> 1 - Indexed and RGB" (port of Mirone src_figs/image_enhance.m). The
// dialog (ImageEnhanceDialog, 70_window.cpp, deps/ui/image_enhance.ui) owns the contrast WINDOW (a
// display state) and sends every PIXEL operation here as "op;args":
//   init;<name>                     load the image, histogram each band, hand back its data range
//   stretchlim;<band>;<pct>         the limits that clip <pct>% of outliers (localStretchlim)
//   contrast;lo;hi;lo;hi;lo;hi      imadjust with each band's window -> a new stretched image
//   decorr;<pct>                    decorrelation stretch -> a new image
//   scatter                         the 3-band scatter plot (push_scaterPlot_CB)
//   close                           the dialog went away; drop its state
// `dlg` is the ImageEnhanceDialog* so Julia can push the histograms (gmtvtk_enhance_set_band) and
// answered limits (gmtvtk_enhance_set_window) back into it. Returns 1/0. nullptr to detach.
typedef int (*JuliaImageEnhanceFn)(void *scene, void *dlg, const char *params);
static JuliaImageEnhanceFn g_juliaImageEnhance = nullptr;

// "Image > Image resize" (port of Mirone src_figs/imageresize.m). params = "op;args":
//   "init;<name>"                    -> Julia answers with gmtvtk_resize_set_size(dlg, w, h)
//   "resize;<name>;<w>;<h>;<method>" -> gdalwarp -ts w h -r <method> into a new image row
// method is a gdalwarp -r name (near|bilinear|cubic|average), so the four entries of Mirone's
// popup_resampMethod arrive as what GDAL is going to be asked for, decided in ONE place (the dialog).
typedef int (*JuliaImageResizeFn)(void *scene, void *dlg, const char *params);
static JuliaImageResizeFn g_juliaImageResize = nullptr;

// "Image > Shape detector" (port of Mirone src_figs/floodfill.m — the magic wand). params =
//   "<op>;<name>;<tol>;<conn>;<dilate>;<mahal>;<mode>;<minpts>;<bg>;x1,y1[;x2,y2…]"
// op = "seed" (one point) | "multi"; mode = 0 digitize / 1 colour segmentation / 2 mask. The seeds
// are WORLD coordinates: the viewer picks the point, Julia (src/floodfill.jl) owns every pixel.
typedef int (*JuliaFloodFillFn)(void *scene, const char *params);
static JuliaFloodFillFn g_juliaFloodFill = nullptr;

// "Image > K-means classification" (port of Mirone src_figs/classificationfig.m). params = "op;args":
//   "compute;<name>;<supervised>;<nClasses>;<nNeighbors>;x1,y1[;x2,y2…]"
//        supervised = 1: the seeds ARE the initial centres (click-define); their colour is the mean
//        over an nNeighbors-wide window. supervised = 0: nClasses random centres. Julia answers with
//        gmtvtk_classify_set_classes(dlg, k) so the listbox can offer the classes it found.
//   "isolate;<name>;<asMask>;<r>,<g>,<b>;<c1>[,<c2>…]"      push_getClass_CB
// `dlg` is the ClassificationDialog* for that answer. Returns 1/0. nullptr to detach.
typedef int (*JuliaClassifyFn)(void *scene, void *dlg, const char *params);
static JuliaClassifyFn g_juliaClassify = nullptr;

// The Adjust Contrast dialog's ScaterPlot (push_scaterPlot_CB). It plots the bands the user is
// LOOKING AT — after a Decorrelation Stretch that is the decorrelated image — so the pixels come
// from sceneDisplayedRGB, the SAME "what is on screen" reader "Image -> Show Histogram" uses, and
// NOT from anything the dialog remembered. `px` is pixel-interleaved, `nb` components per pixel.
typedef int (*JuliaRgbScatterFn)(void *scene, const unsigned char *px, int npix, int nb,
                                 const char *label);
static JuliaRgbScatterFn g_juliaRgbScatter = nullptr;

// Empilhador (Tools), port of Mirone's src_figs/empilhador.m. The dialog (EmpilhadorDialog,
// 70_window.cpp, loads deps/ui/empilhador.ui) hands a newline-separated "key=value" block to Julia
// (_on_empilhador, src/empilhador.jl), which calls GMT.jl's `empilhador`:
//   list=<the list file, or a wildcard request>      | file<i>=<path>   (files picked one by one)
//   out=<output file>            fmt=netcdf|vtk|tiff|vrt
//   region=<w/e/s/n>             (absent when "Use sub-region?" is off)
//   l2=0|1   config=0|1   quality=<-2..2>   bitflags=0|1   inc=<cell size>   ncells=<n>
// Returns 1 on success, 0 on failure -- same yes/no contract as g_juliaClipGrid, and for the same
// reason: the dialog has to report it, the parent's Errors console may be on another window.
// nullptr to detach.
typedef int (*JuliaEmpilhadorFn)(void *scene, const char *params);
static JuliaEmpilhadorFn g_juliaEmpilhador = nullptr;

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

// grdfft (GMT menu), the 2-D FFT of the window's grid. GrdFFTDialog (70_window.cpp, loads
// deps/ui/grdfft_dialog.ui) hands a newline-separated "key=value" block to Julia (_on_grdfft,
// src/grdfft.jl). `mode` is the outcome the dialog's two radios pick:
//   grid      azim (-A), upward (-C), dfdz + dfdz_val (-D), integrate + integrate_val (-I),
//             noop (-Q), scale (-S) and filter (the whole -F string, shape + wavelengths)
//   spectrum  espec (the whole -E string) and an optional grid2 -> the 17-column cross-spectrum
// plus the shared -N block (fftdim, detrend, extend, taper, fftverbose), mgal45 (-M), geog (-fg),
// outfile and grid (the DISPLAYED layer's label). A grid result is added to `scene` as a NEW derived
// grid; a spectrum goes to the Data Viewer and the X,Y plot tool. Returns 1 on success, 0 on
// failure. nullptr to detach.
typedef int (*JuliaGrdFFTFn)(void *scene, const char *params);
static JuliaGrdFFTFn g_juliaGrdFFT = nullptr;

// trend2d (GMT menu), the TABLE twin of grdtrend: fit a [weighted] [robust] polynomial z = f(x,y)
// to scattered x,y,z points. Trend2DDialog (70_window.cpp, loads deps/ui/trend2d_dialog.ui) hands a
// newline-separated "key=value" block to Julia (_on_trend2d, src/trend2d.jl): infile, headers and
// toggle (the file's own reading, as in Interpolate), model + robust (-N n[+r]), iterate +
// confidence (-I), condition (-C), weights (-W, "plain"|"+s"|"+w"), then either params=1 (-Fp, the
// model parameters) or one col_<letter>=1 per ticked output column (-Fxyzmrw) with plotpts, plus
// outfile. The result goes to the window's Data Viewer (and optionally onto the map as points).
// Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaTrend2DFn)(void *scene, const char *params);
static JuliaTrend2DFn g_juliaTrend2D = nullptr;

// Make CPT (GMT menu), ONE dialog over makecpt AND grd2cpt. CptBuildDialog (70_window.cpp, loads
// deps/ui/cpt_build_dialog.ui) hands a newline-separated "key=value" block to Julia (_on_cptbuild,
// src/cptbuild.jl): mode=make|grd (where the z range comes from — the boxes, or the window's grid),
// master (a GMT master name or a .cpt path), then either tmin/tmax/tinc/tmod (-T) or
// nlevels/cdf/lmin/lmax/symmetric (-E, -L, -S), plus the options both modules share — continuous
// (-Z), log (-Q), invert (-I), colormodel (-F), glo/ghi (-G), alpha + alphaall (-A), bfn (-D/-M/-N),
// categorical + wrap (-W) — and finally what to DO with the palette: apply (recolour the layer named
// by grid, addressed by gridsel over zmin..zmax) and/or outfile. Returns 1 on success, 0 on failure.
// nullptr to detach.
typedef int (*JuliaCptBuildFn)(void *scene, const char *params);
static JuliaCptBuildFn g_juliaCptBuild = nullptr;

// grdhisteq (GMT menu), histogram equalization of the window's grid. GrdHistEqDialog
// (70_window.cpp, loads deps/ui/grdhisteq_dialog.ui) hands a newline-separated "key=value" block to
// Julia (_on_grdhisteq, src/grdhisteq.jl): mode=grid|table (the module's own two outputs, one of
// which it requires), flavour=cells|gaussian (equal-area cell indices with ncells + quadratic, or
// normal scores with an optional norm — which GMT refuses to mix with the other two), region, and
// grid (the DISPLAYED layer's label), plus outfile. A grid result is added to `scene` as a NEW
// derived grid; the level table goes to the Data Viewer. Returns 1 on success, 0 on failure.
// nullptr to detach.
typedef int (*JuliaGrdHistEqFn)(void *scene, const char *params);
static JuliaGrdHistEqFn g_juliaGrdHistEq = nullptr;

// xyz2grd (GMT menu), a TABLE whose points already sit on the nodes -> a grid. Xyz2GrdDialog
// (70_window.cpp, loads deps/ui/xyz2grd_dialog.ui) hands a newline-separated "key=value" block to
// Julia (_on_xyz2grd, src/xyz2grd.jl): infile plus how to READ it (headers, incols, toggle, and the
// one-column zflags = the module's -Z), the geometry it is sampled on (region, inc, pixel, geog),
// amode (-A: what to do when several records land on one node) and the grid header fields
// (dxname/dyname/dzname/dtitle/dremark -> -D), plus outfile. The new grid is added to `scene` as a
// derived grid. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaXyz2GrdFn)(void *scene, const char *params);
static JuliaXyz2GrdFn g_juliaXyz2Grd = nullptr;

// grdfill (GMT menu), filling the holes in the window's grid. GrdFillDialog (70_window.cpp, loads
// deps/ui/grdfill_dialog.ui) hands a newline-separated "key=value" block to Julia (_on_grdfill,
// src/grdfill.jl): mode=fill|list (the module's own two outcomes), and for a fill the algorithm
// (algo=c|n|s|g with its own value/radius/tension/gridfile -> -A), or for a list polygons + draw
// (-L / -Lp, the polygons optionally drawn on the map as their own line layer), plus nodata (-N:
// what counts as a hole, NaN by default), region, outfile and grid (the DISPLAYED layer's label).
// A filled grid is added to `scene` as a NEW derived grid; a hole list goes to the Data Viewer.
// Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGrdFillFn)(void *scene, const char *params);
static JuliaGrdFillFn g_juliaGrdFill = nullptr;

// gravfft (GMT menu), the spectral potential-field tool. GravFFTDialog (70_window.cpp, loads
// deps/ui/gravfft_dialog.ui) hands a newline-separated "key=value" block to Julia (_on_gravfft,
// src/gravfft.jl). The tab in front is `mode`:
//   surface  density (a value or a grid) -> the geopotential of the window's surface
//   flexure  te/rhol/rhom/rhow/rhoi + zm/zl -> the isostatic response; moho (+m), flextopo (Q) and
//            subplate (S) pick WHICH result
//   admitt   grid2 (the gravity|geoid grid) + iflags (the -I letters) -> the admittance|coherence
//   theo     cn/clambda/cdepth/cmodel/cwave -> the theoretical curve alone, no grid read at all
// plus the shared field (the -F argument), terms, level, geog, the -N block (fftdim, detrend,
// extend, taper, fftverbose), outfile and grid (the DISPLAYED layer's label). A grid result is added
// to `scene` as a NEW derived grid; a spectrum goes to the Data Viewer and the X,Y plot tool.
// Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGravFFTFn)(void *scene, const char *params);
static JuliaGravFFTFn g_juliaGravFFT = nullptr;

// grdrotater (GMT menu), reconstructing the window's geographic grid by an Euler rotation (the
// spotter supplement). GrdRotaterDialog (70_window.cpp, loads deps/ui/grdrotater_dialog.ui) hands a
// newline-separated "key=value" block to Julia (_on_grdrotater, src/grdrotater.jl):
// emode=pole|file|plates with elon/elat/eangle, efile or eplates (the three forms of -E) plus
// invert (+i); polyfile (-F, rotate only what is inside it), time (-T, one reconstruction time),
// outlineonly (-S), outline (-D) and drawoutline (put that outline on the map), region (-A),
// outfile, outoutline and grid (the DISPLAYED layer's label). The rotated grid is added to `scene`
// as a NEW derived grid. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGrdRotaterFn)(void *scene, const char *params);
static JuliaGrdRotaterFn g_juliaGrdRotater = nullptr;

// talwani2d (GMT menu), geopotential anomalies over 2-D bodies given as cross-section polygons.
// Talwani2DDialog (70_window.cpp, loads deps/ui/talwani2d_dialog.ui) hands a newline-separated
// "key=value" block to Julia (_on_talwani2d, src/talwani2d.jl): infile (the model), field=f|n|v with
// lat (-F), density (-D), zup (-A), hkm/vkm (-M), mode=lattice|track with tmin/tmax/tinc/tnum (-T)
// or trackfile (-N), level plus y25min/y25max (-Z, the last two being the 2.5-D strike extent),
// outfile and plot. The modelled profile goes to the Data Viewer and, when asked, to the X,Y plot
// tool. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaTalwani2DFn)(void *scene, const char *params);
static JuliaTalwani2DFn g_juliaTalwani2D = nullptr;

// talwani3d (GMT menu), geopotential anomalies over 3-D bodies given as stacked horizontal
// contours. Talwani3DDialog (70_window.cpp, loads deps/ui/talwani3d_dialog.ui) hands a
// newline-separated "key=value" block to Julia (_on_talwani3d, src/talwani3d.jl): infile (the
// model), field=f|n|v with lat (-F), density (-D), zup (-A), hkm/vkm (-M), geog (-fg),
// mode=grid|track|obsgrid with region/inc/pixel (-R -I -r), trackfile (-N) and plotpts, or zgrid
// (-Z<grid>), level (-Z<constant>) and outfile. A grid run adds a NEW derived grid to `scene`; a
// track run fills the Data Viewer. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaTalwani3DFn)(void *scene, const char *params);
static JuliaTalwani3DFn g_juliaTalwani3D = nullptr;

// greenspline (GMT menu), gridding (or evaluation) of scattered data with the Green's function of
// one of six splines, in 1, 2 or 3 dimensions. GreensplineDialog (70_window.cpp, loads
// deps/ui/greenspline_dialog.ui) hands a newline-separated "key=value" block to Julia
// (_on_greenspline, src/greenspline.jl): infile with headers and toggle (the table row every dialog
// of this family shares); dmode (-D), spline and tension (-S); what=grid|nodes with nodefile (-N),
// or region/inc/pixel (-R -I -r) or maskgrid (-T); approx with ckind/cvalue/cfile (-C); gradfile and
// gradformat (-A); deriv with derivdir (-Q); misfit with misfitfile and reportfile (-E); notrend and
// norestore (-L); uncert and isweight (-W); verbose; plotpts and outfile. A grid run adds a NEW
// derived grid to `scene`; a nodes run fills the Data Viewer. Returns 1 on success, 0 on failure.
// nullptr to detach.
typedef int (*JuliaGreensplineFn)(void *scene, const char *params);
static JuliaGreensplineFn g_juliaGreenspline = nullptr;

// gmtflexure (GMT menu), the flexure of a 2-D (profile) plate under a load. GmtFlexureDialog
// (70_window.cpp, loads deps/ui/gmtflexure_dialog.ui) hands a newline-separated "key=value" block to
// Julia (_on_gmtflexure, src/gmtflexure.jl): qmode=t|q|n with loadfile or qmin/qmax/qinc (-Q), te
// (-E), rhom/rhol/rhoi/rhow (-D), lbc/largs and rbc/rargs (-A), poisson and young (-Cp, -Cy), force
// (-F), water (-W), zobs (-Z), wfile (-T), varrestore (-L), curvature (-S), hkm/vkm (-M), outfile
// and plot. The profile fills the Data Viewer and, when asked, the X,Y plot tool. Returns 1 on
// success, 0 on failure. nullptr to detach.
typedef int (*JuliaGmtFlexureFn)(void *scene, const char *params);
static JuliaGmtFlexureFn g_juliaGmtFlexure = nullptr;

// grdflexure (GMT menu), the 3-D twin: flexure of a surface under a topographic load, in the
// wavenumber domain. GrdFlexureDialog (70_window.cpp, loads deps/ui/grdflexure_dialog.ui) hands a
// newline-separated "key=value" block to Julia (_on_grdflexure, src/grdflexure.jl): loadgrid with
// loadkm (+uk) and rhogrid (-H); rhom/rhol/rhoi/rhow/rhoroot (-D), beta (-S), water (-W), zobs (-Z),
// geog (-fg); te and te2 (-E), nua/ha/num (-F), maxwell (-M), nx/ny/nxy (-A), poisson and young
// (-Cp, -Cy); t0/t1/dt/tlog or tfile (-T), outfile (-G), the fftdim/detrend/extend/taper/fftverbose
// set (-N, the same keys grdfft sends), and transfer (-Q). Without times the flexure grid is added
// to `scene` as a NEW derived grid; with times the module writes one grid per time and the last one
// is loaded. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGrdFlexureFn)(void *scene, const char *params);
static JuliaGrdFlexureFn g_juliaGrdFlexure = nullptr;

// grdvolume (GMT menu), the area/volume/mean height of the window's grid against contour levels.
// GrdVolumeDialog (70_window.cpp, loads deps/ui/grdvolume_dialog.ui) hands a newline-separated
// "key=value" block to Julia (_on_grdvolume, src/grdvolume.jl): cmode=all|above|range|below|between
// (the five shapes of -C) with cval or clow/chigh/cdelta, slices (-D, a range only), base (-L),
// unit (-S), tmax (-T h|c), zfact/zshift (-Z), region, plot (draw the curves when a range produced
// several rows), outfile and grid (the DISPLAYED layer's label). The result table goes to the Data
// Viewer, and to the X,Y plot tool when asked. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGrdVolumeFn)(void *scene, const char *params);
static JuliaGrdVolumeFn g_juliaGrdVolume = nullptr;

// gravprisms (GMT menu), the geopotential field of vertically oriented rectangular prisms — read
// from a table or created here to approximate a seamount or a layer. GravPrismsDialog (70_window.cpp,
// loads deps/ui/gravprisms_dialog.ui) hands a newline-separated "key=value" block to Julia
// (_on_gravprisms, src/gravprisms.jl): source=table|create with infile and dxdy (-E), or shape (-S),
// base (-L), top (-T), dz (+z), saveprisms (+w) and quit (+q); density and contrast (-D[+c]),
// radial with href/rholo/rhohi/boost/densify/power (-H) and avedens (-W); field=f|n|v with lat (-F),
// zup (-A), hkm/vkm (-M), geog (-fg); mode=grid|track|obsgrid with region/inc/pixel (-R -I -r),
// trackfile (-N) and plotpts, or zgrid (-Z<grid>); level (-Z<constant>) and outfile. A grid run adds
// a NEW derived grid to `scene`; a track run — and a +q run, whose answer IS the prisms — fills the
// Data Viewer. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaGravPrismsFn)(void *scene, const char *params);
static JuliaGravPrismsFn g_juliaGravPrisms = nullptr;

// grdvector (GMT menu), the vector field of two grids drawn over the window's map. GrdVectorDialog
// (70_window.cpp, loads deps/ui/grdvector_dialog.ui) hands a newline-separated "key=value" block to
// Julia (_on_grdvector, src/grdvector.jl): usescene with grid1, and grid2 (the two component grids,
// each a file or the name of a layer in this window); polar (-A), azimuth (-Z) and geog; incmode
// with incx/incy (-I); scalemode with scale (-S); heads with headlen/headang/norm (-Q); color, bymag
// with nclass (-C), drape, table, name; xmin/xmax/ymin/ymax (-R); outfile; and grid, the DISPLAYED
// layer's label. The arrows are added to `scene` as a line overlay (the module itself only makes
// PostScript, so the geometry is built Julia-side). Returns 1 on success, 0 on failure. nullptr to
// detach.
typedef int (*JuliaGrdVectorFn)(void *scene, const char *params);
static JuliaGrdVectorFn g_juliaGrdVector = nullptr;

// Earth regions (Tools menu), GMT.jl's `earthregions`: a named geographic region out of its
// collections, brought back as data or as boundaries. EarthRegionsDialog (70_window.cpp, loads
// deps/ui/earthregions_dialog.ui) hands a newline-separated "key=value" block to Julia
// (_on_earthregions, src/earthregions.jl): mode=list with collection (print that collection's
// codes and names to the window's message pane), or mode=raster|region with code, exact, round,
// country and name — plus, for a raster, dataset, res and registration. A raster is added to
// `scene` as a new layer (grid or image); mode=region downloads nothing and reports the W/E/S/N,
// drawing the DCW border lines when asked. Returns 1 on success, 0 on failure. nullptr to detach.
// `dlg` is the EarthRegionsDialog that asked, so a listing can be handed straight back to IT (via
// gmtvtk_earthregions_set_listing) instead of being shouted into the window at large — the same
// dialog-pointer shape the image-histogram callback uses.
typedef int (*JuliaEarthRegionsFn)(void *scene, void *dlg, const char *params);
static JuliaEarthRegionsFn g_juliaEarthRegions = nullptr;

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

// Project (Tools menu), port of Mirone's "Projections > GDAL project" (src_figs/gdal_project.m) —
// the GDAL dropped from the name, but it IS an interface to gdalwarp. ProjectDialog (70_window.cpp,
// loads deps/ui/project_dialog.ui) hands a newline-separated "key=value" block to Julia
// (_on_project, src/project.jl):
//   s_srs=<source PROJ4/WKT/EPSG, empty = let GDAL read it off the data>
//   t_srs=<target, REQUIRED>          resample=near|bilinear|cubic|cubicspline
//   rows= cols=                       (gdalwarp -ts, used only when BOTH are given)
//   xinc= yinc=                       (gdalwarp -tr; resolution takes precedence over rows/cols)
//   projname=<the combo's label, for the Scene Objects name>
//   grid=<the DISPLAYED layer's label; empty for the base surface / an image-only window>
// The warped raster is added to `scene` as a NEW derived grid (or image), and the window's CRS is
// re-stamped to the target. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaProjectFn)(void *scene, const char *params);
static JuliaProjectFn g_juliaProject = nullptr;

// Interpolation / griding (GMT menu), dialog laid out after Mirone's Surface window
// (src_figs/griding_mir.m). InterpolationDialog (70_window.cpp, loads deps/ui/interpolation_dialog.ui)
// hands a newline-separated "key=value" block to Julia (_on_interpolate, src/interpolate.jl):
// method (surface|nearneighbor|triangulate|blockmean|blockmedian|blockmode|greenspline|sphinterpolate),
// infile, headers, region, inc, pixel, toggle, coords, radius (nearneighbor's -S, unit appended),
// verbose, plotpts, outfile, plus one "opt_<kwarg>=<value>" line per row of the method's own Options
// window. The gridded result is added to `scene` as a NEW derived grid. Returns 1 on success, 0 on
// failure. nullptr to detach.
typedef int (*JuliaInterpolateFn)(void *scene, const char *params);
static JuliaInterpolateFn g_juliaInterpolate = nullptr;

// Euler rotations (Plates menu). Port of Mirone's src_figs/euler_stuff.m, re-based on GMT's own
// spotter supplement (which did not exist when the Mirone code was written): every rotation and every
// bit of pole algebra is a GMT call — backtracker for the line rotations and flow lines, rotconverter
// for adding / converting / interpolating poles, mapproject -Ng for the geodetic-latitude option.
// EulerDialog (70_window.cpp, loads deps/ui/euler_stuff.ui) hands a newline-separated "key=value"
// block to Julia (_on_euler, src/plates.jl); `op` says which of the dialog's three tabs asked:
//   op=rotate  target<i>=<Scene Objects label>, one per selected line   polesfile=<GMT rotation file>
//              ages=<comma list>  agelabels=<comma list>  revert=0|1  geodetic=0|1
//              usepole=0|1  polelon= polelat= poleang=   showcmd=0|1 (report the command, run nothing)
//   op=add     p1lon= p1lat= p1ang=  p2lon= p2lat= p2ang=
//   op=interp  polesfile=<finite poles file>  poles=<inline "lon lat ang age;…" from the catalogue>
//              ages=<comma list>  outfile=<path or empty>
//   op=headerpoles  target1=<label>   — the Euler poles that line carries in its OWN header (a Mirone
//              isochron's FIN"…"/STG0"…"), answered as catalogue lines for the Poles selector
//   op=stages  poles=<inline list>  half=0|1  inverse=0|1  side=1|-1|0  geodetic=0|1  — finite poles
//              to a stage-pole FILE (rotconverter -Fs [-M0.5] [-N|-S]); answers "<path>\n<table>"
// The SAME channel carries the Plate calculator (PlateCalcDialog, Mirone's src_figs/plate_calculator.m
// — one Plates menu, one Julia door):
//   op=plates    model=<Nuvel1A|MORVEL|PB|GEODVEL|NNR|DEOS2K|REVEL>  — answers one "AB<tab>Name" line
//              per plate of that model's poles table (data/plates/<model>_poles.dat)
//   op=platepole model= fix=<abbrev> mov=<abbrev>  — the Euler pole of `mov` relative to `fix`, as
//              "lon lat rate"; empty when the two plates are the same (no motion)
//   op=platevel  polelon= polelat= polerate= lon= lat=  — the plate velocity at that point, as
//              "speed azimuth" (mm/yr and degrees cw from N, gmtpmodeler)
// …and Compute Euler pole (ComputeEulerDialog, Mirone's src_figs/compute_euler.m), same door again:
//   op=ceuler  line1= line2=<Scene Objects label or file path>  polelon= polelat= poleang=
//              lonrange= latrange= angrange=  nlon= nlat= nang=   — the search box around the pole
//              hellinger=0|1  dptol=<km>  showstats=0|1 ellipse=0|1 forcepole=0|1 colorseg=0|1
//              plotres=0|1   — signed residues of the starting pole only, no pole computed
//              residfile=<path> residfmt=nc|vtk showcube=0|1   — the (lat, lon, angle) residues cube
//              loop=0|1      — restart from the pole just found until it stops improving
//   op=ceuler_stop   — ask the running search to stop; it reports its best so far
// The brute-force search does NOT block: op=ceuler returns as soon as the worker task is running and
// the answer arrives through gmtvtk_compute_euler_progress (90_c_api.cpp). The Hellinger and
// "plot residues only" branches are synchronous and answer through gmtvtk_euler_result as usual.
// The rotated lines land in `scene` as ONE new Scene Objects group of named lines (the source line
// stays visible — a rotation is a comparison); `add`/`interp` answer through gmtvtk_euler_result.
// Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaEulerFn)(void *scene, const char *params);
static JuliaEulerFn g_juliaEuler = nullptr;

// Vector Operations (Tools menu). Port of Mirone's src_figs/line_operations.m — a command line over
// the window's line elements, with Mirone's own command language kept verbatim. LineOpsDialog
// (70_window.cpp, loads deps/ui/line_operations.ui) hands a newline-separated "key=value" block to
// Julia (_on_lineops, src/lineops.jl):
//   cmd=<the whole command, e.g. "buffer 10K NPTS=24 SIDE=left">
//   target<i>=<Scene Objects label>, one per selected line (absent for the ops that need no pick:
//             pline, scale, GMT_DB, self-crossings, delete)
// Results land in `scene` as new named elements (the additive ops) and/or replace what they consumed
// (delete, group, line2patch, stitch) — Mirone's own placement, op by op. The tool's report comes
// back through gmtvtk_lineops_result while the callback is still running (synchronous, UI thread).
// Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaLineOpsFn)(void *scene, const char *params);
static JuliaLineOpsFn g_juliaLineOps = nullptr;
static std::string g_lineOpsResult;
// Julia's answer for the tabs that have one (the summed pole, the interpolated pole table, the GMT
// command "Show GMT command" asked for). Written from Julia through gmtvtk_euler_result while the
// callback is still running — the call is synchronous on the UI thread, so the dialog simply clears
// this before calling and reads it straight after.
static std::string g_eulerResult;

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

// Same idea for the Binarize dialog (Image menu, 70_window.cpp): closing it only HIDES it, so the
// image's Scene Objects handle offers "Binarize Image…" to bring it back with its mask and undo
// intact. imageObjectMenu (50_scene.cpp) is compiled before 70_window.cpp, so it reaches the dialog
// through these hooks, which 70 installs at load. `has` reports whether the scene owns one (unused
// for gating today — the entry also OPENS one when there is none); `reopen` shows/creates it.
// `name` is the Scene Objects name of the image to threshold ("" = the window's primary image), so
// the dialog works on the image whose handle was clicked, never on "whichever image is first".
static bool (*g_binarizeHasDialog)(Scene *scene) = nullptr;
static void (*g_binarizeReopen)(Scene *scene, const char *name) = nullptr;
// Same for the "Adjust Contrast" dialog (Image > Image Enhance > 1 - Indexed and RGB): closing it
// only HIDES it, so the image's Scene Objects handle is where it lives while "minimized" and where
// it comes back from, with its per-band contrast windows intact.
static void (*g_enhanceReopen)(Scene *scene, const char *name) = nullptr;
// "Image > Show Histogram", reachable from an image's own Scene Objects handle as well as from the
// menu bar. ONE function draws it (70_window.cpp `showImageHistogram`); this hook is only how the
// earlier fragment reaches it. `name` names the image whose handle was clicked ("" = the primary).
static void (*g_showImageHisto)(Scene *scene, const char *name) = nullptr;
// Same, for "Image > Image resize": the image's own handle opens it on THAT image.
static void (*g_showImageResize)(Scene *scene, const char *name) = nullptr;
// Same, for "Image > Shape detector".
static void (*g_showFloodFill)(Scene *scene, const char *name) = nullptr;
// Same, for "Image > K-means classification".
static void (*g_showClassification)(Scene *scene, const char *name) = nullptr;

// "Image > Flip > Up-Down / Left-Right" (Mirone mirone.m `Transfer_CB` 'flipUD'/'flipLR'). Calls
// fn(scene, "ud;<name>" | "lr;<name>") — name = the Scene Objects name of the image to flip ("" =
// the window's primary image). Julia flips THE PIXELS of that image (its stored GMTimage as well as
// the texture the viewer shows) and leaves the georeference alone: a flip re-orders pixels inside
// the same ground extent, so a referenced image keeps its coordinates exactly. Set via
// gmtvtk_set_image_flip_callback; nullptr -> a status-bar notice.
typedef void (*JuliaImageFlipFn)(void *scene, const char *req);
static JuliaImageFlipFn g_juliaImageFlip = nullptr;

// "Image > Explore RGB" (Mirone mirone.m `Transfer_CB` 'RGBexp' + utils/montage.m). fn(scene, dlg,
// params) with params = "op;args":
//   "init;<name>"      -> Julia answers with gmtvtk_rgbexp_set_thumbs(dlg, rgba, w, h, 13): the 13
//                         colour components of the image, as thumbnails, in Mirone's order
//   "pick;<name>;<k>"  -> component k (1..13) at FULL resolution, added to this window's image list
// `name` = the Scene Objects name of the image ("" = the window's primary one). Returns 1/0. Set via
// gmtvtk_set_rgbexplore_callback; nullptr -> the menu entry says so and opens nothing.
typedef int (*JuliaRgbExploreFn)(void *scene, void *dlg, const char *params);
static JuliaRgbExploreFn g_juliaRgbExplore = nullptr;

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

// A Scene Objects row was REMOVED: drop the live GMTgrid/GMTimage Julia keeps behind it
// (_SCENE_OBJS, savefile.jl). Called from sceneRemoveExtraAt / sceneRemoveSurface — the one pair of
// removal points — so a deleted row can never leave its data alive and still resolvable by a tool
// that looks an object up by kind. `kind` is "grid" / "image" / "mesh", `name` the row's label
// ("" = the window's primary object). nullptr to detach.
typedef void (*JuliaForgetFn)(void *scene, const char *kind, const char *name);
static JuliaForgetFn g_juliaForget = nullptr;

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
// The three `vtk_*` codes are the exception — GMT and GDAL cannot write VTK, so saveObjectDialog
// keeps those in-process and hands them to vtkioSaveObject (87_vtkio.cpp) instead of Julia.
static const SaveFmt kGridFmts[] = {
	{ "netCDF grid",       "nc",         "netCDF grid (*.nc *.grd)",      ".nc"  },
	{ "GeoTIFF",           "gtiff",      "GeoTIFF (*.tif *.tiff)",        ".tif" },
	{ "JPEG2000",          "jp2",        "JPEG2000 (*.jp2)",              ".jp2" },
	{ "Erdas Imagine",     "erdas",      "Erdas Imagine (*.img)",         ".img" },
	{ "Surfer 6 grid",     "surfer",     "Surfer 6 grid (*.grd)",         ".grd" },
	{ "ENVI",              "envi",       "ENVI (*.hdr)",                  ".hdr" },
	{ "VTK XML ImageData", "vtk_vti",    "VTK XML ImageData (*.vti)",     ".vti" },
	{ "VTK XML PolyData",  "vtk_vtp",    "VTK XML PolyData (*.vtp)",      ".vtp" },
	{ "VTK legacy",        "vtk_legacy", "VTK legacy (*.vtk)",            ".vtk" },
};
// Images always go through GMT.gdalwrite (driver by extension) — except the VTK entries, which
// write the layer's own texture raster as a vtkImageData (same in-process path as the grid case).
static const SaveFmt kImageFmts[] = {
	{ "GeoTIFF",           "gtiff",      "GeoTIFF (*.tif *.tiff)",        ".tif" },
	{ "JPEG2000",          "jp2",        "JPEG2000 (*.jp2)",              ".jp2" },
	{ "Erdas Imagine",     "erdas",      "Erdas Imagine (*.img)",         ".img" },
	{ "ENVI",              "envi",       "ENVI (*.hdr)",                  ".hdr" },
	{ "JPEG",              "jpg",        "JPEG (*.jpg *.jpeg)",           ".jpg" },
	{ "PNG",               "png",        "PNG (*.png)",                   ".png" },
	{ "TIFF",              "tif",        "TIFF (*.tif)",                  ".tif" },
	{ "BMP",               "bmp",        "BMP (*.bmp)",                   ".bmp" },
	{ "VTK XML ImageData", "vtk_vti",    "VTK XML ImageData (*.vti)",     ".vti" },
	{ "VTK legacy",        "vtk_legacy", "VTK legacy (*.vtk)",            ".vtk" },
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

// VTK's own formats, written in-process by 87_vtkio.cpp (GMT/GDAL cannot write them, so these never
// reach g_juliaSave). Forward-declared: 87_vtkio.cpp is #included after this fragment.
static bool vtkioIsVtkSaveCode(const QString &code);
static bool vtkioSaveObject(Scene *s, const std::string &name, const std::string &code,
                            const std::string &path, std::string &err);

// Open the Save dialog for a scene object and hand the choice to Julia (g_juliaSave) as
// "<kind>;<fmt>;<path>;<name>". kind = "grid" | "image"; name identifies which object (empty for the
// File-menu "save the window's grid/image"). nullptr callback -> a status-bar notice.
static void saveObjectDialog(Scene *s, const char *kind, const QString &name) {
	const bool isGrid = (QString(kind) == QLatin1String("grid"));
	// The VTK entries are written HERE, so the dialog must still open when Julia's save callback is
	// missing; only the GMT/GDAL formats need it. The check therefore moves below the format choice.
	SaveFormatDialog dlg(s ? s->win : nullptr, isGrid, name);
	if (dlg.exec() != QDialog::Accepted || dlg.path.isEmpty()) return;
	if (vtkioIsVtkSaveCode(dlg.code)) {
		std::string err;
		if (vtkioSaveObject(s, name.toStdString(), dlg.code.toStdString(), dlg.path.toStdString(), err)) {
			if (s && s->win) s->win->statusBar()->showMessage("Saved " + dlg.path, 4000);
		}
		else if (s && s->win) {
			QMessageBox::warning(s->win, "Save failed",
			                     "Could not write " + dlg.path + "\n\n" + QString::fromStdString(err));
		}
		return;
	}
	if (!g_juliaSave) {
		if (s && s->win) s->win->statusBar()->showMessage("Save: callback not registered", 3000);
		return;
	}
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

// Ask Julia to flip the named image up-down / left-right IN PLACE (g_juliaImageFlip, req =
// "ud;<name>" | "lr;<name>"). ONE function for both directions and for every caller (menu bar
// today) — the direction is data, never a second code path. A nullptr callback posts a notice.
static void flipImageObject(Scene *s, const QString &name, bool updown) {
	if (!g_juliaImageFlip) {
		if (s && s->win) s->win->statusBar()->showMessage("Flip image: callback not registered", 3000);
		return;
	}
	const QString req = QString("%1;%2").arg(updown ? "ud" : "lr").arg(name);
	g_juliaImageFlip(s, req.toUtf8().constData());
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

// solar (Geography > "Sun and terminators"), GMT's own solar/pssolar. SolarDialog (70_window.cpp,
// loads deps/ui/solar_dialog.ui) hands a newline-separated "key=value" block to Julia (_on_solar,
// src/solar.jl):
//   terms=<subset of dcna>   which terminators to draw (-T): day/night, civil, nautical, astronomical
//   date=<ISO 8601>          absent = the module's own "now" (+d)
//   tz=<offset from UTC>     e.g. -03:00 (+z)
//   width=<line width>       for the drawn terminator lines, in POINTS
//   fill=<subset of dcna>    which of those terminators also PAINT their night side (pssolar's -G)
//   fillrgb=<r/g/b>          the paint colour, 0-255 each
//   filltr=<0..100>          its transparency in percent, the same number pssolar's @<transp> takes
//   sun=0|1                  report the sun's position (-I, read back through g_solarReport)
//   marksun=0|1              put a star at the sub-solar point
//   lon=, lat=               optional observer position: adds sunrise/noon/sunset/day length
//   outfile=                 optional table to also write the drawn polygons to (-M dump)
//   mapw=<western edge>      this window's longitude frame, so the small circle lands inside it
// Each terminator becomes (or REPLACES) its own line-overlay layer in `scene`, and a painted one a
// filled-polygon layer beside it. Returns 1 on success, 0 on failure. nullptr to detach.
typedef int (*JuliaSolarFn)(void *scene, const char *params);
static JuliaSolarFn g_juliaSolar = nullptr;

// What the sun report has to SAY, on its way back to the dialog's read-only box: Julia writes it
// from inside the callback (gmtvtk_solar_report), the dialog reads it once the call returns. Same
// write-only text channel as g_eulerResult — one pattern for "the module answered in words".
static std::string g_solarReport;

// Live scenes, keyed by the Scene *returned to the host as an opaque figure handle.
// A handle is valid only while its window is open; the window-destroyed lambda erases
// it here so a stale handle from a closed figure is rejected instead of dereferenced.
static std::unordered_set<Scene*> g_scenes;
// "Alive" means SAFE TO TOUCH, not merely "still in the registry": a window that has begun its
// destruction is already unusable (its widgets are gone before Qt even emits destroyed()), so it
// answers false here — the one place every caller asks, rather than a teardown check bolted onto
// each of them (SACRED_LAW.md: fix the shared source).
// MEMBERSHIP FIRST, THEN THE STRUCT. The whole point of this guard is that the handle may be
// garbage (a stale pointer from a closed window, or — in the host's unit tests — an invented one),
// so the registry lookup, which only compares pointer VALUES, has to come before any dereference.
// Reading `s->tearingDown` first made every call with a bogus handle an access violation instead of
// a clean `false` (`gmtvtk_log_error` on a fake scene: EXCEPTION_ACCESS_VIOLATION, test-gmtmodules).
static bool sceneAlive(Scene *s) { return s && g_scenes.count(s) != 0 && !s->tearingDown; }

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
#if defined(_WIN32)
	const unsigned long GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 0x4;
	const unsigned long GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 0x2;
	void *hm = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<const char *>(&gmtvtkModuleDir), &hm);
	char buf[260];
	unsigned long n = hm ? GetModuleFileNameA(hm, buf, sizeof(buf)) : 0;
	if (n == 0) return QString();
	return QFileInfo(QString::fromLocal8Bit(buf, (int)n)).absolutePath();
#else
	Dl_info info{};
	if (dladdr(reinterpret_cast<const void *>(&gmtvtkModuleDir), &info) == 0 || !info.dli_fname)
		return QString();
	return QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absolutePath();
#endif
}

// Where the runtime-loaded .ui files are. THE HOST DECIDES, because only it knows: the .ui ship
// with the Julia package (they are in git next to src/), while the DLL may well be loaded from
// somewhere else entirely — the depot-wide runtime cache ~/.julia/gmtvtk_runtime/deps/build. In that
// (normal, non-dev) install <module dir>/../ui is a directory that need not exist, or that holds an
// OLDER set of .ui than the package: every dialog whose .ui is missing there then refuses to open
// with "cannot open …". So Julia calls gmtvtk_set_ui_dir(<pkgroot>/deps/ui) at load time and that
// wins whenever it really exists. The two old rules stay as fallbacks, in their old order, for any
// host that never sets it (the standalone demo exe, an NSIS tree).
static QString g_uiDirOverride;

static QString gmtvtkUiDir() {
	if (!g_uiDirOverride.isEmpty() && QDir(g_uiDirOverride).exists()) return g_uiDirOverride;
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
// Ctrl+V — paste the clipboard INTO a window
// ============================================================================================
// Ctrl+V keeps its ORDINARY meaning inside any text entry (the in-window Julia console, a dialog's
// edit box, an editable combo): a window-scoped shortcut fires BEFORE the focus widget sees the
// key, so the handler first offers the paste to the focused widget when that widget has a paste()
// slot. Returns true when it did — the caller then does nothing else. Probing the metaobject (not
// a qobject_cast list of every text class) keeps it quiet and covers widgets we never enumerated.
static bool pasteIntoFocusedTextEntry() {
	QWidget *f = QApplication::focusWidget();
	if (!f || f->metaObject()->indexOfMethod("paste()") < 0) return false;
	return QMetaObject::invokeMethod(f, "paste");
}

// Read the clipboard and hand it to `s`'s Julia paste callback. Returns false when the clipboard
// holds nothing usable (the caller says so in its status bar).
//  - copied FILES (Explorer's Copy puts file: URLs on the clipboard) go through juliaOpenFile, i.e.
//    the drop path verbatim — pasting a copied file is opening that file.
//  - an IMAGE is flattened to packed RGB and handed over as bytes (no temp file, same buffer layout
//    as the GeoTIFF screenshot path: (band, col, row) once wrapped in Julia).
//  - TEXT goes over as-is; Julia owns the number parsing.
static bool scenePasteClipboard(Scene *s) {
	const QMimeData *md = QApplication::clipboard()->mimeData();
	if (!md) return false;
	if (md->hasUrls()) {
		bool any = false;
		for (const QUrl &u : md->urls()) {
			const QString f = u.toLocalFile();
			if (f.isEmpty()) continue;
			const QByteArray utf8 = f.toUtf8();      // keep the buffer alive across the call
			juliaOpenFile(s, utf8.constData());      // busy dialog up before Julia is entered
			any = true;
		}
		if (any) return true;
	}
	if (!g_juliaPaste) return false;
	if (md->hasImage()) {
		QImage im = qvariant_cast<QImage>(md->imageData());
		if (!im.isNull()) {
			if (im.hasAlphaChannel()) {              // flatten onto white: the image path here is RGB
				QImage flat(im.size(), QImage::Format_RGB888);
				flat.fill(Qt::white);
				QPainter p(&flat);
				p.drawImage(0, 0, im);
				p.end();
				im = flat;
			}
			else {
				im = im.convertToFormat(QImage::Format_RGB888);
			}
			const int w = im.width(), h = im.height();
			// Copy row by row: a QImage scanline is padded to a 4-byte boundary, the buffer Julia
			// wraps must be tightly packed w*3.
			std::vector<unsigned char> buf((size_t)w * (size_t)h * 3);
			for (int r = 0; r < h; ++r)
				memcpy(buf.data() + (size_t)r * (size_t)w * 3, im.constScanLine(r), (size_t)w * 3);
			showBusyDialog("Pasting image…");        // up before Julia is entered, as for a drop
			g_juliaPaste(s, "", buf.data(), w, h, 3);
			closeBusyDialog();
			return true;
		}
	}
	if (md->hasText() && !md->text().trimmed().isEmpty()) {
		const QByteArray utf8 = md->text().toUtf8();
		g_juliaPaste(s, utf8.constData(), nullptr, 0, 0, 0);
		return true;
	}
	return false;
}

// ONE Ctrl+V mechanism for every window kind (3-D viewer, X,Y tool, …): builds the shared "Paste"
// action with the standard shortcut and the text-entry hand-back above. The CALLER adds it to its
// own File menu (that is what activates the shortcut window-wide) and supplies what a paste does
// there. Never a second key handler per window type.
static QAction *makePasteAction(QWidget *win, std::function<void()> onPaste) {
	QAction *a = new QAction("&Paste", win);
	a->setShortcut(QKeySequence::Paste);
	a->setShortcutContext(Qt::WindowShortcut);
	a->setToolTip("Paste the clipboard (image, numeric table, or a copied file) into this window");
	QObject::connect(a, &QAction::triggered, win, [onPaste]() {
		if (pasteIntoFocusedTextEntry()) return;    // console / edit box keeps the normal Ctrl+V
		onPaste();
	});
	return a;
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
// `moduleOf` is asked for the page name AT CLICK TIME, so a dialog that drives several modules (the
// interpolation dialog: surface / nearneighbor / triangulate / block* / greenspline) opens the page
// of whatever method is selected right then. The fixed-name overloads below feed it a constant —
// one implementation of the button, never a second copy for the varying case.
static void addManualButton(QDialog *dlg, QBoxLayout *row, std::function<QString()> moduleOf) {
	if (!dlg || !row) return;
	const int px = 18;
	auto *btn = new QToolButton(dlg);
	btn->setIcon(makeHelpDiskIcon(px));
	btn->setIconSize(QSize(px, px));
	btn->setAutoRaise(true);                             // no frame: the disk IS the button
	btn->setCursor(Qt::PointingHandCursor);
	btn->setToolTip("Open full manual page");
	btn->setFocusPolicy(Qt::NoFocus);                    // never steals Enter/Tab from the real controls
	QObject::connect(btn, &QToolButton::clicked, dlg, [dlg, moduleOf]() {
		if (!g_juliaOpenManual) {
			QMessageBox::warning(dlg, "Manual", "Manual: callback not registered (rebuild/restart needed?).");
			return;
		}
		const QString moduleName = moduleOf();
		if (!g_juliaOpenManual(moduleName.toUtf8().constData()))
			QMessageBox::warning(dlg, "Manual",
				QString("Could not open the manual page for %1.").arg(moduleName));
	});
	row->insertWidget(0, btn);
}

static void addManualButton(QDialog *dlg, QBoxLayout *row, const QString &moduleName) {
	addManualButton(dlg, row, [moduleName]() { return moduleName; });
}

// Reflow every tooltip in a dialog onto SHORT LINES. Qt lays a plain-text tooltip out on one line
// however long it is, which on a sentence of explanation gives a ribbon stretching past the screen
// edge and is unreadable. A tooltip that is RICH text wraps, so each one is turned into rich text
// with explicit breaks at word boundaries. Tooltips that already carry markup are left alone, and
// so are short ones (nothing to wrap). One function, applied where every .ui dialog finishes, so no
// dialog has to remember to do it — and none of them carries <br> in its .ui.
static void wrapTooltips(QWidget *root, int cols = 64) {
	if (!root) return;
	QList<QWidget *> all = root->findChildren<QWidget *>();
	all.prepend(root);
	for (QWidget *w : all) {
		const QString t = w->toolTip();
		if (t.isEmpty() || t.size() <= cols) continue;
		if (t.startsWith("<") || t.contains("<br", Qt::CaseInsensitive)) continue;   // already rich
		QString out;
		int line = 0;
		for (const QString &word : t.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
			if (line > 0 && line + 1 + word.size() > cols) { out += "<br>"; line = 0; }
			else if (line > 0)                             { out += ' ';    line += 1; }
			out += word.toHtmlEscaped();
			line += word.size();
		}
		w->setToolTip("<html>" + out + "</html>");
	}
}

// Same thing for a dialog loaded from a .ui, where the bottom row is the one named
// "horizontalLayout_buttons" by convention. This is also where a .ui dialog FINISHES being built —
// every one of them calls it as its last construction step — so the tooltip reflow above rides
// along here rather than being remembered separately in twenty constructors.
static void addManualButton(QDialog *dlg, const QString &moduleName) {
	if (!dlg) return;
	addManualButton(dlg, dlg->findChild<QHBoxLayout *>("horizontalLayout_buttons"), moduleName);
	wrapTooltips(dlg);
}

static void addManualButton(QDialog *dlg, std::function<QString()> moduleOf) {
	if (!dlg) return;
	addManualButton(dlg, dlg->findChild<QHBoxLayout *>("horizontalLayout_buttons"), std::move(moduleOf));
	wrapTooltips(dlg);
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
