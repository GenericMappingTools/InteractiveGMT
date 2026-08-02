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
	if (s && !image_only && !tiled)                    // drape path: keep full-res z for hover/profile
		sceneSetGridLayer(s, z, nx, ny, x0, x1, y0, y1);   // (tiled path already populated it in buildAndShow)
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
static void configurePointCloud(Scene *s, vtkSmartPointer<vtkPolyData> pd, double pointsize,
                                double pickr, double pickg, double pickb);   // defined just below

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
	configurePointCloud(s, pd, pointsize, pickr, pickg, pickb);
	s->widget->renderWindow()->Render();
	return s;
}

// Turn a scene whose content was just built with buildSceneContent(pointCloud=true) into a real
// point-cloud display. THE one place the cloud look/behaviour is defined -- gmtvtk_view_points (own
// window) and gmtvtk_promote_points_h (empty launcher reused in place) both call it, so a cloud
// looks and behaves identically whichever door opened it (SACRED_LAW.md).
static void configurePointCloud(Scene *s, vtkSmartPointer<vtkPolyData> pd, double pointsize,
                                double pickr, double pickg, double pickb) {
	if (!s || !s->surf)
		return;
	s->surfCloud = true;      // the primary surface IS the data layer: colour bar + Z axis + readout
	                          // resolve through it (resolveActiveGrid, 50_scene.cpp)
	// Render as round points of the requested size (the Verts-only polydata draws as points).
	// Lighting OFF: the builder set a PBR material (for surfaces); shading N lit sphere
	// impostors per frame makes rotate/zoom crawl on big clouds. Flat CPT-coloured points
	// (round via the sphere impostor mask, but unlit) render far faster and read the same.
	s->surf->GetProperty()->SetRepresentationToPoints();
	s->surf->GetProperty()->SetPointSize(pointsize > 0.0 ? pointsize : 4.0);
	s->surf->GetProperty()->SetRenderPointsAsSpheres(false);  // plain GL_POINTS = one fast draw (sphere impostors are fill-heavy)
	s->surf->GetProperty()->LightingOff();
	s->surf->GetProperty()->SetInterpolationToFlat();
	// Bypass the builder's vtkPolyDataNormals stage (useless for unlit points) -> the mapper
	// draws the cloud polydata directly.
	if (auto *m = vtkPolyDataMapper::SafeDownCast(s->surf->GetMapper()))
		m->SetInputData(pd);
	// Ctrl+right-drag rubber-band selection over this cloud.
	enableRubberBand(s, pd, pickr, pickg, pickb);
}

// Promote an EMPTY launcher window into a full point-cloud viewer IN PLACE (same window), the
// cloud becoming the window's PRIMARY surface: CPT colouring by z, colour bar, its own X/Y/Z axes,
// VE gizmo, LOD interaction, rubber-band selection. Mirrors gmtvtk_promote_fv_h / _surface_h --
// recompute the scene through buildSceneContent, the EXACT same builder gmtvtk_view_points uses,
// so a dropped .laz and `view_points(D)` produce the same display. Returns 1 (window reused) or
// 0 (declined: not an empty launcher -> the caller falls back to an overlay).
GMTVTK_API int gmtvtk_promote_points_h(void *handle, const double *xyz, int npts,
                                       const double *cz, const double *crgb, int ncolor,
                                       double x0, double x1, double y0, double y1, int geographic,
                                       double pointsize, double pickr, double pickg, double pickb,
                                       const char *objname) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xyz || npts <= 0)
		return 0;
	if (!s->emptyStart)                       // a window with real data keeps it; caller overlays instead
		return 0;

	double zmin = 0.0, zmax = 1.0;
	auto pd = makePointCloud(xyz, npts, zmin, zmax);
	double xfac, zfac, ve0;
	computeScales(geographic, x0, x1, y0, y1, zmin, zmax, xfac, zfac, ve0);

	// buildSceneContent's caller contract: imageOnly, x0..y1, zmin/zmax, xfac/zfac/ve set first.
	s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1; s->zmin = zmin; s->zmax = zmax;
	s->xfac = xfac; s->zfac = zfac; s->ve = ve0;
	s->imageOnly = false;
	s->surfName  = (objname && objname[0]) ? objname : "";

	// A cloud is a 3-D object, and the launcher is sitting in flat-2D ORTHO. Switch to perspective
	// BEFORE buildSceneContent, not after: its default-view block (ResetCamera + 35deg + Zoom 1.5)
	// frames the data through whatever projection is active, so fitting under ortho and flipping to
	// perspective afterwards left the cloud clipped and off-centre.
	s->flat2d = false;
	if (s->act2D) s->act2D->setChecked(false);
	if (vtkCamera *cam = s->ren->GetActiveCamera()) cam->ParallelProjectionOff();

	buildSceneContent(s, pd, x0, x1, y0, y1, cz, crgb, ncolor, nullptr, 0, 0, 0,
	                  /*edges=*/0, /*pointCloud=*/true, geographic, nullptr, 0, 0, /*blankStart=*/false);
	s->emptyStart = false;

	// Rebuild the gizmo against the REAL data (the launcher's was sized for the 0..1 placeholder).
	disableGizmo(s);
	s->giz = enableGizmo(s, 0.01);

	// applyShading FIRST, configurePointCloud AFTER it -- the same order a fresh window uses
	// (buildAndShow runs applyShading internally, then gmtvtk_view_points configures the cloud).
	// applyShading's applySurfStyle re-asserts the PBR/lit material on s->surf, so running it AFTER
	// the cloud config re-lit the points and washed the CPT colours out to a pale IBL tint.
	applyShading(s);
	configurePointCloud(s, pd, pointsize, pickr, pickg, pickb);
	// Only now (surfCloud set) does the cloud resolve as the active layer -> the bar targets it, and
	// the Scene Objects rows are built against the bar that actually ends up on screen.
	refreshGridColorbar(s);
	rebuildSceneObjects(s);
	s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
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
// name. Used to add a "layerN" blank grid HIDDEN: it still gets a (unchecked) Scene Objects
// row, but its surface is not drawn. Re-renders + rebuilds the panel so the checkbox tracks it.
GMTVTK_API int gmtvtk_set_object_visible(void *handle, const char *name, int vis) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !name)
		return 0;
	for (auto &ex : s->extras) {
		if (ex.name != name) continue;
		if (ex.actor) ex.actor->SetVisibility(vis ? 1 : 0);
		if (ex.drape) ex.drape->SetVisibility(vis ? 1 : 0);
		if (!ex.isImage) refreshGridColorbar(s);   // grid hidden/shown -> retarget the colorbar + readout
		rebuildSceneObjects(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return 1;
	}
	// The BASE surface goes through this SAME setter — not a parallel show/hide pair. gmtvtk_hide_surface
	// could only hide, and gmtvtk_hide_other_grids already treats base + extras uniformly, so anything
	// that unchecked the base (the derived-variable display law: "the source is UNCHECKED") had no way
	// to check it back. "" means the window's primary, the same convention _find_object_named uses.
	if (s->surf && (!*name || s->surfName == name)) {
		surfSetVisibility(s, vis ? 1 : 0);
		if (s->drape) s->drape->SetVisibility(vis ? 1 : 0);
		refreshGridColorbar(s);
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

// Same as gmtvtk_add_overlay_ex_h (groupName folds Scene Objects rows sharing it under one
// collapsible parent with hide-all/Remove-all -- SACRED_LAW.md: "each file creates its own master
// handle"), plus an optional INTERIOR point swarm (x,y,z, `nInterior` points -- null/0 for none)
// stashed on the new overlay instead of being added to the scene. SHAPENC "bounded ensemble"
// support (shapenc.jl/drop.jl): Mirone's own convention is to plot only an ensemble's OUTER/INNER
// boundary polygon, not its raw point swarm -- the swarm rides along on the OUT polygon's Overlay
// (Overlay::interiorXYZ, 10_geometry.cpp) until the user picks "Plot interior points" in its Scene
// Objects context menu (popupLineObjectMenu, 55_lineprops.cpp), which adds it as its own ordinary
// points overlay (in the SAME group) via this SAME addOverlay() -- one function, not a fork.
// `isShapencBoundary` marks the new overlay as a coverage boundary, not a measurable line --
// Overlay::isShapencBoundary (10_geometry.cpp) -- so its context menu drops "Line length…"/
// "Azimuth…"/"Convert to points" (popupLineObjectMenu, 55_lineprops.cpp; user: these make no sense
// on a SHAPENC OUT/IN polygon). Pass it for BOTH the OUT polygon and its IN holes -- all of a
// bounded ensemble's boundary rows, not just the outer ring.
GMTVTK_API int gmtvtk_add_overlay_bounded_h(void *handle, const double *xyz, int npts, const int *segoff, int nseg,
									   int mode, double r, double g, double b,
									   double linewidth, double pointsize, const char *name, const char *groupName,
									   const double *interiorXYZ, int nInterior, int isShapencBoundary) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	addOverlay(s, xyz, npts, segoff, nseg, mode, r, g, b, linewidth, pointsize, name, groupName, nullptr,
	           interiorXYZ, nInterior, isShapencBoundary != 0);
	return 1;
}

// Fill caller-allocated `out` (n points -> 3n doubles) with an overlay's raw x,y,z, found by its
// actor pointer -- ALL in-process, no file (project rule: never write a temp file). COLUMN-MAJOR
// (out[0..n)=x, out[n..2n)=y, out[2n..3n)=z), NOT interleaved per-point -- so the Julia caller can
// `reshape(buf, n, 3)` straight into an n×3 Matrix with ZERO copy (matches Julia's own column-major
// layout exactly); an interleaved fill would force a real copy/transpose on the Julia side to
// un-interleave it, which is exactly the wasted work this is avoiding by not going through a file.
// Used by "Point cloud view" (55_lineprops.cpp) to hand a SHAPENC interior-points overlay's swarm to
// Julia's view_points; `n` must match the overlay's actual point count exactly (the caller already
// knows it -- it built the menu action from the same vtkPoints this reads). Returns 1 on success, 0
// if the actor/overlay isn't found or `n` doesn't match.
GMTVTK_API int gmtvtk_overlay_points_h(void *handle, void *actorPtr, double *out, int n) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !actorPtr || !out || n <= 0)
		return 0;
	vtkActor *a = static_cast<vtkActor*>(actorPtr);
	for (auto &ov : s->overlays) {
		if (ov.actor.Get() != a || !ov.baseLine)
			continue;
		vtkPoints *pts = ov.baseLine->GetPoints();
		if (!pts || (int)pts->GetNumberOfPoints() != n)
			return 0;
		for (int i = 0; i < n; ++i) {
			double p[3]; pts->GetPoint(i, p);
			out[i] = p[0]; out[n + i] = p[1]; out[2*n + i] = p[2];
		}
		return 1;
	}
	return 0;
}

// Extended form of gmtvtk_add_overlay_h: also tags the overlay with a Scene Objects GROUP name
// (rebuildSceneObjects folds every overlay sharing the same non-empty group under one collapsible
// parent row -- same fold the slip-model patches use) and, for line mode, a per-SEGMENT hover info
// block: nseg records joined by RS ('\x1e', the gmtvtk_add_symbols_h `info` convention), shown as a
// tooltip when the cursor hovers that segment (pickOverlayAt's segment index). groupName/info may
// be null/"" (behaves exactly like gmtvtk_add_overlay_h). Used by Geography > Plate boundaries
// (7 boundary-type layers, one group, each segment carrying its own type/velocity/plate-pair text).
// UNLIKE gmtvtk_add_overlay_h, `linewidth` here is in POINTS, not device pixels -- this is the
// user-facing unit everywhere else a line width is entered (the Line Properties dialog's width
// box, 55_lineprops.cpp) -- converted to px with the SAME dpi/72 formula that dialog uses, so a
// layer added here reads back the exact width a user would have typed. `pointsize` (points-mode
// overlays only) is unchanged, still px. Returns 1 if added.
GMTVTK_API int gmtvtk_add_overlay_ex_h(void *handle, const double *xyz, int npts, const int *segoff, int nseg,
								      int mode, double r, double g, double b,
								      double linewidth, double pointsize,
								      const char *name, const char *groupName, const char *info) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	double dpi = 72.0;
	if (s->widget && s->widget->renderWindow() && s->widget->renderWindow()->GetDPI() > 0)
		dpi = s->widget->renderWindow()->GetDPI();
	const double pxPerPt = dpi / 72.0;
	addOverlay(s, xyz, npts, segoff, nseg, mode, r, g, b, linewidth * pxPerPt, pointsize, name, groupName, info);
	return 1;
}

// Same as gmtvtk_add_overlay_ex_h, plus `noConvertToPoints`: suppresses ONLY the "Convert to
// points"/"Convert to line" context-menu item (Overlay::noConvertToPoints, 10_geometry.cpp) --
// unlike gmtvtk_add_overlay_bounded_h's isShapencBoundary, "Line length…"/"Azimuth…" stay. Also
// `zIsPlaceholder`: the caller's source was 2-column (x,y only), z=0 is a stored-geometry filler,
// not real data -- showLineDataTable (55_lineprops.cpp) must show only #/X/Y, never invent a Z
// column (Overlay::zIsPlaceholder). Used by Geography > Magnetic isochrons > GPlates (a coverage
// line you'd still want to measure, just never scatter to points, and whose source truly has no
// z). Returns 1 if added.
GMTVTK_API int gmtvtk_add_overlay_ex2_h(void *handle, const double *xyz, int npts, const int *segoff, int nseg,
								      int mode, double r, double g, double b,
								      double linewidth, double pointsize,
								      const char *name, const char *groupName, const char *info,
								      int noConvertToPoints, int zIsPlaceholder) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	double dpi = 72.0;
	if (s->widget && s->widget->renderWindow() && s->widget->renderWindow()->GetDPI() > 0)
		dpi = s->widget->renderWindow()->GetDPI();
	const double pxPerPt = dpi / 72.0;
	addOverlay(s, xyz, npts, segoff, nseg, mode, r, g, b, linewidth * pxPerPt, pointsize, name, groupName, info,
	           nullptr, 0, false, false, noConvertToPoints != 0, zIsPlaceholder != 0);
	return 1;
}

// Same as gmtvtk_add_overlay_ex2_h, plus `noDataTable`: suppresses "Show data table…" entirely
// (Overlay::noDataTable, 10_geometry.cpp) -- for overlays whose raw per-vertex table is meaningless
// to the user (e.g. Geophysics > Magnetics > Import *.gmt/*.nc cruise tracks: thousands of raw nav
// fixes). Returns 1 if added.
GMTVTK_API int gmtvtk_add_overlay_ex3_h(void *handle, const double *xyz, int npts, const int *segoff, int nseg,
								      int mode, double r, double g, double b,
								      double linewidth, double pointsize,
								      const char *name, const char *groupName, const char *info,
								      int noConvertToPoints, int zIsPlaceholder, int noDataTable) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	double dpi = 72.0;
	if (s->widget && s->widget->renderWindow() && s->widget->renderWindow()->GetDPI() > 0)
		dpi = s->widget->renderWindow()->GetDPI();
	const double pxPerPt = dpi / 72.0;
	addOverlay(s, xyz, npts, segoff, nseg, mode, r, g, b, linewidth * pxPerPt, pointsize, name, groupName, info,
	           nullptr, 0, false, false, noConvertToPoints != 0, zIsPlaceholder != 0, noDataTable != 0);
	return 1;
}

// Read a line OVERLAY's current pen by its Scene Objects name, so Save Session can capture edits the
// user made AFTER the layer was added (coastlines/borders/rivers are :menu recipes that otherwise
// replay with the default pen). out = { r, g, b, width_px, style(0 solid/1 dashed/2 dotted), opacity }.
// Returns 1 if a matching overlay was found (out filled), 0 otherwise (out untouched).
GMTVTK_API int gmtvtk_overlay_style_h(void *handle, const char *name, double *out) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !name || !out) return 0;
	for (auto &ov : s->overlays) {
		if (!ov.actor || ov.name != name) continue;
		double c[3]; ov.actor->GetProperty()->GetColor(c);
		out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
		out[3] = ov.actor->GetProperty()->GetLineWidth();
		out[4] = (double)ov.lineStyle;
		out[5] = ov.actor->GetProperty()->GetOpacity();
		return 1;
	}
	return 0;
}

// Apply a pen (colour 0..1, width px, style 0/1/2, opacity) to the line OVERLAY named `name` — the
// Load Session twin of gmtvtk_overlay_style_h, used to re-apply a coastline/border/river layer's saved
// edits after it is re-generated with the default pen. Reuses applyLineStyle for the stipple so it
// matches the Line Properties dialog exactly. Returns 1 if applied, 0 if no overlay matched.
GMTVTK_API int gmtvtk_set_overlay_style_h(void *handle, const char *name,
                                          double r, double g, double b, double width, int style, double opacity) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !name) return 0;
	for (auto &ov : s->overlays) {
		if (!ov.actor || ov.name != name) continue;
		ov.actor->GetProperty()->SetColor(r, g, b);       // colour first: applyLineStyle bakes it into the stripe
		ov.actor->GetProperty()->SetLineWidth(width);
		applyLineStyle(s, ov.actor, style);               // sets ov.lineStyle + rebuilds the stipple texture (also sets opacity)
		if (style == 0) ov.actor->GetProperty()->SetOpacity(opacity);   // solid: honour saved opacity (stipple needs its own)
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return 1;
	}
	return 0;
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

