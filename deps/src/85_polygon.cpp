// ============================================================================
//  Draw / edit tools (five icon-only toolbar buttons routed through one engine).
//
//  Polygon, polyline, rectangle and circle all finalize into a `Polygon` (a vertex
//  ring; polyline is the only OPEN one) and so share the preview / edit / delete /
//  Scene-Objects / Line-Properties code below. The active tool is Scene::polyShape,
//  set by polygonToolToggled when a button is checked. Text places a billboard label.
//
//  Polygon / polyline DRAW MODE: left-click adds a vertex, right-click removes the
//  last one, double-left-click closes the polygon (>=3) or ends the open polyline
//  (>=2). Rectangle / circle: two clicks (corner+corner / centre+edge). Text: one
//  click on the scene, then a dialog. A live rubber preview trails the cursor.
//
//  EDIT MODE (button unchecked): double-click ON a finished polygon shows square
//  handles at its vertices; click-drag a handle to move that vertex. Double-click
//  off any handle leaves edit mode.
//
//  Vertices are picked ON the scene (grid heightfield / image plane / FV-point
//  cellpicker) so they carry true elevation -> the polygon is 3-D over a grid.
//  They are stored in TRUE coords; the actors hang in the surface's scaled space
//  (xfac, 1, zfac*ve) via applyVE, so the polygon tracks vertical exaggeration.
//  Mouse observers sit at priority 20 (above the gizmo's 10) and abort the event
//  only while they actually handle it, leaving normal navigation untouched when idle.
// ============================================================================

// Crisp toolbar-icon canvas: a `logical`-px transparent pixmap supersampled `dpr`x, so each glyph
// is rasterised at high resolution then downscaled by Qt -> no fuzz on any display DPI. Because the
// pixmap carries the devicePixelRatio, ALL painting (coordinates AND pen widths) is in logical
// units 0..logical; LOGICAL is the on-screen size. Mirrors makeObjectIcon's supersampling trick.
static QPixmap iconCanvas(int logical = 24, qreal dpr = 4.0) {
	QPixmap pm(int(logical * dpr), int(logical * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	return pm;
}

// Pentagon outline icon for the toolbar button (drawn, no asset file).
static QIcon makePolygonIcon() {
	// An irregular closed ring with vertex handles (matches the Scene Objects polygon icon). A
	// regular pentagon wrongly implied "draw a pentagon" — this reads as "draw an arbitrary polygon".
	QPixmap pm = iconCanvas();
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	QPolygonF poly; poly << QPointF(5, 7) << QPointF(19, 6) << QPointF(21, 16)
	                     << QPointF(12, 21) << QPointF(3, 16);
	p.setPen(QPen(QColor(40, 40, 40), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(QColor(255, 200, 120, 150));
	p.drawPolygon(poly);
	p.setPen(Qt::NoPen); p.setBrush(QColor(190, 110, 30));   // vertex handles
	for (const QPointF &q : poly) p.drawEllipse(q, 2.0, 2.0);
	p.end();
	return QIcon(pm);
}

// Open zig-zag polyline icon (no fill). Thin, round-capped/joined so the supersampled stroke
// reads as a clean sharp line rather than a thick blob.
static QIcon makePolylineIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	QPolygonF line; line << QPointF(3, 18) << QPointF(9, 6) << QPointF(15, 16) << QPointF(21, 5);
	p.drawPolyline(line);
	p.end(); return QIcon(pm);
}

// Straight two-point line icon (no fill) with an endpoint handle at each end.
static QIcon makeLineIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	const QPointF a(4, 19), b(20, 5);
	p.drawLine(a, b);
	p.setPen(Qt::NoPen); p.setBrush(QColor(190, 110, 30));
	p.drawEllipse(a, 2.0, 2.0); p.drawEllipse(b, 2.0, 2.0);
	p.end(); return QIcon(pm);
}

// Square outline icon (light fill).
static QIcon makeRectIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(QColor(255, 200, 120));
	p.drawRect(4, 5, 16, 14);
	p.end(); return QIcon(pm);
}

// Nested-rectangle icon: three concentric thin outlines (no fill), for the "special" rectangle whose
// dimensions are constrained / governed by its context menus (props wired later, see MATLAB ref). Thin
// round-joined pens so all three rings stay visible at the small toolbar size.
static QIcon makeNestedRectIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.1, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
	p.setBrush(Qt::NoBrush);
	p.drawRect(QRectF(2.5, 4.0, 19.0, 16.0));    // outer
	p.drawRect(QRectF(5.5, 7.0, 13.0, 10.0));    // middle
	p.drawRect(QRectF(8.5, 10.0,  7.0,  4.0));   // inner
	p.end(); return QIcon(pm);
}

// Circle outline icon (light fill).
static QIcon makeCircleIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.6)); p.setBrush(QColor(255, 200, 120));
	p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
	p.end(); return QIcon(pm);
}

// Symbols flyout icons: a SMALL marker centred on the canvas (unlike the big shape-tool icons
// above) — these place a native screen-constant-size glyph (SymbolLayer), not a drawn shape, so
// the icon must read as "drop a small marker", not "draw a circle/square". Radii are deliberately
// a fraction of the shape-tool icons' (~30% canvas fill vs ~75%).
static QIcon makeSymCircleIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.2)); p.setBrush(QColor(255, 200, 120));
	p.drawEllipse(QPointF(12, 12), 4.2, 4.2);
	p.end(); return QIcon(pm);
}
static QIcon makeSymSquareIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.2)); p.setBrush(QColor(255, 200, 120));
	p.drawRect(QRectF(8.2, 8.2, 7.6, 7.6));
	p.end(); return QIcon(pm);
}
static QIcon makeSymStarIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	QPolygonF star;
	const QPointF c(12, 12);
	for (int k = 0; k < 10; ++k) {
		const double r = (k % 2 == 0) ? 4.8 : 2.1;
		const double a = -vtkMath::Pi()/2.0 + k * vtkMath::Pi()/5.0;
		star << QPointF(c.x() + r*std::cos(a), c.y() + r*std::sin(a));
	}
	p.setPen(QPen(QColor(40, 40, 40), 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(QColor(255, 200, 120));
	p.drawPolygon(star);
	p.end(); return QIcon(pm);
}

// Text-tool icon: a stylised serif "T". A serif face (Georgia, fallback Times) gives the glyph
// real bracketed serifs at the foot + arm ends, so it reads as a LETTER T — not the plain-bar
// cross the geometric version looked like. Rendered as an actual glyph path, supersampled (dpr 4
// via iconCanvas) and text-antialiased so it stays sharp at the small toolbar size.
static QIcon makeTextIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::TextAntialiasing, true);
	QFont f("Georgia"); f.setStyleHint(QFont::Serif); f.setBold(true); f.setPointSizeF(16.0);
	p.setFont(f);
	p.setPen(QColor(35, 35, 40));
	p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, "T");
	p.end(); return QIcon(pm);
}

// Info-tool icon: a stylised lowercase serif 'i' in a soft rounded blue badge — the universal
// "information" glyph for the grdinfo / gdalinfo flyout. Dot (tittle) + stem in white for contrast.
static QIcon makeInfoIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(45, 110, 190));
	p.drawRoundedRect(QRectF(3, 3, 18, 18), 5, 5);            // rounded badge backdrop
	p.setBrush(Qt::white);
	p.drawEllipse(QPointF(12, 8.5), 1.7, 1.7);               // tittle (the dot)
	p.drawRoundedRect(QRectF(10.5, 11, 3, 7), 1.2, 1.2);     // stem
	p.end(); return QIcon(pm);
}

// Swipe-tool icon: a framed tile split down the middle — dark raster left, light raster right —
// with the divider line and its round grab handle drawn exactly as the live overlay draws them, so
// the button reads as a miniature of what the tool puts on the scene.
static QIcon makeSwipeIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	const QRectF box(3, 4, 18, 16);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(70, 95, 125));                     // left half: the "before" raster
	p.drawRect(QRectF(box.left(), box.top(), box.width() * 0.5, box.height()));
	p.setBrush(QColor(215, 205, 180));                   // right half: the "after" raster
	p.drawRect(QRectF(box.center().x(), box.top(), box.width() * 0.5, box.height()));
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(45, 45, 50), 1.2));
	p.drawRect(box);
	p.setPen(QPen(QColor(250, 250, 250), 1.4));          // the divider
	p.drawLine(QPointF(box.center().x(), box.top()), QPointF(box.center().x(), box.bottom()));
	p.setPen(QPen(QColor(45, 45, 50), 1.0));
	p.setBrush(QColor(250, 250, 250));
	p.drawEllipse(box.center(), 3.6, 3.6);               // the grab handle
	p.end(); return QIcon(pm);
}

// Link-tool icon: two small windows side by side joined by a chain link — the Swipe icon's sibling
// on the SAME toolbar slot (its dropdown picks between the two). Reads as "these two are paired"
// rather than Swipe's "one tile split in half".
static QIcon makeLinkIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	auto winGlyph = [&](double x) {
		QRectF r(x, 5, 8, 12);
		p.setPen(QPen(QColor(45, 45, 50), 1.2));
		p.setBrush(QColor(215, 225, 235));
		p.drawRoundedRect(r, 1.5, 1.5);
		p.setBrush(QColor(70, 95, 125));                  // titlebar strip
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(QRectF(r.left(), r.top(), r.width(), 3.0), 1.5, 1.5);
	};
	winGlyph(2.0);
	winGlyph(14.0);
	// The chain link between them: two small overlapping rings.
	p.setPen(QPen(QColor(230, 175, 40), 1.8));
	p.setBrush(Qt::NoBrush);
	p.drawEllipse(QPointF(10.6, 13.0), 2.1, 2.1);
	p.drawEllipse(QPointF(13.4, 13.0), 2.1, 2.1);
	p.end(); return QIcon(pm);
}

// ── 3-D Bodies flyout icons (cube / sphere / torus / cylinder + a generic polyhedron) ──────
// Small isometric glyphs for the "3-D Bodies" toolbar flyout. Each is a stylised wireframe of the
// GMT solid the entry builds; the generic polyhedron stands in for the platonic solids and the
// parametric generators (label disambiguates). Same supersampled iconCanvas trick as the others.

// Cube: front face + top/right parallelograms (simple isometric box).
static QIcon makeCubeIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	const QPointF a(5, 9), b(14, 6), c(20, 9), d(11, 12);          // top face (a-b-c-d)
	QPolygonF top; top << a << b << c << d;
	p.setBrush(QColor(255, 215, 140)); p.drawPolygon(top);
	QPolygonF front; front << a << d << QPointF(11, 21) << QPointF(5, 18);   // left/front face
	p.setBrush(QColor(235, 180, 95));  p.drawPolygon(front);
	QPolygonF right; right << d << c << QPointF(20, 18) << QPointF(11, 21);  // right face
	p.setBrush(QColor(210, 150, 70));  p.drawPolygon(right);
	p.end(); return QIcon(pm);
}

// Sphere: filled circle + equator/meridian ellipses for a 3-D read.
static QIcon makeSphereIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.5)); p.setBrush(QColor(150, 195, 245));
	p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
	p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor(40, 40, 40), 1.0));
	p.drawEllipse(QPointF(12, 12), 8.5, 3.4);                      // equator
	p.drawEllipse(QPointF(12, 12), 3.4, 8.5);                      // meridian
	p.end(); return QIcon(pm);
}

// Torus: outer + inner ellipse (a donut seen at a slight tilt).
static QIcon makeTorusIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.5)); p.setBrush(QColor(245, 165, 120));
	p.drawEllipse(QPointF(12, 12), 9.0, 5.5);                      // outer rim
	p.setBrush(QColor(255, 255, 255, 0));
	p.drawEllipse(QPointF(12, 12), 3.6, 2.2);                      // hole
	p.end(); return QIcon(pm);
}

// Cylinder: side rectangle capped by top/bottom ellipses.
static QIcon makeCylinderIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(QColor(180, 210, 160));
	p.drawRect(QRectF(5, 6, 14, 12));                             // body
	p.drawEllipse(QPointF(12, 18), 7.0, 2.6);                     // bottom cap
	p.setBrush(QColor(205, 230, 185));
	p.drawEllipse(QPointF(12, 6), 7.0, 2.6);                      // top cap
	p.end(); return QIcon(pm);
}

// Generic polyhedron (icosahedron-ish): hexagon outline + spokes. Used for the platonic solids and
// the parametric generators in the flyout — the action label tells them apart.
static QIcon makePolyhedronIcon() {
	QPixmap pm = iconCanvas();
	QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	QPolygonF hex;
	const QPointF ctr(12, 12);
	for (int k = 0; k < 6; ++k) {
		double th = M_PI / 6.0 + k * M_PI / 3.0;
		hex << QPointF(ctr.x() + 9.0 * std::cos(th), ctr.y() + 9.0 * std::sin(th));
	}
	p.setBrush(QColor(200, 185, 235)); p.drawPolygon(hex);
	p.setBrush(Qt::NoBrush);
	for (int k = 0; k < 6; k += 2) p.drawLine(ctr, hex[k]);        // a few inner edges
	p.end(); return QIcon(pm);
}

// "2D"/"3D" glyph for the icon-only view-toggle button (twoD -> show "2D", else "3D").
static QIcon makeViewModeIcon(bool twoD) {
	QPixmap pm = iconCanvas();
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::TextAntialiasing, true);
	QFont f = p.font(); f.setBold(true); f.setPointSizeF(9.5); p.setFont(f);
	p.setPen(QColor(35, 35, 40));
	p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, twoD ? "2D" : "3D");
	p.end(); return QIcon(pm);
}

