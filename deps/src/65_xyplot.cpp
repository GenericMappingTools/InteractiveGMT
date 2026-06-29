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
};

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
	bool                        ticksBusy = false;
	// Interactive Spector-Grant tool (left-drag a band on a spectrum -> live depth-to-sources).
	bool                        sgActive = false;
	bool                        sgDragging = false;
	int                         sgSel = -1;       // target series captured on activation
	double                      sgX0 = 0.0;       // drag-start frequency
	double                      sgUnit = 1000.0;  // wavenumber->metres factor (1/km default)
	vtkPlot                    *sgFit = nullptr;  // the live fit line (not in series)
	vtkSmartPointer<vtkTable>   sgFitTable;
};

// A live X,Y plot window. The opaque handle handed to the host is this XYPlot*.
// Per-curve / per-axis state lives in the PAGES; the window owns only the shared
// widgets (one VTK view, the Object Manager, the Data Viewer, the Console, tabs).
struct XYPlot {
	QMainWindow                        *win = nullptr;
	QVTKOpenGLNativeWidget             *widget = nullptr;
	vtkSmartPointer<vtkContextView>     view;
	QTreeWidget                        *objMgr = nullptr;      // Object Manager (series list)
	QTableWidget                       *dataTable = nullptr;   // Data Viewer spreadsheet
	QDockWidget                        *dataDock = nullptr;    // foldable bottom dock
	QPlainTextEdit                     *console = nullptr;       // ERRORS tab: read-only execution-error log (xyLog)
	QPlainTextEdit                     *cmdConsole = nullptr;    // JULIA tab: read-only output of typed commands
	QLineEdit                          *consoleInput = nullptr;  // interactive julia> line (shares Main with the 3-D viewer)
	QWidget                            *consolePanel = nullptr;  // collapsible container under the chart
	QTabWidget                         *xyErrTab = nullptr;      // console body tabs (Errors | Julia); xyLog raises Errors
	QToolButton                        *consoleToggle = nullptr; // disclosure triangle (collapsed by default)
	QAction                            *actLegend = nullptr;
	QAction                            *actGrid = nullptr;
	QAction                            *actDataView = nullptr;
	QAction                            *actConsole = nullptr;
	vtkSmartPointer<vtkCallbackCommand> moveCb;               // mouse-move coord readout
	vtkSmartPointer<vtkCallbackCommand> ticksCb;             // time-axis tick refresh on range change
	bool                                rebuilding = false;    // guard objMgr itemChanged storms
	// pages (Excel-like tabs). Always >= 1; the current one is mounted in the scene.
	std::vector<XYPage>                 pages;
	int                                 curPageIdx = 0;
	QTabBar                            *tabs = nullptr;
	bool                                tabBusy = false;       // guard QTabBar::currentChanged storms
};

// Live X,Y windows, keyed by the XYPlot *handed back to the host. A handle is
// valid only while its window is open (the destroyed-lambda erases it).
static std::unordered_set<XYPlot*> g_xyplots;
static bool xyAlive(XYPlot *p) { return p && g_xyplots.count(p) != 0; }

// The current page of a window (always valid once the window is built: there is
// always at least one page).
static inline XYPage &xyCur(XYPlot *s) { return s->pages[s->curPageIdx]; }

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

// Push a series' presentation (colour / width / line style / markers) onto its chart plot.
static void xyApplyStyle(XYSeries &se) {
	if (!se.plot)
		return;
	se.plot->SetColor((unsigned char)(se.r * 255), (unsigned char)(se.g * 255),
	                  (unsigned char)(se.b * 255), 255);
	se.plot->SetWidth((float)se.width);
	if (se.plot->GetPen())
		se.plot->GetPen()->SetLineType(se.lineType);
	if (vtkPlotPoints *pp = vtkPlotPoints::SafeDownCast(se.plot)) {
		pp->SetMarkerStyle(se.marker);
		pp->SetMarkerSize((float)se.markerSize);
	}
}

