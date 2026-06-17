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
	QVBoxLayout* col = new QVBoxLayout(s->objPanel);
	col->setContentsMargins(8, 8, 8, 8);
	col->setSpacing(4);

	auto addRow = [&](const QString& label, vtkProp3D* a) {   // vtkActor upcasts; surface row passes the tile assembly
		if (!a)
			return;
		QCheckBox* cb = new QCheckBox(label, s->objPanel);
		cb->setChecked(a->GetVisibility() != 0);
		QObject::connect(cb, &QCheckBox::toggled, [s, a](bool on) {
			a->SetVisibility(on ? 1 : 0);
			if (s->widget && s->widget->renderWindow())
				s->widget->renderWindow()->Render();
		});
		col->addWidget(cb);
	};

	// A bare image has NO surface: don't list one, and the image is the object itself (not an
	// "Image drape" over a surface).
	if (!s->imageOnly)
		addRow(s->surfName.empty() ? QString("Surface") : QString::fromStdString(s->surfName), surfProp(s));
	if (s->drape)
		addRow(s->imageOnly ? (s->surfName.empty() ? QString("Image") : QString::fromStdString(s->surfName))
		                    : QString("Image drape"),
		       s->drape);
	for (auto& ov : s->overlays)
		addRow(QString::fromStdString(ov.name), ov.actor);
	for (auto& cu : s->curtains)
		addRow(QString::fromStdString(cu.name), cu.actor);
	for (auto& ex : s->extras) {                 // grids/images dropped in after the window opened
		addRow(QString::fromStdString(ex.name), ex.actor);
		if (ex.drape) addRow(QString::fromStdString(ex.name + " (image)"), ex.drape);
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

// Right-click menu for the profile track line: colour, width, line style (solid/dashed/
// dotted via the same stipple-texture path as overlays), save to a text file, delete.
static void popupProfileMenu(Scene* s, const QPoint& globalPos) {
	if (!s || !s->profLine) return;
	QWidget* win = s->win;
	vtkActor* a = s->profLine;
	QMenu m(win);

	m.addAction("Line color…", [=]() {
		double c[3]; a->GetProperty()->GetColor(c);
		QColor q = QColorDialog::getColor(QColor(int(c[0]*255), int(c[1]*255), int(c[2]*255)),
										  win, "Profile line color");
		if (!q.isValid()) return;
		a->GetProperty()->SetColor(q.redF(), q.greenF(), q.blueF());
		if (s->profStyle != 0) {                          // colour lives in the stipple texture
			s->profStripe = makeStripeTex(s->profStyle == 1 ? 0.5 : 0.18, q.redF(), q.greenF(), q.blueF());
			a->SetTexture(s->profStripe);
		}
		s->widget->renderWindow()->Render();
	});

	m.addAction("Line width…", [=]() {
		bool ok = false;
		double w = QInputDialog::getDouble(win, "Line width", "width (px):",
			a->GetProperty()->GetLineWidth(), 0.5, 40.0, 1, &ok);
		if (ok) {
			a->GetProperty()->SetLineWidth(w);
			s->widget->renderWindow()->Render();
		}
	});

	auto restyle = [s, a](int style) {
		if (!s->profPD) return;
		s->profStyle = style;
		if (style == 0) {                                 // solid: drop the texture + tcoords
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
			a->GetProperty()->SetOpacity(0.999);          // force translucent pass so alpha gaps show
		}
		if (auto* mm = vtkPolyDataMapper::SafeDownCast(a->GetMapper())) mm->Modified();
		s->widget->renderWindow()->Render();
	};
	QMenu* st = m.addMenu("Line style");
	st->addAction("Solid",  [=]() { restyle(0); });
	st->addAction("Dashed", [=]() { restyle(1); });
	st->addAction("Dotted", [=]() { restyle(2); });

	m.addSeparator();
	m.addAction("Save profile…", [=]() {
		if (!s->profPD || !s->profPD->GetPoints()) return;
		QString fn = QFileDialog::getSaveFileName(win, "Save profile", "profile.txt",
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
	m.addAction("Delete profile", [=]() { profileClear(s); });
	m.exec(globalPos);
}

// QVTKOpenGLNativeWidget forwards left/right/wheel to the VTK interactor, but VTK's adapter
// never delivers the MIDDLE button to our observers (both MiddleCB and the old event-filter
// were dead for that reason). Qt DOES deliver mouse events to the widget itself (the
// right-click context menu proves it), so handle the middle button HERE in the widget's own
// Qt handlers: middle-click with no drag recenters the view on the surface point under the
// cursor (identical math to the working `c` hotkey); middle-drag pans. Left/right/wheel fall
// through to the base class unchanged, so VTK interaction + the context menu still work.
