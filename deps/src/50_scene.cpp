static void rebuildSceneObjects(Scene *s);          // defined just below; refreshed after edits
static void symbolLayerMenu(Scene *s, vtkActor *act, const QPoint& gp);   // symbol-layer properties (defined below)
static void toggleShadingFold(Scene *s);            // defined in 70_window.cpp (FoldTitleBar complete there)
static void textApplyProps(Scene *s, TextLabel& tl); // 85_polygon.cpp: re-apply font fields to the actor
static void deleteSlipGroup(Scene *s, const QString& groupName); // 85_polygon.cpp: delete all patches in a slip model
static void deleteMecaGroup(Scene *s, const QString& groupName); // 85_polygon.cpp: delete a focal-mechanism batch
static void mecaGroupPropsDialog(Scene *s, const QString& groupName, const QPoint& gp); // defined below
static void showInfoText(QWidget *parent, const QString &title, const QString &text); // 70_window.cpp: read-only text popup

// Append one execution-error line to a window's read-only "Errors" tab and raise it (so a failure in
// a background op is VISIBLE in the window, not just on the REPL's stderr). Shared by the
// gmtvtk_log_error export and the fire-and-forget g_juliaEval callers below. Best-effort / no-throw.
static void sceneLogError(Scene *s, const QString& msg) {
	if (!s || !sceneAlive(s) || !s->errConsole) return;
	s->errConsole->appendPlainText(QString("[%1]  %2")
		.arg(QTime::currentTime().toString("HH:mm:ss")).arg(msg));
	if (s->bottomTabs) {
		int idx = s->bottomTabs->indexOf(s->errConsole);
		if (idx >= 0) s->bottomTabs->setCurrentIndex(idx);   // pop the Errors tab to the front
	}
}

// Hand a colormap NAME back to the Julia host, which recomputes CPT nodes over the surface's data
// range and ccalls gmtvtk_set_cpt to recolour live. `fig` is bound to THIS window by _console_eval.
// A NEGATIVE return flags a Julia error (|n| bytes of text) -> route it to the Errors tab.
static void applyColormap(Scene *s, const QString& name, int gridSel) {
	if (!s || !g_juliaEval || name.isEmpty()) return;
	// Recolour the grid the Color Bar row belongs to (gridSel is the grid's UNIQUE GROUP TAG: -1 = base
	// relief, >0 = a dropped grid's tag). Resolve BY TAG (never by index — indices shift on delete) and
	// hand Julia THAT grid's own z range so the CPT spans the right grid, not the first one.
	double zmn = s->zmin, zmx = s->zmax;
	if (gridSel >= 0)
		for (auto& ex : s->extras)
			if (!ex.isImage && ex.tag == gridSel) { zmn = ex.zmin; zmx = ex.zmax; break; }
	char rng[80];
	snprintf(rng, sizeof rng, "%.17g, %.17g, %d", zmn, zmx, gridSel);
	std::string cmd = "InteractiveGMT._recolor_grid(fig, \"" + name.toStdString() + "\", " + rng + ")";
	std::vector<char> buf(1 << 12);
	int n = g_juliaEval(s, cmd.c_str(), buf.data(), (int)buf.size());
	if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));
}

// ===== COLORBAR (scalar bar strip + our own ticks/numbers) ===============================
// vtkScalarBarActor draws ONLY the coloured strip; the tick marks and numbers are our own 2D
// actors (VTK 9.6's strip has no tick geometry and its built-in labels overlap a narrow bar).
// All three are positioned from s->barX0/barY0 so the whole assembly toggles + drags as a unit.
namespace cbar {
	constexpr double W = 0.06, H = 0.40;     // frame size (normalized viewport)
	constexpr double BARRATIO = 0.30;        // coloured strip = right 30% of W
	constexpr double TICKLEN = 0.006;        // tick length (points LEFT from the strip edge)
	constexpr double LABELGAP = 0.004;       // gap between a tick's outer end and its number
}

// Reposition the existing colorbar actors from s->barX0/barY0 — called on build and each drag
// step. Rewrites coords in place; never recreates actors.
static void layoutColorbar(Scene *s) {
	if (!s || !s->bar) return;
	const double X0 = s->barX0, Y0 = s->barY0;
	const double barLeft = X0 + cbar::W * (1.0 - cbar::BARRATIO);
	s->bar->SetPosition(X0, Y0);
	s->bar->SetWidth(cbar::W); s->bar->SetHeight(cbar::H);
	const double lo = s->barLo, hi = s->barHi;       // active grid's display range (not the base zmin/zmax)
	const double span = (hi > lo) ? (hi - lo) : 1.0;
	for (size_t i = 0; i < s->barValues.size(); ++i) {
		const double frac = (s->barValues[i] - lo) / span;       // 0 at bottom (zmin) .. 1 at top
		const double y = Y0 + frac * cbar::H;
		if (s->barTickPts) {
			s->barTickPts->SetPoint(2*i,   barLeft,                y, 0.0);
			s->barTickPts->SetPoint(2*i+1, barLeft - cbar::TICKLEN, y, 0.0);
		}
		if (i < s->barLabels.size() && s->barLabels[i])
			s->barLabels[i]->SetPosition(barLeft - cbar::TICKLEN - cbar::LABELGAP, y);
	}
	if (s->barTickPts) s->barTickPts->Modified();
}

// Build the colorbar actors and add them to the renderer, for the colour map `lut` over [lo,hi]
// (the active grid's range — may differ from the base relief's zmin/zmax). Stores the range in
// barLo/barHi so layoutColorbar places ticks against it.
static void buildColorbar(Scene *s, vtkScalarsToColors *lut, double lo, double hi) {
	if (!s || s->imageOnly) return;          // bare image -> no colorbar
	if (!(hi > lo)) hi = lo + 1.0;           // guard a degenerate (flat) grid
	s->barLo = lo; s->barHi = hi;
	s->bar = vtkSmartPointer<vtkScalarBarActor>::New();
	s->bar->SetLookupTable(lut);
	s->bar->SetTitle("");                    // drop the big 'Z' title
	s->bar->SetDrawTickLabels(false);        // WE draw the numbers — kill the overlapping built-ins
	s->bar->SetTextPositionToPrecedeScalarBar();
	s->bar->SetBarRatio(cbar::BARRATIO);
	// Without this, vtkScalarBarActor still auto-shrinks the PAINTED swatch inside its own
	// Position/Height box to leave internal room for label text metrics -- even with DrawTickLabels
	// off -- so the coloured strip does not reach the actor's own top/bottom edge (the strip visibly
	// stops short of the 0 tick our OWN layoutColorbar draws at the true edge). Forces the strip to
	// fill the full declared height.
	s->bar->SetUnconstrainedFontSize(true);
	s->ren->AddActor2D(s->bar);

	// Nice round tick values (800, 900, ...) at a constant 1/2/5 x10^n step.
	const double step = niceNum(niceNum(hi - lo, false) / 5.0, true);
	// Decimal places to print: driven by the tick STEP, not hard-coded. A relief grid (step 100/1000)
	// prints integers; a tsunami water range (step 0.02/0.05 m) prints "0.02", not "0". The old fixed
	// "%.0f" collapsed every fractional tick to "0"/"-0" -- the all-zero colourbar users kept hitting.
	int decimals = 0;
	if (step > 0) {
		const double d = std::floor(std::log10(step));
		if (d < 0) decimals = std::min(6, (int)(-d));
	}
	const double qscale = std::pow(10.0, decimals);
	s->barValues.clear(); s->barLabels.clear();
	s->barTickPts = vtkSmartPointer<vtkPoints>::New();
	vtkNew<vtkCellArray> tlines;
	for (double v = std::ceil(lo / step) * step; v <= hi + 1e-9 * (hi - lo); v += step) {
		s->barValues.push_back(v);
		vtkIdType a = s->barTickPts->InsertNextPoint(0, 0, 0);   // real coords set by layoutColorbar
		vtkIdType b = s->barTickPts->InsertNextPoint(0, 0, 0);
		tlines->InsertNextCell(2); tlines->InsertCellPoint(a); tlines->InsertCellPoint(b);
		// Snap a tick that rounds to zero at this precision to +0 so it never prints "-0".
		double vv = (std::llround(v * qscale) == 0) ? 0.0 : v;
		char buf[32]; snprintf(buf, sizeof buf, "%.*f", decimals, vv);
		vtkSmartPointer<vtkTextActor> ta = vtkSmartPointer<vtkTextActor>::New();
		ta->SetInput(buf);
		ta->GetTextProperty()->SetColor(0.9, 0.9, 0.9);
		ta->GetTextProperty()->SetFontSize(10);
		ta->GetTextProperty()->SetJustificationToRight();
		ta->GetTextProperty()->SetVerticalJustificationToCentered();
		ta->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
		s->ren->AddActor2D(ta);
		s->barLabels.push_back(ta);
	}
	vtkNew<vtkPolyData> tpd; tpd->SetPoints(s->barTickPts); tpd->SetLines(tlines);
	vtkNew<vtkCoordinate> nc; nc->SetCoordinateSystemToNormalizedViewport();
	vtkNew<vtkPolyDataMapper2D> tmap; tmap->SetInputData(tpd); tmap->SetTransformCoordinate(nc);
	s->barTicks = vtkSmartPointer<vtkActor2D>::New();
	s->barTicks->SetMapper(tmap);
	s->barTicks->GetProperty()->SetColor(0.9, 0.9, 0.9);
	s->barTicks->GetProperty()->SetLineWidth(1.5);
	s->ren->AddActor2D(s->barTicks);

	layoutColorbar(s);
}

// Aquamoto's LAND colorbar: a SEPARATE, persistent bar for the (static, file-open-time) bathymetry
// range -- `bar` above serves as the WATER bar (built the normal way by showLayerImageTail). Only
// one of the two is ever visible at once (see refreshGridColorbar), so this shares the water bar's
// screen position (barX0/barY0) rather than tracking its own. Kept as its own small function rather
// than parameterizing buildColorbar/layoutColorbar -- those are threaded through a dozen call sites
// (drag, colormap chooser, session save/restore) that all assume exactly ONE bar; this is a
// genuinely different lifecycle (built once, never retargeted to "the active grid").
static void layoutAquaLandColorbar(Scene *s) {
	if (!s || !s->aquaLandBar) return;
	const double X0 = s->barX0, Y0 = s->barY0;   // shared position: never shown at the same time as `bar`
	const double barLeft = X0 + cbar::W * (1.0 - cbar::BARRATIO);
	s->aquaLandBar->SetPosition(X0, Y0);
	s->aquaLandBar->SetWidth(cbar::W); s->aquaLandBar->SetHeight(cbar::H);
	const double lo = s->aquaLandBarLo, hi = s->aquaLandBarHi;
	const double span = (hi > lo) ? (hi - lo) : 1.0;
	for (size_t i = 0; i < s->aquaLandBarValues.size(); ++i) {
		const double frac = (s->aquaLandBarValues[i] - lo) / span;
		const double y = Y0 + frac * cbar::H;
		if (s->aquaLandBarTickPts) {
			s->aquaLandBarTickPts->SetPoint(2*i,   barLeft,                y, 0.0);
			s->aquaLandBarTickPts->SetPoint(2*i+1, barLeft - cbar::TICKLEN, y, 0.0);
		}
		if (i < s->aquaLandBarLabels.size() && s->aquaLandBarLabels[i])
			s->aquaLandBarLabels[i]->SetPosition(barLeft - cbar::TICKLEN - cbar::LABELGAP, y);
	}
	if (s->aquaLandBarTickPts) s->aquaLandBarTickPts->Modified();
}

// Built ONCE when an Aquamoto file opens (bathymetry range is static -- never re-scanned per
// slice). Mirrors buildColorbar's tick generation exactly.
static void buildAquaLandColorbar(Scene *s, vtkScalarsToColors *lut, double lo, double hi) {
	if (!s) return;
	if (s->aquaLandBar)      { s->ren->RemoveActor2D(s->aquaLandBar); s->aquaLandBar = nullptr; }
	if (s->aquaLandBarTicks) { s->ren->RemoveActor2D(s->aquaLandBarTicks); s->aquaLandBarTicks = nullptr; }
	for (auto& ta : s->aquaLandBarLabels) if (ta) s->ren->RemoveActor2D(ta);
	s->aquaLandBarLabels.clear(); s->aquaLandBarValues.clear(); s->aquaLandBarTickPts = nullptr;
	if (!(hi > lo)) hi = lo + 1.0;
	s->aquaLandBarLo = lo; s->aquaLandBarHi = hi;
	s->aquaLandBar = vtkSmartPointer<vtkScalarBarActor>::New();
	s->aquaLandBar->SetLookupTable(lut);
	s->aquaLandBar->SetTitle("");
	s->aquaLandBar->SetDrawTickLabels(false);
	s->aquaLandBar->SetTextPositionToPrecedeScalarBar();
	s->aquaLandBar->SetBarRatio(cbar::BARRATIO);
	s->aquaLandBar->SetUnconstrainedFontSize(true);   // see buildColorbar -- stops the painted strip
	                                                   // shrinking inside its own Position/Height box
	s->aquaLandBar->SetVisibility(0);   // hidden until refreshGridColorbar decides it should show
	s->ren->AddActor2D(s->aquaLandBar);

	const double step = niceNum(niceNum(hi - lo, false) / 5.0, true);
	int decimals = 0;
	if (step > 0) {
		const double d = std::floor(std::log10(step));
		if (d < 0) decimals = std::min(6, (int)(-d));
	}
	const double qscale = std::pow(10.0, decimals);
	vtkNew<vtkCellArray> tlines;
	s->aquaLandBarTickPts = vtkSmartPointer<vtkPoints>::New();
	// LAND bar MUST show a tick at exactly `lo` (sea level, 0) -- ceil(lo/step)*step can drift off lo
	// by floating-point noise and silently skip it. Build the value list explicitly: lo first, then
	// the regular step grid strictly above it (skip a regular tick that lands on/near lo already).
	std::vector<double> ticks;
	ticks.push_back(lo);
	for (double v = std::ceil(lo / step) * step; v <= hi + 1e-9 * (hi - lo); v += step) {
		if (v <= lo + 1e-9 * std::max(1.0, hi - lo)) continue;   // don't duplicate the forced `lo` tick
		ticks.push_back(v);
	}
	for (double v : ticks) {
		s->aquaLandBarValues.push_back(v);
		vtkIdType a = s->aquaLandBarTickPts->InsertNextPoint(0, 0, 0);
		vtkIdType b = s->aquaLandBarTickPts->InsertNextPoint(0, 0, 0);
		tlines->InsertNextCell(2); tlines->InsertCellPoint(a); tlines->InsertCellPoint(b);
		double vv = (std::llround(v * qscale) == 0) ? 0.0 : v;
		char buf[32]; snprintf(buf, sizeof buf, "%.*f", decimals, vv);
		vtkSmartPointer<vtkTextActor> ta = vtkSmartPointer<vtkTextActor>::New();
		ta->SetInput(buf);
		ta->GetTextProperty()->SetColor(0.9, 0.9, 0.9);
		ta->GetTextProperty()->SetFontSize(10);
		ta->GetTextProperty()->SetJustificationToRight();
		ta->GetTextProperty()->SetVerticalJustificationToCentered();
		ta->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
		s->ren->AddActor2D(ta);
		s->aquaLandBarLabels.push_back(ta);
	}
	vtkNew<vtkPolyData> tpd; tpd->SetPoints(s->aquaLandBarTickPts); tpd->SetLines(tlines);
	vtkNew<vtkCoordinate> nc; nc->SetCoordinateSystemToNormalizedViewport();
	vtkNew<vtkPolyDataMapper2D> tmap; tmap->SetInputData(tpd); tmap->SetTransformCoordinate(nc);
	s->aquaLandBarTicks = vtkSmartPointer<vtkActor2D>::New();
	s->aquaLandBarTicks->SetMapper(tmap);
	s->aquaLandBarTicks->GetProperty()->SetColor(0.9, 0.9, 0.9);
	s->aquaLandBarTicks->GetProperty()->SetLineWidth(1.5);
	s->aquaLandBarTicks->SetVisibility(0);
	s->ren->AddActor2D(s->aquaLandBarTicks);

	layoutAquaLandColorbar(s);
}

static void setAquaLandColorbarVisible(Scene *s, bool on) {
	if (!s || !s->aquaLandBar) return;
	s->aquaLandBar->SetVisibility(on ? 1 : 0);
	if (s->aquaLandBarTicks) s->aquaLandBarTicks->SetVisibility(on ? 1 : 0);
	for (auto& ta : s->aquaLandBarLabels) if (ta) ta->SetVisibility(on ? 1 : 0);
}

// Show/hide the WHOLE colorbar (strip + ticks + numbers) together. The old code toggled only the
// strip, so the numbers kept floating after the strip vanished.
static void setColorbarVisible(Scene *s, bool on) {
	if (!s || !s->bar) return;
	s->bar->SetVisibility(on ? 1 : 0);
	if (s->barTicks) s->barTicks->SetVisibility(on ? 1 : 0);
	for (auto& ta : s->barLabels) if (ta) ta->SetVisibility(on ? 1 : 0);
}

static bool colorbarVisible(Scene *s) { return s && s->bar && s->bar->GetVisibility() != 0; }
// Aquamoto's LAND bar (see layoutAquaLandColorbar) shares the SAME barX0/barY0 frame as `s->bar` —
// only one of the two is ever visible at once — so drag hit-testing must treat either as "the bar".
static bool aquaLandColorbarVisible(Scene *s) { return s && s->aquaLandBar && s->aquaLandBar->GetVisibility() != 0; }

// --- colorbar left-drag, handled at the GLView widget level (60_profile.cpp), exactly like the
// polygon-vertex / text-label drags. Qt delivers the press to the widget BEFORE VTK's interactor
// adapter, so a VTK observer would lose the race to the trackball — the widget path is the only
// reliable one in this codebase. nx/ny are NORMALIZED viewport coords (bottom-up, 0..1).
static bool colorbarHit(Scene *s, double nx, double ny) {   // cursor over the (visible) bar frame?
	if (!s || (!colorbarVisible(s) && !aquaLandColorbarVisible(s))) return false;
	const double L = s->barX0 - 0.05, R = s->barX0 + cbar::W;   // include the numbers to the left
	const double B = s->barY0 - 0.02, T = s->barY0 + cbar::H + 0.02;
	return nx >= L && nx <= R && ny >= B && ny <= T;
}
static bool colorbarGrab(Scene *s, double nx, double ny) {
	if (!colorbarHit(s, nx, ny)) return false;
	s->barDragging = true;
	s->barGrabX = nx - s->barX0;
	s->barGrabY = ny - s->barY0;
	return true;
}
static bool colorbarDragTo(Scene *s, double nx, double ny) {
	if (!s || !s->barDragging) return false;
	s->barX0 = std::min(std::max(nx - s->barGrabX, 0.05), 1.0 - cbar::W);
	s->barY0 = std::min(std::max(ny - s->barGrabY, 0.0),  1.0 - cbar::H);
	layoutColorbar(s);              // repositions s->bar (water/grid) -- harmless no-op if hidden
	layoutAquaLandColorbar(s);      // repositions the LAND bar too -- whichever is visible moves
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return true;
}
static bool colorbarRelease(Scene *s) {
	if (!s || !s->barDragging) return false;
	s->barDragging = false;
	return true;
}

// The colormap chooser: a popup of common GMT master CPTs (applied on click) plus a Custom… entry
// for any name GMT's makecpt accepts. Opened from the colorbar's Scene Objects row. `apply` is what
// a picked name DOES — the generic case recolours one grid via applyColormap (see the colorbarRow
// callers below); Aquamoto's water/land rows (aquaWaterColorbarRow/aquaLandColorbarRow) pass a
// different `apply` since their colour comes from a host-composited texture, not a scalar+LUT
// surface, and needs its own Julia entry point + a slice re-render (g_aquamotoSetCmap).
static void chooseColormap(Scene *s, const QPoint& gp, std::function<void(const QString&)> apply) {
	if (!s || !apply) return;
	static const char *kMaps[] = {
		"viridis", "turbo", "jet", "hot", "haxby", "geo", "relief", "rainbow",
		"polar", "seis", "gray", "plasma", "magma", "cividis", "roma", "vik",
	};
	QMenu m(s->win);
	for (const char *nm : kMaps) {
		const QString q = QString::fromLatin1(nm);
		m.addAction(q, [apply, q]() { apply(q); });
	}
	m.addSeparator();
	m.addAction("Custom…", [s, apply]() {
		bool ok = false;
		const QString nm = QInputDialog::getText(s->win, "Colormap", "GMT CPT name:",
		                                         QLineEdit::Normal, "", &ok);
		if (ok) apply(nm.trimmed());
	});
	m.exec(gp);
}

