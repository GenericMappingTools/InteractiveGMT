// ============================================================================
//  C API — the Julia bridge. Self-contained; ccall these from Julia.
// ============================================================================
#if defined(_WIN32)
#  define GMTVTK_API extern "C" __declspec(dllexport)
#else
#  define GMTVTK_API extern "C"
#endif

// Earth metrics for geographic grids.
static const double kMetersPerDegLat = 111111.0;

// Work out the base actor scales + an initial vertical exaggeration from the data
// extents. The displayed VE factor is relative to TRUE scale (VE 1 = 1:1), but if
// the true relief is < 10% of the horizontal size we start exaggerated so the
// surface is not a flat sheet.
//   geographic: x,y are degrees -> xfac = cos(midlat) makes the lon axis the right
//     physical width vs lat; zfac = 1/111111 converts z (assumed metres) into the
//     lat-degree base unit so VE 1 is physically true.
//   cartesian:  xfac = zfac = 1 (z assumed in the same unit as x,y).
// The 10% floor also rescues non-metre z (the assumption may be wrong): whatever
// the unit, the relief is forced to a visible fraction of the footprint.
static void computeScales(int geographic, double x0, double x1, double y0, double y1,
						  double zmin, double zmax,
						  double &xfac, double &zfac, double &ve0) {
	double widthM, heightM;
	if (geographic) {
		const double midlat  = 0.5 * (y0 + y1);
		const double mDegLon = kMetersPerDegLat * std::cos(midlat * vtkMath::Pi() / 180.0);
		xfac = std::max(1e-6, mDegLon / kMetersPerDegLat);   // = cos(midlat)
		zfac = 1.0 / kMetersPerDegLat;
		widthM  = std::abs(x1 - x0) * mDegLon;
		heightM = std::abs(y1 - y0) * kMetersPerDegLat;
	}
	else {
		xfac = 1.0; zfac = 1.0;
		widthM  = std::abs(x1 - x0);
		heightM = std::abs(y1 - y0);
	}
	const double Hm = std::max(widthM, heightM);
	const double zspanM = zmax - zmin;
	// Auto aspect fit: keep the relief between 10% and 100% of the footprint so the
	// surface is neither a flat sheet nor a vertical needle.
	double fit = 1.0;
	if (zspanM > 0.0 && Hm > 0.0) {
		if (zspanM < 0.10 * Hm)      fit = 0.10 * Hm / zspanM;   // too flat -> raise
		else if (zspanM > Hm)        fit = Hm / zspanM;          // too tall -> shrink
	}
	if (geographic) {
		// Geographic z is physical metres: VE 1 must mean true 1:1, so the fit is the
		// DISPLAYED starting exaggeration (the gizmo factor), zfac stays the unit conversion.
		ve0 = fit;
	}
	else {
		// Cartesian z is an arbitrary unit: "true scale" is meaningless. Fold the fit into
		// the base zfac and start the displayed VE at 1, so the gizmo / VE dialog operate
		// in their comfortable 0.01..1e4 range around 1 instead of around a tiny ve0 that
		// the 0.01 VE floor would snap back up into a needle.
		zfac = fit; ve0 = 1.0;
	}
}

// View a GMT.jl grid (non-blocking; pump gmtvtk_process_events to run the loop).
// `z` is the grid's column-major matrix (ny rows x nx cols, element (iy,ix) at
// z[ix*ny + iy]); (x0,x1,y0,y1) is its data range (y ascending). `geographic`!=0
// treats x,y as degrees (z assumed metres). NaN nodes are skipped.
// Returns an opaque figure handle (the Scene*); pass it to gmtvtk_add_overlay_h to add
// elements to THIS window later. The handle is valid until the window is closed.
GMTVTK_API void* gmtvtk_view_grid(const float *z, int nx, int ny, double x0, double x1, double y0, double y1, int geographic,
								 const double *cz, const double *crgb, int ncolor, const unsigned char *img,
								 int iw, int ih, int ibands, int edges, int triangulate, int image_only, const char* title) {
	double zmin = 0.0, zmax = 1.0;
	// Plain CPT grid (no drape, not a bare image) -> TILED render: no giant single polydata; the
	// tile actors are built in buildAndShow from z. Drape / image_only keep the single-polydata
	// path. Tiled path still needs the global z range (LUT + Z axis) -> one cheap scan, no alloc.
	const bool tiled = (img == nullptr) && (image_only == 0);
	vtkSmartPointer<vtkPolyData> pd;
	if (tiled) {
		zmin = 1e30; zmax = -1e30;
		for (vtkIdType k = 0, ntot = (vtkIdType)nx * ny; k < ntot; ++k) {
			const float zz = z[k];
			if (!std::isnan(zz)) { if (zz < zmin) zmin = zz; if (zz > zmax) zmax = zz; }
		}
		if (zmin > zmax) { zmin = 0.0; zmax = 1.0; }
	} else {
		pd = makeGridFromArray(z, nx, ny, x0, x1, y0, y1, zmin, zmax, triangulate != 0, /*wantTC=*/img != nullptr);
	}
	double xfac, zfac, ve0;
	computeScales(geographic, x0, x1, y0, y1, zmin, zmax, xfac, zfac, ve0);
	Scene *s = buildAndShow(pd, x0, x1, y0, y1, zmin, zmax, xfac, zfac, ve0, cz, crgb, ncolor, img, iw, ih, ibands, edges, false, geographic, title,
	                        /*objname=*/nullptr, /*imageOnly=*/image_only != 0,
	                        /*gz=*/tiled ? z : nullptr, /*gnx=*/nx, /*gny=*/ny);
	if (s && !image_only && !tiled) {                  // drape path: keep full-res z for hover/profile
		s->gridZ.assign(z, z + (size_t)nx * ny);       // (tiled path already populated it in buildAndShow)
		s->gnx = nx; s->gny = ny;
		s->gx0 = x0; s->gx1 = x1; s->gy0 = y0; s->gy1 = y1;
	}
	if (s && image_only) {
		// Bare image (imageOnly already set inside buildAndShow before the panel was built, so the
		// Scene Objects list has no "Surface"/"Image drape" rows). Open straight into a top-down
		// orthographic map. image_only==1 -> REFERENCED image: keep the X/Y (lon/lat) axes and
		// leave a margin so their labels stay on screen. image_only==2 -> PLAIN image (no georef):
		// hide the axes entirely and fill the viewport edge-to-edge.
		vtkCamera *cam = s->ren->GetActiveCamera();
		double fp[3]; cam->GetFocalPoint(fp);
		cam->SetViewUp(0.0, 1.0, 0.0);
		cam->SetPosition(fp[0], fp[1], fp[2] + 1.0);
		cam->ParallelProjectionOn();
		if (image_only == 2) {                 // plain image: no axes, maximize edge-to-edge
			s->axes->SetVisibility(0);
			fitSnapView(s, /*topMode=*/true);
		} else {                               // referenced image: keep X/Y axes, leave a margin
			s->axes->SetZAxisVisibility(0);
			s->axes->DrawZGridlinesOff();
			fitSnapView(s, /*topMode=*/true, /*fill=*/0.84);
		}
		// A bare image IS a 2D map: enter flat-2D so the toolbar/menu "Flat 2D" button shows
		// pressed and drag-rotation is locked. We're already in the top-down ortho view, so don't
		// re-run the enter-2D camera setup — just set the flag and prime the saved 3D state used
		// when the user later toggles to 3D (perspective view of the flat textured plane).
		s->flat2d = true;
		cam->GetPosition(s->sav_pos);
		cam->GetFocalPoint(s->sav_foc);
		s->sav_vup[0] = 0.0; s->sav_vup[1] = 1.0; s->sav_vup[2] = 0.0;
		s->sav_parallel = 0;        // 3D = perspective
		s->sav_ve       = s->ve;
		s->sav_surfLit  = false;    // image plane: unlit albedo
		if (s->giz) setGizmoVisible(*s->giz, false);
		if (s->act2D) s->act2D->setChecked(true);
		s->widget->renderWindow()->Render();
	}
	return s;
}