// POP a floating table of `data` for this window — show_table(fig, D) and everything built on it
// (the polyline Line-length/Azimuth tables, gmtgravmag3d's track mode, grdseamount's statistics).
// `data` is COLUMN-MAJOR (Julia layout): element (row r, col c) lives at data[(size_t)c*nrows + r].
// `headers`, if non-null/non-empty, is the column names joined by TAB ('\t'), one per column
// (missing/empty -> "C1, C2, ..."). `name` (or null) titles the window. Returns 1 if shown, 0 if the
// handle is dead. The data is COPIED (the caller's pointer is only valid during this call).
//
// SACRED_LAW: this pops THE shared table dialog (buildDataTableDialog, 55_lineprops.cpp) — the same
// one the per-line "Show data table…" and the X,Y tool's "Show in Data Table" use, Save button and
// all. There is exactly ONE "show a table of numbers" implementation in the app; the old bottom-dock
// "Data Viewer" spreadsheet tab was a second one, and is gone.
GMTVTK_API int gmtvtk_set_table(void *handle, const char *name, const double *data,
								int nrows, int ncols, const char *headers) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	if (nrows < 0) nrows = 0;
	if (ncols < 0) ncols = 0;

	QStringList cols;
	if (headers && headers[0])
		cols = QString::fromUtf8(headers).split('\t');
	QStringList hdr;  hdr << "#";                       // the builder numbers the rows in column 0
	for (int c = 0; c < ncols; ++c)
		hdr << ((c < cols.size() && !cols[c].isEmpty()) ? cols[c] : QString("C%1").arg(c + 1));

	std::vector<double> vals;                            // own the numbers: the caller's buffer dies now
	vals.reserve((size_t)nrows * (size_t)ncols);
	for (int c = 0; c < ncols; ++c)
		for (int r = 0; r < nrows; ++r)
			vals.push_back(data ? data[(size_t)c * nrows + r] : 0.0);

	const QString title = (name && name[0]) ? QString::fromUtf8(name) : QString("Data");
	// Save…: the table as it stands, tab-separated with its header line — the plain text every other
	// tool in this app can read straight back.
	auto onSave = [vals, hdr, nrows, ncols, title]() {
		QString fn = QFileDialog::getSaveFileName(nullptr, "Save table", prefStartDir("table.txt"),
		                                          "Text (*.txt *.dat *.csv);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		QFile f(fn);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
		QTextStream out(&f);
		out << "#";
		for (int c = 0; c < ncols; ++c) out << '\t' << hdr[c + 1];
		out << '\n';
		for (int r = 0; r < nrows; ++r) {
			for (int c = 0; c < ncols; ++c)
				out << (c ? "\t" : "") << QString::number(vals[(size_t)c * nrows + r], 'g', 10);
			out << '\n';
		}
		f.close();
	};
	QTableWidget *tbl = buildDataTableDialog(title, nrows, hdr,
		[vals, nrows](int row, int col) { return vals[(size_t)col * nrows + row]; },
		/*editable=*/false, onSave);
	// Remember the last table popped for this window, ONLY so gmtvtk_scene_state can report its row
	// count (n_table). It is parentless and self-deleting, so drop the pointer when it goes.
	s->dataTable = tbl;
	if (tbl) QObject::connect(tbl, &QObject::destroyed, tbl, [s]() { if (sceneAlive(s)) s->dataTable = nullptr; });
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
	const std::vector<double> &X = s->prof->seriesX();
	const std::vector<double> &Y = s->prof->seriesY();
	if (X.size() < 2)
		return nullptr;
	const QByteArray t = s->prof->seriesTitle().toUtf8();
	const QByteArray xl = s->prof->seriesXLabel().toUtf8();
	const QByteArray yl = s->prof->seriesYLabel().toUtf8();
	XYPlot *p = openSeriesInXYTool(X, Y, t.isEmpty() ? "i'GMT  —  Profile" : t.constData(),
	                               xl.constData(), yl.constData());
	// This plot came OUT of `s`, so `s` is where it parks when its window is closed with the X.
	if (p) p->owner = s;
	return p;
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

// Set the Scene Objects label of the window's BASE surface. gmtvtk_view_grid always opens the base as
// the unnamed "Surface"; "Move to new window" re-opens a grid there and then calls this so the moved
// grid KEEPS its name — and with it every name-driven per-row option (e.g. a "layerN" blank
// grid's "Transplant 2nd grid…"). Null/empty -> back to the default "Surface".
GMTVTK_API void gmtvtk_set_surface_name_h(void *handle, const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	s->surfName = (name && name[0]) ? name : "";
	rebuildSceneObjects(s);                 // relabel the base row in the Scene Objects panel
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

// Show a NON-MODAL progress dialog with range 0..max (max==0 => an indeterminate "busy" marquee). Unlike
// gmtvtk_progress_show (application-modal, for a tight synchronous loop), this leaves the main window fully
// interactive — used for a long asynchronous run (NSWING) whose advance is pushed from a Julia Timer via
// gmtvtk_progress_update while the run proceeds on a separate task. Returns 1 on success, 0 on failure.
GMTVTK_API int gmtvtk_progress_show_async(int max, const char *title) {
	// ensureApp, not a bail-out: the FIRST grid of a session is exactly the slow one worth reporting,
	// and at that moment no window — and therefore no QApplication — exists yet. Returning 0 here left
	// that one case, the one the dialog is for, with no dialog at all.
	ensureApp();
	if (!QApplication::instance()) return 0;
	if (g_progress) { delete g_progress; g_progress = nullptr; }
	g_progress = new QProgressDialog();
	g_progress->setWindowTitle("Working");          // titlebar caption: fixed, distinct from the label text below
	g_progress->setLabelText(title ? title : "Working");
	g_progress->setRange(0, max > 0 ? max : 0);     // max==0 -> busy marquee (indeterminate)
	g_progress->setValue(0);
	g_progress->setMinimumDuration(0);              // show immediately
	g_progress->setCancelButton(nullptr);           // no cancel button
	g_progress->setWindowModality(Qt::NonModal);    // main window stays interactive during the run
	// Stays-on-top so a later click on the main iGMT window (during a long run) can't bury it again —
	// raise()/activateWindow() alone only win the FIRST time, not for the rest of the run.
	g_progress->setWindowFlags(g_progress->windowFlags() | Qt::WindowStaysOnTopHint);
	g_progress->show();
	g_progress->raise();
	g_progress->activateWindow();
	QApplication::processEvents();
	return 1;
}

// Set the progress bar's value AND/OR its label text. value < 0 leaves the value untouched (so a caller
// can update only the status text — e.g. the live nswing -V line + ETA). label == nullptr leaves the text.
GMTVTK_API void gmtvtk_progress_status(int value, const char *label) {
	if (!g_progress) return;
	if (value >= 0)  g_progress->setValue(value);
	if (label)       g_progress->setLabelText(QString::fromUtf8(label));
	QApplication::processEvents();
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

// Reveal + un-fold the Scene Objects dock (unfoldSceneObjects, 70_window.cpp — same helper the
// first nested-rectangle drop uses). Called from Julia after RTP3D / Total-field-to-Components
// finishes, so the user immediately sees the new grid land in the panel instead of it staying
// folded shut.
GMTVTK_API void gmtvtk_unfold_scene_objects_h(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (sceneAlive(s)) unfoldSceneObjects(s);
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
		kvi("cubeZLock", s->cubeZLock ? 1 : 0);
		// Axis box Z extent — a cube must report the SAME value on every layer (regression guard).
		if (s->axes) { double ab[6]; s->axes->GetBounds(ab); kvd("axZ0", ab[4]); kvd("axZ1", ab[5]); }
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
			o += (s->extras[i].isImage ? "image:" : s->extras[i].isMesh ? "mesh:" : "grid:");
			o += s->extras[i].name; o += ';';
		}
		// Per-polygon introspection (drives the fault-trace icon/menu regression tests): the icon kind a
		// row would get + the flags it derives from. "poly<i>=<isFault>,<closed>,<nestKind>:<name>;"
		for (size_t i = 0; i < s->polys.size(); ++i) {
			const Polygon &pg = s->polys[i];
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

// Serialize the window's RESTORABLE display state (Save Session) as a ";"-terminated "k=v;" string:
// vertical exaggeration, flat-2D mode, the full active camera (position/focal/up + parallel scale +
// view angle + projection flag) and the colorbar frame position. Two-pass like gmtvtk_scene_state
// (call with buf=nullptr to size, then again to fill). gmtvtk_apply_scene_state consumes this verbatim.
GMTVTK_API int gmtvtk_scene_state_full(void *handle, char *buf, int cap) {
	Scene *s = static_cast<Scene*>(handle);
	std::string o;
	char t[96];
	auto kvi = [&](const char *k, long v) { o += k; o += '='; o += std::to_string(v); o += ';'; };
	auto kvd = [&](const char *k, double v) { snprintf(t, sizeof(t), "%s=%.12g;", k, v); o += t; };
	if (sceneAlive(s)) {
		kvi("alive", 1);
		kvd("ve", s->ve);
		kvi("flat2d", s->flat2d ? 1 : 0);
		kvd("barX0", s->barX0); kvd("barY0", s->barY0);
		if (s->ren) {
			if (vtkCamera *cam = s->ren->GetActiveCamera()) {
				double p[3], f[3], u[3];
				cam->GetPosition(p); cam->GetFocalPoint(f); cam->GetViewUp(u);
				kvd("cam_px", p[0]); kvd("cam_py", p[1]); kvd("cam_pz", p[2]);
				kvd("cam_fx", f[0]); kvd("cam_fy", f[1]); kvd("cam_fz", f[2]);
				kvd("cam_ux", u[0]); kvd("cam_uy", u[1]); kvd("cam_uz", u[2]);
				kvd("cam_ps", cam->GetParallelScale());
				kvd("cam_va", cam->GetViewAngle());
				kvi("cam_par", cam->GetParallelProjection());
			}
		}
	}
	const int n = (int)o.size();
	if (buf && cap > 0) { int c = (n < cap - 1) ? n : cap - 1; memcpy(buf, o.data(), c); buf[c] = '\0'; }
	return n;
}

// Restore the display state produced by gmtvtk_scene_state_full (Load Session). Order matters: VE
// rescales the geometry and flat-2D moves the camera to top-down, so both run BEFORE the saved camera
// is applied verbatim on top. Missing keys are left untouched. Each key is matched whole (";key=") so
// no key can be a substring of another (barX0 vs barY0, ve vs cam_va).
GMTVTK_API void gmtvtk_apply_scene_state(void *handle, const char *kv) {
	Scene *s = static_cast<Scene*>(handle);
	if (!s || !sceneAlive(s) || !kv) return;
	const std::string buf = std::string(";") + kv;
	auto getd = [&](const char *k, double &out) -> bool {
		const std::string pat = std::string(";") + k + "=";
		const size_t p = buf.find(pat);
		if (p == std::string::npos) return false;
		out = atof(buf.c_str() + p + pat.size());
		return true;
	};
	auto geti = [&](const char *k, int &out) -> bool { double d; if (!getd(k, d)) return false; out = (int)d; return true; };

	double d; int i;
	if (getd("ve", d) && d > 0.0) { s->ve = d; applyVE(s); }         // VE first: rescales all actors
	if (geti("flat2d", i)) {                                          // then the 2-D/3-D mode switch
		if (i && !s->flat2d)      sceneSetFlat2D(s, true);
		else if (!i && s->flat2d) sceneSetFlat2D(s, false);
	}
	if (s->ren) {                                                    // finally the exact saved camera
		if (vtkCamera *cam = s->ren->GetActiveCamera()) {
			double px, py, pz, fx, fy, fz, ux, uy, uz, ps, va;
			if (geti("cam_par", i)) { if (i) cam->ParallelProjectionOn(); else cam->ParallelProjectionOff(); }
			if (getd("cam_fx", fx) && getd("cam_fy", fy) && getd("cam_fz", fz)) cam->SetFocalPoint(fx, fy, fz);
			if (getd("cam_px", px) && getd("cam_py", py) && getd("cam_pz", pz)) cam->SetPosition(px, py, pz);
			if (getd("cam_ux", ux) && getd("cam_uy", uy) && getd("cam_uz", uz)) cam->SetViewUp(ux, uy, uz);
			if (getd("cam_ps", ps)) cam->SetParallelScale(ps);
			if (getd("cam_va", va)) cam->SetViewAngle(va);
			s->ren->ResetCameraClippingRange();
		}
	}
	bool okbar = false; double bx, by;
	if (getd("barX0", bx)) { s->barX0 = bx; okbar = true; }
	if (getd("barY0", by)) { s->barY0 = by; okbar = true; }
	if (okbar && s->bar) layoutColorbar(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
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
	// In "flat image" relief look the surface is a PRE-BAKED RGBA texture, not scalar+LUT mapped --
	// mutating the CTF alone recolours the colorbar but leaves the already-baked pixels untouched.
	// rebakeLayerImage re-bakes the drape from the (now-updated) CTF; no-op when not in that mode.
	if (s->layerImgMode && !s->customLayerTexture) rebakeLayerImage(s);
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
	else for (auto &ex : s->extras) if (!ex.isImage && ex.tag == gridSel) { lut = ex.lut; break; }
	vtkColorTransferFunction *ctf = vtkColorTransferFunction::SafeDownCast(lut);
	if (!ctf) return;                         // only the CTF path supports live recolour
	ctf->RemoveAllPoints();
	for (int i = 0; i < n; ++i)
		ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
	if (gridSel < 0) {
		s->surfCtfRange = true;
		// Base relief in "flat image" look is a PRE-BAKED RGBA texture, not scalar+LUT mapped --
		// re-bake it from the just-updated CTF (extras are always live LUT-mapped, no bake needed).
		if (s->layerImgMode && !s->customLayerTexture) rebakeLayerImage(s);
	}
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
	else for (auto &ex : s->extras) if (!ex.isImage && ex.tag == gridSel) { lut = ex.lut; break; }
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

// Read the window's CRS back (the counterpart to gmtvtk_set_crs — nothing previously exposed the
// actual proj4/wkt strings, only a "has one at all" boolean via gmtvtk_scene_state). Used by "Crop
// Image (with coords)" to georeference a captured picture with the SAME CRS the window itself
// already carries, so it round-trips through Save Image as a real GeoTIFF. Truncates into the
// caller's buffers (null-terminated); returns the EPSG code (0 if none / window not alive).
GMTVTK_API int gmtvtk_get_crs(void *handle, char *proj4buf, int proj4cap, char *wktbuf, int wktcap) {
	Scene *s = static_cast<Scene*>(handle);
	if (proj4buf && proj4cap > 0) proj4buf[0] = '\0';
	if (wktbuf && wktcap > 0) wktbuf[0] = '\0';
	if (!sceneAlive(s)) return 0;
	if (proj4buf && proj4cap > 0) {
		std::strncpy(proj4buf, s->crsProj4.c_str(), proj4cap - 1);
		proj4buf[proj4cap - 1] = '\0';
	}
	if (wktbuf && wktcap > 0) {
		std::strncpy(wktbuf, s->crsWkt.c_str(), wktcap - 1);
		wktbuf[wktcap - 1] = '\0';
	}
	return s->crsEpsg;
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

// Register the Ctrl+V paste callback (paste.jl `_on_paste`, signature JuliaPasteFn), called
// fn(scene, text, rgb, w, h, nbands): exactly one of `text` (a numeric table) and `rgb` (w*h*nbands
// packed bytes, top row first, borrowed for the call only) carries the clipboard payload. Copied
// FILES never reach here — they go through the drop callback, the same path a real drop takes.
// Pass nullptr to detach (Ctrl+V then only handles copied files).
GMTVTK_API void gmtvtk_set_paste_callback(JuliaPasteFn fn) {
	g_juliaPaste = fn;
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

// Register the LIDAR2011-PT callback (Mirone cartas_militares.m 'nikles' mode). `fn` (Julia
// @cfunction, signature JuliaLidarFn) gets "op;..." from the picker: "init" (asks for the tile table,
// pushed back via gmtvtk_lidar_set_tiles) and "go;rMin/rMax/cMin/cMax;res;dir" (build the mosaic).
// nullptr to detach.
GMTVTK_API void gmtvtk_set_lidar_callback(JuliaLidarFn fn) {
	g_juliaLidar = fn;
}

// Set the path to the LIDAR2011 picker's background image (data/PTimg_lidar.jpg, mainland Portugal in
// the survey's metric frame over the extent lidarPT() hard-codes). Pushed from Julia at __init__.
GMTVTK_API void gmtvtk_set_lidar_image(const char *path) {
	g_lidarImg = QString::fromUtf8(path ? path : "");
}

// Push the survey's tile table into the open LIDAR2011 picker `dlg` (a LidarPicker*). `rects` is 4*n
// doubles — x0,x1,y0,y1 per row, row 0 being the survey's global bounding box — and `names` the
// matching newline-joined tile names, i.e. exactly the rows Julia read from data/lidarPT.dat with
// gmtread. Called SYNCHRONOUSLY from Julia's op "init", so `dlg` is the live picker.
GMTVTK_API void gmtvtk_lidar_set_tiles(void *dlg, const double *rects, const char *names, int n) {
	if (!dlg || !rects || n < 1) return;
	QStringList nm = QString::fromUtf8(names ? names : "").split('\n');
	LidarPicker *p = reinterpret_cast<LidarPicker*>(dlg);
	p->map->setTiles(rects, nm, n);
	p->map->update();
}

// One-line progress text in the open LIDAR2011 picker (replaces Mirone's aguentabar while the tiles
// are being read). Empty string hides it. `dlg` = the live LidarPicker* that issued the request.
GMTVTK_API void gmtvtk_lidar_status(void *dlg, const char *msg) {
	if (!dlg) return;
	LidarPicker *p = reinterpret_cast<LidarPicker*>(dlg);
	p->setStatus(QString::fromUtf8(msg ? msg : ""));
	QApplication::processEvents();          // we are called from inside the blocking build -> paint now
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

// Register the Save/Load Session callbacks (File menu). Save: fn(scene, path) writes THIS window's
// state to `path` (.igmtz); Load: fn(path) rebuilds a window from `path`. session.jl
// _on_save_session / _on_load_session. nullptr to detach.
GMTVTK_API void gmtvtk_set_save_session_callback(JuliaSaveSessionFn fn) {
	g_juliaSaveSession = fn;
}
GMTVTK_API void gmtvtk_set_load_session_callback(JuliaLoadSessionFn fn) {
	g_juliaLoadSession = fn;
}

// Load a .igmtz session into `scene` -- the SAME path (loadSessionIntoWindow, 70_window.cpp) the
// File > Load Session menu action uses. Called from drop.jl's _on_drop for a dropped/opened .igmtz
// (window drop, File > Open, Recent Files, desktop-icon drop) so there is ONE session-load path,
// not a second Julia-side re-derivation of the menu's priming warm-up (SACRED_LAW.md).
GMTVTK_API void gmtvtk_load_session_h(void *scene, const char *path) {
	Scene *s = static_cast<Scene*>(scene);
	if (!sceneAlive(s) || !s->win) return;
	loadSessionIntoWindow(s, s->win, QString::fromUtf8(path));
}

// Grab the ENTIRE main window (titlebar-to-statusbar, incl. the Scene Objects dock) -- unlike
// gmtvtk_save_png (VTK render surface only), this is the only way to visually diagnose a Scene
// Objects panel paint bug (stale/overlapping tree rows etc).
GMTVTK_API int gmtvtk_window_screenshot(void *scene, const char *path) {
	Scene *s = static_cast<Scene*>(scene);
	if (!sceneAlive(s) || !s->win) return 0;
	QPixmap pm = s->win->grab();
	return pm.save(QString::fromUtf8(path), "PNG") ? 1 : 0;
}

// Register the IGRF Calculator's single-point callback (Geophysics > Magnetics > IGRF). fn(state)
// with state = "lon/lat/elev_m/date_dec" returns "F/H/X/Y/Z/D/I" (or "" on failure). Same
// Julia-owned-buffer convention as gmtvtk_set_dimfun_callback. nullptr to detach.
GMTVTK_API void gmtvtk_set_igrf_point_callback(JuliaIgrfPointFn fn) {
	g_juliaIgrfPoint = fn;
}

// Register the IGRF Calculator's grid Compute callback. fn(scene, params) with params =
// "W/E/S/N/xinc/yinc/elev_m/date_dec/fieldcode" (fieldcode one of T|H|X|Y|Z|D|I) opens a new
// viewer window with the computed field grid. nullptr to detach.
GMTVTK_API void gmtvtk_set_igrf_grid_callback(JuliaIgrfGridFn fn) {
	g_juliaIgrfGrid = fn;
}

// Register the IGRF Calculator's Input Mag File "Compute" callback. fn(scene, params) with
// params = "infile;outfile;nHeaders;elev_m;date_dec" reads infile's lon/lat columns, computes
// Total Field for each row and writes the result to outfile. nullptr to detach.
GMTVTK_API void gmtvtk_set_igrf_file_callback(JuliaIgrfFileFn fn) {
	g_juliaIgrfFile = fn;
}

// Register the Reduction to the Pole / Total field to Components Compute callback (Geophysics >
// Magnetics). fn(scene, params) with params = "fieldFile;fieldDip;fieldDec;magDip;magDec;
// component;newRows;newCols;mirror" runs rtp3d() and adds the result grid to `scene`. nullptr to detach.
GMTVTK_API void gmtvtk_set_rtp3d_callback(JuliaRtp3DFn fn) {
	g_juliaRtp3D = fn;
}

// Register the gravmag3d Compute callback (Geophysics > Magnetics). fn(scene, params) with
// params a newline-separated "key=value" block (see JuliaGravMag3DFn in 30_app.cpp) runs GMT.jl's
// gravmag3d() and adds the anomaly grid to `scene`. nullptr to detach.
GMTVTK_API void gmtvtk_set_gravmag3d_callback(JuliaGravMag3DFn fn) {
	g_juliaGravMag3D = fn;
}

// Register the grdgravmag3d Compute callback (Geophysics > Magnetics). fn(scene, params) with params
// a newline-separated "key=value" block (see JuliaGrdGravMag3DFn in 30_app.cpp) runs GMT.jl's
// grdgravmag3d() and adds the anomaly grid to `scene`. nullptr to detach.
GMTVTK_API void gmtvtk_set_grdgravmag3d_callback(JuliaGrdGravMag3DFn fn) {
	g_juliaGrdGravMag3D = fn;
}

// Register the grdredpol Compute callback (Geophysics > Magnetics). fn(scene, params) with params a
// newline-separated "key=value" block (see JuliaGrdRedPolFn in 30_app.cpp) runs the grdredpol
// supplement and adds the continuous-RTP grid to `scene`. nullptr to detach.
GMTVTK_API void gmtvtk_set_grdredpol_callback(JuliaGrdRedPolFn fn) {
	g_juliaGrdRedPol = fn;
}

// Register the "Open full manual page" callback (the green ? disk on every module dialog).
// fn(name) opens that GMT module's page of the GMTjl_doc manual. nullptr to detach.
GMTVTK_API void gmtvtk_set_manual_callback(JuliaOpenManualFn fn) {
	g_juliaOpenManual = fn;
}

// Register the grdgradient OK callback (GMT menu). fn(scene, params) with params a newline-separated
// "key=value" block (see JuliaGrdGradientFn in 30_app.cpp) runs GMT.jl's grdgradient() on the
// window's grid and adds the result. nullptr to detach.
GMTVTK_API void gmtvtk_set_grdgradient_callback(JuliaGrdGradientFn fn) {
	g_juliaGrdGradient = fn;
}

// Register the Illumination / Hillshade OK callback (View menu, port of Mirone shading_params.m).
// fn(scene, params) with params a newline-separated "key=value" block (see JuliaHillshadeFn in
// 30_app.cpp) computes the reflectance and pushes it back with gmtvtk_set_shade_intensity_h.
// nullptr to detach.
GMTVTK_API void gmtvtk_set_hillshade_callback(JuliaHillshadeFn fn) {
	g_juliaHillshade = fn;
}

// Register the JIT warm-up callback (see JuliaWarmupFn / warmupTool in 30_app.cpp). fn(tool) is
// called when a tool's dialog OPENS, so Julia can start compiling that tool's code while the user
// is still filling the dialog in. It must return at once (spawn, don't compute). nullptr to detach.
GMTVTK_API void gmtvtk_set_warmup_callback(JuliaWarmupFn fn) {
	g_juliaWarmup = fn;
}

// Load (or clear) the EXTERNAL illumination: a per-node reflectance grid computed by GMT
// grdgradient in Julia (src/hillshade.jl, the Hillshade tool). `inten` is column-major
// inten[ix*ny + iy] — the same layout the surface z uses — over the true-coord box [x0,x1]x[y0,y1];
// `model` is the Mirone illum_model that produced it, kept only so the dialog can report what is
// loaded. nx < 2 || ny < 2 || inten == nullptr CLEARS it and hands the look back to the Shading dock.
//
// Loading one turns the hillshade master ON (the surface must render UNLIT for baked colours to show)
// and selects the grdimage style, which is the honest label: the modulator this reflectance ends in
// IS gmt_illuminate, exactly as in Mirone's mex_illuminate. Nothing else about the shade engine
// changes — applyReliefShade simply takes the intensity from the grid instead of from the normal.
GMTVTK_API void gmtvtk_set_shade_intensity_h(void *handle, const float *inten, int nx, int ny,
                                             double x0, double x1, double y0, double y1, int model) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	if (!inten || nx < 2 || ny < 2) {
		s->shadeInten.clear();
		s->shadeInX = s->shadeInY = 0;
		s->shadeInModel = 0;
		s->useHillshade = false;
		// model < 0 is the tool's "Remove illumination" (Mirone's ImageResetOrigImg_CB): EVERY light
		// goes off, not just this tool's reflectance, so the grid falls back to plain CPT colour. Just
		// dropping the reflectance would hand it to the Shading dock's own look — still an
		// illumination, and to the eye "the button did nothing". model == 0 is the plain internal
		// clear (a model that replaced the shade with its own picture) and leaves the dock alone.
		if (model < 0) {
			s->useShadows = false;
			s->litBake    = false;      // flat-image mode -> plain CPT, no PBR bake
			s->noShade    = true;       // 3-D surface   -> unlit, plain CPT (applySurfStyle)
		}
		applyShading(s);
		return;
	}
	s->shadeInten.assign(inten, inten + (size_t)nx * ny);
	s->shadeInX = nx;   s->shadeInY = ny;
	s->shadeInX0 = x0;  s->shadeInX1 = x1;
	s->shadeInY0 = y0;  s->shadeInY1 = y1;
	s->shadeInModel = model;
	s->noShade      = false;                          // a model IS a light: ends "Remove illumination"
	s->useHillshade = true;
	s->hillGrd      = true;
	s->useShadows   = false;                          // cast-shadows is the alternative look, not an add-on
	applyShading(s);
}

// Register the grdseamount Compute callback (GMT menu). fn(scene, params) with params a
// newline-separated "key=value" block (see JuliaGrdSeamountFn in 30_app.cpp) builds the synthetic
// seamount grid and adds it to `scene`. nullptr to detach.
GMTVTK_API void gmtvtk_set_grdseamount_callback(JuliaGrdSeamountFn fn) {
	g_juliaGrdSeamount = fn;
}

// Register the Import *.gmt/*.nc cruise track callback (Geophysics > Magnetics). fn(scene, path,
// isList) — isList nonzero means `path` is a list-file (one cruise file path per line). nullptr to
// detach.
GMTVTK_API void gmtvtk_set_import_gmt_callback(JuliaImportGmtFn fn) {
	g_juliaImportGmt = fn;
}

// Register the Clip Grid Apply callback (Grid Tools, port of Mirone src_figs/ml_clip.m). fn(scene,
// params) with params = "below;above;belowVal;aboveVal;inBetween;stretch" clips the window's grid
// and adds the result to `scene`. Returns 1/0. nullptr to detach.
GMTVTK_API void gmtvtk_set_clipgrid_callback(JuliaClipGridFn fn) {
	g_juliaClipGrid = fn;
}

// Register the Binarize callback (Image menu, port of Mirone src_figs/thresholdit.m). fn(scene, dlg,
// params) with params = "op;args" (see JuliaBinarizeFn, 30_app.cpp) does every threshold/clean-up op
// on the window's image and pushes the result back into `dlg` with the two functions below. Returns
// 1/0. nullptr to detach.
GMTVTK_API void gmtvtk_set_binarize_callback(JuliaBinarizeFn fn) {
	g_juliaBinarize = fn;
}

// Push the grey-level histogram into the open Binarize dialog `dlg` (a BinarizeDialog*). `counts` is
// `n` (256) bin counts, as GMT.jl's histogray gives them. Called SYNCHRONOUSLY from Julia's op
// "init", so `dlg` is the live dialog.
GMTVTK_API void gmtvtk_binarize_set_histogram(void *dlg, const double *counts, int n) {
	if (!dlg || !counts || n < 1) return;
	reinterpret_cast<BinarizeDialog *>(dlg)->setHistogram(counts, n);
}

// Push the CURRENT mask into the open Binarize dialog `dlg` for display. `mask` is w*h bytes,
// row-major with the TOP row first, 0 = black / 255 = white (a display copy — Julia keeps the
// full-resolution mask, this one may be decimated). `level` moves the single-line handle, `lo`/`hi`
// the window handles; any of them < 0 leaves that handle where the user put it. Called
// SYNCHRONOUSLY from Julia while it services an op, so `dlg` is the live dialog.
GMTVTK_API void gmtvtk_binarize_set_preview(void *dlg, int w, int h, const unsigned char *mask,
                                            double level, double lo, double hi) {
	if (!dlg) return;
	reinterpret_cast<BinarizeDialog *>(dlg)->setPreview(w, h, mask, level, lo, hi);
}

// Register the "Image -> Show Histogram" callback (port of Mirone src_figs/image_histo.m).
// fn(scene, dlg, px, npix, nb) receives the pixels the window is DISPLAYING and pushes one band's
// counts at a time back with gmtvtk_histo_set_counts. Returns 1/0. nullptr to detach.
GMTVTK_API void gmtvtk_set_image_histo_callback(JuliaImageHistoFn fn) {
	g_juliaImageHisto = fn;
}

// Push one band's histogram into the open Image histogram dialog `dlg` (an ImageHistoDialog*).
// `band` is 0/1/2 (R,G,B — 0 alone for a single-band display) and `counts` is `n` (256) bin counts,
// as GMT.jl's histogray gives them. Called SYNCHRONOUSLY from the JuliaImageHistoFn call above, so
// `dlg` is the live dialog.
GMTVTK_API void gmtvtk_histo_set_counts(void *dlg, int band, const double *counts, int n) {
	if (!dlg || !counts || n < 1) return;
	reinterpret_cast<ImageHistoDialog *>(dlg)->setCounts(band, counts, n);
}

// Register the row-removed callback. fn(scene, kind, name) drops the live grid/image Julia keeps
// behind a Scene Objects row that was just deleted (see JuliaForgetFn, 30_app.cpp). nullptr to detach.
GMTVTK_API void gmtvtk_set_forget_callback(JuliaForgetFn fn) {
	g_juliaForget = fn;
}

// Register the "Image Enhance -> 1 - Indexed and RGB" callback (port of Mirone src_figs/image_enhance.m).
// fn(scene, dlg, params) with params = "op;args" (see JuliaImageEnhanceFn, 30_app.cpp). Returns 1/0.
// nullptr to detach.
GMTVTK_API void gmtvtk_set_image_enhance_callback(JuliaImageEnhanceFn fn) {
	g_juliaImageEnhance = fn;
}

// Push one band into the open Adjust Contrast dialog `dlg` (an ImageEnhanceDialog*): its `n` (256)
// histogram bins, as GMT.jl's histogray gives them, plus the band's own data range [lo,hi] (Mirone's
// minCData/maxCData) and how many bands the image has (1 = the reshaped single-plot figure).
GMTVTK_API void gmtvtk_enhance_set_band(void *dlg, int band, const double *counts, int n,
                                        double lo, double hi, int nbands) {
	if (!dlg || !counts || n < 1) return;
	reinterpret_cast<ImageEnhanceDialog *>(dlg)->setBand(band, counts, n, lo, hi, nbands);
}

// Register the "Image > Image resize" callback (port of Mirone src_figs/imageresize.m).
// fn(scene, dlg, params) with params = "op;args" (see JuliaImageResizeFn, 30_app.cpp). Returns 1/0.
// nullptr to detach.
GMTVTK_API void gmtvtk_set_image_resize_callback(JuliaImageResizeFn fn) {
	g_juliaImageResize = fn;
}

// Answer to the Resize Image dialog's "init": the SOURCE image's size in pixels. The dialog cannot
// measure it itself — the texture it displays was padded to the window bbox by _drape_to_bbox.
GMTVTK_API void gmtvtk_resize_set_size(void *dlg, int w, int h) {
	if (!dlg || w < 1 || h < 1) return;
	reinterpret_cast<ImageResizeDialog *>(dlg)->setSize(w, h);
}

// Register the "Image > Shape detector" callback (port of Mirone src_figs/floodfill.m).
// fn(scene, params) with params = "op;args" (see JuliaFloodFillFn, 30_app.cpp). Returns 1/0.
// nullptr to detach.
GMTVTK_API void gmtvtk_set_floodfill_callback(JuliaFloodFillFn fn) {
	g_juliaFloodFill = fn;
}

// Register the Adjust Contrast ScaterPlot callback. fn(scene, px, npix, nb, label) plots the three
// bands of the pixels the window is DISPLAYING. Returns 1/0. nullptr to detach.
GMTVTK_API void gmtvtk_set_rgb_scatter_callback(JuliaRgbScatterFn fn) {
	g_juliaRgbScatter = fn;
}

// Push a contrast window into `dlg` — the answer to a "stretchlim" op (Eliminate outliers + Apply).
GMTVTK_API void gmtvtk_enhance_set_window(void *dlg, int band, double lo, double hi) {
	if (!dlg) return;
	reinterpret_cast<ImageEnhanceDialog *>(dlg)->setWindow(band, lo, hi);
}

// Register the Empilhador Compute callback (Tools, port of Mirone src_figs/empilhador.m). fn(scene,
// params) with params = the "list=/out=/fmt=/region=/l2=..." block described in 30_app.cpp stacks the
// listed files into one 3-D file. Returns 1/0. nullptr to detach.
GMTVTK_API void gmtvtk_set_empilhador_callback(JuliaEmpilhadorFn fn) {
	g_juliaEmpilhador = fn;
}

// Register the Grid calculator Compute callback (Grid Tools, port of Mirone src_figs/grid_calculator.m).
// fn(scene, params) with params = the "expr=/base=/file<i>=" block described in 30_app.cpp evaluates
// the expression over the named grids and adds the result to `scene`. Returns 1/0. nullptr to detach.
GMTVTK_API void gmtvtk_set_gridcalc_callback(JuliaGridCalcFn fn) {
	g_juliaGridCalc = fn;
}

// Register the grdtrend Compute callback (GMT menu). fn(scene, params) with the "key=value" block
// described in 30_app.cpp fits the trend and adds trend/residuals/weights to `scene`. Returns 1/0.
// nullptr to detach.
GMTVTK_API void gmtvtk_set_grdtrend_callback(JuliaGrdTrendFn fn) {
	g_juliaGrdTrend = fn;
}

// Register the Euler rotations Compute callback (Plates menu). fn(scene, params) with the "key=value"
// block described in 30_app.cpp rotates the chosen line / adds two poles / interpolates a rotation
// model, all through GMT's spotter modules. Returns 1/0. nullptr to detach.
GMTVTK_API void gmtvtk_set_euler_callback(JuliaEulerFn fn) {
	g_juliaEuler = fn;
}

// Julia hands the Euler dialog back whatever the tab it just served has to SAY (the summed pole as
// "lon lat angle", the interpolated pole table, or the GMT command line). Called from inside the
// callback; the dialog reads it once the call returns.
GMTVTK_API void gmtvtk_euler_result(const char *txt) {
	g_eulerResult = (txt && txt[0]) ? txt : "";
}

// Compute Euler pole: the brute-force search runs on a Julia task, so its progress cannot come back
// as the return value of a callback. It is pushed here instead — `cur` of the `nmax` longitudes
// done (a negative `cur` means the run has ended) and `txt`, the tab-separated pole lon, lat,
// angle, starting residue and best-fit residue (plus the confidence volume, for Hellinger). The
// dialog writes them straight into its boxes, the way the MATLAB dialog updated them live.
GMTVTK_API void gmtvtk_compute_euler_progress(int cur, int nmax, const char *txt) {
	computeEulerProgress(cur, nmax, QString::fromUtf8(txt ? txt : ""));
}

// Register the grdlandmask Compute callback (GMT menu). fn(scene, params) with the "key=value" block
// described in 30_app.cpp builds the wet/dry mask (or masks the window's grid) and adds it to
// `scene`. Returns 1/0. nullptr to detach.
GMTVTK_API void gmtvtk_set_grdlandmask_callback(JuliaGrdLandmaskFn fn) {
	g_juliaGrdLandmask = fn;
}

// Register the grdfilter Compute callback (GMT menu). fn(scene, params) with the "key=value" block
// described in 30_app.cpp filters the window's grid and adds the result to `scene`. Returns 1/0.
// nullptr to detach.
GMTVTK_API void gmtvtk_set_grdfilter_callback(JuliaGrdFilterFn fn) {
	g_juliaGrdFilter = fn;
}

// Register the interpolation (griding) Compute callback (GMT menu). fn(scene, params) with the
// "key=value" block described in 30_app.cpp grids the chosen data file with the chosen module and
// adds the result to `scene`. Returns 1/0. nullptr to detach.
GMTVTK_API void gmtvtk_set_interpolate_callback(JuliaInterpolateFn fn) {
	g_juliaInterpolate = fn;
}

// Tell the viewer where the .ui files are (the host's own deps/ui — they ship WITH the Julia
// package, while this DLL may be loaded from the depot runtime cache instead). Called once at load
// time from src/libgmtvtk.jl. An empty/absent path is ignored, so the module-dir rule still applies.
GMTVTK_API void gmtvtk_set_ui_dir(const char *path) {
	g_uiDirOverride = (path && path[0]) ? QString::fromUtf8(path) : QString();
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

// Rebuild the dipping plane + gray surface-projection patch + slip arrows for EVERY fault in the window
// from each fault's stored geometry. Load Session adds the faults BEFORE it applies the saved display
// state (VE / flat-2D / camera); calling this AFTER that state is applied re-seats the planes in the
// final scaled space — the same effect the user otherwise got by opening and closing the elastic dialog.
// Faults with unknown geometry (NaN width/dip/strike — a bare trace) are skipped. No-op on a dead handle.
GMTVTK_API void gmtvtk_refresh_fault_planes(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	const bool geog = s->hasCRS();
	bool any = false;
	for (int i = 0; i < (int)s->polys.size(); ++i) {
		Polygon &pg = s->polys[i];
		if (!pg.isFault || std::isnan(pg.faultWidth) || std::isnan(pg.faultDip) || std::isnan(pg.faultStrike)) continue;
		faultUpdatePlane(s, pg.faultWidth, pg.faultDip, pg.faultStrike,
		                 std::isnan(pg.faultRake) ? 0.0 : pg.faultRake, geog, i);
		any = true;
	}
	if (any && s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
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
	tl.actor = vtkSmartPointer<vtkBillboardTextActor3D>::New();     // ALWAYS billboard, see TextLabel
	textApplyProps(s, tl);
	(s->axesRen ? s->axesRen : s->ren)->AddActor(tl.actor);
	s->texts.push_back(tl);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// Serialize the window's USER-placed text labels (Save Session) as "x;y;r;g;b;size;text\n" lines, one
// per label. Batch/group labels (groupName non-empty, e.g. focal-mechanism date labels) are SKIPPED —
// they are rebuilt by their owning recipe, not the session's text blob. The text string comes last so
// it may contain ';' (the reader splits on the first 6 separators); newlines are flattened to spaces.
// Two-pass like the other serializers (buf=nullptr sizes, second call fills). Session rebuilds each
// line via gmtvtk_add_text_h.
GMTVTK_API int gmtvtk_serialize_texts(void *handle, char *buf, int cap) {
	Scene *s = static_cast<Scene*>(handle);
	std::string o;
	char t[128];
	if (sceneAlive(s)) {
		for (auto &tl : s->texts) {
			if (!tl.groupName.empty()) continue;
			snprintf(t, sizeof(t), "%.12g;%.12g;%.6g;%.6g;%.6g;%d;",
			         tl.pos[0], tl.pos[1], tl.color[0], tl.color[1], tl.color[2], tl.size);
			o += t;
			std::string txt = tl.text;
			for (char &c : txt) if (c == '\n' || c == '\r') c = ' ';
			o += txt; o += '\n';
		}
	}
	const int n = (int)o.size();
	if (buf && cap > 0) { int c = (n < cap - 1) ? n : cap - 1; memcpy(buf, o.data(), c); buf[c] = '\0'; }
	return n;
}

// Copy ONE vector element's vertices out of the window, looked up by the SAME label Scene Objects
// shows. Both element families answer here — a drawn Polygon (Scene::polys) and an imported line
// Overlay (Scene::overlays) — because an operation on "a line" must not care which door the line came
// in through (SACRED_LAW: one operation, one function). Drawn polygons are searched first, then
// overlays; the first exact name match wins. Output: one SEGMENT per line, its vertices written "x,y"
// and joined by '|', in TRUE world coordinates (the stored points are true — only the actor carries
// xfac/VE). Two-pass buffer, like every other serializer here: call with buf=nullptr for the length,
// then again with a buffer of that size + 1. Returns 0 when no element carries that name.
GMTVTK_API int gmtvtk_serialize_vector_h(void *handle, const char *name, char *buf, int cap) {
	Scene *s = static_cast<Scene*>(handle);
	std::string o;
	char t[64];
	if (sceneAlive(s) && name && name[0]) {
		const std::string want = name;
		bool done = false;
		for (auto &pg : s->polys) {
			if (pg.name != want) continue;
			for (size_t i = 0; i < pg.v.size(); ++i) {
				snprintf(t, sizeof(t), "%.12g,%.12g", pg.v[i][0], pg.v[i][1]);
				o += t; if (i + 1 < pg.v.size()) o += '|';
			}
			o += '\n';
			done = true;
			break;
		}
		for (auto &ov : s->overlays) {
			if (done) break;
			if (ov.name != want) continue;
			vtkPolyData *pd = ov.baseLine;
			if (!pd) {
				if (vtkPolyDataMapper *m = vtkPolyDataMapper::SafeDownCast(ov.actor ? ov.actor->GetMapper() : nullptr))
					pd = m->GetInput();
			}
			vtkPoints *pts = pd ? pd->GetPoints() : nullptr;
			if (!pts) break;
			const vtkIdType np = pts->GetNumberOfPoints();
			// segoff holds nseg+1 start offsets; an overlay stored without them is one single segment.
			std::vector<int> off = ov.segoff;
			if ((int)off.size() < 2) off = { 0, (int)np };
			for (size_t k = 0; k + 1 < off.size(); ++k) {
				const int i0 = off[k], i1 = off[k + 1];
				for (int i = i0; i < i1 && i < (int)np; ++i) {
					double p[3]; pts->GetPoint(i, p);
					snprintf(t, sizeof(t), "%.12g,%.12g", p[0], p[1]);
					o += t; if (i + 1 < i1 && i + 1 < (int)np) o += '|';
				}
				o += '\n';
			}
			done = true;
		}
	}
	const int n = (int)o.size();
	if (buf && cap > 0) { int c = (n < cap - 1) ? n : cap - 1; memcpy(buf, o.data(), c); buf[c] = '\0'; }
	return n;
}

// The per-segment INFO text of one named line element — for an imported table that is the segment
// header the file carried ("> 5 EURASIA/NORTH AMERICA FIN\"…\" STG0\"…\"" for a Mirone isochron), which
// is where an isochron keeps its own Euler poles. One segment per output line; empty when the element
// has no info (drawn polygons never do). Two-pass buffer, same as the serializers above.
GMTVTK_API int gmtvtk_vector_info_h(void *handle, const char *name, char *buf, int cap) {
	Scene *s = static_cast<Scene*>(handle);
	std::string o;
	if (sceneAlive(s) && name && name[0]) {
		const std::string want = name;
		for (auto &ov : s->overlays) {
			if (ov.name != want) continue;
			for (auto &t : ov.info) {
				std::string one = t;
				for (char &c : one) if (c == '\n' || c == '\r') c = ' ';
				o += one; o += '\n';
			}
			break;
		}
	}
	const int n = (int)o.size();
	if (buf && cap > 0) { int c = (n < cap - 1) ? n : cap - 1; memcpy(buf, o.data(), c); buf[c] = '\0'; }
	return n;
}

// Serialize the window's USER-drawn polygons/polylines/rectangles/circles (Save Session) as one line
// each:
//   closed;isRect;lr;lg;lb;lw;lstyle;fr;fg;fb;fop;name;x0,y0,z0|x1,y1,z1|...\n
// Line colour/width are read off the actor (they live there, not on Polygon); fill colour/opacity and
// lineStyle come from the struct. SKIPS faults (isFault), nested-grid rects (nestKind) and slip-model
// patches (groupName) — those are rebuilt by their own recipes, not this generic-polygon blob. `name`
// is sanitized of the delimiters (; | newline). Two-pass buffer. Session rebuilds via gmtvtk_add_poly_full.
GMTVTK_API int gmtvtk_serialize_polys(void *handle, char *buf, int cap) {
	Scene *s = static_cast<Scene*>(handle);
	std::string o;
	char t[160];
	if (sceneAlive(s)) {
		for (auto &pg : s->polys) {
			if (pg.isFault || pg.nestKind != 0 || !pg.groupName.empty()) continue;
			double lc[3] = { 0, 0, 0 }; double lw = 2.5;
			if (pg.line) { pg.line->GetProperty()->GetColor(lc); lw = pg.line->GetProperty()->GetLineWidth(); }
			// field 7 (lstyle) is reserved 0 — Polygon has no per-line dashed/dotted style (that lives on
			// Overlay). Kept in the format so the reader's fixed 12-field layout is stable.
			snprintf(t, sizeof(t), "%d;%d;%.6g;%.6g;%.6g;%.6g;%d;%.6g;%.6g;%.6g;%.6g;",
			         pg.closed ? 1 : 0, pg.isRect ? 1 : 0, lc[0], lc[1], lc[2], lw, 0,
			         pg.fillColor[0], pg.fillColor[1], pg.fillColor[2], pg.fillOpacity);
			o += t;
			std::string nm = pg.name;
			for (char &c : nm) if (c == ';' || c == '\n' || c == '\r' || c == '|') c = '_';
			o += nm; o += ';';
			for (size_t i = 0; i < pg.v.size(); ++i) {
				snprintf(t, sizeof(t), "%.10g,%.10g,%.10g", pg.v[i][0], pg.v[i][1], pg.v[i][2]);
				o += t; if (i + 1 < pg.v.size()) o += '|';
			}
			o += '\n';
		}
	}
	const int n = (int)o.size();
	if (buf && cap > 0) { int c = (n < cap - 1) ? n : cap - 1; memcpy(buf, o.data(), c); buf[c] = '\0'; }
	return n;
}

// Rebuild one polygon from a saved session (the inverse of gmtvtk_serialize_polys). `xyz` = npts
// (x,y,z) triples (the stored ring, closing duplicate included for closed shapes). Builds the line +
// fill exactly as the draw tool would (polyRebuildLine reads fillColor/fillOpacity from the struct),
// then stamps the saved outline colour/width onto the actor. lineStyle is stored but not re-applied
// yet (rebuilt polys are solid). Returns the new polygon's index, or -1.
GMTVTK_API int gmtvtk_add_poly_full(void *handle, const double *xyz, int npts, int closed, int isRect,
                                    double lr, double lg, double lb, double lw, int lstyle,
                                    double fr, double fg, double fb, double fop, const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xyz || npts <= 0) return -1;
	Polygon pg;
	pg.closed = closed != 0; pg.isRect = isRect != 0;
	pg.name = (name && name[0]) ? name : "polygon";
	(void)lstyle;                                   // reserved (Polygon has no dashed/dotted style)
	pg.fillColor[0] = fr; pg.fillColor[1] = fg; pg.fillColor[2] = fb; pg.fillOpacity = fop;
	for (int i = 0; i < npts; ++i) pg.v.push_back({ xyz[3*i], xyz[3*i+1], xyz[3*i+2] });
	polyRebuildLine(s, pg);
	if (pg.line) { pg.line->GetProperty()->SetColor(lr, lg, lb); pg.line->GetProperty()->SetLineWidth(lw); }
	pg.stack = s->vecSeq++;
	s->polys.push_back(pg);
	applyVectorStacking(s);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return (int)s->polys.size() - 1;
}

// Serialize the window's fault traces AND slip-model patches (Save Session) — the two Polygon kinds
// the generic gmtvtk_serialize_polys skips. Two record shapes, tagged by the first field:
//   F;slip;rake;strike;dip;width;depthTop;geog;name;x0,y0|x1,y1|...        (a Draw/Import fault trace)
//   S;group;slip;rake;strike;dip;depthTop;length;width;seg;fr,fg,fb;x,y|…  (one slip-model patch)
// Unknown numeric geometry is written by %g as "nan" (the reader turns it back into NaN). Slip-patch
// vertices drop the closing duplicate (the rebuild re-closes each ring). `geog` (whole-scene proxy =
// hasCRS) selects the fault plane's geodesic vs cartesian down-dip walk on rebuild. Session rebuilds
// faults via gmtvtk_add_fault_geom_h/_h and slip groups via gmtvtk_add_slip_patches_h. Two-pass buffer.
GMTVTK_API int gmtvtk_serialize_faults(void *handle, char *buf, int cap) {
	Scene *s = static_cast<Scene*>(handle);
	std::string o;
	char t[256];
	if (sceneAlive(s)) {
		const int geog = s->hasCRS() ? 1 : 0;
		for (auto &pg : s->polys) {
			if (pg.isFault) {
				std::string nm = pg.name;
				for (char &c : nm) if (c == ';' || c == '\n' || c == '\r' || c == '|') c = '_';
				snprintf(t, sizeof(t), "F;%.10g;%.10g;%.10g;%.10g;%.10g;%.10g;%d;",
				         pg.faultSlip, pg.faultRake, pg.faultStrike, pg.faultDip, pg.faultWidth, pg.faultDepthTop, geog);
				o += t; o += nm; o += ';';
				for (size_t i = 0; i < pg.v.size(); ++i) {
					snprintf(t, sizeof(t), "%.10g,%.10g", pg.v[i][0], pg.v[i][1]);
					o += t; if (i + 1 < pg.v.size()) o += '|';
				}
				o += '\n';
			}
			else if (pg.isSlip) {
				std::string grp = pg.groupName;
				for (char &c : grp) if (c == ';' || c == '\n' || c == '\r' || c == '|') c = '_';
				snprintf(t, sizeof(t), "S;%s;%.10g;%.10g;%.10g;%.10g;%.10g;%.10g;%.10g;%d;%.6g,%.6g,%.6g;",
				         grp.c_str(), pg.faultSlip, pg.faultRake, pg.faultStrike, pg.faultDip, pg.faultDepthTop,
				         pg.faultLength, pg.faultWidth, pg.slipSeg, pg.fillColor[0], pg.fillColor[1], pg.fillColor[2]);
				o += t;
				size_t nv = pg.v.size();
				if (nv >= 2 && pg.v.front() == pg.v.back()) --nv;   // drop closing dup (rebuild re-closes)
				for (size_t i = 0; i < nv; ++i) {
					snprintf(t, sizeof(t), "%.10g,%.10g", pg.v[i][0], pg.v[i][1]);
					o += t; if (i + 1 < nv) o += '|';
				}
				o += '\n';
			}
			else if (pg.nestKind == 1) {   // "Nested grids" (tsunami) rectangle — its own quantization params
				std::string nm = pg.name;
				for (char &c : nm) if (c == ';' || c == '\n' || c == '\r' || c == '|') c = '_';
				snprintf(t, sizeof(t), "N;%.12g;%.12g;%d;", pg.nestXi, pg.nestYi, pg.nestReg);
				o += t; o += nm; o += ';';
				for (size_t i = 0; i < pg.v.size(); ++i) {
					snprintf(t, sizeof(t), "%.12g,%.12g", pg.v[i][0], pg.v[i][1]);
					o += t; if (i + 1 < pg.v.size()) o += '|';
				}
				o += '\n';
			}
		}
	}
	const int n = (int)o.size();
	if (buf && cap > 0) { int c = (n < cap - 1) ? n : cap - 1; memcpy(buf, o.data(), c); buf[c] = '\0'; }
	return n;
}

// Rebuild a "Nested grids" (tsunami) rectangle from a saved session (inverse of the N record in
// gmtvtk_serialize_faults). `xy` = npts (x,y) corner pairs; xi/yi = child cell sizes; reg = grid(0)/
// pixel(1) registration. Recreates the nestKind==1 Polygon exactly as the draw tool would, then runs
// nestReflow(snap=false) to recompute the chain indices WITHOUT moving the saved (already-snapped)
// verts — re-snapping them would grow the rect one parent cell per reflow. Returns the polygon index.
GMTVTK_API int gmtvtk_add_nested_rect(void *handle, const double *xy, int npts,
                                      double xi, double yi, int reg, const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xy || npts < 2) return -1;
	Polygon pg;
	pg.closed = true; pg.isRect = true; pg.nestKind = 1;
	pg.nestXi = xi; pg.nestYi = yi; pg.nestReg = reg;
	pg.name = (name && name[0]) ? name : "Nested rectangle";
	for (int i = 0; i < npts; ++i) pg.v.push_back({ xy[2*i], xy[2*i + 1], 0.0 });
	if (pg.v.size() >= 2 && !(pg.v.front() == pg.v.back())) pg.v.push_back(pg.v.front());   // close the ring
	polyRebuildLine(s, pg);
	pg.stack = s->vecSeq++;
	s->polys.push_back(pg);
	applyVectorStacking(s);
	nestReflow(s, false);                          // restore: keep saved verts, only recompute chain indices
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return (int)s->polys.size() - 1;
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
// `vcenter` forces CENTRED vertical justification on a batch-owned label, which textApplyProps
// otherwise bottom-justifies (TextLabel::vcenter): a contour annotation must straddle its own line,
// where a focal-mechanism date must grow upward off its ball.
//
// `z` and `angleDeg` (both may be NULL) and `flat` serve the ONE exception to the billboard rule
// (TextLabel::flat, 10_geometry.cpp): a contour label must read ALONG its contour and sit at the
// contour's own height, neither of which an always-upright camera-facing billboard can do. With
// flat != 0 the labels are vtkTextActor3D lying in the XY plane, turned by `angleDeg[i]` about Z,
// anchored at height `z[i]`, and uniformly world-scaled by sceneWorldPerPixel so their size matches
// the gap the caller cut for them. Returns the number added.
GMTVTK_API int gmtvtk_add_texts_ex_h(void *handle, const double *xy, const char *texts, int n,
                                  double r, double g, double b, int size,
                                  const char *font, int bold, int italic, const char *groupName,
                                  const int *eventIdx, int vcenter,
                                  const double *z, const double *angleDeg, int flat) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xy || !texts || n < 1) return 0;
	const char *p = texts;
	int added = 0;
	// Flat labels are world-scaled, so they need to know how big a screen pixel is in world units.
	// Plain world-per-pixel: the device-DPI and oversampling factors belong with the TEXTURE, and
	// textApplyProps applies them there (see its flat branch).
	const double wscale = flat ? sceneWorldPerPixel(s) : 0.0;
	for (int i = 0; i < n; ++i) {
		const char *e = strchr(p, '\x1e');
		std::string txt = e ? std::string(p, e - p) : std::string(p);
		if (!txt.empty()) {
			TextLabel tl;
			tl.pos = { xy[2*i], xy[2*i + 1], z ? z[i] : 0.0 };
			tl.text = std::move(txt);
			tl.name = "Text " + std::to_string((int)s->texts.size() + 1);
			tl.color[0] = r; tl.color[1] = g; tl.color[2] = b;
			if (size > 0) tl.size = size;
			if (font && font[0]) tl.font = font;
			tl.bold = bold != 0;
			tl.italic = italic != 0;
			if (groupName && groupName[0]) tl.groupName = groupName;
			if (eventIdx) tl.mecaEvent = eventIdx[i];
			tl.vcenter = vcenter != 0;
			tl.flat    = flat != 0;
			tl.angle   = angleDeg ? angleDeg[i] : 0.0;
			tl.wscale  = wscale;
			// A billboard (2026-07-24 standing rule, see TextLabel) — camera-facing, constant screen
			// size, same as the cube's tick numbers (placeTickBillboards, 10_geometry.cpp) — for every
			// label EXCEPT the one documented exception: a contour annotation, which has to be rotated
			// along its line and so must be a flat vtkTextActor3D in the XY plane.
			if (tl.flat) tl.actor = vtkSmartPointer<vtkTextActor3D>::New();
			else         tl.actor = vtkSmartPointer<vtkBillboardTextActor3D>::New();
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

// The original batch-text entry point: gmtvtk_add_texts_ex_h with vcenter = 0, i.e. batch-owned
// labels keep bottom justification (Focal mechanisms' per-event dates grow upward off their ball).
GMTVTK_API int gmtvtk_add_texts_h(void *handle, const double *xy, const char *texts, int n,
                                  double r, double g, double b, int size,
                                  const char *font, int bold, int italic, const char *groupName,
                                  const int *eventIdx) {
	return gmtvtk_add_texts_ex_h(handle, xy, texts, n, r, g, b, size, font, bold, italic,
	                             groupName, eventIdx, 0, nullptr, nullptr, 0);
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

// World-space WIDTH that a screen-constant billboard label of `text` would occupy at the scene's
// focal depth, given the same font fields gmtvtk_add_texts_h takes. Two steps, both taken in the
// machinery that actually draws the label: vtkTextRenderer measures the string in PIXELS with the
// billboards' own text engine (so kerning/bold/italic/font family are the real ones, not a
// chars * average-width guess), then ONE display->world round trip at the camera's focal point
// converts pixels to world units (correct under perspective too, unlike a parallel-scale formula).
// Used by Grid Tools > Contours to leave un-annotated any contour too short to carry its own label
// (src/contours.jl). Returns 0 on any failure, so a caller can fall back rather than mis-filter.
// `outHeight` (may be NULL) receives the label's world-space HEIGHT the same way — a caller that has
// to keep labels from overlapping each other needs both sides of the box, not just its width.
GMTVTK_API double gmtvtk_label_width_world_h(void *handle, const char *text, int size,
                                             const char *font, int bold, int italic,
                                             double *outHeight) {
	Scene *s = static_cast<Scene*>(handle);
	if (outHeight) *outHeight = 0.0;
	if (!sceneAlive(s) || !text || !*text) return 0.0;
	if (!s->widget || !s->widget->renderWindow()) return 0.0;

	int dpi = s->widget->renderWindow()->GetDPI();
	if (dpi <= 0) dpi = 72;
	vtkNew<vtkTextProperty> tp;                               // mirror textApplyProps' font fields
	tp->SetFontFamilyAsString((font && *font) ? font : "Arial");
	tp->SetFontSize(size > 0 ? size : 18);
	tp->SetBold(bold != 0);
	tp->SetItalic(italic != 0);
	vtkTextRenderer *tr = vtkTextRenderer::GetInstance();
	int bb[4] = { 0, 0, 0, 0 };
	if (!tr || !tr->GetBoundingBox(tp, text, bb, dpi)) return 0.0;
	const double wpx = double(bb[1] - bb[0] + 1);
	const double hpx = double(bb[3] - bb[2] + 1);
	if (!(wpx > 0.0)) return 0.0;

	const double wpp = sceneWorldPerPixel(s);      // the SAME scale the flat label actor is built with
	if (!(wpp > 0.0)) return 0.0;
	if (outHeight) *outHeight = hpx * wpp;
	return wpx * wpp;
}

// Same as gmtvtk_add_overlay_ex2_h, plus DISPLAY-ONLY holes for annotations. `gapAnchors` are
// ascending GLOBAL 0-based vertex indices where a label sits, and `gapHalfPx` is how far the hole
// reaches each side of one in SCREEN PIXELS. The points and the segment offsets stay WHOLE, so the
// data table, Line length, Save and double-click-to-edit all still see one unbroken polyline — only
// the drawn line cells have holes (cutting the geometry instead is what made a double-click grab a
// fragment of a contour and spawn a row per piece).
//
// Pixels, not world units, on purpose: the labels are screen-constant, so the holes are re-cut from
// these same anchors whenever the zoom changes (overlayRebuildGapCells / followZoomAnnotations,
// 50_scene.cpp) and stay exactly as wide as the text sitting in them. Returns 1 if added.
GMTVTK_API int gmtvtk_add_overlay_gapped_h(void *handle, const double *xyz, int npts, const int *segoff, int nseg,
                                      int mode, double r, double g, double b,
                                      double linewidth, double pointsize,
                                      const char *name, const char *groupName, const char *info,
                                      int noConvertToPoints, int zIsPlaceholder,
                                      const int *gapAnchors, int nGapAnchors, double gapHalfPx,
                                      int cptColorable) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))
		return 0;
	double dpi = 72.0;
	if (s->widget && s->widget->renderWindow() && s->widget->renderWindow()->GetDPI() > 0)
		dpi = s->widget->renderWindow()->GetDPI();
	const double pxPerPt = dpi / 72.0;
	addOverlay(s, xyz, npts, segoff, nseg, mode, r, g, b, linewidth * pxPerPt, pointsize, name, groupName, info,
	           nullptr, 0, false, false, noConvertToPoints != 0, zIsPlaceholder != 0, false,
	           gapAnchors, nGapAnchors, gapHalfPx, cptColorable != 0);
	return 1;
}

// World units per screen pixel at the camera's focal point — the scale everything screen-constant is
// built from. Exposed so the host can express a size in pixels (Grid Tools > Contours turns its
// measured label width into the pixel half-width the line holes are cut with).
GMTVTK_API double gmtvtk_world_per_pixel_h(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return 0.0;
	return sceneWorldPerPixel(s);
}

// test hook: run the REAL double-click handler (polygonHandleDblClick) at the screen position of
// world point (x,y,z), then report what is under vertex edit. out2 = { polyEdit, ovEdit }, and the
// return value is ovEditSeg. Lets the test suite check the in-place overlay edit without a mouse.
GMTVTK_API int gmtvtk_dblclick_test(void *scene, double x, double y, double z, int *out2) {
	Scene *s = static_cast<Scene*>(scene);
	if (!sceneAlive(s) || !s->widget) return -1;
	double d[2];
	polyToDisplay(s, { x, y, z }, d);
	polygonHandleDblClick(s, (int)std::lround(d[0]), (int)std::lround(d[1]));
	if (out2) { out2[0] = s->polyEdit; out2[1] = s->ovEdit; }
	return s->ovEditSeg;
}

// Remove every line/point OVERLAY tagged with `groupName`, plus the text labels that carry the same
// group tag (gmtvtk_add_texts_h's groupName) -- the overlay twin of gmtvtk_remove_meca_group_h, and
// the same "drop the batch's labels too, or a re-plot leaves the old ones behind" rule
// deleteMecaGroup follows. Used by Grid Tools > Contours, whose Apply always redraws the WHOLE
// elevation list and so must first wipe what the previous Apply drew. Returns 1 if the handle was
// alive, 0 otherwise.
GMTVTK_API int gmtvtk_remove_overlay_group_h(void *handle, const char *groupName) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !groupName || !*groupName) return 0;
	// overlayDeleteGroup drops the group's labels too, and does the restack + rebuild + render — so
	// this export and the group row's own "Remove" cannot behave differently.
	overlayDeleteGroup(s, std::string(groupName));
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);   // see gmtvtk_add_meca_h
	return 1;
}

// --- test-only hooks for the fault-trace endpoint logic (exercised by the Julia test suite) -------
// Compiled ONLY into gmtvtk_test.dll (GMTVTK_TEST_API, set by the gmtvtk_test CMake target).
// The production gmtvtk.dll never sees these symbols at all — not hidden, not exported.
#ifdef GMTVTK_TEST_API
// test hook: grab an X,Y plot window (titlebar-to-statusbar) as a PNG, so a failing test/manual
// probe can SEE the live axis/tick state instead of guessing from data alone. Deliberately skips
// xyAlive's g_xyplots lookup: that registry is a file-static in gmtvtk.dll, invisible from this
// test dll's own copy (same reason _register_faultgeom_test mirrors callbacks, see
// test/libgmtvtk_test.jl) -- the handle is still a valid address, so a plain null-check suffices.
GMTVTK_API int gmtvtk_xyplot_screenshot_test(void *handle, const char *path) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!p || !p->win || !path) return 0;
	QPixmap pm = p->win->grab();
	return pm.save(QString::fromUtf8(path), "PNG") ? 1 : 0;
}

// test hook: fire the Object Manager's "Show in Data Table" action for series `idx` (same call
// xyObjMgrMenu makes), then grab the resulting floating, parentless QDialog (found by scanning
// top-level widgets -- it isn't a child of p->win, so p->win->grab() alone would miss it) as a PNG.
GMTVTK_API int gmtvtk_xyplot_show_table_test(void *handle, int idx, const char *path) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!p || !path) return 0;
	xyShowDataTable(p, idx);
	QDialog *dlg = nullptr;
	for (QWidget *w : QApplication::topLevelWidgets())
		if (auto *d = qobject_cast<QDialog*>(w); d && d->isVisible()) dlg = d;
	if (!dlg) return 0;
	QPixmap pm = dlg->grab();
	return pm.save(QString::fromUtf8(path), "PNG") ? 1 : 0;
}

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
	int n = 0; for (auto &p : s->polys) if (p.isFault) ++n;
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
	for (auto &p : s->polys) if (p.isFault) return (int)p.v.size();
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
	for (auto &p : s->polys) if (p.isFault && p.linePD) {
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
	SymbolLayer &sl = s->symbols[idx];
	auto *pd = symInputPD(sl);
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
	auto *pd = symInputPD(sl);
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
	auto send = [&](QEvent::Type t, const QPointF &p, Qt::MouseButton btn, Qt::MouseButtons btns) {
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
	auto send = [&](QEvent::Type t, const QPointF &p, Qt::MouseButton btn, Qt::MouseButtons btns) {
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

// test hooks: open/close the REAL IGRF Calculator (drives the actual QUiLoader load path, not a
// stand-in) and report back what it actually loaded — proof the .ui is respected, not a claim.
// out[0]=width out[1]=height out[2]=mapArea found out[3]=latDec found out[4]=fieldCombo item count
// out[5]=magFile1Edit found out[6]=xMin(geometry box) found. Returns 1 if the dialog loaded at all.
static IgrfDialog *g_igrfTestDlg = nullptr;
GMTVTK_API int gmtvtk_igrf_open_dialog_test(double *out) {
	ensureApp();   // QWidget ctor crashes hard with no QApplication instance yet
	if (g_igrfTestDlg && g_igrfTestDlg->dlg) g_igrfTestDlg->dlg->close();
	g_igrfTestDlg = new IgrfDialog(nullptr, nullptr);
	if (!g_igrfTestDlg->dlg) return 0;
	g_igrfTestDlg->dlg->show();
	QApplication::processEvents();
	if (out) {
		out[0] = g_igrfTestDlg->dlg->width();
		out[1] = g_igrfTestDlg->dlg->height();
		out[2] = g_igrfTestDlg->mapArea ? 1 : 0;
		out[3] = g_igrfTestDlg->latDec ? 1 : 0;
		out[4] = g_igrfTestDlg->fieldCombo ? g_igrfTestDlg->fieldCombo->count() : -1;
		out[5] = g_igrfTestDlg->magFile1Edit ? 1 : 0;
		out[6] = g_igrfTestDlg->xMin ? 1 : 0;
		out[7] = g_igrfTestDlg->dlg->maximumWidth();
		out[8] = g_igrfTestDlg->dlg->maximumHeight();
		out[9] = g_igrfTestDlg->dlg->minimumWidth();
		out[10] = g_igrfTestDlg->dlg->minimumHeight();
		out[11] = (g_igrfTestDlg->dlg->windowFlags() & Qt::MSWindowsFixedSizeDialogHint) ? 1 : 0;
		out[12] = g_igrfTestDlg->dlg->layout() ? g_igrfTestDlg->dlg->layout()->sizeConstraint() : -1;
		out[13] = g_igrfTestDlg->mapArea ? g_igrfTestDlg->mapArea->maximumHeight() : -1;
	}
	return 1;
}
GMTVTK_API void gmtvtk_igrf_close_dialog_test() {
	if (g_igrfTestDlg && g_igrfTestDlg->dlg) g_igrfTestDlg->dlg->close();   // WA_DeleteOnClose self-frees the wrapper
	g_igrfTestDlg = nullptr;
}
// Grab an actual PNG of the currently-open test dialog — lets a real visual check happen instead
// of trusting width/height numbers alone. Returns 1 on success.
GMTVTK_API int gmtvtk_igrf_screenshot_test(const char *path) {
	if (!g_igrfTestDlg || !g_igrfTestDlg->dlg) return 0;
	QApplication::processEvents();
	QPixmap pm = g_igrfTestDlg->dlg->grab();
	return pm.save(QString::fromUtf8(path), "PNG") ? 1 : 0;
}

// test hook: grab the ENTIRE main window (titlebar-to-statusbar, incl. the Scene Objects dock) --
// unlike gmtvtk_save_png (VTK render surface only), this is the only way to visually diagnose a
// Scene Objects panel paint bug (stale/overlapping tree rows etc). No processEvents pump here on
// purpose -- the caller controls timing so a race can be captured, not papered over.

// test hooks: open/close/screenshot the REAL Geomagnetic Bar Code dialog (same drive-the-actual-
// widget-then-look pattern as the IGRF hooks above — never trust "didn't throw" for a paint bug).
GMTVTK_API void *gmtvtk_open_empty(const char *title);   // defined below; forward-declared for the test hook

// test hooks: open/close the REAL Euler rotations dialog on a REAL window (`handle`, or a throwaway
// empty one when null). Same purpose as the IGRF/bar-code pair: a QUiLoader failure or a widget name
// that drifted out of the .ui shows up here instead of on the user's first click.
static EulerDialog *g_eulerTestDlg = nullptr;
static void *g_eulerTestWin = nullptr;
GMTVTK_API int gmtvtk_euler_open_dialog_test(void *handle) {
	ensureApp();
	// The caller's window was built by gmtvtk.dll, whose `g_scenes` registry is NOT this dll's, so
	// sceneAlive() would reject a perfectly live handle here — the pointer is taken as given (same
	// convention as gmtvtk_fault_open_dialog_test). Only a null handle gets a throwaway window.
	Scene *s = static_cast<Scene *>(handle);
	if (!s) {
		if (!g_eulerTestWin) g_eulerTestWin = gmtvtk_open_empty("euler test");
		s = static_cast<Scene *>(g_eulerTestWin);
	}
	// …and the dialog itself asks sceneAlive() before touching the scene (it must, in production), so
	// this dll's own registry has to know about the borrowed window too.
	g_scenes.insert(s);
	if (!s || !s->win) return 0;
	// Exactly what the menu entry does: a dialog already open (or PARKED) for this window comes back,
	// it is never doubled.
	auto it = g_eulerDlgs.find(s);
	if (it != g_eulerDlgs.end() && it->second && it->second->dlg) {
		g_eulerTestDlg = it->second;
		g_eulerTestDlg->unpark();
		QApplication::processEvents();
		return 1;
	}
	g_eulerTestDlg = new EulerDialog(s->win, s);
	if (!g_eulerTestDlg->dlg) return 0;
	g_eulerTestDlg->dlg->show();
	QApplication::processEvents();
	return 1;
}
// The X: PARKS the dialog (hidden, still alive, a row in Scene Objects) — so the pointer is kept, and
// gmtvtk_euler_targets_test still answers, which is how a test checks the state survived the close.
GMTVTK_API void gmtvtk_euler_close_dialog_test() {
	if (g_eulerTestDlg && g_eulerTestDlg->dlg) g_eulerTestDlg->dlg->close();
	QApplication::processEvents();
}
// Really destroy it (the parked row's "Delete"), for a test that wants a clean window afterwards.
GMTVTK_API void gmtvtk_euler_delete_dialog_test() {
	if (g_eulerTestDlg && g_eulerTestDlg->dlg) {
		g_eulerTestDlg->reallyClose = true;
		g_eulerTestDlg->dlg->close();
	}
	g_eulerTestDlg = nullptr;
	QApplication::processEvents();
}
// ---- Compute Euler pole (ComputeEulerDialog) --------------------------------------------------
// Same three-part shape as the Euler rotations hooks above: open the REAL dialog on a REAL window,
// drive its widgets by name, read the boxes back. A .ui rename or a QUiLoader failure surfaces here.
static ComputeEulerDialog *g_ceulerTestDlg = nullptr;
static void *g_ceulerTestWin = nullptr;

GMTVTK_API int gmtvtk_ceuler_open_dialog_test(void *handle) {
	ensureApp();
	Scene *s = static_cast<Scene *>(handle);
	if (!s) {
		if (!g_ceulerTestWin) g_ceulerTestWin = gmtvtk_open_empty("compute euler test");
		s = static_cast<Scene *>(g_ceulerTestWin);
	}
	g_scenes.insert(s);
	if (!s || !s->win) return 0;
	auto it = g_ceulerDlgs.find(s);
	if (it != g_ceulerDlgs.end() && it->second && it->second->dlg) {
		g_ceulerTestDlg = it->second;
		g_ceulerTestDlg->unpark();
		QApplication::processEvents();
		return 1;
	}
	g_ceulerTestDlg = new ComputeEulerDialog(s->win, s);
	if (!g_ceulerTestDlg->dlg) return 0;
	g_ceulerTestDlg->dlg->show();
	QApplication::processEvents();
	return 1;
}

// Set one control by name ("line1", "line2", "polelon", "polelat", "poleang", "lonrange",
// "latrange", "angrange", "nlon", "nlat", "nang", "hellinger", "plotres", "residfile").
GMTVTK_API int gmtvtk_ceuler_set_test(const char *what, const char *value) {
	if (!g_ceulerTestDlg || !g_ceulerTestDlg->dlg || !what) return 0;
	ComputeEulerDialog *D = g_ceulerTestDlg;
	const QString k = QString::fromUtf8(what);
	const QString v = QString::fromUtf8(value ? value : "");
	auto put = [&v](QLineEdit *e) { if (!e) return 0; e->setText(v); return 1; };
	int r = 0;
	if      (k == "line1")    { if (D->line1) { D->line1->setCurrentText(v); r = 1; } }
	else if (k == "line2")    { if (D->line2) { D->line2->setCurrentText(v); r = 1; } }
	else if (k == "polelon")  r = put(D->pLon);
	else if (k == "polelat")  r = put(D->pLat);
	else if (k == "poleang")  r = put(D->pAng);
	else if (k == "lonrange") r = put(D->lonRange);
	else if (k == "latrange") r = put(D->latRange);
	else if (k == "angrange") r = put(D->angRange);
	// The N-points boxes go through the same "N*Delta" rule typing them does (editingFinished).
	else if (k == "nlon")     { r = put(D->nLon); D->syncNInt(D->nLon, D->lonRange); }
	else if (k == "nlat")     { r = put(D->nLat); D->syncNInt(D->nLat, D->latRange); }
	else if (k == "nang")     { r = put(D->nAng); D->syncNInt(D->nAng, D->angRange); }
	else if (k == "residfile") r = put(D->errFile);
	else if (k == "hellinger") { if (D->hellChk) { D->hellChk->setChecked(v == "1"); r = 1; } }
	else if (k == "plotres")   { if (D->plotResChk) { D->plotResChk->setChecked(v == "1"); r = 1; } }
	// Press / release "Pick lines from Figure" — the pick answers then arrive through the shared
	// gmtvtk_euler_pick_deliver_test door (one pick mechanism, one test door).
	else if (k == "pick")      { if (D->pickBtn) { D->setPickDown(v == "1"); r = 1; } }
	QApplication::processEvents();
	return r;
}

// Press Compute. The brute-force search runs on a Julia task, so this returns as soon as it started;
// the caller then polls gmtvtk_ceuler_read_test("running") / the result boxes.
GMTVTK_API int gmtvtk_ceuler_compute_test() {
	if (!g_ceulerTestDlg || !g_ceulerTestDlg->dlg) return 0;
	g_ceulerTestDlg->run();
	QApplication::processEvents();
	return 1;
}

GMTVTK_API int gmtvtk_ceuler_stop_test() {
	if (!g_ceulerTestDlg || !g_ceulerTestDlg->dlg) return 0;
	g_ceulerTestDlg->stop();
	QApplication::processEvents();
	return 1;
}

// Read one box by name ("polelon", "polelat", "poleang", "stresidue", "bfresidue", "lines",
// "nlat", "nintlabel", "running"). Returns the number of bytes the answer needs.
GMTVTK_API int gmtvtk_ceuler_read_test(const char *what, char *buf, int cap) {
	if (!g_ceulerTestDlg || !g_ceulerTestDlg->dlg || !what) return -1;
	ComputeEulerDialog *D = g_ceulerTestDlg;
	const QString k = QString::fromUtf8(what);
	QString v;
	if      (k == "polelon")   v = D->fLon ? D->fLon->text() : QString();
	else if (k == "polelat")   v = D->fLat ? D->fLat->text() : QString();
	else if (k == "poleang")   v = D->fAng ? D->fAng->text() : QString();
	else if (k == "stresidue") v = D->stRes ? D->stRes->text() : QString();
	else if (k == "bfresidue") v = D->bfRes ? D->bfRes->text() : QString();
	else if (k == "nlat")      v = D->nLat ? D->nLat->text() : QString();
	else if (k == "nlon")      v = D->nLon ? D->nLon->text() : QString();
	else if (k == "lonrange")  v = D->lonRange ? D->lonRange->text() : QString();
	else if (k == "nintlabel") v = D->nIntLbl ? D->nIntLbl->text() : QString();
	else if (k == "running")   v = (D->stopBtn && D->stopBtn->isEnabled()) ? "1" : "0";
	else if (k == "visible")   v = D->dlg->isVisible() ? "1" : "0";     // the FIRST open must be visible
	else if (k == "line1")     v = D->line1 ? D->line1->currentText() : QString();
	else if (k == "line2")     v = D->line2 ? D->line2->currentText() : QString();
	else if (k == "pickdown")  v = (D->pickBtn && D->pickBtn->isChecked()) ? "1" : "0";
	else if (k == "lines") {                       // what the two combos offer, '\n' joined
		QStringList all;
		if (D->line1) for (int i = 0; i < D->line1->count(); ++i) all << D->line1->itemText(i);
		v = all.join('\n');
	}
	const QByteArray cur = v.toUtf8();
	if (buf && cap > 0) {
		const int c = (cur.size() < cap - 1) ? cur.size() : cap - 1;
		memcpy(buf, cur.constData(), c);
		buf[c] = '\0';
	}
	return cur.size();
}

// Find an action anywhere in a menu tree whose text CONTAINS `needle` ('&' accelerators ignored).
// `exact` first over the WHOLE tree, then a contains pass: "Plates" must not land on "GPlates".
static QAction *menuFindDeep(QWidget *root, const QString &needle, bool exact) {
	if (!root) return nullptr;
	for (QAction *a : root->actions()) {
		QString t = a->text();
		t.remove('&');
		t.remove(QChar(0x2026));                       // trailing "…" is decoration, not identity
		t = t.trimmed();
		if (exact ? (t.compare(needle, Qt::CaseInsensitive) == 0) : t.contains(needle, Qt::CaseInsensitive))
			return a;
		if (QMenu *m = a->menu())
			if (QAction *r = menuFindDeep(m, needle, exact)) return r;
	}
	return nullptr;
}

// Pick a REAL menu entry exactly as the user does, '/'-separated for the dynamic discipline menus
// ("Plates/Compute Euler pole" first rebuilds the Geophysics menu into the Plates block, then fires
// the entry). Returns how many steps actually fired — the only way a test can exercise the menu
// path itself, which is NOT the same code the *_open_dialog_test hooks run.
GMTVTK_API int gmtvtk_menu_trigger_test(void *handle, const char *path) {
	Scene *s = static_cast<Scene *>(handle);
	if (!s || !s->win || !path) return 0;
	int n = 0;
	for (const QString &step : QString::fromUtf8(path).split('/', Qt::SkipEmptyParts)) {
		QAction *a = menuFindDeep(s->win->menuBar(), step.trimmed(), true);
		if (!a) a = menuFindDeep(s->win->menuBar(), step.trimmed(), false);
		if (!a) return n;
		a->trigger();
		QApplication::processEvents();
		++n;
	}
	return n;
}

// The whole menu tree as text, one action per line, indented by depth ("" = no window). Diagnostic
// companion of gmtvtk_menu_trigger_test: it says what the menus ACTUALLY hold at that moment, which
// the dynamic discipline menus (they clear and repopulate themselves) make impossible to assume.
static void menuDumpDeep(QWidget *root, int depth, QString &out) {
	if (!root || depth > 6) return;
	for (QAction *a : root->actions()) {
		QString t = a->text();
		t.remove('&');
		out += QString(depth * 2, ' ') + (t.isEmpty() ? QString("---") : t) + '\n';
		if (QMenu *m = a->menu()) menuDumpDeep(m, depth + 1, out);
	}
}

GMTVTK_API int gmtvtk_menu_dump_test(void *handle, char *buf, int cap) {
	Scene *s = static_cast<Scene *>(handle);
	QString out;
	if (s && s->win) menuDumpDeep(s->win->menuBar(), 0, out);
	const QByteArray cur = out.toUtf8();
	if (buf && cap > 0) {
		const int c = (cur.size() < cap - 1) ? cur.size() : cap - 1;
		memcpy(buf, cur.constData(), c);
		buf[c] = '\0';
	}
	return cur.size();
}

// Adopt whatever Compute Euler pole dialog this window already has (one opened through the MENU) as
// the dialog gmtvtk_ceuler_read_test/_set_test talk to. 0 when there is none.
GMTVTK_API int gmtvtk_ceuler_adopt_test(void *handle) {
	Scene *s = static_cast<Scene *>(handle);
	auto it = g_ceulerDlgs.find(s);
	if (it == g_ceulerDlgs.end() || !it->second || !it->second->dlg) return 0;
	g_ceulerTestDlg = it->second;
	return 1;
}

GMTVTK_API void gmtvtk_ceuler_delete_dialog_test() {
	if (g_ceulerTestDlg && g_ceulerTestDlg->dlg) {
		g_ceulerTestDlg->reallyClose = true;
		g_ceulerTestDlg->dlg->close();
	}
	g_ceulerTestDlg = nullptr;
	QApplication::processEvents();
}

// 1 when the dialog is currently PARKED (hidden but alive, with its Scene Objects row), 0 when it is
// on screen, -1 when there is none.
GMTVTK_API int gmtvtk_euler_parked_test(void *handle) {
	Scene *s = static_cast<Scene *>(handle);
	if (!g_eulerTestDlg || !g_eulerTestDlg->dlg) return -1;
	const bool hidden = !g_eulerTestDlg->dlg->isVisible();
	bool row = false;
	if (s) for (auto &pt : s->parkedTools) if (pt.win == g_eulerTestDlg->dlg) row = true;
	return (hidden && row) ? 1 : 0;
}
// How many line elements the dialog's target list offers, and which of them are selected (joined by
// '\n' into `buf`) — enough to check the list really sees the window's lines, and that a pick landed,
// without a screenshot.
GMTVTK_API int gmtvtk_euler_targets_test(char *buf, int cap) {
	if (!g_eulerTestDlg || !g_eulerTestDlg->targetList) return -1;
	QStringList sel;
	for (QListWidgetItem *it : g_eulerTestDlg->targetList->selectedItems()) sel << it->text();
	const QByteArray cur = sel.join('\n').toUtf8();
	if (buf && cap > 0) {
		const int c = (cur.size() < cap - 1) ? cur.size() : cap - 1;
		memcpy(buf, cur.constData(), c);
		buf[c] = '\0';
	}
	return g_eulerTestDlg->targetList->count();
}

// (No "read the Euler answer" hook lives here on purpose: Julia writes that answer into gmtvtk.dll's
// own g_eulerResult, and THIS dll has a separate copy of it — a hook here would always read empty.
// The tests check the answer through Julia's own record of it, InteractiveGMT._euler_last_result.)

// Drive the dialog's own pick machinery from a test: arm mode (1 = click, 2 = rectangle) and feed it
// clicks at TRUE world coordinates, exactly as the interactor would. Returns 1 when the pick was
// armed / the click was delivered.
GMTVTK_API int gmtvtk_euler_arm_pick_test(int mode) {
	if (!g_eulerTestDlg || !g_eulerTestDlg->dlg) return 0;
	QPushButton *b = (mode == 2) ? g_eulerTestDlg->rectBtn : g_eulerTestDlg->pickBtn;
	if (!b) return 0;
	b->setChecked(mode != 0);
	QApplication::processEvents();
	return 1;
}
// Deliver ONE pick answer the way the scene would (names '\n'-separated) — covers the dialog half of
// the pick without needing a synthetic mouse event on the render window.
GMTVTK_API int gmtvtk_euler_pick_deliver_test(void *handle, const char *names) {
	Scene *s = static_cast<Scene *>(handle);
	if (!s || !s->vectorPickCB) return 0;
	auto cb = s->vectorPickCB;              // copy: a tool may end its own pick from inside the answer
	cb(names ? names : "");
	QApplication::processEvents();
	return 1;
}

// test hooks: the REAL Plate calculator dialog on a REAL window. Unlike the Euler pair above these
// DO exercise the Julia round trip — the dialog fills its combos and its pole boxes from it — which
// works because the test setup registers the euler callback into THIS dll too and mirrors Julia's
// answer back through this dll's own gmtvtk_euler_result (see libgmtvtk_test.jl).
static PlateCalcDialog *g_plateTestDlg = nullptr;
static void *g_plateTestWin = nullptr;
GMTVTK_API int gmtvtk_platecalc_open_dialog_test(void *handle) {
	ensureApp();
	Scene *s = static_cast<Scene *>(handle);         // taken as given, like the Euler hook above
	if (!s) {
		if (!g_plateTestWin) g_plateTestWin = gmtvtk_open_empty("plate calc test");
		s = static_cast<Scene *>(g_plateTestWin);
	}
	g_scenes.insert(s);
	if (!s || !s->win) return 0;
	auto it = g_plateDlgs.find(s);
	if (it != g_plateDlgs.end() && it->second && it->second->dlg) {
		g_plateTestDlg = it->second;
		g_plateTestDlg->unpark();
		QApplication::processEvents();
		return 1;
	}
	g_plateTestDlg = new PlateCalcDialog(s->win, s);
	if (!g_plateTestDlg->dlg) return 0;
	g_plateTestDlg->dlg->show();
	QApplication::processEvents();
	return 1;
}
// The X: PARKS it (kept alive), same contract as the Euler dialog's.
GMTVTK_API void gmtvtk_platecalc_close_dialog_test() {
	if (g_plateTestDlg && g_plateTestDlg->dlg) g_plateTestDlg->dlg->close();
	QApplication::processEvents();
}
GMTVTK_API void gmtvtk_platecalc_delete_dialog_test() {
	if (g_plateTestDlg && g_plateTestDlg->dlg) {
		g_plateTestDlg->reallyClose = true;
		g_plateTestDlg->dlg->close();
	}
	g_plateTestDlg = nullptr;
	QApplication::processEvents();
}
// 1 = parked (hidden + a Scene Objects row), 0 = on screen, -1 = no dialog.
GMTVTK_API int gmtvtk_platecalc_parked_test(void *handle) {
	Scene *s = static_cast<Scene *>(handle);
	if (!g_plateTestDlg || !g_plateTestDlg->dlg) return -1;
	const bool hidden = !g_plateTestDlg->dlg->isVisible();
	bool row = false;
	if (s) for (auto &pt : s->parkedTools) if (pt.win == g_plateTestDlg->dlg) row = true;
	return (hidden && row) ? 1 : 0;
}
// Drive the three selectors the way a user would: the model radio (by its key), then the two plate
// combos (by plate abbreviation). Any of them may be empty to leave that one alone. Returns the
// number of plates the moving combo ended up holding, or -1 when there is no dialog.
GMTVTK_API int gmtvtk_platecalc_select_test(const char *model, const char *fix, const char *mov) {
	if (!g_plateTestDlg || !g_plateTestDlg->dlg) return -1;
	PlateCalcDialog *p = g_plateTestDlg;
	if (model && *model) {
		const QString key = QString::fromUtf8(model);
		for (int i = 0; i < (int)p->modelBtns.size(); ++i)
			if (p->modelKeys[i] == key) { p->modelBtns[i]->setChecked(true); break; }
		QApplication::processEvents();
	}
	auto pick = [](QComboBox *cb, const char *abbrev) {
		if (!cb || !abbrev || !*abbrev) return;
		const int i = cb->findData(QString::fromUtf8(abbrev));
		if (i >= 0) cb->setCurrentIndex(i);
	};
	pick(p->cbFixed, fix);
	pick(p->cbMoving, mov);
	QApplication::processEvents();
	return p->cbMoving ? p->cbMoving->count() : -1;
}
// Fill the point boxes and press Calculate, exactly as the button does.
GMTVTK_API int gmtvtk_platecalc_calc_test(double lon, double lat) {
	if (!g_plateTestDlg || !g_plateTestDlg->dlg) return 0;
	if (g_plateTestDlg->edLon) g_plateTestDlg->edLon->setText(QString::number(lon, 'g', 10));
	if (g_plateTestDlg->edLat) g_plateTestDlg->edLat->setText(QString::number(lat, 'g', 10));
	g_plateTestDlg->calculate(g_plateTestDlg->dlg, false);
	QApplication::processEvents();
	return 1;
}
// A click on the plate map, at true lon/lat — the whole bdn_plate path (plate under the point, its
// nearest neighbour, the pole, the velocity) without a synthetic mouse event.
GMTVTK_API int gmtvtk_platecalc_map_click_test(double lon, double lat) {
	if (!g_plateTestDlg || !g_plateTestDlg->map) return 0;
	g_plateTestDlg->map->pickAt(lon, lat);
	QApplication::processEvents();
	return 1;
}
// How many plate polygons the map holds for the current model, and the tag of the plate under a
// point ('' when none) — enough to check the map really loaded and hit-tests where it should.
GMTVTK_API int gmtvtk_platecalc_map_test(double lon, double lat, char *buf, int cap) {
	if (!g_plateTestDlg || !g_plateTestDlg->map) return -1;
	const QByteArray b = g_plateTestDlg->map->plateAt(lon, lat).toUtf8();
	if (buf && cap > 0) {
		const int c = (b.size() < cap - 1) ? b.size() : cap - 1;
		memcpy(buf, b.constData(), c);
		buf[c] = '\0';
	}
	return (int)g_plateTestDlg->map->polys.size();
}
// Read one field back: "polelon" | "polelat" | "polerate" | "lon" | "lat" | "speed" | "azim" |
// "abs2rel" (visible/checked state) | "fixenabled". Returns the text length, -1 when unknown.
GMTVTK_API int gmtvtk_platecalc_read_test(const char *what, char *buf, int cap) {
	if (!g_plateTestDlg || !g_plateTestDlg->dlg || !what) return -1;
	PlateCalcDialog *p = g_plateTestDlg;
	const QString w = QString::fromUtf8(what);
	QString v;
	if      (w == "polelon")    v = p->poleLon  ? p->poleLon->text()  : QString();
	else if (w == "polelat")    v = p->poleLat  ? p->poleLat->text()  : QString();
	else if (w == "polerate")   v = p->poleRate ? p->poleRate->text() : QString();
	else if (w == "lon")        v = p->edLon    ? p->edLon->text()    : QString();
	else if (w == "lat")        v = p->edLat    ? p->edLat->text()    : QString();
	else if (w == "speed")      v = p->lbSpeed  ? p->lbSpeed->text()  : QString();
	else if (w == "azim")       v = p->lbAzim   ? p->lbAzim->text()   : QString();
	else if (w == "abs2rel")    v = (p->abs2rel && p->abs2rel->isVisible()) ? "1" : "0";
	else if (w == "fixenabled") v = (p->cbFixed && p->cbFixed->isEnabled()) ? "1" : "0";
	else return -1;
	const QByteArray b = v.toUtf8();
	if (buf && cap > 0) {
		const int c = (b.size() < cap - 1) ? b.size() : cap - 1;
		memcpy(buf, b.constData(), c);
		buf[c] = '\0';
	}
	return b.size();
}
// Grab the whole dialog: the only way to check the LAYOUT (Mirone's geometry, box sizes, the map).
GMTVTK_API int gmtvtk_platecalc_screenshot_test(const char *path) {
	if (!g_plateTestDlg || !g_plateTestDlg->dlg) return 0;
	QApplication::processEvents();
	QPixmap pm = g_plateTestDlg->dlg->grab();
	return pm.save(QString::fromUtf8(path), "PNG") ? 1 : 0;
}

static MagBarcodeDialog *g_magbarTestDlg = nullptr;
static void *g_magbarTestWin = nullptr;
// Goes through the EXACT real path: a real empty window (gmtvtk_open_empty) as parent, no manual
// resize (the dialog gets whatever size the .ui/layout naturally gives it, same as production) -
// eliminates "my test harness parented/sized it differently than the real menu action" as a cause.
GMTVTK_API int gmtvtk_magbarcode_open_dialog_test() {
	ensureApp();
	if (g_magbarTestDlg && g_magbarTestDlg->dlg) g_magbarTestDlg->dlg->close();
	if (!g_magbarTestWin) g_magbarTestWin = gmtvtk_open_empty("magbar test");
	Scene *s = (Scene*)g_magbarTestWin;
	if (!s || !s->win) return 0;
	g_magbarTestDlg = new MagBarcodeDialog(s->win);
	if (!g_magbarTestDlg->dlg) return 0;
	g_magbarTestDlg->dlg->show();
	QApplication::processEvents();
	return 1;
}
GMTVTK_API void gmtvtk_magbarcode_close_dialog_test() {
	if (g_magbarTestDlg && g_magbarTestDlg->dlg) g_magbarTestDlg->dlg->close();   // WA_DeleteOnClose self-frees
	g_magbarTestDlg = nullptr;
}
GMTVTK_API int gmtvtk_magbarcode_screenshot_test(const char *path) {
	if (!g_magbarTestDlg || !g_magbarTestDlg->dlg) return 0;
	QApplication::processEvents();
	QPixmap pm = g_magbarTestDlg->dlg->grab();
	return pm.save(QString::fromUtf8(path), "PNG") ? 1 : 0;
}
// Scroll the view to a given age (Ma) so a screenshot can inspect any part of the timescale.
GMTVTK_API void gmtvtk_magbarcode_scroll_test(double ageMa) {
	if (!g_magbarTestDlg || !g_magbarTestDlg->barcodeArea) return;
	g_magbarTestDlg->barcodeArea->setScroll(ageMa);
	if (g_magbarTestDlg->scrollBar)
		g_magbarTestDlg->scrollBar->setValue((int)(g_magbarTestDlg->barcodeArea->scrollY *
		                                            g_magbarTestDlg->barcodeArea->barHeight));
	QApplication::processEvents();
}
// Simulate a real left click at widget-local (x,y) on barcodeArea, then read back the resulting
// statusLabel text (proves the click->age readout actually fires through Qt's own event path).
GMTVTK_API void gmtvtk_magbarcode_click_test(double x, double y, char *outText, int outLen) {
	if (!g_magbarTestDlg || !g_magbarTestDlg->barcodeArea) return;
	QPointF pt(x, y);
	QPoint gpt = g_magbarTestDlg->barcodeArea->mapToGlobal(pt.toPoint());
	QMouseEvent press(QEvent::MouseButtonPress, pt, gpt, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(g_magbarTestDlg->barcodeArea, &press);
	QMouseEvent release(QEvent::MouseButtonRelease, pt, gpt, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(g_magbarTestDlg->barcodeArea, &release);
	QApplication::processEvents();
	if (outText && outLen > 0 && g_magbarTestDlg->barcodeArea->statusLabel) {
		QByteArray b = g_magbarTestDlg->barcodeArea->statusLabel->text().toUtf8();
		int n = std::min((int)b.size(), outLen - 1);
		memcpy(outText, b.constData(), n);
		outText[n] = 0;
	}
}
// Chron under a widget-local point (same lookup hover uses) — proves hover targeting + the new
// data/Cande_Kent_95.dat are wired correctly, without needing a real popup tooltip to screenshot.
GMTVTK_API void gmtvtk_magbarcode_hover_test(double x, double y, char *outText, int outLen) {
	if (!g_magbarTestDlg || !g_magbarTestDlg->barcodeArea) return;
	const auto *c = g_magbarTestDlg->barcodeArea->chronAt(x, y);
	QByteArray b = c ? c->name.toUtf8() : QByteArray();
	if (outText && outLen > 0) {
		int n = std::min((int)b.size(), outLen - 1);
		memcpy(outText, b.constData(), n);
		outText[n] = 0;
	}
}

// Run the REAL faultUpdatePlane (gray surface patch + buried 3-D plane) and report the 3-D plane.
// out[0]=exists out[1]=npts out[2]=visibility out[3]=zTop out[4]=zBot out[5]=grayVisible.
// Returns 1 when a 3-D plane actor exists. Lets the Julia test assert the plane is actually built.
GMTVTK_API int gmtvtk_fault_plane_test(void *scene, double width, double dip, double strike,
                                       int geog, double *out) {
	Scene *s = (Scene*)scene; if (!s) return 0;
	faultUpdatePlane(s, width, dip, strike, 90.0, geog != 0);   // rake fixed: this test asserts plane geometry, not arrows
	for (auto &p : s->polys) if (p.isFault && p.faultPlane3D) {
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

// test hook: verify NswingDialog's "only RUN executes" invariant for real, instead of by reading the
// source. Builds a real (never shown) NswingDialog, sends a SYNTHETIC Return keypress straight into
// its Name field via QApplication::sendEvent (same technique as gmtvtk_meca_drag_test/
// gmtvtk_symbol_ui_drag_test — drive the real widget, don't call internals directly), and reports
// whether the dialog ended up ACCEPTED as a result. Never touches g_juliaNswing/GMT.
GMTVTK_API int gmtvtk_nswing_enter_test(void *scene) {
	Scene *s = static_cast<Scene*>(scene);
	NswingDialog dlg(nullptr, s);
	dlg.show();                                 // QDialog::keyPressEvent's default-button search checks
	QApplication::processEvents();              // isVisible() — an unshown dialog would false-negative
	dlg.nameEdit->setFocus();
	QApplication::processEvents();
	QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	QApplication::sendEvent(dlg.nameEdit, &press);
	QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
	QApplication::sendEvent(dlg.nameEdit, &release);
	int ran = dlg.result() == QDialog::Accepted ? 1 : 0;
	dlg.hide();
	return ran;
}

// test hook: open a real AquamotoWindow (loaded from aquamoto.ui via QUiLoader, no parent), show
// it, and report whether every expected control was actually found by findChild — the exact bug
// class an "it compiled" claim would miss (a reparenting mistake leaving the window visually
// empty). Optionally grabs a screenshot to `pngPath` ("" to skip) so the result can be visually
// verified, not just asserted. Returns: 1 = window + every control found, 0 = the .ui failed to
// load at all, -1 = window loaded but one or more expected controls were missing.
GMTVTK_API int gmtvtk_aquamoto_open_test(void *scene, const char *pngPath) {
	auto *w = new AquamotoWindow(nullptr, (Scene*)scene);
	if (!w->win) { delete w; return 0; }
	w->win->show();
	// Pump events for a bit (not just one processEvents() call) -- a brand-new native window's
	// first paint can lag the OS theme engine (UxTheme "Scrollbar" data), so a screenshot grabbed
	// too early can catch an under-themed frame that a normally-running app never shows.
	{
		QElapsedTimer t; t.start();
		while (t.elapsed() < 500) { QApplication::processEvents(QEventLoop::AllEvents, 50); }
	}
	const bool allFound = w->pathEdit && w->sliceSlider && w->sliceSpin && w->splitDryWetCheck &&
	                       w->scaleGlobalCheck && w->waterTransparencySlider && w->showSliceBtn && w->runInBtn;
	if (pngPath && pngPath[0]) {
		QPixmap pm = w->win->grab();
		pm.save(QString::fromUtf8(pngPath));
		if (w->sliceSlider) {
			// A separate, upscaled close-up of JUST the slider widget -- the arrow glyphs are only
			// a few px tall in the full-window shot, easy to miss/misjudge at that scale.
			QPixmap sl = w->sliceSlider->grab();
			QPixmap big = sl.scaled(sl.width() * 4, sl.height() * 4);
			QString p2 = QString::fromUtf8(pngPath) + ".slider.png";
			big.save(p2);
		}
	}
	return allFound ? 1 : -1;
}

// test hook: diagnose WHERE the AquamotoWindow's size diverges from the .ui's declared geometry.
// out[0,1] = win->size() right after QUiLoader::load() (before show()); out[2,3] = win->sizeHint();
// out[4,5] = win->layout() ? layout()->minimumSize() : (-1,-1); out[6,7] = win->size() after
// show()+processEvents(). Lets the mismatch be pinned to "load already wrong" vs
// "content needs more room than 760x640" vs "show() itself re-lays-out" instead of guessing.
GMTVTK_API int gmtvtk_aquamoto_size_test(void *scene, double *out) {
	auto *w = new AquamotoWindow(nullptr, (Scene*)scene);
	if (!w->win) { delete w; return 0; }
	out[0] = w->win->size().width();  out[1] = w->win->size().height();
	out[2] = w->win->sizeHint().width(); out[3] = w->win->sizeHint().height();
	if (w->win->layout()) {
		out[4] = w->win->layout()->minimumSize().width();
		out[5] = w->win->layout()->minimumSize().height();
	} else { out[4] = -1; out[5] = -1; }
	w->win->show();
	QApplication::processEvents();
	out[6] = w->win->size().width(); out[7] = w->win->size().height();
	return 1;
}

// test hook: isolate whether a bare "windowsvista"-styled QScrollBar draws arrow buttons AT ALL in
// this environment -- a standalone QWidget + QScrollBar, nothing from AquamotoWindow/CubeLayerDialog
// involved, to settle whether the "force windowsvista" trick genuinely works here before blaming
// any specific dialog's wiring.
GMTVTK_API int gmtvtk_scrollbar_style_test(const char *pngPath, const char *styleKey) {
	auto *host = new QWidget(nullptr);
	host->resize(300, 60);
	auto *sb = new QScrollBar(Qt::Horizontal, host);
	sb->setGeometry(10, 20, 260, 20);
	sb->setRange(1, 180);
	sb->setValue(1);
	QString styleName = "(app default, no override)";
	const QString key = QString::fromUtf8(styleKey ? styleKey : "");
	if (!key.isEmpty()) {
		if (QStyle *classicStyle = QStyleFactory::create(key)) {
			classicStyle->setParent(sb);
			sb->setStyle(classicStyle);
			styleName = classicStyle->objectName();
		} else {
			styleName = "(create FAILED)";
		}
	}
	host->show();
	QElapsedTimer t; t.start();
	while (t.elapsed() < 500) { QApplication::processEvents(QEventLoop::AllEvents, 50); }
	if (pngPath && pngPath[0]) {
		QPixmap pm = host->grab();
		QPixmap big = pm.scaled(pm.width() * 3, pm.height() * 3);
		big.save(QString::fromUtf8(pngPath));
	}
	qWarning("scrollbar_style_test: requested=%s style=%s transient=%d", qUtf8Printable(key),
	         qUtf8Printable(styleName), sb->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, sb));
	return 1;
}

// test hook: flip an Aquamoto scene to "Shade Land" (same state change the Scene Objects "Color Bar
// Land" checkbox lambda makes, 50_scene.cpp:1489/1496) so the LAND colorbar becomes visible without
// driving the real Qt checkbox — lets a screenshot show its ACTUAL rendered geometry (bar rect vs
// tick positions) for diagnosis. Returns 1 on success, 0 on a dead scene.
GMTVTK_API int gmtvtk_aqua_force_land_bar_test(void *scene) {
	// NOTE: sceneAlive() checks a file-static registry that does NOT cross the DLL boundary (this
	// hook is called with a Scene *opened through the PRODUCTION gmtvtk.dll, not this test dll) --
	// only a null-check here, no registry lookup.
	Scene *s = static_cast<Scene*>(scene);
	if (!s || !s->ren) return 0;
	s->aquaLandShowBar = true;
	s->aquaShowWater = false;
	refreshGridColorbar(s);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// test hook: is the VISIBLE Swipe/Link toolbar button currently enabled? Direct property read, no
// simulated click -- lets a test catch a model/view desync (swipeAct enabled but the on-screen
// tbSwipe button left disabled, or vice versa) that driving only the QAction would never expose.
GMTVTK_API int gmtvtk_swipe_btn_enabled_test(void *scene) {
	Scene *s = static_cast<Scene*>(scene);
	if (!s || !s->swipeToolBtn) return -1;
	return s->swipeToolBtn->isEnabled() ? 1 : 0;
}

// test hook: put the toolbar slot in Swipe or Link mode WITHOUT arming it (swipeSetMode) -- the state
// the real dropdown produces on its way to arming, isolated so a test can check availability/icon
// bookkeeping alone. The real dropdown handler is swipeSelectMode = this PLUS the arming click;
// gmtvtk_swipe_select_mode_h wraps that one.
GMTVTK_API void gmtvtk_swipe_set_mode_test(void *scene, int linkMode) {
	Scene *s = static_cast<Scene*>(scene);
	if (!s) return;
	swipeSetMode(s, linkMode != 0);
}

// test hook: what kind of Link is running on this window -- 0 = off, 1 = in-window pair (two rasters
// of this window), 2 = cross-window pair (partner is another open window).
GMTVTK_API int gmtvtk_link_state_test(void *scene) {
	Scene *s = static_cast<Scene*>(scene);
	if (!s || !s->linkOn) return 0;
	return s->linkPartnerScene ? 2 : 1;
}

// test hook: simulate a REAL click on the Swipe/Link toolbar button — QAbstractButton::click(), the
// SAME press+release+clicked() sequence Qt performs for an actual mouse click. Verifies the ACTUAL
// signal wiring (tbSwipe -> actSwipe -> swipeToggled/linkToggled), not just calling the internal
// handler directly. Returns the button's checked state afterward, or -1 if it doesn't exist yet.
GMTVTK_API int gmtvtk_swipe_click_test(void *scene) {
	Scene *s = static_cast<Scene*>(scene);
	if (!s || !s->swipeToolBtn) return -1;
	s->swipeToolBtn->click();
	return s->swipeToolBtn->isChecked() ? 1 : 0;
}

// test hook: emit the viewport's customContextMenuRequested signal directly at (x,y) WIDGET-LOCAL
// pixel coordinates — the SAME signal Qt delivers for a real right-click (Qt::CustomContextMenu
// policy). Verifies the ACTUAL connected routing lambda (70_window.cpp) -- the Link swallow, the
// rubber-band/polygon-drawing guards, the per-element hit tests.
GMTVTK_API void gmtvtk_right_click_test(void *scene, int x, int y) {
	Scene *s = static_cast<Scene*>(scene);
	if (!s || !s->widget) return;
	s->widget->customContextMenuRequested(QPoint(x, y));
}

// test hook: turn Link mode on/off exactly as the real toolbar toggle does (linkToggled,
// 57_swipe.cpp), without needing to click the real checkable button. Returns 1 if Link ended up ON
// (0 if it refused — not enough rasters, or the pair doesn't overlap; check status bar text via
// gmtvtk_objrows_test-style introspection if you need the reason).
GMTVTK_API int gmtvtk_link_toggle_test(void *scene, int on) {
	Scene *s = static_cast<Scene*>(scene);
	if (!s || !s->swipeAct) return 0;
	linkToggled(s, s->swipeAct, on != 0);
	return s->linkOn ? 1 : 0;
}

// test hook: Link's press-and-hold peek (linkPeek, 57_swipe.cpp) driven directly — on=1 is "right
// button held", on=0 is "released". Bypasses the event plumbing; gmtvtk_right_button_test below is
// the one that goes through it.
GMTVTK_API void gmtvtk_link_peek_test(void *scene, int on) {
	Scene *s = static_cast<Scene*>(scene);
	if (!s) return;
	linkPeek(s, on != 0);
}

// test hook: deliver a REAL right-button press (press=1) or release (press=0) to the viewport at
// (x,y) WIDGET-LOCAL pixels, exactly as Qt does for a physical click — QApplication::sendEvent runs
// the installed event filters, so this exercises the ACTUAL LinkPeekFilter wiring (and its
// rubber-band / polygon-drawing guards), not just linkPeek called by hand.
GMTVTK_API void gmtvtk_right_button_test(void *scene, int x, int y, int press) {
	Scene *s = static_cast<Scene*>(scene);
	if (!s || !s->widget) return;
	const QPointF pos(x, y);
	QMouseEvent ev(press ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
	               pos, s->widget->mapToGlobal(pos.toPoint()),
	               Qt::RightButton, press ? Qt::RightButton : Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(s->widget, &ev);
}

// test hook: read back a window's CURRENT visible world region (sceneVisibleRegion, 10_geometry.cpp)
// as (W,E,S,N) into `out4`. Returns 0 if there is no renderer yet (an unbuilt empty launcher).
GMTVTK_API int gmtvtk_visible_region_test(void *scene, double *out4) {
	Scene *s = static_cast<Scene*>(scene);
	if (!s || !out4) return 0;
	double W, E, S, N;
	if (!sceneVisibleRegion(s, W, E, S, N)) return 0;
	out4[0] = W; out4[1] = E; out4[2] = S; out4[3] = N;
	return 1;
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

// Register the File > Save Screenshot GeoTIFF callback (in-memory pixel hand-off, see
// JuliaSaveGeoTiffFn). nullptr to detach.
GMTVTK_API void gmtvtk_set_save_geotiff_callback(JuliaSaveGeoTiffFn fn) {
	g_juliaSaveGeoTiff = fn;
}

// Register the Scene Objects "Move to new window" callback (grid rows). fn(scene, "<kind>;<name>")
// returns 1 if Julia re-opened the grid in a new window (the source window then removes it), 0 on
// failure (the grid stays put). nullptr to detach.
GMTVTK_API void gmtvtk_set_move_callback(JuliaMoveFn fn) {
	g_juliaMove = fn;
}

// Register the Scene Objects image-row "Auto histogram stretch" callback. fn(scene, "image;<name>")
// builds a percentile histogram-stretched 8-bit copy of the named image as a new row. nullptr to detach.
GMTVTK_API void gmtvtk_set_img_stretch_callback(JuliaImgStretchFn fn) {
	g_juliaImgStretch = fn;
}

// Register the tide-station download callback. The two "Download Mareg …" entries on a Tide
// Stations star's right-click menu call fn(scene, mode, station): mode "2days" | "calendar",
// station = the clicked star's "Name:/Code:/Country:" block. Julia opens the download window.
GMTVTK_API void gmtvtk_set_tides_callback(JuliaTidesFn fn) {
	g_juliaTides = fn;
}

// Register the tide-prediction callback. The "Plot tides (now)"/"Plot tides (calendar)" entries on
// a Tide Prediction Stations triangle's right-click menu call fn(scene, mode, station): mode is
// "now" or "calendar/<startISO>/<endISO>"; station is the clicked triangle's hover text (the
// station name). Julia harmonic-synthesizes a prediction over the requested window and opens an
// X,Y plot window.
GMTVTK_API void gmtvtk_set_tidemodel_callback(JuliaTideModelFn fn) {
	g_juliaTideModel = fn;
}

// Register the Earth-tides callback. The Geography > Earth Tides dialog calls fn(scene, req) with
// "<mode>/<startISO>/<endISO>/<lon>/<lat>/<comp>/<W>/<E>/<S>/<N>" (mode "series"|"grid", comp a
// subset of "VEN"); Julia runs GMT.earthtide. nullptr to detach.
GMTVTK_API void gmtvtk_set_earthtide_callback(JuliaEarthTideFn fn) {
	g_juliaEarthTide = fn;
}

// Register the 3D cube layer selector callback. Called when the user selects a layer from the
// non-modal cube layer dialog. nullptr to detach.
GMTVTK_API void gmtvtk_set_cube_layer_callback(JuliaCubeLayerFn fn) {
	g_juliaCubeLayer = fn;
}

// Register the "Load all in RAM" callback for the cube layer dock. nullptr to detach.
GMTVTK_API void gmtvtk_set_cube_loadall_callback(JuliaCubeLoadAllFn fn) {
	g_juliaCubeLoadAll = fn;
}

// Register the per-element "Cube layers…" callback (reopen the slider bound to a named cube). nullptr to detach.
GMTVTK_API void gmtvtk_set_cube_slider_callback(JuliaCubeSliderFn fn) {
	g_juliaCubeSlider = fn;
}

// Flag the Scene Objects element `name` (the base surface or an extra grid) as a 3-D cube with
// `nLayers` layers, so its properties menu offers "Cube layers…". nLayers <= 1 clears the flag.
GMTVTK_API void gmtvtk_mark_element_cube(void *scene, const char *name, int nLayers) {
	Scene *s = static_cast<Scene*>(scene);
	if (!sceneAlive(s) || !name) return;
	QString nm = QString::fromUtf8(name);
	int nl = nLayers > 1 ? nLayers : 0;
	if (!s->surfName.empty() && nm == QString::fromStdString(s->surfName)) { s->cubeNLayers = nl; return; }
	for (auto &ex : s->extras)
		if (nm == QString::fromStdString(ex.name)) { ex.cubeLayers = nl; return; }
}

// Which cube-layer algorithm is selected in the Shading dock: 1 = flat shaded image (fast), 0 = a
// surface look (Cast shadows / Hillshade). Julia's cube-layer callback reads this to pick how to
// render the layer.
GMTVTK_API int gmtvtk_cube_flat_mode(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	return (sceneAlive(s) && s->cubeFlatImg) ? 1 : 0;
}

// Julia marks the window as showing a 3-D-cube layer (0-based index + colour-range choice) after each
// render, so the Shading dock can re-render THIS layer when the user switches algorithm and reflect
// the active choice in its checkboxes (enables the cube-only "Shaded image (2-D)" box).
GMTVTK_API void gmtvtk_mark_cube(void *handle, int layer_index, int use_global) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	s->isCube = true; s->cubeLayerCur = layer_index; s->cubeUseGlobal = use_global;
	syncShadeChecks(s);
}

// Pin the vertical axis box + Z tick labels to the whole cube's z-range so the axes do NOT shift as
// the user switches layers (each layer's own min/max differs slightly). Call once when a cube is
// dropped, BEFORE the first layer builds; pass zmax <= zmin to clear the lock. No render here — the
// next surface build / applyVE picks it up.
GMTVTK_API void gmtvtk_set_cube_axes_zrange(void *handle, double zmin, double zmax) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	if (zmax > zmin) { s->cubeZLock = true; s->cubeZMin = zmin; s->cubeZMax = zmax; }
	else             { s->cubeZLock = false; }
}

// Show the non-modal 3D cube layer selector dialog. `scene` is the target window, `name` is the
// cube's base name (for the dialog title), `nLayers` is the number of layers in the cube. The
// dialog stays open until the user closes it, allowing quick layer switching.
GMTVTK_API void gmtvtk_show_cube_layer_dialog(void *scene, const char *name, int nLayers) {
	ensureApp();
	Scene *s = static_cast<Scene*>(scene);
	if (!sceneAlive(s) || !name || nLayers <= 0) return;
	showCubeLayerDialog(s, QString::fromUtf8(name), nLayers);   // per-Scene dialog (70_window.cpp)
}

// Show the MODAL multi-variable netCDF picker. `rows` is a "\t"-separated,
// "\n"-terminated table ("name\tsize\ttype\n..."). Writes the 0-based indices of
// the ticked variables into `sel` (capacity `maxSel`), sets `*prescan` to whether
// the per-layer min/max option is on, and returns how many variables were chosen
// (0 = cancelled). Blocks until the user answers (called synchronously from the
// Julia drop handler).
GMTVTK_API int gmtvtk_pick_netcdf_var(void *scene, const char *title, const char *rows,
                                      int *sel, int maxSel, int *prescan) {
	ensureApp();
	Scene *s = static_cast<Scene*>(scene);
	if (!rows) return 0;
	return showNetcdfVarDialog(s, QString::fromUtf8(title ? title : ""), QString::fromUtf8(rows),
	                           sel, maxSel, prescan);
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

// Hide the window's base surface plane AND its own drape, if any (keeping geometry for axis bounds
// + hover sampling). Used by the basemap/drop scaffold paths: the promoted flat z=0 plane is only
// scaffold under the draped tile, which is added as a SEPARATE ExtraObj there, so those callers
// never have a base s->drape and hiding surf alone was enough. A PRIMARY bare image window
// (gmtvtk_view_grid with image_only) DOES carry its texture on the base s->drape, not an extra — so
// this must hide both, or "hide the primary" leaves the actual picture fully visible underneath.
// Pick the Swipe/Link toolbar mode programmatically — the SAME entry point the dropdown menu items
// use (swipeSelectMode, 57_swipe.cpp): reset the shared toggle off, swap the slot's icon/tooltip,
// re-check availability, then TURN THE PICKED MODE ON (picking is starting — there is no separate
// arming call, here or in the UI). A real (non-test) export: it is legitimate small
// host-scriptable UI state, and — unlike a `_test`-dll hook — it runs the availability check
// against THIS window's own production `g_scenes`, the one that actually has every other open
// window in it (a test-dll hook has its own separate, always-empty copy of that registry; see the
// cross-DLL note on gmtvtk_aqua_force_land_bar_test below).
GMTVTK_API void gmtvtk_swipe_select_mode_h(void *handle, int linkMode) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	swipeSelectMode(s, linkMode != 0);
}

GMTVTK_API void gmtvtk_hide_surface(void *handle) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	surfSetVisibility(s, 0);
	if (s->drape) s->drape->SetVisibility(0);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Hide every grid (base surface AND every non-image extra) EXCEPT `keepname` — the ground-truth
// version of the SACRED_LAW.md derived-variable display rule. Operates DIRECTLY on the live scene
// (s->extras / s->surf / s->surfName), never through a Julia-side bookkeeping dict — a Julia-side
// registry can only hide what IT remembers being added, so any grid that reached the scene through
// a path the registry doesn't track is invisible to a Julia-only fix. Reading straight off the
// actual actors/names removes that whole class of gap: whatever the Scene Objects panel shows a
// checkbox for is exactly what this loop can see and hide.
// `keep_none` != 0 hides EVERY grid, no exceptions (used when the new derived variable is a
// DIFFERENT kind than what it was derived from — e.g. "Crop Image" capturing a picture FROM a
// grid: the source to uncheck is a grid, but the result is an image, so there is no grid NAME to
// pass as `keepname` at all). Without this, an empty/"" `keepname` would be ambiguous with "keep
// the unnamed base" instead of "keep nothing".
GMTVTK_API void gmtvtk_hide_other_grids(void *handle, const char *keepname, int keep_none) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	const std::string keep = keepname ? keepname : "";
	for (auto &ex : s->extras) {
		if (ex.isImage || (!keep_none && ex.name == keep)) continue;
		if (ex.actor) ex.actor->SetVisibility(0);
		if (ex.drape) ex.drape->SetVisibility(0);
	}
	if (surfProp(s) && (keep_none || s->surfName != keep)) {
		surfSetVisibility(s, 0);
		if (s->drape) s->drape->SetVisibility(0);
	}
	refreshGridColorbar(s);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Same as gmtvtk_hide_other_grids, for images: hide every image (primary bare image AND every
// image extra) EXCEPT `keepname`, reading directly off the live scene. The primary bare image's
// texture lives on s->drape gated by s->imageOnly (sceneHasImage's own check), never on a grid's
// s->surf mesh, so it is hidden the same way gmtvtk_hide_surface already does for that case.
GMTVTK_API void gmtvtk_hide_other_images(void *handle, const char *keepname) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	const std::string keep = keepname ? keepname : "";
	for (auto &ex : s->extras) {
		if (!ex.isImage || ex.name == keep) continue;
		if (ex.actor) ex.actor->SetVisibility(0);
		if (ex.drape) ex.drape->SetVisibility(0);
	}
	if (s->imageOnly && s->drape && s->surfName != keep) {
		surfSetVisibility(s, 0);
		s->drape->SetVisibility(0);
	}
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Capture the CURRENT render as RGB pixels, cropped to the on-screen rectangle that world bbox
// (w,e,south,north — plain data coordinates, same convention rectRoiCrop already computes) projects
// to at the CURRENT camera state (no flat2d forcing, unlike the GeoTIFF export — works at any
// tilt/zoom). Same proven NDC-projection technique as captureAxesInteriorRGB (70_window.cpp, Save
// Screenshot GeoTIFF), applied to an arbitrary caller-supplied bbox instead of the whole data bbox.
// Z isn't sampled per-point (would need the real terrain height under the rectangle); the corners
// bracket the WHOLE scene's z-range instead, which only ever widens the captured pixel rect a
// little under a tilted view and is exact in the app's common flat/top-down orthographic mode
// (z doesn't move screen x/y there at all). Used by Roi Crop Tools' "Crop Image" on a window with
// no separate bitmap image — the rendered GRID is itself the picture to crop in that case. Axes /
// colorbar, and every drawn polygon/line overlay/symbol layer (the crop-selection rectangle ITSELF
// is one of these — it must never bake into the picture it's selecting) are hidden for the capture
// and restored after. Caller owns the returned buffer (gmtvtk_free_rgb frees it). Returns 1 on
// success, 0 if the bbox has zero on-screen area (e.g. the rectangle is fully off the viewport).
GMTVTK_API int gmtvtk_capture_rect_rgb(void *handle, double w, double e, double south, double north,
                                        unsigned char **outRgb, int *outW, int *outH) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !s->ren || !s->widget || !outRgb || !outW || !outH) return 0;
	const int axesVis = s->axes ? s->axes->GetVisibility() : 0;
	const bool barVis  = colorbarVisible(s);
	if (s->axes) s->axes->SetVisibility(0);   // decoration only — never part of the captured pixels
	if (s->bar)  setColorbarVisible(s, false);
	// The crop-selection rectangle (and any other drawn line/point overlay, symbol layer) is UI
	// markup, never part of the picture being captured — hide every one for the shot, remember
	// which were actually visible so only those come back after.
	std::vector<vtkActor*> hiddenPolyLine, hiddenPolyFill;
	for (auto &pg : s->polys) {
		if (pg.line && pg.line->GetVisibility()) { pg.line->SetVisibility(0); hiddenPolyLine.push_back(pg.line.Get()); }
		if (pg.fill && pg.fill->GetVisibility()) { pg.fill->SetVisibility(0); hiddenPolyFill.push_back(pg.fill.Get()); }
	}
	std::vector<vtkActor*> hiddenOverlay;
	for (auto &ov : s->overlays)
		if (ov.actor && ov.actor->GetVisibility()) { ov.actor->SetVisibility(0); hiddenOverlay.push_back(ov.actor.Get()); }
	std::vector<vtkActor*> hiddenSymbol;
	for (auto &sl : s->symbols)
		if (sl.actor && sl.actor->GetVisibility()) { sl.actor->SetVisibility(0); hiddenSymbol.push_back(sl.actor.Get()); }
	s->widget->renderWindow()->Render();

	vtkNew<vtkWindowToImageFilter> w2i;
	w2i->SetInput(s->widget->renderWindow());
	w2i->SetScale(2); w2i->Update();
	vtkImageData *full = w2i->GetOutput();
	int dims[3]; full->GetDimensions(dims);

	vtkCamera *cam = s->ren->GetActiveCamera();
	const double aspect = (dims[1] > 0) ? double(dims[0]) / double(dims[1]) : 1.0;
	vtkMatrix4x4 *M = cam->GetCompositeProjectionTransformMatrix(aspect, -1.0, 1.0);
	const double bx0 = w * s->xfac, bx1 = e * s->xfac;                    // world/scene units (actor scale)
	const double bz0 = s->zmin * s->zfac * s->ve, bz1 = s->zmax * s->zfac * s->ve;
	double nx0=1e300, nx1=-1e300, ny0=1e300, ny1=-1e300;
	for (double cx : { bx0, bx1 })
		for (double cy : { south, north })
			for (double cz : { bz0, bz1 }) {
				double p[4] = { cx, cy, cz, 1.0 }, o[4];
				M->MultiplyPoint(p, o);
				if (o[3] != 0.0) {
					const double ndcx = o[0]/o[3], ndcy = o[1]/o[3];
					nx0 = std::min(nx0, ndcx); nx1 = std::max(nx1, ndcx);
					ny0 = std::min(ny0, ndcy); ny1 = std::max(ny1, ndcy);
				}
			}
	auto ndcToPix = [](double n, int size) { return std::clamp(int(std::round((n*0.5+0.5) * (size-1))), 0, size-1); };
	const int px0 = ndcToPix(nx0, dims[0]), px1 = ndcToPix(nx1, dims[0]);
	const int py0 = ndcToPix(ny0, dims[1]), py1 = ndcToPix(ny1, dims[1]);
	const bool ok = (px1 > px0 && py1 > py0);
	if (ok) {
		const int cw = px1 - px0 + 1, ch = py1 - py0 + 1;
		*outW = cw; *outH = ch;
		unsigned char *buf = new unsigned char[size_t(cw) * size_t(ch) * 3];
		for (int row = 0; row < ch; ++row) {
			auto *src = static_cast<unsigned char*>(full->GetScalarPointer(px0, py0 + row, 0));
			unsigned char *dst = buf + size_t(ch - 1 - row) * size_t(cw) * 3;   // bottom-up -> top-first
			std::memcpy(dst, src, size_t(cw) * 3);
		}
		*outRgb = buf;
	}
	if (s->axes) s->axes->SetVisibility(axesVis);
	if (s->bar)  setColorbarVisible(s, barVis);
	for (auto *a : hiddenPolyLine) a->SetVisibility(1);
	for (auto *a : hiddenPolyFill) a->SetVisibility(1);
	for (auto *a : hiddenOverlay)  a->SetVisibility(1);
	for (auto *a : hiddenSymbol)   a->SetVisibility(1);
	s->widget->renderWindow()->Render();
	return ok ? 1 : 0;
}

// Capture the rectangle's picture straight FROM THE GRID DATA, at native data resolution, through
// bakeLayerRGBA (40_shading.cpp) -- the SAME relief-shade source of truth the flat-image drape and
// its hi-res zoom-detail tile already bake from (SACRED_LAW.md: one shading engine, never forked).
// Unlike gmtvtk_capture_rect_rgb (a SCREEN-SPACE grab: quality is tied to the CURRENT render-window
// size and zoom, so a small window or a zoomed-out view yields a tiny/blurry crop even though the
// underlying grid is full-resolution), this bakes as many pixels as the rectangle spans grid nodes
// (capped like every other bake, kLayerTexMaxPix) -- resolution matches "Crop Grid"'s own data-space
// crop, independent of window size or camera angle. Used by Roi Crop Tools' "Crop Image"/"Crop Image
// (with coords)" whenever there is no separate bitmap image to crop (the rendered GRID IS the
// picture); falls back (returns 0) to the screen-space capture when there is no baked grid to read
// (e.g. surfLut isn't a vtkColorTransferFunction), letting the caller keep the old path as a fallback.
GMTVTK_API int gmtvtk_capture_rect_databaked(void *handle, double w, double e, double south, double north,
                                              unsigned char **outRgb, int *outW, int *outH) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !outRgb || !outW || !outH) return 0;
	if (s->gridZ.empty() || s->gnx < 2 || s->gny < 2 || s->gdx == 0.0 || s->gdy == 0.0) return 0;
	if (!(e > w && north > south)) return 0;
	vtkColorTransferFunction *ctf = vtkColorTransferFunction::SafeDownCast(s->surfLut);
	if (!ctf) return 0;
	const int wnx = std::max(2, (int)std::lround((e - w) / s->gdx) + 1);
	const int wny = std::max(2, (int)std::lround((north - south) / s->gdy) + 1);
	int txW, txH; layerTexSize(wnx, wny, txW, txH);
	std::vector<unsigned char> rgba;
	bakeLayerRGBA(s, s->gridZ.data(), s->gnx, s->gny, s->gx0, s->gy0, s->gdx, s->gdy, ctf,
	              s->zmin, s->zmax, w, e, south, north, txW, txH, rgba);
	if (rgba.size() != (size_t)txW * txH * 4) return 0;
	unsigned char *buf = new unsigned char[(size_t)txW * txH * 3];
	for (size_t i = 0, n = (size_t)txW * txH; i < n; ++i) {           // drop alpha; flip row 0 south -> row 0 north
		const size_t row = i / txW, col = i % txW;
		const size_t srci = (row * txW + col) * 4, dsti = ((size_t)(txH - 1 - row) * txW + col) * 3;
		buf[dsti] = rgba[srci]; buf[dsti+1] = rgba[srci+1]; buf[dsti+2] = rgba[srci+2];
	}
	*outRgb = buf; *outW = txW; *outH = txH;
	return 1;
}

// Free a buffer returned by gmtvtk_capture_rect_rgb / gmtvtk_capture_rect_databaked.
GMTVTK_API void gmtvtk_free_rgb(unsigned char *buf) { delete[] buf; }

// Re-frame the AXES CUBE + camera to an ARBITRARY world bbox (x0,x1,y0,y1 — plain data
// coordinates), instead of always the window's PRIMARY surface (`s->axes->SetBounds` normally only
// ever gets set from `surfGetBounds`, e.g. in applyVE — see that function's own comment). Used by
// Roi Crop Tools: SACRED_LAW.md's group-uncheck/derived-variable laws already make the crop the
// only thing shown, but the axes cube kept the PARENT's full extent since it never tracked anything
// but the primary — a derived variable's axes must fit ITS OWN limits. Z IS re-derived too, from the ACTIVE
// grid, since a derived variable is a NEW quantity in ITS OWN units; the degenerate-Z guard mirrors
// applyVE's own (a zero Z range makes vtkCubeAxesActor compute NaN label counts and abort the
// render). Camera re-fit reuses fitSnapView's exact technique (20_gizmo.cpp), generalized to a
// caller-supplied bbox instead of always surfGetBounds.
//
// `keepMargin`: images keep their axis tick LABELS on screen with a margin (fill=0.84 — the SAME
// value gmtvtk_view_grid's own referenced-image path already uses, 90_c_api.cpp's imode==1 branch)
// instead of grids' own edge-to-edge fill=1.0 (which deliberately pushes labels off-screen — see
// fitSnapView's own comment; that is correct, EXISTING behaviour for grids, not something to
// change). Passing the wrong one for an image wiped its axis labels entirely — found live 2026-07-21.
// Z-EXPLICIT form. Same body; the caller supplies the Z range instead of it being resolved from the
// active grid. A MESH layer (a VTK .vtp surface, a GMTfv solid) has no grid data layer at all, so
// activeGridZRange cannot speak for it and would leave the box wearing the previous layer's Z —
// exactly the mistake SACRED_LAW.md's derived-variable axes law, Z half, is about. gmtvtk_reframe_h
// below is this function with the grid-resolved Z, NOT a second implementation. The camera-fit
// maths itself lives in ONE place, `cameraFitToScaledBBox` (10_geometry.cpp) — this function is that
// PLUS the axes/frame-bookkeeping half; the Link tool's cross-window sync (57_swipe.cpp) calls the
// camera-only half directly, since it must never touch the PARTNER window's own axes/frame.
GMTVTK_API void gmtvtk_reframe_z_h(void *handle, double x0, double x1, double y0, double y1,
                                   double z0, double z1, int keepMargin) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !s->ren || !s->widget || !s->axes) return;

	const double zlo = z0, zhi = z1;
	double b[6] = { x0 * s->xfac, x1 * s->xfac, y0, y1,
	                zlo * s->zfac * s->ve, zhi * s->zfac * s->ve };
	if (b[5] <= b[4]) b[5] = b[4] + 1.0;               // degenerate-Z guard, same as applyVE's flatZ case
	// The ACTUAL visible tick-label TEXT is a separate custom billboard system (rebuildAxisLabels,
	// 10_geometry.cpp) — NOT vtkCubeAxesActor's own native labels (deliberately off, "different text
	// engine"). It positions every X/Y/Z billboard from surfGetBounds(), i.e. the PRIMARY SURFACE's
	// bounds — never from s->axes directly, so SetBounds/SetXAxisRange/SetYAxisRange on s->axes alone
	// (tried first, still produced NO visible labels) can't fix this. Setting the override here makes
	// surfGetBounds report the crop's bounds instead, so rebuildAxisLabels below draws labels for the
	// RIGHT region automatically, with no changes needed to it or any other surfGetBounds caller.
	s->viewBoundsOverride = true;
	for (int i = 0; i < 6; ++i) s->viewBounds[i] = b[i];
	s->axes->SetBounds(b);
	s->axes->SetXAxisRange(x0, x1);
	s->axes->SetYAxisRange(y0, y1);
	s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1;    // keep the frame bookkeeping in sync (gmtvtk_grow_frame_h's shrink counterpart)

	cameraFitToScaledBBox(s, b, keepMargin != 0);
	rebuildAxisLabels(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Z comes from the ACTIVE grid (activeGridZRange), NOT s->zmin/zmax: the caller of this form is
// always a DERIVED grid variable, which carries its own units, so the base surface's range would box
// and label it wrong (SACRED_LAW.md derived-variable axes law, Z half). Falls back to the base range
// when no grid layer resolves (image / cloud / solid window).
GMTVTK_API void gmtvtk_reframe_h(void *handle, double x0, double x1, double y0, double y1, int keepMargin) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	double zlo = s->zmin, zhi = s->zmax;
	activeGridZRange(s, zlo, zhi);
	gmtvtk_reframe_z_h(handle, x0, x1, y0, y1, zlo, zhi, keepMargin);
}

// Re-frame the window onto the content it actually holds NOW (forward-declared in 10_geometry.cpp for
// `sceneClearViewOverride`, which calls it after releasing a derived variable's frame pin). Bounds
// come from `surfGetBounds` — which, with the pin gone, reports the real remaining actors — and are
// un-scaled back to data coordinates because gmtvtk_reframe_z_h takes plain data limits. `keepMargin`
// follows the house convention: a picture keeps the margin that holds its tick labels on screen, a
// grid fills edge-to-edge (see gmtvtk_reframe_z_h's own note).
static void sceneReframeToContent(Scene *s) {
	if (!sceneAlive(s) || !s->ren || !s->axes) return;
	double b[6]; surfGetBounds(s, b);
	const double xf = (s->xfac != 0.0) ? s->xfac : 1.0;
	const double zs = s->zfac * s->ve;
	const double zf = (zs != 0.0) ? zs : 1.0;
	double zlo, zhi;
	const bool hasGrid = activeGridZRange(s, zlo, zhi);
	gmtvtk_reframe_z_h(s, b[0] / xf, b[1] / xf, b[2], b[3], b[4] / zf, b[5] / zf, hasGrid ? 0 : 1);
	s->viewBoundsOverride = false;     // reframe re-pins it; the "override" now IS the real content
}

// A NEWLY LOADED element has arrived in this window: make it the thing on display. ONE function for
// the whole "a new file/variable landed" transition, called by every load path so none of them can
// drift (SACRED_LAW.md derived-variable display law + group-uncheck law):
//
//   * every OTHER data layer (base surface, grids, images, meshes) is hidden -> its Scene Objects
//     group and every child row re-read the live actor visibility and show unchecked
//   * `name` is made visible and checked
//   * the axes cube + camera re-frame onto the NEW element's OWN bbox, X, Y *and* Z -- a new
//     quantity in its own units never wears the previous layer's frame
//   * Scene Objects is unfolded so the new row is actually on screen, never hidden in a closed group
//
// `hasBbox` == 0 skips only the re-frame (caller has no meaningful extent, e.g. a bare table).
// `keepMargin` follows gmtvtk_reframe_h's convention: 1 for images (keeps tick labels on screen),
// 0 for grids (edge-to-edge, the existing grid convention).
GMTVTK_API void gmtvtk_show_new_element_h(void *handle, const char *name,
                                          double x0, double x1, double y0, double y1,
                                          double z0, double z1, int hasBbox, int keepMargin) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return;
	const std::string keep = name ? name : "";
	for (auto &ex : s->extras) {
		const bool isKeep = (ex.name == keep);
		if (ex.actor) ex.actor->SetVisibility(isKeep ? 1 : 0);
		if (ex.drape) ex.drape->SetVisibility(isKeep ? 1 : 0);
	}
	if (surfProp(s) && s->surfName != keep) {         // the window's own base layer is a layer too
		surfSetVisibility(s, 0);
		if (s->drape) s->drape->SetVisibility(0);
	}
	// A MESH is a 3-D body: seen through the flat-2D top-down orthographic camera a cube is just a
	// square. Switch the window to the 3-D view it would have opened in on its own, through the ONE
	// 2D<->3D function (sceneSetFlat2D) — never inlined camera maths. Done BEFORE the re-frame, whose
	// camera fit must run against the view mode the element ends up being shown in.
	bool wantsPerspective = s->fvSolid && s->surfName == keep;
	for (auto &ex : s->extras) if (ex.name == keep && ex.isMesh) wantsPerspective = true;
	if (wantsPerspective && s->flat2d) sceneSetFlat2D(s, false);
	if (hasBbox) gmtvtk_reframe_z_h(handle, x0, x1, y0, y1, z0, z1, keepMargin);
	refreshGridColorbar(s);          // bar + hover readout follow whatever is now the visible layer
	rebuildSceneObjects(s);          // every row's checked state re-read from the live actors
	unfoldSceneObjects(s);           // ... and the panel actually open, so the new row is visible
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Add a POLYGON MESH (a VTK .vtp/.vtu surface, a GMTfv solid) into an EXISTING window as its own
// Scene Objects layer -- the mesh counterpart of gmtvtk_add_surface_h. Built with the SAME makeFvMesh
// the fv viewer uses, hung in the window's own scaled space (xfac, 1, zfac*ve) like every other extra
// so it tracks VE, and given a stacking rank off the same unified pile. Starts HIDDEN, exactly like a
// dropped grid does; the caller's gmtvtk_show_new_element_h is what puts it on screen.
// Returns 1 / 0.
GMTVTK_API int gmtvtk_add_mesh_h(void *handle, const double *xyz, int nv, const int *sides, int nfaces,
                                 const int *indices, const double *cz, const double *crgb, int ncolor,
                                 const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !xyz || nv <= 0 || !sides || !indices || nfaces <= 0) return 0;
	double zmin, zmax;
	auto pd = makeFvMesh(xyz, nv, sides, nfaces, indices, nullptr, nullptr, zmin, zmax);

	vtkSmartPointer<vtkScalarsToColors> lut;
	if (cz && crgb && ncolor > 0) {
		vtkNew<vtkColorTransferFunction> ctf;
		for (int i = 0; i < ncolor; ++i) ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
		lut = ctf;
	}
	else {
		vtkNew<vtkLookupTable> t;
		t->SetHueRange(0.667, 0.0); t->SetNumberOfTableValues(256); t->SetRampToLinear();
		t->SetTableRange(zmin, zmax); t->Build(); lut = t;
	}
	vtkNew<vtkPolyDataNormals> fn;                 // faceted normals: sharp solid edges, like view_fv
	fn->SetInputData(pd);
	fn->SplittingOn(); fn->SetFeatureAngle(30.0); fn->ConsistencyOn();
	vtkNew<vtkPolyDataMapper> map;
	map->SetInputConnection(fn->GetOutputPort());
	map->SetLookupTable(lut); map->SetScalarRange(zmin, zmax);
	map->ScalarVisibilityOn();

	ExtraObj ex;
	ex.isMesh = true;
	ex.actor = vtkSmartPointer<vtkActor>::New();
	ex.actor->SetMapper(map);
	ex.actor->GetProperty()->SetInterpolationToPBR();
	ex.actor->GetProperty()->SetMetallic(0.0);
	ex.actor->GetProperty()->SetRoughness(0.45);
	ex.actor->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	ex.actor->SetVisibility(0);
	s->ren->AddActor(ex.actor);
	ex.name = (name && name[0]) ? name : ("Mesh " + std::to_string((int)s->extras.size() + 1));
	ex.gstack = s->vecSeq++;                       // unified pile: newest layer lands on top
	ex.tag    = ++s->gridTagSeq;
	s->extras.push_back(ex);
	applyGridStacking(s);
	rebuildSceneObjects(s);
	s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
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
// `geographic` (!=0 -> x,y are lon,lat) sits in the SAME argument slot as gmtvtk_promote_surface_h's,
// so the two calls read identically at every call site. It is per-GRID, not per-window: a window
// built around a geographic parent can perfectly well be showing a CARTESIAN derived grid (a
// gravmag3d anomaly computed with the dialog's "Geographic" unchecked has x,y in metres), and the
// axis NAMES follow the ACTIVE grid's flag — see activeGridGeog/syncAxisNames.
GMTVTK_API int gmtvtk_add_surface_h(void *handle, const float *z, int nx, int ny,
									double x0, double x1, double y0, double y1, int geographic,
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
		applyNanColorToLut(lut, s->nanColor);   // paint this grid's NaN cells with the NaN fill colour
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
		// A newly ADDED grid starts UNCHECKED (hidden): this function stacks a new grid on top of
		// whatever the window already shows, and two grids visible/checked at once is never wanted —
		// the user can't tell anything happened (they overlap) and it's confusing besides. The
		// Scene Objects "Surface" row checkbox mirrors this actor's own VTK visibility, so this ALONE
		// makes the row start unchecked, for every caller of this shared function (RTP, IGRF grid,
		// nested transplant, dropped grids, …) — same operation, same behaviour, no per-caller flag.
		ex.actor->SetVisibility(0);
		s->ren->AddActor(ex.actor);
		// Make this dropped grid a FULL layer: keep its full-res z (readout source when it is the active
		// grid) and its own LUT + z range (so the colorbar can be retargeted to it). applyGridStacking()
		// below puts it on top -> refreshGridColorbar() makes it the active grid + shows its colorbar.
		ex.gridZ.assign(z, z + (size_t)nx * ny);
		ex.gnx = nx; ex.gny = ny;
		ex.gx0 = x0; ex.gx1 = x1; ex.gy0 = y0; ex.gy1 = y1;
		ex.zmin = zmin; ex.zmax = zmax; ex.lut = lut; ex.geog = geographic ? 1 : 0;
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
		sceneSetGridLayer(s, z, nx, ny, x0, x1, y0, y1);
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
		return gmtvtk_add_surface_h(handle, z, nx, ny, x0, x1, y0, y1, geographic,
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
	if (!gz)
		sceneSetGridLayer(s, z, nx, ny, x0, x1, y0, y1);

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

// Replace the window's BASE grid data IN PLACE (Grid Tools > Transplant 2nd grid, and its undo).
// Unlike gmtvtk_add_surface_h (which adds a NEW layer) this rebuilds the existing base surface from
// new z of the SAME region, keeping the user's camera, vertical exaggeration, gizmo, colormap-driving
// name, AND every extra/overlay/polygon (buildSceneContent's teardown removes only the base surface,
// axes and colorbar — see its prologue). Scaling (xfac/zfac/ve) is deliberately KEPT, not recomputed,
// so the saved camera stays valid and the data edit does not jump the view. `name` (may be empty ->
// keep current) drives the Scene Objects "Surface" row label.
GMTVTK_API int gmtvtk_replace_base_grid_h(void *handle, const float *z, int nx, int ny,
                                          double x0, double x1, double y0, double y1, int geographic,
                                          const double *cz, const double *crgb, int ncolor,
                                          const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !z || nx < 2 || ny < 2)
		return 0;

	// Snapshot the camera (world coords stay valid because xfac/zfac/ve are unchanged below).
	vtkCamera *cam = s->ren->GetActiveCamera();
	double cpos[3], cfoc[3], cvup[3];
	cam->GetPosition(cpos);  cam->GetFocalPoint(cfoc);  cam->GetViewUp(cvup);
	const double cpscale = cam->GetParallelScale();
	const int    cpar    = cam->GetParallelProjection();

	// New z range (for the colorbar) — but KEEP xfac/zfac/ve so the surface's world scaling is stable.
	double zmin = 1e30, zmax = -1e30;
	for (vtkIdType k = 0, ntot = (vtkIdType)nx * ny; k < ntot; ++k) {
		const float zz = z[k];
		if (!std::isnan(zz)) { if (zz < zmin) zmin = zz; if (zz > zmax) zmax = zz; }
	}
	if (zmin > zmax) { zmin = 0.0; zmax = 1.0; }
	s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1; s->zmin = zmin; s->zmax = zmax;
	s->imageOnly = false;
	if (name && name[0]) s->surfName = name;

	// Rebuild ONLY the base surface (tiled gz path). gz fills s->gridZ/gnx/gny/gx*/gd *internally.
	buildSceneContent(s, nullptr, x0, x1, y0, y1, cz, crgb, ncolor, nullptr, 0, 0, 0,
	                  /*edges=*/0, /*pointCloud=*/false, geographic, z, nx, ny, /*blankStart=*/false);

	// Rebuild the gizmo against the new surface (as promote does), then restore the camera so the
	// user's zoom / orientation / 2D-or-3D view is untouched by the data edit.
	disableGizmo(s);  s->giz = enableGizmo(s, 0.01);
	cam->SetPosition(cpos);  cam->SetFocalPoint(cfoc);  cam->SetViewUp(cvup);
	cam->SetParallelProjection(cpar);  if (cpar) cam->SetParallelScale(cpscale);
	if (s->giz) setGizmoVisible(*s->giz, !s->flat2d);   // flat-2D hides the gizmo, 3-D shows it

	applyVE(s);              // re-scale surface + extras + cube axes to the current VE and new bounds
	applyStacking(s);        // re-offset extras/vectors against the rebuilt base + refresh colorbar
	rebuildSceneObjects(s);
	applyShading(s);
	s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// Show a 2-D grid layer as a fast ILLUMINATED IMAGE (Mirone-style) — the cheap alternative to a
// warped 3-D surface for scrubbing the layers of a heavy 3-D cube. The relief is NOT triangulated:
// a single flat quad carries an nx*ny texture, so a layer switch is a texture repaint + Render, not
// a per-layer geometry rebuild. The full-res z stays in s->gridZ, so the coordinate readout still
// reports the true elevation (not a pixel colour) and overlays drape correctly. The colorbar is
// kept (this is a grid shown as an image, not a bare image).
//
// Shared tail of gmtvtk_show_layer_image_h (bakes the texture from a CPT + illumination) and
// gmtvtk_show_layer_rgba_h (the texture arrives ALREADY composited, e.g. Aquamoto's dry/wet blend).
// First call (or a grid-size / extent change) builds the flat drape scene; every later same-size
// call takes the FAST path: overwrite the texture bytes + s->gridZ, one Render. `cz/crgb/ncolor` is
// only the CPT the colourbar legend reflects (the surface itself is 100% covered by the drape, so
// this never colours a visible pixel). `isCustom` marks the texture as host-composited so the
// generic relight/hi-res-detail paths (rebakeLayerImage/refineLayerDetail, 40_shading.cpp) never
// try to regenerate it from gridZ+cpt and stomp it. Returns 1 on success, 0 on a dead/invalid scene.
static int showLayerImageTail(Scene *s, const unsigned char *rgba, int txW, int txH,
                              const float *z, int nx, int ny,
                              double x0, double x1, double y0, double y1, int geographic,
                              const double *cz, const double *crgb, int ncolor, const char *name,
                              bool isCustom) {
	const double dx = (nx > 1) ? (x1 - x0) / (nx - 1) : 0.0;
	const double dy = (ny > 1) ? (y1 - y0) / (ny - 1) : 0.0;

	// ---- FAST path: same window, same grid size + extent -> just repaint the drape texture ----
	if (s->layerImgMode && s->drape && !s->emptyStart &&
	    s->gnx == nx && s->gny == ny && s->layerTexW == txW && s->layerTexH == txH &&
	    s->gx0 == x0 && s->gx1 == x1 && s->gy0 == y0 && s->gy1 == y1) {
		vtkTexture *tx = s->drape->GetTexture();
		vtkImageData *id = tx ? vtkImageData::SafeDownCast(tx->GetInput()) : nullptr;
		int dims[3] = { 0, 0, 0 }; if (id) id->GetDimensions(dims);
		if (id && dims[0] == txW && dims[1] == txH) {
			memcpy(id->GetScalarPointer(), rgba, (size_t)txW * txH * 4);
			id->Modified(); tx->Modified();
			s->gridZ.assign(z, z + (size_t)nx * ny);     // hover now reads the NEW layer's z
			s->zmin = cz[0]; s->zmax = cz[ncolor - 1];
			if (name && name[0]) s->surfName = name;
			s->customLayerTexture = isCustom;
			// Aquamoto: keep the UNSHADED composite as the shading base, then relight it through the
			// SHARED engine (bakeAquaShade -> applyReliefShade), so this slice is hillshaded like the
			// previous one and the Shading dock keeps driving it. gridZ (above) is this slice's stage.
			if (isCustom) {
				s->aquaBaseRGBA.assign(rgba, rgba + (size_t)txW * txH * 4);
				bakeAquaShade(s);
			}

			// Recolour + retarget the colorbar to THIS layer -- was previously only s->zmin/zmax being
			// updated silently, leaving the on-screen bar (ticks, labels, LUT) frozen on whatever layer
			// built it first (e.g. every later cube-layer switch or Aquamoto slice change never moved
			// the bar). Mutate s->surfLut's CTF nodes in place (same trick as gmtvtk_set_cpt) so the
			// surface mapper stays in sync too, then rebuild the bar's ticks/labels for the new range.
			if (cz && crgb && ncolor > 1) {
				vtkColorTransferFunction *ctf = vtkColorTransferFunction::SafeDownCast(s->surfLut);
				if (ctf) {
					ctf->RemoveAllPoints();
					for (int i = 0; i < ncolor; ++i)
						ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
				}
				s->baseCz.assign(cz, cz + ncolor);
				s->baseCrgb.assign(crgb, crgb + 3 * ncolor);
			}
			destroyColorbar(s);
			// For an Aquamoto layer, `bar` is specifically the WATER side -- only rebuild/show it while
			// aquaShowWater is true (the Shade Water/Land radio). Without this gate, every slice repaint
			// (fireSlice fires right after the radio toggle) unconditionally redrew the water bar and
			// undid whatever refreshGridColorbar had just done to switch to the land bar.
			if (s->surfShowBar && (!s->customLayerTexture || s->aquaShowWater))
				buildColorbar(s, s->surfLut, s->zmin, s->zmax);

			invalidateLayerDetail(s);   // the zoom detail tile is for the OLD layer -> refresh on settle
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
			return 1;
		}
	}

	// ---- FULL build: flat draped scene (first layer, or grid size/extent changed) ----
	const bool fromEmpty = s->emptyStart;
	const double zmin = cz[0], zmax = cz[ncolor - 1];
	s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1; s->zmin = zmin; s->zmax = zmax;
	if (name && name[0]) s->surfName = name;

	// Scale: from the real extent on a launcher promote; kept (with a camera snapshot) on an in-place
	// rebuild so the view does not jump.
	vtkCamera *cam = s->ren->GetActiveCamera();
	double cpos[3], cfoc[3], cvup[3], cpscale = 1.0; int cpar = 0;
	if (fromEmpty) {
		double xfac, zfac, ve0;
		computeScales(geographic, x0, x1, y0, y1, zmin, zmax, xfac, zfac, ve0);
		s->xfac = xfac; s->zfac = zfac; s->ve = ve0;
	}
	else {
		cam->GetPosition(cpos); cam->GetFocalPoint(cfoc); cam->GetViewUp(cvup);
		cpscale = cam->GetParallelScale(); cpar = cam->GetParallelProjection();
	}

	// Flat 2x2 quad (z=0) with tcoords; the nx*ny hillshade rides on it as the drape texture.
	const float flatz[4] = { 0.f, 0.f, 0.f, 0.f };
	double zlo = 0, zhi = 0;
	vtkSmartPointer<vtkPolyData> pd = makeGridFromArray(flatz, 2, 2, x0, x1, y0, y1, zlo, zhi,
	                                                    /*triangulate=*/true, /*wantTC=*/true);
	s->imageOnly = false;          // keep the colorbar + z readout (a grid shown as an image, not a bare image)
	buildSceneContent(s, pd, x0, x1, y0, y1, cz, crgb, ncolor, rgba, txW, txH, 4,
	                  /*edges=*/0, /*pointCloud=*/false, geographic, nullptr, 0, 0, /*blankStart=*/false);

	// Full-res data layer for the hover/coordinate readout (the single-actor drape path does NOT
	// populate gridZ; only the tiled path does — so set it here, and re-point the active-grid routing).
	sceneSetGridLayer(s, z, nx, ny, x0, x1, y0, y1, dx, dy);
	s->actZ = &s->gridZ; s->actNx = nx; s->actNy = ny;
	s->actX0 = x0; s->actX1 = x1; s->actY0 = y0; s->actY1 = y1;
	s->layerImgMode = true;
	s->layerTexW = txW; s->layerTexH = txH;
	s->customLayerTexture = isCustom;
	// Aquamoto: stash the UNSHADED composite as the shading base (applyShading below -> bakeAquaShade
	// relights it through the shared engine). Clear it for a plain baked layer so bakeAquaShade stays a
	// no-op there.
	if (isCustom) {
		s->aquaBaseRGBA.assign(rgba, rgba + (size_t)txW * txH * 4);
		// Seed each side's OWN light from the current dock ONCE (both start equally lit); thereafter each
		// side is only ever updated when it is the selected side, so they stay independent.
		if (!s->aquaWaterShade.valid) s->aquaWaterShade = snapshotShade(s);
		if (!s->aquaLandShade.valid)  s->aquaLandShade  = snapshotShade(s);
	}
	else          { s->aquaBaseRGBA.clear(); s->aquaBathyZ.clear(); }
	s->emptyStart = false;
	if (s->axes) { s->axes->SetZAxisVisibility(0); s->axes->DrawZGridlinesOff(); }   // a 2-D map: no Z axis

	// Rebuild the gizmo against the new bounds, then either open flat-2D top-down (launcher promote,
	// the way a normal grid opens) or restore the previous view (in-place rebuild).
	disableGizmo(s); s->giz = enableGizmo(s, 0.01);
	if (fromEmpty) {
		s->flat2d = false; sceneSetFlat2D(s, true);
		// The empty launcher created the Shading dock HIDDEN (no body to light back then). There is a
		// surface now, so reveal it FOLDED to the side strip — exactly as promote_surface_h does, so a
		// cube layer gets the same live Shading controls (relight via rebakeLayerImage) as any grid.
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
	else {
		cam->SetPosition(cpos); cam->SetFocalPoint(cfoc); cam->SetViewUp(cvup);
		cam->SetParallelProjection(cpar); if (cpar) cam->SetParallelScale(cpscale);
	}
	if (s->giz) setGizmoVisible(*s->giz, !s->flat2d);

	// Hi-res zoom detail: a settle-debounced timer re-bakes a sharp tile of the visible region, driven
	// by a camera-modified observer (see refineLayerDetail / onLayerCamera, 40_shading.cpp).
	if (!s->layerDetailTimer) {
		s->layerDetailTimer = new QTimer(s->win);
		s->layerDetailTimer->setSingleShot(true);
		QObject::connect(s->layerDetailTimer, &QTimer::timeout, s->win, [s]() { refineLayerDetail(s); });
	}
	if (s->layerCamCmd && s->ren->GetActiveCamera()) s->ren->GetActiveCamera()->RemoveObserver(s->layerCamCmd);
	s->layerCamCmd = vtkSmartPointer<vtkCallbackCommand>::New();
	s->layerCamCmd->SetCallback(onLayerCamera);
	s->layerCamCmd->SetClientData(s);
	s->ren->GetActiveCamera()->AddObserver(vtkCommand::ModifiedEvent, s->layerCamCmd);

	applyVE(s);
	applyStacking(s);
	rebuildSceneObjects(s);
	applyShading(s);
	s->ren->ResetCameraClippingRange();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

GMTVTK_API int gmtvtk_show_layer_image_h(void *handle, const float *z, int nx, int ny,
                                         double x0, double x1, double y0, double y1, int geographic,
                                         const double *cz, const double *crgb, int ncolor,
                                         const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !z || nx < 2 || ny < 2 || !cz || !crgb || ncolor < 2)
		return 0;

	// CPT for baking (a vtkColorTransferFunction over the exact GMT control nodes).
	vtkNew<vtkColorTransferFunction> ctf;
	for (int i = 0; i < ncolor; ++i)
		ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);

	// A cube opens as an illuminated relief map: default a FRESH cube window to the grdimage hillshade
	// (done before the bake so the very first texture is already shaded). The Shading dock then switches
	// style (Lambert / off) or moves the sun and relights live via rebakeLayerImage.
	if (s->emptyStart && !s->layerImgMode) { s->useHillshade = true; s->hillGrd = true; }

	const double dx = (nx > 1) ? (x1 - x0) / (nx - 1) : 0.0;
	const double dy = (ny > 1) ? (y1 - y0) / (ny - 1) : 0.0;
	int txW, txH; layerTexSize(nx, ny, txW, txH);      // capped texture size (subsample a heavy cube)
	std::vector<unsigned char> rgba;
	bakeLayerRGBA(s, z, nx, ny, x0, y0, dx, dy, ctf, cz[0], cz[ncolor - 1],
	              x0, x1, y0, y1, txW, txH, rgba);     // base texture = the whole extent

	return showLayerImageTail(s, rgba.data(), txW, txH, z, nx, ny, x0, x1, y0, y1, geographic,
	                          cz, crgb, ncolor, name, /*isCustom=*/false);
}

// Aquamoto-style variant: the caller (Julia) hands over an ALREADY-COMPOSITED RGBA texture (e.g.
// a dry-land / wet-water blend that no single CPT could produce) instead of a z array to bake —
// same flat-quad "first call builds, later same-size calls just repaint" scene as
// gmtvtk_show_layer_image_h, minus the internal CPT bake. `rgba` is nx*ny*4 bytes (no subsampling —
// the caller already sized it to the display resolution it wants). `zhover` (nx*ny floats, e.g. the
// water stage) feeds the coordinate readout the same way gridZ does for the CPT path. `cz/crgb/
// ncolor` is only the colourbar legend's scale (e.g. the water colour ramp) — it never colours a
// visible pixel, since the drape covers the surface 100%.
GMTVTK_API int gmtvtk_show_layer_rgba_h(void *handle, const unsigned char *rgba, int nx, int ny,
                                        double x0, double x1, double y0, double y1, int geographic,
                                        const double *cz, const double *crgb, int ncolor,
                                        const float *zhover, const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !rgba || nx < 2 || ny < 2 || !cz || !crgb || ncolor < 2 || !zhover)
		return 0;
	// A FRESH tsunami (no base composite pushed yet): WATER opens FLAT (the host-composited 256-colour
	// :polar image, Mirone-style) -- hillshade relighting saturates the water's steep-gradient normals
	// and washes the palette toward white/dark, a 2-colour collapse of an image that is really 256.
	// LAND opens SHADED (grdimage hillshade) by default -- AquaSideShade's own defaults (useHillshade=
	// true, hillGrd=true) already say so; only WATER needs an explicit flat override. Each side gets
	// its OWN valid snapshot here so bakeAquaShade never falls back to the live dock state for either.
	if (s->aquaBaseRGBA.empty()) {
		s->aquaWaterShade = AquaSideShade{};
		s->aquaWaterShade.valid = true; s->aquaWaterShade.useHillshade = false; s->aquaWaterShade.hillGrd = false;
		s->aquaWaterShade.litBake = false;
		s->aquaLandShade = AquaSideShade{};
		s->aquaLandShade.valid = true;                    // useHillshade/hillGrd default true -> shaded
		// Live dock state mirrors whichever side aquaShadeSelWater currently edits, so the Shading
		// dock checkboxes reflect the truth the first time it's opened on this file.
		const AquaSideShade &live = s->aquaShadeSelWater ? s->aquaWaterShade : s->aquaLandShade;
		s->useHillshade = live.useHillshade; s->hillGrd = live.hillGrd; s->litBake = live.litBake;
	}
	return showLayerImageTail(s, rgba, nx, ny, zhover, nx, ny, x0, x1, y0, y1, geographic,
	                          cz, crgb, ncolor, name, /*isCustom=*/true);
}

// Build/replace Aquamoto's persistent LAND colorbar (the bathymetry range + :geo ramp) once, at
// file-open time -- it never changes per slice, unlike the water bar showLayerImageTail already
// refreshes every call. Visibility is decided by refreshGridColorbar (aquaShowWater / aquaLandShowBar),
// never forced on here. Returns 1 on success, 0 on a dead scene or bad CPT.
GMTVTK_API int gmtvtk_aqua_set_land_cpt_h(void *handle, const double *cz, const double *crgb, int ncolor,
                                          double lo, double hi) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !cz || !crgb || ncolor < 2) return 0;
	vtkNew<vtkColorTransferFunction> ctf;
	for (int i = 0; i < ncolor; ++i) ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
	s->aquaLandLut = ctf;
	buildAquaLandColorbar(s, s->aquaLandLut, lo, hi);
	setAquaLandColorbarVisible(s, s->aquaLandShowBar && !s->aquaShowWater);
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return 1;
}

// Hand the Aquamoto layer its static BATHYMETRY (the LAND surface for hillshading), column-major
// z[ix*ny+iy] exactly like gridZ (the per-slice stage = WATER surface). Stored once at file-open;
// bakeAquaShade then shades LAND pixels from this and WATER pixels from the live stage, both through
// the SAME applyReliefShade the rest of the app uses. Relights immediately. Returns 1 on success.
GMTVTK_API int gmtvtk_aqua_set_bathy_h(void *handle, const float *z, int nx, int ny) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !z || nx < 2 || ny < 2) return 0;
	s->aquaBathyZ.assign(z, z + (size_t)nx * ny);
	bakeAquaShade(s);   // land surface now available -> relight through the shared engine
	return 1;
}

// Display label for the composited water/land surface's OWN Scene Objects group -- the active
// time-varying quantity variable's real name, whatever the file itself calls it (no assumed
// naming). The outer per-file wrapper group (rebuildSceneObjects) uses surfName (the file name)
// instead, so the two never collide. Purely cosmetic: Save/session still key off surfName/name.
// Returns 1 on success, 0 on a dead scene.
GMTVTK_API int gmtvtk_aqua_set_var_label_h(void *handle, const char *label) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s)) return 0;
	s->aquaVarLabel = label ? label : "";
	rebuildSceneObjects(s);
	return 1;
}

