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
GMTVTK_API void *gmtvtk_view_grid(const float *z, int nx, int ny, double x0, double x1, double y0, double y1, int geographic,
								 const double *cz, const double *crgb, int ncolor, const unsigned char *img,
								 int iw, int ih, int ibands, int edges, int triangulate, int image_only, const char *title) {
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
	                        /*gz=*/tiled ? z : nullptr, /*gnx=*/nx, /*gny=*/ny,
	                        /*blankStart=*/false, /*openFlat2D=*/image_only == 0);   // grids open in 2D from frame 1
	if (s && !image_only && !tiled) {                  // drape path: keep full-res z for hover/profile
		s->gridZ.assign(z, z + (size_t)nx * ny);       // (tiled path already populated it in buildAndShow)
		s->gnx = nx; s->gny = ny;
		s->gx0 = x0; s->gx1 = x1; s->gy0 = y0; s->gy1 = y1;
	}
	// (grids already opened in flat-2D from frame 1 via buildAndShow's openFlat2D — no post-hoc switch,
	// no 3-D flash.)
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
GMTVTK_API void *gmtvtk_view_points(const double *xyz, int npts,
									const double *cz, const double *crgb, int ncolor,
									double x0, double x1, double y0, double y1, int geographic,
									double pointsize, double pickr, double pickg, double pickb,
									const char *title) {
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
	if (auto *m = vtkPolyDataMapper::SafeDownCast(s->surf->GetMapper()))
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
GMTVTK_API void *gmtvtk_view_fv(const double *xyz, int nv, const int *sides, int nfaces,
								const int *indices, const unsigned char *facergb, const double *facez,
								const double *cz, const double *crgb, int ncolor,
								double x0, double x1, double y0, double y1, double z0, double z1,
								int geographic, double zscale, int edges, const char *title,
								const char *objname) {
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
		if (auto *m = vtkPolyDataMapper::SafeDownCast(s->surf->GetMapper())) {
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
				if (auto *ctf = vtkColorTransferFunction::SafeDownCast(m->GetLookupTable()))
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

// Build a named GMT solid (cube/sphere/torus/…) INTO an existing window IN PLACE — the 3-D Bodies
// toolbar path. Reuses the SAME window (no new window) when it is an empty launcher OR already holds a
// body-button solid (then the old body is REPLACED). A window showing REAL data (grid/image/points/
// poly-mesh) is left untouched and we return 0, so the host instead opens the solid in its own window
// (gmtvtk_view_fv). Mirrors gmtvtk_promote_surface_h: recompute the scene through buildSceneContent —
// the EXACT same build path gmtvtk_view_fv uses — so nothing drifts. Returns 1 (reused) / 0 (declined).
GMTVTK_API int gmtvtk_promote_fv_h(void *handle,
								   const double *xyz, int nv, const int *sides, int nfaces,
								   const int *indices, const unsigned char *facergb, const double *facez,
								   const double *cz, const double *crgb, int ncolor,
								   double x0, double x1, double y0, double y1, double z0, double z1,
								   int geographic, double zscale, int edges, const char *objname) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xyz || nv <= 0 || !sides || !indices || nfaces <= 0)
		return 0;
	// Only an empty launcher OR a window already holding a body-button solid may be reused; a window
	// with real data declines (return 0) so the caller opens the solid in a fresh window instead.
	if (!s->emptyStart && !s->fvSolid)
		return 0;

	double zmin, zmax;
	auto pd = makeFvMesh(xyz, nv, sides, nfaces, indices, facergb, facez, zmin, zmax);
	const bool   direct = (facergb != nullptr);
	const bool   cellz  = (!direct && facez != nullptr);
	const int    nc = direct ? 0 : ncolor;
	const double ve = (zscale > 0.0) ? zscale : 1.0;

	// FV uses UNIT horizontal scale + zscale as VE (matches gmtvtk_view_fv: buildAndShow(...,1,1,ve,...)).
	// The CALLER's contract for buildSceneContent: set imageOnly, x0..y1, zmin/zmax, xfac/zfac/ve first.
	s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1; s->zmin = z0; s->zmax = z1;
	s->xfac = 1.0; s->zfac = 1.0; s->ve = ve;
	s->imageOnly = false;
	s->surfName  = (objname && objname[0]) ? objname : "";

	buildSceneContent(s, pd, x0, x1, y0, y1, direct ? nullptr : cz, direct ? nullptr : crgb, nc,
					  nullptr, 0, 0, 0, edges, false, geographic, nullptr, 0, 0, /*blankStart=*/false);

	// Faceted colouring (sharp edges + per-face colours that match the colorbar) — SAME post-step as
	// gmtvtk_view_fv, replacing buildSceneContent's smooth-normal surface for the direct/cell-z modes.
	if (direct || cellz) {
		vtkNew<vtkPolyDataNormals> fn;
		fn->SetInputData(pd);
		fn->SplittingOn();
		fn->SetFeatureAngle(30.0);
		fn->ConsistencyOn();
		if (auto *m = vtkPolyDataMapper::SafeDownCast(s->surf->GetMapper())) {
			m->SetInputConnection(fn->GetOutputPort());
			m->SetScalarModeToUseCellData();
			m->InterpolateScalarsBeforeMappingOff();
			if (direct) {
				m->SetColorModeToDirectScalars();
				if (s->bar) setColorbarVisible(s, false);
			} else {
				m->SetColorModeToMapScalars();
				if (auto *ctf = vtkColorTransferFunction::SafeDownCast(m->GetLookupTable()))
					ctf->SetClamping(1);
				m->UseLookupTableScalarRangeOn();
			}
			m->ScalarVisibilityOn();
			m->Modified();
		}
		s->matteSurf = true;
	}

	s->emptyStart = false;
	s->fvSolid    = true;     // window now holds a body-button solid -> the next body click REPLACES it

	// Rebuild the gizmo from scratch against the REAL surface (the launcher's was sized for the 0..1
	// placeholder), exactly as gmtvtk_promote_surface_h does for a promoted grid.
	disableGizmo(s);
	s->giz = enableGizmo(s, 0.01);

	// A solid is 3-D: leave flat2d OFF, refresh the 2D/3D toolbar icon, force perspective (the launcher
	// opened in flat-2D ortho), and re-show the Shading dock folded (the launcher hid it with no body).
	s->flat2d = false;
	if (s->act2D) s->act2D->setChecked(false);
	if (vtkCamera *cam = s->ren->GetActiveCamera()) cam->ParallelProjectionOff();
	if (s->shadeDock && s->shadeFoldBar) {
		if (QWidget *body = s->shadeDock->widget()) body->setVisible(false);
		s->shadeFoldBar->folded    = true;
		s->shadeFoldBar->openWidth = 240;
		s->shadeFoldBar->updateGeometry();
		s->shadeFoldBar->update();
		s->shadeDock->setVisible(true);
		if (s->win)
			s->win->resizeDocks({s->shadeDock}, {s->shadeFoldBar->sizeHint().width()}, Qt::Horizontal);
	}

	rebuildSceneObjects(s);
	applyShading(s);
	s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// Number of points currently selected (Ctrl+right-drag) in a point-cloud figure.
GMTVTK_API int gmtvtk_selection_count(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	return (int)s->rbSel.size();
}

// Copy up to `n` selected point ids (0-based, into the cloud passed to view_points) into
// `out`. Returns the number copied.
GMTVTK_API int gmtvtk_get_selection(void *handle, int *out, int n) {
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

// Set the visibility of an extra object (dropped/added grid or image) found by its Scene Objects
// name. Used to add a "Nested grid N" blank grid HIDDEN: it still gets a (unchecked) Scene Objects
// row, but its surface is not drawn. Re-renders + rebuilds the panel so the checkbox tracks it.
GMTVTK_API int gmtvtk_set_object_visible(void *handle, const char *name, int vis) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !name)
		return 0;
	for (auto& ex : s->extras) {
		if (ex.name != name) continue;
		if (ex.actor) ex.actor->SetVisibility(vis ? 1 : 0);
		if (ex.drape) ex.drape->SetVisibility(vis ? 1 : 0);
		if (!ex.isImage) refreshGridColorbar(s);   // grid hidden/shown -> retarget the colorbar + readout
		rebuildSceneObjects(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return 1;
	}
	return 0;
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
GMTVTK_API int gmtvtk_set_table(void *handle, const char *name, const double *data,
								int nrows, int ncols, const char *headers) {
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

// Plot a generic (x,y) series in the bottom-dock "Profile" panel and surface it. Used by the tide
// download (x = epoch seconds, y = sea level, isDate=1 -> the x axis paints date/time labels).
// Returns 0 on a dead window / no panel / fewer than 2 points. NOTE: this reuses the elevation
// profiler's panel, so a later Ctrl+drag profile overwrites it — the shared panel needs work.
GMTVTK_API int gmtvtk_show_profile_xy(void *handle, const double *x, const double *y, int n,
                                      const char *title, const char *xlabel, const char *ylabel,
                                      int isDate) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !s->prof || !x || !y || n < 2)
		return 0;
	std::vector<double> xv(x, x + n), yv(y, y + n);
	s->prof->setSeries(xv, yv,
		QString::fromUtf8(title  ? title  : ""),
		QString::fromUtf8((xlabel && xlabel[0]) ? xlabel : "x"),
		QString::fromUtf8((ylabel && ylabel[0]) ? ylabel : "y"),
		isDate != 0);
	if (s->bottomDock) s->bottomDock->setVisible(true);
	setBottomCollapsed(s, false);
	if (s->bottomTabs) s->bottomTabs->setCurrentWidget(s->prof);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// Open the window's CURRENT profile/series (whatever its bottom-dock Profile panel shows — a
// Ctrl-drag elevation profile or a downloaded tide series) in a standalone X,Y plot tool window.
// The programmatic twin of the panel's right-click "Open in X,Y plot tool". Returns the new
// XYPlot *handle (opaque), or null on a dead window / no panel / fewer than 2 points.
GMTVTK_API void *gmtvtk_open_profile_in_xyplot(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !s->prof)
		return nullptr;
	const std::vector<double>& X = s->prof->seriesX();
	const std::vector<double>& Y = s->prof->seriesY();
	if (X.size() < 2)
		return nullptr;
	const QByteArray t = s->prof->seriesTitle().toUtf8();
	const QByteArray xl = s->prof->seriesXLabel().toUtf8();
	const QByteArray yl = s->prof->seriesYLabel().toUtf8();
	return openSeriesInXYTool(X, Y, t.isEmpty() ? "i'GMT  —  Profile" : t.constData(),
	                          xl.constData(), yl.constData());
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

// Change an already-open window's titlebar (e.g. drag-drop / promote loading a file into the
// empty launcher, which was built with a generic placeholder title).
GMTVTK_API void gmtvtk_set_title_h(void *handle, const char *title) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !s->win || !title) return;
	s->win->setWindowTitle(QString::fromUtf8(title));
}

// ============================================================================================
// Progress dialog for long operations (multi-patch Okada calculation)
// ============================================================================================

extern QProgressDialog *g_progress;  // from 30_app.cpp

// Show a modal progress dialog with range 0..max. `title` is shown above the bar.
// The dialog has no cancel button (auto-close only). Returns 1 on success, 0 on failure.
GMTVTK_API int gmtvtk_progress_show(int max, const char *title) {
	if (!QApplication::instance()) return 0;
	if (g_progress) delete g_progress;
	g_progress = new QProgressDialog();
	g_progress->setWindowTitle(title ? title : "Processing");
	g_progress->setRange(0, max);
	g_progress->setValue(0);
	g_progress->setMinimumDuration(0);   // show immediately
	g_progress->setCancelButton(nullptr); // no cancel button
	g_progress->setWindowModality(Qt::ApplicationModal);
	g_progress->show();
	QApplication::processEvents();
	return 1;
}

// Update the progress value. Does nothing if no dialog is shown.
GMTVTK_API void gmtvtk_progress_update(int value) {
	if (g_progress) {
		g_progress->setValue(value);
		QApplication::processEvents();  // keep UI responsive
	}
}

// Close and destroy the progress dialog. Safe to call when none exists.
GMTVTK_API void gmtvtk_progress_close() {
	if (g_progress) {
		g_progress->close();
		delete g_progress;
		g_progress = nullptr;
	}
	QApplication::processEvents();
}

// Does this window have a primary surface? 0 for a bare empty() launcher (no data yet); used by
// the drop handler to decide between PROMOTING an empty window vs adding into a populated one.
GMTVTK_API int gmtvtk_has_surface(void *handle) {
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
GMTVTK_API int gmtvtk_scene_state(void *handle, char *buf, int cap) {
	Scene *s = static_cast<Scene*>(handle);
	std::string o;
	char t[96];
	auto kvi = [&](const char *k, long v) { o += k; o += '='; o += std::to_string(v); o += ';'; };
	auto kvd = [&](const char *k, double v) { snprintf(t, sizeof(t), "%s=%.10g;", k, v); o += t; };
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
		// Per-polygon introspection (drives the fault-trace icon/menu regression tests): the icon kind a
		// row would get + the flags it derives from. "poly<i>=<isFault>,<closed>,<nestKind>:<name>;"
		for (size_t i = 0; i < s->polys.size(); ++i) {
			const Polygon& pg = s->polys[i];
			o += "poly" + std::to_string((int)i) + '=';
			o += std::to_string(pg.isFault ? 1 : 0) + ',';
			o += std::to_string(pg.closed  ? 1 : 0) + ',';
			o += std::to_string(pg.nestKind) + ':';
			o += pg.name; o += ';';
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
GMTVTK_API void gmtvtk_set_cpt(void *handle, const double *cz, const double *crgb, int n) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !cz || !crgb || n < 2) return;
	vtkColorTransferFunction *ctf = vtkColorTransferFunction::SafeDownCast(s->surfLut);
	if (!ctf) return;                       // only the CTF path supports live recolour
	ctf->RemoveAllPoints();
	for (int i = 0; i < n; ++i)
		ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
	s->surfCtfRange = true;
	if (s->bar) s->bar->SetLookupTable(s->surfLut);   // refresh the legend strip
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Recolour ONE grid of a multi-grid window. gridSel: -1 = base relief surface (s->surfLut, shared
// by the surface, every LOD tile and the colorbar); 0..N-1 = the Nth dropped/added grid (its own
// ExtraObj lut, shared by that grid's mapper). Mutating the target's CTF nodes in place recolours
// exactly that grid — fixing the old bug where the colormap chooser on any grid's Color Bar row
// always recoloured the FIRST grid. refreshGridColorbar then rebuilds the legend strip if the
// recoloured grid is the active (topmost-visible) one. Called from Julia (_recolor_grid).
GMTVTK_API void gmtvtk_set_cpt_grid(void *handle, int gridSel, const double *cz, const double *crgb, int n) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !cz || !crgb || n < 2) return;
	vtkScalarsToColors *lut = nullptr;          // gridSel is the grid's UNIQUE TAG (-1 = base relief)
	if (gridSel < 0) lut = s->surfLut;
	else for (auto& ex : s->extras) if (!ex.isImage && ex.tag == gridSel) { lut = ex.lut; break; }
	vtkColorTransferFunction *ctf = vtkColorTransferFunction::SafeDownCast(lut);
	if (!ctf) return;                         // only the CTF path supports live recolour
	ctf->RemoveAllPoints();
	for (int i = 0; i < n; ++i)
		ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
	if (gridSel < 0) s->surfCtfRange = true;
	refreshGridColorbar(s);                   // retarget/refresh the single legend strip to the active grid
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// TEST PROBE: sample the colour ONE grid's own lut maps a z value to (RGB, 0..1 into out3). gridSel:
// -1 = base relief (s->surfLut), 0..N-1 = the Nth extra grid (its own lut). Lets the test suite assert
// per-grid colorbar isolation (recolouring grid A must NOT change grid B's colours). Returns 1 on
// success, 0 if the handle/grid/lut is missing. Not used by the UI — purely for regression tests.
GMTVTK_API int gmtvtk_grid_rgb_at(void *handle, int gridSel, double z, double *out3) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !out3) return 0;
	vtkScalarsToColors *lut = nullptr;          // gridSel is the grid's UNIQUE TAG (-1 = base relief)
	if (gridSel < 0) lut = s->surfLut;
	else for (auto& ex : s->extras) if (!ex.isImage && ex.tag == gridSel) { lut = ex.lut; break; }
	if (!lut) return 0;
	const unsigned char *c = lut->MapValue(z);   // works for both vtkColorTransferFunction and vtkLookupTable
	out3[0] = c[0] / 255.0; out3[1] = c[1] / 255.0; out3[2] = c[2] / 255.0;
	return 1;
}

// Set the window's coordinate reference system (all three interchangeable forms — PROJ4 / WKT /
// EPSG). Julia resolves them via GMT.jl and pushes them here right after the window opens. Storing
// any of them marks the data as referenced and enables the Geography menu (disabled by default since
// it needs a reference frame); an all-empty CRS disables it again.
GMTVTK_API void gmtvtk_set_crs(void *handle, const char *proj4, const char *wkt, int epsg) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	s->crsProj4 = proj4 ? proj4 : "";
	s->crsWkt   = wkt   ? wkt   : "";
	s->crsEpsg  = epsg;
	if (s->geoMenu) s->geoMenu->menuAction()->setEnabled(s->hasCRS());
}

