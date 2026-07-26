// ============================================================================
//  gmtedit — the MGD77 track editor. A STANDALONE QMainWindow with three
//  stacked vtkChartXY panels sharing one horizontal (distance) axis, a scroll
//  bar that slides a fixed-width window along the track, and point-level
//  editing by mouse. Port of Mirone's src_figs/gmtedit.m ("Revival of the
//  ancient Sunview gmtedit").
//
//  Like the X,Y plot tool (65_xyplot.cpp) this window is NOT a 3-D Scene: it
//  has its own struct (GmtEdit), its own C API (gmtvtk_gmtedit_*, in
//  90_c_api.cpp) and shares ONLY the QApplication + the gmtvtk_process_events
//  pump with everything else.
//
//  DIVISION OF LABOUR — this file owns the GUI and the point editing; Julia
//  (src/gmtedit.jl) owns every byte of data: reading the cruise, the outlier /
//  nav-filter / despike arithmetic, and writing the MGD77+ netCDF back. They
//  talk through ONE callback, geCall(s, action, arg), whose `arg` is a
//  ';'-joined payload — the same convention 65_xyplot.cpp and the Tiles Tool
//  already use.
//
//  EDITING MODEL (gmtedit.m):
//    • left-click a point            -> flag it BAD (green square turns red);
//                                       click it again to unflag it
//    • Shift-click a point           -> "despike" it: Julia relocates it onto
//                                       the value a cubic spline through its
//                                       neighbours predicts
//    • toolbar "Rectangular region"  -> drag a box; every point inside is flagged
//    • toolbar "Select for moving"   -> drag a box; the points inside detach into
//                                       a draggable blue segment (gmtedit.m's
//                                       h_broken + ui_edit_polygon), which the
//                                       save then merges back sorted by X
// ============================================================================

// One panel: its variable, its data, its red flags, and the VTK/Qt bits that
// draw them. `x` is per-channel because "Select for moving" may shift a segment
// ALONG the track (gmtedit.m sorts the moved X back in on save), so the three
// panels stop sharing one abscissa the moment a segment is dragged.
struct GmtEditChannel {
	std::string          var;              // netCDF variable name ("faa", "mtf1", …)
	std::string          label;            // Y-axis title
	std::vector<double>  x;                // this panel's abscissa (starts as the shared one)
	std::vector<double>  y;                // CURRENT values (despikes / drags included)
	std::vector<char>    bad;              // 1 = flagged (red)

	vtkSmartPointer<vtkTable> tGood, tBad, tMoved, tRub;
	vtkPlot                  *pGood = nullptr, *pBad = nullptr;
	vtkPlot                  *pMoved = nullptr, *pRub = nullptr;
	std::vector<vtkSmartPointer<vtkTable> > ovTables;   // overlay curves (grid track, extra variables)

	// "Select for moving" state — the detached segment and how far it has been dragged.
	std::vector<int>     movedIdx;
	bool                 dragging = false;
	double               dragX0 = 0.0, dragY0 = 0.0;

	vtkSmartPointer<vtkTable> tMark;       // the -P "start here" vertical marker
	vtkPlot                  *pMark = nullptr;

	QVTKOpenGLNativeWidget         *widget = nullptr;
	vtkSmartPointer<vtkContextView> view;
	vtkSmartPointer<vtkChartXY>     chart;
	double               yLo = 0.0, yHi = 1.0;          // fixed Y range (over ALL data, as in MATLAB)
};

struct GmtEdit {
	QMainWindow    *win = nullptr;
	GmtEditChannel  ch[3];
	int             n = 0;                 // records
	bool            xIsDist = true;
	double          widthKm = 200.0;       // gmtedit's -L: the displayed window width
	double          xMin = 0.0, xMax = 1.0;
	QScrollBar     *scroll = nullptr;
	bool            scrollBusy = false;
	QPlainTextEdit *console = nullptr;
	QToolButton    *consoleToggle = nullptr;
	int             mode = 0;              // 0 = pick points, 1 = rectangle-flag, 2 = select-for-moving,
	                                        // 3 = link (send the clicked record to the parent 3-D window)
	QAction        *actRect = nullptr, *actRectMove = nullptr, *actLink = nullptr;
	// The 3-D viewer this editor was opened from (gmtedit.m's hMirAxes). Only the link tool needs
	// it, and only to know whether it may be enabled — Julia holds its own copy of the handle and
	// does the actual plotting through add_symbols!.
	Scene          *parent = nullptr;
	QStringList     varList;               // variables the file offers (the "Plot variable" submenu)
	bool            loaded = false;
	double          markX = std::numeric_limits<double>::quiet_NaN();
	// The two modeless dialogs, kept only so the host can write results back into them
	// (the csaps-chosen p, and the "(found N)" count in the nav-filter title).
	QPointer<QLineEdit> outlierP;
	QPointer<QDialog>   navDlg;
};

// Live editor windows, keyed by the GmtEdit* handed back to the host.
static std::unordered_set<GmtEdit*> g_gmtedits;
static bool geAlive(GmtEdit *s) { return s && g_gmtedits.count(s) != 0; }

// The one callback into Julia: fn(edit, action, arg). `action` names the operation
// ("open" | "save" | "info" | "outliers" | "navfilter" | "despike" | "setvar" |
// "overlayvar" | "interp" | "toxy" | "autop" | "closed"); `arg` is its ';'-joined
// payload. Set via gmtvtk_set_gmtedit_callback; nullptr -> the action just logs.
typedef void (*JuliaGmtEditFn)(void *edit, const char *action, const char *arg);
static JuliaGmtEditFn g_juliaGmtEdit = nullptr;

static void geLog(GmtEdit *s, const QString &msg, bool isError = false);

// Fire one action at Julia. Every user gesture that needs data goes through here.
static void geCall(GmtEdit *s, const char *action, const QString &arg = QString()) {
	if (!geAlive(s))
		return;
	if (!g_juliaGmtEdit) {
		geLog(s, QString("gmtedit: no Julia callback registered (action \"%1\")").arg(action), true);
		return;
	}
	g_juliaGmtEdit(s, action, arg.toUtf8().constData());
}

// Append one timestamped line to the collapsible log panel and echo it on the status
// bar. `isError` pops the panel open so a genuine failure cannot hide.
static void geLog(GmtEdit *s, const QString &msg, bool isError) {
	if (!geAlive(s))
		return;
	if (s->console) {
		s->console->appendPlainText(QString("[%1]  %2")
			.arg(QTime::currentTime().toString("HH:mm:ss")).arg(msg));
		if (isError && s->consoleToggle && !s->consoleToggle->isChecked())
			s->consoleToggle->setChecked(true);
	}
	if (s->win)
		s->win->statusBar()->showMessage(msg, 6000);
}

// ---------------------------------------------------------------------------
//  tables / plots
// ---------------------------------------------------------------------------

static vtkSmartPointer<vtkTable> geNewTable(const char *xname, const char *yname) {
	vtkSmartPointer<vtkTable> t = vtkSmartPointer<vtkTable>::New();
	vtkNew<vtkDoubleArray> ax; ax->SetName(xname); t->AddColumn(ax);
	vtkNew<vtkDoubleArray> ay; ay->SetName(yname); t->AddColumn(ay);
	return t;
}