// Open a VTK-format file (.vtp/.vti/.vtr/.vts/.vtu/.vtm/.vtk/.vtkhdf and the .pvt* parallel forms)
// INTO this window. Neither GMT nor GDAL can parse these, so Julia's `_open_spec_into` catches the
// extension and routes it here instead of `gmtread` (src/drop.jl); the reading and the classification
// happen in 87_vtkio.cpp, and the result is handed to the EXACT builders every other data source
// already uses (SACRED_LAW.md -- one display path per kind of object, never a VTK-only one):
//
//   grid-shaped structured data -> gmtvtk_promote_surface_h / gmtvtk_add_surface_h
//   polygon cells               -> gmtvtk_promote_fv_h / gmtvtk_add_mesh_h
//   line cells / points only    -> gmtvtk_add_overlay_h (over a blank frame if the window is empty)
//
// EVERY kind lands in THIS window -- a VTK file never spawns a second one. Whatever it turned out to
// hold is then adopted through gmtvtk_show_new_element_h: shown, everything previously displayed
// hidden/unchecked, axes + camera re-framed onto the new element's own X/Y/Z extent.
//
// `promote` != 0 means the receiving window is a bare launcher and may be promoted in place.
// Returns 1 (loaded), 0 (failed -> `errbuf` says why).
GMTVTK_API int gmtvtk_open_vtk_h(void *handle, const char *path, const char *name, int promote,
                                 char *errbuf, int errcap) {
	auto fail = [errbuf, errcap](const std::string &msg) {
		if (errbuf && errcap > 0) {
			const int n = std::min((int)msg.size(), errcap - 1);
			std::memcpy(errbuf, msg.c_str(), n);
			errbuf[n] = '\0';
		}
		return 0;
	};
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s))     return fail("window is closed");
	if (!path || !path[0])  return fail("no file path");

	VtkIoLoad L;
	std::string err;
	if (!vtkioLoad(path, L, err)) return fail(err.empty() ? "could not read the VTK file" : err);
	const char *nm = (name && name[0]) ? name : "";
	if (s->win) s->win->statusBar()->showMessage(QString::fromStdString("VTK: " + L.detail), 5000);
	// File > Recent Files is stamped HERE, not by the Julia caller: only now is it known whether the
	// file held a grid (category 0) or a mesh / vector dataset (category 2).
	gmtvtk_add_recent(QFileInfo(QString::fromUtf8(path)).absoluteFilePath().toUtf8().constData(),
	                  L.kind == VtkIoLoad::Grid ? 0 : 2);

	if (L.kind == VtkIoLoad::Grid) {
		const int ok = promote
			? gmtvtk_promote_surface_h(handle, L.z.data(), L.nx, L.ny, L.x0, L.x1, L.y0, L.y1, 0,
			                           nullptr, nullptr, 0, nullptr, 0, 0, 0, 0, nm)
			: gmtvtk_add_surface_h(handle, L.z.data(), L.nx, L.ny, L.x0, L.x1, L.y0, L.y1, 0,
			                       nullptr, nullptr, 0, nullptr, 0, 0, 0, 0, nm);
		if (!ok) return fail("the grid could not be added to the window");
		double zlo = L.z.empty() ? 0.0 : (double)L.z[0], zhi = zlo;
		for (float v : L.z) { if (v < zlo) zlo = v; if (v > zhi) zhi = v; }
		gmtvtk_show_new_element_h(handle, nm, L.x0, L.x1, L.y0, L.y1, zlo, zhi, 1, 0);
		return 1;
	}

	if (L.kind == VtkIoLoad::Mesh) {
		const int nv     = (int)(L.xyz.size() / 3);
		const int nfaces = (int)L.sides.size();
		// A mesh loads IN PLACE like everything else -- as its own Scene Objects layer of THIS window,
		// never a second window. An empty launcher is promoted (the mesh becomes the window's base
		// surface); a window already holding data gets the mesh as an extra layer.
		const int ok = promote
			? gmtvtk_promote_fv_h(handle, L.xyz.data(), nv, L.sides.data(), nfaces, L.indices.data(),
			                      nullptr, nullptr, nullptr, nullptr, 0,
			                      L.x0, L.x1, L.y0, L.y1, L.z0, L.z1, 0, 1.0, 0, nm)
			: gmtvtk_add_mesh_h(handle, L.xyz.data(), nv, L.sides.data(), nfaces, L.indices.data(),
			                    nullptr, nullptr, 0, nm);
		if (!ok) return fail("the mesh could not be displayed");
		gmtvtk_show_new_element_h(handle, nm, L.x0, L.x1, L.y0, L.y1, L.z0, L.z1, 1, 0);
		return 1;
	}

	// Lines / points are OVERLAYS, and an overlay needs a frame to live in. An empty launcher gets the
	// same blank 2x2 hidden base surface the SHAPENC vector path already promotes it with (drop.jl's
	// _add_shapenc_bounded) -- one way to frame vector data, not a second.
	if (promote) {
		const float blank[4] = { 0.f, 0.f, 0.f, 0.f };
		const double px = (L.x1 - L.x0) * 0.05, py = (L.y1 - L.y0) * 0.05;
		gmtvtk_promote_surface_h(handle, blank, 2, 2, L.x0 - px, L.x1 + px, L.y0 - py, L.y1 + py, 0,
		                         nullptr, nullptr, 0, nullptr, 0, 0, 0, 1, "");
		gmtvtk_hide_surface(handle);
	}
	const int npts = (int)(L.xyz.size() / 3);
	const int mode = (L.kind == VtkIoLoad::Lines) ? 1 : 0;
	const int nseg = (L.kind == VtkIoLoad::Lines) ? (int)L.segoff.size() - 1 : 0;
	const int ok = gmtvtk_add_overlay_h(handle, L.xyz.data(), npts,
	                                    mode ? L.segoff.data() : nullptr, nseg,
	                                    mode, 0.9, 0.35, 0.1, 0.0, 0.0, nm);
	if (!ok) return fail("the overlay could not be added to the window");
	// An overlay is drawn ON TOP of the rasters rather than instead of them, but the rule is the same:
	// the file that just arrived is what the window shows, framed on ITS extent. The overlay itself is
	// not in s->extras, so passing its name simply hides every raster layer.
	gmtvtk_show_new_element_h(handle, nm, L.x0, L.x1, L.y0, L.y1, L.z0, L.z1, 1, 0);
	return 1;
}