// Close a window programmatically (WA_DeleteOnClose -> destroy + bookkeeping). Used to retire an
// empty launcher once a dropped file has been promoted into a full viewer window.
GMTVTK_API void gmtvtk_close(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (sceneAlive(s) && s->win) s->win->close();
}

// Register a freshly-opened file in the persistent Recent Files list (File > Recent Files).
// Called from Julia after a successful open. cat: 0 = grid, 1 = image, 2 = dataset.
GMTVTK_API void gmtvtk_add_recent(const char *path, int cat) {
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

// Push one execution-error line into a 3-D viewer window's read-only "Errors" tab and raise that
// tab (so a failure in a background callback is VISIBLE in the window, not just on the REPL's
// stderr). `scene` is the window's Scene*; no-op if the handle is dead or has no Errors tab. The
// X,Y tool has its own twin, gmtvtk_xyplot_log. Best-effort: Julia calls this from catch blocks.
GMTVTK_API void gmtvtk_log_error(void *scene, const char *msg) {
	if (msg) sceneLogError(static_cast<Scene*>(scene), QString::fromUtf8(msg));
}

// Modal error box — for host failures the user MUST see. sceneLogError alone silently drops the
// message when the window has no Errors console (the bare empty launcher), and even with one the
// bottom tabs may be folded — "it failed but the reason was written somewhere invisible" reads as
// "the feature did nothing" (Focal mechanisms bug reports, 2026-07-04). Runs on the UI thread
// (host callbacks are invoked from menu actions).
// The window's X actor scale (cos(midlat) for geographic windows, 1 for cartesian) — the host
// needs it to build symbols that are ROUND ON SCREEN: a symbol drawn in raw degrees is squeezed
// by xfac on X, so the host pre-divides its X offsets by this (focal-mechanism beachballs; any
// future screen-round symbol). Returns 1.0 for a dead/unknown handle.
GMTVTK_API double gmtvtk_get_xfac(void *scene) {
	Scene *s = static_cast<Scene*>(scene);
	if (!sceneAlive(s) || !(s->xfac > 0.0)) return 1.0;
	return s->xfac;
}

GMTVTK_API void gmtvtk_error_box(void *scene, const char *title, const char *msg) {
	Scene *s = static_cast<Scene*>(scene);
	if (!msg) return;
	QWidget *parent = (s && sceneAlive(s)) ? static_cast<QWidget*>(s->win) : nullptr;
	QMessageBox::warning(parent, QString::fromUtf8(title && title[0] ? title : "Error"),
	                     QString::fromUtf8(msg));
}

// Register the basemap-picker callback. `fn` (Julia @cfunction, signature JuliaBaseMapFn) is called
// with a clicked tile's geographic region "W/E/S/N/wrap"; Julia crops data/etopo4.jpg + adds it.
GMTVTK_API void gmtvtk_set_basemap_callback(JuliaBaseMapFn fn) {
	g_juliaBaseMap = fn;
}

// Set the path to the world logo image painted in the basemap picker (data/etopo4_logo.jpg).
GMTVTK_API void gmtvtk_set_basemap_logo(const char *path) {
	g_basemapLogo = QString::fromUtf8(path ? path : "");
}

// Set the path to the Base Map toolbar button icon (data/basemap_icon.png). Must be set before a
// window is built (the toolbar reads g_basemapIcon at construction); empty -> hand-painted fallback.
GMTVTK_API void gmtvtk_set_basemap_icon(const char *path) {
	g_basemapIcon = QString::fromUtf8(path ? path : "");
}

// Register the Tiles-Tool callback. `fn` (Julia @cfunction, signature JuliaTilesFn) is called with a
// "op;..." request from the picker; op "go" hands "go;W/E/S/N;zoom;provider;cache;merc" and Julia builds
// the mosaic (GMT.mosaic, two zoom levels coarser) into a new viewer. nullptr to detach.
GMTVTK_API void gmtvtk_set_tiles_callback(JuliaTilesFn fn) {
	g_juliaTiles = fn;
}

// Set the equirectangular world image (data/etopo4.jpg, [-180 180]/[-90 90]) the Tiles-Tool picker
// crops/zooms as its base. Pushed from Julia (gmtvtk_set_tiles_world) at __init__.
GMTVTK_API void gmtvtk_set_tiles_world(const char *path) {
	g_tilesWorld = QString::fromUtf8(path ? path : "");
}

// Phase 2: push a coarser-mosaic background (a PNG written by Julia) into the open Tiles-Tool picker
// `dlg` (a TilesPicker*), covering [W..E]/[S..N]; painted over the etopo base, under the refined mesh.
// Called SYNCHRONOUSLY from Julia's op "bg" (so `dlg` is the live picker that issued the request). A
// bad path / null pixmap is ignored inside setBg.
GMTVTK_API void gmtvtk_tiles_set_bg(void *dlg, const char *pngpath, double W, double E, double S, double N) {
	if (!dlg) return;
	reinterpret_cast<TilesPicker*>(dlg)->map->setBg(QString::fromUtf8(pngpath ? pngpath : ""), W, E, S, N);
}

// Append one line to the open Tiles-Tool picker's collapsible "Downloads info" console. Called from
// Julia (GMT.mosaic's per-tile fetch messages via TILE_LOGGER, plus the download/ready bracket), so the
// user sees tile activity in the picker itself rather than the iGMT viewer's Errors tab. `dlg` = the
// live TilesPicker *that issued the request.
GMTVTK_API void gmtvtk_tiles_log(void *dlg, const char *msg) {
	if (!dlg || !msg) return;
	reinterpret_cast<TilesPicker*>(dlg)->logDownload(QString::fromUtf8(msg));
}

// Register the Background-region callback. `fn` (Julia @cfunction, signature JuliaBgRegionFn) is
// called with "W/E/S/N/geographic" from the File > Background region dialog; Julia opens a fresh
// blank white 2-D map framed to those limits. nullptr to detach.
GMTVTK_API void gmtvtk_set_bgregion_callback(JuliaBgRegionFn fn) {
	g_juliaBgRegion = fn;
}

// Register the New-Window callback. `fn` (Julia @cfunction, signature JuliaNewWindowFn) is called
// (with the clicked window's Scene*) from File > New Window; Julia opens a fresh empty launcher and
// registers it. nullptr to detach.
GMTVTK_API void gmtvtk_set_newwindow_callback(JuliaNewWindowFn fn) {
	g_juliaNewWindow = fn;
}

// Register the Geography-menu callback. `fn` (Julia @cfunction, signature JuliaGeoFn) is called
// with "<kind>/<res>/W/E/S/N" (the visible region at the current zoom) when a Plot-coastline leaf
// is chosen; Julia runs GMT.coast and adds the lines via gmtvtk_add_overlay_h. nullptr to detach.
GMTVTK_API void gmtvtk_set_geography_callback(JuliaGeoFn fn) {
	g_juliaGeo = fn;
}

// Register the 3-D Bodies toolbar callback. `fn` (Julia @cfunction, signature JuliaSolidFn) is
// called with a GMT solid name ("cube"/"sphere"/"torus"/…) when the user clicks a body in the
// flyout; Julia builds the named GMTfv via SOLIDS and opens it with view_fv. nullptr to detach.
GMTVTK_API void gmtvtk_set_solid_callback(JuliaSolidFn fn) {
	g_juliaSolid = fn;
}

// Register the grdsample callback (GMT menu). `fn` (Julia @cfunction, signature JuliaGrdsampleFn)
// is called with (scene, "input;output;I;R;n;r;T;S") when the user runs the grdsample dialog: scene
// is the receiving window (for "selected" input + adding the result as a layer), S the source
// element's Scene Objects label. nullptr to detach.
GMTVTK_API void gmtvtk_set_grdsample_callback(JuliaGrdsampleFn fn) {
	g_juliaGrdsample = fn;
}

// Register the NSWING tsunami callback (Geophysics menu). `fn` (Julia @cfunction, JuliaNswingFn) is
// called with (scene, "key=value\n…") when the user hits RUN in the NSWING dialog: scene is the
// receiving window, the block carries every field (grids, output mode/name, -M/-Z/-X/-N/-t/…).
// nullptr to detach.
GMTVTK_API void gmtvtk_set_nswing_callback(JuliaNswingFn fn) {
	g_juliaNswing = fn;
}

// Register the Plot seismicity callback (Geophysics > Seismology). `fn` (Julia @cfunction,
// JuliaSeismicityFn) is called with (scene, "key=value\n…") on the dialog's OK: scene is the
// receiving window, the block carries format/file/date range/magnitude/depth filters, the
// per-interval sizes/colours and the visible map region. nullptr to detach.
GMTVTK_API void gmtvtk_set_seismicity_callback(JuliaSeismicityFn fn) {
	g_juliaSeismicity = fn;
}

// Register the Vertical elastic deformation callback. fn(scene, params) is called with the
// "action;coord;len;wid;…;mu;R;I" string when the user clicks Compute / Save fault. nullptr to detach.
GMTVTK_API void gmtvtk_set_elastic_callback(JuliaElasticFn fn) {
	g_juliaElastic = fn;
}

// Register the fault-trace endpoint callback (Strike/Length edits in the elastic dialog).
// fn(lon1, lat1, strike, len_km) returns "lon2/lat2" (the direct-geodesic endpoint) or "". nullptr
// to detach (geographic Strike/Length edits then leave the drawn trace unchanged).
GMTVTK_API void gmtvtk_set_faultgeom_callback(JuliaFaultGeomFn fn) {
	g_juliaFaultGeom = fn;
}

// Register the Import-Trace-Fault callback (Geophysics > Seismology > Elastic deformation). fn(scene,
// path) reads the sub-fault file and adds the traces via gmtvtk_add_fault_h. nullptr to detach.
GMTVTK_API void gmtvtk_set_importfault_callback(JuliaImportFaultFn fn) {
	g_juliaImportFault = fn;
}

// Add a fault trace LINE to a window by its handle — the host-import twin of the interactive Draw
// Fault tool. `xy` is `npts` (lon,lat) pairs in TRUE (data) coords; z is draped onto the surface by
// polyRebuildLine, so the host need not supply it. The line is finalized through the very same path
// as a drawn fault (polyFinalize with the "fault" prefix), so it lands as an isFault polyline named
// "fault N" with the Vertical-elastic-deformation context menu — identical properties to Draw Fault.
// `slip` (METERS) and `rake` (DEGREES) seed the Vertical elastic deformation dialog's Dislocation
// Geometry boxes when the fault's dialog opens (pass NaN to leave the dialog default). Returns 1 if
// added, 0 on a dead handle / too few points.
GMTVTK_API int gmtvtk_add_fault_h(void *handle, const double *xy, int npts, double slip, double rake) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xy || npts < 2) return 0;
	std::vector<std::array<double,3>> verts;
	verts.reserve(npts);
	for (int i = 0; i < npts; ++i) verts.push_back({ xy[2*i], xy[2*i + 1], 0.0 });
	polyFinalize(s, verts, false, "fault");
	if (!s->polys.empty()) {                 // polyFinalize pushed the new fault last
		s->polys.back().faultSlip = slip;    // meters (NaN = unknown)
		s->polys.back().faultRake = rake;    // degrees (NaN = unknown)
	}
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// Add a fault trace LINE *with its full slip-model geometry* — the host-import twin of Draw Fault for
// Import Trace Fault. Same as gmtvtk_add_fault_h (finalizes the trace through polyFinalize → isFault
// "fault N" line, seeds slip/rake) but ALSO carries the geometry read from the sub-fault file:
// `strike`/`dip` (DEGREES), `width` = the TOTAL down-dip width ny·Dy (km geog / data units), and
// `depthTop` = depth to the top of the shallowest patch (km). With the geometry known, it immediately
// draws the dipping fault plane and ITS SURFACE-PROJECTION RECTANGLE (the gray patch hugging the
// ground) via faultUpdatePlane — exactly what Mirone's subfault() plots — instead of waiting for the
// user to open the elastic dialog. `geog` (1/0) selects the geodesic vs cartesian down-dip walk. The
// geometry is also stored on the polygon so the dialog opens seeded with the file's true values.
// Returns 1 if added, 0 on a dead handle / too few points.
GMTVTK_API int gmtvtk_add_fault_geom_h(void *handle, const double *xy, int npts,
                                       double slip, double rake,
                                       double strike, double dip, double width, double depthTop, int geog) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xy || npts < 2) return 0;
	std::vector<std::array<double,3>> verts;
	verts.reserve(npts);
	for (int i = 0; i < npts; ++i) verts.push_back({ xy[2*i], xy[2*i + 1], 0.0 });
	polyFinalize(s, verts, false, "fault");
	if (s->polys.empty()) return 0;          // polyFinalize pushed the new fault last
	int pi = (int)s->polys.size() - 1;
	Polygon &pg = s->polys[pi];
	pg.faultSlip = slip;  pg.faultRake = rake;          // meters / degrees (NaN = unknown)
	pg.faultStrike = strike; pg.faultDip = dip; pg.faultWidth = width; pg.faultDepthTop = depthTop;
	faultUpdatePlane(s, width, dip, strike, rake, geog != 0, pi);   // draw plane + surface-projection rect now
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// Register the Import-Model-Slip callback (Geophysics > Seismology > Elastic deformation). fn(scene,
// path) reads the sub-fault file and adds every patch via gmtvtk_add_slip_patches_h. nullptr to detach.
GMTVTK_API void gmtvtk_set_modelslip_callback(JuliaModelSlipFn fn) {
	g_juliaModelSlip = fn;
}

