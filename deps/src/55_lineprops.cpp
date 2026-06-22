// ============================================================================
//  Line Properties tool — a shared, extensible dialog for every line-like scene
//  object (the profile track, GMTdataset line overlays, drawn polygons).
//
//  It supersedes the old per-line context-menu property entries: those menus now
//  just open this dialog ("Line properties…"). The same dialog is reachable from a
//  right-click on the line in the 3-D view OR a right-click on the object's row in
//  the Scene Objects list. Add a new property by adding one row to the QFormLayout
//  in showLineProperties — every line type picks it up.
// ============================================================================

// Current solid/dashed/dotted style of a line object (so the dialog opens on the right value).
static int lineCurrentStyle(Scene* s, const LineRef& lr) {
	if (lr.kind == LK_Profile) return s->profStyle;
	if (lr.kind == LK_Overlay)
		for (auto& o : s->overlays) if (o.actor.Get() == lr.actor) return o.lineStyle;
	return 0;                                            // polygons: solid only for now
}

// Apply solid(0)/dashed(1)/dotted(2) to a line object. Each kind stipples differently: overlays
// go through applyLineStyle (per-overlay stipple texture); the profile rebuilds its own stripe on
// s->profPD; polygons stay solid (their draped geometry is rebuilt on every vertex edit, which
// would drop a stipple). Centralised here so the dialog is style-agnostic.
static void lineApplyStyle(Scene* s, const LineRef& lr, int style) {
	vtkActor* a = lr.actor;
	if (!a) return;
	if (lr.kind == LK_Overlay) { applyLineStyle(s, a, style); return; }
	if (lr.kind == LK_Polygon) {                         // polygon: solid only
		a->SetTexture(nullptr);
		if (s->widget) s->widget->renderWindow()->Render();
		return;
	}
	// LK_Profile — mirror the stipple path computeProfile / the old menu used.
	if (!s->profPD) return;
	s->profStyle = style;
	if (style == 0) {
		a->SetTexture(nullptr);
		s->profPD->GetPointData()->SetTCoords(nullptr);
		a->GetProperty()->SetOpacity(1.0);
		s->profStripe = nullptr;
	}
	else {
		double sc[3]; a->GetScale(sc);
		double b[6]; s->profPD->GetBounds(b);
		const double ex = (b[1]-b[0])*sc[0], ey = (b[3]-b[2])*sc[1];
		double diag = std::sqrt(ex*ex + ey*ey);
		if (!(diag > 0.0)) diag = 1.0;
		const double onFrac = (style == 1) ? 0.5 : 0.18;   // dotted = short on -> a dot
		const double nDiv   = (style == 1) ? 100.0 : 260.0;
		setStippleTCoords(s->profPD, sc, diag / nDiv);
		double c[3]; a->GetProperty()->GetColor(c);
		s->profStripe = makeStripeTex(onFrac, c[0], c[1], c[2]);
		a->SetTexture(s->profStripe);
		a->GetProperty()->SetOpacity(0.999);               // force translucent pass so alpha gaps show
	}
	if (auto* mm = vtkPolyDataMapper::SafeDownCast(a->GetMapper())) mm->Modified();
	if (s->widget) s->widget->renderWindow()->Render();
}

