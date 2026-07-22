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
static int lineCurrentStyle(Scene *s, const LineRef& lr) {
	if (lr.kind == LK_Profile) return s->profStyle;
	if (lr.kind == LK_Overlay)
		for (auto& o : s->overlays) if (o.actor.Get() == lr.actor) return o.lineStyle;
	return 0;                                            // polygons: solid only for now
}

// Apply solid(0)/dashed(1)/dotted(2) to a line object. Each kind stipples differently: overlays
// go through applyLineStyle (per-overlay stipple texture); the profile rebuilds its own stripe on
// s->profPD; polygons stay solid (their draped geometry is rebuilt on every vertex edit, which
// would drop a stipple). Centralised here so the dialog is style-agnostic.
static void lineApplyStyle(Scene *s, const LineRef& lr, int style) {
	vtkActor *a = lr.actor;
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
	if (auto *mm = vtkPolyDataMapper::SafeDownCast(a->GetMapper())) mm->Modified();
	if (s->widget) s->widget->renderWindow()->Render();
}

// The dialog. Live-applies every change to the actor; add a row to extend it.
static void showLineProperties(Scene *s, const LineRef& lr) {
	if (!s || !lr.actor) return;
	vtkActor *a = lr.actor;
	QDialog dlg(s->win);
	dlg.setWindowTitle("Line Properties");
	QFormLayout *form = new QFormLayout(&dlg);

	// --- Colour: a swatch button that opens QColorDialog ---------------------
	double c0[3]; a->GetProperty()->GetColor(c0);
	QPushButton *colBtn = new QPushButton(&dlg);
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
	// One underlying VTK line width (pixels), exposed BOTH in pixels and in POINTS (the cartographic
	// / GMT pen unit). px = pt · dpi/72; the two boxes stay in two-way sync. A guard flag stops the
	// programmatic setValue of one box from recursing back into the other.
	double dpi = 72.0;
	if (s->widget && s->widget->renderWindow() && s->widget->renderWindow()->GetDPI() > 0)
		dpi = s->widget->renderWindow()->GetDPI();
	const double pxPerPt = dpi / 72.0;
	auto wGuard = std::make_shared<bool>(false);

	QDoubleSpinBox *wBox = new QDoubleSpinBox(&dlg);
	wBox->setRange(0.5, 40.0); wBox->setSingleStep(0.5); wBox->setDecimals(1);
	wBox->setValue(a->GetProperty()->GetLineWidth());

	QDoubleSpinBox *wptBox = new QDoubleSpinBox(&dlg);
	wptBox->setRange(0.25, 30.0); wptBox->setSingleStep(0.25); wptBox->setDecimals(2);
	wptBox->setValue(a->GetProperty()->GetLineWidth() / pxPerPt);

	QObject::connect(wBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
		[s, a, wptBox, pxPerPt, wGuard](double w) {
			if (*wGuard) return;
			*wGuard = true;
			a->GetProperty()->SetLineWidth(w);
			wptBox->setValue(w / pxPerPt);
			*wGuard = false;
			if (s->widget) s->widget->renderWindow()->Render();
		});
	QObject::connect(wptBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
		[s, a, wBox, pxPerPt, wGuard](double pt) {
			if (*wGuard) return;
			*wGuard = true;
			const double w = pt * pxPerPt;
			a->GetProperty()->SetLineWidth(w);
			wBox->setValue(w);
			*wGuard = false;
			if (s->widget) s->widget->renderWindow()->Render();
		});
	form->addRow("Width (px)", wBox);
	form->addRow("Width (points)", wptBox);

	// --- Style (solid / dashed / dotted) -------------------------------------
	QComboBox *stBox = new QComboBox(&dlg);
	stBox->addItems(QStringList() << "Solid" << "Dashed" << "Dotted");
	stBox->setCurrentIndex(lineCurrentStyle(s, lr));
	if (lr.kind == LK_Polygon) stBox->setEnabled(false);   // polygon outlines: solid only for now
	QObject::connect(stBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [s, lr](int idx) {
		lineApplyStyle(s, lr, idx);
	});
	form->addRow("Style", stBox);

	// --- Render as tubes -----------------------------------------------------
	QCheckBox *tubeBox = new QCheckBox(&dlg);
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
	QDoubleSpinBox *opBox = new QDoubleSpinBox(&dlg);
	opBox->setRange(0.05, 1.0); opBox->setSingleStep(0.05); opBox->setDecimals(2);
	opBox->setValue(a->GetProperty()->GetOpacity());
	QObject::connect(opBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [s, a](double v) {
		a->GetProperty()->SetOpacity(v);
		if (s->widget) s->widget->renderWindow()->Render();
	});
	form->addRow("Opacity", opBox);

	// --- Fill (closed polygons / rectangles only) ----------------------------
	// A closed ring carries a FILLED FACE whose colour and transparency are INDEPENDENT of the
	// outline above. Default fill opacity is 0 (no fill) so the historic outline-only look is kept
	// until the user dials it up here. Open lines / polylines have no fill, so these rows are absent.
	if (lineClosedRing(s, lr)) {
		const int pi0 = polyIndexOfActor(s, a);
		if (pi0 >= 0) {
			double f0[3]; { Polygon& pg = s->polys[pi0]; f0[0]=pg.fillColor[0]; f0[1]=pg.fillColor[1]; f0[2]=pg.fillColor[2]; }

			QPushButton *fillBtn = new QPushButton(&dlg);
			auto paintFill = [fillBtn](const QColor& q) {
				fillBtn->setStyleSheet(QString("background:%1; min-width:64px;").arg(q.name()));
			};
			paintFill(QColor(int(f0[0]*255), int(f0[1]*255), int(f0[2]*255)));
			QObject::connect(fillBtn, &QPushButton::clicked, [&dlg, s, a, paintFill]() {
				const int pi = polyIndexOfActor(s, a);
				if (pi < 0) return;
				Polygon& pg = s->polys[pi];
				QColor q = QColorDialog::getColor(QColor(int(pg.fillColor[0]*255), int(pg.fillColor[1]*255),
				                                         int(pg.fillColor[2]*255)), &dlg, "Fill color");
				if (!q.isValid()) return;
				pg.fillColor[0]=q.redF(); pg.fillColor[1]=q.greenF(); pg.fillColor[2]=q.blueF();
				if (pg.fill) pg.fill->GetProperty()->SetColor(pg.fillColor[0], pg.fillColor[1], pg.fillColor[2]);
				paintFill(q);
				if (s->widget) s->widget->renderWindow()->Render();
			});
			form->addRow("Fill color", fillBtn);

			// Fill transparency: 0 = none (no fill drawn) … 1 = opaque. Independent of the outline opacity.
			QDoubleSpinBox *fopBox = new QDoubleSpinBox(&dlg);
			fopBox->setRange(0.0, 1.0); fopBox->setSingleStep(0.05); fopBox->setDecimals(2);
			fopBox->setValue(s->polys[pi0].fillOpacity);
			QObject::connect(fopBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [s, a](double v) {
				const int pi = polyIndexOfActor(s, a);
				if (pi < 0) return;
				Polygon& pg = s->polys[pi];
				pg.fillOpacity = v;
				if (pg.fill) {
					pg.fill->GetProperty()->SetOpacity(v);
					pg.fill->SetVisibility((v > 0.0 && pg.line && pg.line->GetVisibility()) ? 1 : 0);
				}
				if (s->widget) s->widget->renderWindow()->Render();
			});
			form->addRow("Fill opacity", fopBox);
		}
	}

	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
	QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	form->addRow(bb);

	dlg.exec();
}