// Add a whole SLIP MODEL — every sub-fault patch as a filled surface-projection polygon — to a window
// by its handle. The host-import twin of Import Trace Fault, but for the full Mirone subfault() plot:
// each patch is a flat quad coloured by its slip (NOT the dipping 3-D plane). `xy` is the concatenated
// (x,y) vertex pairs of every patch in TRUE (data) coords; `vcounts[i]` = vertex count of patch i
// (typically 4); `npatch` = number of patches; `rgb` = 3*npatch face colours in 0..1 (one per patch,
// already mapped from slip by the host); `name` = the Scene Objects group label (e.g. "Slip model")
// every patch folds under. The per-patch DISLOCATION GEOMETRY arrays (each npatch long; null = absent)
// — `slip` (m), `rake`/`strike`/`dip` (deg), `depthTop` (km, top of patch) — plus the model-wide
// `dx`/`dy` patch length/width (km) and optional `seg` segment id — are stored on each patch so its
// "Vertical elastic deformation" menu opens the dialog seeded from THIS patch and the dialog can list
// every patch in its Faults combo and Compute the whole model. z is left 0 and draped onto the surface
// by polyRebuildLine. Each patch is a closed, filled Polygon named "patch N". The Scene Objects panel +
// draw-order pile are rebuilt ONCE after the whole batch. Returns the number of patches added, 0 on a
// dead handle / bad input.
GMTVTK_API int gmtvtk_add_slip_patches_h(void *handle, const double *xy, const int *vcounts,
                                         int npatch, const double *rgb, const char *name,
                                         const double *slip, const double *rake, const double *strike,
                                         const double *dip, const double *depthTop,
                                         double dx, double dy, const int *seg) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xy || !vcounts || !rgb || npatch < 1) return 0;
	const std::string grp = (name && name[0]) ? name : "Slip model";
	int xyoff = 0, added = 0;
	for (int p = 0; p < npatch; ++p) {
		const int nv = vcounts[p];
		if (nv < 3) { xyoff += 2 * (nv > 0 ? nv : 0); continue; }   // need >=3 corners for a face
		Polygon pg;
		pg.v.reserve(nv + 1);
		for (int i = 0; i < nv; ++i) pg.v.push_back({ xy[xyoff + 2*i], xy[xyoff + 2*i + 1], 0.0 });
		pg.v.push_back(pg.v.front());                  // close the ring (first == last)
		xyoff += 2 * nv;
		pg.closed = true;
		pg.groupName = grp;
		pg.name = "patch " + std::to_string(p + 1);
		pg.fillColor[0] = rgb[3*p]; pg.fillColor[1] = rgb[3*p + 1]; pg.fillColor[2] = rgb[3*p + 2];
		pg.fillOpacity = 1.0;                          // slip patches are SOLID-filled (Mirone FaceColor)
		pg.isSlip = true;                              // rectangular sub-fault: opens the elastic dialog, lists in its Faults combo
		pg.slipSeg = seg ? seg[p] : 0;
		if (slip)     pg.faultSlip     = slip[p];      // dislocation slip (m)
		if (rake)     pg.faultRake     = rake[p];      // rake (deg)
		if (strike)   pg.faultStrike   = strike[p];    // strike (deg)
		if (dip)      pg.faultDip      = dip[p];       // dip (deg)
		if (depthTop) pg.faultDepthTop = depthTop[p];  // depth to top of patch (km)
		pg.faultLength = dx;                           // along-strike patch length (km)
		pg.faultWidth  = dy;                           // down-dip patch width (km)
		polyRebuildLine(s, pg);                        // builds the outline + the filled face from pg
		if (pg.line) { pg.line->GetProperty()->SetColor(0.0, 0.0, 0.0); pg.line->GetProperty()->SetLineWidth(0.4); }  // thin black edges (Mirone patch default)
		pg.stack = s->vecSeq++;                        // each patch lands on the shared vector pile
		s->polys.push_back(pg);
		++added;
	}
	if (added == 0) return 0;
	applyVectorStacking(s);                            // normalize ranks + draw-order offsets across the batch
	rebuildSceneObjects(s);                            // ONE panel rebuild for the whole model
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return added;
}

// Register the Focal mechanisms callback (Geophysics > Seismology > Focal mechanisms). fn(scene,
// params) reads/filters the catalog and adds every event's beachball via gmtvtk_add_meca_h.
// nullptr to detach.
GMTVTK_API void gmtvtk_set_focal_callback(JuliaFocalFn fn) {
	g_juliaFocal = fn;
}