// View a point cloud (non-blocking). `xyz` = npts (x,y,z) triples in true data coords;
// the points are coloured by z through the CPT control nodes (cz[i] -> crgb[i], rgb 0..1;
// 0 = built-in ramp). (x0,x1,y0,y1) is the data range; geographic!=0 treats x,y as degrees
// (z metres). `pointsize` in px (<=0 = default). Ctrl+right-drag selects points (box marquee,
// toggle, Ctrl+Z undo); pick(r,g,b) is the highlight colour for the selected points.
// Returns the figure handle (Scene*); read the selection back with gmtvtk_get_selection.
GMTVTK_API void *gmtvtk_view_points(const double* xyz, int npts,
									const double* cz, const double* crgb, int ncolor,
									double x0, double x1, double y0, double y1, int geographic,
									double pointsize, double pickr, double pickg, double pickb,
									const char* title) {
	if (!xyz || npts <= 0)
		return nullptr;
	double zmin = 0.0, zmax = 1.0;
	auto pd = makePointCloud(xyz, npts, zmin, zmax);
	double xfac, zfac, ve0;
	computeScales(geographic, x0, x1, y0, y1, zmin, zmax, xfac, zfac, ve0);
	Scene *s = buildAndShow(pd, x0, x1, y0, y1, zmin, zmax, xfac, zfac, ve0,
							cz, crgb, ncolor, nullptr, 0, 0, 0, 0, true, geographic, title);
	if (!s)
		return nullptr;
	// Render as round points of the requested size (the Verts-only polydata draws as points).
	// Lighting OFF: buildAndShow set a PBR material (for surfaces); shading N lit sphere
	// impostors per frame makes rotate/zoom crawl on big clouds. Flat CPT-coloured points
	// (round via the sphere impostor mask, but unlit) render far faster and read the same.
	s->surf->GetProperty()->SetRepresentationToPoints();
	s->surf->GetProperty()->SetPointSize(pointsize > 0.0 ? pointsize : 4.0);
	s->surf->GetProperty()->SetRenderPointsAsSpheres(false);  // plain GL_POINTS = one fast draw (sphere impostors are fill-heavy)
	s->surf->GetProperty()->LightingOff();
	s->surf->GetProperty()->SetInterpolationToFlat();
	// Bypass buildAndShow's vtkPolyDataNormals stage (useless for unlit points) -> the mapper
	// draws the cloud polydata directly.
	if (auto* m = vtkPolyDataMapper::SafeDownCast(s->surf->GetMapper()))
		m->SetInputData(pd);
	// Ctrl+right-drag rubber-band selection over this cloud.
	enableRubberBand(s, pd, pickr, pickg, pickb);
	s->widget->renderWindow()->Render();
	return s;
}

// View an arbitrary GMTfv mesh (solids / polygons; non-blocking). `xyz` = nv vertex triples
// (true coords); `nfaces` polygon cells given by corner counts sides[nfaces] + flat 0-based
// corner ids indices[sum(sides)]. `facergb` (nfaces*3, 0..255) gives flat per-face colours;
// pass null + CPT nodes (cz[ncolor]/crgb[ncolor*3], 0 = built-in ramp) to colour per-vertex by
// z. (x0,x1,y0,y1,z0,z1) is the data bbox (z0,z1 label the Z axis). geographic!=0 -> lon/lat
// axis titles. zscale = vertical exaggeration (GMTfv.zscale; <=0 -> 1). edges!=0 draws cell
// wires (toggle live with 'e'). Returns the figure handle (Scene*), valid until the window closes.
GMTVTK_API void* gmtvtk_view_fv(const double* xyz, int nv, const int* sides, int nfaces,
								const int* indices, const unsigned char* facergb, const double* facez,
								const double* cz, const double* crgb, int ncolor,
								double x0, double x1, double y0, double y1, double z0, double z1,
								int geographic, double zscale, int edges, const char* title,
								const char* objname) {
	if (!xyz || nv <= 0 || !sides || !indices || nfaces <= 0)
		return nullptr;
	double zmin, zmax;
	auto pd = makeFvMesh(xyz, nv, sides, nfaces, indices, facergb, facez, zmin, zmax);
	// Three colouring modes:
	//   direct : explicit per-face RGB (categorical) -> direct cell colours, NO colorbar.
	//   cellz  : per-face z scalar through the CPT/CTF -> faceted colours that MATCH the colorbar.
	//   else   : per-vertex z (smooth) through the CPT -> matches the colorbar (grid-like).
	const bool   direct = (facergb != nullptr);
	const bool   cellz  = (!direct && facez != nullptr);
	const int    nc = direct ? 0 : ncolor;           // direct face colours override any CPT
	const double ve = (zscale > 0.0) ? zscale : 1.0; // GMTfv.zscale already resolves the exaggeration
	// objname (named solid e.g. "Torus") labels the Scene Objects checkbox; buildAndShow sets it
	// BEFORE the panel is built so the checkbox is created once with the right name (no overlap).
	Scene *s = buildAndShow(pd, x0, x1, y0, y1, z0, z1, 1.0, 1.0, ve,
							direct ? nullptr : cz, direct ? nullptr : crgb, nc,
							nullptr, 0, 0, 0, edges, false, geographic, title, objname);
	if (!s)
		return nullptr;
	if (direct || cellz) {
		// Faceted normals (sharp solid edges, split at >30deg), replacing buildAndShow's
		// smooth-normal surface path. Kept alive by the mapper's input connection.
		vtkNew<vtkPolyDataNormals> fn;
		fn->SetInputData(pd);
		fn->SplittingOn();
		fn->SetFeatureAngle(30.0);
		fn->ConsistencyOn();
		if (auto* m = vtkPolyDataMapper::SafeDownCast(s->surf->GetMapper())) {
			m->SetInputConnection(fn->GetOutputPort());
			m->SetScalarModeToUseCellData();          // colour per FACE (flat), not per vertex
			// CRITICAL for CELL data: buildAndShow turned InterpolateScalarsBeforeMapping ON (a
			// POINT-data optimisation — it bakes the LUT into a texture indexed by per-POINT
			// tcoords). Cell scalars have NO per-point tcoord, so on some GPUs those cells sample
			// the texture border and render GREY ("grey top row"). Per-cell flat colours MUST map
			// directly through the LUT, not via the texture -> turn it OFF.
			m->InterpolateScalarsBeforeMappingOff();
			if (direct) {
				m->SetColorModeToDirectScalars();     // RGB straight from the cell array
				if (s->bar) setColorbarVisible(s, false);  // explicit colours have no z legend
			}
			else {
				m->SetColorModeToMapScalars();        // face-z through the CTF = same as the bar
				// THE GREY-TOP-ROW BUG: faces at the max z sit AT the colormap's upper limit.
				// With clamping off (or via the ISBM texture border) a value at/above the top
				// node maps to grey instead of the top colour -> a grey ring on the torus crest
				// (the grid hid it: only one peak vertex hits the limit). Force the CTF to CLAMP
				// so above-range == top colour, below-range == bottom colour, NEVER grey.
				if (auto* ctf = vtkColorTransferFunction::SafeDownCast(m->GetLookupTable()))
					ctf->SetClamping(1);
				m->UseLookupTableScalarRangeOn();     // map through the CTF's own [zmin,zmax] node range
			}
			m->ScalarVisibilityOn();
			m->Modified();
		}
		// Matte (no specular) so the data colour reads true, not a glossy sheen. (This was NOT the
		// grey-cell cause — that was the cell-data texture path above — but a colormap mesh still
		// reads better matte.) applyShading honours s->matteSurf on every re-apply.
		s->matteSurf = true;
		applyShading(s);
		s->widget->renderWindow()->Render();
	}
	return s;
}