// Shared string/font/colour editor for a text label — the ONE dialog both the Text tool's
// PLACEMENT (polyPlaceText, 85_polygon.cpp — string/font/size/colour/bold/italic chosen up front,
// not just left at TextLabel's bare defaults) and an EXISTING label's right-click "Text
// Properties…" (textPropsDialog, below) use, never two forked copies of the same form. Seeded from
// (and writes back into) the given fields directly. Returns true iff accepted with a non-empty
// string.
static bool textFontDialog(QWidget *parent, const QString &title, std::string &text,
                           std::string &font, int &size, double color[3], bool &bold, bool &italic) {
	QDialog d(parent);
	d.setWindowTitle(title);
	QFormLayout *fl = new QFormLayout(&d);
	QLineEdit *eText = new QLineEdit(QString::fromStdString(text), &d);
	QComboBox *eFont = new QComboBox(&d);
	eFont->addItems({ "Arial", "Courier", "Times" });
	eFont->setCurrentText(QString::fromStdString(font));
	QSpinBox *eSize = new QSpinBox(&d); eSize->setRange(4, 300); eSize->setValue(size);
	QCheckBox *eBold = new QCheckBox("Bold", &d);   eBold->setChecked(bold);
	QCheckBox *eItal = new QCheckBox("Italic", &d); eItal->setChecked(italic);
	QColor col = QColor::fromRgbF(color[0], color[1], color[2]);
	QPushButton *eColor = new QPushButton(&d);
	auto paintSwatch = [eColor](const QColor& c) { eColor->setStyleSheet("background:" + c.name()); };
	paintSwatch(col);
	QObject::connect(eColor, &QPushButton::clicked, [&]() {
		const QColor c = QColorDialog::getColor(col, &d, "Text colour");
		if (c.isValid()) { col = c; paintSwatch(c); }
	});
	fl->addRow("Text", eText);
	fl->addRow("Font", eFont);
	fl->addRow("Size", eSize);
	fl->addRow(eBold);
	fl->addRow(eItal);
	fl->addRow("Colour", eColor);
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
	fl->addRow(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
	if (d.exec() != QDialog::Accepted) return false;
	text = eText->text().toStdString();
	if (text.empty()) return false;
	font   = eFont->currentText().toStdString();
	size   = eSize->value();
	bold   = eBold->isChecked();
	italic = eItal->isChecked();
	color[0] = col.redF(); color[1] = col.greenF(); color[2] = col.blueF();
	return true;
}

// Font / colour editor for an EXISTING text label. Reachable from the label's Scene-Objects row or
// a canvas right-click. Edits the string, family (Arial/Courier/Times — VTK's built-in faces),
// size, colour, bold and italic, then re-applies and re-renders.
static void textPropsDialog(Scene *s, vtkProp3D *act) {
	TextLabel *tl = nullptr;
	for (auto& t : s->texts) if (t.actor.Get() == act) { tl = &t; break; }
	if (!tl) return;
	if (!textFontDialog(s->widget, "Text Properties", tl->text, tl->font, tl->size, tl->color, tl->bold, tl->italic))
		return;
	textApplyProps(s, *tl);
	rebuildSceneObjects(s);                            // the row label tracks the (possibly new) text
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Right-click menu for a text label: edit properties or delete it.
static void textLabelMenu(Scene *s, vtkProp3D *act, const QPoint& globalPos) {
	QMenu m(s->widget);
	QAction *props = m.addAction("Text Properties…");
	QAction *del   = m.addAction("Remove");
	QAction *chosen = m.exec(globalPos);
	if (chosen == props) { textPropsDialog(s, act); return; }
	if (chosen != del) return;
	for (size_t i = 0; i < s->texts.size(); ++i) {
		if (s->texts[i].actor.Get() != act) continue;
		if (s->axesRen) s->axesRen->RemoveActor(act);
		if (s->ren)     s->ren->RemoveActor(act);   // (harmless if it was on the overlay layer)
		s->texts.erase(s->texts.begin() + i);
		rebuildSceneObjects(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return;
	}
}

// Fledermaus-style type icons for the Scene Objects rows: a tiny glyph that says, at a glance,
// what kind of element a row is (surface / image / line / points / curtain / polygon / text /
// colourbar / profile). Drawn into a 16x16 transparent pixmap (matches the small checkbox).
enum ObjIcon { IC_Surface, IC_Image, IC_Line, IC_Points, IC_Curtain,
               IC_Polygon, IC_Polyline, IC_Rect, IC_Circle, IC_Text, IC_ColorBar, IC_Profile, IC_NestRect,
               IC_Axes, IC_StraightLine, IC_Beachball };

static QPixmap makeObjectIcon(int kind) {
	// Drawn in a 16-unit coordinate space but rasterised at high DPI (supersampled) so the glyph
	// stays crisp instead of pixelated. LOGICAL is the on-screen size; dpr the supersample factor.
	const int    LOGICAL = 18;
	const qreal  dpr     = 4.0;
	QPixmap pm(int(LOGICAL * dpr), int(LOGICAL * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.scale(LOGICAL / 16.0, LOGICAL / 16.0);             // map the 0..16 drawing space onto LOGICAL px
	auto dots = [&](std::initializer_list<QPointF> ps, const QColor& c) {  // small vertex handles
		p.setPen(Qt::NoPen); p.setBrush(c);
		for (const QPointF& q : ps) p.drawEllipse(q, 1.5, 1.5);
	};
	switch (kind) {
	case IC_Surface: {
		// A tilted, colour-graded mesh patch (perspective top face) — the Fledermaus "surface" look.
		const QPointF A(1, 10), B(9, 13), C(15, 7), D(7, 3);     // bottom-L, bottom-R, top-R, top-L
		const int n = 4;
		auto P = [&](double u, double v) {                       // bilinear over the quad
			QPointF bot = A * (1 - u) + B * u;
			QPointF top = D * (1 - u) + C * u;
			return bot * (1 - v) + top * v;
		};
		for (int j = 0; j < n; ++j)                              // colour cells: far(v=1) blue -> near(v=0) tan
			for (int i = 0; i < n; ++i) {
				double v = (j + 0.5) / n;
				QColor c(int(70 + 150 * (1 - v)), int(120 + 70 * (1 - v)), int(190 * v + 60));
				QPolygonF cell; cell << P((double)i/n, (double)j/n) << P((double)(i+1)/n, (double)j/n)
				                     << P((double)(i+1)/n, (double)(j+1)/n) << P((double)i/n, (double)(j+1)/n);
				p.setPen(Qt::NoPen); p.setBrush(c); p.drawPolygon(cell);
			}
		p.setPen(QPen(QColor(30, 50, 60), 0.6));                 // mesh lines
		for (int i = 0; i <= n; ++i) { p.drawLine(P((double)i/n, 0), P((double)i/n, 1));
		                               p.drawLine(P(0, (double)i/n), P(1, (double)i/n)); }
		break;
	}
	case IC_Image: {                                            // framed picture: sky + hill
		p.setPen(QPen(QColor(60, 60, 60), 1.0)); p.setBrush(QColor(150, 200, 235));
		p.drawRect(2, 3, 12, 10);
		p.setPen(Qt::NoPen); p.setBrush(QColor(110, 160, 90));
		QPolygonF hill; hill << QPointF(2, 13) << QPointF(6, 7) << QPointF(9, 11) << QPointF(12, 6) << QPointF(14, 13);
		p.drawPolygon(hill);
		p.setBrush(QColor(245, 220, 90)); p.drawEllipse(QPointF(11, 5.5), 1.6, 1.6);
		break;
	}
	case IC_Line: {                                            // overlay track: a clean coloured line, no handles
		p.setPen(QPen(QColor(40, 90, 170), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		QPolygonF l; l << QPointF(2, 12) << QPointF(6, 5) << QPointF(10, 10) << QPointF(14, 3);
		p.drawPolyline(l);
		break;
	}
	case IC_StraightLine: {                                    // fault trace: ONE straight diagonal segment
		p.setPen(QPen(QColor(40, 90, 170), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.drawLine(QPointF(2, 13), QPointF(14, 3));
		break;
	}
	case IC_Points: {
		p.setPen(Qt::NoPen); p.setBrush(QColor(210, 90, 60));
		const QPointF pts[5] = {{3,5},{8,3},{13,6},{5,12},{11,12}};
		for (auto& q : pts) p.drawEllipse(q, 1.8, 1.8);
		break;
	}
	case IC_Curtain: {                                         // hanging panel with a wavy bottom
		p.setPen(QPen(QColor(60, 60, 60), 1.0)); p.setBrush(QColor(120, 170, 210, 180));
		QPainterPath cp; cp.moveTo(2, 2); cp.lineTo(14, 2); cp.lineTo(14, 11);
		cp.cubicTo(12, 14, 10, 9, 8, 12); cp.cubicTo(6, 14, 4, 10, 2, 12); cp.closeSubpath();
		p.drawPath(cp);
		break;
	}
	case IC_Polygon: {                                         // irregular closed ring, fill + vertex handles
		QPolygonF poly; poly << QPointF(3, 5) << QPointF(13, 4) << QPointF(14, 11)
		                     << QPointF(8, 14) << QPointF(2, 11);
		p.setPen(QPen(QColor(40, 40, 40), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(QColor(255, 200, 120, 150)); p.drawPolygon(poly);
		dots({{3,5},{13,4},{14,11},{8,14},{2,11}}, QColor(190, 110, 30));
		break;
	}
	case IC_Polyline: {                                        // open vertex chain, handles at the bends
		p.setPen(QPen(QColor(40, 40, 40), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		QPolygonF l; l << QPointF(2, 13) << QPointF(6, 4) << QPointF(10, 11) << QPointF(14, 4);
		p.drawPolyline(l);
		dots({{2,13},{6,4},{10,11},{14,4}}, QColor(40, 40, 40));
		break;
	}
	case IC_Rect: {                                            // rectangle, fill + corner handles
		p.setPen(QPen(QColor(40, 40, 40), 1.4)); p.setBrush(QColor(255, 200, 120, 150));
		p.drawRect(3, 4, 10, 9);
		dots({{3,4},{13,4},{13,13},{3,13}}, QColor(190, 110, 30));
		break;
	}
	case IC_Circle: {                                          // circle outline, light fill
		p.setPen(QPen(QColor(40, 40, 40), 1.4)); p.setBrush(QColor(255, 200, 120, 150));
		p.drawEllipse(QPointF(8, 8.5), 6, 6);
		break;
	}
	case IC_NestRect: {                                        // "Nested grids": three concentric thin rects
		p.setPen(QPen(QColor(40, 40, 40), 0.8, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
		p.setBrush(Qt::NoBrush);
		p.drawRect(QRectF(1.5, 2.5, 13.0, 11.0));   // outer
		p.drawRect(QRectF(4.0, 4.5,  8.0,  7.0));   // middle
		p.drawRect(QRectF(6.5, 6.5,  3.0,  3.0));   // inner
		break;
	}
	case IC_Beachball: {                                      // strike-slip focal-mechanism beachball:
		// circle split by the two (vertical/horizontal) nodal planes into 4 quadrants, alternating
		// black compressional / white dilatational — the classic strike-slip "bowtie" pattern.
		const QPointF O(8, 8); const double R = 6.5;
		p.setPen(Qt::NoPen); p.setBrush(Qt::white); p.drawEllipse(O, R, R);
		p.setBrush(Qt::black);
		p.drawPie(QRectF(O.x() - R, O.y() - R, 2 * R, 2 * R), 45 * 16, 90 * 16);
		p.drawPie(QRectF(O.x() - R, O.y() - R, 2 * R, 2 * R), 225 * 16, 90 * 16);
		p.setPen(QPen(QColor(20, 20, 20), 1.0)); p.setBrush(Qt::NoBrush); p.drawEllipse(O, R, R);
		break;
	}
	case IC_Text: {
		QFont f = p.font(); f.setBold(true); f.setPointSize(11); p.setFont(f);
		p.setPen(QColor(40, 40, 40)); p.drawText(pm.rect(), Qt::AlignCenter, "T");
		break;
	}
	case IC_ColorBar: {                                        // vertical rainbow strip
		for (int y = 2; y < 14; ++y) {
			double t = (y - 2) / 12.0;
			p.setPen(QColor::fromHsvF(0.66 * (1 - t), 0.85, 0.95));
			p.drawLine(5, y, 11, y);
		}
		p.setPen(QPen(QColor(60, 60, 60), 0.8)); p.setBrush(Qt::NoBrush); p.drawRect(5, 2, 6, 11);
		break;
	}
	case IC_Profile: {                                         // axes + a profile curve
		p.setPen(QPen(QColor(120, 120, 120), 1.0)); p.drawLine(2, 13, 14, 13); p.drawLine(2, 13, 2, 3);
		p.setPen(QPen(QColor(200, 70, 60), 1.6));
		QPainterPath pp; pp.moveTo(2, 10); pp.cubicTo(5, 4, 8, 12, 14, 5);
		p.drawPath(pp);
		break;
	}
	case IC_Axes: {                                            // a small 3-axis corner gizmo (X red, Y green, Z blue)
		const QPointF O(4, 12);
		p.setPen(QPen(QColor(210, 70, 60), 1.6)); p.drawLine(O, QPointF(14, 12));   // X -> right
		p.setPen(QPen(QColor(70, 170, 80), 1.6)); p.drawLine(O, QPointF(4, 2));     // Z -> up
		p.setPen(QPen(QColor(70, 110, 200), 1.6)); p.drawLine(O, QPointF(11, 6));   // Y -> back/oblique
		p.setPen(Qt::NoPen); p.setBrush(QColor(50, 50, 50)); p.drawEllipse(O, 1.4, 1.4);
		break;
	}
	}
	p.end();
	return pm;
}

// ---- dropped-image placement & draping ------------------------------------
// An image has no elevation, so a dropped image must NOT sit at z=0 where it would slice through
// the relief. It rides on a horizontal plane at ex.zpos (TRUE z, defaulting to ON TOP of the
// surface); it can be raised/lowered (stack) or DRAPED onto the grid when the footprints overlap.
// imageRebuildActor rebuilds ex.actor for the current mode; the texture (ex.tex) is reused either way.

// Index of the extra owning this actor (re-find each call; the vector may have changed). -1 if gone.
static int extraIndexOfActor(Scene *s, vtkProp3D *a) {
	for (size_t i = 0; i < s->extras.size(); ++i)
		if (s->extras[i].actor.Get() == a) return (int)i;
	return -1;
}

// True if the image footprint overlaps the host grid footprint (a drape needs a grid underneath).
static bool imageOverlapsGrid(Scene *s, const ExtraObj& ex) {
	if (s->gridZ.empty() || s->gnx < 2 || s->gny < 2) return false;
	return ex.bx0 < s->gx1 && ex.bx1 > s->gx0 && ex.by0 < s->gy1 && ex.by1 > s->gy0;
}

// Vertical step for "stack up/down" + the default on-top gap: 2% of the relief range.
static double imageStackStep(Scene *s) {
	const double r = s->zmax - s->zmin;
	return (r > 0) ? 0.02 * r : 1.0;
}

// (Re)build the image actor for its current flat/draped state and (re)register it in the renderer.
static void imageRebuildActor(Scene *s, ExtraObj& ex) {
	if (ex.actor) s->ren->RemoveActor(ex.actor);
	vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
	const bool drape = ex.draped && imageOverlapsGrid(s, ex);
	ex.draped = drape;
	if (drape) {
		// Drape: sample the grid heightfield over the image∩grid footprint, texture-map the image.
		const double x0 = std::max(ex.bx0, s->gx0), x1 = std::min(ex.bx1, s->gx1);
		const double y0 = std::max(ex.by0, s->gy0), y1 = std::min(ex.by1, s->gy1);
		int nx = std::min(s->gnx, 256), ny = std::min(s->gny, 256);
		if (nx < 2) nx = 2;
		if (ny < 2) ny = 2;
		vtkNew<vtkPoints> pts; pts->SetDataTypeToFloat(); pts->Allocate(nx * ny);
		vtkNew<vtkFloatArray> tc; tc->SetNumberOfComponents(2); tc->SetName("tc"); tc->Allocate(2 * nx * ny);
		for (int j = 0; j < ny; ++j) {
			const double y = y0 + (y1 - y0) * j / (ny - 1);
			for (int i = 0; i < nx; ++i) {
				const double x = x0 + (x1 - x0) * i / (nx - 1);
				double z = sampleZ(s, x, y);
				if (std::isnan(z)) z = 0.0;
				pts->InsertNextPoint(x, y, z);
				tc->InsertNextTuple2((x - ex.bx0) / (ex.bx1 - ex.bx0), (y - ex.by0) / (ex.by1 - ex.by0));
			}
		}
		vtkNew<vtkCellArray> cells;
		for (int j = 0; j < ny - 1; ++j)
			for (int i = 0; i < nx - 1; ++i) {
				vtkIdType q[4] = { (vtkIdType)(j*nx+i), (vtkIdType)(j*nx+i+1),
				                   (vtkIdType)((j+1)*nx+i+1), (vtkIdType)((j+1)*nx+i) };
				cells->InsertNextCell(4, q);
			}
		pd->SetPoints(pts); pd->SetPolys(cells); pd->GetPointData()->SetTCoords(tc);
	}
	else {
		// Flat horizontal plane at ex.zpos spanning the image bbox (single quad, full-texture tcoords).
		vtkNew<vtkPoints> pts; pts->SetDataTypeToFloat();
		pts->InsertNextPoint(ex.bx0, ex.by0, ex.zpos); pts->InsertNextPoint(ex.bx1, ex.by0, ex.zpos);
		pts->InsertNextPoint(ex.bx1, ex.by1, ex.zpos); pts->InsertNextPoint(ex.bx0, ex.by1, ex.zpos);
		vtkNew<vtkFloatArray> tc; tc->SetNumberOfComponents(2); tc->SetName("tc");
		tc->InsertNextTuple2(0, 0); tc->InsertNextTuple2(1, 0); tc->InsertNextTuple2(1, 1); tc->InsertNextTuple2(0, 1);
		vtkNew<vtkCellArray> cells; vtkIdType q[4] = { 0, 1, 2, 3 }; cells->InsertNextCell(4, q);
		pd->SetPoints(pts); pd->SetPolys(cells); pd->GetPointData()->SetTCoords(tc);
	}
	vtkNew<vtkPolyDataMapper> map; map->SetInputData(pd); map->ScalarVisibilityOff();
	if (drape) {                              // pull the drape toward the camera so it wins z-fighting on the relief
		vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
		map->SetRelativeCoincidentTopologyPolygonOffsetParameters(-1.0, -1.0);
	}
	vtkSmartPointer<vtkActor> a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(map); a->SetTexture(ex.tex);
	a->GetProperty()->LightingOff();          // a finished picture: full albedo, no shading
	a->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	ex.actor = a;
	s->ren->AddActor(a);
}

// Properties menu for a dropped image (left-click its Scene Objects row): order it relative to the
// surface (top/bottom/stack), drape it on the grid (if footprints overlap), or delete it.
static void imageObjectMenu(Scene *s, vtkProp3D *actor, const QPoint &g) {
	int idx = extraIndexOfActor(s, actor);
	if (idx < 0) return;
	const bool draped = s->extras[idx].draped;
	// Stacking orders the image against the RELIEF SURFACE, and drape lays it on the grid — both are
	// meaningless without a real grid. An image-only window (iview_image_obj) carries only a hidden
	// blank scaffold plane, which is NOT a grid: sceneHasGrid excludes it (gates on !imageOnly), so
	// the stack + drape entries grey out when the window holds just image(s).
	const bool hasGrid = sceneHasGrid(s);
	const bool canDrape = !draped && hasGrid && imageOverlapsGrid(s, s->extras[idx]);
	QMenu m(s->widget);
	QMenu *stackMenu = m.addMenu("Stack order");
	QAction *aTop = stackMenu->addAction("Place on top");
	QAction *aBot = stackMenu->addAction("Place at bottom");
	QAction *aUp  = stackMenu->addAction("Bring forward");
	QAction *aDn  = stackMenu->addAction("Send backward");
	stackMenu->setEnabled(!draped && hasGrid);                                       // need a relief to stack against
	for (QAction *a : { aTop, aBot, aUp, aDn }) a->setEnabled(!draped && hasGrid);
	m.addSeparator();
	QAction *aDrape = m.addAction(draped ? "Undrape (flat)" : "Drape on grid");
	aDrape->setEnabled(draped || canDrape);
	m.addSeparator();
	// Percentile histogram stretch -> new 8-bit image row. Meaningful for a wide-range (e.g. 16-bit
	// satellite) image shown as a fast min-max preview; Julia reports if there is no wider source.
	QAction *aStretch = m.addAction("Auto histogram stretch (new image)");
	QAction *aSave = m.addAction("Save image…");
	QAction *aDel = m.addAction("Remove");
	QAction *c = m.exec(g);
	if (!c) return;
	if (c == aSave) { saveObjectDialog(s, "image", QString::fromStdString(s->extras[idx].name)); return; }
	if (c == aStretch) { stretchImageObject(s, QString::fromStdString(s->extras[idx].name)); return; }
	ExtraObj& ex = s->extras[idx];            // vector unchanged during exec -> index still valid
	const double step = imageStackStep(s);
	if (c == aDel) {
		if (ex.actor) s->ren->RemoveActor(ex.actor);
		s->extras.erase(s->extras.begin() + idx);
		rebuildSceneObjects(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return;
	}
	if (c == aDrape) {
		ex.draped = !ex.draped;
		imageRebuildActor(s, ex);
	}
	else {
		// Stacking = the VERTICAL ORDER of the whole pile, in which the RELIEF SURFACE is itself a
		// member. So with one grid + one image, "Stack down" pushes the image below the surface — the
		// same as "Place at bottom" (there is nothing else to pass). Build the ordered pile bottom->top
		// (images sorted by zpos, the surface slotted at its mid-z), move THIS image one slot (or to an
		// end), then re-derive every image's zpos from its side of the surface.
		const bool hasSurf = (surfProp(s) != nullptr);
		const double smid = 0.5 * (s->zmin + s->zmax);
		struct Slot { bool surf; int idx; double key; };
		std::vector<Slot> pile;
		for (size_t i = 0; i < s->extras.size(); ++i)
			if (s->extras[i].isImage) pile.push_back({ false, (int)i, s->extras[i].zpos });
		if (hasSurf) pile.push_back({ true, -1, smid });
		std::sort(pile.begin(), pile.end(), [](const Slot& a, const Slot& b) { return a.key < b.key; });
		int pos = 0; for (size_t k = 0; k < pile.size(); ++k) if (!pile[k].surf && pile[k].idx == idx) { pos = (int)k; break; }
		Slot tgt = pile[pos]; pile.erase(pile.begin() + pos);    // pull the image out, then reinsert one slot over
		int dest = pos;
		if      (c == aTop) dest = (int)pile.size();             // top of the pile
		else if (c == aBot) dest = 0;                            // bottom of the pile
		else if (c == aUp)  dest = std::min(pos + 1, (int)pile.size());   // climb one slot (may cross the surface)
		else if (c == aDn)  dest = std::max(pos - 1, 0);                  // sink one slot
		pile.insert(pile.begin() + dest, tgt);
		// Re-derive z: images below the surface go under zmin, images above go over zmax (step apart).
		int si = -1; for (size_t k = 0; k < pile.size(); ++k) if (pile[k].surf) { si = (int)k; break; }
		for (int k = 0; k < (int)pile.size(); ++k) {
			if (pile[k].surf) continue;
			ExtraObj& e = s->extras[pile[k].idx];
			e.zpos = (si < 0)      ? s->zmax + (k + 1) * step          // no surface: stack all above z=zmax
			       : (k < si)      ? s->zmin - (si - k) * step          // below the relief
			                       : s->zmax + (k - si) * step;         // above the relief
		}
		for (auto &e : s->extras) if (e.isImage) imageRebuildActor(s, e);
	}
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// ---- UNIFIED draw-order pile (base relief + GRIDS + VECTORS) --------------------------------
// ONE pile so any element — a grid, the base relief, an overlay, a symbol layer, or a polygon /
// fault trace — can be ordered ABOVE or BELOW any other element it overlaps. Previously grids and
// vectors lived in two separate piles (vectors hard-wired above all grids), so "Place on top /
// at bottom" on a fault drawn over a grid had nothing in its own pile to move past and did
// nothing. Now everything shares one polygon-offset ramp: rank 0 = bottom, higher = drawn on top.
// Images are excluded (they carry their own zpos stacking).
//
// `vec` marks line/point geometry: it gets an extra half-step lift on its LINE/POINT offsets so a
// vector sitting at the same rank as the surface beneath it stays visible (resolves the z-fight
// with that surface) without overtaking the next element a full rank up.
struct StackItem { std::vector<vtkActor*> actors; int *stack; bool vec; bool solid3D = false; };

static std::vector<StackItem> gatherStackItems(Scene *s) {
	std::vector<StackItem> v;
	std::vector<vtkActor*> base = surfActors(s);
	if (!base.empty()) v.push_back({ base, &s->surfStack, false });   // base relief
	for (auto& ex : s->extras) {                                      // dropped grids (images stack via zpos)
		if (ex.isImage || !ex.actor) continue;
		std::vector<vtkActor*> a = { ex.actor.Get() };
		if (ex.drape) a.push_back(ex.drape.Get());
		v.push_back({ a, &ex.gstack, false });
	}
	for (auto& o  : s->overlays) if (o.actor) v.push_back({ { o.actor.Get()  }, &o.stack,  true, false });
	for (auto& sl : s->symbols)  if (sl.actor) v.push_back({ { sl.actor.Get() }, &sl.stack, true, sl.solid3D });
	for (auto& pg : s->polys)    if (pg.line && !pg.isMeca) v.push_back({ { pg.line.Get()  }, &pg.stack, true, false });
	return v;
}

// Park a vector actor in the depth-cleared OVERLAY layer (axesRen, layer 1: shares the main camera but
// clears its own depth) so it is drawn ON TOP of all 3-D terrain by RENDER ORDER, or move it back to the
// main 3-D layer. Idempotent. This is how a vector ranked above every raster wins without any depth-
// offset magic — the exact mechanism the Z-axis labels already use. Removal paths must clear BOTH layers.
static void setActorTopLayer(Scene *s, vtkActor *a, bool top) {
	if (!a || !s->ren) return;
	vtkRenderer *ov = s->axesRen ? s->axesRen.Get() : s->ren.Get();
	if (top && ov != s->ren.Get()) {
		if (s->ren->HasViewProp(a)) s->ren->RemoveActor(a);
		if (!ov->HasViewProp(a))    ov->AddActor(a);
	} else {
		if (ov != s->ren.Get() && ov->HasViewProp(a)) ov->RemoveActor(a);
		if (!s->ren->HasViewProp(a)) s->ren->AddActor(a);
	}
}

// Tear the colorbar actors out of the renderer so the bar can be rebuilt for a different (active) grid.
// Leaves s->barX0/barY0 (drag position) intact so the bar stays put across retargets.
static void destroyColorbar(Scene *s) {
	if (!s) return;
	if (s->bar)      { s->ren->RemoveActor2D(s->bar);      s->bar = nullptr; }
	if (s->barTicks) { s->ren->RemoveActor2D(s->barTicks); s->barTicks = nullptr; }
	for (auto& ta : s->barLabels) if (ta) s->ren->RemoveActor2D(ta);
	s->barLabels.clear();
	s->barValues.clear();
	s->barTickPts = nullptr;
}

// One resolved grid layer: which grid currently drives the readout + colorbar. valid=false when no grid
// is visible (every grid hidden) -> caller hides the colorbar and clears the readout routing.
struct ActiveGrid {
	bool   valid = false;
	const std::vector<float>* z = nullptr;
	int    nx = 0, ny = 0;
	double x0 = 0, x1 = 1, y0 = 0, y1 = 1, zmin = 0, zmax = 1;
	vtkScalarsToColors *lut = nullptr;
	bool   showBar = true;     // active grid's per-grid "show colorbar" intent
};

// The active grid is the TOPMOST VISIBLE grid (highest pile rank). The base relief participates via
// s->surfStack / s->surfLut / s->zmin-zmax; each dropped grid via its own ExtraObj fields.
static ActiveGrid resolveActiveGrid(Scene *s) {
	ActiveGrid ag;
	int  bestStack = 0;
	bool have = false;
	if (!s->gridZ.empty()) {                                   // base relief, if it carries a data layer
		vtkProp3D *sp = surfProp(s);
		if (sp && sp->GetVisibility()) {
			ag.valid = true; ag.z = &s->gridZ; ag.nx = s->gnx; ag.ny = s->gny;
			ag.x0 = s->gx0; ag.x1 = s->gx1; ag.y0 = s->gy0; ag.y1 = s->gy1;
			ag.zmin = s->zmin; ag.zmax = s->zmax; ag.lut = s->surfLut; ag.showBar = s->surfShowBar;
			bestStack = s->surfStack; have = true;
		}
	}
	for (auto& ex : s->extras) {
		if (ex.isImage || ex.gridZ.empty() || !ex.actor || !ex.actor->GetVisibility()) continue;
		if (!have || ex.gstack >= bestStack) {                 // ties impossible (ranks normalized unique)
			bestStack = ex.gstack; have = true; ag.valid = true;
			ag.z = &ex.gridZ; ag.nx = ex.gnx; ag.ny = ex.gny;
			ag.x0 = ex.gx0; ag.x1 = ex.gx1; ag.y0 = ex.gy0; ag.y1 = ex.gy1;
			ag.zmin = ex.zmin; ag.zmax = ex.zmax; ag.lut = ex.lut; ag.showBar = ex.showBar;
		}
	}
	return ag;
}

// Retarget the single rendered colorbar + the hover/coordinate readout to the active (topmost-visible)
// grid. Called on every grid add / visibility toggle / restack / delete. No grid visible -> bar hidden.
// For an Aquamoto layer (customLayerTexture): `bar` (built here as usual) is the WATER bar, shown
// only while aquaShowWater is true; the separate, persistent aquaLandBar is shown only while it's
// false -- the two are mutually exclusive, matching the dialog's Shade Water/Land radio.
static void refreshGridColorbar(Scene *s) {
	if (!s || s->imageOnly) return;            // bare-image windows never carry a z colorbar
	ActiveGrid ag = resolveActiveGrid(s);
	destroyColorbar(s);
	const bool isAqua = s->customLayerTexture;
	if (!ag.valid || !ag.lut) {                // nothing visible to colour -> no bar, readout falls back
		s->actZ = nullptr;
		if (isAqua) setAquaLandColorbarVisible(s, false);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return;
	}
	// Route the hover/coordinate readout to the active grid ALWAYS; only DRAW the bar if this grid wants it.
	s->actZ = ag.z; s->actNx = ag.nx; s->actNy = ag.ny;
	s->actX0 = ag.x0; s->actX1 = ag.x1; s->actY0 = ag.y0; s->actY1 = ag.y1;
	const bool showWaterBar = ag.showBar && (!isAqua || s->aquaShowWater);
	if (showWaterBar) buildColorbar(s, ag.lut, ag.zmin, ag.zmax);
	if (isAqua) setAquaLandColorbarVisible(s, s->aquaLandShowBar && !s->aquaShowWater);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Normalize every element's rank to 0..n-1 by current `stack`, then map rank -> polygon offset
// (k=0 bottom .. higher k drawn on top). Grids/relief and vector lines share the SAME ramp, so a
// fault can sit above or below the grid it was drawn on.
static void applyStacking(Scene *s) {
	std::vector<StackItem> it = gatherStackItems(s);
	const int n = (int)it.size();
	if (n == 0) return;
	std::vector<int> ord(n);
	for (int i = 0; i < n; ++i) ord[i] = i;
	std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) { return *it[a].stack < *it[b].stack; });
	// SMALL per-rank step: the coincident offset is used ONLY to break near-coplanar z-fights (e.g. two
	// overlapping grids at the same elevation, a line lying on its grid). It must NOT be large — a big
	// offset drags a vector so far toward the camera it pokes THROUGH a grid drawn over it. Genuine 3-D
	// occlusion (a line hidden under taller grid2) is left to real geometry; "always on top" is handled by
	// the depth-cleared overlay layer, not by offset magnitude. 20000 << the -200000 edit-handle band.
	const double step = 20000.0;
	// ANCHOR the ramp on the base relief: it is real lit 3-D terrain, not a coplanar overlay, so it must
	// keep its build-default coincident offset (0,0) — stamping a huge offset on it saturates the depth
	// buffer and visibly shifts its shading/tonality. Offsets are computed RELATIVE to the surface rank,
	// so the surface stays at u=0 regardless of stacking; a vector ranked below it gets a positive (push
	// away) offset, above it a negative (pull nearer) one. Absent base relief -> anchor 0 (old behaviour).
	int surfRank = 0;
	for (int k = 0; k < n; ++k) if (it[ord[k]].stack == &s->surfStack) { surfRank = k; break; }
	int topRasterRank = -1;
	for (int k = 0; k < n; ++k) if (!it[ord[k]].vec) topRasterRank = k;   // ranks == position (sorted), so last raster
	for (int k = 0; k < n; ++k) {
		*it[ord[k]].stack = k;                              // 0..n-1 (survives deletes)
		const bool   vec     = it[ord[k]].vec;
		const bool   solid3D = it[ord[k]].solid3D;
		const double u   = -(k - surfRank) * step;          // rank ramp; base relief stays at 0 (tonality-safe)
		// A vector gets its PLAIN rank offset (no half-step): it is then nearer than every raster ranked
		// below it (drawn on top of them) and farther than every raster ranked above it (occluded by them
		// through real depth). That is exactly what lets a line sit BETWEEN two grids — visible over grid1,
		// hidden under grid2. A vector ranked above EVERY raster cannot be lifted over taller terrain by any
		// offset, so it goes to the depth-cleared OVERLAY layer and is drawn on top purely by render order.
		// solid3D (sphere/cube) is exempted from the coincident-offset ramp always: it is real lit 3-D
		// geometry with genuine depth of its own (e.g. a buried earthquake), not a coincident 2-D-ish
		// overlay, so it never gets an offset bias — real depth decides whether 3-D terrain occludes it.
		// But in flat-2D (sceneSetFlat2D, top-down ORTHOGRAPHIC camera) that real depth test would just
		// as legitimately hide a buried event behind the terrain directly above it — wrong there: 2-D is
		// a MAP view, every marker must show regardless of its true depth, pinned at its surface (x,y).
		// So flat2d still promotes solid3D to the depth-cleared overlay layer, same as any other vector;
		// only genuine 3-D perspective gets the real-occlusion treatment. sceneSetFlat2D re-runs this
		// function on every 2D<->3D toggle so the promotion updates immediately.
		const bool onTop = vec && (!solid3D || s->flat2d) && topRasterRank >= 0 && k > topRasterRank;
		for (vtkActor *a : it[ord[k]].actors) {
			if (vec) setActorTopLayer(s, a, onTop);         // solid3D: onTop is always false here
			vtkPolyDataMapper *mp = vtkPolyDataMapper::SafeDownCast(a->GetMapper());
			if (!mp) continue;
			if (solid3D) {
				// leave depth resolution at the mapper's own default (no bias) -> real occlusion
			} else if (vec) {
				mp->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, u);
				mp->SetRelativeCoincidentTopologyPointOffsetParameter(u);
			} else {
				mp->SetRelativeCoincidentTopologyPolygonOffsetParameters(0.0, u);
			}
		}
	}
	refreshGridColorbar(s);                                // topmost grid may have changed -> retarget bar
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Move the element owning *stackPtr through the unified pile (op: 0=top, 1=bottom, 2=up, 3=down),
// then re-apply offsets. Works across grids AND vectors — that is what makes a lone fault's stack
// actions actually do something against the grid under it.
static void restackStack(Scene *s, int *stackPtr, int op) {
	std::vector<StackItem> it = gatherStackItems(s);
	const int n = (int)it.size();
	if (n < 2 || !stackPtr) return;
	std::vector<int> ord(n);
	for (int i = 0; i < n; ++i) ord[i] = i;
	std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) { return *it[a].stack < *it[b].stack; });
	int pos = -1;
	for (int k = 0; k < n; ++k) if (it[ord[k]].stack == stackPtr) { pos = k; break; }
	if (pos < 0) return;
	const bool movingVec = it[ord[pos]].vec;
	int dest = (op == 0) ? n - 1 : (op == 1) ? 0 : (op == 2) ? std::min(pos + 1, n - 1) : std::max(pos - 1, 0);
	// RULE 1+3: a RASTER (grid/image) must ALWAYS hold the base (rank 0); a VECTOR may never sit under
	// the base raster. So floor a moving vector at rank 1 (it can still slide between any rasters above).
	if (movingVec && dest < 1) dest = 1;
	if (dest == pos) return;
	const int moved = ord[pos];
	ord.erase(ord.begin() + pos);
	ord.insert(ord.begin() + dest, moved);
	// SAFETY (RULE 1): if the move stranded a vector at the base, pull the lowest raster back down to
	// rank 0. A lone raster therefore can never be lifted above a vector — one raster always stays base.
	if (it[ord[0]].vec) {
		int r = -1;
		for (int k = 1; k < n; ++k) if (!it[ord[k]].vec) { r = k; break; }
		if (r > 0) { const int rr = ord[r]; ord.erase(ord.begin() + r); ord.insert(ord.begin(), rr); }
	}
	for (int k = 0; k < n; ++k) *it[ord[k]].stack = k;
	applyStacking(s);
}

// Back-compat aliases: grids and vectors now share ONE pile, so both entry points funnel here.
static void applyGridStacking(Scene *s) { applyStacking(s); }
static void restackGrid(Scene *s, int *stackPtr, int op) { restackStack(s, stackPtr, op); }

// Append the 4 grid-pile stacking actions to a grid row's menu, wired to *stackPtr. Enabled only when
// there are 2+ grids (base relief counts) to order against. Shared by the base surface + extra grids.
static void addGridStackActions(Scene *s, QMenu &m, int *stackPtr) {
	const int nGrids = (int)gatherStackItems(s).size();
	QMenu *stackMenu = m.addMenu("Stack order");
	QAction *aTop = stackMenu->addAction("Place on top");
	QAction *aBot = stackMenu->addAction("Place at bottom");
	QAction *aUp  = stackMenu->addAction("Bring forward");
	QAction *aDn  = stackMenu->addAction("Send backward");
	stackMenu->setEnabled(nGrids > 1);
	for (QAction *a : { aTop, aBot, aUp, aDn }) a->setEnabled(nGrids > 1);
	QObject::connect(aTop, &QAction::triggered, [s, stackPtr]() { restackGrid(s, stackPtr, 0); rebuildSceneObjects(s); });
	QObject::connect(aBot, &QAction::triggered, [s, stackPtr]() { restackGrid(s, stackPtr, 1); rebuildSceneObjects(s); });
	QObject::connect(aUp,  &QAction::triggered, [s, stackPtr]() { restackGrid(s, stackPtr, 2); rebuildSceneObjects(s); });
	QObject::connect(aDn,  &QAction::triggered, [s, stackPtr]() { restackGrid(s, stackPtr, 3); rebuildSceneObjects(s); });
}

// Remove the primary surface (grid or image), its colorbar, and its cube axes from the scene,
// leaving the window open and empty so a new file can be dropped in. Mirrors the self-cleaning
// prologue of buildSceneContent — same actors removed, same pointers nulled.
static void sceneRemoveSurface(Scene *s) {
	if (!s || !s->ren) return;
	// The Aquamoto control window is lifetime-tied to its nc cube surface: removing the surface destroys
	// the (otherwise un-killable) window too. Do it FIRST, before the surface actors go.
	if (g_aquamotoDestroy && g_aquamotoHasWindow && g_aquamotoHasWindow(s)) g_aquamotoDestroy(s);
	// LOD quad-tree observer + tile cache
	if (s->lodCmd && s->ren->GetActiveCamera())
		s->ren->GetActiveCamera()->RemoveObserver(s->lodCmd);
	s->lodCmd = nullptr; s->quadRoot = nullptr; s->tiles.clear();
	// Primary surface actors (tiled or single) + image drape
	if (s->surfGroup) s->ren->RemoveActor(s->surfGroup);
	if (s->surf)      s->ren->RemoveActor(s->surf);
	if (s->drape)     s->ren->RemoveActor(s->drape);
	s->surfGroup = nullptr; s->surf = nullptr; s->drape = nullptr; s->surfLut = nullptr;
	// Cube axes box + tick geometry + all billboard actors in the overlay renderer
	if (s->axes)      s->ren->RemoveActor(s->axes);
	if (s->axisTicks) s->ren->RemoveActor(s->axisTicks);
	s->axes = nullptr; s->axisTicks = nullptr;
	if (s->axesRen) {
		for (int i = 0; i < 3; ++i)
			if (s->axTitle[i]) { s->axesRen->RemoveViewProp(s->axTitle[i]); s->axTitle[i] = nullptr; }
		for (auto &l : s->xlabels) if (l) s->axesRen->RemoveViewProp(l);
		for (auto &l : s->ylabels) if (l) s->axesRen->RemoveViewProp(l);
		for (auto &l : s->zlabels) if (l) s->axesRen->RemoveViewProp(l);
		s->xlabels.clear(); s->ylabels.clear(); s->zlabels.clear();
	}
	// Colorbar strip + tick lines + numeric labels
	if (s->bar)      s->ren->RemoveActor2D(s->bar);
	if (s->barTicks) s->ren->RemoveActor2D(s->barTicks);
	for (auto& ta : s->barLabels) if (ta) s->ren->RemoveActor2D(ta);
	s->bar = nullptr; s->barTicks = nullptr;
	s->barLabels.clear(); s->barValues.clear();
	// Profile line anchored to the old surface
	if (s->profLine) { s->ren->RemoveActor(s->profLine); s->profLine = nullptr; }
	// Full-res z data buffer + active-grid pointer
	s->gridZ.clear(); s->gnx = 0; s->gny = 0; s->actZ = nullptr;
	// Mark window as empty — a dropped file will promote into it
	s->emptyStart   = true;
	s->imageOnly    = false;
	s->gridAdopted  = false;
	s->fvSolid      = false;
	s->surfName.clear();
	// Hide the Shading dock when no other loaded grids remain
	{
		bool hasOtherGrid = false;
		for (const auto& ex : s->extras) if (!ex.isImage) { hasOtherGrid = true; break; }
		if (!hasOtherGrid && s->shadeDock) s->shadeDock->setVisible(false);
	}
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// A "layerN" blank grid (hollow, made by the Nested-grids tool) can be FILLED by sampling a 2nd
// grid onto its nodes ("Transplant 2nd grid…"). Transplantability is an INTRINSIC property of the grid
// (encoded in its name, carried on G.title), NOT a per-window flag — so the option follows the grid
// wherever its row lives: as a dropped EXTRA grid (gridObjectMenu) or, after "Move to new window", as
// that window's BASE surface (surfaceObjectMenu). One predicate, one handler, offered identically.
static bool gridIsNestedBlank(const QString& nm) { return QRegularExpression("^layer\\d+$").match(nm).hasMatch(); }

// Run the "Transplant 2nd grid…" fill on the nested blank grid named `nm`: pick an implant file and hand
// it to Julia (_on_nested_transplant), which samples it onto this grid's nodes. Works for the base
// surface and for an extra grid alike — Julia detects which and replaces in place / re-adds.
static void runNestedTransplant(Scene *s, const QString& nm) {
	if (!g_juliaEval) return;
	const QString fn = QFileDialog::getOpenFileName(s->win, "Select grid to implant", prefStartDir(),
		"Grids (*.grd *.nc *.tif *.tiff *.img);;All files (*)");
	if (fn.isEmpty()) return;
	rememberStartDir(fn);
	const QString cmd = QString("InteractiveGMT._on_nested_transplant(Ptr{Cvoid}(UInt(%1)),raw\"%2\",raw\"%3\")")
		.arg((qulonglong)reinterpret_cast<uintptr_t>(s)).arg(nm).arg(fn);
	std::vector<char> buf(1 << 12);
	int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
	if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));
}

// Run grdinfo on the named grid (Julia InteractiveGMT._info_text_named, info.jl) and show the report
// in the same read-only popup the toolbar 'i' button uses. Shared by the base-surface and extra-grid
// handle menus — one grdinfo entry point, not a per-menu reimplementation.
static void runGridInfo(Scene *s, const QString &nm) {
	if (!g_juliaEval) { if (s->win) s->win->statusBar()->showMessage("Info: Julia eval not registered", 3000); return; }
	const QString cmd = QString("InteractiveGMT._info_text_named(Ptr{Cvoid}(UInt(%1)),raw\"%2\",\"grdinfo\")")
		.arg((qulonglong)reinterpret_cast<uintptr_t>(s)).arg(nm);
	std::vector<char> buf(1 << 16);
	int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
	QString txt = QString::fromUtf8(buf.data(), n < 0 ? -n : n);
	showInfoText(s->win, "grdinfo — " + nm, txt);
}

// Properties menu for the BASE relief surface row: Save, grid-pile stacking, and Remove.
static void surfaceObjectMenu(Scene *s, const QPoint& gp) {
	const QString nm = s->surfName.empty() ? QString("Surface") : QString::fromStdString(s->surfName);
	QMenu m(s->widget);
	QAction *aSave = m.addAction("Save grid…");
	QAction *aInfo = m.addAction("Info (grdinfo)…");
	QAction *aMove = m.addAction("Move to new window");      // re-open this grid in a fresh iGMT window, then drop it here
	QAction *aTransplant = nullptr;                          // present iff this base grid is a nested blank (moved here)
	if (g_juliaEval && gridIsNestedBlank(nm))
		aTransplant = m.addAction("Transplant 2nd grid…");
	QAction *aCube = nullptr;                                // present iff this base grid is a 3-D cube variable
	if (s->cubeNLayers > 1 && g_juliaCubeSlider)
		aCube = m.addAction("Cube layers…");
	QAction *aAqua = nullptr;                                // present iff this surface is an Aquamoto layer
	if (g_aquamotoReopen && g_aquamotoHasWindow && g_aquamotoHasWindow(s))
		aAqua = m.addAction("Aquamoto viewer…");             // re-show the hidden Aquamoto control window
	m.addSeparator();
	addGridStackActions(s, m, &s->surfStack);
	m.addSeparator();
	QAction *aRem = m.addAction("Remove");                   // removes surface + colorbar + axes; window stays open
	QAction *c = m.exec(gp);
	if (!c) return;
	if (c == aSave) { saveObjectDialog(s, "grid", nm); return; }
	if (c == aInfo) { runGridInfo(s, nm); return; }
	if (aTransplant && c == aTransplant) { runNestedTransplant(s, nm); return; }
	if (aCube && c == aCube) { g_juliaCubeSlider(s, nm.toUtf8().constData()); return; }
	if (aAqua && c == aAqua) { g_aquamotoReopen(s); return; }
	if (c == aMove) { if (moveObjectToNewWindow(s, "grid", nm)) sceneRemoveSurface(s); return; }
	if (c == aRem) sceneRemoveSurface(s);
}

// Properties menu for a dropped GRID surface (its Scene Objects row). EVERY added element must carry
// a handle menu — for a grid that is: save it to disk, stack it in the grid pile, or delete it.
// Reached by a left- OR right-click on the row label.
static void gridObjectMenu(Scene *s, vtkProp3D *actor, const QPoint &g) {
	int idx = extraIndexOfActor(s, actor);
	if (idx < 0) return;
	const QString nm = QString::fromStdString(s->extras[idx].name);
	QMenu m(s->widget);
	QAction *aSave  = m.addAction("Save grid…");
	QAction *aInfo  = m.addAction("Info (grdinfo)…");
	QAction *aMove  = m.addAction("Move to new window"); // re-open this grid in a fresh iGMT window, then drop it here
	// A "Nested grid N" blank grid (created hollow by the Nested-grids tool) can be filled: implant a
	// 2nd grid, sampled onto this grid's nodes, REPLACING the blank nodes. Julia removes this blank
	// grid + re-adds a filled one under the same name (gmtvtk_remove_grid_h + _add_grid_to_scene).
	QAction *aTransplant = nullptr;
	if (g_juliaEval && gridIsNestedBlank(nm))
		aTransplant = m.addAction("Transplant 2nd grid…");
	QAction *aCube = nullptr;                             // present iff this extra grid is a 3-D cube variable
	if (s->extras[idx].cubeLayers > 1 && g_juliaCubeSlider)
		aCube = m.addAction("Cube layers…");
	m.addSeparator();
	addGridStackActions(s, m, &s->extras[idx].gstack);   // grid-pile draw order (base relief + grids)
	m.addSeparator();
	QAction *aDel   = m.addAction("Remove");
	QAction *c = m.exec(g);
	if (!c) return;
	if (c == aSave) { saveObjectDialog(s, "grid", nm); return; }
	if (c == aInfo) { runGridInfo(s, nm); return; }
	if (aTransplant && c == aTransplant) { runNestedTransplant(s, nm); return; }  // fill blank nested grid
	if (aCube && c == aCube) { g_juliaCubeSlider(s, nm.toUtf8().constData()); return; }
	if (c == aMove || c == aDel) {
		// Move = open this grid in a new window, then delete it from here; a failed move keeps it.
		if (c == aMove && !moveObjectToNewWindow(s, "grid", nm)) return;
		ExtraObj &ex = s->extras[idx];
		if (ex.actor) s->ren->RemoveActor(ex.actor);
		if (ex.drape) s->ren->RemoveActor(ex.drape);
		s->extras.erase(s->extras.begin() + idx);
		applyGridStacking(s);                   // renormalize ranks + retarget colorbar to the new topmost
		rebuildSceneObjects(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	}
}

// Focal-mechanism GROUP properties: compression / dilatation / outline colour + outline width,
// plus the per-event date label (off/on, font/size/colour/bold/italic). EVERY control applies
// IMMEDIATELY (user requirement, 2026-07-04) — there is no Apply/OK gate here, unlike this
// codebase's usual "only the action button executes" dialogs (measure/NSWING/etc, where a text
// field mid-edit is meaningless). A colour/width/font pick IS already a complete, meaningful value
// the moment it's chosen, so each control's own change signal fires the SAME commit routine used
// to round-trip through g_juliaMecaProps (remove the old batch, re-plot from the cached ORIGINAL
// catalog params with the new overrides merged in) — closing the dialog (OK or Cancel/X, both wired
// to just close) never "loses" anything since it already happened live.
static void mecaGroupPropsDialog(Scene *s, const QString &groupName, const QPoint & /*gp*/) {
	const std::string gname = groupName.toStdString();
	MecaGroupProps *gp = nullptr;
	for (auto &g : s->mecaGroups) if (g.name == gname) { gp = &g; break; }
	if (!gp) { s->mecaGroups.push_back(MecaGroupProps{}); gp = &s->mecaGroups.back(); gp->name = gname; }
	if (gp->propsDlg) {                        // already open -> bring it to front, don't stack a duplicate
		gp->propsDlg->raise();
		gp->propsDlg->activateWindow();
		return;
	}
	MecaGroupProps cur = *gp;

	QDialog *dlg = new QDialog(s->widget);
	gp->propsDlg = dlg;
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle("Focal mechanisms — properties");
	QVBoxLayout *lay = new QVBoxLayout(dlg);

	// commit() (assigned further down, once every widget exists) is called from every control's
	// change signal. The dialog is now NON-MODAL (dlg->show(), not exec()) so this function returns
	// immediately — any lambda referencing local stack variables by reference would dangle the
	// instant that happens. Hold `commit` in a heap object via shared_ptr so every connect() lambda
	// (each capturing the shared_ptr BY VALUE, refcounted) keeps it alive for exactly as long as the
	// widgets/connections themselves live, which is exactly the dialog's own lifetime.
	auto commit = std::make_shared<std::function<void()>>();
	auto colorRow = [&](const QString &label, const double rgb[3], const QString &dialogTitle) {
		QHBoxLayout *h = new QHBoxLayout();
		h->addWidget(new QLabel(label, dlg));
		QPushButton *btn = new QPushButton(dlg);
		btn->setFixedWidth(60);
		auto paint = [btn](double r, double g, double b) {
			btn->setProperty("r", r); btn->setProperty("g", g); btn->setProperty("b", b);
			btn->setStyleSheet(QString("background-color: %1;").arg(QColor::fromRgbF(r, g, b).name()));
		};
		paint(rgb[0], rgb[1], rgb[2]);
		QObject::connect(btn, &QPushButton::clicked, [btn, paint, dialogTitle, commit]() {
			QColor init = QColor::fromRgbF(btn->property("r").toDouble(), btn->property("g").toDouble(), btn->property("b").toDouble());
			QColor picked = QColorDialog::getColor(init, btn, dialogTitle);
			if (picked.isValid()) { paint(picked.redF(), picked.greenF(), picked.blueF()); if (*commit) (*commit)(); }
		});
		h->addWidget(btn);
		lay->addLayout(h);
		return btn;
	};
	QPushButton *compBtn  = colorRow("Compression colour:", cur.compColor, "Choose compression colour");
	QPushButton *dilatBtn = colorRow("Dilatation colour:",  cur.dilatColor, "Choose dilatation colour");
	QPushButton *rimBtn   = colorRow("Outline colour:",     cur.rimColor, "Choose outline colour");

	QHBoxLayout *hw = new QHBoxLayout();
	hw->addWidget(new QLabel("Outline width:", dlg));
	QDoubleSpinBox *rimSpin = new QDoubleSpinBox(dlg);
	rimSpin->setRange(0.0, 10.0);  rimSpin->setSingleStep(0.1);  rimSpin->setSuffix(" %");
	rimSpin->setValue(cur.rimWidthPct);
	hw->addWidget(rimSpin);
	lay->addLayout(hw);

	// Per-event date label — OFF by default (matches the import dialog's chkPlotEventDate default).
	// Font row only makes sense once the checkbox is on, so it's enabled/disabled alongside it.
	QCheckBox *dateChk = new QCheckBox("Plot event date", dlg);
	dateChk->setChecked(cur.plotDate);
	lay->addWidget(dateChk);

	QWidget *fontRow = new QWidget(dlg);
	QHBoxLayout *hf = new QHBoxLayout(fontRow);
	hf->setContentsMargins(0, 0, 0, 0);
	hf->addWidget(new QLabel("Date font:", fontRow));
	QComboBox *dateFontCombo = new QComboBox(fontRow);
	dateFontCombo->addItems({ "Arial", "Courier", "Times" });   // textApplyProps' supported font families
	dateFontCombo->setCurrentText(QString::fromStdString(cur.dateFont));
	hf->addWidget(dateFontCombo);
	QSpinBox *dateSizeSpin = new QSpinBox(fontRow);
	dateSizeSpin->setRange(4, 300);  dateSizeSpin->setValue(cur.dateFontSize);
	hf->addWidget(dateSizeSpin);
	QCheckBox *dateBoldChk = new QCheckBox("Bold", fontRow);     dateBoldChk->setChecked(cur.dateBold);
	QCheckBox *dateItalChk = new QCheckBox("Italic", fontRow);  dateItalChk->setChecked(cur.dateItalic);
	hf->addWidget(dateBoldChk);
	hf->addWidget(dateItalChk);
	QPushButton *dateColorBtn = new QPushButton(fontRow);
	dateColorBtn->setFixedWidth(60);
	auto paintDateColor = [dateColorBtn](double r, double g, double b) {
		dateColorBtn->setProperty("r", r); dateColorBtn->setProperty("g", g); dateColorBtn->setProperty("b", b);
		dateColorBtn->setStyleSheet(QString("background-color: %1;").arg(QColor::fromRgbF(r, g, b).name()));
	};
	paintDateColor(cur.dateColor[0], cur.dateColor[1], cur.dateColor[2]);
	QObject::connect(dateColorBtn, &QPushButton::clicked, [dateColorBtn, paintDateColor, commit]() {
		QColor init = QColor::fromRgbF(dateColorBtn->property("r").toDouble(), dateColorBtn->property("g").toDouble(), dateColorBtn->property("b").toDouble());
		QColor picked = QColorDialog::getColor(init, dateColorBtn, "Choose date colour");
		if (picked.isValid()) { paintDateColor(picked.redF(), picked.greenF(), picked.blueF()); if (*commit) (*commit)(); }
	});
	hf->addWidget(dateColorBtn);
	lay->addWidget(fontRow);
	fontRow->setEnabled(dateChk->isChecked());
	QObject::connect(dateChk, &QCheckBox::toggled, fontRow, &QWidget::setEnabled);

	// Placeholder — NOT wired to commit()/g_juliaMecaProps, no MecaGroupProps field: purely a
	// locked stub until repel is actually implemented (planned: GMT.jl's textrepel.jl or a
	// variant of its algorithm, to spread overlapping beachballs apart).
	QCheckBox *repelChk = new QCheckBox("Repel", dlg);
	repelChk->setToolTip("Repel the beachballs such that they do not overlap");
	repelChk->setEnabled(false);
	lay->addWidget(repelChk);

	// Single "Close" button — Qt maps it to the box's rejected() signal; nothing to Apply/Cancel
	// anymore since every control already committed live, so it just closes the (WA_DeleteOnClose) dialog.
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
	QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::close);
	lay->addWidget(bb);

	// The commit routine: read every widget's CURRENT value, update the s->mecaGroups cache, and
	// round-trip through g_juliaMecaProps — this is what USED to run only after `dlg.exec()`
	// accepted; now every control's own change signal calls it directly, so the 3-D view updates
	// as each value is picked instead of waiting for a button that no longer exists.
	*commit = [=]() {
		double cc[3] = { compBtn->property("r").toDouble(),  compBtn->property("g").toDouble(),  compBtn->property("b").toDouble() };
		double dc[3] = { dilatBtn->property("r").toDouble(), dilatBtn->property("g").toDouble(), dilatBtn->property("b").toDouble() };
		double rc[3] = { rimBtn->property("r").toDouble(),   rimBtn->property("g").toDouble(),   rimBtn->property("b").toDouble() };
		const double rimPct = rimSpin->value();
		const bool plotDate = dateChk->isChecked();
		const std::string dateFont = dateFontCombo->currentText().toStdString();
		const int dateFontSize = dateSizeSpin->value();
		double dtc[3] = { dateColorBtn->property("r").toDouble(), dateColorBtn->property("g").toDouble(), dateColorBtn->property("b").toDouble() };
		const bool dateBold = dateBoldChk->isChecked();
		const bool dateItalic = dateItalChk->isChecked();

		auto fillProps = [&](MecaGroupProps &g) {
			g.compColor[0]=cc[0]; g.compColor[1]=cc[1]; g.compColor[2]=cc[2];
			g.dilatColor[0]=dc[0]; g.dilatColor[1]=dc[1]; g.dilatColor[2]=dc[2];
			g.rimColor[0]=rc[0]; g.rimColor[1]=rc[1]; g.rimColor[2]=rc[2];
			g.rimWidthPct = rimPct;
			g.plotDate = plotDate;
			g.dateFont = dateFont;
			g.dateFontSize = dateFontSize;
			g.dateColor[0]=dtc[0]; g.dateColor[1]=dtc[1]; g.dateColor[2]=dtc[2];
			g.dateBold = dateBold;
			g.dateItalic = dateItalic;
		};
		bool found = false;
		for (auto &g : s->mecaGroups) if (g.name == gname) { fillProps(g); found = true; break; }
		if (!found) {
			MecaGroupProps g; g.name = gname;
			fillProps(g);
			s->mecaGroups.push_back(g);
		}

		if (!g_juliaMecaProps) { sceneLogError(s, "Focal mechanisms: properties callback not registered"); return; }
		char buf[512];
		std::snprintf(buf, sizeof(buf),
		              "compcolor=%d/%d/%d\ndilatcolor=%d/%d/%d\nrimcolor=%d/%d/%d\nrimwidth=%.5f\n"
		              "plotdate=%d\ndatefont=%s\ndatefontsize=%d\ndatecolor=%d/%d/%d\ndatebold=%d\ndateitalic=%d",
		              (int)(cc[0]*255.0+0.5), (int)(cc[1]*255.0+0.5), (int)(cc[2]*255.0+0.5),
		              (int)(dc[0]*255.0+0.5), (int)(dc[1]*255.0+0.5), (int)(dc[2]*255.0+0.5),
		              (int)(rc[0]*255.0+0.5), (int)(rc[1]*255.0+0.5), (int)(rc[2]*255.0+0.5),
		              rimPct / 100.0,
		              plotDate ? 1 : 0, dateFont.c_str(), dateFontSize,
		              (int)(dtc[0]*255.0+0.5), (int)(dtc[1]*255.0+0.5), (int)(dtc[2]*255.0+0.5),
		              dateBold ? 1 : 0, dateItalic ? 1 : 0);
		g_juliaMecaProps(s, gname.c_str(), buf);
	};

	// Colour buttons (compBtn/dilatBtn/rimBtn via colorRow, dateColorBtn above) are already wired to
	// commit() at construction. Wire the remaining controls the same way — QColorDialog::getColor is
	// itself a blocking modal (fires once on its own OK, never mid-drag), so none of this can spam
	// the round-trip faster than the user actually picks a new value.
	QObject::connect(rimSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [commit](double) { if (*commit) (*commit)(); });
	QObject::connect(dateChk, &QCheckBox::toggled, [commit](bool) { if (*commit) (*commit)(); });
	QObject::connect(dateFontCombo, &QComboBox::currentTextChanged, [commit](const QString &) { if (*commit) (*commit)(); });
	QObject::connect(dateSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged), [commit](int) { if (*commit) (*commit)(); });
	QObject::connect(dateBoldChk, &QCheckBox::toggled, [commit](bool) { if (*commit) (*commit)(); });
	QObject::connect(dateItalChk, &QCheckBox::toggled, [commit](bool) { if (*commit) (*commit)(); });

	dlg->show();   // non-modal: the 3-D view stays interactive while values are picked live
}

static void rebuildSceneObjects(Scene *s) {
	if (!s || !s->objPanel)
		return;
	// Wipe the previous layout + its checkboxes before rebuilding. deleteLater() (not delete) is
	// deliberate -- this can run reentrantly from inside a row's own signal (a checkbox toggle that
	// triggers a visibility change that ends up calling back in here), so the old row widgets must
	// outlive the current call stack. But a caller that fires several rebuilds back-to-back with no
	// real event-loop turn between them (session replay: one rebuild per recipe, all inside one
	// synchronous Julia call) would otherwise have the OLD tree widget still alive and painting
	// UNDER the new one, overlapping row text until the deferred delete eventually lands. Flushing
	// DeferredDelete events immediately (safe -- it only runs queued destructors, it doesn't reenter
	// arbitrary widget code) guarantees the old tree is actually gone before the new one is built.
	if (QLayout *old = s->objPanel->layout()) {
		QLayoutItem *it;
		while ((it = old->takeAt(0)) != nullptr) {
			if (it->widget())
				it->widget()->deleteLater();
			delete it;
		}
		delete old;
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	}
	// Small checkboxes: the indicator is the ONLY hit target for show/hide. The type icon + the
	// descriptive label sit to its right; right-clicking the icon/label opens the row's properties
	// menu (toggle is no longer wired to the text — Fledermaus behaviour the user asked for).
	s->objPanel->setStyleSheet("QCheckBox::indicator{width:13px;height:13px;}"
	                           "QTreeWidget{border:none;background:transparent;}");
	QVBoxLayout *col = new QVBoxLayout(s->objPanel);
	col->setContentsMargins(2, 2, 2, 2);
	col->setSpacing(0);

	// The panel is a real Qt tree: each grid / image / fault is a collapsible PARENT node, its parts
	// (surface, drape, colorbar, axes — or trace, surface-projection, fault-plane) are CHILD rows.
	// Each leaf still hosts the exact same [checkbox][icon][label] widget (so the checkbox alone
	// toggles, and a left-click on the label opens properties); the tree only adds the grouping.
	QTreeWidget *tree = new QTreeWidget(s->objPanel);
	tree->setHeaderHidden(true);
	tree->setColumnCount(1);
	tree->setRootIsDecorated(true);              // show expand / collapse arrows on parent nodes
	tree->setUniformRowHeights(false);           // child rows size to their hosted widget
	tree->setSelectionMode(QAbstractItemView::NoSelection);
	tree->setFocusPolicy(Qt::NoFocus);
	tree->setIndentation(14);
	col->addWidget(tree);

	// curParent: the group node that newly-made rows attach to. null = top level.
	QTreeWidgetItem *curParent = nullptr;
	// parentStack: lets beginGroupHandle/beginSlipGroup NEST (a group opened while curParent is
	// already set attaches as ITS child instead of a new top-level item) -- used by the Aquamoto file
	// wrapper (one variable's group nested inside the main per-file group). endGroup restores the
	// OUTER curParent instead of unconditionally nulling it, so non-nested callers (the overwhelming
	// majority) see no behaviour change: push null, pop null.
	std::vector<QTreeWidgetItem*> parentStack;

	// One row = [checkbox] [type icon] [label] hosted in a tree item (child of curParent, else top-level).
	// onToggle drives visibility; onProps (optional) is the properties menu, opened by a LEFT click on the
	// description label (the checkbox only toggles).
	auto makeRow = [&](const QString &label, int iconKind, bool checked,
	                   std::function<void(bool)> onToggle,
	                   std::function<void(const QPoint&)> onProps,
	                   const QString &tip = QString(),
	                   std::function<void(const QPoint&)> onContext = nullptr) {
		QWidget *row = new QWidget(tree);
		QHBoxLayout *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 1, 0, 1);
		h->setSpacing(5);

		QCheckBox *cb = new QCheckBox(row);                  // box only — no text, so only the box toggles
		cb->setChecked(checked);
		cb->setToolTip("Show / hide");
		QObject::connect(cb, &QCheckBox::toggled, [s, onToggle](bool on) {
			onToggle(on);
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		});

		QLabel *icon = new QLabel(row); icon->setPixmap(makeObjectIcon(iconKind));
		ClickableLabel *text = new ClickableLabel(label, row);   // left-click -> properties
		if (!tip.isEmpty()) { icon->setToolTip(tip); text->setToolTip(tip); }

		h->addWidget(cb, 0);
		h->addWidget(icon, 0);
		h->addWidget(text, 1);

		if (onProps) {                                      // left-click the description -> properties menu
			text->setCursor(Qt::PointingHandCursor);
			text->onClick = onProps;
		}
		if (onContext) text->onRightClick = onContext;      // right-click the description -> context menu (Save…)

		QTreeWidgetItem *item = new QTreeWidgetItem();
		if (curParent) curParent->addChild(item); else tree->addTopLevelItem(item);
		tree->setItemWidget(item, 0, row);
	};

	// beginGroupHandle / endGroup: a collapsible PARENT node that is ITSELF a full handle — its header
	// hosts the exact same [checkbox][icon][label] widget a leaf row does, so the container toggles and
	// carries its own properties menu (same options as the Surface/Image handle it stands for). Rows made
	// between it and endGroup attach as its children. onToggle drives the container's show/hide; onProps
	// (left-click) / onContext (right-click) are its properties menus. Any may be null.
	auto beginGroupHandle = [&](const QString &name, int iconKind, bool checked,
	                            std::function<void(const QPoint&)> onProps,
	                            std::function<void(const QPoint&)> onContext,
	                            const QString &tip = QString()) {
		QTreeWidgetItem *grp = new QTreeWidgetItem();
		if (curParent) curParent->addChild(grp); else tree->addTopLevelItem(grp);
		grp->setExpanded(true);
		parentStack.push_back(curParent);
		curParent = grp;

		QWidget *row = new QWidget(tree);
		QHBoxLayout *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 1, 0, 1);
		h->setSpacing(5);

		QCheckBox *cb = new QCheckBox(row);                  // box only — toggles the WHOLE group
		cb->setChecked(checked);
		cb->setToolTip("Show / hide the whole group");
		// The container checkbox drives EVERY containee: it flips each child row's own checkbox, so every
		// part (surface, drape, colorbar, axes, trace, plane, …) toggles through its own handler.
		QObject::connect(cb, &QCheckBox::toggled, [s, grp, tree](bool on) {
			for (int i = 0; i < grp->childCount(); ++i) {
				QWidget *cw = tree->itemWidget(grp->child(i), 0);
				if (!cw) continue;
				QCheckBox *ccb = cw->findChild<QCheckBox*>();
				if (ccb && ccb->isChecked() != on) ccb->setChecked(on);   // fires the child's own toggle
			}
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		});

		QLabel *icon = new QLabel(row); icon->setPixmap(makeObjectIcon(iconKind));
		ClickableLabel *text = new ClickableLabel(name, row);
		QFont f = text->font(); f.setBold(true); text->setFont(f);   // container labels stay bold
		if (!tip.isEmpty()) { icon->setToolTip(tip); text->setToolTip(tip); }
		if (onProps)   { text->setCursor(Qt::PointingHandCursor); text->onClick = onProps; }
		if (onContext) text->onRightClick = onContext;

		h->addWidget(cb, 0);
		h->addWidget(icon, 0);
		h->addWidget(text, 1);
		tree->setItemWidget(grp, 0, row);
	};
	auto endGroup = [&]() {
		curParent = parentStack.empty() ? nullptr : parentStack.back();
		if (!parentStack.empty()) parentStack.pop_back();
	};

	// Slip-model group (Import Model Slip): a collapsible parent with a "Delete group" property.
	// Replaces the default beginGroup for slip groups to add the delete menu.
	auto beginSlipGroup = [&](const QString &name, int iconKind = IC_Rect) {
		QTreeWidgetItem *grp = new QTreeWidgetItem();
		if (curParent) curParent->addChild(grp); else tree->addTopLevelItem(grp);
		grp->setExpanded(false);   // start collapsed: a slip model is many patches
		parentStack.push_back(curParent);
		curParent = grp;

		// Custom row widget: [checkbox] [icon] [label] with right-click delete menu.
		QWidget *row = new QWidget(tree);
		QHBoxLayout *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 1, 0, 1);
		h->setSpacing(5);

		// Checkbox toggles all children in this group.
		QCheckBox *cb = new QCheckBox(row);
		cb->setToolTip("Show / hide all patches");
		cb->setChecked(true);   // start checked: patches visible by default
		QObject::connect(cb, &QCheckBox::toggled, [s, name](bool on) {
			std::string gname = name.toStdString();
			// Toggle all patches with this groupName directly on their actors.
			for (auto& pg : s->polys) {
				if (pg.groupName != gname) continue;
				if (pg.line)  pg.line->SetVisibility(on ? 1 : 0);
				if (pg.fill)  pg.fill->SetVisibility((on && pg.fill->GetProperty()->GetOpacity() > 0.0) ? 1 : 0);
			}
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		});

		QLabel *icon = new QLabel(row); icon->setPixmap(makeObjectIcon(iconKind));
		ClickableLabel *text = new ClickableLabel(name, row);
		QString tip = "Right-click to delete the entire slip model";
		icon->setToolTip(tip); text->setToolTip(tip);

		h->addWidget(cb, 0);
		h->addWidget(icon, 0);
		h->addWidget(text, 1);

		// Right-click opens delete menu.
		text->onRightClick = [s, name](const QPoint& g) {
			QMenu m(s->widget);
			QAction *aDel = m.addAction(QString("Delete group (%1)").arg(name));
			if (m.exec(g) == aDel) {
				deleteSlipGroup(s, name);
			}
		};

		tree->setItemWidget(grp, 0, row);
	};

	// Actor-backed rows (show/hide = actor visibility); lr != null adds the line-object menu.
	auto addRow = [&](const QString& label, vtkProp3D *a, int iconKind, const LineRef *lr = nullptr) {
		if (!a)
			return;
		std::function<void(const QPoint&)> ctx = nullptr;
		if (lr) { LineRef ref = *lr; QString nm = label;
			ctx = [s, ref, nm](const QPoint& g) { popupLineObjectMenu(s, ref, nm, g); }; }
		makeRow(label, iconKind, a->GetVisibility() != 0,
		        [a](bool on) { a->SetVisibility(on ? 1 : 0); }, ctx,
		        lr ? QString("Left-click for properties") : QString());
	};

	// Right-click handler factory for a saveable grid/image row: pops a one-item "Save…" menu that
	// opens the format-picker dialog for that object (kind = "grid" | "image"; name = the row label,
	// matched against the Julia object store). kind is a string literal (static lifetime).
	auto saveCtx = [s](const char *kind, const QString& name) {
		return [s, kind, name](const QPoint& g) {
			QMenu m(s->widget);
			QAction *a = m.addAction(QString("Save %1…").arg(kind));
			if (m.exec(g) == a) saveObjectDialog(s, kind, name);
		};
	};

	// This grid's COLORBAR row: per-grid show/hide intent (*flag = &s->surfShowBar or &ex.showBar,
	// honoured by refreshGridColorbar when the grid is active) + colormap chooser on the label.
	// grpVisible gates the row's INITIAL checkbox so a hidden grid group's children start UNCHECKED,
	// matching its unchecked container (the sacred law: children mirror the container). A hidden
	// nested grid must not show a ticked Color Bar. Default true keeps every visible grid unchanged.
	auto colorbarRow = [&](bool *flag, int gridSel, bool grpVisible = true) {
		makeRow("Color Bar", IC_ColorBar, *flag && grpVisible,
		        [s, flag](bool on) { *flag = on; refreshGridColorbar(s); },
		        [s, gridSel](const QPoint& g) {
		            chooseColormap(s, g, [s, gridSel](const QString& nm) { applyColormap(s, nm, gridSel); });
		        },
		        "Show / hide this grid's colorbar · left-click the label to choose a colormap");
	};
	// Aquamoto's two colorbar rows. Unlike the generic colorbarRow above (checkbox = a separate
	// "want this shown" intent flag, meaningful because there's only ever ONE active grid to be
	// wrong about), these checkboxes reflect the bar's ACTUAL on-screen visibility directly --
	// otherwise, since only ONE of water/land is ever drawn at a time, the un-shown side's checkbox
	// would stay stuck exactly as the user last left it and never visibly react to the Shade
	// Water/Land radio, which is precisely the "not toggling" bug this replaces. Checking either
	// box also SWITCHES the active side (mirrors clicking the corresponding radio); unchecking one
	// just hides it without touching the other.
	// Colormap picks on either side go through g_aquamotoSetCmap (75_aquamoto.cpp): it sets the
	// side's cmap in the Julia _AquaState and re-renders the CURRENT slice, unlike the generic
	// applyColormap/gmtvtk_set_cpt_grid path (a host-composited texture has no scalar+LUT surface
	// for that to recolour — see the flat-image-bake bug this replaces).
	auto aquaWaterColorbarRow = [&]() {
		const bool vis = s->bar && s->bar->GetVisibility() != 0;
		makeRow("Color Bar water", IC_ColorBar, vis,
		        [s](bool on) { s->surfShowBar = on; if (on) s->aquaShowWater = true; refreshGridColorbar(s); rebuildSceneObjects(s); },
		        [s](const QPoint& g) {
		            chooseColormap(s, g, [s](const QString& nm) {
		                if (g_aquamotoSetCmap) g_aquamotoSetCmap(s, 0, nm.toUtf8().constData());
		            });
		        },
		        "Show / hide the Water colorbar · checking it switches to Shade Water · left-click the label to choose a colormap");
	};
	auto aquaLandColorbarRow = [&]() {
		const bool vis = s->aquaLandBar && s->aquaLandBar->GetVisibility() != 0;
		makeRow("Color Bar Land", IC_ColorBar, vis,
		        [s](bool on) { s->aquaLandShowBar = on; if (on) s->aquaShowWater = false; refreshGridColorbar(s); rebuildSceneObjects(s); },
		        [s](const QPoint& g) {
		            chooseColormap(s, g, [s](const QString& nm) {
		                if (g_aquamotoSetCmap) g_aquamotoSetCmap(s, 1, nm.toUtf8().constData());
		            });
		        },
		        "Show / hide the Land colorbar · checking it switches to Shade Land · left-click the label to choose a colormap");
	};
	// Per-grid / per-image AXES handle. Properties come LATER; for now the box toggles the cube axes
	// and the label shows a placeholder. Every grid (and referenced image) carries one. grpVisible
	// gates the initial checkbox so a hidden group's Axes row starts unchecked (see colorbarRow).
	auto axesRow = [&](bool grpVisible = true) {
		const bool av = grpVisible && s->axes && s->axes->GetVisibility() != 0;
		makeRow("Axes", IC_Axes, av,
		        [s](bool on) { if (s->axes) s->axes->SetVisibility(on ? 1 : 0); },
		        [s](const QPoint&) { if (s->win) s->win->statusBar()->showMessage("Axes properties — coming soon", 2500); },
		        "Axes handle (properties coming soon)");
	};

	// ── AQUAMOTO FILE WRAPPER ── every variable loaded from the open tsunami netCDF -- the composited
	// water/land surface below, PLUS bathymetry and any other static grid loaded alongside it as an
	// extra -- nests under ONE collapsible parent named after the file: each variable gets its own
	// group, all hosted in the main group for the file. Scoped strictly to customLayerTexture (an
	// Aquamoto window) so a plain window's base grid + extras render exactly as before.
	const bool aquaWrap = s->customLayerTexture;
	if (aquaWrap) {
		const QString fileNm = s->surfName.empty() ? QString("Tsunami") : QString::fromStdString(s->surfName);
		beginGroupHandle(fileNm, IC_Surface, true, nullptr, nullptr, "Every variable loaded from this file");
	}

	// ── GRID GROUPS ── each grid = [surface][drape?][colorbar][axes], split by a light rule. A bare
	// image (view_image) is its own group (image row + axes). Non-grid objects follow, after a rule.
	if (!s->imageOnly) {
		if (vtkProp3D *sp = surfProp(s)) {                  // base relief grid group — header IS the surface handle
			const QString nm = (s->customLayerTexture && !s->aquaVarLabel.empty()) ? QString::fromStdString(s->aquaVarLabel)
			                  : s->surfName.empty() ? QString("Surface") : QString::fromStdString(s->surfName);
			beginGroupHandle(nm, IC_Surface, sp->GetVisibility() != 0,
			        nullptr,                                              // container does NOT fold the Shading dock (the Surface leaf does)
			        [s](const QPoint& g) { surfaceObjectMenu(s, g); },
			        "Checkbox toggles the whole group · right-click for save / stacking");
			makeRow("Surface", IC_Surface, sp->GetVisibility() != 0,     // Surface leaf handle kept as a child
			        [s, sp](bool on) { sp->SetVisibility(on ? 1 : 0); refreshGridColorbar(s); },
			        [s](const QPoint&) { toggleShadingFold(s); },
			        "Left-click to fold / un-fold the Shading panel · right-click for save / stacking",
			        [s](const QPoint& g) { surfaceObjectMenu(s, g); });
			if (s->drape) addRow(QString("Image drape"), s->drape, IC_Image);   // grid's drape texture
			// Base-surface children must mirror the container's hidden state exactly like an extra
			// grid's children do (colorbarRow/axesRow below, in the s->extras loop) — WITHOUT this,
			// hiding the base surface (e.g. SACRED_LAW.md's "uncheck the source" when a derived
			// result replaces it) left its Color Bar / Axes rows still shown checked, because both
			// calls here used to omit the `grpVisible` argument entirely (defaults to true).
			const bool baseVis = sp->GetVisibility() != 0;
			if (s->customLayerTexture) {         // Aquamoto: same file group, but each variable's row is
				                                  // its OWN independent handle -- never merged into one
				                                  // combined label/row (that would mix variables together).
				aquaWaterColorbarRow();
				aquaLandColorbarRow();
			} else {
				colorbarRow(&s->surfShowBar, -1, baseVis);    // base relief grid
			}
			axesRow(baseVis);
			endGroup();
		}
	} else if (s->drape) {                                  // bare image (view_image) group — header IS the image handle
		const QString nm = s->surfName.empty() ? QString("Image") : QString::fromStdString(s->surfName);
		vtkProp3D *dp = s->drape;
		std::function<void(const QPoint&)> imgMenu = [s, nm](const QPoint& g) {   // primary image props: Save + Remove
			QMenu m(s->widget);
			// Percentile histogram stretch -> new 8-bit image row (meaningful for a wide-range e.g.
			// 16-bit satellite band shown as a fast min-max preview; Julia reports if no wider source).
			QAction *aStretch = m.addAction("Auto histogram stretch (new image)");
			QAction *aSave = m.addAction("Save image…");
			m.addSeparator();
			QAction *aRem  = m.addAction("Remove"); // removes image + axes; window stays open
			QAction *c = m.exec(g);
			if (!c) return;
			if (c == aStretch) stretchImageObject(s, nm);
			else if (c == aSave) saveObjectDialog(s, "image", nm);
			else if (c == aRem) sceneRemoveSurface(s);
		};
		beginGroupHandle(nm, IC_Image, dp->GetVisibility() != 0,
		        imgMenu, imgMenu,
		        "Left- or right-click for properties (save / remove)");
		makeRow("Image", IC_Image, dp->GetVisibility() != 0,        // Image leaf handle kept as a child
		        [dp](bool on) { dp->SetVisibility(on ? 1 : 0); }, nullptr,
		        "Right-click for properties (save / remove)", imgMenu);
		axesRow(dp->GetVisibility() != 0);   // group-uncheck law: Axes mirrors the container (see grid case above)
		endGroup();
	}

	for (size_t ei = 0; ei < s->extras.size(); ++ei) {      // dropped grids / images: one group each
		auto& ex = s->extras[ei];
		const QString nm = QString::fromStdString(ex.name);
		if (ex.isImage) {                                  // dropped image group — header IS the image handle
			vtkProp3D *a = ex.actor.Get();
			beginGroupHandle(nm, IC_Image, a && a->GetVisibility() != 0,
			        [s, a](const QPoint& g) { imageObjectMenu(s, a, g); },
			        [s, a](const QPoint& g) { imageObjectMenu(s, a, g); },
			        "Left- or right-click for image properties (incl. Save)");
			makeRow("Image", IC_Image, a && a->GetVisibility() != 0,   // Image leaf handle kept as a child
			        [a](bool on) { if (a) a->SetVisibility(on ? 1 : 0); },
			        [s, a](const QPoint& g) { imageObjectMenu(s, a, g); },
			        "Left- or right-click for image properties (incl. Save)",
			        [s, a](const QPoint& g) { imageObjectMenu(s, a, g); });
			axesRow(a && a->GetVisibility() != 0);   // group-uncheck law: Axes mirrors the container
		} else {                                           // dropped grid group — header mirrors the surface handle
			vtkProp3D *a = ex.actor.Get();
			beginGroupHandle(nm, IC_Surface, a && a->GetVisibility() != 0,
			        nullptr,                                               // container does NOT fold the Shading dock (the Surface leaf does)
			        [s, a](const QPoint& g) { gridObjectMenu(s, a, g); },  // right-click: save / delete
			        "Checkbox toggles the whole group · right-click to save / delete");
			makeRow("Surface", IC_Surface, a && a->GetVisibility() != 0,   // Surface leaf handle kept as a child
			        [s, a](bool on) { if (a) a->SetVisibility(on ? 1 : 0); refreshGridColorbar(s); },
			        [s](const QPoint&) { toggleShadingFold(s); },
			        "Left-click for Shading · right-click to save / delete",
			        [s, a](const QPoint& g) { gridObjectMenu(s, a, g); });
			if (ex.drape) addRow("Image drape", ex.drape, IC_Image);
			const bool gvis = a && a->GetVisibility() != 0;   // hidden grid -> children start unchecked (mirror the container)
			colorbarRow(&ex.showBar, ex.tag, gvis);    // resolve by the grid's UNIQUE tag, not its (shifting) index
			axesRow(gvis);
		}
		endGroup();
	}

	if (aquaWrap) endGroup();   // close the per-file wrapper opened above

	// ── OTHER OBJECTS ── lines / points / curtains / polygons / text / profile (top-level rows; a fault
	// with planes becomes its own group, see below).
	QString ovlGroupOpen;                                // name of the currently-open overlay-group node (empty = none)
	for (auto& ov : s->overlays) {
		// Overlays sharing a non-empty groupName (e.g. Geography > Plate boundaries' 7 boundary-type
		// layers, added back-to-back in one batch) fold under ONE collapsible parent row instead of
		// flooding the list -- same consecutive-run fold the slip-model patches use above. The
		// container's own checkbox cascades to every child row's checkbox via beginGroupHandle's
		// existing toggle handler, so unchecking the group hides every member (SACRED_LAW: group-
		// uncheck cascades to every child, full stop).
		if (!ovlGroupOpen.isEmpty() && QString::fromStdString(ov.groupName) != ovlGroupOpen) {
			endGroup();  ovlGroupOpen.clear();
		}
		if (!ov.groupName.empty() && ovlGroupOpen.isEmpty()) {
			const std::string gn = ov.groupName;
			beginGroupHandle(QString::fromStdString(ov.groupName), IC_Line,
			                 ov.actor && ov.actor->GetVisibility() != 0, nullptr,
			                 [s, gn](const QPoint& g) {
			                     QMenu m(s->widget);
			                     QAction *aRem = m.addAction("Remove");
			                     if (m.exec(g) == aRem) overlayDeleteGroup(s, gn);
			                 },
			                 "Right-click to remove every line in this group");
			ovlGroupOpen = QString::fromStdString(ov.groupName);
		}
		// popupLineObjectMenu handles LK_Overlay for EITHER mode already (Convert to points/line +
		// Delete) -- gating &lr to line-mode only left every points-mode overlay (a SHAPENC Point
		// ensemble, a dropped x,y table converted via "Convert to points", ...) with NO context menu
		// at all: no properties, no Delete, not even the "Left-click for properties" tooltip. Every
		// Scene Objects element needs a working properties/Remove menu (SACRED_LAW.md); this was the
		// one row builder that silently dropped it for one sub-kind of the SAME element type.
		LineRef lr{ LK_Overlay, ov.actor };
		addRow(QString::fromStdString(ov.name), ov.actor, ov.mode == 1 ? IC_Line : IC_Points, &lr);
	}
	if (!ovlGroupOpen.isEmpty()) endGroup();
	for (auto& sl : s->symbols) {                        // screen-constant symbol layers (props menu)
		vtkActor *a = sl.actor.Get();
		makeRow(QString::fromStdString(sl.name), IC_Points, a && a->GetVisibility() != 0,
		        [a](bool on) { if (a) a->SetVisibility(on ? 1 : 0); },
		        [s, a](const QPoint& g) { symbolLayerMenu(s, a, g); },
		        "Left-click for symbol properties");
	}
	for (auto& cu : s->curtains)
		addRow(QString::fromStdString(cu.name), cu.actor, IC_Curtain);
	QString slipGroupOpen;                               // name of the currently-open slip-patch group node (empty = none)
	std::set<std::string> mecaGroupsShown;               // focal-mechanism groupNames already given their ONE row
	for (auto& pg : s->polys) {                          // user-drawn polygons / polylines / rects / circles
		// Focal-mechanism beachball patches (comp/dilat/rim-ring, dozens to hundreds per catalog) get
		// NO individual rows at all — just ONE row per batch (groupName), first time it's seen. Left-
		// click opens the colours/outline properties dialog; right-click offers Remove.
		if (pg.isMeca) {
			if (mecaGroupsShown.insert(pg.groupName).second) {
				const QString gname = QString::fromStdString(pg.groupName);
				// A ball's rim/nodal-plane STROKE is `p.line` (mecaBuildLines), a SEPARATE actor from the
				// comp/dilat FILL (`p.fill`, mecaBuildPatch) — scan the whole group for either, so the row's
				// checkbox is never stuck unchecked just because the first-seen patch is line-only.
				bool vis = false;
				for (auto& p : s->polys) if (p.isMeca && p.groupName == pg.groupName &&
				                              ((p.fill && p.fill->GetVisibility() != 0) || (p.line && p.line->GetVisibility() != 0))) { vis = true; break; }
				makeRow(gname, IC_Beachball, vis,
				        [s, gname](bool on) {
				            const std::string gn = gname.toStdString();
				            for (auto& p : s->polys) if (p.isMeca && p.groupName == gn) {
				                if (p.fill) p.fill->SetVisibility(on ? 1 : 0);
				                if (p.line) p.line->SetVisibility(on ? 1 : 0);
				            }
				        },
				        [s, gname](const QPoint& g) { mecaGroupPropsDialog(s, gname, g); },
				        "Left-click for properties (colours, outline) · right-click to remove",
				        [s, gname](const QPoint& g) {
				            QMenu m(s->widget);
				            QAction *aRem = m.addAction("Remove");
				            if (m.exec(g) == aRem) deleteMecaGroup(s, gname);
				        });
			}
			continue;
		}
		// Slip-model patches (Import Model Slip) carry a groupName: fold every consecutive patch that
		// shares it under ONE collapsible parent node so 100s of patches don't flood the panel as a flat
		// list. Close the open group when the run ends or the name changes.
		if (!slipGroupOpen.isEmpty() && QString::fromStdString(pg.groupName) != slipGroupOpen) {
			endGroup();  slipGroupOpen.clear();
		}
		if (!pg.groupName.empty() && slipGroupOpen.isEmpty()) {
			beginSlipGroup(QString::fromStdString(pg.groupName), IC_Rect);
			slipGroupOpen = QString::fromStdString(pg.groupName);
		}
		LineRef lr{ LK_Polygon, pg.line };
		const QString nm = QString::fromStdString(pg.name);   // name is prefixed per type by polyFinalize
		// Icon by SHAPE, gated on vertex count for open chains (a 2-point line is a LINE, not a
		// polyline/polygon): fault/line with 2 vertices -> straight-line icon; >2 -> polyline icon.
		// Closed rings -> rect / circle / polygon.
		int ic = pg.isFault              ? (pg.v.size() > 2 ? IC_Polyline : IC_StraightLine)
		       : !pg.groupName.empty()   ? IC_Rect      // slip-model patches are rectangles, not generic polygons
		       : pg.nestKind == 1        ? IC_NestRect
		       : !pg.closed              ? (pg.v.size() > 2 ? IC_Polyline : IC_StraightLine)
		       : nm.startsWith("rect")   ? IC_Rect
		       : nm.startsWith("circle") ? IC_Circle
		                                 : IC_Polygon;
		// A fault that carries a surface-projection and/or a 3-D plane becomes its OWN collapsible group
		// (trace + surface projection + fault plane as children). A bare 2-point trace stays a top-level row.
		const bool faultGroup = pg.isFault && (pg.faultPlane || pg.faultPlane3D);
		if (faultGroup) {                                   // container checkbox toggles trace + projection + plane at once
			LineRef cref = lr; QString cnm = nm;
			const bool anyVis = pg.line && pg.line->GetVisibility() != 0;
			beginGroupHandle(nm, ic, anyVis,
			        [s, cref, cnm](const QPoint& g) { popupLineObjectMenu(s, cref, cnm, g); },
			        [s, cref, cnm](const QPoint& g) { popupLineObjectMenu(s, cref, cnm, g); },
			        "Left- or right-click for fault properties");
		}

		// Custom row (not addRow) so the filled FACE follows the outline's checkbox; the fill's own
		// opacity still gates whether anything is actually drawn (opacity 0 = no fill).
		{
			vtkActor *la = pg.line.Get();
			vtkActor *fa = pg.fill.Get();
			LineRef ref = lr; QString nm2 = nm;
			makeRow(faultGroup ? QString("Trace") : nm, ic, la && la->GetVisibility() != 0,
			        [la, fa](bool on) {
			            if (la) la->SetVisibility(on ? 1 : 0);
			            if (fa) fa->SetVisibility((on && fa->GetProperty()->GetOpacity() > 0.0) ? 1 : 0);
			        },
			        [s, ref, nm2](const QPoint& g) { popupLineObjectMenu(s, ref, nm2, g); },
			        "Left-click for properties");
		}

		// The gray surface-PROJECTION patch is its OWN graphical element — its own handle (hide + Remove).
		if (pg.isFault && pg.faultPlane) {
			vtkActor *fp = pg.faultPlane.Get();
			makeRow(QString("Surface projection"), IC_Rect, fp->GetVisibility() != 0,
			        [fp](bool on) { fp->SetVisibility(on ? 1 : 0); },
			        [s, fp](const QPoint& g) {
			            QMenu m(s->widget);
			            QAction *aRem = m.addAction("Remove");
			            if (m.exec(g) != aRem) return;
			            if (s->ren) s->ren->RemoveActor(fp);
			            for (auto& p : s->polys) if (p.faultPlane.Get() == fp) {
			                p.faultPlane = nullptr; p.faultPlanePD = nullptr; break;
			            }
			            rebuildSceneObjects(s);
			            if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
			        },
			        "Gray surface projection of the fault plane · left-click for Remove");
		}

		// The buried 3-D dipping fault plane is its OWN graphical element with its OWN handle — NOT the
		// gray surface projection (that is a separate, surface-draped patch). The handle toggles only the
		// 3-D plane (hidden in flat-2D); left-click the label for a Remove menu.
		if (pg.isFault && pg.faultPlane3D) {
			// Capture the STABLE actor pointer (vtkSmartPointer keeps the object alive at a fixed
			// address even if s->polys reallocates); never a raw Polygon* (that dangles on regrow).
			vtkActor *fp3 = pg.faultPlane3D.Get();
			makeRow(QString("Fault plane"), IC_Rect, pg.faultPlane3DShown,
			        [s, fp3](bool on) {
			            for (auto& p : s->polys) if (p.faultPlane3D.Get() == fp3) {
			                p.faultPlane3DShown = on;
			                if (p.faultArrows) p.faultArrows->SetVisibility((on && !s->flat2d) ? 1 : 0);
			                break;
			            }
			            fp3->SetVisibility((on && !s->flat2d) ? 1 : 0); },
			        [s, fp3](const QPoint& g) {
			            QMenu m(s->widget);
			            QAction *aRem = m.addAction("Remove");
			            if (m.exec(g) != aRem) return;
			            if (s->ren) s->ren->RemoveActor(fp3);
			            for (auto& p : s->polys) if (p.faultPlane3D.Get() == fp3) {   // null the owning fault's slot
			                if (p.faultArrows && s->ren) s->ren->RemoveActor(p.faultArrows);
			                p.faultArrows = nullptr; p.faultArrowsPD = nullptr;
			                p.faultPlane3D = nullptr; p.faultPlane3DPD = nullptr; break;
			            }
			            rebuildSceneObjects(s);
			            if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
			        },
			        "Buried 3-D fault plane (visible from below the surface) · left-click for Remove");
		}

		if (faultGroup) endGroup();
	}
	if (!slipGroupOpen.isEmpty()) endGroup();            // close a slip-model group still open at the list's end
	for (auto &tl : s->texts) {                          // user-placed text labels (toggle + right-click menu)
		if (!tl.actor) continue;
		// Batch-owned labels (Focal mechanisms' per-event date, gmtvtk_add_texts_h groupName tag) get
		// NO row of their own — a catalog can carry 100s, which would flood this panel exactly like the
		// meca fill patches would without their own ONE-row-per-batch folding above. They're controlled
		// entirely by the batch's own row (visibility + Plot event date / font in mecaGroupPropsDialog).
		if (!tl.groupName.empty()) continue;
		// GetVisibility/SetVisibility are vtkProp3D base methods — no downcast needed, works the
		// same whether tl.actor is the label's actual concrete type or not.
		vtkProp3D *act = tl.actor.Get();
		makeRow(QString::fromStdString(tl.name), IC_Text, act->GetVisibility() != 0,
		        [act](bool on) { act->SetVisibility(on ? 1 : 0); },
		        [s, act](const QPoint& g) { textLabelMenu(s, act, g); },
		        "Left-click for properties");
	}
	if (s->profLine && s->profLine->GetVisibility()) {  // the profile track (when one exists)
		LineRef lr{ LK_Profile, s->profLine };
		addRow("Profile", s->profLine, IC_Profile, &lr);
	}
	// The Aquamoto CONTROL window's own STANDALONE handle (its own top-level row, NOT nested inside the
	// nc cube's group). It is only ASSOCIATED with the cube by LIFETIME: destroying the cube surface
	// (its Remove) also destroys this window -- see g_aquamotoDestroy in sceneRemoveSurface. The window
	// is otherwise un-killable by its own X (75_aquamoto.cpp); this handle's checkbox shows/hides it.
	if (g_aquamotoHasWindow && g_aquamotoHasWindow(s)) {
		makeRow("Aquamoto viewer", IC_Image,
		        g_aquamotoIsVisible && g_aquamotoIsVisible(s),
		        [s](bool on) { if (g_aquamotoSetVisible) g_aquamotoSetVisible(s, on ? 1 : 0); },
		        [s](const QPoint&) { if (g_aquamotoReopen) g_aquamotoReopen(s); },
		        "Show / hide the Aquamoto control window · left-click to raise it");
	}
}

// `interiorXYZ`/`nInterior`: SHAPENC "bounded ensemble" support (Mirone convention -- an OUTER
// boundary polygon wraps a point swarm that's plotted only on demand). Meaningful only when this
// call is adding the OUT polygon itself: the swarm is stashed on the new Overlay, NOT added to the
// scene, until the user picks "Plot interior points" (popupLineObjectMenu, 55_lineprops.cpp) --
// which just calls this SAME function again with mode=0 to add it as its own ordinary overlay.
static void addOverlay(Scene *s, const double *xyz, int npts, const int *segoff, int nseg,
					   int mode, double r, double g, double b, double linewidth, double pointsize,
					   const char *name = nullptr, const char *groupName = nullptr, const char *info = nullptr,
					   const double *interiorXYZ = nullptr, int nInterior = 0, bool isShapencBoundary = false,
					   bool isShapencInteriorPoints = false, bool noConvertToPoints = false,
					   bool zIsPlaceholder = false, bool noDataTable = false) {
	if (!s || !xyz || npts <= 0)
		return;

	vtkNew<vtkPoints> pts; pts->SetDataTypeToDouble(); pts->Allocate(npts);
	for (int i = 0; i < npts; ++i)
		pts->InsertNextPoint(xyz[3*i], xyz[3*i+1], xyz[3*i+2]);

	vtkNew<vtkCellArray> cells;
	if (mode == 1) {                          // polylines: one cell per segment
		for (int k = 0; k < nseg; ++k) {
			const int a = segoff[k], z = segoff[k+1];
			if (z - a < 2)
				continue;                     // a lone point is not a line
			cells->InsertNextCell(z - a);
			for (int i = a; i < z; ++i)
				cells->InsertCellPoint(i);
		}
	}
	else {                                    // points: one vertex cell per point
		for (int i = 0; i < npts; ++i) {
			cells->InsertNextCell(1);
			cells->InsertCellPoint(i);
		}
	}

	vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
	pd->SetPoints(pts);
	if (mode == 1)
		pd->SetLines(cells);
	else
		pd->SetVerts(cells);

	vtkNew<vtkPolyDataMapper> map;
	map->SetInputData(pd);
	map->ScalarVisibilityOff();               // flat single colour, not the grid CPT
	// Pull the overlay toward the camera so a track laid on the surface is not z-fought.
	vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
	map->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, -8000.0);
	map->SetRelativeCoincidentTopologyPointOffsetParameter(-8000.0);

	vtkSmartPointer<vtkActor> a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(map);
	a->GetProperty()->LightingOff();          // overlays read as a flat HUD colour
	a->GetProperty()->SetColor(r, g, b);
	// Caller-unspecified width (<=0, e.g. every dropped/imported vector: drop.jl, grid.jl add!,
	// geography.jl, aquamoto.jl, focal.jl) falls back to Preferences "Default line thickness" --
	// the SAME preference interactively-drawn polygons already honour (85_polygon.cpp) -- not a
	// second hardcoded default (SACRED_LAW.md: fix the shared source once, not each call site).
	a->GetProperty()->SetLineWidth(linewidth > 0.0 ? linewidth : prefLineWidthPx(s));
	a->GetProperty()->SetPointSize(pointsize > 0.0 ? pointsize : 6.0);
	if (mode == 0)
		a->GetProperty()->SetRenderPointsAsSpheres(true);   // round points (toggle in the menu)
	a->SetScale(s->xfac, 1.0, s->zfac * s->ve);             // register with the surface

	s->ren->AddActor(a);
	Overlay ov{ a, mode };
	ov.baseLine = pd;                         // keep the geometry (both modes) for restyling + line<->points toggle
	ov.segoff.assign(segoff, segoff + nseg + 1);   // remember segments so a Points overlay can rebuild polylines
	ov.nseg = nseg;
	// Source-identity naming: if the caller knows where this came from (a GMT feature, a file, a
	// dataset) it passes that name and we use it verbatim; otherwise fall back to "Line N"/"Points N".
	ov.name = (name && name[0]) ? std::string(name)
	                            : (mode == 1 ? "Line " : "Points ") + std::to_string((int)s->overlays.size() + 1);
	if (groupName && groupName[0])
		ov.groupName = groupName;
	if (info && info[0] && mode == 1) {        // per-segment hover text: nseg records joined by RS ('\x1e')
		std::vector<std::string> recs;
		const char *p = info;
		while (true) {
			const char *e = strchr(p, '\x1e');
			recs.emplace_back(e ? std::string(p, e - p) : std::string(p));
			if (!e) break;
			p = e + 1;
		}
		if ((int)recs.size() == nseg)          // adopt only if it aligns 1:1 with the segments, else drop it
			ov.info = std::move(recs);
	}
	if (interiorXYZ && nInterior > 0)
		ov.interiorXYZ.assign(interiorXYZ, interiorXYZ + 3 * nInterior);
	ov.isShapencBoundary = isShapencBoundary;
	ov.isShapencInteriorPoints = isShapencInteriorPoints;
	ov.noConvertToPoints = noConvertToPoints;
	ov.zIsPlaceholder = zIsPlaceholder;
	ov.noDataTable = noDataTable;
	ov.stack = s->vecSeq++;                    // new overlay lands on top of the shared vector pile
	s->overlays.push_back(ov);
	applyVectorStacking(s);                   // normalize ranks + set this overlay's draw-order offset
	rebuildSceneObjects(s);                   // refresh the Scene Objects checkbox list
	// Every other actor-adding path (surfaces, images, gizmo, curtains, profiles) resets the
	// clipping range after adding its actor -- this one didn't. Harmless normally (a real grid's
	// own bounds already give a comfortably wide clip range), but a promoted EMPTY-launcher scaffold
	// (_promote_dataset, drop.jl) is a perfectly flat z=0 placeholder, so ResetCamera() at promote
	// time sets a razor-thin clip slab around z=0 -- any overlay added afterward with real (nonzero)
	// relief, e.g. a SHAPENC point cloud's actual depth/elevation column, falls outside near/far and
	// is silently culled: no error, just an empty-looking window. Proven live (CARTA604_IH.nc, a
	// 17245-pt bathymetry cloud: invisible with real ~-30 m depths, visible with z~0).
	if (s->ren) s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Toggle an existing overlay between polyline (mode 1) and points (mode 0) IN PLACE: rebuild the
// polydata cells from the retained points + segment offsets, no re-add. `toPoints` != 0 -> points.
// The "Convert to points / Convert to line" menu action (55_lineprops.cpp) drives this.
static void overlaySetMode(Scene *s, vtkActor *actor, int toPoints) {
	if (!s || !actor) return;
	Overlay *ov = nullptr;
	for (auto& o : s->overlays) if (o.actor.Get() == actor) { ov = &o; break; }
	if (!ov || !ov->baseLine) return;
	const int mode = toPoints ? 0 : 1;
	if (ov->mode == mode) return;
	vtkPolyData *pd = ov->baseLine;
	const int npts = (int)pd->GetNumberOfPoints();
	vtkNew<vtkCellArray> cells;
	if (mode == 1) {                          // points -> polylines (one cell per stored segment)
		for (int k = 0; k < ov->nseg; ++k) {
			const int a = ov->segoff[k], z = ov->segoff[k+1];
			if (z - a < 2) continue;          // a lone point is not a line
			cells->InsertNextCell(z - a);
			for (int i = a; i < z; ++i) cells->InsertCellPoint(i);
		}
		pd->SetVerts(nullptr);
		pd->SetLines(cells);
	}
	else {                                    // polylines -> points (one vertex cell per point)
		for (int i = 0; i < npts; ++i) { cells->InsertNextCell(1); cells->InsertCellPoint(i); }
		pd->SetLines(nullptr);
		pd->SetVerts(cells);
	}
	pd->Modified();
	ov->mode = mode;
	ov->actor->GetProperty()->SetRenderPointsAsSpheres(mode == 0);   // round points, like a fresh Points overlay
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Query an overlay's current mode (1 = line, 0 = points; -1 if the actor is not an overlay). Lets
// the context menu label the toggle "Convert to points" vs "Convert to line".
static int overlayMode(Scene *s, vtkActor *actor) {
	if (s && actor)
		for (auto& o : s->overlays) if (o.actor.Get() == actor) return o.mode;
	return -1;
}

// Detach ONE stored segment [segoff[segIdx],segoff[segIdx+1]) from a multi-segment overlay,
// rebuilding its points + cells from the remaining segments. Used when a segment is promoted into
// an editable Polygon (85_polygon.cpp overlayPromoteSegmentToPolygon) — the rest of the overlay
// (e.g. a coastline's other islands) stays exactly as it was.
static void overlayRemoveSegment(Scene *s, Overlay& ov, int segIdx) {
	if (!s || segIdx < 0 || segIdx >= ov.nseg || !ov.baseLine) return;
	vtkPolyData *pd = ov.baseLine;
	vtkPoints *oldPts = pd->GetPoints();
	if (!oldPts) return;

	vtkNew<vtkPoints> newPts; newPts->SetDataTypeToDouble();
	std::vector<int> segoffNew; segoffNew.push_back(0);
	for (int k = 0; k < ov.nseg; ++k) {
		if (k == segIdx) continue;
		const int ka = ov.segoff[k], kz = ov.segoff[k+1];
		for (int i = ka; i < kz; ++i) {
			double p[3]; oldPts->GetPoint(i, p);
			newPts->InsertNextPoint(p);
		}
		segoffNew.push_back((int)newPts->GetNumberOfPoints());
	}
	ov.segoff = segoffNew;
	ov.nseg -= 1;

	vtkNew<vtkCellArray> cells;
	if (ov.mode == 1) {
		for (int k = 0; k < ov.nseg; ++k) {
			const int ka = ov.segoff[k], kz = ov.segoff[k+1];
			if (kz - ka < 2) continue;          // a lone point is not a line
			cells->InsertNextCell(kz - ka);
			for (int i = ka; i < kz; ++i) cells->InsertCellPoint(i);
		}
		pd->SetLines(cells);
	}
	else {
		for (int i = 0; i < (int)newPts->GetNumberOfPoints(); ++i) { cells->InsertNextCell(1); cells->InsertCellPoint(i); }
		pd->SetVerts(cells);
	}
	pd->SetPoints(newPts);
	pd->Modified();
}

// ---- generic screen-constant SYMBOL layers (volcanoes, seismicity, cities, …) ----------------
// Build a UNIT glyph (centred at origin, fits a radius-0.5 circle -> diameter 1) for a GMT symbol
// code. Filled shapes -> a single polygon cell (fill = actor colour, outline = actor EdgeVisibility);
// open shapes (x + -) -> line cells (no fill, drawn in the edge colour). `filled` is returned so the
// caller knows whether the fill colour applies. Unknown code -> circle.
static vtkSmartPointer<vtkPolyData> makeSymbolGlyph(const std::string& sym, bool& filled) {
	vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
	vtkNew<vtkPoints> p;
	vtkNew<vtkCellArray> ca;
	const double R = 0.5, D2R = vtkMath::Pi() / 180.0;
	// closed regular n-gon, first vertex at `startDeg`, on radius R -> one filled polygon
	auto poly = [&](int n, double startDeg) {
		filled = true;
		ca->InsertNextCell(n);
		for (int i = 0; i < n; ++i) {
			const double a = (startDeg + i * 360.0 / n) * D2R;
			ca->InsertCellPoint(p->InsertNextPoint(R * std::cos(a), R * std::sin(a), 0.0));
		}
		pd->SetPoints(p); pd->SetPolys(ca);
	};
	if      (sym == "c") poly(32, 0.0);    // circle
	else if (sym == "s") poly(4, 45.0);    // square
	else if (sym == "t") poly(3, 90.0);    // triangle (apex up)
	else if (sym == "i") poly(3, -90.0);   // inverted triangle (apex down)
	else if (sym == "d") poly(4, 90.0);    // diamond
	else if (sym == "h") poly(6, 0.0);     // hexagon
	else if (sym == "n") poly(5, 90.0);    // pentagon
	else if (sym == "g") poly(8, 22.5);    // octagon
	else if (sym == "a") {                 // 5-point star (concave -> hand-triangulated + outline)
		// The OpenGL2 mapper fan-triangulates a polygon cell from its vertex 0, which fills a CONCAVE
		// star wrong (the notches get covered -> an arrow blob). So build the FILL as an explicit fan
		// of triangles from the centre, and carry the 10-segment boundary as a separate closed
		// polyline. addSymbols paints the triangles with the fill colour and the polyline with the
		// edge colour (per-cell direct colours), so there are no internal "spoke" edges.
		filled = true;
		const double ri = 0.20;
		const vtkIdType ctr = p->InsertNextPoint(0.0, 0.0, 0.0);
		vtkIdType v[10];
		for (int i = 0; i < 10; ++i) {
			const double a = (90.0 + i * 36.0) * D2R, rr = (i % 2 == 0) ? R : ri;
			v[i] = p->InsertNextPoint(rr * std::cos(a), rr * std::sin(a), 0.0);
		}
		for (int i = 0; i < 10; ++i) {            // fill: centre-fan triangles
			ca->InsertNextCell(3);
			ca->InsertCellPoint(ctr); ca->InsertCellPoint(v[i]); ca->InsertCellPoint(v[(i + 1) % 10]);
		}
		vtkNew<vtkCellArray> lines;               // outline: closed boundary polyline (no spokes)
		lines->InsertNextCell(11);
		for (int i = 0; i < 10; ++i) lines->InsertCellPoint(v[i]);
		lines->InsertCellPoint(v[0]);
		pd->SetPoints(p); pd->SetPolys(ca); pd->SetLines(lines);
	}
	else if (sym == "x" || sym == "+" || sym == "-") {       // open line glyphs
		filled = false;
		auto seg = [&](double x0, double y0, double x1, double y1) {
			const vtkIdType i0 = p->InsertNextPoint(x0, y0, 0.0);
			const vtkIdType i1 = p->InsertNextPoint(x1, y1, 0.0);
			ca->InsertNextCell(2); ca->InsertCellPoint(i0); ca->InsertCellPoint(i1);
		};
		if      (sym == "x") { seg(-R, -R, R, R); seg(-R, R, R, -R); }   // diagonal cross
		else if (sym == "+") { seg(-R, 0, R, 0);  seg(0, -R, 0, R);  }   // plus
		else                 { seg(-R, 0, R, 0); }                       // dash
		pd->SetPoints(p); pd->SetLines(ca);
	}
	else poly(32, 0.0);                    // unknown -> circle
	return pd;
}

// Unit-diameter (radius 0.5) SOLID 3-D glyph for "o" (sphere) / "u" (cube) — a true volume, not a
// flat XY polygon, so it stays visible from any camera angle (the flat glyphs above go edge-on
// invisible in an oblique 3-D view). Always filled; normals included so LightingOn() actually
// shades it (addSymbols turns lighting on only for these two codes).
static vtkSmartPointer<vtkPolyData> makeSolidGlyph(const std::string& sym) {
	if (sym == "u") {                      // cube: vtkCubeSource has no built-in normals -> compute them
		vtkNew<vtkCubeSource> cube;
		cube->SetXLength(1.0); cube->SetYLength(1.0); cube->SetZLength(1.0);
		vtkNew<vtkPolyDataNormals> norms;
		norms->SetInputConnection(cube->GetOutputPort());
		norms->ComputePointNormalsOn(); norms->ComputeCellNormalsOff(); norms->SplittingOn();
		norms->Update();
		return norms->GetOutput();
	}
	vtkNew<vtkSphereSource> sph;           // sphere: normals are generated automatically
	sph->SetRadius(0.5); sph->SetThetaResolution(16); sph->SetPhiResolution(16);
	sph->Update();
	return sph->GetOutput();
}

// Per-frame (renderer StartEvent): rescale every symbol layer's glyph so its on-screen size stays
// `sizePx` pixels at any zoom. `vph` = world height spanning the full viewport at the focal plane —
// the SAME camera math the gizmo uses; worldPerPx = vph / viewportHeightPx. The unit glyph is Ø1, so
// ScaleFactor = sizePx * worldPerPx. ClientData is the Scene*. (Also callable directly to prime it.)
static void symbolRescaleCB(vtkObject*, unsigned long, void *clientData, void*) {
	Scene *s = static_cast<Scene*>(clientData);
	if (!s || !s->ren || s->symbols.empty()) return;
	vtkCamera *cam = s->ren->GetActiveCamera();
	if (!cam) return;
	const int *sz = s->ren->GetSize();
	const double Hpx = (sz && sz[1] > 0) ? (double)sz[1] : 600.0;   // DEVICE px (matches WorldToDisplay)
	double vph;
	if (cam->GetParallelProjection())
		vph = 2.0 * cam->GetParallelScale();
	else
		vph = 2.0 * cam->GetDistance() * std::tan(cam->GetViewAngle() * 0.5 * vtkMath::Pi() / 180.0);
	// sizePx is LOGICAL pixels (what a user means); 1 logical px = dpr device px, so multiply by dpr.
	const double dpr = (s->widget) ? s->widget->devicePixelRatioF() : 1.0;
	const double worldPerLogPx = (vph / Hpx) * dpr;
	for (auto& sl : s->symbols) {
		const double scale = std::max(1e-9, sl.sizePx * worldPerLogPx);
		if (sl.glyphMapper) {                  // solid3D: GPU-instanced path (vtkGlyph3DMapper)
			sl.glyphMapper->SetScaleFactor(scale);
			sl.glyphMapper->Modified();
		}
		else if (sl.glyph) {                 // flat glyphs: CPU-duplicated path (vtkGlyph3D)
			sl.glyph->SetScaleFactor(scale);
			sl.glyph->Modified();
		}
		else continue;
		if (sl.solid3D && sl.zfix) {           // cancel the actor's (1,1,zfac*ve) Z-squash, see SymbolLayer
			const double zc = s->zfac * s->ve;
			const double zInv = (std::fabs(zc) > 1e-12) ? (1.0 / zc) : 0.0;   // ve==0 (flat) -> degenerate, harmless
			sl.zfix->Identity();
			sl.zfix->Scale(1.0, 1.0, zInv);
		}
	}
}

// ---- vector DRAW-ORDER stacking ----------------------------------------------------------------
// Vectors (line overlays, symbol layers, polygons / fault traces) now share the SAME unified pile
// as the base relief and grids (see applyStacking above) — "stacking" decides WHO DRAWS ON TOP
// where elements overlap, including a vector versus the grid it sits on. These thin wrappers keep
// the historical call sites working; they funnel into the one pile. polyHandles ride a much-more-
// negative constant offset (85_polygon.cpp) so edit handles stay visible above any pile rank.
static void applyVectorStacking(Scene *s) { applyStacking(s); }
static void restackVector(Scene *s, int *stackPtr, int op) { restackStack(s, stackPtr, op); }

// Stamp N glyphs of one GMT symbol code at N (x,y,z) points (TRUE coords). Screen-constant size:
// x is pre-baked with xfac so the glyph is NOT x-stretched; the actor carries only the z scale so
// symbols ride VE. The per-frame observer (installed once) keeps `sizePx` literal at any zoom.
static int addSymbols(Scene *s, const double *xyz, int npts, const std::string& sym,
                      double sizePx, int filled,
                      double fr, double fg, double fb,
                      double er, double eg, double eb, double edgeWidth,
                      const std::string& name, const char *info = nullptr, bool oneShot = false) {
	if (!s || !xyz || npts <= 0) return 0;

	vtkNew<vtkPoints> pts; pts->SetDataTypeToDouble(); pts->Allocate(npts);
	for (int i = 0; i < npts; ++i)
		pts->InsertNextPoint(xyz[3*i] * s->xfac, xyz[3*i+1], xyz[3*i+2]);   // x baked, z raw
	vtkSmartPointer<vtkPolyData> in = vtkSmartPointer<vtkPolyData>::New();
	in->SetPoints(pts);

	const bool solid3D = (sym == "o" || sym == "u");      // sphere / cube: true volume, not flat-XY
	bool glyphFilled = true;
	vtkSmartPointer<vtkPolyData> src = solid3D ? makeSolidGlyph(sym) : makeSymbolGlyph(sym, glyphFilled);
	const bool wantFill = glyphFilled && (filled != 0);
	// A glyph that carries BOTH filled polys and outline lines (the concave star) is coloured PER
	// CELL — fill triangles in the fill colour, the boundary polyline in the edge colour — so the
	// outline is the star boundary only (no internal triangulation spokes that EdgeVisibility draws).
	const bool cellColoured = (src->GetNumberOfLines() > 0 && src->GetNumberOfPolys() > 0);
	if (cellColoured) {
		auto to255 = [](double c) { return (unsigned char)(c < 0 ? 0 : c > 1 ? 255 : c * 255.0 + 0.5); };
		unsigned char cf[3] = { to255(fr), to255(fg), to255(fb) };   // fill (triangles)
		unsigned char ce[3] = { to255(er), to255(eg), to255(eb) };   // edge (boundary line)
		const vtkIdType nL = src->GetNumberOfLines(), nP = src->GetNumberOfPolys();
		vtkNew<vtkUnsignedCharArray> cc; cc->SetNumberOfComponents(3); cc->SetNumberOfTuples(nL + nP);
		vtkIdType t = 0;                              // vtkPolyData cell order is Verts, Lines, Polys
		for (vtkIdType i = 0; i < nL; ++i, ++t) cc->SetTypedTuple(t, wantFill ? ce : cf);
		for (vtkIdType i = 0; i < nP; ++i, ++t) cc->SetTypedTuple(t, cf);
		src->GetCellData()->SetScalars(cc);
	}

	// Solid 3-D glyphs ride the actor's (1,1,zfac*ve) Z scale for POSITION like every other symbol,
	// but that would also squash their own volume flat (zfac converts metres to a tiny degree-
	// equivalent unit). `zfix` pre-divides the unit source's Z by that same factor so it cancels
	// out after the actor re-applies it — kept live (not baked once) because ve changes at runtime;
	// symbolRescaleCB updates it every frame alongside the screen-size ScaleFactor.
	vtkSmartPointer<vtkTransform> zfix;
	vtkSmartPointer<vtkTransformPolyDataFilter> zfixFilter;
	if (solid3D) {
		zfix = vtkSmartPointer<vtkTransform>::New();
		zfixFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
		zfixFilter->SetInputData(src);
		zfixFilter->SetTransform(zfix);
	}

	// solid3D (sphere/cube) uses vtkGlyph3DMapper — GPU instancing, the source mesh is uploaded ONCE
	// and drawn N times, never duplicated into one combined CPU polydata. vtkGlyph3D would tessellate
	// and copy the full sphere mesh (e.g. 16x16 res, ~480 tris) into ONE polydata PER point — for a
	// 30k-event seismicity catalog that's ~15M triangles built on the CPU every time the source is
	// touched, which is exactly what made "Global seismicity" slow to plot and to manipulate (point
	// clouds never hit this path — they render as bare vtkPoints, no glyphing at all).
	vtkSmartPointer<vtkGlyph3D>       g;
	vtkSmartPointer<vtkGlyph3DMapper> gm;
	vtkSmartPointer<vtkMapper> map3D;
	if (solid3D) {
		vtkNew<vtkGlyph3DMapper> m;
		m->SetInputData(in);
		m->SetSourceConnection(zfixFilter->GetOutputPort());
		m->SetScaleModeToNoDataScaling();     // uniform size; ScaleFactor driven by the observer
		m->OrientOff();
		m->SetScaleFactor(sizePx > 0.0 ? sizePx : 8.0);   // placeholder; primed below + per frame
		m->ScalarVisibilityOff();
		gm = m;
		map3D = m;
	}
	else {
		vtkSmartPointer<vtkGlyph3D> gg = vtkSmartPointer<vtkGlyph3D>::New();
		gg->SetSourceData(src);
		gg->SetInputData(in);
		gg->SetScaleModeToDataScalingOff();    // uniform size; ScaleFactor driven by the observer
		gg->OrientOff();
		gg->SetScaleFactor(sizePx > 0.0 ? sizePx : 8.0);   // placeholder; primed below + per frame
		g = gg;

		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(g->GetOutputPort());
		if (cellColoured) { map->ScalarVisibilityOn(); map->SetScalarModeToUseCellData(); map->SetColorModeToDirectScalars(); }
		else              { map->ScalarVisibilityOff(); }
		// The huge negative bias below forces a flat glyph to WIN the depth test against the coincident
		// surface it sits directly on (z=0, e.g. a volcano marker) — it has no real depth of its own.
		// A solid3D glyph (sphere/cube) DOES carry real depth (e.g. a buried earthquake hypocentre) and
		// must lose the depth test to genuinely shallower surface geometry above it (handled in the
		// solid3D branch above, which leaves its mapper at the default unbiased depth resolution).
		vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();   // lift off the z=0 map plane
		map->SetRelativeCoincidentTopologyPolygonOffsetParameters(0.0, -8000.0);
		map->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, -8000.0);
		map->SetRelativeCoincidentTopologyPointOffsetParameter(-8000.0);
		map3D = map;
	}

	vtkSmartPointer<vtkActor> a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(map3D);
	// Flat glyphs are unlit (constant colour, like the gizmo). A sphere/cube needs real shading to
	// read as a volume rather than a flat disc — that's the whole point of offering them — so turn
	// lighting ON just for those two; a sphere's fine facet mesh also looks messy with edges drawn,
	// so skip EdgeVisibility for it (a cube keeps its box outline, which reads fine).
	a->GetProperty()->SetLighting(solid3D);
	if (wantFill) {
		a->GetProperty()->SetColor(fr, fg, fb);
		a->GetProperty()->SetEdgeColor(er, eg, eb);
		// Cell-coloured glyphs (star) draw their outline as a coloured boundary line, NOT actor edges
		// (which would expose the fill triangulation as spokes); edgeWidth sets that line's width.
		a->GetProperty()->SetEdgeVisibility((edgeWidth > 0.0 && !cellColoured && sym != "o") ? 1 : 0);
		a->GetProperty()->SetLineWidth(edgeWidth > 0.0 ? edgeWidth : 1.0);
	} else {                               // open glyph: drawn in the edge colour, no fill
		a->GetProperty()->SetColor(er, eg, eb);
		a->GetProperty()->SetLineWidth(edgeWidth > 0.0 ? edgeWidth : 1.5);
	}
	a->SetScale(1.0, 1.0, s->zfac * s->ve);            // ride VE; x already baked

	s->ren->AddActor(a);
	SymbolLayer sl;
	sl.actor = a; sl.glyph = g; sl.glyphMapper = gm; sl.zfix = zfix; sl.zfixFilter = zfixFilter;
	sl.sizePx = (sizePx > 0.0 ? sizePx : 8.0);
	sl.filled = wantFill; sl.sym = sym; sl.solid3D = solid3D;
	sl.oneShot = oneShot;                // Symbols draw tool: exactly one point, whole-layer drag applies
	sl.stack = s->vecSeq++;              // new layer lands on top of the whole vector pile
	sl.name = name.empty() ? ("Symbols " + std::to_string((int)s->symbols.size() + 1) + " (" + sym + ")")
	                       : name;
	// Per-point hover info (optional): one record per point, records joined by RS ('\x1e'), each
	// record a ready-to-show multi-line block. Only adopt it when it aligns 1:1 with the points.
	if (info && info[0]) {
		std::vector<std::string> recs;
		const std::string packed(info);
		size_t a = 0;
		while (a <= packed.size()) {
			size_t b = packed.find('\x1e', a);
			if (b == std::string::npos) { recs.push_back(packed.substr(a)); break; }
			recs.push_back(packed.substr(a, b - a));
			a = b + 1;
		}
		if ((int)recs.size() == npts)
			sl.info = std::move(recs);
	}
	s->symbols.push_back(sl);
	applyVectorStacking(s);                // normalize ranks + set this layer's draw-order offset

	if (!s->symSizeCmd) {                  // install the per-frame rescaler once per scene
		vtkSmartPointer<vtkCallbackCommand> cmd = vtkSmartPointer<vtkCallbackCommand>::New();
		cmd->SetCallback(symbolRescaleCB);
		cmd->SetClientData(s);
		s->ren->AddObserver(vtkCommand::StartEvent, cmd);
		s->symSizeCmd = cmd;
	}
	symbolRescaleCB(nullptr, 0, s, nullptr);           // prime size for the first frame

	rebuildSceneObjects(s);
	// A stale clip range (computed before this actor existed) can cut it out of the rendered image
	// even though it's on-screen — sceneSetFlat2D always resets this on a 2D<->3D transition, which
	// is why a round-trip toggle "fixes" newly-added symbols that would otherwise clip; do it here
	// too so a fresh layer never depends on a later, unrelated mode toggle to become visible.
	if (s->ren) s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
	return 1;
}

// Index of the symbol layer whose actor==a, or -1 (mirrors polyIndexOfActor's "never cache" style —
// callers re-find by pointer since s->symbols can shift under an unrelated erase elsewhere).
static int symbolLayerIndexOfActor(Scene *s, vtkActor *a) {
	for (int i = 0; i < (int)s->symbols.size(); ++i) if (s->symbols[i].actor.Get() == a) return i;
	return -1;
}

// Floating data viewer for a symbol layer: a non-modal window with a table of its point(s) in TRUE
// coords (X un-baked out of xfac). Mirrors showLineDataTable's look/role (55_lineprops.cpp) — same
// #/X/Y[/Z] columns, same floating/non-modal/WA_DeleteOnClose window. Editable for every layer (a
// symbol has no "drawn-tool-only" restriction the way LK_Polygon editing does): a committed cell
// rewrites the point directly in the glyph's input vtkPoints and re-renders. Re-finds the layer by
// actor pointer on every edit (never caches the index — `act` is the stable identity, matching
// polyIndexOfActor's convention) so a layer deleted while the table is open just no-ops instead of
// writing into freed/reused memory.
static void showSymbolDataTable(Scene *s, vtkActor *act, const QString& name) {
	const int si0 = symbolLayerIndexOfActor(s, act);
	if (si0 < 0) return;
	vtkPolyData *pd0 = symInputPD(s->symbols[si0]);
	if (!pd0 || !pd0->GetPoints()) return;
	const int nrows = (int)pd0->GetPoints()->GetNumberOfPoints();
	if (nrows == 0) return;
	const double xfacInv = (s->xfac != 0.0) ? 1.0 / s->xfac : 1.0;

	QDialog *dlg = new QDialog(nullptr);                  // top-level, parentless -> truly floating
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle(name.isEmpty() ? QString("Symbol data") : (name + " — data"));
	dlg->setWindowFlag(Qt::Window, true);
	QVBoxLayout *lay = new QVBoxLayout(dlg);

	const int ncoord = s->flat2d ? 2 : 3;
	QStringList hdr; hdr << "#" << "X" << "Y"; if (!s->flat2d) hdr << "Z";
	QTableWidget *tbl = new QTableWidget(nrows, ncoord + 1, dlg);
	tbl->setHorizontalHeaderLabels(hdr);
	tbl->verticalHeader()->setVisible(false);
	tbl->setSelectionBehavior(QAbstractItemView::SelectRows);

	for (int k = 0; k < nrows; ++k) {
		double p[3]; pd0->GetPoints()->GetPoint(k, p);
		const double row[3] = { p[0] * xfacInv, p[1], p[2] };
		QTableWidgetItem *idx = new QTableWidgetItem(QString::number(k + 1));
		idx->setFlags(idx->flags() & ~Qt::ItemIsEditable);   // the "#" column is never editable
		tbl->setItem(k, 0, idx);
		for (int c = 0; c < ncoord; ++c)
			tbl->setItem(k, c + 1, new QTableWidgetItem(QString::number(row[c], 'g', 10)));
	}
	tbl->resizeColumnsToContents();
	lay->addWidget(tbl);
	dlg->resize(360, 420);

	std::shared_ptr<bool> guard = std::make_shared<bool>(false);
	QObject::connect(tbl, &QTableWidget::cellChanged, dlg, [s, tbl, act, guard](int row, int col) {
		if (*guard || col < 1 || col > 3) return;             // ignore the "#" column / our own edits
		const int si = symbolLayerIndexOfActor(s, act);
		if (si < 0) return;                                   // layer was deleted -> nothing to write
		vtkPolyData *pd = symInputPD(s->symbols[si]);
		if (!pd || !pd->GetPoints() || row < 0 || row >= (int)pd->GetPoints()->GetNumberOfPoints()) return;
		bool ok = false;
		const double val = tbl->item(row, col)->text().toDouble(&ok);
		double p[3]; pd->GetPoints()->GetPoint(row, p);
		*guard = true;
		if (!ok) {                                            // bad number -> restore the old cell text
			const double old = (col == 1) ? p[0] * (s->xfac != 0.0 ? 1.0 / s->xfac : 1.0) : p[col - 1];
			tbl->item(row, col)->setText(QString::number(old, 'g', 10));
			*guard = false;
			return;
		}
		if (col == 1) p[0] = val * s->xfac;                   // X: re-bake xfac, matches addSymbols
		else          p[col - 1] = val;                       // Y / Z
		pd->GetPoints()->SetPoint(row, p);
		pd->GetPoints()->Modified();
		pd->Modified();
		*guard = false;
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	});
	dlg->show();                                          // non-modal: REPL + viewer stay live
}

// Font/colour/visibility properties for a grouped (billboard) text label, right-clicked directly.
// `clickedIdx` is the SPECIFIC label under the cursor (index into s->texts) — its CURRENT values
// seed the dialog, and a scope choice ("This label only" / "All labels in this group") decides
// whether a change lands on just that one label or every label sharing its `groupName` (e.g. every
// Cities star name). Same live-edit idea as mecaGroupPropsDialog's date-font controls (font
// family/size/colour/bold/italic), but standalone: no compression/dilatation/rim colours
// (meca-specific), no g_juliaMecaProps replot round-trip — these labels are already plotted, so
// every control just re-`textApplyProps`-s the target label(s) directly and re-renders.
static void batchTextLabelsDialog(Scene *s, const std::string &groupName, int clickedIdx, const QPoint &gp) {
	std::vector<TextLabel*> labels;
	for (auto &tl : s->texts) if (tl.groupName == groupName) labels.push_back(&tl);
	if (labels.empty()) return;
	const bool hasClicked = clickedIdx >= 0 && clickedIdx < (int)s->texts.size()
	                      && s->texts[clickedIdx].groupName == groupName;
	TextLabel &first = hasClicked ? s->texts[clickedIdx] : *labels.front();

	QDialog *dlg = new QDialog(s->widget);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle(QString::fromStdString(groupName) + " — text label properties");
	QVBoxLayout *lay = new QVBoxLayout(dlg);

	// Scope choice only makes sense when there's more than one label in the group AND we actually
	// know which specific one was clicked (a right-click straight on the star, rather than through
	// some other path with no particular label in mind, has no "this one" to offer).
	const bool offerScope = hasClicked && labels.size() > 1;
	QRadioButton *scopeOne = offerScope ? new QRadioButton("This label only", dlg) : nullptr;
	QRadioButton *scopeAll = offerScope ? new QRadioButton("All labels in this group", dlg) : nullptr;
	if (offerScope) {
		scopeOne->setChecked(true);
		lay->addWidget(scopeOne);
		lay->addWidget(scopeAll);
	}

	QCheckBox *visChk = new QCheckBox("Show name", dlg);
	visChk->setChecked(first.actor && first.actor->GetVisibility() != 0);
	lay->addWidget(visChk);

	QWidget *fontRow = new QWidget(dlg);
	QHBoxLayout *hf = new QHBoxLayout(fontRow);
	hf->setContentsMargins(0, 0, 0, 0);
	hf->addWidget(new QLabel("Font:", fontRow));
	QComboBox *fontCombo = new QComboBox(fontRow);
	fontCombo->addItems({ "Arial", "Courier", "Times" });   // textApplyProps' supported font families
	fontCombo->setCurrentText(QString::fromStdString(first.font));
	hf->addWidget(fontCombo);
	QSpinBox *sizeSpin = new QSpinBox(fontRow);
	sizeSpin->setRange(4, 300); sizeSpin->setValue(first.size);
	hf->addWidget(sizeSpin);
	QCheckBox *boldChk = new QCheckBox("Bold", fontRow);     boldChk->setChecked(first.bold);
	QCheckBox *italChk = new QCheckBox("Italic", fontRow);   italChk->setChecked(first.italic);
	hf->addWidget(boldChk);
	hf->addWidget(italChk);
	QPushButton *colorBtn = new QPushButton(fontRow);
	colorBtn->setFixedWidth(60);
	auto paintColor = [colorBtn](double r, double g, double b) {
		colorBtn->setProperty("r", r); colorBtn->setProperty("g", g); colorBtn->setProperty("b", b);
		colorBtn->setStyleSheet(QString("background-color: %1;").arg(QColor::fromRgbF(r, g, b).name()));
	};
	paintColor(first.color[0], first.color[1], first.color[2]);
	auto commit = std::make_shared<std::function<void()>>();
	QObject::connect(colorBtn, &QPushButton::clicked, [colorBtn, paintColor, commit, dlg]() {
		QColor init = QColor::fromRgbF(colorBtn->property("r").toDouble(), colorBtn->property("g").toDouble(), colorBtn->property("b").toDouble());
		QColor picked = QColorDialog::getColor(init, dlg, "Choose text colour");
		if (picked.isValid()) { paintColor(picked.redF(), picked.greenF(), picked.blueF()); if (*commit) (*commit)(); }
	});
	hf->addWidget(colorBtn);
	lay->addWidget(fontRow);

	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
	QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::close);
	lay->addWidget(bb);

	*commit = [=]() {
		const bool vis = visChk->isChecked();
		const std::string font = fontCombo->currentText().toStdString();
		const int size = sizeSpin->value();
		const bool bold = boldChk->isChecked(), italic = italChk->isChecked();
		double c[3] = { colorBtn->property("r").toDouble(), colorBtn->property("g").toDouble(), colorBtn->property("b").toDouble() };
		auto apply = [&](TextLabel &tl) {
			tl.font = font; tl.size = size; tl.bold = bold; tl.italic = italic;
			tl.color[0] = c[0]; tl.color[1] = c[1]; tl.color[2] = c[2];
			textApplyProps(s, tl);
			if (tl.actor) tl.actor->SetVisibility(vis ? 1 : 0);
		};
		if (offerScope && scopeOne->isChecked()) {
			if (clickedIdx >= 0 && clickedIdx < (int)s->texts.size()) apply(s->texts[clickedIdx]);
		} else {
			for (auto &tl : s->texts) if (tl.groupName == groupName) apply(tl);
		}
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	};
	if (offerScope) {
		QObject::connect(scopeOne, &QRadioButton::toggled, dlg, [commit](bool){ (*commit)(); });
		QObject::connect(scopeAll, &QRadioButton::toggled, dlg, [commit](bool){ (*commit)(); });
	}
	QObject::connect(visChk,    &QCheckBox::toggled,          dlg, [commit](bool){ (*commit)(); });
	QObject::connect(fontCombo, &QComboBox::currentTextChanged, dlg, [commit](const QString&){ (*commit)(); });
	QObject::connect(sizeSpin,  QOverload<int>::of(&QSpinBox::valueChanged), dlg, [commit](int){ (*commit)(); });
	QObject::connect(boldChk,   &QCheckBox::toggled,          dlg, [commit](bool){ (*commit)(); });
	QObject::connect(italChk,   &QCheckBox::toggled,          dlg, [commit](bool){ (*commit)(); });
	dlg->move(gp);
	dlg->show();
}

// Right-click / left-click properties for a symbol layer row: change shape, fill + edge colour,
// on-screen size and edge width, or delete the layer. Edits the SymbolLayer + its actor/glyph in
// place (no re-upload of points); size goes through the per-frame rescaler so it stays literal px.
static void symbolLayerMenu(Scene *s, vtkActor *act, const QPoint& gp) {
	SymbolLayer *sl = nullptr;
	for (auto& x : s->symbols) if (x.actor.Get() == act) { sl = &x; break; }
	if (!sl) return;
	auto reRender = [&] { if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render(); };

	// Keep a cell-coloured glyph (a star: filled polys + outline lines) in sync with the layer's
	// current fill (actor Color) and edge (actor EdgeColor) colours, by repainting its per-cell
	// direct scalars. For a plain source (no outline lines) it restores ordinary actor colouring +
	// EdgeVisibility. Call after any shape / fill / edge change so those menu edits keep working.
	auto applyCellColours = [&] {
		if (!sl->glyph) return;                    // solid3D: plain lit actor colour, no per-cell scalars
		vtkPolyData *src = vtkPolyData::SafeDownCast(sl->glyph->GetSource());
		vtkPolyDataMapper *mp = vtkPolyDataMapper::SafeDownCast(sl->actor->GetMapper());
		if (!src || !mp) return;
		const vtkIdType nL = src->GetNumberOfLines(), nP = src->GetNumberOfPolys();
		if (nL > 0 && nP > 0) {                       // cell-coloured (star)
			auto to255 = [](double c) { return (unsigned char)(c < 0 ? 0 : c > 1 ? 255 : c * 255.0 + 0.5); };
			double *fc = sl->actor->GetProperty()->GetColor();
			double *ec = sl->actor->GetProperty()->GetEdgeColor();
			unsigned char cf[3] = { to255(fc[0]), to255(fc[1]), to255(fc[2]) };
			unsigned char ce[3] = { to255(ec[0]), to255(ec[1]), to255(ec[2]) };
			vtkNew<vtkUnsignedCharArray> arr; arr->SetNumberOfComponents(3); arr->SetNumberOfTuples(nL + nP);
			vtkIdType t = 0;                          // vtkPolyData cell order: Verts, Lines, Polys
			for (vtkIdType i = 0; i < nL; ++i, ++t) arr->SetTypedTuple(t, sl->filled ? ce : cf);
			for (vtkIdType i = 0; i < nP; ++i, ++t) arr->SetTypedTuple(t, cf);
			src->GetCellData()->SetScalars(arr);
			mp->ScalarVisibilityOn(); mp->SetScalarModeToUseCellData(); mp->SetColorModeToDirectScalars();
			sl->actor->GetProperty()->SetEdgeVisibility(0);
		} else {                                      // plain glyph: ordinary actor colouring
			src->GetCellData()->SetScalars(nullptr);
			mp->ScalarVisibilityOff();
			sl->actor->GetProperty()->SetEdgeVisibility(
				sl->filled && sl->actor->GetProperty()->GetLineWidth() > 0 ? 1 : 0);
		}
		src->Modified(); sl->glyph->Modified();
	};

	QMenu m(s->widget);
	// Tide-station layers get two download entries at the TOP of the menu. They hand the star under
	// the cursor (reuse the hover picker to grab its Name/Code/Country block) to Julia, which opens
	// the Mareg download window in the requested mode. Only shown when a tides callback is registered.
	QAction *dl2 = nullptr; QAction *dlCal = nullptr; std::string station;
	if (sl->name == "Tide Stations" && g_juliaTides && s->widget && s->widget->renderWindow()) {
		const QPoint lp = s->widget->mapFromGlobal(gp);
		const double r  = s->widget->devicePixelRatioF();
		const int    H  = s->widget->renderWindow()->GetSize()[1];
		pickSymbolInfoAt(s, int(lp.x() * r), int(H - lp.y() * r), station);
		dl2   = m.addAction("Download Mareg (2 days)");
		dlCal = m.addAction("Download Mareg (Calendar)");
		m.addSeparator();
	}
	// Tide-prediction (xtide.mat harmonic model) triangles get two entries at the top, same
	// reuse-the-hover-picker trick as the download entries above (the hover text IS the exact
	// station name, no further parsing needed -- see _tidestations_data). "(now)" = the existing
	// 7-day-centred-on-now window with the Next High/Low + "Now" cross. "(calendar)" lets the user
	// pick an arbitrary [start,end] (unlike the mareg download's calendar dialog, NOT capped at
	// "now" -- this is a harmonic MODEL, past and future predictions are equally valid).
	QAction *plotTidesNowA = nullptr; QAction *plotTidesCalA = nullptr; std::string tideStation;
	if (sl->name == "Tide Prediction Stations" && g_juliaTideModel && s->widget && s->widget->renderWindow()) {
		const QPoint lp = s->widget->mapFromGlobal(gp);
		const double r  = s->widget->devicePixelRatioF();
		const int    H  = s->widget->renderWindow()->GetSize()[1];
		pickSymbolInfoAt(s, int(lp.x() * r), int(H - lp.y() * r), tideStation);
		plotTidesNowA = m.addAction("Plot tides (now)");
		plotTidesCalA = m.addAction("Plot tides (calendar)");
		m.addSeparator();
	}
	QAction *tblA = m.addAction("Show data table…");      // floating point-table viewer (X/Y[/Z])
	// Any linked name labels (Cities' city names) get their OWN properties menu on a right-click of
	// the LABEL itself (70_window.cpp's view dispatch -> batchTextLabelsDialog) — never nested in
	// here. This menu stays symbol-only: shape/colour/size/stacking/remove.
	m.addSeparator();
	QMenu *tm = m.addMenu("Symbol");
	static const std::pair<const char*, const char*> KINDS[] = {
		{"Circle","c"}, {"Square","s"}, {"Triangle","t"}, {"Inverted triangle","i"},
		{"Diamond","d"}, {"Hexagon","h"}, {"Pentagon","n"}, {"Octagon","g"},
		{"Star","a"}, {"Cross","x"}, {"Plus","+"}, {"Dash","-"} };
	std::vector<QAction*> kindActs;
	for (const auto& k : KINDS) {
		QAction *a = tm->addAction(k.first); a->setCheckable(true); a->setChecked(sl->sym == k.second);
		kindActs.push_back(a);
	}
	QMenu *propM = m.addMenu("Symb properties");
	QAction *fillA = propM->addAction("Fill colour…");
	QAction *edgeA = propM->addAction("Edge colour…");
	QAction *sizeA = propM->addAction("Size (px)…");
	QAction *sizePtA = propM->addAction("Size (points)…");
	QAction *ewA   = propM->addAction("Edge width (px)…");
	m.addSeparator();
	// Draw-order stacking in the SHARED vector pile (overlays + symbols + polygons): controls who
	// draws on top where vector elements overlap; they stay on the relief (no z lift). Enabled once
	// there are 2+ vector elements of ANY type to order against.
	QMenu *stackMenu = m.addMenu("Stack order");
	QAction *topA = stackMenu->addAction("Place on top");
	QAction *botA = stackMenu->addAction("Place at bottom");
	QAction *upA  = stackMenu->addAction("Bring forward");
	QAction *dnA  = stackMenu->addAction("Send backward");
	const size_t nVec = s->overlays.size() + s->symbols.size() + s->polys.size();
	stackMenu->setEnabled(nVec > 1);
	for (QAction *a : { topA, botA, upA, dnA }) a->setEnabled(nVec > 1);
	m.addSeparator();
	QAction *delA  = m.addAction("Remove");
	QAction *ch = m.exec(gp);
	if (!ch) return;

	if (ch == tblA) { showSymbolDataTable(s, act, QString::fromStdString(sl->name)); return; }
	if (ch == plotTidesNowA) { g_juliaTideModel(s, "now", tideStation.c_str()); return; }
	if (ch == plotTidesCalA) {
		const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
		QDialog dlg(s->widget);
		dlg.setWindowTitle("Plot tides — date range (UTC)");
		QFormLayout *fl = new QFormLayout(&dlg);
		QDateTimeEdit *eStart = new QDateTimeEdit(nowUtc.addDays(-3), &dlg);
		QDateTimeEdit *eEnd   = new QDateTimeEdit(nowUtc.addDays(4), &dlg);
		for (QDateTimeEdit *e : { eStart, eEnd }) {
			e->setDisplayFormat("yyyy-MM-dd HH:mm");
			e->setCalendarPopup(true);
			e->setTimeSpec(Qt::UTC);
		}
		fl->addRow("Start time:", eStart);
		fl->addRow("End time:",   eEnd);
		QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		fl->addRow(bb);
		QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		// Keep end >= start as either side is edited (clamp toward the moved edge, don't fight the user).
		QObject::connect(eStart, &QDateTimeEdit::dateTimeChanged, &dlg,
		                 [eEnd](const QDateTime& d){ if (eEnd->dateTime() < d) eEnd->setDateTime(d); });
		QObject::connect(eEnd, &QDateTimeEdit::dateTimeChanged, &dlg,
		                 [eStart](const QDateTime& d){ if (eStart->dateTime() > d) eStart->setDateTime(d); });
		if (dlg.exec() != QDialog::Accepted) return;
		const QString req = "calendar/" + eStart->dateTime().toString("yyyy-MM-ddTHH:mm:ss")
		                  + "/"          + eEnd->dateTime().toString("yyyy-MM-ddTHH:mm:ss");
		g_juliaTideModel(s, req.toUtf8().constData(), tideStation.c_str());
		return;
	}
	if (ch == dl2)   { g_juliaTides(s, "2days",    station.c_str()); return; }
	if (ch == dlCal) {
		// Calendar download: pop a small dialog with two calendar-linked date/time editors (start,
		// end). Default = the last 2 days (start = now-2d, end = now), all in UTC to match the IOC
		// feed and the Profile time axis. Both editors are capped at "now" via setMaximumDateTime
		// so no future instant can ever be picked. On OK we hand Julia "calendar/<startISO>/<endISO>"
		// and _on_tides_download turns that into maregrams(starttime, days). No Q_OBJECT/moc needed
		// (no new signals/slots — connect to QDialog::accept and a clamp lambda).
		const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
		QDialog dlg(s->widget);
		dlg.setWindowTitle("Download Mareg — date range (UTC)");
		QFormLayout *fl = new QFormLayout(&dlg);
		QDateTimeEdit *eStart = new QDateTimeEdit(nowUtc.addDays(-2), &dlg);
		QDateTimeEdit *eEnd   = new QDateTimeEdit(nowUtc, &dlg);
		for (QDateTimeEdit *e : { eStart, eEnd }) {
			e->setDisplayFormat("yyyy-MM-dd HH:mm");
			e->setCalendarPopup(true);
			e->setTimeSpec(Qt::UTC);
			e->setMaximumDateTime(nowUtc);            // guard: no date/time in the future
		}
		fl->addRow("Start:", eStart);
		fl->addRow("End:",   eEnd);
		QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		fl->addRow(bb);
		QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		// Keep end >= start as either side is edited (clamp toward the moved edge, don't fight the user).
		QObject::connect(eStart, &QDateTimeEdit::dateTimeChanged, &dlg,
		                 [eEnd](const QDateTime& d){ if (eEnd->dateTime() < d) eEnd->setDateTime(d); });
		QObject::connect(eEnd, &QDateTimeEdit::dateTimeChanged, &dlg,
		                 [eStart](const QDateTime& d){ if (eStart->dateTime() > d) eStart->setDateTime(d); });
		if (dlg.exec() != QDialog::Accepted) return;
		const QString req = "calendar/" + eStart->dateTime().toString("yyyy-MM-ddTHH:mm:ss")
		                  + "/"          + eEnd->dateTime().toString("yyyy-MM-ddTHH:mm:ss");
		g_juliaTides(s, req.toUtf8().constData(), station.c_str());
		return;
	}

	for (size_t i = 0; i < kindActs.size(); ++i) if (ch == kindActs[i]) {     // change shape (any -> flat glyph)
		bool filled = true;
		sl->sym = KINDS[i].second;
		if (sl->solid3D) {
			// Downgrading a sphere/cube to a flat glyph: it currently rides the GPU-instanced
			// vtkGlyph3DMapper (see addSymbols) with no vtkGlyph3D at all, so rebuild the CPU
			// glyph pipeline flat symbols use, fed by the SAME points dataset (symInputPD).
			vtkPolyData *pd = symInputPD(*sl);
			vtkSmartPointer<vtkGlyph3D> gg = vtkSmartPointer<vtkGlyph3D>::New();
			gg->SetSourceData(makeSymbolGlyph(sl->sym, filled));
			gg->SetInputData(pd);
			gg->SetScaleModeToDataScalingOff();
			gg->OrientOff();
			gg->SetScaleFactor(sl->sizePx);
			vtkNew<vtkPolyDataMapper> map;
			map->SetInputConnection(gg->GetOutputPort());
			map->ScalarVisibilityOff();
			vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
			map->SetRelativeCoincidentTopologyPolygonOffsetParameters(0.0, -8000.0);
			map->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, -8000.0);
			map->SetRelativeCoincidentTopologyPointOffsetParameter(-8000.0);
			sl->actor->SetMapper(map);
			sl->glyph = gg;
			sl->glyphMapper = nullptr;
			sl->zfix = nullptr;
			sl->zfixFilter = nullptr;
			sl->solid3D = false;
			sl->actor->GetProperty()->SetLighting(false);
		}
		else {
			sl->glyph->SetSourceData(makeSymbolGlyph(sl->sym, filled));
			sl->glyph->Modified();
		}
		sl->filled = filled;
		applyCellColours();                          // star -> per-cell outline; others -> actor edges
		reRender(); return;
	}
	if (ch == fillA) {
		double *c = sl->actor->GetProperty()->GetColor();
		QColor q = QColorDialog::getColor(QColor(int(c[0]*255), int(c[1]*255), int(c[2]*255)), s->widget, "Fill colour");
		if (q.isValid()) { sl->actor->GetProperty()->SetColor(q.redF(), q.greenF(), q.blueF()); applyCellColours(); reRender(); }
		return;
	}
	if (ch == edgeA) {
		double *c = sl->actor->GetProperty()->GetEdgeColor();
		QColor q = QColorDialog::getColor(QColor(int(c[0]*255), int(c[1]*255), int(c[2]*255)), s->widget, "Edge colour");
		if (q.isValid()) { sl->actor->GetProperty()->SetEdgeColor(q.redF(), q.greenF(), q.blueF()); applyCellColours(); reRender(); }
		return;
	}
	// Size dialogs: a live QDoubleSpinBox so the glyph resizes AS the value changes (not on OK).
	// px and points share one helper; pxPerUnit converts the spinbox unit to sl->sizePx (1 pt = 96/72 px).
	auto liveSizeDialog = [&](const char *unitLabel, double pxPerUnit, double lo, double hi) {
		QDialog dlg(s->widget);
		dlg.setWindowTitle("Symbol size");
		QFormLayout *form = new QFormLayout(&dlg);
		QDoubleSpinBox *box = new QDoubleSpinBox(&dlg);
		box->setRange(lo, hi); box->setSingleStep(1.0); box->setDecimals(1);
		box->setValue(sl->sizePx / pxPerUnit);
		QObject::connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [s, sl, pxPerUnit](double v) {
			sl->sizePx = v * pxPerUnit;
			symbolRescaleCB(nullptr, 0, s, nullptr);     // re-stamp glyph scale, then redraw
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		});
		form->addRow(QString("Size (%1)").arg(unitLabel), box);
		QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
		QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
		form->addRow(bb);
		dlg.exec();
	};
	if (ch == sizeA)   { liveSizeDialog("px", 1.0, 1, 400); return; }
	if (ch == sizePtA) { liveSizeDialog("points", 96.0 / 72.0, 1, 300); return; }
	if (ch == ewA) {
		bool ok = false;
		double v = QInputDialog::getDouble(s->widget, "Edge width", "width (px):",
		                                   sl->actor->GetProperty()->GetLineWidth(), 0, 20, 1, &ok);
		if (ok) {
			sl->actor->GetProperty()->SetLineWidth(v);
			if (sl->filled) sl->actor->GetProperty()->SetEdgeVisibility(v > 0 ? 1 : 0);
			reRender();
		}
		return;
	}
	if (ch == topA || ch == botA || ch == upA || ch == dnA) {
		restackVector(s, &sl->stack, ch == topA ? 0 : ch == botA ? 1 : ch == upA ? 2 : 3);
		reRender(); return;
	}
	if (ch == delA) {
		for (size_t i = 0; i < s->symbols.size(); ++i) if (s->symbols[i].actor.Get() == act) {
			if (s->ren)     s->ren->RemoveActor(act);
			if (s->axesRen) s->axesRen->RemoveActor(act);   // may have been parked on the overlay layer
			s->symbols.erase(s->symbols.begin() + i);
			break;
		}
		applyVectorStacking(s);        // renormalize ranks after the removed slot
		rebuildSceneObjects(s); reRender();
		return;
	}
}

// Add a Fledermaus-style vertical "curtain": an image hung on a vertical wall that
// follows the XY track (px,py) THROUGH the scene (a seismic / midwater profile). Geometry
// = 2N verts (top+bottom per column) joined by N-1 quads; the picture is an UNLIT texture
// (emissive look = true colour, doesn't take the scene lighting). `u` is the per-column
// horizontal texture coord (0..1, computed host-side: chord length or even spacing). `topz`
// (or null) clips each column's top vertex to a surface so the wall hugs the relief; null =
// flat top at zmax. img is row-major, row 0 = BOTTOM of the picture, w*h pixels of `comps`
// each. Shares the surface's scale so the wall rises/falls with the relief under VE.
// Texture from a raw RGB[A] buffer (row 0 = BOTTOM of the picture, VTK texture origin).
static vtkSmartPointer<vtkTexture> makeBufferTexture(const unsigned char *img, int w, int h, int comps) {
	if (!img || w <= 0 || h <= 0 || comps <= 0)
		return nullptr;
	vtkNew<vtkImageData> tex_img;
	tex_img->SetDimensions(w, h, 1);
	tex_img->AllocateScalars(VTK_UNSIGNED_CHAR, comps);
	memcpy(tex_img->GetScalarPointer(), img, (size_t)w * h * comps);
	vtkSmartPointer<vtkTexture> tex = vtkSmartPointer<vtkTexture>::New();
	tex->SetInputData(tex_img);
	tex->InterpolateOn();
	return tex;
}

// Texture decoded from an image FILE by VTK itself (JPEG/PNG/TIFF/...). Avoids any GMT
// layout ambiguity. VTK image readers already place the picture's first (top) scanline at
// the bottom of the texture image, which matches our tcoord convention (v=1 at the wall
// top shows the picture top), so NO Y-flip is applied. Use flipv at the call site to invert.
static vtkSmartPointer<vtkTexture> makeFileTexture(const char *path) {
	if (!path || !*path)
		return nullptr;
	vtkSmartPointer<vtkImageReader2> reader;
	reader.TakeReference(vtkImageReader2Factory::CreateImageReader2(path));
	if (!reader)
		return nullptr;
	reader->SetFileName(path);
	reader->Update();
	vtkSmartPointer<vtkTexture> tex = vtkSmartPointer<vtkTexture>::New();
	tex->SetInputConnection(reader->GetOutputPort());
	tex->InterpolateOn();
	return tex;
}

static void addCurtain(Scene *s, const double *px, const double *py, const double *u, int n,
					   const double *topz, vtkSmartPointer<vtkTexture> tex,
					   double zmin, double zmax, int flipv) {
	if (!s || !px || !py || n < 2 || !tex || zmax <= zmin)
		return;
	const double span = zmax - zmin;
	auto vat = [&](double z) -> float {
		float f = float((z - zmin) / span);
		return flipv ? (1.0f - f) : f;        // image first scanline -> top by default
	};

	vtkNew<vtkPoints> pts; pts->SetDataTypeToDouble(); pts->Allocate(2 * n);
	vtkNew<vtkFloatArray> tc; tc->SetNumberOfComponents(2); tc->SetName("tc"); tc->Allocate(2 * 2 * n);
	for (int i = 0; i < n; ++i) {
		double ztop = topz ? topz[i] : zmax;
		if (ztop < zmin) ztop = zmin;
		if (ztop > zmax) ztop = zmax;
		pts->InsertNextPoint(px[i], py[i], ztop);     // vertex 2i   = top
		pts->InsertNextPoint(px[i], py[i], zmin);     // vertex 2i+1 = bottom
		float ui = float(u ? u[i] : (n > 1 ? double(i) / (n - 1) : 0.0));
		tc->InsertNextTuple2(ui, vat(ztop));
		tc->InsertNextTuple2(ui, vat(zmin));
	}
	vtkNew<vtkCellArray> quads;
	for (int i = 0; i < n - 1; ++i) {
		vtkIdType q[4] = { 2*i, 2*i+1, 2*(i+1)+1, 2*(i+1) };   // top_i, bot_i, bot_i+1, top_i+1
		quads->InsertNextCell(4, q);
	}
	vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
	pd->SetPoints(pts);
	pd->SetPolys(quads);
	pd->GetPointData()->SetTCoords(tc);

	vtkNew<vtkPolyDataMapper> map;
	map->SetInputData(pd);
	map->ScalarVisibilityOff();               // colour comes from the texture

	vtkSmartPointer<vtkActor> a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(map);
	a->SetTexture(tex);
	a->GetProperty()->LightingOff();          // unlit = the emissive curtain look (true image colour)
	a->SetScale(s->xfac, 1.0, s->zfac * s->ve);   // hang in the surface's scaled space

	s->ren->AddActor(a);
	Curtain cu{ a, "Curtain " + std::to_string((int)s->curtains.size() + 1) };
	s->curtains.push_back(cu);
	rebuildSceneObjects(s);                   // refresh the Scene Objects checkbox list
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// A 1-D stipple texture: one repeat = `onFrac` opaque dash/dot then a transparent gap, with
// SOFT (antialiased) edges so it doesn't look ragged. RGB is the line's colour (so it shows
// even if VTK replaces material colour with the texture); alpha gives the gaps. Tiled along the
// line via tcoords -> dashes/dots, with NO change to the geometry (one cheap cell).
static vtkSmartPointer<vtkTexture> makeStripeTex(double onFrac, double r, double g, double b) {
	const int W = 256;
	const double on   = std::max(2.0, onFrac * W);     // dash length in texels
	const double edge = std::clamp(0.08 * W, 2.0, on * 0.5);   // soft ramp width
	vtkNew<vtkImageData> img;
	img->SetDimensions(W, 1, 1);
	img->AllocateScalars(VTK_UNSIGNED_CHAR, 4);
	unsigned char *p = static_cast<unsigned char*>(img->GetScalarPointer());
	const unsigned char cr = (unsigned char)std::lround(255.0 * r);
	const unsigned char cg = (unsigned char)std::lround(255.0 * g);
	const unsigned char cb = (unsigned char)std::lround(255.0 * b);
	for (int x = 0; x < W; ++x) {
		// smoothstep up at [0,edge], full across the dash, smoothstep down at [on-edge,on]
		double a;
		const double xd = x + 0.5;
		if (xd >= on) a = 0.0;
		else {
			const double rise = std::clamp(xd / edge, 0.0, 1.0);
			const double fall = std::clamp((on - xd) / edge, 0.0, 1.0);
			const double s = std::min(rise, fall);
			a = s * s * (3.0 - 2.0 * s);               // smoothstep
		}
		p[4*x+0] = cr; p[4*x+1] = cg; p[4*x+2] = cb;
		p[4*x+3] = (unsigned char)std::lround(255.0 * a);
	}
	vtkSmartPointer<vtkTexture> t = vtkSmartPointer<vtkTexture>::New();
	t->SetInputData(img);
	t->InterpolateOn();                         // smooth (antialiased) edges
	t->MipmapOn();                              // no shimmer when zoomed out
	t->RepeatOn();                              // tile along the line
	return t;
}

// 1-D texture coords along the line = cumulative arc length / `period`, measured in the
// DISPLAYED (actor-scaled) space so z (metres, drawn at zfac) does not dominate the spacing.
// `period` is a FIXED world length = one dash+gap, so the pattern repeats every `period` along
// EVERY polyline independently (a coastline's short island segments still get real dots — the
// old total-length normalization gave each short polyline < 1 repeat -> a solid blob).
static void setStippleTCoords(vtkPolyData *pd, const double sc[3], double period) {
	vtkPoints *sp = pd->GetPoints();
	vtkCellArray *sl = pd->GetLines();
	if (!sp || !sl || !(period > 0.0)) return;
	const vtkIdType np = sp->GetNumberOfPoints();
	vtkNew<vtkFloatArray> tc; tc->SetNumberOfComponents(2); tc->SetNumberOfTuples(np);
	for (vtkIdType i = 0; i < np; ++i) tc->SetTuple2(i, 0.0, 0.0);

	auto segLen = [&](const double A[3], const double B[3]) {
		const double dx = (B[0]-A[0])*sc[0], dy = (B[1]-A[1])*sc[1], dz = (B[2]-A[2])*sc[2];
		const double L = std::sqrt(dx*dx + dy*dy + dz*dz);
		return (L > 0.0 && L < 1e30) ? L : 0.0;
	};
	const double inv = 1.0 / period;
	vtkNew<vtkIdList> idl;
	sl->InitTraversal();
	while (sl->GetNextCell(idl)) {
		const vtkIdType n = idl->GetNumberOfIds();
		if (n > 0) tc->SetTuple2(idl->GetId(0), 0.0, 0.0);
		double cum = 0.0;
		for (vtkIdType k = 0; k + 1 < n; ++k) {
			double A[3], B[3]; sp->GetPoint(idl->GetId(k), A); sp->GetPoint(idl->GetId(k+1), B);
			cum += segLen(A, B);
			tc->SetTuple2(idl->GetId(k+1), cum * inv, 0.0);
		}
	}
	pd->GetPointData()->SetTCoords(tc);
}

// Apply a line style: 0 = solid, 1 = dashed, 2 = dotted. Implemented as a repeating stipple
// TEXTURE on the unchanged line geometry (one cell), so it is cheap and crash-proof regardless
// of how densely the line is sampled. The texture carries the current colour, so a colour edit
// re-runs this. `style` is remembered on the Overlay so the colour action can rebuild.
static void applyLineStyle(Scene *s, vtkActor *a, int style) {
	Overlay *ov = nullptr;
	for (auto& o : s->overlays) if (o.actor.Get() == a) { ov = &o; break; }
	if (!ov || !ov->baseLine) return;
	vtkPolyDataMapper *m = vtkPolyDataMapper::SafeDownCast(a->GetMapper());
	if (!m) return;
	ov->lineStyle = style;
	m->SetInputData(ov->baseLine);              // geometry never changes
	if (style == 0) {                           // solid: drop the texture + tcoords
		a->SetTexture(nullptr);
		ov->baseLine->GetPointData()->SetTCoords(nullptr);
		a->GetProperty()->SetOpacity(1.0);
		ov->stripeTex = nullptr;
	}
	else {
		double sc[3]; a->GetScale(sc);
		// period = a small FIXED fraction of the line's horizontal (scaled) extent -> a fine,
		// even pattern on every polyline, independent of total length / island count.
		double b[6]; ov->baseLine->GetBounds(b);
		const double ex = (b[1]-b[0])*sc[0], ey = (b[3]-b[2])*sc[1];
		double diag = std::sqrt(ex*ex + ey*ey);
		if (!(diag > 0.0)) diag = 1.0;
		const double onFrac = (style == 1) ? 0.5  : 0.18;     // dotted: short on -> a dot
		const double nDiv   = (style == 1) ? 100.0 : 260.0;   // dotted: more, closer
		const double period = diag / nDiv;
		double c[3]; a->GetProperty()->GetColor(c);
		setStippleTCoords(ov->baseLine, sc, period);
		ov->stripeTex = makeStripeTex(onFrac, c[0], c[1], c[2]);
		a->SetTexture(ov->stripeTex);
		a->GetProperty()->SetOpacity(0.999);    // force the translucent pass so alpha gaps show
	}
	m->Modified();
	if (s->widget) s->widget->renderWindow()->Render();
}

// Per-element context menu for an overlay (lines: colour / width / style / tubes; points:
// colour / size / round). Pops on a RIGHT-click only (customContextMenuRequested, 70_window.cpp);
// no visual selection state, just the menu.
static void popupOverlayMenu(Scene *s, vtkActor *a, int mode, const QPoint& globalPos) {
	if (!s || !a) return;
	if (mode == 1) {                         // lines -> the shared Line Properties tool
		QString nm = "Line";
		for (auto& o : s->overlays) if (o.actor.Get() == a) { nm = QString::fromStdString(o.name); break; }
		popupLineObjectMenu(s, LineRef{ LK_Overlay, a }, nm, globalPos);
		return;
	}
	QWidget *win = s->win;
	QMenu m(win);
	m.addAction("Overlay color…", [=]() {
		double c[3]; a->GetProperty()->GetColor(c);
		QColor init(int(c[0]*255), int(c[1]*255), int(c[2]*255));
		QColor q = QColorDialog::getColor(init, win, "Overlay color");
		if (q.isValid()) {
			a->GetProperty()->SetColor(q.redF(), q.greenF(), q.blueF());
			Overlay *ov = nullptr;                       // a dashed/dotted line carries its colour in
			for (auto& o : s->overlays) if (o.actor.Get() == a) { ov = &o; break; }   // the stipple
			if (ov && ov->lineStyle != 0) applyLineStyle(s, a, ov->lineStyle);         // texture -> rebuild
			else s->widget->renderWindow()->Render();
		}
	});
	if (mode == 1) {                         // lines: width + style + tube
		m.addAction("Line width…", [=]() {
			bool ok = false;
			double w = QInputDialog::getDouble(win, "Line width", "width (px):",
				a->GetProperty()->GetLineWidth(), 0.5, 40.0, 1, &ok);
			if (ok) {
				a->GetProperty()->SetLineWidth(w);
				s->widget->renderWindow()->Render();
			}
		});
		QMenu *st = m.addMenu("Line style");
		st->addAction("Solid",  [=]() { applyLineStyle(s, a, 0); });
		st->addAction("Dashed", [=]() { applyLineStyle(s, a, 1); });
		st->addAction("Dotted", [=]() { applyLineStyle(s, a, 2); });
		QAction *tu = m.addAction("Render as tubes", [=]() {
			const bool on = !a->GetProperty()->GetRenderLinesAsTubes();
			a->GetProperty()->SetRenderLinesAsTubes(on);
			if (on) {                            // tube = shaded round cross-section: need lighting,
				a->GetProperty()->LightingOn();  // Phong, and a wide line or it looks like a flat line
				a->GetProperty()->SetInterpolationToPhong();
				a->GetProperty()->SetAmbient(0.25); a->GetProperty()->SetDiffuse(0.8);
				a->GetProperty()->SetSpecular(0.3); a->GetProperty()->SetSpecularPower(20.0);
				if (a->GetProperty()->GetLineWidth() < 10.0) a->GetProperty()->SetLineWidth(10.0);
			}
			else {
				a->GetProperty()->LightingOff();
			}
			s->widget->renderWindow()->Render();
		});
		tu->setCheckable(true); tu->setChecked(a->GetProperty()->GetRenderLinesAsTubes());
	}
	else {                                   // points: size + round toggle
		m.addAction("Point size…", [=]() {           // live spinbox (mirror the Line Properties dialog)
			QDialog dlg(win);
			dlg.setWindowTitle("Point size");
			QFormLayout *form = new QFormLayout(&dlg);
			QDoubleSpinBox *szBox = new QDoubleSpinBox(&dlg);
			szBox->setRange(1.0, 60.0); szBox->setSingleStep(1.0); szBox->setDecimals(1);
			szBox->setValue(a->GetProperty()->GetPointSize());
			QObject::connect(szBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [s, a](double sz) {
				a->GetProperty()->SetPointSize(sz);
				if (s->widget) s->widget->renderWindow()->Render();   // apply as the value changes
			});
			form->addRow("Size (px)", szBox);
			QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
			QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
			QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
			form->addRow(bb);
			dlg.exec();
		});
		QAction *rp = m.addAction("Round points", [=]() {
			a->GetProperty()->SetRenderPointsAsSpheres(!a->GetProperty()->GetRenderPointsAsSpheres());
			s->widget->renderWindow()->Render();
		});
		rp->setCheckable(true); rp->setChecked(a->GetProperty()->GetRenderPointsAsSpheres());
	}
	m.addSeparator();
	m.addAction("Hide overlay", [=]() {
		a->SetVisibility(0);
		s->widget->renderWindow()->Render();
	});
	m.exec(globalPos);
}

// Delete an overlay line/point element (coastlines, boundaries, rivers, imported xy) by its actor:
// drop the actor, erase the record, restack and rebuild the Scene Objects list. The TWIN of
// polygonDelete — overlays hide via the Scene Objects checkbox and DELETE via the unified line menu.
static void overlayDelete(Scene *s, vtkActor *a) {
	for (int i = 0; i < (int)s->overlays.size(); ++i) {
		if (s->overlays[i].actor.Get() != a) continue;
		if (s->ren && s->overlays[i].actor)     s->ren->RemoveActor(s->overlays[i].actor);
		if (s->axesRen && s->overlays[i].actor) s->axesRen->RemoveActor(s->overlays[i].actor);  // overlay layer
		s->overlays.erase(s->overlays.begin() + i);
		break;
	}
	applyVectorStacking(s);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Delete EVERY overlay tagged with `groupName` (the grouped-overlay row's own "Remove" property,
// e.g. Geography > Plate boundaries' "Plate boundaries PB" handle deleting all 7 boundary-type
// layers in one go) -- same actor/record removal overlayDelete does per-item, batched into ONE
// restack + rebuild + render instead of one per member.
static void overlayDeleteGroup(Scene *s, const std::string& groupName) {
	if (!s || groupName.empty()) return;
	for (int i = (int)s->overlays.size() - 1; i >= 0; --i) {
		if (s->overlays[i].groupName != groupName) continue;
		if (s->ren && s->overlays[i].actor)     s->ren->RemoveActor(s->overlays[i].actor);
		if (s->axesRen && s->overlays[i].actor) s->axesRen->RemoveActor(s->overlays[i].actor);
		s->overlays.erase(s->overlays.begin() + i);
	}
	applyVectorStacking(s);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Right-click menu for the profile track line. The property entries now live in the shared Line
// Properties tool (55_lineprops.cpp); this just routes the profile line to the unified menu.
static void popupProfileMenu(Scene *s, const QPoint& globalPos) {
	if (!s || !s->profLine) return;
	popupLineObjectMenu(s, LineRef{ LK_Profile, s->profLine }, "Profile", globalPos);
}

// QVTKOpenGLNativeWidget forwards left/right/wheel to the VTK interactor, but VTK's adapter
// never delivers the MIDDLE button to our observers (both MiddleCB and the old event-filter
// were dead for that reason). Qt DOES deliver mouse events to the widget itself (the
// right-click context menu proves it), so handle the middle button HERE in the widget's own
// Qt handlers: middle-click with no drag recenters the view on the surface point under the
// cursor (identical math to the working `c` hotkey); middle-drag pans. Left/right/wheel fall
// through to the base class unchanged, so VTK interaction + the context menu still work.