// (Re)build the current page's chart plot items from its series tables. Called after a delete
// (plot indices shift, so it is simplest to clear + re-add all). Incremental adds add one plot
// directly. Re-applies style + label + visibility.
static void xyRebuildPlots(XYPlot *s) {
	XYPage &pg = xyCur(s);
	pg.chart->ClearPlots();
	for (auto &se : pg.series) {
		vtkPlot *pl = pg.chart->AddPlot(vtkChart::LINE);
		pl->SetInputData(se.table, 0, 1);
		pl->SetLabel(se.name);
		pl->SetTooltipLabelFormat("%x, %y");   // hover shows ONLY x,y (not the series name)
		pl->SetVisible(se.visible);
		se.plot = pl;
		xyApplyStyle(se);
	}
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
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
	s->rebuilding = false;
	if (toSelect)
		s->objMgr->setCurrentItem(toSelect);            // guarantee a current row for Analysis
}

// Fill the Data Viewer spreadsheet with one (current-page) series' (x,y) columns.
static void xyFillDataTable(XYPlot *s, int idx) {
	std::vector<XYSeries> &series = xyCur(s).series;
	if (!s->dataTable || idx < 0 || idx >= (int)series.size())
		return;
	const XYSeries &se = series[idx];
	vtkTable *t = se.table;
	const int nrows = t ? (int)t->GetNumberOfRows() : 0;
	QTableWidget *w = s->dataTable;
	w->clearContents();
	w->setColumnCount(2);
	w->setRowCount(nrows);
	w->setHorizontalHeaderItem(0, new QTableWidgetItem("X"));
	w->setHorizontalHeaderItem(1, new QTableWidgetItem(QString::fromStdString(se.name)));
	for (int r = 0; r < nrows; ++r) {
		for (int c = 0; c < 2; ++c) {
			QTableWidgetItem *cell = new QTableWidgetItem(QString::number(t->GetValue(r, c).ToDouble(), 'g', 8));
			cell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
			w->setItem(r, c, cell);
		}
	}
	w->resizeColumnsToContents();
}

// Append one (x,y) series to the CURRENT page. Returns its index (or -1 on bad input). Renders.
// `lineType` (vtkPen), `marker` (vtkPlotPoints) and `markerSize` set the presentation; pass
// lineType<0 / marker<0 / markerSize<=0 for the defaults (solid line, no marker, size 7).
static int xyAddSeries(XYPlot *s, const double *x, const double *y, int n,
                       const char *name, double r, double g, double b, double width,
                       int lineType, int marker, double markerSize) {
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

	se.table = vtkSmartPointer<vtkTable>::New();
	// X is DOUBLE: epoch-seconds time values (~1.6e9) lose ~128 s of resolution in float32, which
	// collapses finely-sampled series (e.g. minute-spaced tides) onto duplicate X. Y stays float.
	vtkNew<vtkDoubleArray> ax; ax->SetName("X");                      se.table->AddColumn(ax);
	vtkNew<vtkFloatArray> ay; ay->SetName(se.name.c_str());          se.table->AddColumn(ay);
	se.table->SetNumberOfRows(n);
	for (int i = 0; i < n; ++i) {
		se.table->SetValue(i, 0, x[i]);
		se.table->SetValue(i, 1, (float)y[i]);
	}

	vtkPlot *pl = pg.chart->AddPlot(vtkChart::LINE);
	pl->SetInputData(se.table, 0, 1);
	pl->SetLabel(se.name);
	pl->SetTooltipLabelFormat("%x, %y");   // hover shows ONLY x,y (not the series name)
	se.plot = pl;
	xyApplyStyle(se);

	const int idx = (int)pg.series.size();
	pg.series.push_back(se);
	xyRebuildObjMgr(s);
	xyFillDataTable(s, idx);
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
	return idx;
}

// Drop every series on the current page.
static void xyClear(XYPlot *s) {
	if (!xyAlive(s))
		return;
	XYPage &pg = xyCur(s);
	pg.chart->ClearPlots();
	pg.series.clear();
	xyRebuildObjMgr(s);
	if (s->dataTable) { s->dataTable->clearContents(); s->dataTable->setRowCount(0); }
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Delete the series at idx on the current page (rebuilds plots + the tree so indices re-sync).
static void xyDeleteSeries(XYPlot *s, int idx) {
	std::vector<XYSeries> &series = xyCur(s).series;
	if (idx < 0 || idx >= (int)series.size())
		return;
	series.erase(series.begin() + idx);
	xyRebuildPlots(s);
	xyRebuildObjMgr(s);
	if (s->dataTable) { s->dataTable->clearContents(); s->dataTable->setRowCount(0); }
}

// (defined below) — needed here for the time-mode status-bar readout.
static QString xyFmtTimeHover(double t, int fmt);

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
	const QString xs = pg.xTimeFmt ? xyFmtTimeHover(dx, pg.xTimeFmt)
	                               : QString("%1").arg(dx, 0, 'g', 8);
	s->win->statusBar()->showMessage(QString("%1,  %2").arg(xs).arg(dy, 0, 'g', 6));
}