// The dialog. Live-applies every change to the actor; add a row to extend it.
static void showLineProperties(Scene* s, const LineRef& lr) {
	if (!s || !lr.actor) return;
	vtkActor* a = lr.actor;
	QDialog dlg(s->win);
	dlg.setWindowTitle("Line Properties");
	QFormLayout* form = new QFormLayout(&dlg);

	// --- Colour: a swatch button that opens QColorDialog ---------------------
	double c0[3]; a->GetProperty()->GetColor(c0);
	QPushButton* colBtn = new QPushButton(&dlg);
	auto paintSwatch = [colBtn](const QColor& q) {
		colBtn->setStyleSheet(QString("background:%1; min-width:64px;").arg(q.name()));
	};
	paintSwatch(QColor(int(c0[0]*255), int(c0[1]*255), int(c0[2]*255)));
	QObject::connect(colBtn, &QPushButton::clicked, [&dlg, s, lr, a, paintSwatch]() {
		double c[3]; a->GetProperty()->GetColor(c);
		QColor q = QColorDialog::getColor(QColor(int(c[0]*255), int(c[1]*255), int(c[2]*255)),
										  &dlg, "Line color");
		if (!q.isValid()) return;
		a->GetProperty()->SetColor(q.redF(), q.greenF(), q.blueF());
		paintSwatch(q);
		lineApplyStyle(s, lr, lineCurrentStyle(s, lr));    // refresh the stipple colour if dashed/dotted
		if (s->widget) s->widget->renderWindow()->Render();
	});
	form->addRow("Color", colBtn);

	// --- Width ---------------------------------------------------------------
	QDoubleSpinBox* wBox = new QDoubleSpinBox(&dlg);
	wBox->setRange(0.5, 40.0); wBox->setSingleStep(0.5); wBox->setDecimals(1);
	wBox->setValue(a->GetProperty()->GetLineWidth());
	QObject::connect(wBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [s, a](double w) {
		a->GetProperty()->SetLineWidth(w);
		if (s->widget) s->widget->renderWindow()->Render();
	});
	form->addRow("Width (px)", wBox);

	// --- Style (solid / dashed / dotted) -------------------------------------
	QComboBox* stBox = new QComboBox(&dlg);
	stBox->addItems(QStringList() << "Solid" << "Dashed" << "Dotted");
	stBox->setCurrentIndex(lineCurrentStyle(s, lr));
	if (lr.kind == LK_Polygon) stBox->setEnabled(false);   // polygon outlines: solid only for now
	QObject::connect(stBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [s, lr](int idx) {
		lineApplyStyle(s, lr, idx);
	});
	form->addRow("Style", stBox);

	// --- Render as tubes -----------------------------------------------------
	QCheckBox* tubeBox = new QCheckBox(&dlg);
	tubeBox->setChecked(a->GetProperty()->GetRenderLinesAsTubes());
	QObject::connect(tubeBox, &QCheckBox::toggled, [s, a, wBox](bool on) {
		a->GetProperty()->SetRenderLinesAsTubes(on);
		if (on) {                                          // tube = shaded round cross-section
			a->GetProperty()->LightingOn();
			a->GetProperty()->SetInterpolationToPhong();
			a->GetProperty()->SetAmbient(0.25); a->GetProperty()->SetDiffuse(0.8);
			a->GetProperty()->SetSpecular(0.3); a->GetProperty()->SetSpecularPower(20.0);
			if (a->GetProperty()->GetLineWidth() < 10.0) { a->GetProperty()->SetLineWidth(10.0); wBox->setValue(10.0); }
		}
		else a->GetProperty()->LightingOff();
		if (s->widget) s->widget->renderWindow()->Render();
	});
	form->addRow("Render as tubes", tubeBox);

	// --- Opacity -------------------------------------------------------------
	QDoubleSpinBox* opBox = new QDoubleSpinBox(&dlg);
	opBox->setRange(0.05, 1.0); opBox->setSingleStep(0.05); opBox->setDecimals(2);
	opBox->setValue(a->GetProperty()->GetOpacity());
	QObject::connect(opBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [s, a](double v) {
		a->GetProperty()->SetOpacity(v);
		if (s->widget) s->widget->renderWindow()->Render();
	});
	form->addRow("Opacity", opBox);

	QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
	QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	form->addRow(bb);

	dlg.exec();
}