// Remove an EXTRA grid (a dropped/added grid surface, addressed by its Scene Objects name) IN PLACE,
// same teardown as the grid row's "Remove" menu item. Used by the "layerN" transplant path:
// Julia removes the blank grid, then re-adds a FILLED grid under the SAME name. Returns 1 if removed,
// 0 if no extra grid carried that name (or the window is dead).
GMTVTK_API int gmtvtk_remove_grid_h(void *handle, const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !name || !name[0]) return 0;
	const std::string want = name;
	for (size_t i = 0; i < s->extras.size(); ++i) {
		if (s->extras[i].isImage || s->extras[i].name != want) continue;
		sceneRemoveExtraAt(s, i);                // THE removal (50_scene.cpp), same as the row's Remove
		return 1;
	}
	return 0;
}

// Remove an EXTRA image by its Scene Objects name — the image twin of gmtvtk_remove_grid_h above,
// through the SAME `sceneRemoveExtraAt` the image row's own "Remove" uses. Returns 1 if removed, 0 if
// no extra image carried that name (or the window is dead).
GMTVTK_API int gmtvtk_remove_image_h(void *handle, const char *name) {
	Scene *s = static_cast<Scene*>(handle);
	if (!sceneAlive(s) || !name || !name[0]) return 0;
	const std::string want = name;
	for (size_t i = 0; i < s->extras.size(); ++i) {
		if (!s->extras[i].isImage || s->extras[i].name != want) continue;
		sceneRemoveExtraAt(s, i);
		return 1;
	}
	return 0;
}