// Cursor (mx,my device px) -> a point ON the scene, returned in TRUE coords. Mirrors the hover
// ray-cast in onMouseMove: march the unprojected ray against the grid heightfield (sampleZ), the
// flat image plane, or the FV/point cellpicker. Returns false if the ray misses the scene.
static bool polyPickWorld(Scene *s, int mx, int my, double outTrue[3]) {
	double nr[4], fr[4];
	s->ren->SetDisplayPoint((double)mx, (double)my, 0.0); s->ren->DisplayToWorld();
	for (int i = 0; i < 4; ++i) nr[i] = s->ren->GetWorldPoint()[i];
	s->ren->SetDisplayPoint((double)mx, (double)my, 1.0); s->ren->DisplayToWorld();
	for (int i = 0; i < 4; ++i) fr[i] = s->ren->GetWorldPoint()[i];
	if (nr[3] != 0.0) { nr[0] /= nr[3]; nr[1] /= nr[3]; nr[2] /= nr[3]; }
	if (fr[3] != 0.0) { fr[0] /= fr[3]; fr[1] /= fr[3]; fr[2] /= fr[3]; }
	const double dirx = fr[0] - nr[0], diry = fr[1] - nr[1], dirz = fr[2] - nr[2];
	const double zsc = s->zfac * s->ve;
	const double gx  = (s->xfac != 0.0) ? s->xfac : 1.0;

	if (!s->gridZ.empty()) {
		// A NaN is a DATA HOLE, and a hole still has a surface to click on: the flat NaN backdrop the
		// renderer draws just under the grid floor (nanPlaneUpdate). Sampling NaN used to ABORT the
		// ray-march ("no surface here"), so the surface the user could plainly see in the NaN fill
		// colour refused every vertex -- the renderer and the picker disagreeing about what a hole is.
		// They agree now: a hole is the floor.
		auto eval = [&](double t, double &fval) -> bool {
			const double X = nr[0] + t*dirx, Y = nr[1] + t*diry, Z = nr[2] + t*dirz;
			double h = sampleZ(s, X / gx, Y);
			if (std::isnan(h)) h = s->zmin;         // the hole's backdrop sits at the grid floor
			fval = Z - h * zsc; return true;
		};
		const int NS = 512;
		double pt = 0.0, pf = 0.0; bool have = false;
		for (int k = 0; k <= NS; ++k) {
			const double t = (double)k / NS; double fv;
			if (!eval(t, fv)) { have = false; continue; }
			if (have && ((pf <= 0.0 && fv >= 0.0) || (pf >= 0.0 && fv <= 0.0))) {
				double a = pt, b = t, fa = pf;
				for (int it = 0; it < 40; ++it) {
					const double m = 0.5*(a+b); double fm;
					if (!eval(m, fm)) break;
					if ((fa <= 0.0 && fm <= 0.0) || (fa >= 0.0 && fm >= 0.0)) { a = m; fa = fm; }
					else b = m;
				}
				const double t0 = 0.5*(a+b);
				const double wx = nr[0] + t0*dirx, wy = nr[1] + t0*diry;
				outTrue[0] = wx / gx; outTrue[1] = wy;
				outTrue[2] = sampleZ(s, wx / gx, wy);
				// In a hole the vertex sits ON the hole's backdrop (the grid floor), not at z=0 --
				// same surface the ray was just intersected against, so the point lands where clicked.
				if (std::isnan(outTrue[2])) outTrue[2] = s->zmin;
				return true;
			}
			pt = t; pf = fv; have = true;
		}
		return false;
	}
	if (s->imageOnly) {
		if (dirz != 0.0) {
			const double t0 = -nr[2] / dirz;
			if (t0 >= 0.0 && t0 <= 1.0) {
				outTrue[0] = (nr[0] + t0*dirx) / gx; outTrue[1] = nr[1] + t0*diry; outTrue[2] = 0.0;
				return true;
			}
		}
		return false;
	}
	if (s->picker) {
		if (s->picker->Pick((double)mx, (double)my, 0.0, s->ren) && s->picker->GetCellId() >= 0) {
			double w[3]; s->picker->GetPickPosition(w);
			outTrue[0] = w[0] / gx; outTrue[1] = w[1]; outTrue[2] = (zsc != 0.0) ? w[2] / zsc : 0.0;
			return true;
		}
	}
	return false;
}

// TRUE-coord vertex -> display px (device, bottom-up; matches GetEventPosition), using the same
// scaled space the actors live in. For hit-testing handles / polygon edges against a click.
static void polyToDisplay(Scene *s, const std::array<double,3> &v, double d[2]) {
	s->ren->SetWorldPoint(v[0]*s->xfac, v[1], v[2]*s->zfac*s->ve, 1.0);
	s->ren->WorldToDisplay();
	const double *dp = s->ren->GetDisplayPoint();
	d[0] = dp[0]; d[1] = dp[1];
}

// Common look for a 3-D polyline / preview actor sitting above the relief.
static vtkSmartPointer<vtkActor> polyMakeLineActor(Scene *s, vtkPolyData *pd, double r, double g, double b) {
	vtkNew<vtkPolyDataMapper> map; map->SetInputData(pd); map->ScalarVisibilityOff();
	vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
	map->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, -66000.0);   // lift the line above the surface
	auto a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(map);
	a->GetProperty()->SetColor(r, g, b);
	a->GetProperty()->SetLineWidth(2.5);
	a->GetProperty()->LightingOff();
	a->PickableOff();
	a->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	return a;
}

// Fill a polydata with a polyline through `verts`; closed adds the return edge to vertex 0.
static void polyFillLine(vtkPolyData *pd, const std::vector<std::array<double,3>> &verts, bool closed) {
	vtkNew<vtkPoints> pts;
	for (auto &v : verts) pts->InsertNextPoint(v[0], v[1], v[2]);
	vtkNew<vtkCellArray> lines;
	const vtkIdType n = (vtkIdType)verts.size();
	if (n >= 2) {
		vtkNew<vtkIdList> ids;
		for (vtkIdType i = 0; i < n; ++i) ids->InsertNextId(i);
		if (closed) ids->InsertNextId(0);
		lines->InsertNextCell(ids);
	}
	pd->SetPoints(pts);
	pd->SetLines(lines);
	pd->Modified();
}

// Drape a corner list onto the relief: each edge is densified (one sub-point per grid node along
// it) with z sampled from the full-res heightfield (sampleZ), so the edge HUGS the terrain instead
// of cutting a straight chord between corners. Off a grid (image / FV / point cloud there's no
// heightfield) it falls back to straight segments (linear z). Corner zs are kept as-is.
static void polyDrapeCorners(Scene *s, const std::vector<std::array<double,3>> &corners,
							 std::vector<std::array<double,3>> &out) {
	out.clear();
	const int m = (int)corners.size();
	if (m == 0) return;
	const bool canDrape = !s->gridZ.empty();
	double spacing = 0.0;
	if (canDrape) {
		const double sx = std::abs(s->gdx), sy = std::abs(s->gdy);
		spacing = (sx > 0 && sy > 0) ? std::min(sx, sy) : std::max(sx, sy);
	}
	for (int i = 0; i < m - 1; ++i) {
		const auto &a = corners[i];
		const auto &b = corners[i + 1];
		const double dx = b[0] - a[0], dy = b[1] - a[1];
		const double dist = std::hypot(dx, dy);
		int nsub = 1;
		if (canDrape && spacing > 0.0 && dist > 0.0)
			nsub = std::clamp((int)std::ceil(dist / spacing), 1, 4000);
		for (int k = 0; k < nsub; ++k) {            // push a..(b exclusive); next edge pushes b
			const double t = (double)k / nsub;
			const double x = a[0] + t * dx, y = a[1] + t * dy;
			double z = a[2] + t * (b[2] - a[2]);    // straight-segment fallback
			if (canDrape) { const double h = sampleZ(s, x, y); if (!std::isnan(h)) z = h; }
			out.push_back({ x, y, z });
		}
	}
	auto last = corners[m - 1];                     // the final corner — drape it too (else it floats
	if (canDrape) { const double h = sampleZ(s, last[0], last[1]); if (!std::isnan(h)) last[2] = h; }
	out.push_back(last);                            // at the previous corner's z, off the ground)
}

// Rebuild the finished-polygon actor `pg` from its vertices. pg.v is a closed ring (first == last);
// each edge is draped on the relief so the outline hugs the terrain.
// Rebuild the filled FACE of a closed polygon/rectangle from its draped ring. The fill colour and
// opacity live on pg (fillColor/fillOpacity) INDEPENDENT of the outline; default opacity 0 keeps the
// historic outline-only look until the user dials up transparency in Line Properties. Open polylines
// have no fill (hidden). The face sits just below the outline (polygon offset) but above the surface.
static void polyRebuildFill(Scene *s, Polygon &pg) {
	if (!pg.closed) {                                  // only closed rings can carry a fill
		if (pg.fill) pg.fill->VisibilityOff();
		return;
	}
	if (!pg.fillPD) pg.fillPD = vtkSmartPointer<vtkPolyData>::New();

	// Boundary = the drawn corners (drop pg.v's closing duplicate). Densified edge points are NOT used
	// for the fill: triangulating the 3-D draped, non-planar boundary makes vtkPolygon project to a
	// best-fit plane where the wiggly ring self-intersects -> garbage triangulation (the half-filled
	// "bowtie"). Need >=3 corners for a face.
	std::vector<std::array<double,3>> ring = pg.v;
	if (ring.size() >= 2 && ring.front() == ring.back()) ring.pop_back();
	if ((int)ring.size() < 3) { if (pg.fill) pg.fill->VisibilityOff(); return; }

	const bool canDrape = !s->gridZ.empty();
	double spacing = 0.0;
	if (canDrape) { const double sx = std::abs(s->gdx), sy = std::abs(s->gdy);
		spacing = (sx > 0 && sy > 0) ? std::min(sx, sy) : std::max(sx, sy); }
	auto drapeZ = [&](double x, double y) -> double {
		if (canDrape) { const double h = sampleZ(s, x, y); if (!std::isnan(h)) return h; }
		return 0.0;
	};

	// 1) Triangulate the polygon PLANAR in XY (robust; correct shape, handles concave). 2) Subdivide
	//    each base triangle into a barycentric grid at grid resolution and DRAPE every node onto the
	//    relief, so the filled face hugs the terrain instead of a flat chord that the hills poke
	//    through (which read as a half-filled polygon).
	vtkNew<vtkPoints> flatPts;
	for (auto &c : ring) flatPts->InsertNextPoint(c[0], c[1], 0.0);
	vtkNew<vtkCellArray> flatPoly;
	{ vtkNew<vtkIdList> ids; for (vtkIdType i = 0; i < (vtkIdType)ring.size(); ++i) ids->InsertNextId(i);
	  flatPoly->InsertNextCell(ids); }
	vtkNew<vtkPolyData> flat; flat->SetPoints(flatPts); flat->SetPolys(flatPoly);
	vtkNew<vtkTriangleFilter> tri; tri->SetInputData(flat); tri->Update();
	vtkPolyData *base = tri->GetOutput();
	vtkPoints *bp   = base->GetPoints();

	vtkNew<vtkPoints>    outPts;
	vtkNew<vtkCellArray> outTris;
	vtkCellArray *bt = base->GetPolys();
	bt->InitTraversal();
	vtkNew<vtkIdList> tids;
	while (bt->GetNextCell(tids)) {
		if (tids->GetNumberOfIds() < 3) continue;
		double A[3], B[3], C[3];
		bp->GetPoint(tids->GetId(0), A); bp->GetPoint(tids->GetId(1), B); bp->GetPoint(tids->GetId(2), C);
		const double emax = std::max({ std::hypot(B[0]-A[0], B[1]-A[1]),
		                               std::hypot(C[0]-B[0], C[1]-B[1]),
		                               std::hypot(A[0]-C[0], A[1]-C[1]) });
		int R = 1;                                     // sub-triangles per edge (terrain hug resolution)
		if (canDrape && spacing > 0.0 && emax > 0.0) R = std::clamp((int)std::ceil(emax / spacing), 1, 48);
		const vtkIdType base0 = outPts->GetNumberOfPoints();
		auto off = [R](int i, int j) { return i * (R + 1) - i * (i - 1) / 2 + j; };
		for (int i = 0; i <= R; ++i) for (int j = 0; j <= R - i; ++j) {
			const double a = (double)(R - i - j) / R, b = (double)i / R, c = (double)j / R;
			const double x = a*A[0] + b*B[0] + c*C[0];
			const double y = a*A[1] + b*B[1] + c*C[1];
			outPts->InsertNextPoint(x, y, drapeZ(x, y));
		}
		for (int i = 0; i < R; ++i) for (int j = 0; j < R - i; ++j) {
			vtkNew<vtkIdList> t1;
			t1->InsertNextId(base0 + off(i, j)); t1->InsertNextId(base0 + off(i+1, j)); t1->InsertNextId(base0 + off(i, j+1));
			outTris->InsertNextCell(t1);
			if (j < R - i - 1) {
				vtkNew<vtkIdList> t2;
				t2->InsertNextId(base0 + off(i+1, j)); t2->InsertNextId(base0 + off(i+1, j+1)); t2->InsertNextId(base0 + off(i, j+1));
				outTris->InsertNextCell(t2);
			}
		}
	}
	pg.fillPD->SetPoints(outPts);
	pg.fillPD->SetPolys(outTris);
	pg.fillPD->Modified();
	if (!pg.fill) {
		vtkNew<vtkPolyDataMapper> map; map->SetInputData(pg.fillPD);   // already triangulated + draped
		map->ScalarVisibilityOff();
		vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
		map->SetRelativeCoincidentTopologyPolygonOffsetParameters(0.0, -33000.0);  // above surface, below the outline (-66000)
		pg.fill = vtkSmartPointer<vtkActor>::New();
		pg.fill->SetMapper(map);
		pg.fill->GetProperty()->LightingOff();
		pg.fill->GetProperty()->EdgeVisibilityOff();
		pg.fill->GetProperty()->BackfaceCullingOff();
		pg.fill->PickableOff();
		s->ren->AddActor(pg.fill);
	}
	pg.fill->GetProperty()->SetColor(pg.fillColor[0], pg.fillColor[1], pg.fillColor[2]);
	pg.fill->GetProperty()->SetOpacity(pg.fillOpacity);
	pg.fill->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	pg.fill->SetVisibility(pg.fillOpacity > 0.0 ? 1 : 0);   // no fill drawn until the user raises opacity
}

static void polyRebuildLine(Scene *s, Polygon &pg) {
	if (!pg.linePD) pg.linePD = vtkSmartPointer<vtkPolyData>::New();
	std::vector<std::array<double,3>> draped;
	polyDrapeCorners(s, pg.v, draped);
	polyFillLine(pg.linePD, draped, false);
	if (!pg.line) {
		double cr, cg, cb; prefLineColorRGB(cr, cg, cb);             // Preferences "Default line color"
		pg.line = polyMakeLineActor(s, pg.linePD, cr, cg, cb);       // (default Orange = the old look)
		pg.line->GetProperty()->SetLineWidth(prefLineWidthPx(s));    // Preferences "Default line thickness"
		s->ren->AddActor(pg.line);
	}
	polyRebuildFill(s, pg);                                          // keep the filled face in sync with the outline
}

// Axis-aligned rectangle (TRUE x,y) from two opposite corners a,b. z is left at the corners' value
// (re-draped by polyDrapeCorners on rebuild). Returns the 4 corners, not yet closed.
static void polyRectCorners(const double a[3], const double b[3], std::vector<std::array<double,3>> &out) {
	out = { { a[0], a[1], a[2] }, { b[0], a[1], a[2] }, { b[0], b[1], b[2] }, { a[0], b[1], b[2] } };
}

// A polygon is a rectangle if it was drawn with a rect tool (isRect) OR it is a nested-grids
// rectangle (nestKind==1). Single source of truth — every nested rect, whatever the creation
// path (toolbar draw, chain refine, reflow split), is a rectangle and edits must keep it so.
static inline bool polyIsRect(const Polygon &pg) { return pg.isRect || pg.nestKind == 1; }

// Drag corner `i` of a rectangle to (wx,wy) while KEEPING it axis-aligned. The ring is the 4
// corners (+ closing dup) laid out by polyRectCorners: v0=(ax,ay) v1=(bx,ay) v2=(bx,by) v3=(ax,by),
// so corner i and its opposite (i+2)%4 are the two free diagonal corners. We anchor the opposite
// corner Q and rebuild the other three from the dragged corner P=(wx,wy): the two neighbours take
// the mixed (Q.x,P.y)/(P.x,Q.y) coords, the split flipping with i's parity (even/odd winding).
static void rectDragCorner(Polygon &pg, int i, double wx, double wy) {
	if (pg.v.size() < 4 || i < 0 || i > 3) return;
	const int op = (i + 2) % 4;
	const double qx = pg.v[op][0], qy = pg.v[op][1];
	const double pz = pg.v[i][2];
	pg.v[i] = { wx, wy, pz };
	if (i % 2 == 0) { pg.v[(i+1)%4] = { qx, wy, pg.v[(i+1)%4][2] }; pg.v[(i+3)%4] = { wx, qy, pg.v[(i+3)%4][2] }; }
	else            { pg.v[(i+1)%4] = { wx, qy, pg.v[(i+1)%4][2] }; pg.v[(i+3)%4] = { qx, wy, pg.v[(i+3)%4][2] }; }
	if (pg.closed && pg.v.size() >= 2) pg.v.back() = pg.v.front();   // keep the closing dup in sync
}