// Number of points currently selected (Ctrl+right-drag) in a point-cloud figure.
GMTVTK_API int gmtvtk_selection_count(void* handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	return (int)s->rbSel.size();
}

// Copy up to `n` selected point ids (0-based, into the cloud passed to view_points) into
// `out`. Returns the number copied.
GMTVTK_API int gmtvtk_get_selection(void* handle, int* out, int n) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !out)
		return 0;
	int k = 0;
	for (vtkIdType id : s->rbSel) {
		if (k >= n) break;
		out[k++] = (int)id;
	}
	return k;
}

// Add a GMTdataset overlay to the most-recent window (call right after gmtvtk_view_grid).
// `xyz` = npts triples (x,y,z) in true data coords; `segoff` = nseg+1 segment offsets;
// mode 0 = points, 1 = polylines. rgb in 0..1; linewidth/pointsize in px (<=0 = default).
GMTVTK_API void gmtvtk_add_overlay(const double *xyz, int npts, const int *segoff, int nseg,
								   int mode, double r, double g, double b,
								   double linewidth, double pointsize) {
	addOverlay(g_lastScene, xyz, npts, segoff, nseg, mode, r, g, b, linewidth, pointsize);
}

// Add an overlay to a SPECIFIC window by its handle (from gmtvtk_view_grid). Lets the
// host add points/lines to an existing figure that is no longer the most-recent one.
// A stale handle (window already closed) is silently ignored. Returns 1 if added, 0 if
// the handle is dead. addOverlay re-renders, so the new elements appear immediately.
GMTVTK_API int gmtvtk_add_overlay_h(void *handle, const double *xyz, int npts, const int *segoff, int nseg,
								   int mode, double r, double g, double b,
								   double linewidth, double pointsize, const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	addOverlay(s, xyz, npts, segoff, nseg, mode, r, g, b, linewidth, pointsize, name);
	return 1;
}

// Add a screen-constant SYMBOL layer to a window by its handle: `npts` (x,y,z) triples in TRUE
// coords, one GMT symbol code (sym: "c" circle "s" square "t" triangle "i" inv-triangle "d" diamond
// "h" hexagon "n" pentagon "g" octagon "a" star "x" cross "+" plus "-" dash), `sizePx` = on-screen
// size in PIXELS (caller converts points->px), `filled` 0/1, (fr,fg,fb) fill + (er,eg,eb) edge rgb
// 0..1, `edgeWidth` outline px. Symbols stay a constant pixel size at any zoom. Returns 1 if added.
// `info` (or null) = optional per-point hover text: npts records joined by RS ('\x1e'), each a
// ready-to-show multi-line block (lines separated by '\n'). Adopted only if it has exactly npts
// records; the viewer then pops the matching block as a tooltip when the cursor is over a symbol.
GMTVTK_API int gmtvtk_add_symbols_h(void *handle, const double *xyz, int npts, const char *sym,
                                    double sizePx, int filled,
                                    double fr, double fg, double fb,
                                    double er, double eg, double eb, double edgeWidth,
                                    const char *name, const char *info) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	return addSymbols(s, xyz, npts, std::string(sym ? sym : "c"), sizePx, filled,
	                  fr, fg, fb, er, eg, eb, edgeWidth, std::string(name ? name : ""), info);
}

// Add a vertical image "curtain" to a window by its handle (from gmtvtk_view_grid). The
// wall follows the XY track (px,py); `u` = per-column horizontal texture coord (0..1);
// `topz` (or null) clips each column top to a surface (else flat top at zmax). img is
// row-major, row 0 = BOTTOM of the picture, w*h pixels of `comps` (3=RGB, 4=RGBA). zmin/zmax
// are the TRUE vertical extent (same z units as the grid). flipv!=0 inverts the image's
// vertical sense. Returns 1 if added, 0 if the handle is dead.
GMTVTK_API int gmtvtk_add_curtain_h(void *handle, const double *px, const double *py, const double *u, int n,
									const double *topz, const unsigned char *img, int w, int h, int comps,
									double zmin, double zmax, int flipv) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	auto tex = makeBufferTexture(img, w, h, comps);
	if (!tex)
		return 0;
	addCurtain(s, px, py, u, n, topz, tex, zmin, zmax, flipv);
	return 1;
}

// Same, but the curtain image is read from a FILE by VTK (JPEG/PNG/TIFF/...). The host
// passes a path instead of a packed buffer, sidestepping image-layout ambiguity. Returns
// 1 if added, 0 if the handle is dead or the image could not be read.
GMTVTK_API int gmtvtk_add_curtain_file_h(void *handle, const double *px, const double *py, const double *u, int n,
										 const double *topz, const char *imgpath,
										 double zmin, double zmax, int flipv) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	auto tex = makeFileTexture(imgpath);
	if (!tex)
		return 0;
	addCurtain(s, px, py, u, n, topz, tex, zmin, zmax, flipv);
	return 1;
}