// Build ONE flat "meca" patch (outline + fill) directly, bypassing the shared polyRebuildFill
// triangulator (85_polygon.cpp). That path runs vtkTriangleFilter -> vtkPolygon::Triangulate,
// which assumes a SIMPLE (non-self-intersecting) polygon; patch_meca's equal-area boundary can
// carry a hairline self-crossing right at its start/end seam (several pieces of the boundary all
// meet at the same disk point), which corrupts ear-clipping into a jagged self-crossing fill.
// vtkContourTriangulator is built for exactly this class of ill-conditioned contour input (it's
// the tool used for medical-imaging contour reconstruction, where messy/self-touching contours
// are the norm) and triangulates it correctly. `z0` is a single CONSTANT for the whole patch
// (sampled once at the event's location, or 0 off a grid) — beachball disks are flat schematic
// symbols, never terrain-draped (matches Mirone, which draws them on the flat 2-D map).
//
// `rank` (this patch's 0-based index within the whole gmtvtk_add_meca_h batch) breaks ties
// between PHYSICALLY OVERLAPPING beachballs. A dense catalog routinely has two events' disks
// overlap in world space (their epicentres are closer than the sum of their plotted radii — no
// data-entry error, just real geology plotted at a schematic symbol size); every other filled-
// polygon feature in the app (drawn polygons, slip patches) never overlaps another instance of
// itself, so this never came up before. applyStacking's per-element depth ramp (50_scene.cpp)
// only touches LINE/POINT coincident-topology params for a "vec" stack item, never polygon
// params — so every meca fill was left at the SAME fixed offset, and two overlapping fills had
// no consistent front/back order: the GPU rasterizer's tie-break is whatever triangle happens to
// win per-pixel, which is why the render looked shredded/random at overlaps, not "wrong" per
// mechanism (each mechanism's own geometry was already verified correct in isolation). Fix: give
// every patch (fill AND outline) its OWN small offset step by rank, so patch order — which is
// catalog order, deterministic — always wins ties consistently, the same way applyStacking's
// per-rank ramp does for every other vector element.
// `rings` = ALL the simple closed contours (open form, no duplicated end point; pg.v holds the
// first one for the pick/Scene-Objects plumbing) sharing ONE depth rank — i.e. one event's
// whole dilatational sector set or whole compressive sector set, in ONE actor. Grouping per
// rank instead of one-actor-per-ring matters for real catalogs: a 133-event ISF batch as
// per-ring actors was ~thousands of VTK actors and rendered "horribly slow"; per-rank it is
// ≤2 fill actors + 1 line actor per event. The rings of one rank never overlap (disk sectors),
// so feeding the whole set to vtkContourTriangulator (even-odd across the set = their union)
// triangulates them together correctly in one pass.
static void mecaBuildPatch(Scene *s, Polygon &pg, double z0, int rank, double zStep,
                           const std::vector<std::vector<std::array<double,3>>> &rings) {
	// `rank` is the EVENT index (gmtvtk_add_meca_h passes `evid[p]`, NOT the flat patch index `p`) —
	// every sub-loop and border-ring segment of the SAME event shares one rank, since they never
	// spatially overlap each other by construction and so never need to out-rank one another.
	// (2026-07-05, USER LAW) Every ball's fill is ALWAYS the complete, uncut set of sector triangles
	// — geometric clipping against a neighbour was tried and reverted same day (it bakes the
	// neighbour's PLOT-TIME position into this ball's polydata; dragging the neighbour only moves
	// ITS actor, so the bake goes stale and leaves a permanent "bite" where it used to sit). Cross-
	// ball occlusion is depth-test ONLY, via `rank*zStep` applied as the actor's POSITION (see the
	// SetPosition call below), not baked into the vertex Z — a baked-in-vertex real-Z step (tried and
	// reverted same day) gets multiplied by the actor's own SetScale(xfac,1,zfac*ve): for geographic
	// data `zfac` is ~1/111111 (metres-to-degrees), so a "1.0" bake shrinks to ~9e-6 and is lost to
	// depth-buffer noise regardless of rank — exactly why a lower ball's rim/nodal STROKE kept
	// showing through a higher ball's opaque FILL ("why do I still see nodal lines behind the top
	// beachball"). `SetPosition` is applied in PARENT space AFTER Scale (same trick MecaBall drag
	// already relies on for its X/Y offset), so `zStep` — sized by the caller off a REAL on-screen
	// quantity (batch reference radius), not a raw world-Z unit — survives ANY zfac/VE combination.
	std::vector<std::array<double,3>> ring(pg.v.begin(), pg.v.end() - 1);   // pg.v is closed (front==back)
	for (auto &p : ring) p[2] = z0;

	// Build the outline actor (other code — Scene Objects rows, click-menu targeting, delete paths —
	// expects every polygon's `pg.line` to exist and null-checks it defensively) but NEVER add it to
	// any renderer: a beachball's two parts are each already a SOLID opaque fill (black/white), so
	// their shared boundary IS the visible nodal-plane line — a stroke drawn on top has no visual
	// job here, and its GL_POLYGON_OFFSET_LINE bias is a SEPARATE, not-numerically-comparable state
	// from the fill's GL_POLYGON_OFFSET_FILL bias, so a lower-rank event's own outline kept winning
	// against a higher-rank event's opaque fill and bled through it. An actor that's never added to
	// a renderer can never bleed through anything, while `pg.line`'s default VTK visibility (true)
	// keeps the Scene Objects checkbox state consistent with the (always-visible) fill.
	if (!pg.linePD) pg.linePD = vtkSmartPointer<vtkPolyData>::New();
	std::vector<std::array<double,3>> closed = ring; closed.push_back(ring.front());
	polyFillLine(pg.linePD, closed, false);
	pg.line = polyMakeLineActor(s, pg.linePD, 0.0, 0.0, 0.0);
	pg.line->GetProperty()->SetLineWidth(0.6);

	// Triangulate EVERY ring SEPARATELY and append the triangles (union). Feeding the whole set
	// to ONE vtkContourTriangulator with even-odd across contours was tried and is WRONG here:
	// adjacent same-colour sectors share whole nodal-curve edges (and near-degenerate mechanisms
	// produce slightly overlapping sliver sectors), and even-odd CANCELS coincident/overlapping
	// coverage — a thrust ball's black lens vanished. Per-ring triangulation + append renders
	// the union: double-painted overlap is harmless for one opaque colour.
	const double zl = z0;
	vtkNew<vtkAppendPolyData> app;
	for (const auto &rg : rings) {
		if (rg.size() < 3) continue;
		vtkNew<vtkPoints> pts;
		vtkNew<vtkCellArray> lines;
		for (auto &p : rg) pts->InsertNextPoint(p[0], p[1], zl);
		vtkNew<vtkIdList> ids;
		for (vtkIdType i = 0; i < (vtkIdType)rg.size(); ++i) ids->InsertNextId(i);
		ids->InsertNextId(0);
		lines->InsertNextCell(ids);
		vtkNew<vtkPolyData> contour; contour->SetPoints(pts); contour->SetLines(lines);
		vtkNew<vtkContourTriangulator> tri; tri->SetInputData(contour); tri->Update();
		vtkNew<vtkPolyData> piece; piece->ShallowCopy(tri->GetOutput());
		app->AddInputData(piece);
	}
	app->Update();

	if (!pg.fillPD) pg.fillPD = vtkSmartPointer<vtkPolyData>::New();
	pg.fillPD->ShallowCopy(app->GetOutput());
	vtkNew<vtkPolyDataMapper> map; map->SetInputData(pg.fillPD); map->ScalarVisibilityOff();
	pg.fill = vtkSmartPointer<vtkActor>::New();
	pg.fill->SetMapper(map);
	pg.fill->GetProperty()->SetColor(pg.fillColor[0], pg.fillColor[1], pg.fillColor[2]);
	pg.fill->GetProperty()->SetOpacity(1.0);       // beachballs are ALWAYS fully opaque — never translucent
	pg.fill->GetProperty()->LightingOff();
	pg.fill->GetProperty()->EdgeVisibilityOff();
	pg.fill->GetProperty()->BackfaceCullingOff();
	pg.fill->PickableOff();
	pg.fill->ForceOpaqueOn();     // hard-pin to VTK's opaque render pass, never the translucent/blended one
	pg.line->ForceOpaqueOn();
	pg.fill->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	pg.fill->SetPosition(0.0, 0.0, rank * zStep);   // cross-ball depth rank — see comment above
	(s->axesRen ? s->axesRen : s->ren)->AddActor(pg.fill);
}

// One event's stroke set (rim circle + the two nodal-plane curves) as ONE real LINE actor with
// a constant PIXEL width — the beachball's black separating lines must be visible at ANY zoom
// (world-space ribbon quads went sub-pixel and dissolved into dotted noise on small balls, and
// cost ~135 extra actors per event). Cross-event depth ordering is `rank*zStep` via SetPosition
// (AFTER SetScale, immune to zfac/VE) — MUST match mecaBuildPatch's convention exactly, or a
// higher-ranked ball's fill and a lower-ranked ball's own stroke stop comparing consistently.
static void mecaBuildLines(Scene *s, Polygon &pg, double z0, int rank, double zStep, double widthPx,
                           const std::vector<std::vector<std::array<double,3>>> &plines) {
	const double zl = z0;
	vtkNew<vtkPoints> pts;
	vtkNew<vtkCellArray> cells;
	for (const auto &pl : plines) {
		if (pl.size() < 2) continue;
		const vtkIdType base = pts->GetNumberOfPoints();
		for (auto &p : pl) pts->InsertNextPoint(p[0], p[1], zl);
		vtkNew<vtkIdList> ids;
		for (vtkIdType i = 0; i < (vtkIdType)pl.size(); ++i) ids->InsertNextId(base + i);
		cells->InsertNextCell(ids);
	}
	if (!pg.linePD) pg.linePD = vtkSmartPointer<vtkPolyData>::New();
	pg.linePD->SetPoints(pts);
	pg.linePD->SetLines(cells);
	vtkNew<vtkPolyDataMapper> map; map->SetInputData(pg.linePD); map->ScalarVisibilityOff();
	pg.line = vtkSmartPointer<vtkActor>::New();
	pg.line->SetMapper(map);
	pg.line->GetProperty()->SetColor(pg.fillColor[0], pg.fillColor[1], pg.fillColor[2]);
	pg.line->GetProperty()->SetLineWidth((float)std::max(0.5, widthPx));
	pg.line->GetProperty()->LightingOff();
	pg.line->PickableOff();
	pg.line->ForceOpaqueOn();
	pg.line->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	pg.line->SetPosition(0.0, 0.0, rank * zStep);   // cross-ball depth rank — MUST match mecaBuildPatch
	(s->axesRen ? s->axesRen : s->ren)->AddActor(pg.line);
}

// Add a flat text label to a window by its handle — the host/import twin of the Text draw tool
// (polyPlaceText, above). Sits on the XY plane at (x,y), rendered in the overlay layer so it is
// never occluded by the relief. Used by Focal mechanisms' "Plot event date" option (and any future
// host-driven label). `size` <= 0 keeps the TextLabel default (18px). Returns 1 if added.
GMTVTK_API int gmtvtk_add_text_h(void *handle, double x, double y, const char *text,
                                 double r, double g, double b, int size) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !text || !text[0]) return 0;
	TextLabel tl;
	tl.pos = { x, y, 0.0 };
	tl.text = text;
	tl.name = "Text " + std::to_string((int)s->texts.size() + 1);
	tl.color[0] = r; tl.color[1] = g; tl.color[2] = b;
	if (size > 0) tl.size = size;
	tl.actor = vtkSmartPointer<vtkTextActor3D>::New();
	textApplyProps(s, tl);
	(s->axesRen ? s->axesRen : s->ren)->AddActor(tl.actor);
	s->texts.push_back(tl);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// Batch form of gmtvtk_add_text_h: `xy` = n (x,y) pairs, `texts` = n records joined by RS
// ('\x1e', the gmtvtk_add_symbols_h `info` convention). ONE Scene-Objects rebuild + ONE render
// for the whole batch — the per-call rebuild+Render of the single-label form is what made
// Focal mechanisms' per-event "Plot event date" loop take ~90 s for a 133-event catalog
// (2026-07-04); a label loop must come through here instead. `font` (NULL/"" -> TextLabel's own
// default "Arial") and `groupName` (NULL/"" -> ungrouped) let a batch owner (Focal mechanisms'
// date labels) style + tag every label in one call — the tag is what lets deleteMecaGroup find
// and erase them again, and rebuildSceneObjects fold them under the batch's own row instead of
// flooding Scene Objects with one row per event. `eventIdx` (may be NULL) is a parallel n-length
// array giving each label's 0-based event index (evid/3) — gmtvtk_add_meca_h reads the resulting
// TextLabel::mecaEvent to wire MecaBall::dateLabel, so dragging a ball carries its date along.
// Returns the number added.
GMTVTK_API int gmtvtk_add_texts_h(void *handle, const double *xy, const char *texts, int n,
                                  double r, double g, double b, int size,
                                  const char *font, int bold, int italic, const char *groupName,
                                  const int *eventIdx) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xy || !texts || n < 1) return 0;
	const char *p = texts;
	int added = 0;
	for (int i = 0; i < n; ++i) {
		const char *e = strchr(p, '\x1e');
		std::string txt = e ? std::string(p, e - p) : std::string(p);
		if (!txt.empty()) {
			TextLabel tl;
			tl.pos = { xy[2*i], xy[2*i + 1], 0.0 };
			tl.text = std::move(txt);
			tl.name = "Text " + std::to_string((int)s->texts.size() + 1);
			tl.color[0] = r; tl.color[1] = g; tl.color[2] = b;
			if (size > 0) tl.size = size;
			if (font && font[0]) tl.font = font;
			tl.bold = bold != 0;
			tl.italic = italic != 0;
			if (groupName && groupName[0]) tl.groupName = groupName;
			if (eventIdx) tl.mecaEvent = eventIdx[i];
			// Batch-owned labels (groupName set, e.g. Focal mechanisms' dates) are BILLBOARDS —
			// vtkBillboardTextActor3D always faces the camera at a constant screen size, same as the
			// cube's tick numbers (placeTickBillboards, 10_geometry.cpp). Plain vtkTextActor3D (the
			// Text-tool's default) instead lies FLAT in the surface's XY plane, which is correct for a
			// map "sticker" annotation but made the date read as if painted into the terrain/basemap
			// texture from most view angles — exactly the "burned into background image" complaint.
			if (tl.groupName.empty()) tl.actor = vtkSmartPointer<vtkTextActor3D>::New();
			else                      tl.actor = vtkSmartPointer<vtkBillboardTextActor3D>::New();
			textApplyProps(s, tl);
			(s->axesRen ? s->axesRen : s->ren)->AddActor(tl.actor);
			s->texts.push_back(tl);
			++added;
		}
		if (!e) break;
		p = e + 1;
	}
	if (added == 0) return 0;
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return added;
}