// Circle (in the TRUE x,y plane) centred at c, passing through edge point e, as N corner points.
static void polyCircleCorners(const double c[3], const double e[3], std::vector<std::array<double,3>> &out) {
	const double r = std::hypot(e[0] - c[0], e[1] - c[1]);
	out.clear();
	const int N = 72;
	for (int k = 0; k < N; ++k) {
		const double a = 2.0 * vtkMath::Pi() * k / N;
		out.push_back({ c[0] + r * std::cos(a), c[1] + r * std::sin(a), c[2] });
	}
}

// Refresh the in-progress draw preview from polyCur, optionally trailing a segment to `cursor`.
// Polygon/polyline trail the placed vertices to the cursor (polygon closes the loop, polyline
// stays open). Rectangle/circle preview the full shape from the first click to the cursor.
static void polyRebuildPreview(Scene *s, const double *cursor) {
	if (!s->polyPreviewPD) s->polyPreviewPD = vtkSmartPointer<vtkPolyData>::New();
	std::vector<std::array<double,3>> verts;
	if (s->polyShape == Scene::SH_Rect || s->polyShape == Scene::SH_RectN || s->polyShape == Scene::SH_Circle) {
		if (s->polyDrawing && !s->polyCur.empty() && cursor) {
			if (s->polyShape != Scene::SH_Circle) polyRectCorners(s->polyCur[0].data(), cursor, verts);
			else                                polyCircleCorners(s->polyCur[0].data(), cursor, verts);
			if (!verts.empty()) verts.push_back(verts.front());   // close the ring for the preview
		}
	} else {                                                      // polygon / polyline
		verts = s->polyCur;
		if (cursor) verts.push_back({ cursor[0], cursor[1], cursor[2] });
		if (s->polyShape == Scene::SH_Polygon && verts.size() >= 2)
			verts.push_back(verts.front());                       // polygon: close so the loop drapes too
	}
	std::vector<std::array<double,3>> draped;
	polyDrapeCorners(s, verts, draped);
	polyFillLine(s->polyPreviewPD, draped, false);   // already a draped ring/chain
	if (!s->polyPreview) {
		s->polyPreview = polyMakeLineActor(s, s->polyPreviewPD, 1.0, 0.85, 0.2);   // drawing: yellow
		s->ren->AddActor(s->polyPreview);
	}
	s->polyPreview->SetVisibility(verts.size() >= 2 ? 1 : 0);
}

// ONE editable-vertex view over EITHER a drawn Polygon (Scene::polyEdit) or one segment of an
// OVERLAY edited in place (Scene::ovEdit/ovEditSeg). Handles, hit test and drag all read and write
// through this, so a contour and a hand-drawn polyline are edited by exactly the same code — the
// overlay case is not a second edit implementation, it is the same one pointed at other storage.
// Vertices are in DATA coords for both (the handle actor carries the scene's xfac/VE scale).
struct EditVerts {
	Polygon *pg = nullptr;                 // drawn polygon under edit …
	Overlay *ov = nullptr;                 // … or an overlay, whose segment is [a, z)
	int a = 0, z = 0;

	bool valid() const { return pg || (ov && ov->baseLine && ov->baseLine->GetPoints() && z - a >= 2); }
	int  n()     const { return pg ? (int)pg->v.size() : (z - a); }
	void get(int i, double p[3]) const {
		if (pg) { p[0] = pg->v[i][0]; p[1] = pg->v[i][1]; p[2] = pg->v[i][2]; }
		else    ov->baseLine->GetPoints()->GetPoint(a + i, p);
	}
	void set(int i, const double p[3]) {
		if (pg) pg->v[i] = { p[0], p[1], p[2] };
		else    ov->baseLine->GetPoints()->SetPoint(a + i, p);
	}
	// A closed ring stores its first vertex again at the end; one corner must still get ONE handle,
	// and dragging vertex 0 has to carry that closing copy along.
	bool closedRing() const {
		const int m = n();
		if (m < 2) return false;
		double p0[3], p1[3];
		get(0, p0);  get(m - 1, p1);
		return p0[0] == p1[0] && p0[1] == p1[1] && p0[2] == p1[2];
	}
	void commit(Scene *s) {
		if (pg) polyRebuildLine(s, *pg);
		else if (ov && ov->baseLine) { ov->baseLine->GetPoints()->Modified(); ov->baseLine->Modified(); }
	}
};

static EditVerts editVerts(Scene *s) {
	EditVerts e;
	if (!s) return e;
	if (s->polyEdit >= 0 && s->polyEdit < (int)s->polys.size()) {
		e.pg = &s->polys[s->polyEdit];
		e.z  = (int)e.pg->v.size();
		return e;
	}
	if (s->ovEdit >= 0 && s->ovEdit < (int)s->overlays.size()) {
		Overlay &o = s->overlays[s->ovEdit];
		if (s->ovEditSeg >= 0 && s->ovEditSeg < o.nseg && (int)o.segoff.size() > s->ovEditSeg + 1) {
			e.ov = &o;
			e.a  = o.segoff[s->ovEditSeg];
			e.z  = o.segoff[s->ovEditSeg + 1];
		}
	}
	return e;
}

// True while ANY element is under vertex edit — a drawn polygon or an overlay line.
static bool polyEditing(Scene *s) { return s && (s->polyEdit >= 0 || s->ovEdit >= 0); }

// Rebuild the square vertex handles shown for the element under edit (polygon or overlay segment).
static void polyRebuildHandles(Scene *s) {
	if (!s->polyHandlePD) s->polyHandlePD = vtkSmartPointer<vtkPolyData>::New();
	vtkNew<vtkPoints> pts;
	vtkNew<vtkCellArray> verts;
	EditVerts ev = editVerts(s);
	if (ev.valid()) {
		const int m = ev.closedRing() ? ev.n() - 1 : ev.n();
		for (int i = 0; i < m; ++i) {
			double p[3];  ev.get(i, p);
			const vtkIdType id = pts->InsertNextPoint(p);
			verts->InsertNextCell(1, &id);
		}
	}
	s->polyHandlePD->SetPoints(pts);
	s->polyHandlePD->SetVerts(verts);
	s->polyHandlePD->Modified();
	if (!s->polyHandles) {
		vtkNew<vtkPolyDataMapper> map; map->SetInputData(s->polyHandlePD); map->ScalarVisibilityOff();
		vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
		map->SetRelativeCoincidentTopologyPointOffsetParameter(-200000.0); // handles above ANY pile rank while editing
		s->polyHandles = vtkSmartPointer<vtkActor>::New();
		s->polyHandles->SetMapper(map);
		s->polyHandles->GetProperty()->SetColor(1.0, 1.0, 0.0);            // yellow squares
		s->polyHandles->GetProperty()->SetPointSize(11.0);                 // GL_POINTS render as squares (no sphere)
		s->polyHandles->GetProperty()->SetRenderPointsAsSpheres(false);
		s->polyHandles->GetProperty()->LightingOff();
		s->polyHandles->PickableOff();
		s->polyHandles->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		s->ren->AddActor(s->polyHandles);
	}
	s->polyHandles->SetVisibility(polyEditing(s) ? 1 : 0);
}

static void polyEnterEdit(Scene *s, int idx) {
	s->polyEdit = idx;
	s->ovEdit = -1;  s->ovEditSeg = -1;      // the two edit targets are mutually exclusive
	s->polyDragVert = -1;
	polyRebuildHandles(s);
}

// Edit an OVERLAY line where it lies: no promotion, no copy, no new Scene Objects row. Same entry
// contract as polyEnterEdit above, pointed at the overlay's own points.
static void overlayEnterEdit(Scene *s, int ovIdx, int segIdx) {
	s->polyEdit = -1;
	s->ovEdit = ovIdx;  s->ovEditSeg = segIdx;
	s->polyDragVert = -1;
	polyRebuildHandles(s);
}

static void polyExitEdit(Scene *s) {
	s->polyEdit = -1;
	s->ovEdit = -1;  s->ovEditSeg = -1;
	s->polyDragVert = -1;
	polyRebuildHandles(s);
}

// Rebuild the single yellow "armed" handle shown on a double-click-selected symbol (Scene::symArmed)
// — visible confirmation that the double-click SELECTED it (mirrors the polygon vertex handles'
// look/role exactly), and a big, comfortable hit target for the follow-up press+drag (symHitHandle)
// instead of re-hitting the tiny glyph itself. The handle's point is copied straight from the
// symbol's own polydata (already x*xfac-baked), so it uses the SAME actor scale convention as
// SymbolLayer actors (1,1,zfac*ve) — NOT polyHandles' (xfac,1,zfac*ve), which is for RAW-coord
// polygon vertices.
static void symRebuildHandle(Scene *s) {
	if (!s->symHandlePD) s->symHandlePD = vtkSmartPointer<vtkPolyData>::New();
	vtkNew<vtkPoints> pts;
	vtkNew<vtkCellArray> verts;
	if (s->symArmed >= 0 && s->symArmed < (int)s->symbols.size()) {
		SymbolLayer &sl = s->symbols[s->symArmed];
		if (auto *pd = symInputPD(sl)) {
			if (pd->GetPoints() && pd->GetPoints()->GetNumberOfPoints() > 0) {
				double p[3]; pd->GetPoints()->GetPoint(0, p);
				const vtkIdType id = pts->InsertNextPoint(p[0], p[1], p[2]);
				verts->InsertNextCell(1, &id);
			}
		}
	}
	s->symHandlePD->SetPoints(pts);
	s->symHandlePD->SetVerts(verts);
	s->symHandlePD->Modified();
	if (!s->symHandle) {
		vtkNew<vtkPolyDataMapper> map; map->SetInputData(s->symHandlePD); map->ScalarVisibilityOff();
		map->SetRelativeCoincidentTopologyPointOffsetParameter(-200000.0);
		s->symHandle = vtkSmartPointer<vtkActor>::New();
		s->symHandle->SetMapper(map);
		s->symHandle->GetProperty()->SetColor(1.0, 1.0, 0.0);                // yellow, same as polyHandles
		s->symHandle->GetProperty()->SetPointSize(16.0);                    // bigger than polyHandles' 11 —
		                                                                     // symbols are small, needs slack
		s->symHandle->GetProperty()->SetRenderPointsAsSpheres(false);
		s->symHandle->GetProperty()->LightingOff();
		s->symHandle->PickableOff();
		s->symHandle->SetScale(1.0, 1.0, s->zfac * s->ve);
		// Goes into the OVERLAY renderer (s->axesRen, layer 1 — same one the Z-axis tick labels use,
		// "own depth, never occluded by the surface"), NOT s->ren — a separate compositing layer with
		// its own depth buffer wins trivially against the main scene, no coincident-topology-offset
		// guesswork needed (an offset-only attempt against the glyph's own filled-polygon depth bias
		// did NOT reliably win in testing).
		s->axesRen->AddActor(s->symHandle);
	}
	s->symHandle->SetVisibility(s->symArmed >= 0 ? 1 : 0);
}

// Is (x,y) within tol px of the armed symbol's handle? A fixed, generous tolerance (unlike
// pickSymbolAt's size-scaled one) since the glyph itself can be tiny — mirrors polyHitHandle's role.
static bool symHitHandle(Scene *s, int x, int y, double tol) {
	if (s->symArmed < 0 || s->symArmed >= (int)s->symbols.size()) return false;
	SymbolLayer &sl = s->symbols[s->symArmed];
	auto *pd = symInputPD(sl);
	if (!pd || !pd->GetPoints() || pd->GetPoints()->GetNumberOfPoints() == 0) return false;
	double p[3]; pd->GetPoints()->GetPoint(0, p);
	double sc[3]; sl.actor->GetScale(sc);
	s->ren->SetWorldPoint(p[0] * sc[0], p[1] * sc[1], p[2] * sc[2], 1.0);
	s->ren->WorldToDisplay();
	double d[3]; s->ren->GetDisplayPoint(d);
	const double dx = d[0] - x, dy = d[1] - y;
	return (dx * dx + dy * dy) <= tol * tol;
}

// Index of the finished polygon whose outline (any edge, incl. the closing one) passes within
// `tol` px of (x,y) display px, or -1. Topmost (last drawn) wins.
static int polyHitPolygon(Scene *s, int x, int y, double tol) {
	const double tol2 = tol * tol;
	for (int pi = (int)s->polys.size() - 1; pi >= 0; --pi) {
		auto &v = s->polys[pi].v;
		const int n = (int)v.size();
		if (n < 2) continue;
		const int edges = s->polys[pi].closed ? n : (n - 1);   // open polyline: no closing edge
		for (int i = 0; i < edges; ++i) {
			double a[2], b[2];
			polyToDisplay(s, v[i], a);
			polyToDisplay(s, v[(i + 1) % n], b);
			if (segDist2((double)x, (double)y, a, b) <= tol2) return pi;
		}
	}
	return -1;
}

// Index of the edited element's vertex within `tol` px of (x,y), or -1. Works off the same EditVerts
// view the handles are drawn from, so a handle you can see is always a handle you can grab.
static int polyHitHandle(Scene *s, int x, int y, double tol) {
	EditVerts ev = editVerts(s);
	if (!ev.valid()) return -1;
	const double tol2 = tol * tol;
	const int m = ev.closedRing() ? ev.n() - 1 : ev.n();
	int best = -1; double bestd = tol2;
	for (int i = 0; i < m; ++i) {
		double p[3];  ev.get(i, p);
		double d[2];  polyToDisplay(s, { p[0], p[1], p[2] }, d);
		const double dx = d[0] - x, dy = d[1] - y, dd = dx*dx + dy*dy;
		if (dd <= bestd) { bestd = dd; best = i; }
	}
	return best;
}

// Close the current draw into a finished polygon (>=3 vertices). The stored ring is explicitly
// closed (a copy of vertex 0 is appended so first == last), it is listed in the Scene Objects
// panel as "polygon N", and the draw tool then ends (button untoggled -> arrow cursor).
static void polyFinalize(Scene *s, std::vector<std::array<double,3>> verts, bool closed, const char *prefix) {
	Polygon pg; pg.v = std::move(verts); pg.closed = closed;
	if (std::string(prefix) == "Nested rectangle") pg.nestKind = 1;   // special "Nested grids" rectangle
	if (std::string(prefix) == "fault") pg.isFault = true;            // Draw Fault line: props open the elastic dialog
	if (std::string(prefix) == "rectangle" || pg.nestKind == 1) pg.isRect = true;   // rect tools: edits keep it axis-aligned
	if (closed && pg.v.size() >= 2 && !(pg.v.front() == pg.v.back()))
		pg.v.push_back(pg.v.front());      // close the ring (first == last)
	const std::string pre = std::string(prefix) + " ";   // number PER type: "polygon 1", "rectangle 1", ...
	int idx = 1;
	for (auto &p : s->polys) if (p.name.rfind(pre, 0) == 0) ++idx;
	pg.name = pre + std::to_string(idx);
	polyRebuildLine(s, pg);
	pg.stack = s->vecSeq++;                 // new polygon lands on top of the shared vector pile
	s->polys.push_back(pg);
	applyVectorStacking(s);                // normalize ranks + set this polygon's draw-order offset
	s->polyCur.clear();
	s->polyDrawing = false;
	if (s->polyPreview) s->polyPreview->SetVisibility(0);
	rebuildSceneObjects(s);                // add the new shape's row to the Scene Objects list
	if (pg.nestKind == 1) {
		nestReflow(s);                     // snap the new nested rectangle to its parent's grid
		int nnest = 0; for (auto &p : s->polys) if (p.nestKind == 1) ++nnest;
		if (nnest == 1) unfoldSceneObjects(s);   // first one: reveal AND un-fold the dock so it's visible
	}
	// Finishing ends the draw session: untoggle the toolbar button (-> polygonSetMode(false),
	// which restores the arrow cursor and clears draw state). Falls back if there's no button.
	if (s->polyAct) s->polyAct->setChecked(false);
	else            polygonSetMode(s, false);
}