static void geFillTable(vtkTable *t, const std::vector<double> &x, const std::vector<double> &y,
                        const std::vector<char> &mask, char want) {
	vtkIdType k = 0;
	const vtkIdType n = (vtkIdType)std::min(x.size(), y.size());
	t->SetNumberOfRows(n);                             // upper bound; shrunk below
	for (vtkIdType i = 0; i < n; ++i) {
		if (!mask.empty() && mask[(size_t)i] != want)
			continue;
		if (!std::isfinite(y[(size_t)i]) || !std::isfinite(x[(size_t)i]))
			continue;                                   // a NaN record simply has nothing to draw
		t->SetValue(k, 0, x[(size_t)i]);
		t->SetValue(k, 1, y[(size_t)i]);
		++k;
	}
	t->SetNumberOfRows(k);
}

// Refill one panel's good/bad/moved tables from its current data and re-render.
static void geRefresh(GmtEdit *s, int slot) {
	if (!geAlive(s) || slot < 0 || slot > 2)
		return;
	GmtEditChannel &c = s->ch[slot];
	if (!c.chart)
		return;
	// The moved segment is drawn by its own plot, so it must not also appear in good/bad.
	std::vector<char> shown(c.y.size(), 0);
	for (size_t i = 0; i < c.movedIdx.size(); ++i)
		shown[(size_t)c.movedIdx[i]] = 1;

	std::vector<char> mGood(c.y.size()), mBad(c.y.size());
	for (size_t i = 0; i < c.y.size(); ++i) {
		const bool isBad = (i < c.bad.size() && c.bad[i] != 0);
		mGood[i] = (!isBad && !shown[i]) ? 1 : 0;
		mBad[i]  = ( isBad && !shown[i]) ? 1 : 0;
	}
	geFillTable(c.tGood, c.x, c.y, mGood, 1);
	geFillTable(c.tBad,  c.x, c.y, mBad,  1);

	c.tMoved->SetNumberOfRows((vtkIdType)c.movedIdx.size());
	for (size_t k = 0; k < c.movedIdx.size(); ++k) {
		const size_t i = (size_t)c.movedIdx[k];
		c.tMoved->SetValue((vtkIdType)k, 0, c.x[i]);
		c.tMoved->SetValue((vtkIdType)k, 1, std::isfinite(c.y[i]) ? c.y[i] : 0.0);
	}
	if (c.pGood)  c.pGood->SetInputData(c.tGood, 0, 1);
	if (c.pBad)   c.pBad->SetInputData(c.tBad, 0, 1);
	// A LINE plot with fewer than 2 rows makes vtkContext2D log an error on EVERY render, so the
	// three occasional line plots (moved segment, rubber band, -P marker) only exist while they
	// have something to draw.
	if (c.movedIdx.empty()) {
		if (c.pMoved) { c.chart->RemovePlotInstance(c.pMoved); c.pMoved = nullptr; }
	}
	else {
		if (!c.pMoved) {
			c.pMoved = c.chart->AddPlot(vtkChart::LINE);
			c.pMoved->SetColor(0, 0, 230, 255);
			c.pMoved->SetWidth(1.2f);
			static_cast<vtkPlotLine*>(c.pMoved)->SetMarkerStyle(vtkPlotPoints::SQUARE);
			static_cast<vtkPlotLine*>(c.pMoved)->SetMarkerSize(5.0f);
		}
		c.pMoved->SetInputData(c.tMoved, 0, 1);
	}
	if (c.widget && c.widget->renderWindow())
		c.widget->renderWindow()->Render();
}

// Fixed Y range over ALL the channel's data (MATLAB autoscales an axes over the whole
// line, not over the visible window, and gmtedit.m never overrides that).
static void geAutoY(GmtEdit *s, int slot) {
	GmtEditChannel &c = s->ch[slot];
	double lo = 0.0, hi = 0.0;
	bool any = false;
	for (size_t i = 0; i < c.y.size(); ++i) {
		if (!std::isfinite(c.y[i]))
			continue;
		if (!any) { lo = hi = c.y[i]; any = true; }
		else      { lo = std::min(lo, c.y[i]); hi = std::max(hi, c.y[i]); }
	}
	if (!any) { lo = 0.0; hi = 1.0; }
	if (hi <= lo) hi = lo + 1.0;
	const double pad = 0.04 * (hi - lo);
	c.yLo = lo - pad; c.yHi = hi + pad;
	if (c.chart) {
		vtkAxis *ay = c.chart->GetAxis(vtkAxis::LEFT);
		ay->SetBehavior(vtkAxis::FIXED);
		ay->SetMinimum(c.yLo); ay->SetMaximum(c.yHi);
		ay->RecalculateTickSpacing();
	}
}

// Put all three bottom axes on [lo, lo+widthKm] (the scrolled view).
static void geSetXWindow(GmtEdit *s, double lo) {
	const double hi = lo + s->widthKm;
	for (int k = 0; k < 3; ++k) {
		if (!s->ch[k].chart)
			continue;
		vtkAxis *ax = s->ch[k].chart->GetAxis(vtkAxis::BOTTOM);
		ax->SetBehavior(vtkAxis::FIXED);
		ax->SetMinimum(lo); ax->SetMaximum(hi);
		ax->RecalculateTickSpacing();
		if (s->ch[k].widget && s->ch[k].widget->renderWindow())
			s->ch[k].widget->renderWindow()->Render();
	}
}

// The scroll bar carries hundredths of a km so a 200 km window still slides smoothly.
static const double GE_SCROLL_SCALE = 100.0;

static void geSyncScroll(GmtEdit *s) {
	if (!s->scroll)
		return;
	const double span = s->xMax - s->xMin;
	s->scrollBusy = true;
	if (span <= s->widthKm) {
		s->scroll->setRange(0, 0);
		s->scroll->setValue(0);
		s->scroll->setEnabled(false);
		geSetXWindow(s, s->xMin);
	}
	else {
		s->scroll->setEnabled(true);
		s->scroll->setRange((int)(s->xMin * GE_SCROLL_SCALE),
		                    (int)((s->xMax - s->widthKm) * GE_SCROLL_SCALE));
		// gmtedit.m gives a 10% overlap between successive pages (its SliderStep 0.9*width).
		s->scroll->setSingleStep((int)(0.10 * s->widthKm * GE_SCROLL_SCALE));
		s->scroll->setPageStep((int)(0.90 * s->widthKm * GE_SCROLL_SCALE));
		const int v = std::min(s->scroll->value(), s->scroll->maximum());
		s->scroll->setValue(v);
		geSetXWindow(s, v / GE_SCROLL_SCALE);
	}
	s->scrollBusy = false;
}

// ---------------------------------------------------------------------------
//  pixel <-> data mapping (same technique as xySGDataX, both axes)
// ---------------------------------------------------------------------------

static QPointF geDataPos(GmtEdit *s, int slot, const QPointF &p) {
	GmtEditChannel &c = s->ch[slot];
	const double dpr = c.widget->devicePixelRatioF();
	const double px  = p.x() * dpr;
	const double py  = (c.widget->height() - p.y()) * dpr;    // VTK's origin is bottom-left
	vtkAxis *ax = c.chart->GetAxis(vtkAxis::BOTTOM);
	vtkAxis *ay = c.chart->GetAxis(vtkAxis::LEFT);
	float *a1 = ax->GetPoint1(), *a2 = ax->GetPoint2();
	float *b1 = ay->GetPoint1(), *b2 = ay->GetPoint2();
	const double w = a2[0] - a1[0], h = b2[1] - b1[1];
	const double fx = (w == 0.0) ? 0.0 : (px - a1[0]) / w;
	const double fy = (h == 0.0) ? 0.0 : (py - b1[1]) / h;
	return QPointF(ax->GetMinimum() + fx * (ax->GetMaximum() - ax->GetMinimum()),
	               ay->GetMinimum() + fy * (ay->GetMaximum() - ay->GetMinimum()));
}

