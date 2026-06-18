// ============================================================================
//  Polygon draw / edit tool (toolbar pentagon button).
//
//  DRAW MODE (button checked): left-click adds a vertex, right-click removes the
//  last one, double-left-click closes the polygon (>=3 vertices). A live rubber
//  preview trails the cursor while drawing.
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

// Pentagon outline icon for the toolbar button (drawn, no asset file).
static QIcon makePolygonIcon() {
	QPixmap pm(24, 24);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(40, 40, 40), 1.8));
	p.setBrush(QColor(255, 200, 120));
	QPolygonF poly;                                   // regular pentagon centred in the icon
	const double cx = 12, cy = 13, r = 9;
	for (int k = 0; k < 5; ++k) {
		const double a = -vtkMath::Pi() / 2 + k * 2 * vtkMath::Pi() / 5;
		poly << QPointF(cx + r * std::cos(a), cy + r * std::sin(a));
	}
	p.drawPolygon(poly);
	p.end();
	return QIcon(pm);
}

// Cursor (mx,my device px) -> a point ON the scene, returned in TRUE coords. Mirrors the hover
// ray-cast in onMouseMove: march the unprojected ray against the grid heightfield (sampleZ), the
// flat image plane, or the FV/point cellpicker. Returns false if the ray misses the scene.
static bool polyPickWorld(Scene* s, int mx, int my, double outTrue[3]) {
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
		auto eval = [&](double t, double& fval) -> bool {
			const double X = nr[0] + t*dirx, Y = nr[1] + t*diry, Z = nr[2] + t*dirz;
			const double h = sampleZ(s, X / gx, Y);
			if (std::isnan(h)) return false;
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
				if (std::isnan(outTrue[2])) outTrue[2] = 0.0;
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
static void polyToDisplay(Scene* s, const std::array<double,3>& v, double d[2]) {
	s->ren->SetWorldPoint(v[0]*s->xfac, v[1], v[2]*s->zfac*s->ve, 1.0);
	s->ren->WorldToDisplay();
	const double* dp = s->ren->GetDisplayPoint();
	d[0] = dp[0]; d[1] = dp[1];
}

// Common look for a 3-D polyline / preview actor sitting above the relief.
static vtkSmartPointer<vtkActor> polyMakeLineActor(Scene* s, vtkPolyData* pd, double r, double g, double b) {
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
static void polyFillLine(vtkPolyData* pd, const std::vector<std::array<double,3>>& verts, bool closed) {
	vtkNew<vtkPoints> pts;
	for (auto& v : verts) pts->InsertNextPoint(v[0], v[1], v[2]);
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
static void polyDrapeCorners(Scene* s, const std::vector<std::array<double,3>>& corners,
							 std::vector<std::array<double,3>>& out) {
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
		const auto& a = corners[i];
		const auto& b = corners[i + 1];
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
	out.push_back(corners[m - 1]);                  // the final corner
}

// Rebuild the finished-polygon actor `pg` from its vertices. pg.v is a closed ring (first == last);
// each edge is draped on the relief so the outline hugs the terrain.
static void polyRebuildLine(Scene* s, Polygon& pg) {
	if (!pg.linePD) pg.linePD = vtkSmartPointer<vtkPolyData>::New();
	std::vector<std::array<double,3>> draped;
	polyDrapeCorners(s, pg.v, draped);
	polyFillLine(pg.linePD, draped, false);
	if (!pg.line) {
		pg.line = polyMakeLineActor(s, pg.linePD, 1.0, 0.55, 0.0);   // finished polygons: orange
		s->ren->AddActor(pg.line);
	}
}

// Refresh the in-progress draw preview from polyCur, optionally trailing a segment to `cursor`.
static void polyRebuildPreview(Scene* s, const double* cursor) {
	if (!s->polyPreviewPD) s->polyPreviewPD = vtkSmartPointer<vtkPolyData>::New();
	std::vector<std::array<double,3>> verts = s->polyCur;
	if (cursor) verts.push_back({ cursor[0], cursor[1], cursor[2] });
	if (verts.size() >= 2) verts.push_back(verts.front());   // close the ring so the closing edge drapes too
	std::vector<std::array<double,3>> draped;
	polyDrapeCorners(s, verts, draped);
	polyFillLine(s->polyPreviewPD, draped, false);   // already a closed, draped ring
	if (!s->polyPreview) {
		s->polyPreview = polyMakeLineActor(s, s->polyPreviewPD, 1.0, 0.85, 0.2);   // drawing: yellow
		s->ren->AddActor(s->polyPreview);
	}
	s->polyPreview->SetVisibility(verts.size() >= 2 ? 1 : 0);
}

// Rebuild the square vertex handles shown for the edited polygon (polyEdit).
static void polyRebuildHandles(Scene* s) {
	if (!s->polyHandlePD) s->polyHandlePD = vtkSmartPointer<vtkPolyData>::New();
	vtkNew<vtkPoints> pts;
	vtkNew<vtkCellArray> verts;
	if (s->polyEdit >= 0 && s->polyEdit < (int)s->polys.size()) {
		auto& v = s->polys[s->polyEdit].v;
		// v is a closed ring (first == last) -> drop the duplicate closing point so one corner
		// gets one handle (dragging vertex 0 carries the closing point with it).
		const int m = (v.size() >= 2 && v.front() == v.back()) ? (int)v.size() - 1 : (int)v.size();
		for (int i = 0; i < m; ++i) {
			const vtkIdType id = pts->InsertNextPoint(v[i][0], v[i][1], v[i][2]);
			verts->InsertNextCell(1, &id);
		}
	}
	s->polyHandlePD->SetPoints(pts);
	s->polyHandlePD->SetVerts(verts);
	s->polyHandlePD->Modified();
	if (!s->polyHandles) {
		vtkNew<vtkPolyDataMapper> map; map->SetInputData(s->polyHandlePD); map->ScalarVisibilityOff();
		vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
		map->SetRelativeCoincidentTopologyPointOffsetParameter(-9000.0);   // handles on top of the line
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
	s->polyHandles->SetVisibility(s->polyEdit >= 0 ? 1 : 0);
}

static void polyEnterEdit(Scene* s, int idx) {
	s->polyEdit = idx;
	s->polyDragVert = -1;
	polyRebuildHandles(s);
}

static void polyExitEdit(Scene* s) {
	s->polyEdit = -1;
	s->polyDragVert = -1;
	polyRebuildHandles(s);
}

// Index of the finished polygon whose outline (any edge, incl. the closing one) passes within
// `tol` px of (x,y) display px, or -1. Topmost (last drawn) wins.
static int polyHitPolygon(Scene* s, int x, int y, double tol) {
	const double tol2 = tol * tol;
	for (int pi = (int)s->polys.size() - 1; pi >= 0; --pi) {
		auto& v = s->polys[pi].v;
		const int n = (int)v.size();
		if (n < 2) continue;
		for (int i = 0; i < n; ++i) {
			double a[2], b[2];
			polyToDisplay(s, v[i], a);
			polyToDisplay(s, v[(i + 1) % n], b);
			if (segDist2((double)x, (double)y, a, b) <= tol2) return pi;
		}
	}
	return -1;
}

// Index of the edited polygon's vertex within `tol` px of (x,y), or -1.
static int polyHitHandle(Scene* s, int x, int y, double tol) {
	if (s->polyEdit < 0 || s->polyEdit >= (int)s->polys.size()) return -1;
	const double tol2 = tol * tol;
	auto& v = s->polys[s->polyEdit].v;
	const int m = (v.size() >= 2 && v.front() == v.back()) ? (int)v.size() - 1 : (int)v.size();
	int best = -1; double bestd = tol2;
	for (int i = 0; i < m; ++i) {
		double d[2]; polyToDisplay(s, v[i], d);
		const double dx = d[0] - x, dy = d[1] - y, dd = dx*dx + dy*dy;
		if (dd <= bestd) { bestd = dd; best = i; }
	}
	return best;
}

// Close the current draw into a finished polygon (>=3 vertices). The stored ring is explicitly
// closed (a copy of vertex 0 is appended so first == last), it is listed in the Scene Objects
// panel as "polygon N", and the draw tool then ends (button untoggled -> arrow cursor).
static void polyFinalize(Scene* s, const std::vector<std::array<double,3>>& verts) {
	Polygon pg; pg.v = verts;
	pg.v.push_back(pg.v.front());          // close the ring (first == last)
	pg.name = "polygon " + std::to_string((int)s->polys.size() + 1);
	polyRebuildLine(s, pg);
	s->polys.push_back(pg);
	s->polyCur.clear();
	s->polyDrawing = false;
	if (s->polyPreview) s->polyPreview->SetVisibility(0);
	rebuildSceneObjects(s);                // add the "polygon N" row to the Scene Objects list
	// Finishing ends the draw session: untoggle the toolbar button (-> polygonSetMode(false),
	// which restores the arrow cursor and clears draw state). Falls back if there's no button.
	if (s->polyAct) s->polyAct->setChecked(false);
	else            polygonSetMode(s, false);
}

// Remove a finished polygon (identified by its line actor): drop the actor, erase it, fix the
// edit index, and refresh the Scene Objects list. Called from the unified line menu's "Delete".
static void polygonDelete(Scene* s, vtkActor* lineActor) {
	for (int i = 0; i < (int)s->polys.size(); ++i) {
		if (s->polys[i].line.Get() != lineActor) continue;
		if (s->ren && s->polys[i].line) s->ren->RemoveActor(s->polys[i].line);
		if (s->polyEdit == i)      polyExitEdit(s);      // was being edited -> drop the handles
		else if (s->polyEdit > i)  s->polyEdit--;        // keep the edit index valid past the erase
		s->polys.erase(s->polys.begin() + i);
		rebuildSceneObjects(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return;
	}
}

// Toolbar toggle: enter/leave draw mode. Switching cancels any in-progress draw and edit.
static void polygonSetMode(Scene* s, bool on) {
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

// --- Qt-level mouse handlers (called from GLView, 60_profile.cpp) -------------------------
// VTK's interactor adapter does NOT deliver Qt double-clicks as a second LeftButtonPress, so the
// draw/edit gestures are driven from the GLView widget overrides instead. Each returns true when
// it consumed the event (the widget then skips the VTK base handler, so the gizmo never rotates).
// (x,y) are VTK display px (device, bottom-up), matching polyPickWorld / the projection helpers.

// Left/right press. button: 0 = left, 1 = right.
static bool polygonHandlePress(Scene* s, int button, int x, int y) {
	if (button == 1) {                                   // right-click: undo last vertex while drawing
		if (s->polyMode && s->polyDrawing) {
			if (!s->polyCur.empty()) s->polyCur.pop_back();
			if (s->polyCur.empty()) s->polyDrawing = false;
			polyRebuildPreview(s, nullptr);
			s->widget->renderWindow()->Render();
			return true;
		}
		return false;
	}
	if (s->polyMode) {                                   // draw mode: every left-click adds a vertex
		double w[3];
		if (polyPickWorld(s, x, y, w)) {
			if (!s->polyDrawing) { s->polyDrawing = true; s->polyCur.clear(); }
			s->polyCur.push_back({ w[0], w[1], w[2] });
			polyRebuildPreview(s, nullptr);
			s->widget->renderWindow()->Render();
		}
		return true;                                     // consume even on a miss (no rotate while drawing)
	}
	if (s->polyEdit >= 0) {                              // edit mode: grab a vertex handle to drag it
		const int h = polyHitHandle(s, x, y, 10.0);
		if (h >= 0) { s->polyDragVert = h; return true; }
	}
	return false;                                        // otherwise let VTK navigate normally
}

// Left double-click: close the polygon (draw mode) or enter/switch/leave edit mode (idle).
static bool polygonHandleDblClick(Scene* s, int x, int y) {
	if (s->polyMode && s->polyDrawing) {
		if (s->polyCur.size() >= 3) {                    // need >=3 vertices for an area
			polyFinalize(s, s->polyCur);
			s->widget->renderWindow()->Render();
		}
		return true;
	}
	if (!s->polyMode) {
		const int pi = polyHitPolygon(s, x, y, 8.0);
		if (s->polyEdit >= 0) {
			if (pi >= 0 && pi != s->polyEdit) polyEnterEdit(s, pi);   // double-click another -> switch
			else                              polyExitEdit(s);        // off the handles -> leave edit
			s->widget->renderWindow()->Render();
			return true;
		}
		if (pi >= 0) { polyEnterEdit(s, pi); s->widget->renderWindow()->Render(); return true; }
	}
	return false;
}

// Mouse move: extend the draw preview to the cursor, or drag the grabbed vertex.
static bool polygonHandleMove(Scene* s, int x, int y) {
	if (s->polyMode && s->polyDrawing) {
		double w[3];
		polyRebuildPreview(s, polyPickWorld(s, x, y, w) ? w : nullptr);
		s->widget->renderWindow()->Render();
		return true;
	}
	if (s->polyEdit >= 0 && s->polyDragVert >= 0) {
		double w[3];
		if (polyPickWorld(s, x, y, w)) {
			Polygon& pg = s->polys[s->polyEdit];
			pg.v[s->polyDragVert] = { w[0], w[1], w[2] };
			const int n = (int)pg.v.size();
			// The ring is closed (v[0] == v[n-1]). Moving vertex 0 must carry the closing point with
			// it so they never decouple. (Check AFTER assigning v[0]: comparing front()==back() here
			// would already be false, which is the bug that let them split.)
			if (s->polyDragVert == 0 && n >= 2)
				pg.v[n - 1] = pg.v[0];
			polyRebuildLine(s, pg);
			polyRebuildHandles(s);
			s->widget->renderWindow()->Render();
		}
		return true;
	}
	return false;
}

// Left release: end a vertex drag.
static bool polygonHandleRelease(Scene* s) {
	if (s->polyDragVert >= 0) { s->polyDragVert = -1; return true; }
	return false;
}