// Collect a line object's polylines in TRUE coords (one inner vector per polyline cell).
// Polygon -> its closed ring; profile -> the draped track; overlay -> each polyline cell.
static void lineGatherPolylines(Scene *s, const LineRef& lr,
								std::vector<std::vector<std::array<double,3>>>& out) {
	out.clear();
	if (lr.kind == LK_Polygon) {
		for (auto& pg : s->polys) if (pg.line.Get() == lr.actor) { out.push_back(pg.v); break; }
	}
	else if (lr.kind == LK_Profile) {
		if (s->profPD && s->profPD->GetPoints()) {
			vtkPoints *pts = s->profPD->GetPoints();
			std::vector<std::array<double,3>> pl;
			for (vtkIdType i = 0; i < pts->GetNumberOfPoints(); ++i) {
				double p[3]; pts->GetPoint(i, p); pl.push_back({ p[0], p[1], p[2] });
			}
			out.push_back(pl);
		}
	}
	else {                                               // overlay (line OR points mode): walk each
		// stored SEGMENT via ov.segoff -- set unconditionally by addOverlay for both modes, so this is
		// the ONE way to recover an overlay's segments regardless of how its polydata stores cells. A
		// points-mode overlay (vtkPolyData::SetVerts, no SetLines at all) has ZERO line cells, so the
		// old GetLines()-traversal silently produced an empty `out` for every points-mode overlay (a
		// SHAPENC Point ensemble, "Convert to points", ...) -- "Show data table..." did nothing, no
		// error. ov.segoff/nseg already carry the same segment boundaries GetLines() would have
		// walked, so reading them directly works for either mode with no branch, instead of a second
		// points-only traversal bolted on next to this one.
		for (auto& o : s->overlays) {
			if (o.actor.Get() != lr.actor || !o.baseLine) continue;
			vtkPoints *pts = o.baseLine->GetPoints();
			if (pts && o.nseg > 0 && (int)o.segoff.size() == o.nseg + 1) {
				for (int k = 0; k < o.nseg; ++k) {
					const int a = o.segoff[k], z = o.segoff[k + 1];
					if (z <= a) continue;
					std::vector<std::array<double,3>> pl;
					for (int i = a; i < z; ++i) {
						double p[3]; pts->GetPoint(i, p); pl.push_back({ p[0], p[1], p[2] });
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
static bool lineWriteTable(Scene *s, const std::vector<std::vector<std::array<double,3>>>& polylines,
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
static void lineSavePoints(Scene *s, const LineRef& lr) {
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
	QString fn = QFileDialog::getSaveFileName(s->win, isPoly ? "Save polygon" : "Save line", prefStartDir(defName),
		"GMT table (*.txt *.dat);;GeoPackage (*.gpkg);;Shapefile (*.shp);;KML (*.kml);;All files (*)");
	if (fn.isEmpty()) return;
	rememberStartDir(fn);

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
static int polyIndexOfActor(Scene *s, vtkActor *a) {
	for (size_t i = 0; i < s->polys.size(); ++i) if (s->polys[i].line.Get() == a) return (int)i;
	return -1;
}

// Floating data viewer for a line object: a non-modal window with a table of its vertices in TRUE
// coords. Reuses lineGatherPolylines, so it serves polygon / polyline / rect / circle / profile /
// overlay alike. Columns are #, X, Y, Z. For the drawn shapes (LK_Polygon: polygon / polyline /
// rect / circle) the X/Y/Z cells are EDITABLE: committing a cell writes the new coordinate back to
// pg.v, keeps a closed ring's duplicated first/last vertex in sync, and rebuilds + re-renders the
// outline live. Profile/overlay lines are GMT-owned, so they stay read-only. The window is its own
// top-level (no parent), deletes itself on close, and does NOT block the viewer. A "Save…" button
// calls lineSavePoints(s, lr) directly — the exact function "Save line/polygon…" uses — so there
// is one save path, not a second one reimplemented here.
static void showLineDataTable(Scene *s, const LineRef& lr, const QString& name) {
	std::vector<std::vector<std::array<double,3>>> polylines;
	lineGatherPolylines(s, lr, polylines);
	if (polylines.empty()) return;

	// Editing writes back to a single polygon ring; only LK_Polygon has that 1:1 row<->pg.v mapping.
	const bool editable = (lr.kind == LK_Polygon && polylines.size() == 1);
	const std::vector<std::array<double,3>>& pl = polylines[0];
	const int nrows = (int)pl.size();
	vtkActor *actor = lr.actor;

	QDialog *dlg = new QDialog(nullptr);                  // top-level, parentless -> truly floating
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle(name.isEmpty() ? QString("Line data") : (name + " — data"));
	dlg->setWindowFlag(Qt::Window, true);
	QVBoxLayout *lay = new QVBoxLayout(dlg);

	// In flat-2D a DRAWN shape's (LK_Polygon: rectangle/polygon/circle) Z is genuinely meaningless --
	// it was drawn on the 2-D screen, z=0 by construction -- so drop that column there: #/X/Y (3) in
	// 2D, #/X/Y/Z (4) in 3D. An OVERLAY (LK_Overlay: a dropped/imported dataset, e.g. a SHAPENC point
	// cloud) is different: its Z came from the SOURCE data at import time regardless of how the
	// window happens to be displaying it right now, so hiding it in flat-2D would silently throw away
	// real input data the user explicitly asked this table to show -- always show all 3 for overlays.
	const bool showZ  = !s->flat2d || lr.kind == LK_Overlay;
	const int  ncoord = showZ ? 3 : 2;
	QStringList hdr;   hdr << "#" << "X" << "Y";   if (showZ) hdr << "Z";
	QTableWidget *tbl = new QTableWidget(nrows, ncoord + 1, dlg);
	tbl->setHorizontalHeaderLabels(hdr);
	tbl->verticalHeader()->setVisible(false);            // our "#" column already numbers the rows
	tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
	if (!editable) tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);

	for (int k = 0; k < nrows; ++k) {
		QTableWidgetItem *idx = new QTableWidgetItem(QString::number(k + 1));
		idx->setFlags(idx->flags() & ~Qt::ItemIsEditable);   // the "#" column is never editable
		tbl->setItem(k, 0, idx);
		for (int c = 0; c < ncoord; ++c) {
			QTableWidgetItem *it = new QTableWidgetItem(QString::number(pl[k][c], 'g', 10));
			if (!editable) it->setFlags(it->flags() & ~Qt::ItemIsEditable);
			tbl->setItem(k, c + 1, it);
		}
	}
	tbl->resizeColumnsToContents();
	lay->addWidget(tbl);

	QHBoxLayout *btnRow = new QHBoxLayout();
	btnRow->addStretch(1);
	QPushButton *saveBtn = new QPushButton("Save…", dlg);
	btnRow->addWidget(saveBtn);
	lay->addLayout(btnRow);
	QObject::connect(saveBtn, &QPushButton::clicked, dlg, [s, lr]() { lineSavePoints(s, lr); });
	dlg->resize(360, 450);

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

// ---- "Nested grids" menu actions (the special tsunami rectangle; see nesting_sizes.m) ----------

// Axis-aligned bbox of the polygon at index pi.
static void nestPolyBBox(Scene *s, int pi, double& x0, double& x1, double& y0, double& y1) {
	x0 = y0 = 1e300; x1 = y1 = -1e300;
	for (auto& v : s->polys[pi].v) { x0 = std::min(x0, v[0]); x1 = std::max(x1, v[0]);
	                                 y0 = std::min(y0, v[1]); y1 = std::max(y1, v[1]); }
}

// "Show nesting info": the COMCOT/NSWING -R/-I + start/end indices (1-based) for this nested rect.
static void nestShowInfo(Scene *s, vtkActor *a) {
	const int pi = polyIndexOfActor(s, a);
	if (pi < 0) return;
	const Polygon& pg = s->polys[pi];
	double x0, x1, y0, y1; nestPolyBBox(s, pi, x0, x1, y0, y1);
	const double xi = pg.nestXi, yi = pg.nestYi;
	const long ncols = (xi > 0) ? (long)std::lround((x1 - x0) / xi) + 1 : 0;
	const long nrows = (yi > 0) ? (long)std::lround((y1 - y0) / yi) + 1 : 0;
	QString t;
	t += QString("-R%1/%2/%3/%4 -I%5/%6\n")
	         .arg(x0, 0, 'g', 12).arg(x1, 0, 'g', 12).arg(y0, 0, 'g', 12).arg(y1, 0, 'g', 12)
	         .arg(xi, 0, 'g', 12).arg(yi, 0, 'g', 12);
	t += QString("nx = %1   ny = %2\n").arg(ncols).arg(nrows);
	t += QString("x_start = %1\nx_end = %2\ny_start = %3\ny_end = %4")
	         .arg(pg.nestIx0 + 1).arg(pg.nestIx1 + 1).arg(pg.nestIy0 + 1).arg(pg.nestIy1 + 1);
	showInfoText(s->win, "Nesting info", t);
}

// "Create blank grid": build a zero grid at this rect's limits/increments via the Julia/GMT host
// (the C++ viewer has no GMT) and add it to THIS SAME window as a HIDDEN extra surface — it shows
// up as an (unchecked) "Nested grid N" row in Scene Objects but is not drawn. The scene handle is
// embedded in the command so Julia adds to this exact window. Deferred a turn so the menu's event
// finishes first.
static void nestCreateBlankGrid(Scene *s, vtkActor *a) {
	const int pi = polyIndexOfActor(s, a);
	if (pi < 0) return;
	const Polygon& pg = s->polys[pi];
	double x0, x1, y0, y1; nestPolyBBox(s, pi, x0, x1, y0, y1);
	const double xi = pg.nestXi, yi = pg.nestYi;
	if (!(xi > 0 && yi > 0)) {
		if (s->win) s->win->statusBar()->showMessage("Nested grid has no cell size yet.", 4000);
		return;
	}
	if (!g_juliaEval) {
		QMessageBox::warning(s->win, "Create blank grid", "Creating a grid needs the Julia/GMT host.");
		return;
	}
	// Chain position (1-based) of THIS rect among the nested rectangles, so the grid is named to
	// follow the stack order: base grid first, then "layer1", "layer2", … inward.
	int chainIdx = 0;
	for (int j = 0; j <= pi; ++j) if (s->polys[j].nestKind == 1) ++chainIdx;
	auto num = [](double v) { return QString::number(v, 'g', 15); };
	const QString cmd = QString("InteractiveGMT._nested_blank_grid(Ptr{Cvoid}(UInt(%1)),%2,%3,%4,%5,%6,%7,%8,%9)")
	                        .arg((qulonglong)reinterpret_cast<uintptr_t>(s))
	                        .arg(num(x0)).arg(num(x1)).arg(num(y0)).arg(num(y1))
	                        .arg(num(xi)).arg(num(yi)).arg(s->hasCRS() ? "true" : "false").arg(chainIdx);
	QTimer::singleShot(0, s->win, [s, cmd]() {
		std::vector<char> buf(1 << 12);
		int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
		if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));
	});
}

// ---- Length / azimuth / area (CRS-aware, computed in Julia/GMT) --------------------------------
//
// Total vertex count across all of a line object's polylines.
static int lineVertexCount(Scene *s, const LineRef& lr) {
	std::vector<std::vector<std::array<double,3>>> pls;
	lineGatherPolylines(s, lr, pls);
	int n = 0; for (auto& pl : pls) n += (int)pl.size();
	return n;
}

// Total straight segments (sum of verts-1 per polyline). 1 = a single straight line -> singular
// menu labels + a single-number result; >1 = a polyline -> plural labels + a Data Viewer table.
static int lineSegmentCount(Scene *s, const LineRef& lr) {
	std::vector<std::vector<std::array<double,3>>> pls;
	lineGatherPolylines(s, lr, pls);
	int n = 0; for (auto& pl : pls) if (pl.size() >= 2) n += (int)pl.size() - 1;
	return n;
}

// Which objects get the length/azimuth + area properties: drawn polygons and imported overlay
// LINES, but ONLY when small (<=100 vertices). That single threshold excludes BOTH the coastlines
// (thousands of points) AND moderately large imported xy files — exactly the user's cut. The
// profile track is out (it has its own "Save with distance"); points/grids/images are not lines.
static bool lineMeasurable(Scene *s, const LineRef& lr) {
	if (lr.kind != LK_Overlay && lr.kind != LK_Polygon) return false;
	const int n = lineVertexCount(s, lr);
	return n >= 2 && n <= 100;
}

// A finished, closed polygon ring (>=4 vertices incl. the repeated first==last) — the only shape an
// "Area under polygon" makes sense for. Open polylines / rects-in-progress are excluded.
static bool lineClosedRing(Scene *s, const LineRef& lr) {
	if (lr.kind != LK_Polygon) return false;
	const int pi = polyIndexOfActor(s, lr.actor);
	return pi >= 0 && s->polys[pi].closed && s->polys[pi].v.size() >= 4;
}

// Hand the object's 2-D vertices to a Julia measure function over the in-process console bridge.
// `fn` is "_line_measure" (table -> Data Viewer) or "_poly_area" (number -> message box). The temp
// table, the scene handle and the window's proj4 go across; Julia decides geographic vs cartesian.
// Deferred a turn (like nestCreateBlankGrid) so the menu's own event finishes before the (possibly
// slow first-call) GMT compile runs. `box` shows the result in a dialog, else the status bar.
static void lineRunMeasure(Scene *s, const LineRef& lr, const char *fn, bool box) {
	if (!g_juliaEval) {
		QMessageBox::warning(s->win, "Measure", "This computation needs the Julia/GMT host.");
		return;
	}
	std::vector<std::vector<std::array<double,3>>> polylines;
	lineGatherPolylines(s, lr, polylines);
	if (polylines.empty()) return;
	const QString tmp = QDir::tempPath() + "/igmt_measure_" +
						QString::number(QDateTime::currentMSecsSinceEpoch()) + ".txt";
	if (!lineWriteTable(s, polylines, /*threeD=*/false, tmp)) return;   // 2-D x y, '>' multisegment
	const bool isPoly = (lr.kind == LK_Polygon);
	// Thread the Preferences Dist/Azim approximation + azimuth direction so the host picks the
	// matching engine (geod for Ellipsoidal/Spherical, mapproject -jf for Flat Earth; geodesicarea
	// vs gmtspatial -Q for areas). Defaults applied Julia-side if these are empty.
	const QString distAzimType = prefDistAzimType();   // capture for window title
	const QString cmd = QString("InteractiveGMT.%1(Ptr{Cvoid}(UInt(%2)),raw\"%3\",raw\"%4\",%5,raw\"%6\",raw\"%7\",raw\"%8\")")
							.arg(fn)
							.arg((qulonglong)reinterpret_cast<uintptr_t>(s))
							.arg(tmp)
							.arg(QString::fromStdString(s->crsProj4))
							.arg(isPoly ? "true" : "false")
							.arg(distAzimType)
							.arg(prefAzimDir())
							.arg(prefMeasureUnits());
	QTimer::singleShot(0, s->win, [s, cmd, box, distAzimType]() {
		std::vector<char> buf(1 << 14);
		int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
		const QString out = QString::fromUtf8(buf.data(), std::abs(n));
		if (n < 0) { sceneLogError(s, out); return; }              // Julia threw -> Errors tab
		if (box) {
			QMessageBox mb(QMessageBox::Information, QString("Result (%1)").arg(distAzimType), out, QMessageBox::Ok, s->win);
			mb.setTextInteractionFlags(Qt::TextSelectableByMouse);
			mb.exec();
		} else if (!out.isEmpty() && s->win) {
			s->win->statusBar()->showMessage(out, 6000);           // total length -> status bar
		}
	});
}

// "Extract profile…": sample the ACTIVE grid along this drawn line/polyline via GMT.grdtrack (the
// host owns GMT) and show the (distance, elevation) graph in the bottom-dock Profile panel — the
// grdtrack twin of the Ctrl+left-drag profiler. The line is densified in Julia by grid spacing, so
// even a two-vertex straight line yields a full-resolution profile. `_extract_profile` pushes the
// result back through gmtvtk_show_profile_xy; nothing is returned here. Deferred a turn (like the
// measurements) so the menu's own event finishes before the possibly-slow first grdtrack compile.
static void lineExtractProfile(Scene *s, const LineRef& lr) {
	if (!g_juliaEval) {
		QMessageBox::warning(s->win, "Extract profile", "This computation needs the Julia/GMT host.");
		return;
	}
	std::vector<std::vector<std::array<double,3>>> polylines;
	lineGatherPolylines(s, lr, polylines);
	if (polylines.empty()) return;
	const QString tmp = QDir::tempPath() + "/igmt_profile_" +
						QString::number(QDateTime::currentMSecsSinceEpoch()) + ".txt";
	if (!lineWriteTable(s, polylines, /*threeD=*/false, tmp)) return;   // 2-D x y, '>' multisegment
	const QString cmd = QString("InteractiveGMT._extract_profile(Ptr{Cvoid}(UInt(%1)),raw\"%2\",raw\"%3\",raw\"%4\",raw\"%5\")")
							.arg((qulonglong)reinterpret_cast<uintptr_t>(s))
							.arg(tmp)
							.arg(QString::fromStdString(s->crsProj4))
							.arg(prefDistAzimType())
							.arg(prefMeasureUnits());
	QTimer::singleShot(0, s->win, [s, cmd]() {
		std::vector<char> buf(1 << 12);
		int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
		if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));   // Julia threw -> Errors tab
	});
}

// "Transplant 2nd grid…": the rectangle-handle path of Grid Tools > Transplant 2nd grid (port of
// Mirone utils/transplants.m). The clicked rectangle/polygon's bounding box (W/E/S/N) clips the
// implant grid before it is implanted into THIS window's host grid — the "connection to rectangle
// handles". Same _on_transplant as the Grid Tools menu (which passes an empty rect), keep-host-res.
static void rectTransplant(Scene *s, const LineRef& lr) {
	if (!g_juliaEval) {
		QMessageBox::warning(s->win, "Transplant 2nd grid", "This computation needs the Julia/GMT host.");
		return;
	}
	std::vector<std::vector<std::array<double,3>>> polylines;
	lineGatherPolylines(s, lr, polylines);
	if (polylines.empty()) return;
	double w = 1e300, e = -1e300, so = 1e300, no = -1e300;                 // rectangle bounding box
	for (const auto& pl : polylines)
		for (const auto& p : pl) {
			w  = std::min(w,  p[0]);  e  = std::max(e,  p[0]);
			so = std::min(so, p[1]);  no = std::max(no, p[1]);
		}
	if (!(e > w && no > so)) return;
	const QString fn = QFileDialog::getOpenFileName(s->win, "Select grid to implant", prefStartDir(),
		"Grids (*.grd *.nc *.tif *.tiff *.img);;All files (*)");
	if (fn.isEmpty()) return;
	rememberStartDir(fn);
	const QString rect = QString("%1/%2/%3/%4").arg(w, 0, 'g', 16).arg(e, 0, 'g', 16)
	                                           .arg(so, 0, 'g', 16).arg(no, 0, 'g', 16);
	const QString cmd = QString("InteractiveGMT._on_transplant(Ptr{Cvoid}(UInt(%1)),raw\"%2\",1,\"%3\")")
							.arg((qulonglong)reinterpret_cast<uintptr_t>(s)).arg(fn).arg(rect);
	std::vector<char> buf(1 << 12);
	int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
	if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));         // Julia threw -> Errors tab
}

// Undo the last transplant (restores the original grid kept on the Julia side). Same action as Ctrl+Z.
static void rectTransplantUndo(Scene *s) {
	if (!g_juliaEval) return;
	const QString cmd = QString("InteractiveGMT._on_transplant_undo(Ptr{Cvoid}(UInt(%1)))")
							.arg((qulonglong)reinterpret_cast<uintptr_t>(s));
	std::vector<char> buf(1 << 12);
	int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
	if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));
}