// Data -> device pixels, for the "is this click near that point?" test.
static QPointF gePixPos(GmtEdit *s, int slot, double x, double y) {
	GmtEditChannel &c = s->ch[slot];
	vtkAxis *ax = c.chart->GetAxis(vtkAxis::BOTTOM);
	vtkAxis *ay = c.chart->GetAxis(vtkAxis::LEFT);
	float *a1 = ax->GetPoint1(), *a2 = ax->GetPoint2();
	float *b1 = ay->GetPoint1(), *b2 = ay->GetPoint2();
	const double sx = ax->GetMaximum() - ax->GetMinimum();
	const double sy = ay->GetMaximum() - ay->GetMinimum();
	const double fx = (sx == 0.0) ? 0.0 : (x - ax->GetMinimum()) / sx;
	const double fy = (sy == 0.0) ? 0.0 : (y - ay->GetMinimum()) / sy;
	return QPointF(a1[0] + fx * (a2[0] - a1[0]), b1[1] + fy * (b2[1] - b1[1]));
}

// Index of the plotted point nearest the click, or -1 when nothing is close enough.
// gmtedit.m does this in normalised data units with a hard-wired 6:1 factor "to
// compensate the ~6:1 horizontal/vertical axes dimension" and a 0.04 cut-off — i.e. it
// is approximating a distance in SCREEN space with the aspect ratio its own figure
// happened to have. This window's panels have their own aspect, so the same test is
// done directly in device pixels, which is what that fudge was standing in for.
static const double GE_PICK_RADIUS_PX = 14.0;

static int gePickPoint(GmtEdit *s, int slot, const QPointF &pos) {
	GmtEditChannel &c = s->ch[slot];
	vtkAxis *ax = c.chart->GetAxis(vtkAxis::BOTTOM);
	const double xlo = ax->GetMinimum(), xhi = ax->GetMaximum();
	const double dpr = c.widget->devicePixelRatioF();
	const QPointF pt(pos.x() * dpr, (c.widget->height() - pos.y()) * dpr);
	const double lim = GE_PICK_RADIUS_PX * dpr;
	int best = -1;
	double bestd = lim * lim;
	for (size_t i = 0; i < c.y.size(); ++i) {
		if (!std::isfinite(c.y[i]) || !std::isfinite(c.x[i]))
			continue;
		if (c.x[i] < xlo || c.x[i] > xhi)               // only what is on screen
			continue;
		const QPointF q = gePixPos(s, slot, c.x[i], c.y[i]);
		const double dx = q.x() - pt.x(), dy = q.y() - pt.y();
		const double d2 = dx * dx + dy * dy;
		if (d2 < bestd) { bestd = d2; best = (int)i; }
	}
	return best;
}

// ---------------------------------------------------------------------------
//  the rubber band (drawn as a chart plot, so it composites over the GL widget)
// ---------------------------------------------------------------------------

static void geSetRubber(GmtEdit *s, int slot, double x0, double y0, double x1, double y1, bool on) {
	GmtEditChannel &c = s->ch[slot];
	if (!on) {
		if (c.pRub) { c.chart->RemovePlotInstance(c.pRub); c.pRub = nullptr; }
	}
	else {
		if (!c.pRub) {
			c.pRub = c.chart->AddPlot(vtkChart::LINE);
			c.pRub->SetColor(0, 0, 0, 255);
			c.pRub->GetPen()->SetLineType(vtkPen::DASH_LINE);
			c.pRub->SetWidth(1.0f);
		}
		const double xs[5] = { x0, x1, x1, x0, x0 };
		const double ys[5] = { y0, y0, y1, y1, y0 };
		c.tRub->SetNumberOfRows(5);
		for (int i = 0; i < 5; ++i) {
			c.tRub->SetValue(i, 0, xs[i]);
			c.tRub->SetValue(i, 1, ys[i]);
		}
		c.pRub->SetInputData(c.tRub, 0, 1);
	}
	if (c.widget && c.widget->renderWindow())
		c.widget->renderWindow()->Render();
}

// ---------------------------------------------------------------------------
//  the editing gestures
// ---------------------------------------------------------------------------

// Toggle one point's red flag (gmtedit.m's add_MarkColor: a second click on an
// already-red point removes the marker instead of adding a duplicate).
static void geToggleFlag(GmtEdit *s, int slot, int idx) {
	GmtEditChannel &c = s->ch[slot];
	if (idx < 0 || idx >= (int)c.bad.size())
		return;
	c.bad[(size_t)idx] = c.bad[(size_t)idx] ? 0 : 1;
	geRefresh(s, slot);
}

// Flag every point inside the dragged box (gmtedit.m's rectang_clickedCB).
static void geFlagRect(GmtEdit *s, int slot, double x0, double y0, double x1, double y1) {
	if (x0 > x1) std::swap(x0, x1);
	if (y0 > y1) std::swap(y0, y1);
	GmtEditChannel &c = s->ch[slot];
	int hits = 0;
	for (size_t i = 0; i < c.y.size(); ++i) {
		if (!std::isfinite(c.y[i]) || !std::isfinite(c.x[i]))
			continue;
		if (c.x[i] < x0 || c.x[i] > x1 || c.y[i] < y0 || c.y[i] > y1)
			continue;
		c.bad[i] = 1; ++hits;
	}
	geRefresh(s, slot);
	geLog(s, QString("Flagged %1 point(s) in %2").arg(hits).arg(QString::fromStdString(c.var)));
}

// Detach every point inside the dragged box into the movable segment
// (gmtedit.m's rectangMove_clickedCB -> handles.h_broken + ui_edit_polygon).
static void geSelectForMoving(GmtEdit *s, int slot, double x0, double y0, double x1, double y1) {
	if (x0 > x1) std::swap(x0, x1);
	if (y0 > y1) std::swap(y0, y1);
	GmtEditChannel &c = s->ch[slot];
	c.movedIdx.clear();
	for (size_t i = 0; i < c.y.size(); ++i) {
		if (!std::isfinite(c.y[i]) || !std::isfinite(c.x[i]))
			continue;
		if (c.x[i] < x0 || c.x[i] > x1 || c.y[i] < y0 || c.y[i] > y1)
			continue;
		c.movedIdx.push_back((int)i);
	}
	geRefresh(s, slot);
	geLog(s, c.movedIdx.empty()
		? QString("Nothing inside the rectangle to move")
		: QString("%1 point(s) detached — drag them with the left button").arg(c.movedIdx.size()));
}