// Add a batch of focal-mechanism "beachball" patches (Seismology > Focal mechanisms) to a window
// by its handle. Each event contributes TWO consecutive patches — compressive then dilatational,
// computed in Julia from the nodal-plane geometry (Mirone's patch_meca.m equal-area projection)
// and centred on the event's lon/lat. Packed exactly like gmtvtk_add_slip_patches_h: `xy` =
// concatenated (x,y) vertex pairs of every patch, in order; `vcounts[i]` = vertex count of patch
// i; `npatch` = 2*nevents; `rgb` = 3*npatch face colours (black/white, precomputed by the host).
// `evid[i]` = the 0-based EVENT index patch i belongs to (Julia assigns it once per kept event,
// shared by that event's comp/dilat sub-loops AND its border-ring segments) — the cross-event
// depth-ordering rank is THIS, not the flat patch index `p`: a per-patch rank grows with however
// many sub-loops/ring-segments an event happens to split into (a dense border ring alone can be
// 60+ extra patches), inflating the polygon-offset magnitude far beyond what's needed and, past
// some point, its ordering stops being reliable (observed: an entire quadrant vanishing). Ranking
// by event index instead keeps the magnitude bounded by EVENT COUNT regardless of how finely any
// one event's disk is subdivided, and patches sharing the same event legitimately don't need to
// out-rank each other (comp/dilat/ring never spatially overlap within one event by construction).
// Patches are PLAIN closed polygons (no isSlip/isFault wiring, unlike the slip-model patches) —
// clicking one just shows the ordinary polygon Remove/fill menu. `name` = the Scene Objects group
// label ("Focal mechanisms" if null/empty). Returns the number of patches added.
GMTVTK_API int gmtvtk_add_meca_h(void *handle, const double *xy, const int *vcounts, int npatch,
                                 const double *rgb, const int *evid,
                                 double compR, double compG, double compB,
                                 double dilatR, double dilatG, double dilatB,
                                 double rimR, double rimG, double rimB, double rimWidthPct,
                                 const char *name,
                                 int plotDate, const char *dateFont, int dateFontSize,
                                 double dateR, double dateG, double dateB,
                                 int dateBold, int dateItalic) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xy || !vcounts || !rgb || !evid || npatch < 1) return 0;
	const std::string grp = (name && name[0]) ? name : "Focal mechanisms";
	// Wire format: vcounts[p] > 0 = a simple closed FILL ring; vcounts[p] < 0 = |vcounts[p]|
	// vertices of an open POLYLINE to stroke (rim circle / nodal-plane curve). Entries sharing
	// the same evid[p] (the cross-event depth rank, ei*3+role from Julia) are batched into ONE
	// actor — one fill actor per event per colour role, one line actor per event — which is
	// what keeps a real 100+-event catalog at a few hundred actors instead of tens of thousands
	// (the "horribly slow" bug, 2026-07-04). rgb is uniform within a rank by construction
	// (role determines colour), so the first entry's colour serves the whole group.
	struct MecaIn { int p; std::vector<std::vector<std::array<double,3>>> rings, plines;
	                double cx = 0.0, cy = 0.0; int nv = 0; };
	std::map<int, MecaIn> groups;                      // keyed by rank; std::map = build in rank order
	int xyoff = 0, added = 0;
	for (int p = 0; p < npatch; ++p) {
		const bool isline = vcounts[p] < 0;
		const int nv = isline ? -vcounts[p] : vcounts[p];
		if (nv < (isline ? 2 : 3)) { xyoff += 2 * nv; continue; }
		std::vector<std::array<double,3>> rg;
		rg.reserve(nv);
		double cx = 0.0, cy = 0.0;
		for (int i = 0; i < nv; ++i) {
			const double x = xy[xyoff + 2*i], y = xy[xyoff + 2*i + 1];
			rg.push_back({ x, y, 0.0 });
			cx += x; cy += y;
		}
		xyoff += 2 * nv;
		auto it = groups.find(evid[p]);
		if (it == groups.end()) { it = groups.emplace(evid[p], MecaIn{}).first; it->second.p = p; }
		MecaIn &mi = it->second;
		(isline ? mi.plines : mi.rings).push_back(std::move(rg));
		mi.cx += cx; mi.cy += cy; mi.nv += nv;
	}
	// A single BATCH-WIDE reference radius (largest ball, from each event's own rim/nodal-line
	// group) sizes the cross-ball Z-position step below — see mecaBuildPatch's comment for why a
	// batch-shared, on-screen-scaled step (not a fixed raw-Z unit) is what makes it survive both
	// geographic vs cartesian zfac AND the user's VE slider.
	double refRadius = 0.0;
	for (auto &kv2 : groups) {
		if (kv2.first % 3 != 2) continue;          // only the rim/nodal-line group has the full disk extent
		MecaIn &mi = kv2.second;
		if (mi.plines.empty() || mi.nv == 0) continue;
		const double ccx = mi.cx / mi.nv, ccy = mi.cy / mi.nv;
		const auto &first = mi.plines.front();
		if (first.empty()) continue;
		const double ddx = (first[0][0] - ccx) * s->xfac, ddy = first[0][1] - ccy;
		refRadius = std::max(refRadius, std::sqrt(ddx*ddx + ddy*ddy));
	}
	const double zStep = (refRadius > 0.0) ? 0.02 * refRadius : 1.0;
	for (auto &kv2 : groups) {
		const int rank = kv2.first;
		MecaIn &mi = kv2.second;
		Polygon pg;
		const auto &vref = mi.rings.empty() ? mi.plines.front() : mi.rings.front();
		pg.v.assign(vref.begin(), vref.end());
		pg.v.push_back(pg.v.front());
		pg.closed = true;
		pg.isMeca = true;
		pg.groupName = grp;
		pg.name = "beachball patch " + std::to_string(mi.p + 1);   // vestigial: isMeca patches never get their own row
		pg.fillColor[0] = rgb[3*mi.p]; pg.fillColor[1] = rgb[3*mi.p + 1]; pg.fillColor[2] = rgb[3*mi.p + 2];
		pg.fillOpacity = 1.0;                          // beachball quadrants are SOLID-filled (Mirone FaceColor)
		const double z0raw = s->gridZ.empty() ? 0.0 : sampleZ(s, mi.cx / mi.nv, mi.cy / mi.nv);
		const double z0 = std::isnan(z0raw) ? 0.0 : z0raw;
		// (2026-07-05, USER LAW) Every ball's own geometry is ALWAYS the complete, uncut disk —
		// NEVER permanently clip/delete part of one ball's fill or stroke against a neighbour.
		// A geometric clip (tried and reverted same day) bakes the neighbour's PLOT-TIME position
		// into this ball's polydata; dragging only moves the neighbour's actor (SetPosition), so the
		// bake goes stale and leaves a permanent "bite" where the neighbour USED to be. Occlusion
		// between balls is depth-test ONLY (see mecaBuildPatch/mecaBuildLines: both fill and line
		// carry the SAME real-Z-per-rank convention, no incomparable polygon-offset bias) — a real
		// GPU depth test is re-evaluated every frame from each actor's CURRENT transform, so it
		// tracks a drag for free with zero rebuild, and never mutates anyone's shape.
		if (!mi.rings.empty()) {
			mecaBuildPatch(s, pg, z0, rank, zStep, mi.rings);
		} else {
			mecaBuildLines(s, pg, z0, rank, zStep, rimWidthPct, mi.plines);
		}
		// Drag bookkeeping: fold this rank's actor(s) into its EVENT's MecaBall (rank = ei*3+role,
		// see the struct comment). The role==2 (rim/nodal-line) rank alone carries the authoritative
		// centre + radius reference, in the SAME xy convention as `xy`/pg.v (x pre-divided by xfac
		// for the screen-round trick) — mecaHitAt/mecaUpdateAnchor (85_polygon.cpp) reproject it the
		// same way, so the convention only has to be internally consistent, never "true degrees".
		{
			const int ei = rank / 3, role = rank % 3;
			MecaBall *mb = nullptr;
			for (auto &b : s->mecaBalls) if (b.groupName == grp && b.event == ei) { mb = &b; break; }
			if (!mb) { s->mecaBalls.push_back(MecaBall{}); mb = &s->mecaBalls.back(); mb->groupName = grp; mb->event = ei; }
			if (pg.fill) mb->actors.push_back(pg.fill.Get());
			if (pg.line) mb->actors.push_back(pg.line.Get());
			if (!pg.v.empty()) mb->zLow = std::min(mb->zLow, pg.v[0][2]);   // vertex Z is z0 only now (rank lives in actor Position) — mecaHitAt/mecaUpdateAnchor placeholder, not a real occlusion key
			if (role == 2 && !pg.v.empty()) {
				mb->x0 = mi.cx / mi.nv; mb->y0 = mi.cy / mi.nv;
				// pg.v[0] is the rim circle's angle-0 vertex, i.e. on the RAW (pre-scale) x-axis —
				// its raw x-distance from centre is rdeg/xfac (the ellipse's x semi-axis in this
				// pre-scaled space), NOT the ball's true on-screen radius. The actor's SetScale(xfac,…)
				// is what turns that ellipse into a round ball; scale ddx by xfac here so mb->radius is
				// the TRUE visual radius (uniform in every direction), matching what mecaCoveredByAnyBall/
				// mecaClipTrail (85_polygon.cpp) test against.
				const double ddx = (pg.v[0][0] - mb->x0) * s->xfac, ddy = pg.v[0][1] - mb->y0;
				mb->radius = std::sqrt(ddx*ddx + ddy*ddy);
			}
		}
		pg.stack = s->vecSeq++;                        // lands on the shared vector pile
		s->polys.push_back(pg);
		++added;
	}
	if (added == 0) return 0;
	// Cache this batch's colours/rim-width AND date-label settings for the group's properties dialog
	// (mecaGroupPropsDialog, 50_scene.cpp) to pre-fill from, without asking Julia — the ACTUAL Apply
	// round-trip still goes through Julia (a new rim width needs fresh geodesic geometry). Every field
	// the dialog can show MUST be cached here: before this fix only compColor/dilatColor/rimColor/
	// rimWidthPct were, so opening the dialog on a catalog plotted WITH "Plot event date" on still
	// showed the checkbox OFF (MecaGroupProps' struct default) — touching any OTHER control (e.g.
	// outline colour) then fired commit(), which read that wrong OFF state and round-tripped
	// plotdate=0 to Julia, silently deleting the date labels the user already had on screen.
	auto fillCache = [&](MecaGroupProps &g) {
		g.compColor[0]=compR; g.compColor[1]=compG; g.compColor[2]=compB;
		g.dilatColor[0]=dilatR; g.dilatColor[1]=dilatG; g.dilatColor[2]=dilatB;
		g.rimColor[0]=rimR; g.rimColor[1]=rimG; g.rimColor[2]=rimB;
		g.rimWidthPct = rimWidthPct;
		g.plotDate = plotDate != 0;
		g.dateFont = (dateFont && dateFont[0]) ? dateFont : "Arial";
		g.dateFontSize = dateFontSize > 0 ? dateFontSize : 7;
		g.dateColor[0]=dateR; g.dateColor[1]=dateG; g.dateColor[2]=dateB;
		g.dateBold = dateBold != 0;
		g.dateItalic = dateItalic != 0;
	};
	bool found = false;
	for (auto &g : s->mecaGroups) if (g.name == grp) { fillCache(g); found = true; break; }
	if (!found) {
		MecaGroupProps g; g.name = grp;
		fillCache(g);
		s->mecaGroups.push_back(g);
	}
	// Wire each ball to its "Plot event date" label (gmtvtk_add_texts_h groupName+mecaEvent tag), if
	// any — so a drag (mecaDragTo, 85_polygon.cpp) carries the date text along instead of leaving it
	// behind at the epicenter. Idempotent re-scan: safe whether the texts were added before or after
	// this call, and cheap (at most one match per ball).
	for (auto &mb : s->mecaBalls) {
		if (mb.groupName != grp) continue;
		mb.dateLabel = nullptr;
		for (auto &tl : s->texts)
			if (tl.groupName == grp && tl.mecaEvent == mb.event) { mb.dateLabel = tl.actor.Get(); break; }
	}
	applyVectorStacking(s);
	rebuildSceneObjects(s);
	// A recolour (gmtvtk_remove_meca_group_h then straight back here) does TWO rebuilds of the Scene
	// Objects panel back-to-back with no real event-loop turn in between — rebuildSceneObjects wipes
	// the OLD QTreeWidget via deleteLater(), which normally only actually runs once control returns
	// to the top of the app's event loop; back-to-back rebuilds can otherwise leave the stale tree
	// (and its old "Focal mechanisms" row) alive alongside the new one until then. Force the pending
	// deferred delete through NOW so the panel never shows a transient duplicate row.
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	// Beachballs are placed at their true world lon/lat (geodesic), but a still-blank empty
	// launcher's camera is framing its tiny placeholder plane, nowhere near that -> the batch
	// plots successfully yet is invisible (looked like "loading does nothing"). Same "first
	// content into an empty window: frame it" rule gmtvtk_add_surface_h already applies. MUST
	// reset from axesRen (mecaBuildPatch adds pg.fill there, never to s->ren) since ResetCamera
	// fits ONLY that renderer's own props -> resetting s->ren sees just the hidden placeholder
	// and leaves the camera pointed at nothing; axesRen shares s->ren's camera object (SetActiveCamera
	// in buildAndShow) so fitting through it still repositions the one camera both renderers use.
	if (s->emptyStart) (s->axesRen ? s->axesRen : s->ren)->ResetCamera();
	s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return added;
}

// Attach per-EVENT hover metadata (date/magnitude/depth, newline-separated display text) to an
// already-plotted focal-mechanism batch: infos[ei] goes to the group's ball whose event index is
// ei — the same 0-based ei the host encoded as evid = ei*3+role in gmtvtk_add_meca_h, so the two
// calls pair naturally. onMouseMove (10_geometry.cpp) pops the string as a tooltip when the cursor
// is over that ball, reusing the symbol-hover mechanism (same anti-flicker rules). Returns the
// number of balls that received a string.
GMTVTK_API int gmtvtk_set_meca_infos_h(void *handle, const char *name, const char *const *infos, int n) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !infos || n < 1) return 0;
	const std::string grp = (name && name[0]) ? name : "Focal mechanisms";
	int nset = 0;
	for (auto &b : s->mecaBalls)
		if (b.groupName == grp && b.event >= 0 && b.event < n && infos[b.event]) {
			b.info = infos[b.event];
			++nset;
		}
	return nset;
}

// Register the callback for the focal-mechanism GROUP properties dialog (mecaGroupPropsDialog,
// 50_scene.cpp): fn(scene, groupName, "key=value\n…") on Apply. nullptr to detach.
GMTVTK_API void gmtvtk_set_meca_props_callback(JuliaMecaPropsFn fn) {
	g_juliaMecaProps = fn;
}

