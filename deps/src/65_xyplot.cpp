// ============================================================================
//  X,Y plot tool — the evolution of the Profile. A STANDALONE QMainWindow with
//  a vtkChartXY plot, an Object Manager (tree of plotted series), a foldable
//  Data Viewer spreadsheet, a menubar (File / Analysis / Misc) and a toolbar.
//  Modelled on Mirone's ecran.m. This window is NOT a 3-D Scene: it has its own
//  struct (XYPlot) + its own C API (gmtvtk_xyplot_*, in 90_c_api.cpp) and shares
//  ONLY the QApplication + the gmtvtk_process_events pump with the 3-D viewer.
//
//  PAGES (Excel-like tabs at the bottom, above the Console): one window holds a
//  vector of XYPage, each its OWN chart + series + axes/time-mode. Only the
//  current page's chart sits in the shared vtkContextScene; switching pages swaps
//  it and rebuilds the Object Manager + Data Viewer. Derived quantities whose
//  units don't fit the parent axes (FFT, autocorrelation, derivatives) land on a
//  NEW page instead of a new window — see Julia _on_xy_analysis.
// ============================================================================

// The owner of a "Plot with GMT.jl" script editor. There is ONE editor (buildScriptEditor,
// 70_window.cpp) for every kind of window that has the menu item — the 3-D viewer and this X,Y plot —
// and this is all it needs to know about the window it belongs to. Defined here because this fragment
// is the first that builds one (scriptHostXY); 70_window.cpp builds the 3-D viewer's (scriptHostScene).
struct ScriptHost {
	void *key = nullptr;                    // the owner's handle: registry key, and what Julia is handed
	QWidget *parent = nullptr;              // the editor's parent window
	Scene *parkIn = nullptr;                // whose Scene Objects dock the editor parks in (null: X closes it)
	// An owner with a park place of its OWN (the X,Y window's Object Manager): set, these are used
	// instead of parkIn. `park` gets the editor already hidden.
	std::function<void(QWidget *)> park, unpark;
	QString parkLabel = "GMT.jl script";
	QString openCall;                       // the Julia function that builds the script for this owner
	std::function<bool()> alive;
	std::function<void(const QString &)> logError;
	std::function<void(const QString &, int)> status;
};
static void showScriptEditor(const ScriptHost &h);                          // 70_window.cpp
static void buildScriptEditor(const ScriptHost &host, const QString &text); // 70_window.cpp
static void scriptEditorUnpark(const ScriptHost &h);                        // 70_window.cpp
static std::function<void(const QPoint &)> scriptEditorParkedMenu(const ScriptHost &h); // 70_window.cpp

// One plotted curve: its data table (column 0 = X, column 1 = Y) + the chart
// plot item (owned by the chart) + presentation state.
struct XYSeries {
	vtkSmartPointer<vtkTable> table;
	vtkPlot                  *plot = nullptr;   // owned by the page chart; rebuilt on delete
	std::string               name;
	double                    r = 0.92, g = 0.67, b = 0.0;
	double                    width = 1.5;
	int                       lineType = 1;     // vtkPen: SOLID=1 DASH=2 DOT=3 DASH_DOT=4 DASH_DOT_DOT=5 NONE=0
	int                       marker = 0;       // vtkPlotPoints: NONE=0 CROSS=1 PLUS=2 SQUARE=3 CIRCLE=4 DIAMOND=5
	double                    markerSize = 7.0;
	bool                      visible = true;
	bool                      inLegend = true;  // false: drawn, but no entry in the legend (a biplot's x = 0, y = 0)
	// Columns the series' points came FROM, one value per row (a biplot's observations carry the input
	// variables their scores were computed from). Shown before X and Y by "Show in Data Table".
	std::vector<std::string>          srcNames;
	std::vector<std::vector<double>>  srcCols;
	std::string                       xName, yName;   // the table's X / Y headers ("" = "X" / the series name)
	// Chart plot type: 0 = line (vtkChart::LINE, the default), 1 = BAR (histogram / bar graph),
	// 2 = AREA, 3 = STEM, 4 = STAIRS, 5 = SCATTER (vtkChart::POINTS), 6 = BARS ON A BASE, 7 = HORIZONTAL
	// BARS ON A BASE, 8 = FILLED POLYGONS (vtkXYShapePlot, the plot-type gallery's own kinds).
	// It is a property of the SERIES, not of the call that made it, so a rebuild (xyRebuildPlots,
	// after a delete) re-creates the right plot item. `barWidth` is vtkPlotBar's bar width, which
	// is expressed in X DATA units (not pixels) -- hence its own field: `width` stays the pen width
	// in px for every kind, so the two never overwrite each other (see xyApplyStyle).
	int                       kind = 0;
	double                    barWidth = 1.0;
	bool                      barH = false;     // a BAR series drawn horizontally (the gallery's "barh")
	// What an area / based-bar series stands on (xyRestack): 0 y = 0, 1 the series before it (stacked
	// bars), 2 its own running sum (waterfall), 3 the page's first curve (fill_between). `base` is the
	// result, per row; derived, never saved.
	int                       stack = 0;
	std::vector<double>       base;
	bool                      rightAxis = false;   // plotyy: drawn against the right-hand Y axis
	// Screen-constant "+" cross (e.g. Tide tool's "Now" indicator) -- NOT a vtkPlotPoints marker
	// (VTK's marker glyph rendering literally overwrites the plot's pen width with MarkerSize, so a
	// marker-only series has no independent thickness control at all; see xyRecomputeCross). Built
	// instead from a 5-point NaN-broken path (H arm, gap, V arm) whose data-space half-lengths get
	// recomputed from the CURRENT per-axis pixel scale every render, so both arms stay visually
	// equal and the true on-screen size (crossSizePx) survives zoom/pan/resize. Reuses `width` for
	// the (real, working) stroke thickness -- only crossCenterX/Y/crossSizePx are new state.
	bool                      nowCross = false;
	double                    crossCenterX = 0.0, crossCenterY = 0.0;
	double                    crossSizePx = 24.0;
	double                    lastHx = -1.0, lastHy = -1.0;   // cached data-space half-lengths
	// Its part in the page's magnetic modelling (the "Geomagnetic bar code" strip, ecran's isocs tools):
	// 0 none, 1 the MEASURED anomaly profile the bar code / synthetic / fit refer to, 2 the synthetic RTP
	// profile, 3 the synthetic chunk moved to its best-correlation place by "Fit". A flag on the series,
	// not an index on the page, so a delete or a reorder never points the tools at the wrong curve.
	int                       magRole = 0;
	// The track under the curve, one (x, y) per row — a profile opened from the 3-D viewer carries it
	// (ProfilePanel::track). trackGeog: -1 no track, 0 cartesian, 1 lon/lat. ecran's handles.data(:,1:2):
	// what the magnetic model reads the profile azimuth and the bathymetry along the track from.
	std::vector<double>       trackX, trackY;
	int                       trackGeog = -1;
	// Per-point values the gallery's errorbar (kind 9) and bubblechart (kind 10) draw; empty = none.
	// Error bars are DELTAS from the point, lower and upper, on Y and on X (GMT.jl's error_bars); a
	// bubble's size is in the series' own units, mapped to the bubble pixel range at draw time.
	std::vector<double>       errYlo, errYhi, errXlo, errXhi;
	std::vector<double>       bubble;
	// On a LOG axis (xyLogInput): what the plot is drawn from instead of `table` — a copy with every point
	// a log axis cannot place (<= 0) made NaN, so the curve breaks there, as MATLAB's does. `table` itself
	// is never touched. logDropped = how many points that left out.
	vtkSmartPointer<vtkTable> logTable;
	int                       logDropped = 0;
};

// One Spector-Grant fit line kept on a spectrum page (ecran's 'FitLine': the line a drag left behind,
// editable by its two ends, with its own right-click menu). The ends are stored as PLOTTED (x, power);
// m, b are the ln(power) = m*k + b fit, depth = |m|/(4 pi) * unit.
struct XYSGLine {
	double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	double m = 0, b = 0, depth = 0, unit = 1000.0;
	double r = 0, g = 0, bl = 0;
	vtkSmartPointer<vtkTable> table;
	vtkPlot *plot = nullptr;                   // owned by the page chart
};

// One page's magnetic-anomaly modelling state — a port of what ecran.m keeps in its handles for the
// bar code (push_magBar_CB), the synthetic RTP profile (push_syntheticRTP_CB, slider_filter_CB) and
// the isochron age fit (popup_ageFit_CB / edit_ageFit_CB / push_ageFit_CB / pinpointLine).
struct XYMag {
	double ageStart = 0.0;                      // as typed: ageStart > ageEnd = bar drawn reversed (old -> young)
	double ageEnd   = std::numeric_limits<double>::quiet_NaN();
	bool   barOn = false;                       // the bar code exists
	std::vector<double>      bStart, bEnd;      // its normal-polarity bricks (ascending ages, clipped)
	std::vector<std::string> bTxt;              // anomaly names ("a" = unnamed)
	double barX0 = 0.0, barX1 = 1.0;            // the measured profile's first / last x: the bar spans them
	std::vector<double> picos;                  // red "pico" markers on the bar (ages)
	// model parameters (the Parameters dialog; ecran reads them from OPTcontrol.txt MIR_MAGPROF lines)
	double dec = 0.0, inc = 90.0;               // field declination / inclination (deg)
	double speed = 0.0;                         // FULL spreading rate, cm/yr (0 = from the profile and the ages)
	double profDir = 0.0;                       // profile azimuth (deg)
	double spreadDir = std::numeric_limits<double>::quiet_NaN();   // NaN = the profile azimuth
	double depth = 2.5;                         // top of the magnetized layer (km)
	int    unit = 1;                            // profile distance unit: 0 m, 1 km, 2 NM
	double contam = 1.0;                        // Tissot & Patriat contamination factor [0.5 1]
	std::string isocs;                          // isochrons to fit: "age[_pad[_stretch]] ..."
	std::string batFile;                        // bathymetry grid sampled along the track (ecran's BAT)
	double barStretch = 1.0;                    // ecran's stretchMagBar: oblique profile/spreading (1 = none)
	double lastSpeed = 0.0;                     // the rate the last synthetic was computed with
	// the synthetic and the fit
	std::vector<double> ageLine;                // age under every point of the synthetic
	int    ageIdx = 0;                          // isochron list choice (0 = none)
	double ageTyped = std::numeric_limits<double>::quiet_NaN();    // the age typed when there is no list
	int    starIdx = -1;                        // red star: row of the synthetic at the chosen age
	int    guessIdx = -1;                       // "Guide me" first guess: row of the measured profile
	double ageLineX = std::numeric_limits<double>::quiet_NaN();    // the fitted isochron (dashed line)
	std::vector<double> pinned;                 // "Pin-point me" isochron lines
	bool   rerun = false;                       // the measured track was redrawn while this page was not shown
};

// A text placed on a page at a DATA position (the plot types that draw their own frame: a radar's
// spoke names, a rose's angles, a parallel plot's variable names, a corner plot's panel scales). Drawn
// by the chart itself (xyLabelPaint), so it moves with zoom and pan like the curves.
struct XYLabel {
	double x = 0, y = 0;
	std::string text;
	double r = 0, g = 0, b = 0;
	double size = 11;
	int ha = 0, va = 0;                        // -1 left / bottom, 0 centre, 1 right / top
	bool bold = false;
	// where it was last drawn, scene px (xyLabelPaint): what a press is tested against to drag it
	bool  shown = false;
	float bx = 0, by = 0, bw = 0, bh = 0;
};

// A hand-drawn annotation (MATLAB's figure ANNOTATIONS: text arrow, arrow, double arrow, line, text
// box, rectangle, ellipse), made with the tool bar's annotation tools. Placed in DATA coordinates, like
// XYLabel, so it moves with zoom and pan, a saved session keeps it, and Plot with GMT.jl draws it at
// the same place on the same axes. Painted by the chart itself (xyAnnotPaint).
enum { XY_AN_TEXTARROW, XY_AN_ARROW, XY_AN_DARROW, XY_AN_LINE, XY_AN_TEXTBOX, XY_AN_RECT, XY_AN_ELLIPSE, XY_AN_COUNT };
static const char *const xyAnnotNames[XY_AN_COUNT] = {
	"Text Arrow", "Arrow", "Double Arrow", "Line", "Text Box", "Rectangle", "Ellipse" };
struct XYAnnot {
	int    kind = XY_AN_ARROW;
	// Line kinds: from (x0,y0) to (x1,y1), the head at (x1,y1) (the text of a text arrow at its tail).
	// Box kinds: two opposite corners, in any order.
	double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	std::string text;
	double r = 0, g = 0, b = 0;                // line and text colour
	double width = 1.5;                        // pen width, px (as a series' width)
	int    lineType = 1;                       // vtkPen: SOLID=1 DASH=2 DOT=3 DASH_DOT=4 DASH_DOT_DOT=5
	double size = 11;                          // font size
	bool   filled = false;                     // box kinds: painted inside
	double fr = 1, fg = 1, fb = 1;             // ...with this colour
	bool   visible = true;
	bool   fit = false;                        // a text box made by a click: its box follows its text
	bool   boxed = true;                       // a text box draws its box (outline + fill); off = the text alone
	// where it was last drawn, scene px (xyAnnotPaint): what a press is tested against
	bool   shown = false;
	float  sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
	float  tbx = 0, tby = 0, tbw = 0, tbh = 0;  // its text's box (text kinds), 0 wide = none
};
static bool xyAnnotIsLine(int k) { return k == XY_AN_TEXTARROW || k == XY_AN_ARROW || k == XY_AN_DARROW || k == XY_AN_LINE; }
static bool xyAnnotHasText(int k) { return k == XY_AN_TEXTARROW || k == XY_AN_TEXTBOX; }

// One PAGE inside an X,Y window: its own chart, its own series list, its own axis
// time-mode + Spector-Grant drag state. Only the current page's chart is mounted
// in the window's shared scene.
struct XYPage {
	vtkSmartPointer<vtkChartXY> chart;          // really an XYChart; owner -> the XYPlot
	std::vector<XYSeries>       series;
	std::string                 name;
	// X-axis time mode: 0 linear, 1 date-auto, 2 date(yyyy-mm-dd), 3 time(HH:MM), 4 decimal year,
	// 5 day-of-year. When non-zero the bottom axis ticks are formatted from epoch-seconds X and
	// regenerated on every range change (xyTicksOnRender observer).
	int                         xTimeFmt = 0;
	double                      lastLo = 0.0, lastHi = 0.0;
	double                      lastYLo = 0.0, lastYHi = 0.0, lastRLo = 0.0, lastRHi = 0.0;   // left / right Y
	bool                        yTicksUser = false;   // the Axes dialog set an explicit Y annotation stride
	bool                        ticksBusy = false;
	// Interactive Spector-Grant tool (left-drag a band on a spectrum -> live depth-to-sources).
	bool                        sgActive = false;
	bool                        sgDragging = false;
	int                         sgSel = -1;       // target series captured on activation
	double                      sgX0 = 0.0;       // drag-start frequency
	double                      sgUnit = 1000.0;  // wavenumber->metres factor (1/km default)
	vtkPlot                    *sgFit = nullptr;  // the live fit line (not in series)
	vtkSmartPointer<vtkTable>   sgFitTable;
	double                      sgR = 0, sgG = 0, sgB = 0;   // its colour: black first, random after (mkAnother)
	std::vector<XYSGLine>       sgLines;          // the fit lines kept (ecran 'FitLine's)
	int                         sgEdit = -1, sgEditEnd = 0;  // the line / end being dragged
	// Curve marker — the same gestures as the Profile panel's: a click ON a curve drops a circle there,
	// dragging slides it ALONG that curve with an "x,  y" label beside it, a double-click on it removes
	// it. Position = fractional row index into series[markSel] (row i + t towards row i+1), so it works
	// for any curve, not only one whose x ascends. Drawn by the chart itself (xyMarkPaint).
	int                         markSel = -1;
	double                      markU = 0.0;
	bool                        markDrag = false, markNew = false, markMoved = false;
	QPointF                     markPress;
	XYMag                       mag;              // magnetic modelling of this page (bar code, synthetic, fit)
	// "Reference a curve to another" (ecran's pick_onLines2reference / add_refMarkers / do_reference):
	// refLine[0] is the curve that gets referenced (red markers), refLine[1] the reference (green);
	// refRows[k] the rows picked on each, in click order. refLast = the curve marked last.
	bool                        refPicking = false;
	int                         refLine[2] = { -1, -1 };
	std::vector<int>            refRows[2];
	int                         refLast = -1;
	// The look of a page a plot type draws with its own frame (radar, rose, biplot, cornerplot,
	// marginalhist): a SQUARE plot area, the chart's own axes hidden, and its own labels.
	bool                        square = false;
	bool                        axesHidden = false;
	std::vector<XYLabel>        labels;
	std::vector<XYAnnot>        annots;           // the annotations drawn on it (the tool bar's annotation tools)
	int                         annotSel = -1;    // the selected one (its handles show), -1 none
	bool                        annotOpen = false;   // its Object Manager group is unfolded
	// The page's own axes as its Object Manager "Axes" row shows them: drawn or not, and the Axes
	// properties dialog's fields (deps/ui/axes_props.ui, GMT's own keys "KEY" / "KEY.part"), per page.
	bool                        axesShown = true;
	std::map<std::string, std::string> axProps;
	bool                        example = false;   // made by the gallery's "Synthetic example"
};

// Reveal + unfold a viewer's Scene Objects dock (defined in 70_window.cpp, later in this same
// translation unit): a plot that parks there must not land in a folded, invisible panel.
static void unfoldSceneObjects(Scene *s);

// A live X,Y plot window. The opaque handle handed to the host is this XYPlot*.
// Per-curve / per-axis state lives in the PAGES; the window owns only the shared
// widgets (one VTK view, the Object Manager, the Data Viewer, the Console, tabs).
struct XYPlot {
	QMainWindow                        *win = nullptr;
	// The 3-D viewer this plot belongs to, when it was opened from one (gmtvtk_xyplot_set_owner).
	// It decides WHOSE Scene Objects dock the window parks in when closed with the X. A plot with no
	// owner (a bare `xyplot()` from the REPL) has no dock to park in, so its X really closes it.
	Scene                              *owner = nullptr;
	bool                                parked = false;   // closed with the X: hidden, row in the dock
	// Tool windows of THIS plot (its GMT.jl script editor) parked as rows of the strip at the bottom of
	// its own Object Manager panel (parkStrip): hidden, not destroyed.
	struct ParkedTool {
		QWidget *w = nullptr;
		QString label;
		std::function<void()> unpark;
		std::function<void(const QPoint &)> menu;
	};
	std::vector<ParkedTool>             parkedTools;
	bool                                reallyClose = false;  // set by the row's "Delete" — let the X through
	bool                                tearingDown = false;  // programmatic close requested: dead to
	                                                          // xyAlive from that instant, not only once
	                                                          // the event loop has destroyed the window
	QLabel                             *infoLabel = nullptr;      // optional header strip above the chart
	                                                                // (e.g. Tide tool's Next High/Now/Next Low)
	double                              infoFontPt = 8.0;          // user-adjustable via the Object Manager
	QVTKOpenGLNativeWidget             *widget = nullptr;
	vtkSmartPointer<vtkContextView>     view;
	QTreeWidget                        *objMgr = nullptr;      // Object Manager (series list)
	QTreeWidget                        *parkStrip = nullptr;   // its bottom strip of parked tools (xyRebuildParkStrip)
	std::function<void()>               omUnfold;              // open the Object Manager panel if folded
	QPlainTextEdit                     *console = nullptr;       // ERRORS tab: read-only execution-error log (xyLog)
	QPlainTextEdit                     *cmdConsole = nullptr;    // JULIA tab: the ReplConsole (shares Main with the 3-D
	                                                             // viewer; the same widget the viewer's console uses)
	QWidget                            *consolePanel = nullptr;  // collapsible container under the chart
	QTabWidget                         *xyErrTab = nullptr;      // console body tabs (Errors | Julia); xyLog raises Errors
	QToolButton                        *consoleToggle = nullptr; // disclosure triangle (collapsed by default)
	bool                                legendVisible = true;    // legend show/hide, driven by the Object Manager's
	                                                              // "Legend" row (replaces the old toolbar toggle)
	double                              legendFontPt = 12.0;     // legend label font size (Legend row's "Font size…")
	// Misc > Grid (checkable: it SHOWS the current page's grid, xyGridSyncUi) and the tool bar's Grid,
	// X-Grid, Y-Grid buttons (plain clicks that flip it, like every button of that row). All set the grid
	// through the page's Axes properties (xyGridSet).
	QAction                            *actGrid = nullptr;
	QAction                            *actGridTb[3] = {};    // tool bar Grid / X-Grid / Y-Grid: exclusive modes
	QToolBar                           *toolsBar = nullptr;   // the Tools row: off until the page has data
	QAction                            *actConsole = nullptr;
	// Misc menu items that SHOW the current page's axis state (xySyncMiscMenu keeps them true to it,
	// whoever changed it: the menu, Julia's xtime!/logscale!, a page switch, a session load).
	QAction                            *actLogX = nullptr, *actLogY = nullptr;
	QAction                            *actTime[6] = {};          // indexed by XYPage::xTimeFmt
	vtkSmartPointer<vtkCallbackCommand> moveCb;               // mouse-move coord readout
	vtkSmartPointer<vtkCallbackCommand> ticksCb;             // time-axis tick refresh on range change
	bool                                rebuilding = false;    // guard objMgr itemChanged storms
	// pages (Excel-like tabs). Always >= 1; the current one is mounted in the scene.
	std::vector<XYPage>                 pages;
	int                                 curPageIdx = 0;
	QTabBar                            *tabs = nullptr;
	bool                                tabBusy = false;       // guard QTabBar::currentChanged storms
	// Opened from a Profile panel: its copy of the curve (series markLinkSel on page markLinkPage)
	// shares the panel's marker, both ways. The panel owns the position; null = not linked.
	QPointer<ProfilePanel>              markPanel;
	int                                 markLinkPage = -1, markLinkSel = -1;
	bool                                markSyncing = false;   // applying the panel's change: don't echo it
	// "Geomagnetic bar code" tools (ecran's isocs toggle): the toolbar toggle, the bar strip over the
	// chart, the control strip under it. Widgets are the window's; their values are the current page's.
	QAction                            *actMag = nullptr;
	QWidget                            *magBar = nullptr;      // the bar code strip (XYMagBarStrip)
	QWidget                            *magCtl = nullptr;      // the control strip
	QLineEdit                          *magStart = nullptr, *magEnd = nullptr, *magAgeEdit = nullptr;
	QPushButton                        *magBtnBar = nullptr, *magBtnSynth = nullptr, *magBtnParams = nullptr,
	                                   *magBtnFit = nullptr;
	QSlider                            *magSlider = nullptr;
	QComboBox                          *magAgeCombo = nullptr;
	bool                                magGuiding = false;    // "Guide me": next left click sets the first guess
	bool                                magSyncing = false;    // filling the widgets: their signals do nothing
	QString                             reqErr;                // a failed Julia request's message (xyRequest shows it)
	bool                                cometBusy = false;     // the gallery's "comet" is animating
	QTimer                             *profTimer = nullptr;   // a redrawn 3-D track re-runs the mag tools once it settles
	QAction                            *actRef = nullptr;      // Analysis > Reference a curve to another (checkable)
	// The tool bar's annotation tools: the one armed (XY_AN_*, -1 none) — the next press on the chart
	// draws one — and the in-place text editor a text annotation is typed in.
	QAction                            *actAnnot[XY_AN_COUNT] = {};
	int                                 annotTool = -1;
	QPointer<QLineEdit>                 annotEdit;
	// The Spector-Grant fit's answer from GMT's trend1d (xyanalysis.jl, gmtvtk_xyplot_sg_set_fit).
	bool                                sgFitGot = false;
	double                              sgFitM = 0.0, sgFitB = 0.0;
};

// Live X,Y windows, keyed by the XYPlot *handed back to the host. A handle is
// valid only while its window is open (the destroyed-lambda erases it).
static std::unordered_set<XYPlot*> g_xyplots;
// Registry membership FIRST (it only compares pointer values, so a garbage handle is safe), and the
// struct only after — same rule, same reason, as sceneAlive (30_app.cpp).
static bool xyAlive(XYPlot *p) { return p && g_xyplots.count(p) != 0 && !p->tearingDown; }

// The current page of a window (always valid once the window is built: there is
// always at least one page).
static inline XYPage &xyCur(XYPlot *s) { return s->pages[s->curPageIdx]; }

// The annotations' menu / properties / redraw (defined with the annotation tools, below).
static void xyAnnotMenu(XYPlot *s, int i, const QPoint &global);
static void xyAnnotProps(XYPlot *s, int i);
static void xyAnnotRender(XYPlot *s);

// Append one timestamped line to the collapsible Console panel AND echo it on the status bar, so a
// failing Analysis op (or any callback) is VISIBLE in the window instead of vanishing into the
// REPL's stderr. The panel is collapsed by default; `isError` auto-expands it so genuine failures
// can't hide, while ordinary info lines just accumulate quietly behind the triangle.
static void xyLog(XYPlot *s, const QString &msg, bool isError = false) {
	if (!xyAlive(s))
		return;
	if (s->console) {
		s->console->appendPlainText(QString("[%1]  %2")
			.arg(QTime::currentTime().toString("HH:mm:ss")).arg(msg));
		if (isError) {
			if (s->xyErrTab) s->xyErrTab->setCurrentWidget(s->console);   // raise the Errors tab
			if (s->consoleToggle && !s->consoleToggle->isChecked())
				s->consoleToggle->setChecked(true);    // pop open on errors only
		}
	}
	if (s->win)
		s->win->statusBar()->showMessage(msg, 5000);
}

// File-menu callback into Julia (Open / Save / New). The host owns data + file IO,
// so the menu hands Julia the action + the chosen path (the C side runs the native
// QFileDialog) + the selected series index (for Save; -1 = none/all). action is
// "open" | "save" | "new". Set via gmtvtk_xyplot_set_callback; nullptr -> the menu
// item just shows a status message.
typedef void (*JuliaXYFn)(void *plot, const char *action, int sel, const char *path);
static JuliaXYFn g_juliaXY = nullptr;

// Analysis-menu callback into Julia. fn(plot, op, sel): op = the operation tag
// ("remove_mean" | "remove_trend" | "deriv1" | "deriv2" | "autocorr" | "fft_amp" |
// "fft_psd"); sel = the Object-Manager-selected series the op runs on. Julia pulls the
// series data via gmtvtk_xyplot_get_series, computes the transform, then either overlays
// it on the current page or spawns a NEW page (gmtvtk_xyplot_add_page) for results whose
// units don't fit the parent axes. Set via gmtvtk_xyplot_set_analysis_callback.
typedef void (*JuliaXYAnaFn)(void *plot, const char *op, int sel);
static JuliaXYAnaFn g_juliaXYAna = nullptr;

// Seed callback: when an X,Y window is spawned from C++ (the Profile panel's "Open in X,Y plot
// tool"), this hands the initial (x,y) series to Julia so Julia REGISTERS a QtXYPlot mirror for
// the window + records the data. Without it a C++-spawned window has no Julia-side series, so its
// File>Save and Analysis (which route through Julia) would find nothing. fn(plot, x, y, n, name).
// nullptr (e.g. the standalone demo exe) -> openSeriesInXYTool adds the series directly instead.
typedef void (*JuliaXYSeedFn)(void *plot, const double *x, const double *y, int n, const char *name);
static JuliaXYSeedFn g_juliaXYSeed = nullptr;

// "New blank window" callback: when an EMPTY X,Y window is opened from C++ (the 3-D viewer's
// Tools > X,Y plot), this lets Julia register a QtXYPlot mirror for it so File>Open / Save /
// Analysis (which route through Julia) work. fn(plot). nullptr (demo exe) -> no mirror.
typedef void (*JuliaXYNewFn)(void *plot);
static JuliaXYNewFn g_juliaXYNew = nullptr;

// ---- helpers ---------------------------------------------------------------

// Default line-colour palette (matplotlib "tab10"), cycled by series index when the
// caller does not pass an explicit colour. Distinct, print-friendly hues.
static const double *xyPalette(int i) {
	static const double pal[10][3] = {
		{0.12, 0.47, 0.71}, {1.00, 0.50, 0.05}, {0.17, 0.63, 0.17}, {0.84, 0.15, 0.16},
		{0.58, 0.40, 0.74}, {0.55, 0.34, 0.29}, {0.89, 0.47, 0.76}, {0.50, 0.50, 0.50},
		{0.74, 0.74, 0.13}, {0.09, 0.75, 0.81},
	};
	return pal[((i % 10) + 10) % 10];
}

// The plot items the plot-type gallery adds to vtkPlotLine: AREA (filled down to y = 0, or down to
// another curve -- fill_between), STEM (a stick from y = 0 up to each point, a marker on its top),
// STAIRS (a step at every x) and BASED BARS (each bar standing on its own base -- stacked bars, stacked
// horizontal bars, a waterfall). They paint from the SAME cached points a plain line paints (shifted,
// scaled, log'd by vtkPlotPoints), so the hover tooltip, log axes and zoom behave exactly as on a line.
// A NaN breaks the shape, as it breaks a line.
class vtkXYShapePlot : public vtkPlotLine {
public:
	static vtkXYShapePlot *New();
	vtkTypeMacro(vtkXYShapePlot, vtkPlotLine);
	int Shape = 2;                     // XYSeries::kind: 2 area, 3 stem, 4 stairs, 6 bars, 7 horizontal bars, 8 polygons,
	                                   // 9 error bars, 10 bubbles, 11 arrows
	std::vector<double> Base;          // per row, data units: what an area / a bar stands on (empty = 0)
	double BarW = 1.0;                 // bar width, data units of the position axis
	double VLo = 0, VHi = 0, PLo = 0, PHi = 0;   // a based bar series' data extent: values / positions
	// Error bars: per row, the bar's ENDS in data units (empty = none on that axis); CapPx the half
	// width of a bar's caps, screen px. Bubbles: per row, the diameter in screen px (NaN = none).
	std::vector<double> YLo, YHi, XLo, XHi, BubblePx;
	double CapPx = 4.0;
	bool Paint(vtkContext2D *painter) override {
		if (!this->Visible || !this->Points || this->Points->GetNumberOfPoints() == 0)
			return false;
		const vtkIdType n = this->Points->GetNumberOfPoints();
		const vtkRectd ss = this->GetShiftScale();
		auto baseAt = [&](vtkIdType i) { return i < (vtkIdType)Base.size() ? Base[i] : 0.0; };
		// Data -> the points' own (shifted, scaled, log'd) space, as Base is mapped below.
		auto sx = [&](double v) { double t = (v + ss[0]) * ss[2]; return this->LogX ? std::log10(t) : t; };
		auto sy = [&](double v) { double t = (v + ss[1]) * ss[3]; return this->LogY ? std::log10(t) : t; };
		// Screen px -> that space, from the painter's own transform (the chart's zoom).
		double pxX = 1.0, pxY = 1.0;
		if (vtkTransform2D *tf = painter->GetTransform()) {
			const double *m = tf->GetMatrix()->GetData();
			if (m[0] != 0.0) pxX = 1.0 / std::abs(m[0]);
			if (m[4] != 0.0) pxY = 1.0 / std::abs(m[4]);
		}
		if (Shape == 9) {
			// A whisker from each point's lower to its upper end, capped, on Y and/or X; then the curve
			// and its markers, drawn as any line is (vtkPlotLine::Paint), on top.
			painter->ApplyPen(this->Pen);
			painter->GetPen()->SetLineType(vtkPen::SOLID_LINE);
			const float cx = (float)(CapPx * pxX), cy = (float)(CapPx * pxY);
			for (vtkIdType i = 0; i < n; ++i) {
				double p[3];
				this->Points->GetPoint(i, p);
				if (!std::isfinite(p[0]) || !std::isfinite(p[1])) continue;
				if (i < (vtkIdType)YLo.size() && i < (vtkIdType)YHi.size()) {
					const double a = sy(YLo[i]), b = sy(YHi[i]);
					if (std::isfinite(a) && std::isfinite(b)) {
						painter->DrawLine((float)p[0], (float)a, (float)p[0], (float)b);
						painter->DrawLine((float)p[0] - cx, (float)a, (float)p[0] + cx, (float)a);
						painter->DrawLine((float)p[0] - cx, (float)b, (float)p[0] + cx, (float)b);
					}
				}
				if (i < (vtkIdType)XLo.size() && i < (vtkIdType)XHi.size()) {
					const double a = sx(XLo[i]), b = sx(XHi[i]);
					if (std::isfinite(a) && std::isfinite(b)) {
						painter->DrawLine((float)a, (float)p[1], (float)b, (float)p[1]);
						painter->DrawLine((float)a, (float)p[1] - cy, (float)a, (float)p[1] + cy);
						painter->DrawLine((float)b, (float)p[1] - cy, (float)b, (float)p[1] + cy);
					}
				}
			}
			return this->Superclass::Paint(painter);
		}
		if (Shape == 11) {
			// Arrows (feather): every NaN-separated run is a shaft from its first point to its last, with a
			// filled head on the last. The head is sized in screen px, so neither the axes' different x and
			// y scales nor a zoom bend it.
			painter->ApplyPen(this->Pen);
			painter->ApplyBrush(this->Brush);
			const double hl = 3.0 * std::max(2.5f, this->Pen->GetWidth()) + 4.0;   // head length, px
			const double hw = 0.35 * hl;                                             // half width, px
			vtkIdType i = 0;
			while (i < n) {
				double a[3], b[3];
				this->Points->GetPoint(i, a);
				if (!std::isfinite(a[0]) || !std::isfinite(a[1])) { ++i; continue; }
				vtkIdType j = i;
				while (j + 1 < n) {
					double q[3];
					this->Points->GetPoint(j + 1, q);
					if (!std::isfinite(q[0]) || !std::isfinite(q[1])) break;
					++j;
				}
				this->Points->GetPoint(j, b);
				i = j + 1;
				const double dx = (b[0] - a[0]) / pxX, dy = (b[1] - a[1]) / pxY;   // the shaft in px
				const double len = std::hypot(dx, dy);
				if (len <= 0.0) continue;
				const double ux = dx / len, uy = dy / len;
				const double h = std::min(hl, 0.6 * len), w = h * hw / hl;          // a short shaft: smaller head
				// the shaft stops where the head begins, so a thick pen never pokes through its tip
				painter->DrawLine((float)a[0], (float)a[1], (float)(b[0] - h * ux * pxX), (float)(b[1] - h * uy * pxY));
				float tri[6] = {
					(float)b[0], (float)b[1],
					(float)(b[0] + (-h * ux - w * uy) * pxX), (float)(b[1] + (-h * uy + w * ux) * pxY),
					(float)(b[0] + (-h * ux + w * uy) * pxX), (float)(b[1] + (-h * uy - w * ux) * pxY),
				};
				painter->DrawPolygon(tri, 3);
			}
			return true;
		}
		if (Shape == 10) {
			// A filled, see-through circle per point, its diameter the point's bubble size in px.
			painter->ApplyPen(this->Pen);
			painter->GetPen()->SetLineType(vtkPen::SOLID_LINE);
			painter->ApplyBrush(this->Brush);
			for (vtkIdType i = 0; i < n; ++i) {
				double p[3];
				this->Points->GetPoint(i, p);
				const double d = i < (vtkIdType)BubblePx.size() ? BubblePx[i] : NAN;
				if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(d) || d <= 0.0) continue;
				// a 36-sided polygon: vtkContext2D's own ellipse is visibly faceted at bubble sizes
				float ring[2 * 37];
				for (int k = 0; k <= 36; ++k) {
					const double a = k * 2.0 * M_PI / 36.0;
					ring[2 * k] = (float)(p[0] + std::cos(a) * d / 2 * pxX);
					ring[2 * k + 1] = (float)(p[1] + std::sin(a) * d / 2 * pxY);
				}
				painter->DrawPolygon(ring, 36);
				painter->DrawPoly(ring, 37);
			}
			return true;
		}
		if (Shape == 6 || Shape == 7) {
			// Rows -> rectangles. A vertical bar's value is on Y, a horizontal one's on X (its input
			// columns are swapped, xyApplyStyle), so the base adds along that axis.
			painter->ApplyPen(this->Pen);
			painter->ApplyBrush(this->Brush);
			for (vtkIdType i = 0; i < n; ++i) {
				double p[3];
				this->Points->GetPoint(i, p);
				const double b = baseAt(i);
				if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(b))
					continue;
				if (Shape == 6) {
					const double w = BarW * ss[2], y0 = (b + ss[1]) * ss[3], y1 = p[1] + b * ss[3];
					painter->DrawRect((float)(p[0] - w / 2), (float)std::min(y0, y1), (float)w, (float)std::abs(y1 - y0));
				}
				else {
					const double h = BarW * ss[3], x0 = (b + ss[0]) * ss[2], x1 = p[0] + b * ss[2];
					painter->DrawRect((float)std::min(x0, x1), (float)(p[1] - h / 2), (float)std::abs(x1 - x0), (float)h);
				}
			}
			return true;
		}
		if (Shape == 8) {
			// Filled polygons, one per NaN-separated run, each filled as a fan around its vertices' mean
			// (a radar polygon, a rose petal: shapes that are star-shaped about that point), then outlined.
			std::vector<float> ring;
			auto flush = [&] {
				const int m = (int)ring.size() / 2;
				if (m >= 3) {
					float cx = 0, cy = 0;
					for (int i = 0; i < m; ++i) { cx += ring[2 * i]; cy += ring[2 * i + 1]; }
					cx /= m; cy /= m;
					std::vector<float> q;
					q.reserve(4 * ((size_t)m + 1));
					for (int i = 0; i <= m; ++i) {
						q.push_back(cx); q.push_back(cy);
						q.push_back(ring[2 * (i % m)]); q.push_back(ring[2 * (i % m) + 1]);
					}
					painter->ApplyBrush(this->Brush);
					painter->GetPen()->SetLineType(vtkPen::NO_PEN);
					painter->DrawQuadStrip(q.data(), 2 * (m + 1));
					painter->ApplyPen(this->Pen);
					painter->DrawPoly(ring.data(), m);
				}
				ring.clear();
			};
			for (vtkIdType i = 0; i < n; ++i) {
				double p[3];
				this->Points->GetPoint(i, p);
				if (!std::isfinite(p[0]) || !std::isfinite(p[1])) { flush(); continue; }
				ring.push_back((float)p[0]);
				ring.push_back((float)p[1]);
			}
			flush();
			return true;
		}
		std::vector<std::vector<float>> runs(1);      // runs of finite points, x y x y ...
		std::vector<std::vector<float>> runBase(1);   // and the base under each of them (scene y)
		float ymin = std::numeric_limits<float>::max();
		for (vtkIdType i = 0; i < n; ++i) {
			double p[3];
			this->Points->GetPoint(i, p);
			double b = (baseAt(i) + ss[1]) * ss[3];
			if (this->LogY) b = Base.empty() ? 0.0 : std::log10(b);
			if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(b)) {
				if (!runs.back().empty()) { runs.emplace_back(); runBase.emplace_back(); }
				continue;
			}
			runs.back().push_back((float)p[0]);
			runs.back().push_back((float)p[1]);
			runBase.back().push_back((float)b);
			ymin = std::min(ymin, (float)p[1]);
		}
		// The baseline y = 0 in the points' own (shifted/scaled) space; a log Y axis has no 0, so there
		// it is the lowest point (unless the base is another curve, which a log axis shows as it is).
		const bool logFloor = this->LogY && Base.empty();
		for (size_t k = 0; k < runs.size(); ++k) {
			const std::vector<float> &r = runs[k];
			const std::vector<float> &rb = runBase[k];
			const int m = (int)r.size() / 2;
			if (m < 1)
				continue;
			painter->ApplyPen(this->Pen);
			if (Shape == 2) {
				if (m < 2)
					continue;
				std::vector<float> q;
				q.reserve(4 * (size_t)m);
				for (int i = 0; i < m; ++i) {
					q.push_back(r[2 * i]); q.push_back(r[2 * i + 1]);
					q.push_back(r[2 * i]); q.push_back(logFloor ? ymin : rb[i]);
				}
				painter->ApplyBrush(this->Brush);
				painter->DrawQuadStrip(q.data(), 2 * m);
				painter->DrawPoly(const_cast<float *>(r.data()), m);
			}
			else if (Shape == 3) {
				for (int i = 0; i < m; ++i)
					painter->DrawLine(r[2 * i], logFloor ? ymin : rb[i], r[2 * i], r[2 * i + 1]);
			}
			else {
				std::vector<float> q;
				q.reserve(4 * (size_t)m);
				for (int i = 0; i < m; ++i) {
					if (i > 0) { q.push_back(r[2 * i]); q.push_back(r[2 * i - 1]); }   // across, then up/down
					q.push_back(r[2 * i]); q.push_back(r[2 * i + 1]);
				}
				if (q.size() >= 4)
					painter->DrawPoly(q.data(), (int)q.size() / 2);
			}
		}
		return this->vtkPlotPoints::Paint(painter);   // the markers (a stem's heads), nothing else
	}
	// What an area / a stem / a bar stands on is part of its extent (y = 0 unless Y is log, which has no
	// 0; or the curve under it): the axes then show the whole fill, every stick, every bar.
	void GetBounds(double bounds[4]) override {
		const vtkRectd ss = this->GetShiftScale();
		if (Shape == 6 || Shape == 7) {
			double u[4];
			GetUnscaledInputBounds(u);
			bounds[0] = (u[0] + ss[0]) * ss[2]; bounds[1] = (u[1] + ss[0]) * ss[2];
			bounds[2] = (u[2] + ss[1]) * ss[3]; bounds[3] = (u[3] + ss[1]) * ss[3];
			return;
		}
		this->Superclass::GetBounds(bounds);
		if ((Shape == 2 || Shape == 3) && !this->LogY) {
			double lo, hi;
			BaseRange(lo, hi);
			bounds[2] = std::min(bounds[2], (lo + ss[1]) * ss[3]);
			bounds[3] = std::max(bounds[3], (hi + ss[1]) * ss[3]);
		}
		if (Shape == 9) {                              // every error bar's ends are part of the extent
			double lo, hi;
			if (!this->LogX && EndsRange(XLo, XHi, lo, hi)) {
				bounds[0] = std::min(bounds[0], (lo + ss[0]) * ss[2]);
				bounds[1] = std::max(bounds[1], (hi + ss[0]) * ss[2]);
			}
			if (!this->LogY && EndsRange(YLo, YHi, lo, hi)) {
				bounds[2] = std::min(bounds[2], (lo + ss[1]) * ss[3]);
				bounds[3] = std::max(bounds[3], (hi + ss[1]) * ss[3]);
			}
		}
		if (Shape == 10 && !this->LogX && !this->LogY) PadBubbles(bounds);
	}
	// The data extent of a pair of per-row end vectors; false when there is none.
	static bool EndsRange(const std::vector<double> &a, const std::vector<double> &b, double &lo, double &hi) {
		lo = std::numeric_limits<double>::max(); hi = -lo;
		for (const std::vector<double> *v : { &a, &b })
			for (double t : *v)
				if (std::isfinite(t)) { lo = std::min(lo, t); hi = std::max(hi, t); }
		return lo <= hi;
	}
	void GetUnscaledInputBounds(double bounds[4]) override {
		if (Shape == 6) { bounds[0] = PLo; bounds[1] = PHi; bounds[2] = VLo; bounds[3] = VHi; return; }
		if (Shape == 7) { bounds[0] = VLo; bounds[1] = VHi; bounds[2] = PLo; bounds[3] = PHi; return; }
		this->Superclass::GetUnscaledInputBounds(bounds);
		if ((Shape == 2 || Shape == 3) && !(this->YAxis && this->YAxis->GetLogScaleActive())) {
			double lo, hi;
			BaseRange(lo, hi);
			bounds[2] = std::min(bounds[2], lo);
			bounds[3] = std::max(bounds[3], hi);
		}
		if (Shape == 9) {
			double lo, hi;
			if (EndsRange(XLo, XHi, lo, hi)) { bounds[0] = std::min(bounds[0], lo); bounds[1] = std::max(bounds[1], hi); }
			if (EndsRange(YLo, YHi, lo, hi)) { bounds[2] = std::min(bounds[2], lo); bounds[3] = std::max(bounds[3], hi); }
		}
		if (Shape == 10) PadBubbles(bounds);
	}
	// Bubbles have a size: the axes leave 6 % of the data range on every side, so the bubbles at the
	// extremes are drawn whole (a bubble is pixels, the range is data: a fraction of the range is the
	// only margin both sides know).
	static void PadBubbles(double b[4]) {
		for (int k = 0; k < 4; k += 2) {
			const double d = b[k + 1] - b[k], m = (d > 0 ? d : std::max(1.0, std::abs(b[k]))) * 0.06;
			b[k] -= m; b[k + 1] += m;
		}
	}
	void BaseRange(double &lo, double &hi) const {
		lo = hi = 0.0;
		if (Base.empty())
			return;
		lo = std::numeric_limits<double>::max(); hi = -lo;
		for (double b : Base)
			if (std::isfinite(b)) { lo = std::min(lo, b); hi = std::max(hi, b); }
		if (lo > hi) lo = hi = 0.0;
	}
	bool PaintLegend(vtkContext2D *painter, const vtkRectf &rect, int idx) override {
		if (Shape != 2 && Shape != 6 && Shape != 7 && Shape != 8 && Shape != 10 && Shape != 11)
			return this->Superclass::PaintLegend(painter, rect, idx);
		painter->ApplyPen(this->Pen);
		painter->ApplyBrush(this->Brush);
		if (Shape == 11) {                             // an arrow's legend is an arrow: the head Paint draws
			const float y = rect[1] + rect[3] / 2, xa = rect[0], xb = rect[0] + rect[2];
			const float hl = 3.0f * std::max(2.5f, this->Pen->GetWidth()) + 4.0f;
			const float h = std::min(hl, 0.6f * (xb - xa)), w = 0.35f * h;
			painter->DrawLine(xa, y, xb - h, y);
			float tri[6] = { xb, y, xb - h, y + w, xb - h, y - w };
			painter->DrawPolygon(tri, 3);
			return true;
		}
		if (Shape == 10) {                             // a bubble's legend is a bubble
			const float r = 0.5f * std::min(rect[2], rect[3]);
			painter->DrawEllipse(rect[0] + rect[2] / 2, rect[1] + rect[3] / 2, r, r);
			return true;
		}
		painter->DrawRect(rect[0], rect[1], rect[2], rect[3]);
		return true;
	}
};
vtkStandardNewMacro(vtkXYShapePlot);

// Add the chart plot item a series' `kind` asks for. THE one place a series' plot item is made, so a
// new series, a rebuild, a page copy and a session load all draw a kind the same way.
static vtkPlot *xyAddPlotItem(vtkChartXY *chart, const XYSeries &se) {
	vtkPlot *pl = nullptr;
	if ((se.kind >= 2 && se.kind <= 4) || (se.kind >= 6 && se.kind <= 11)) {
		vtkNew<vtkXYShapePlot> sp;
		sp->Shape = se.kind;
		chart->AddPlot(sp);                           // the chart keeps its own reference
		pl = sp;
	}
	else {
		pl = chart->AddPlot(se.kind == 1 ? vtkChart::BAR : se.kind == 5 ? vtkChart::POINTS : vtkChart::LINE);
	}
	if (se.rightAxis)                                 // plotyy: this curve reads the right-hand Y axis
		chart->SetPlotCorner(pl, 1);
	return pl;
}

// What each series of a page stands on (XYSeries::stack): its own running sum (waterfall), the
// series before it (stacked bars) or the page's first curve (fill_between). Recomputed whenever the
// page's series or their kinds change, so a delete re-stacks what is left.
static void xyRestack(XYPage &pg) {
	std::vector<double> run;                          // the stack so far, per row
	for (int k = 0; k < (int)pg.series.size(); ++k) {
		XYSeries &se = pg.series[k];
		se.base.clear();
		if (se.stack == 0 || !se.table)
			continue;
		const int n = (int)se.table->GetNumberOfRows();
		se.base.assign((size_t)n, 0.0);
		if (se.stack == 1) {
			if ((int)run.size() < n) run.resize((size_t)n, 0.0);
			for (int i = 0; i < n; ++i) {
				se.base[i] = run[i];
				const double y = se.table->GetValue(i, 1).ToDouble();
				if (std::isfinite(y)) run[i] += y;
			}
		}
		else if (se.stack == 2) {
			double c = 0.0;
			for (int i = 0; i < n; ++i) {
				se.base[i] = c;
				const double y = se.table->GetValue(i, 1).ToDouble();
				if (std::isfinite(y)) c += y;
			}
		}
		else if (k > 0 && pg.series[0].table) {       // 3: down to the first curve, row by row
			vtkTable *t0 = pg.series[0].table;
			for (int i = 0; i < n; ++i)
				se.base[i] = i < t0->GetNumberOfRows() ? t0->GetValue(i, 1).ToDouble()
				                                       : std::numeric_limits<double>::quiet_NaN();
		}
	}
}

// Push a series' presentation (colour / width / line style / markers) onto its chart plot.
// `se.markerSize` is the caller-facing, DPI-independent size (what a spinbox/Julia API sets and
// shows back) -- scaled by devicePixelRatioF() here, at the one point it actually reaches VTK, so
// a HiDPI display (this app runs on Windows, where 125%/150% scaling is the common case) doesn't
// silently render a "size 18" marker at a fraction of that many physical pixels.
// The bubble diameter range, in points (MATLAB bubblechart's default BubbleSizeRange, [5 20]).
static const double XY_BUBBLE_PT_LO = 5.0, XY_BUBBLE_PT_HI = 20.0;

// An error bar / bubble series' per-point values, onto its plot item: the bars' ENDS from the series'
// deltas (point - lower, point + upper), the bubbles' diameters in px. Bubble AREA is linear in the size
// value, between the smallest and largest value of the series (MATLAB's mapping); equal values -> the
// middle of the range.
static void xyShapeExtras(XYPlot *s, const XYSeries &se, vtkXYShapePlot *sp) {
	sp->YLo.clear(); sp->YHi.clear(); sp->XLo.clear(); sp->XHi.clear(); sp->BubblePx.clear();
	const double dpr = (s && s->widget) ? s->widget->devicePixelRatioF() : 1.0;
	sp->CapPx = 4.0 * dpr;
	const int n = se.table ? (int)se.table->GetNumberOfRows() : 0;
	if (se.kind == 9) {
		auto ends = [&](const std::vector<double> &lo, const std::vector<double> &hi, int col,
		                std::vector<double> &a, std::vector<double> &b) {
			if (lo.empty() && hi.empty()) return;
			a.assign((size_t)n, NAN); b.assign((size_t)n, NAN);
			for (int i = 0; i < n; ++i) {
				const double v = se.table->GetValue(i, col).ToDouble();
				const double l = i < (int)lo.size() ? lo[i] : (i < (int)hi.size() ? hi[i] : NAN);
				const double h = i < (int)hi.size() ? hi[i] : l;
				a[i] = v - l; b[i] = v + h;
			}
		};
		ends(se.errYlo, se.errYhi, 1, sp->YLo, sp->YHi);
		ends(se.errXlo, se.errXhi, 0, sp->XLo, sp->XHi);
	}
	else if (se.kind == 10 && !se.bubble.empty()) {
		double lo = std::numeric_limits<double>::max(), hi = -lo;
		for (double v : se.bubble) if (std::isfinite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
		const double d0 = XY_BUBBLE_PT_LO * 96.0 / 72.0 * dpr, d1 = XY_BUBBLE_PT_HI * 96.0 / 72.0 * dpr;
		sp->BubblePx.assign((size_t)n, NAN);
		for (int i = 0; i < n && i < (int)se.bubble.size(); ++i) {
			const double v = se.bubble[i];
			if (!std::isfinite(v)) continue;
			const double t = hi > lo ? (v - lo) / (hi - lo) : 0.5;
			sp->BubblePx[i] = std::sqrt(d0 * d0 + t * (d1 * d1 - d0 * d0));
		}
	}
}

// A LOG axis can only place positive values. MATLAB's semilogx / semilogy / loglog leave the others out
// ("Negative data ignored") and draw the rest: so a series on a page with a log axis is drawn from a copy
// of its table in which every point that axis cannot take (<= 0, or not finite) is NaN — the curve breaks
// there — and the axis, seeing only placeable data, can really go logarithmic. The series' own `table`
// (its data: saved, analysed, shown in the Data Table) is never touched; with no log axis the plot
// reads it again. Called by xyApplyStyle, the step every plot build / rebuild / restyle goes through.
static void xyLogInput(XYSeries &se) {
	se.logDropped = 0;
	if (!se.plot || !se.table || se.nowCross) return;   // the "now" cross rebuilds its own table every render
	const int xc = se.kind == 7 ? 1 : 0, yc = 1 - xc;     // horizontal bars: the value on X
	const bool lx = se.plot->GetXAxis() && se.plot->GetXAxis()->GetLogScale();
	const bool ly = se.plot->GetYAxis() && se.plot->GetYAxis()->GetLogScale();
	if (!lx && !ly) {
		if (se.logTable) { se.logTable = nullptr; se.plot->SetInputData(se.table, xc, yc); }
		return;
	}
	if (!se.logTable) se.logTable = vtkSmartPointer<vtkTable>::New();
	se.logTable->DeepCopy(se.table);
	const double nan = std::numeric_limits<double>::quiet_NaN();
	for (vtkIdType r = 0; r < se.table->GetNumberOfRows(); ++r) {
		const double vx = se.table->GetValue(r, xc).ToDouble(), vy = se.table->GetValue(r, yc).ToDouble();
		if (!std::isfinite(vx) || !std::isfinite(vy)) continue;          // already a break
		if ((lx && !(vx > 0.0)) || (ly && !(vy > 0.0))) {
			se.logTable->SetValue(r, xc, nan);
			se.logTable->SetValue(r, yc, nan);
			++se.logDropped;
		}
	}
	se.plot->SetInputData(se.logTable, xc, yc);
}

static void xyApplyStyle(XYPlot *s, XYSeries &se) {
	if (!se.plot)
		return;
	se.plot->SetColor((unsigned char)(se.r * 255), (unsigned char)(se.g * 255),
	                  (unsigned char)(se.b * 255), 255);
	se.plot->SetLegendVisibility(se.inLegend);
	// vtkPlotBar::SetWidth is the BAR WIDTH in x data units, not a pen thickness -- feeding it
	// `se.width` (px) would blow a histogram's bars up to the whole axis. The outline pen keeps
	// `se.width`, so both stay independently controllable.
	if (vtkPlotBar *pb = vtkPlotBar::SafeDownCast(se.plot)) {
		pb->SetOrientation(se.barH ? vtkPlotBar::HORIZONTAL : vtkPlotBar::VERTICAL);
		pb->SetWidth((float)se.barWidth);
		if (pb->GetPen()) pb->GetPen()->SetWidth((float)se.width);
	}
	else {
		se.plot->SetWidth((float)se.width);
	}
	if (se.plot->GetPen())
		se.plot->GetPen()->SetLineType(se.lineType);
	// An area is filled in its own colour, see-through so areas over areas show; a bar solid.
	if ((se.kind == 2 || (se.kind >= 6 && se.kind <= 8) || se.kind == 10 || se.kind == 11) && se.plot->GetBrush())
		se.plot->GetBrush()->SetColor((unsigned char)(se.r * 255), (unsigned char)(se.g * 255),
		                              (unsigned char)(se.b * 255), se.kind == 2 || se.kind == 10 ? 150 : se.kind == 8 ? 110 : 255);
	if (vtkXYShapePlot *sp = vtkXYShapePlot::SafeDownCast(se.plot)) {
		sp->Base = se.base;
		sp->BarW = se.barWidth;
		xyShapeExtras(s, se, sp);
		if (se.kind == 6 || se.kind == 7) {
			if (se.kind == 7) sp->SetInputData(se.table, 1, 0);   // value on X, position on Y
			// The data extent the axes must show: every bar's base and top, every bar's full width.
			sp->VLo = sp->PLo = std::numeric_limits<double>::max();
			sp->VHi = sp->PHi = -sp->VLo;
			const int n = se.table ? (int)se.table->GetNumberOfRows() : 0;
			for (int i = 0; i < n; ++i) {
				const double x = se.table->GetValue(i, 0).ToDouble(), y = se.table->GetValue(i, 1).ToDouble();
				const double b = i < (int)se.base.size() ? se.base[i] : 0.0;
				if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(b)) continue;
				sp->VLo = std::min({ sp->VLo, b, b + y }); sp->VHi = std::max({ sp->VHi, b, b + y });
				sp->PLo = std::min(sp->PLo, x - se.barWidth / 2); sp->PHi = std::max(sp->PHi, x + se.barWidth / 2);
			}
			if (sp->VLo > sp->VHi) sp->VLo = sp->VHi = sp->PLo = sp->PHi = 0.0;
		}
		sp->Modified();
	}
	if (vtkPlotPoints *pp = vtkPlotPoints::SafeDownCast(se.plot)) {
		const double dpr = (s && s->widget) ? s->widget->devicePixelRatioF() : 1.0;
		// a stem has a head, a scatter has its points
		pp->SetMarkerStyle((se.kind == 3 || se.kind == 5) && se.marker == 0 ? vtkPlotPoints::CIRCLE : se.marker);
		pp->SetMarkerSize((float)(se.markerSize * dpr));
	}
	xyLogInput(se);
}

// Recompute a "now cross" series' 5-point path (H arm, NaN gap, V arm) from the CURRENT axis pixel
// scale, so its on-screen half-length is really `crossSizePx` on BOTH axes (equal arms) no matter
// how different the X and Y data ranges are (e.g. epoch-seconds vs metres) or how far zoomed in/out
// the chart is. `vtkAxis::GetPoint1()/GetPoint2()` give the axis's onscreen pixel endpoints;
// dividing the data range by that pixel span gives data-per-pixel, independently for each axis.
// Returns true if the geometry actually changed (caller only needs to re-render then).
static bool xyRecomputeCross(XYPage &pg, XYSeries &se) {
	if (!se.nowCross || !se.table || !pg.chart)
		return false;
	vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM);
	vtkAxis *ay = pg.chart->GetAxis(vtkAxis::LEFT);
	if (!ax || !ay)
		return false;
	float *p1 = ax->GetPoint1(); float *p2 = ax->GetPoint2();
	float *q1 = ay->GetPoint1(); float *q2 = ay->GetPoint2();
	const double pxSpanX = std::abs((double)p2[0] - (double)p1[0]);
	const double pxSpanY = std::abs((double)q2[1] - (double)q1[1]);
	const double dataSpanX = ax->GetMaximum() - ax->GetMinimum();
	const double dataSpanY = ay->GetMaximum() - ay->GetMinimum();
	if (pxSpanX <= 0.0 || pxSpanY <= 0.0 || !(dataSpanX > 0.0) || !(dataSpanY > 0.0))
		return false;
	const double hx = se.crossSizePx * (dataSpanX / pxSpanX);
	const double hy = se.crossSizePx * (dataSpanY / pxSpanY);
	const double tolX = 1e-6 * std::max(1.0, std::abs(hx));
	const double tolY = 1e-6 * std::max(1.0, std::abs(hy));
	if (std::abs(hx - se.lastHx) < tolX && std::abs(hy - se.lastHy) < tolY)
		return false;
	se.lastHx = hx; se.lastHy = hy;
	const double nan = std::numeric_limits<double>::quiet_NaN();
	se.table->SetValue(0, 0, se.crossCenterX - hx); se.table->SetValue(0, 1, se.crossCenterY);
	se.table->SetValue(1, 0, se.crossCenterX + hx); se.table->SetValue(1, 1, se.crossCenterY);
	se.table->SetValue(2, 0, nan);                  se.table->SetValue(2, 1, nan);
	se.table->SetValue(3, 0, se.crossCenterX);      se.table->SetValue(3, 1, se.crossCenterY - hy);
	se.table->SetValue(4, 0, se.crossCenterX);      se.table->SetValue(4, 1, se.crossCenterY + hy);
	se.table->Modified();
	return true;
}

// Push infoFontPt onto the header-strip QLabel's stylesheet. Called at creation and whenever the
// Object Manager's "Info" row > Font size… changes it.
static void xyApplyInfoStyle(XYPlot *s) {
	if (!s || !s->infoLabel)
		return;
	s->infoLabel->setStyleSheet(QString("font-size: %1pt; padding: 2px;").arg(s->infoFontPt));
}

// (Re)build the current page's chart plot items from its series tables. Called after a delete
// (plot indices shift, so it is simplest to clear + re-add all). Incremental adds add one plot
// directly. Re-applies style + label + visibility.
static void xyRebuildPlots(XYPlot *s) {
	XYPage &pg = xyCur(s);
	pg.chart->ClearPlots();
	xyRestack(pg);                                   // a delete changes what the stacked ones stand on
	for (auto &se : pg.series) {
		vtkPlot *pl = xyAddPlotItem(pg.chart, se);
		pl->SetInputData(se.table, 0, 1);
		pl->SetLabel(se.name);
		pl->SetTooltipLabelFormat("%x, %y");   // hover shows ONLY x,y (not the series name)
		pl->SetVisible(se.visible);
		se.plot = pl;
		xyApplyStyle(s, se);
	}
	// ClearPlots took the Spector-Grant lines too: the live one is dropped, the kept ones re-plotted.
	pg.sgFit = nullptr;
	pg.sgFitTable = nullptr;
	for (XYSGLine &l : pg.sgLines) {
		l.plot = pg.chart->AddPlot(vtkChart::LINE);
		l.plot->SetInputData(l.table, 0, 1);
		l.plot->SetColor((unsigned char)(l.r * 255), (unsigned char)(l.g * 255), (unsigned char)(l.bl * 255), 255);
		l.plot->SetWidth(2.5f);
		l.plot->SetLabel("S&G fit");
		l.plot->SetVisible(pg.sgActive);
	}
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// The parked tool windows' strip at the BOTTOM EDGE of the Object Manager panel — its own list UNDER the
// series tree (as the 3-D viewer's Scene Objects keeps its parked tools), so it stays at the bottom however
// long the series list grows. Row UserRole = XYPlot::parkedTools index. Hidden when nothing is parked.
static void xyRebuildParkStrip(XYPlot *s) {
	if (!s->parkStrip) return;
	s->parkStrip->clear();
	for (int k = 0; k < (int)s->parkedTools.size(); ++k) {
		QTreeWidgetItem *it = new QTreeWidgetItem(s->parkStrip);
		it->setText(0, s->parkedTools[k].label);
		QFont f = it->font(0); f.setItalic(true); it->setFont(0, f);
		it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
		it->setCheckState(0, Qt::Unchecked);
		it->setData(0, Qt::UserRole, k);
		it->setToolTip(0, "Parked — double-click or check it to bring it back, right-click for its menu");
	}
	const int n = s->parkStrip->topLevelItemCount();
	int h = 4;                                        // exactly as tall as its rows: no dead space
	for (int i = 0; i < n; ++i) h += s->parkStrip->visualItemRect(s->parkStrip->topLevelItem(i)).height();
	if (n > 0 && h <= 4) h += n * s->parkStrip->fontMetrics().height() + 6;   // not laid out yet
	s->parkStrip->setFixedHeight(h);
	s->parkStrip->setVisible(n > 0);
	if (n > 0 && s->omUnfold) s->omUnfold();          // a handle nobody can see is no handle
}

static XYPlot::ParkedTool *xyParkedOfRow(XYPlot *s, QTreeWidgetItem *it) {
	const int k = it ? it->data(0, Qt::UserRole).toInt() : -1;
	return (k >= 0 && k < (int)s->parkedTools.size()) ? &s->parkedTools[k] : nullptr;
}

// Rebuild the Object Manager tree from the CURRENT page's series list (one checkable row each,
// foreground tinted to the series colour, UserRole = series index).
static void xyRebuildObjMgr(XYPlot *s) {
	if (!s->objMgr)
		return;
	std::vector<XYSeries> &series = xyCur(s).series;
	// Remember which series was current so a rebuild (add/delete/rename) doesn't drop the selection —
	// Analysis ops run on the CURRENT row, so there must always be one once any series exists.
	QTreeWidgetItem *cur = s->objMgr->currentItem();
	const int prevSel = cur ? cur->data(0, Qt::UserRole).toInt() : -1;
	s->rebuilding = true;
	s->objMgr->clear();
	// "Info" row (UserRole -1, never a real series index): only shown once something has actually
	// been set via gmtvtk_xyplot_set_info (Tide tool's Next High/Now/Next Low strip). Checkbox
	// toggles the header strip's visibility; right-click gives Font size…/Remove (the strip's own
	// hide/kill handle, since it isn't a chart series and has no other UI otherwise).
	if (s->infoLabel && !s->infoLabel->text().isEmpty()) {
		QTreeWidgetItem *it = new QTreeWidgetItem(s->objMgr);
		it->setText(0, "Info");
		QFont f = it->font(0); f.setItalic(true); it->setFont(0, f);
		it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
		it->setCheckState(0, s->infoLabel->isVisible() ? Qt::Checked : Qt::Unchecked);
		it->setData(0, Qt::UserRole, -1);
	}
	// "Legend" row (UserRole -2, never a real series index): the chart legend's own handle, replacing
	// the old "Show legend" toolbar/menu toggle. Checkbox shows/hides it; right-click gives Font size…
	// (same live-apply pattern as the Info row's Font size…). Only shown once the page HAS a series:
	// a legend of nothing is not a handle to anything, so an empty page's Object Manager is empty --
	// same rule as the Info row above, which only appears once something set it.
	if (!series.empty()) {
		QTreeWidgetItem *it = new QTreeWidgetItem(s->objMgr);
		it->setText(0, "Legend");
		QFont f = it->font(0); f.setItalic(true); it->setFont(0, f);
		it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
		it->setCheckState(0, xyCur(s).chart->GetShowLegend() ? Qt::Checked : Qt::Unchecked);
		it->setData(0, Qt::UserRole, -2);
	}
	// "Axes" row (UserRole -3): the page's own axes, as the 3-D viewer's Scene Objects has one per raster.
	// Checkbox draws / hides them; clicking the label (or right-click > Properties…) opens their Axes
	// properties. Not on a page that draws its own frame (radar, polar, pie): it has no chart axes.
	if (!series.empty() && !xyCur(s).axesHidden) {
		QTreeWidgetItem *it = new QTreeWidgetItem(s->objMgr);
		it->setText(0, "Axes");
		QFont f = it->font(0); f.setItalic(true); it->setFont(0, f);
		it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
		it->setCheckState(0, xyCur(s).axesShown ? Qt::Checked : Qt::Unchecked);
		it->setData(0, Qt::UserRole, -3);
		it->setToolTip(0, "This page's axes · click the label for its Axes properties");
	}
	QTreeWidgetItem *toSelect = nullptr;
	for (int i = 0; i < (int)series.size(); ++i) {
		const XYSeries &se = series[i];
		QTreeWidgetItem *it = new QTreeWidgetItem(s->objMgr);
		it->setText(0, QString::fromStdString(se.name));
		it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
		it->setCheckState(0, se.visible ? Qt::Checked : Qt::Unchecked);
		it->setForeground(0, QColor((int)(se.r * 255), (int)(se.g * 255), (int)(se.b * 255)));
		it->setData(0, Qt::UserRole, i);
		if (i == prevSel) toSelect = it;               // restore the prior selection if it survived
		if (!toSelect && i == (int)series.size() - 1) toSelect = it;  // else default to the last
	}
	// "Annotations" group (UserRole -4) and one child per annotation (UserRole -1000 - i): every drawn
	// annotation has its handle — find it, hide it, open its properties, delete it. The group starts
	// folded; its box shows / hides every child.
	const std::vector<XYAnnot> &an = xyCur(s).annots;
	s->objMgr->setRootIsDecorated(!an.empty());          // the group's fold arrow
	if (!an.empty()) {
		int nvis = 0;
		for (const XYAnnot &a : an) nvis += a.visible ? 1 : 0;
		QTreeWidgetItem *grp = new QTreeWidgetItem(s->objMgr);
		grp->setText(0, "Annotations");
		QFont f = grp->font(0); f.setItalic(true); grp->setFont(0, f);
		grp->setFlags(grp->flags() | Qt::ItemIsUserCheckable);
		grp->setCheckState(0, nvis == 0 ? Qt::Unchecked : nvis == (int)an.size() ? Qt::Checked : Qt::PartiallyChecked);
		grp->setData(0, Qt::UserRole, -4);
		grp->setToolTip(0, "The annotations drawn on this page · the box shows / hides them all");
		for (int i = 0; i < (int)an.size(); ++i) {
			QTreeWidgetItem *c = new QTreeWidgetItem(grp);
			QString t = xyAnnotNames[an[i].kind];
			if (xyAnnotHasText(an[i].kind) && !an[i].text.empty()) t += ": " + QString::fromStdString(an[i].text);
			c->setText(0, t);
			c->setFlags(c->flags() | Qt::ItemIsUserCheckable);
			c->setCheckState(0, (an[i].visible && nvis > 0) ? Qt::Checked : Qt::Unchecked);
			c->setForeground(0, QColor((int)(an[i].r * 255), (int)(an[i].g * 255), (int)(an[i].b * 255)));
			c->setData(0, Qt::UserRole, -1000 - i);
		}
		grp->setExpanded(xyCur(s).annotOpen);
	}
	s->rebuilding = false;
	// The Tools row acts on what the page shows: nothing to act on before it has data.
	if (s->toolsBar) {
		const bool on = !series.empty();
		for (QAction *a : s->toolsBar->actions()) a->setEnabled(on);
		if (!on && s->annotTool >= 0) {                 // an armed annotation tool goes off with it
			for (QAction *a : s->actAnnot) if (a && a->isChecked()) { QSignalBlocker b(a); a->setChecked(false); }
			s->annotTool = -1;
			if (s->widget) s->widget->unsetCursor();
		}
	}
	if (toSelect)
		s->objMgr->setCurrentItem(toSelect);            // guarantee a current row for Analysis
}

// Save one series' (x,y) data to a file via GMT.gmtwrite (extension picks the format). THE ONE
// shared "save this series" path: Line Properties' Save button, the Object Manager's "Save data…"
// action, and the Show-in-Data-Table dialog's Save button all call this SAME function.
static void xySaveSeries(XYPlot *s, int idx) {
	if (!g_juliaXY) { xyLog(s, "Save: not wired (rebuild the DLL + restart Julia)", true); return; }
	const QString fn = QFileDialog::getSaveFileName(s->win, "Save series", prefStartDir("series.dat"),
		"Text (*.dat *.txt);;CSV (*.csv);;All files (*)");
	if (fn.isEmpty()) return;
	rememberStartDir(fn);
	g_juliaXY(s, "save", idx, fn.toUtf8().constData());
	xyLog(s, QString("Saved series #%1 to %2").arg(idx).arg(fn));
}

// Ctrl+V / File > Paste in an X,Y window: the clipboard's numeric text becomes new series on the
// CURRENT page. The text goes over as the "paste" action of the SAME File callback File > Open uses
// (xyplot.jl `_on_xy`), and Julia parses it with the SAME table parser the 3-D viewer's paste uses
// — one clipboard-table reader for the whole app, never a second one here (SACRED_LAW.md).
static void xyPasteClipboard(XYPlot *s) {
	if (!xyAlive(s) || !s->win) return;
	const QString t = QApplication::clipboard()->text();
	if (t.trimmed().isEmpty()) {
		s->win->statusBar()->showMessage("Clipboard has no text to plot", 3000);
		return;
	}
	if (!g_juliaXY) { xyLog(s, "Paste: not wired (rebuild the DLL)", true); return; }
	g_juliaXY(s, "paste", -1, t.toUtf8().constData());
}

// "Show in Data Table" — every series (line handle) carries this property in its right-click menu
// (xyObjMgrMenu). Replaces the old permanently-docked Data Viewer spreadsheet. Calls the SAME
// buildDataTableDialog (55_lineprops.cpp) the 3-D viewer's per-line "Show data table…" uses, Save
// button included (xySaveSeries above) — SACRED_LAW: one "pop a table of this object's data"
// operation, one function, never a second, feature-short reimplementation here.
static void xyShowDataTable(XYPlot *s, int idx) {
	std::vector<XYSeries> &series = xyCur(s).series;
	if (idx < 0 || idx >= (int)series.size())
		return;
	const XYSeries &se = series[idx];
	vtkTable *t = se.table;
	const int nrows = t ? (int)t->GetNumberOfRows() : 0;

	// The columns the points came from (XYSeries::srcCols) first, then the series' own X and Y.
	const std::vector<std::vector<double>> src = se.srcCols;
	const int ns = (int)src.size();
	QStringList hdr;  hdr << "#";
	for (const std::string &n : se.srcNames) hdr << QString::fromStdString(n);
	hdr << QString::fromStdString(se.xName.empty() ? "X" : se.xName)
	    << QString::fromStdString(se.yName.empty() ? se.name : se.yName);
	buildDataTableDialog(QString::fromStdString(se.name) + " — data", nrows, hdr,
		[t, src, ns](int row, int col) {
			const double v = col < ns ? (row < (int)src[col].size() ? src[col][row] : NAN)
			                          : t->GetValue(row, col - ns).ToDouble();
			return QString::number(v, 'g', 10);
		},
		/*editable=*/false, [s, idx]() { xySaveSeries(s, idx); });
}

// The bar width a series' abscissa asks for: 0.9 of the MEDIAN |dx| (not the mean, and not the full
// span / n — the median survives a duplicate or a gap), leaving a hairline between neighbouring bars.
// 0 when there is no spacing at all (one point, or all x equal). THE one bar-width rule: a series born
// as bars (xyAddSeries) and a line turned into bars (the plot-type gallery) both get it from here.
static double xyAutoBarWidth(const double *x, int n) {
	std::vector<double> dx;
	dx.reserve(n > 1 ? (size_t)n - 1 : 0);
	for (int i = 1; i < n; ++i) {
		const double d = std::abs(x[i] - x[i - 1]);
		if (d > 0.0) dx.push_back(d);
	}
	if (dx.empty())
		return 0.0;
	std::nth_element(dx.begin(), dx.begin() + dx.size() / 2, dx.end());
	return 0.9 * dx[dx.size() / 2];
}

// Append one (x,y) series to the CURRENT page. Returns its index (or -1 on bad input). Renders.
// `lineType` (vtkPen), `marker` (vtkPlotPoints) and `markerSize` set the presentation; pass
// lineType<0 / marker<0 / markerSize<=0 for the defaults (solid line, no marker, size 7).
// `kind` picks the chart plot type (0 = line, 1 = BAR — histogram / bar graph); `barWidth` is the
// bar width in X DATA units (<=0 => 90% of the median x spacing, which is what a histogram wants).
// THE one series-add: every caller (both C exports, the Profile seed) comes through here, so a bar
// series is built, styled, copied between pages and rebuilt by exactly the same code as a line.
static int xyAddSeries(XYPlot *s, const double *x, const double *y, int n,
                       const char *name, double r, double g, double b, double width,
                       int lineType, int marker, double markerSize,
                       int kind = 0, double barWidth = 0.0) {
	if (!xyAlive(s) || !x || !y || n < 1)
		return -1;
	XYPage &pg = xyCur(s);
	XYSeries se;
	se.name  = (name && name[0]) ? name : ("Line " + std::to_string((int)pg.series.size() + 1));
	if (r >= 0.0) {
		se.r = r; se.g = g; se.b = b;             // explicit colour
	}
	else {
		const double *c = xyPalette((int)pg.series.size());   // cycle the default palette
		se.r = c[0]; se.g = c[1]; se.b = c[2];
	}
	if (width > 0.0)      se.width = width;
	if (lineType >= 0)    se.lineType = lineType;
	if (marker >= 0)      se.marker = marker;
	if (markerSize > 0.0) se.markerSize = markerSize;
	se.kind = (kind == 1) ? 1 : 0;
	if (se.kind == 1) {
		// No caller-supplied bar width: derive it from the data itself. The MEDIAN |dx| (not the mean,
		// and not the full span / n) survives a duplicate or a gap in the abscissa; 0.9 of it leaves a
		// hairline between neighbouring bars. A single point has no spacing at all -> keep the 1.0.
		if (barWidth > 0.0) {
			se.barWidth = barWidth;
		}
		else if (n > 1) {
			const double w = xyAutoBarWidth(x, n);
			if (w > 0.0) se.barWidth = w;
		}
	}

	se.table = vtkSmartPointer<vtkTable>::New();
	// X is DOUBLE: epoch-seconds time values (~1.6e9) lose ~128 s of resolution in float32, which
	// collapses finely-sampled series (e.g. minute-spaced tides) onto duplicate X. Y stays float.
	// The columns are named "X" and "Y", never after the series: a series named "X" made two columns
	// called "X", and vtkPlot::SetInputData crashed on it. The series' name lives in se.name / SetLabel.
	vtkNew<vtkDoubleArray> ax; ax->SetName("X");                      se.table->AddColumn(ax);
	vtkNew<vtkFloatArray> ay; ay->SetName("Y");                       se.table->AddColumn(ay);
	se.table->SetNumberOfRows(n);
	for (int i = 0; i < n; ++i) {
		se.table->SetValue(i, 0, x[i]);
		se.table->SetValue(i, 1, (float)y[i]);
	}

	vtkPlot *pl = xyAddPlotItem(pg.chart, se);
	pl->SetInputData(se.table, 0, 1);
	pl->SetLabel(se.name);
	pl->SetTooltipLabelFormat("%x, %y");   // hover shows ONLY x,y (not the series name)
	se.plot = pl;
	xyApplyStyle(s, se);

	const int idx = (int)pg.series.size();
	pg.series.push_back(se);
	xyRebuildObjMgr(s);
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
	return idx;
}

// vtkPlotLine subclass whose LEGEND ICON is drawn as a "+" instead of vtkPlotLine's normal
// straight-line sample -- which is what a plain vtkPlotLine legend shows when MarkerStyle is NONE
// (our case: the on-chart cross is hand-drawn NaN-broken geometry, not a VTK marker glyph -- see
// xyRecomputeCross's comment for why). ONLY PaintLegend is overridden; on-chart rendering (Paint)
// is untouched/inherited from vtkPlotLine, so the actual cross geometry is unaffected.
class vtkNowCrossPlot : public vtkPlotLine {
public:
	static vtkNowCrossPlot *New();
	vtkTypeMacro(vtkNowCrossPlot, vtkPlotLine);
	bool PaintLegend(vtkContext2D *painter, const vtkRectf &rect, int) override {
		painter->ApplyPen(this->Pen);
		const float cx = rect[0] + 0.5f * rect[2];
		const float cy = rect[1] + 0.5f * rect[3];
		const float h  = std::min(rect[2], rect[3]) * 0.42f;
		painter->DrawLine(cx - h, cy, cx + h, cy);
		painter->DrawLine(cx, cy - h, cx, cy + h);
		return true;
	}
};
vtkStandardNewMacro(vtkNowCrossPlot);

// Append a screen-constant "+" cross (e.g. the Tide tool's "Now" indicator). A 5-point NaN-broken
// path (H arm, gap, V arm) on a vtkNowCrossPlot (so the LEGEND matches what's on the chart -- see
// above), flagged `nowCross` with its true centre + target on-screen size stored -- xyRecomputeCross
// (called from xyTicksOnRender on every render) then rescales it to the CURRENT per-axis pixel
// scale, so it renders as a real equal-armed "+" of `sizePx` on screen regardless of the X/Y
// data-range mismatch, and keeps that shape across zoom/pan/resize. `widthPx` is the stroke
// thickness -- an everyday vtkPlotLine width, so it's genuinely, independently controllable (unlike
// a vtkPlotPoints marker's size/width, which VTK itself conflates into one parameter).
static int xyAddNowCross(XYPlot *s, double cx, double cy, double r, double g, double b,
                         double sizePx, double widthPx, const char *name) {
	if (!xyAlive(s))
		return -1;
	XYPage &pg = xyCur(s);
	XYSeries se;
	se.name = (name && name[0]) ? name : "Now";
	se.r = r; se.g = g; se.b = b;
	if (widthPx > 0.0) se.width = widthPx;
	se.marker = 0;                                   // no vtkPlotPoints marker glyph on the chart
	se.nowCross = true;
	se.crossCenterX = cx; se.crossCenterY = cy;
	se.crossSizePx = sizePx > 0.0 ? sizePx : 24.0;

	se.table = vtkSmartPointer<vtkTable>::New();
	vtkNew<vtkDoubleArray> ax; ax->SetName("X");             se.table->AddColumn(ax);
	vtkNew<vtkFloatArray> ay; ay->SetName("Y");              se.table->AddColumn(ay);   // see xyAddSeries
	se.table->SetNumberOfRows(5);
	// Rough placeholder path -- a real cross only exists once xyRecomputeCross has real axis pixel
	// spans to work from, which needs the chart to have actually laid out at least once.
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double gx[5] = { cx - 1.0, cx + 1.0, nan, cx, cx };
	const double gy[5] = { cy, cy, nan, cy - 1.0, cy + 1.0 };
	for (int i = 0; i < 5; ++i) { se.table->SetValue(i, 0, gx[i]); se.table->SetValue(i, 1, (float)gy[i]); }

	vtkNew<vtkNowCrossPlot> plot;
	pg.chart->AddPlot(plot);
	plot->SetInputData(se.table, 0, 1);
	plot->SetLabel(se.name);
	plot->SetTooltipLabelFormat("%x, %y");
	se.plot = plot;
	xyApplyStyle(s, se);

	const int idx = (int)pg.series.size();
	pg.series.push_back(se);
	xyRebuildObjMgr(s);

	XYSeries &st = pg.series[idx];                    // recompute the STORED copy, not the local `se`
	if (xyRecomputeCross(pg, st) && s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
	else if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();          // still show the placeholder path immediately
	return idx;
}

// Drop every series on the current page.
static void xyClear(XYPlot *s) {
	if (!xyAlive(s))
		return;
	XYPage &pg = xyCur(s);
	pg.chart->ClearPlots();
	pg.series.clear();
	pg.sgLines.clear();                    // ClearPlots took their plots
	pg.refLine[0] = pg.refLine[1] = -1;    // the reference picks went with their curves
	pg.refRows[0].clear(); pg.refRows[1].clear(); pg.refPicking = false;
	pg.sgFit = nullptr;
	pg.sgFitTable = nullptr;
	pg.markSel = -1;                       // its curve is gone
	if (s->curPageIdx == s->markLinkPage) s->markLinkSel = -1;
	xyRebuildObjMgr(s);
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Delete the series at idx on the current page (rebuilds plots + the tree so indices re-sync).
static void xyDeleteSeries(XYPlot *s, int idx) {
	std::vector<XYSeries> &series = xyCur(s).series;
	if (idx < 0 || idx >= (int)series.size())
		return;
	series.erase(series.begin() + idx);
	int &ms = xyCur(s).markSel;            // the marker stays on ITS curve, or goes with it
	if (ms == idx) ms = -1;
	else if (ms > idx) --ms;
	if (s->curPageIdx == s->markLinkPage) {        // …and so does the Profile link
		if (s->markLinkSel == idx)     s->markLinkSel = -1;
		else if (s->markLinkSel > idx) --s->markLinkSel;
	}
	XYPage &rp = xyCur(s);                         // the reference picks follow their curves, or go with them
	for (int k = 0; k < 2; ++k) {
		if (rp.refLine[k] == idx) { rp.refLine[0] = rp.refLine[1] = -1; rp.refRows[0].clear(); rp.refRows[1].clear(); rp.refPicking = false; break; }
		if (rp.refLine[k] > idx) --rp.refLine[k];
	}
	xyRebuildPlots(s);
	xyRebuildObjMgr(s);
}

// (defined below) — needed here for the time-mode status-bar readout.
static QString xyFmtTimeHover(double t, int fmt);

// "x,  y" as this window prints a data point — ONE formatter for the status readout, the hover
// tooltip and the curve marker's label. `fmt` is the page's X time mode (0 = plain number).
static QString xyCoordText(int fmt, double x, double y) {
	const QString xs = fmt ? xyFmtTimeHover(x, fmt) : QString("%1").arg(x, 0, 'g', 8);
	return QString("%1,  %2").arg(xs).arg(y, 0, 'g', 6);
}

// Live coordinate readout: map the cursor (interactor event px, bottom-up) into
// data coords via the current page chart's bottom/left axes, and show "x, y".
static void xyMouseMove(vtkObject *caller, unsigned long, void *clientData, void *) {
	XYPlot *s = static_cast<XYPlot*>(clientData);
	vtkRenderWindowInteractor *rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
	if (!xyAlive(s) || !rwi || !s->win)
		return;
	XYPage &pg = xyCur(s);
	vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM);
	vtkAxis *ay = pg.chart->GetAxis(vtkAxis::LEFT);
	if (!ax || !ay)
		return;
	const int *ep = rwi->GetEventPosition();
	const double px = ep[0], py = ep[1];
	float *a1 = ax->GetPoint1(), *a2 = ax->GetPoint2();   // bottom axis: left/right ends (screen px)
	float *b1 = ay->GetPoint1(), *b2 = ay->GetPoint2();   // left axis: bottom/top ends
	const double wpx = a2[0] - a1[0], hpx = b2[1] - b1[1];
	if (wpx == 0.0 || hpx == 0.0)
		return;
	const double fx = (px - a1[0]) / wpx, fy = (py - b1[1]) / hpx;
	if (fx < 0.0 || fx > 1.0 || fy < 0.0 || fy > 1.0) {   // outside the plot frame
		s->win->statusBar()->clearMessage();
		return;
	}
	const double dx = ax->GetMinimum() + fx * (ax->GetMaximum() - ax->GetMinimum());
	const double dy = ay->GetMinimum() + fy * (ay->GetMaximum() - ay->GetMinimum());
	s->win->statusBar()->showMessage(xyCoordText(pg.xTimeFmt, dx, dy));
}
// A page's axes: its Axes properties dialog and their apply (below, with the page look).
static void xyAxesPropsOpen(XYPlot *s);
static void xyApplyAxes(XYPlot *s, XYPage &pg);

// Where an Object Manager row's LABEL starts (viewport x): past its check indicator. What tells a click
// on the "Axes" row's label (open its properties) from one on its checkbox (draw / hide).
static int xyObjMgrLabelX(XYPlot *s, QTreeWidgetItem *it) {
	const QRect r = s->objMgr->visualItemRect(it);
	return r.left() + s->objMgr->style()->pixelMetric(QStyle::PM_IndicatorWidth) + 6;
}


// Live per-series line-properties dialog: colour / width / line style / marker / marker size. Each
// control applies immediately to the plot + records on the XYSeries (so the look survives a later
// delete-rebuild), then re-renders. Modal (no series add/remove happens while it is open, so the
// captured `se` reference stays valid).
static void xyLineProperties(XYPlot *s, int idx) {
	std::vector<XYSeries> &series = xyCur(s).series;
	if (idx < 0 || idx >= (int)series.size())
		return;
	XYSeries &se = series[idx];

	QDialog dlg(s->win);
	dlg.setWindowTitle(QString("%1 properties — %2")
		.arg(se.kind == 1 ? "Bar" : "Line", QString::fromStdString(se.name)));
	QFormLayout *form = new QFormLayout(&dlg);
	auto rr = [s] { if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render(); };

	QPushButton *colBtn = new QPushButton(&dlg);
	auto swatch = [&se, colBtn] {
		colBtn->setStyleSheet(QString("background:%1").arg(
			QColor((int)(se.r * 255), (int)(se.g * 255), (int)(se.b * 255)).name()));
	};
	swatch();
	form->addRow("Colour", colBtn);
	QObject::connect(colBtn, &QPushButton::clicked, &dlg, [&, swatch, rr] {
		QColor c = QColorDialog::getColor(QColor((int)(se.r * 255), (int)(se.g * 255), (int)(se.b * 255)),
		                                  &dlg, "Line colour");
		if (!c.isValid()) return;
		se.r = c.redF(); se.g = c.greenF(); se.b = c.blueF();
		xyApplyStyle(s, se); swatch(); xyRebuildObjMgr(s); rr();
	});

	// A "now cross" is created/sized in POINTS (xynowcross!'s `size`/`width`, 96/72 dpi conversion,
	// same convention `add_symbols!` uses -- physical, DPI-independent, never raw screen px). The
	// dialog must speak the SAME unit the caller set it in, or editing it live silently changes the
	// number's meaning underneath the user. `se.width`/`se.crossSizePx` stay stored in px internally
	// (what VTK/xyRecomputeCross actually consume) -- only the DISPLAYED/EDITED value converts.
	constexpr double PX_PER_PT = 96.0 / 72.0;
	QDoubleSpinBox *wsp = new QDoubleSpinBox(&dlg);
	if (se.nowCross) {
		wsp->setRange(0.25, 20.0); wsp->setSingleStep(0.25);
		wsp->setValue(se.width / PX_PER_PT);
		form->addRow("Width (pt)", wsp);
		QObject::connect(wsp, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg,
			[&, rr](double v) { se.width = v * PX_PER_PT; xyApplyStyle(s, se); rr(); });
	} else {
		wsp->setRange(0.5, 12.0); wsp->setSingleStep(0.5); wsp->setValue(se.width);
		form->addRow(se.kind == 1 ? "Outline (px)" : "Width (px)", wsp);
		QObject::connect(wsp, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg,
			[&, rr](double v) { se.width = v; xyApplyStyle(s, se); rr(); });
	}

	// A BAR series' own width, in X DATA units (a histogram's bin span) -- a different quantity from
	// the pen thickness above, so it gets its own row rather than overloading that one. Wide range +
	// generous decimals: the abscissa can be anything from 1e-6 (a wavenumber) to 1e9 (epoch seconds).
	if (se.kind == 1) {
		QDoubleSpinBox *bsp = new QDoubleSpinBox(&dlg);
		bsp->setDecimals(6); bsp->setRange(1e-9, 1e12);
		bsp->setSingleStep(std::max(1e-9, se.barWidth / 10.0));
		bsp->setValue(se.barWidth);
		form->addRow("Bar width (x units)", bsp);
		QObject::connect(bsp, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg,
			[&, rr](double v) { se.barWidth = v; xyApplyStyle(s, se); rr(); });
	}

	// combo index -> vtkPen line type
	static const int kPen[] = { 1, 2, 3, 4, 5, 0 };  // Solid Dashed Dotted Dash-Dot Dash-Dot-Dot None
	QComboBox *lsc = new QComboBox(&dlg);
	lsc->addItems(QStringList() << "Solid" << "Dashed" << "Dotted" << "Dash-Dot" << "Dash-Dot-Dot" << "None");
	for (int i = 0; i < 6; ++i) if (kPen[i] == se.lineType) lsc->setCurrentIndex(i);
	form->addRow("Line style", lsc);
	QObject::connect(lsc, qOverload<int>(&QComboBox::currentIndexChanged), &dlg,
		[&, rr](int i) { se.lineType = kPen[i]; xyApplyStyle(s, se); rr(); });

	// combo index == vtkPlotPoints marker style (NONE=0 .. DIAMOND=5) -- meaningless for a "now
	// cross" (always a hand-drawn line path, never a vtkPlotPoints marker), so disabled for one.
	QComboBox *mkc = new QComboBox(&dlg);
	mkc->addItems(QStringList() << "None" << "Cross" << "Plus" << "Square" << "Circle" << "Diamond");
	mkc->setCurrentIndex(se.marker);
	// A bar series is a vtkPlotBar, not a vtkPlotPoints: it has no marker glyph at all (same reason
	// the row is off for a "now cross"), so the control is shown-but-dead rather than lying.
	mkc->setEnabled(!se.nowCross && se.kind != 1);
	form->addRow("Marker", mkc);
	QObject::connect(mkc, qOverload<int>(&QComboBox::currentIndexChanged), &dlg,
		[&, rr](int i) { se.marker = i; xyApplyStyle(s, se); rr(); });

	// A "now cross" repurposes this field as its true on-screen half-arm-length target, in POINTS
	// (same PX_PER_PT conversion as Width above) -- xyRecomputeCross re-derives the data-space
	// geometry from the stored px every render; an ordinary series' vtkPlotPoints marker size (px,
	// unconverted) is unaffected.
	QDoubleSpinBox *msp = new QDoubleSpinBox(&dlg);
	if (se.nowCross) {
		msp->setRange(2.0, 150.0);
		msp->setValue(se.crossSizePx / PX_PER_PT);
		form->addRow("Cross size (pt)", msp);
		QObject::connect(msp, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg,
			[&, s, rr](double v) {
				se.crossSizePx = v * PX_PER_PT;
				se.lastHx = -1.0; se.lastHy = -1.0;      // force xyRecomputeCross to apply
				if (xyRecomputeCross(xyCur(s), se)) rr();
			});
	} else {
		msp->setRange(1.0, 200.0);
		msp->setValue(se.markerSize);
		msp->setEnabled(se.kind != 1);
		form->addRow("Marker size", msp);
		QObject::connect(msp, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg,
			[&, rr](double v) { se.markerSize = v; xyApplyStyle(s, se); rr(); });
	}

	// Save THIS series to a file (same Julia gmtwrite path as File>Save, but pre-targeted to idx) --
	// meaningless for a "now cross" (2 synthetic segments, not real data), so left off for one.
	QDialogButtonBox *bb = new QDialogButtonBox(
		se.nowCross ? QDialogButtonBox::Close : (QDialogButtonBox::Save | QDialogButtonBox::Close), &dlg);
	form->addRow(bb);
	if (!se.nowCross) {
		QObject::connect(bb->button(QDialogButtonBox::Save), &QPushButton::clicked, &dlg,
			[s, idx] { xySaveSeries(s, idx); });
	}
	QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
	dlg.exec();
}

// Right-click on the Object Manager: properties / rename / delete the clicked series.
static void xyObjMgrMenu(XYPlot *s, const QPoint &pos) {
	QTreeWidgetItem *it = s->objMgr->itemAt(pos);
	if (!it)
		return;
	s->objMgr->setCurrentItem(it);                     // right-click also PICKS the row (so Analysis /
	                                                   // Save then target the clicked series, not a stale one)
	const int idx = it->data(0, Qt::UserRole).toInt();
	if (idx == -1) {                                   // "Info" row: font size + its own hide/kill handle
		QMenu im(s->objMgr);
		QAction *aFont = im.addAction("Font size…");
		QAction *aHide = im.addAction(s->infoLabel && s->infoLabel->isVisible() ? "Hide" : "Show");
		QAction *aRemove = im.addAction("Remove");
		QAction *ipick = im.exec(s->objMgr->viewport()->mapToGlobal(pos));
		if (ipick == aFont) {
			// Live-apply, not a blocking QInputDialog::getInt -- every spin click/keystroke resizes
			// the strip immediately (same pattern as Line-properties' Width/size fields), a single
			// "Close" button just dismisses (there's nothing left to commit, it's already applied).
			QDialog dlg(s->win);
			dlg.setWindowTitle("Info font size");
			QFormLayout *fl = new QFormLayout(&dlg);
			QSpinBox *fsp = new QSpinBox(&dlg);
			fsp->setRange(6, 36); fsp->setValue((int)s->infoFontPt);
			fl->addRow("Points:", fsp);
			QObject::connect(fsp, qOverload<int>(&QSpinBox::valueChanged), &dlg,
				[s](int v) { s->infoFontPt = v; xyApplyInfoStyle(s); });
			QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
			fl->addRow(bb);
			QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
			QObject::connect(bb, &QDialogButtonBox::clicked, &dlg, &QDialog::accept);
			dlg.exec();
		}
		else if (ipick == aHide && s->infoLabel) {
			s->infoLabel->setVisible(!s->infoLabel->isVisible());
			xyRebuildObjMgr(s);
		}
		else if (ipick == aRemove && s->infoLabel) {
			s->infoLabel->setText(QString());
			s->infoLabel->setVisible(false);
			xyRebuildObjMgr(s);
		}
		return;
	}
	if (idx <= -1000) {                                // one annotation: the same menu as on the chart
		xyAnnotMenu(s, -1000 - idx, s->objMgr->viewport()->mapToGlobal(pos));
		return;
	}
	if (idx == -4) {                                   // the "Annotations" group: all of them at once
		std::vector<XYAnnot> &an = xyCur(s).annots;
		QMenu gm(s->objMgr);
		QAction *aShow = gm.addAction("Show all");
		QAction *aHide = gm.addAction("Hide all");
		QAction *aDel = gm.addAction("Delete all");
		QAction *gpick = gm.exec(s->objMgr->viewport()->mapToGlobal(pos));
		if (!gpick || !xyAlive(s)) return;
		if (gpick == aDel) { an.clear(); xyCur(s).annotSel = -1; }
		else for (XYAnnot &a : an) a.visible = (gpick == aShow);
		xyRebuildObjMgr(s);
		xyAnnotRender(s);
		return;
	}
	if (idx == -3) {                                   // "Axes" row: its properties + its own show/hide
		QMenu am(s->objMgr);
		QAction *aProps = am.addAction("Properties…");
		QAction *aHide = am.addAction(xyCur(s).axesShown ? "Hide" : "Show");
		QAction *apick = am.exec(s->objMgr->viewport()->mapToGlobal(pos));
		if (apick == aProps) xyAxesPropsOpen(s);
		else if (apick == aHide) {
			xyCur(s).axesShown = !xyCur(s).axesShown;
			xyApplyAxes(s, xyCur(s));
			xyRebuildObjMgr(s);
		}
		return;
	}
	if (idx == -2) {                                   // "Legend" row: font size + its own show/hide
		QMenu lm(s->objMgr);
		QAction *aFont = lm.addAction("Font size…");
		QAction *aHide = lm.addAction(xyCur(s).chart->GetShowLegend() ? "Hide" : "Show");
		QAction *lpick = lm.exec(s->objMgr->viewport()->mapToGlobal(pos));
		if (lpick == aFont) {
			// Live-apply, same pattern as the Info row's Font size… above.
			QDialog dlg(s->win);
			dlg.setWindowTitle("Legend font size");
			QFormLayout *fl = new QFormLayout(&dlg);
			QSpinBox *fsp = new QSpinBox(&dlg);
			fsp->setRange(6, 36); fsp->setValue((int)s->legendFontPt);
			fl->addRow("Points:", fsp);
			QObject::connect(fsp, qOverload<int>(&QSpinBox::valueChanged), &dlg,
				[s](int v) {
					s->legendFontPt = v;
					if (vtkChartLegend *lg = xyCur(s).chart->GetLegend())
						lg->GetLabelProperties()->SetFontSize(v);
					if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
				});
			QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
			fl->addRow(bb);
			QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
			QObject::connect(bb, &QDialogButtonBox::clicked, &dlg, &QDialog::accept);
			dlg.exec();
		}
		else if (lpick == aHide) {
			s->legendVisible = !xyCur(s).chart->GetShowLegend();
			xyCur(s).chart->SetShowLegend(s->legendVisible);
			xyRebuildObjMgr(s);
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		}
		return;
	}
	QMenu m(s->objMgr);
	QAction *aProp = m.addAction("Line properties…");
	QAction *aTable = m.addAction("Show in Data Table");
	QAction *aSave = m.addAction("Save data…");
	QAction *aRen = m.addAction("Rename…");
	QAction *aDel = m.addAction("Delete");
	QAction *pick = m.exec(s->objMgr->viewport()->mapToGlobal(pos));
	std::vector<XYSeries> &series = xyCur(s).series;
	if (pick == aProp) {
		xyLineProperties(s, idx);
	}
	else if (pick == aTable) {
		xyShowDataTable(s, idx);
	}
	else if (pick == aSave) {
		xySaveSeries(s, idx);
	}
	else if (pick == aDel) {
		xyDeleteSeries(s, idx);
	}
	else if (pick == aRen && idx >= 0 && idx < (int)series.size()) {
		bool ok = false;
		const QString nn = QInputDialog::getText(s->win, "Rename series", "Name:",
			QLineEdit::Normal, QString::fromStdString(series[idx].name), &ok);
		if (ok && !nn.isEmpty()) {
			series[idx].name = nn.toStdString();
			if (series[idx].plot) series[idx].plot->SetLabel(nn.toStdString());
			xyRebuildObjMgr(s);
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		}
	}
}

// Small modal dialog for the Butterworth filter: type (low/high) + cutoff frequency. Returns false
// if cancelled; else sets `type` to "low"/"high" and `fc`.
static bool xyAskButter(QWidget *parent, QString &type, double &fc) {
	QDialog d(parent);
	d.setWindowTitle("Butterworth filter");
	QFormLayout *f = new QFormLayout(&d);
	QComboBox *tc = new QComboBox(&d);
	tc->addItems(QStringList() << "Low-pass" << "High-pass");
	QDoubleSpinBox *cs = new QDoubleSpinBox(&d);
	cs->setDecimals(6); cs->setRange(1e-9, 1e12); cs->setValue(0.1);
	f->addRow("Type", tc);
	f->addRow("Cutoff frequency", cs);
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
	f->addRow(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
	if (d.exec() != QDialog::Accepted)
		return false;
	type = (tc->currentIndex() == 0) ? "low" : "high";
	fc = cs->value();
	return true;
}

// Sound Velocity Profile (ecran AnalysisSVP_CB) inputs: three pulldowns over every Data-Viewer column
// (the abscissa = code -1, each series' value column = its index) for Temperature, Salinity and
// Pressure (dbar), plus the latitude the depths are computed at. Each pulldown starts on the first
// column whose name says what it is. Builds "T=<code>:S=<code>:P=<code>:lat=<deg>" for _xy_svp.
static bool xyAskSVP(QWidget *parent, XYPlot *s, QString &args) {
	std::vector<XYSeries> &series = xyCur(s).series;
	QDialog d(parent);
	d.setWindowTitle("Sound Velocity Profile");
	QFormLayout *f = new QFormLayout(&d);
	const char *axt = xyCur(s).chart && xyCur(s).chart->GetAxis(vtkAxis::BOTTOM)
	                      ? xyCur(s).chart->GetAxis(vtkAxis::BOTTOM)->GetTitle().c_str() : nullptr;
	const QString xlab = (axt && *axt) ? QString::fromUtf8(axt) : QStringLiteral("X");
	auto makeCol = [&](const char *hint) {
		QComboBox *cb = new QComboBox(&d);
		cb->addItem(xlab, -1);
		for (int i = 0; i < (int)series.size(); ++i)
			cb->addItem(QString::fromStdString(series[i].name), i);
		for (int k = 0; k < cb->count(); ++k)
			if (cb->itemText(k).contains(hint, Qt::CaseInsensitive)) { cb->setCurrentIndex(k); break; }
		return cb;
	};
	QComboBox *ct = makeCol("temp");
	QComboBox *cs = makeCol("sal");
	QComboBox *cp = makeCol("pres");
	QDoubleSpinBox *lat = new QDoubleSpinBox(&d);
	lat->setDecimals(4); lat->setRange(-90.0, 90.0); lat->setValue(0.0);
	f->addRow("Temperature (\xC2\xB0""C)", ct);
	f->addRow("Salinity", cs);
	f->addRow("Pressure (dbar)", cp);
	f->addRow("Latitude", lat);
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
	f->addRow(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
	if (d.exec() != QDialog::Accepted)
		return false;
	args = QString("T=%1:S=%2:P=%3:lat=%4").arg(ct->currentData().toInt()).arg(cs->currentData().toInt())
	           .arg(cp->currentData().toInt()).arg(lat->value(), 0, 'g', 10);
	return true;
}

// ---- GMT menu: full spectrum1d interface -----------------------------------
// Options dialog for GMT's spectrum1d (auto- and cross-spectra). Two pulldowns pick the input
// series from the current page (the same names shown in the Data Viewer): the 1st series is X(t),
// the 2nd (default "(none)") is Y(t) for cross-spectra. Builds the op-args string
// "S=<seg>:D=<dt>:L=<mode>:C=<flags>:y1=<idx>:y2=<idx>:W=<0|1>" consumed Julia-side by _xy_spectrum1d
// (run through the existing Analysis callback). `sel` = the initially-selected 1st series. Returns
// false if cancelled.
static bool xyAskSpectrum1d(QWidget *parent, XYPlot *s, int sel, QString &args) {
	std::vector<XYSeries> &series = xyCur(s).series;
	QDialog d(parent);
	d.setWindowTitle("spectrum1d — auto / cross spectra");
	QVBoxLayout *top = new QVBoxLayout(&d);
	QFormLayout *f = new QFormLayout();
	top->addLayout(f);

	// Both pulldowns offer EVERY column shown in the Data Viewer: the abscissa (X) column (code -1)
	// AND each series' value column (code = series index). Pick any column for either — no restriction.
	// (code -2 = "(none)", 2nd pulldown only.) The abscissa label = the bottom-axis title.
	const char *axt = xyCur(s).chart && xyCur(s).chart->GetAxis(vtkAxis::BOTTOM)
	                      ? xyCur(s).chart->GetAxis(vtkAxis::BOTTOM)->GetTitle().c_str() : nullptr;
	const QString xlab = (axt && *axt) ? QString::fromUtf8(axt) : QStringLiteral("X");
	auto fillCols = [&](QComboBox *cb, bool withNone) {
		if (withNone) cb->addItem("(none — autospectrum)", -2);
		cb->addItem(xlab, -1);                                  // the abscissa column
		for (int i = 0; i < (int)series.size(); ++i)
			cb->addItem(QString::fromStdString(series[i].name), i);
	};

	// 1st column X(t): defaults to the OM-selected value column.
	QComboBox *y1 = new QComboBox(&d);
	fillCols(y1, false);
	{ const int at = y1->findData(sel); if (at >= 0) y1->setCurrentIndex(at); }
	f->addRow("1st column X(t)", y1);

	// 2nd column Y(t) for cross-spectra; default "(none)" => autospectrum of the 1st column.
	QComboBox *y2 = new QComboBox(&d);
	fillCols(y2, true);
	f->addRow("2nd column Y(t)", y2);

	QSpinBox *seg = new QSpinBox(&d);
	seg->setRange(0, 1 << 24); seg->setValue(0);
	seg->setSpecialValueText("auto (largest 2^n <= N)");
	f->addRow("Segment size", seg);

	QLineEdit *dt = new QLineEdit(&d);
	dt->setPlaceholderText("auto (from X spacing)");
	f->addRow("Sample interval", dt);

	QComboBox *det = new QComboBox(&d);
	det->addItems(QStringList() << "Remove linear trend (default)" << "Leave trend"
	                            << "Remove mean" << "Remove mid-value");
	f->addRow("Detrend", det);

	QCheckBox *wl = new QCheckBox("X axis = wavelength (1/freq)", &d);
	f->addRow("", wl);

	// Output components (-C flags). X power is the autospectrum; the rest need a 2nd series.
	QGroupBox *og = new QGroupBox("Outputs", &d);
	QVBoxLayout *ol = new QVBoxLayout(og);
	QCheckBox *cx = new QCheckBox("X power", og); cx->setChecked(true);
	QCheckBox *cy = new QCheckBox("Y power", og);
	QCheckBox *cc = new QCheckBox("Cross power", og);
	QCheckBox *cn = new QCheckBox("Noise power", og);
	QCheckBox *co = new QCheckBox("Coherency", og);
	QCheckBox *ca = new QCheckBox("Admittance", og);
	QCheckBox *cg = new QCheckBox("Gain", og);
	QCheckBox *cp = new QCheckBox("Phase", og);
	QCheckBox *crossBoxes[] = {cy, cc, cn, co, ca, cg, cp};
	ol->addWidget(cx);
	for (QCheckBox *c : crossBoxes) ol->addWidget(c);
	top->addWidget(og);

	auto syncCross = [=]{
		const bool cross = y2->currentData().toInt() != -2;     // -2 = "(none)"
		for (QCheckBox *c : crossBoxes) c->setEnabled(cross);
	};
	QObject::connect(y2, qOverload<int>(&QComboBox::currentIndexChanged), &d, [syncCross](int){ syncCross(); });
	syncCross();

	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
	top->addWidget(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
	if (d.exec() != QDialog::Accepted)
		return false;

	const int y2i = y2->currentData().toInt();
	const bool cross = y2i != -2;                              // -2 = "(none)" => autospectrum
	QString flags;
	if (cx->isChecked()) flags += 'x';
	if (cross) {
		if (cy->isChecked()) flags += 'y';
		if (cc->isChecked()) flags += 'c';
		if (cn->isChecked()) flags += 'n';
		if (co->isChecked()) flags += 'o';
		if (ca->isChecked()) flags += 'a';
		if (cg->isChecked()) flags += 'g';
		if (cp->isChecked()) flags += 'p';
	}
	if (flags.isEmpty()) flags = "x";
	args = QString("S=%1:D=%2:L=%3:C=%4:y1=%5:y2=%6:W=%7")
	           .arg(seg->value())
	           .arg(dt->text().trimmed())
	           .arg(det->currentIndex())
	           .arg(flags)
	           .arg(y1->currentData().toInt())
	           .arg(y2i)
	           .arg(wl->isChecked() ? 1 : 0);
	return true;
}

// ---- interactive Spector-Grant depth-to-sources (ecran dynSlope) -----------
// Left-drag a frequency band on a (wavenumber, power) spectrum; fit ln(power) vs k live, draw the
// fit line over the band and show depth = |slope|/(4Ï€)·unit in the status bar.

// Ask the wavenumber unit (sets the metres factor) when the tool is switched on.
static bool xyAskFreqUnit(QWidget *parent, double &xf) {
	QStringList items; items << "1/m" << "1/km" << "1/NM";
	bool ok = false;
	const QString it = QInputDialog::getItem(parent, "Spector-Grant", "Frequency unit:", items, 1, false, &ok);
	if (!ok)
		return false;
	xf = (it == "1/m") ? 1.0 : (it == "1/km") ? 1000.0 : 1852.0;
	return true;
}

// Map a Qt widget point to a DATA x value via the current page's bottom axis (linear x only).
static double xySGDataX(XYPlot *s, const QPointF &p) {
	const double px = p.x() * s->widget->devicePixelRatioF();
	vtkAxis *ax = xyCur(s).chart->GetAxis(vtkAxis::BOTTOM);
	float *a1 = ax->GetPoint1(), *a2 = ax->GetPoint2();
	const double w = a2[0] - a1[0];
	if (w == 0.0)
		return ax->GetMinimum();
	return ax->GetMinimum() + (px - a1[0]) / w * (ax->GetMaximum() - ax->GetMinimum());
}

// The fit of ln(power) vs k over band [xa,xb] of series idx (positive power only) on the current page —
// ecran's `trend1d_m(xy, '-N2r')`: GMT's own trend1d, ROBUST (Huber-weighted) when the band holds more
// than two points, run by xyanalysis.jl (`_xy_sgfit`) and handed back through gmtvtk_xyplot_sg_set_fit.
// The plain least squares below is the FALLBACK ONLY, for a window with no Julia side (the demo exe).
static bool xySGFit(XYPlot *s, int idx, double xa, double xb, double unit,
                    double &slope, double &inter, double &depth, double &xlo, double &xhi) {
	std::vector<XYSeries> &series = xyCur(s).series;
	if (idx < 0 || idx >= (int)series.size())
		return false;
	if (xa > xb) std::swap(xa, xb);
	vtkTable *t = series[idx].table;
	if (!t) return false;
	if (g_juliaXYAna) {
		s->sgFitGot = false;
		g_juliaXYAna(s, QString("sgfit:xa=%1:xb=%2").arg(xa, 0, 'g', 17).arg(xb, 0, 'g', 17).toUtf8().constData(), idx);
		if (!xyAlive(s) || !s->sgFitGot) return false;
		slope = s->sgFitM;
		inter = s->sgFitB;
		depth = std::abs(slope) / (4.0 * vtkMath::Pi()) * unit;
		xlo = xa; xhi = xb;
		return true;
	}
	double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
	const vtkIdType nr = t->GetNumberOfRows();
	for (vtkIdType r = 0; r < nr; ++r) {
		const double x = t->GetValue(r, 0).ToDouble();
		const double y = t->GetValue(r, 1).ToDouble();
		if (x < xa || x > xb || !(y > 0.0)) continue;
		const double ly = std::log(y);
		sx += x; sy += ly; sxx += x * x; sxy += x * ly; ++n;
	}
	if (n < 2) return false;
	const double den = n * sxx - sx * sx;
	if (den == 0.0) return false;
	slope = (n * sxy - sx * sy) / den;
	inter = (sy - slope * sx) / n;
	depth = std::abs(slope) / (4.0 * vtkMath::Pi()) * unit;
	xlo = xa; xhi = xb;
	return true;
}

// Recompute the fit for the current drag end and refresh the fit line + status readout.
static void xySGUpdate(XYPlot *s, double x1) {
	XYPage &pg = xyCur(s);
	double slope, inter, depth, xlo, xhi;
	if (!xySGFit(s, pg.sgSel, pg.sgX0, x1, pg.sgUnit, slope, inter, depth, xlo, xhi))
		return;
	if (!pg.sgFitTable) {
		pg.sgFitTable = vtkSmartPointer<vtkTable>::New();
		vtkNew<vtkFloatArray> ax; ax->SetName("k");       pg.sgFitTable->AddColumn(ax);
		vtkNew<vtkFloatArray> ay; ay->SetName("S&G fit"); pg.sgFitTable->AddColumn(ay);
		pg.sgFitTable->SetNumberOfRows(2);
	}
	pg.sgFitTable->SetValue(0, 0, (float)xlo); pg.sgFitTable->SetValue(0, 1, (float)std::exp(slope * xlo + inter));
	pg.sgFitTable->SetValue(1, 0, (float)xhi); pg.sgFitTable->SetValue(1, 1, (float)std::exp(slope * xhi + inter));
	if (!pg.sgFit) {
		pg.sgFit = pg.chart->AddPlot(vtkChart::LINE);
		pg.sgFit->SetColor((unsigned char)(pg.sgR * 255), (unsigned char)(pg.sgG * 255), (unsigned char)(pg.sgB * 255), 255);
		pg.sgFit->SetWidth(2.5f);
		pg.sgFit->SetLabel("S&G fit");
	}
	pg.sgFit->SetInputData(pg.sgFitTable, 0, 1);
	s->win->statusBar()->showMessage(QString("Spector-Grant:  band Î”=%1   slope=%2   Depth = %3 m")
		.arg(xhi - xlo, 0, 'g', 4).arg(slope, 0, 'g', 4).arg(depth, 0, 'f', 0));
	if (s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// The fit lines kept on the page show while the tool is on and hide when it is off (dynSlope_CB).
static void xySGShowLines(XYPage &pg, bool on) {
	for (XYSGLine &l : pg.sgLines) if (l.plot) l.plot->SetVisible(on);
}

// Tool turned off: the live fit goes, the kept lines hide.
static void xySGClear(XYPlot *s) {
	XYPage &pg = xyCur(s);
	if (pg.sgFit) { pg.chart->RemovePlotInstance(pg.sgFit); pg.sgFit = nullptr; }
	pg.sgFitTable = nullptr;
	pg.sgEdit = -1;
	xySGShowLines(pg, false);
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Add a kept fit line to page `pg` (a released drag, or a session being reopened).
static void xySGAddLine(XYPage &pg, XYSGLine l) {
	l.table = vtkSmartPointer<vtkTable>::New();
	vtkNew<vtkFloatArray> ax; ax->SetName("k");       l.table->AddColumn(ax);
	vtkNew<vtkFloatArray> ay; ay->SetName("S&G fit"); l.table->AddColumn(ay);
	l.table->SetNumberOfRows(2);
	l.table->SetValue(0, 0, (float)l.x0); l.table->SetValue(0, 1, (float)l.y0);
	l.table->SetValue(1, 0, (float)l.x1); l.table->SetValue(1, 1, (float)l.y1);
	l.plot = pg.chart->AddPlot(vtkChart::LINE);
	l.plot->SetInputData(l.table, 0, 1);
	l.plot->SetColor((unsigned char)(l.r * 255), (unsigned char)(l.g * 255), (unsigned char)(l.bl * 255), 255);
	l.plot->SetWidth(2.5f);
	l.plot->SetLabel("S&G fit");
	l.plot->SetVisible(pg.sgActive);
	pg.sgLines.push_back(l);
}

// wbu_dynSlope: the drag is over — the live fit becomes a kept line, with its own menu and ends.
static void xySGKeep(XYPlot *s) {
	XYPage &pg = xyCur(s);
	if (!pg.sgFit || !pg.sgFitTable) return;
	XYSGLine l;
	l.x0 = pg.sgFitTable->GetValue(0, 0).ToDouble(); l.y0 = pg.sgFitTable->GetValue(0, 1).ToDouble();
	l.x1 = pg.sgFitTable->GetValue(1, 0).ToDouble(); l.y1 = pg.sgFitTable->GetValue(1, 1).ToDouble();
	double slope, inter, depth, xlo, xhi;
	if (!xySGFit(s, pg.sgSel, l.x0, l.x1, pg.sgUnit, slope, inter, depth, xlo, xhi)) return;
	l.m = slope; l.b = inter; l.depth = depth; l.unit = pg.sgUnit;
	l.r = pg.sgR; l.g = pg.sgG; l.bl = pg.sgB;
	pg.chart->RemovePlotInstance(pg.sgFit);
	pg.sgFit = nullptr;
	pg.sgFitTable = nullptr;
	xySGAddLine(pg, l);
}

// recompSI: slope and intercept again from the line's two ends, once they have been dragged. In
// ln(power), the space the line was fitted in (ecran takes the plotted power itself, which for the
// Spector & Grant line is not the quantity whose slope is the depth).
static void xySGRecomp(XYPlot *s, int i) {
	XYPage &pg = xyCur(s);
	if (i < 0 || i >= (int)pg.sgLines.size()) return;
	XYSGLine &l = pg.sgLines[i];
	if (!(l.y0 > 0 && l.y1 > 0) || l.x1 == l.x0) return;
	l.m = (std::log(l.y1) - std::log(l.y0)) / (l.x1 - l.x0);
	l.b = std::log(l.y0) - l.m * l.x0;
	l.depth = std::abs(l.m) / (4.0 * vtkMath::Pi()) * l.unit;
	s->win->statusBar()->showMessage(QString("Dist=%1   Depth=%2").arg(l.x1 - l.x0, 0, 'g', 6).arg(l.depth, 0, 'f', 3));
}

static bool xyDataToScene(XYPage &pg, double x, double y, double &sx, double &sy);   // (curve marker, below)
static void xyQtToScene(XYPlot *s, const QPointF &p, double &sx, double &sy);
static bool xyRequest(XYPlot *s, const QString &op, int sel, const char *busy, bool fileCb = false,
                      const QString &path = QString());

// Map a Qt widget point to a DATA y through the current page's left axis (log10 when it is active).
static double xySGDataY(XYPlot *s, const QPointF &p) {
	double sx, sy;
	xyQtToScene(s, p, sx, sy);
	vtkAxis *ay = xyCur(s).chart->GetAxis(vtkAxis::LEFT);
	const float *a1 = ay->GetPoint1(), *a2 = ay->GetPoint2();
	const double h = a2[1] - a1[1];
	double v = (h == 0.0) ? ay->GetMinimum() : ay->GetMinimum() + (sy - a1[1]) / h * (ay->GetMaximum() - ay->GetMinimum());
	return ay->GetLogScaleActive() ? std::pow(10.0, v) : v;
}

// ecran's bandpass(): Low cut / Low pass / High cut / High pass, in frequency or wavelength, prefilled
// from the line's band with a 10% transition. Returns the pass band in CYCLES PER METRE (the outer
// cuts, which is what fft_stuff.m's 'bpass' uses), false on Cancel.
static bool xySGAskBand(QWidget *parent, double fa, double fb, double &flo, double &fhi) {
	if (fa > fb) std::swap(fa, fb);
	const double tr = (fb - fa) * 0.1;                  // 10%
	double LC = std::max(fa - tr / 2, 0.0), HP = std::max(fb - tr / 2, 0.0);
	double LP = std::min(fa + tr / 2, fb), HC = fb + tr / 2;
	// frequencies, increasing: LC <= LP <= HP' ... held as the four band edges f[0..3]
	double f[4] = { LC, LP, HP, HC };
	QDialog d(parent);
	d.setWindowTitle("Bandpass");
	QGridLayout *g = new QGridLayout(&d);
	QRadioButton *rFreq = new QRadioButton("Frequency", &d), *rWave = new QRadioButton("Wavelength", &d);
	rWave->setChecked(true);
	QLineEdit *e[4];
	const char *lab[4] = { "Low cut", "Low pass", "High cut", "High pass" };
	const char *tip[4] = { "Low Cut frequency", "Low Pass frequency", "High Cut frequency", "High Pass frequency" };
	for (int i = 0; i < 4; ++i) { e[i] = new QLineEdit(&d); e[i]->setToolTip(tip[i]); }
	g->addWidget(rFreq, 0, 1); g->addWidget(rWave, 0, 3);
	g->addWidget(new QLabel(lab[0], &d), 1, 0); g->addWidget(e[0], 1, 1);
	g->addWidget(new QLabel(lab[2], &d), 1, 2); g->addWidget(e[2], 1, 3);
	g->addWidget(new QLabel(lab[1], &d), 2, 0); g->addWidget(e[1], 2, 1);
	g->addWidget(new QLabel(lab[3], &d), 2, 2); g->addWidget(e[3], 2, 3);
	QPushButton *ok = new QPushButton("OK", &d);
	g->addWidget(ok, 3, 3);
	// The boxes read low -> high in the unit shown: as wavelengths the order of the frequencies turns
	// over (ecran's prefill: LC = 1/HC, LP = 1/HP, HC = 1/LP, HP = 1/LC).
	auto fill = [&]() {
		const bool wave = rWave->isChecked();
		const double v[4] = { wave ? 1 / f[3] : f[0], wave ? 1 / f[2] : f[1], wave ? 1 / f[1] : f[2], wave ? 1 / f[0] : f[3] };
		for (int i = 0; i < 4; ++i) e[i]->setText(QString::number(v[i], 'g', 6));
	};
	auto read = [&](double v[4]) {
		for (int i = 0; i < 4; ++i) {
			bool okv = false;
			v[i] = e[i]->text().trimmed().toDouble(&okv);
			if (!okv || v[i] <= 0) return false;
		}
		return true;
	};
	fill();
	auto flip = [&]() {                                // keep the band when the unit changes
		double v[4];
		if (!read(v)) return;
		const bool toWave = rWave->isChecked();       // the boxes still hold the OTHER unit
		if (toWave) { f[0] = v[0]; f[1] = v[1]; f[2] = v[2]; f[3] = v[3]; }
		else        { f[0] = 1 / v[3]; f[1] = 1 / v[2]; f[2] = 1 / v[1]; f[3] = 1 / v[0]; }
		fill();
	};
	QObject::connect(rWave, &QRadioButton::toggled, &d, [&](bool) { flip(); });
	QObject::connect(ok, &QPushButton::clicked, &d, [&]() {
		double v[4];
		if (!read(v)) { QMessageBox::warning(&d, "Error", "Each box needs a positive number."); return; }
		QString msg;
		if (v[0] > v[1])      msg = "Low cut cannot be higher than Low pass";
		else if (v[2] > v[3]) msg = "High cut cannot be higher than High pass";
		else if (v[1] > v[2]) msg = "Low pass cannot be higher than High cut";
		if (!msg.isEmpty()) { QMessageBox::warning(&d, "Error", msg); return; }
		if (rWave->isChecked()) { flo = 1 / v[3]; fhi = 1 / v[0]; }   // 1./[HP HC LP LC]: bp(1), bp(4)
		else                    { flo = v[0];     fhi = v[3]; }
		d.accept();
	});
	wrapTooltips(&d);
	return d.exec() == QDialog::Accepted;
}

// The kept line's right-click menu (wbu_dynSlope's uicontextmenu).
static void xySGLineMenu(XYPlot *s, int i, const QPoint &global) {
	XYPage &pg = xyCur(s);
	if (i < 0 || i >= (int)pg.sgLines.size()) return;
	const XYSGLine l = pg.sgLines[i];
	QMenu m(s->win);
	m.addAction("Slope(m/cycle) &  Intercept")->setEnabled(false);
	m.addAction(QString("%1   %2").arg(l.m / (2 * vtkMath::Pi()), 0, 'f', 2).arg(l.b, 0, 'g', 9))->setEnabled(false);
	m.addAction(QString("Depth to sources (m) = %1").arg(l.depth, 0, 'f', 3))->setEnabled(false);
	m.addSeparator();
	QAction *aBand = m.addAction("Bandpass Filter");
	m.addSeparator();
	QAction *aRe = m.addAction("Recomp Slope/Intercept");
	m.addSeparator();
	QAction *aDel = m.addAction("Delete this line");
	QAction *pick = m.exec(global);
	if (!pick || !xyAlive(s)) return;
	if (pick == aRe) {
		xySGRecomp(s, i);
	}
	else if (pick == aDel) {
		XYPage &p2 = xyCur(s);
		if (i < (int)p2.sgLines.size()) {
			if (p2.sgLines[i].plot) p2.chart->RemovePlotInstance(p2.sgLines[i].plot);
			p2.sgLines.erase(p2.sgLines.begin() + i);
		}
	}
	else if (pick == aBand) {
		double flo, fhi;
		if (!xySGAskBand(s->win, l.x0 / l.unit, l.x1 / l.unit, flo, fhi)) return;   // frequencies in 1/m
		xyRequest(s, QString("sgbandpass:flo=%1:fhi=%2").arg(flo, 0, 'g', 17).arg(fhi, 0, 'g', 17), -1,
		          "Band-pass filtering the grid…");
	}
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// The kept line (and its end, 0/1) under widget point `qp`: an end within 7 px, or the line within 5.
static int xySGLineAt(XYPlot *s, const QPointF &qp, int &end) {
	XYPage &pg = xyCur(s);
	double sx, sy;
	xyQtToScene(s, qp, sx, sy);
	const double dpr = s->widget->devicePixelRatioF();
	end = -1;
	for (int i = (int)pg.sgLines.size() - 1; i >= 0; --i) {
		const XYSGLine &l = pg.sgLines[i];
		if (!l.plot || !l.plot->GetVisible()) continue;
		double ax, ay, bx, by;
		if (!xyDataToScene(pg, l.x0, l.y0, ax, ay) || !xyDataToScene(pg, l.x1, l.y1, bx, by)) continue;
		const double t7 = 7.0 * dpr;
		if ((sx - ax) * (sx - ax) + (sy - ay) * (sy - ay) <= t7 * t7) { end = 0; return i; }
		if ((sx - bx) * (sx - bx) + (sy - by) * (sy - by) <= t7 * t7) { end = 1; return i; }
		const double a[2] = { ax, ay }, b[2] = { bx, by };
		if (segDist2(sx, sy, a, b) <= 25.0 * dpr * dpr) return i;
	}
	return -1;
}

// Qt event filter on the chart widget: while the tool is active, left-drag selects the band and
// drives the live fit (consuming the events so the chart's own zoom/pan stays out of the way); a
// press on a kept line's END drags that end (ui_edit_polygon), a right-click on a kept line opens
// its menu.
class XYSGFilter : public QObject {
public:
	XYPlot *s = nullptr;
	explicit XYSGFilter(XYPlot *sc, QObject *parent) : QObject(parent), s(sc) {}
protected:
	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (!s || !xyAlive(s) || !xyCur(s).sgActive)
			return QObject::eventFilter(obj, ev);
		XYPage &pg = xyCur(s);
		const QEvent::Type t = ev->type();
		if (t == QEvent::MouseButtonPress) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			int end = -1;
			const int hit = xySGLineAt(s, me->position(), end);
			if (me->button() == Qt::RightButton && hit >= 0) {
				xySGLineMenu(s, hit, me->globalPosition().toPoint());
				return true;
			}
			if (me->button() == Qt::LeftButton) {
				if (hit >= 0 && end >= 0) { pg.sgEdit = hit; pg.sgEditEnd = end; return true; }
				pg.sgDragging = true;
				pg.sgX0 = xySGDataX(s, me->position());
				// mkAnother: the first fit line is black, every later one a colour of its own
				if (pg.sgLines.empty()) { pg.sgR = pg.sgG = pg.sgB = 0.0; }
				else {
					const double *c = xyPalette((int)pg.sgLines.size());
					pg.sgR = c[0]; pg.sgG = c[1]; pg.sgB = c[2];
				}
				return true;
			}
		}
		else if (t == QEvent::MouseMove && pg.sgEdit >= 0) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (pg.sgEdit < (int)pg.sgLines.size()) {
				XYSGLine &l = pg.sgLines[pg.sgEdit];
				const double x = xySGDataX(s, me->position()), y = xySGDataY(s, me->position());
				if (pg.sgEditEnd == 0) { l.x0 = x; l.y0 = y; } else { l.x1 = x; l.y1 = y; }
				l.table->SetValue(pg.sgEditEnd, 0, (float)x);
				l.table->SetValue(pg.sgEditEnd, 1, (float)y);
				l.table->Modified();
				if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
			}
			return true;
		}
		else if (t == QEvent::MouseMove && pg.sgDragging) {
			xySGUpdate(s, xySGDataX(s, static_cast<QMouseEvent*>(ev)->position()));
			return true;
		}
		else if (t == QEvent::MouseButtonRelease && (pg.sgDragging || pg.sgEdit >= 0)) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (me->button() == Qt::LeftButton) {
				if (pg.sgDragging) xySGKeep(s);
				pg.sgDragging = false;
				pg.sgEdit = -1;
				return true;
			}
		}
		return QObject::eventFilter(obj, ev);
	}
};

// Currently-selected series in the Object Manager (current page). Falls back to the LAST series when
// nothing is explicitly selected (so Analysis "just works" on the obvious series without a prior row
// click); -1 only when the page has no series at all.
static int xyCurrentSel(XYPlot *s) {
	std::vector<XYSeries> &series = xyCur(s).series;
	if (!s->objMgr || series.empty())
		return -1;
	QTreeWidgetItem *it = s->objMgr->currentItem();
	if (it) {
		const int idx = it->data(0, Qt::UserRole).toInt();
		if (idx >= 0 && idx < (int)series.size())
			return idx;
	}
	return (int)series.size() - 1;                      // no/invalid selection -> the last series
}

// ---- time-axis tick formatting ---------------------------------------------
// The X data is treated as Unix epoch SECONDS (UTC); the chosen mode only changes how the bottom
// axis tick LABELS read (no data conversion). Ticks regenerate on every range change.

// A natural time step (seconds) so ~ <=6 ticks span `span` seconds: seconds .. decade. Kept low so
// the (wide) date labels never collide.
static double xyNiceDateStep(double span) {
	static const double c[] = {
		1, 2, 5, 10, 15, 30,
		60, 120, 300, 600, 900, 1800,
		3600, 2*3600.0, 3*3600.0, 6*3600.0, 12*3600.0,
		86400.0, 2*86400.0, 7*86400.0, 14*86400.0,
		30*86400.0, 90*86400.0, 180*86400.0,
		365*86400.0, 2*365*86400.0, 5*365*86400.0, 10*365*86400.0,
	};
	const int n = (int)(sizeof(c) / sizeof(c[0]));
	for (int i = 0; i < n; ++i)
		if (span / c[i] <= 6.0) return c[i];
	return c[n - 1];
}

// A 1/2/2.5/5 ×10^k "nice" step giving ~`target` ticks across `span`. Used to lay out the LINEAR
// bottom-axis ticks ourselves so wide labels (e.g. epoch seconds ~1.7e9) can't crowd/overlap the way
// vtkChartXY's auto ticks do.
static double xyNiceLinStep(double span, int target) {
	if (!(span > 0.0) || target < 1)
		return 1.0;
	const double raw = span / target;
	const double mag = std::pow(10.0, std::floor(std::log10(raw)));
	const double norm = raw / mag;
	const double step = (norm <= 1.0 ? 1.0 : norm <= 2.0 ? 2.0 : norm <= 2.5 ? 2.5 : norm <= 5.0 ? 5.0 : 10.0);
	return step * mag;
}

// Format an epoch-seconds value per the time mode (`span` resolves the "auto" date format).
static QString xyFmtTime(double t, int fmt, double span) {
	const QDateTime dt = QDateTime::fromSecsSinceEpoch((qint64)llround(t), Qt::UTC);
	switch (fmt) {
	case 1:                                            // date — auto (by span)
		if (span < 2 * 86400.0)        return dt.toString("MM-dd HH:mm");
		else if (span < 730 * 86400.0) return dt.toString("yyyy-MM-dd");
		else                           return dt.toString("yyyy");
	case 2:                                            // date — yyyy-mm-dd
		return dt.toString("yyyy-MM-dd");
	case 3:                                            // time — HH:MM
		return dt.toString("HH:mm");
	case 4: {                                          // decimal year
		const int y = dt.date().year();
		const QDateTime y0(QDate(y, 1, 1), QTime(0, 0), Qt::UTC);
		const QDateTime y1(QDate(y + 1, 1, 1), QTime(0, 0), Qt::UTC);
		const double frac = double(dt.toSecsSinceEpoch() - y0.toSecsSinceEpoch())
		                  / double(y1.toSecsSinceEpoch() - y0.toSecsSinceEpoch());
		return QString::number(y + frac, 'f', 2);
	}
	case 5: {                                          // decimal day-of-year
		const double doy = dt.date().dayOfYear() + dt.time().msecsSinceStartOfDay() / 86400000.0;
		return QString::number(doy, 'f', 1);
	}
	}
	return QString::number(t);
}

// Full-resolution time label for the HOVER readout (tooltip + status bar). The tick formatter
// above goes coarse with the visible span (drops the time-of-day at multi-day spans); a single
// hovered point must always read to the second, so this keeps date AND time regardless of zoom.
static QString xyFmtTimeHover(double t, int fmt) {
	const QDateTime dt = QDateTime::fromSecsSinceEpoch((qint64)llround(t), Qt::UTC);
	switch (fmt) {
	case 1: case 2: case 3:                            // any date/time mode -> full date + time
		return dt.toString("yyyy-MM-dd HH:mm:ss");
	case 4: {                                          // decimal year, high precision
		const int y = dt.date().year();
		const QDateTime y0(QDate(y, 1, 1), QTime(0, 0), Qt::UTC);
		const QDateTime y1(QDate(y + 1, 1, 1), QTime(0, 0), Qt::UTC);
		const double frac = double(dt.toSecsSinceEpoch() - y0.toSecsSinceEpoch())
		                  / double(y1.toSecsSinceEpoch() - y0.toSecsSinceEpoch());
		return QString::number(y + frac, 'f', 6);
	}
	case 5: {                                          // decimal day-of-year, high precision
		const double doy = dt.date().dayOfYear() + dt.time().msecsSinceStartOfDay() / 86400000.0;
		return QString::number(doy, 'f', 4);
	}
	}
	return QString::number(t);
}

// Rebuild the current page's bottom-axis custom ticks for its range. We ALWAYS lay the ticks out
// ourselves (capped at ~6) — for time modes as dates, otherwise as nice 1/2/2.5/5 round numbers —
// because vtkChartXY's auto ticks crowd and OVERLAP whenever the labels are wide (e.g. epoch-second
// X ~1.7e9). The only exception is a log axis, where VTK's own decade ticks are correct, so we hand
// it back to auto.
static void xyRefreshXTicks(XYPage &pg, vtkAxis *ax);
static void xyRefreshTicks(XYPlot *s) {
	XYPage &pg = xyCur(s);
	vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM);
	if (ax->GetLogScaleActive())
		ax->SetCustomTickPositions(nullptr);
	else if (ax->GetMaximum() > ax->GetMinimum())
		xyRefreshXTicks(pg, ax);
	// The Y axes get the same ~6 round-number ticks: vtkChartXY's own pack them far too densely.
	// A log axis keeps VTK's decades; a stride set in the Axes dialog is left as it was set.
	for (int id : { (int)vtkAxis::LEFT, (int)vtkAxis::RIGHT }) {
		vtkAxis *ay = pg.chart->GetAxis(id);
		if (!ay || (id == vtkAxis::LEFT && pg.yTicksUser)) continue;
		if (ay->GetLogScaleActive()) {
			ay->SetCustomTickPositions(nullptr);
			continue;
		}
		const double ylo = ay->GetMinimum(), yhi = ay->GetMaximum();
		if (!(yhi > ylo)) continue;
		const double step = xyNiceLinStep(yhi - ylo, 6);
		vtkNew<vtkDoubleArray> ypos;
		vtkNew<vtkStringArray> ylab;
		for (double k = std::ceil(ylo / step - 1e-9); k * step <= yhi + 1e-9 * step; k += 1.0) {
			const double t = std::abs(k) < 0.5 ? 0.0 : k * step;     // an exact 0, never -1e-17
			ypos->InsertNextValue(t);
			ylab->InsertNextValue(std::string(QString::number(t, 'g', 6).toUtf8().constData()));
		}
		ay->SetCustomTickPositions(ypos, ylab);
	}
}

// The bottom axis' ticks, linear or time (xyRefreshTicks).
static void xyRefreshXTicks(XYPage &pg, vtkAxis *ax) {
	const double lo = ax->GetMinimum(), hi = ax->GetMaximum();
	const double span = hi - lo;
	vtkNew<vtkDoubleArray> pos;
	vtkNew<vtkStringArray> lab;
	if (pg.xTimeFmt) {
		const double step = xyNiceDateStep(span);
		for (double t = std::ceil(lo / step) * step; t <= hi + 0.5 * step; t += step) {
			pos->InsertNextValue(t);
			lab->InsertNextValue(std::string(xyFmtTime(t, pg.xTimeFmt, span).toUtf8().constData()));
		}
	} else {
		const double step = xyNiceLinStep(span, 6);
		for (double t = std::ceil(lo / step) * step; t <= hi + 0.5 * step; t += step) {
			pos->InsertNextValue(t);
			lab->InsertNextValue(std::string(QString::number(t, 'g', 6).toUtf8().constData()));
		}
	}
	ax->SetCustomTickPositions(pos, lab);
}

// Render-end observer: regenerate the bottom-axis ticks if its range changed (zoom/pan/new data)
// AND recompute any "now cross" series (xyRecomputeCross) -- decoupled from the tick-range check
// on purpose: a plain window RESIZE changes the axis' PIXEL span without changing its data range,
// which the tick check alone would miss but a screen-constant cross must still react to. Re-renders
// once if either needed it. Guarded against reentrancy (the re-render fires EndEvent again with
// nothing left changed, so both checks fall through and it returns without looping).
static void xyTicksOnRender(vtkObject *, unsigned long, void *clientData, void *) {
	XYPlot *s = static_cast<XYPlot*>(clientData);
	if (!xyAlive(s))
		return;
	XYPage &pg = xyCur(s);
	if (pg.ticksBusy)
		return;
	bool needRender = false;
	vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM);
	const double lo = ax->GetMinimum(), hi = ax->GetMaximum();
	if (hi > lo) {
		const double tol = 1e-6 * (std::abs(hi) + std::abs(lo) + 1.0);
		// NaN lastLo/lastHi (xySetXTime/xySwitchPage/xySetLog's "force a recompute" sentinel) must
		// count as a forced mismatch: abs(x - NaN) is NaN, and NaN >= tol is ALWAYS false, so the
		// plain difference check alone can never fire on a NaN baseline -- it would latch this page
		// out of ticks forever (first hit: xyplot(...; xtime=:date) sets xtime BEFORE any series
		// exists, so the one immediate xyRefreshTicks call in xySetXTime draws ticks for the
		// still-empty chart's default axis range; once real data lands and the axis autoscales to
		// it, this observer must still recompute -- but with a raw NaN comparison it silently never
		// does, leaving custom tick positions parked outside the real, now-visible range).
		if (std::isnan(pg.lastLo) || std::isnan(pg.lastHi) ||
		    std::abs(lo - pg.lastLo) >= tol || std::abs(hi - pg.lastHi) >= tol) {
			pg.lastLo = lo; pg.lastHi = hi;
			xyRefreshTicks(s);
			needRender = true;
		}
	}
	// a Y range change (autoscale to new data, a zoom) re-lays the Y ticks the same way
	vtkAxis *al = pg.chart->GetAxis(vtkAxis::LEFT), *ar = pg.chart->GetAxis(vtkAxis::RIGHT);
	const double yl = al->GetMinimum(), yh = al->GetMaximum(), rl = ar->GetMinimum(), rh = ar->GetMaximum();
	if (!needRender && (yl != pg.lastYLo || yh != pg.lastYHi || rl != pg.lastRLo || rh != pg.lastRHi)) {
		xyRefreshTicks(s);
		needRender = true;
	}
	pg.lastYLo = yl; pg.lastYHi = yh; pg.lastRLo = rl; pg.lastRHi = rh;
	for (auto &se : pg.series)
		if (xyRecomputeCross(pg, se))
			needRender = true;
	if (needRender && s->widget && s->widget->renderWindow()) {
		pg.ticksBusy = true;
		s->widget->renderWindow()->Render();
		pg.ticksBusy = false;
	}
}

// Tick the Misc menu's "Log X/Y axis" and "Time axis (X)" items from the CURRENT page, without
// re-applying what they show.
static void xySyncMiscMenu(XYPlot *s) {
	if (!s || s->pages.empty()) return;
	XYPage &pg = xyCur(s);
	if (s->actLogX) { QSignalBlocker b(s->actLogX); s->actLogX->setChecked(pg.chart->GetAxis(vtkAxis::BOTTOM)->GetLogScale()); }
	if (s->actLogY) { QSignalBlocker b(s->actLogY); s->actLogY->setChecked(pg.chart->GetAxis(vtkAxis::LEFT)->GetLogScale()); }
	// NOT blocked: the exclusive QActionGroup clears the other items off this action's own signal, and
	// the items act on `triggered`, which setChecked never emits.
	if (pg.xTimeFmt >= 0 && pg.xTimeFmt < 6 && s->actTime[pg.xTimeFmt])
		s->actTime[pg.xTimeFmt]->setChecked(true);
}

// Toggle log scaling on a current-page axis (axis 0 = bottom/X, 1 = left/Y). A log axis places only
// positive values, so — as MATLAB does — the points it cannot take are left out of the drawing
// (xyLogInput, through xyApplyStyle) and the window says how many: the axis then really goes log
// instead of VTK silently keeping it linear. Recomputes bounds + renders.
static void xySetLog(XYPlot *s, int axis, bool on) {
	XYPage &pg = xyCur(s);
	vtkAxis *ax = pg.chart->GetAxis(axis == 0 ? vtkAxis::BOTTOM : vtkAxis::LEFT);
	ax->SetLogScale(on);
	int dropped = 0;
	for (XYSeries &se : pg.series) {                       // each plot re-reads its (masked) data
		xyApplyStyle(s, se);
		dropped += se.logDropped;
	}
	pg.chart->RecalculateBounds();
	if (on && dropped > 0)
		xyLog(s, QString("Log %1 axis: %2 point(s) <= 0 left out of the drawing (a log axis cannot place them; "
		                 "the data itself is unchanged)").arg(axis == 0 ? "X" : "Y").arg(dropped));
	pg.lastLo = std::numeric_limits<double>::quiet_NaN();   // X log on/off changes the tick scheme
	pg.lastHi = std::numeric_limits<double>::quiet_NaN();
	xyRefreshTicks(s);
	xySyncMiscMenu(s);                                       // set from Julia too: the menu shows it
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Set the current page's X-axis time mode (0 = linear, 1..5 see XYPage::xTimeFmt) + refresh.
static void xySetXTime(XYPlot *s, int fmt) {
	XYPage &pg = xyCur(s);
	pg.xTimeFmt = fmt;
	pg.lastLo = std::numeric_limits<double>::quiet_NaN();   // force the observer to recompute
	pg.lastHi = std::numeric_limits<double>::quiet_NaN();
	xyRefreshTicks(s);
	xySyncMiscMenu(s);
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// vtkChartXY subclass whose only job is to format the hover tooltip's X value as a calendar
// date/time when the current page is in a time mode. VTK's built-in tooltip prints the raw X (epoch
// seconds, ~1e9) via the plot's "%x" format with no date support, so we override the one virtual
// that fills the tooltip and build "date,  y" ourselves. Linear mode falls back to the base.
// ---- curve marker (click a curve, drag along it, double-click it away) -----

// A data value -> scene px along one axis, through the axis' OWN scaling (log10 when active, which is
// how the axis stores its Minimum/Maximum), so the marker sits exactly where the plot draws the curve.
static double xyAxisToScene(vtkAxis *a, double v, int k) {
	if (a->GetLogScaleActive()) v = std::log10(std::fabs(v));
	const double lo = a->GetMinimum(), hi = a->GetMaximum();
	const float *p1 = a->GetPoint1(), *p2 = a->GetPoint2();
	if (hi == lo) return p1[k];
	return p1[k] + (v - lo) / (hi - lo) * (p2[k] - p1[k]);
}
static bool xyDataToScene(XYPage &pg, double x, double y, double &sx, double &sy) {
	vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM), *ay = pg.chart->GetAxis(vtkAxis::LEFT);
	if (!ax || !ay || !std::isfinite(x) || !std::isfinite(y)) return false;
	sx = xyAxisToScene(ax, x, 0);  sy = xyAxisToScene(ay, y, 1);
	return std::isfinite(sx) && std::isfinite(sy);
}
// Qt widget point (logical px, top-down) -> scene px (device px, bottom-up).
static void xyQtToScene(XYPlot *s, const QPointF &p, double &sx, double &sy) {
	const double dpr = s->widget->devicePixelRatioF();
	const int *sz = s->widget->renderWindow()->GetSize();
	sx = p.x() * dpr;  sy = sz[1] - p.y() * dpr;
}
// A series the marker can ride: a visible line (not a bar, not the screen-constant "now" cross).
static bool xyMarkable(const XYSeries &se) {
	return se.visible && se.plot && se.kind == 0 && !se.nowCross && se.table && se.table->GetNumberOfRows() >= 2;
}
static void xyRowXY(vtkTable *t, vtkIdType r, double &x, double &y) {
	x = t->GetValue(r, 0).ToDouble();  y = t->GetValue(r, 1).ToDouble();
}
// The marker's data point (row floor(u) towards the next by the fraction), false when it has none.
static bool xyMarkPoint(XYPage &pg, double &x, double &y) {
	if (pg.markSel < 0 || pg.markSel >= (int)pg.series.size() || !xyMarkable(pg.series[pg.markSel]))
		return false;
	vtkTable *t = pg.series[pg.markSel].table;
	const vtkIdType n = t->GetNumberOfRows();
	vtkIdType i = std::clamp<vtkIdType>((vtkIdType)std::floor(pg.markU), 0, n - 2);
	const double f = std::clamp(pg.markU - (double)i, 0.0, 1.0);
	double x0, y0, x1, y1;
	xyRowXY(t, i, x0, y0);  xyRowXY(t, i + 1, x1, y1);
	x = x0 + f * (x1 - x0);  y = y0 + f * (y1 - y0);
	return std::isfinite(x) && std::isfinite(y);
}
// Nearest point ON a curve to scene px (sx,sy), within tol px. `only` >= 0 restricts the search to
// that series (a drag stays on its own curve); -1 searches every markable one.
static bool xyCurveNearest(XYPage &pg, int only, double sx, double sy, double tol, int &sel, double &u) {
	double best = tol * tol;
	bool hit = false;
	for (int k = 0; k < (int)pg.series.size(); ++k) {
		if ((only >= 0 && k != only) || !xyMarkable(pg.series[k])) continue;
		vtkTable *t = pg.series[k].table;
		const vtkIdType n = t->GetNumberOfRows();
		double ax = 0, ay = 0;
		bool aok = false;
		for (vtkIdType r = 0; r < n; ++r) {
			double x, y, bx, by;
			xyRowXY(t, r, x, y);
			const bool bok = xyDataToScene(pg, x, y, bx, by);
			if (aok && bok) {
				const double a[2] = { ax, ay }, b[2] = { bx, by };
				const double d2 = segDist2(sx, sy, a, b);
				if (d2 <= best) {
					const double vx = bx - ax, vy = by - ay, L2 = vx*vx + vy*vy;
					const double tt = L2 > 0.0 ? std::clamp(((sx - ax) * vx + (sy - ay) * vy) / L2, 0.0, 1.0) : 0.0;
					best = d2;  hit = true;  sel = k;  u = (double)(r - 1) + tt;
				}
			}
			ax = bx;  ay = by;  aok = bok;
		}
	}
	return hit;
}
// The panel's marker (an x — distance along the profile) onto the linked curve: the fractional row
// where that x falls (the profile's x ascends). NaN removes it.
static void xyMarkFromPanel(XYPlot *s, double x) {
	if (s->markLinkPage < 0 || s->markLinkPage >= (int)s->pages.size()) return;
	XYPage &pg = s->pages[s->markLinkPage];
	if (s->markLinkSel < 0 || s->markLinkSel >= (int)pg.series.size()) return;
	vtkTable *t = pg.series[s->markLinkSel].table;
	const vtkIdType n = t ? t->GetNumberOfRows() : 0;
	if (std::isnan(x) || n < 2) {
		if (pg.markSel == s->markLinkSel) pg.markSel = -1;
	}
	else {
		vtkIdType lo = 0, hi = n - 1;                    // x(lo) <= x <= x(hi)
		while (hi - lo > 1) {
			const vtkIdType mid = (lo + hi) / 2;
			if (t->GetValue(mid, 0).ToDouble() <= x) lo = mid; else hi = mid;
		}
		const double x0 = t->GetValue(lo, 0).ToDouble(), x1 = t->GetValue(hi, 0).ToDouble();
		pg.markSel = s->markLinkSel;
		pg.markU = (double)lo + (x1 > x0 ? std::clamp((x - x0) / (x1 - x0), 0.0, 1.0) : 0.0);
	}
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}
// …and back: a marker moved / placed / removed HERE on a linked page tells the panel, which tells the
// 3-D track and every other window linked to it.
static void xyMarkToPanel(XYPlot *s) {
	if (!s->markPanel || s->markSyncing || s->markLinkSel < 0 || s->curPageIdx != s->markLinkPage) return;
	XYPage &pg = xyCur(s);
	double x = std::numeric_limits<double>::quiet_NaN(), y;
	if (pg.markSel == s->markLinkSel && !xyMarkPoint(pg, x, y)) x = std::numeric_limits<double>::quiet_NaN();
	s->markSyncing = true;
	s->markPanel->setMarker(x);
	s->markSyncing = false;
}
static bool xyMarkHit(XYPlot *s, const QPointF &qp) {
	XYPage &pg = xyCur(s);
	double x, y, mx, my, sx, sy;
	if (!xyMarkPoint(pg, x, y) || !xyDataToScene(pg, x, y, mx, my)) return false;
	xyQtToScene(s, qp, sx, sy);
	const double tol = 9.0 * s->widget->devicePixelRatioF();
	return (sx - mx) * (sx - mx) + (sy - my) * (sy - my) <= tol * tol;
}
// Drawn by the chart at the END of its own paint (XYChart::Paint), so the axes it maps through are
// the ones just laid out — the marker follows zoom, pan and resize with no observer of its own.
static void xyMarkPaint(XYPlot *s, vtkChartXY *chart, vtkContext2D *p) {
	if (!s || s->pages.empty()) return;
	XYPage *pg = nullptr;
	for (auto &q : s->pages) if (q.chart.Get() == chart) { pg = &q; break; }
	double x, y, mx, my;
	if (!pg || !xyMarkPoint(*pg, x, y) || !xyDataToScene(*pg, x, y, mx, my)) return;
	const float dpr = s->widget ? (float)s->widget->devicePixelRatioF() : 1.0f;
	p->GetPen()->SetColor(30, 30, 30, 255);
	p->GetPen()->SetWidth(1.5f * dpr);
	p->GetBrush()->SetColor(255, 255, 255, 200);
	p->DrawEllipse((float)mx, (float)my, 5.5f * dpr, 5.5f * dpr);
	const std::string lab = xyCoordText(pg->xTimeFmt, x, y).toStdString();
	p->GetTextProp()->SetColor(0.0, 0.0, 0.0);
	p->GetTextProp()->SetFontSize(11);
	p->GetTextProp()->SetJustificationToLeft();
	p->GetTextProp()->SetVerticalJustificationToBottom();
	float b[4];
	p->ComputeStringBounds(lab, b);
	const float pad = 4.0f * dpr, w = b[2] + 2 * pad, h = b[3] + 2 * pad;
	float bx = (float)mx + 9 * dpr, by = (float)my + 7 * dpr;   // up-right of the circle (scene y is up)
	const float *xr = chart->GetAxis(vtkAxis::BOTTOM)->GetPoint2(), *yt = chart->GetAxis(vtkAxis::LEFT)->GetPoint2();
	if (bx + w > xr[0]) bx = (float)mx - 9 * dpr - w;            // kept inside the plot frame
	if (by + h > yt[1]) by = (float)my - 7 * dpr - h;
	p->GetPen()->SetColor(120, 120, 120, 255);
	p->GetPen()->SetWidth(1.0f);
	p->GetBrush()->SetColor(255, 255, 225, 255);
	p->DrawRect(bx, by, w, h);
	p->DrawString(bx + pad, by + pad, lab);
}

// The page's own labels (XYPage::labels), at their data positions. Clipped to the plot frame like a
// curve would be: a label outside the axes' current range is simply not drawn.
static void xyLabelPaint(XYPlot *s, vtkChartXY *chart, vtkContext2D *p) {
	if (!s || s->pages.empty()) return;
	XYPage *pg = nullptr;
	for (auto &q : s->pages) if (q.chart.Get() == chart) { pg = &q; break; }
	if (!pg || pg->labels.empty()) return;
	const float dpr = s->widget ? (float)s->widget->devicePixelRatioF() : 1.0f;
	const float *x0 = chart->GetAxis(vtkAxis::BOTTOM)->GetPoint1(), *x1 = chart->GetAxis(vtkAxis::BOTTOM)->GetPoint2();
	const float *y0 = chart->GetAxis(vtkAxis::LEFT)->GetPoint1(),   *y1 = chart->GetAxis(vtkAxis::LEFT)->GetPoint2();
	const float tol = 40.0f * dpr;                    // labels just outside a hidden frame (spoke names) still show
	for (XYLabel &l : pg->labels) {
		l.shown = false;
		double sx, sy;
		if (!xyDataToScene(*pg, l.x, l.y, sx, sy)) continue;
		if (sx < x0[0] - tol || sx > x1[0] + tol || sy < y0[1] - tol || sy > y1[1] + tol) continue;
		vtkTextProperty *tp = p->GetTextProp();
		tp->SetColor(l.r, l.g, l.b);
		tp->SetFontSize((int)std::lround(l.size));
		tp->SetBold(l.bold);
		if (l.ha < 0) tp->SetJustificationToLeft(); else if (l.ha > 0) tp->SetJustificationToRight(); else tp->SetJustificationToCentered();
		if (l.va < 0) tp->SetVerticalJustificationToBottom(); else if (l.va > 0) tp->SetVerticalJustificationToTop(); else tp->SetVerticalJustificationToCentered();
		p->DrawString((float)sx, (float)sy, l.text);
		float b[4];
		p->ComputeJustifiedStringBounds(l.text.c_str(), b);   // justified offsets from the anchor
		l.bx = (float)sx + b[0];  l.by = (float)sy + b[1];  l.bw = b[2];  l.bh = b[3];
		l.shown = true;
	}
}

// A page's square plot area and hidden axes (XYPage::square / axesHidden), applied to its chart. The
// square is the largest one centred in the window's scene, so the page's own frame (a radar's circle,
// a corner plot's panels) is never stretched; every other page keeps the chart's own full-size layout.
static void xyApplyPageLook(XYPlot *s, XYPage &pg) {
	vtkChartXY *c = pg.chart;
	if (!c) return;
	c->SetAutoAxes(!pg.axesHidden);                  // the chart would otherwise show them again
	if (pg.axesHidden)
		for (int a : { vtkAxis::BOTTOM, vtkAxis::LEFT, vtkAxis::RIGHT, vtkAxis::TOP }) {
			c->GetAxis(a)->SetVisible(false);
			c->GetAxis(a)->SetGridVisible(false);
		}
	if (!pg.square || !s->view || !s->view->GetScene()) {
		c->SetAutoSize(true);
		return;
	}
	const float w = (float)s->view->GetScene()->GetSceneWidth(), h = (float)s->view->GetScene()->GetSceneHeight();
	if (w <= 0 || h <= 0) return;
	const float side = std::min(w, h);
	c->SetAutoSize(false);
	c->SetSize(vtkRectf((w - side) / 2, (h - side) / 2, side, side));
}

// The Axes properties fields a 2-D chart draws. Every other field of axes_props.ui is greyed out in the
// X,Y dialog (Z, 3-D, oblique and secondary annotations, the map-only settings): a field that does
// nothing is not offered. Keys are matched as "KEY" or "KEY.part" prefixes.
static bool xyAxpHonoured(const QString &key) {
	static const char *const keys[] = {
		"-R.xauto", "-R.xmin", "-R.xmax", "-R.yauto", "-R.ymin", "-R.ymax",
		"-Bx.label", "-By.label", "-Bpx.a", "-Bpy.a", "-Bpx.g", "-Bpy.g", "-B.+t", "-B.+s", "-B.+g",
		"MAP_FRAME_AXES.W", "MAP_FRAME_AXES.E", "MAP_FRAME_AXES.S", "MAP_FRAME_AXES.N",
		"MAP_FRAME_AXES.Wg", "MAP_FRAME_AXES.Sg",
		"MAP_FRAME_PEN", "MAP_GRID_PEN_PRIMARY", "MAP_TICK_LENGTH_PRIMARY.major",
		"FONT", "FONT_ANNOT_PRIMARY", "FONT_LABEL", "FONT_TITLE", "FORMAT_FLOAT_MAP" };
	for (const char *k : keys)
		if (key == k || key.startsWith(QString(k) + "."))
			return true;
	return false;
}

// A chart's title WITHOUT the subtitle line xyApplyAxes puts under it.
static QString xyChartTitle(vtkChartXY *c) {
	return QString::fromStdString(c->GetTitle()).section('\n', 0, 0);
}

// The grid toggles show the CURRENT page's grid as drawn: Grid = both directions, X-Grid = the vertical
// lines (the x axis'), Y-Grid = the horizontal ones. Called after anything that can change it.
static void xyGridSyncUi(XYPlot *s) {
	if (!xyAlive(s) || s->pages.empty() || !xyCur(s).chart) return;
	vtkChartXY *c = xyCur(s).chart;
	const bool gx = c->GetAxis(vtkAxis::BOTTOM)->GetGridVisible(), gy = c->GetAxis(vtkAxis::LEFT)->GetGridVisible();
	auto put = [](QAction *a, bool on) { if (a && a->isChecked() != on) { QSignalBlocker b(a); a->setChecked(on); } };
	put(s->actGrid, gx && gy);
	// the tool bar's three are one choice: both = Grid, vertical only = X-Grid, horizontal only = Y-Grid
	put(s->actGridTb[0], gx && gy);
	put(s->actGridTb[1], gx && !gy);
	put(s->actGridTb[2], !gx && gy);
	// ...and so does this page's Axes properties dialog when it is open (its Apply must not undo a toggle)
	if (s->win)
		for (QDialog *d : s->win->findChildren<QDialog *>(QString("xyAxesProps_%1").arg((quintptr)c))) {
			if (auto *cb = d->findChild<QCheckBox *>("chkGridS")) cb->setChecked(gx);
			if (auto *cb = d->findChild<QCheckBox *>("chkGridW")) cb->setChecked(gy);
		}
}

// THE APPLY of a page's axes: its Object Manager row (drawn or not) and its stored Axes properties onto
// the chart's own vtkAxis objects. The page stores only the fields CHANGED from the .ui's (GMT's)
// defaults, so everything left alone keeps the chart's own look. A page that draws its own frame
// (radar, pie, polar: axesHidden) is left to xyApplyPageLook.
static void xyApplyAxes(XYPlot *s, XYPage &pg) {
	vtkChartXY *c = pg.chart;
	if (!c || pg.axesHidden) return;
	QMap<QString, QString> v;
	for (const auto &kv : pg.axProps) v[QString::fromStdString(kv.first)] = QString::fromStdString(kv.second);
	vtkAxis *ax[4] = { c->GetAxis(vtkAxis::LEFT), c->GetAxis(vtkAxis::RIGHT), c->GetAxis(vtkAxis::BOTTOM), c->GetAxis(vtkAxis::TOP) };
	static const char *const kSide[4] = { "W", "E", "S", "N" };
	static const char *const kMode[4] = { "Annot. + ticks", "Line + ticks", "Line only", "Not drawn" };
	bool sidesSet = false;
	for (int i = 0; i < 4; ++i) sidesSet |= v.contains(QString("MAP_FRAME_AXES.") + kSide[i]);
	// Sides: the row's checkbox hides them all; set sides take over from the chart's automatic axes.
	c->SetAutoAxes(pg.axesShown && !sidesSet);
	for (int i = 0; i < 4; ++i) {
		int mode = -1;
		const QString m = v.value(QString("MAP_FRAME_AXES.") + kSide[i]);
		for (int j = 0; j < 4; ++j) if (m == kMode[j]) mode = j;
		if (!pg.axesShown) mode = 3;
		else if (!sidesSet) continue;                  // the chart's own choice of sides
		else if (mode < 0) mode = (i == 0 || i == 2) ? 0 : 3;   // an unset side: as the chart draws it
		ax[i]->SetVisible(mode != 3);
		ax[i]->SetLabelsVisible(mode == 0);
		ax[i]->SetTicksVisible(mode <= 1);
	}
	// The two data axes: limits, label, annotation interval, grid, number format.
	struct A { vtkAxis *a; const char *x; };
	for (const A &t : { A{ ax[2], "x" }, A{ ax[0], "y" } }) {
		const QString X = t.x;
		if (v.contains("-R." + X + "auto") || v.contains("-R." + X + "min") || v.contains("-R." + X + "max")) {
			bool ok0 = false, ok1 = false;
			const double lo = v.value("-R." + X + "min").toDouble(&ok0), hi = v.value("-R." + X + "max").toDouble(&ok1);
			if (v.value("-R." + X + "auto") != "1" && ok0 && ok1 && hi > lo) {
				t.a->SetBehavior(vtkAxis::FIXED);
				t.a->SetRange(lo, hi);
			}
			else
				t.a->SetBehavior(vtkAxis::AUTO);
		}
		if (v.contains("-B" + X + ".label")) t.a->SetTitle(v.value("-B" + X + ".label").toStdString());
		bool on = false, isAuto = true, gon = false, gauto = true;
		double val = 0.0, ph = 0.0, gval = 0.0, gph = 0.0;
		axpStride(v, "-Bp" + X + ".a", on, isAuto, val, ph);
		if (v.contains("-Bp" + X + ".a") && pg.xTimeFmt == 0) {       // a time axis makes its own ticks
			if (on && !isAuto && val > 0.0) {
				const double lo = t.a->GetMinimum(), hi = t.a->GetMaximum();
				vtkNew<vtkDoubleArray> pos;
				const double k0 = std::ceil((lo - ph) / val);
				for (double k = k0; ph + k * val <= hi + 1e-9 * std::abs(val) && pos->GetNumberOfTuples() < 1000; k += 1.0)
					pos->InsertNextValue(ph + k * val);
				t.a->SetCustomTickPositions(pos);
			}
			else
				t.a->SetCustomTickPositions(nullptr);
			if (X == "y") {                                  // auto: ours again (xyRefreshTicks)
				pg.yTicksUser = on && !isAuto && val > 0.0;
				pg.lastYLo = std::numeric_limits<double>::quiet_NaN();
			}
			if (!on) t.a->SetLabelsVisible(false);
		}
		if (v.contains("-Bp" + X + ".g")) {
			axpStride(v, "-Bp" + X + ".g", gon, gauto, gval, gph);
			t.a->SetGridVisible(gon);
		}
		const QString fmt = v.value("FORMAT_FLOAT_MAP");
		if (fmt.contains('%')) {
			t.a->SetNotation(vtkAxis::PRINTF_NOTATION);
			t.a->SetLabelFormat(fmt.toStdString());
		}
	}
	// The sides' Grid boxes: West = the horizontal lines (the y axis' grid), South = the vertical ones.
	if (v.contains("MAP_FRAME_AXES.Wg")) ax[0]->SetGridVisible(v.value("MAP_FRAME_AXES.Wg") == "1");
	if (v.contains("MAP_FRAME_AXES.Sg")) ax[2]->SetGridVisible(v.value("MAP_FRAME_AXES.Sg") == "1");
	// Pens, tick length, fonts: only what was changed.
	double px = 0.0;
	for (vtkAxis *a : ax) {
		if (axpLengthPx(v.value("MAP_FRAME_PEN.width"), px)) a->GetPen()->SetWidth((float)px);
		const QColor fc(v.value("MAP_FRAME_PEN.color"));
		if (fc.isValid()) a->GetPen()->SetColor((unsigned char)fc.red(), (unsigned char)fc.green(), (unsigned char)fc.blue());
		if (axpLengthPx(v.value("MAP_GRID_PEN_PRIMARY.width"), px)) a->GetGridPen()->SetWidth((float)px);
		const QColor gc(v.value("MAP_GRID_PEN_PRIMARY.color"));
		if (gc.isValid()) a->GetGridPen()->SetColor((unsigned char)gc.red(), (unsigned char)gc.green(), (unsigned char)gc.blue());
		if (axpLengthPx(v.value("MAP_TICK_LENGTH_PRIMARY.major"), px)) a->SetTickLength((float)px);
		axpApplyFont(a->GetLabelProperties(), v, "FONT_ANNOT_PRIMARY");
		axpApplyFont(a->GetTitleProperties(), v, "FONT_LABEL");
	}
	// The title, and the subtitle as its second line (a chart has one title; GMT has -B+t and -B+s).
	if (v.contains("-B.+t") || v.contains("-B.+s")) {
		const QString t = v.contains("-B.+t") ? v.value("-B.+t") : xyChartTitle(c);
		const QString sub = v.value("-B.+s");
		c->SetTitle((sub.isEmpty() ? t : t + "\n" + sub).toStdString());
	}
	axpApplyFont(c->GetTitleProperties(), v, "FONT_TITLE");
	if (v.value("-B.+g") == "1") {
		const QColor bg(v.value("-B.+g.color"));
		if (bg.isValid()) c->GetBackgroundBrush()->SetColor((unsigned char)bg.red(), (unsigned char)bg.green(), (unsigned char)bg.blue(), 255);
	}
	else if (v.contains("-B.+g"))
		c->GetBackgroundBrush()->SetColor(255, 255, 255, 0);
	c->RecalculateBounds();
	xyGridSyncUi(s);                                   // the grid toggles show what was just applied
	if (s && s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// A grid toggle: the current page's grid in direction x (the vertical lines) and / or y, set THROUGH its
// Axes properties — the same "MAP_FRAME_AXES.Sg" / ".Wg" fields the dialog's Grid boxes write — so the
// toggles, the dialog and a saved session never disagree.
static void xyGridSet(XYPlot *s, bool x, bool y, bool on) {
	if (!xyAlive(s) || s->pages.empty()) return;
	XYPage &pg = xyCur(s);
	if (x) pg.axProps["MAP_FRAME_AXES.Sg"] = on ? "1" : "0";
	if (y) pg.axProps["MAP_FRAME_AXES.Wg"] = on ? "1" : "0";
	xyApplyAxes(s, pg);
	xyGridSyncUi(s);                                   // a page with hidden axes is not re-applied: say so anyway
}

// The tool bar icon of a Title / Subtitle / X-Label / Y-Label button (0..3): a little chart on a light
// tile with a bar where that text goes — over it (a thinner, shorter one for the subtitle), under it,
// or beside it.
static QIcon xyAxisTextIcon(int which, double dpr) {
	const int S = 18;
	QPixmap pm((int)std::ceil(S * dpr), (int)std::ceil(S * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter g(&pm);
	g.setRenderHint(QPainter::Antialiasing, true);
	const QRectF tile = which == 3 ? QRectF(4.5, 3.5, 13, 12) : which == 2 ? QRectF(2.5, 1.5, 13, 12) : QRectF(2.5, 4.5, 13, 12);
	g.setPen(QPen(QColor(100, 100, 100), 1.0));
	g.setBrush(QColor(244, 244, 244));
	g.drawRoundedRect(tile, 1.5, 1.5);
	g.setPen(QPen(QColor(150, 150, 150), 1.0));                 // the chart's zigzag
	const QPointF c = tile.center();
	const QPointF zz[4] = { c + QPointF(-4.5, 1.5), c + QPointF(-1.5, -1.5), c + QPointF(1.5, 1.5), c + QPointF(4.5, -1.5) };
	g.drawPolyline(zz, 4);
	g.setPen(Qt::NoPen);
	g.setBrush(QColor(110, 110, 110));
	switch (which) {
	case 0:  g.drawRect(QRectF(4.0, 1.0, 10.0, 2.0)); break;        // title: a bar over it
	case 1:  g.drawRect(QRectF(5.5, 1.5, 7.0, 1.2)); break;         // subtitle: a thinner, shorter one
	case 2:  g.drawRect(QRectF(4.0, 15.0, 10.0, 2.0)); break;       // x label: under it
	default: g.drawRect(QRectF(1.0, 5.5, 2.0, 9.0)); break;         // y label: beside it
	}
	return QIcon(pm);
}

// The tool bar icon of a grid toggle (0 Grid, 1 X-Grid, 2 Y-Grid): a framed tile ruled as it draws.
static QIcon xyGridIcon(int mode, double dpr) {
	const int S = 18;
	QPixmap pm((int)std::ceil(S * dpr), (int)std::ceil(S * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter g(&pm);
	g.setRenderHint(QPainter::Antialiasing, true);
	g.setPen(QPen(QColor(100, 100, 100), 1.0));
	g.setBrush(QColor(244, 244, 244));
	g.drawRoundedRect(QRectF(0.5, 1.5, S - 1, S - 3), 2, 2);
	g.setPen(QPen(QColor(150, 150, 150), 1.0));
	for (int k = 1; k <= 3; ++k) {
		if (mode != 2) g.drawLine(QPointF(0.5 + k * 4.25, 2.5), QPointF(0.5 + k * 4.25, S - 2.5));   // vertical
		if (mode != 1) g.drawLine(QPointF(1.5, 1.5 + k * 3.75), QPointF(S - 1.5, 1.5 + k * 3.75));    // horizontal
	}
	return QIcon(pm);
}

// The Axes properties dialog of ONE X,Y page: the SAME deps/ui/axes_props.ui the 3-D viewer's rasters
// use, read and written with the same field helpers (axpKey / axpGet / axpSet / axpPaintSwatch), bound
// here to a page (found again by its chart on every use: pages move when one is deleted). Only Apply
// acts. The page keeps the fields changed from the .ui's defaults; xyApplyAxes draws them.
static XYPage *xyPageOfChart(XYPlot *s, vtkChartXY *c) {
	if (!xyAlive(s)) return nullptr;
	for (XYPage &pg : s->pages) if (pg.chart == c) return &pg;
	return nullptr;
}

static void xyAxesPropsOpen(XYPlot *s) {
	if (!xyAlive(s)) return;
	vtkChartXY *chart = xyCur(s).chart;
	const QString objName = QString("xyAxesProps_%1").arg((quintptr)chart);
	for (QDialog *d : s->win->findChildren<QDialog *>(objName)) { d->show(); d->raise(); d->activateWindow(); return; }
	QFile uif(QDir(gmtvtkUiDir()).filePath("axes_props.ui"));
	QUiLoader loader;
	QDialog *dlg = uif.open(QIODevice::ReadOnly) ? qobject_cast<QDialog *>(loader.load(&uif, s->win)) : nullptr;
	uif.close();
	if (!dlg) { QMessageBox::warning(s->win, "Axes properties", "Cannot load " + uif.fileName()); return; }
	auto *tabs = dlg->findChild<QTabWidget *>("tabs");
	auto *apply = dlg->findChild<QPushButton *>("btnApply");
	auto *defs = dlg->findChild<QPushButton *>("btnDefaults");
	if (!(tabs && apply && defs)) {
		QMessageBox::warning(s->win, "Axes properties", "axes_props.ui does not carry this dialog's controls (a renamed or removed widget).");
		delete dlg;
		return;
	}
	dlg->setObjectName(objName);
	// The font lists and -B units, as the 3-D dialog fills them.
	for (QComboBox *cb : dlg->findChildren<QComboBox *>())
		if (cb->property("gmtPart").toString() == "name" && cb->count() == 0) {
			for (const char *f : kGmtFontNames) cb->addItem(f);
			if (cb->property("gmtKey").toString() == "FONT") cb->insertItem(0, QString());
			cb->setCurrentIndex(0);
		}
	auto fields = std::make_shared<std::vector<QWidget *>>();
	auto defaults = std::make_shared<QMap<QString, QString>>();
	for (QWidget *w : dlg->findChildren<QWidget *>()) {
		const QString key = axpKey(w);
		if (key.isEmpty()) continue;
		fields->push_back(w);
		if (auto *b = qobject_cast<QPushButton *>(w)) axpPaintSwatch(b, b->property("gmtColor").toString());
		(*defaults)[key] = axpGet(w);
		if (!xyAxpHonoured(key) || w->property("vertical").toBool()) w->setEnabled(false);   // not drawn on a 2-D chart
	}
	for (QWidget *w : dlg->findChildren<QWidget *>())   // a vertical (Z / 3-D) tab: off as a tab
		if (w->property("vertical").toBool()) { const int ti = tabs->indexOf(w); if (ti >= 0) tabs->setTabEnabled(ti, false); }
	for (int ti = 0; ti < tabs->count(); ++ti) {        // a tab with nothing a 2-D chart draws: off
		bool any = false;
		for (QWidget *w : *fields) if (w->isEnabled() && tabs->widget(ti)->isAncestorOf(w)) { any = true; break; }
		if (!any) tabs->setTabEnabled(ti, false);
	}
	auto field = [fields](const QString &k) -> QWidget * {
		for (QWidget *w : *fields) if (axpKey(w) == k) return w;
		return nullptr;
	};
	// Defaults, then the page's current labels and limits, then what the page has stored.
	auto fill = [s, chart, fields, defaults, field](bool defaultsOnly) {
		for (QWidget *w : *fields) axpSet(w, defaults->value(axpKey(w)));
		XYPage *pg = xyPageOfChart(s, chart);
		if (!pg) return;
		vtkAxis *bx = chart->GetAxis(vtkAxis::BOTTOM), *ly = chart->GetAxis(vtkAxis::LEFT);
		if (QWidget *w = field("-Bx.label")) axpSet(w, QString::fromStdString(bx->GetTitle()));
		if (QWidget *w = field("-By.label")) axpSet(w, QString::fromStdString(ly->GetTitle()));
		if (QWidget *w = field("-B.+t")) axpSet(w, xyChartTitle(chart));   // its subtitle line is -B.+s's
		struct L { vtkAxis *a; const char *x; };
		for (const L &l : { L{ bx, "x" }, L{ ly, "y" } }) {
			if (QWidget *w = field(QString("-R.") + l.x + "min")) axpSet(w, QString::number(l.a->GetMinimum(), 'g', 12));
			if (QWidget *w = field(QString("-R.") + l.x + "max")) axpSet(w, QString::number(l.a->GetMaximum(), 'g', 12));
			if (QWidget *w = field(QString("-R.") + l.x + "auto")) axpSet(w, l.a->GetBehavior() == vtkAxis::FIXED ? "0" : "1");
		}
		// the grid as drawn — the tool bar's Grid / X-Grid / Y-Grid toggles set it too — and its pen colour
		if (!defaultsOnly) {
			if (QWidget *w = field("MAP_FRAME_AXES.Sg")) axpSet(w, bx->GetGridVisible() ? "1" : "0");
			if (QWidget *w = field("MAP_FRAME_AXES.Wg")) axpSet(w, ly->GetGridVisible() ? "1" : "0");
			unsigned char gc[4];
			bx->GetGridPen()->GetColor(gc);
			if (QWidget *w = field("MAP_GRID_PEN_PRIMARY.color")) axpSet(w, QColor(gc[0], gc[1], gc[2]).name());
		}
		if (!defaultsOnly)
			for (const auto &kv : pg->axProps)
				if (QWidget *w = field(QString::fromStdString(kv.first))) axpSet(w, QString::fromStdString(kv.second));
	};
	for (QCheckBox *cb : { dlg->findChild<QCheckBox *>("chkDataLimitsX"), dlg->findChild<QCheckBox *>("chkDataLimitsY") }) {
		if (!cb) continue;
		const QString X = cb->objectName().endsWith('X') ? "X" : "Y";
		auto sync = [dlg, cb, X] {
			for (const char *e : { "min", "max" })
				if (QLineEdit *le = dlg->findChild<QLineEdit *>("edit" + X + e)) le->setEnabled(!cb->isChecked());
		};
		QObject::connect(cb, &QCheckBox::toggled, dlg, sync);
	}
	for (QWidget *w : *fields)                          // colour buttons pick their colour
		if (auto *b = qobject_cast<QPushButton *>(w))
			QObject::connect(b, &QPushButton::clicked, dlg, [dlg, b] {
				const QColor c = QColorDialog::getColor(QColor(b->property("gmtColor").toString()), dlg, "Colour");
				if (c.isValid()) axpPaintSwatch(b, c.name());
			});
	QObject::connect(apply, &QPushButton::clicked, dlg, [s, chart, fields, defaults, dlg] {
		XYPage *pg = xyPageOfChart(s, chart);
		if (!pg) {
			QMessageBox::warning(dlg, "Axes properties", "The page these axes belong to is no longer in the window.");
			dlg->close();
			return;
		}
		pg->axProps.clear();
		for (QWidget *w : *fields) {
			const QString k = axpKey(w), val = axpGet(w);
			// the two Grid boxes always: "off" must turn off a grid the toggles turned on, default or not
			const bool grid = (k == "MAP_FRAME_AXES.Sg" || k == "MAP_FRAME_AXES.Wg");
			if (w->isEnabled() && (grid || defaults->value(k) != val)) pg->axProps[k.toStdString()] = val.toStdString();
		}
		xyApplyAxes(s, *pg);
	});
	QObject::connect(defs, &QPushButton::clicked, dlg, [fill] { fill(true); });
	// Only Apply acts: Enter in a box must never press a button.
	for (QPushButton *b : dlg->findChildren<QPushButton *>()) { b->setAutoDefault(false); b->setDefault(false); }
	struct NoEnter : QObject {
		using QObject::QObject;
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::KeyPress) {
				const int k = static_cast<QKeyEvent *>(e)->key();
				if (k == Qt::Key_Return || k == Qt::Key_Enter) return true;
			}
			return QObject::eventFilter(o, e);
		}
	};
	dlg->installEventFilter(new NoEnter(dlg));
	dlg->setWindowIcon(appIcon());
	dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	const QString pn = QString::fromStdString(xyCur(s).name);
	dlg->setWindowTitle(pn.isEmpty() ? QString("Axes properties") : "Axes properties - " + pn);
	fill(false);
	for (QCheckBox *cb : { dlg->findChild<QCheckBox *>("chkDataLimitsX"), dlg->findChild<QCheckBox *>("chkDataLimitsY") })
		if (cb) emit cb->toggled(cb->isChecked());
	wrapTooltips(dlg);
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}

static void xyMagPaint(XYPlot *s, vtkChartXY *chart, vtkContext2D *p);   // the magnetic picks (below)
static void xyAnnotPaint(XYPlot *s, vtkChartXY *chart, vtkContext2D *p); // the annotations (below)
static void xyRefPaint(XYPlot *s, vtkChartXY *chart, vtkContext2D *p);   // the reference picks (below)

class XYChart : public vtkChartXY {
public:
	static XYChart *New();
	vtkTypeMacro(XYChart, vtkChartXY);
	XYPlot *owner = nullptr;                            // set right after New(); never reparented
	// A dragged legend (CUSTOM alignment, XYLegendFilter) has its place as its box's bottom-left corner
	// in FRACTIONS of the scene: the one source of its position, written by the drag and by a loaded
	// session, read by every paint. A scene that changes size (a resize, a reopened window still
	// being laid out) keeps the legend where it was put; px are only ever derived from it.
	float legFx = 0.0f, legFy = 0.0f;
	float legBoxW = 0.0f, legBoxH = 0.0f;               // its box at the last paint, px (the drag's limits)
	bool Paint(vtkContext2D *painter) override {
		vtkChartLegend *lg = this->GetLegend();
		const float W = this->GetScene() ? (float)this->GetScene()->GetSceneWidth() : 0.0f;
		const float H = this->GetScene() ? (float)this->GetScene()->GetSceneHeight() : 0.0f;
		if (lg && lg->GetVisible() && lg->GetHorizontalAlignment() == vtkChartLegend::CUSTOM && W > 0.0f && H > 0.0f) {
			const vtkRectf box = lg->GetBoundingRect(painter);   // wholly inside the scene, however small
			legBoxW = box.GetWidth();  legBoxH = box.GetHeight();
			const float x = std::max(0.0f, std::min(legFx * W, W - box.GetWidth()));
			const float y = std::max(0.0f, std::min(legFy * H, H - box.GetHeight()));
			const float *p = lg->GetPoint();
			if (x != p[0] || y != p[1]) lg->SetPoint(x, y);
		}
		const bool r = vtkChartXY::Paint(painter);
		if (owner && xyAlive(owner)) xyLabelPaint(owner, this, painter);  // the page's own labels
		if (owner && xyAlive(owner)) xyAnnotPaint(owner, this, painter);  // the hand-drawn annotations
		if (owner && xyAlive(owner)) xyMagPaint(owner, this, painter);    // chron names, isochron picks
		if (owner && xyAlive(owner)) xyRefPaint(owner, this, painter);    // the reference pick markers
		if (owner && xyAlive(owner)) xyMarkPaint(owner, this, painter);   // the curve marker, on top
		return r;
	}
protected:
	void SetTooltipInfo(const vtkContextMouseEvent &mouse, const vtkVector2d &plotPos,
	                    vtkIdType seriesIndex, vtkPlot *plot, vtkIdType segmentIndex) override {
		const int fmt = (owner && !owner->pages.empty()) ? owner->pages[owner->curPageIdx].xTimeFmt : 0;
		if (fmt == 0 || !this->GetTooltip()) {
			vtkChartXY::SetTooltipInfo(mouse, plotPos, seriesIndex, plot, segmentIndex);
			return;
		}
		const QString lab = xyCoordText(fmt, plotPos.GetX(), plotPos.GetY());
		this->GetTooltip()->SetText(lab.toUtf8().constData());
		this->GetTooltip()->SetPosition(mouse.GetScenePos()[0] + 2, mouse.GetScenePos()[1] + 2);
	}
};
vtkStandardNewMacro(XYChart);

// Mouse for the curve marker, on the chart widget. Consumes only what is the marker's (a press on a
// curve or on the marker, its drag, its double-click), so the chart's own pan/zoom keeps everything
// else. Stands aside while the Spector-Grant band tool owns the left button.
class XYMarkFilter : public QObject {
public:
	XYPlot *s = nullptr;
	explicit XYMarkFilter(XYPlot *sc, QObject *parent) : QObject(parent), s(sc) {}
protected:
	void render() { if (s->widget->renderWindow()) s->widget->renderWindow()->Render(); }
	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (!s || !xyAlive(s) || s->pages.empty() || xyCur(s).sgActive)
			return QObject::eventFilter(obj, ev);
		XYPage &pg = xyCur(s);
		const QEvent::Type t = ev->type();
		if (t != QEvent::MouseButtonPress && t != QEvent::MouseButtonDblClick &&
		    t != QEvent::MouseMove && t != QEvent::MouseButtonRelease)
			return QObject::eventFilter(obj, ev);
		QMouseEvent *me = static_cast<QMouseEvent *>(ev);
		const QPointF qp = me->position();
		double sx, sy;
		xyQtToScene(s, qp, sx, sy);
		const double tol = 6.0 * s->widget->devicePixelRatioF();
		int sel = -1;
		double u = 0.0;
		if (t == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
			pg.markPress = qp;  pg.markMoved = false;
			if (xyMarkHit(s, qp)) { pg.markDrag = true; pg.markNew = false; return true; }
			if (xyCurveNearest(pg, -1, sx, sy, tol, sel, u)) {   // click ON a curve: drop it there, grabbed
				pg.markSel = sel;  pg.markU = u;  pg.markDrag = true;  pg.markNew = true;
				xyMarkToPanel(s);
				render();
				return true;
			}
		}
		else if (t == QEvent::MouseButtonDblClick && me->button() == Qt::LeftButton) {
			// The double-click's first press already ran above: if it PLACED the marker, this
			// double-click is the placing one and keeps it; on an older marker it removes it.
			pg.markDrag = false;
			if (xyMarkHit(s, qp)) {
				if (!pg.markNew) pg.markSel = -1;
				xyMarkToPanel(s);
				pg.markNew = false;
				render();
				return true;
			}
			if (xyCurveNearest(pg, -1, sx, sy, tol, sel, u)) {
				pg.markSel = sel;  pg.markU = u;  pg.markNew = false;
				xyMarkToPanel(s);
				render();
				return true;
			}
		}
		else if (t == QEvent::MouseMove) {
			if (pg.markDrag) {
				if (!pg.markMoved && (qp - pg.markPress).manhattanLength() > 3) pg.markMoved = true;
				// slide along ITS curve: the nearest point of that series, however far the cursor strays
				if (pg.markMoved && xyCurveNearest(pg, pg.markSel, sx, sy, 1e9, sel, u)) {
					pg.markU = u;
					xyMarkToPanel(s);
					render();
				}
				return true;
			}
			const bool over = xyMarkHit(s, qp);       // the same four-arrow cursor as every draggable
			const bool isAll = s->widget->cursor().shape() == Qt::SizeAllCursor;
			if (over && !isAll)       s->widget->setCursor(Qt::SizeAllCursor);
			else if (!over && isAll)  s->widget->unsetCursor();
		}
		else if (t == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton && pg.markDrag) {
			pg.markDrag = false;
			return true;
		}
		return QObject::eventFilter(obj, ev);
	}
};

// Mouse for the chart legend: a left press inside its box grabs it and the drag moves it, box and
// contents together, with the four-arrow cursor over it like every draggable. The first grab turns
// the legend's alignment to CUSTOM, so the chart stops re-placing it in its corner on every paint
// (vtkChartXY::SetLegendPosition) and it stays where it was dropped. Installed LAST, so it sees a
// press before every tool's filter: the legend is drawn on top of the curves, so a press on it is
// the legend's.
class XYLegendFilter : public QObject {
public:
	XYPlot *s = nullptr;
	explicit XYLegendFilter(XYPlot *sc, QObject *parent) : QObject(parent), s(sc) {}
protected:
	bool   drag = false;
	double lastX = 0.0, lastY = 0.0;                  // previous drag position, scene px
	bool eventFilter(QObject *obj, QEvent *ev) override {
		const QEvent::Type t = ev->type();
		if (t != QEvent::MouseButtonPress && t != QEvent::MouseButtonDblClick &&
		    t != QEvent::MouseMove && t != QEvent::MouseButtonRelease)
			return QObject::eventFilter(obj, ev);
		if (!s || !xyAlive(s) || s->pages.empty() || !xyCur(s).chart) { drag = false; return QObject::eventFilter(obj, ev); }
		vtkChartXY *chart = xyCur(s).chart;
		vtkChartLegend *lg = chart->GetLegend();
		if (!lg) return QObject::eventFilter(obj, ev);
		QMouseEvent *me = static_cast<QMouseEvent *>(ev);
		double sx, sy;
		xyQtToScene(s, me->position(), sx, sy);
		if (t == QEvent::MouseButtonRelease) {
			if (drag && me->button() == Qt::LeftButton) { drag = false; return true; }
			return QObject::eventFilter(obj, ev);
		}
		if (t == QEvent::MouseMove && drag) {
			XYChart *c = static_cast<XYChart *>(chart);
			const float W = (float)chart->GetScene()->GetSceneWidth(), H = (float)chart->GetScene()->GetSceneHeight();
			if (W > 0.0f && H > 0.0f) {                // the place is a fraction (XYChart::legFx); Paint sets the px
				// kept inside the scene here too, so a drag past an edge does not have to be undone
				c->legFx = std::max(0.0f, std::min(c->legFx + (float)(sx - lastX) / W, std::max(0.0f, (W - c->legBoxW) / W)));
				c->legFy = std::max(0.0f, std::min(c->legFy + (float)(sy - lastY) / H, std::max(0.0f, (H - c->legBoxH) / H)));
			}
			lastX = sx;  lastY = sy;
			chart->GetScene()->SetDirty(true);
			if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
			return true;
		}
		vtkContextMouseEvent cm;
		cm.SetPos(vtkVector2f((float)sx, (float)sy));
		const bool over = chart->GetShowLegend() && lg->Hit(cm);   // the box as last painted
		if (t == QEvent::MouseMove) {
			if (!over) return QObject::eventFilter(obj, ev);
			if (s->widget->cursor().shape() != Qt::SizeAllCursor) s->widget->setCursor(Qt::SizeAllCursor);
			return true;                              // over the legend, no other tool's hover
		}
		if (!over || me->button() != Qt::LeftButton) return QObject::eventFilter(obj, ev);
		if (t == QEvent::MouseButtonPress) {
			if (lg->GetHorizontalAlignment() != vtkChartLegend::CUSTOM) {   // first grab: from where it is
				XYChart *c = static_cast<XYChart *>(chart);
				const float W = (float)chart->GetScene()->GetSceneWidth(), H = (float)chart->GetScene()->GetSceneHeight();
				if (W <= 0.0f || H <= 0.0f) return true;
				c->legFx = lg->GetPoint()[0] / W;  c->legFy = lg->GetPoint()[1] / H;
				lg->SetHorizontalAlignment(vtkChartLegend::CUSTOM);
				lg->SetVerticalAlignment(vtkChartLegend::CUSTOM);
			}
			drag = true;
			lastX = sx;  lastY = sy;
		}
		return true;                                  // a double-click on the legend is the legend's too
	}
};

// Scene px -> data along axis `a` (k: 0 x, 1 y), the inverse of xyAxisToScene.
static double xySceneToAxis(vtkAxis *a, double s, int k) {
	const double lo = a->GetMinimum(), hi = a->GetMaximum();
	const float *p1 = a->GetPoint1(), *p2 = a->GetPoint2();
	const double v = (p2[k] == p1[k]) ? lo : lo + (s - p1[k]) / (p2[k] - p1[k]) * (hi - lo);
	return a->GetLogScaleActive() ? std::pow(10.0, v) : v;
}

// Mouse for the page's own labels (XYPage::labels: a biplot's variable names, a radar's spoke names,
// ...): a left press on one grabs it and the drag moves it, with the four-arrow cursor over it as over
// every draggable. It keeps its DATA position, so it still moves with zoom and pan, and a saved session
// keeps it where it was dropped. Installed just before the legend's filter: the legend, drawn on top,
// wins a press on both.
class XYLabelFilter : public QObject {
public:
	XYPlot *s = nullptr;
	explicit XYLabelFilter(XYPlot *sc, QObject *parent) : QObject(parent), s(sc) {}
protected:
	int    grab = -1;                                 // the label being dragged, index in the page's list
	double offX = 0.0, offY = 0.0;                    // press point -> label anchor, scene px
	int hit(XYPage &pg, double sx, double sy) {
		const float pad = 3.0f * (s->widget ? (float)s->widget->devicePixelRatioF() : 1.0f);
		for (int i = (int)pg.labels.size() - 1; i >= 0; --i) {   // the last drawn is on top
			const XYLabel &l = pg.labels[i];
			if (l.shown && sx >= l.bx - pad && sx <= l.bx + l.bw + pad && sy >= l.by - pad && sy <= l.by + l.bh + pad)
				return i;
		}
		return -1;
	}
	bool eventFilter(QObject *obj, QEvent *ev) override {
		const QEvent::Type t = ev->type();
		if (t != QEvent::MouseButtonPress && t != QEvent::MouseButtonDblClick &&
		    t != QEvent::MouseMove && t != QEvent::MouseButtonRelease)
			return QObject::eventFilter(obj, ev);
		if (!s || !xyAlive(s) || s->pages.empty() || !xyCur(s).chart) { grab = -1; return QObject::eventFilter(obj, ev); }
		XYPage &pg = xyCur(s);
		QMouseEvent *me = static_cast<QMouseEvent *>(ev);
		double sx, sy;
		xyQtToScene(s, me->position(), sx, sy);
		if (t == QEvent::MouseButtonRelease) {
			if (grab >= 0 && me->button() == Qt::LeftButton) { grab = -1; return true; }
			return QObject::eventFilter(obj, ev);
		}
		if (t == QEvent::MouseMove && grab >= 0) {
			if (grab < (int)pg.labels.size()) {
				XYLabel &l = pg.labels[grab];
				l.x = xySceneToAxis(pg.chart->GetAxis(vtkAxis::BOTTOM), sx + offX, 0);
				l.y = xySceneToAxis(pg.chart->GetAxis(vtkAxis::LEFT), sy + offY, 1);
				pg.chart->GetScene()->SetDirty(true);
				if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
			}
			return true;
		}
		const int i = hit(pg, sx, sy);
		if (t == QEvent::MouseMove) {
			if (i < 0) return QObject::eventFilter(obj, ev);
			if (s->widget->cursor().shape() != Qt::SizeAllCursor) s->widget->setCursor(Qt::SizeAllCursor);
			return true;                              // over a label, no other tool's hover
		}
		if (i < 0 || me->button() != Qt::LeftButton) return QObject::eventFilter(obj, ev);
		if (t == QEvent::MouseButtonPress) {
			double ax, ay;
			if (!xyDataToScene(pg, pg.labels[i].x, pg.labels[i].y, ax, ay)) return QObject::eventFilter(obj, ev);
			grab = i;
			offX = ax - sx;  offY = ay - sy;
		}
		return true;
	}
};

// ---- annotations (the tool bar's Text Arrow ... Ellipse tools) -------------------------------------

// The head of an arrow: a filled triangle with its tip at (tx,ty), pointing away from (fx,fy). Same
// shape as an arrow series' (vtkXYShapePlot, kind 11): length 3*max(2.5, w)+4 px, half width 0.35 of it.
static float xyAnnotHeadLen(const XYAnnot &a, float dpr) { return (float)(3.0 * std::max(2.5, a.width) + 4.0) * dpr; }
static void xyAnnotHead(vtkContext2D *p, float tx, float ty, float fx, float fy, float hl) {
	const float dx = tx - fx, dy = ty - fy, L = std::hypot(dx, dy);
	if (!(L > 0.0f)) return;
	const float ux = dx / L, uy = dy / L, hw = 0.35f * hl;
	float pts[6] = { tx, ty, tx - ux * hl - uy * hw, ty - uy * hl + ux * hw, tx - ux * hl + uy * hw, ty - uy * hl - ux * hw };
	p->DrawPolygon(pts, 3);
}

// The page's annotations, at their data positions, with the selected one's grab handles. Not clipped
// to the plot frame: an annotation belongs to the figure, as in MATLAB, and may point at anything.
static void xyAnnotPaint(XYPlot *s, vtkChartXY *chart, vtkContext2D *p) {
	if (!s || s->pages.empty()) return;
	XYPage *pg = nullptr;
	for (auto &q : s->pages) if (q.chart.Get() == chart) { pg = &q; break; }
	if (!pg || pg->annots.empty()) return;
	const float dpr = s->widget ? (float)s->widget->devicePixelRatioF() : 1.0f;
	vtkAxis *ax = chart->GetAxis(vtkAxis::BOTTOM), *ay = chart->GetAxis(vtkAxis::LEFT);
	for (int i = 0; i < (int)pg->annots.size(); ++i) {
		XYAnnot &a = pg->annots[i];
		a.shown = false;
		a.tbw = 0;
		double sx0, sy0, sx1, sy1;
		if (!a.visible || !xyDataToScene(*pg, a.x0, a.y0, sx0, sy0) || !xyDataToScene(*pg, a.x1, a.y1, sx1, sy1)) continue;
		vtkTextProperty *tp = p->GetTextProp();
		tp->SetColor(a.r, a.g, a.b);
		tp->SetFontSize((int)std::lround(a.size));
		tp->SetBold(false);
		if (a.kind == XY_AN_TEXTBOX && a.fit) {       // made by a click: the box wraps its text
			float b[4] = { 0, 0, 0, 0 };
			const std::string t = a.text.empty() ? std::string("Text") : a.text;
			tp->SetJustificationToLeft(); tp->SetVerticalJustificationToBottom();
			p->ComputeStringBounds(t, b);
			const float pad = 4.0f * dpr;
			sx1 = sx0 + b[2] + 2 * pad;  sy1 = sy0 - b[3] - 2 * pad;
			a.x1 = xySceneToAxis(ax, sx1, 0);  a.y1 = xySceneToAxis(ay, sy1, 1);
		}
		a.sx0 = (float)sx0; a.sy0 = (float)sy0; a.sx1 = (float)sx1; a.sy1 = (float)sy1;
		p->GetPen()->SetColorF(a.r, a.g, a.b);
		p->GetPen()->SetWidth((float)a.width * dpr);
		p->GetPen()->SetLineType(a.lineType);
		if (xyAnnotIsLine(a.kind)) {
			const float hl = xyAnnotHeadLen(a, dpr);
			const float L = std::hypot(a.sx1 - a.sx0, a.sy1 - a.sy0);
			const float ux = L > 0 ? (a.sx1 - a.sx0) / L : 0, uy = L > 0 ? (a.sy1 - a.sy0) / L : 0;
			const bool he = a.kind != XY_AN_LINE, hb = a.kind == XY_AN_DARROW;
			// the shaft stops inside the head, so a thick pen never pokes out past its tip
			const float e = (he && L > hl) ? 0.8f * hl : 0.0f, bgn = (hb && L > 2 * hl) ? 0.8f * hl : 0.0f;
			p->DrawLine(a.sx0 + ux * bgn, a.sy0 + uy * bgn, a.sx1 - ux * e, a.sy1 - uy * e);
			p->GetPen()->SetLineType(vtkPen::SOLID_LINE);
			p->GetBrush()->SetColorF(a.r, a.g, a.b, 1.0);
			if (he) xyAnnotHead(p, a.sx1, a.sy1, a.sx0, a.sy0, hl);
			if (hb) xyAnnotHead(p, a.sx0, a.sy0, a.sx1, a.sy1, hl);
			if (a.kind == XY_AN_TEXTARROW && !a.text.empty()) {
				// the text sits at the TAIL, on the far side from the head
				const float off = 3.0f * dpr;
				if (std::abs(ux) >= std::abs(uy)) {
					if (ux >= 0) tp->SetJustificationToRight(); else tp->SetJustificationToLeft();
					tp->SetVerticalJustificationToCentered();
				}
				else {
					tp->SetJustificationToCentered();
					if (uy >= 0) tp->SetVerticalJustificationToTop(); else tp->SetVerticalJustificationToBottom();
				}
				const float tx = a.sx0 - ux * off, ty = a.sy0 - uy * off;
				p->DrawString(tx, ty, a.text);
				float b[4];
				p->ComputeJustifiedStringBounds(a.text.c_str(), b);
				a.tbx = tx + b[0]; a.tby = ty + b[1]; a.tbw = b[2]; a.tbh = b[3];
			}
		}
		else {
			const float bx = std::min(a.sx0, a.sx1), by = std::min(a.sy0, a.sy1);
			const float bw = std::abs(a.sx1 - a.sx0), bh = std::abs(a.sy1 - a.sy0);
			p->GetBrush()->SetColorF(a.fr, a.fg, a.fb, a.filled ? 1.0 : 0.0);
			if (a.kind == XY_AN_ELLIPSE) p->DrawEllipse(bx + bw / 2, by + bh / 2, bw / 2, bh / 2);
			else if (a.kind != XY_AN_TEXTBOX || a.boxed) p->DrawRect(bx, by, bw, bh);   // a text box may hide its box
			if (a.kind == XY_AN_TEXTBOX && !a.text.empty()) {   // top-left inside the box, as MATLAB's
				const float pad = 4.0f * dpr;
				tp->SetJustificationToLeft(); tp->SetVerticalJustificationToTop();
				p->DrawString(bx + pad, by + bh - pad, a.text);
			}
		}
		a.shown = true;
		if (i == pg->annotSel) {                       // the grab handles
			const float h = 3.5f * dpr;
			p->GetPen()->SetColor(0, 0, 0, 255);
			p->GetPen()->SetWidth(1.0f);
			p->GetPen()->SetLineType(vtkPen::SOLID_LINE);
			p->GetBrush()->SetColor(255, 255, 255, 255);
			std::vector<std::pair<float, float>> hs;
			if (xyAnnotIsLine(a.kind)) hs = { { a.sx0, a.sy0 }, { a.sx1, a.sy1 } };
			else hs = { { a.sx0, a.sy0 }, { a.sx1, a.sy0 }, { a.sx1, a.sy1 }, { a.sx0, a.sy1 } };
			for (const auto &c : hs) p->DrawRect(c.first - h, c.second - h, 2 * h, 2 * h);
		}
	}
}

// What a press at scene (sx,sy) is on: the topmost annotation's index (-1 none) and its part: -1 the
// body, else the handle — 0/1 a line's ends, 0..3 a box's corners (x0y0, x1y0, x1y1, x0y1). With
// `only` >= 0 just that annotation is tested (the one in edit).
static int xyAnnotHit(XYPlot *s, const XYPage &pg, double sx, double sy, int &part, int only = -1) {
	const double dpr = s->widget ? s->widget->devicePixelRatioF() : 1.0;
	const double tol = 5.0 * dpr, htol = 7.0 * dpr;
	part = -1;
	for (int i = (int)pg.annots.size() - 1; i >= 0; --i) {     // the last drawn is on top
		const XYAnnot &a = pg.annots[i];
		if (!a.shown || (only >= 0 && i != only)) continue;
		std::vector<std::pair<double, double>> hs;
		if (xyAnnotIsLine(a.kind)) hs = { { a.sx0, a.sy0 }, { a.sx1, a.sy1 } };
		else hs = { { a.sx0, a.sy0 }, { a.sx1, a.sy0 }, { a.sx1, a.sy1 }, { a.sx0, a.sy1 } };
		for (int k = 0; k < (int)hs.size(); ++k)
			if (std::hypot(sx - hs[k].first, sy - hs[k].second) <= htol) { part = k; return i; }
		if (xyAnnotIsLine(a.kind)) {
			const double vx = a.sx1 - a.sx0, vy = a.sy1 - a.sy0, L2 = vx * vx + vy * vy;
			const double t = L2 > 0 ? std::max(0.0, std::min(1.0, ((sx - a.sx0) * vx + (sy - a.sy0) * vy) / L2)) : 0.0;
			if (std::hypot(sx - (a.sx0 + t * vx), sy - (a.sy0 + t * vy)) <= tol) return i;
			if (a.tbw > 0 && sx >= a.tbx - 3 && sx <= a.tbx + a.tbw + 3 && sy >= a.tby - 3 && sy <= a.tby + a.tbh + 3) return i;
			continue;
		}
		const double bx0 = std::min(a.sx0, a.sx1), bx1 = std::max(a.sx0, a.sx1);
		const double by0 = std::min(a.sy0, a.sy1), by1 = std::max(a.sy0, a.sy1);
		// a text box is grabbed anywhere inside (it is a block of text); an empty shape by its outline,
		// so the chart under a big rectangle can still be panned and zoomed
		const bool inside = a.kind == XY_AN_TEXTBOX || a.filled;
		if (a.kind == XY_AN_ELLIPSE) {
			const double rx = std::max(1e-9, (bx1 - bx0) / 2), ry = std::max(1e-9, (by1 - by0) / 2);
			const double r = std::hypot((sx - (bx0 + rx)) / rx, (sy - (by0 + ry)) / ry);
			if (std::abs(r - 1.0) * std::min(rx, ry) <= tol || (inside && r < 1.0)) return i;
			continue;
		}
		const bool in = sx >= bx0 - tol && sx <= bx1 + tol && sy >= by0 - tol && sy <= by1 + tol;
		const bool deep = sx > bx0 + tol && sx < bx1 - tol && sy > by0 + tol && sy < by1 - tol;
		if (in && (inside || !deep)) return i;
	}
	return -1;
}

static void xyAnnotRender(XYPlot *s) {
	if (!xyAlive(s) || s->pages.empty()) return;
	if (xyCur(s).chart && xyCur(s).chart->GetScene()) xyCur(s).chart->GetScene()->SetDirty(true);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

static void xyAnnotDelete(XYPlot *s, int i) {
	XYPage &pg = xyCur(s);
	if (i < 0 || i >= (int)pg.annots.size()) return;
	pg.annots.erase(pg.annots.begin() + i);
	if (pg.annotSel == i) pg.annotSel = -1;
	else if (pg.annotSel > i) --pg.annotSel;
	xyRebuildObjMgr(s);
	xyAnnotRender(s);
}

// THE in-place text editor of the X,Y chart: a line edit over the chart, where the text goes (`place`
// gets it sized and returns its top-left, widget px; kept inside the widget here). Enter, or leaving
// the box, hands what was typed to `keep`; Esc drops it. One at a time: a new one replaces an open one.
// The annotation tools and the Title / Subtitle / X-Label / Y-Label buttons all type through it.
static void xyEditInPlace(XYPlot *s, const QString &text, const QString &tip,
                          const std::function<QPoint(QLineEdit *)> &place, std::function<void(const QString &)> keep) {
	if (!xyAlive(s) || !s->widget) return;
	if (s->annotEdit) s->annotEdit->deleteLater();
	auto *e = new QLineEdit(s->widget);
	e->setObjectName("xyTextEdit");
	e->setText(text);
	e->setPlaceholderText("Type the text, Enter");
	e->setToolTip(tip + " · Enter keeps it, Esc leaves it as it was");
	e->resize(220, e->sizeHint().height());
	const QPoint at = place(e);
	e->move(std::max(0, std::min(at.x(), s->widget->width() - e->width())),
	        std::max(0, std::min(at.y(), s->widget->height() - e->height())));
	s->annotEdit = e;
	auto done = std::make_shared<bool>(false);
	auto commit = [s, e, done, keep](bool ok) {
		if (*done || !xyAlive(s)) return;
		*done = true;
		const QString t = e->text();
		e->hide();
		e->deleteLater();
		if (ok) keep(t);
	};
	QObject::connect(e, &QLineEdit::returnPressed, e, [commit] { commit(true); });
	QObject::connect(e, &QLineEdit::editingFinished, e, [commit] { commit(true); });
	struct EscKey : QObject {
		std::function<void(bool)> f;
		EscKey(QObject *parent, std::function<void(bool)> fn) : QObject(parent), f(std::move(fn)) {}
		bool eventFilter(QObject *o, QEvent *ev) override {
			if (ev->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(ev)->key() == Qt::Key_Escape) { f(false); return true; }
			return QObject::eventFilter(o, ev);
		}
	};
	e->installEventFilter(new EscKey(e, commit));
	e->show();
	e->setFocus();
	e->selectAll();
}

// Type the text of annotation `i` in place, where its text goes (a text arrow's at its tail, left of it
// when the head points right; a text box's at its top-left corner).
static void xyAnnotEditText(XYPlot *s, int i) {
	XYPage &pg = xyCur(s);
	if (i < 0 || i >= (int)pg.annots.size() || !xyAnnotHasText(pg.annots[i].kind) || !s->widget) return;
	const XYAnnot &a = pg.annots[i];
	const double dpr = s->widget->devicePixelRatioF();
	const int H = s->widget->renderWindow() ? s->widget->renderWindow()->GetSize()[1] : s->widget->height();
	const double ax = a.kind == XY_AN_TEXTBOX ? std::min(a.sx0, a.sx1) : a.sx0;
	const double ay = a.kind == XY_AN_TEXTBOX ? std::max(a.sy0, a.sy1) : a.sy0;
	const bool leftOf = a.kind == XY_AN_TEXTARROW && a.sx1 >= a.sx0;
	const int page = s->curPageIdx;
	xyEditInPlace(s, QString::fromStdString(a.text), "The annotation's text",
		[ax, ay, dpr, H, leftOf](QLineEdit *e) {
			const int qx = (int)std::lround(ax / dpr) - (leftOf ? e->width() : 0);
			return QPoint(qx, (int)std::lround((H - ay) / dpr) - e->height() / 2);
		},
		[s, i, page](const QString &t) {
			if (page < (int)s->pages.size() && i < (int)s->pages[page].annots.size())
				s->pages[page].annots[i].text = t.toStdString();
			xyRebuildObjMgr(s);
			xyAnnotRender(s);
		});
}

// The Title / Subtitle / X-Label / Y-Label buttons (MATLAB's): type that text in place, where it is
// drawn — the top of the chart, under the title, the bottom, the left side. What is typed goes into
// the page's Axes properties ("-B.+t", "-B.+s", "-Bx.label", "-By.label": the dialog's own fields, so the
// two never disagree) and is applied by xyApplyAxes. 0 title, 1 subtitle, 2 x label, 3 y label.
static void xyEditAxisText(XYPlot *s, int which) {
	if (!xyAlive(s) || s->pages.empty() || !s->widget) return;
	XYPage &pg = xyCur(s);
	static const char *const kKey[4] = { "-B.+t", "-B.+s", "-Bx.label", "-By.label" };
	static const char *const kTip[4] = { "The page's title", "The page's subtitle (under the title)",
	                                     "The x axis label", "The y axis label" };
	QString cur;
	if (pg.axProps.count(kKey[which])) cur = QString::fromStdString(pg.axProps.at(kKey[which]));
	else if (which == 0) cur = xyChartTitle(pg.chart);
	else if (which == 2) cur = QString::fromStdString(pg.chart->GetAxis(vtkAxis::BOTTOM)->GetTitle());
	else if (which == 3) cur = QString::fromStdString(pg.chart->GetAxis(vtkAxis::LEFT)->GetTitle());
	const int page = s->curPageIdx;
	xyEditInPlace(s, cur, kTip[which],
		[s, which](QLineEdit *e) {
			const int W = s->widget->width(), Hh = s->widget->height();
			switch (which) {
			case 0:  return QPoint((W - e->width()) / 2, 4);
			case 1:  return QPoint((W - e->width()) / 2, 8 + e->height());
			case 2:  return QPoint((W - e->width()) / 2, Hh - e->height() - 4);
			default: return QPoint(4, (Hh - e->height()) / 2);
			}
		},
		[s, which, page](const QString &t) {
			if (page >= (int)s->pages.size()) return;
			XYPage &p = s->pages[page];
			p.axProps[kKey[which]] = t.toStdString();
			xyApplyAxes(s, p);
		});
}

// Properties of annotation `i`: text, colour, line width / style, font size, fill. Every control applies
// at once (the Line properties dialog's way). Modal, so no annotation comes or goes while it is open.
static void xyAnnotProps(XYPlot *s, int i) {
	XYPage &pg = xyCur(s);
	if (i < 0 || i >= (int)pg.annots.size()) return;
	XYAnnot &a = pg.annots[i];
	QDialog dlg(s->win);
	dlg.setWindowTitle(QString("%1 properties").arg(xyAnnotNames[a.kind]));
	QFormLayout *form = new QFormLayout(&dlg);
	auto rr = [s] { xyAnnotRender(s); };
	if (xyAnnotHasText(a.kind)) {
		QLineEdit *te = new QLineEdit(QString::fromStdString(a.text), &dlg);
		form->addRow("Text", te);
		QObject::connect(te, &QLineEdit::textChanged, &dlg, [&a, s, rr](const QString &t) {
			a.text = t.toStdString(); xyRebuildObjMgr(s); rr(); });
		QSpinBox *fs = new QSpinBox(&dlg);
		fs->setRange(6, 72); fs->setValue((int)std::lround(a.size));
		form->addRow("Font size", fs);
		QObject::connect(fs, qOverload<int>(&QSpinBox::valueChanged), &dlg, [&a, rr](int v) { a.size = v; rr(); });
	}
	auto colourRow = [&](const char *label, double &r, double &g, double &b, const char *title) {
		QPushButton *btn = new QPushButton(&dlg);
		auto swatch = [btn, &r, &g, &b] {
			btn->setStyleSheet(QString("background:%1").arg(QColor((int)(r * 255), (int)(g * 255), (int)(b * 255)).name()));
		};
		swatch();
		form->addRow(label, btn);
		QObject::connect(btn, &QPushButton::clicked, &dlg, [&dlg, &r, &g, &b, swatch, title, s, rr] {
			const QColor c = QColorDialog::getColor(QColor((int)(r * 255), (int)(g * 255), (int)(b * 255)), &dlg, title);
			if (!c.isValid()) return;
			r = c.redF(); g = c.greenF(); b = c.blueF();
			swatch(); xyRebuildObjMgr(s); rr();
		});
		return btn;
	};
	colourRow("Colour", a.r, a.g, a.b, "Annotation colour");
	QDoubleSpinBox *wsp = new QDoubleSpinBox(&dlg);
	wsp->setRange(0.5, 12.0); wsp->setSingleStep(0.5); wsp->setValue(a.width);
	form->addRow("Width (px)", wsp);
	QObject::connect(wsp, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg, [&a, rr](double v) { a.width = v; rr(); });
	static const int kPen[] = { 1, 2, 3, 4, 5 };      // Solid Dashed Dotted Dash-Dot Dash-Dot-Dot
	QComboBox *lsc = new QComboBox(&dlg);
	lsc->addItems(QStringList() << "Solid" << "Dashed" << "Dotted" << "Dash-Dot" << "Dash-Dot-Dot");
	for (int k = 0; k < 5; ++k) if (kPen[k] == a.lineType) lsc->setCurrentIndex(k);
	form->addRow("Line style", lsc);
	QObject::connect(lsc, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, [&a, rr](int k) { a.lineType = kPen[k]; rr(); });
	if (a.kind == XY_AN_TEXTBOX) {                    // the box itself on or off: off leaves the text alone
		QCheckBox *bc = new QCheckBox("Show box", &dlg);
		bc->setObjectName("annotShowBox");
		bc->setChecked(a.boxed);
		bc->setToolTip("Draw the box around the text (its outline and fill); unchecked, only the text shows");
		form->addRow("Box", bc);
		QObject::connect(bc, &QCheckBox::toggled, &dlg, [&a, rr](bool on) { a.boxed = on; rr(); });
	}
	if (!xyAnnotIsLine(a.kind)) {
		QCheckBox *fc = new QCheckBox("Filled", &dlg);
		fc->setChecked(a.filled);
		form->addRow("Fill", fc);
		QPushButton *fb = colourRow("Fill colour", a.fr, a.fg, a.fb, "Fill colour");
		fb->setEnabled(a.filled);
		QObject::connect(fc, &QCheckBox::toggled, &dlg, [&a, fb, rr](bool on) { a.filled = on; fb->setEnabled(on); rr(); });
	}
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
	form->addRow(bb);
	QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::clicked, &dlg, &QDialog::accept);
	for (QPushButton *b : dlg.findChildren<QPushButton *>()) { b->setAutoDefault(false); b->setDefault(false); }
	wrapTooltips(&dlg);
	dlg.exec();
}

// The right-click menu of annotation `i` (on the chart and on its Object Manager row).
static void xyAnnotMenu(XYPlot *s, int i, const QPoint &global) {
	XYPage &pg = xyCur(s);
	if (i < 0 || i >= (int)pg.annots.size()) return;
	QMenu m(s->win);
	QAction *aProps = m.addAction("Properties…");
	QAction *aText = xyAnnotHasText(pg.annots[i].kind) ? m.addAction("Edit text") : nullptr;
	QAction *aDel = m.addAction("Delete");
	QAction *pick = m.exec(global);
	if (!xyAlive(s)) return;
	if (pick == aProps)              xyAnnotProps(s, i);
	else if (aText && pick == aText) xyAnnotEditText(s, i);
	else if (pick == aDel)           xyAnnotDelete(s, i);
}

// Arm (or disarm) the tool bar's annotation tool `k`: the next left press on the chart draws one.
static void xyAnnotArm(XYPlot *s, int k) {
	s->annotTool = k;
	for (int j = 0; j < XY_AN_COUNT; ++j)
		if (s->actAnnot[j] && s->actAnnot[j]->isChecked() != (j == k)) {
			QSignalBlocker blk(s->actAnnot[j]);
			s->actAnnot[j]->setChecked(j == k);
		}
	if (s->widget) {
		if (k >= 0) s->widget->setCursor(Qt::CrossCursor);
		else        s->widget->unsetCursor();
	}
	if (s->win) {
		if (k >= 0) s->win->statusBar()->showMessage(QString("%1: press and drag on the chart to draw it (a click gives the default size)").arg(xyAnnotNames[k]));
		else        s->win->statusBar()->clearMessage();
	}
}

// Mouse for the annotations. With a tool armed, a left press draws a new one (the drag sizes it, a
// bare click gives it a default size) and the tool disarms, as MATLAB's do; a text kind then takes
// its text in place. With none armed, an annotation is inert until DOUBLE-CLICKED: that puts it in edit
// (its grab handles show), and then a drag on it moves it, a drag on a handle reshapes it, and Delete
// removes it; a press elsewhere ends the edit. Its properties are reached only through its Object
// Manager row. Installed LAST: annotations are drawn over everything else.
class XYAnnotFilter : public QObject {
public:
	XYPlot *s = nullptr;
	explicit XYAnnotFilter(XYPlot *sc, QObject *parent) : QObject(parent), s(sc) {}
protected:
	enum { NONE, CREATE, MOVE, RESIZE } mode = NONE;
	int    grab = -1, part = -1;
	double px = 0, py = 0;                            // press point, scene px
	float  o0x = 0, o0y = 0, o1x = 0, o1y = 0;        // the grabbed annotation's points at the press, scene px
	bool   moved = false;
	void toData(XYPage &pg, XYAnnot &a, double x0, double y0, double x1, double y1) {
		vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM), *ay = pg.chart->GetAxis(vtkAxis::LEFT);
		a.x0 = xySceneToAxis(ax, x0, 0); a.y0 = xySceneToAxis(ay, y0, 1);
		a.x1 = xySceneToAxis(ax, x1, 0); a.y1 = xySceneToAxis(ay, y1, 1);
	}
	bool eventFilter(QObject *obj, QEvent *ev) override {
		const QEvent::Type t = ev->type();
		if (!s || !xyAlive(s) || s->pages.empty() || !xyCur(s).chart) { mode = NONE; return QObject::eventFilter(obj, ev); }
		XYPage &pg = xyCur(s);
		if (t == QEvent::KeyPress) {
			const int k = static_cast<QKeyEvent *>(ev)->key();
			if ((k == Qt::Key_Delete || k == Qt::Key_Backspace) && pg.annotSel >= 0 && pg.annotSel < (int)pg.annots.size()) {
				xyAnnotDelete(s, pg.annotSel);
				return true;
			}
			if (k == Qt::Key_Escape && s->annotTool >= 0) { xyAnnotArm(s, -1); return true; }
			return QObject::eventFilter(obj, ev);
		}
		if (t != QEvent::MouseButtonPress && t != QEvent::MouseButtonDblClick &&
		    t != QEvent::MouseMove && t != QEvent::MouseButtonRelease)
			return QObject::eventFilter(obj, ev);
		QMouseEvent *me = static_cast<QMouseEvent *>(ev);
		double sx, sy;
		xyQtToScene(s, me->position(), sx, sy);
		const double dpr = s->widget->devicePixelRatioF();
		if (t == QEvent::MouseButtonRelease) {
			if (mode == NONE || me->button() != Qt::LeftButton) return QObject::eventFilter(obj, ev);
			const int was = mode, i = grab;
			mode = NONE;
			if (was == CREATE && i >= 0 && i < (int)pg.annots.size()) {
				XYAnnot &a = pg.annots[i];
				if (!moved) {                             // a bare click: the default size, down and right
					const double w = (xyAnnotIsLine(a.kind) ? 60.0 : 90.0) * dpr, h = (xyAnnotIsLine(a.kind) ? 40.0 : 45.0) * dpr;
					toData(pg, a, px, py, px + w, py - h);
					a.fit = a.kind == XY_AN_TEXTBOX;
				}
				xyAnnotArm(s, -1);
				xyRebuildObjMgr(s);
				xyAnnotRender(s);
				if (xyAnnotHasText(a.kind)) xyAnnotEditText(s, i);
			}
			return true;
		}
		if (t == QEvent::MouseMove) {
			if (mode != NONE && grab >= 0 && grab < (int)pg.annots.size()) {
				if (!moved && std::hypot(sx - px, sy - py) > 3.0 * dpr) moved = true;
				if (!moved) return true;
				XYAnnot &a = pg.annots[grab];
				const double dx = sx - px, dy = sy - py;
				if (mode == CREATE) toData(pg, a, px, py, sx, sy);
				else if (mode == MOVE) toData(pg, a, o0x + dx, o0y + dy, o1x + dx, o1y + dy);
				else {                                    // a handle: a line's end, or a box's corner
					double x0 = o0x, y0 = o0y, x1 = o1x, y1 = o1y;
					if (xyAnnotIsLine(a.kind)) { if (part == 0) { x0 += dx; y0 += dy; } else { x1 += dx; y1 += dy; } }
					else {
						if (part == 0 || part == 3) x0 += dx; else x1 += dx;
						if (part == 0 || part == 1) y0 += dy; else y1 += dy;
					}
					a.fit = false;
					toData(pg, a, x0, y0, x1, y1);
				}
				xyAnnotRender(s);
				return true;
			}
			if (s->annotTool >= 0) {
				if (s->widget->cursor().shape() != Qt::CrossCursor) s->widget->setCursor(Qt::CrossCursor);
				return true;
			}
			// only the annotation in edit (double-clicked) is grabbable; the others are just drawing
			int hp = -1;
			const int i = pg.annotSel >= 0 ? xyAnnotHit(s, pg, sx, sy, hp, pg.annotSel) : -1;
			if (i < 0) return QObject::eventFilter(obj, ev);
			const Qt::CursorShape want = hp >= 0 ? Qt::CrossCursor : Qt::SizeAllCursor;
			if (s->widget->cursor().shape() != want) s->widget->setCursor(want);
			return true;                                  // over an annotation, no other tool's hover
		}
		// a press or a double-click
		if (me->button() == Qt::LeftButton && s->annotTool >= 0 && t == QEvent::MouseButtonPress) {
			XYAnnot a;
			a.kind = s->annotTool;
			pg.annots.push_back(a);
			grab = (int)pg.annots.size() - 1;
			pg.annotSel = grab;
			px = sx; py = sy; moved = false;
			toData(pg, pg.annots[grab], sx, sy, sx, sy);
			mode = CREATE;
			return true;
		}
		if (me->button() != Qt::LeftButton) return QObject::eventFilter(obj, ev);
		int hp = -1;
		if (t == QEvent::MouseButtonDblClick) {           // a double-click puts an annotation in edit
			const int d = xyAnnotHit(s, pg, sx, sy, hp);
			if (d < 0) return QObject::eventFilter(obj, ev);
			mode = NONE;
			pg.annotSel = d;
			s->widget->setFocus();                        // so Delete reaches this filter
			xyAnnotRender(s);
			return true;
		}
		// a press: grabs only the annotation in edit, by its body (move) or a handle (reshape)
		const int i = pg.annotSel >= 0 ? xyAnnotHit(s, pg, sx, sy, hp, pg.annotSel) : -1;
		if (i < 0) {
			if (pg.annotSel >= 0) {
				pg.annotSel = -1;                         // a press elsewhere ends the edit
				xyAnnotRender(s);
			}
			return QObject::eventFilter(obj, ev);
		}
		const XYAnnot &a = pg.annots[i];
		grab = i; part = hp; px = sx; py = sy; moved = false;
		o0x = a.sx0; o0y = a.sy0; o1x = a.sx1; o1y = a.sy1;
		mode = hp >= 0 ? RESIZE : MOVE;
		s->widget->setFocus();                            // so Delete reaches this filter
		xyAnnotRender(s);
		return true;
	}
};

// The tool bar icon of annotation tool `k`: MATLAB's — a light tile with the shape drawn on it.
static QIcon xyAnnotIcon(int k, double dpr) {
	const int S = 18;
	QPixmap pm((int)std::ceil(S * dpr), (int)std::ceil(S * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter g(&pm);
	g.setRenderHint(QPainter::Antialiasing, true);
	g.setPen(QPen(QColor(100, 100, 100), 1.0));
	g.setBrush(QColor(244, 244, 244));
	g.drawRoundedRect(QRectF(0.5, 0.5, S - 1, S - 1), 2, 2);
	const QColor ink(40, 40, 40);
	g.setPen(QPen(ink, 1.2, Qt::SolidLine, Qt::RoundCap));
	g.setBrush(ink);
	auto head = [&g](QPointF tip, QPointF from) {
		const QPointF d = tip - from;
		const double L = std::hypot(d.x(), d.y()), hl = 4.5, hw = 2.2;
		const QPointF u = d / L, n(-u.y(), u.x());
		const QPointF pts[3] = { tip, tip - u * hl + n * hw, tip - u * hl - n * hw };
		g.drawPolygon(pts, 3);
	};
	auto shaft = [&g](QPointF a, QPointF b, bool ha, bool hb) {
		const QPointF d = b - a;
		const double L = std::hypot(d.x(), d.y());
		const QPointF u = d / L;
		g.drawLine(ha ? a + u * 3.5 : a, hb ? b - u * 3.5 : b);
	};
	QFont f = g.font();
	f.setPixelSize(6); f.setBold(true);
	g.setFont(f);
	switch (k) {
	case XY_AN_TEXTARROW:
		g.drawText(QRectF(2, 1.5, 6, 6), Qt::AlignCenter, "T");
		shaft(QPointF(5.5, 6.5), QPointF(14.5, 14.5), false, true);
		head(QPointF(14.5, 14.5), QPointF(5.5, 6.5));
		break;
	case XY_AN_ARROW:
		shaft(QPointF(3.5, 3.5), QPointF(14.5, 14.5), false, true);
		head(QPointF(14.5, 14.5), QPointF(3.5, 3.5));
		break;
	case XY_AN_DARROW:
		shaft(QPointF(3.5, 3.5), QPointF(14.5, 14.5), true, true);
		head(QPointF(14.5, 14.5), QPointF(3.5, 3.5));
		head(QPointF(3.5, 3.5), QPointF(14.5, 14.5));
		break;
	case XY_AN_LINE:
		g.drawLine(QPointF(3.5, 3.5), QPointF(14.5, 14.5));
		break;
	case XY_AN_TEXTBOX:
		g.setBrush(Qt::NoBrush);
		g.drawRect(QRectF(3.5, 4.5, 11, 9));
		g.drawText(QRectF(3.5, 4.5, 11, 9), Qt::AlignCenter, "T");
		break;
	case XY_AN_RECT:
		g.setBrush(Qt::NoBrush);
		g.drawRoundedRect(QRectF(3.5, 4.5, 11, 9), 1, 1);
		break;
	case XY_AN_ELLIPSE:
		g.setBrush(Qt::NoBrush);
		g.drawEllipse(QRectF(3.5, 3.5, 11, 11));
		break;
	}
	return QIcon(pm);
}

// ---- pages (Excel-like tabs) -----------------------------------------------

// Build a fresh chart configured like the window's first one: owner set (so the tooltip can read
// the live time mode), legend/grid matching the current toolbar toggles, compact axis labels.
static vtkSmartPointer<vtkChartXY> xyMakeChart(XYPlot *s) {
	vtkSmartPointer<XYChart> chart = vtkSmartPointer<XYChart>::New();
	chart->owner = s;
	chart->SetShowLegend(s->legendVisible);
	if (vtkChartLegend *lg = chart->GetLegend())
		lg->GetLabelProperties()->SetFontSize((int)s->legendFontPt);
	// No axis titles: an axis is labelled only when something says what it is (set_labels, a file's
	// column names, the X-Label / Y-Label buttons). A bare "X" / "Y" tells the reader nothing.
	chart->GetAxis(vtkAxis::BOTTOM)->SetTitle("");
	chart->GetAxis(vtkAxis::LEFT)->SetTitle("");
	// a new page's grid is the one the page on screen has (both, for the first page)
	vtkChartXY *cur = s->pages.empty() ? nullptr : xyCur(s).chart.GetPointer();
	const bool gx = cur ? cur->GetAxis(vtkAxis::BOTTOM)->GetGridVisible() : true;
	const bool gy = cur ? cur->GetAxis(vtkAxis::LEFT)->GetGridVisible() : true;
	// Compact tick labels: smaller font + bounded precision so x ticks stop colliding. STANDARD
	// notation trims trailing-zero clutter.
	for (int an : { vtkAxis::BOTTOM, vtkAxis::LEFT }) {
		vtkAxis *ax = chart->GetAxis(an);
		ax->GetLabelProperties()->SetFontSize(11);
		ax->GetTitleProperties()->SetFontSize(13);
		ax->SetNotation(vtkAxis::STANDARD_NOTATION);
		ax->SetPrecision(4);
		ax->SetGridVisible(an == vtkAxis::BOTTOM ? gx : gy);
	}
	// VTK's grid grey is barely visible on white: a darker one, on every side (the Axes properties'
	// grid pen colour shows and changes it)
	for (int an : { vtkAxis::BOTTOM, vtkAxis::LEFT, vtkAxis::RIGHT, vtkAxis::TOP })
		chart->GetAxis(an)->GetGridPen()->SetColor(190, 190, 190, 255);
	return chart;
}

static void xySwitchPage(XYPlot *s, int idx);
static void xyMagSyncUi(XYPlot *s);                      // magnetic tools (below)

// Append a new page (its own chart + empty series list). Inserts a tab. If `switchTo`, makes it
// current. Returns the new page index.
static int xyNewPage(XYPlot *s, const char *name, bool switchTo) {
	XYPage pg;
	pg.chart = xyMakeChart(s);
	pg.name  = (name && name[0]) ? name : ("Page " + std::to_string((int)s->pages.size() + 1));
	s->pages.push_back(std::move(pg));
	const int idx = (int)s->pages.size() - 1;
	if (s->tabs) {
		s->tabBusy = true;
		s->tabs->insertTab(idx, QString::fromStdString(s->pages[idx].name));
		s->tabBusy = false;
	}
	if (switchTo)
		xySwitchPage(s, idx);
	return idx;
}

// Mount page `idx` in the shared scene (detaching the old one) and rebuild the side panels from it.
static void xySwitchPage(XYPlot *s, int idx) {
	if (idx < 0 || idx >= (int)s->pages.size())
		return;
	if (s->curPageIdx >= 0 && s->curPageIdx < (int)s->pages.size()) {
		vtkChartXY *old = s->pages[s->curPageIdx].chart;
		if (old) s->view->GetScene()->RemoveItem(old);
	}
	s->curPageIdx = idx;
	XYPage &pg = s->pages[idx];
	s->view->GetScene()->AddItem(pg.chart);
	if (s->tabs && s->tabs->currentIndex() != idx) {
		s->tabBusy = true; s->tabs->setCurrentIndex(idx); s->tabBusy = false;
	}
	xyRebuildObjMgr(s);
	pg.lastLo = std::numeric_limits<double>::quiet_NaN();   // force a tick recompute for this page
	pg.lastHi = std::numeric_limits<double>::quiet_NaN();
	xyRefreshTicks(s);
	xySyncMiscMenu(s);                                       // log / time items show THIS page's
	xyGridSyncUi(s);                                         // ...and the grid toggles
	xyMagSyncUi(s);                                       // the bar strip + controls show THIS page's
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Deep-copy page `idx` (its series tables + styles + axis titles + time mode) into a new page and
// switch to it.
static void xyDuplicatePage(XYPlot *s, int idx) {
	if (idx < 0 || idx >= (int)s->pages.size())
		return;
	const std::string base = s->pages[idx].name + " copy";
	const int newIdx = xyNewPage(s, base.c_str(), false);
	// copy axis titles + time mode from the source
	XYPage &src = s->pages[idx];
	XYPage &dst = s->pages[newIdx];
	dst.chart->GetAxis(vtkAxis::BOTTOM)->SetTitle(src.chart->GetAxis(vtkAxis::BOTTOM)->GetTitle());
	dst.chart->GetAxis(vtkAxis::LEFT)->SetTitle(src.chart->GetAxis(vtkAxis::LEFT)->GetTitle());
	dst.xTimeFmt = src.xTimeFmt;
	dst.mag = src.mag;                                   // the copied series keep their mag roles, so the state goes too
	dst.square = src.square;                             // a radar / corner plot page stays one
	dst.axesHidden = src.axesHidden;
	dst.axesShown = src.axesShown;
	dst.axProps = src.axProps;
	dst.labels = src.labels;
	dst.annots = src.annots;
	xyApplyPageLook(s, dst);
	xyApplyAxes(s, dst);
	for (const XYSeries &so : src.series) {
		XYSeries se = so;                                // copies style/name; deep-copy the table next
		se.table = vtkSmartPointer<vtkTable>::New();
		se.table->DeepCopy(so.table);
		vtkPlot *pl = xyAddPlotItem(dst.chart, se);
		pl->SetInputData(se.table, 0, 1);
		pl->SetLabel(se.name);
		pl->SetTooltipLabelFormat("%x, %y");
		pl->SetVisible(se.visible);
		se.plot = pl;
		xyApplyStyle(s, se);
		dst.series.push_back(se);
	}
	xySwitchPage(s, newIdx);
}

// Delete page `idx`. Keeps at least one page (a lone page is cleared instead of removed).
static void xyDeletePage(XYPlot *s, int idx) {
	if (idx < 0 || idx >= (int)s->pages.size())
		return;
	if (s->pages.size() == 1) {                          // never zero pages: just empty the last one
		xyClear(s);
		return;
	}
	// detach if it is the mounted chart
	if (idx == s->curPageIdx)
		s->view->GetScene()->RemoveItem(s->pages[idx].chart);
	s->pages.erase(s->pages.begin() + idx);
	if (s->markLinkPage == idx)     s->markLinkPage = -1;   // the Profile-linked curve went with it
	else if (s->markLinkPage > idx) --s->markLinkPage;
	if (s->tabs) {
		s->tabBusy = true;
		s->tabs->removeTab(idx);
		s->tabBusy = false;
	}
	// pick a valid current page and mount it
	int next = s->curPageIdx;
	if (idx < s->curPageIdx)        next = s->curPageIdx - 1;
	else if (idx == s->curPageIdx)  next = std::min(idx, (int)s->pages.size() - 1);
	s->curPageIdx = -1;                                  // force xySwitchPage to mount (no stale detach)
	xySwitchPage(s, next);
}

// Rename page `idx` (updates the tab text too).
static void xyRenamePage(XYPlot *s, int idx, const QString &name) {
	if (idx < 0 || idx >= (int)s->pages.size() || name.isEmpty())
		return;
	s->pages[idx].name = name.toStdString();
	if (s->tabs) {
		s->tabBusy = true;
		s->tabs->setTabText(idx, name);
		s->tabBusy = false;
	}
}

// Combined X data range across every series of page `idx` (column 0 of each series' table).
// Returns false (lo/hi untouched) if the page has no series at all.
static bool xyPageXRange(XYPlot *s, int idx, double &lo, double &hi) {
	if (idx < 0 || idx >= (int)s->pages.size())
		return false;
	bool any = false;
	for (const XYSeries &se : s->pages[idx].series) {
		if (!se.table || se.table->GetNumberOfRows() == 0)
			continue;
		vtkDataArray *xcol = vtkDataArray::SafeDownCast(se.table->GetColumn(0));
		if (!xcol)
			continue;
		double r[2];
		xcol->GetRange(r);
		if (!any || r[0] < lo) lo = r[0];
		if (!any || r[1] > hi) hi = r[1];
		any = true;
	}
	return any;
}

// Copy every series of page `srcIdx` into page `dstIdx` (appends -- the target page's own
// series are kept). If the two pages' X data ranges don't intersect at all, warns first and
// asks for confirmation: that usually means the wrong target was picked (e.g. mixing up two
// unrelated time windows).
static void xyCopyToPage(XYPlot *s, int srcIdx, int dstIdx) {
	if (srcIdx < 0 || srcIdx >= (int)s->pages.size() || dstIdx < 0 || dstIdx >= (int)s->pages.size() || srcIdx == dstIdx)
		return;
	double sLo, sHi, dLo, dHi;
	const bool haveSrc = xyPageXRange(s, srcIdx, sLo, sHi);
	const bool haveDst = xyPageXRange(s, dstIdx, dLo, dHi);
	if (haveSrc && haveDst && (sHi < dLo || dHi < sLo)) {
		const QString msg = QString(
			"\"%1\" spans X = [%2, %3], and \"%4\" spans X = [%5, %6] — these ranges do not "
			"overlap at all.\n\nCopying data whose X never intersects the target page usually "
			"means the wrong page was picked.\n\nCopy anyway?")
			.arg(QString::fromStdString(s->pages[srcIdx].name)).arg(sLo).arg(sHi)
			.arg(QString::fromStdString(s->pages[dstIdx].name)).arg(dLo).arg(dHi);
		const auto ans = QMessageBox::warning(s->win, "Copy to Page — X ranges don't overlap", msg,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (ans != QMessageBox::Yes)
			return;
	}
	XYPage &src = s->pages[srcIdx];
	XYPage &dst = s->pages[dstIdx];
	for (const XYSeries &so : src.series) {
		XYSeries se = so;                                // copies style/name; deep-copy the table next
		se.magRole = 0;                                  // its mag role belongs to the SOURCE page's state
		se.table = vtkSmartPointer<vtkTable>::New();
		se.table->DeepCopy(so.table);
		vtkPlot *pl = xyAddPlotItem(dst.chart, se);
		pl->SetInputData(se.table, 0, 1);
		pl->SetLabel(se.name);
		pl->SetTooltipLabelFormat("%x, %y");
		pl->SetVisible(se.visible);
		se.plot = pl;
		xyApplyStyle(s, se);
		dst.series.push_back(se);
	}
	if (dstIdx == s->curPageIdx) {
		dst.chart->RecalculateBounds();
		xyRebuildObjMgr(s);
		dst.lastLo = std::numeric_limits<double>::quiet_NaN();  // force a tick recompute
		dst.lastHi = std::numeric_limits<double>::quiet_NaN();
		xyRefreshTicks(s);
		if (s->widget && s->widget->renderWindow())
			s->widget->renderWindow()->Render();
	}
}

// Right-click on the tab bar: New / Duplicate / Rename / Copy to Page / Delete the clicked page.
static void xyTabMenu(XYPlot *s, const QPoint &pos) {
	const int idx = s->tabs->tabAt(pos);
	QMenu m(s->tabs);
	QAction *aNew = m.addAction("New");
	QAction *aDup = (idx >= 0) ? m.addAction("Duplicate") : nullptr;
	QAction *aRen = (idx >= 0) ? m.addAction("Rename…")   : nullptr;
	std::vector<std::pair<QAction*, int>> copyTargets;
	if (idx >= 0 && s->pages.size() > 1) {
		QMenu *mCopy = m.addMenu("Copy to Page");
		for (int i = 0; i < (int)s->pages.size(); ++i) {
			if (i == idx)
				continue;
			copyTargets.push_back({mCopy->addAction(QString::fromStdString(s->pages[i].name)), i});
		}
	}
	m.addSeparator();
	QAction *aDel = (idx >= 0) ? m.addAction("Delete")    : nullptr;
	QAction *pick = m.exec(s->tabs->mapToGlobal(pos));
	if (!pick)
		return;
	if (pick == aNew) {
		xyNewPage(s, "", true);
	} else if (aDup && pick == aDup) {
		xyDuplicatePage(s, idx);
	} else if (aRen && pick == aRen) {
		bool ok = false;
		const QString nn = QInputDialog::getText(s->win, "Rename page", "Name:",
			QLineEdit::Normal, QString::fromStdString(s->pages[idx].name), &ok);
		if (ok) xyRenamePage(s, idx, nn);
	} else if (aDel && pick == aDel) {
		xyDeletePage(s, idx);
	} else {
		for (const auto &ct : copyTargets) {
			if (pick == ct.first) {
				xyCopyToPage(s, idx, ct.second);
				break;
			}
		}
	}
}

// Bring a parked plot back: show it, raise it, and drop the "parked" mark so the dock stops listing
// it. ONE function for every way back in (the row's checkbox, its double-click, its "Show" item), so
// they can never drift apart.
static void xyUnpark(XYPlot *p) {
	if (!xyAlive(p) || !p->win) return;
	p->parked = false;
	if (p->owner && sceneAlive(p->owner)) unparkTool(p->owner, p->win);   // drops the row + rebuilds
	p->win->setWindowState(p->win->windowState() & ~Qt::WindowMinimized);
	p->win->showNormal();
	p->win->raise();
	p->win->activateWindow();
}

static std::function<void(const QPoint &)> xyParkedMenu(XYPlot *p);

// Park a plot (already hidden) as a handle in its owner window's Scene Objects. ONE function for both
// ways of parking it: the X (XYCloseParks) and the minimise button (gmtvtk_xyplot_set_owner).
static void xyParkNow(XYPlot *p) {
	if (!xyAlive(p) || !p->win || !p->owner || !sceneAlive(p->owner)) return;
	p->parked = true;
	parkTool(p->owner, p->win, p->win->windowTitle(), IC_Profile,
	         "Closed X,Y plot — double-click to bring it back, click for Show / Delete",
	         [p]() { xyUnpark(p); }, xyParkedMenu(p));
	// A handle the user cannot see is the same as no handle at all: reveal + unfold the dock,
	// exactly as a derived grid does when it lands there.
	unfoldSceneObjects(p->owner);
}

// The parked plot's menu, reached by EITHER button — the row's properties button and its context menu
// are the same thing, so they are the same lambda, not two look-alikes.
static std::function<void(const QPoint &)> xyParkedMenu(XYPlot *p) {
	return [p](const QPoint &g) {
		QMenu m;
		QAction *aShow = m.addAction("Show");
		m.addSeparator();
		QAction *aDel  = m.addAction("Delete");
		QAction *pick  = m.exec(g);
		if (pick == aShow) xyUnpark(p);
		else if (pick == aDel) {
			if (QMessageBox::question(nullptr, "Delete X,Y plot",
			        QString("Delete \"%1\" for good? Its pages and data go with it.")
			            .arg(p->win->windowTitle())) != QMessageBox::Yes)
				return;
			p->reallyClose = true;      // let the close through the parking filter
			p->win->close();            // WA_DeleteOnClose -> destroyed -> the row goes with it
		}
	};
}

// ---- magnetic anomaly modelling (ecran's "Geomagnetic bar code" tools) ----------------------------
// The toolbar toggle (ecran's isocs_CB) shows a control strip under the chart — Start / End age,
// Create Mag Bar, Synthetic RTP, Parameters…, the contamination slider, the isochron age and Fit — and,
// once a bar exists, the bar code strip over the chart. The maths is Julia's (xymag.jl, reached through
// the Analysis callback with "mag…" ops); its results come back through gmtvtk_xyplot_mag_set_*. The
// picks — red star, first-guess circle, fitted and pinned isochron lines, chron names — are drawn by
// the chart itself (xyMagPaint), so they follow zoom and pan.

// Requests that go to Julia from this window and must be answered in front of the user when they fail
// (the action-errors-pop-up rule): xymag.jl / xysession.jl hand the message back through
// gmtvtk_xyplot_request_failed into `reqErr`, and the ONE door below shows it. The busy notice is raised
// at the same door (No-dead-time law).
static bool xyRequest(XYPlot *s, const QString &op, int sel, const char *busy, bool fileCb,
                      const QString &path) {
	if (!xyAlive(s)) return false;
	if (fileCb ? !g_juliaXY : !g_juliaXYAna) {
		QMessageBox::warning(s->win, "X,Y plot", "Not wired: the Julia side of the X,Y tool is not loaded.");
		return false;
	}
	s->reqErr.clear();
	if (busy) showBusyDialog(busy);
	if (fileCb) g_juliaXY(s, op.toUtf8().constData(), sel, path.toUtf8().constData());
	else        g_juliaXYAna(s, op.toUtf8().constData(), sel);
	if (busy) closeBusyDialog();
	if (!xyAlive(s)) return false;
	if (!s->reqErr.isEmpty()) {
		QMessageBox::warning(s->win, "X,Y plot", s->reqErr);
		return false;
	}
	return true;
}

// The page's series playing `role` (XYSeries::magRole), -1 if none.
static int xyMagSeries(const XYPage &pg, int role) {
	for (int i = 0; i < (int)pg.series.size(); ++i)
		if (pg.series[i].magRole == role) return i;
	return -1;
}

// A bar-code age -> data x and back. The bar spans the measured profile's first..last x, from the
// start age to the end age — reversed (old -> young, ecran's XDir 'reverse') when the start is older.
static bool xyMagAgeToX(const XYMag &m, double age, double &x) {
	const double lo = std::min(m.ageStart, m.ageEnd), hi = std::max(m.ageStart, m.ageEnd);
	if (!m.barOn || !(hi > lo)) return false;
	double f = (age - lo) / (hi - lo);
	if (m.ageStart > m.ageEnd) f = 1.0 - f;
	x = m.barX0 + f * (m.barX1 - m.barX0);
	return true;
}
static bool xyMagXToAge(const XYMag &m, double x, double &age) {
	const double lo = std::min(m.ageStart, m.ageEnd), hi = std::max(m.ageStart, m.ageEnd);
	if (!m.barOn || !(hi > lo) || m.barX1 == m.barX0) return false;
	double f = (x - m.barX0) / (m.barX1 - m.barX0);
	if (m.ageStart > m.ageEnd) f = 1.0 - f;
	age = lo + f * (hi - lo);
	return true;
}

// Data x <-> logical px across the chart widget — and across the bar strip above it, which shares its
// left edge (both sit in the same zero-margin column).
static double xyMagXToPx(XYPlot *s, double x) {
	return xyAxisToScene(xyCur(s).chart->GetAxis(vtkAxis::BOTTOM), x, 0) / s->widget->devicePixelRatioF();
}
static double xyMagPxToX(XYPlot *s, double px) {
	vtkAxis *ax = xyCur(s).chart->GetAxis(vtkAxis::BOTTOM);
	const float *p1 = ax->GetPoint1(), *p2 = ax->GetPoint2();
	const double w = p2[0] - p1[0];
	if (w == 0.0) return ax->GetMinimum();
	return ax->GetMinimum() + (px * s->widget->devicePixelRatioF() - p1[0]) / w * (ax->GetMaximum() - ax->GetMinimum());
}

static void xyMagRender(XYPlot *s) {
	if (s->magBar) s->magBar->update();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// One isochron of the Parameters dialog's list: "age[_pad[_stretch]]", ecran's MIR_MAGPROF ISOC syntax.
// pad (Ma either side of the age) defaults to 1.5, stretch (max % the synthetic is expanded/shrunk
// while correlating) to 0.
struct XYIsoc { QString age; double pad = 1.5; int stretch = 0; };
static std::vector<XYIsoc> xyMagIsocList(const std::string &txt) {
	std::vector<XYIsoc> out;
	for (const QString &tok : QString::fromStdString(txt).split(QRegularExpression("[\\s,;]+"), Qt::SkipEmptyParts)) {
		const QStringList p = tok.split('_');
		bool ok = false;
		p[0].toDouble(&ok);
		if (!ok) continue;
		XYIsoc it;
		it.age = p[0];
		if (p.size() >= 2) it.pad = p[1].toDouble();
		if (p.size() >= 3) it.stretch = (int)p[2].toDouble();
		out.push_back(it);
	}
	return out;
}

// The age the fit works on: the list's choice when there is a list, else the typed one. NaN = none.
static double xyMagChosenAge(const XYMag &m, double *pad = nullptr, int *stretch = nullptr) {
	const std::vector<XYIsoc> list = xyMagIsocList(m.isocs);
	if (!list.empty()) {
		if (m.ageIdx <= 0 || m.ageIdx > (int)list.size()) return std::numeric_limits<double>::quiet_NaN();
		const XYIsoc &it = list[m.ageIdx - 1];
		if (pad) *pad = it.pad;
		if (stretch) *stretch = it.stretch;
		return it.age.toDouble();
	}
	if (pad) *pad = 1.5;                       // ecran's default pad, and its 15% stretch for a typed age
	if (stretch) *stretch = 15;
	return m.ageTyped;
}

// popup_ageFit_CB / edit_ageFit_CB: the red star on the synthetic, at the point whose age is closest
// to the chosen one.
static void xyMagPlaceStar(XYPlot *s) {
	XYPage &pg = xyCur(s);
	XYMag &m = pg.mag;
	const double age = xyMagChosenAge(m);
	const int syn = xyMagSeries(pg, 2);
	m.starIdx = -1;
	if (std::isnan(age) || syn < 0 || m.ageLine.empty()) return;
	double best = std::numeric_limits<double>::infinity();
	for (int i = 0; i < (int)m.ageLine.size(); ++i)
		if (std::abs(m.ageLine[i] - age) < best) { best = std::abs(m.ageLine[i] - age); m.starIdx = i; }
}

static void xyMagRerun(XYPlot *s);            // below

// Fill the widgets from the CURRENT page's state and show what applies (isocs_CB's visibility rules:
// Synthetic RTP once a bar exists, slider/age/Fit once a synthetic exists, the age LIST when the
// parameters carry isochrons, the age BOX otherwise).
static void xyMagSyncUi(XYPlot *s) {
	if (!xyAlive(s) || !s->magCtl || !s->magBar || !s->actMag || s->pages.empty()) return;
	XYPage &pg = xyCur(s);
	XYMag &m = pg.mag;
	const bool tool = s->actMag->isChecked();
	const bool syn = xyMagSeries(pg, 2) >= 0;
	const std::vector<XYIsoc> list = xyMagIsocList(m.isocs);
	auto num = [](double v) { return std::isnan(v) ? QString() : QString::number(v, 'g', 10); };
	s->magSyncing = true;
	s->magCtl->setVisible(tool);
	s->magBar->setVisible(tool && m.barOn);
	s->magStart->setText(num(m.ageStart));
	s->magEnd->setText(num(m.ageEnd));
	s->magBtnSynth->setVisible(m.barOn);
	s->magBtnParams->setVisible(m.barOn);
	s->magSlider->setVisible(syn);
	s->magSlider->setValue(qRound(m.contam * 100));
	s->magBtnFit->setVisible(syn);
	s->magAgeCombo->setVisible(syn && !list.empty());
	s->magAgeEdit->setVisible(syn && list.empty());
	s->magAgeCombo->clear();
	s->magAgeCombo->addItem("");                   // ecran's popup starts with an empty entry
	for (const XYIsoc &it : list) s->magAgeCombo->addItem(it.age);
	if (m.ageIdx > (int)list.size()) m.ageIdx = 0;
	s->magAgeCombo->setCurrentIndex(m.ageIdx);
	s->magAgeEdit->setText(num(m.ageTyped));
	s->magSyncing = false;
	if (s->magBar) s->magBar->update();
	// Its track was redrawn while another page was showing: re-run now it is shown (not from inside
	// whatever switched to it).
	if (m.rerun) QTimer::singleShot(0, s->win, [s] { if (xyAlive(s) && xyCur(s).mag.rerun) xyMagRerun(s); });
}

// "Create Mag Bar" (push_magBar_CB). The measured profile is the Object Manager's current curve (a
// plain line), else the one already chosen; the bar spans its first..last x.
static void xyMagCreateBar(XYPlot *s) {
	XYPage &pg = xyCur(s);
	XYMag &m = pg.mag;
	if (std::isnan(m.ageStart) || std::isnan(m.ageEnd) || m.ageStart == m.ageEnd) {
		QMessageBox::warning(s->win, "Geomagnetic bar code", "Take a second look to what you are asking for. Wrong ages");
		return;
	}
	int sel = xyCurrentSel(s);
	if (sel >= 0 && (pg.series[sel].magRole >= 2 || pg.series[sel].kind != 0 || pg.series[sel].nowCross)) sel = -1;
	if (sel < 0) sel = xyMagSeries(pg, 1);
	if (sel < 0 || !pg.series[sel].table || pg.series[sel].table->GetNumberOfRows() < 2) {
		QMessageBox::warning(s->win, "Geomagnetic bar code", "Select the measured anomaly profile in the Object Manager first.");
		return;
	}
	for (XYSeries &se : pg.series) if (se.magRole == 1) se.magRole = 0;
	pg.series[sel].magRole = 1;
	vtkTable *t = pg.series[sel].table;
	m.barX0 = t->GetValue(0, 0).ToDouble();
	m.barX1 = t->GetValue(t->GetNumberOfRows() - 1, 0).ToDouble();
	xyRequest(s, QString("magbar:aS=%1:aE=%2:stretch=%3").arg(m.ageStart, 0, 'g', 17).arg(m.ageEnd, 0, 'g', 17)
	             .arg(m.barStretch, 0, 'g', 17), sel, nullptr);
}

// A value inside a ":"-joined op string: '%' and ':' escaped (a Windows path carries a ':'). xymag.jl's
// `_mag_unesc` undoes it.
static QString xyOpEsc(const QString &v) {
	QString o = v;
	return o.replace("%", "%25").replace(":", "%3A");
}

// "Synthetic RTP" (push_syntheticRTP_CB) — and, with only `contam` changed, the slider (slider_filter_CB).
// `quiet`: an automatic re-run (the track was redrawn) — a wait cursor, not a modal busy box over a drag.
static void xyMagSynth(XYPlot *s, bool quiet = false) {
	XYPage &pg = xyCur(s);
	XYMag &m = pg.mag;
	const int sel = xyMagSeries(pg, 1);
	if (!m.barOn || m.bStart.empty() || sel < 0) {
		if (!quiet) QMessageBox::warning(s->win, "Geomagnetic bar code", "Create the bar code first: it sets the ages the profile spans.");
		return;
	}
	auto n = [](double v) { return std::isnan(v) ? QString() : QString::number(v, 'g', 17); };
	const QString op = QString("magsynth:aS=%1:aE=%2:x0=%3:rev=%4:unit=%5:speed=%6:pdir=%7:sdir=%8:depth=%9"
	                           ":dec=%10:inc=%11:contam=%12:bat=%13:dtype=%14")
		.arg(n(m.ageStart), n(m.ageEnd), n(m.bStart.front()), QString::number(m.ageStart > m.ageEnd ? 1 : 0),
		     QString::number(m.unit), n(m.speed), n(m.profDir), n(m.spreadDir), n(m.depth))
		.arg(n(m.dec), n(m.inc), n(m.contam), xyOpEsc(QString::fromStdString(m.batFile)),
		     xyOpEsc(prefDistAzimType()));        // the track is measured like everything else (Preferences)
	if (quiet) {
		QApplication::setOverrideCursor(Qt::WaitCursor);
		xyRequest(s, op, sel, nullptr);
		QApplication::restoreOverrideCursor();
	}
	else {
		xyRequest(s, op, sel, "Computing the synthetic magnetic anomaly…");
	}
}

// "Fit" (push_ageFit_CB): the synthetic chunk around the chosen age, moved to its best correlation
// with the measured profile.
static void xyMagFit(XYPlot *s) {
	XYPage &pg = xyCur(s);
	XYMag &m = pg.mag;
	const int dat = xyMagSeries(pg, 1), syn = xyMagSeries(pg, 2);
	if (dat < 0 || syn < 0) {
		QMessageBox::warning(s->win, "Geomagnetic bar code", "Compute the synthetic profile first.");
		return;
	}
	double pad = 1.5;
	int stretch = 15;
	const double age = xyMagChosenAge(m, &pad, &stretch);
	if (std::isnan(age)) {
		QMessageBox::warning(s->win, "Geomagnetic bar code", "Please, fit what? Your anniversary? How many Ma?");
		return;
	}
	xyMagPlaceStar(s);
	if (m.starIdx < 0) return;
	const QString op = QString("magfit:syn=%1:age=%2:pad=%3:stretch=%4:rev=%5:star=%6:guess=%7")
		.arg(syn).arg(age, 0, 'g', 17).arg(pad, 0, 'g', 17).arg(stretch)
		.arg(m.ageStart > m.ageEnd ? 1 : 0).arg(m.starIdx).arg(m.guessIdx);
	xyRequest(s, op, dat, "Fitting the isochron by correlation…");
}

// Replace a series' data in place (its plot, style and Object Manager row stay).
static void xyMagSetData(XYPlot *s, XYSeries &se, const double *x, const double *y, int n) {
	se.table->SetNumberOfRows(n);
	for (int i = 0; i < n; ++i) {
		se.table->SetValue(i, 0, x[i]);
		se.table->SetValue(i, 1, (float)y[i]);
	}
	se.table->Modified();
	xyCur(s).chart->RecalculateBounds();
}

// Julia's answers.
static void xyMagSetBar(XYPlot *s, const double *a0, const double *a1, int n, const char *labels,
                        double aS, double aE) {
	XYMag &m = xyCur(s).mag;
	m.bStart.assign(a0, a0 + n);
	m.bEnd.assign(a1, a1 + n);
	m.bTxt.clear();
	for (const QString &t : QString::fromUtf8(labels ? labels : "").split(QChar(0x1f)))
		m.bTxt.push_back(t.toStdString());
	m.bTxt.resize(n, "a");
	m.ageStart = aS;
	m.ageEnd = aE;
	m.barOn = n > 0;
	xyMagSyncUi(s);
	xyMagRender(s);
}
static void xyMagSetSynth(XYPlot *s, const double *x, const double *y, const double *ageLine, int n, double speed) {
	XYPage &pg = xyCur(s);
	int k = xyMagSeries(pg, 2);
	if (k >= 0) {
		xyMagSetData(s, pg.series[k], x, y, n);
	}
	else {
		k = xyAddSeries(s, x, y, n, "Synthetic RTP", 1.0, 0.0, 0.0, 1.5, -1, -1, -1.0);
		if (k >= 0) xyCur(s).series[k].magRole = 2;
	}
	XYMag &m = xyCur(s).mag;
	m.ageLine.assign(ageLine, ageLine + n);
	m.lastSpeed = speed;
	xyMagPlaceStar(s);                         // the chosen age keeps its star on the new curve
	xyMagSyncUi(s);
	xyMagRender(s);
}
static void xyMagSetFit(XYPlot *s, const double *x, const double *y, int n, double ageX, const char *name) {
	XYPage &pg = xyCur(s);
	const std::string nm = (name && name[0]) ? name : "Fit";
	int k = xyMagSeries(pg, 3);
	if (k >= 0) {
		xyMagSetData(s, pg.series[k], x, y, n);
		pg.series[k].name = nm;
		if (pg.series[k].plot) pg.series[k].plot->SetLabel(nm);
		xyRebuildObjMgr(s);
	}
	else {
		k = xyAddSeries(s, x, y, n, nm.c_str(), 0.0, 0.0, 1.0, 1.5, /*DASH*/2, -1, -1.0);
		if (k >= 0) xyCur(s).series[k].magRole = 3;
	}
	xyCur(s).mag.ageLineX = ageX;
	xyMagRender(s);
}

// pinpointLine: the fitted isochron becomes a fixed grey line, and the next fit draws a new one.
static void xyMagPin(XYPlot *s) {
	XYMag &m = xyCur(s).mag;
	if (std::isnan(m.ageLineX)) return;
	if (std::find(m.pinned.begin(), m.pinned.end(), m.ageLineX) == m.pinned.end()) m.pinned.push_back(m.ageLineX);
	std::sort(m.pinned.begin(), m.pinned.end());
	m.ageLineX = std::numeric_limits<double>::quiet_NaN();
	xyMagRender(s);
}

// ecran's freshFig == false path: the measured track changed, so the bar spans its new x, the old fit
// (dashed chunk + its age line) goes, and the synthetic + the star are computed again on it.
static void xyMagRerun(XYPlot *s) {
	XYPage &pg = xyCur(s);
	XYMag &m = pg.mag;
	m.rerun = false;
	const int dat = xyMagSeries(pg, 1);
	if (dat < 0 || !m.barOn) return;
	vtkTable *t = pg.series[dat].table;
	if (!t || t->GetNumberOfRows() < 2) return;
	m.barX0 = t->GetValue(0, 0).ToDouble();
	m.barX1 = t->GetValue(t->GetNumberOfRows() - 1, 0).ToDouble();
	m.ageLineX = std::numeric_limits<double>::quiet_NaN();
	m.guessIdx = -1;
	const int fit = xyMagSeries(pg, 3);
	if (fit >= 0) xyDeleteSeries(s, fit);
	if (xyMagSeries(xyCur(s), 2) >= 0) xyMagSynth(s, true);
	xyMagSyncUi(s);
	xyMagRender(s);
}

// The 3-D profile this window's linked curve came from was redrawn (ProfilePanel data listener): the
// curve takes the new samples and track at once; the magnetic tools re-run once the drag settles.
static void xyProfileUpdated(XYPlot *p) {
	if (!xyAlive(p) || !p->markPanel || p->markLinkPage < 0 || p->markLinkPage >= (int)p->pages.size()) return;
	XYPage &pg = p->pages[p->markLinkPage];
	if (p->markLinkSel < 0 || p->markLinkSel >= (int)pg.series.size()) return;
	const std::vector<double> &X = p->markPanel->seriesX(), &Y = p->markPanel->seriesY();
	if (X.size() < 2 || Y.size() != X.size()) return;
	XYSeries &se = pg.series[p->markLinkSel];
	se.table->SetNumberOfRows((vtkIdType)X.size());
	for (size_t i = 0; i < X.size(); ++i) {
		se.table->SetValue((vtkIdType)i, 0, X[i]);
		se.table->SetValue((vtkIdType)i, 1, (float)Y[i]);
	}
	se.table->Modified();
	int g = -1;
	if (p->markPanel->track(se.trackX, se.trackY, g)) se.trackGeog = g;
	else { se.trackX.clear(); se.trackY.clear(); se.trackGeog = -1; }
	pg.chart->RecalculateBounds();
	if (se.magRole == 1 && pg.mag.barOn) {
		pg.mag.rerun = true;
		if (p->markLinkPage == p->curPageIdx) {
			if (!p->profTimer) {
				p->profTimer = new QTimer(p->win);
				p->profTimer->setSingleShot(true);
				QObject::connect(p->profTimer, &QTimer::timeout, p->win, [p] {
					if (xyAlive(p) && xyCur(p).mag.rerun) xyMagRerun(p); });
			}
			p->profTimer->start(400);
		}
	}
	xyMagRender(p);
}

// The model parameters — what ecran reads from OPTcontrol.txt's MIR_MAGPROF lines (DEC, INC, SPEED,
// SPREADIR, ISOC), plus the layer depth and the profile's distance unit that ecran takes from its
// caller. A synthetic already standing is recomputed: it must describe the model now in force.
static void xyMagParams(XYPlot *s) {
	XYMag &m = xyCur(s).mag;
	QDialog d(s->win);
	d.setWindowTitle("Magnetic model parameters");
	QFormLayout *f = new QFormLayout(&d);
	auto num = [&d](double v, const QString &tip) {
		QLineEdit *e = new QLineEdit(std::isnan(v) ? QString() : QString::number(v, 'g', 10), &d);
		e->setToolTip(tip);
		return e;
	};
	QLineEdit *eDec   = num(m.dec, "Declination of the magnetization (degrees).");
	QLineEdit *eInc   = num(m.inc, "Inclination of the magnetization (degrees). 90 with declination 0 is the reduction to the pole.");
	QLineEdit *eSpeed = num(m.speed > 0 ? m.speed : std::numeric_limits<double>::quiet_NaN(),
		"FULL spreading rate in cm/yr. Empty: twice the profile length over the bar code's age span.");
	QLineEdit *ePdir  = num(m.profDir, "Azimuth of the profile (degrees clockwise from north).");
	QLineEdit *eSdir  = num(m.spreadDir, "Spreading direction (degrees). Empty: the profile azimuth, i.e. no obliquity.");
	QLineEdit *eDepth = num(m.depth, "Depth of the top of the magnetized layer (km). The layer is 0.5 km thick.");
	QComboBox *cUnit = new QComboBox(&d);
	cUnit->addItems({ "m", "km", "NM" });
	cUnit->setCurrentIndex(std::clamp(m.unit, 0, 2));
	cUnit->setToolTip("Unit of the profile's distance (its X). Not used when the profile carries a lon/lat "
	                  "track: the distance is then measured on the track itself, geodesically.");
	// The measured profile's own track, when it carries one (a profile from the 3-D viewer): its azimuth
	// is then read off the track (ecran: azimuth_geo of the first and last points) and a bathymetry grid
	// can be sampled along it (ecran's BAT).
	const int dat = xyMagSeries(xyCur(s), 1);
	const bool hasTrack = dat >= 0 && xyCur(s).series[dat].trackGeog >= 0;
	if (hasTrack) {
		ePdir->setEnabled(false);
		ePdir->setText("(from the track)");
		if (xyCur(s).series[dat].trackGeog == 1) cUnit->setEnabled(false);   // distance measured on the track
	}
	QLineEdit *eBat = new QLineEdit(QString::fromStdString(m.batFile), &d);
	eBat->setToolTip("A bathymetry grid, sampled along the profile's track for the depth of the magnetized "
	                 "layer (instead of the constant depth above). Needs a profile drawn in the 3-D viewer.");
	QPushButton *bBat = new QPushButton("…", &d);
	bBat->setAutoDefault(false);
	bBat->setFixedWidth(28);
	QObject::connect(bBat, &QPushButton::clicked, &d, [&d, eBat] {
		const QString fn = QFileDialog::getOpenFileName(&d, "Bathymetry grid", prefStartDir(),
			"Grids (*.grd *.nc *.tif *.tiff *.img);;All files (*)");
		if (!fn.isEmpty()) { rememberStartDir(fn); eBat->setText(fn); }
	});
	QWidget *batRow = new QWidget(&d);
	QHBoxLayout *batLay = new QHBoxLayout(batRow);
	batLay->setContentsMargins(0, 0, 0, 0);
	batLay->addWidget(eBat, 1);
	batLay->addWidget(bBat, 0);
	batRow->setEnabled(hasTrack);
	QLineEdit *eStretch = num(m.barStretch, "Stretch (> 1) or shrink (< 1) the bar code's bricks, to simulate an "
	                                        "oblique profile and/or oblique spreading. 1 = none.");
	QLineEdit *eIso = new QLineEdit(QString::fromStdString(m.isocs), &d);
	eIso->setToolTip("Isochrons to fit, separated by spaces: age[_pad[_stretch]]. pad = Ma either side of the age "
	                 "the correlation looks at (1.5 when omitted); stretch = the largest % the synthetic is "
	                 "expanded or shrunk while correlating (0 when omitted). Empty: type one age at a time.");
	f->addRow("Declination", eDec);
	f->addRow("Inclination", eInc);
	f->addRow("Full spreading rate (cm/yr)", eSpeed);
	f->addRow("Profile azimuth", ePdir);
	f->addRow("Spreading direction", eSdir);
	f->addRow("Layer top depth (km)", eDepth);
	f->addRow("Bathymetry grid", batRow);
	f->addRow("Distance unit", cUnit);
	f->addRow("Bar code stretch", eStretch);
	f->addRow("Isochrons to fit", eIso);
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
	f->addRow(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
	wrapTooltips(&d);
	if (d.exec() != QDialog::Accepted || !xyAlive(s)) return;
	auto get = [](QLineEdit *e, double def) {
		bool ok = false;
		const double v = e->text().trimmed().toDouble(&ok);
		return ok ? v : def;
	};
	XYMag &mm = xyCur(s).mag;
	mm.dec = get(eDec, mm.dec);
	mm.inc = get(eInc, mm.inc);
	mm.speed = std::max(0.0, get(eSpeed, 0.0));
	if (!hasTrack) mm.profDir = get(ePdir, mm.profDir);
	mm.spreadDir = get(eSdir, std::numeric_limits<double>::quiet_NaN());
	mm.depth = get(eDepth, mm.depth);
	mm.unit = cUnit->currentIndex();
	mm.batFile = eBat->text().trimmed().toStdString();
	const double st = get(eStretch, 1.0);
	const bool stretchChanged = st > 0 && st != mm.barStretch;
	if (st > 0) mm.barStretch = st;
	if (stretchChanged && mm.barOn) {          // the bricks move: the bar is made again (push_magBar_CB)
		xyRequest(s, QString("magbar:aS=%1:aE=%2:stretch=%3").arg(mm.ageStart, 0, 'g', 17)
		             .arg(mm.ageEnd, 0, 'g', 17).arg(mm.barStretch, 0, 'g', 17), xyMagSeries(xyCur(s), 1), nullptr);
	}
	const std::string iso = eIso->text().trimmed().toStdString();
	if (iso != mm.isocs) { mm.isocs = iso; mm.ageIdx = 0; mm.starIdx = -1; }
	xyMagSyncUi(s);
	if (xyMagSeries(xyCur(s), 2) >= 0) xyMagSynth(s);
	xyMagRender(s);
}

// The bar code strip over the chart (ecran's axes2): black normal-polarity bricks on white, spanning
// the measured profile under the chart's own x mapping, so it follows zoom and pan. Double-click
// drops a red "pico" marker (bdn_MagBar) with a line down through the plot; drag moves it; a
// double-click on it removes it (bdn_pico).
class XYMagBarStrip : public QWidget {
public:
	XYPlot *s = nullptr;
	int drag = -1;
	explicit XYMagBarStrip(XYPlot *sc, QWidget *parent = nullptr) : QWidget(parent), s(sc) {
		setFixedHeight(22);
		setObjectName("xyMagBar");
	}
protected:
	bool span(double &a, double &b) const {
		if (!xyAlive(s) || s->pages.empty() || !s->widget) return false;
		vtkAxis *ax = xyCur(s).chart->GetAxis(vtkAxis::BOTTOM);
		const double dpr = s->widget->devicePixelRatioF();
		a = ax->GetPoint1()[0] / dpr;
		b = ax->GetPoint2()[0] / dpr;
		return b > a;
	}
	double halfW() const {
		double a, b;
		return span(a, b) ? std::max(2.0, 0.005 * (b - a)) : 2.0;   // ecran: 0.5% of the bar's span
	}
	double picoPx(double age) const {
		double x;
		return xyMagAgeToX(xyCur(s).mag, age, x) ? xyMagXToPx(s, x) : -1e9;
	}
	int picoAt(double px) const {
		const std::vector<double> &pc = xyCur(s).mag.picos;
		for (int i = 0; i < (int)pc.size(); ++i)
			if (std::abs(picoPx(pc[i]) - px) <= halfW() + 2.0) return i;
		return -1;
	}
	bool ageAt(double px, double &age) const {
		const XYMag &m = xyCur(s).mag;
		if (!xyMagXToAge(m, xyMagPxToX(s, px), age)) return false;
		const double lo = std::min(m.ageStart, m.ageEnd), hi = std::max(m.ageStart, m.ageEnd);
		age = std::clamp(age, lo, hi);           // don't get out of the bar code
		return true;
	}
	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		g.fillRect(rect(), palette().window());
		double a, b;
		if (!span(a, b)) return;
		const XYMag &m = xyCur(s).mag;
		if (!m.barOn) return;
		const QRectF box(a, 1.0, b - a, height() - 2.0);
		g.fillRect(box, Qt::white);
		g.save();
		g.setClipRect(box);
		for (size_t i = 0; i < m.bStart.size(); ++i) {
			double x0, x1;
			if (!xyMagAgeToX(m, m.bStart[i], x0) || !xyMagAgeToX(m, m.bEnd[i], x1)) continue;
			const double p0 = xyMagXToPx(s, x0), p1 = xyMagXToPx(s, x1);
			g.fillRect(QRectF(std::min(p0, p1), box.top(), std::max(1.0, std::abs(p1 - p0)), box.height()), Qt::black);
		}
		const double xw = halfW(), H = box.height(), yb = box.bottom();
		g.setPen(QPen(Qt::black, 1));
		g.setBrush(Qt::red);
		for (double age : m.picos) {
			const double px = picoPx(age);
			QPolygonF st;
			st << QPointF(px, yb) << QPointF(px - xw, yb - 0.32 * H) << QPointF(px - xw, yb - 0.8 * H)
			   << QPointF(px + xw, yb - 0.8 * H) << QPointF(px + xw, yb - 0.32 * H);
			g.drawPolygon(st);
		}
		g.restore();
		g.setPen(QPen(Qt::black, 1));
		g.setBrush(Qt::NoBrush);
		g.drawRect(box);
	}
	void mouseDoubleClickEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton || !xyAlive(s)) return;
		XYMag &m = xyCur(s).mag;
		const int hit = picoAt(e->position().x());
		double age;
		if (hit >= 0) m.picos.erase(m.picos.begin() + hit);
		else if (ageAt(e->position().x(), age)) m.picos.push_back(age);
		drag = -1;
		xyMagRender(s);
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && xyAlive(s)) drag = picoAt(e->position().x());
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (drag < 0 || !xyAlive(s)) return;
		XYMag &m = xyCur(s).mag;
		double age;
		if (drag < (int)m.picos.size() && ageAt(e->position().x(), age)) {
			m.picos[drag] = age;
			xyMagRender(s);
		}
	}
	void mouseReleaseEvent(QMouseEvent *) override { drag = -1; }
};

// The picks, drawn by the chart at the end of its own paint (scene px, y up).
static void xyMagPaint(XYPlot *s, vtkChartXY *chart, vtkContext2D *p) {
	if (!s || s->pages.empty() || !s->widget) return;
	XYPage *pg = nullptr;
	for (auto &q : s->pages) if (q.chart.Get() == chart) { pg = &q; break; }
	if (!pg) return;
	const XYMag &m = pg->mag;
	vtkAxis *ax = chart->GetAxis(vtkAxis::BOTTOM), *ay = chart->GetAxis(vtkAxis::LEFT);
	const float x0 = ax->GetPoint1()[0], x1 = ax->GetPoint2()[0];
	const float y0 = ay->GetPoint1()[1], y1 = ay->GetPoint2()[1];
	const float dpr = (float)s->widget->devicePixelRatioF();
	auto vline = [&](double x, unsigned char r, unsigned char g, unsigned char b, float w, int ltype) {
		const double sx = xyAxisToScene(ax, x, 0);
		if (!std::isfinite(sx) || sx < x0 - 0.5 || sx > x1 + 0.5) return;
		p->GetPen()->SetColor(r, g, b, 255);
		p->GetPen()->SetWidth(w * dpr);
		p->GetPen()->SetLineType(ltype);
		p->DrawLine((float)sx, y0, (float)sx, y1);
		p->GetPen()->SetLineType(vtkPen::SOLID_LINE);
	};
	const bool tool = s->actMag && s->actMag->isChecked();
	if (tool && m.barOn) {
		// The chron names, at the top of the plot (ecran writes them in axes1 at ylim(2), 'top')
		const bool rev = m.ageStart > m.ageEnd;
		p->GetTextProp()->SetColor(0.0, 0.0, 0.0);
		p->GetTextProp()->SetFontSize(11);
		p->GetTextProp()->SetVerticalJustificationToTop();
		if (rev) p->GetTextProp()->SetJustificationToRight();
		else     p->GetTextProp()->SetJustificationToLeft();
		for (size_t i = 0; i < m.bStart.size() && i < m.bTxt.size(); ++i) {
			double x;
			if (m.bTxt[i] == "a" || !xyMagAgeToX(m, m.bStart[i], x)) continue;
			const double sx = xyAxisToScene(ax, x, 0);
			if (sx < x0 - 0.5 || sx > x1 + 0.5) continue;
			p->DrawString((float)sx + (rev ? -2.0f : 2.0f) * dpr, y1 - 2.0f * dpr, m.bTxt[i]);
		}
		for (double age : m.picos) {           // each pico's line down through the profile
			double x;
			if (xyMagAgeToX(m, age, x)) vline(x, 0, 0, 255, 1.0f, vtkPen::SOLID_LINE);
		}
	}
	for (double x : m.pinned) vline(x, 179, 179, 179, 3.0f, vtkPen::SOLID_LINE);     // pinned isochrons
	if (!std::isnan(m.ageLineX)) vline(m.ageLineX, 0, 0, 255, 1.0f, vtkPen::DASH_LINE);   // the fitted one
	// the red star on the synthetic at the chosen age
	const int syn = xyMagSeries(*pg, 2);
	if (syn >= 0 && m.starIdx >= 0 && pg->series[syn].table && m.starIdx < pg->series[syn].table->GetNumberOfRows()) {
		double x, y, sx, sy;
		xyRowXY(pg->series[syn].table, m.starIdx, x, y);
		if (xyDataToScene(*pg, x, y, sx, sy)) {
			const float R = 7.0f * dpr, r = 2.8f * dpr;
			float pts[22];
			for (int k = 0; k < 10; ++k) {
				const double a = vtkMath::Pi() / 2 + k * vtkMath::Pi() / 5;
				const float rr = (k % 2) ? r : R;
				pts[2 * k] = (float)sx + rr * (float)std::cos(a);
				pts[2 * k + 1] = (float)sy + rr * (float)std::sin(a);
			}
			pts[20] = pts[0]; pts[21] = pts[1];
			p->GetBrush()->SetColor(255, 0, 0, 255);
			p->GetPen()->SetColor(255, 0, 0, 255);
			for (int k = 0; k < 10; k += 2) {      // five kites, each convex: centre, inner, tip, inner
				const int a = (k + 9) % 10, c = (k + 1) % 10;
				float q[8] = { (float)sx, (float)sy, pts[2 * a], pts[2 * a + 1], pts[2 * k], pts[2 * k + 1], pts[2 * c], pts[2 * c + 1] };
				p->DrawQuad(q);
			}
			p->GetPen()->SetColor(0, 0, 0, 255);
			p->GetPen()->SetWidth(1.0f * dpr);
			p->DrawPoly(pts, 11);
		}
	}
	// the "Guide me" first guess on the measured profile
	const int dat = xyMagSeries(*pg, 1);
	if (dat >= 0 && m.guessIdx >= 0 && pg->series[dat].table && m.guessIdx < pg->series[dat].table->GetNumberOfRows()) {
		double x, y, sx, sy;
		xyRowXY(pg->series[dat].table, m.guessIdx, x, y);
		if (xyDataToScene(*pg, x, y, sx, sy)) {
			p->GetPen()->SetColor(0, 0, 0, 255);
			p->GetPen()->SetWidth(1.0f * dpr);
			p->GetBrush()->SetColor(0, 153, 0, 255);
			p->DrawEllipse((float)sx, (float)sy, 4.0f * dpr, 4.0f * dpr);
		}
	}
}

// Mouse on the chart for the picks: right-click on the red star -> "Guide me", on the fitted isochron
// -> "Pin-point me", on a pinned one -> "Delete me"; after "Guide me" the next left click on the
// measured profile sets the first guess (setFirstGuessForCorrelation / add_firstGuesssMarker).
// Installed after XYMarkFilter so it sees the press first; consumes only what is its own.
class XYMagFilter : public QObject {
public:
	XYPlot *s = nullptr;
	explicit XYMagFilter(XYPlot *sc, QObject *parent) : QObject(parent), s(sc) {}
protected:
	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (ev->type() != QEvent::MouseButtonPress || !s || !xyAlive(s) || s->pages.empty())
			return QObject::eventFilter(obj, ev);
		QMouseEvent *me = static_cast<QMouseEvent *>(ev);
		XYPage &pg = xyCur(s);
		XYMag &m = pg.mag;
		double sx, sy;
		xyQtToScene(s, me->position(), sx, sy);
		const double dpr = s->widget->devicePixelRatioF();
		if (s->magGuiding && me->button() == Qt::LeftButton) {
			s->magGuiding = false;
			s->widget->unsetCursor();
			s->win->statusBar()->clearMessage();
			const int dat = xyMagSeries(pg, 1);
			int sel = -1;
			double u = 0.0;
			if (dat >= 0 && xyCurveNearest(pg, dat, sx, sy, 1e9, sel, u))
				m.guessIdx = std::clamp((int)std::lround(u), 0, (int)pg.series[dat].table->GetNumberOfRows() - 1);
			xyMagRender(s);
			return true;
		}
		if (me->button() != Qt::RightButton) return QObject::eventFilter(obj, ev);
		vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM), *ay = pg.chart->GetAxis(vtkAxis::LEFT);
		const bool inY = sy >= ay->GetPoint1()[1] && sy <= ay->GetPoint2()[1];
		int kind = 0, pin = -1;
		const int syn = xyMagSeries(pg, 2);
		if (syn >= 0 && m.starIdx >= 0 && m.starIdx < pg.series[syn].table->GetNumberOfRows()) {
			double x, y, tx, ty;
			xyRowXY(pg.series[syn].table, m.starIdx, x, y);
			if (xyDataToScene(pg, x, y, tx, ty) && (tx - sx) * (tx - sx) + (ty - sy) * (ty - sy) <= 81.0 * dpr * dpr) kind = 1;
		}
		if (!kind && inY && !std::isnan(m.ageLineX) && std::abs(xyAxisToScene(ax, m.ageLineX, 0) - sx) <= 5.0 * dpr) kind = 2;
		for (int i = 0; !kind && inY && i < (int)m.pinned.size(); ++i)
			if (std::abs(xyAxisToScene(ax, m.pinned[i], 0) - sx) <= 5.0 * dpr) { kind = 3; pin = i; }
		if (!kind) return QObject::eventFilter(obj, ev);
		QMenu menu(s->win);
		QAction *a = menu.addAction(kind == 1 ? "Guide me" : kind == 2 ? "Pin-point me" : "Delete me");
		if (menu.exec(me->globalPosition().toPoint()) == a && xyAlive(s)) {
			if (kind == 1) {
				s->magGuiding = true;
				s->widget->setCursor(Qt::CrossCursor);
				s->win->statusBar()->showMessage("Click the measured profile where this isochron should roughly be");
			}
			else if (kind == 2) {
				xyMagPin(s);
			}
			else {
				XYMag &mm = xyCur(s).mag;
				if (pin < (int)mm.pinned.size()) mm.pinned.erase(mm.pinned.begin() + pin);
				xyMagRender(s);
			}
		}
		return true;
	}
};

// The control strip under the chart (ecran's uicontrols at the bottom of the figure).
static QWidget *xyMagBuildCtl(XYPlot *s) {
	QWidget *w = new QWidget();
	w->setObjectName("xyMagCtl");
	QHBoxLayout *l = new QHBoxLayout(w);
	l->setContentsMargins(6, 2, 6, 2);
	l->setSpacing(6);
	auto edit = [w](const char *name, const char *tip) {
		QLineEdit *e = new QLineEdit(w);
		e->setObjectName(name);
		e->setFixedWidth(55);
		e->setToolTip(tip);
		return e;
	};
	auto button = [w](const char *name, const char *text, const char *tip) {
		QPushButton *b = new QPushButton(text, w);
		b->setObjectName(name);
		b->setAutoDefault(false);
		if (tip) b->setToolTip(tip);
		return b;
	};
	s->magStart = edit("xyMagStart", "Age at which we start plotting the bars. If older than \"End age\" "
	                   "the bar code is plotted reversed. That is, from older to younger ages");
	s->magEnd = edit("xyMagEnd", "Age at which we stop plotting the bars.");
	s->magBtnBar = button("xyMagCreateBar", "Create Mag Bar", "Create a magnetic code bar on top of figure");
	s->magBtnSynth = button("xyMagSynthetic", "Synthetic RTP", "Create a synthetic profile reduced to the Pole");
	s->magBtnParams = button("xyMagParams", "Parameters…",
		"The magnetic model: declination, inclination, spreading rate and direction, layer depth, "
		"distance unit and the isochrons to fit");
	s->magSlider = new QSlider(Qt::Horizontal, w);
	s->magSlider->setObjectName("xyMagContam");
	s->magSlider->setRange(50, 100);
	s->magSlider->setFixedWidth(121);
	s->magSlider->setToolTip("Contamination factor ([0.5 1])");
	s->magAgeCombo = new QComboBox(w);
	s->magAgeCombo->setObjectName("xyMagAgeList");
	s->magAgeCombo->setMinimumWidth(70);
	s->magAgeCombo->setToolTip("Isochron age to fit (the Parameters list)");
	s->magAgeEdit = edit("xyMagAge", "Fit synthetic curve at this age by correlation to measured profile.");
	s->magBtnFit = button("xyMagFit", "Fit", "Fit the synthetic at this age to the measured profile by correlation");
	l->addWidget(new QLabel("Start age", w));
	l->addWidget(s->magStart);
	l->addWidget(new QLabel("End age", w));
	l->addWidget(s->magEnd);
	l->addWidget(s->magBtnBar);
	l->addWidget(s->magBtnSynth);
	l->addWidget(s->magBtnParams);
	l->addWidget(s->magSlider);
	l->addWidget(s->magAgeCombo);
	l->addWidget(s->magAgeEdit);
	l->addWidget(s->magBtnFit);
	l->addStretch(1);
	wrapTooltips(w);

	QObject::connect(s->magStart, &QLineEdit::editingFinished, w, [s] {   // edit_startAge_CB
		if (s->magSyncing || !xyAlive(s)) return;
		bool ok = false;
		double v = s->magStart->text().trimmed().toDouble(&ok);
		if (!ok) { v = std::numeric_limits<double>::quiet_NaN(); s->magStart->clear(); }
		else if (v < 0) { v = 0.0; s->magStart->setText("0"); }
		xyCur(s).mag.ageStart = v;
	});
	QObject::connect(s->magEnd, &QLineEdit::editingFinished, w, [s] {     // edit_ageEnd_CB
		if (s->magSyncing || !xyAlive(s)) return;
		bool ok = false;
		double v = s->magEnd->text().trimmed().toDouble(&ok);
		if (!ok) { v = std::numeric_limits<double>::quiet_NaN(); s->magEnd->clear(); }
		xyCur(s).mag.ageEnd = v;
	});
	QObject::connect(s->magBtnBar, &QPushButton::clicked, w, [s] { xyMagCreateBar(s); });
	QObject::connect(s->magBtnSynth, &QPushButton::clicked, w, [s] { xyMagSynth(s); });
	QObject::connect(s->magBtnParams, &QPushButton::clicked, w, [s] { xyMagParams(s); });
	QObject::connect(s->magSlider, &QSlider::sliderReleased, w, [s] {   // slider_filter_CB
		if (s->magSyncing || !xyAlive(s)) return;
		xyCur(s).mag.contam = s->magSlider->value() / 100.0;
		xyMagSynth(s);
	});
	QObject::connect(s->magAgeCombo, &QComboBox::currentIndexChanged, w, [s](int i) {   // popup_ageFit_CB
		if (s->magSyncing || !xyAlive(s)) return;
		XYMag &m = xyCur(s).mag;
		m.ageIdx = std::max(0, i);
		m.guessIdx = -1;                       // a new age: the old first guess means nothing
		xyMagPlaceStar(s);
		xyMagRender(s);
	});
	QObject::connect(s->magAgeEdit, &QLineEdit::editingFinished, w, [s] {   // edit_ageFit_CB
		if (s->magSyncing || !xyAlive(s)) return;
		bool ok = false;
		const double v = std::abs(s->magAgeEdit->text().trimmed().toDouble(&ok));
		if (!ok) s->magAgeEdit->clear();
		XYMag &m = xyCur(s).mag;
		m.ageTyped = ok ? v : std::numeric_limits<double>::quiet_NaN();
		m.guessIdx = -1;
		xyMagPlaceStar(s);
		xyMagRender(s);
	});
	QObject::connect(s->magBtnFit, &QPushButton::clicked, w, [s] { xyMagFit(s); });
	w->setVisible(false);
	return w;
}

// The toolbar icon: a little bar code.
static QIcon xyMagIcon() {
	QPixmap pm(18, 18);
	pm.fill(Qt::transparent);
	QPainter g(&pm);
	g.fillRect(QRect(1, 4, 16, 10), Qt::white);
	const int bars[][2] = { { 1, 2 }, { 4, 1 }, { 6, 3 }, { 11, 1 }, { 13, 3 } };
	for (const auto &b : bars) g.fillRect(QRect(b[0], 4, b[1], 10), Qt::black);
	g.setPen(QPen(Qt::black, 1));
	g.drawRect(QRect(1, 4, 15, 9));
	return QIcon(pm);
}

// ---- Reference a curve to another (ecran's pick_onLines2reference / add_refMarkers / do_reference) ----
// Two curves of the same thing on different x scales: points are picked ALTERNATELY on the two, and
// "Reference me" stretches the first, piece by piece, so each of its picked points lands on the x of
// its partner on the second. The stretching is Julia's (xyanalysis.jl `_xy_reference`, "reference:" op).

// ecran's get_pointOnLine: the row of curve `k` nearest the click, looking only at points within
// 1/20 of the plot width either side of it, distance measured in plot-size units on both axes.
// Returns -1 when the curve has none there; `dist` is that normalized distance.
static int xyRefNearest(XYPlot *s, int k, double sx, double sy, double &dist) {
	XYPage &pg = xyCur(s);
	dist = std::numeric_limits<double>::infinity();
	if (k < 0 || k >= (int)pg.series.size() || !pg.series[k].table) return -1;
	vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM), *ay = pg.chart->GetAxis(vtkAxis::LEFT);
	const double W = std::abs((double)ax->GetPoint2()[0] - ax->GetPoint1()[0]);
	const double H = std::abs((double)ay->GetPoint2()[1] - ay->GetPoint1()[1]);
	if (!(W > 0) || !(H > 0)) return -1;
	vtkTable *t = pg.series[k].table;
	int best = -1;
	for (vtkIdType r = 0; r < t->GetNumberOfRows(); ++r) {
		double x, y, px, py;
		xyRowXY(t, r, x, y);
		if (!xyDataToScene(pg, x, y, px, py) || std::abs(px - sx) > W / 20) continue;
		const double d = std::hypot((px - sx) / W, (py - sy) / H);
		if (d < dist) { dist = d; best = (int)r; }
	}
	return best;
}

// add_refMarkers: a click goes to the nearer of the two curves. A NEW marker while the two counts stay
// within one of each other and the click is on the other curve than the last one; otherwise it
// REPLACES the last marker on that curve (a corrected pick).
static void xyRefClick(XYPlot *s, double sx, double sy) {
	XYPage &pg = xyCur(s);
	double d0, d1;
	const int r0 = xyRefNearest(s, pg.refLine[0], sx, sy, d0), r1 = xyRefNearest(s, pg.refLine[1], sx, sy, d1);
	if (r0 < 0 && r1 < 0) return;
	const int k = (d0 <= d1) ? 0 : 1;
	const int row = k == 0 ? r0 : r1;
	std::vector<int> &v = pg.refRows[k];
	const int dp = std::abs((int)pg.refRows[0].size() - (int)pg.refRows[1].size());
	if (dp < 2 && k != pg.refLast) v.push_back(row);
	else if (v.empty()) v.push_back(row);
	else v.back() = row;
	pg.refLast = k;
	if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// The picks: filled circles, red on the curve to reference, green on the reference (ecran's colours).
static void xyRefPaint(XYPlot *s, vtkChartXY *chart, vtkContext2D *p) {
	if (!s || s->pages.empty() || !s->widget) return;
	XYPage *pg = nullptr;
	for (auto &q : s->pages) if (q.chart.Get() == chart) { pg = &q; break; }
	if (!pg) return;
	const float dpr = (float)s->widget->devicePixelRatioF();
	const unsigned char col[2][3] = { { 200, 0, 0 }, { 0, 200, 0 } };
	for (int k = 0; k < 2; ++k) {
		const int li = pg->refLine[k];
		if (li < 0 || li >= (int)pg->series.size() || !pg->series[li].table) continue;
		for (int row : pg->refRows[k]) {
			if (row < 0 || row >= pg->series[li].table->GetNumberOfRows()) continue;
			double x, y, sx, sy;
			xyRowXY(pg->series[li].table, row, x, y);
			if (!xyDataToScene(*pg, x, y, sx, sy)) continue;
			p->GetPen()->SetColor(0, 0, 0, 255);
			p->GetPen()->SetWidth(1.0f * dpr);
			p->GetBrush()->SetColor(col[k][0], col[k][1], col[k][2], 255);
			p->DrawEllipse((float)sx, (float)sy, 3.5f * dpr, 3.5f * dpr);
		}
	}
}

// do_reference: hand the picks to Julia, which stretches the curve and puts the result on a page of
// its own ("Referenced", ecran's new figure).
static void xyRefDo(XYPlot *s) {
	XYPage &pg = xyCur(s);
	const std::vector<int> &a = pg.refRows[0], &b = pg.refRows[1];
	if (a.size() != b.size() || a.size() < 2) {
		QMessageBox::warning(s->win, "Reference",
			QString("Pick the same number of points on the two curves, at least two each (now %1 and %2).")
			.arg(a.size()).arg(b.size()));
		return;
	}
	const int ref = pg.refLine[1];
	QStringList offs, xm;
	for (size_t i = 0; i < a.size(); ++i) {
		double x, y;
		xyRowXY(pg.series[ref].table, b[i], x, y);
		offs << QString::number(a[i]);
		xm << QString::number(x, 'g', 17);
	}
	const QString xl = QString::fromStdString(pg.chart->GetAxis(vtkAxis::BOTTOM)->GetTitle());
	const QString yl = QString::fromStdString(pg.chart->GetAxis(vtkAxis::LEFT)->GetTitle());
	xyRequest(s, QString("reference:offs=%1:xm=%2:xl=%3:yl=%4").arg(offs.join(' '), xm.join(' '), xyOpEsc(xl), xyOpEsc(yl)),
	          pg.refLine[0], nullptr);
}

// The tool's switch (ecran's toggle button). On: the Object Manager's current curve is the one to
// reference; the other curve of a two-curve page is the reference, else the user names it. Off (also
// reached by a right- or double-click while picking): the picks stay, for "Reference me".
static void xyRefSetPicking(XYPlot *s, bool on) {
	XYPage &pg = xyCur(s);
	if (!on) {
		pg.refPicking = false;
		if (s->actRef && s->actRef->isChecked()) { QSignalBlocker b(s->actRef); s->actRef->setChecked(false); }
		s->widget->unsetCursor();
		if (!pg.refRows[0].empty())
			s->win->statusBar()->showMessage("Right-click a red marker -> Reference me", 6000);
		return;
	}
	std::vector<int> lines;
	for (int i = 0; i < (int)pg.series.size(); ++i)
		if (pg.series[i].kind == 0 && !pg.series[i].nowCross && pg.series[i].table && pg.series[i].table->GetNumberOfRows() >= 2)
			lines.push_back(i);
	int sel = xyCurrentSel(s);
	if (std::find(lines.begin(), lines.end(), sel) == lines.end() || lines.size() < 2) {
		QMessageBox::warning(s->win, "Reference", "This needs two curves on the page; select the one to reference "
		                                          "in the Object Manager.");
		xyRefSetPicking(s, false);
		return;
	}
	int other = -1;
	if (lines.size() == 2) other = lines[0] == sel ? lines[1] : lines[0];
	else {
		QStringList names;
		std::vector<int> idx;
		for (int i : lines) if (i != sel) { names << QString::fromStdString(pg.series[i].name); idx.push_back(i); }
		bool ok = false;
		const QString pick = QInputDialog::getItem(s->win, "Reference",
			QString("Reference \"%1\" to which curve?").arg(QString::fromStdString(pg.series[sel].name)), names, 0, false, &ok);
		if (!ok) { xyRefSetPicking(s, false); return; }
		other = idx[names.indexOf(pick)];
	}
	if (pg.refLine[0] != sel || pg.refLine[1] != other) {      // another pair: the old picks mean nothing
		pg.refRows[0].clear(); pg.refRows[1].clear(); pg.refLast = -1;
	}
	pg.refLine[0] = sel; pg.refLine[1] = other;
	pg.refPicking = true;
	if (s->actRef && !s->actRef->isChecked()) { QSignalBlocker b(s->actRef); s->actRef->setChecked(true); }
	s->widget->setCursor(Qt::CrossCursor);
	s->win->statusBar()->showMessage("Pick points alternately on the two lines (red: the one to reference); "
	                                 "right- or double-click to finish");
}

// Mouse for the tool: while picking, a left click adds/replaces a marker and a right- or double-click
// ends picking; afterwards a right-click on a red marker offers "Reference me". Installed last, so it
// sees the press first; it consumes only its own.
class XYRefFilter : public QObject {
public:
	XYPlot *s = nullptr;
	explicit XYRefFilter(XYPlot *sc, QObject *parent) : QObject(parent), s(sc) {}
protected:
	bool eventFilter(QObject *obj, QEvent *ev) override {
		const QEvent::Type t = ev->type();
		if ((t != QEvent::MouseButtonPress && t != QEvent::MouseButtonDblClick && t != QEvent::MouseButtonRelease) ||
		    !s || !xyAlive(s) || s->pages.empty())
			return QObject::eventFilter(obj, ev);
		XYPage &pg = xyCur(s);
		QMouseEvent *me = static_cast<QMouseEvent *>(ev);
		if (pg.refPicking) {
			if (t == QEvent::MouseButtonRelease) return true;
			if (t == QEvent::MouseButtonDblClick || me->button() != Qt::LeftButton) { xyRefSetPicking(s, false); return true; }
			double sx, sy;
			xyQtToScene(s, me->position(), sx, sy);
			xyRefClick(s, sx, sy);
			return true;
		}
		if (t != QEvent::MouseButtonPress || me->button() != Qt::RightButton || pg.refRows[0].empty()) return QObject::eventFilter(obj, ev);
		const int li = pg.refLine[0];
		if (li < 0 || li >= (int)pg.series.size() || !pg.series[li].table) return QObject::eventFilter(obj, ev);
		double sx, sy;
		xyQtToScene(s, me->position(), sx, sy);
		const double tol = 8.0 * s->widget->devicePixelRatioF();
		for (int row : pg.refRows[0]) {
			if (row < 0 || row >= pg.series[li].table->GetNumberOfRows()) continue;
			double x, y, px, py;
			xyRowXY(pg.series[li].table, row, x, y);
			if (!xyDataToScene(pg, x, y, px, py) || std::hypot(px - sx, py - sy) > tol) continue;
			QMenu m(s->win);
			QAction *a = m.addAction("Reference me");
			if (m.exec(me->globalPosition().toPoint()) == a && xyAlive(s)) xyRefDo(s);
			return true;
		}
		return QObject::eventFilter(obj, ev);
	}
};

// ---- X,Y session (ecran's File > Session: Save / Open) ----------------------------------------------
// The whole window as a `[section] key=value` manifest — the SAME layout session.jl's manifest uses —
// with [session] kind=xyplot, then [window], and per page a [page] followed by its [series]. Values are
// one line (newlines escaped); number lists space-separated; NaN written "nan". The series' DATA is not
// in here: xysession.jl writes one binary file per series beside the manifest in the .igmtz zip, and
// on Open rebuilds the pages and series first, then hands this text back to xySessionApply.

static QString xyEsc(const QString &v) {
	QString o = v;
	o.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r");
	return o;
}
static QString xyUnesc(const QString &v) {
	QString o;
	for (int i = 0; i < v.size(); ++i) {
		if (v[i] == '\\' && i + 1 < v.size()) {
			const QChar c = v[++i];
			o += (c == 'n') ? QChar('\n') : (c == 'r') ? QChar('\r') : c;
		}
		else {
			o += v[i];
		}
	}
	return o;
}
static QString xyNum(double v) { return std::isnan(v) ? QString("nan") : QString::number(v, 'g', 17); }
static double xyToNum(const QString &v) {
	bool ok = false;
	const double d = v.trimmed().toDouble(&ok);
	return ok ? d : std::numeric_limits<double>::quiet_NaN();
}
static QString xyNums(const std::vector<double> &v) {
	QStringList l;
	for (double d : v) l << xyNum(d);
	return l.join(' ');
}
static std::vector<double> xyToNums(const QString &v) {
	std::vector<double> o;
	for (const QString &t : v.split(' ', Qt::SkipEmptyParts)) o.push_back(xyToNum(t));
	return o;
}

static std::string xySessionState(XYPlot *s) {
	QString o;
	auto kv = [&o](const char *k, const QString &v) { o += QString::fromLatin1(k) + "=" + v + "\n"; };
	o += "[session]\n";
	kv("kind", "xyplot");
	kv("version", "1");
	o += "[window]\n";
	kv("title", xyEsc(s->win->windowTitle()));
	kv("winsize", QString::number(s->win->width()) + " " + QString::number(s->win->height()));
	kv("legend", QString::number(s->legendVisible ? 1 : 0));
	kv("legendfont", xyNum(s->legendFontPt));
	kv("info", xyEsc(s->infoLabel ? s->infoLabel->text() : QString()));
	kv("infofont", xyNum(s->infoFontPt));
	kv("infovisible", QString::number(s->infoLabel && s->infoLabel->isVisible() ? 1 : 0));
	kv("grid", QString::number(s->actGrid && s->actGrid->isChecked() ? 1 : 0));
	kv("magtool", QString::number(s->actMag && s->actMag->isChecked() ? 1 : 0));
	kv("curpage", QString::number(s->curPageIdx));
	kv("npages", QString::number((int)s->pages.size()));
	for (int pi = 0; pi < (int)s->pages.size(); ++pi) {
		const XYPage &pg = s->pages[pi];
		vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM), *ay = pg.chart->GetAxis(vtkAxis::LEFT);
		o += "[page]\n";
		kv("name", xyEsc(QString::fromStdString(pg.name)));
		kv("xlabel", xyEsc(QString::fromStdString(ax->GetTitle())));
		kv("ylabel", xyEsc(QString::fromStdString(ay->GetTitle())));
		kv("xtime", QString::number(pg.xTimeFmt));
		kv("square", QString::number(pg.square ? 1 : 0));
		kv("axeshidden", QString::number(pg.axesHidden ? 1 : 0));
		kv("axesshown", QString::number(pg.axesShown ? 1 : 0));
		// the grid as drawn, per direction (read by Plot with GMT.jl; a session restores it from axprop)
		// the title and subtitle as drawn (read by Plot with GMT.jl; a session restores them from axprop)
		kv("title", xyEsc(xyChartTitle(pg.chart)));
		kv("subtitle", xyEsc(pg.axProps.count("-B.+s") ? QString::fromStdString(pg.axProps.at("-B.+s")) : QString()));
		kv("gridx", QString::number(ax->GetGridVisible() ? 1 : 0));
		kv("gridy", QString::number(ay->GetGridVisible() ? 1 : 0));
		// Only a legend the user dragged has a place of its own (XYChart::legFx, scene fractions).
		if (vtkChartLegend *lg = pg.chart->GetLegend())
			if (lg->GetHorizontalAlignment() == vtkChartLegend::CUSTOM) {
				const XYChart *c = static_cast<const XYChart *>(pg.chart.GetPointer());
				kv("legendpos", xyNum(c->legFx) + " " + xyNum(c->legFy));
			}
		for (const auto &ap : pg.axProps)             // the Axes properties fields changed from the .ui's
			kv("axprop", QString::fromStdString(ap.first) + " " + xyEsc(QString::fromStdString(ap.second)));
		for (const XYLabel &l : pg.labels) {             // one key per label, in order
			kv("label", xyNums({ l.x, l.y, l.r, l.g, l.b, l.size, (double)l.ha, (double)l.va }) + " " +
			            xyEsc(QString::fromStdString(l.text)));
			if (l.bold) kv("labelbold", "1");            // of the label just written
		}
		for (const XYAnnot &a : pg.annots) {             // one key per annotation, in drawing order
			kv("annot", xyNums({ (double)a.kind, a.x0, a.y0, a.x1, a.y1, a.r, a.g, a.b, a.width, (double)a.lineType,
			                     a.size, a.filled ? 1.0 : 0.0, a.fr, a.fg, a.fb, a.visible ? 1.0 : 0.0, a.fit ? 1.0 : 0.0 }) +
			            " " + xyEsc(QString::fromStdString(a.text)));
			if (!a.boxed) kv("annotbox", "0");           // of the annotation just written (a text box without its box)
		}
		// whether VTK really DRAWS the axis log (only when it can place every value it shows); read-only
		kv("logxactive", QString::number(ax->GetLogScaleActive() ? 1 : 0));
		kv("logyactive", QString::number(ay->GetLogScaleActive() ? 1 : 0));
		kv("logx", QString::number(ax->GetLogScale() ? 1 : 0));
		kv("logy", QString::number(ay->GetLogScale() ? 1 : 0));
		kv("xfixed", QString::number(ax->GetBehavior() == vtkAxis::FIXED ? 1 : 0));
		kv("yfixed", QString::number(ay->GetBehavior() == vtkAxis::FIXED ? 1 : 0));
		kv("xrange", xyNum(ax->GetUnscaledMinimum()) + " " + xyNum(ax->GetUnscaledMaximum()));
		kv("yrange", xyNum(ay->GetUnscaledMinimum()) + " " + xyNum(ay->GetUnscaledMaximum()));
		kv("nseries", QString::number((int)pg.series.size()));
		const XYMag &m = pg.mag;
		QStringList txt;
		for (const std::string &t : m.bTxt) txt << QString::fromStdString(t);
		kv("mag.ageStart", xyNum(m.ageStart));
		kv("mag.ageEnd", xyNum(m.ageEnd));
		kv("mag.barOn", QString::number(m.barOn ? 1 : 0));
		kv("mag.bStart", xyNums(m.bStart));
		kv("mag.bEnd", xyNums(m.bEnd));
		kv("mag.bTxt", xyEsc(txt.join(' ')));
		kv("mag.barX", xyNum(m.barX0) + " " + xyNum(m.barX1));
		kv("mag.picos", xyNums(m.picos));
		kv("mag.dec", xyNum(m.dec));
		kv("mag.inc", xyNum(m.inc));
		kv("mag.speed", xyNum(m.speed));
		kv("mag.profDir", xyNum(m.profDir));
		kv("mag.spreadDir", xyNum(m.spreadDir));
		kv("mag.depth", xyNum(m.depth));
		kv("mag.unit", QString::number(m.unit));
		kv("mag.contam", xyNum(m.contam));
		kv("mag.isocs", xyEsc(QString::fromStdString(m.isocs)));
		kv("mag.batFile", xyEsc(QString::fromStdString(m.batFile)));
		kv("mag.barStretch", xyNum(m.barStretch));
		kv("mag.lastSpeed", xyNum(m.lastSpeed));
		kv("mag.ageLine", xyNums(m.ageLine));
		kv("mag.ageIdx", QString::number(m.ageIdx));
		kv("mag.ageTyped", xyNum(m.ageTyped));
		kv("mag.starIdx", QString::number(m.starIdx));
		kv("mag.guessIdx", QString::number(m.guessIdx));
		kv("mag.ageLineX", xyNum(m.ageLineX));
		kv("mag.pinned", xyNums(m.pinned));
		std::vector<double> sg;                       // kept Spector-Grant lines, 11 numbers each
		for (const XYSGLine &l : pg.sgLines)
			sg.insert(sg.end(), { l.x0, l.y0, l.x1, l.y1, l.m, l.b, l.depth, l.unit, l.r, l.g, l.bl });
		kv("sglines", xyNums(sg));
		for (int i = 0; i < (int)pg.series.size(); ++i) {
			const XYSeries &se = pg.series[i];
			o += "[series]\n";
			kv("page", QString::number(pi));             // page + idx FIRST: xySessionApply keys on them
			kv("idx", QString::number(i));
			kv("name", xyEsc(QString::fromStdString(se.name)));
			kv("kind", QString::number(se.kind));
			kv("barh", QString::number(se.barH ? 1 : 0));
			kv("stack", QString::number(se.stack));
			kv("right", QString::number(se.rightAxis ? 1 : 0));
			kv("nowcross", QString::number(se.nowCross ? 1 : 0));
			kv("rgb", xyNum(se.r) + " " + xyNum(se.g) + " " + xyNum(se.b));
			kv("width", xyNum(se.width));
			kv("ltype", QString::number(se.lineType));
			kv("marker", QString::number(se.marker));
			kv("msize", xyNum(se.markerSize));
			kv("visible", QString::number(se.visible ? 1 : 0));
			kv("inlegend", QString::number(se.inLegend ? 1 : 0));
			kv("barwidth", xyNum(se.barWidth));
			kv("cross", xyNum(se.crossCenterX) + " " + xyNum(se.crossCenterY) + " " + xyNum(se.crossSizePx));
			kv("magrole", QString::number(se.magRole));
			kv("trackgeog", QString::number(se.trackGeog));
			kv("trackx", xyNums(se.trackX));
			kv("tracky", xyNums(se.trackY));
			kv("erryl", xyNums(se.errYlo));
			kv("erryh", xyNums(se.errYhi));
			kv("errxl", xyNums(se.errXlo));
			kv("errxh", xyNums(se.errXhi));
			kv("bubble", xyNums(se.bubble));
			kv("npoints", QString::number(se.table ? (int)se.table->GetNumberOfRows() : 0));
		}
	}
	return o.toStdString();
}

// Put the saved state onto a window whose pages and series xysession.jl has just rebuilt (same count,
// same order). Everything the data files do not carry comes from here.
static void xySessionApply(XYPlot *s, const char *text) {
	if (!xyAlive(s) || !text) return;
	QString sec;
	int pi = -1, sp = -1, si = -1, cur = 0;
	bool grid = true;
	auto pageOk = [s](int p) { return p >= 0 && p < (int)s->pages.size(); };
	for (const QString &raw : QString::fromUtf8(text).split('\n')) {
		const QString line = raw.endsWith('\r') ? raw.chopped(1) : raw;
		if (line.isEmpty()) continue;
		if (line.startsWith('[') && line.endsWith(']')) {
			sec = line.mid(1, line.size() - 2);
			if (sec == "page") ++pi;
			if (sec == "series") { sp = -1; si = -1; }
			continue;
		}
		const int eq = line.indexOf('=');
		if (eq < 0) continue;
		const QString k = line.left(eq), v = line.mid(eq + 1);
		if (sec == "window") {
			if (k == "title") s->win->setWindowTitle(xyUnesc(v));
			else if (k == "winsize") {
				const QStringList f = v.split(' ');
				if (f.size() == 2 && f[0].toInt() > 0 && f[1].toInt() > 0) s->win->resize(f[0].toInt(), f[1].toInt());
			}
			else if (k == "legend") s->legendVisible = v.toInt() != 0;
			else if (k == "legendfont") s->legendFontPt = xyToNum(v);
			else if (k == "info" && s->infoLabel) s->infoLabel->setText(xyUnesc(v));
			else if (k == "infofont") { s->infoFontPt = xyToNum(v); xyApplyInfoStyle(s); }
			else if (k == "infovisible" && s->infoLabel) s->infoLabel->setVisible(v.toInt() != 0 && !s->infoLabel->text().isEmpty());
			else if (k == "grid") grid = v.toInt() != 0;
			else if (k == "magtool" && s->actMag) s->actMag->setChecked(v.toInt() != 0);
			else if (k == "curpage") cur = v.toInt();
		}
		else if (sec == "page" && pageOk(pi)) {
			XYPage &pg = s->pages[pi];
			XYMag &m = pg.mag;
			vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM), *ay = pg.chart->GetAxis(vtkAxis::LEFT);
			if (k == "name") xyRenamePage(s, pi, xyUnesc(v));
			else if (k == "xlabel") ax->SetTitle(xyUnesc(v).toStdString());
			else if (k == "ylabel") ay->SetTitle(xyUnesc(v).toStdString());
			else if (k == "xtime") pg.xTimeFmt = v.toInt();
			else if (k == "square") { pg.square = v.toInt() != 0; xyApplyPageLook(s, pg); }
			else if (k == "axeshidden") { pg.axesHidden = v.toInt() != 0; xyApplyPageLook(s, pg); }
			else if (k == "axesshown") { pg.axesShown = v.toInt() != 0; xyApplyAxes(s, pg); }
			else if (k == "legendpos") {
				const std::vector<double> f = xyToNums(v);
				if (f.size() == 2 && std::isfinite(f[0]) && std::isfinite(f[1])) {
					XYChart *c = static_cast<XYChart *>(pg.chart.GetPointer());
					c->legFx = (float)f[0];  c->legFy = (float)f[1];   // placed in px by every paint
					if (vtkChartLegend *lg = pg.chart->GetLegend()) {
						lg->SetHorizontalAlignment(vtkChartLegend::CUSTOM);
						lg->SetVerticalAlignment(vtkChartLegend::CUSTOM);
					}
				}
			}
			else if (k == "axprop") {
				const int sp = v.indexOf(' ');
				if (sp > 0) { pg.axProps[v.left(sp).toStdString()] = xyUnesc(v.mid(sp + 1)).toStdString(); xyApplyAxes(s, pg); }
			}
			else if (k == "label") {
				const QStringList f = v.split(' ');
				if (f.size() >= 9) {
					XYLabel l;
					l.x = xyToNum(f[0]); l.y = xyToNum(f[1]); l.r = xyToNum(f[2]); l.g = xyToNum(f[3]); l.b = xyToNum(f[4]);
					l.size = xyToNum(f[5]); l.ha = f[6].toInt(); l.va = f[7].toInt();
					l.text = xyUnesc(f.mid(8).join(' ')).toStdString();
					pg.labels.push_back(l);
				}
			}
			else if (k == "labelbold") {
				if (!pg.labels.empty()) pg.labels.back().bold = v.toInt() != 0;
			}
			else if (k == "annot") {
				const QStringList f = v.split(' ');
				if (f.size() >= 18) {
					XYAnnot a;
					a.kind = std::max(0, std::min(XY_AN_COUNT - 1, f[0].toInt()));
					a.x0 = xyToNum(f[1]); a.y0 = xyToNum(f[2]); a.x1 = xyToNum(f[3]); a.y1 = xyToNum(f[4]);
					a.r = xyToNum(f[5]); a.g = xyToNum(f[6]); a.b = xyToNum(f[7]);
					a.width = xyToNum(f[8]); a.lineType = f[9].toInt(); a.size = xyToNum(f[10]);
					a.filled = f[11].toInt() != 0;
					a.fr = xyToNum(f[12]); a.fg = xyToNum(f[13]); a.fb = xyToNum(f[14]);
					a.visible = f[15].toInt() != 0; a.fit = f[16].toInt() != 0;
					a.text = xyUnesc(f.mid(17).join(' ')).toStdString();
					pg.annots.push_back(a);
				}
			}
			else if (k == "annotbox") {
				if (!pg.annots.empty()) pg.annots.back().boxed = v.toInt() != 0;
			}
			else if (k == "logx") ax->SetLogScale(v.toInt() != 0);
			else if (k == "logy") ay->SetLogScale(v.toInt() != 0);
			else if (k == "xfixed" || k == "yfixed") (k == "xfixed" ? ax : ay)->SetBehavior(v.toInt() ? vtkAxis::FIXED : vtkAxis::AUTO);
			else if (k == "xrange" || k == "yrange") {
				vtkAxis *a = (k == "xrange") ? ax : ay;
				const std::vector<double> r = xyToNums(v);
				if (r.size() == 2 && a->GetBehavior() == vtkAxis::FIXED && std::isfinite(r[0]) && std::isfinite(r[1]))
					a->SetUnscaledRange(r[0], r[1]);
			}
			else if (k == "mag.ageStart") m.ageStart = xyToNum(v);
			else if (k == "mag.ageEnd") m.ageEnd = xyToNum(v);
			else if (k == "mag.barOn") m.barOn = v.toInt() != 0;
			else if (k == "mag.bStart") m.bStart = xyToNums(v);
			else if (k == "mag.bEnd") m.bEnd = xyToNums(v);
			else if (k == "mag.bTxt") {
				m.bTxt.clear();
				for (const QString &t : xyUnesc(v).split(' ', Qt::SkipEmptyParts)) m.bTxt.push_back(t.toStdString());
				m.bTxt.resize(m.bStart.size(), "a");
			}
			else if (k == "mag.barX") { const std::vector<double> r = xyToNums(v); if (r.size() == 2) { m.barX0 = r[0]; m.barX1 = r[1]; } }
			else if (k == "mag.picos") m.picos = xyToNums(v);
			else if (k == "mag.dec") m.dec = xyToNum(v);
			else if (k == "mag.inc") m.inc = xyToNum(v);
			else if (k == "mag.speed") m.speed = xyToNum(v);
			else if (k == "mag.profDir") m.profDir = xyToNum(v);
			else if (k == "mag.spreadDir") m.spreadDir = xyToNum(v);
			else if (k == "mag.depth") m.depth = xyToNum(v);
			else if (k == "mag.unit") m.unit = v.toInt();
			else if (k == "mag.contam") m.contam = xyToNum(v);
			else if (k == "mag.isocs") m.isocs = xyUnesc(v).toStdString();
			else if (k == "mag.batFile") m.batFile = xyUnesc(v).toStdString();
			else if (k == "mag.barStretch") m.barStretch = xyToNum(v);
			else if (k == "mag.lastSpeed") m.lastSpeed = xyToNum(v);
			else if (k == "mag.ageLine") m.ageLine = xyToNums(v);
			else if (k == "mag.ageIdx") m.ageIdx = v.toInt();
			else if (k == "mag.ageTyped") m.ageTyped = xyToNum(v);
			else if (k == "mag.starIdx") m.starIdx = v.toInt();
			else if (k == "mag.guessIdx") m.guessIdx = v.toInt();
			else if (k == "mag.ageLineX") m.ageLineX = xyToNum(v);
			else if (k == "mag.pinned") m.pinned = xyToNums(v);
			else if (k == "sglines") {
				const std::vector<double> a = xyToNums(v);
				for (size_t j = 0; j + 11 <= a.size(); j += 11) {
					XYSGLine l;
					l.x0 = a[j]; l.y0 = a[j + 1]; l.x1 = a[j + 2]; l.y1 = a[j + 3]; l.m = a[j + 4]; l.b = a[j + 5];
					l.depth = a[j + 6]; l.unit = a[j + 7]; l.r = a[j + 8]; l.g = a[j + 9]; l.bl = a[j + 10];
					xySGAddLine(pg, l);
				}
			}
		}
		else if (sec == "series") {
			if (k == "page") { sp = v.toInt(); continue; }
			if (k == "idx")  { si = v.toInt(); continue; }
			if (!pageOk(sp) || si < 0 || si >= (int)s->pages[sp].series.size()) continue;
			XYSeries &se = s->pages[sp].series[si];
			if (k == "name") { se.name = xyUnesc(v).toStdString(); if (se.plot) se.plot->SetLabel(se.name); }
			else if (k == "rgb") { const std::vector<double> c = xyToNums(v); if (c.size() == 3) { se.r = c[0]; se.g = c[1]; se.b = c[2]; } }
			else if (k == "width") se.width = xyToNum(v);
			else if (k == "ltype") se.lineType = v.toInt();
			else if (k == "marker") se.marker = v.toInt();
			else if (k == "msize") se.markerSize = xyToNum(v);
			else if (k == "visible") { se.visible = v.toInt() != 0; if (se.plot) se.plot->SetVisible(se.visible); }
			else if (k == "inlegend") se.inLegend = v.toInt() != 0;
			else if (k == "barwidth") se.barWidth = xyToNum(v);
			else if (k == "stack") se.stack = v.toInt();      // its base is re-derived below (xyRestack)
			else if (k == "kind" || k == "barh" || k == "right") {
				// The loader made a line or vertical bars; any other gallery type gets its own item.
				const int kd = (k == "kind") ? v.toInt() : se.kind;
				const bool bh = (k == "barh") ? v.toInt() != 0 : se.barH;
				const bool ra = (k == "right") ? v.toInt() != 0 : se.rightAxis;
				if ((kd != se.kind || bh != se.barH || ra != se.rightAxis) && !se.nowCross && se.table) {
					XYPage &pg = s->pages[sp];
					se.kind = kd;
					se.barH = bh;
					se.rightAxis = ra;
					if (se.plot) pg.chart->RemovePlotInstance(se.plot);
					se.plot = xyAddPlotItem(pg.chart, se);
					se.plot->SetInputData(se.table, 0, 1);
					se.plot->SetLabel(se.name);
					se.plot->SetTooltipLabelFormat("%x, %y");
					se.plot->SetVisible(se.visible);
				}
			}
			else if (k == "magrole") se.magRole = v.toInt();
			else if (k == "trackgeog") se.trackGeog = v.toInt();
			else if (k == "trackx") se.trackX = xyToNums(v);
			else if (k == "tracky") se.trackY = xyToNums(v);
			else if (k == "erryl") se.errYlo = xyToNums(v);
			else if (k == "erryh") se.errYhi = xyToNums(v);
			else if (k == "errxl") se.errXlo = xyToNums(v);
			else if (k == "errxh") se.errXhi = xyToNums(v);
			else if (k == "bubble") se.bubble = xyToNums(v);
			if (se.plot) xyApplyStyle(s, se);
		}
	}
	for (XYPage &pg : s->pages) {
		xyRestack(pg);                                // stacked / waterfall / fill_between bases
		for (XYSeries &se : pg.series)
			if (se.plot) xyApplyStyle(s, se);
		pg.chart->SetShowLegend(s->legendVisible);
		if (vtkChartLegend *lg = pg.chart->GetLegend()) lg->GetLabelProperties()->SetFontSize((int)s->legendFontPt);
		// the window-wide "grid" of an older session; a page whose grid is its own (X-Grid / Y-Grid, or
		// its Axes properties) keeps it
		if (!pg.axProps.count("MAP_FRAME_AXES.Sg")) pg.chart->GetAxis(vtkAxis::BOTTOM)->SetGridVisible(grid);
		if (!pg.axProps.count("MAP_FRAME_AXES.Wg")) pg.chart->GetAxis(vtkAxis::LEFT)->SetGridVisible(grid);
		pg.chart->RecalculateBounds();
	}
	xySwitchPage(s, std::clamp(cur, 0, (int)s->pages.size() - 1));   // remounts, rebuilds the panels + mag UI
	xyMagRender(s);
}

// The X,Y window as the owner of a "Plot with GMT.jl" editor — the SAME editor the 3-D viewer opens.
// It parks in the viewer this plot belongs to (where the plot itself parks); the script is built from
// the current page by xyscript.jl.
// Park a tool window of the plot (already hidden) as a row of the plot's OWN Object Manager. Idempotent:
// a tool parked again keeps its one row.
static void xyParkTool(XYPlot *p, QWidget *w, const QString &label, std::function<void()> unpark,
                       std::function<void(const QPoint &)> menu) {
	if (!xyAlive(p) || !w) return;
	for (auto &t : p->parkedTools)
		if (t.w == w) { t.label = label; t.unpark = unpark; t.menu = menu; xyRebuildParkStrip(p); return; }
	p->parkedTools.push_back({ w, label, unpark, menu });
	QObject::connect(w, &QObject::destroyed, p->win, [p, w]() {   // a deleted tool loses its row too
		if (!xyAlive(p)) return;
		auto &v = p->parkedTools;
		v.erase(std::remove_if(v.begin(), v.end(), [w](const XYPlot::ParkedTool &t) { return t.w == w; }), v.end());
		xyRebuildParkStrip(p);
	});
	xyRebuildParkStrip(p);
}

static void xyUnparkTool(XYPlot *p, QWidget *w) {
	if (!xyAlive(p)) return;
	auto &v = p->parkedTools;
	v.erase(std::remove_if(v.begin(), v.end(), [w](const XYPlot::ParkedTool &t) { return t.w == w; }), v.end());
	xyRebuildParkStrip(p);
}

static ScriptHost scriptHostXY(XYPlot *p) {
	ScriptHost h;
	h.key = p;
	h.parent = p ? p->win : nullptr;
	// It parks in THIS window's Object Manager, never in the viewer's Scene Objects.
	h.park = [p](QWidget *w) {
		ScriptHost me = scriptHostXY(p);
		xyParkTool(p, w, me.parkLabel, [me]() { scriptEditorUnpark(me); }, scriptEditorParkedMenu(me));
	};
	h.unpark = [p](QWidget *w) { xyUnparkTool(p, w); };
	h.parkLabel = "GMT.jl script";
	h.openCall = "InteractiveGMT._xy_script_open_editor";
	h.alive = [p]() { return xyAlive(p); };
	h.logError = [p](const QString &m) { if (xyAlive(p)) xyLog(p, m, true); };
	h.status = [p](const QString &m, int ms) { if (xyAlive(p) && p->win) p->win->statusBar()->showMessage(m, ms); };
	return h;
}

// ---- window builder --------------------------------------------------------

// Which viewer a new plot belongs to — i.e. whose Scene Objects dock it parks in when its window is
// closed with the X. Resolved HERE, once, for EVERY way of opening a plot (the Tools menu, a profile,
// Julia's own xyplot() / File > New, the tide and gmtedit tools…): making each call site remember to
// set an owner is exactly how the feature ended up doing nothing at all. The active viewer window
// wins; failing that the most recently used one; failing that the only one open.
static Scene *xyDefaultOwner() {
	if (QWidget *aw = QApplication::activeWindow())
		for (Scene *sc : g_scenes)
			if (sceneAlive(sc) && sc->win && static_cast<QWidget*>(sc->win) == aw) return sc;
	if (sceneAlive(g_lastScene)) return g_lastScene;
	if (g_scenes.size() == 1)    return *g_scenes.begin();
	return nullptr;                     // no viewer at all -> nowhere to park, the X really closes
}

// ---- plot-type gallery (the strip of miniatures under the menus, MATLAB's PLOTS tab) ----------------
// One entry per type; the index is what xySetPlotType takes.
// MATLAB's set first, then the rest of GMT.jl's 2-D plotting functions (its "Plotting functions"
// examples): scatter, plotyy, fill_between, stacked bars (bar stack=true, also hbar), the waterfall
// (bar stacked=:water), and the statistical plots. Those last ones (from XY_PT_HIST on) are not a way
// of drawing the curves but a different quantity (a distribution), so they land on a NEW page, computed
// in Julia by GMT.jl's own helpers (xyanalysis.jl `_xy_stat`).
enum { XY_PT_PLOT, XY_PT_AREA, XY_PT_BAR, XY_PT_SEMILOGX, XY_PT_SEMILOGY, XY_PT_LOGLOG, XY_PT_COMET,
       XY_PT_STEM, XY_PT_STAIRS, XY_PT_BARH, XY_PT_SCATTER, XY_PT_PLOTYY, XY_PT_FILLBETWEEN, XY_PT_BARSTACK,
       XY_PT_BARHSTACK, XY_PT_WATERFALL, XY_PT_ERRORBAR, XY_PT_BUBBLE, XY_PT_HIST, XY_PT_DENSITY, XY_PT_BOXPLOT,
       XY_PT_VIOLIN, XY_PT_ECDF, XY_PT_QQNORM, XY_PT_QQPLOT, XY_PT_RADAR, XY_PT_ROSE, XY_PT_FEATHER,
       XY_PT_POLARPLOT, XY_PT_POLARSCATTER, XY_PT_COMPASS, XY_PT_PIE, XY_PT_DONUT, XY_PT_PARALLEL, XY_PT_CORNER, XY_PT_MARGINAL,
       XY_PT_BIPLOT, XY_PT_COUNT };
static const char *const xyPlotTypeNames[XY_PT_COUNT] = { "plot", "area", "bar", "semilogx", "semilogy", "loglog",
	"comet", "stem", "stairs", "barh", "scatter", "plotyy", "fill_between", "bar stack", "barh stack", "waterfall",
	"errorbar", "bubblechart", "histogram", "density", "boxplot", "violin", "ecdfplot", "qqnorm", "qqplot", "radar",
	"rose", "feather", "polarplot", "polarscatter", "compassplot", "piechart", "donutchart", "parallelplot", "cornerplot", "marginalhist",
	"biplot" };
static void xyComet(XYPlot *s);   // below: comet is an animation, not a kind of series

// Re-plot every series of the CURRENT page as `type`. The series keep their data and style; only the
// plot item (line / bar, bar orientation) and the axis scaling change. The plot items are re-made by
// xyRebuildPlots and the axes set by xySetLog — the same functions a delete and the Misc menu use.
// Does a series carry the per-point values a per-point type (errorbar / bubblechart) draws?
static bool xyHasPerPoint(const XYSeries &se, int type) {
	return type == XY_PT_ERRORBAR ? !(se.errYlo.empty() && se.errYhi.empty() && se.errXlo.empty() && se.errXhi.empty())
	                              : !se.bubble.empty();
}

// Re-plot the current page as error bars / bubbles: a series that carries the values is drawn with them,
// one that does not stays a line. The same plot items, rebuild and axes reset as every gallery type.
static void xyDrawPerPoint(XYPlot *s, int type) {
	if (!xyAlive(s)) return;
	XYPage &pg = xyCur(s);
	for (XYSeries &se : pg.series) {
		if (se.nowCross) continue;
		se.kind = xyHasPerPoint(se, type) ? (type == XY_PT_ERRORBAR ? 9 : 10) : 0;
		se.barH = false; se.stack = 0; se.rightAxis = false;
	}
	xyRebuildPlots(s);
	pg.chart->GetAxis(vtkAxis::BOTTOM)->SetBehavior(vtkAxis::AUTO);
	pg.chart->GetAxis(vtkAxis::LEFT)->SetBehavior(vtkAxis::AUTO);
	xySetLog(s, 0, false);
	xySetLog(s, 1, false);
	s->win->statusBar()->showMessage(QString("Plot type: %1").arg(xyPlotTypeNames[type]), 3000);
}

// The values a series without them gets, from the dialog: from another series of the page (row by row),
// a constant, or (error bars) a percentage of each point's Y. `dir` (error bars): 0 Y, 1 X, 2 both.
// The source series is hidden, not deleted: its numbers are now the bars / sizes. THE one place the
// dialog's answer becomes data, so the dialog and the test hook can never differ.
static bool xyPerPointFrom(XYPlot *s, int type, int target, int source, int mode, double value, int dir, bool hideSource) {
	if (!xyAlive(s)) return false;
	XYPage &pg = xyCur(s);
	if (target < 0 || target >= (int)pg.series.size() || !pg.series[target].table) return false;
	XYSeries &se = pg.series[target];
	const int n = (int)se.table->GetNumberOfRows();
	std::vector<double> v((size_t)n, NAN);
	if (mode == 0) {                                  // another series
		if (source < 0 || source >= (int)pg.series.size() || source == target || !pg.series[source].table) return false;
		vtkTable *t = pg.series[source].table;
		const int m = std::min(n, (int)t->GetNumberOfRows());
		for (int i = 0; i < m; ++i) v[i] = t->GetValue(i, 1).ToDouble();
	}
	else if (mode == 1) std::fill(v.begin(), v.end(), value);
	else {                                            // a percentage of Y
		for (int i = 0; i < n; ++i) v[i] = std::abs(se.table->GetValue(i, 1).ToDouble()) * value / 100.0;
	}
	if (type == XY_PT_ERRORBAR) {
		for (double &e : v) e = std::abs(e);
		if (dir == 0 || dir == 2) { se.errYlo = v; se.errYhi = v; }
		if (dir == 1 || dir == 2) { se.errXlo = v; se.errXhi = v; }
	}
	else
		se.bubble = v;
	if (mode == 0 && hideSource) pg.series[source].visible = false;
	return true;
}

// The dialog a per-point type opens when a series of the page carries no values for it. Window-modal
// but not blocking (open(), not exec()), so the chart stays drawn behind it; OK goes through
// xyPerPointFrom and then draws the page.
static void xyPerPointDialog(XYPlot *s, int type) {
	XYPage &pg = xyCur(s);
	const bool eb = (type == XY_PT_ERRORBAR);
	QDialog *dlg = new QDialog(s->win);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setObjectName("xyPerPointDlg");
	dlg->setWindowTitle(eb ? "Error bars" : "Bubble sizes");
	QFormLayout *form = new QFormLayout(dlg);
	QLabel *msg = new QLabel(eb ? "This series has no error values. Take them from:" :
	                              "This series has no bubble sizes. Take them from:", dlg);
	msg->setWordWrap(true);
	form->addRow(msg);
	QComboBox *tgt = new QComboBox(dlg);
	tgt->setObjectName("ppSeries");
	QComboBox *src = new QComboBox(dlg);
	src->setObjectName("ppSource");
	for (int k = 0; k < (int)pg.series.size(); ++k) {
		const XYSeries &se = pg.series[k];
		if (se.nowCross) continue;
		const QString nm = QString::fromStdString(se.name.empty() ? "Line " + std::to_string(k + 1) : se.name);
		if (!xyHasPerPoint(se, type)) tgt->addItem(nm, k);
		src->addItem("Series: " + nm, k);
	}
	src->addItem("Constant", -1);
	if (eb) src->addItem("% of Y", -2);
	form->addRow("Series", tgt);
	form->addRow("Values from", src);
	QDoubleSpinBox *val = new QDoubleSpinBox(dlg);
	val->setObjectName("ppValue");
	val->setDecimals(6); val->setRange(0.0, 1e12); val->setValue(1.0);
	form->addRow("Value", val);
	QComboBox *dir = nullptr;
	if (eb) {
		dir = new QComboBox(dlg);
		dir->setObjectName("ppDir");
		dir->addItems(QStringList() << "Vertical (Y)" << "Horizontal (X)" << "Both");
		form->addRow("Direction", dir);
	}
	QCheckBox *hide = new QCheckBox("Hide the series the values come from", dlg);
	hide->setObjectName("ppHide");
	hide->setChecked(true);
	form->addRow(hide);
	auto sync = [src, val, hide] {
		const int m = src->currentData().toInt();
		val->setEnabled(m < 0);
		hide->setEnabled(m >= 0);
	};
	QObject::connect(src, qOverload<int>(&QComboBox::currentIndexChanged), dlg, sync);
	if (tgt->count() > 0)                             // first source that is not the target
		for (int i = 0; i < src->count(); ++i)
			if (src->itemData(i).toInt() != tgt->currentData().toInt()) { src->setCurrentIndex(i); break; }
	sync();
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
	bb->button(QDialogButtonBox::Ok)->setObjectName("ppOk");
	bb->button(QDialogButtonBox::Cancel)->setObjectName("ppCancel");
	form->addRow(bb);
	QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
	QObject::connect(bb, &QDialogButtonBox::accepted, dlg, [s, type, dlg, tgt, src, val, dir, hide] {
		const int t = tgt->currentData().toInt(), m = src->currentData().toInt();
		const int mode = m >= 0 ? 0 : (m == -1 ? 1 : 2);
		if (mode == 0 && m == t) {
			QMessageBox::warning(dlg, dlg->windowTitle(), "A series cannot take its values from itself.");
			return;
		}
		if (!xyPerPointFrom(s, type, t, m, mode, val->value(), dir ? dir->currentIndex() : 0, hide->isChecked())) {
			QMessageBox::warning(dlg, dlg->windowTitle(), "Those values could not be taken.");
			return;
		}
		dlg->accept();
		xyDrawPerPoint(s, type);
		xyRebuildObjMgr(s);
	});
	dlg->open();
}

// errorbar / bubblechart: draw at once when every series carries its values, else ask for them first.
static void xyPerPoint(XYPlot *s, int type) {
	XYPage &pg = xyCur(s);
	for (const XYSeries &se : pg.series)
		if (!se.nowCross && !xyHasPerPoint(se, type)) {
			xyPerPointDialog(s, type);
			return;
		}
	xyDrawPerPoint(s, type);
}

static void xySetPlotType(XYPlot *s, int type) {
	if (!xyAlive(s) || type < 0 || type >= XY_PT_COUNT || s->cometBusy)
		return;
	if (type == XY_PT_COMET) {
		xyComet(s);
		return;
	}
	XYPage &pg = xyCur(s);
	if (pg.series.empty()) {
		s->win->statusBar()->showMessage("This page has no series to re-plot", 3000);
		return;
	}
	if (type == XY_PT_ERRORBAR || type == XY_PT_BUBBLE) {   // per-point values: carried, or asked for
		xyPerPoint(s, type);
		return;
	}
	if (type >= XY_PT_HIST) {                         // a distribution: computed in Julia, on a new page
		if (type == XY_PT_HIST) {
			const int sel = xyCurrentSel(s);
			if (sel < 0) { xyLog(s, "Select a series in the Object Manager first"); return; }
			xyRequest(s, "hist:0", sel, "Computing the histogram...");
		}
		else {
			xyRequest(s, QString("stat:%1").arg(xyPlotTypeNames[type]), xyCurrentSel(s),
			          QString("Computing the %1...").arg(xyPlotTypeNames[type]).toUtf8().constData());
		}
		return;
	}
	if ((type == XY_PT_PLOTYY || type == XY_PT_FILLBETWEEN) && pg.series.size() < 2) {
		QMessageBox::information(s->win, "X,Y plot", QString("%1 needs two curves on this page.").arg(xyPlotTypeNames[type]));
		return;
	}
	const bool bar = (type == XY_PT_BAR || type == XY_PT_BARH || type == XY_PT_BARSTACK || type == XY_PT_BARHSTACK ||
	                  type == XY_PT_WATERFALL);
	for (int k = 0; k < (int)pg.series.size(); ++k) {
		XYSeries &se = pg.series[k];
		if (se.nowCross)
			continue;                                // a marker, not data: it stays what it is
		if (bar && se.kind != 1 && se.kind != 6 && se.kind != 7 && se.table) {   // a line becoming bars: width
			const int n = (int)se.table->GetNumberOfRows();                          // from its own x spacing
			std::vector<double> x((size_t)n);
			for (int i = 0; i < n; ++i)
				x[i] = se.table->GetValue(i, 0).ToDouble();
			const double w = xyAutoBarWidth(x.data(), n);
			if (w > 0.0) se.barWidth = w;
		}
		switch (type) {
		case XY_PT_BAR: case XY_PT_BARH:  se.kind = 1; break;
		case XY_PT_AREA:                  se.kind = 2; break;
		case XY_PT_STEM:                  se.kind = 3; break;
		case XY_PT_STAIRS:                se.kind = 4; break;
		case XY_PT_SCATTER:               se.kind = 5; break;
		case XY_PT_BARSTACK: case XY_PT_WATERFALL: se.kind = 6; break;
		case XY_PT_BARHSTACK:             se.kind = 7; break;
		case XY_PT_FILLBETWEEN:           se.kind = k == 0 ? 0 : 2; break;
		default:                          se.kind = 0; break;
		}
		se.barH = (type == XY_PT_BARH);
		se.stack = (type == XY_PT_BARSTACK || type == XY_PT_BARHSTACK) ? 1 : type == XY_PT_WATERFALL ? 2 :
		           (type == XY_PT_FILLBETWEEN && k > 0) ? 3 : 0;
		se.rightAxis = (type == XY_PT_PLOTYY && k > 0);
	}
	xyRebuildPlots(s);
	pg.chart->GetAxis(vtkAxis::BOTTOM)->SetBehavior(vtkAxis::AUTO);   // a bar <-> barh swap trades the axes' ranges
	pg.chart->GetAxis(vtkAxis::LEFT)->SetBehavior(vtkAxis::AUTO);
	pg.chart->GetAxis(vtkAxis::RIGHT)->SetBehavior(vtkAxis::AUTO);    // plotyy's second axis
	xySetLog(s, 0, type == XY_PT_SEMILOGX || type == XY_PT_LOGLOG);
	xySetLog(s, 1, type == XY_PT_SEMILOGY || type == XY_PT_LOGLOG);
	s->win->statusBar()->showMessage(QString("Plot type: %1").arg(xyPlotTypeNames[type]), 3000);
}

// The gallery choice by its name ("stairs", "scatter", ...): what the host uses to draw a page it has
// just built in one of the gallery's types (the ECDF page is stairs). False for an unknown name.
static bool xySetPlotTypeByName(XYPlot *s, const char *name) {
	for (int t = 0; name && t < XY_PT_COUNT; ++t)
		if (std::strcmp(name, xyPlotTypeNames[t]) == 0) {
			xySetPlotType(s, t);
			return true;
		}
	return false;
}

// MATLAB's comet: the page is re-plotted as lines, and each curve is then drawn in over ~2 s, a circle
// at its head. The axes are held at the whole data's range meanwhile, so nothing rescales as it grows.
// The real series are only hidden while it runs; the moving trail + head are temporary plot items
// (not series, no legend entry), removed at the end, when the real curves come back.
static void xyComet(XYPlot *s) {
	XYPage &pg0 = xyCur(s);
	if (pg0.series.empty()) {
		s->win->statusBar()->showMessage("This page has no series to animate", 3000);
		return;
	}
	xySetPlotType(s, XY_PT_PLOT);                     // lines on linear axes, bounds of all the data
	XYPage &pg = xyCur(s);
	struct Run {
		vtkSmartPointer<vtkChartXY> chart;
		std::vector<int> idx;                         // series animated
		std::vector<vtkSmartPointer<vtkTable>> src, trail, head;
		std::vector<vtkSmartPointer<vtkPlot>> plots;  // temporary trail + head items
		int step = 0;
	};
	auto run = std::make_shared<Run>();
	run->chart = pg.chart;
	pg.chart->GetAxis(vtkAxis::BOTTOM)->SetBehavior(vtkAxis::FIXED);
	pg.chart->GetAxis(vtkAxis::LEFT)->SetBehavior(vtkAxis::FIXED);
	for (int i = 0; i < (int)pg.series.size(); ++i) {
		XYSeries &se = pg.series[i];
		if (se.nowCross || !se.visible || !se.plot || !se.table || se.table->GetNumberOfRows() < 2)
			continue;
		se.plot->SetVisible(false);
		auto mk = [&](vtkSmartPointer<vtkTable> &t, int type, float width) {
			t = vtkSmartPointer<vtkTable>::New();
			vtkNew<vtkDoubleArray> ax; ax->SetName("X"); t->AddColumn(ax);
			vtkNew<vtkFloatArray> ay; ay->SetName("Y"); t->AddColumn(ay);
			vtkPlot *pl = pg.chart->AddPlot(type);
			pl->SetInputData(t, 0, 1);
			pl->SetLegendVisibility(false);
			pl->SetColor((unsigned char)(se.r * 255), (unsigned char)(se.g * 255), (unsigned char)(se.b * 255), 255);
			pl->SetWidth(width);
			run->plots.push_back(pl);
			return pl;
		};
		vtkSmartPointer<vtkTable> tt, th;
		mk(tt, vtkChart::LINE, (float)se.width);
		if (vtkPlotPoints *hp = vtkPlotPoints::SafeDownCast(mk(th, vtkChart::POINTS, 2.0f))) {
			hp->SetMarkerStyle(vtkPlotPoints::CIRCLE);
			hp->SetMarkerSize((float)(9.0 * s->widget->devicePixelRatioF()));
		}
		for (vtkIdType r = 0; r < 2; ++r) {               // the first two points: a line is never painted shorter
			tt->InsertNextBlankRow();
			tt->SetValue(r, 0, se.table->GetValue(r, 0));
			tt->SetValue(r, 1, se.table->GetValue(r, 1));
		}
		th->InsertNextBlankRow();
		th->SetValue(0, 0, se.table->GetValue(1, 0));
		th->SetValue(0, 1, se.table->GetValue(1, 1));
		run->idx.push_back(i);
		run->src.push_back(se.table);
		run->trail.push_back(tt);
		run->head.push_back(th);
	}
	if (run->idx.empty())
		return;
	pg.chart->SetShowLegend(false);                   // its entries are hidden meanwhile: an empty box
	s->cometBusy = true;
	const int steps = 60;
	QTimer *tm = new QTimer(s->win);
	tm->setInterval(33);
	QObject::connect(tm, &QTimer::timeout, s->win, [s, run, tm, steps] {
		// The end (or the page went away from under it): drop the temporary items, bring the curves back.
		auto finish = [&] {
			tm->stop();
			tm->deleteLater();
			for (auto &pl : run->plots)
				run->chart->RemovePlotInstance(pl);
			for (XYPage &p : s->pages)
				if (p.chart == run->chart)
					for (XYSeries &se : p.series)
						if (se.plot) se.plot->SetVisible(se.visible);
			run->chart->SetShowLegend(s->legendVisible);
			run->chart->GetAxis(vtkAxis::BOTTOM)->SetBehavior(vtkAxis::AUTO);
			run->chart->GetAxis(vtkAxis::LEFT)->SetBehavior(vtkAxis::AUTO);
			run->chart->RecalculateBounds();
			s->cometBusy = false;
			if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
		};
		if (!xyAlive(s) || xyCur(s).chart != run->chart || ++run->step > steps) {
			finish();
			return;
		}
		for (size_t k = 0; k < run->idx.size(); ++k) {
			vtkTable *src = run->src[k], *tr = run->trail[k], *hd = run->head[k];
			const vtkIdType n = src->GetNumberOfRows();
			const vtkIdType m = std::min(n, std::max<vtkIdType>(2, (n * run->step + steps - 1) / steps));
			for (vtkIdType r = tr->GetNumberOfRows(); r < m; ++r) {
				tr->InsertNextBlankRow();
				tr->SetValue(r, 0, src->GetValue(r, 0));
				tr->SetValue(r, 1, src->GetValue(r, 1));
			}
			hd->SetNumberOfRows(1);
			hd->SetValue(0, 0, src->GetValue(m - 1, 0));
			hd->SetValue(0, 1, src->GetValue(m - 1, 1));
			tr->Modified();
			hd->Modified();
		}
		if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
	});
	tm->start();
}

// The gallery miniature of a plot type: a framed little chart, drawn here (no image files to ship).
static QIcon xyPlotTypeIcon(int type, qreal dpr) {
	const int W = 56, H = 36;
	QPixmap pm(qRound(W * dpr), qRound(H * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::white);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	const QColor blue(0, 114, 189), orange(217, 83, 25), grid(200, 200, 200);
	const QRectF fr(1.5, 1.5, W - 3.0, H - 3.0);
	const double x0 = fr.left() + 3, x1 = fr.right() - 3, y0 = fr.top() + 3, y1 = fr.bottom() - 3;
	// log-spaced grid lines (one decade's 2..9 ticks squeezed into the frame)
	auto logLines = [&](bool vertical) {
		p.setPen(QPen(grid, 0.8));
		for (int d = 0; d < 2; ++d)
			for (int k = 1; k <= 9; ++k) {
				const double f = (d + std::log10((double)k)) / 2.0;
				if (vertical) { const double x = x0 + f * (x1 - x0); p.drawLine(QPointF(x, y0), QPointF(x, y1)); }
				else          { const double y = y1 - f * (y1 - y0); p.drawLine(QPointF(x0, y), QPointF(x1, y)); }
			}
	};
	// a wave whose x is warped by `warp` (0..1 -> 0..1), so the log types squeeze it the way they do
	auto wave = [&](auto warp) {
		QPainterPath path;
		for (int i = 0; i <= 80; ++i) {
			const double t = i / 80.0;
			const double x = x0 + warp(t) * (x1 - x0);
			const double y = (y0 + y1) / 2 - 0.42 * (y1 - y0) * std::sin(t * 4.0 * M_PI);
			if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
		}
		p.setPen(QPen(blue, 1.6));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
	};
	switch (type) {
	case XY_PT_PLOT:
		wave([](double t) { return t; });
		break;
	case XY_PT_BAR:
	case XY_PT_BARH:
		p.setPen(Qt::NoPen);
		for (int i = 0; i < 6; ++i) {
			const double v1 = 0.2 + 0.13 * i, v2 = 0.12 + 0.15 * i;
			if (type == XY_PT_BAR) {
				const double bx = x0 + 2 + i * (x1 - x0 - 2) / 6.0, bw = (x1 - x0) / 15.0;
				p.setBrush(orange); p.drawRect(QRectF(bx, y1 - v2 * (y1 - y0), bw, v2 * (y1 - y0)));
				p.setBrush(blue);   p.drawRect(QRectF(bx + bw, y1 - v1 * (y1 - y0), bw, v1 * (y1 - y0)));
			}
			else {
				const double by = y0 + 1 + i * (y1 - y0 - 1) / 6.0, bh = (y1 - y0) / 14.0;
				p.setBrush(blue);   p.drawRect(QRectF(x0, by, (1.0 - 0.14 * i) * (x1 - x0), bh));
				p.setBrush(orange); p.drawRect(QRectF(x0, by + bh, (0.9 - 0.13 * i) * (x1 - x0), bh));
			}
		}
		break;
	case XY_PT_SEMILOGX:
		logLines(true);
		wave([](double t) { return std::log10(1.0 + 99.0 * t) / 2.0; });
		break;
	case XY_PT_SEMILOGY:
		logLines(false);
		wave([](double t) { return t; });
		break;
	case XY_PT_LOGLOG:
		logLines(true);
		logLines(false);
		wave([](double t) { return std::log10(1.0 + 99.0 * t) / 2.0; });
		break;
	case XY_PT_AREA: {
		QPainterPath path;
		path.moveTo(x0, y1);
		for (int i = 0; i <= 80; ++i) {
			const double t = i / 80.0;
			path.lineTo(x0 + t * (x1 - x0), y1 - (0.1 + 0.8 * std::abs(std::sin(t * 2.0 * M_PI))) * (y1 - y0));
		}
		path.lineTo(x1, y1);
		path.closeSubpath();
		p.setPen(QPen(blue, 1.0));
		p.setBrush(blue);
		p.drawPath(path);
		break;
	}
	case XY_PT_COMET: {
		p.setPen(QPen(blue, 1.4, Qt::DotLine));
		p.setBrush(Qt::NoBrush);
		QPainterPath path;
		const double hx = x0 + 0.62 * (x1 - x0), hy = y0 + 0.35 * (y1 - y0);
		path.moveTo(x0 + 0.4 * (x1 - x0), y1);
		path.quadTo(x0 + 0.5 * (x1 - x0), y0, hx, hy);
		p.drawPath(path);
		p.setPen(QPen(blue, 1.4));
		p.drawEllipse(QPointF(hx + 2, hy + 1), 2.6, 2.6);
		break;
	}
	case XY_PT_STEM:
		for (int i = 0; i < 12; ++i) {
			const double x = x0 + 2 + i * (x1 - x0 - 4) / 11.0;
			const double y = y1 - (0.25 + 0.65 * std::abs(std::sin(i * 0.55))) * (y1 - y0);
			p.setPen(QPen(blue, 1.2));
			p.drawLine(QPointF(x, y1), QPointF(x, y));
			p.setBrush(Qt::white);
			p.drawEllipse(QPointF(x, y), 1.6, 1.6);
		}
		break;
	case XY_PT_STAIRS: {
		QPainterPath path;
		const int ns = 12;
		for (int i = 0; i <= ns; ++i) {
			const double x = x0 + i * (x1 - x0) / ns;
			const double y = (y0 + y1) / 2 - 0.38 * (y1 - y0) * std::sin(i * 2.0 * M_PI / 6.0);
			if (i == 0) path.moveTo(x, y);
			else { path.lineTo(x, path.currentPosition().y()); path.lineTo(x, y); }
		}
		p.setPen(QPen(blue, 1.4));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
		break;
	}
	case XY_PT_SCATTER:
		p.setPen(QPen(blue, 1.0));
		p.setBrush(Qt::NoBrush);
		for (int i = 0; i < 22; ++i) {
			const double t = std::fmod(i * 0.618034, 1.0), u = std::fmod(i * 0.414214 + 0.2, 1.0);
			p.drawEllipse(QPointF(x0 + 2 + t * (x1 - x0 - 4), y0 + 2 + (0.15 + 0.7 * u * (1 - 0.5 * t)) * (y1 - y0 - 4)), 1.8, 1.8);
		}
		break;
	case XY_PT_PLOTYY: {
		QPainterPath a, b;
		for (int i = 0; i <= 60; ++i) {
			const double t = i / 60.0, x = x0 + t * (x1 - x0);
			const double ya = (y0 + y1) / 2 - 0.35 * (y1 - y0) * std::sin(t * 4.0 * M_PI);
			const double yb = y1 - (0.15 + 0.7 * t * t) * (y1 - y0);
			if (i == 0) { a.moveTo(x, ya); b.moveTo(x, yb); } else { a.lineTo(x, ya); b.lineTo(x, yb); }
		}
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(blue, 1.4));   p.drawPath(a);
		p.setPen(QPen(orange, 1.4)); p.drawPath(b);
		p.setPen(QPen(blue, 1.0));   p.drawLine(QPointF(x0 + 1, y0), QPointF(x0 + 1, y1));
		p.setPen(QPen(orange, 1.0)); p.drawLine(QPointF(x1 - 1, y0), QPointF(x1 - 1, y1));
		break;
	}
	case XY_PT_FILLBETWEEN: {
		QPainterPath top, band;
		std::vector<QPointF> a, b;
		for (int i = 0; i <= 60; ++i) {
			const double t = i / 60.0, x = x0 + t * (x1 - x0);
			a.emplace_back(x, (y0 + y1) / 2 - 0.35 * (y1 - y0) * std::sin(t * 2.0 * M_PI));
			b.emplace_back(x, (y0 + y1) / 2 + 0.2 * (y1 - y0) * std::sin(t * 2.0 * M_PI + 1.0));
		}
		band.moveTo(a[0]);
		for (const QPointF &q : a) band.lineTo(q);
		for (auto it = b.rbegin(); it != b.rend(); ++it) band.lineTo(*it);
		band.closeSubpath();
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 114, 189, 110));
		p.drawPath(band);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(blue, 1.3));
		p.drawPolyline(a.data(), (int)a.size());
		p.setPen(QPen(orange, 1.3));
		p.drawPolyline(b.data(), (int)b.size());
		break;
	}
	case XY_PT_BARSTACK:
	case XY_PT_BARHSTACK:
		p.setPen(Qt::NoPen);
		for (int i = 0; i < 5; ++i) {
			const double v1 = 0.2 + 0.1 * i, v2 = 0.15 + 0.06 * (4 - i);
			if (type == XY_PT_BARSTACK) {
				const double bx = x0 + 3 + i * (x1 - x0 - 3) / 5.0, bw = (x1 - x0) / 8.0, H = y1 - y0;
				p.setBrush(blue);   p.drawRect(QRectF(bx, y1 - v1 * H, bw, v1 * H));
				p.setBrush(orange); p.drawRect(QRectF(bx, y1 - (v1 + v2) * H, bw, v2 * H));
			}
			else {
				const double by = y0 + 1 + i * (y1 - y0 - 1) / 5.0, bh = (y1 - y0) / 7.5, W = x1 - x0;
				p.setBrush(blue);   p.drawRect(QRectF(x0, by, v1 * W, bh));
				p.setBrush(orange); p.drawRect(QRectF(x0 + v1 * W, by, v2 * W, bh));
			}
		}
		break;
	case XY_PT_WATERFALL: {
		const double steps[6] = { 0.35, 0.2, 0.15, -0.12, -0.2, 0.0 };
		double c = 0.0;
		p.setPen(Qt::NoPen);
		for (int i = 0; i < 6; ++i) {
			const double bx = x0 + 2 + i * (x1 - x0 - 2) / 6.0, bw = (x1 - x0) / 8.0, H = y1 - y0;
			const double lo = i == 5 ? 0.0 : std::min(c, c + steps[i]), hi = i == 5 ? c : std::max(c, c + steps[i]);
			p.setBrush(i == 5 ? QColor(120, 120, 120) : steps[i] >= 0 ? blue : orange);
			p.drawRect(QRectF(bx, y1 - hi * H, bw, (hi - lo) * H));
			c += steps[i];
		}
		break;
	}
	case XY_PT_HIST:
		p.setPen(QPen(Qt::white, 0.6));
		p.setBrush(blue);
		for (int i = 0; i < 9; ++i) {
			const double t = (i - 4) / 2.2, h = std::exp(-t * t / 2) * 0.88;
			const double bw = (x1 - x0) / 9.0;
			p.drawRect(QRectF(x0 + i * bw, y1 - h * (y1 - y0), bw, h * (y1 - y0)));
		}
		break;
	case XY_PT_DENSITY:
		p.setBrush(QColor(0, 114, 189, 70));
		p.setPen(QPen(blue, 1.4));
		{
			QPainterPath d;
			d.moveTo(x0, y1);
			for (int i = 0; i <= 60; ++i) {
				const double t = i / 60.0, z1 = (t - 0.35) / 0.12, z2 = (t - 0.7) / 0.1;
				d.lineTo(x0 + t * (x1 - x0), y1 - (0.8 * std::exp(-z1 * z1 / 2) + 0.5 * std::exp(-z2 * z2 / 2)) * (y1 - y0));
			}
			d.lineTo(x1, y1);
			p.drawPath(d);
		}
		break;
	case XY_PT_BOXPLOT:
		for (int i = 0; i < 3; ++i) {
			const double cx = x0 + (i + 0.5) * (x1 - x0) / 3.0, H = y1 - y0;
			const double q0 = 0.1 + 0.05 * i, q1 = 0.3 + 0.05 * i, q2 = 0.45 + 0.08 * i, q3 = 0.6 + 0.06 * i, q4 = 0.85;
			p.setPen(QPen(i == 1 ? orange : blue, 1.1));
			p.setBrush(Qt::NoBrush);
			p.drawLine(QPointF(cx, y1 - q0 * H), QPointF(cx, y1 - q1 * H));
			p.drawLine(QPointF(cx, y1 - q3 * H), QPointF(cx, y1 - q4 * H));
			p.drawLine(QPointF(cx - 2.5, y1 - q0 * H), QPointF(cx + 2.5, y1 - q0 * H));
			p.drawLine(QPointF(cx - 2.5, y1 - q4 * H), QPointF(cx + 2.5, y1 - q4 * H));
			p.setBrush(QColor(i == 1 ? 217 : 0, i == 1 ? 83 : 114, i == 1 ? 25 : 189, 70));
			p.drawRect(QRectF(cx - 5, y1 - q3 * H, 10, (q3 - q1) * H));
			p.drawLine(QPointF(cx - 5, y1 - q2 * H), QPointF(cx + 5, y1 - q2 * H));
		}
		break;
	case XY_PT_VIOLIN:
		for (int i = 0; i < 3; ++i) {
			const double cx = x0 + (i + 0.5) * (x1 - x0) / 3.0;
			QPainterPath v;
			for (int side = 0; side < 2; ++side)
				for (int j = 0; j <= 30; ++j) {
					const double t = side == 0 ? j / 30.0 : 1.0 - j / 30.0;
					const double z = (t - 0.45 - 0.05 * i) / 0.18;
					const double w = 6.5 * (std::exp(-z * z / 2) + 0.25 * std::exp(-(t - 0.85) * (t - 0.85) / 0.005)) + 0.3;
					const QPointF q(cx + (side == 0 ? w : -w), y1 - t * (y1 - y0));
					if (side == 0 && j == 0) v.moveTo(q); else v.lineTo(q);
				}
			v.closeSubpath();
			p.setPen(QPen(i == 1 ? orange : blue, 1.0));
			p.setBrush(QColor(i == 1 ? 217 : 0, i == 1 ? 83 : 114, i == 1 ? 25 : 189, 90));
			p.drawPath(v);
		}
		break;
	case XY_PT_ECDF: {
		QPainterPath path;
		const int ns = 14;
		double c = 0.0;
		path.moveTo(x0, y1);
		for (int i = 0; i < ns; ++i) {
			const double x = x0 + (i + 0.5) * (x1 - x0) / ns;
			const double t = (i - ns / 2.0) / 3.0;
			const double nc = 1.0 / (1.0 + std::exp(-1.7 * t));
			path.lineTo(x, y1 - c * (y1 - y0));
			c = nc;
			path.lineTo(x, y1 - c * (y1 - y0));
		}
		path.lineTo(x1, y1 - c * (y1 - y0));
		p.setPen(QPen(blue, 1.4));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
		break;
	}
	case XY_PT_QQNORM:
	case XY_PT_QQPLOT:
		p.setPen(QPen(type == XY_PT_QQNORM ? orange : QColor(80, 80, 80), 1.2, Qt::DashLine));
		p.drawLine(QPointF(x0 + 2, y1 - 2), QPointF(x1 - 2, y0 + 2));
		p.setPen(QPen(blue, 1.0));
		p.setBrush(QColor(0, 114, 189));
		for (int i = 0; i < 14; ++i) {
			const double t = (i + 0.5) / 14.0, d = 0.08 * std::sin(i * 1.3) + (type == XY_PT_QQPLOT ? 0.1 * (t - 0.5) * (t - 0.5) * 4 : 0.0);
			p.drawEllipse(QPointF(x0 + 2 + t * (x1 - x0 - 4), y1 - 2 - std::clamp(t + d, 0.0, 1.0) * (y1 - y0 - 4)), 1.4, 1.4);
		}
		break;
	case XY_PT_RADAR: {
		const QPointF c((x0 + x1) / 2, (y0 + y1) / 2);
		const double R = (y1 - y0) / 2;
		const double v[5] = { 0.9, 0.55, 0.8, 0.4, 0.7 };
		p.setPen(QPen(grid, 0.8));
		p.setBrush(Qt::NoBrush);
		QPolygonF poly, ring;
		for (int k = 0; k < 5; ++k) {
			const double a = M_PI / 2 + k * 2 * M_PI / 5;
			const QPointF e(c.x() + R * std::cos(a), c.y() - R * std::sin(a));
			p.drawLine(c, e);
			ring << e;
			poly << QPointF(c.x() + v[k] * R * std::cos(a), c.y() - v[k] * R * std::sin(a));
		}
		p.drawPolygon(ring);
		p.setPen(QPen(blue, 1.2));
		p.setBrush(QColor(0, 114, 189, 90));
		p.drawPolygon(poly);
		break;
	}
	case XY_PT_ROSE: {
		const QPointF c((x0 + x1) / 2, (y0 + y1) / 2);
		const double R = (y1 - y0) / 2;
		p.setPen(QPen(Qt::white, 0.5));
		p.setBrush(blue);
		for (int k = 0; k < 12; ++k) {
			const double r = R * (0.35 + 0.65 * std::abs(std::sin(k * 0.9 + 0.4)));
			const QRectF box(c.x() - r, c.y() - r, 2 * r, 2 * r);
			p.drawPie(box, k * 30 * 16, 30 * 16);
		}
		break;
	}
	case XY_PT_ERRORBAR: {
		QPolygonF l;
		for (int i = 0; i < 7; ++i) {
			const double x = x0 + 3 + i * (x1 - x0 - 6) / 6.0;
			const double y = (y0 + y1) / 2 - 0.25 * (y1 - y0) * std::sin(i * 0.9);
			const double e = (0.08 + 0.05 * (i % 3)) * (y1 - y0);
			p.setPen(QPen(orange, 1.0));
			p.drawLine(QPointF(x, y - e), QPointF(x, y + e));
			p.drawLine(QPointF(x - 2, y - e), QPointF(x + 2, y - e));
			p.drawLine(QPointF(x - 2, y + e), QPointF(x + 2, y + e));
			l << QPointF(x, y);
		}
		p.setPen(QPen(blue, 1.4));
		p.drawPolyline(l);
		break;
	}
	case XY_PT_BUBBLE: {
		const double v[7][3] = { {0.12, 0.7, 3}, {0.28, 0.35, 6}, {0.42, 0.62, 4}, {0.55, 0.25, 8},
		                         {0.68, 0.55, 5}, {0.82, 0.3, 9}, {0.9, 0.75, 3.5} };
		p.setPen(QPen(blue, 0.8));
		p.setBrush(QColor(0, 114, 189, 110));
		for (const auto &b : v)
			p.drawEllipse(QPointF(x0 + b[0] * (x1 - x0), y0 + b[1] * (y1 - y0)), b[2], b[2]);
		break;
	}
	case XY_PT_FEATHER: {
		p.setPen(QPen(QColor(150, 150, 150), 0.8));
		const double yb = (y0 + y1) / 2 + 4;
		p.drawLine(QPointF(x0, yb), QPointF(x1, yb));
		p.setPen(QPen(blue, 1.2));
		for (int i = 0; i < 9; ++i) {
			const double x = x0 + 3 + i * (x1 - x0 - 8) / 8.0;
			const double a = 0.4 + i * 0.35, L = 0.32 * (y1 - y0);
			const QPointF tip(x + L * std::cos(a) * 0.6, yb - L * std::sin(a));
			p.drawLine(QPointF(x, yb), tip);
		}
		break;
	}
	case XY_PT_POLARPLOT:
	case XY_PT_POLARSCATTER:
	case XY_PT_COMPASS: {
		const QPointF c((x0 + x1) / 2, (y0 + y1) / 2);
		const double R = (y1 - y0) / 2;
		p.setPen(QPen(grid, 0.8));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(c, R, R);
		p.drawEllipse(c, R / 2, R / 2);
		for (int k = 0; k < 4; ++k) {
			const double a = k * M_PI / 4;
			p.drawLine(QPointF(c.x() - R * std::cos(a), c.y() + R * std::sin(a)),
			           QPointF(c.x() + R * std::cos(a), c.y() - R * std::sin(a)));
		}
		const double pts[6][2] = { {0.5, 0.8}, {1.3, 0.55}, {2.2, 0.9}, {3.4, 0.4}, {4.3, 0.7}, {5.4, 0.6} };
		if (type == XY_PT_POLARPLOT) {                // a closed rose curve, r = |cos 2θ|
			QPolygonF l;
			for (int i = 0; i <= 120; ++i) {
				const double a = i * 2 * M_PI / 120, r = 0.9 * R * std::abs(std::cos(2 * a));
				l << QPointF(c.x() + r * std::cos(a), c.y() - r * std::sin(a));
			}
			p.setPen(QPen(blue, 1.3));
			p.setBrush(Qt::NoBrush);
			p.drawPolyline(l);
			break;
		}
		for (const auto &q : pts) {
			const QPointF e(c.x() + q[1] * R * std::cos(q[0]), c.y() - q[1] * R * std::sin(q[0]));
			if (type == XY_PT_COMPASS) {
				p.setPen(QPen(blue, 1.2));
				p.drawLine(c, e);
				const double a = std::atan2(c.y() - e.y(), e.x() - c.x());   // arrow head: two short barbs
				for (double d : { 2.6, -2.6 })
					p.drawLine(e, QPointF(e.x() - 3.5 * std::cos(a + d / 6), e.y() + 3.5 * std::sin(a + d / 6)));
			}
			else {
				p.setPen(QPen(blue, 0.8));
				p.setBrush(QColor(0, 114, 189, 140));
				p.drawEllipse(e, 2.0, 2.0);
			}
		}
		break;
	}
	case XY_PT_PIE:
	case XY_PT_DONUT: {
		const QPointF c((x0 + x1) / 2, (y0 + y1) / 2);
		const double R = (y1 - y0) / 2;
		const QRectF box(c.x() - R, c.y() - R, 2 * R, 2 * R);
		const int span[4] = { 140, 90, 75, 55 };
		const QColor col[4] = { blue, orange, QColor(237, 177, 32), QColor(126, 47, 142) };
		p.setPen(QPen(Qt::white, 0.6));
		int a0 = 90 * 16;
		for (int k = 0; k < 4; ++k) {
			p.setBrush(col[k]);
			p.drawPie(box, a0, -span[k] * 16);
			a0 -= span[k] * 16;
		}
		if (type == XY_PT_DONUT) {
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::white);
			p.drawEllipse(c, R / 2, R / 2);
		}
		break;
	}
	case XY_PT_PARALLEL:
		p.setPen(QPen(QColor(90, 90, 90), 1.0));
		for (int k = 0; k < 4; ++k) {
			const double x = x0 + 2 + k * (x1 - x0 - 4) / 3.0;
			p.drawLine(QPointF(x, y0), QPointF(x, y1));
		}
		for (int i = 0; i < 6; ++i) {
			QPolygonF l;
			for (int k = 0; k < 4; ++k)
				l << QPointF(x0 + 2 + k * (x1 - x0 - 4) / 3.0,
				             y0 + (0.1 + 0.8 * std::fmod(0.17 * i + 0.31 * k * (i % 2 ? 1 : 2) + 0.05, 1.0)) * (y1 - y0));
			p.setPen(QPen(i < 3 ? blue : orange, 1.0));
			p.drawPolyline(l);
		}
		break;
	case XY_PT_CORNER:
	case XY_PT_MARGINAL: {
		auto cell = [&](double cx, double cy, double cw, double ch) {
			return QRectF(x0 + cx * (x1 - x0), y0 + cy * (y1 - y0), cw * (x1 - x0), ch * (y1 - y0));
		};
		auto hist = [&](const QRectF &r, bool horiz) {
			p.setPen(Qt::NoPen);
			p.setBrush(blue);
			for (int i = 0; i < 6; ++i) {
				const double t = (i - 2.5) / 1.6, h = std::exp(-t * t / 2) * 0.9;
				if (!horiz) p.drawRect(QRectF(r.left() + i * r.width() / 6, r.bottom() - h * r.height(), r.width() / 6, h * r.height()));
				else        p.drawRect(QRectF(r.left(), r.top() + i * r.height() / 6, h * r.width(), r.height() / 6));
			}
		};
		auto dots = [&](const QRectF &r) {
			p.setPen(Qt::NoPen);
			p.setBrush(blue);
			for (int i = 0; i < 14; ++i) {
				const double t = std::fmod(i * 0.618034, 1.0), u = std::clamp(t + 0.25 * std::sin(i * 2.1), 0.05, 0.95);
				p.drawEllipse(QPointF(r.left() + t * r.width(), r.bottom() - u * r.height()), 1.2, 1.2);
			}
		};
		if (type == XY_PT_CORNER) {
			hist(cell(0.0, 0.0, 0.32, 0.31), false);
			dots(cell(0.0, 0.34, 0.32, 0.31)); hist(cell(0.34, 0.34, 0.32, 0.31), false);
			dots(cell(0.0, 0.68, 0.32, 0.31)); dots(cell(0.34, 0.68, 0.32, 0.31)); hist(cell(0.68, 0.68, 0.32, 0.31), false);
		}
		else {
			hist(cell(0.0, 0.0, 0.75, 0.22), false);
			dots(cell(0.0, 0.26, 0.75, 0.74));
			hist(cell(0.79, 0.26, 0.21, 0.74), true);
		}
		break;
	}
	case XY_PT_BIPLOT: {
		const QPointF c((x0 + x1) / 2, (y0 + y1) / 2);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(120, 120, 120));
		for (int i = 0; i < 18; ++i) {
			const double t = std::fmod(i * 0.618034, 1.0) - 0.5, u = std::fmod(i * 0.414214, 1.0) - 0.5;
			p.drawEllipse(QPointF(c.x() + t * (x1 - x0) * 0.8, c.y() + (0.6 * u + 0.4 * t) * (y1 - y0) * 0.8), 1.1, 1.1);
		}
		p.setPen(QPen(blue, 1.4));
		const double ends[3][2] = { { 0.4, -0.3 }, { -0.25, -0.4 }, { 0.3, 0.35 } };
		for (const auto &e : ends) {
			const QPointF q(c.x() + e[0] * (x1 - x0), c.y() + e[1] * (y1 - y0));
			p.drawLine(c, q);
			p.setBrush(blue);
			p.drawEllipse(q, 1.5, 1.5);
		}
		break;
	}
	}
	p.setPen(QPen(QColor(90, 90, 90), 1.0));
	p.setBrush(Qt::NoBrush);
	p.drawRect(fr);
	p.end();
	return QIcon(pm);
}

// Two fixes to the PostScript GL2PS writes:
//  - it declares no page size, so a viewer or printer puts the chart on its default paper and cuts it
//    off; the page is set to the chart's own %%BoundingBox.
//  - on VTK's OpenGL2 backend it always writes the window background as a full-window raster
//    (vtkGL2PSExporter.h: "On the OpenGL2 backend, the background is always drawn") — 10 MB of hex for
//    a 2036x819 chart that is white everywhere. When that raster is ONE colour it is replaced by a
//    filled rectangle of that colour; anything else in it is left exactly as written.
static void xyPsFixUp(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) return;
	QByteArray ps = f.readAll();
	f.close();
	const int a = ps.indexOf("/rgbstr ");
	const int c = a < 0 ? -1 : ps.indexOf("colorimage", a);
	const int d = c < 0 ? -1 : ps.indexOf('\n', c) + 1;         // the first line of hex
	const int e = d <= 0 ? -1 : ps.indexOf("grestore", d);        // the end of the image's block
	bool uniform = e > 0;
	QByteArray hex = uniform ? ps.mid(d, e - d) : QByteArray();
	hex.replace('\n', QByteArray());
	if (hex.size() < 6 || hex.size() % 6) uniform = false;
	for (int i = 0; uniform && i < hex.size(); ++i)
		if (!std::isxdigit((unsigned char)hex[i])) uniform = false;
	for (int i = 6; uniform && i < hex.size(); i += 6)
		if (memcmp(hex.constData() + i, hex.constData(), 6) != 0) uniform = false;
	if (uniform) {
		const double r = hex.mid(0, 2).toInt(nullptr, 16) / 255.0, g = hex.mid(2, 2).toInt(nullptr, 16) / 255.0,
		             b = hex.mid(4, 2).toInt(nullptr, 16) / 255.0;
		// the image was drawn into the unit square the preceding `scale` maps onto the page
		ps.replace(a, e - a, QString("%1 %2 %3 setrgbcolor 0 0 1 1 rectfill\n").arg(r).arg(g).arg(b).toUtf8());
	}
	const QRegularExpressionMatch bb = QRegularExpression("%%BoundingBox: (\\d+) (\\d+) (\\d+) (\\d+)")
	                                       .match(QString::fromLatin1(ps.left(4096)));
	if (bb.hasMatch() && ps.indexOf("setpagedevice") < 0) {
		const QString page = QString("<< /PageSize [%1 %2] >> setpagedevice\n")
		                         .arg(bb.captured(3).toInt() - bb.captured(1).toInt())
		                         .arg(bb.captured(4).toInt() - bb.captured(2).toInt());
		const int bs = ps.indexOf("%%BeginSetup\n"), ep = ps.indexOf("%%EndProlog\n");
		if (bs >= 0)                                  // into GL2PS's own setup section
			ps.insert(bs + 13, page.toLatin1());
		else if (ep >= 0)
			ps.insert(ep + 12, ("%%BeginSetup\n" + page + "%%EndSetup\n").toLatin1());
	}
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(ps);
		f.close();
	}
}

// VTK's vector devices (PDF, SVG) stroke a line INSIDE the chart's data transform, whose x and y scales
// differ, so its width depended on its slope (feather sticks: hairline when steep, fat when flat). Here
// the points are taken to page coordinates first and stroked under an identity transform: one width,
// any slope. Where the transform scales x and y alike VTK's own stroke is right and is left alone.
template <class Base>
class XYFlatStroke : public Base {
public:
	void DrawPoly(float *pts, int n, unsigned char *c = nullptr, int nc = 0) override {
		std::vector<float> q;
		if (!toPage(pts, n, q)) { Base::DrawPoly(pts, n, c, nc); return; }
		Base::DrawPoly(q.data(), n, c, nc);
		this->PopMatrix();
	}
	void DrawLines(float *pts, int n, unsigned char *c = nullptr, int nc = 0) override {
		std::vector<float> q;
		if (!toPage(pts, n, q)) { Base::DrawLines(pts, n, c, nc); return; }
		Base::DrawLines(q.data(), n, c, nc);
		this->PopMatrix();
	}
private:
	bool toPage(const float *pts, int n, std::vector<float> &q) {
		vtkNew<vtkMatrix3x3> m;
		this->GetMatrix(m);
		const double *a = m->GetData();
		const double sx = std::hypot(a[0], a[3]), sy = std::hypot(a[1], a[4]);
		if (std::abs(sx - sy) <= 1e-6 * std::max(sx, sy)) return false;
		q.resize(2 * (size_t)n);
		for (int i = 0; i < n; ++i) {
			const double x = pts[2 * i], y = pts[2 * i + 1];
			q[2 * i]     = (float)(a[0] * x + a[1] * y + a[2]);
			q[2 * i + 1] = (float)(a[3] * x + a[4] * y + a[5]);
		}
		this->PushMatrix();
		vtkNew<vtkMatrix3x3> id;
		this->SetMatrix(id);
		return true;
	}
};

// The PDF device also sizes text in points with no DPI, while the screen draws it at size * DPI / 72,
// so every label came out a fraction of its on-screen size. `fontScale` is that DPI / 72. (The SVG
// device already applies the window's DPI.)
class XYPdfDevice : public XYFlatStroke<vtkPDFContextDevice2D> {
public:
	static XYPdfDevice *New();
	double fontScale = 1.0;
	void ApplyTextProp(vtkTextProperty *prop) override {
		vtkPDFContextDevice2D::ApplyTextProp(prop);
		if (prop && this->TextProp)
			this->TextProp->SetFontSize((int)std::lround(prop->GetFontSize() * fontScale));
	}
};
vtkStandardNewMacro(XYPdfDevice);

class XYSvgDevice : public XYFlatStroke<vtkSVGContextDevice2D> {
public:
	static XYSvgDevice *New();
};
vtkStandardNewMacro(XYSvgDevice);

// vtkSVGExporter with XYSvgDevice in place of its device: its own WriteSVG sequence otherwise.
class XYSvgExporter : public vtkSVGExporter {
public:
	static XYSvgExporter *New();
protected:
	void WriteData() override {
		if (!this->FileName || !*this->FileName || !this->RenderWindow) return;
		this->PrepareDocument();
		this->Device->Delete();
		this->Device = XYSvgDevice::New();
		this->Device->SetSVGContext(this->PageNode, this->DefinitionNode);
		this->Device->SetTextAsPath(this->TextAsPath);
		this->Device->SetSubdivisionThreshold(this->SubdivisionThreshold);
		this->RenderContextActors();
		if (this->DefinitionNode->GetNumberOfNestedElements() == 0) {
			this->RootNode->RemoveNestedElement(this->DefinitionNode);
			this->DefinitionNode = nullptr;
		}
		else
			this->Device->GenerateDefinitions();
		this->RootNode->PrintXML(this->FileName);
		this->Device->Delete();
		this->Device = nullptr;
		this->RootNode->Delete();
		this->RootNode = nullptr;
		this->DefinitionNode = nullptr;
		this->PageNode = nullptr;
	}
};
vtkStandardNewMacro(XYSvgExporter);

// The PDF writer: vtkPDFExporter's own sequence (one page the window's size, every context actor painted
// into it), with XYPdfDevice in place of its device. "" when written, else why not.
static void xyHaruError(HPDF_STATUS e, HPDF_STATUS d, void *) {
	throw std::runtime_error(QString("libharu error 0x%1 (detail %2)").arg(e, 0, 16).arg(d).toStdString());
}
static QString xyWritePdf(vtkRenderWindow *rw, const std::string &fn) {
	HPDF_Doc doc = HPDF_New(xyHaruError, nullptr);
	if (!doc) return "Could not start the PDF document.";
	QString err;
	try {
		HPDF_SetCompressionMode(doc, HPDF_COMP_ALL);
		HPDF_SetInfoAttr(doc, HPDF_INFO_CREATOR, "iGMT X,Y plot");
		HPDF_Page page = HPDF_AddPage(doc);
		HPDF_Page_SetWidth(page, (HPDF_REAL)rw->GetSize()[0]);
		HPDF_Page_SetHeight(page, (HPDF_REAL)rw->GetSize()[1]);
		const double fontScale = rw->GetDPI() > 0 ? rw->GetDPI() / 72.0 : 1.0;
		vtkRendererCollection *rc = rw->GetRenderers();
		for (int layer = 0; layer < rw->GetNumberOfLayers(); ++layer) {
			vtkCollectionSimpleIterator ri;
			vtkRenderer *ren;
			for (rc->InitTraversal(ri); (ren = rc->GetNextRenderer(ri));) {
				if (ren->GetLayer() != layer) continue;
				vtkCollectionSimpleIterator pi;
				vtkProp *prop;
				vtkPropCollection *props = ren->GetViewProps();
				for (props->InitTraversal(pi); (prop = props->GetNextProp(pi));) {
					vtkContextActor *actor = vtkContextActor::SafeDownCast(prop);
					if (!actor) continue;
					vtkContextDevice2D *old = actor->GetForceDevice();
					vtkNew<XYPdfDevice> dev;
					dev->fontScale = fontScale;
					dev->SetHaruObjects(&doc, &page);
					dev->SetRenderer(ren);
					actor->SetForceDevice(dev);
					actor->RenderOverlay(ren);
					actor->SetForceDevice(old);
				}
			}
		}
		HPDF_SaveToFile(doc, fn.c_str());
	}
	catch (const std::exception &e) {
		err = QString::fromStdString(e.what());
	}
	HPDF_Free(doc);
	return err;
}

// File > Export figure: the current page as drawn, in the format the file's extension names. PNG and
// JPEG are the window's pixels; PDF, SVG and PostScript are VECTOR — the chart's own lines, markers
// and text, written by VTK's exporters from the same 2-D scene the window paints. Returns "" when the
// file was written, else why not.
static QString xyExportFigure(XYPlot *s, const QString &path) {
	if (!xyAlive(s) || !s->widget || !s->widget->renderWindow())
		return "The X,Y window is gone.";
	vtkRenderWindow *rw = s->widget->renderWindow();
	const QString ext = QFileInfo(path).suffix().toLower();
	const std::string fn = path.toUtf8().constData();
	rw->Render();
	if (ext == "png" || ext == "jpg" || ext == "jpeg") {
		vtkNew<vtkWindowToImageFilter> w2i;
		w2i->SetInput(rw);
		w2i->ReadFrontBufferOff();
		w2i->Update();
		vtkSmartPointer<vtkImageWriter> wr;
		if (ext == "png") wr = vtkSmartPointer<vtkPNGWriter>::New();
		else {
			auto j = vtkSmartPointer<vtkJPEGWriter>::New();
			j->SetQuality(95);
			wr = j;
		}
		wr->SetFileName(fn.c_str());
		wr->SetInputConnection(w2i->GetOutputPort());
		wr->Write();
	}
	else if (ext == "pdf") {
		const QString err = xyWritePdf(rw, fn);
		if (!err.isEmpty()) return err;
	}
	else if (ext == "svg") {
		vtkNew<XYSvgExporter> ex;
		ex->SetRenderWindow(rw);
		ex->SetFileName(fn.c_str());
		ex->Write();
	}
	else if (ext == "ps") {
		vtkNew<vtkGL2PSExporter> ex;
		ex->SetRenderWindow(rw);
		ex->SetFileFormatToPS();
		ex->CompressOff();
		ex->SetSortToOff();                           // a flat chart: nothing to depth-sort
		// GL2PS appends the extension itself, so it is handed the name without it.
		const QString prefix = path.left(path.size() - ext.size() - 1);
		ex->SetFilePrefix(prefix.toUtf8().constData());
		ex->Write();
		rw->Render();                                 // GL2PS renders through its own feedback pass
		xyPsFixUp(path);
	}
	else
		return QString("Unknown figure format '.%1': use .pdf, .ps, .svg, .png or .jpg").arg(ext);
	const QFileInfo fi(path);
	if (!fi.exists() || fi.size() == 0)
		return "VTK did not write " + path;
	return QString();
}

static XYPlot *buildXYPlot(const char *title) {
	ensureApp();

	XYPlot *s = new XYPlot();
	s->owner = xyDefaultOwner();
	s->win = new QMainWindow();
	s->win->setAttribute(Qt::WA_DeleteOnClose, true);
	s->win->setWindowTitle(title && title[0] ? QString::fromUtf8(title) : QString("i'GMT  —  X,Y plot"));
	s->win->setWindowIcon(appIcon());
	s->win->resize(1060, 560);                         // landscape — better for time-series plots

	// --- central VTK view (one shared scene; the current page's chart is mounted into it) ---
	s->widget = new QVTKOpenGLNativeWidget();
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	s->widget->setRenderWindow(rw.Get());
	s->view = vtkSmartPointer<vtkContextView>::New();
	s->view->SetRenderWindow(rw.Get());
	s->view->SetInteractor(s->widget->interactor());
	s->view->GetRenderer()->SetBackground(1.0, 1.0, 1.0);

	// --- collapsible "Panels" fold under the chart + a TWO-TAB body, IDENTICAL in name & structure to
	// the 3-D viewer (a "Panels" fold over "Julia Console" / "Errors" tabs). Starts COLLAPSED. The body
	// holds the same two consoles as the viewer:
	//   • "Julia Console" — interactive REPL: a typed command goes back to Julia (g_juliaEval) and is
	//                eval'd in the SHARED Main (same session as the 3-D viewer). A command's own error
	//                shows inline HERE; genuine execution errors land in the Errors tab.
	//   • "Errors" — read-only sink for execution errors (xyLog: failed Analysis / File / callbacks).
	//                Auto-pops open + raises on an error so a failure can't stay hidden. ---
	s->console = new QPlainTextEdit();                  // ERRORS tab body
	s->console->setReadOnly(true);
	s->console->setMaximumBlockCount(2000);            // cap memory; oldest lines drop
	s->console->setFont(QFont("Consolas", 9));
	s->console->setPlaceholderText("Execution errors (Analysis / File / callbacks) appear here.");

	// JULIA tab — the same ReplConsole as the 3-D viewer (30_app.cpp): prompt, input and output in one
	// pane. A command's own error stays inline here.
	s->cmdConsole = new ReplConsole(s, "`fig` is this window — shared session with the 3-D viewer.", 9);
	QWidget *juliaTab = s->cmdConsole;

	QTabWidget *consoleTabs = new QTabWidget();        // the collapsible body: Julia Console | Errors (as in iGMT)
	consoleTabs->setDocumentMode(true);
	consoleTabs->setMaximumHeight(150);
	consoleTabs->addTab(juliaTab, "Julia Console");
	consoleTabs->addTab(s->console, "Errors");
	consoleTabs->setVisible(false);                    // hidden => panel collapsed
	// xyLog appends to s->console; when it raises an error it pops the panel open (below) — make the
	// Errors tab the front one so the new line is the one the user sees.
	s->xyErrTab = consoleTabs;

	s->consoleToggle = new QToolButton();
	s->consoleToggle->setText("Panels");
	s->consoleToggle->setCheckable(true);
	s->consoleToggle->setChecked(false);
	s->consoleToggle->setAutoRaise(true);
	s->consoleToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	s->consoleToggle->setArrowType(Qt::RightArrow);    // ▸ collapsed / ▾ expanded
	QObject::connect(s->consoleToggle, &QToolButton::toggled, s->win, [s, consoleTabs](bool on){
		consoleTabs->setVisible(on);
		s->consoleToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
		if (s->actConsole && s->actConsole->isChecked() != on) s->actConsole->setChecked(on);
	});

	s->consolePanel = new QWidget();
	QVBoxLayout *cpLayout = new QVBoxLayout(s->consolePanel);
	cpLayout->setContentsMargins(0, 0, 0, 0);
	cpLayout->setSpacing(0);
	cpLayout->addWidget(s->consoleToggle);
	cpLayout->addWidget(consoleTabs);

	// --- page tab bar (Excel-like): sits between the chart and the Console. A "+" button adds a
	// page; right-click a tab for New/Duplicate/Rename/Delete. NO per-tab close box (too easy to kill
	// a page by accident) — Delete lives only in the right-click menu. ---
	s->tabs = new QTabBar();
	s->tabs->setShape(QTabBar::RoundedSouth);
	s->tabs->setExpanding(false);
	s->tabs->setTabsClosable(false);
	s->tabs->setDrawBase(false);
	s->tabs->setContextMenuPolicy(Qt::CustomContextMenu);
	QToolButton *addPageBtn = new QToolButton();
	addPageBtn->setText("+");
	addPageBtn->setAutoRaise(true);
	addPageBtn->setToolTip("New page");
	QWidget *tabRow = new QWidget();
	QHBoxLayout *tabLayout = new QHBoxLayout(tabRow);
	tabLayout->setContentsMargins(2, 0, 0, 0);
	tabLayout->setSpacing(2);
	tabLayout->addWidget(s->tabs, 0);
	tabLayout->addWidget(addPageBtn, 0);
	tabLayout->addStretch(1);
	QObject::connect(addPageBtn, &QToolButton::clicked, s->win, [s]{ xyNewPage(s, "", true); });
	QObject::connect(s->tabs, &QTabBar::currentChanged, s->win, [s](int idx){
		if (s->tabBusy || idx < 0 || idx == s->curPageIdx) return;
		xySwitchPage(s, idx);
	});
	QObject::connect(s->tabs, &QTabBar::tabBarDoubleClicked, s->win, [s](int idx){
		if (idx < 0) return;
		bool ok = false;
		const QString nn = QInputDialog::getText(s->win, "Rename page", "Name:",
			QLineEdit::Normal, QString::fromStdString(s->pages[idx].name), &ok);
		if (ok) xyRenamePage(s, idx, nn);
	});
	QObject::connect(s->tabs, &QWidget::customContextMenuRequested, s->win, [s](const QPoint &p){ xyTabMenu(s, p); });

	// Optional header strip above the chart (rich text, e.g. the Tide tool's Next High/Now/Next Low
	// lines) -- empty/hidden by default, set via gmtvtk_xyplot_set_info.
	s->infoLabel = new QLabel();
	s->infoLabel->setTextFormat(Qt::RichText);
	s->infoLabel->setAlignment(Qt::AlignHCenter);
	xyApplyInfoStyle(s);
	s->infoLabel->setVisible(false);

	QWidget *central = new QWidget();
	QVBoxLayout *centralLayout = new QVBoxLayout(central);
	centralLayout->setContentsMargins(0, 0, 0, 0);
	centralLayout->setSpacing(0);
	centralLayout->addWidget(s->infoLabel, 0);          // header strip, hugs the top
	s->magBar = new XYMagBarStrip(s);                  // geomagnetic bar code, over the chart (hidden until made)
	s->magBar->setVisible(false);
	centralLayout->addWidget(s->magBar, 0);
	centralLayout->addWidget(s->widget, 1);            // chart takes all the stretch
	s->magCtl = xyMagBuildCtl(s);                      // its controls, under the chart (toolbar toggle)
	centralLayout->addWidget(s->magCtl, 0);
	centralLayout->addWidget(tabRow, 0);               // page tabs just under the chart
	centralLayout->addWidget(s->consolePanel, 0);      // console hugs the bottom
	s->win->setCentralWidget(central);

	// --- Object Manager dock (left): movable only -- NEVER floating, NEVER closable (it can be
	// folded to a thin strip, never fully killed). Fold control wired up below via FoldTitleBar
	// (10_geometry.cpp), the SAME shared mechanism the 3-D viewer's Scene Objects dock uses, not a
	// reimplementation. Initial width capped to ~4 cm (resizeDocks call near the end of this
	// function), user-resizable afterwards.
	QDockWidget *omDock = new QDockWidget("Object Manager", s->win);
	omDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
	omDock->setFeatures(QDockWidget::DockWidgetMovable);
	s->objMgr = new QTreeWidget(omDock);
	s->objMgr->setHeaderLabels(QStringList() << "Series");
	s->objMgr->setRootIsDecorated(false);
	s->objMgr->setContextMenuPolicy(Qt::CustomContextMenu);
	// The panel: the series tree, and UNDER it the strip of parked tools (xyRebuildParkStrip).
	QWidget *omBody = new QWidget(omDock);
	{
		QVBoxLayout *col = new QVBoxLayout(omBody);
		col->setContentsMargins(0, 0, 0, 0);
		col->setSpacing(0);
		s->objMgr->setParent(omBody);
		col->addWidget(s->objMgr, 1);
		s->parkStrip = new QTreeWidget(omBody);
		s->parkStrip->setHeaderHidden(true);
		s->parkStrip->setRootIsDecorated(false);
		s->parkStrip->setIndentation(0);
		s->parkStrip->setSelectionMode(QAbstractItemView::NoSelection);
		s->parkStrip->setFocusPolicy(Qt::NoFocus);
		s->parkStrip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		s->parkStrip->setContextMenuPolicy(Qt::CustomContextMenu);
		s->parkStrip->setVisible(false);
		col->addWidget(s->parkStrip, 0);              // stretch 0: the tree keeps all the spare height
		// Every route out of a parked row is DEFERRED: un-parking rebuilds this very strip.
		auto later = [s](QTreeWidgetItem *it, bool wantMenu, QPoint g) {
			XYPlot::ParkedTool *t = xyParkedOfRow(s, it);
			if (!t) return;
			std::function<void()> un = t->unpark;
			std::function<void(const QPoint &)> menu = t->menu;
			QTimer::singleShot(0, s->win, [un, menu, wantMenu, g]() {
				if (wantMenu) { if (menu) menu(g); }
				else if (un) un();
			});
		};
		QObject::connect(s->parkStrip, &QTreeWidget::itemChanged, s->win, [later](QTreeWidgetItem *it, int) {
			if (it->checkState(0) == Qt::Checked) later(it, false, QPoint());   // ticking = bring it back
		});
		QObject::connect(s->parkStrip, &QTreeWidget::itemDoubleClicked, s->win,
		                 [later](QTreeWidgetItem *it, int) { later(it, false, QPoint()); });
		QObject::connect(s->parkStrip, &QWidget::customContextMenuRequested, s->win, [s, later](const QPoint &p) {
			later(s->parkStrip->itemAt(p), true, s->parkStrip->viewport()->mapToGlobal(p));
		});
	}
	omDock->setWidget(omBody);
	s->win->addDockWidget(Qt::LeftDockWidgetArea, omDock);
	// Never floats, but it can be dragged to the other edge: the shared memory (10_geometry.cpp)
	// carries its width across that move, same as every dock in the 3-D viewer.
	installDockGeometryMemory(s->win, omDock, "objectManagerDock");

	// --- menubar ---
	QMenuBar *mb = s->win->menuBar();
	QMenu *mFile = mb->addMenu("File");
	QAction *aNew  = mFile->addAction("New");
	QAction *aOpen = mFile->addAction("Open…");
	QAction *aSave = mFile->addAction("Save…");
	QAction *aExport = mFile->addAction("Export figure…");
	aExport->setObjectName("xyExportFigure");
	aExport->setToolTip("The current page as a figure: PDF, PostScript and SVG are vector, PNG and JPEG pixels");
	QObject::connect(aExport, &QAction::triggered, s->win, [s]{
		QString sel;
		QString fn = QFileDialog::getSaveFileName(s->win, "Export figure", prefStartDir("xyplot.pdf"),
			"PDF (*.pdf);;PostScript (*.ps);;SVG (*.svg);;PNG (*.png);;JPEG (*.jpg *.jpeg)", &sel);
		if (fn.isEmpty()) return;
		if (QFileInfo(fn).suffix().isEmpty())           // no extension typed: the chosen filter's
			fn += "." + sel.mid(sel.indexOf("*.") + 2).section(QRegularExpression("[ )]"), 0, 0);
		rememberStartDir(fn);
		QApplication::setOverrideCursor(Qt::WaitCursor);
		const QString err = xyExportFigure(s, fn);
		QApplication::restoreOverrideCursor();
		if (!err.isEmpty())
			QMessageBox::warning(s->win, "Export figure", err);
		else
			s->win->statusBar()->showMessage("Figure exported to " + fn, 5000); });
	// ecran's File > Session: the whole window (pages, series, styles, axes, magnetic modelling) in the
	// same .igmtz container the 3-D viewer's Save Session writes (xysession.jl).
	QMenu *mSes = mFile->addMenu("Session");
	QAction *aSesOpen = mSes->addAction("Open…");
	QAction *aSesSave = mSes->addAction("Save…");
	QObject::connect(aSesOpen, &QAction::triggered, s->win, [s]{
		const QString fn = QFileDialog::getOpenFileName(s->win, "Open X,Y session", prefStartDir(),
			"i'GMT session (*.igmtz);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		xyRequest(s, "session_open", -1, nullptr, true, fn); });   // the session loader shows its own progress
	QObject::connect(aSesSave, &QAction::triggered, s->win, [s]{
		QString fn = QFileDialog::getSaveFileName(s->win, "Save X,Y session", prefStartDir("xyplot.igmtz"),
			"i'GMT session (*.igmtz)");
		if (fn.isEmpty()) return;
		if (!fn.endsWith(".igmtz", Qt::CaseInsensitive)) fn += ".igmtz";
		rememberStartDir(fn);
		if (xyRequest(s, "session_save", -1, "Saving the X,Y session…", true, fn))
			s->win->statusBar()->showMessage("Session saved to " + fn, 5000); });
	mFile->addSeparator();
	// File > Plot with GMT.jl… — the main window's own item, the same editor (Run, Save…, Standalone).
	QAction *aScript = mFile->addAction("Plot with GMT.jl…");
	aScript->setObjectName("xyPlotWithGMT");
	QObject::connect(aScript, &QAction::triggered, s->win, [s]{ showScriptEditor(scriptHostXY(s)); });
	mFile->addSeparator();
	// Ctrl+V: the clipboard's numeric text becomes new series here. Shared paste action
	// (makePasteAction, 30_app.cpp) — same key, same text-entry hand-back, as the 3-D viewer's.
	mFile->addAction(makePasteAction(s->win, [s]() { xyPasteClipboard(s); }));
	mFile->addSeparator();
	QAction *aClose = mFile->addAction("Close");
	QObject::connect(aNew, &QAction::triggered, s->win, [s]{
		if (g_juliaXY) g_juliaXY(s, "new", -1, "");
		else s->win->statusBar()->showMessage("New: not wired yet", 3000); });
	QObject::connect(aOpen, &QAction::triggered, s->win, [s]{
		if (!g_juliaXY) { s->win->statusBar()->showMessage("Open: not wired yet", 3000); return; }
		const QString fn = QFileDialog::getOpenFileName(s->win, "Open data file", prefStartDir(),
			"Data files (*.dat *.txt *.csv *.xy *.gmt);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		g_juliaXY(s, "open", -1, fn.toUtf8().constData()); });
	QObject::connect(aSave, &QAction::triggered, s->win, [s]{
		if (!g_juliaXY) { s->win->statusBar()->showMessage("Save: not wired yet", 3000); return; }
		if (xyCur(s).series.empty()) { s->win->statusBar()->showMessage("Nothing to save", 3000); return; }
		const QString fn = QFileDialog::getSaveFileName(s->win, "Save series", prefStartDir("series.dat"),
			"Text (*.dat *.txt);;CSV (*.csv);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		g_juliaXY(s, "save", xyCurrentSel(s), fn.toUtf8().constData()); });
	QObject::connect(aClose,  &QAction::triggered, s->win, [s]{ s->win->close(); });

	// Analysis menu — each op runs in Julia on the Object-Manager-selected series.
	QMenu *mAna = mb->addMenu("Analysis");
	auto addAna = [&](const char *label, const char *op) {
		QAction *a = mAna->addAction(label);
		QObject::connect(a, &QAction::triggered, s->win, [s, op, label] {
			if (!g_juliaXYAna) { xyLog(s, "Analysis: not wired (rebuild the DLL + restart Julia)"); return; }
			const int sel = xyCurrentSel(s);
			if (sel < 0) { xyLog(s, "Select a series in the Object Manager first"); return; }
			xyLog(s, QString("Analysis: %1 on series #%2…").arg(label).arg(sel));
			g_juliaXYAna(s, op, sel);
		});
	};
	addAna("Remove Mean",  "remove_mean");
	addAna("Remove Trend", "remove_trend");
	mAna->addSeparator();
	addAna("FFT — Amplitude Spectrum",     "fft_amp");
	addAna("FFT — Power Spectrum Density", "fft_psd");
	addAna("Autocorrelation",              "autocorr");
	mAna->addSeparator();
	addAna("1st derivative", "deriv1");
	addAna("2nd derivative", "deriv2");
	mAna->addSeparator();
	// Parameterised ops (Phase 3): pop a dialog for the parameter, then hand an encoded op tag to
	// Julia. `gate` returns the selected series (-1 + a status hint if none / not wired).
	auto gate = [s]() -> int {
		if (!g_juliaXYAna) { xyLog(s, "Analysis: not wired (rebuild the DLL + restart Julia)"); return -1; }
		const int sel = xyCurrentSel(s);
		if (sel < 0) xyLog(s, "Select a series in the Object Manager first");
		return sel;
	};
	{
		QAction *a = mAna->addAction("Fit polynomial…");
		QObject::connect(a, &QAction::triggered, s->win, [s, gate] {
			const int sel = gate(); if (sel < 0) return;
			bool ok = false;
			const int deg = QInputDialog::getInt(s->win, "Fit polynomial", "Degree:", 3, 1, 15, 1, &ok);
			if (!ok) return;
			g_juliaXYAna(s, QString("fitpoly:%1").arg(deg).toUtf8().constData(), sel);
		});
	}
	{
		QAction *a = mAna->addAction("Smoothing (Savitzky-Golay)…");
		QObject::connect(a, &QAction::triggered, s->win, [s, gate] {
			const int sel = gate(); if (sel < 0) return;
			bool ok = false;
			const int w = QInputDialog::getInt(s->win, "Smoothing", "Window (points, odd):", 11, 3, 1001, 2, &ok);
			if (!ok) return;
			g_juliaXYAna(s, QString("savgol:%1").arg(w).toUtf8().constData(), sel);
		});
	}
	{
		QAction *a = mAna->addAction("Filter (Butterworth)…");
		QObject::connect(a, &QAction::triggered, s->win, [s, gate] {
			const int sel = gate(); if (sel < 0) return;
			QString type; double fc;
			if (!xyAskButter(s->win, type, fc)) return;
			g_juliaXYAna(s, QString("butter:%1:%2").arg(type).arg(fc, 0, 'g', 9).toUtf8().constData(), sel);
		});
	}
	{
		QAction *a = mAna->addAction("Filter outliers (despike)…");
		QObject::connect(a, &QAction::triggered, s->win, [s, gate] {
			const int sel = gate(); if (sel < 0) return;
			bool ok = false;
			const double k = QInputDialog::getDouble(s->win, "Despike",
				"Threshold (× robust Ïƒ of the residual):", 2.0, 0.5, 20.0, 2, &ok);
			if (!ok) return;
			g_juliaXYAna(s, QString("despike:%1").arg(k, 0, 'g', 6).toUtf8().constData(), sel);
		});
	}
	// (Histogram and Bar graph are the plot-type gallery's icons, not Analysis entries.)
	mAna->addSeparator();
	{
		// ecran's "Sound Velocity Profile" (Chen & Millero / UNESCO 1983) -> page "SVP diagram".
		QAction *a = mAna->addAction("Sound Velocity Profile…");
		QObject::connect(a, &QAction::triggered, s->win, [s] {
			if (xyCur(s).series.empty()) { xyLog(s, "Sound Velocity Profile: this page has no data"); return; }
			QString args;
			if (!xyAskSVP(s->win, s, args)) return;
			xyRequest(s, QString("svp:%1").arg(args), 0, "Computing the sound velocity profile...");
		});
	}
	{
		// ecran's "Reference me": pick points alternately on two curves, then stretch the selected one
		// so its points land on the other's (xyRefSetPicking / XYRefFilter).
		s->actRef = mAna->addAction("Reference a curve to another…");
		s->actRef->setObjectName("xyReference");
		s->actRef->setCheckable(true);
		s->actRef->setToolTip("Pick points alternately on the selected curve and on another one, then right-click "
		                      "a red marker -> Reference me: the selected curve is stretched, piece by piece, so "
		                      "each red point lands on the x of its green partner");
		QObject::connect(s->actRef, &QAction::toggled, s->win, [s](bool on) { xyRefSetPicking(s, on); });
	}
	{
		// Spector & Grant (interactive): on a (wavenumber, power) spectrum, LEFT-DRAG a band and the
		// slope of ln(power) gives the depth to the magnetic source ensemble — live fit line + readout.
		QAction *a = mAna->addAction("Depth to sources (Spector-Grant)");
		a->setObjectName("xySpectorGrant");
		a->setCheckable(true);
		QObject::connect(a, &QAction::toggled, s->win, [s, a](bool on) {
			XYPage &pg = xyCur(s);
			if (on) {
				const int sel = xyCurrentSel(s);
				if (sel < 0) { s->win->statusBar()->showMessage("Select the spectrum series first", 3500); a->setChecked(false); return; }
				double xf;
				if (!xyAskFreqUnit(s->win, xf)) { a->setChecked(false); return; }
				pg.sgSel = sel; pg.sgUnit = xf; pg.sgActive = true;
				xySGShowLines(pg, true);                     // the lines kept earlier come back (dynSlope_CB)
				if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
				s->win->statusBar()->showMessage("Spector-Grant: left-drag a frequency band on the spectrum; "
				                                 "drag a fit line's end to edit it, right-click it for its menu");
			}
			else {
				pg.sgActive = false; pg.sgDragging = false;
				xySGClear(s);
				s->win->statusBar()->clearMessage();
			}
		});
	}

	{
		// ecran's isocs tools: the geomagnetic bar code strip over the chart and its controls under it
		// (ages, synthetic RTP anomaly, isochron fit). A toggle: checked shows them, unchecked hides them.
		s->actMag = mAna->addAction(xyMagIcon(), "Synthetic geomag anomalies");
		s->actMag->setObjectName("xyMagTool");
		s->actMag->setCheckable(true);
		s->actMag->setToolTip("Enter ages & plot a geomagnetic bar code, its synthetic anomaly and the isochron fit");
		QObject::connect(s->actMag, &QAction::toggled, s->win, [s](bool) { xyMagSyncUi(s); xyMagRender(s); });
	}

	QMenu *mMisc = mb->addMenu("Misc");
	// Misc > GMT — GUI front-ends for GMT modules that operate on table data. Each item pops an options
	// dialog, then hands an encoded op string to Julia (through the same Analysis callback) which runs
	// the module via GMT.jl and lands the result on its own page(s).
	QMenu *mGMT = mMisc->addMenu("GMT");
	{
		QAction *a = mGMT->addAction("spectrum1d…");
		QObject::connect(a, &QAction::triggered, s->win, [s, gate] {
			const int sel = gate(); if (sel < 0) return;
			QString args;
			if (!xyAskSpectrum1d(s->win, s, sel, args)) return;
			g_juliaXYAna(s, QString("spectrum1d:%1").arg(args).toUtf8().constData(), sel);
		});
	}

	// Grid, Panels, Log X / Log Y are no longer menu items (the tool bar's grid buttons, the Panels fold,
	// the Axes properties and logscale! do those jobs); their actions stay, unlisted, as the state the
	// window keeps in step (xyGridSyncUi, xySyncMiscMenu, a session's "grid").
	s->actGrid = new QAction("Grid", s->win);
	s->actGrid->setCheckable(true); s->actGrid->setChecked(true);
	s->actConsole = new QAction("Panels", s->win);
	s->actConsole->setCheckable(true); s->actConsole->setChecked(false);
	QObject::connect(s->actConsole, &QAction::toggled, s->win, [s](bool on){
		if (s->consoleToggle && s->consoleToggle->isChecked() != on) s->consoleToggle->setChecked(on); });
	// Time axis (X) — interpret X as epoch seconds and format ticks as date / time / decimal year /
	// day-of-year. Pure label formatting (no data change); exclusive, like a radio group.
	QMenu *tMenu = mMisc->addMenu("Time axis (X)");
	QActionGroup *tGroup = new QActionGroup(s->win);
	tGroup->setExclusive(true);
	auto addTime = [&](const char *label, int code, bool on) {
		QAction *a = tMenu->addAction(label);
		a->setCheckable(true); a->setChecked(on);
		tGroup->addAction(a);
		if (code >= 0 && code < 6) s->actTime[code] = a;
		QObject::connect(a, &QAction::triggered, s->win, [s, code] { xySetXTime(s, code); });
	};
	addTime("Linear (numbers)", 0, true);
	addTime("Date — auto",      1, false);
	addTime("Date — yyyy-mm-dd",2, false);
	addTime("Time — HH:MM",     3, false);
	addTime("Decimal year",     4, false);
	addTime("Day-of-Year",      5, false);
	// Log scaling (X / Y). Independent checkable toggles. Data must be positive.
	QAction *aLogX = new QAction("Log X axis", s->win);
	aLogX->setCheckable(true);
	s->actLogX = aLogX;
	QObject::connect(aLogX, &QAction::toggled, s->win, [s](bool on){ xySetLog(s, 0, on); });
	QAction *aLogY = new QAction("Log Y axis", s->win);
	aLogY->setCheckable(true);
	s->actLogY = aLogY;
	QObject::connect(aLogY, &QAction::toggled, s->win, [s](bool on){ xySetLog(s, 1, on); });
	// the Grid state = both directions, through xyGridSet like the tool bar's Grid / X-Grid / Y-Grid (below)
	QObject::connect(s->actGrid, &QAction::toggled, s->win, [s](bool on) { xyGridSet(s, true, true, on); });

	// --- toolbar ---
	QToolBar *tb = s->win->addToolBar("Tools");
	tb->setObjectName("xyToolBar");     // named for saveState/restoreState (see installDockGeometryMemory)
	tb->setIconSize(QSize(18, 18));
	QStyle *st = s->win->style();
	QAction *tFit = tb->addAction(st->standardIcon(QStyle::SP_BrowserReload), "Zoom to fit");
	QObject::connect(tFit, &QAction::triggered, s->win, [s]{
		xyCur(s).chart->RecalculateBounds();
		xyCur(s).chart->GetAxis(vtkAxis::BOTTOM)->SetBehavior(vtkAxis::AUTO);
		xyCur(s).chart->GetAxis(vtkAxis::LEFT)->SetBehavior(vtkAxis::AUTO);
		if (s->widget->renderWindow()) s->widget->renderWindow()->Render(); });
	// Title, Subtitle, X-Label, Y-Label (MATLAB's): type that text in place on the chart (xyEditAxisText)
	tb->addSeparator();
	static const char *const kText[4][3] = {
		{ "Title",    "xyTitle",    "Title — type the page's title, at the top of the chart" },
		{ "Subtitle", "xySubtitle", "Subtitle — type the line under the title" },
		{ "X-Label",  "xyXLabel",   "X-Label — type the x axis label" },
		{ "Y-Label",  "xyYLabel",   "Y-Label — type the y axis label" } };
	for (int m = 0; m < 4; ++m) {
		QAction *a = tb->addAction(xyAxisTextIcon(m, s->win->devicePixelRatioF()), kText[m][0]);
		a->setObjectName(kText[m][1]);
		a->setToolTip(kText[m][2]);
		QObject::connect(a, &QAction::triggered, s->win, [s, m] { xyEditAxisText(s, m); });
	}
	tb->addSeparator();
	// Grid, X-Grid, Y-Grid: ONE choice of three (both directions / the vertical lines only / the
	// horizontal ones only); a click on the active one turns the grid off. The active one wears a light
	// face — the app-wide checked look's own 2 px border, so its size never changes, in a soft colour.
	static const char *const kGrid[3][3] = {
		{ "Grid",   "xyGrid",  "Grid — the grid lines of both axes (click again: no grid)" },
		{ "X-Grid", "xyGridX", "X-Grid — only the vertical grid lines, at the x axis' ticks (click again: no grid)" },
		{ "Y-Grid", "xyGridY", "Y-Grid — only the horizontal grid lines, at the y axis' ticks (click again: no grid)" } };
	QActionGroup *gridGrp = new QActionGroup(s->win);
	gridGrp->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
	for (int m = 0; m < 3; ++m) {
		QAction *a = tb->addAction(xyGridIcon(m, s->win->devicePixelRatioF()), kGrid[m][0]);
		a->setObjectName(kGrid[m][1]);
		a->setToolTip(kGrid[m][2]);
		a->setCheckable(true);
		a->setChecked(m == 0);                          // a new window draws both
		gridGrp->addAction(a);
		s->actGridTb[m] = a;
		if (QWidget *b = tb->widgetForAction(a))
			b->setStyleSheet("QToolButton:checked { background-color: #e4eaf1; border: 2px solid #c3cfdc; }");
		QObject::connect(a, &QAction::triggered, s->win, [s, m](bool on) {
			if (!xyAlive(s) || s->pages.empty()) return;
			XYPage &pg = xyCur(s);
			pg.axProps["MAP_FRAME_AXES.Sg"] = (on && m != 2) ? "1" : "0";
			pg.axProps["MAP_FRAME_AXES.Wg"] = (on && m != 1) ? "1" : "0";
			xyApplyAxes(s, pg);
			xyGridSyncUi(s);
		});
	}
	// MATLAB's ANNOTATIONS: arm a tool, then press and drag on the chart to draw it (XYAnnotFilter).
	tb->addSeparator();
	for (int k = 0; k < XY_AN_COUNT; ++k) {
		QAction *a = tb->addAction(xyAnnotIcon(k, s->win->devicePixelRatioF()), xyAnnotNames[k]);
		a->setObjectName(QString("xyAnnot_%1").arg(QString(xyAnnotNames[k]).remove(' ')));
		a->setCheckable(true);
		a->setToolTip(QString("%1 — press and drag on the chart to draw it; double-click it later to edit it (drag it "
		                      "or its handles); its properties are in its Object Manager row").arg(xyAnnotNames[k]));
		s->actAnnot[k] = a;
		QObject::connect(a, &QAction::toggled, s->win, [s, k](bool on) {
			if (on) xyAnnotArm(s, k);
			else if (s->annotTool == k) xyAnnotArm(s, -1);
		});
	}
	// the row stays off until the page shows data (xyRebuildObjMgr turns it on / off from then on)
	s->toolsBar = tb;
	for (QAction *a : tb->actions()) a->setEnabled(!s->pages.empty() && !xyCur(s).series.empty());

	// --- plot-type gallery: its own row under the tool bar; a click re-plots the current page ---
	s->win->addToolBarBreak(Qt::TopToolBarArea);
	QToolBar *gal = s->win->addToolBar("Plot types");
	gal->setObjectName("xyPlotTypes");
	gal->setIconSize(QSize(50, 32));   // 90% of the drawn 56x36 (xyPlotTypeIcon) — the user's size, keep it
	gal->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
	// Display order (the enum's order is only an id): the bar plots side by side.
	static const int order[XY_PT_COUNT] = {
		XY_PT_PLOT, XY_PT_AREA, XY_PT_BAR, XY_PT_BARH, XY_PT_BARSTACK, XY_PT_BARHSTACK, XY_PT_WATERFALL,
		XY_PT_HIST, XY_PT_SEMILOGX, XY_PT_SEMILOGY, XY_PT_LOGLOG, XY_PT_COMET, XY_PT_STEM, XY_PT_STAIRS,
		XY_PT_SCATTER, XY_PT_BUBBLE, XY_PT_ERRORBAR, XY_PT_PLOTYY, XY_PT_FILLBETWEEN, XY_PT_FEATHER,
		XY_PT_DENSITY, XY_PT_BOXPLOT, XY_PT_VIOLIN, XY_PT_ECDF, XY_PT_QQNORM, XY_PT_QQPLOT,
		XY_PT_RADAR, XY_PT_ROSE, XY_PT_POLARPLOT, XY_PT_POLARSCATTER, XY_PT_COMPASS, XY_PT_PIE, XY_PT_DONUT,
		XY_PT_PARALLEL, XY_PT_CORNER, XY_PT_MARGINAL, XY_PT_BIPLOT };
	for (int t : order) {
		if (t == XY_PT_SCATTER || t == XY_PT_DENSITY)
			gal->addSeparator();                      // MATLAB's set | GMT.jl's other plots | statistics
		QAction *a = gal->addAction(xyPlotTypeIcon(t, s->win->devicePixelRatioF()), xyPlotTypeNames[t]);
		a->setObjectName(QString("xyPlotType_%1").arg(QString(xyPlotTypeNames[t]).replace(' ', '_')));
		a->setToolTip(t == XY_PT_COMET     ? QString("Animate this page's series as comets") :
		              t == XY_PT_PLOTYY    ? QString("The first curve on the left Y axis, the others on a right one") :
		              t == XY_PT_FILLBETWEEN ? QString("Fill between the first curve and each of the others") :
		              t == XY_PT_WATERFALL ? QString("Each bar starts where the previous one ended") :
		              t == XY_PT_HIST      ? QString("Histogram of the selected series, on a new page") :
		              t == XY_PT_QQPLOT    ? QString("Quantiles of the first two curves against each other, on a new page") :
		              t == XY_PT_RADAR     ? QString("Each series of this page as one radar polygon (a spoke per point), on a new page") :
		              t == XY_PT_ROSE      ? QString("Rose diagram of the selected series' values as azimuths (degrees), on a new page") :
		              t == XY_PT_MARGINAL  ? QString("The selected series' x against y, with the histogram of each on the margins, on a new page") :
		              t == XY_PT_ERRORBAR  ? QString("Each series with its error bars (asks for the errors of a series that carries none)") :
		              t == XY_PT_BUBBLE    ? QString("Each series as bubbles sized by its values (asks for the sizes of a series that carries none)") :
		              t == XY_PT_FEATHER   ? QString("The first two curves as U and V of a feather (stick) plot, on a new page") :
		              t == XY_PT_POLARPLOT ? QString("Each series' x as angle (radians) and y as radius, as a line on polar axes, on a new page") :
		              t == XY_PT_POLARSCATTER ? QString("Each series' x as angle (radians) and y as radius, as points on polar axes, on a new page") :
		              t == XY_PT_COMPASS   ? QString("Each series' x as angle (radians) and y as radius, as arrows from the origin on polar axes, on a new page") :
		              t == XY_PT_PIE       ? QString("Pie chart of the selected series' values, on a new page") :
		              t == XY_PT_DONUT     ? QString("Donut chart of the selected series' values, on a new page") :
		              t > XY_PT_HIST       ? QString("%1 of this page's series, on a new page").arg(xyPlotTypeNames[t]) :
		                                     QString("Re-plot this page's series as %1").arg(xyPlotTypeNames[t]));
		QObject::connect(a, &QAction::triggered, s->win, [s, t] { xySetPlotType(s, t); });
		a->setData(t);
	}
	// The labels slightly smaller, as a STYLE SHEET: the app-wide sheet (30_app.cpp, the pressed-button
	// look) styles every QToolButton, and a widget a sheet styles ignores setFont — only a sheet's
	// font-size reaches it. 0.9 of the button's font: slightly smaller than the Object Manager's text,
	// never tiny (the default 0.8 was barely readable; 0.7 and 0.5 were rejected).
	if (QWidget *b0 = gal->widgetForAction(gal->actions().value(0))) {
		const QFont f0 = b0->font();
		const QString sz = f0.pointSizeF() > 0 ? QString("%1pt").arg(f0.pointSizeF() * 0.9, 0, 'f', 2)
		                                       : QString("%1px").arg(std::max(6, qRound(f0.pixelSize() * 0.9)));
		gal->setStyleSheet(QString("QToolButton { font-size: %1; }").arg(sz));
	}
	// Right-click on a type: "Help" opens that type's page of the manual (docs/src/41-xyplot-<name>.md),
	// the same plain openUrl the dialogs' green ? disk uses. "Synthetic example" asks Julia for a new page
	// of made-up data (xyanalysis.jl `_xy_example`) and then applies the type to it with xySetPlotType,
	// the function a click on the icon runs.
	gal->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(gal, &QWidget::customContextMenuRequested, s->win, [s, gal](const QPoint &p) {
		QAction *a = gal->actionAt(p);
		if (!a || a->isSeparator() || !a->data().isValid()) return;
		const int t = a->data().toInt();
		const QString name = xyPlotTypeNames[t];
		auto *m = new QMenu(s->win);
		m->setAttribute(Qt::WA_DeleteOnClose);
		m->addAction(makeHelpDiskIcon(16), "Help", s->win, [s, name]() {
			const QUrl u("https://www.generic-mapping-tools.org/InteractiveGMT/dev/41-xyplot-" +
			             QString(name).replace(' ', '-') + "/");
			if (!QDesktopServices::openUrl(u))
				QMessageBox::warning(s->win, "Help", QString("Could not open a browser for\n\n%1").arg(u.toString()));
		});
		// Only THAT example is shown: the previous example's pages go first, and a type that draws its result
		// on a page of its own (histogram, boxplot, pie, ...) leaves just that result, not the data page.
		// A page is deleted while it is the one on screen: that is the case xyDeletePage unmounts its chart.
		auto drop = [s](int i) { xySwitchPage(s, i); xyDeletePage(s, i); };
		m->addAction("Synthetic example", s->win, [s, t, name, drop]() {
			if (s->cometBusy) return;                     // a comet is still drawing on its page
			for (int i = (int)s->pages.size() - 1; i >= 0; --i)
				if (s->pages[i].example) drop(i);
			for (XYPage &pg : s->pages) pg.example = false;   // a lone example page is cleared, not deleted
			int first = (int)s->pages.size();
			if (!xyRequest(s, "example:" + name, -1, "Building the example...") || !xyAlive(s))
				return;
			if (first == 1 && s->pages[0].series.empty()) {  // an empty window: the example is its only page
				drop(0);
				first = 0;
			}
			xySwitchPage(s, (int)s->pages.size() - 1);
			xySetPlotType(s, t);
			if (!xyAlive(s)) return;
			if ((int)s->pages.size() > first + 1)
				drop(first);
			for (int i = first; i < (int)s->pages.size(); ++i)
				s->pages[i].example = true;
			xySwitchPage(s, (int)s->pages.size() - 1);
			xyRebuildObjMgr(s);
		});
		m->popup(gal->mapToGlobal(p));
	});

	// --- Object Manager interactions ---
	QObject::connect(s->objMgr, &QTreeWidget::itemChanged, s->win, [s](QTreeWidgetItem *it, int){
		if (s->rebuilding) return;
		const int idx = it->data(0, Qt::UserRole).toInt();
		if (idx == -1) {                                   // "Info" row: toggle the header strip
			if (s->infoLabel) s->infoLabel->setVisible(it->checkState(0) == Qt::Checked);
			return;
		}
		if (idx == -2) {                                   // "Legend" row: toggle the chart legend
			s->legendVisible = (it->checkState(0) == Qt::Checked);
			xyCur(s).chart->SetShowLegend(s->legendVisible);
			if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
			return;
		}
		if (idx == -4 || idx <= -1000) {                  // "Annotations" group (all of them) or one of them
			std::vector<XYAnnot> &an = xyCur(s).annots;
			const bool on = (it->checkState(0) == Qt::Checked);
			if (idx == -4) for (XYAnnot &a : an) a.visible = on;
			else if (-1000 - idx < (int)an.size()) an[-1000 - idx].visible = on;
			// the group's box follows its children: rebuilt after this signal, not inside it
			QTimer::singleShot(0, s->win, [s] { if (xyAlive(s)) xyRebuildObjMgr(s); });
			xyAnnotRender(s);
			return;
		}
		if (idx == -3) {                                   // "Axes" row: draw / hide the page's axes
			xyCur(s).axesShown = (it->checkState(0) == Qt::Checked);
			xyApplyAxes(s, xyCur(s));
			return;
		}
		std::vector<XYSeries> &series = xyCur(s).series;
		if (idx < 0 || idx >= (int)series.size()) return;
		const bool on = (it->checkState(0) == Qt::Checked);
		series[idx].visible = on;
		if (series[idx].plot) series[idx].plot->SetVisible(on);
		if (s->widget->renderWindow()) s->widget->renderWindow()->Render(); });
	QObject::connect(s->objMgr, &QTreeWidget::itemDoubleClicked, s->win, [s](QTreeWidgetItem *it, int){
		if (!it) return;
		const int idx = it->data(0, Qt::UserRole).toInt();
		if (idx <= -1000) xyAnnotProps(s, -1000 - idx);
		else if (idx == -3) xyAxesPropsOpen(s); else xyLineProperties(s, idx); });
	// An annotation's row picked: that annotation is selected on the chart (its handles show).
	QObject::connect(s->objMgr, &QTreeWidget::currentItemChanged, s->win, [s](QTreeWidgetItem *it, QTreeWidgetItem *) {
		if (s->rebuilding || !it || !xyAlive(s)) return;
		const int idx = it->data(0, Qt::UserRole).toInt();
		if (idx > -1000 || -1000 - idx >= (int)xyCur(s).annots.size()) return;
		xyCur(s).annotSel = -1000 - idx;
		xyAnnotRender(s); });
	auto annotFold = [s](QTreeWidgetItem *it, bool open) {
		if (it && it->data(0, Qt::UserRole).toInt() == -4 && xyAlive(s)) xyCur(s).annotOpen = open;
	};
	QObject::connect(s->objMgr, &QTreeWidget::itemExpanded,  s->win, [annotFold](QTreeWidgetItem *it) { annotFold(it, true); });
	QObject::connect(s->objMgr, &QTreeWidget::itemCollapsed, s->win, [annotFold](QTreeWidgetItem *it) { annotFold(it, false); });
	// The "Axes" row's LABEL opens its Axes properties (its checkbox only draws / hides), as the 3-D
	// viewer's Scene Objects Axes row does: a left click released right of the check indicator.
	struct AxesRowClick : QObject {
		XYPlot *s;
		AxesRowClick(XYPlot *s_, QObject *parent) : QObject(parent), s(s_) {}
		bool eventFilter(QObject *, QEvent *e) override {
			if (e->type() != QEvent::MouseButtonRelease) return false;
			auto *me = static_cast<QMouseEvent *>(e);
			if (me->button() != Qt::LeftButton) return false;
			const QPoint pt = me->position().toPoint();
			QTreeWidgetItem *it = s->objMgr->itemAt(pt);
			if (!it || it->data(0, Qt::UserRole).toInt() != -3) return false;
			if (pt.x() > xyObjMgrLabelX(s, it)) {
				XYPlot *sp = s;
				QTimer::singleShot(0, sp->win, [sp] { xyAxesPropsOpen(sp); });
			}
			return false;
		}
	};
	s->objMgr->viewport()->installEventFilter(new AxesRowClick(s, s->objMgr));
	QObject::connect(s->objMgr, &QWidget::customContextMenuRequested, s->win, [s](const QPoint &p){ xyObjMgrMenu(s, p); });

	// --- live coordinate readout ---
	s->moveCb = vtkSmartPointer<vtkCallbackCommand>::New();
	s->moveCb->SetCallback(xyMouseMove);
	s->moveCb->SetClientData(s);
	s->widget->interactor()->AddObserver(vtkCommand::MouseMoveEvent, s->moveCb);

	// --- time-axis tick refresh on every range change (zoom/pan) ---
	s->ticksCb = vtkSmartPointer<vtkCallbackCommand>::New();
	s->ticksCb->SetCallback(xyTicksOnRender);
	s->ticksCb->SetClientData(s);
	rw->AddObserver(vtkCommand::EndEvent, s->ticksCb);

	// --- interactive Spector-Grant drag-band tool (left-drag while the tool is active) ---
	s->widget->installEventFilter(new XYSGFilter(s, s->win));
	// --- curve marker: click a curve, drag along it, double-click it away ---
	s->widget->installEventFilter(new XYMarkFilter(s, s->win));
	// --- magnetic picks (Guide me / Pin-point me / Delete me, the first-guess click). Installed LAST so
	// it sees a press before the marker does; it consumes only its own. ---
	s->widget->installEventFilter(new XYMagFilter(s, s->win));
	// --- "Reference a curve to another": its picks, and "Reference me" on them (sees a press first) ---
	s->widget->installEventFilter(new XYRefFilter(s, s->win));
	// --- the page's own labels: drag them anywhere ---
	s->widget->installEventFilter(new XYLabelFilter(s, s->win));
	// --- the legend box: drag it anywhere (installed LAST: a press on the legend is the legend's) ---
	s->widget->installEventFilter(new XYLegendFilter(s, s->win));
	// --- the annotations: drawn over everything, so a press on one is theirs (installed after the legend) ---
	s->widget->installEventFilter(new XYAnnotFilter(s, s->win));
	// --- a square page keeps its square through every resize: sized before each render ---
	{
		vtkNew<vtkCallbackCommand> cb;
		cb->SetClientData(s);
		cb->SetCallback([](vtkObject *, unsigned long, void *cd, void *) {
			XYPlot *p = static_cast<XYPlot *>(cd);
			if (xyAlive(p) && !p->pages.empty() && xyCur(p).square) xyApplyPageLook(p, xyCur(p)); });
		rw->AddObserver(vtkCommand::StartEvent, cb);
	}
	// --- the bar code strip follows every chart render (zoom / pan / resize move its mapping) ---
	{
		vtkNew<vtkCallbackCommand> cb;
		cb->SetClientData(s);
		cb->SetCallback([](vtkObject *, unsigned long, void *cd, void *) {
			XYPlot *p = static_cast<XYPlot *>(cd);
			if (xyAlive(p) && p->magBar) p->magBar->update(); });
		rw->AddObserver(vtkCommand::EndEvent, cb);
	}

	// --- the X PARKS the window, it does not destroy it -------------------------------------------
	// Same contract as the Aquamoto control window: closing hides, and the plot lives on as a handle
	// at the bottom of its owner's Scene Objects dock (xyAppendParkedRows below), where a double-click
	// brings it back and the properties menu offers the real deletion. Only that menu (reallyClose)
	// lets a close through — and a plot with NO owner has nowhere to park, so its X closes for real.
	struct XYCloseParks : QObject {
		XYPlot *p;
		XYCloseParks(QObject *parent, XYPlot *plot) : QObject(parent), p(plot) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::Close && p && !p->reallyClose && p->owner && sceneAlive(p->owner)) {
				e->ignore();
				p->win->hide();
				xyParkNow(p);
				return true;
			}
			return QObject::eventFilter(o, e);
		}
	};
	s->win->installEventFilter(new XYCloseParks(s->win, s));

	// --- lifetime bookkeeping (shares the pump/window count with the 3-D viewer) ---
	g_xyplots.insert(s);
	g_openWindows++;
	g_lastRW = rw.Get();
	QObject::connect(s->win, &QObject::destroyed, [s]{
		Scene *owner = s->owner;
		QWidget *w   = s->win;                      // dying, but still the parked entry's identity
		g_xyplots.erase(s);
		if (g_openWindows > 0) g_openWindows--;
		delete s;                                   // struct outlives the QMainWindow; free it here
		// A really-deleted plot must lose its parked row too, or the dock keeps offering a dead handle.
		if (owner && sceneAlive(owner)) { unparkTool(owner, w); rebuildSceneObjects(owner); }
	});

	// First page (mounts a chart in the scene + adds its tab). Must come AFTER objMgr/tabs exist so
	// xySwitchPage can populate them.
	xyNewPage(s, "Page 1", true);

	// FOLD control on the Object Manager's title bar -- same shared FoldTitleBar mechanism
	// (10_geometry.cpp) as the 3-D viewer's Scene Objects dock: click folds it to a thin strip;
	// with no Closable/Floatable feature (see the dock's setFeatures above) it can be folded, but
	// never fully closed or torn off. Building the bar + its onClick here (pre-show) is fine --
	// only the resizeDocks CALLS must wait until after win->show() (see below).
	const int omPx = qRound(s->win->logicalDpiX() * 4.0 / 2.54);   // ~4 cm target width
	QMainWindow *xywin = s->win;
	FoldTitleBar *omBar = new FoldTitleBar("Object Manager", omDock);
	omDock->setTitleBarWidget(omBar);
	omBar->onClick = [xywin, omDock, body = omDock->widget(), omBar, omPx]() {
		const bool fold = body->isVisible();          // visible now -> fold it away
		if (fold) omBar->openWidth = omDock->width(); // remember the open width to restore later
		body->setVisible(!fold);
		omBar->folded = fold;
		omBar->updateGeometry();
		omBar->update();
		const int w = fold ? omBar->sizeHint().width()
						   : (omBar->openWidth > 0 ? omBar->openWidth : omPx);
		xywin->resizeDocks({ omDock }, { w }, Qt::Horizontal);
	};
	s->omUnfold = [omBar, body = omDock->widget()]() { if (!body->isVisible() && omBar->onClick) omBar->onClick(); };

	s->win->statusBar()->showMessage("X,Y plot — add series from Julia (xyplot / add!)");
	s->win->show();
	s->win->raise();
	s->win->activateWindow();

	// Object Manager's INITIAL width only (~4 cm) -- resizeDocks only bites once the window has
	// real laid-out geometry, i.e. AFTER show() (same rule 70_window.cpp's dock folding follows;
	// calling it pre-show corrupts the central widget's layout instead of just sizing the dock).
	s->win->resizeDocks({ omDock }, { omPx }, Qt::Horizontal);

	rw->Render();
	return s;
}

// Open a BLANK X,Y plot window from the host UI (3-D viewer's Tools > X,Y plot) and let Julia
// register a mirror for it (so its File/Analysis menus work). Returns the new window.
static XYPlot *xyOpenBlankFromHost() {
	XYPlot *p = buildXYPlot(nullptr);
	if (p && g_juliaXYNew)
		g_juliaXYNew(p);
	return p;
}

// Open a standalone X,Y plot window pre-loaded with one (x,y) series. Called by the 3-D viewer's
// Profile panel ("Open in X,Y plot tool") so a Ctrl-drag elevation profile — or a downloaded tide
// series — lands in the full plotter. Returns the new window (null on <2 points / mismatch).
static XYPlot *openSeriesInXYTool(const std::vector<double> &x, const std::vector<double> &y,
                                  const char *title, const char *xlabel, const char *ylabel,
                                  ProfilePanel *markFrom) {
	if (x.size() < 2 || y.size() != x.size())
		return nullptr;
	XYPlot *p = buildXYPlot(title);
	if (!p)
		return nullptr;
	if (xlabel && xlabel[0]) xyCur(p).chart->GetAxis(vtkAxis::BOTTOM)->SetTitle(xlabel);
	if (ylabel && ylabel[0]) xyCur(p).chart->GetAxis(vtkAxis::LEFT)->SetTitle(ylabel);
	if (g_juliaXYSeed)                                 // let Julia register a mirror + add the series
		g_juliaXYSeed(p, x.data(), y.data(), (int)x.size(), "Profile");
	else                                               // no host (demo exe) -> add directly
		xyAddSeries(p, x.data(), y.data(), (int)x.size(), "Profile", -1.0, 0.0, 0.0, 0.0, -1, -1, -1.0);
	// The curve just added is the panel's: share its marker (it may already have one).
	if (markFrom && xyAlive(p) && !xyCur(p).series.empty()) {
		p->markPanel = markFrom;
		p->markLinkPage = p->curPageIdx;
		p->markLinkSel = (int)xyCur(p).series.size() - 1;
		markFrom->addMarkerListener(p, [p](double mx) { if (!p->markSyncing) xyMarkFromPanel(p, mx); });
		QPointer<ProfilePanel> pan(markFrom);
		QObject::connect(p->win, &QObject::destroyed, [pan, p]() { if (pan) pan->removeMarkerListener(p); });
		xyMarkFromPanel(p, markFrom->markerX());
		// A Ctrl-drag profile hands over its TRACK too (the magnetic model's azimuth and bathymetry),
		// and the copy follows the track when it is redrawn (ecran's freshFig == false path).
		XYSeries &se = xyCur(p).series[p->markLinkSel];
		int g = -1;
		if (markFrom->track(se.trackX, se.trackY, g)) {
			se.trackGeog = g;
			markFrom->addDataListener(p, [p]() { xyProfileUpdated(p); });
			QObject::connect(p->win, &QObject::destroyed, [pan, p]() { if (pan) pan->removeDataListener(p); });
		}
	}
	return p;
}