// Julia toggles whether a transplant undo is currently available (set true after a transplant, false
// after it is undone). Gates the "Undo transplant" rectangle-menu entry so it vanishes once applied.
GMTVTK_API void gmtvtk_set_transplant_undo(void *handle, int on) {
	Scene *s = static_cast<Scene*>(handle);
	if (sceneAlive(s)) s->transplantUndoAvail = (on != 0);
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

// Add a screen-constant "+" cross at data point (x,y) -- e.g. the Tide tool's "Now" indicator.
// sizePx is the on-screen half-arm-length target (both arms always equal, independent of X/Y data
// scale, recomputed every render so it survives zoom/pan/resize); widthPx is the stroke thickness,
// a real (not VTK-marker-hijacked) line width. Returns the series index, or -1 on a dead handle.
GMTVTK_API int gmtvtk_xyplot_add_now_cross(void *handle, double x, double y,
                                           double r, double g, double b,
                                           double sizePx, double widthPx, const char *name) {
	return xyAddNowCross(static_cast<XYPlot*>(handle), x, y, r, g, b, sizePx, widthPx, name);
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

// Tell an X,Y plot window which 3-D viewer it belongs to. That viewer's Scene Objects dock is where
// the plot PARKS as a handle when its window is closed with the X (instead of being destroyed):
// double-click the row to bring it back, or use its menu to delete it for good. Without an owner
// there is no dock to park in and the X closes the window for real, as before. Call right after
// gmtvtk_xyplot_open when the plot was opened from a viewer window.
GMTVTK_API void gmtvtk_xyplot_set_owner(void *handle, void *scene) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p)) return;
	Scene *s = static_cast<Scene*>(scene);
	p->owner = (s && sceneAlive(s)) ? s : nullptr;
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
// drag tool runs): fit ln(power) vs wavenumber, return depth = |slope|/(4Ï€)·unit in metres. Returns
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