// Fill the Data Viewer spreadsheet tab of a window (by handle) with table data, and bring
// that tab forward. `data` is COLUMN-MAJOR (Julia layout): element (row r, col c) lives at
// data[(size_t)c*nrows + r]. `headers`, if non-null/non-empty, is the column names joined by
// TAB ('\t'), one per column (missing/empty -> "C1, C2, ..."). `name` (or null) labels the
// tab. Returns 1 if shown, 0 if the handle is dead. The data is copied into the table.
GMTVTK_API int gmtvtk_set_table(void* handle, const char* name, const double* data,
								int nrows, int ncols, const char* headers) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !s->dataTable)
		return 0;
	if (nrows < 0) nrows = 0;
	if (ncols < 0) ncols = 0;
	QTableWidget *t = s->dataTable;
	t->clearContents();
	t->setColumnCount(ncols);
	t->setRowCount(nrows);

	QStringList hdr;
	if (headers && headers[0])
		hdr = QString::fromUtf8(headers).split('\t');
	for (int c = 0; c < ncols; ++c) {
		QString h = (c < hdr.size() && !hdr[c].isEmpty()) ? hdr[c] : QString("C%1").arg(c + 1);
		t->setHorizontalHeaderItem(c, new QTableWidgetItem(h));
	}
	if (data) {
		for (int c = 0; c < ncols; ++c) {
			for (int r = 0; r < nrows; ++r) {
				double v = data[(size_t)c * nrows + r];
				QTableWidgetItem *it = new QTableWidgetItem(QString::number(v, 'g', 8));
				it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
				t->setItem(r, c, it);
			}
		}
	}
	t->resizeColumnsToContents();

	if (s->bottomTabs) {
		int idx = s->bottomTabs->indexOf(t);
		if (idx >= 0)
			s->bottomTabs->setTabText(idx, (name && name[0]) ? QString("Data: %1").arg(QString::fromUtf8(name))
															  : QString("Data Viewer"));
		if (s->bottomDock) s->bottomDock->setVisible(true);
		setBottomCollapsed(s, false);
		if (idx >= 0) s->bottomTabs->setCurrentIndex(idx);
	}
	return 1;
}

// Is a figure handle still live (its window open)?  1 = yes, 0 = closed/invalid.
GMTVTK_API int gmtvtk_is_alive(void *handle) {
	return sceneAlive(static_cast<Scene*>(handle)) ? 1 : 0;
}

// Bring an existing window to the front. Used when the host detects a file is already open and,
// instead of opening a duplicate, raises the window that already shows it.
GMTVTK_API void gmtvtk_raise(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !s->win) return;
	s->win->setWindowState(s->win->windowState() & ~Qt::WindowMinimized);
	s->win->showNormal();
	s->win->raise();
	s->win->activateWindow();
}

// Does this window have a primary surface? 0 for a bare empty() launcher (no data yet); used by
// the drop handler to decide between PROMOTING an empty window vs adding into a populated one.
GMTVTK_API int gmtvtk_has_surface(void* handle) {
	Scene *s = static_cast<Scene*>(handle);
	// emptyStart launcher carries a HIDDEN placeholder surf only -> report "no surface" so a dropped
	// file PROMOTES into a fresh full window (and the launcher is retired) rather than adding in.
	return (sceneAlive(s) && s->surf && !s->emptyStart) ? 1 : 0;
}

// Read-only introspection for the test suite: serialize the window's scene state into `buf` as a
// single semicolon-separated list of key=value tokens (split on ';', then on the first '='). Returns
// the FULL length (excluding the NUL); if it is >= cap the caller's buffer was too small (re-call
// with a bigger one). NEVER mutates the scene — purely a snapshot. The reported `axes` flag is
// whether the cube axes are actually IN the renderer (an empty launcher carries the axes object but
// does NOT add it), so it doubles as the "coordinate grid present" invariant. Each extra object is
// emitted as extraN=kind:name (kind = image | grid). No-op (returns 0) on a dead handle.
GMTVTK_API int gmtvtk_scene_state(void* handle, char* buf, int cap) {
	Scene *s = static_cast<Scene*>(handle);
	std::string o;
	char t[96];
	auto kvi = [&](const char* k, long v) { o += k; o += '='; o += std::to_string(v); o += ';'; };
	auto kvd = [&](const char* k, double v) { snprintf(t, sizeof(t), "%s=%.10g;", k, v); o += t; };
	const bool alive = sceneAlive(s);
	kvi("alive", alive ? 1 : 0);
	if (alive) {
		const int axesShown = (s->axes && s->ren && s->ren->HasViewProp(s->axes)) ? 1 : 0;
		kvi("has_surface", (s->surf && !s->emptyStart) ? 1 : 0);
		kvi("emptyStart",  s->emptyStart ? 1 : 0);
		kvi("imageOnly",   s->imageOnly ? 1 : 0);
		kvi("flat2d",      s->flat2d ? 1 : 0);
		kvi("axes",        axesShown);
		kvi("crs",         s->hasCRS() ? 1 : 0);
		kvd("x0", s->x0); kvd("x1", s->x1); kvd("y0", s->y0); kvd("y1", s->y1);
		kvd("zmin", s->zmin); kvd("zmax", s->zmax);
		kvi("n_extras",   (long)s->extras.size());
		kvi("n_overlays", (long)s->overlays.size());
		kvi("n_curtains", (long)s->curtains.size());
		kvi("n_polys",    (long)s->polys.size());
		kvi("n_texts",    (long)s->texts.size());
		kvi("drape",      s->drape ? 1 : 0);
		kvi("n_table",    s->dataTable ? (long)s->dataTable->rowCount() : -1);
		o += "surf_name="; o += s->surfName; o += ';';
		for (size_t i = 0; i < s->extras.size(); ++i) {
			o += "extra" + std::to_string((int)i) + '=';
			o += (s->extras[i].isImage ? "image:" : "grid:");
			o += s->extras[i].name; o += ';';
		}
	}
	const int n = (int)o.size();
	if (buf && cap > 0) { int c = (n < cap - 1) ? n : cap - 1; memcpy(buf, o.data(), c); buf[c] = '\0'; }
	return n;
}