// Remove every isMeca patch sharing groupName (a whole focal-mechanism batch) plus its cached
// properties-dialog state. Used by the Scene Objects group row's "Remove" menu AND as the first
// step of a recolour/re-stroke (Julia removes the old batch, then re-plots it fresh — see
// gmtvtk_set_meca_props_callback). Returns 1 if the scene handle was alive, 0 otherwise.
GMTVTK_API int gmtvtk_remove_meca_group_h(void *handle, const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !name) return 0;
	deleteMecaGroup(s, QString::fromUtf8(name));
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);   // see gmtvtk_add_meca_h
	return 1;
}

// --- test-only hooks for the fault-trace endpoint logic (exercised by the Julia test suite) -------
// Compiled ONLY into gmtvtk_test.dll (GMTVTK_TEST_API, set by the gmtvtk_test CMake target).
// The production gmtvtk.dll never sees these symbols at all — not hidden, not exported.
#ifdef GMTVTK_TEST_API
// test hook: diagnostic — the format (0=NativeFormat/registry, 1=IniFormat) and fileName() of the
// app's actual settings store (igmtSettings(), 30_app.cpp), into a caller buffer. Returns the format.
GMTVTK_API int gmtvtk_settings_format_test(char *buf, int cap) {
	QSettings st = igmtSettings();
	QByteArray fn = st.fileName().toUtf8();
	if (buf && cap > 0) {
		int n = std::min((int)fn.size(), cap - 1);
		memcpy(buf, fn.constData(), n);
		buf[n] = '\0';
	}
	return (int)st.format();
}

// Inject a 2-vertex fault line (lon1,lat1)->(lon2,lat2) into the scene so the apply logic has a
// target without going through the interactive draw tool. Returns the number of fault polygons.
GMTVTK_API int gmtvtk_fault_add_test(void *scene, double lon1, double lat1, double lon2, double lat2) {
	Scene *s = (Scene*)scene; if (!s) return 0;
	Polygon pg; pg.isFault = true; pg.closed = false; pg.name = "Fault 1";
	pg.v = { { lon1, lat1, 0.0 }, { lon2, lat2, 0.0 } };
	pg.stack = s->vecSeq++;
	polyRebuildLine(s, pg);
	s->polys.push_back(pg);
	int n = 0; for (auto& p : s->polys) if (p.isFault) ++n;
	return n;
}

// test hook: inject a polygon/polyline from a flat xyz array (npts * 3 doubles) and put it straight
// into vertex-edit mode (polyEnterEdit) — bypasses the double-click-to-enter-edit gesture so the
// Ctrl+C-copies-line-vertices path (60_profile.cpp keyPressEvent) can be tested directly. Returns
// the new polygon's index (s->polyEdit).
GMTVTK_API int gmtvtk_poly_edit_add_test(void *scene, const double *xyz, int npts, int closed) {
	Scene *s = (Scene*)scene; if (!s || npts <= 0) return -1;
	Polygon pg; pg.closed = closed != 0; pg.name = "test poly";
	for (int i = 0; i < npts; ++i) pg.v.push_back({ xyz[3*i], xyz[3*i+1], xyz[3*i+2] });
	pg.stack = s->vecSeq++;
	polyRebuildLine(s, pg);
	s->polys.push_back(pg);
	const int idx = (int)s->polys.size() - 1;
	polyEnterEdit(s, idx);
	return s->polyEdit;
}

// Run the real endpoint-recompute core (the same faultApplyGeom the dialog calls). Writes the new
// endpoint to out2[0..1] and returns the fault line's vertex count after the apply (0 on failure).
GMTVTK_API int gmtvtk_fault_apply_test(void *scene, double strike, double len, int geog, double *out2) {
	Scene *s = (Scene*)scene; if (!s) return 0;
	double lo = 0, la = 0;
	if (!faultApplyGeom(s, strike, len, geog != 0, &lo, &la)) return 0;
	if (out2) { out2[0] = lo; out2[1] = la; }
	for (auto& p : s->polys) if (p.isFault) return (int)p.v.size();
	return 0;
}

// test hook: newline-joined text of every label in the Scene Objects panel (lets the test assert
// the "<fault> — plane" handle row actually exists).
GMTVTK_API const char *gmtvtk_objrows_test(void *scene) {
	static std::string buf; buf.clear();
	Scene *s = (Scene*)scene; if (!s || !s->objPanel) return "";
	for (QLabel *l : s->objPanel->findChildren<QLabel*>()) {
		const std::string t = l->text().toStdString();
		if (!t.empty()) { buf += t; buf += '\n'; }
	}
	return buf.c_str();
}

// test hook: z-range + vertex count of the fault trace line geometry (draped if z spans the relief,
// a flat chord if ~constant). out[0]=zmin out[1]=zmax out[2]=npts. Returns 1 if a fault line exists.
GMTVTK_API int gmtvtk_trace_zbounds_test(void *scene, double *out) {
	Scene *s = (Scene*)scene; if (!s) return 0;
	for (auto& p : s->polys) if (p.isFault && p.linePD) {
		double b[6] = {0,0,0,0,0,0}; p.linePD->GetBounds(b);
		if (out) { out[0] = b[4]; out[1] = b[5]; out[2] = (double)p.linePD->GetNumberOfPoints(); }
		return 1;
	}
	return 0;
}

// test hook: drag focal-mechanism ball `idx` (0-based, plot order) by (dx,dy) in its own xy
// convention (see MecaBall's struct comment) — calls the SAME mecaDragTo the live mouse drag uses,
// skipping only the screen-to-world projection step. out3 (or null) receives [offX, offY, anchor?].
// Returns 1 if idx was valid.
GMTVTK_API int gmtvtk_meca_drag_test(void *scene, int idx, double dx, double dy, double *out3) {
	Scene *s = (Scene*)scene;
	if (!s || idx < 0 || idx >= (int)s->mecaBalls.size()) return 0;
	MecaBall &mb = s->mecaBalls[idx];
	mecaDragTo(s, idx, mb.x0 + dx, mb.y0 + dy);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	if (out3) { out3[0] = mb.offX; out3[1] = mb.offY; out3[2] = mb.anchor ? 1.0 : 0.0; }
	return 1;
}

// test hook: diagnostic — s->symArmed, whether the yellow handle actor exists/is visible, and its
// current point count. out4 = [symArmed, handleExists, handleVisible, handleNumPoints].
GMTVTK_API void gmtvtk_sym_debug_test(void *scene, double *out4) {
	Scene *s = (Scene*)scene;
	if (!s || !out4) return;
	out4[0] = s->symArmed;
	out4[1] = s->symHandle ? 1 : 0;
	out4[2] = (s->symHandle && s->symHandle->GetVisibility()) ? 1 : 0;
	out4[3] = (s->symHandlePD && s->symHandlePD->GetPoints()) ? s->symHandlePD->GetPoints()->GetNumberOfPoints() : 0;
}

// test hook: send a synthetic Ctrl+C key press to the GL widget (GLView::keyPressEvent) — same
// dispatch mechanism as gmtvtk_symbol_ui_drag_test's mouse events, exercising the REAL copy-armed-
// symbol-to-clipboard code path, not a bypass.
GMTVTK_API void gmtvtk_send_ctrlc_test(void *scene) {
	Scene *s = (Scene*)scene;
	if (!s || !s->widget) return;
	QKeyEvent ev(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, "c");
	QApplication::sendEvent(s->widget, &ev);
}

// test hook: current camera position(3)+focal point(3) into out6 — used to prove Ctrl+C does/doesn't
// leak into the gizmo's bare-'c' recenter-camera hotkey (20_gizmo.cpp KeyCB) when the copy-armed-
// symbol branch (60_profile.cpp) doesn't consume the event. Returns 1 if scene/camera valid.
GMTVTK_API int gmtvtk_camera_get_test(void *scene, double *out6) {
	Scene *s = (Scene*)scene;
	if (!s || !s->ren || !out6) return 0;
	vtkCamera *cam = s->ren->GetActiveCamera();
	if (!cam) return 0;
	double pos[3], fp[3]; cam->GetPosition(pos); cam->GetFocalPoint(fp);
	for (int i = 0; i < 3; ++i) { out6[i] = pos[i]; out6[3+i] = fp[i]; }
	return 1;
}

// test hook: current clipboard text (UTF-8, caller-owned buffer semantics like other _test string
// getters — returns a pointer to a static buffer, valid until the next call).
GMTVTK_API const char *gmtvtk_clipboard_get_test() {
	static std::string buf;
	buf = QApplication::clipboard()->text().toStdString();
	return buf.c_str();
}

// test hook: place a NATIVE symbol (kind 0=circle/1=square/2=star, GMT codes c/s/a) at world
// (x,y,z), on-screen size `sizePx`, bypassing pixel-picking — mirrors gmtvtk_fault_add_test's
// world-coords pattern. Calls the SAME addSymbols (oneShot=true) the live one-click Symbols
// flyout uses. Returns the new layer's index in s->symbols, or -1.
GMTVTK_API int gmtvtk_symbol_add_test(void *scene, int kind, double x, double y, double z, double sizePx) {
	Scene *s = (Scene*)scene;
	if (!s) return -1;
	const double w[3] = { x, y, z };
	const char *sym = kind == 0 ? "c" : kind == 1 ? "s" : "a";
	addSymbols(s, w, 1, sym, sizePx, 1, 1.0, 0.55, 0.0, 0.0, 0.0, 0.0, 1.0, "", nullptr, true);
	return (int)s->symbols.size() - 1;
}