// Is the click on the detached segment (so a left-drag should move it)?
static bool geOnMoved(GmtEdit *s, int slot, const QPointF &pos) {
	GmtEditChannel &c = s->ch[slot];
	if (c.movedIdx.empty())
		return false;
	const double dpr = c.widget->devicePixelRatioF();
	const QPointF pt(pos.x() * dpr, (c.widget->height() - pos.y()) * dpr);
	const double lim = 18.0 * dpr;
	for (size_t k = 0; k < c.movedIdx.size(); ++k) {
		const size_t i = (size_t)c.movedIdx[k];
		const QPointF q = gePixPos(s, slot, c.x[i], c.y[i]);
		const double dx = q.x() - pt.x(), dy = q.y() - pt.y();
		if (dx * dx + dy * dy <= lim * lim)
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
//  chart mouse handling
// ---------------------------------------------------------------------------

class GmtEditFilter : public QObject {
public:
	GmtEdit *s = nullptr;
	int      slot = 0;
	GmtEditFilter(GmtEdit *sc, int sl, QObject *parent) : QObject(parent), s(sc), slot(sl) {}
protected:
	bool  banding = false;
	QPointF p0;
	double  d0x = 0.0, d0y = 0.0;

	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (!geAlive(s) || !s->ch[slot].chart)
			return QObject::eventFilter(obj, ev);
		const QEvent::Type t = ev->type();

		if (t == QEvent::MouseButtonPress) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (me->button() != Qt::LeftButton)
				return QObject::eventFilter(obj, ev);
			const QPointF d = geDataPos(s, slot, me->position());

			if (s->mode == 3) {
				// Link tool: the click identifies a RECORD, which Julia plots on the parent 3-D
				// window at its own lon/lat (gmtedit.m's bdn_ptcoords, which reuses add_MarkColor
				// purely to get the index of the clicked point).
				const int idx = gePickPoint(s, slot, me->position());
				if (idx >= 0)
					geCall(s, "pickpt", QString("%1;%2").arg(slot + 1).arg(idx));
				return true;
			}
			if (s->mode == 0) {
				// A left-press on the detached segment starts a drag; anything else picks a point.
				if (geOnMoved(s, slot, me->position())) {
					s->ch[slot].dragging = true;
					s->ch[slot].dragX0 = d.x(); s->ch[slot].dragY0 = d.y();
					s->ch[slot].widget->setCursor(Qt::SizeAllCursor);
					return true;
				}
				const int idx = gePickPoint(s, slot, me->position());
				if (idx < 0)
					return true;
				if (me->modifiers() & Qt::ShiftModifier)
					geCall(s, "despike", QString("%1;%2").arg(slot + 1).arg(idx));
				else
					geToggleFlag(s, slot, idx);
				return true;
			}
			// modes 1 (flag) and 2 (select for moving) both start a rubber band
			banding = true; p0 = me->position(); d0x = d.x(); d0y = d.y();
			geSetRubber(s, slot, d0x, d0y, d0x, d0y, true);
			return true;
		}

		if (t == QEvent::MouseMove) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (banding) {
				const QPointF d = geDataPos(s, slot, me->position());
				geSetRubber(s, slot, d0x, d0y, d.x(), d.y(), true);
				return true;
			}
			if (s->ch[slot].dragging) {
				GmtEditChannel &c = s->ch[slot];
				const QPointF d = geDataPos(s, slot, me->position());
				const double dx = d.x() - c.dragX0, dy = d.y() - c.dragY0;
				for (size_t k = 0; k < c.movedIdx.size(); ++k) {
					const size_t i = (size_t)c.movedIdx[k];
					c.x[i] += dx;
					if (std::isfinite(c.y[i])) c.y[i] += dy;
				}
				c.dragX0 = d.x(); c.dragY0 = d.y();
				geRefresh(s, slot);
				return true;
			}
			return QObject::eventFilter(obj, ev);
		}

		if (t == QEvent::MouseButtonRelease) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (me->button() != Qt::LeftButton)
				return QObject::eventFilter(obj, ev);
			if (s->ch[slot].dragging) {
				s->ch[slot].dragging = false;
				s->ch[slot].widget->unsetCursor();
				return true;
			}
			if (!banding)
				return QObject::eventFilter(obj, ev);
			banding = false;
			const QPointF d = geDataPos(s, slot, me->position());
			geSetRubber(s, slot, 0, 0, 0, 0, false);
			if (std::abs(me->position().x() - p0.x()) < 3 && std::abs(me->position().y() - p0.y()) < 3)
				return true;                            // a click, not a box
			if (s->mode == 1)
				geFlagRect(s, slot, d0x, d0y, d.x(), d.y());
			else
				geSelectForMoving(s, slot, d0x, d0y, d.x(), d.y());
			return true;
		}
		return QObject::eventFilter(obj, ev);
	}
};

// ---------------------------------------------------------------------------
//  dialogs (gmtedit.m builds these three with uicontrol; they have no .ui file)
// ---------------------------------------------------------------------------

// gmtedit_outliersdetect — smoothing parameter, threshold, channel radios, Apply /
// Apply n return / Clear. Modeless, so it can stay up while the user looks at the result.
static void geOutliersDialog(GmtEdit *s) {
	QDialog *dlg = new QDialog(s->win);
	dlg->setWindowTitle("Detect outliers");
	dlg->setAttribute(Qt::WA_DeleteOnClose, true);

	QLineEdit *edP = new QLineEdit("1");
	edP->setValidator(new QDoubleValidator(0.0, 1.0, 12, edP));
	edP->setToolTip("Enter a Smoothing Parameter between [0 1]");
	QLineEdit *edT = new QLineEdit("4");
	edT->setValidator(new QDoubleValidator(0.0, 1e12, 6, edT));
	edT->setToolTip("Residues greater or equal than this are outliers.\n"
	                "Notice that we use small numbers because the spline\n"
	                "smoothing will do only a mild smoothing, so the residues\n"
	                "are naturally small. Unless you decrease the p parameter");

	QRadioButton *rb[3];
	QButtonGroup *grp = new QButtonGroup(dlg);
	for (int k = 0; k < 3; ++k) {
		rb[k] = new QRadioButton(QString::fromStdString(s->ch[k].var.empty()
			? std::string(1, (char)('1' + k)) : s->ch[k].var));
		rb[k]->setToolTip(QString("Select the %1 channel").arg(QString::fromStdString(s->ch[k].label)));
		grp->addButton(rb[k], k);
	}
	// gmtedit.m's priority: the first channel that actually has data.
	int first = 0;
	for (int k = 0; k < 3; ++k) {
		bool any = false;
		for (size_t i = 0; i < s->ch[k].y.size() && !any; ++i)
			any = std::isfinite(s->ch[k].y[i]);
		rb[k]->setEnabled(any);
		if (any && first == 0) first = k + 1;
	}
	rb[first > 0 ? first - 1 : 0]->setChecked(true);

	// The default p is the one csaps itself would pick — asked of Julia for the selected
	// channel (gmtedit.m calls csaps once per channel just to harvest that estimate).
	QObject::connect(grp, &QButtonGroup::idClicked, dlg, [s](int id) {
		geCall(s, "autop", QString::number(id + 1));
	});
	s->outlierP = edP;                                  // where gmtvtk_gmtedit_set_autop writes
	geCall(s, "autop", QString::number(first > 0 ? first : 1));

	QPushButton *bApply = new QPushButton("Apply");
	bApply->setToolTip("Use this for testing");
	QPushButton *bApplyGo = new QPushButton("Apply n return");
	bApplyGo->setToolTip("Do it and go away");
	QPushButton *bClear = new QPushButton("Clear");
	bClear->setToolTip("Clear detections from current selected channel");
	for (QPushButton *b : { bApply, bApplyGo, bClear })
		b->setAutoDefault(false), b->setDefault(false);

	auto run = [s, grp, edP, edT]() {
		const int slot = grp->checkedId() < 0 ? 0 : grp->checkedId();
		geCall(s, "outliers", QString("%1;%2;%3").arg(slot + 1).arg(edP->text()).arg(edT->text()));
	};
	QObject::connect(bApply,   &QPushButton::clicked, dlg, run);
	QObject::connect(bApplyGo, &QPushButton::clicked, dlg, [dlg, run]() { run(); dlg->close(); });
	QObject::connect(bClear,   &QPushButton::clicked, dlg, [s, grp]() {
		const int slot = grp->checkedId() < 0 ? 0 : grp->checkedId();
		std::fill(s->ch[slot].bad.begin(), s->ch[slot].bad.end(), (char)0);
		geRefresh(s, slot);
	});

	QGridLayout *g = new QGridLayout(dlg);
	g->addWidget(new QLabel("Smoothing parameter (p)"), 0, 0);
	g->addWidget(edP, 1, 0);
	g->addWidget(new QLabel("Threshold"), 0, 1);
	g->addWidget(edT, 1, 1);
	g->addWidget(bApply, 1, 2);
	QHBoxLayout *hr = new QHBoxLayout();
	for (int k = 0; k < 3; ++k) hr->addWidget(rb[k]);
	hr->addStretch(1);
	g->addLayout(hr, 2, 0, 1, 2);
	QHBoxLayout *hb = new QHBoxLayout();
	hb->addWidget(bClear); hb->addWidget(bApplyGo);
	g->addLayout(hb, 2, 2);
	dlg->show();
}