// Recolour a live surface from a new CPT: cz[n] boundary z + crgb[n*3] (0..1). s->surfLut is always
// a vtkColorTransferFunction, shared by the surface mapper, every LOD tile mapper and the colorbar,
// so mutating its nodes in place recolours all of them at once. Called from Julia (_recolor) after
// the colormap chooser picks a name. No-op on a bare image (no surfLut/colorbar).
GMTVTK_API void gmtvtk_set_cpt(void* handle, const double* cz, const double* crgb, int n) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !cz || !crgb || n < 2) return;
	vtkColorTransferFunction* ctf = vtkColorTransferFunction::SafeDownCast(s->surfLut);
	if (!ctf) return;                       // only the CTF path supports live recolour
	ctf->RemoveAllPoints();
	for (int i = 0; i < n; ++i)
		ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
	s->surfCtfRange = true;
	if (s->bar) s->bar->SetLookupTable(s->surfLut);   // refresh the legend strip
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Set the window's coordinate reference system (all three interchangeable forms — PROJ4 / WKT /
// EPSG). Julia resolves them via GMT.jl and pushes them here right after the window opens. Storing
// any of them marks the data as referenced and reveals the Geography menu (hidden by default since
// it needs a reference frame); an all-empty CRS hides it again.
GMTVTK_API void gmtvtk_set_crs(void* handle, const char* proj4, const char* wkt, int epsg) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	s->crsProj4 = proj4 ? proj4 : "";
	s->crsWkt   = wkt   ? wkt   : "";
	s->crsEpsg  = epsg;
	if (s->geoMenu) s->geoMenu->menuAction()->setVisible(s->hasCRS());
}

// Close a window programmatically (WA_DeleteOnClose -> destroy + bookkeeping). Used to retire an
// empty launcher once a dropped file has been promoted into a full viewer window.
GMTVTK_API void gmtvtk_close(void* handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (sceneAlive(s) && s->win) s->win->close();
}

// Register a freshly-opened file in the persistent Recent Files list (File > Recent Files).
// Called from Julia after a successful open. cat: 0 = grid, 1 = image, 2 = dataset.
GMTVTK_API void gmtvtk_add_recent(const char* path, int cat) {
	addRecentFile(path, cat);
}

// Standalone demo surface (MATLAB peaks) — no host data needed.
GMTVTK_API void gmtvtk_view_demo(void) {
	double zmin = 0.0, zmax = 1.0;
	auto pd = makeGridSurface(140, 140, -3, 3, -3, 3, zmin, zmax);
	double xfac, zfac, ve0;
	computeScales(0, -3, 3, -3, 3, zmin, zmax, xfac, zfac, ve0);
	buildAndShow(pd, -3, 3, -3, 3, zmin, zmax, xfac, zfac, ve0, nullptr, nullptr, 0,
				 nullptr, 0, 0, 0, 0, false, 0, "i'GMT  —  demo");
}

// Register the Julia eval callback used by the in-window console dock. `fn` is a Julia
// @cfunction (signature JuliaEvalFn). Pass nullptr to detach. Called once after dlopen.
GMTVTK_API void gmtvtk_set_julia_eval(JuliaEvalFn fn) {
	g_juliaEval = fn;
}

// Register the file-drop callback. `fn` is a Julia @cfunction (signature JuliaDropFn) called
// with each dropped file's path; Julia reads + views it. Pass nullptr to detach.
GMTVTK_API void gmtvtk_set_drop_callback(JuliaDropFn fn) {
	g_juliaDrop = fn;
}

// Register the basemap-picker callback. `fn` (Julia @cfunction, signature JuliaBaseMapFn) is called
// with a clicked tile's geographic region "W/E/S/N/wrap"; Julia crops data/etopo4.jpg + adds it.
GMTVTK_API void gmtvtk_set_basemap_callback(JuliaBaseMapFn fn) {
	g_juliaBaseMap = fn;
}

// Set the path to the world logo image painted in the basemap picker (data/etopo4_logo.jpg).
GMTVTK_API void gmtvtk_set_basemap_logo(const char* path) {
	g_basemapLogo = QString::fromUtf8(path ? path : "");
}

// Register the Geography-menu callback. `fn` (Julia @cfunction, signature JuliaGeoFn) is called
// with "<kind>/<res>/W/E/S/N" (the visible region at the current zoom) when a Plot-coastline leaf
// is chosen; Julia runs GMT.coast and adds the lines via gmtvtk_add_overlay_h. nullptr to detach.
GMTVTK_API void gmtvtk_set_geography_callback(JuliaGeoFn fn) {
	g_juliaGeo = fn;
}

// Prepare an EMPTY launcher to receive geographic IMAGE objects as ExtraObj images. The basemap
// must NOT promote its first tile into the window's "surface" (that row has no image-properties
// menu); instead every tile is an ExtraObj image listed in Scene Objects with the same menu. This
// recomputes the window scales from the image bbox + switches it to a flat-2-D geographic map, then
// the caller adds the image via gmtvtk_add_surface_h and calls gmtvtk_fit2d. No-op (0) on a window
// that already has data (caller then just adds the image on top, keeping the current view).
GMTVTK_API int gmtvtk_frame_for_image_h(void* handle, double x0, double x1, double y0, double y1,
                                        int geographic) {
	Scene* s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !s->emptyStart) return 0;
	double zmin = 0.0, zmax = 1.0, xfac, zfac, ve0;
	computeScales(geographic, x0, x1, y0, y1, zmin, zmax, xfac, zfac, ve0);
	s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1; s->zmin = zmin; s->zmax = zmax;
	s->xfac = xfac; s->zfac = zfac; s->ve = ve0;
	s->imageOnly  = true;             // a basemap is a 2-D map: no colorbar, no surface row
	s->emptyStart = false;            // real data from here on -> images add via the normal path
	if (s->surf) s->surf->SetVisibility(0);                 // keep the launcher placeholder hidden
	s->axes->SetVisibility(1);
	s->axes->SetZAxisVisibility(0); s->axes->DrawZGridlinesOff();
	return 1;
}