// "Roi Crop Tools" (port of Mirone's draw_funs.m rectangle "ROI Crop Tools" submenu /
// mirone.m ImageCrop_CB): crop this rectangle's bounding box out of the window's PRIMARY grid or
// image and open the result as a NEW window. kind: "grid" | "image" (plain, no coord annotations,
// Mirone's bare `mirone(I)`) | "image_coords" (keeps geo axes, Mirone's `mirone(I,tmp)`). Only the
// three basic crops are ported for now — Mirone's fuller ROI toolset (stats/clip/fill-gaps/spectral…)
// is not.
static void rectRoiCrop(Scene *s, const LineRef& lr, const char *kind) {
	if (!g_juliaEval) {
		QMessageBox::warning(s->win, "Roi Crop Tools", "This computation needs the Julia/GMT host.");
		return;
	}
	std::vector<std::vector<std::array<double,3>>> polylines;
	lineGatherPolylines(s, lr, polylines);
	if (polylines.empty()) return;
	double w = 1e300, e = -1e300, so = 1e300, no = -1e300;                 // rectangle bounding box
	for (const auto& pl : polylines)
		for (const auto& p : pl) {
			w  = std::min(w,  p[0]);  e  = std::max(e,  p[0]);
			so = std::min(so, p[1]);  no = std::max(no, p[1]);
		}
	if (!(e > w && no > so)) return;
	const QString rect = QString("%1/%2/%3/%4").arg(w, 0, 'g', 16).arg(e, 0, 'g', 16)
	                                           .arg(so, 0, 'g', 16).arg(no, 0, 'g', 16);
	const QString cmd = QString("InteractiveGMT._on_roi_crop(Ptr{Cvoid}(UInt(%1)),\"%2\",\"%3\")")
							.arg((qulonglong)reinterpret_cast<uintptr_t>(s)).arg(kind).arg(rect);
	std::vector<char> buf(1 << 12);
	int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
	if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));         // Julia threw -> Errors tab
}