// gmtedit_NavFilters — max/min speed and max gradient, Clean / Apply. Modeless.
static void geNavFiltersDialog(GmtEdit *s) {
	QDialog *dlg = new QDialog(s->win);
	dlg->setWindowTitle("Speed and Slope filter");
	dlg->setAttribute(Qt::WA_DeleteOnClose, true);
	s->navDlg = dlg;                                    // where gmtvtk_gmtedit_set_navfound writes

	QLineEdit *edMax = new QLineEdit("15");   edMax->setToolTip("Flag speeds higher than this");
	QLineEdit *edMin = new QLineEdit("1");
	QLineEdit *edSlp = new QLineEdit("250");
	for (QLineEdit *e : { edMax, edMin, edSlp })
		e->setValidator(new QDoubleValidator(0.0, 1e9, 6, e)), e->setMaximumWidth(60);

	// gmtedit.m operates this only on the magnetic channel (the middle axes).
	QComboBox *cbSlot = new QComboBox();
	for (int k = 0; k < 3; ++k)
		cbSlot->addItem(QString::fromStdString(s->ch[k].var.empty() ? "-" : s->ch[k].var));
	cbSlot->setCurrentIndex(1);

	QPushButton *bClean = new QPushButton("Clean");
	QPushButton *bApply = new QPushButton("Apply");
	bClean->setAutoDefault(false); bClean->setDefault(false);
	bApply->setAutoDefault(false); bApply->setDefault(false);

	QObject::connect(bApply, &QPushButton::clicked, dlg, [s, cbSlot, edMin, edMax, edSlp]() {
		geCall(s, "navfilter", QString("%1;%2;%3;%4").arg(cbSlot->currentIndex() + 1)
			.arg(edMin->text()).arg(edMax->text()).arg(edSlp->text()));
	});
	QObject::connect(bClean, &QPushButton::clicked, dlg, [s, cbSlot, dlg]() {
		const int slot = cbSlot->currentIndex();
		std::fill(s->ch[slot].bad.begin(), s->ch[slot].bad.end(), (char)0);
		geRefresh(s, slot);
		dlg->setWindowTitle("Speed and Slope filter");
	});

	QGridLayout *g = new QGridLayout(dlg);
	g->addWidget(edMax, 0, 0); g->addWidget(new QLabel("Max speed (knots)"), 0, 1);
	g->addWidget(edMin, 0, 2); g->addWidget(new QLabel("Min speed"), 0, 3);
	g->addWidget(bClean, 0, 4);
	g->addWidget(edSlp, 1, 0); g->addWidget(new QLabel("Max Slope (nT/km)"), 1, 1);
	g->addWidget(cbSlot, 1, 2); g->addWidget(new QLabel("Channel"), 1, 3);
	g->addWidget(bApply, 1, 4);
	dlg->show();
}

// gmtedit_track — "Grid to sample along track coords" + an optional IGRF, overlaid on
// the panel the context menu was opened on.
static void geTrackFromGridDialog(GmtEdit *s, int slot) {
	QDialog *dlg = new QDialog(s->win);
	dlg->setWindowTitle("Track from grid");
	dlg->setAttribute(Qt::WA_DeleteOnClose, true);

	QLineEdit *ed = new QLineEdit();
	ed->setToolTip("Name (full name) of grid to sample along track coords");
	QPushButton *bBrowse = new QPushButton("...");
	bBrowse->setFixedWidth(28);
	QCheckBox *ck = new QCheckBox("Add IGRF");
	ck->setToolTip("For magnetic anomalies only, add an IGRF to interpolation");
	QPushButton *bOK = new QPushButton("OK");
	for (QPushButton *b : { bBrowse, bOK })
		b->setAutoDefault(false), b->setDefault(false);

	QObject::connect(bBrowse, &QPushButton::clicked, dlg, [dlg, ed]() {
		QString fn = QFileDialog::getOpenFileName(dlg, "Select GMT grid", prefStartDir(),
			"Grid files (*.grd *.GRD *.nc *.NC);;All Files (*.*)");
		if (!fn.isEmpty()) { rememberStartDir(fn); ed->setText(fn); }
	});
	QObject::connect(bOK, &QPushButton::clicked, dlg, [s, slot, ed, ck, dlg]() {
		if (ed->text().isEmpty())
			return;
		geCall(s, "interp", QString("%1;%2;%3").arg(slot + 1).arg(ed->text()).arg(ck->isChecked() ? 1 : 0));
		dlg->close();
	});

	QGridLayout *g = new QGridLayout(dlg);
	g->addWidget(new QLabel("Grid to sample along track coords"), 0, 0, 1, 2);
	g->addWidget(ed, 1, 0); g->addWidget(bBrowse, 1, 1);
	g->addWidget(ck, 2, 0); g->addWidget(bOK, 2, 1);
	dlg->resize(360, 90);
	dlg->show();
}

// ---------------------------------------------------------------------------
//  per-panel context menu (gmtedit.m's uicontextmenu on the three axes)
// ---------------------------------------------------------------------------

static void geContextMenu(GmtEdit *s, int slot, const QPoint &globalPos) {
	QMenu m;
	m.addAction("Overlay interpolation", [s, slot]() { geTrackFromGridDialog(s, slot); });
	QMenu *mOv = m.addMenu("Overlay another variable");
	QMenu *mPl = m.addMenu("Plot variable");
	for (const QString &v : s->varList) {
		mOv->addAction(v, [s, slot, v]() { geCall(s, "overlayvar", QString("%1;%2").arg(slot + 1).arg(v)); });
		QAction *a = mPl->addAction(v, [s, slot, v]() { geCall(s, "setvar", QString("%1;%2").arg(slot + 1).arg(v)); });
		a->setCheckable(true);
		a->setChecked(v == QString::fromStdString(s->ch[slot].var));
	}
	if (s->varList.isEmpty()) { mOv->setEnabled(false); mPl->setEnabled(false); }
	QMenu *mXY = m.addMenu("Show in XY grapher");
	mXY->addAction("All data", [s, slot]() { geCall(s, "toxy", QString("%1;all").arg(slot + 1)); });
	mXY->addAction("This window data", [s, slot]() {
		vtkAxis *ax = s->ch[slot].chart->GetAxis(vtkAxis::BOTTOM);
		geCall(s, "toxy", QString("%1;window;%2;%3").arg(slot + 1)
			.arg(ax->GetMinimum(), 0, 'g', 12).arg(ax->GetMaximum(), 0, 'g', 12));
	});
	m.addSeparator();
	m.addAction("Clear red flags on this panel", [s, slot]() {
		std::fill(s->ch[slot].bad.begin(), s->ch[slot].bad.end(), (char)0);
		geRefresh(s, slot);
	});
	if (!s->ch[slot].movedIdx.empty()) {
		m.addAction("Release the moved segment", [s, slot]() {
			s->ch[slot].movedIdx.clear();
			geRefresh(s, slot);
		});
	}
	m.exec(globalPos);
}