// Collect a line object's polylines in TRUE coords (one inner vector per polyline cell).
// Polygon -> its closed ring; profile -> the draped track; overlay -> each polyline cell.
static void lineGatherPolylines(Scene* s, const LineRef& lr,
								std::vector<std::vector<std::array<double,3>>>& out) {
	out.clear();
	if (lr.kind == LK_Polygon) {
		for (auto& pg : s->polys) if (pg.line.Get() == lr.actor) { out.push_back(pg.v); break; }
	}
	else if (lr.kind == LK_Profile) {
		if (s->profPD && s->profPD->GetPoints()) {
			vtkPoints* pts = s->profPD->GetPoints();
			std::vector<std::array<double,3>> pl;
			for (vtkIdType i = 0; i < pts->GetNumberOfPoints(); ++i) {
				double p[3]; pts->GetPoint(i, p); pl.push_back({ p[0], p[1], p[2] });
			}
			out.push_back(pl);
		}
	}
	else {                                               // overlay line: walk each polyline cell
		for (auto& o : s->overlays) {
			if (o.actor.Get() != lr.actor || !o.baseLine) continue;
			vtkPoints* pts = o.baseLine->GetPoints();
			vtkCellArray* lines = o.baseLine->GetLines();
			if (pts && lines) {
				lines->InitTraversal();
				vtkNew<vtkIdList> ids;
				while (lines->GetNextCell(ids)) {
					std::vector<std::array<double,3>> pl;
					for (vtkIdType k = 0; k < ids->GetNumberOfIds(); ++k) {
						double p[3]; pts->GetPoint(ids->GetId(k), p); pl.push_back({ p[0], p[1], p[2] });
					}
					out.push_back(pl);
				}
			}
			break;
		}
	}
}

// Write the gathered polylines as a multisegment table. 2D = x y (corners). 3D = x y z, each
// segment subdivided (one sub-point per grid node) with z INTERPOLATED from the grid below
// (sampleZ); off a grid, the stored vertex z is used. Multi-segment uses GMT '>' headers.
static bool lineWriteTable(Scene* s, const std::vector<std::vector<std::array<double,3>>>& polylines,
						   bool threeD, const QString& path) {
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
	QTextStream out(&f);
	const bool haveGrid = !s->gridZ.empty();
	double spacing = 0.0;                                // grid node spacing for the 3-D subdivision
	if (haveGrid) {
		const double sx = std::abs(s->gdx), sy = std::abs(s->gdy);
		spacing = (sx > 0 && sy > 0) ? std::min(sx, sy) : std::max(sx, sy);
	}
	const bool multi = polylines.size() > 1;
	for (auto& pl : polylines) {
		if (pl.empty()) continue;
		if (multi) out << ">\n";                         // GMT multisegment header
		if (!threeD) {
			for (auto& p : pl) out << p[0] << '\t' << p[1] << '\n';   // 2D: corners only
			continue;
		}
		const int m = (int)pl.size();                    // 3D: subdivide + interpolate z from the grid
		for (int i = 0; i < m - 1; ++i) {
			const auto& a = pl[i];
			const auto& b = pl[i + 1];
			const double dx = b[0] - a[0], dy = b[1] - a[1];
			const double dist = std::hypot(dx, dy);
			int nsub = 1;
			if (haveGrid && spacing > 0.0 && dist > 0.0)
				nsub = std::clamp((int)std::ceil(dist / spacing), 1, 20000);
			for (int k = 0; k < nsub; ++k) {             // a..(b exclusive); next segment writes b
				const double t = (double)k / nsub;
				const double x = a[0] + t * dx, y = a[1] + t * dy;
				double z = a[2] + t * (b[2] - a[2]);     // fallback: linear along the chord
				if (haveGrid) { const double h = sampleZ(s, x, y); if (!std::isnan(h)) z = h; }
				out << x << '\t' << y << '\t' << z << '\n';
			}
		}
		const auto& last = pl[m - 1];                    // the final vertex
		double z = last[2];
		if (haveGrid) { const double h = sampleZ(s, last[0], last[1]); if (!std::isnan(h)) z = h; }
		out << last[0] << '\t' << last[1] << '\t' << z << '\n';
	}
	f.close();
	return true;
}