// Set (or clear, with null/empty) the rich-text header strip above an X,Y plot window's chart —
// e.g. the Tide tool's "Next High Tide … / Time now … / Next Low Tide …" lines. HTML-ish rich
// text (Qt::RichText): <b>, <br>, and inline color spans all work. Hidden when empty.
GMTVTK_API void gmtvtk_xyplot_set_info(void *handle, const char *html) {
	XYPlot *p = static_cast<XYPlot*>(handle);
	if (!xyAlive(p) || !p->infoLabel)
		return;
	const bool has = html && html[0];
	p->infoLabel->setText(has ? QString::fromUtf8(html) : QString());
	p->infoLabel->setVisible(has);
	xyRebuildObjMgr(p);                    // (dis)appear in the Object Manager along with the text
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

// ============================================================================
//  gmtedit — the MGD77 track editor (67_gmtedit.cpp). Its opaque handle is a
//  GmtEdit* (NOT a Scene*), like the X,Y tool's. Julia owns the data: it reads
//  the cruise, computes the outlier/nav/despike numbers and writes the netCDF
//  back; this API is only how the window is filled and read back.
// ============================================================================

// Open an editor window (non-blocking; pump gmtvtk_process_events). `widthKm` is
// gmtedit's -L, the width of the displayed distance window. Returns the GmtEdit*.
GMTVTK_API void *gmtvtk_gmtedit_open(const char *title, double widthKm) {
	return buildGmtEdit(title, widthKm);
}

// Is an editor handle still live (its window open)? 1 = yes, 0 = closed/invalid.
GMTVTK_API int gmtvtk_gmtedit_is_alive(void *handle) {
	return geAlive(static_cast<GmtEdit*>(handle)) ? 1 : 0;
}

// Close an editor window programmatically.
GMTVTK_API void gmtvtk_gmtedit_close(void *handle) {
	GmtEdit *s = static_cast<GmtEdit*>(handle);
	if (geAlive(s) && s->win) s->win->close();
}

// Bring an editor window to the front.
GMTVTK_API void gmtvtk_gmtedit_raise(void *handle) {
	GmtEdit *s = static_cast<GmtEdit*>(handle);
	if (geAlive(s) && s->win) { s->win->raise(); s->win->activateWindow(); }
}

// Register the one Julia callback the editor talks back through, fn(edit, action, arg).
// See JuliaGmtEditFn (67_gmtedit.cpp) for the action list. Pass nullptr to detach.
GMTVTK_API void gmtvtk_set_gmtedit_callback(JuliaGmtEditFn fn) {
	g_juliaGmtEdit = fn;
}

// Window title (Julia sets it to "gmtedit <cruise>" after a load).
GMTVTK_API void gmtvtk_gmtedit_set_title(void *handle, const char *title) {
	GmtEdit *s = static_cast<GmtEdit*>(handle);
	if (geAlive(s) && s->win && title) s->win->setWindowTitle(QString::fromUtf8(title));
}

// The file's plottable variable names, comma-separated (the context-menu submenus).
GMTVTK_API void gmtvtk_gmtedit_set_varlist(void *handle, const char *csv) {
	geSetVarList(static_cast<GmtEdit*>(handle), csv);
}

// Attach the 3-D viewer window (a Scene*) this editor was opened from — gmtedit.m's hMirAxes. It
// enables the toolbar's link tool, which sends the clicked record to that window as a marker at
// its own lon/lat. Pass nullptr to detach.
GMTVTK_API void gmtvtk_gmtedit_set_parent(void *handle, void *scene) {
	geSetParent(static_cast<GmtEdit*>(handle), scene);
}

// The shared abscissa: `n` values, `isDist` != 0 when they are km along track (else
// record numbers). Resets the scroll bar range and any detached segment.
GMTVTK_API void gmtvtk_gmtedit_set_x(void *handle, const double *x, int n, int isDist) {
	geSetX(static_cast<GmtEdit*>(handle), x, n, isDist != 0);
}

// Fill one panel (slot 0..2) with a variable's values plus its name and axis title.
GMTVTK_API void gmtvtk_gmtedit_set_channel(void *handle, int slot, const char *var,
                                            const char *label, const double *y, int n) {
	geSetChannel(static_cast<GmtEdit*>(handle), slot, var, label, y, n);
}

// Replace one panel's red flags (the outlier detector / nav filter results).
GMTVTK_API void gmtvtk_gmtedit_set_flags(void *handle, int slot, const int *flags, int n) {
	geSetFlags(static_cast<GmtEdit*>(handle), slot, flags, n);
}

// Read one panel's red flags back (1 = flagged). Returns the count written, 0 if the
// buffer is too small or the handle is dead.
GMTVTK_API int gmtvtk_gmtedit_get_flags(void *handle, int slot, int *out, int n) {
	return geGetFlags(static_cast<GmtEdit*>(handle), slot, out, n);
}

// Read one panel's CURRENT values back (despikes and dragged segments included), in the
// same record order as gmtvtk_gmtedit_get_flags. Returns the count written.
GMTVTK_API int gmtvtk_gmtedit_get_channel(void *handle, int slot, double *out, int n) {
	return geGetChannel(static_cast<GmtEdit*>(handle), slot, out, n);
}

// Records currently loaded (0 when the window is empty).
GMTVTK_API int gmtvtk_gmtedit_npoints(void *handle) {
	GmtEdit *s = static_cast<GmtEdit*>(handle);
	return geAlive(s) ? s->n : 0;
}

// Move one point onto a new value (the despike result).
GMTVTK_API void gmtvtk_gmtedit_set_point(void *handle, int slot, int idx, double y) {
	geSetPoint(static_cast<GmtEdit*>(handle), slot, idx, y);
}

// Overlay an extra curve on one panel (a grid sampled along track, or another variable).
GMTVTK_API void gmtvtk_gmtedit_add_overlay(void *handle, int slot, const double *x, const double *y,
                                            int n, const char *name, double r, double g, double b,
                                            double width) {
	geAddOverlay(static_cast<GmtEdit*>(handle), slot, x, y, n, name, r, g, b, width);
}

// gmtedit's -P: draw a vertical marker at `x` and open the display centred on it.
GMTVTK_API void gmtvtk_gmtedit_set_mark(void *handle, double x) {
	geSetMark(static_cast<GmtEdit*>(handle), x);
}

// Write the smoothing parameter csaps itself would choose into the outlier dialog's
// "Smoothing parameter (p)" box (the dialog asks for it whenever the channel changes).
GMTVTK_API void gmtvtk_gmtedit_set_autop(void *handle, int slot, double p) {
	GmtEdit *s = static_cast<GmtEdit*>(handle);
	(void)slot;
	if (geAlive(s) && s->outlierP)
		s->outlierP->setText(QString::number(p, 'g', 8));
}

// Show the nav filter's hit count in its own title, as gmtedit.m's "(found %d)" does.
GMTVTK_API void gmtvtk_gmtedit_set_navfound(void *handle, int nfound) {
	GmtEdit *s = static_cast<GmtEdit*>(handle);
	if (geAlive(s) && s->navDlg)
		s->navDlg->setWindowTitle(QString("Speed and Slope filter (found %1)").arg(nfound));
}

// Append one line to the editor's collapsible message panel; `isError` pops it open.
GMTVTK_API void gmtvtk_gmtedit_log(void *handle, const char *msg, int isError) {
	geLog(static_cast<GmtEdit*>(handle), QString::fromUtf8(msg ? msg : ""), isError != 0);
}

// Pop a read-only report window (Cruise Info, and the warnings gmtedit.m puts in a warndlg).
GMTVTK_API void gmtvtk_gmtedit_message(void *handle, const char *title, const char *text) {
	geShowMessage(static_cast<GmtEdit*>(handle), title, text);
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
