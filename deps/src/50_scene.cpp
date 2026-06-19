static void rebuildSceneObjects(Scene* s);          // defined just below; refreshed after edits
static void textApplyProps(Scene* s, TextLabel& tl); // 85_polygon.cpp: re-apply font fields to the actor

// Hand a colormap NAME back to the Julia host, which recomputes CPT nodes over the surface's data
// range and ccalls gmtvtk_set_cpt to recolour live. `fig` is bound to THIS window by _console_eval.
static void applyColormap(Scene* s, const QString& name) {
	if (!s || !g_juliaEval || name.isEmpty()) return;
	std::string cmd = "InteractiveGMT._recolor(fig, \"" + name.toStdString() + "\")";
	std::vector<char> buf(1 << 12);
	g_juliaEval(s, cmd.c_str(), buf.data(), (int)buf.size());
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
static void layoutColorbar(Scene* s) {
	if (!s || !s->bar) return;
	const double X0 = s->barX0, Y0 = s->barY0;
	const double barLeft = X0 + cbar::W * (1.0 - cbar::BARRATIO);
	s->bar->SetPosition(X0, Y0);
	s->bar->SetWidth(cbar::W); s->bar->SetHeight(cbar::H);
	const double lo = s->zmin, hi = s->zmax;
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

// Build the colorbar actors once and add them to the renderer. zmin/zmax must already be set.
static void buildColorbar(Scene* s, vtkScalarsToColors* lut) {
	if (!s || s->imageOnly) return;          // bare image -> no colorbar
	s->bar = vtkSmartPointer<vtkScalarBarActor>::New();
	s->bar->SetLookupTable(lut);
	s->bar->SetTitle("");                    // drop the big 'Z' title
	s->bar->SetDrawTickLabels(false);        // WE draw the numbers — kill the overlapping built-ins
	s->bar->SetTextPositionToPrecedeScalarBar();
	s->bar->SetBarRatio(cbar::BARRATIO);
	s->ren->AddActor2D(s->bar);

	// Nice round tick values (800, 900, ...) at a constant 1/2/5 x10^n step.
	const double lo = s->zmin, hi = s->zmax;
	const double step = niceNum(niceNum(hi - lo, false) / 5.0, true);
	s->barValues.clear(); s->barLabels.clear();
	s->barTickPts = vtkSmartPointer<vtkPoints>::New();
	vtkNew<vtkCellArray> tlines;
	for (double v = std::ceil(lo / step) * step; v <= hi + 1e-9 * (hi - lo); v += step) {
		s->barValues.push_back(v);
		vtkIdType a = s->barTickPts->InsertNextPoint(0, 0, 0);   // real coords set by layoutColorbar
		vtkIdType b = s->barTickPts->InsertNextPoint(0, 0, 0);
		tlines->InsertNextCell(2); tlines->InsertCellPoint(a); tlines->InsertCellPoint(b);
		char buf[32]; snprintf(buf, sizeof buf, "%.0f", v);
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

// Show/hide the WHOLE colorbar (strip + ticks + numbers) together. The old code toggled only the
// strip, so the numbers kept floating after the strip vanished.
static void setColorbarVisible(Scene* s, bool on) {
	if (!s || !s->bar) return;
	s->bar->SetVisibility(on ? 1 : 0);
	if (s->barTicks) s->barTicks->SetVisibility(on ? 1 : 0);
	for (auto& ta : s->barLabels) if (ta) ta->SetVisibility(on ? 1 : 0);
}

static bool colorbarVisible(Scene* s) { return s && s->bar && s->bar->GetVisibility() != 0; }

// --- colorbar left-drag, handled at the GLView widget level (60_profile.cpp), exactly like the
// polygon-vertex / text-label drags. Qt delivers the press to the widget BEFORE VTK's interactor
// adapter, so a VTK observer would lose the race to the trackball — the widget path is the only
// reliable one in this codebase. nx/ny are NORMALIZED viewport coords (bottom-up, 0..1).
static bool colorbarHit(Scene* s, double nx, double ny) {   // cursor over the (visible) bar frame?
	if (!s || !s->bar || !colorbarVisible(s)) return false;
	const double L = s->barX0 - 0.05, R = s->barX0 + cbar::W;   // include the numbers to the left
	const double B = s->barY0 - 0.02, T = s->barY0 + cbar::H + 0.02;
	return nx >= L && nx <= R && ny >= B && ny <= T;
}
static bool colorbarGrab(Scene* s, double nx, double ny) {
	if (!colorbarHit(s, nx, ny)) return false;
	s->barDragging = true;
	s->barGrabX = nx - s->barX0;
	s->barGrabY = ny - s->barY0;
	return true;
}
static bool colorbarDragTo(Scene* s, double nx, double ny) {
	if (!s || !s->barDragging) return false;
	s->barX0 = std::min(std::max(nx - s->barGrabX, 0.05), 1.0 - cbar::W);
	s->barY0 = std::min(std::max(ny - s->barGrabY, 0.0),  1.0 - cbar::H);
	layoutColorbar(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return true;
}
static bool colorbarRelease(Scene* s) {
	if (!s || !s->barDragging) return false;
	s->barDragging = false;
	return true;
}

// The colormap chooser: a popup of common GMT master CPTs (applied on click) plus a Custom… entry
// for any name GMT's makecpt accepts. Opened from the colorbar's Scene Objects row.
static void chooseColormap(Scene* s, const QPoint& gp) {
	if (!s || !s->bar) return;
	static const char* kMaps[] = {
		"viridis", "turbo", "jet", "hot", "haxby", "geo", "relief", "rainbow",
		"polar", "seis", "gray", "plasma", "magma", "cividis", "roma", "vik",
	};
	QMenu m(s->win);
	for (const char* nm : kMaps) {
		const QString q = QString::fromLatin1(nm);
		m.addAction(q, [s, q]() { applyColormap(s, q); });
	}
	m.addSeparator();
	m.addAction("Custom…", [s]() {
		bool ok = false;
		const QString nm = QInputDialog::getText(s->win, "Colormap", "GMT CPT name:",
		                                         QLineEdit::Normal, "", &ok);
		if (ok) applyColormap(s, nm.trimmed());
	});
	m.exec(gp);
}

// Font / colour editor for a text label. Reachable from the label's Scene-Objects row or a canvas
// right-click. Edits the string, family (Arial/Courier/Times — VTK's built-in faces), size, colour,
// bold and italic, then re-applies and re-renders.
static void textPropsDialog(Scene* s, vtkTextActor3D* act) {
	TextLabel* tl = nullptr;
	for (auto& t : s->texts) if (t.actor.Get() == act) { tl = &t; break; }
	if (!tl) return;

	QDialog d(s->widget);
	d.setWindowTitle("Text Properties");
	QFormLayout* fl = new QFormLayout(&d);
	QLineEdit* eText = new QLineEdit(QString::fromStdString(tl->text), &d);
	QComboBox* eFont = new QComboBox(&d);
	eFont->addItems({ "Arial", "Courier", "Times" });
	eFont->setCurrentText(QString::fromStdString(tl->font));
	QSpinBox* eSize = new QSpinBox(&d); eSize->setRange(4, 300); eSize->setValue(tl->size);
	QCheckBox* eBold = new QCheckBox("Bold", &d);   eBold->setChecked(tl->bold);
	QCheckBox* eItal = new QCheckBox("Italic", &d); eItal->setChecked(tl->italic);
	QColor col = QColor::fromRgbF(tl->color[0], tl->color[1], tl->color[2]);
	QPushButton* eColor = new QPushButton(&d);
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
	QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
	fl->addRow(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
	if (d.exec() != QDialog::Accepted) return;

	tl->text   = eText->text().toStdString();
	tl->font   = eFont->currentText().toStdString();
	tl->size   = eSize->value();
	tl->bold   = eBold->isChecked();
	tl->italic = eItal->isChecked();
	tl->color[0] = col.redF(); tl->color[1] = col.greenF(); tl->color[2] = col.blueF();
	textApplyProps(s, *tl);
	rebuildSceneObjects(s);                            // the row label tracks the (possibly new) text
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Right-click menu for a text label: edit properties or delete it.
static void textLabelMenu(Scene* s, vtkTextActor3D* act, const QPoint& globalPos) {
	QMenu m(s->widget);
	QAction* props = m.addAction("Text Properties…");
	QAction* del   = m.addAction("Delete text");
	QAction* chosen = m.exec(globalPos);
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
               IC_Polygon, IC_Polyline, IC_Rect, IC_Circle, IC_Text, IC_ColorBar, IC_Profile };

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
	}
	p.end();
	return pm;
}

static void rebuildSceneObjects(Scene* s) {
	if (!s || !s->objPanel)
		return;
	// Wipe the previous layout + its checkboxes before rebuilding.
	if (QLayout* old = s->objPanel->layout()) {
		QLayoutItem* it;
		while ((it = old->takeAt(0)) != nullptr) {
			if (it->widget())
				it->widget()->deleteLater();
			delete it;
		}
		delete old;
	}
	// Small checkboxes: the indicator is the ONLY hit target for show/hide. The type icon + the
	// descriptive label sit to its right; right-clicking the icon/label opens the row's properties
	// menu (toggle is no longer wired to the text — Fledermaus behaviour the user asked for).
	s->objPanel->setStyleSheet("QCheckBox::indicator{width:13px;height:13px;}");
	QVBoxLayout* col = new QVBoxLayout(s->objPanel);
	col->setContentsMargins(8, 8, 8, 8);
	col->setSpacing(3);

	// One row = [checkbox] [type icon] [label]. onToggle drives visibility; onProps (optional) is the
	// properties menu, opened by a LEFT click on the description label (the checkbox only toggles).
	auto makeRow = [&](const QString& label, int iconKind, bool checked,
	                   std::function<void(bool)> onToggle,
	                   std::function<void(const QPoint&)> onProps,
	                   const QString& tip = QString()) {
		QWidget* row = new QWidget(s->objPanel);
		QHBoxLayout* h = new QHBoxLayout(row);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(5);

		QCheckBox* cb = new QCheckBox(row);                  // box only — no text, so only the box toggles
		cb->setChecked(checked);
		cb->setToolTip("Show / hide");
		QObject::connect(cb, &QCheckBox::toggled, [s, onToggle](bool on) {
			onToggle(on);
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		});

		QLabel* icon = new QLabel(row); icon->setPixmap(makeObjectIcon(iconKind));
		ClickableLabel* text = new ClickableLabel(label, row);   // left-click -> properties
		if (!tip.isEmpty()) { icon->setToolTip(tip); text->setToolTip(tip); }

		h->addWidget(cb, 0);
		h->addWidget(icon, 0);
		h->addWidget(text, 1);

		if (onProps) {                                      // left-click the description -> properties menu
			text->setCursor(Qt::PointingHandCursor);
			text->onClick = onProps;
		}
		col->addWidget(row);
	};

	// Actor-backed rows (show/hide = actor visibility); lr != null adds the line-object menu.
	auto addRow = [&](const QString& label, vtkProp3D* a, int iconKind, const LineRef* lr = nullptr) {
		if (!a)
			return;
		std::function<void(const QPoint&)> ctx = nullptr;
		if (lr) { LineRef ref = *lr; QString nm = label;
			ctx = [s, ref, nm](const QPoint& g) { popupLineObjectMenu(s, ref, nm, g); }; }
		makeRow(label, iconKind, a->GetVisibility() != 0,
		        [a](bool on) { a->SetVisibility(on ? 1 : 0); }, ctx,
		        lr ? QString("Left-click for properties") : QString());
	};

	// A bare image has NO surface: don't list one, and the image is the object itself (not an
	// "Image drape" over a surface).
	if (!s->imageOnly)
		addRow(s->surfName.empty() ? QString("Surface") : QString::fromStdString(s->surfName), surfProp(s), IC_Surface);
	if (s->drape)
		addRow(s->imageOnly ? (s->surfName.empty() ? QString("Image") : QString::fromStdString(s->surfName))
		                    : QString("Image drape"),
		       s->drape, IC_Image);
	if (s->bar)                                          // colorbar: toggle visibility + colormap chooser
		makeRow("Color Bar", IC_ColorBar, s->bar->GetVisibility() != 0,
		        [s](bool on) { setColorbarVisible(s, on); },
		        [s](const QPoint& g) { chooseColormap(s, g); },
		        "Left-click to choose a colormap");
	for (auto& ov : s->overlays) {
		LineRef lr{ LK_Overlay, ov.actor };
		addRow(QString::fromStdString(ov.name), ov.actor, ov.mode == 1 ? IC_Line : IC_Points, ov.mode == 1 ? &lr : nullptr);
	}
	for (auto& cu : s->curtains)
		addRow(QString::fromStdString(cu.name), cu.actor, IC_Curtain);
	for (auto& pg : s->polys) {                          // user-drawn polygons / polylines / rects / circles
		LineRef lr{ LK_Polygon, pg.line };
		const QString nm = QString::fromStdString(pg.name);   // name is prefixed per type by polyFinalize
		int ic = !pg.closed              ? IC_Polyline
		       : nm.startsWith("rect")   ? IC_Rect
		       : nm.startsWith("circle") ? IC_Circle
		                                 : IC_Polygon;
		addRow(nm, pg.line, ic, &lr);
	}
	for (auto& tl : s->texts) {                          // user-placed text labels (toggle + right-click menu)
		if (!tl.actor) continue;
		vtkTextActor3D* act = tl.actor.Get();
		makeRow(QString::fromStdString(tl.name), IC_Text, act->GetVisibility() != 0,
		        [act](bool on) { act->SetVisibility(on ? 1 : 0); },
		        [s, act](const QPoint& g) { textLabelMenu(s, act, g); },
		        "Left-click for properties");
	}
	if (s->profLine && s->profLine->GetVisibility()) {  // the profile track (when one exists)
		LineRef lr{ LK_Profile, s->profLine };
		addRow("Profile", s->profLine, IC_Profile, &lr);
	}
	for (auto& ex : s->extras) {                 // grids/images dropped in after the window opened
		addRow(QString::fromStdString(ex.name), ex.actor, IC_Surface);
		if (ex.drape) addRow(QString::fromStdString(ex.name + " (image)"), ex.drape, IC_Image);
	}
	col->addStretch(1);
}

static void addOverlay(Scene* s, const double* xyz, int npts, const int* segoff, int nseg,
					   int mode, double r, double g, double b, double linewidth, double pointsize) {
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
	a->GetProperty()->SetLineWidth(linewidth > 0.0 ? linewidth : 2.0);
	a->GetProperty()->SetPointSize(pointsize > 0.0 ? pointsize : 6.0);
	if (mode == 0)
		a->GetProperty()->SetRenderPointsAsSpheres(true);   // round points (toggle in the menu)
	a->SetScale(s->xfac, 1.0, s->zfac * s->ve);             // register with the surface

	s->ren->AddActor(a);
	Overlay ov{ a, mode };
	if (mode == 1) ov.baseLine = pd;          // keep the solid geometry for line-style restyling
	ov.name = (mode == 1 ? "Line " : "Points ") + std::to_string((int)s->overlays.size() + 1);
	s->overlays.push_back(ov);
	rebuildSceneObjects(s);                   // refresh the Scene Objects checkbox list
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
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
static vtkSmartPointer<vtkTexture> makeBufferTexture(const unsigned char* img, int w, int h, int comps) {
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
static vtkSmartPointer<vtkTexture> makeFileTexture(const char* path) {
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

static void addCurtain(Scene* s, const double* px, const double* py, const double* u, int n,
					   const double* topz, vtkSmartPointer<vtkTexture> tex,
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
	unsigned char* p = static_cast<unsigned char*>(img->GetScalarPointer());
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
static void setStippleTCoords(vtkPolyData* pd, const double sc[3], double period) {
	vtkPoints*    sp = pd->GetPoints();
	vtkCellArray* sl = pd->GetLines();
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
static void applyLineStyle(Scene* s, vtkActor* a, int style) {
	Overlay* ov = nullptr;
	for (auto& o : s->overlays) if (o.actor.Get() == a) { ov = &o; break; }
	if (!ov || !ov->baseLine) return;
	vtkPolyDataMapper* m = vtkPolyDataMapper::SafeDownCast(a->GetMapper());
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
// colour / size / round). Pops on a left- OR right-click that lands on the element; no
// visual selection state, just the menu.
static void popupOverlayMenu(Scene* s, vtkActor* a, int mode, const QPoint& globalPos) {
	if (!s || !a) return;
	if (mode == 1) {                         // lines -> the shared Line Properties tool
		QString nm = "Line";
		for (auto& o : s->overlays) if (o.actor.Get() == a) { nm = QString::fromStdString(o.name); break; }
		popupLineObjectMenu(s, LineRef{ LK_Overlay, a }, nm, globalPos);
		return;
	}
	QWidget* win = s->win;
	QMenu m(win);
	m.addAction("Overlay color…", [=]() {
		double c[3]; a->GetProperty()->GetColor(c);
		QColor init(int(c[0]*255), int(c[1]*255), int(c[2]*255));
		QColor q = QColorDialog::getColor(init, win, "Overlay color");
		if (q.isValid()) {
			a->GetProperty()->SetColor(q.redF(), q.greenF(), q.blueF());
			Overlay* ov = nullptr;                       // a dashed/dotted line carries its colour in
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
		QMenu* st = m.addMenu("Line style");
		st->addAction("Solid",  [=]() { applyLineStyle(s, a, 0); });
		st->addAction("Dashed", [=]() { applyLineStyle(s, a, 1); });
		st->addAction("Dotted", [=]() { applyLineStyle(s, a, 2); });
		QAction* tu = m.addAction("Render as tubes", [=]() {
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
		m.addAction("Point size…", [=]() {
			bool ok = false;
			double sz = QInputDialog::getDouble(win, "Point size", "size (px):",
				a->GetProperty()->GetPointSize(), 1.0, 60.0, 1, &ok);
			if (ok) {
				a->GetProperty()->SetPointSize(sz);
				s->widget->renderWindow()->Render();
			}
		});
		QAction* rp = m.addAction("Round points", [=]() {
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

// Right-click menu for the profile track line. The property entries now live in the shared Line
// Properties tool (55_lineprops.cpp); this just routes the profile line to the unified menu.
static void popupProfileMenu(Scene* s, const QPoint& globalPos) {
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