// ---------------------------------------------------------------------------
//  building the window
// ---------------------------------------------------------------------------

static GmtEdit *buildGmtEdit(const char *title, double widthKm) {
	ensureApp();

	GmtEdit *s = new GmtEdit();
	s->widthKm = (widthKm > 0.0) ? widthKm : 200.0;
	s->win = new QMainWindow();
	s->win->setAttribute(Qt::WA_DeleteOnClose, true);
	s->win->setWindowTitle(title && title[0] ? QString::fromUtf8(title) : QString("gmtedit"));
	s->win->setWindowIcon(appIcon());
	s->win->resize(1200, 860);                          // three stacked panels want height

	QWidget     *central = new QWidget();
	QVBoxLayout *lay     = new QVBoxLayout(central);
	lay->setContentsMargins(2, 2, 2, 2);
	lay->setSpacing(3);

	static const char *defLabels[3] = { "Gravity anomaly (mGal)", "Magnetic field (nT)", "Bathymetry (m)" };
	for (int k = 0; k < 3; ++k) {
		GmtEditChannel &c = s->ch[k];
		c.label  = defLabels[k];
		c.widget = new QVTKOpenGLNativeWidget();
		c.widget->setMinimumHeight(150);
		vtkNew<vtkGenericOpenGLRenderWindow> rw;
		c.widget->setRenderWindow(rw.Get());
		c.view = vtkSmartPointer<vtkContextView>::New();
		c.view->SetRenderWindow(rw.Get());
		c.view->SetInteractor(c.widget->interactor());
		c.view->GetRenderer()->SetBackground(1.0, 1.0, 1.0);
		c.chart = vtkSmartPointer<vtkChartXY>::New();
		// The chart's own zoom/pan would fight the scroll bar (which owns the X window),
		// so the interactive actions are turned off; editing owns the mouse instead.
		c.chart->SetActionToButton(vtkChart::PAN,    -1);
		c.chart->SetActionToButton(vtkChart::ZOOM,   -1);
		c.chart->SetActionToButton(vtkChart::SELECT, -1);
		c.chart->SetShowLegend(false);
		c.view->GetScene()->AddItem(c.chart);
		c.chart->GetAxis(vtkAxis::LEFT)->SetTitle(c.label);
		c.chart->GetAxis(vtkAxis::BOTTOM)->SetTitle(k == 2 ? "Distance (km)" : "");

		c.tGood  = geNewTable("X", "good");
		c.tBad   = geNewTable("X", "bad");
		c.tMoved = geNewTable("X", "moved");
		c.tRub   = geNewTable("X", "rubber");

		// gmtedit.m: green square markers, red for the de-activated ones, blue for a
		// detached ("broken") segment; no connecting line on the data itself.
		c.pGood = c.chart->AddPlot(vtkChart::POINTS);
		c.pGood->SetColor(0, 170, 0, 255);
		static_cast<vtkPlotPoints*>(c.pGood)->SetMarkerStyle(vtkPlotPoints::SQUARE);
		static_cast<vtkPlotPoints*>(c.pGood)->SetMarkerSize(5.0f);
		c.pGood->SetInputData(c.tGood, 0, 1);

		c.pBad = c.chart->AddPlot(vtkChart::POINTS);
		c.pBad->SetColor(220, 0, 0, 255);
		static_cast<vtkPlotPoints*>(c.pBad)->SetMarkerStyle(vtkPlotPoints::SQUARE);
		static_cast<vtkPlotPoints*>(c.pBad)->SetMarkerSize(5.0f);
		c.pBad->SetInputData(c.tBad, 0, 1);

		// pMoved / pRub / pMark are created on demand (see geRefresh / geSetRubber / geSetMark):
		// an empty LINE plot logs a vtkContext2D error on every single render.
		c.tMark = geNewTable("X", "mark");

		c.widget->installEventFilter(new GmtEditFilter(s, k, c.widget));
		c.widget->setContextMenuPolicy(Qt::CustomContextMenu);
		QObject::connect(c.widget, &QWidget::customContextMenuRequested, s->win,
			[s, k](const QPoint &p) { geContextMenu(s, k, s->ch[k].widget->mapToGlobal(p)); });

		lay->addWidget(c.widget, 1);
	}

	// --- the scroll bar that slides the fixed-width window along the track ---
	s->scroll = new QScrollBar(Qt::Horizontal);
	s->scroll->setEnabled(false);
	QObject::connect(s->scroll, &QScrollBar::valueChanged, s->win, [s](int v) {
		if (!s->scrollBusy)
			geSetXWindow(s, v / GE_SCROLL_SCALE);
	});
	lay->addWidget(s->scroll, 0);

	// --- collapsible log panel (same "pop open on an error" behaviour as the X,Y tool) ---
	s->console = new QPlainTextEdit();
	s->console->setReadOnly(true);
	s->console->setMaximumBlockCount(2000);
	s->console->setMaximumHeight(120);
	s->console->setFont(QFont("Consolas", 9));
	s->console->setVisible(false);
	s->consoleToggle = new QToolButton();
	s->consoleToggle->setText("Messages");
	s->consoleToggle->setCheckable(true);
	s->consoleToggle->setAutoRaise(true);
	s->consoleToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	s->consoleToggle->setArrowType(Qt::RightArrow);
	QObject::connect(s->consoleToggle, &QToolButton::toggled, s->win, [s](bool on) {
		s->console->setVisible(on);
		s->consoleToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
	});
	lay->addWidget(s->consoleToggle, 0);
	lay->addWidget(s->console, 0);

	s->win->setCentralWidget(central);
	s->win->statusBar()->showMessage("Click a point to flag it bad; click it again to undo. Shift-click despikes.");

	// --- toolbar (gmtedit.m's uipushtools, same order and tooltips) ---
	QToolBar *tb = s->win->addToolBar("gmtedit");
	tb->setIconSize(QSize(20, 20));
	QStyle *st = s->win->style();

	QAction *aOpen = tb->addAction(st->standardIcon(QStyle::SP_DialogOpenButton), "Open gmt file");
	QObject::connect(aOpen, &QAction::triggered, s->win, [s]() {
		QString fn = QFileDialog::getOpenFileName(s->win, "Select gmt File", prefStartDir(),
			"Cruise files (*.nc *.NC *.gmt *.GMT);;MGD77+ netCDF (*.nc *.NC);;"
			"Legacy *.gmt binary (*.gmt *.GMT);;All Files (*.*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		geCall(s, "open", fn);
	});
	QAction *aSave = tb->addAction(st->standardIcon(QStyle::SP_DialogSaveButton), "Save gmt file");
	QObject::connect(aSave, &QAction::triggered, s->win, [s]() {
		if (!s->loaded) { geLog(s, "Nothing loaded to save", true); return; }
		geCall(s, "save", QString());                   // empty path = save in place
	});
	// gmtedit.m's `force_gmt`: write the cruise out in the legacy pre-MGD77 *.gmt binary, converting
	// an MGD77+ one on the way (mtf1 - 40000 nT, time re-based to the cruise year). The .m carries
	// the whole branch but never sets the flag from its own GUI — this is that branch's button.
	QAction *aSaveGmt = tb->addAction(QString("→.gmt"));
	aSaveGmt->setToolTip("Save as old *.gmt file (legacy pre-MGD77 binary)");
	QObject::connect(aSaveGmt, &QAction::triggered, s->win, [s]() {
		if (!s->loaded) { geLog(s, "Nothing loaded to save", true); return; }
		QString fn = QFileDialog::getSaveFileName(s->win, "Save as old .gmt file", prefStartDir(),
			"Legacy *.gmt binary (*.gmt *.GMT);;All Files (*.*)");
		if (fn.isEmpty()) return;
		if (!fn.endsWith(".gmt", Qt::CaseInsensitive)) fn += ".gmt";
		rememberStartDir(fn);
		geCall(s, "saveas_gmt", fn);
	});
	QAction *aInfo = tb->addAction(st->standardIcon(QStyle::SP_MessageBoxInformation), "Cruise Info");
	QObject::connect(aInfo, &QAction::triggered, s->win, [s]() { geCall(s, "info"); });

	tb->addSeparator();
	s->actRect = tb->addAction(QString("▭"));
	s->actRect->setToolTip("Rectangular region — drag a box to flag every point inside it");
	s->actRect->setCheckable(true);
	s->actRectMove = tb->addAction(QString("▭→"));
	s->actRectMove->setToolTip("Select for moving — drag a box, then drag the detached points");
	s->actRectMove->setCheckable(true);
	QObject::connect(s->actRect, &QAction::toggled, s->win, [s](bool on) {
		s->mode = on ? 1 : 0;
		if (on && s->actRectMove->isChecked()) s->actRectMove->setChecked(false);
	});
	QObject::connect(s->actRectMove, &QAction::toggled, s->win, [s](bool on) {
		s->mode = on ? 2 : 0;
		if (on && s->actRect->isChecked()) s->actRect->setChecked(false);
	});
	// Link tool (gmtedit.m's ptcoords toggle + its hand-drawn link icon): pick a point in a curve
	// and plot it on the 3-D window this editor came from. Only meaningful with a parent, so it
	// starts disabled and geSetParent enables it.
	s->actLink = tb->addAction(QString("⛓"));
	s->actLink->setToolTip("Pick a data point in a curve and plot it on the parent 3-D window");
	s->actLink->setCheckable(true);
	s->actLink->setEnabled(false);
	QObject::connect(s->actLink, &QAction::toggled, s->win, [s](bool on) {
		s->mode = on ? 3 : 0;
		if (on) {
			if (s->actRect->isChecked())     s->actRect->setChecked(false);
			if (s->actRectMove->isChecked()) s->actRectMove->setChecked(false);
		}
		for (int k = 0; k < 3; ++k)
			if (s->ch[k].widget) s->ch[k].widget->setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
	});

	tb->addSeparator();
	QAction *aIn  = tb->addAction(QString("+"));
	aIn->setToolTip("Increase scale (narrower window)");
	QAction *aOut = tb->addAction(QString("−"));
	aOut->setToolTip("Decrease scale (wider window)");
	QObject::connect(aIn, &QAction::triggered, s->win, [s]() {
		// gmtedit.m: 50 km steps, never below 25 km.
		s->widthKm = std::max(s->widthKm - 50.0, 25.0);
		geSyncScroll(s);
	});
	QObject::connect(aOut, &QAction::triggered, s->win, [s]() {
		s->widthKm += 50.0;
		geSyncScroll(s);
	});

	tb->addSeparator();
	QAction *aOut2 = tb->addAction(QString("Outliers"));
	aOut2->setToolTip("Outliers detector");
	QObject::connect(aOut2, &QAction::triggered, s->win, [s]() {
		if (!s->loaded) { geLog(s, "Load a cruise first", true); return; }
		geOutliersDialog(s);
	});
	QAction *aNav = tb->addAction(QString("Nav/Grad"));
	aNav->setToolTip("Find Nav/Grad troubles");
	QObject::connect(aNav, &QAction::triggered, s->win, [s]() {
		if (!s->loaded) { geLog(s, "Load a cruise first", true); return; }
		geNavFiltersDialog(s);
	});

	g_gmtedits.insert(s);
	// The editor is a real top-level window: it must count towards g_openWindows or the Julia
	// pump (eventloop.jl, which stops as soon as gmtvtk_process_events reports 0) would leave it
	// unpumped and dead to the mouse.
	g_openWindows++;
	QObject::connect(s->win, &QObject::destroyed, [s]() {
		if (g_openWindows > 0) g_openWindows--;
		// Every child widget is already gone here, so tell Julia directly rather than through
		// geCall (whose failure path would touch the destroyed status bar / log panel).
		s->win = nullptr; s->console = nullptr; s->consoleToggle = nullptr; s->scroll = nullptr;
		if (g_juliaGmtEdit)
			g_juliaGmtEdit(s, "closed", "");
		g_gmtedits.erase(s);
		delete s;
	});
	s->win->show();
	return s;
}

// ---------------------------------------------------------------------------
//  host-side setters (the bodies of the gmtvtk_gmtedit_* exports)
// ---------------------------------------------------------------------------

static void geSetX(GmtEdit *s, const double *x, int n, bool isDist) {
	if (!geAlive(s) || !x || n <= 0)
		return;
	s->n = n;
	s->xIsDist = isDist;
	s->xMin = s->xMax = x[0];
	for (int i = 1; i < n; ++i) {
		if (x[i] < s->xMin) s->xMin = x[i];
		if (x[i] > s->xMax) s->xMax = x[i];
	}
	if (!(s->xMax > s->xMin))
		s->xMax = s->xMin + 1.0;
	for (int k = 0; k < 3; ++k) {
		s->ch[k].x.assign(x, x + n);
		s->ch[k].movedIdx.clear();
		if ((int)s->ch[k].y.size() != n) {
			s->ch[k].y.assign((size_t)n, std::numeric_limits<double>::quiet_NaN());
			s->ch[k].bad.assign((size_t)n, 0);
		}
		if (s->ch[k].chart)
			s->ch[k].chart->GetAxis(vtkAxis::BOTTOM)->SetTitle(isDist ? "Distance (km)" : "Record #");
	}
	geSyncScroll(s);
	for (int k = 0; k < 3; ++k)
		geRefresh(s, k);
}

static void geSetChannel(GmtEdit *s, int slot, const char *var, const char *label,
                         const double *y, int n) {
	if (!geAlive(s) || slot < 0 || slot > 2 || !y || n <= 0)
		return;
	GmtEditChannel &c = s->ch[slot];
	const std::string newVar = var ? var : "";
	// Refreshing the SAME variable (a nav despike rebuilt the speed column, say) must not throw
	// away the red flags the user has already painted; switching variables must.
	const bool keepFlags = (newVar == c.var) && ((int)c.bad.size() == n);
	c.var   = newVar;
	c.label = label ? label : c.var;
	c.y.assign(y, y + n);
	if (!keepFlags) {
		c.bad.assign((size_t)n, 0);
		c.movedIdx.clear();
	}
	if ((int)c.x.size() != n)                          // a channel arriving before set_x
		c.x.resize((size_t)n, std::numeric_limits<double>::quiet_NaN());
	if (c.chart)
		c.chart->GetAxis(vtkAxis::LEFT)->SetTitle(c.label);
	geAutoY(s, slot);
	geRefresh(s, slot);
	s->loaded = true;
}

static void geAddOverlay(GmtEdit *s, int slot, const double *x, const double *y, int n,
                         const char *name, double r, double g, double b, double width) {
	if (!geAlive(s) || slot < 0 || slot > 2 || !x || !y || n < 2)   // a 1-point LINE plot errors
		return;
	GmtEditChannel &c = s->ch[slot];
	vtkSmartPointer<vtkTable> t = geNewTable("X", name && name[0] ? name : "overlay");
	t->SetNumberOfRows(n);
	for (int i = 0; i < n; ++i) {
		t->SetValue(i, 0, x[i]);
		t->SetValue(i, 1, std::isfinite(y[i]) ? y[i] : std::numeric_limits<double>::quiet_NaN());
	}
	c.ovTables.push_back(t);
	vtkPlot *p = c.chart->AddPlot(vtkChart::LINE);
	p->SetColor((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
	p->SetWidth((float)(width > 0 ? width : 1.5));
	p->SetInputData(t, 0, 1);
	// An overlay must not silently rescale the channel it is compared against, but it
	// does have to be visible, so widen the fixed Y range when it falls outside.
	double lo = c.yLo, hi = c.yHi;
	for (int i = 0; i < n; ++i) {
		if (!std::isfinite(y[i])) continue;
		lo = std::min(lo, y[i]); hi = std::max(hi, y[i]);
	}
	if (lo < c.yLo || hi > c.yHi) {
		c.yLo = lo; c.yHi = hi;
		vtkAxis *ay = c.chart->GetAxis(vtkAxis::LEFT);
		ay->SetMinimum(c.yLo); ay->SetMaximum(c.yHi);
		ay->RecalculateTickSpacing();
	}
	if (c.widget && c.widget->renderWindow())
		c.widget->renderWindow()->Render();
}

// The permutation that orders a channel by its CURRENT x. Identity unless a segment was
// dragged along-track; gmtedit.m's save does exactly this (`sort([x_m x_broken])`, then
// reorders y by the returned index) before writing the records out.
static std::vector<int> geOrder(const GmtEditChannel &c) {
	std::vector<int> ord(c.x.size());
	for (size_t i = 0; i < ord.size(); ++i)
		ord[i] = (int)i;
	if (c.movedIdx.empty())
		return ord;
	std::stable_sort(ord.begin(), ord.end(), [&c](int a, int b) { return c.x[(size_t)a] < c.x[(size_t)b]; });
	return ord;
}

// Replace one panel's red flags wholesale (the outlier detector / nav filter results).
static void geSetFlags(GmtEdit *s, int slot, const int *flags, int n) {
	if (!geAlive(s) || slot < 0 || slot > 2 || !flags)
		return;
	GmtEditChannel &c = s->ch[slot];
	const int m = std::min<int>(n, (int)c.bad.size());
	std::fill(c.bad.begin(), c.bad.end(), (char)0);
	for (int i = 0; i < m; ++i)
		c.bad[(size_t)i] = flags[i] ? 1 : 0;
	geRefresh(s, slot);
}

// Hand the flags back in the order geOrder defines (see there: gmtedit.m sorts a dragged
// segment back into place before writing, and the flags must follow the same permutation).
static int geGetFlags(GmtEdit *s, int slot, int *out, int n) {
	if (!geAlive(s) || slot < 0 || slot > 2 || !out)
		return 0;
	const GmtEditChannel &c = s->ch[slot];
	if (n < (int)c.bad.size())
		return 0;
	const std::vector<int> ord = geOrder(c);
	for (size_t i = 0; i < ord.size(); ++i)
		out[i] = c.bad[(size_t)ord[i]] ? 1 : 0;
	return (int)ord.size();
}

// The CURRENT values of one panel (despikes and drags included), in the same order.
static int geGetChannel(GmtEdit *s, int slot, double *out, int n) {
	if (!geAlive(s) || slot < 0 || slot > 2 || !out)
		return 0;
	const GmtEditChannel &c = s->ch[slot];
	if (n < (int)c.y.size())
		return 0;
	const std::vector<int> ord = geOrder(c);
	for (size_t i = 0; i < ord.size(); ++i)
		out[i] = c.y[(size_t)ord[i]];
	return (int)ord.size();
}

// Move one point onto a new value (the despike result Julia computed).
static void geSetPoint(GmtEdit *s, int slot, int idx, double y) {
	if (!geAlive(s) || slot < 0 || slot > 2)
		return;
	GmtEditChannel &c = s->ch[slot];
	if (idx < 0 || idx >= (int)c.y.size())
		return;
	c.y[(size_t)idx] = y;
	geRefresh(s, slot);
}

// gmtedit's -P: draw a vertical marker at `x` on all three panels and scroll so the
// window opens centred on it (gmtedit.m's `begin == 0` branch).
static void geSetMark(GmtEdit *s, double x) {
	if (!geAlive(s) || !std::isfinite(x))
		return;
	s->markX = x;
	for (int k = 0; k < 3; ++k) {
		GmtEditChannel &c = s->ch[k];
		if (!c.chart)
			continue;
		if (!c.pMark) {
			c.pMark = c.chart->AddPlot(vtkChart::LINE);
			c.pMark->SetColor(0, 0, 0, 255);
			c.pMark->SetWidth(1.0f);
		}
		c.tMark->SetNumberOfRows(2);
		c.tMark->SetValue(0, 0, x); c.tMark->SetValue(0, 1, c.yLo);
		c.tMark->SetValue(1, 0, x); c.tMark->SetValue(1, 1, c.yHi);
		c.pMark->SetInputData(c.tMark, 0, 1);
	}
	if (s->scroll && s->scroll->isEnabled()) {
		const int v = (int)((x - s->widthKm / 2.0) * GE_SCROLL_SCALE);
		s->scroll->setValue(std::max(s->scroll->minimum(), std::min(v, s->scroll->maximum())));
	}
	for (int k = 0; k < 3; ++k)
		geRefresh(s, k);
}

// Attach (or detach, with nullptr) the 3-D viewer this editor was opened from, and tell Julia the
// same handle so its "pickpt" action knows where to stamp the marker. The Scene* travels as text
// because the one callback carries a single string payload; it is the SAME opaque handle Julia
// already keys its own figure registry on, not a new kind of value.
static void geSetParent(GmtEdit *s, void *scene) {
	if (!geAlive(s))
		return;
	s->parent = static_cast<Scene*>(scene);
	if (s->actLink)
		s->actLink->setEnabled(s->parent != nullptr);
	geCall(s, "setparent", s->parent ? QString("0x%1").arg((quintptr)scene, 0, 16) : QString("0"));
}

// The file's variable names (comma-separated), for the "Plot variable" / "Overlay another
// variable" context submenus.
static void geSetVarList(GmtEdit *s, const char *csv) {
	if (!geAlive(s))
		return;
	s->varList.clear();
	if (!csv || !csv[0])
		return;
	for (const QString &v : QString::fromUtf8(csv).split(',', Qt::SkipEmptyParts))
		s->varList << v.trimmed();
}

// ---------------------------------------------------------------------------
//  a small read-only report window (Cruise Info)
// ---------------------------------------------------------------------------

static void geShowMessage(GmtEdit *s, const char *title, const char *text) {
	if (!geAlive(s))
		return;
	QDialog *dlg = new QDialog(s->win);
	dlg->setAttribute(Qt::WA_DeleteOnClose, true);
	dlg->setWindowTitle(title && title[0] ? QString::fromUtf8(title) : "Info");
	QPlainTextEdit *te = new QPlainTextEdit(QString::fromUtf8(text ? text : ""));
	te->setReadOnly(true);
	te->setFont(QFont("Consolas", 9));
	te->setLineWrapMode(QPlainTextEdit::NoWrap);
	QPushButton *b = new QPushButton("Close");
	b->setAutoDefault(false); b->setDefault(false);
	QObject::connect(b, &QPushButton::clicked, dlg, &QDialog::close);
	QVBoxLayout *v = new QVBoxLayout(dlg);
	v->addWidget(te, 1);
	QHBoxLayout *h = new QHBoxLayout();
	h->addStretch(1); h->addWidget(b);
	v->addLayout(h, 0);
	dlg->resize(660, 480);
	dlg->show();
}