// test hook: drag native symbol `idx` (index into s->symbols) to world (x,y,z) — calls the SAME
// single-point update the live double-click-then-drag uses (mirrors gmtvtk_meca_drag_test).
GMTVTK_API int gmtvtk_symbol_drag_test(void *scene, int idx, double x, double y, double z) {
	Scene *s = (Scene*)scene;
	if (!s || idx < 0 || idx >= (int)s->symbols.size()) return 0;
	SymbolLayer& sl = s->symbols[idx];
	auto *pd = vtkPolyData::SafeDownCast(sl.glyph->GetInput());
	if (!pd || !pd->GetPoints() || pd->GetPoints()->GetNumberOfPoints() == 0) return 0;
	pd->GetPoints()->SetPoint(0, x * s->xfac, y, z);
	pd->GetPoints()->Modified();
	pd->Modified();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// test hook: current world (x,y,z) of symbol layer `idx`'s single point (TRUE coords, x un-baked
// out of xfac). Returns 1 if idx valid and has a point.
GMTVTK_API int gmtvtk_symbol_get_pos_test(void *scene, int idx, double *out3) {
	Scene *s = (Scene*)scene;
	if (!s || idx < 0 || idx >= (int)s->symbols.size() || !out3) return 0;
	SymbolLayer &sl = s->symbols[idx];
	auto *pd = vtkPolyData::SafeDownCast(sl.glyph->GetInput());
	if (!pd || !pd->GetPoints() || pd->GetPoints()->GetNumberOfPoints() == 0) return 0;
	double p[3]; pd->GetPoints()->GetPoint(0, p);
	out3[0] = (s->xfac != 0.0) ? p[0] / s->xfac : p[0];
	out3[1] = p[1];
	out3[2] = p[2];
	return 1;
}

// test hook: simulate a REAL double-click-then-drag gesture at world (x1,y1,z1), dragging to world
// (x2,y2,z2), by sending genuine QMouseEvents through s->widget — the SAME Qt dispatch path a live
// user's mouse goes through (GLView's overridden mousePress/DblClick/Move/ReleaseEvent, the gizmo's
// abort-guard, the symArmed/symLayerDrag state machine) — NOT just the underlying point-update
// code (that's gmtvtk_symbol_drag_test). Sequence: press-release-press-DBLCLICK-release (a complete
// double-click gesture, arms it), THEN a SEPARATE press-move-release (the actual drag).
GMTVTK_API int gmtvtk_symbol_ui_drag_test(void *scene, double x1, double y1, double z1,
                                          double x2, double y2, double z2) {
	Scene *s = (Scene*)scene;
	if (!s || !s->widget || !s->ren || !s->widget->renderWindow()) return 0;
	vtkRenderer *ren = s->ren;
	const double zc = s->zfac * s->ve;
	const double dpr = s->widget->devicePixelRatioF();
	const int Hpx = s->widget->renderWindow()->GetSize()[1];
	auto toLogical = [&](double wx, double wy, double wz) -> QPointF {
		ren->SetWorldPoint(wx * s->xfac, wy, wz * zc, 1.0);
		ren->WorldToDisplay();
		double d[3]; ren->GetDisplayPoint(d);
		return QPointF(d[0] / dpr, (Hpx - d[1]) / dpr);
	};
	const QPointF p1 = toLogical(x1, y1, z1), p2 = toLogical(x2, y2, z2);
	QWidget *w = s->widget;
	auto send = [&](QEvent::Type t, const QPointF& p, Qt::MouseButton btn, Qt::MouseButtons btns) {
		QMouseEvent ev(t, p, w->mapToGlobal(p.toPoint()), btn, btns, Qt::NoModifier);
		QApplication::sendEvent(w, &ev);
	};
	send(QEvent::MouseButtonPress,    p1, Qt::LeftButton, Qt::LeftButton);
	send(QEvent::MouseButtonRelease,  p1, Qt::LeftButton, Qt::NoButton);
	send(QEvent::MouseButtonPress,    p1, Qt::LeftButton, Qt::LeftButton);
	send(QEvent::MouseButtonDblClick, p1, Qt::LeftButton, Qt::LeftButton);
	send(QEvent::MouseButtonRelease,  p1, Qt::LeftButton, Qt::NoButton);
	send(QEvent::MouseButtonPress,    p1, Qt::LeftButton, Qt::LeftButton);
	send(QEvent::MouseMove,           p2, Qt::LeftButton, Qt::LeftButton);
	send(QEvent::MouseButtonRelease,  p2, Qt::LeftButton, Qt::NoButton);
	s->widget->renderWindow()->Render();
	return 1;
}

// test hook: PLAIN click (press-move-release, NO double-click, NO arm toggle) at world (x,y,z),
// with the release offset by (dxPx,dyPx) SCREEN pixels from the press — simulates the sub-pixel
// jitter a real mouse has between down and up on an already-armed symbol, to test whether that
// alone (i.e. NOT a deliberate drag gesture) nudges the symbol via the symArmed/symLayerDrag
// press-starts-a-drag-with-no-threshold path (85_polygon.cpp:1475).
GMTVTK_API int gmtvtk_symbol_click_jitter_test(void *scene, double x, double y, double z,
                                               double dxPx, double dyPx) {
	Scene *s = (Scene*)scene;
	if (!s || !s->widget || !s->ren || !s->widget->renderWindow()) return 0;
	vtkRenderer *ren = s->ren;
	const double zc = s->zfac * s->ve;
	const double dpr = s->widget->devicePixelRatioF();
	const int Hpx = s->widget->renderWindow()->GetSize()[1];
	ren->SetWorldPoint(x * s->xfac, y, z * zc, 1.0);
	ren->WorldToDisplay();
	double d[3]; ren->GetDisplayPoint(d);
	const QPointF p1(d[0] / dpr, (Hpx - d[1]) / dpr);
	const QPointF p2 = p1 + QPointF(dxPx, dyPx);
	QWidget *w = s->widget;
	auto send = [&](QEvent::Type t, const QPointF& p, Qt::MouseButton btn, Qt::MouseButtons btns) {
		QMouseEvent ev(t, p, w->mapToGlobal(p.toPoint()), btn, btns, Qt::NoModifier);
		QApplication::sendEvent(w, &ev);
	};
	send(QEvent::MouseButtonPress,   p1, Qt::LeftButton, Qt::LeftButton);
	send(QEvent::MouseMove,          p2, Qt::LeftButton, Qt::LeftButton);
	send(QEvent::MouseButtonRelease, p2, Qt::LeftButton, Qt::NoButton);
	s->widget->renderWindow()->Render();
	return 1;
}

// test hook: flip the flat-2D / 3-D view mode (drives the same sceneSetFlat2D the toolbar uses).
GMTVTK_API void gmtvtk_set_flat2d_test(void *scene, int on) {
	Scene *s = (Scene*)scene; if (!s) return;
	sceneSetFlat2D(s, on != 0);
}

// test hooks: open / close the REAL Vertical-elastic-deformation dialog (drives the actual lifecycle,
// so the test sees whether the plane + handle SURVIVE the dialog closing). open returns 1 if a dialog
// is up; close fires the destroyed handler (WA_DeleteOnClose).
GMTVTK_API int gmtvtk_fault_open_dialog_test(void *scene) {
	Scene *s = (Scene*)scene; if (!s) return 0;
	faultRunDialog(s);
	return s->elasticDlg ? 1 : 0;
}
GMTVTK_API void gmtvtk_fault_close_dialog_test(void *scene) {
	Scene *s = (Scene*)scene; if (!s || !s->elasticDlg) return;
	s->elasticDlg->close();
}

// Run the REAL faultUpdatePlane (gray surface patch + buried 3-D plane) and report the 3-D plane.
// out[0]=exists out[1]=npts out[2]=visibility out[3]=zTop out[4]=zBot out[5]=grayVisible.
// Returns 1 when a 3-D plane actor exists. Lets the Julia test assert the plane is actually built.
GMTVTK_API int gmtvtk_fault_plane_test(void *scene, double width, double dip, double strike,
                                       int geog, double *out) {
	Scene *s = (Scene*)scene; if (!s) return 0;
	faultUpdatePlane(s, width, dip, strike, 90.0, geog != 0);   // rake fixed: this test asserts plane geometry, not arrows
	for (auto& p : s->polys) if (p.isFault && p.faultPlane3D) {
		double b[6] = {0,0,0,0,0,0};
		if (p.faultPlane3DPD) p.faultPlane3DPD->GetBounds(b);
		if (out) {
			out[0] = 1;
			out[1] = p.faultPlane3DPD ? (double)p.faultPlane3DPD->GetNumberOfPoints() : 0;
			out[2] = (double)p.faultPlane3D->GetVisibility();
			out[3] = b[5];   // zmax = top
			out[4] = b[4];   // zmin = bottom
			out[5] = p.faultPlane ? (double)p.faultPlane->GetVisibility() : 0;
		}
		return 1;
	}
	if (out) for (int i = 0; i < 6; ++i) out[i] = 0;
	return 0;
}
#endif // GMTVTK_TEST_API

// Register the grid-metadata callback used by the grdsample dialog's "OR Ref grid" picker.
// fn(path) returns "W/E/S/N/xinc/yinc/nx/ny" (or "" on failure). nullptr to detach.
GMTVTK_API void gmtvtk_set_gridmeta_callback(JuliaGridMetaFn fn) {
	g_juliaGridMeta = fn;
}

// Register the grdsample Region cross-field recompute callback (Mirone dim_funs.m, in Julia).
// fn(which, state) -> 8 recomputed "xMin/xMax/yMin/yMax/xInc/yInc/nCols/nRows" fields. nullptr to detach.
GMTVTK_API void gmtvtk_set_dimfun_callback(JuliaDimFunFn fn) {
	g_juliaDimFun = fn;
}

// Register the File > Save Grid / Save Image callback. `fn` (Julia @cfunction, signature
// JuliaSaveFn) is called with "<kind>;<fmt>;<path>" when the user picks a file in the Save dialog;
// Julia writes the window's primary grid/image to that path. nullptr to detach.
GMTVTK_API void gmtvtk_set_save_callback(JuliaSaveFn fn) {
	g_juliaSave = fn;
}

// Register the Scene Objects "Move to new window" callback (grid rows). fn(scene, "<kind>;<name>")
// returns 1 if Julia re-opened the grid in a new window (the source window then removes it), 0 on
// failure (the grid stays put). nullptr to detach.
GMTVTK_API void gmtvtk_set_move_callback(JuliaMoveFn fn) {
	g_juliaMove = fn;
}

// Register the tide-station download callback. The two "Download Mareg …" entries on a Tide
// Stations star's right-click menu call fn(scene, mode, station): mode "2days" | "calendar",
// station = the clicked star's "Name:/Code:/Country:" block. Julia opens the download window.
GMTVTK_API void gmtvtk_set_tides_callback(JuliaTidesFn fn) {
	g_juliaTides = fn;
}

// Register the Earth-tides callback. The Geography > Earth Tides dialog calls fn(scene, req) with
// "<mode>/<startISO>/<endISO>/<lon>/<lat>/<comp>/<W>/<E>/<S>/<N>" (mode "series"|"grid", comp a
// subset of "VEN"); Julia runs GMT.earthtide. nullptr to detach.
GMTVTK_API void gmtvtk_set_earthtide_callback(JuliaEarthTideFn fn) {
	g_juliaEarthTide = fn;
}

// Prepare an EMPTY launcher to receive geographic IMAGE objects as ExtraObj images. The basemap
// must NOT promote its first tile into the window's "surface" (that row has no image-properties
// menu); instead every tile is an ExtraObj image listed in Scene Objects with the same menu. This
// recomputes the window scales from the image bbox + switches it to a flat-2-D geographic map, then
// the caller adds the image via gmtvtk_add_surface_h and calls gmtvtk_fit2d. No-op (0) on a window
// that already has data (caller then just adds the image on top, keeping the current view).
GMTVTK_API int gmtvtk_frame_for_image_h(void *handle, double x0, double x1, double y0, double y1,
                                        int geographic) {
	Scene *s = static_cast<Scene*>(handle);
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
GMTVTK_API void gmtvtk_fit2d(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	vtkCamera *cam = s->ren->GetActiveCamera();
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
GMTVTK_API int gmtvtk_grow_frame_h(void *handle, double x0, double x1, double y0, double y1) {
	Scene *s = static_cast<Scene*>(handle);
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
	vtkCamera *cam = s->ren->GetActiveCamera();      // back to the top-down flat-2-D map view
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
GMTVTK_API void gmtvtk_hide_surface(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	surfSetVisibility(s, 0);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Open an EMPTY viewer window: a FULL-chrome launcher (menus + toolbar + 2-D map) that simply has
// no data yet -> a blank dark canvas, exactly as if a (still unloaded) image were about to show.
// It is built through buildAndShow (imageOnly => no colorbar) on a tiny placeholder plane that we
// then hide, so the window carries the real UI; drop a file (or use the toolbar Open button) to
// load data, which PROMOTES into a fresh full window (emptyStart -> gmtvtk_has_surface reports 0).
GMTVTK_API void *gmtvtk_open_empty(const char *title) {
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
GMTVTK_API int gmtvtk_add_surface_h(void *handle, const float *z, int nx, int ny,
									double x0, double x1, double y0, double y1,
									const double *cz, const double *crgb, int ncolor,
									const unsigned char *img, int iw, int ih, int ibands,
									int image_only, const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !z || nx < 2 || ny < 2)
		return 0;
	double zmin = 0.0, zmax = 1.0;
	auto pd = makeGridFromArray(z, nx, ny, x0, x1, y0, y1, zmin, zmax, false);

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
		// Make this dropped grid a FULL layer: keep its full-res z (readout source when it is the active
		// grid) and its own LUT + z range (so the colorbar can be retargeted to it). applyGridStacking()
		// below puts it on top -> refreshGridColorbar() makes it the active grid + shows its colorbar.
		ex.gridZ.assign(z, z + (size_t)nx * ny);
		ex.gnx = nx; ex.gny = ny;
		ex.gx0 = x0; ex.gx1 = x1; ex.gy0 = y0; ex.gy1 = y1;
		ex.zmin = zmin; ex.zmax = zmax; ex.lut = lut;
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

	// Adopt a dropped GRID as the hover heightfield when this window had none — a blank / image canvas
	// (imageOnly: a Background-region white plane, or a bare image). The readout then reports the
	// grid's z instead of the canvas pixel colour. We DO NOT touch imageOnly (the canvas itself is NOT
	// a grid and must not gain a grid surface row); a separate gridAdopted flag flips just the readout.
	// The existing AXES extent (x0..y1), CRS and base scales are LEFT UNTOUCHED — the grid is added
	// INTO the existing frame, not promoted over it. Images carry no elevation, so they are skipped.
	if (!ex.isImage && s->imageOnly && !s->gridAdopted) {
		s->gridZ.assign(z, z + (size_t)nx * ny);       // column-major z[i*gny+j], same layout as view_grid
		s->gnx = nx; s->gny = ny;
		s->gx0 = x0; s->gx1 = x1; s->gy0 = y0; s->gy1 = y1;
		s->gdx = (nx > 1) ? (x1 - x0) / (nx - 1) : 0.0;
		s->gdy = (ny > 1) ? (y1 - y0) / (ny - 1) : 0.0;
		s->gridAdopted = true;                         // readout switches from pixel-colour to z
		surfSetVisibility(s, 0);                       // HIDE the opaque blank canvas so the grid shows through
		if (s->shadeDock) s->shadeDock->setVisible(true);   // canvas now has a shaded body -> reveal Shading dock
	}

	ex.name = (name && name[0]) ? name : ("Object " + std::to_string((int)s->extras.size() + 1));
	const bool addedGrid = !ex.isImage;
	if (addedGrid) ex.gstack = s->vecSeq++;  // unified pile: newest grid lands on top of EVERYTHING
	if (addedGrid) ex.tag    = ++s->gridTagSeq;  // UNIQUE, STABLE group tag (the Color Bar resolves by this)
	s->extras.push_back(ex);
	if (addedGrid) { applyShading(s); applyGridStacking(s); }   // shade + order the new grid in the pile
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
GMTVTK_API int gmtvtk_promote_surface_h(void *handle, const float *z, int nx, int ny,
										double x0, double x1, double y0, double y1, int geographic,
										const double *cz, const double *crgb, int ncolor,
										const unsigned char *img, int iw, int ih, int ibands,
										int image_only, const char *name) {
	Scene *s = static_cast<Scene*>(handle);
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
	if (imageOnly) { s->useTone = false; s->useSSAO = false; }   // bare picture: no PBR/lit content to tone-map/occlude (see buildAndShow)
	s->surfName  = (name && name[0]) ? name : "";

	// Plain grid -> TILED path (gz), exactly like gmtvtk_view_grid. Draped image -> single actor
	// with tcoords (pd) so the texture can sit on it. buildSceneContent removes the launcher's
	// placeholder content and rebuilds everything (surface, axes, colorbar, default 3-D view, ...).
	vtkSmartPointer<vtkPolyData> pd;
	const float *gz = nullptr; int gnx = 0, gny = 0;
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

	// Both a grid and a bare image open in top-down flat-2D (matching gmtvtk_view_grid): grids as a
	// shaded-relief map, images as the textured plane. The grid branch saves the 3-D view first.
	vtkCamera *cam = s->ren->GetActiveCamera();
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
		// A grid opens in flat-2D (top-down shaded-relief map) — the SAME switch gmtvtk_view_grid uses
		// (sceneSetFlat2D saves the just-built oblique 3-D camera for a later switch back). buildSceneContent
		// left the 3-D camera but the flag still reads 2D from the launcher, so clear it first to FORCE the
		// switch (and there is no intermediate render here, so the 3-D view never flashes on screen).
		s->flat2d = false;
		sceneSetFlat2D(s, true);
		// The empty launcher's Shading dock was created HIDDEN (no body to light back then). Now there
		// IS a surface, so re-show it FOLDED to the side strip — exactly the state a fresh grid opens in,
		// and what the Surface row click then un-folds. Without this the dock stays permanently hidden
		// when a grid is opened via Recent Files / drop into the launcher.
		if (s->shadeDock && s->shadeFoldBar) {
			if (QWidget *body = s->shadeDock->widget()) body->setVisible(false);
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
GMTVTK_API int gmtvtk_save_png(const char *path) {
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
GMTVTK_API void gmtvtk_orbit(void *handle, double az, double el, double zoom) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	vtkCamera *cam = s->ren->GetActiveCamera();
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
GMTVTK_API int gmtvtk_set_stereo(void *handle, int on) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return -1;
	vtkRenderWindow *rw = s->widget->renderWindow();
	const int want = (on < 0) ? (rw->GetStereoRender() ? 0 : 1) : (on ? 1 : 0);
	rw->SetStereoTypeToAnaglyph();
	rw->SetStereoRender(want);
	rw->Render();
	return want;
}

// ============================================================================
//  X,Y plot tool (65_xyplot.cpp) — standalone 2-D plotter. Its opaque handle is
//  an XYPlot* (NOT a Scene*), so it has its own is_alive/close/raise. It shares
//  the QApplication + the gmtvtk_process_events pump with the 3-D windows.
// ============================================================================

// Open an empty X,Y plot window (non-blocking; pump gmtvtk_process_events). Returns
// the opaque XYPlot *handle; add curves with gmtvtk_xyplot_add_series.
GMTVTK_API void *gmtvtk_xyplot_open(const char *title) {
	return buildXYPlot(title);
}

// Add one (x,y) series of `n` points to a plot window. `name` labels it in the
// legend / Object Manager (null -> "Line N"). (r,g,b) in 0..1 is the line colour
// (pass r<0 for the default); `width` in px (<=0 -> default). Returns the series
// index, or -1 on a dead handle / bad input. Renders immediately.
GMTVTK_API int gmtvtk_xyplot_add_series(void *handle, const double *x, const double *y, int n,
                                        const char *name, double r, double g, double b, double width,
                                        int lineType, int marker, double markerSize) {
	return xyAddSeries(static_cast<XYPlot*>(handle), x, y, n, name, r, g, b, width,
	                   lineType, marker, markerSize);
}

// Remove every series from a plot window.
GMTVTK_API void gmtvtk_xyplot_clear(void *handle) {
	xyClear(static_cast<XYPlot*>(handle));
}

// Is an X,Y plot handle still live (its window open)? 1 = yes, 0 = closed/invalid.
GMTVTK_API int gmtvtk_xyplot_is_alive(void *handle) {
	return xyAlive(static_cast<XYPlot*>(handle)) ? 1 : 0;
}

// Close an X,Y plot window programmatically.
GMTVTK_API void gmtvtk_xyplot_close(void *handle) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (xyAlive(p) && p->win) p->win->close();
}

// Bring an X,Y plot window to the front.
GMTVTK_API void gmtvtk_xyplot_raise(void *handle) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p) || !p->win) return;
	p->win->setWindowState(p->win->windowState() & ~Qt::WindowMinimized);
	p->win->showNormal(); p->win->raise(); p->win->activateWindow();
}

// Set the X-axis TIME mode of an X,Y plot window: X is read as Unix epoch SECONDS and the bottom
// axis ticks are formatted accordingly. fmt: 0 = linear (plain numbers), 1 = date (auto by span),
// 2 = date yyyy-mm-dd, 3 = time HH:MM, 4 = decimal year, 5 = day-of-year. Ticks auto-update on
// zoom/pan. No-op on a dead handle.
GMTVTK_API void gmtvtk_xyplot_set_xtime(void *handle, int fmt) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p))
		return;
	xySetXTime(p, fmt);
}

// Current page's X-axis time mode (0 = linear, 1..5 as gmtvtk_xyplot_set_xtime). 0 on a dead handle.
// Lets Julia read the parent page's mode so a derivative result (same x = time) inherits it.
GMTVTK_API int gmtvtk_xyplot_get_xtime(void *handle) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p))
		return 0;
	return xyCur(p).xTimeFmt;
}

