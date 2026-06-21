// ============================================================================
//  X,Y plot tool — the evolution of the Profile. A STANDALONE QMainWindow with
//  a vtkChartXY plot, an Object Manager (tree of plotted series), a foldable
//  Data Viewer spreadsheet, a menubar (File / Analysis / Misc) and a toolbar.
//  Modelled on Mirone's ecran.m. This window is NOT a 3-D Scene: it has its own
//  struct (XYPlot) + its own C API (gmtvtk_xyplot_*, in 90_c_api.cpp) and shares
//  ONLY the QApplication + the gmtvtk_process_events pump with the 3-D viewer.
//
//  Phase 1 = the shell + plumbing: open a window, add/remove/toggle (x,y) series
//  driven from Julia, zoom/pan/legend/grid (vtkChartXY built-ins), a live coord
//  readout, the Data Viewer table, export PNG. Analysis ops come in Phase 2.
// ============================================================================

// One plotted curve: its data table (column 0 = X, column 1 = Y) + the chart
// plot item (owned by the chart) + presentation state.
struct XYSeries {
	vtkSmartPointer<vtkTable> table;
	vtkPlot                  *plot = nullptr;   // owned by s->chart; rebuilt on delete
	std::string               name;
	double                    r = 0.92, g = 0.67, b = 0.0;
	double                    width = 1.5;
	int                       lineType = 1;     // vtkPen: SOLID=1 DASH=2 DOT=3 DASH_DOT=4 DASH_DOT_DOT=5 NONE=0
	int                       marker = 0;       // vtkPlotPoints: NONE=0 CROSS=1 PLUS=2 SQUARE=3 CIRCLE=4 DIAMOND=5
	double                    markerSize = 7.0;
	bool                      visible = true;
};

// A live X,Y plot window. The opaque handle handed to the host is this XYPlot*.
struct XYPlot {
	QMainWindow                        *win = nullptr;
	QVTKOpenGLNativeWidget             *widget = nullptr;
	vtkSmartPointer<vtkContextView>     view;
	vtkSmartPointer<vtkChartXY>         chart;
	QTreeWidget                        *objMgr = nullptr;      // Object Manager (series list)
	QTableWidget                       *dataTable = nullptr;   // Data Viewer spreadsheet
	QDockWidget                        *dataDock = nullptr;    // foldable bottom dock
	QAction                            *actLegend = nullptr;
	QAction                            *actGrid = nullptr;
	QAction                            *actDataView = nullptr;
	vtkSmartPointer<vtkCallbackCommand> moveCb;               // mouse-move coord readout
	std::vector<XYSeries>               series;
	bool                                rebuilding = false;    // guard objMgr itemChanged storms
	// X-axis time mode: 0 linear, 1 date-auto, 2 date(yyyy-mm-dd), 3 time(HH:MM), 4 decimal year,
	// 5 day-of-year. When non-zero the bottom axis ticks are formatted from epoch-seconds X and
	// regenerated on every range change (xyTicksOnRender observer).
	int                                 xTimeFmt = 0;
	double                              lastLo = 0.0, lastHi = 0.0;
	bool                                ticksBusy = false;
	vtkSmartPointer<vtkCallbackCommand> ticksCb;
};

// Live X,Y windows, keyed by the XYPlot* handed back to the host. A handle is
// valid only while its window is open (the destroyed-lambda erases it).
static std::unordered_set<XYPlot*> g_xyplots;
static bool xyAlive(XYPlot *p) { return p && g_xyplots.count(p) != 0; }

// File-menu callback into Julia (Open / Save / New). The host owns data + file IO,
// so the menu hands Julia the action + the chosen path (the C side runs the native
// QFileDialog) + the selected series index (for Save; -1 = none/all). action is
// "open" | "save" | "new". Set via gmtvtk_xyplot_set_callback; nullptr -> the menu
// item just shows a status message.
typedef void (*JuliaXYFn)(void *plot, const char *action, int sel, const char *path);
static JuliaXYFn g_juliaXY = nullptr;