// Frame the camera to the scene in flat-2-D top-down. Called after adding image objects into a
// launcher prepared by gmtvtk_frame_for_image_h (the normal add path does not auto-frame because
// the hidden placeholder still counts as a surface). Sets the shared "Flat 2D" state + button.
GMTVTK_API void gmtvtk_fit2d(void* handle) {
	Scene* s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	vtkCamera* cam = s->ren->GetActiveCamera();
	double fp[3]; cam->GetFocalPoint(fp);
	cam->SetViewUp(0.0, 1.0, 0.0);
	cam->SetPosition(fp[0], fp[1], fp[2] + 1.0);
	cam->ParallelProjectionOn();
	fitSnapView(s, /*topMode=*/true);
	s->flat2d = true;
	if (s->giz) setGizmoVisible(*s->giz, false);
	if (s->act2D) s->act2D->setChecked(true);
	s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Grow the flat geographic frame to also cover [x0,x1]x[y0,y1] (a basemap window getting a 2nd/3rd
// tile outside the current extent). The base reference plane + cube axes are pinned to the surface
// bounds (s->x0..y1 + the flat z=0 placeholder grid), so a tile added beyond them left the axes
// frozen and the hover readout dead outside the first tile's box (sampleZ off-grid -> no hit).
// We union the bbox, rebuild ONLY the base plane + axes via buildSceneContent (self-cleaning of
// surface/axes/colorbar — it leaves s->extras/overlays untouched, so already-added tiles survive),
// keep xfac UNCHANGED (so the existing image actors stay aligned), then refit the flat-2-D view.
// No-op (0) on a non-flat or already-covering window. Geographic-only (basemap use).
GMTVTK_API int gmtvtk_grow_frame_h(void* handle, double x0, double x1, double y0, double y1) {
	Scene* s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || s->emptyStart) return 0;
	const double nx0 = std::min(s->x0, x0), nx1 = std::max(s->x1, x1);
	const double ny0 = std::min(s->y0, y0), ny1 = std::max(s->y1, y1);
	if (nx0 == s->x0 && nx1 == s->x1 && ny0 == s->y0 && ny1 == s->y1)
		return 0;                                   // new tile already inside the frame -> nothing to do
	s->x0 = nx0; s->x1 = nx1; s->y0 = ny0; s->y1 = ny1;   // xfac/zfac/ve kept -> extras stay aligned
	float zblank[4] = { 0, 0, 0, 0 };               // flat z=0 reference plane over the grown union
	buildSceneContent(s, nullptr, nx0, nx1, ny0, ny1, nullptr, nullptr, 0, nullptr, 0, 0, 0,
					  /*edges=*/0, /*pointCloud=*/false, /*geographic=*/1,
					  zblank, 2, 2, /*blankStart=*/false);
	surfSetVisibility(s, 0);                         // the z=0 plane is a scaffold (bounds + hover) only:
	                                                 // hide it so it never shows under/around the tiles
	vtkCamera* cam = s->ren->GetActiveCamera();      // back to the top-down flat-2-D map view
	s->axes->SetZAxisVisibility(0); s->axes->DrawZGridlinesOff();
	double fp[3]; cam->GetFocalPoint(fp);
	cam->SetViewUp(0.0, 1.0, 0.0);
	cam->SetPosition(fp[0], fp[1], fp[2] + 1.0);
	cam->ParallelProjectionOn();
	fitSnapView(s, /*topMode=*/true);
	s->flat2d = true;
	if (s->giz) setGizmoVisible(*s->giz, false);
	s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// Hide the window's base surface plane (keeping its geometry for axis bounds + hover sampling).
// Used by the basemap path: the promoted flat z=0 plane is only scaffold under the draped tile, so
// hiding it stops a coloured plane peeking out when the tile is toggled off. Bounds/readout unaffected.
GMTVTK_API void gmtvtk_hide_surface(void* handle) {
	Scene* s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	surfSetVisibility(s, 0);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Open an EMPTY viewer window: a FULL-chrome launcher (menus + toolbar + 2-D map) that simply has
// no data yet -> a blank dark canvas, exactly as if a (still unloaded) image were about to show.
// It is built through buildAndShow (imageOnly => no colorbar) on a tiny placeholder plane that we
// then hide, so the window carries the real UI; drop a file (or use the toolbar Open button) to
// load data, which PROMOTES into a fresh full window (emptyStart -> gmtvtk_has_surface reports 0).
GMTVTK_API void *gmtvtk_open_empty(const char* title) {
	double zmin = 0.0, zmax = 1.0;
	const double x0 = 0.0, x1 = 1.0, y0 = 0.0, y1 = 1.0;
	float z[4] = {0, 0, 0, 0};
	auto pd = makeGridFromArray(z, 2, 2, x0, x1, y0, y1, zmin, zmax, /*triangulate=*/true);
	double xfac, zfac, ve0;
	computeScales(0, x0, x1, y0, y1, zmin, zmax, xfac, zfac, ve0);
	Scene *s = buildAndShow(pd, x0, x1, y0, y1, zmin, zmax, xfac, zfac, ve0, nullptr, nullptr, 0,
	                        nullptr, 0, 0, 0, 0, false, 0,
	                        title ? title : "i'GMT  —  drop a file",
	                        /*objname=*/nullptr, /*imageOnly=*/true,
	                        /*gz=*/nullptr, /*gnx=*/0, /*gny=*/0, /*blankStart=*/true);
	if (!s)
		return nullptr;
	s->emptyStart = true;                    // hidden placeholder only -> drop promotes to a real window
	// (Scene Objects dock already starts folded with no flash, done in buildAndShow's blankStart block.)
	if (s->surf) s->surf->SetVisibility(0);  // hide the placeholder plane -> blank canvas
	s->axes->SetVisibility(0);               // no axes until there is data
	if (s->giz) setGizmoVisible(*s->giz, false);

	// Same flat-2D state a bare image opens in (top-down ortho, drag-rotation locked, "Flat 2D"
	// button pressed), with the saved 3-D state primed for a later toggle.
	vtkCamera *cam = s->ren->GetActiveCamera();
	double fp[3]; cam->GetFocalPoint(fp);
	cam->SetViewUp(0.0, 1.0, 0.0);
	cam->SetPosition(fp[0], fp[1], fp[2] + 1.0);
	cam->ParallelProjectionOn();
	fitSnapView(s, /*topMode=*/true);
	s->flat2d = true;
	cam->GetPosition(s->sav_pos);
	cam->GetFocalPoint(s->sav_foc);
	s->sav_vup[0] = 0.0; s->sav_vup[1] = 1.0; s->sav_vup[2] = 0.0;
	s->sav_parallel = 0;
	s->sav_ve       = s->ve;
	s->sav_surfLit  = false;
	if (s->act2D) s->act2D->setChecked(true);

	s->win->statusBar()->showMessage("Drop a grid / image / table file here, or use the Open button");
	// Force VISIBLE even when the process was launched hidden (desktop shortcut via wscript style 0
	// sets STARTUPINFO SW_HIDE, which the first top-level window would otherwise inherit).
	s->win->setWindowState(s->win->windowState() & ~Qt::WindowMinimized);
	s->win->showNormal();
	s->win->raise();
	s->win->activateWindow();
	s->widget->renderWindow()->Render();
	return s;
}

// Add a grid/image dropped into an EXISTING window: build a CPT-coloured surface (or, with an
// image buffer, additionally a flat textured drape) actor, register it in the Scene Objects
// panel, and render. Aligns with the window's base scale (xfac/zfac/VE). `name` labels the row.
// Returns 1 on success, 0 if the handle is dead / inputs invalid.
GMTVTK_API int gmtvtk_add_surface_h(void* handle, const float* z, int nx, int ny,
									double x0, double x1, double y0, double y1,
									const double* cz, const double* crgb, int ncolor,
									const unsigned char* img, int iw, int ih, int ibands,
									int image_only, const char* name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !z || nx < 2 || ny < 2)
		return 0;
	double zmin = 0.0, zmax = 1.0;
	auto pd = makeGridFromArray(z, nx, ny, x0, x1, y0, y1, zmin, zmax, true);

	vtkNew<vtkPolyDataNormals> norms;
	norms->SetInputData(pd);
	norms->SetFeatureAngle(90.0); norms->SplittingOff(); norms->ConsistencyOn();

	ExtraObj ex;
	const bool hasImg = (img && iw > 0 && ih > 0 && ibands > 0);
	if (image_only && hasImg) {
		// A dropped IMAGE: no elevation, so it must NOT sit at z=0 slicing the relief. It rides a
		// horizontal plane that defaults to ON TOP of the surface (z = zmax + a small gap) and can be
		// re-ordered / draped via its Scene Objects properties menu (imageObjectMenu). The texture is
		// kept on the ExtraObj so the actor can be rebuilt flat<->draped without re-uploading pixels.
		vtkNew<vtkImageData> tex_img;
		tex_img->SetDimensions(iw, ih, 1);
		tex_img->AllocateScalars(VTK_UNSIGNED_CHAR, ibands);
		memcpy(tex_img->GetScalarPointer(), img, (size_t)iw * ih * ibands);
		ex.tex = vtkSmartPointer<vtkTexture>::New();
		ex.tex->SetInputData(tex_img); ex.tex->InterpolateOn();
		ex.isImage = true;
		ex.bx0 = x0; ex.bx1 = x1; ex.by0 = y0; ex.by1 = y1;
		ex.zpos = s->zmax + imageStackStep(s);     // default: sit just above the relief, never at z=0
		imageRebuildActor(s, ex);                  // builds ex.actor (flat plane) + adds it to the renderer
	} else {
		// A grid: CPT-coloured surface (+ optional image drape on top).
		vtkSmartPointer<vtkScalarsToColors> lut;
		bool ctfRange = false;
		if (cz && crgb && ncolor > 0) {
			vtkNew<vtkColorTransferFunction> ctf;
			for (int i = 0; i < ncolor; ++i) ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
			lut = ctf; ctfRange = true;
		}
		else {
			vtkNew<vtkLookupTable> t;
			t->SetHueRange(0.667, 0.0); t->SetNumberOfTableValues(256); t->SetRampToLinear();
			t->SetTableRange(zmin, zmax); t->Build(); lut = t;
		}
		vtkNew<vtkPolyDataMapper> map;
		map->SetInputConnection(norms->GetOutputPort());
		map->SetLookupTable(lut); map->SetScalarRange(zmin, zmax);
		if (ctfRange) map->UseLookupTableScalarRangeOn();
		map->ScalarVisibilityOn(); map->InterpolateScalarsBeforeMappingOn();
		ex.actor = vtkSmartPointer<vtkActor>::New();
		ex.actor->SetMapper(map);
		ex.actor->GetProperty()->SetInterpolationToPBR();
		ex.actor->GetProperty()->SetMetallic(0.0);
		ex.actor->GetProperty()->SetRoughness(0.45);
		ex.actor->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		s->ren->AddActor(ex.actor);
		if (hasImg) {                                // grid + drape image on top
			vtkNew<vtkImageData> tex_img;
			tex_img->SetDimensions(iw, ih, 1);
			tex_img->AllocateScalars(VTK_UNSIGNED_CHAR, ibands);
			memcpy(tex_img->GetScalarPointer(), img, (size_t)iw * ih * ibands);
			vtkNew<vtkTexture> tex; tex->SetInputData(tex_img); tex->InterpolateOn();
			vtkNew<vtkPolyDataMapper> dmap;
			dmap->SetInputConnection(norms->GetOutputPort());
			dmap->ScalarVisibilityOff();
			vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
			dmap->SetRelativeCoincidentTopologyPolygonOffsetParameters(-1.0, -1.0);
			ex.drape = vtkSmartPointer<vtkActor>::New();
			ex.drape->SetMapper(dmap); ex.drape->SetTexture(tex);
			ex.drape->GetProperty()->LightingOff();
			ex.drape->SetScale(s->xfac, 1.0, s->zfac * s->ve);
			s->ren->AddActor(ex.drape);
		}
	}

	ex.name = (name && name[0]) ? name : ("Object " + std::to_string((int)s->extras.size() + 1));
	s->extras.push_back(ex);
	rebuildSceneObjects(s);
	if (!s->surf && s->extras.size() == 1)   // first content dropped into an empty window: frame it
		s->ren->ResetCamera();
	s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// PROMOTE an empty launcher window into a real grid/image window IN PLACE: the SAME window is
// reused (no new window, the old one is NOT destroyed). We recompute the scales from the dropped
// data and rebuild the scene through buildSceneContent — the EXACT same data-build path a fresh
// view_grid uses — so there is nothing to reproduce and nothing to drift. If the window already
// has a surface (not an empty launcher) we just add the surface as an extra. Returns 1 / 0.
GMTVTK_API int gmtvtk_promote_surface_h(void* handle, const float* z, int nx, int ny,
										double x0, double x1, double y0, double y1, int geographic,
										const double* cz, const double* crgb, int ncolor,
										const unsigned char* img, int iw, int ih, int ibands,
										int image_only, const char* name) {
	Scene* s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !z || nx < 2 || ny < 2)
		return 0;
	if (!s->emptyStart)   // already a real window -> ordinary "add into existing window"
		return gmtvtk_add_surface_h(handle, z, nx, ny, x0, x1, y0, y1,
									cz, crgb, ncolor, img, iw, ih, ibands, image_only, name);

	const bool hasImg    = (img && iw > 0 && ih > 0 && ibands > 0);
	const bool imageOnly = (image_only != 0);

	// z range + scales from the REAL data (the launcher's were for the 0..1 placeholder).
	double zmin = 1e30, zmax = -1e30;
	for (vtkIdType k = 0, ntot = (vtkIdType)nx * ny; k < ntot; ++k) {
		const float zz = z[k];
		if (!std::isnan(zz)) { if (zz < zmin) zmin = zz; if (zz > zmax) zmax = zz; }
	}
	if (zmin > zmax) { zmin = 0.0; zmax = 1.0; }
	double xfac, zfac, ve0;
	computeScales(geographic, x0, x1, y0, y1, zmin, zmax, xfac, zfac, ve0);
	s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1; s->zmin = zmin; s->zmax = zmax;
	s->xfac = xfac; s->zfac = zfac; s->ve = ve0;
	s->imageOnly = imageOnly;
	s->surfName  = (name && name[0]) ? name : "";

	// Plain grid -> TILED path (gz), exactly like gmtvtk_view_grid. Draped image -> single actor
	// with tcoords (pd) so the texture can sit on it. buildSceneContent removes the launcher's
	// placeholder content and rebuilds everything (surface, axes, colorbar, default 3-D view, ...).
	vtkSmartPointer<vtkPolyData> pd;
	const float* gz = nullptr; int gnx = 0, gny = 0;
	if (hasImg) {
		double zlo = zmin, zhi = zmax;
		pd = makeGridFromArray(z, nx, ny, x0, x1, y0, y1, zlo, zhi, /*triangulate=*/true, /*wantTC=*/true);
	} else {
		gz = z; gnx = nx; gny = ny;
	}
	buildSceneContent(s, pd, x0, x1, y0, y1, cz, crgb, ncolor, img, iw, ih, ibands,
					  /*edges=*/0, /*pointCloud=*/false, geographic, gz, gnx, gny, /*blankStart=*/false);

	// The single-actor (drape) path needs full-res z for hover/profile/pick; the tiled path already
	// populated s->gridZ inside buildSceneContent.
	if (!gz) {
		s->gridZ.assign(z, z + (size_t)nx * ny);
		s->gnx = nx; s->gny = ny;
		s->gx0 = x0; s->gx1 = x1; s->gy0 = y0; s->gy1 = y1;
		s->gdx = (nx > 1) ? (x1 - x0) / (nx - 1) : 0.0;
		s->gdy = (ny > 1) ? (y1 - y0) / (ny - 1) : 0.0;
	}

	s->emptyStart = false;

	// Rebuild the gizmo from scratch against the REAL surface, the SAME way a fresh window does (its
	// haxisLen is measured from the visible surface bounds inside enableGizmo). The launcher's gizmo
	// was calibrated for the 0..1 placeholder, and hand re-keying it kept yielding a giant horizontal
	// axis — tearing it down and recreating gives exactly the normal-grid gizmo, no special-casing.
	disableGizmo(s);
	s->giz = enableGizmo(s, 0.01);

	// A grid opens in the default oblique 3-D view buildSceneContent already set. A bare image IS a
	// 2-D map, so drop it into top-down flat-2D (matching gmtvtk_view_grid's image branch).
	vtkCamera* cam = s->ren->GetActiveCamera();
	if (imageOnly) {
		s->axes->SetZAxisVisibility(0); s->axes->DrawZGridlinesOff();
		double fp[3]; cam->GetFocalPoint(fp);
		cam->SetViewUp(0.0, 1.0, 0.0);
		cam->SetPosition(fp[0], fp[1], fp[2] + 1.0);
		cam->ParallelProjectionOn();
		fitSnapView(s, /*topMode=*/true);
		s->flat2d = true;
		cam->GetPosition(s->sav_pos); cam->GetFocalPoint(s->sav_foc);
		s->sav_vup[0] = 0.0; s->sav_vup[1] = 1.0; s->sav_vup[2] = 0.0;
		s->sav_parallel = 0; s->sav_ve = s->ve; s->sav_surfLit = false;
		if (s->giz) setGizmoVisible(*s->giz, false);
		// NB: setChecked WITHOUT a signal blocker so the toolbar 2D/3D icon (driven by act2D::toggled)
		// refreshes. The flat-2D toggle is on act2D::triggered, not toggled, so there is no re-entrancy.
		if (s->act2D) s->act2D->setChecked(true);
	} else {
		s->flat2d = false;
		if (s->act2D) s->act2D->setChecked(false);   // emits toggled -> updates the 2D/3D toolbar icon to "3D"
		// gizmo already rebuilt and visible by enableGizmo above.
		// The empty launcher's Shading dock was created HIDDEN (no body to light back then). Now there
		// IS a surface, so re-show it FOLDED to the side strip — exactly the state a fresh grid opens in,
		// and what the Surface row click then un-folds. Without this the dock stays permanently hidden
		// when a grid is opened via Recent Files / drop into the launcher.
		if (s->shadeDock && s->shadeFoldBar) {
			if (QWidget* body = s->shadeDock->widget()) body->setVisible(false);
			s->shadeFoldBar->folded    = true;
			s->shadeFoldBar->openWidth = 240;
			s->shadeFoldBar->updateGeometry();
			s->shadeFoldBar->update();
			s->shadeDock->setVisible(true);
			if (s->win)
				s->win->resizeDocks({s->shadeDock}, {s->shadeFoldBar->sizeHint().width()}, Qt::Horizontal);
		}
	}

	rebuildSceneObjects(s);
	applyShading(s);
	s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// Pump the Qt event loop once. Returns the number of viewer windows still open
// (0 = all closed; the host can stop pumping).
GMTVTK_API int gmtvtk_process_events(void) {
	if (g_app) g_app->processEvents();
	return g_openWindows;
}

// Save a PNG of the most-recent window (for verification/offscreen capture).
GMTVTK_API int gmtvtk_save_png(const char* path) {
	if (!g_lastRW) return 0;
	vtkNew<vtkWindowToImageFilter> w2i;
	w2i->SetInput(g_lastRW);
	w2i->ShouldRerenderOff();      // capture the DISPLAYED frame, not a clean re-render
	w2i->ReadFrontBufferOn();      // -> GPU-specific artifacts (e.g. grey cells) are visible
	w2i->Update();
	vtkNew<vtkPNGWriter> wr;
	wr->SetFileName(path);
	wr->SetInputConnection(w2i->GetOutputPort());
	wr->Write();
	return 1;
}

// Orbit a window's camera (azimuth/elevation degrees) + re-render. For scripted verification
// from a chosen view (the interactive user does this with the mouse).
GMTVTK_API void gmtvtk_orbit(void* handle, double az, double el, double zoom) {
	Scene* s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	vtkCamera* cam = s->ren->GetActiveCamera();
	cam->Azimuth(az);
	cam->Elevation(el);
	cam->OrthogonalizeViewUp();
	if (zoom > 0.0) cam->Zoom(zoom);
	s->ren->ResetCameraClippingRange();
	s->widget->renderWindow()->Render();
}

// Toggle red/cyan ANAGLYPH stereo on the window. on=1 enable, on=0 disable, on<0 flip.
// Sets the stereo TYPE to anaglyph first so it renders on a normal monitor (VTK's default
// CrystalEyes type needs quad-buffer hardware/shutter glasses and shows nothing without them).
// Returns the new state: 1 = on, 0 = off, -1 = dead handle.
GMTVTK_API int gmtvtk_set_stereo(void* handle, int on) {
	Scene* s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return -1;
	vtkRenderWindow* rw = s->widget->renderWindow();
	const int want = (on < 0) ? (rw->GetStereoRender() ? 0 : 1) : (on ? 1 : 0);
	rw->SetStereoTypeToAnaglyph();
	rw->SetStereoRender(want);
	rw->Render();
	return want;
}

// Standalone executable entry: show the demo surface and block in the loop.
int main(int, char**) {
	gmtvtk_view_demo();
	return g_app ? g_app->exec() : 0;
}