// Save a line/polygon. The user picks 2D (x y) or 3D (x y z, grid-interpolated), then a file. A
// plain table extension (.txt/.dat) is written directly; an OGR vector extension (.gpkg/.shp/.kml)
// is written by handing a temp table to GMT.gmtwrite in Julia (the C++ viewer has no GMT) — GMT
// picks the format from the extension. `g_juliaEval` is the in-process bridge to the host session.
static void lineSavePoints(Scene* s, const LineRef& lr) {
	std::vector<std::vector<std::array<double,3>>> polylines;
	lineGatherPolylines(s, lr, polylines);
	if (polylines.empty()) return;
	const bool isPoly = (lr.kind == LK_Polygon);

	QStringList opts; opts << "2D  (x y)" << "3D  (x y z)";
	bool ok = false;
	QString choice = QInputDialog::getItem(s->win, isPoly ? "Save polygon" : "Save line",
										   "Coordinates:", opts, s->gridZ.empty() ? 0 : 1, false, &ok);
	if (!ok) return;
	const bool threeD = choice.startsWith("3D");

	const QString defName = isPoly ? "polygon.gpkg" : "line.txt";
	QString fn = QFileDialog::getSaveFileName(s->win, isPoly ? "Save polygon" : "Save line", defName,
		"GMT table (*.txt *.dat);;GeoPackage (*.gpkg);;Shapefile (*.shp);;KML (*.kml);;All files (*)");
	if (fn.isEmpty()) return;

	const QString ext = QFileInfo(fn).suffix().toLower();
	const bool ogr = (ext == "gpkg" || ext == "shp" || ext == "kml");

	if (!ogr) {                                          // plain table -> write directly
		lineWriteTable(s, polylines, threeD, fn);
		return;
	}
	// OGR vector format: write a temp table, then GMT.gmtwrite it in Julia by the chosen extension.
	if (!g_juliaEval) {
		QMessageBox::warning(s->win, "Save",
			"Saving to .gpkg/.shp/.kml needs the Julia/GMT host (use a plain .txt here).");
		return;
	}
	const QString tmp = QDir::tempPath() + "/igmt_save_" +
						QString::number(QDateTime::currentMSecsSinceEpoch()) + ".txt";
	if (!lineWriteTable(s, polylines, threeD, tmp)) return;
	std::string cmd = "InteractiveGMT._gmtwrite_line(raw\"" + tmp.toStdString() + "\", raw\"" +
					  fn.toStdString() + "\", " + (isPoly ? "true" : "false") + ")";
	std::vector<char> buf(1 << 12);
	int n = g_juliaEval(s, cmd.c_str(), buf.data(), (int)buf.size());   // _gmtwrite_line removes the temp file
	if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));     // save failed -> Errors tab
}

// Find the index of the finished polygon whose actor is `a`, or -1. (s->polys can be re-found on
// every edit so a deleted/reordered polygon never gets a stale index written back.)
static int polyIndexOfActor(Scene* s, vtkActor* a) {
	for (size_t i = 0; i < s->polys.size(); ++i) if (s->polys[i].line.Get() == a) return (int)i;
	return -1;
}