// Open the Vertical elastic deformation dialog for a fault line (defined in 70_window.cpp, after the
// ElasticDialog class — this fragment is #included before it, so forward-declare it here).
static void faultRunDialog(Scene *s, vtkActor *seedPatch = nullptr);

// The unified right-click menu for a line object: "Line properties…" plus the kind's own actions
// (profile: save / delete; overlay & polygon: hide; polygon: delete). Shared by the 3-D-view
// right-click hit-test and the Scene Objects list rows, so both routes give the same menu.
static void popupLineObjectMenu(Scene *s, const LineRef& lr, const QString& name, const QPoint& gp) {
	if (!s || !lr.actor) return;
	QMenu m(s->win);
	vtkActor *a = lr.actor;

	// Is this a "Nested grids" rectangle, a Draw-Fault line, or an Import-Model-Slip patch?
	bool isNestRect = false, isFault = false, isSlip = false;
	if (lr.kind == LK_Polygon) {
		const int pi = polyIndexOfActor(s, a);
		if (pi >= 0 && s->polys[pi].nestKind == 1) isNestRect = true;
		if (pi >= 0 && s->polys[pi].isFault)       isFault = true;
		if (pi >= 0 && s->polys[pi].isSlip)        isSlip = true;
	}
	// The matching Overlay entry (LK_Overlay only) -- used below both to gate "Line length…"/
	// "Azimuth…"/"Convert to points" off a SHAPENC boundary (isShapencBoundary) and to offer "Plot
	// interior points" when it carries a stashed swarm (interiorXYZ). One lookup, two uses.
	Overlay *ovp = nullptr;
	if (lr.kind == LK_Overlay)
		for (auto& o : s->overlays) if (o.actor.Get() == a) { ovp = &o; break; }

	// A fault line's (or slip patch's) first property is the elastic-deformation dialog. For a slip
	// patch the dialog opens seeded from THIS patch (faultRunDialog finds it via polyIndexOfActor) and
	// lists every patch of the model in its Faults combo.
	if (isFault || isSlip) {
		m.addAction("Vertical elastic deformation", [s, a]() { faultRunDialog(s, a); });
		m.addSeparator();
	}

	// EVERY nested rectangle carries the nesting actions at the TOP of the menu, so the chain can
	// be extended from any level (each "New nested grid" inherits its parent's nesting behaviour).
	if (isNestRect) {
		m.addAction("Show nesting info", [s, a]() { nestShowInfo(s, a); });
		m.addAction("Create blank grid", [s, a]() { nestCreateBlankGrid(s, a); });
		m.addAction("New nested grid",   [s]()    { nestNewChild(s); });
		m.addSeparator();
	}

	// Slip-model patches (Import Model Slip) have ONLY the elastic-deformation dialog — no line
	// properties, save, table, measurements, or delete. The GROUP carries the delete action.
	if (!isSlip) {
		m.addAction("Line properties…", [s, lr]() { showLineProperties(s, lr); });
		m.addAction(isNestRect ? "Save rectangle…"
				  : isFault    ? "Save trace fault…"
				  : (lr.kind == LK_Polygon && lineClosedRing(s, lr) ? "Save polygon…" : "Save line…"),
					[s, lr]() { lineSavePoints(s, lr); });   // 2D / 3D (grid-interpolated z)
		m.addAction("Show data table…",                                      // floating vertex table viewer
					[s, lr, name]() { showLineDataTable(s, lr, name); });
	}

	// CRS-aware measurements (length(s) + azimuth(s) for lines/polygons; area for closed polygons).
	// Gated to small objects so coastlines / large imports don't get them (lineMeasurable). The "(s)"
	// turns plural on a polyline (>1 segment); a polyline shows a table, a single line a number (!many).
	if (!isSlip && lineMeasurable(s, lr)) {
		if (lineClosedRing(s, lr)) {
			// Rectangles & generic polygons (closed rings): AREA only — NEVER line length / azimuth.
			// A "Nested grids" rectangle is excluded: it drives the nesting chain + its own blank grid,
			// so an area figure / transplant-into-host make no sense on it (the transplant lives on that
			// rect's "layerN" blank-grid row instead — see gridObjectMenu).
			if (!isNestRect) {
				m.addAction("Area under polygon…", [s, lr]() { lineRunMeasure(s, lr, "_poly_area", true); });
				// A REAL grid, not the fake 2x2 all-zero placeholder a vector-only empty-launcher
				// promote uses (see the "Extract profile…" comment above, same class of bug, same fix).
				const bool hasRealGrid = !s->gridZ.empty() && s->gnx > 2 && s->gny > 2;
				// On a grid: implant an external grid clipped to this rectangle (Grid Tools > Transplant).
				// "Undo transplant" shows ONLY while a transplant is applied+not-yet-undone (Julia keeps the
				// flag current via gmtvtk_set_transplant_undo); it disappears once undone here or via Ctrl+Z.
				if (!isSlip && hasRealGrid) {
					m.addAction("Transplant 2nd grid…", [s, lr]() { rectTransplant(s, lr); });
					if (s->transplantUndoAvail)
						m.addAction("Undo transplant", [s]() { rectTransplantUndo(s); });
				}
				// "Roi Crop Tools" (Mirone draw_funs.m item_tools): crop this rectangle out of the
				// window's primary grid and/or image into a new window.
				if (!isSlip && (hasRealGrid || sceneHasImage(s))) {
					QMenu *roi = m.addMenu("Roi Crop Tools");
					if (hasRealGrid)
						roi->addAction("Crop Grid", [s, lr]() { rectRoiCrop(s, lr, "grid"); });
					// Mirone (draw_funs.m:409-413) shows Crop Image / Crop Image (with coords) for a
					// GRID too, not only when a separate bitmap image is loaded — its own architecture
					// always has an underlying rendered image for a grid display. Match that: show
					// both whenever EITHER a grid or a real image is present, not gated on image alone.
					if (hasRealGrid || sceneHasImage(s)) {
						roi->addAction("Crop Image", [s, lr]() { rectRoiCrop(s, lr, "image"); });
						roi->addAction("Crop Image (with coords)", [s, lr]() { rectRoiCrop(s, lr, "image_coords"); });
					}
				}
			}
		} else {
			// Open lines / polylines only: length(s) + azimuth(s). Not for a SHAPENC boundary line
			// (user: a coverage OUT/IN boundary is not a thing you measure the length/azimuth of) --
			// Extract profile below keeps its OWN gate (a real grid may still legitimately underlie a
			// boundary added into an already-populated window), so this is scoped to just these two.
			if (!ovp || !ovp->isShapencBoundary) {
				const bool    many = lineSegmentCount(s, lr) > 1;
				const QString sfx  = many ? "s" : "";
				m.addAction(QString("Line length%1…").arg(sfx), [s, lr, many]() { lineRunMeasure(s, lr, "_line_length",  !many); });
				m.addAction(QString("Azimuth%1…").arg(sfx),     [s, lr, many]() { lineRunMeasure(s, lr, "_line_azimuth", !many); });
			}
			// Grid profile along the line (grdtrack) — only when this line actually sits on a REAL
			// grid, not the fake 2x2 all-zero placeholder every vector-only empty-launcher promote
			// uses to get real framed axes without real data (drop.jl's vector overlays, Background
			// region, the Coastlines/basemap tile scaffold -- all share the SAME zblank=zeros(2,2)
			// convention). `!s->gridZ.empty()` alone is true for that placeholder too (it's still a
			// non-empty 4-element vector), so a boundary polygon dropped with no grid underneath was
			// wrongly offering "Extract profile..." with nothing real to sample. A genuine grid is
			// never exactly 2x2 in practice, so gnx/gny > 2 is a safe, minimal-diff signal -- no new
			// C API parameter needed across the half-dozen legitimate promote call sites (IGRF, RTP,
			// clipped grids, Geography-derived grids, ...) that also flow through the SAME
			// gmtvtk_promote_surface_h with real, much finer data.
			if (!s->gridZ.empty() && s->gnx > 2 && s->gny > 2)
				m.addAction("Extract profile…", [s, lr]() { lineExtractProfile(s, lr); });
		}
	}
	m.addSeparator();
	if (lr.kind == LK_Profile) {
		m.addAction("Save profile (with distance)…", [s]() {
			if (!s->profPD || !s->profPD->GetPoints()) return;
			QString fn = QFileDialog::getSaveFileName(s->win, "Save profile", prefStartDir("profile.txt"),
													  "Text (*.txt *.dat);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			QFile f(fn);
			if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
			QTextStream out(&f);
			out << "# distance\tx\ty\tz\n";
			vtkPoints *pts = s->profPD->GetPoints();
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
		// Slip-model patches have no delete action; the GROUP carries it.
		if (!isSlip) {
			m.addAction(isNestRect ? "Delete rectangle" : isFault ? "Delete fault trace"
					  : lineClosedRing(s, lr) ? "Delete polygon" : "Delete line",
						[s, a]() { polygonDelete(s, a); });   // Hide = the Scene Objects checkbox
		}
	}
	else {                                               // overlay line (Coastlines, Boundaries, Rivers, imports)
		// Line <-> points toggle: a dropped x,y table draws as a polyline by default; this converts it
		// to a scatter of points (and back). Label reflects the CURRENT mode. Not for a SHAPENC
		// boundary (user: converting a coverage OUT/IN polygon to a point scatter makes no sense).
		if (!ovp || !ovp->isShapencBoundary) {
			const int omode = overlayMode(s, a);
			if (omode == 1)
				m.addAction("Convert to points", [s, a]() { overlaySetMode(s, a, 1); });
			else if (omode == 0)
				m.addAction("Convert to line",   [s, a]() { overlaySetMode(s, a, 0); });
		}
		// SHAPENC "bounded ensemble" OUT polygon (shapenc.jl/drop.jl): its hidden point swarm rides
		// along in Overlay::interiorXYZ instead of being added to the scene. One-shot -- once added,
		// the swarm is its OWN overlay with its own Scene Objects row/checkbox/Delete, so the item
		// just disappears rather than needing a second "hide interior points" state to track here.
		if (ovp && !ovp->interiorXYZ.empty() && !ovp->interiorAdded) {
			m.addAction("Plot interior points", [s, ovp]() {
				const int n = (int)ovp->interiorXYZ.size() / 3;
				if (n <= 0) return;
				const std::vector<int> pseg = { 0, n };
				// Same groupName as the OUT polygon itself -- stays under the SAME master file handle
				// (SACRED_LAW.md: "each file creates its own master handle", ALL of it, not just the
				// boundary rows), not a separate ungrouped row.
				const char *gn = ovp->groupName.empty() ? nullptr : ovp->groupName.c_str();
				addOverlay(s, ovp->interiorXYZ.data(), n, pseg.data(), 1, /*mode=*/0,
				           1.0, 0.0, 0.0, 0.0, 0.0, (ovp->name + " interior points").c_str(), gn,
				           /*info=*/nullptr, /*interiorXYZ=*/nullptr, /*nInterior=*/0,
				           /*isShapencBoundary=*/false, /*isShapencInteriorPoints=*/true);
				ovp->interiorAdded = true;
			});
		}
		// The points overlay "Plot interior points" itself created (its OWN row, not the OUT polygon):
		// "Quick grid" (gridding a scattered point cloud into a regular grid -- Auto estimates the
		// increment from the data spacing, Set increments… asks for it explicitly; NEITHER actually
		// grids yet, that's deferred) and "Point cloud view" (real 3-D: Z axis, colour-by-height,
		// rubber-band select -- InteractiveGMT.view_points, via a temp-file bridge since the point
		// count can be in the hundreds of thousands, too large to embed in a Julia eval string).
		if (ovp && ovp->isShapencInteriorPoints) {
			QMenu *qg = m.addMenu("Quick grid");
			qg->addAction("auto", [s]() {
				if (s->win) s->win->statusBar()->showMessage("Quick grid (auto): not implemented yet.", 4000);
			});
			qg->addAction("set increments", [s]() {
				QDialog dlg(s->win);
				dlg.setWindowTitle("Quick grid — increments");
				QFormLayout *fl = new QFormLayout(&dlg);
				QLineEdit *exI = new QLineEdit(&dlg);
				QLineEdit *eyI = new QLineEdit(&dlg);
				exI->setValidator(new QDoubleValidator(0.0, 1e30, 12, exI));
				eyI->setValidator(new QDoubleValidator(0.0, 1e30, 12, eyI));
				fl->addRow("Increment X:", exI);
				fl->addRow("Increment Y:", eyI);
				QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
				fl->addRow(bb);
				QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
				QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
				if (dlg.exec() == QDialog::Accepted && s->win)   // OK is the ONE action button -- gridding itself not implemented yet
					s->win->statusBar()->showMessage("Quick grid (increments): not implemented yet.", 4000);
			});
			m.addAction("Point cloud view", [s, ovp]() {
				// ALL in-process, no file (project rule: never write a temp file, no exceptions).
				// Julia calls gmtvtk_overlay_points_h back to fill its OWN buffer directly -- the
				// actor pointer + point count are embedded in the eval command exactly like
				// nestCreateBlankGrid's scene handle above, just two pointers instead of one.
				if (!ovp->baseLine || !ovp->baseLine->GetPoints()) return;
				const int n = (int)ovp->baseLine->GetPoints()->GetNumberOfPoints();
				if (n <= 0) return;
				if (!g_juliaEval) {
					QMessageBox::warning(s->win, "Point cloud view", "Needs the Julia/GMT host.");
					return;
				}
				vtkActor *actor = ovp->actor.Get();
				const QString cmd = QString("InteractiveGMT._on_pointcloud_view(Ptr{Cvoid}(UInt(%1)),Ptr{Cvoid}(UInt(%2)),%3,%4)")
				                        .arg((qulonglong)reinterpret_cast<uintptr_t>(s))
				                        .arg((qulonglong)reinterpret_cast<uintptr_t>(actor))
				                        .arg(n)
				                        .arg(s->hasCRS() ? "true" : "false");
				QTimer::singleShot(0, s->win, [s, cmd]() {
					std::vector<char> buf(1 << 12);
					int n2 = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
					if (n2 < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n2));
				});
			});
		}
		m.addAction(QString("Delete %1").arg(name),       // hide = the Scene Objects checkbox; this DELETES
					[s, a]() { overlayDelete(s, a); });
	}
	// Shared vector-pile draw-order: order this line/polygon against ALL other vector elements
	// (overlays + symbols + polygons). Not for the singleton profile track, nor for nested-grid
	// rectangles (the GRIDS carry the stacking, not their defining rectangles).
	int *stackPtr = nullptr;
	if (lr.kind == LK_Overlay)      { for (auto& o  : s->overlays) if (o.actor.Get() == a) { stackPtr = &o.stack;  break; } }
	else if (lr.kind == LK_Polygon && !isNestRect && !isSlip) { for (auto& pg : s->polys) if (pg.line.Get() == a) { stackPtr = &pg.stack; break; } }
	// Draw-order now spans ONE unified pile (base relief + grids + every vector), so a fault drawn on a
	// grid can be ordered above/below that grid even when it is the only vector. Show the actions whenever
	// there are 2+ stackable elements to reorder against.
	const size_t nVec = gatherStackItems(s).size();
	if (stackPtr && nVec > 1) {
		m.addSeparator();
		auto reRender = [s]() { if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render(); };
		QMenu *stackMenu = m.addMenu("Stack order");
		stackMenu->addAction("Place on top",    [s, stackPtr, reRender]() { restackVector(s, stackPtr, 0); reRender(); });
		stackMenu->addAction("Place at bottom", [s, stackPtr, reRender]() { restackVector(s, stackPtr, 1); reRender(); });
		stackMenu->addAction("Bring forward",   [s, stackPtr, reRender]() { restackVector(s, stackPtr, 2); reRender(); });
		stackMenu->addAction("Send backward",   [s, stackPtr, reRender]() { restackVector(s, stackPtr, 3); reRender(); });
	}
	(void)name;
	m.exec(gp);
}