// Analysis-menu callback into Julia. fn(plot, op, sel): op = the operation tag
// ("remove_mean" | "remove_trend" | "deriv1" | "deriv2" | "autocorr" | "fft_amp" |
// "fft_psd"); sel = the Object-Manager-selected series the op runs on. Julia computes
// the transform and either adds a new series here or opens a new plot window. Set via
// gmtvtk_xyplot_set_analysis_callback; nullptr -> the items show a status message.
typedef void (*JuliaXYAnaFn)(void *plot, const char *op, int sel);
static JuliaXYAnaFn g_juliaXYAna = nullptr;

// Seed callback: when an X,Y window is spawned from C++ (the Profile panel's "Open in X,Y plot
// tool"), this hands the initial (x,y) series to Julia so Julia REGISTERS a QtXYPlot mirror for
// the window + records the data. Without it a C++-spawned window has no Julia-side series, so its
// File>Save and Analysis (which route through Julia) would find nothing. fn(plot, x, y, n, name).
// nullptr (e.g. the standalone demo exe) -> openSeriesInXYTool adds the series directly instead.
typedef void (*JuliaXYSeedFn)(void *plot, const double *x, const double *y, int n, const char *name);
static JuliaXYSeedFn g_juliaXYSeed = nullptr;

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

// (Re)build the chart's plot items from the series tables. Called after a delete
// (plot indices shift, so it is simplest to clear + re-add all). Incremental adds
// add one plot directly. Re-applies style + label + visibility.
static void xyRebuildPlots(XYPlot *s) {
	s->chart->ClearPlots();
	for (auto &se : s->series) {
		vtkPlot *pl = s->chart->AddPlot(vtkChart::LINE);
		pl->SetInputData(se.table, 0, 1);
		pl->SetLabel(se.name);
		pl->SetVisible(se.visible);
		se.plot = pl;
		xyApplyStyle(se);
	}
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Rebuild the Object Manager tree from the series list (one checkable row each,
// foreground tinted to the series colour, UserRole = series index).
static void xyRebuildObjMgr(XYPlot *s) {
	if (!s->objMgr)
		return;
	s->rebuilding = true;
	s->objMgr->clear();
	for (int i = 0; i < (int)s->series.size(); ++i) {
		const XYSeries &se = s->series[i];
		QTreeWidgetItem *it = new QTreeWidgetItem(s->objMgr);
		it->setText(0, QString::fromStdString(se.name));
		it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
		it->setCheckState(0, se.visible ? Qt::Checked : Qt::Unchecked);
		it->setForeground(0, QColor((int)(se.r * 255), (int)(se.g * 255), (int)(se.b * 255)));
		it->setData(0, Qt::UserRole, i);
	}
	s->rebuilding = false;
}

// Fill the Data Viewer spreadsheet with one series' (x,y) columns.
static void xyFillDataTable(XYPlot *s, int idx) {
	if (!s->dataTable || idx < 0 || idx >= (int)s->series.size())
		return;
	const XYSeries &se = s->series[idx];
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

// Append one (x,y) series. Returns its index (or -1 on bad input). Renders. `lineType` (vtkPen),
// `marker` (vtkPlotPoints) and `markerSize` set the presentation; pass lineType<0 / marker<0 /
// markerSize<=0 for the defaults (solid line, no marker, size 7).
static int xyAddSeries(XYPlot *s, const double *x, const double *y, int n,
                       const char *name, double r, double g, double b, double width,
                       int lineType, int marker, double markerSize) {
	if (!xyAlive(s) || !x || !y || n < 1)
		return -1;
	XYSeries se;
	se.name  = (name && name[0]) ? name : ("Line " + std::to_string((int)s->series.size() + 1));
	if (r >= 0.0) {
		se.r = r; se.g = g; se.b = b;             // explicit colour
	}
	else {
		const double *c = xyPalette((int)s->series.size());   // cycle the default palette
		se.r = c[0]; se.g = c[1]; se.b = c[2];
	}
	if (width > 0.0)      se.width = width;
	if (lineType >= 0)    se.lineType = lineType;
	if (marker >= 0)      se.marker = marker;
	if (markerSize > 0.0) se.markerSize = markerSize;

	se.table = vtkSmartPointer<vtkTable>::New();
	vtkNew<vtkFloatArray> ax; ax->SetName("X");                       se.table->AddColumn(ax);
	vtkNew<vtkFloatArray> ay; ay->SetName(se.name.c_str());          se.table->AddColumn(ay);
	se.table->SetNumberOfRows(n);
	for (int i = 0; i < n; ++i) {
		se.table->SetValue(i, 0, (float)x[i]);
		se.table->SetValue(i, 1, (float)y[i]);
	}

	vtkPlot *pl = s->chart->AddPlot(vtkChart::LINE);
	pl->SetInputData(se.table, 0, 1);
	pl->SetLabel(se.name);
	se.plot = pl;
	xyApplyStyle(se);

	const int idx = (int)s->series.size();
	s->series.push_back(se);
	xyRebuildObjMgr(s);
	xyFillDataTable(s, idx);
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
	return idx;
}

// Drop every series.
static void xyClear(XYPlot *s) {
	if (!xyAlive(s))
		return;
	s->chart->ClearPlots();
	s->series.clear();
	xyRebuildObjMgr(s);
	if (s->dataTable) { s->dataTable->clearContents(); s->dataTable->setRowCount(0); }
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Delete the series at idx (rebuilds plots + the tree so indices re-sync).
static void xyDeleteSeries(XYPlot *s, int idx) {
	if (idx < 0 || idx >= (int)s->series.size())
		return;
	s->series.erase(s->series.begin() + idx);
	xyRebuildPlots(s);
	xyRebuildObjMgr(s);
	if (s->dataTable) { s->dataTable->clearContents(); s->dataTable->setRowCount(0); }
}

// Live coordinate readout: map the cursor (interactor event px, bottom-up) into
// data coords via the chart's bottom/left axes, and show "x, y" in the status bar.
static void xyMouseMove(vtkObject *caller, unsigned long, void *clientData, void *) {
	XYPlot *s = static_cast<XYPlot*>(clientData);
	vtkRenderWindowInteractor *rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
	if (!xyAlive(s) || !rwi || !s->win)
		return;
	vtkAxis *ax = s->chart->GetAxis(vtkAxis::BOTTOM);
	vtkAxis *ay = s->chart->GetAxis(vtkAxis::LEFT);
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
	s->win->statusBar()->showMessage(QString("%1,  %2").arg(dx, 0, 'g', 8).arg(dy, 0, 'g', 6));
}

// Export the current plot to a PNG.
static void xyExportPng(XYPlot *s) {
	const QString fn = QFileDialog::getSaveFileName(s->win, "Export plot as PNG", QString(), "PNG image (*.png)");
	if (fn.isEmpty())
		return;
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
	if (idx < 0 || idx >= (int)s->series.size())
		return;
	XYSeries &se = s->series[idx];

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

	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
	form->addRow(bb);
	QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
	dlg.exec();
}

// Right-click on the Object Manager: properties / rename / delete the clicked series.
static void xyObjMgrMenu(XYPlot *s, const QPoint &pos) {
	QTreeWidgetItem *it = s->objMgr->itemAt(pos);
	if (!it)
		return;
	const int idx = it->data(0, Qt::UserRole).toInt();
	QMenu m(s->objMgr);
	QAction *aProp = m.addAction("Line properties…");
	QAction *aRen = m.addAction("Rename…");
	QAction *aDel = m.addAction("Delete");
	QAction *pick = m.exec(s->objMgr->viewport()->mapToGlobal(pos));
	if (pick == aProp) {
		xyLineProperties(s, idx);
	}
	else if (pick == aDel) {
		xyDeleteSeries(s, idx);
	}
	else if (pick == aRen && idx >= 0 && idx < (int)s->series.size()) {
		bool ok = false;
		const QString nn = QInputDialog::getText(s->win, "Rename series", "Name:",
			QLineEdit::Normal, QString::fromStdString(s->series[idx].name), &ok);
		if (ok && !nn.isEmpty()) {
			s->series[idx].name = nn.toStdString();
			if (s->series[idx].plot) s->series[idx].plot->SetLabel(nn.toStdString());
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

// Dialog for Spector-Grant depth-to-sources: a frequency band [f1,f2] (0/0 = whole series) + the
// frequency unit (sets the metres conversion factor). Returns false if cancelled.
static bool xyAskSpecGrant(QWidget *parent, double &f1, double &f2, double &xf) {
	QDialog d(parent);
	d.setWindowTitle("Depth to sources (Spector-Grant)");
	QFormLayout *fo = new QFormLayout(&d);
	QDoubleSpinBox *s1 = new QDoubleSpinBox(&d); s1->setDecimals(6); s1->setRange(0.0, 1e12); s1->setValue(0.0);
	QDoubleSpinBox *s2 = new QDoubleSpinBox(&d); s2->setDecimals(6); s2->setRange(0.0, 1e12); s2->setValue(0.0);
	QComboBox *uc = new QComboBox(&d); uc->addItems(QStringList() << "1/m" << "1/km" << "1/NM");
	uc->setCurrentIndex(1);                            // spectra are usually in 1/km
	fo->addRow("Low frequency (0 = series start)", s1);
	fo->addRow("High frequency (0 = series end)",  s2);
	fo->addRow("Frequency unit", uc);
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
	fo->addRow(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
	if (d.exec() != QDialog::Accepted)
		return false;
	f1 = s1->value(); f2 = s2->value();
	xf = uc->currentIndex() == 0 ? 1.0 : uc->currentIndex() == 1 ? 1000.0 : 1852.0;
	return true;
}

// Currently-selected series in the Object Manager (-1 if none / out of range).
static int xyCurrentSel(XYPlot *s) {
	if (!s->objMgr)
		return -1;
	QTreeWidgetItem *it = s->objMgr->currentItem();
	if (!it)
		return -1;
	const int idx = it->data(0, Qt::UserRole).toInt();
	return (idx >= 0 && idx < (int)s->series.size()) ? idx : -1;
}

// ---- time-axis tick formatting ---------------------------------------------
// The X data is treated as Unix epoch SECONDS (UTC); the chosen mode only changes how the bottom
// axis tick LABELS read (no data conversion). Ticks regenerate on every range change.

// A natural time step (seconds) so ~ <=8 ticks span `span` seconds: seconds .. decade.
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
		if (span / c[i] <= 8.0) return c[i];
	return c[n - 1];
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

// Rebuild the bottom-axis custom ticks for the current range (or revert to auto when linear).
static void xyRefreshDateTicks(XYPlot *s) {
	vtkAxis *ax = s->chart->GetAxis(vtkAxis::BOTTOM);
	if (s->xTimeFmt == 0) {
		ax->SetCustomTickPositions(nullptr);
		return;
	}
	const double lo = ax->GetMinimum(), hi = ax->GetMaximum();
	if (!(hi > lo))
		return;
	const double span = hi - lo, step = xyNiceDateStep(span);
	vtkNew<vtkDoubleArray> pos;
	vtkNew<vtkStringArray> lab;
	for (double t = std::ceil(lo / step) * step; t <= hi + 0.5 * step; t += step) {
		pos->InsertNextValue(t);
		lab->InsertNextValue(std::string(xyFmtTime(t, s->xTimeFmt, span).toUtf8().constData()));
	}
	ax->SetCustomTickPositions(pos, lab);
}

// Render-end observer: when in a time mode, regenerate ticks if the axis range changed (zoom/pan),
// then re-render once. Guarded against reentrancy (the re-render fires EndEvent again with an
// unchanged range, so it returns early).
static void xyTicksOnRender(vtkObject *, unsigned long, void *clientData, void *) {
	XYPlot *s = static_cast<XYPlot*>(clientData);
	if (!xyAlive(s) || s->xTimeFmt == 0 || s->ticksBusy)
		return;
	vtkAxis *ax = s->chart->GetAxis(vtkAxis::BOTTOM);
	const double lo = ax->GetMinimum(), hi = ax->GetMaximum();
	if (!(hi > lo))
		return;
	const double tol = 1e-6 * (std::abs(hi) + std::abs(lo) + 1.0);
	if (std::abs(lo - s->lastLo) < tol && std::abs(hi - s->lastHi) < tol)
		return;
	s->lastLo = lo; s->lastHi = hi; s->ticksBusy = true;
	xyRefreshDateTicks(s);
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
	s->ticksBusy = false;
}

// Toggle log scaling on an axis (axis 0 = bottom/X, 1 = left/Y). Data must be positive for VTK to
// actually activate it. Recomputes bounds + renders.
static void xySetLog(XYPlot *s, int axis, bool on) {
	vtkAxis *ax = s->chart->GetAxis(axis == 0 ? vtkAxis::BOTTOM : vtkAxis::LEFT);
	ax->SetLogScale(on);
	s->chart->RecalculateBounds();
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Set the X-axis time mode (0 = linear, 1..5 see XYPlot::xTimeFmt) + refresh.
static void xySetXTime(XYPlot *s, int fmt) {
	s->xTimeFmt = fmt;
	s->lastLo = std::numeric_limits<double>::quiet_NaN();   // force the observer to recompute
	s->lastHi = std::numeric_limits<double>::quiet_NaN();
	xyRefreshDateTicks(s);
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// ---- window builder --------------------------------------------------------

static XYPlot *buildXYPlot(const char *title) {
	ensureApp();

	XYPlot *s = new XYPlot();
	s->win = new QMainWindow();
	s->win->setAttribute(Qt::WA_DeleteOnClose, true);
	s->win->setWindowTitle(title && title[0] ? QString::fromUtf8(title) : QString("i'GMT  —  X,Y plot"));
	s->win->setWindowIcon(appIcon());
	s->win->resize(900, 600);

	// --- central chart ---
	s->widget = new QVTKOpenGLNativeWidget();
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	s->widget->setRenderWindow(rw.Get());
	s->view = vtkSmartPointer<vtkContextView>::New();
	s->view->SetRenderWindow(rw.Get());
	s->view->SetInteractor(s->widget->interactor());
	s->view->GetRenderer()->SetBackground(1.0, 1.0, 1.0);
	s->chart = vtkSmartPointer<vtkChartXY>::New();
	s->view->GetScene()->AddItem(s->chart.Get());
	s->chart->SetShowLegend(true);
	s->chart->GetAxis(vtkAxis::BOTTOM)->SetTitle("X");   // sensible defaults; Julia overrides via set_labels
	s->chart->GetAxis(vtkAxis::LEFT)->SetTitle("Y");
	s->win->setCentralWidget(s->widget);

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
		const QString fn = QFileDialog::getOpenFileName(s->win, "Open data file", QString(),
			"Data files (*.dat *.txt *.csv *.xy *.gmt);;All files (*)");
		if (fn.isEmpty()) return;
		g_juliaXY(s, "open", -1, fn.toUtf8().constData()); });
	QObject::connect(aSave, &QAction::triggered, s->win, [s]{
		if (!g_juliaXY) { s->win->statusBar()->showMessage("Save: not wired yet", 3000); return; }
		if (s->series.empty()) { s->win->statusBar()->showMessage("Nothing to save", 3000); return; }
		const QString fn = QFileDialog::getSaveFileName(s->win, "Save series", QString(),
			"Text (*.dat *.txt);;CSV (*.csv);;All files (*)");
		if (fn.isEmpty()) return;
		g_juliaXY(s, "save", xyCurrentSel(s), fn.toUtf8().constData()); });
	QObject::connect(aExport, &QAction::triggered, s->win, [s]{ xyExportPng(s); });
	QObject::connect(aClose,  &QAction::triggered, s->win, [s]{ s->win->close(); });

	// Analysis menu — each op runs in Julia on the Object-Manager-selected series.
	QMenu *mAna = mb->addMenu("Analysis");
	auto addAna = [&](const char *label, const char *op) {
		QAction *a = mAna->addAction(label);
		QObject::connect(a, &QAction::triggered, s->win, [s, op] {
			if (!g_juliaXYAna) { s->win->statusBar()->showMessage("Analysis: not wired", 3000); return; }
			const int sel = xyCurrentSel(s);
			if (sel < 0) { s->win->statusBar()->showMessage("Select a series in the Object Manager first", 3500); return; }
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
		if (!g_juliaXYAna) { s->win->statusBar()->showMessage("Analysis: not wired", 3000); return -1; }
		const int sel = xyCurrentSel(s);
		if (sel < 0) s->win->statusBar()->showMessage("Select a series in the Object Manager first", 3500);
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
		// Spector & Grant: on a (frequency, power) spectrum, fit ln(power) over a band -> the slope
		// gives the depth to the magnetic source ensemble. Run it on a PSD/amplitude spectrum series.
		QAction *a = mAna->addAction("Depth to sources (Spector-Grant)…");
		QObject::connect(a, &QAction::triggered, s->win, [s, gate] {
			const int sel = gate(); if (sel < 0) return;
			double f1, f2, xf;
			if (!xyAskSpecGrant(s->win, f1, f2, xf)) return;
			g_juliaXYAna(s, QString("specgrant:%1:%2:%3").arg(f1, 0, 'g', 9).arg(f2, 0, 'g', 9)
			                    .arg(xf, 0, 'g', 9).toUtf8().constData(), sel);
		});
	}

	QMenu *mMisc = mb->addMenu("Misc");
	s->actLegend = mMisc->addAction("Show legend");
	s->actLegend->setCheckable(true); s->actLegend->setChecked(true);
	s->actGrid = mMisc->addAction("Grid");
	s->actGrid->setCheckable(true); s->actGrid->setChecked(true);
	s->actDataView = mMisc->addAction("Data Viewer");
	s->actDataView->setCheckable(true); s->actDataView->setChecked(false);
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
		s->chart->SetShowLegend(on); if (s->widget->renderWindow()) s->widget->renderWindow()->Render(); });
	QObject::connect(s->actGrid, &QAction::toggled, s->win, [s](bool on){
		s->chart->GetAxis(vtkAxis::BOTTOM)->SetGridVisible(on);
		s->chart->GetAxis(vtkAxis::LEFT)->SetGridVisible(on);
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
		s->chart->RecalculateBounds();
		s->chart->GetAxis(vtkAxis::BOTTOM)->SetBehavior(vtkAxis::AUTO);
		s->chart->GetAxis(vtkAxis::LEFT)->SetBehavior(vtkAxis::AUTO);
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
		const int idx = it->data(0, Qt::UserRole).toInt();
		if (idx < 0 || idx >= (int)s->series.size()) return;
		const bool on = (it->checkState(0) == Qt::Checked);
		s->series[idx].visible = on;
		if (s->series[idx].plot) s->series[idx].plot->SetVisible(on);
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

	// --- lifetime bookkeeping (shares the pump/window count with the 3-D viewer) ---
	g_xyplots.insert(s);
	g_openWindows++;
	g_lastRW = rw.Get();
	QObject::connect(s->win, &QObject::destroyed, [s]{
		g_xyplots.erase(s);
		if (g_openWindows > 0) g_openWindows--;
		delete s;                                   // struct outlives the QMainWindow; free it here
	});

	s->win->statusBar()->showMessage("X,Y plot — add series from Julia (xyplot / add!)");
	s->win->show();
	s->win->raise();
	s->win->activateWindow();
	rw->Render();
	return s;
}

// Open a standalone X,Y plot window pre-loaded with one (x,y) series. Called by the 3-D viewer's
// Profile panel ("Open in X,Y plot tool") so a Ctrl-drag elevation profile — or a downloaded tide
// series — lands in the full plotter. Returns the new window (null on <2 points / mismatch).
static XYPlot* openSeriesInXYTool(const std::vector<double>& x, const std::vector<double>& y,
                                  const char* title, const char* xlabel, const char* ylabel) {
	if (x.size() < 2 || y.size() != x.size())
		return nullptr;
	XYPlot* p = buildXYPlot(title);
	if (!p)
		return nullptr;
	if (xlabel && xlabel[0]) p->chart->GetAxis(vtkAxis::BOTTOM)->SetTitle(xlabel);
	if (ylabel && ylabel[0]) p->chart->GetAxis(vtkAxis::LEFT)->SetTitle(ylabel);
	if (g_juliaXYSeed)                                 // let Julia register a mirror + add the series
		g_juliaXYSeed(p, x.data(), y.data(), (int)x.size(), "Profile");
	else                                               // no host (demo exe) -> add directly
		xyAddSeries(p, x.data(), y.data(), (int)x.size(), "Profile", -1.0, 0.0, 0.0, 0.0, -1, -1, -1.0);
	return p;
}