// Floating data viewer for a line object: a non-modal window with a table of its vertices in TRUE
// coords. Reuses lineGatherPolylines, so it serves polygon / polyline / rect / circle / profile /
// overlay alike. Columns are #, X, Y, Z. For the drawn shapes (LK_Polygon: polygon / polyline /
// rect / circle) the X/Y/Z cells are EDITABLE: committing a cell writes the new coordinate back to
// pg.v, keeps a closed ring's duplicated first/last vertex in sync, and rebuilds + re-renders the
// outline live. Profile/overlay lines are GMT-owned, so they stay read-only. The window is its own
// top-level (no parent), deletes itself on close, and does NOT block the viewer.
static void showLineDataTable(Scene* s, const LineRef& lr, const QString& name) {
	std::vector<std::vector<std::array<double,3>>> polylines;
	lineGatherPolylines(s, lr, polylines);
	if (polylines.empty()) return;

	// Editing writes back to a single polygon ring; only LK_Polygon has that 1:1 row<->pg.v mapping.
	const bool editable = (lr.kind == LK_Polygon && polylines.size() == 1);
	const std::vector<std::array<double,3>>& pl = polylines[0];
	const int nrows = (int)pl.size();
	vtkActor* actor = lr.actor;

	QDialog* dlg = new QDialog(nullptr);                  // top-level, parentless -> truly floating
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle(name.isEmpty() ? QString("Line data") : (name + " — data"));
	dlg->setWindowFlag(Qt::Window, true);
	QVBoxLayout* lay = new QVBoxLayout(dlg);

	// In flat-2D the Z is a meaningless z=0, so drop the Z column there: cols are #/X/Y (3) in 2D,
	// #/X/Y/Z (4) in 3D. ncoord = number of coordinate columns drawn (2 or 3).
	const int  ncoord = s->flat2d ? 2 : 3;
	QStringList hdr;   hdr << "#" << "X" << "Y";   if (!s->flat2d) hdr << "Z";
	QTableWidget* tbl = new QTableWidget(nrows, ncoord + 1, dlg);
	tbl->setHorizontalHeaderLabels(hdr);
	tbl->verticalHeader()->setVisible(false);            // our "#" column already numbers the rows
	tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
	if (!editable) tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);

	for (int k = 0; k < nrows; ++k) {
		QTableWidgetItem* idx = new QTableWidgetItem(QString::number(k + 1));
		idx->setFlags(idx->flags() & ~Qt::ItemIsEditable);   // the "#" column is never editable
		tbl->setItem(k, 0, idx);
		for (int c = 0; c < ncoord; ++c) {
			QTableWidgetItem* it = new QTableWidgetItem(QString::number(pl[k][c], 'g', 10));
			if (!editable) it->setFlags(it->flags() & ~Qt::ItemIsEditable);
			tbl->setItem(k, c + 1, it);
		}
	}
	tbl->resizeColumnsToContents();
	lay->addWidget(tbl);
	dlg->resize(360, 420);

	// Live write-back: a committed X/Y/Z cell updates pg.v and rebuilds the outline. Connected only
	// for editable (LK_Polygon) tables. cellChanged also fires when we programmatically fix a cell,
	// so a guard flag stops re-entrancy.
	if (editable) {
		std::shared_ptr<bool> guard = std::make_shared<bool>(false);
		QObject::connect(tbl, &QTableWidget::cellChanged, dlg, [s, tbl, actor, guard](int row, int col) {
			if (*guard || col < 1 || col > 3) return;        // ignore the "#" column / our own edits
			const int pi = polyIndexOfActor(s, actor);
			if (pi < 0) return;                              // polygon was deleted -> nothing to write
			Polygon& pg = s->polys[pi];
			const int n = (int)pg.v.size();
			if (row < 0 || row >= n) return;
			const int ci = col - 1;                          // X=0 / Y=1 / Z=2
			bool ok = false;
			const double val = tbl->item(row, col)->text().toDouble(&ok);
			*guard = true;
			if (!ok) {                                       // bad number -> restore the old cell text
				tbl->item(row, col)->setText(QString::number(pg.v[row][ci], 'g', 10));
				*guard = false;
				return;
			}
			pg.v[row][ci] = val;
			// Closed ring keeps first == last; mirror the partner vertex and its cell.
			if (pg.closed && n >= 2 && (pg.v.front() == pg.v.back() || row == 0 || row == n - 1)) {
				const int partner = (row == 0) ? n - 1 : (row == n - 1) ? 0 : -1;
				if (partner >= 0) {
					pg.v[partner][ci] = val;
					tbl->item(partner, col)->setText(QString::number(val, 'g', 10));
				}
			}
			*guard = false;
			polyRebuildLine(s, pg);
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		});
	}
	dlg->show();                                          // non-modal: REPL + viewer stay live
}