// Export the current plot to a PNG.
static void xyExportPng(XYPlot *s) {
	const QString fn = QFileDialog::getSaveFileName(s->win, "Export plot as PNG", prefStartDir("plot.png"), "PNG image (*.png)");
	if (fn.isEmpty())
		return;
	rememberStartDir(fn);
	vtkNew<vtkWindowToImageFilter> w2i;
	w2i->SetInput(s->view->GetRenderWindow());
	w2i->Update();
	vtkNew<vtkPNGWriter> wr;
	wr->SetFileName(fn.toUtf8().constData());
	wr->SetInputConnection(w2i->GetOutputPort());
	wr->Write();
	s->win->statusBar()->showMessage("Saved " + fn, 4000);
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
	dlg.setWindowTitle(QString("Line properties — %1").arg(QString::fromStdString(se.name)));
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
		xyApplyStyle(se); swatch(); xyRebuildObjMgr(s); rr();
	});

	QDoubleSpinBox *wsp = new QDoubleSpinBox(&dlg);
	wsp->setRange(0.5, 12.0); wsp->setSingleStep(0.5); wsp->setValue(se.width);
	form->addRow("Width (px)", wsp);
	QObject::connect(wsp, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg,
		[&, rr](double v) { se.width = v; xyApplyStyle(se); rr(); });

	// combo index -> vtkPen line type
	static const int kPen[] = { 1, 2, 3, 4, 5, 0 };  // Solid Dashed Dotted Dash-Dot Dash-Dot-Dot None
	QComboBox *lsc = new QComboBox(&dlg);
	lsc->addItems(QStringList() << "Solid" << "Dashed" << "Dotted" << "Dash-Dot" << "Dash-Dot-Dot" << "None");
	for (int i = 0; i < 6; ++i) if (kPen[i] == se.lineType) lsc->setCurrentIndex(i);
	form->addRow("Line style", lsc);
	QObject::connect(lsc, qOverload<int>(&QComboBox::currentIndexChanged), &dlg,
		[&, rr](int i) { se.lineType = kPen[i]; xyApplyStyle(se); rr(); });

	// combo index == vtkPlotPoints marker style (NONE=0 .. DIAMOND=5)
	QComboBox *mkc = new QComboBox(&dlg);
	mkc->addItems(QStringList() << "None" << "Cross" << "Plus" << "Square" << "Circle" << "Diamond");
	mkc->setCurrentIndex(se.marker);
	form->addRow("Marker", mkc);
	QObject::connect(mkc, qOverload<int>(&QComboBox::currentIndexChanged), &dlg,
		[&, rr](int i) { se.marker = i; xyApplyStyle(se); rr(); });

	QDoubleSpinBox *msp = new QDoubleSpinBox(&dlg);
	msp->setRange(1.0, 20.0); msp->setValue(se.markerSize);
	form->addRow("Marker size", msp);
	QObject::connect(msp, qOverload<double>(&QDoubleSpinBox::valueChanged), &dlg,
		[&, rr](double v) { se.markerSize = v; xyApplyStyle(se); rr(); });

	// Save THIS series to a file (same Julia gmtwrite path as File>Save, but pre-targeted to idx).
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, &dlg);
	form->addRow(bb);
	QObject::connect(bb->button(QDialogButtonBox::Save), &QPushButton::clicked, &dlg, [s, idx] {
		if (!g_juliaXY) { xyLog(s, "Save: not wired (rebuild the DLL + restart Julia)", true); return; }
		const QString fn = QFileDialog::getSaveFileName(s->win, "Save series", prefStartDir("series.dat"),
			"Text (*.dat *.txt);;CSV (*.csv);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		g_juliaXY(s, "save", idx, fn.toUtf8().constData());
		xyLog(s, QString("Saved series #%1 to %2").arg(idx).arg(fn));
	});
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
	QMenu m(s->objMgr);
	QAction *aProp = m.addAction("Line properties…");
	QAction *aSave = m.addAction("Save data…");
	QAction *aRen = m.addAction("Rename…");
	QAction *aDel = m.addAction("Delete");
	QAction *pick = m.exec(s->objMgr->viewport()->mapToGlobal(pos));
	std::vector<XYSeries> &series = xyCur(s).series;
	if (pick == aProp) {
		xyLineProperties(s, idx);
	}
	else if (pick == aSave) {
		// Save THIS series' (x,y) data to a file (same Julia path as File>Save, but targeting the
		// clicked series). GMT.gmtwrite picks the format from the extension (.dat/.txt/.csv/…).
		if (!g_juliaXY) { s->win->statusBar()->showMessage("Save: not wired yet", 3000); return; }
		const QString fn = QFileDialog::getSaveFileName(s->win, "Save series data", prefStartDir("series.dat"),
			"Text (*.dat *.txt);;CSV (*.csv);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		g_juliaXY(s, "save", idx, fn.toUtf8().constData());
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
// fit line over the band and show depth = |slope|/(4π)·unit in the status bar.

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

// Least-squares fit of ln(power) vs k over band [xa,xb] of series idx (positive power only) on the
// current page.
static bool xySGFit(XYPlot *s, int idx, double xa, double xb, double unit,
                    double &slope, double &inter, double &depth, double &xlo, double &xhi) {
	std::vector<XYSeries> &series = xyCur(s).series;
	if (idx < 0 || idx >= (int)series.size())
		return false;
	if (xa > xb) std::swap(xa, xb);
	vtkTable *t = series[idx].table;
	if (!t) return false;
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
		pg.sgFit->SetColor(0, 0, 0, 255);
		pg.sgFit->SetWidth(2.5f);
		pg.sgFit->SetLabel("S&G fit");
	}
	pg.sgFit->SetInputData(pg.sgFitTable, 0, 1);
	s->win->statusBar()->showMessage(QString("Spector-Grant:  band Δ=%1   slope=%2   Depth = %3 m")
		.arg(xhi - xlo, 0, 'g', 4).arg(slope, 0, 'g', 4).arg(depth, 0, 'f', 0));
	if (s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Remove the fit line from the current page (tool turned off).
static void xySGClear(XYPlot *s) {
	XYPage &pg = xyCur(s);
	if (pg.sgFit) { pg.chart->RemovePlotInstance(pg.sgFit); pg.sgFit = nullptr; }
	pg.sgFitTable = nullptr;
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Qt event filter on the chart widget: while the tool is active, left-drag selects the band and
// drives the live fit (consuming the events so the chart's own zoom/pan stays out of the way).
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
			if (me->button() == Qt::LeftButton) { pg.sgDragging = true; pg.sgX0 = xySGDataX(s, me->position()); return true; }
		}
		else if (t == QEvent::MouseMove && pg.sgDragging) {
			xySGUpdate(s, xySGDataX(s, static_cast<QMouseEvent*>(ev)->position()));
			return true;
		}
		else if (t == QEvent::MouseButtonRelease && pg.sgDragging) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (me->button() == Qt::LeftButton) { pg.sgDragging = false; return true; }
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
static void xyRefreshTicks(XYPlot *s) {
	XYPage &pg = xyCur(s);
	vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM);
	if (ax->GetLogScaleActive()) {
		ax->SetCustomTickPositions(nullptr);
		return;
	}
	const double lo = ax->GetMinimum(), hi = ax->GetMaximum();
	if (!(hi > lo))
		return;
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

// Render-end observer: regenerate the bottom-axis ticks if its range changed (zoom/pan/new data),
// then re-render once. Guarded against reentrancy (the re-render fires EndEvent again with an
// unchanged range, so it returns early).
static void xyTicksOnRender(vtkObject *, unsigned long, void *clientData, void *) {
	XYPlot *s = static_cast<XYPlot*>(clientData);
	if (!xyAlive(s))
		return;
	XYPage &pg = xyCur(s);
	if (pg.ticksBusy)
		return;
	vtkAxis *ax = pg.chart->GetAxis(vtkAxis::BOTTOM);
	const double lo = ax->GetMinimum(), hi = ax->GetMaximum();
	if (!(hi > lo))
		return;
	const double tol = 1e-6 * (std::abs(hi) + std::abs(lo) + 1.0);
	if (std::abs(lo - pg.lastLo) < tol && std::abs(hi - pg.lastHi) < tol)
		return;
	pg.lastLo = lo; pg.lastHi = hi; pg.ticksBusy = true;
	xyRefreshTicks(s);
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
	pg.ticksBusy = false;
}

// Toggle log scaling on a current-page axis (axis 0 = bottom/X, 1 = left/Y). Data must be positive
// for VTK to actually activate it. Recomputes bounds + renders.
static void xySetLog(XYPlot *s, int axis, bool on) {
	XYPage &pg = xyCur(s);
	vtkAxis *ax = pg.chart->GetAxis(axis == 0 ? vtkAxis::BOTTOM : vtkAxis::LEFT);
	ax->SetLogScale(on);
	pg.chart->RecalculateBounds();
	pg.lastLo = std::numeric_limits<double>::quiet_NaN();   // X log on/off changes the tick scheme
	pg.lastHi = std::numeric_limits<double>::quiet_NaN();
	xyRefreshTicks(s);
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
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// vtkChartXY subclass whose only job is to format the hover tooltip's X value as a calendar
// date/time when the current page is in a time mode. VTK's built-in tooltip prints the raw X (epoch
// seconds, ~1e9) via the plot's "%x" format with no date support, so we override the one virtual
// that fills the tooltip and build "date,  y" ourselves. Linear mode falls back to the base.
class XYChart : public vtkChartXY {
public:
	static XYChart *New();
	vtkTypeMacro(XYChart, vtkChartXY);
	XYPlot *owner = nullptr;                            // set right after New(); never reparented
protected:
	void SetTooltipInfo(const vtkContextMouseEvent &mouse, const vtkVector2d &plotPos,
	                    vtkIdType seriesIndex, vtkPlot *plot, vtkIdType segmentIndex) override {
		const int fmt = (owner && !owner->pages.empty()) ? owner->pages[owner->curPageIdx].xTimeFmt : 0;
		if (fmt == 0 || !this->GetTooltip()) {
			vtkChartXY::SetTooltipInfo(mouse, plotPos, seriesIndex, plot, segmentIndex);
			return;
		}
		const QString lab = xyFmtTimeHover(plotPos.GetX(), fmt)
		                  + ",  " + QString::number(plotPos.GetY(), 'g', 6);
		this->GetTooltip()->SetText(lab.toUtf8().constData());
		this->GetTooltip()->SetPosition(mouse.GetScenePos()[0] + 2, mouse.GetScenePos()[1] + 2);
	}
};
vtkStandardNewMacro(XYChart);

// ---- pages (Excel-like tabs) -----------------------------------------------

// Build a fresh chart configured like the window's first one: owner set (so the tooltip can read
// the live time mode), legend/grid matching the current toolbar toggles, compact axis labels.
static vtkSmartPointer<vtkChartXY> xyMakeChart(XYPlot *s) {
	vtkSmartPointer<XYChart> chart = vtkSmartPointer<XYChart>::New();
	chart->owner = s;
	chart->SetShowLegend(s->actLegend ? s->actLegend->isChecked() : true);
	chart->GetAxis(vtkAxis::BOTTOM)->SetTitle("X");      // sensible defaults; set_labels overrides
	chart->GetAxis(vtkAxis::LEFT)->SetTitle("Y");
	const bool grid = s->actGrid ? s->actGrid->isChecked() : true;
	// Compact tick labels: smaller font + bounded precision so x ticks stop colliding. STANDARD
	// notation trims trailing-zero clutter.
	for (int an : { vtkAxis::BOTTOM, vtkAxis::LEFT }) {
		vtkAxis *ax = chart->GetAxis(an);
		ax->GetLabelProperties()->SetFontSize(11);
		ax->GetTitleProperties()->SetFontSize(13);
		ax->SetNotation(vtkAxis::STANDARD_NOTATION);
		ax->SetPrecision(4);
		ax->SetGridVisible(grid);
	}
	return chart;
}

static void xySwitchPage(XYPlot *s, int idx);

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
	if (pg.series.empty()) {
		if (s->dataTable) { s->dataTable->clearContents(); s->dataTable->setRowCount(0); }
	} else {
		xyFillDataTable(s, (int)pg.series.size() - 1);
	}
	pg.lastLo = std::numeric_limits<double>::quiet_NaN();   // force a tick recompute for this page
	pg.lastHi = std::numeric_limits<double>::quiet_NaN();
	xyRefreshTicks(s);
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
	for (const XYSeries &so : src.series) {
		XYSeries se = so;                                // copies style/name; deep-copy the table next
		se.table = vtkSmartPointer<vtkTable>::New();
		se.table->DeepCopy(so.table);
		vtkPlot *pl = dst.chart->AddPlot(vtkChart::LINE);
		pl->SetInputData(se.table, 0, 1);
		pl->SetLabel(se.name);
		pl->SetTooltipLabelFormat("%x, %y");
		pl->SetVisible(se.visible);
		se.plot = pl;
		xyApplyStyle(se);
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

// Right-click on the tab bar: New / Duplicate / Rename / Delete the clicked page.
static void xyTabMenu(XYPlot *s, const QPoint &pos) {
	const int idx = s->tabs->tabAt(pos);
	QMenu m(s->tabs);
	QAction *aNew = m.addAction("New");
	QAction *aDup = (idx >= 0) ? m.addAction("Duplicate") : nullptr;
	QAction *aRen = (idx >= 0) ? m.addAction("Rename…")   : nullptr;
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
	}
}

// ---- window builder --------------------------------------------------------

static XYPlot *buildXYPlot(const char *title) {
	ensureApp();

	XYPlot *s = new XYPlot();
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

	s->cmdConsole = new QPlainTextEdit();              // JULIA tab — command output
	s->cmdConsole->setReadOnly(true);
	s->cmdConsole->setMaximumBlockCount(2000);
	s->cmdConsole->setFont(QFont("Consolas", 9));
	s->cmdConsole->setPlaceholderText("Julia output. `fig` is this window — shared session with the 3-D viewer.");

	s->consoleInput = new QLineEdit();                 // JULIA tab — interactive input
	s->consoleInput->setFont(QFont("Consolas", 9));
	s->consoleInput->setPlaceholderText("julia>  (Enter to run)");
	QObject::connect(s->consoleInput, &QLineEdit::returnPressed, s->win, [s]() {
		const std::string cmd = s->consoleInput->text().toStdString();
		if (cmd.empty())
			return;
		s->consoleInput->clear();
		if (s->cmdConsole)
			s->cmdConsole->appendPlainText(QString("julia> ") + QString::fromStdString(cmd));
		if (!g_juliaEval) {
			if (s->cmdConsole) s->cmdConsole->appendPlainText("(no Julia eval callback registered)");
			return;
		}
		static std::vector<char> buf(1 << 16);         // 64 KB result buffer (shared, reused)
		// _console_eval returns the byte count; NEGATIVE flags a Julia error (still |n| bytes of text).
		int n   = g_juliaEval(s, cmd.c_str(), buf.data(), (int)buf.size());
		int len = n < 0 ? -n : n;
		if (len > 0 && s->cmdConsole)                  // command's own error stays inline in the Julia tab
			s->cmdConsole->appendPlainText(QString::fromUtf8(buf.data(), len));
	});

	QWidget     *juliaTab = new QWidget();             // JULIA tab = output (stretch) + input line
	QVBoxLayout *jtLay    = new QVBoxLayout(juliaTab);
	jtLay->setContentsMargins(0, 0, 0, 0);
	jtLay->setSpacing(0);
	jtLay->addWidget(s->cmdConsole, 1);
	jtLay->addWidget(s->consoleInput, 0);

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

	QWidget *central = new QWidget();
	QVBoxLayout *centralLayout = new QVBoxLayout(central);
	centralLayout->setContentsMargins(0, 0, 0, 0);
	centralLayout->setSpacing(0);
	centralLayout->addWidget(s->widget, 1);            // chart takes all the stretch
	centralLayout->addWidget(tabRow, 0);               // page tabs just under the chart
	centralLayout->addWidget(s->consolePanel, 0);      // console hugs the bottom
	s->win->setCentralWidget(central);

	// --- Object Manager dock (left) ---
	QDockWidget *omDock = new QDockWidget("Object Manager", s->win);
	omDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
	s->objMgr = new QTreeWidget(omDock);
	s->objMgr->setHeaderLabels(QStringList() << "Series");
	s->objMgr->setRootIsDecorated(false);
	s->objMgr->setContextMenuPolicy(Qt::CustomContextMenu);
	omDock->setWidget(s->objMgr);
	s->win->addDockWidget(Qt::LeftDockWidgetArea, omDock);

	// --- Data Viewer dock (bottom, foldable via the Misc/toolbar toggle) ---
	s->dataDock = new QDockWidget("Data Viewer", s->win);
	s->dataDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
	s->dataTable = new QTableWidget(s->dataDock);
	s->dataTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	s->dataTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	s->dataDock->setWidget(s->dataTable);
	s->win->addDockWidget(Qt::BottomDockWidgetArea, s->dataDock);
	s->dataDock->setVisible(false);                 // starts folded away

	// --- menubar ---
	QMenuBar *mb = s->win->menuBar();
	QMenu *mFile = mb->addMenu("File");
	QAction *aNew  = mFile->addAction("New");
	QAction *aOpen = mFile->addAction("Open…");
	QAction *aSave = mFile->addAction("Save…");
	mFile->addSeparator();
	QAction *aExport = mFile->addAction("Export Image…");
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
	QObject::connect(aExport, &QAction::triggered, s->win, [s]{ xyExportPng(s); });
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
				"Threshold (× robust σ of the residual):", 2.0, 0.5, 20.0, 2, &ok);
			if (!ok) return;
			g_juliaXYAna(s, QString("despike:%1").arg(k, 0, 'g', 6).toUtf8().constData(), sel);
		});
	}
	{
		// Spector & Grant (interactive): on a (wavenumber, power) spectrum, LEFT-DRAG a band and the
		// slope of ln(power) gives the depth to the magnetic source ensemble — live fit line + readout.
		QAction *a = mAna->addAction("Depth to sources (Spector-Grant)");
		a->setCheckable(true);
		QObject::connect(a, &QAction::toggled, s->win, [s, a](bool on) {
			XYPage &pg = xyCur(s);
			if (on) {
				const int sel = xyCurrentSel(s);
				if (sel < 0) { s->win->statusBar()->showMessage("Select the spectrum series first", 3500); a->setChecked(false); return; }
				double xf;
				if (!xyAskFreqUnit(s->win, xf)) { a->setChecked(false); return; }
				pg.sgSel = sel; pg.sgUnit = xf; pg.sgActive = true;
				s->win->statusBar()->showMessage("Spector-Grant: left-drag a frequency band on the spectrum");
			}
			else {
				pg.sgActive = false; pg.sgDragging = false;
				xySGClear(s);
				s->win->statusBar()->clearMessage();
			}
		});
	}

	// GMT menu — GUI front-ends for GMT modules that operate on table data. Each item pops an options
	// dialog, then hands an encoded op string to Julia (through the same Analysis callback) which runs
	// the module via GMT.jl and lands the result on its own page(s).
	QMenu *mGMT = mb->addMenu("GMT");
	{
		QAction *a = mGMT->addAction("spectrum1d…");
		QObject::connect(a, &QAction::triggered, s->win, [s, gate] {
			const int sel = gate(); if (sel < 0) return;
			QString args;
			if (!xyAskSpectrum1d(s->win, s, sel, args)) return;
			g_juliaXYAna(s, QString("spectrum1d:%1").arg(args).toUtf8().constData(), sel);
		});
	}

	QMenu *mMisc = mb->addMenu("Misc");
	s->actLegend = mMisc->addAction("Show legend");
	s->actLegend->setCheckable(true); s->actLegend->setChecked(true);
	s->actGrid = mMisc->addAction("Grid");
	s->actGrid->setCheckable(true); s->actGrid->setChecked(true);
	s->actDataView = mMisc->addAction("Data Viewer");
	s->actDataView->setCheckable(true); s->actDataView->setChecked(false);
	s->actConsole = mMisc->addAction("Panels");
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
		QObject::connect(a, &QAction::triggered, s->win, [s, code] { xySetXTime(s, code); });
	};
	addTime("Linear (numbers)", 0, true);
	addTime("Date — auto",      1, false);
	addTime("Date — yyyy-mm-dd",2, false);
	addTime("Time — HH:MM",     3, false);
	addTime("Decimal year",     4, false);
	addTime("Day-of-Year",      5, false);
	// Log scaling (X / Y). Independent checkable toggles. Data must be positive.
	QAction *aLogX = mMisc->addAction("Log X axis");
	aLogX->setCheckable(true);
	QObject::connect(aLogX, &QAction::toggled, s->win, [s](bool on){ xySetLog(s, 0, on); });
	QAction *aLogY = mMisc->addAction("Log Y axis");
	aLogY->setCheckable(true);
	QObject::connect(aLogY, &QAction::toggled, s->win, [s](bool on){ xySetLog(s, 1, on); });
	QObject::connect(s->actLegend, &QAction::toggled, s->win, [s](bool on){
		xyCur(s).chart->SetShowLegend(on); if (s->widget->renderWindow()) s->widget->renderWindow()->Render(); });
	QObject::connect(s->actGrid, &QAction::toggled, s->win, [s](bool on){
		xyCur(s).chart->GetAxis(vtkAxis::BOTTOM)->SetGridVisible(on);
		xyCur(s).chart->GetAxis(vtkAxis::LEFT)->SetGridVisible(on);
		if (s->widget->renderWindow()) s->widget->renderWindow()->Render(); });
	QObject::connect(s->actDataView, &QAction::toggled, s->win, [s](bool on){ s->dataDock->setVisible(on); });
	QObject::connect(s->dataDock, &QDockWidget::visibilityChanged, s->win, [s](bool vis){
		if (s->actDataView) s->actDataView->setChecked(vis); });

	// --- toolbar ---
	QToolBar *tb = s->win->addToolBar("Tools");
	tb->setIconSize(QSize(18, 18));
	QStyle *st = s->win->style();
	QAction *tFit = tb->addAction(st->standardIcon(QStyle::SP_BrowserReload), "Zoom to fit");
	QObject::connect(tFit, &QAction::triggered, s->win, [s]{
		xyCur(s).chart->RecalculateBounds();
		xyCur(s).chart->GetAxis(vtkAxis::BOTTOM)->SetBehavior(vtkAxis::AUTO);
		xyCur(s).chart->GetAxis(vtkAxis::LEFT)->SetBehavior(vtkAxis::AUTO);
		if (s->widget->renderWindow()) s->widget->renderWindow()->Render(); });
	tb->addAction(s->actLegend);
	tb->addAction(s->actGrid);
	tb->addAction(s->actDataView);
	tb->addSeparator();
	QAction *tPng = tb->addAction(st->standardIcon(QStyle::SP_DialogSaveButton), "Export Image…");
	QObject::connect(tPng, &QAction::triggered, s->win, [s]{ xyExportPng(s); });

	// --- Object Manager interactions ---
	QObject::connect(s->objMgr, &QTreeWidget::itemChanged, s->win, [s](QTreeWidgetItem *it, int){
		if (s->rebuilding) return;
		std::vector<XYSeries> &series = xyCur(s).series;
		const int idx = it->data(0, Qt::UserRole).toInt();
		if (idx < 0 || idx >= (int)series.size()) return;
		const bool on = (it->checkState(0) == Qt::Checked);
		series[idx].visible = on;
		if (series[idx].plot) series[idx].plot->SetVisible(on);
		if (s->widget->renderWindow()) s->widget->renderWindow()->Render(); });
	QObject::connect(s->objMgr, &QTreeWidget::itemSelectionChanged, s->win, [s]{
		QTreeWidgetItem *it = s->objMgr->currentItem();
		if (it) xyFillDataTable(s, it->data(0, Qt::UserRole).toInt()); });
	QObject::connect(s->objMgr, &QTreeWidget::itemDoubleClicked, s->win, [s](QTreeWidgetItem *it, int){
		if (it) xyLineProperties(s, it->data(0, Qt::UserRole).toInt()); });
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

	// --- lifetime bookkeeping (shares the pump/window count with the 3-D viewer) ---
	g_xyplots.insert(s);
	g_openWindows++;
	g_lastRW = rw.Get();
	QObject::connect(s->win, &QObject::destroyed, [s]{
		g_xyplots.erase(s);
		if (g_openWindows > 0) g_openWindows--;
		delete s;                                   // struct outlives the QMainWindow; free it here
	});

	// First page (mounts a chart in the scene + adds its tab). Must come AFTER objMgr/dataTable/tabs
	// exist so xySwitchPage can populate them.
	xyNewPage(s, "Page 1", true);

	s->win->statusBar()->showMessage("X,Y plot — add series from Julia (xyplot / add!)");
	s->win->show();
	s->win->raise();
	s->win->activateWindow();
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
static XYPlot *openSeriesInXYTool(const std::vector<double>& x, const std::vector<double>& y,
                                  const char *title, const char *xlabel, const char *ylabel) {
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
	return p;
}