// Tear down the "Copy me" floating clone (commit or abandon) and reset the drag state.
static void copyMeEnd(Scene *s) {
	if (s->copyGhost.fill) s->ren->RemoveActor(s->copyGhost.fill);
	if (s->copyGhost.line) s->ren->RemoveActor(s->copyGhost.line);
	s->copyGhost = Polygon{};
	s->copyDragging = false;
}

// "Copy me" (line/polyline/polygon context menu, 55_lineprops.cpp): clone `src`'s geometry + look
// into a floating ghost that tracks the cursor (polygonHandleMove) until a left click drops it
// (polygonHandlePress), at which point it commits through the SAME polyFinalize a draw tool's
// double-click uses. The ghost is its OWN Polygon/actor pair, never aliased to `src`'s — assigning
// vtkSmartPointers would alias the SAME actor into both, so only value fields are copied here.
static void copyMeStart(Scene *s, const Polygon &src) {
	if (!s || !s->widget || src.v.empty()) return;
	copyMeEnd(s);   // abandon any clone already in flight (rare: menu re-invoked mid-drag)
	s->copyGhost.v           = src.v;
	s->copyGhost.closed      = src.closed;
	s->copyGhost.fillColor[0] = src.fillColor[0];
	s->copyGhost.fillColor[1] = src.fillColor[1];
	s->copyGhost.fillColor[2] = src.fillColor[2];
	s->copyGhost.fillOpacity = src.fillOpacity;
	s->copyGhost.groupName   = src.groupName;

	const QPoint lp = s->widget->mapFromGlobal(QCursor::pos());
	double w[3];
	if (polyPickWorld(s, lp.x(), lp.y(), w)) {
		s->copyAnchorW = { w[0], w[1], w[2] };
	} else {                                              // off-surface: fall back to the shape's own centroid
		double cx = 0, cy = 0; const int n = (int)src.v.size();
		for (auto &v : src.v) { cx += v[0]; cy += v[1]; }
		s->copyAnchorW = { cx / n, cy / n, 0.0 };
	}

	polyRebuildLine(s, s->copyGhost);   // first build: creates the ghost's own line/fill actors
	if (s->copyGhost.line && src.line) {
		double col[3]; src.line->GetProperty()->GetColor(col);
		s->copyGhost.line->GetProperty()->SetColor(col[0], col[1], col[2]);
		s->copyGhost.line->GetProperty()->SetLineWidth(src.line->GetProperty()->GetLineWidth());
	}
	s->copyDragging = true;
	if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// ===========================================================================================
//  "Nested grids" (tsunami) quantization — port of Mirone's nesting_sizes.m
//
//  Each nested rectangle's edges snap to its PARENT's grid nodes, shifted half a parent cell
//  out and half a child cell in, so the child grid is pixel-aligned inside the parent. The
//  chain is the nestKind==1 polygons in CREATION order (s->polys order, bigger first): the
//  first's parent is the base grid; each later one's parent is the rectangle before it. Editing
//  any rectangle reflows it AND every descendant. Mirrors resize2nesting_size + find_nearest.
// ===========================================================================================
struct NestLims { double x0, x1, y0, y1, xi, yi; };

// Base grid region + node spacing (grid registration assumed). false if no grid is loaded.
static bool nestBaseGrid(Scene *s, NestLims &g) {
	if (s->gridZ.empty() || s->gnx < 2 || s->gny < 2) return false;
	g.x0 = s->gx0; g.x1 = s->gx1; g.y0 = s->gy0; g.y1 = s->gy1;
	g.xi = (s->gx1 - s->gx0) / (s->gnx - 1);
	g.yi = (s->gy1 - s->gy0) / (s->gny - 1);
	return true;
}

// Nearest parent node value+index to pt, clamped to [0, n-1]. (find_nearest, but index-based.)
static void nestNearest(double v0, double inc, int n, double pt, double &val, int &idx) {
	if (inc == 0.0 || n < 1) { val = v0; idx = 0; return; }
	int i = (int)std::lround((pt - v0) / inc);
	if (i < 0) i = 0;
	if (i > n - 1) i = n - 1;
	idx = i; val = v0 + i * inc;
}

// Snap pt to a parent node in one direction: dir<0 floors (node <= pt), dir>0 ceils (node >= pt),
// clamped to [0, n-1]. Used so a nested rect rounds OUTWARD to enclose the drawn box — nearest-node
// rounding on both edges can land them on the same node and collapse the rect to zero width.
static void nestSnapDir(double v0, double inc, int n, double pt, int dir, double &val, int &idx) {
	if (inc == 0.0 || n < 1) { val = v0; idx = 0; return; }
	double r = (pt - v0) / inc;
	int i = dir < 0 ? (int)std::floor(r) : (int)std::ceil(r);
	if (i < 0) i = 0;
	if (i > n - 1) i = n - 1;
	idx = i; val = v0 + i * inc;
}

// Axis-aligned bbox of a polygon ring.
static void nestBBox(const Polygon &pg, double &x0, double &x1, double &y0, double &y1) {
	x0 = y0 = 1e300; x1 = y1 = -1e300;
	for (auto &v : pg.v) { x0 = std::min(x0, v[0]); x1 = std::max(x1, v[0]);
	                       y0 = std::min(y0, v[1]); y1 = std::max(y1, v[1]); }
}

// Force a nested rect's ring to an axis-aligned rectangle at the given limits (z re-draped on rebuild).
static void nestSetRect(Scene *s, Polygon &pg, double x0, double x1, double y0, double y1) {
	const double z = pg.v.empty() ? 0.0 : pg.v[0][2];
	pg.v = { {x0,y0,z}, {x1,y0,z}, {x1,y1,z}, {x0,y1,z}, {x0,y0,z} };
	pg.closed = true;
	polyRebuildLine(s, pg);
}

// Re-quantize the whole nested chain. parent_lims walk the chain (base grid -> rect 1 -> rect 2 ...).
static void nestReflow(Scene *s, bool snap) {
	std::vector<Polygon*> chain;
	for (auto &pg : s->polys) if (pg.nestKind == 1) chain.push_back(&pg);
	if (chain.empty()) return;
	NestLims base; const bool validGrid = nestBaseGrid(s, base);
	NestLims parent{};

	for (size_t k = 0; k < chain.size(); ++k) {
		Polygon &pg = *chain[k];
		double cxi = pg.nestXi, cyi = pg.nestYi;          // child increments (0 = inherit parent)
		if (k == 0) {
			if (!validGrid) {                             // parent rect over an empty region: keep it as-is,
				double bx0, bx1, by0, by1; nestBBox(pg, bx0, bx1, by0, by1);   // and seed the chain from it
				if (cxi <= 0) cxi = (bx1 - bx0); if (cyi <= 0) cyi = (by1 - by0);
				pg.nestXi = cxi; pg.nestYi = cyi;
				parent = { bx0, bx1, by0, by1, cxi, cyi };
				continue;
			}
			parent = base;
			if (cxi <= 0) cxi = base.xi;
			if (cyi <= 0) cyi = base.yi;
		} else {
			if (cxi <= 0) cxi = parent.xi;
			if (cyi <= 0) cyi = parent.yi;
		}
		pg.nestXi = cxi; pg.nestYi = cyi;                 // make the resolved increments concrete

		double rx0, rx1, ry0, ry1; nestBBox(pg, rx0, rx1, ry0, ry1);   // requested (drawn / edited) edges
		if (!snap) {
			// Restore: verts loaded from the session are ALREADY snapped. Re-snapping them would round
			// the half-parent-cell-out edge outward again and grow the rect one parent cell each reflow.
			// So leave the verts put; just reverse the tx0 = vxmin - parent.xi/2 + cxi/2 offset to recover
			// the enclosed node indices, and pass this rect on as the next parent.
			const double vxmin = rx0 + parent.xi / 2 - cxi / 2, vxmax = rx1 - parent.xi / 2 + cxi / 2;
			const double vymin = ry0 + parent.yi / 2 - cyi / 2, vymax = ry1 - parent.yi / 2 + cyi / 2;
			pg.nestIx0 = (int)std::lround((vxmin - parent.x0) / parent.xi);
			pg.nestIx1 = (int)std::lround((vxmax - parent.x0) / parent.xi);
			pg.nestIy0 = (int)std::lround((vymin - parent.y0) / parent.yi);
			pg.nestIy1 = (int)std::lround((vymax - parent.y0) / parent.yi);
			parent = { rx0, rx1, ry0, ry1, cxi, cyi };
			continue;
		}
		const int pnx = (int)std::lround((parent.x1 - parent.x0) / parent.xi) + 1;
		const int pny = (int)std::lround((parent.y1 - parent.y0) / parent.yi) + 1;
		double vxmin, vxmax, vymin, vymax; int ixmin, ixmax, iymin, iymax;
		nestSnapDir(parent.x0, parent.xi, pnx, rx0, -1, vxmin, ixmin);   // round outward so the rect
		nestSnapDir(parent.x0, parent.xi, pnx, rx1, +1, vxmax, ixmax);   // always encloses the drawn box
		nestSnapDir(parent.y0, parent.yi, pny, ry0, -1, vymin, iymin);   // and never collapses to zero
		nestSnapDir(parent.y0, parent.yi, pny, ry1, +1, vymax, iymax);
		if (ixmax <= ixmin) { if (ixmin > 0) { ixmin--; vxmin -= parent.xi; } else if (ixmax < pnx - 1) { ixmax++; vxmax += parent.xi; } }
		if (iymax <= iymin) { if (iymin > 0) { iymin--; vymin -= parent.yi; } else if (iymax < pny - 1) { iymax++; vymax += parent.yi; } }
		const double tx0 = vxmin - parent.xi / 2 + cxi / 2;   // half parent cell out, half child cell in
		const double tx1 = vxmax + parent.xi / 2 - cxi / 2;
		const double ty0 = vymin - parent.yi / 2 + cyi / 2;
		const double ty1 = vymax + parent.yi / 2 - cyi / 2;
		pg.nestIx0 = ixmin; pg.nestIx1 = ixmax; pg.nestIy0 = iymin; pg.nestIy1 = iymax;
		nestSetRect(s, pg, tx0, tx1, ty0, ty1);
		parent = { tx0, tx1, ty0, ty1, cxi, cyi };        // this rect is the parent of the next
	}
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// (The NSWING daughter-grid nesting check — nswing's check_paternity/check_binning — lives in Julia:
//  nswing.jl `_nest_binning` / `_nswing_nest_msg`. It is pure header arithmetic and needs nothing from
//  the viewer, so it is NOT a C export.)

// "New nested grid": append a refined child = the inner half of the current innermost rect, with
// increments = innermost / refinement factor (Mirone make_new_nested). Then reflow snaps it.
static void nestNewChild(Scene *s) {
	std::vector<Polygon*> chain;
	for (auto &pg : s->polys) if (pg.nestKind == 1) chain.push_back(&pg);
	if (chain.empty()) return;
	bool ok = false;
	const int refine = QInputDialog::getInt(s->win, "Refinement factor", "Enter refinement factor:",
	                                        5, 1, 100000, 1, &ok);
	if (!ok || refine <= 0) return;
	Polygon &inner = *chain.back();
	double x0, x1, y0, y1; nestBBox(inner, x0, x1, y0, y1);
	const double dx = x1 - x0, dy = y1 - y0;
	Polygon pg; pg.nestKind = 1; pg.nestReg = inner.nestReg;
	pg.nestXi = inner.nestXi / refine;
	pg.nestYi = inner.nestYi / refine;
	const double z = inner.v.empty() ? 0.0 : inner.v[0][2];
	const double nx0 = x0 + dx / 4, nx1 = x1 - dx / 4, ny0 = y0 + dy / 4, ny1 = y1 - dy / 4;
	pg.v = { {nx0,ny0,z}, {nx1,ny0,z}, {nx1,ny1,z}, {nx0,ny1,z}, {nx0,ny0,z} };
	pg.closed = true;
	const std::string pre = "Nested rectangle ";
	int idx = 1;
	for (auto &p : s->polys) if (p.name.rfind(pre, 0) == 0) ++idx;
	pg.name = pre + std::to_string(idx);
	polyRebuildLine(s, pg);
	// Inherit ALL of the parent rectangle's properties: registration (above) + the line's full visual
	// style (colour / width / stipple / opacity), so a child looks and behaves exactly like its parent
	// and the chain can keep being extended with consistent rectangles.
	if (inner.line && pg.line) pg.line->GetProperty()->DeepCopy(inner.line->GetProperty());
	pg.stack = s->vecSeq++;
	s->polys.push_back(pg);
	applyVectorStacking(s);
	nestReflow(s);
}

// Remove a single polygon (by its line actor): drop the actor, erase it, fix the edit index.
static void polygonEraseOne(Scene *s, vtkActor *lineActor) {
	for (int i = 0; i < (int)s->polys.size(); ++i) {
		if (s->polys[i].line.Get() != lineActor) continue;
		if (s->ren && s->polys[i].line) s->ren->RemoveActor(s->polys[i].line);
		if (s->axesRen && s->polys[i].line) s->axesRen->RemoveActor(s->polys[i].line);  // overlay layer (on-top vectors)
		if (s->ren && s->polys[i].fill) s->ren->RemoveActor(s->polys[i].fill);
		if (s->axesRen && s->polys[i].fill) s->axesRen->RemoveActor(s->polys[i].fill);  // meca fills live here, not s->ren
		if (s->ren && s->polys[i].faultPlane)   s->ren->RemoveActor(s->polys[i].faultPlane);
		if (s->ren && s->polys[i].faultPlane3D) s->ren->RemoveActor(s->polys[i].faultPlane3D);
		if (s->polyEdit == i)      polyExitEdit(s);      // was being edited -> drop the handles
		else if (s->polyEdit > i)  s->polyEdit--;        // keep the edit index valid past the erase
		s->polys.erase(s->polys.begin() + i);
		return;
	}
}

// Delete the "layerN" blank grid extra (if it was ever created), removing its actors.
static void nestDeleteGrid(Scene *s, int chainIdx1) {
	const std::string gn = "layer" + std::to_string(chainIdx1);
	for (int e = (int)s->extras.size() - 1; e >= 0; --e) {
		if (s->extras[e].name != gn) continue;
		if (s->ren && s->extras[e].actor) s->ren->RemoveActor(s->extras[e].actor);
		if (s->ren && s->extras[e].drape) s->ren->RemoveActor(s->extras[e].drape);
		axesDestroy(s, s->extras[e].ax);      // the raster's own axes go with the raster
		s->extras.erase(s->extras.begin() + e);
	}
}

// Remove a finished polygon (identified by its line actor). Called from the unified line menu's
// "Delete". A NESTED-grid rectangle cascades: deleting it also deletes every DESCENDANT rectangle
// (the nestKind==1 polygons after it in the chain) and their "layerN" blank grids — only the
// ancestor rectangles (and their grids) remain. An ordinary polygon is a plain single-shape delete.
static void polygonDelete(Scene *s, vtkActor *lineActor) {
	int pi = -1;
	for (int i = 0; i < (int)s->polys.size(); ++i) if (s->polys[i].line.Get() == lineActor) { pi = i; break; }
	if (pi < 0) return;

	if (s->polys[pi].nestKind != 1) {                    // ordinary shape: just drop it
		polygonEraseOne(s, lineActor);
		applyVectorStacking(s);
		rebuildSceneObjects(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return;
	}

	// Nested rectangle: find its 0-based position in the chain, then cascade to it + all descendants.
	int kpos = 0;
	for (int j = 0; j < pi; ++j) if (s->polys[j].nestKind == 1) ++kpos;
	int chainTotal = 0; for (auto &p : s->polys) if (p.nestKind == 1) ++chainTotal;

	// Blank grids of this rect + every descendant: chain index (1-based) kpos+1 .. chainTotal.
	for (int ci = kpos + 1; ci <= chainTotal; ++ci) nestDeleteGrid(s, ci);

	// Descendant rectangles = this one + every nested rect after it. Collect their actors, then erase.
	std::vector<vtkActor*> kill;
	{ int seen = 0;
	  for (auto &p : s->polys) { if (p.nestKind != 1) continue; if (seen >= kpos && p.line) kill.push_back(p.line.Get()); ++seen; } }
	for (vtkActor *a : kill) polygonEraseOne(s, a);

	applyVectorStacking(s);
	applyGridStacking(s);
	nestReflow(s);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Delete all slip-model patches with a given groupName (Import Model Slip group). Called from the
// Scene Objects slip group node's "Delete" property.
static void deleteSlipGroup(Scene *s, const QString &groupName) {
	std::string gname = groupName.toStdString();
	// Collect all actors of patches with this groupName.
	std::vector<vtkActor*> kill;
	for (auto &p : s->polys) {
		if (p.groupName == gname && p.line) kill.push_back(p.line.Get());
	}
	// Erase them all.
	for (vtkActor *a : kill) polygonEraseOne(s, a);
	applyVectorStacking(s);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Remove a focal-mechanism group: every isMeca patch sharing groupName (same by-groupName removal
// as deleteSlipGroup). NOTE: a meca patch is EITHER a fill (mecaBuildPatch, comp/dilat sectors,
// `pg.fill` only) OR a line (mecaBuildLines, rim + nodal-plane curves, `pg.line` only, added to
// axesRen) — polygonEraseOne removes whichever of the two actually exists. Used both by
// the Scene Objects "Remove" menu AND as the first step of a recolour/re-stroke (Julia removes the
// old batch, then re-plots fresh, see _on_meca_props/gmtvtk_remove_meca_group_h). The group's CACHED
// properties-dialog pre-fill state (s->mecaGroups) is deliberately left alone here — erasing it used
// to also drop its `propsDlg` QPointer, so a recolour round-trip lost track of the dialog the user was
// still live-editing, and the next Scene Objects row click opened a SECOND, duplicate window right
// next to it (2026-07-05 bug). gmtvtk_add_meca_h's own find-or-create overwrites every colour/font
// field on the surviving entry anyway, so nothing here goes stale except a harmless leftover cache
// row if the batch is removed for good and never re-plotted under the same name.
static void deleteMecaGroup(Scene *s, const QString &groupName) {
	deleteSlipGroup(s, groupName);
	std::string gname = groupName.toStdString();
	// Drop this batch's date labels too (gmtvtk_add_texts_h groupName tag) — otherwise a recolour/
	// re-plot round-trip (remove then re-add) would leave the OLD labels behind alongside fresh ones.
	for (size_t i = s->texts.size(); i-- > 0; ) {
		if (s->texts[i].groupName != gname) continue;
		if (s->texts[i].actor) {
			if (s->axesRen) s->axesRen->RemoveActor(s->texts[i].actor);
			if (s->ren)     s->ren->RemoveActor(s->texts[i].actor);
		}
		s->texts.erase(s->texts.begin() + i);
	}
	// Drop this batch's per-event drag state + any anchor lines a user drag left behind — the
	// fill/line actors they pointed at are already gone (deleteSlipGroup, above).
	for (auto it = s->mecaBalls.begin(); it != s->mecaBalls.end(); ) {
		if (it->groupName == gname) {
			if (it->anchor) {
				if (s->axesRen) s->axesRen->RemoveActor(it->anchor);
				if (s->ren)     s->ren->RemoveActor(it->anchor);
			}
			if (it->anchorDot) {
				if (s->axesRen) s->axesRen->RemoveActor(it->anchorDot);
				if (s->ren)     s->ren->RemoveActor(it->anchorDot);
			}
			it = s->mecaBalls.erase(it);
		} else ++it;
	}
	s->mecaDrag = -1;
}

// Toolbar toggle: enter/leave draw mode. Switching cancels any in-progress draw and edit.
static void polygonSetMode(Scene *s, bool on) {
	s->polyMode = on;
	s->polyCur.clear();
	s->polyDrawing = false;
	if (s->polyPreview) s->polyPreview->SetVisibility(0);
	if (on) polyExitEdit(s);
	if (s->widget)
		s->widget->setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
	if (s->widget && s->widget->renderWindow())
		s->widget->renderWindow()->Render();
}

// Toolbar wiring for the five draw tools. Checking a button selects its ShapeKind, untoggles the
// other four (mutually exclusive), and enters draw mode; unchecking the active one leaves draw mode.
static void polygonToolToggled(Scene *s, QAction *act, Scene::ShapeKind shape, bool on) {
	if (on) {
		if (s->emptyStart) {                            // blank launcher: nothing to draw ON yet
			QSignalBlocker bl(act);                     // snap the button back off without re-entering here
			act->setChecked(false);
			if (s->win)
				s->win->statusBar()->showMessage("Load a file first — the draw and text tools need data to draw on.", 4000);
			return;
		}
		s->polyShape = shape;
		s->polyAct   = act;
		for (QAction *a : s->shapeActs)                 // exclusive: drop any other checked tool
			if (a != act && a->isChecked()) a->setChecked(false);
		polygonSetMode(s, true);
	} else if (s->polyAct == act) {                     // the active tool was switched off
		s->polyAct = nullptr;
		polygonSetMode(s, false);
	}
}

// Cursor (mx,my device px) -> the point on the z=0 (XY) plane it projects to, in TRUE coords. Used
// to place / drag flat text labels anywhere, even off the surface (clicking the sky still lands on
// the plane). Returns false only if the view ray is parallel to the plane.
static bool pickPlaneXY(Scene *s, int mx, int my, double outTrue[3]) {
	double nr[4], fr[4];
	s->ren->SetDisplayPoint((double)mx, (double)my, 0.0); s->ren->DisplayToWorld();
	for (int i = 0; i < 4; ++i) nr[i] = s->ren->GetWorldPoint()[i];
	s->ren->SetDisplayPoint((double)mx, (double)my, 1.0); s->ren->DisplayToWorld();
	for (int i = 0; i < 4; ++i) fr[i] = s->ren->GetWorldPoint()[i];
	if (nr[3] != 0.0) { nr[0] /= nr[3]; nr[1] /= nr[3]; nr[2] /= nr[3]; }
	if (fr[3] != 0.0) { fr[0] /= fr[3]; fr[1] /= fr[3]; fr[2] /= fr[3]; }
	const double dirz = fr[2] - nr[2];
	if (dirz == 0.0) return false;
	const double t = -nr[2] / dirz;                 // intersect world z = 0 (scaled space)
	const double gx = (s->xfac != 0.0) ? s->xfac : 1.0;
	outTrue[0] = (nr[0] + t * (fr[0] - nr[0])) / gx;
	outTrue[1] =  nr[1] + t * (fr[1] - nr[1]);
	outTrue[2] = 0.0;
	return true;
}

// Nearest focal-mechanism BALL under the cursor (display px), or -1. Topmost (last-built) wins.
// `radius` was captured once at plot time (mecaBuildLines' rim circle, see gmtvtk_add_meca_h) as a
// pure-x offset from the centre in the SAME xy convention as Polygon::v; projecting both the centre
// and that offset point through polyToDisplay (which reapplies xfac) recovers the correct on-screen
// pixel radius without ever reasoning about "true degrees" ourselves — and tracks a live drag
// automatically since offX/offY are folded into both points identically.
static int mecaHitAt(Scene *s, int x, int y) {
	int best = -1; double bestd2 = 1e30;
	for (int bi = (int)s->mecaBalls.size() - 1; bi >= 0; --bi) {
		MecaBall &mb = s->mecaBalls[bi];
		if (mb.radius <= 0.0) continue;
		const std::array<double,3> c  = { mb.x0 + mb.offX,              mb.y0 + mb.offY, mb.zLow };
		const std::array<double,3> rp = { mb.x0 + mb.offX + mb.radius,  mb.y0 + mb.offY, mb.zLow };
		double cd[2], rd[2];
		polyToDisplay(s, c, cd);
		polyToDisplay(s, rp, rd);
		const double rpx = std::max(6.0, std::hypot(rd[0] - cd[0], rd[1] - cd[1]));
		const double dx = cd[0] - x, dy = cd[1] - y, d2 = dx*dx + dy*dy;
		if (d2 <= rpx*rpx && d2 < bestd2) { bestd2 = d2; best = bi; }
	}
	return best;
}

static void mecaUpdateAnchor(Scene *s, int bi);   // fwd (defined right below)

// Does world point (x,y) [same convention as MecaBall::x0/y0] lie within ANY currently-plotted
// ball's radius? Pure 2-D geometry, no camera/Z involved at all. mb.radius is the ball's TRUE
// on-screen radius (see its assignment in 90_c_api.cpp), which only matches raw (x,y) distances
// once x is scaled by s->xfac — the actor's own SetScale(xfac,1,…) is what turns the pre-scaled
// x/xfac ellipse into a round ball, so the test must scale x the SAME way or it compares an
// ellipse against a circle (this was the actual bug behind "gap before the ball's rim": scaling
// was missing entirely, not a sampling-resolution issue).
static bool mecaCoveredByAnyBall(Scene *s, double x, double y) {
	const double xs = x * s->xfac;
	for (auto &mb : s->mecaBalls) {
		if (mb.radius <= 0.0) continue;
		const double cx = (mb.x0 + mb.offX) * s->xfac, cy = mb.y0 + mb.offY;
		const double dx = xs - cx, dy = y - cy;
		if (dx*dx + dy*dy <= mb.radius * mb.radius) return true;
	}
	return false;
}

// A->B is a STRAIGHT segment, so clip it against a set of circles EXACTLY (analytic line-circle
// intersection). `circles[i]` = {cx, cy, radius} in RAW (unscaled) xy — cx is scaled by `xf` inside
// here, matching the ball's TRUE-visual-radius convention and the actor's own SetScale(xfac,1,…) —
// solving in raw unscaled space compares a circle radius against an ELLIPSE, which was the actual
// bug: away from the equator (xfac = cos(midlat) < 1) the raw x semi-axis is bigger than the ball's
// true radius, so the computed "covered" disk was larger than the visible ball in every direction
// except due x, clipping the line well short of the rim. t is a plain fraction along A->B so it is
// unaffected by the x-only linear rescaling; only the quadratic's coefficients use scaled x.
// Emits the surviving OUTSIDE-every-circle runs as separate 2-point polyline cells (outPts flat list
// + outCounts = vertex count per run, always 2 since each run is itself a straight sub-segment).
static void clipSegmentVsCircles(double xf, double ax, double ay, double bx, double by,
                                  const std::vector<std::array<double,3>> &circles,
                                  std::vector<std::array<double,2>> &outPts, std::vector<int> &outCounts) {
	const double dx = bx - ax, dy = by - ay;             // ORIGINAL segment — used to reconstruct points
	const double dxs = dx * xf;                          // segment delta in xfac-scaled (true-circle) space
	const double A = dxs*dxs + dy*dy;
	if (A <= 0.0) return;                          // zero-length segment: nothing to draw

	std::vector<std::array<double,2>> inside;      // [t0,t1] parametric ranges covered by SOME circle
	for (auto &c : circles) {
		if (c[2] <= 0.0) continue;
		const double cx = c[0] * xf, cy = c[1];
		const double fx = ax * xf - cx, fy = ay - cy;
		const double B = 2.0 * (fx*dxs + fy*dy);
		const double C = fx*fx + fy*fy - c[2]*c[2];
		const double disc = B*B - 4.0*A*C;
		if (disc < 0.0) continue;                  // segment never reaches this circle
		const double sq = std::sqrt(disc);
		const double t0 = std::max(0.0, (-B - sq) / (2.0*A));
		const double t1 = std::min(1.0, (-B + sq) / (2.0*A));
		if (t0 < t1) inside.push_back({ t0, t1 });
	}
	std::sort(inside.begin(), inside.end(),
	          [](const std::array<double,2> &p, const std::array<double,2> &q) { return p[0] < q[0]; });
	std::vector<std::array<double,2>> merged;
	for (auto &iv : inside) {
		if (!merged.empty() && iv[0] <= merged.back()[1]) merged.back()[1] = std::max(merged.back()[1], iv[1]);
		else merged.push_back(iv);
	}

	const auto emitRun = [&](double t0, double t1) {
		if (t1 - t0 <= 0.0) return;
		outCounts.push_back(2);
		outPts.push_back({ ax + t0*dx, ay + t0*dy });
		outPts.push_back({ ax + t1*dx, ay + t1*dy });
	};
	double cursor = 0.0;
	for (auto &iv : merged) { emitRun(cursor, iv[0]); cursor = iv[1]; }
	emitRun(cursor, 1.0);
}

// Anchor-trail flavour: clip against every CURRENTLY-PLOTTED ball (s->mecaBalls), live drag state
// (offX/offY) included. Thin wrapper over clipSegmentVsCircles.
static void mecaClipTrail(Scene *s, double ax, double ay, double bx, double by,
                          std::vector<std::array<double,2>> &outPts, std::vector<int> &outCounts) {
	std::vector<std::array<double,3>> circles;
	circles.reserve(s->mecaBalls.size());
	for (auto &mb : s->mecaBalls)
		if (mb.radius > 0.0) circles.push_back({ mb.x0 + mb.offX, mb.y0 + mb.offY, mb.radius });
	clipSegmentVsCircles(s->xfac, ax, ay, bx, by, circles, outPts, outCounts);
}

// Move ball `bi` so its centre sits at world point (wx,wy) — same xy convention as Polygon::v/
// MecaBall::x0,y0 (see the struct comment) — and (re)builds its anchor line. The single place that
// actually repositions a beachball: both the live mouse drag (polygonHandleMove) and the test hook
// (gmtvtk_meca_drag_test) call this, so a test genuinely exercises the same code that renders.
static void mecaDragTo(Scene *s, int bi, double wx, double wy) {
	MecaBall &mb = s->mecaBalls[bi];
	mb.offX = wx - mb.x0; mb.offY = wy - mb.y0;
	// Z is NOT reset to 0 here: mecaBuildPatch/mecaBuildLines already gave each actor a Position Z of
	// rank*zStep at plot time (the cross-ball depth rank) — only X/Y are a live drag offset, so we must
	// preserve whatever Z the actor already carries or dragging would flatten every ball to the same
	// rank and reopen the exact occlusion bug this convention exists to fix.
	for (vtkActor *a : mb.actors) a->SetPosition(mb.offX * s->xfac, mb.offY, a->GetPosition()[2]);
	// The date label (if "Plot event date" is on) must carry with the ball. Unlike mb.actors
	// (Polygon-baked geometry that uses a plain additive Position offset), TextLabel's Position IS
	// its true anchor (textApplyProps sets pos[0]*xfac directly) — re-find the owning TextLabel by
	// actor pointer (never cache, s->texts can reorder/erase) and recompute that same formula with
	// the offset added. dateLabel is vtkProp3D* (SetPosition only) — works whether the label
	// underneath is the flat vtkTextActor3D or the billboard vtkBillboardTextActor3D.
	if (mb.dateLabel) {
		for (auto &tl : s->texts) {
			if (tl.actor.Get() != mb.dateLabel) continue;
			mb.dateLabel->SetPosition((tl.pos[0] + mb.offX) * s->xfac, tl.pos[1] + mb.offY, 0.0);
			break;
		}
	}
	mecaUpdateAnchor(s, bi);
}

// (Re)build the drag-anchor line + dot for ball `bi`: a thin line from its ORIGINAL plotted centre
// to wherever it currently sits, with a small filled dot marking that original point — the same
// epicenter-to-symbol convention _focal_plot already draws statically for lon0/lat0 anchor columns
// (gmtvtk_add_overlay_h), drawn live here from a mouse drag instead. Occlusion is done by
// mecaClipTrail/mecaCoveredByAnyBall — GEOMETRIC (2-D, in this xy plane), not a Z-buffer trick (see
// their comments for why the two earlier Z-based attempts failed at real-catalog scale). The Z
// coordinate here is therefore just a placeholder to keep the actor in the axesRen overlay plane
// (never occluded by relief, by construction of that render layer) — it plays no role in hiding the
// trail under a ball anymore. The full polyline topology is rebuilt every call (the set of visible
// sub-segments can change shape as the ball moves), so this is NOT a simple move-one-point update
// like the first version.
static void mecaUpdateAnchor(Scene *s, int bi) {
	MecaBall &mb = s->mecaBalls[bi];
	const double z = mb.zLow;
	const double x1 = mb.x0 + mb.offX, y1 = mb.y0 + mb.offY;

	if (!mb.anchor) {
		mb.anchorPD = vtkSmartPointer<vtkPolyData>::New();
		vtkNew<vtkPolyDataMapper> lmap; lmap->SetInputData(mb.anchorPD); lmap->ScalarVisibilityOff();
		mb.anchor = vtkSmartPointer<vtkActor>::New();
		mb.anchor->SetMapper(lmap);
		mb.anchor->GetProperty()->SetColor(0.0, 0.0, 0.0);
		mb.anchor->GetProperty()->SetLineWidth(2.0);
		mb.anchor->GetProperty()->LightingOff();
		mb.anchor->PickableOff();
		mb.anchor->ForceOpaqueOn();
		mb.anchor->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		(s->axesRen ? s->axesRen : s->ren)->AddActor(mb.anchor);

		vtkNew<vtkPoints> dpts; dpts->InsertNextPoint(mb.x0, mb.y0, z);
		vtkNew<vtkCellArray> dverts; const vtkIdType id0 = 0; dverts->InsertNextCell(1, &id0);
		vtkNew<vtkPolyData> dotPD; dotPD->SetPoints(dpts); dotPD->SetVerts(dverts);
		vtkNew<vtkPolyDataMapper> dmap; dmap->SetInputData(dotPD); dmap->ScalarVisibilityOff();
		mb.anchorDot = vtkSmartPointer<vtkActor>::New();
		mb.anchorDot->SetMapper(dmap);
		mb.anchorDot->GetProperty()->SetColor(0.0, 0.0, 0.0);
		mb.anchorDot->GetProperty()->SetPointSize(7.0);
		mb.anchorDot->GetProperty()->SetRenderPointsAsSpheres(true);
		mb.anchorDot->GetProperty()->LightingOff();
		mb.anchorDot->PickableOff();
		mb.anchorDot->ForceOpaqueOn();
		mb.anchorDot->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		(s->axesRen ? s->axesRen : s->ren)->AddActor(mb.anchorDot);
	}

	std::vector<std::array<double,2>> segPts;
	std::vector<int> segCounts;
	mecaClipTrail(s, mb.x0, mb.y0, x1, y1, segPts, segCounts);

	vtkNew<vtkPoints> pts;
	vtkNew<vtkCellArray> cells;
	vtkIdType base = 0;
	for (int cnt : segCounts) {
		vtkNew<vtkIdList> ids;
		for (int i = 0; i < cnt; ++i) {
			pts->InsertNextPoint(segPts[base + i][0], segPts[base + i][1], z);
			ids->InsertNextId(base + i);
		}
		cells->InsertNextCell(ids);
		base += cnt;
	}
	mb.anchorPD->SetPoints(pts);
	mb.anchorPD->SetLines(cells);
	mb.anchorPD->Modified();

	mb.anchorDot->SetVisibility(mecaCoveredByAnyBall(s, mb.x0, mb.y0) ? 0 : 1);
}

// (Re)configure a text label's actor from its font fields. ALWAYS a vtkBillboardTextActor3D
// (2026-07-24 standing rule — see TextLabel, 10_geometry.cpp): camera-facing, constant on-screen
// size, anchored at (x,y,0) in scaled space.
static void textApplyProps(Scene *s, TextLabel &tl) {
	// Two actor kinds: the billboard every normal label uses, and — for contour annotations only
	// (TextLabel::flat) — a vtkTextActor3D lying in the XY plane so it can be ROTATED along the line
	// it labels. Everything below the fork is identical for both; only the placement differs.
	vtkBillboardTextActor3D *ba = vtkBillboardTextActor3D::SafeDownCast(tl.actor);
	vtkTextActor3D          *fa = vtkTextActor3D::SafeDownCast(tl.actor);
	vtkTextProperty *tp = ba ? ba->GetTextProperty() : (fa ? fa->GetTextProperty() : nullptr);
	if (!tp) return;
	tl.offX = tl.offY = 0.0;                 // centring shift, recomputed below for a flat label
	tp->SetFontFamilyAsString(tl.font.c_str());
	tp->SetColor(tl.color[0], tl.color[1], tl.color[2]);
	tp->SetOpacity(1.0);                // SOLID text, always: no faded/translucent glyphs anywhere
	tp->ShadowOff();
	tp->FrameOff();
	tp->SetBackgroundOpacity(0.0);      // no box behind the text either
	tp->SetBold(tl.bold ? 1 : 0);
	tp->SetItalic(tl.italic ? 1 : 0);
	tp->SetJustificationToCentered();
	// Batch-owned labels (Focal mechanisms' per-event date) sit BOTTOM-justified on their anchor
	// instead of centred — centred justification put half the glyph height BELOW the anchor point,
	// which is what drove it down into/touching the ball's rim. Bottom justification makes the whole
	// label grow UPWARD from the anchor instead. (A translucent background box was tried here too and
	// reverted — vtkTextProperty draws it as an opaque rectangle sized to the full string bounds, a
	// big flat gray block that looked far worse than the plain text.) Standalone user-placed text
	// labels (Text tool, no groupName) are unaffected, keeping their original centred look.
	if (tl.groupName.empty() || tl.vcenter) tp->SetVerticalJustificationToCentered();
	else                                    tp->SetVerticalJustificationToBottom();
	if (ba) {
		tp->SetFontSize(tl.size);       // billboard: screen-constant, the size IS the on-screen size
		ba->SetInput(tl.text.c_str());
		ba->ForceOpaqueOn();            // never depth-sorted/faded as translucent (matches gizmo/tick billboards)
	}
	else {
		// SOLID, CRISP glyphs. A vtkTextActor3D is a texture on a quad, so its density depends on how
		// the texture's resolution compares to the pixels it covers: laying it out at the on-screen
		// size and MAGNIFYING it magnifies the antialiasing too (thin, washed-out — the forbidden
		// look). The texture is therefore laid out oversampled and the actor scaled DOWN, so the
		// texture is always minified, never stretched.
		const int ss = 4;
		int dpi = (s->widget && s->widget->renderWindow()) ? s->widget->renderWindow()->GetDPI() : 72;
		if (dpi <= 0) dpi = 72;
		tp->SetFontSize((int)std::lround(tl.size * ss));
		fa->SetInput(tl.text.c_str());
		fa->ForceOpaqueOn();

		// How wide must this label be in WORLD units? Exactly what gmtvtk_label_width_world_h measured
		// when the gap was cut for it: the string's pixel width at the label's own font size and this
		// window's DPI, times world-per-pixel. Same measurement, same numbers, so the text cannot come
		// out a different size from its hole.
		// The world-per-pixel is read NOW, not remembered from when the label was created: that is what
		// makes the text SCREEN-CONSTANT — zoom in and this shrinks in world units by the same factor
		// the view grew, so the label keeps its pixel size. followZoomAnnotations (50_scene.cpp) calls
		// this again whenever the view changes materially, and re-cuts the line holes with the same
		// number, so text and hole never drift apart.
		const double wpp = sceneWorldPerPixel(s);
		double targetW = 0.0;
		{
			vtkNew<vtkTextProperty> mp;
			mp->SetFontFamilyAsString(tl.font.c_str());
			mp->SetFontSize(tl.size);
			mp->SetBold(tl.bold ? 1 : 0);
			mp->SetItalic(tl.italic ? 1 : 0);
			int bb[4] = { 0, 0, 0, 0 };
			vtkTextRenderer *tr = vtkTextRenderer::GetInstance();
			if (tr && tr->GetBoundingBox(mp, tl.text.c_str(), bb, dpi))
				targetW = double(bb[1] - bb[0] + 1) * (wpp > 0.0 ? wpp : tl.wscale);
		}

		// MEASURE the untransformed quad instead of predicting it from the font size: vtkTextActor3D
		// picks its own texture resolution and its own anchor corner, and guessing at either is what
		// left the label the wrong size and sitting outside its gap. Ask the actor, then scale it to
		// the target width and shift it so its CENTRE lands on the anchor.
		fa->SetOrientation(0.0, 0.0, 0.0);
		fa->SetScale(1.0, 1.0, 1.0);
		fa->SetPosition(0.0, 0.0, 0.0);
		double b[6] = { 0, 0, 0, 0, 0, 0 };
		fa->GetBounds(b);
		const double bw = b[1] - b[0], bh = b[3] - b[2];
		double sc = (bw > 1e-9 && targetW > 0.0) ? targetW / bw
		                                        : (tl.wscale > 0.0 ? tl.wscale / ss : 1.0);
		fa->SetScale(sc, sc, sc);
		fa->SetOrientation(0.0, 0.0, tl.angle);
		// Anchor - R(angle)*(scaled local centre): puts the middle of the glyphs on the anchor point,
		// whatever corner the actor measures itself from.
		const double th = tl.angle * vtkMath::Pi() / 180.0;
		const double cx = 0.5 * (b[0] + b[1]) * sc, cy = 0.5 * (b[2] + b[3]) * sc;
		tl.offX = cx * std::cos(th) - cy * std::sin(th);
		tl.offY = cx * std::sin(th) + cy * std::cos(th);
		(void)bh;
	}
	// z is 0 for every plane label; a contour label carries its contour's real height and so rides
	// VE exactly like the line does (applyVE re-applies this same expression, offsets included).
	tl.actor->SetPosition(tl.pos[0] * s->xfac - tl.offX, tl.pos[1] - tl.offY,
	                      tl.pos[2] * s->zfac * s->ve);
	tl.actor->PickableOff();
}

// Index of the text label whose RENDERED extent covers (x,y) display px, or -1. Topmost wins. The
// label can be large, so we test its actual world bounding box projected to the screen (a tiny
// centre-only hit would miss clicks on the visible glyphs -> they would fall through and rotate the
// camera). `tol` pads the box.
//
// Matches EVERY text label, batch-owned or not — all are vtkBillboardTextActor3D now (standing
// rule, TextLabel, 10_geometry.cpp), so `GetBounds()`/`SetPosition()` (the only calls this function
// and polygonHandleMove's textDrag branch make on `tl.actor`) work the same for all of them, no
// downcast needed. Every label is independently click-draggable, not JUST carried along by its
// owning symbol/ball (which can ALSO carry it, via a separate mechanism — see `symPtDrag`/
// `mecaDragTo`). The right-click PROPERTIES path (70_window.cpp) still branches on
// `groupName.empty()` — but only to pick which dialog opens (`textLabelMenu` vs
// `batchTextLabelsDialog`), never to decide whether one does.
static int polyHitText(Scene *s, int x, int y, double tol) {
	for (int i = (int)s->texts.size() - 1; i >= 0; --i) {
		auto &tl = s->texts[i];
		if (!tl.actor || tl.actor->GetVisibility() == 0) continue;
		double b[6]; tl.actor->GetBounds(b);                    // world space (position + scale baked in)
		if (b[0] > b[1] || b[2] > b[3]) continue;               // not rendered yet -> no valid box
		double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
		for (int cx = 0; cx < 2; ++cx)
		for (int cy = 0; cy < 2; ++cy)
		for (int cz = 0; cz < 2; ++cz) {
			s->ren->SetWorldPoint(b[cx], b[2 + cy], b[4 + cz], 1.0);
			s->ren->WorldToDisplay();
			const double *dp = s->ren->GetDisplayPoint();
			minx = std::min(minx, dp[0]); maxx = std::max(maxx, dp[0]);
			miny = std::min(miny, dp[1]); maxy = std::max(maxy, dp[1]);
		}
		if (x >= minx - tol && x <= maxx + tol && y >= miny - tol && y <= maxy + tol) return i;
	}
	return -1;
}

// Text tool: place a billboard label on the XY plane at world point w (TRUE coords, z ignored).
// Asks for string/font/size/colour/bold/italic up front (textFontDialog, 50_scene.cpp — the SAME
// dialog an existing label's "Text Properties…" uses, never a forked copy); Cancel or an empty
// string places nothing.
static void polyPlaceText(Scene *s, const double w[3]) {
	TextLabel tl;                                            // seeds the dialog with its own defaults
	if (textFontDialog(s->widget, "New text label", tl.text, tl.font, tl.size, tl.color, tl.bold, tl.italic)) {
		tl.pos  = { w[0], w[1], 0.0 };
		tl.name = "Text " + std::to_string((int)s->texts.size() + 1);   // short list label
		tl.actor = vtkSmartPointer<vtkBillboardTextActor3D>::New();     // ALWAYS billboard, see TextLabel
		textApplyProps(s, tl);
		// Add to the overlay layer (axesRen): it shares the main camera but clears its own depth, so
		// the relief can NEVER occlude the label — text is always on top, camera-facing.
		(s->axesRen ? s->axesRen : s->ren)->AddActor(tl.actor);
		s->texts.push_back(tl);
		rebuildSceneObjects(s);
	}
	if (s->giz) s->giz->grab = Grab::None;   // belt-and-suspenders: no stale gizmo drag after the modal
	// One-shot tool: untoggle the button (-> polygonSetMode(false)).
	if (s->polyAct) s->polyAct->setChecked(false);
	else            polygonSetMode(s, false);
}

// --- Qt-level mouse handlers (called from GLView, 60_profile.cpp) -------------------------
// VTK's interactor adapter does NOT deliver Qt double-clicks as a second LeftButtonPress, so the
// draw/edit gestures are driven from the GLView widget overrides instead. Each returns true when
// it consumed the event (the widget then skips the VTK base handler, so the gizmo never rotates).
// (x,y) are VTK display px (device, bottom-up), matching polyPickWorld / the projection helpers.

// End an armed vector pick (Scene::vectorPickMode): drop the rubber-band preview, hand polyShape back
// to whatever draw tool owned it, and restore the cursor. Safe to call when nothing is armed.
static void vectorPickDisarm(Scene *s) {
	if (!s) return;
	const bool wasRect = (s->vectorPickMode == 2);
	s->vectorPickMode = 0;
	s->vectorPickDbl = false;
	s->vectorPickDrawing = false;
	if (wasRect) {
		s->polyDrawing = false;
		s->polyCur.clear();
		if (s->polyPreview) s->polyPreview->SetVisibility(0);
		s->polyShape = (Scene::ShapeKind)s->vectorPickPrevShape;
	}
	if (s->widget) s->widget->unsetCursor();
}

// The ONE place an armed CLICK pick (Scene::vectorPickMode == 1) resolves what line is under the
// cursor and answers the tool. Both mouse doors — a single left click, and the double-click variant
// tools ask for with Scene::vectorPickDbl — call THIS; there is no second hit test anywhere.
// The two tests, in this order, are the same ones the double-click edit path uses: a drawn Polygon
// first, then an imported line Overlay. Always returns true: an armed pick consumes the click, so it
// can never leak into a rotate/drag.
// Flash the element a pick just answered with, so the user SEES which line was taken: its own actor
// blinks off/on a few times and is left visible. Lives here, next to the resolver, so EVERY picking
// tool gets the same feedback — never a highlight re-invented per dialog. Named lookup covers both
// doors a line can come in through (a drawn Polygon, an imported line Overlay), like the resolver.
static void blinkElementByName(Scene *s, const std::string &name) {
	if (!s || !sceneAlive(s) || !s->widget || name.empty()) return;
	vtkSmartPointer<vtkActor> act;
	for (auto &pg : s->polys)
		if (pg.name == name) { act = pg.line; break; }
	if (!act)
		for (auto &ov : s->overlays)
			if (ov.name == name) { act = ov.actor; break; }
	if (!act || !act->GetVisibility()) return;      // hidden element: nothing to blink
	auto left = std::make_shared<int>(6);            // three off/on pairs
	QTimer *t = new QTimer(s->widget);
	t->setInterval(110);
	QObject::connect(t, &QTimer::timeout, s->widget, [s, act, left, t]() {
		if (!sceneAlive(s) || !s->widget) { t->stop(); t->deleteLater(); return; }
		act->SetVisibility(act->GetVisibility() ? 0 : 1);
		if (--*left <= 0) { act->SetVisibility(1); t->stop(); t->deleteLater(); }
		s->widget->renderWindow()->Render();
	});
	t->start();
}

static bool vectorPickFire(Scene *s, int x, int y) {
	std::string hit;
	const int pi = polyHitPolygon(s, x, y, 8.0);
	if (pi >= 0)
		hit = s->polys[pi].name;
	else {
		int ovMode = 1, segIdx = -1;
		if (vtkActor *ovAct = pickOverlayAt(s, x, y, ovMode, &segIdx)) {
			for (auto &ov : s->overlays)
				if (ov.actor.Get() == ovAct) { hit = ov.name; break; }
		}
	}
	blinkElementByName(s, hit);                  // say WHICH line answered, before the tool reacts
	// Through a COPY: a tool is allowed to end its own pick from inside the answer (both boxes now
	// full), and that clears Scene::vectorPickCB — which would destroy the very lambda running here.
	auto cb = s->vectorPickCB;
	if (cb) cb(hit);                             // stays armed: the tool collects as many as wanted
	return true;
}

// Left/right press. button: 0 = left, 1 = right. shift: Shift held (whole-element drag in edit mode).
static bool polygonHandlePress(Scene *s, int button, int x, int y, bool shift) {
	// "Copy me" clone in flight: a left click drops it where it stands, committing through the SAME
	// polyFinalize a draw tool's double-click uses (kind inferred from the clone's own closed/vertex-
	// count, exactly how the draw tools' own double-click picks "line"/"polyline"/"polygon"). Any
	// other button is swallowed too — no stray camera nav while a clone is attached to the cursor.
	if (s->copyDragging) {
		if (button == 0 && s->copyGhost.v.size() >= 2) {
			std::vector<std::array<double,3>> verts = s->copyGhost.v;
			const bool   closed = s->copyGhost.closed;
			const double fillColor[3] = { s->copyGhost.fillColor[0], s->copyGhost.fillColor[1], s->copyGhost.fillColor[2] };
			const double fillOpacity  = s->copyGhost.fillOpacity;
			const std::string groupName = s->copyGhost.groupName;
			double outCol[3] = { 1.0, 0.55, 0.0 }; double outW = 2.5;
			if (s->copyGhost.line) {
				s->copyGhost.line->GetProperty()->GetColor(outCol);
				outW = s->copyGhost.line->GetProperty()->GetLineWidth();
			}
			copyMeEnd(s);
			const char *prefix = !closed ? (verts.size() == 2 ? "line" : "polyline") : "polygon";
			polyFinalize(s, verts, closed, prefix);
			if (!s->polys.empty()) {                        // restyle the just-finalized element to match the copy source
				Polygon &np = s->polys.back();
				np.line->GetProperty()->SetColor(outCol[0], outCol[1], outCol[2]);
				np.line->GetProperty()->SetLineWidth(outW);
				np.fillColor[0] = fillColor[0]; np.fillColor[1] = fillColor[1]; np.fillColor[2] = fillColor[2];
				np.fillOpacity  = fillOpacity;
				np.groupName    = groupName;
				polyRebuildFill(s, np);
				if (!groupName.empty()) rebuildSceneObjects(s);   // fold under the source's group (polyFinalize just built it top-level)
			}
			if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
		}
		return true;
	}
	const bool vertexTool = (s->polyShape == Scene::SH_Polygon || s->polyShape == Scene::SH_Polyline ||
	                         s->polyShape == Scene::SH_Line || s->polyShape == Scene::SH_Fault);
	// A tool armed a "point at a line" pick (Scene::vectorPickMode — Plates > Euler rotations'
	// "Pick in view" / "Rect select"). CLICK mode resolves the line under the cursor with the SAME
	// two hit tests, in the same order, the double-click edit path uses: a drawn Polygon first, then
	// an imported line Overlay. RECT mode collects the anchor on the first click and answers with
	// every line inside the box on the second. The click is consumed either way, so an armed pick can
	// never leak into a rotate/drag.
	if (button == 0 && s->vectorPickMode == 1) {
		// The tool asked for double-click picking: consume the press (VTK must not rotate under an
		// armed pick) and let polygonHandleDblClick fire the SAME resolver.
		if (s->vectorPickDbl) return true;
		return vectorPickFire(s, x, y);
	}
	// POINT pick (Shape detector's seed): answer with the world x,y under the cursor and stay armed —
	// "Pick multiple shapes" collects as many as the user clicks. The click is consumed either way.
	if (s->vectorPickMode == 3) {
		// RIGHT-click ends the collection (so does a double-click, see polygonHandleDblClick, and so
		// does pressing the tool's own button again) — the seeds gathered so far are handed over.
		if (button == 1) {
			auto endcb = s->seedPickEndCB;                        // copy: the tool disarms from inside
			if (endcb) { endcb(); return true; }
			return true;
		}
		if (button == 0) {
			double w[3];
			if (!polyPickWorld(s, x, y, w)) return true;          // off-surface click: ignore, stay armed
			auto cb = s->seedPickCB;                              // copy: see vectorPickFire
			if (cb) cb(w[0], w[1]);
			return true;
		}
	}
	if (button == 0 && s->vectorPickMode == 2) {
		double w[3];
		if (!polyPickWorld(s, x, y, w)) return true;             // off-surface click: ignore, stay armed
		if (!s->vectorPickDrawing) {                              // first click: drop the anchor
			s->vectorPickDrawing = true;
			s->vectorPickAnchor = { w[0], w[1], w[2] };
			// The rubber-band rectangle IS the draw tool's rectangle preview (polyRebuildPreview keys
			// off polyShape). polyMode is off while we pick, so polyShape is inert meanwhile; it is put
			// back the moment the pick ends.
			s->polyShape = Scene::SH_Rect;
			s->polyDrawing = true;
			s->polyCur.clear();
			s->polyCur.push_back(s->vectorPickAnchor);
			polyRebuildPreview(s, nullptr);
			s->widget->renderWindow()->Render();
			return true;
		}
		const double x0 = std::min(s->vectorPickAnchor[0], w[0]), x1 = std::max(s->vectorPickAnchor[0], w[0]);
		const double y0 = std::min(s->vectorPickAnchor[1], w[1]), y1 = std::max(s->vectorPickAnchor[1], w[1]);
		vectorPickDisarm(s);
		// Same rule as Mirone's push_rectSelect_CB: a line counts when ANY of its vertices is inside.
		std::string hits;
		auto add = [&hits](const std::string &nm) {
			if (nm.empty()) return;
			if (!hits.empty()) hits += '\n';
			hits += nm;
		};
		for (auto &pg : s->polys) {
			if (pg.isFault || pg.isSlip || pg.nestKind != 0) continue;
			for (auto &v : pg.v)
				if (v[0] >= x0 && v[0] <= x1 && v[1] >= y0 && v[1] <= y1) { add(pg.name); break; }
		}
		for (auto &ov : s->overlays) {
			if (ov.mode != 1 || !ov.actor || !ov.actor->GetVisibility()) continue;
			vtkPolyData *pd = ov.baseLine;
			if (!pd) {
				if (vtkPolyDataMapper *m = vtkPolyDataMapper::SafeDownCast(ov.actor->GetMapper())) pd = m->GetInput();
			}
			vtkPoints *pts = pd ? pd->GetPoints() : nullptr;
			if (!pts) continue;
			const vtkIdType np = pts->GetNumberOfPoints();
			for (vtkIdType i = 0; i < np; ++i) {
				double p[3]; pts->GetPoint(i, p);
				if (p[0] >= x0 && p[0] <= x1 && p[1] >= y0 && p[1] <= y1) { add(ov.name); break; }
			}
		}
		auto cb = s->vectorPickCB;                   // copy: see vectorPickFire
		if (cb) cb(hits);
		s->widget->renderWindow()->Render();
		return true;
	}
	if (button == 1) {                                   // right-click: undo last vertex (polygon/polyline)
		if (s->polyMode && s->polyDrawing && vertexTool) {
			if (!s->polyCur.empty()) s->polyCur.pop_back();
			if (s->polyCur.empty()) s->polyDrawing = false;
			polyRebuildPreview(s, nullptr);
			s->widget->renderWindow()->Render();
			return true;
		}
		return false;
	}
	if (s->polyMode) {                                   // draw mode: dispatch by active shape
		if (s->polyShape == Scene::SH_Text) {            // text lands on the XY plane (works off-surface too)
			double w[3];
			if (pickPlaneXY(s, x, y, w)) {
				// Open the modal dialog AFTER this press event fully returns (a nested modal loop inside
				// the handler eats the matching release). 0 ms timer defers it to the next loop turn.
				const std::array<double,3> wp = { w[0], w[1], w[2] };
				QTimer::singleShot(0, s->widget, [s, wp]() { polyPlaceText(s, wp.data()); });
			}
			// Do NOT consume: let VTK see a MATCHED press+release for this click (consuming only the
			// press fed the interactor an orphan release -> it stayed mid-drag and rotated once the tool
			// untoggled — the "crazy mouse"). The gizmo's guard aborts these events in text mode, so the
			// click never rotates/tilts despite reaching VTK.
			return false;
		}
		double w[3];
		if (!polyPickWorld(s, x, y, w))
			return true;                                 // consume even on a miss (no rotate while drawing)
		switch (s->polyShape) {
		case Scene::SH_Polygon:
		case Scene::SH_Polyline:                         // every left-click adds a vertex
			if (!s->polyDrawing) { s->polyDrawing = true; s->polyCur.clear(); }
			s->polyCur.push_back({ w[0], w[1], w[2] });
			polyRebuildPreview(s, nullptr);
			break;
		case Scene::SH_Line:                             // exactly two points: first click sets the start,
		case Scene::SH_Fault:                            // fault is a two-point line (Draw Fault tool)
			if (!s->polyDrawing) { s->polyDrawing = true; s->polyCur.clear(); }
			if (s->polyCur.size() >= 2) s->polyCur.pop_back();   // any later click just replaces the end point
			s->polyCur.push_back({ w[0], w[1], w[2] });
			polyRebuildPreview(s, nullptr);
			break;
		case Scene::SH_Rect:
		case Scene::SH_RectN:
		case Scene::SH_Circle:                           // two clicks: first sets the anchor, second finalizes
			if (!s->polyDrawing) {
				s->polyDrawing = true; s->polyCur.clear();
				s->polyCur.push_back({ w[0], w[1], w[2] });
			} else {
				std::vector<std::array<double,3>> corners;
				if (s->polyShape == Scene::SH_Circle) polyCircleCorners(s->polyCur[0].data(), w, corners);
				else                                  polyRectCorners(s->polyCur[0].data(), w, corners);
				const char *pre = s->polyShape == Scene::SH_Circle ? "circle"
				                : s->polyShape == Scene::SH_RectN   ? "Nested rectangle" : "rectangle";
				polyFinalize(s, corners, true, pre);
			}
			break;
		case Scene::SH_SymCircle:
		case Scene::SH_SymSquare:
		case Scene::SH_SymStar: {                        // symbols: ONE click places a NATIVE screen-constant
			                                              // glyph (SymbolLayer/vtkGlyph3D), not a drawn Polygon —
			                                              // never deforms on geographic maps (see addSymbols, 50).
			// 10 points, using the SAME px<->pt constant as the Size (points) property dialog
			// (symbolLayerMenu's liveSizeDialog, 50_scene.cpp: pxPerUnit = 96.0/72.0) — must match
			// exactly or the dialog wouldn't read back "10.0" for a symbol placed with this default.
			const double sizePx = 10.0 * (96.0 / 72.0);
			const char *sym = s->polyShape == Scene::SH_SymCircle ? "c"
			                : s->polyShape == Scene::SH_SymSquare ? "s" : "a";
			addSymbols(s, w, 1, sym, sizePx, 1, 1.0, 0.55, 0.0, 0.0, 0.0, 0.0, 1.0, "", nullptr, true);
			if (s->polyAct) s->polyAct->setChecked(false);   // one-shot tool, same as every other draw tool
			else            polygonSetMode(s, false);
			break;
		}
		case Scene::SH_Text: break;                      // handled above
		}
		s->widget->renderWindow()->Render();
		return true;
	}
	if (polyEditing(s)) {                                // edit mode (a drawn polygon OR an overlay line)
		if (shift) {                                     // Shift+drag: translate the WHOLE element (any grab point
			double w[3];                                 // on the line body or a vertex handle)
			int ovm = 1, ovs = -1;                       // grabbing the BODY of the overlay under edit counts too
			vtkActor *ovHit = (s->ovEdit >= 0) ? pickOverlayAt(s, x, y, ovm, &ovs) : nullptr;
			const bool onEditedOverlay = ovHit && s->ovEdit < (int)s->overlays.size() &&
			                             s->overlays[s->ovEdit].actor.Get() == ovHit && ovs == s->ovEditSeg;
			if (polyPickWorld(s, x, y, w) &&
			    (polyHitHandle(s, x, y, 10.0) >= 0 || polyHitPolygon(s, x, y, 10.0) >= 0 || onEditedOverlay)) {
				s->polyDragWhole = true;
				s->polyDragLastW[0] = w[0]; s->polyDragLastW[1] = w[1];
				s->widget->setCursor(Qt::SizeAllCursor); // same thick 4-arrow cross as the other drag ops
				return true;
			}
		}
		const int h = polyHitHandle(s, x, y, 10.0);      // no Shift: grab a single vertex handle to drag it
		if (h >= 0) { s->polyDragVert = h; return true; }
	}
	if (s->symArmed >= 0 && symHitHandle(s, x, y, 16.0)) {   // armed symbol: THIS press ARMS a possible
		s->symLayerDrag = s->symArmed;                        // drag (a separate, later gesture than the
		s->symDragPressX = x; s->symDragPressY = y;           // dblclick) — generous handle tolerance for the
		return true;                                          // hit-test, but see the move-threshold gate below
	}
	// Idle: grab a text label to drag it on the plane. Tested BEFORE the symbol-point pick below —
	// a city's name label sits only a small offset from its star, so their hit zones overlap, and
	// the label (visually on top, and what the user is actually clicking) must win that overlap.
	const int ti = polyHitText(s, x, y, 14.0);
	if (ti >= 0) { s->textDrag = ti; return true; }
	{
		int li = -1, pi = -1;
		double w0[3];
		// Idle: grab a single point in a BATCH symbol layer (e.g. a Cities star) to drag it, exactly
		// like mecaHitAt below grabs a beachball — no arm/double-click step (that flow is oneShot-only).
		if (pickSymbolPointAt(s, x, y, li, pi) && polyPickWorld(s, x, y, w0)) {
			s->symPtDrag = li; s->symPtIdx = pi;
			s->symPtDragLastW[0] = w0[0]; s->symPtDragLastW[1] = w0[1];
			return true;
		}
	}
	const int mi_ = mecaHitAt(s, x, y);                 // idle: grab a beachball to drag it (leaves an anchor line)
	if (mi_ >= 0) { s->mecaDrag = mi_; return true; }
	return false;                                        // otherwise let VTK navigate normally
}

// Double-click on an IMPORTED overlay line segment (a coastline island, a dropped .xy track, any
// GMTdataset drawn via gmtvtk_add_overlay_h) promotes just that segment into a real Polygon in
// s->polys and opens it in the SAME vertex-drag edit mode a drawn line gets (polyEnterEdit) —
// never a second, forked edit implementation (SACRED_LAW: one operation, one function). The
// segment's colour/width carry over; the rest of the overlay (other islands/segments) is untouched.
static void overlayPromoteSegmentToPolygon(Scene *s, Overlay &ov, int segIdx) {
	if (!s || segIdx < 0 || segIdx >= ov.nseg || !ov.baseLine || !ov.baseLine->GetPoints()) return;
	vtkPoints *pts = ov.baseLine->GetPoints();
	const int a = ov.segoff[segIdx], z = ov.segoff[segIdx + 1];
	if (z - a < 2) return;

	Polygon pg;
	for (int i = a; i < z; ++i) {
		double p[3]; pts->GetPoint(i, p);
		pg.v.push_back({ p[0], p[1], p[2] });
	}
	pg.closed = (pg.v.size() >= 3 && pg.v.front() == pg.v.back());
	double col[3]; ov.actor->GetProperty()->GetColor(col);
	const double lw = ov.actor->GetProperty()->GetLineWidth();
	const bool splitOff = ov.nseg > 1;             // more than one segment left in the overlay?
	vtkActor *ovActor = ov.actor.Get();

	const std::string pre = ov.name + " ";         // number per source, like polyFinalize's "polygon N"
	int idx = 1;
	for (auto &p : s->polys) if (p.name.rfind(pre, 0) == 0) ++idx;
	pg.name = splitOff ? (pre + std::to_string(idx)) : ov.name;
	// A piece taken out of a GROUPED overlay (a contour level, a plate-boundary type…) stays inside
	// that group: it folds under the same collapsible Scene Objects parent its source hangs from,
	// instead of appearing as a loose new top-level handle every double-click.
	pg.groupName = ov.groupName;

	polyRebuildLine(s, pg);
	pg.line->GetProperty()->SetColor(col[0], col[1], col[2]);   // keep the overlay's own look, not the default orange
	pg.line->GetProperty()->SetLineWidth(lw > 0.0 ? lw : 2.5);
	pg.stack = s->vecSeq++;
	s->polys.push_back(pg);
	const int newIdx = (int)s->polys.size() - 1;

	if (splitOff) {
		overlayRemoveSegment(s, ov, segIdx);       // ov reference still valid: only its OWN fields changed
		applyVectorStacking(s);
		rebuildSceneObjects(s);
	} else {
		overlayDelete(s, ovActor);                  // whole overlay consumed -> also stacks/rebuilds/renders
	}
	polyEnterEdit(s, newIdx);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Left double-click: close the polygon / end the polyline (draw mode) or enter/switch/leave edit
// mode (idle). Rectangle / circle / text finalize on their own clicks, so double-click is a no-op
// for them beyond being consumed while drawing.
static bool polygonHandleDblClick(Scene *s, int x, int y) {
	// A double-click-armed pick owns the double click before anything else can edit what it points at.
	if (s->vectorPickMode == 1 && s->vectorPickDbl) return vectorPickFire(s, x, y);
	// Seed collection (Shape detector): a double-click means "that's the last one". The press half of
	// this double-click already added its seed, which is what the user expects from the last click.
	if (s->vectorPickMode == 3) {
		auto endcb = s->seedPickEndCB;
		if (endcb) { endcb(); return true; }
		return true;
	}
	if (s->polyMode && s->polyDrawing) {
		if (s->polyShape == Scene::SH_Polygon && s->polyCur.size() >= 3) {   // >=3 vertices for an area
			polyFinalize(s, s->polyCur, true, "polygon");
			s->widget->renderWindow()->Render();
		} else if (s->polyShape == Scene::SH_Polyline && s->polyCur.size() >= 2) {  // >=2 for an open line
			polyFinalize(s, s->polyCur, false, "polyline");
			s->widget->renderWindow()->Render();
		} else if (s->polyShape == Scene::SH_Line && s->polyCur.size() >= 2) {      // two-point open line
			polyFinalize(s, s->polyCur, false, "line");
			s->widget->renderWindow()->Render();
		} else if (s->polyShape == Scene::SH_Fault && s->polyCur.size() >= 2) {     // two-point fault line
			polyFinalize(s, s->polyCur, false, "fault");
			s->widget->renderWindow()->Render();
		}
		return true;
	}
	if (!s->polyMode) {
		vtkActor *symAct = pickSymbolAt(s, x, y);        // native symbols: double-click ARMS it for dragging —
		if (symAct) {                                    // mirrors polyEdit exactly (persists across the
			const int si = symbolLayerIndexOfActor(s, symAct);   // dblclick's own release; a LATER, separate
			if (si >= 0 && s->symbols[si].oneShot) {      // press+drag on it is what actually moves it, see
				s->symArmed = (s->symArmed == si) ? -1 : si;   // polygonHandlePress/symArmed below). Toggle:
				symRebuildHandle(s);                            // double-click the SAME armed symbol disarms it.
				s->widget->renderWindow()->Render();            // show the yellow handle NOW (was missing —
				return true;                                    // silent arming looked like nothing happened)
			}
		}
		const int pi = polyHitPolygon(s, x, y, 8.0);
		if (pi >= 0) {
			if (pi == s->polyEdit) polyExitEdit(s);        // double-click the one being edited -> leave
			else                   polyEnterEdit(s, pi);   // any other drawn polygon -> switch/enter
			s->widget->renderWindow()->Render();
			return true;
		}
		// No drawn polygon hit: an imported overlay line (coastline island, dropped .xy track, a
		// contour, …) under the cursor? Edit it WHERE IT LIES — the handles hang off the overlay's own
		// points. Nothing is converted, copied or added: no new element, no new Scene Objects row, and
		// the line does not change identity under the user's hands. Same toggle as a drawn polygon:
		// double-clicking the one being edited leaves edit mode.
		int ovMode = 1, segIdx = -1;
		vtkActor *ovAct = pickOverlayAt(s, x, y, ovMode, &segIdx);
		if (ovAct && ovMode == 1 && segIdx >= 0) {
			for (int i = 0; i < (int)s->overlays.size(); ++i) {
				if (s->overlays[i].actor.Get() != ovAct) continue;
				if (s->ovEdit == i && s->ovEditSeg == segIdx) polyExitEdit(s);
				else                                         overlayEnterEdit(s, i, segIdx);
				s->widget->renderWindow()->Render();
				return true;
			}
		}
		if (polyEditing(s)) { polyExitEdit(s); s->widget->renderWindow()->Render(); return true; }
	}
	return false;
}

// Mouse move: extend the draw preview to the cursor, or drag the grabbed vertex / text label.
static bool polygonHandleMove(Scene *s, int x, int y) {
	// "Copy me" clone in flight: translate every vertex by the incremental cursor delta (same
	// pattern as polyDragWhole below) and redraw through the SAME polyRebuildLine every drawn
	// shape uses. Takes priority over everything else — nothing else may steal this gesture.
	if (s->copyDragging) {
		double w[3];
		if (polyPickWorld(s, x, y, w)) {
			const double ddx = w[0] - s->copyAnchorW[0], ddy = w[1] - s->copyAnchorW[1];
			s->copyAnchorW[0] = w[0]; s->copyAnchorW[1] = w[1];
			for (auto &v : s->copyGhost.v) { v[0] += ddx; v[1] += ddy; }
			polyRebuildLine(s, s->copyGhost);
		}
		s->widget->renderWindow()->Render();
		return true;
	}
	// An armed RECT vector pick (Scene::vectorPickMode == 2) rubber-bands with the SAME preview the
	// rectangle draw tool uses — one preview implementation, not a second one for selection.
	if (s->vectorPickMode == 2 && s->vectorPickDrawing) {
		double w[3];
		polyRebuildPreview(s, polyPickWorld(s, x, y, w) ? w : nullptr);
		s->widget->renderWindow()->Render();
		return true;
	}
	if (s->symLayerDrag >= 0 && s->symLayerDrag < (int)s->symbols.size()) {   // dragging a native symbol
		// A plain click (press+release near-in-place) has no minimum-movement gate below this point,
		// so ordinary mouse jitter between down and up would otherwise nudge the symbol's TRUE position
		// on every click — the "position drifts a little each time" bug. Require real movement past a
		// few px (same idea as Qt's own drag-start distance) before committing anything.
		const double ddx = x - s->symDragPressX, ddy = y - s->symDragPressY;
		if (ddx * ddx + ddy * ddy < 16.0)   // < 4px from the press point: not a real drag yet
			return true;
		double w[3];
		if (polyPickWorld(s, x, y, w)) {      // terrain-draped, same pick as the original placement click
			SymbolLayer &sl = s->symbols[s->symLayerDrag];
			if (auto *pd = symInputPD(sl)) {
				if (pd->GetPoints() && pd->GetPoints()->GetNumberOfPoints() > 0) {
					pd->GetPoints()->SetPoint(0, w[0] * s->xfac, w[1], w[2]);   // x pre-baked, matches addSymbols
					pd->GetPoints()->Modified();
					pd->Modified();
				}
			}
			symRebuildHandle(s);   // keep the yellow handle glued to the moving symbol
			s->widget->renderWindow()->Render();
		}
		return true;
	}
	if (s->symPtDrag >= 0 && s->symPtDrag < (int)s->symbols.size()) {   // dragging ONE point of a batch symbol
		double w[3];                                                     // layer (e.g. a Cities star)
		if (polyPickWorld(s, x, y, w)) {
			const double ddx = w[0] - s->symPtDragLastW[0], ddy = w[1] - s->symPtDragLastW[1];
			s->symPtDragLastW[0] = w[0]; s->symPtDragLastW[1] = w[1];
			SymbolLayer &sl = s->symbols[s->symPtDrag];
			if (auto *pd = symInputPD(sl)) {
				vtkPoints *pts = pd->GetPoints();
				if (pts && s->symPtIdx >= 0 && s->symPtIdx < pts->GetNumberOfPoints()) {
					double p[3]; pts->GetPoint(s->symPtIdx, p);
					p[0] += ddx * s->xfac; p[1] += ddy;
					pts->SetPoint(s->symPtIdx, p);
					pts->Modified(); pd->Modified();
				}
			}
			// Carry the linked name label along, SAME mechanism as mecaDragTo carrying a ball's date
			// label — found by (groupName, mecaEvent) instead of a cached pointer since s->texts can
			// reorder/erase (same reasoning as mecaDragTo's own re-find-by-actor).
			for (auto &tl : s->texts) {
				if (tl.groupName != sl.name || tl.mecaEvent != s->symPtIdx) continue;
				tl.pos[0] += ddx; tl.pos[1] += ddy;
				if (tl.actor) tl.actor->SetPosition(tl.pos[0] * s->xfac, tl.pos[1], 0.0);
				break;
			}
			s->widget->renderWindow()->Render();
		}
		return true;
	}
	if (s->mecaDrag >= 0 && s->mecaDrag < (int)s->mecaBalls.size()) {   // dragging a beachball across the XY plane
		double w[3];
		if (pickPlaneXY(s, x, y, w)) {
			mecaDragTo(s, s->mecaDrag, w[0], w[1]);
			s->widget->renderWindow()->Render();
		}
		return true;
	}
	if (s->textDrag >= 0) {                              // dragging a text label across the XY plane
		double w[3];
		if (pickPlaneXY(s, x, y, w) && s->textDrag < (int)s->texts.size()) {
			TextLabel &tl = s->texts[s->textDrag];
			tl.pos = { w[0], w[1], 0.0 };
			tl.actor->SetPosition(w[0] * s->xfac, w[1], 0.0);
			s->widget->renderWindow()->Render();
		}
		return true;
	}
	if (s->polyMode && s->polyDrawing) {
		double w[3];
		polyRebuildPreview(s, polyPickWorld(s, x, y, w) ? w : nullptr);
		s->widget->renderWindow()->Render();
		return true;
	}
	if (polyEditing(s) && s->polyDragWhole) {           // Shift+drag: translate every vertex by the cursor delta
		double w[3];
		EditVerts ev = editVerts(s);
		if (polyPickWorld(s, x, y, w) && ev.valid()) {
			const double ddx = w[0] - s->polyDragLastW[0], ddy = w[1] - s->polyDragLastW[1];
			s->polyDragLastW[0] = w[0]; s->polyDragLastW[1] = w[1];
			const int n = ev.n();
			for (int i = 0; i < n; ++i) {
				double p[3];  ev.get(i, p);
				p[0] += ddx;  p[1] += ddy;
				ev.set(i, p);
			}
			ev.commit(s);
			polyRebuildHandles(s);
			s->widget->renderWindow()->Render();
		}
		return true;
	}
	if (polyEditing(s) && s->polyDragVert >= 0) {
		double w[3];
		EditVerts ev = editVerts(s);
		if (ev.valid() && polyPickWorld(s, x, y, w)) {
			if (ev.pg && polyIsRect(*ev.pg)) {               // rectangle: keep it axis-aligned (carry the 2 neighbours)
				rectDragCorner(*ev.pg, s->polyDragVert, w[0], w[1]);
			} else {
				const bool ring = ev.closedRing();           // ask BEFORE the move: after it, first != last
				ev.set(s->polyDragVert, w);
				// For a CLOSED ring (v[0] == v[n-1]) moving vertex 0 must carry the closing point with it
				// so they never decouple. Open polylines have no dup.
				if (ring && s->polyDragVert == 0 && ev.n() >= 2)
					ev.set(ev.n() - 1, w);
			}
			ev.commit(s);
			polyRebuildHandles(s);
			s->widget->renderWindow()->Render();
		}
		return true;
	}
	return false;
}

// Left release: end a vertex / text-label drag.
static bool polygonHandleRelease(Scene *s) {
	if (s->symLayerDrag >= 0) { s->symLayerDrag = -1; return true; }
	if (s->symPtDrag    >= 0) { s->symPtDrag = -1; s->symPtIdx = -1; return true; }
	if (s->mecaDrag >= 0) { s->mecaDrag = -1; return true; }
	if (s->polyDragWhole) {                              // ended a Shift+drag whole-element move
		s->polyDragWhole = false;
		s->widget->unsetCursor();                       // drop the crosshair (hover logic restores as needed)
		if (s->polyEdit >= 0 && s->polyEdit < (int)s->polys.size() && s->polys[s->polyEdit].nestKind == 1)
			nestReflow(s);                              // nested rect: re-quantize + descendants (same as vertex drag)
		return true;
	}
	if (s->polyDragVert >= 0) {
		s->polyDragVert = -1;
		// Edited a nested rectangle: re-quantize it (back to an axis-aligned, snapped rect) + descendants.
		if (s->polyEdit >= 0 && s->polyEdit < (int)s->polys.size() && s->polys[s->polyEdit].nestKind == 1)
			nestReflow(s);
		return true;
	}
	if (s->textDrag     >= 0) { s->textDrag     = -1; return true; }
	return false;
}