// Toggle log scaling on an X,Y plot axis. axis: 0 = X (bottom), 1 = Y (left). on != 0 enables.
// Data must be positive for VTK to activate it. No-op on a dead handle.
GMTVTK_API void gmtvtk_xyplot_set_logscale(void *handle, int axis, int on) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p))
		return;
	xySetLog(p, axis, on != 0);
}

// Spector-Grant depth-to-sources over band [xa,xb] of series `sel` (the SAME fit the interactive
// drag tool runs): fit ln(power) vs wavenumber, return depth = |slope|/(4π)·unit in metres. Returns
// NaN on a dead handle / bad series / < 2 positive-power points in the band. Programmatic twin of
// the Analysis > "Depth to sources (Spector-Grant)" drag tool.
GMTVTK_API double gmtvtk_xyplot_specgrant(void *handle, int sel, double xa, double xb, double unit) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p))
		return std::nan("");
	double slope, inter, depth, xlo, xhi;
	if (!xySGFit(p, sel, xa, xb, unit, slope, inter, depth, xlo, xhi))
		return std::nan("");
	return depth;
}

// Append a line to the X,Y window's collapsible Console panel (and echo on the status bar). Used by
// the Julia callbacks to surface errors/results IN the window instead of only on the REPL's stderr.
// is_error != 0 auto-expands the (default-collapsed) panel. No-op on a dead handle / null msg.
GMTVTK_API void gmtvtk_xyplot_log(void *handle, const char *msg, int is_error) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p) || !msg)
		return;
	xyLog(p, QString::fromUtf8(msg), is_error != 0);
}

// Run an Analysis op exactly as the Analysis menu does: gate on the wired callback + the current
// Object-Manager series (xyCurrentSel, which now defaults to the last series), then dispatch to
// Julia. Returns the series index used, or -1 if not wired / no series. The programmatic twin of
// clicking an Analysis menu item — used by tests and scripts.
GMTVTK_API int gmtvtk_xyplot_run_analysis(void *handle, const char *op) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p) || !op)
		return -1;
	if (!g_juliaXYAna) { xyLog(p, "Analysis: not wired (rebuild the DLL + restart Julia)", true); return -1; }
	const int sel = xyCurrentSel(p);
	if (sel < 0) { xyLog(p, "Select a series in the Object Manager first", true); return -1; }
	g_juliaXYAna(p, op, sel);
	return sel;
}

// Set the bottom (X) and left (Y) axis titles of an X,Y plot window. Null leaves a
// title unchanged. Renders.
GMTVTK_API void gmtvtk_xyplot_set_labels(void *handle, const char *xlabel, const char *ylabel) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p))
		return;
	if (xlabel) xyCur(p).chart->GetAxis(vtkAxis::BOTTOM)->SetTitle(xlabel);
	if (ylabel) xyCur(p).chart->GetAxis(vtkAxis::LEFT)->SetTitle(ylabel);
	if (p->widget && p->widget->renderWindow())
		p->widget->renderWindow()->Render();
}

// Add a new PAGE (Excel-like tab) to an X,Y window and switch to it, so the next gmtvtk_xyplot_*
// calls (add_series / set_labels) land on the fresh page. `name` labels the tab (null -> "Page N").
// Returns the new page index, or -1 on a dead handle. Used by Julia when an Analysis result's units
// don't fit the parent axes (FFT / autocorrelation / derivatives).
GMTVTK_API int gmtvtk_xyplot_add_page(void *handle, const char *name) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p))
		return -1;
	return xyNewPage(p, name, true);
}

// Number of series on the CURRENT page (-1 on a dead handle). Lets Julia iterate the page for Save.
GMTVTK_API int gmtvtk_xyplot_series_count(void *handle) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p))
		return -1;
	return (int)xyCur(p).series.size();
}

// Number of (x,y) points in series `sel` of the current page (-1 on a dead handle / bad index).
GMTVTK_API int gmtvtk_xyplot_series_npoints(void *handle, int sel) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p))
		return -1;
	std::vector<XYSeries> &series = xyCur(p).series;
	if (sel < 0 || sel >= (int)series.size() || !series[sel].table)
		return -1;
	return (int)series[sel].table->GetNumberOfRows();
}

// Copy series `sel` (current page) into caller buffers `x`,`y` (each at least `maxn` doubles).
// Returns the number of points copied (min(npoints, maxn)), or -1 on a dead handle / bad index.
// The C side owns the vtkTables; this hands Julia a snapshot so Analysis / Save can read the data
// that lives on the page actually being shown (Julia no longer mirrors per-page series).
GMTVTK_API int gmtvtk_xyplot_get_series(void *handle, int sel, double *x, double *y, int maxn) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p) || !x || !y || maxn < 1)
		return -1;
	std::vector<XYSeries> &series = xyCur(p).series;
	if (sel < 0 || sel >= (int)series.size() || !series[sel].table)
		return -1;
	vtkTable *t = series[sel].table;
	const int n = std::min(maxn, (int)t->GetNumberOfRows());
	for (int r = 0; r < n; ++r) {
		x[r] = t->GetValue(r, 0).ToDouble();
		y[r] = t->GetValue(r, 1).ToDouble();
	}
	return n;
}

// Name of series `sel` on the current page, into `buf` (capacity `cap`, NUL-terminated). Returns the
// number of chars written (excluding NUL), or -1 on a dead handle / bad index. Lets Julia label an
// Analysis result "<name> <suffix>" without mirroring the names.
GMTVTK_API int gmtvtk_xyplot_series_name(void *handle, int sel, char *buf, int cap) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p) || !buf || cap < 1)
		return -1;
	std::vector<XYSeries> &series = xyCur(p).series;
	if (sel < 0 || sel >= (int)series.size())
		return -1;
	const std::string &nm = series[sel].name;
	const int n = std::min((int)nm.size(), cap - 1);
	std::memcpy(buf, nm.data(), n);
	buf[n] = '\0';
	return n;
}

// Register the File-menu callback (Open / Save / New) for X,Y plot windows. `fn`
// (Julia @cfunction, signature JuliaXYFn) is called fn(plot, action, sel, path):
// action "open" | "save" | "new"; sel = selected series index (Save; -1 = none);
// path = the file chosen in the native dialog. Pass nullptr to detach.
GMTVTK_API void gmtvtk_xyplot_set_callback(JuliaXYFn fn) {
	g_juliaXY = fn;
}

// Register the Analysis-menu callback. `fn` (Julia @cfunction, signature JuliaXYAnaFn) is called
// fn(plot, op, sel): op = the operation tag, sel = the selected series. Pass nullptr to detach.
GMTVTK_API void gmtvtk_xyplot_set_analysis_callback(JuliaXYAnaFn fn) {
	g_juliaXYAna = fn;
}

// Register the seed callback used when a C++-spawned X,Y window (Profile -> X,Y tool) hands its
// initial series to Julia so a QtXYPlot mirror is registered. `fn` signature JuliaXYSeedFn; null
// to detach (the window then adds the series C++-side, losing Julia Save/Analysis on it).
GMTVTK_API void gmtvtk_xyplot_set_seed_callback(JuliaXYSeedFn fn) {
	g_juliaXYSeed = fn;
}

// Register the "new blank X,Y window" callback (3-D viewer Tools > X,Y plot) so Julia registers a
// mirror for the empty window. `fn` signature JuliaXYNewFn; null to detach.
GMTVTK_API void gmtvtk_xyplot_set_new_callback(JuliaXYNewFn fn) {
	g_juliaXYNew = fn;
}

// Open a blank X,Y plot window exactly as the 3-D viewer's Tools > X,Y plot menu does (opens it in
// C++ AND registers the Julia mirror via the new-window callback). Returns the XYPlot *handle.
GMTVTK_API void *gmtvtk_open_xyplot_from_host(void) {
	return xyOpenBlankFromHost();
}

// Standalone executable entry: show the demo surface and block in the loop.
int main(int, char**) {
	gmtvtk_view_demo();
	return g_app ? g_app->exec() : 0;
}