// The unified right-click menu for a line object: "Line properties…" plus the kind's own actions
// (profile: save / delete; overlay & polygon: hide; polygon: delete). Shared by the 3-D-view
// right-click hit-test and the Scene Objects list rows, so both routes give the same menu.
static void popupLineObjectMenu(Scene* s, const LineRef& lr, const QString& name, const QPoint& gp) {
	if (!s || !lr.actor) return;
	QMenu m(s->win);
	vtkActor* a = lr.actor;
	m.addAction("Line properties…", [s, lr]() { showLineProperties(s, lr); });
	m.addAction(lr.kind == LK_Polygon ? "Save polygon…" : "Save line…",   // 2D / 3D (grid-interpolated z)
				[s, lr]() { lineSavePoints(s, lr); });
	m.addAction("Show data table…",                                      // floating vertex table viewer
				[s, lr, name]() { showLineDataTable(s, lr, name); });
	m.addSeparator();
	if (lr.kind == LK_Profile) {
		m.addAction("Save profile (with distance)…", [s]() {
			if (!s->profPD || !s->profPD->GetPoints()) return;
			QString fn = QFileDialog::getSaveFileName(s->win, "Save profile", "profile.txt",
													  "Text (*.txt *.dat);;All files (*)");
			if (fn.isEmpty()) return;
			QFile f(fn);
			if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
			QTextStream out(&f);
			out << "# distance\tx\ty\tz\n";
			vtkPoints* pts = s->profPD->GetPoints();
			const vtkIdType np = pts->GetNumberOfPoints();
			for (vtkIdType i = 0; i < np; ++i) {
				double p[3]; pts->GetPoint(i, p);
				const double dd = (i < (vtkIdType)s->profS.size()) ? s->profS[i] : 0.0;
				out << dd << '\t' << p[0] << '\t' << p[1] << '\t' << p[2] << '\n';
			}
			f.close();
		});
		m.addAction("Delete profile", [s]() { profileClear(s); rebuildSceneObjects(s); });
	}
	else if (lr.kind == LK_Polygon) {
		m.addAction("Delete polygon", [s, a]() { polygonDelete(s, a); });   // Hide = the Scene Objects checkbox
	}
	else {                                               // overlay line
		m.addAction("Hide overlay", [s, a]() { a->SetVisibility(0); if (s->widget) s->widget->renderWindow()->Render(); });
	}
	// Shared vector-pile draw-order: order this line/polygon against ALL other vector elements
	// (overlays + symbols + polygons). Not for the singleton profile track. Stays on the relief.
	int* stackPtr = nullptr;
	if (lr.kind == LK_Overlay)      { for (auto& o  : s->overlays) if (o.actor.Get() == a) { stackPtr = &o.stack;  break; } }
	else if (lr.kind == LK_Polygon) { for (auto& pg : s->polys)    if (pg.line.Get() == a) { stackPtr = &pg.stack; break; } }
	const size_t nVec = s->overlays.size() + s->symbols.size() + s->polys.size();
	if (stackPtr && nVec > 1) {
		m.addSeparator();
		auto reRender = [s]() { if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render(); };
		m.addAction("Place on top",    [s, stackPtr, reRender]() { restackVector(s, stackPtr, 0); reRender(); });
		m.addAction("Place at bottom", [s, stackPtr, reRender]() { restackVector(s, stackPtr, 1); reRender(); });
		m.addAction("Stack up",        [s, stackPtr, reRender]() { restackVector(s, stackPtr, 2); reRender(); });
		m.addAction("Stack down",      [s, stackPtr, reRender]() { restackVector(s, stackPtr, 3); reRender(); });
	}
	(void)name;
	m.exec(gp);
}
