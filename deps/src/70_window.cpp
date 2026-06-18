// ===== tiled-LOD pyramid: build + per-frame screen-space-error refinement ==================
// Quadtree over the full grid; each node renders its region at a stride chosen so its sampled span
// is <= ~512 (leaf = stride 1 = full res). Per camera move, refineNode keeps each visible branch at
// the coarsest LOD whose node gap still projects to <= tau pixels, building tile actors lazily and
// LRU-evicting offscreen ones past a byte budget. Only the data layer (Scene::gridZ) is always
// resident; render geometry is bounded regardless of grid size.

static QuadNode* buildQuadNode(int i0, int i1, int j0, int j1, int level,
							   double x0, double dx, double y0, double dy) {
	QuadNode* n = new QuadNode();
	n->level = level; n->i0 = i0; n->i1 = i1; n->j0 = j0; n->j1 = j1;
	const int w = i1 - i0, h = j1 - j0;
	int step = 1; while (w / step > 512 || h / step > 512) step *= 2;   // sampled span <= ~513
	n->step = step;
	n->cx = x0 + 0.5 * (i0 + i1) * dx;
	n->cy = y0 + 0.5 * (j0 + j1) * dy;
	n->worldSpacing = std::max(std::abs(dx), std::abs(dy)) * step;
	const int im = (i0 + i1) / 2, jm = (j0 + j1) / 2;
	if (step == 1 || im <= i0 || jm <= j0) { n->leaf = true; n->step = 1; return n; }
	n->leaf = false;
	n->child[0] = buildQuadNode(i0, im, j0, jm, level+1, x0,dx,y0,dy);
	n->child[1] = buildQuadNode(im, i1, j0, jm, level+1, x0,dx,y0,dy);
	n->child[2] = buildQuadNode(i0, im, jm, j1, level+1, x0,dx,y0,dy);
	n->child[3] = buildQuadNode(im, i1, jm, j1, level+1, x0,dx,y0,dy);
	return n;
}

static void ensureNodeActor(Scene* s, QuadNode* n) {
	if (n->actor) return;
	auto tpd = makeGridTile(s->gridZ.data(), s->gnx, s->gny,
							n->i0, n->i1, n->j0, n->j1, s->gx0, s->gdx, s->gy0, s->gdy, n->step);
	vtkNew<vtkPolyDataMapper> m; m->SetInputData(tpd);
	m->SetLookupTable(s->surfLut); m->SetScalarRange(s->zmin, s->zmax);
	if (s->surfCtfRange) m->UseLookupTableScalarRangeOn();
	m->ScalarVisibilityOn(); m->InterpolateScalarsBeforeMappingOn();
	auto a = vtkSmartPointer<vtkActor>::New(); a->SetMapper(m);
	a->GetProperty()->SetInterpolationToPBR();
	a->GetProperty()->SetMetallic(0.0); a->GetProperty()->SetRoughness(0.45);
	a->GetProperty()->SetEdgeColor(0.12, 0.12, 0.12); a->GetProperty()->SetLineWidth(1.0);
	a->GetProperty()->SetEdgeVisibility(s->surfEdges);
	const vtkIdType npts = tpd->GetPoints()->GetNumberOfPoints();
	const vtkIdType ncel = tpd->GetPolys()->GetNumberOfCells();
	n->bytes = (size_t)npts * (12 + 4 + 12) + (size_t)ncel * 20;   // pts + z + normal + quad ids
	n->actor = a;
	s->surfGroup->AddPart(a);
	s->tiles.push_back(a);
	s->lodResidentBytes += n->bytes;
}

static void dropNodeActor(Scene* s, QuadNode* n) {
	if (!n->actor) return;
	s->surfGroup->RemovePart(n->actor);
	for (size_t k = 0; k < s->tiles.size(); ++k)
		if (s->tiles[k] == n->actor) { s->tiles.erase(s->tiles.begin() + k); break; }
	s->lodResidentBytes = (s->lodResidentBytes >= n->bytes) ? s->lodResidentBytes - n->bytes : 0;
	n->actor = nullptr; n->bytes = 0;
}

static void dropSubtree(Scene* s, QuadNode* n) {
	if (!n) return;
	dropNodeActor(s, n);
	for (int k = 0; k < 4; ++k) dropSubtree(s, n->child[k]);
}

static void collectResident(QuadNode* n, std::vector<QuadNode*>& out) {
	if (!n) return;
	if (n->actor) out.push_back(n);
	for (int k = 0; k < 4; ++k) collectResident(n->child[k], out);
}

static void evictLRU(Scene* s) {
	std::vector<QuadNode*> res; collectResident(s->quadRoot, res);
	std::sort(res.begin(), res.end(), [](QuadNode* a, QuadNode* b){ return a->lastUsed < b->lastUsed; });
	for (QuadNode* n : res) {
		if (s->lodResidentBytes <= s->lodBudgetBytes) break;
		if (n->lastUsed == s->lodFrame) continue;   // never evict a tile drawn this frame
		dropNodeActor(s, n);
	}
}

static void refineNode(Scene* s, QuadNode* n, vtkCamera* cam, const double camPos[3],
					   double vpH, double tanHalfFov, double parScale, bool parallel, double tau) {
	// node centre in SCALED world (the assembly applies xfac on X, zfac*ve on Z)
	const double zmid = 0.5 * (s->zmin + s->zmax) * s->zfac * s->ve;
	const double pc[3] = { n->cx * s->xfac, n->cy, zmid };
	const double sp = n->worldSpacing * std::max(s->xfac, 1.0);    // scaled node gap
	double px;
	if (parallel) {
		px = (parScale > 0.0) ? sp * vpH / (2.0 * parScale) : 1e9;
	} else {
		const double dvx = pc[0]-camPos[0], dvy = pc[1]-camPos[1], dvz = pc[2]-camPos[2];
		double dist = std::sqrt(dvx*dvx + dvy*dvy + dvz*dvz); if (dist < 1e-6) dist = 1e-6;
		px = sp * vpH / (2.0 * dist * tanHalfFov);
	}
	if (n->leaf || px <= tau) {
		ensureNodeActor(s, n); n->lastUsed = s->lodFrame;   // draw at this LOD
		for (int k = 0; k < 4; ++k) dropSubtree(s, n->child[k]);   // shed finer detail
	} else {
		dropNodeActor(s, n);                                 // too coarse -> recurse
		for (int k = 0; k < 4; ++k) refineNode(s, n->child[k], cam, camPos, vpH, tanHalfFov, parScale, parallel, tau);
	}
}

static void refineQuadtree(Scene* s) {
	if (!s->quadRoot || !s->ren) return;
	vtkCamera* cam = s->ren->GetActiveCamera(); if (!cam) return;
	double camPos[3]; cam->GetPosition(camPos);
	int* sz = s->ren->GetSize(); const double vpH = (sz && sz[1] > 0) ? sz[1] : 600.0;
	const bool parallel = cam->GetParallelProjection() != 0;
	const double tanHalf = std::tan(vtkMath::RadiansFromDegrees(cam->GetViewAngle() * 0.5));
	s->lodFrame++;
	refineNode(s, s->quadRoot, cam, camPos, vpH, tanHalf, cam->GetParallelScale(), parallel, /*tau=*/2.0);
	if (s->lodResidentBytes > s->lodBudgetBytes) evictLRU(s);
}

static void onLodCamera(vtkObject*, unsigned long, void* cd, void*) {
	refineQuadtree(static_cast<Scene*>(cd));
}

// GRAPHICAL ELEMENT: custom dock title bar that folds the dock HORIZONTALLY.
// Open  -> a normal horizontal strip: "▾ Title" across the top.
// Folded -> a thin vertical strip (~one text-height wide) with "▸" at the top and
// the Title painted rotated 90° (reading bottom->top) down the window edge, so the
// collapsed dock costs only its strip width instead of leaving its full open width
// as dead, unusable space. Clicking anywhere on the bar toggles via onClick.
// (No Q_OBJECT: this TU has no moc — we override virtuals and call a std::function.)
struct FoldTitleBar : QWidget {
	QString title;
	bool    folded    = false;
	int     openWidth = 0;            // dock width remembered at fold time, restored on un-fold
	std::function<void()> onClick;
	explicit FoldTitleBar(const QString& t, QWidget* parent = nullptr)
		: QWidget(parent), title(t) {
		setCursor(Qt::PointingHandCursor);
		setToolTip("Fold / un-fold this panel");
	}
	QSize sizeHint() const override {
		QFontMetrics fm(font());
		const int thick = fm.height() + 8;                       // strip thickness
		const int along = fm.horizontalAdvance(title) + thick + 12;
		return folded ? QSize(thick, along) : QSize(along, thick);
	}
	QSize minimumSizeHint() const override { return sizeHint(); }
	void mousePressEvent(QMouseEvent*) override { if (onClick) onClick(); }
	void paintEvent(QPaintEvent*) override {
		QPainter p(this);
		p.setPen(palette().color(QPalette::WindowText));
		QFontMetrics fm(font());
		const QString glyph = folded ? QStringLiteral("▸")  // ▸ folded
									 : QStringLiteral("▾"); // ▾ open
		if (!folded) {
			const int y = (height() + fm.ascent() - fm.descent()) / 2;
			p.drawText(6, y, glyph + " " + title);
		} else {
			// arrow centred near the top of the vertical strip
			p.drawText(QRect(0, 2, width(), fm.height()), Qt::AlignHCenter, glyph);
			// title rotated to read bottom->top, filling the strip below the arrow
			p.save();
			p.translate(0, height());
			p.rotate(-90);
			p.drawText(QRect(4, 0, height() - fm.height() - 8, width()),
					   Qt::AlignVCenter | Qt::AlignLeft, title);
			p.restore();
		}
	}
};

// Polygon draw/edit tool (defined in 85_polygon.cpp, #included after this file). The toolbar
// button toggles draw mode via polygonSetMode; the mouse gestures are driven from GLView.
static void polygonSetMode(Scene* s, bool on);
static QIcon makePolygonIcon();

static Scene* buildAndShow(vtkSmartPointer<vtkPolyData> pd,
						 double x0, double x1, double y0, double y1,
						 double zmin, double zmax,
						 double xfac, double zfac, double ve0,
						 const double *cz, const double *crgb, int ncolor,  // CPT nodes: cz[n] + crgb[n*3] 0..1; 0 = default
						 const unsigned char *img, int iw, int ih, int ibands,  // optional drape: RGB[A] iw*ih*ibands, row 0 = south
						 int edges,                                             // !=0 -> draw mesh edges (GMTF3D :shademesh)
						 bool pointCloud,                                       // true -> Verts-only cloud: LOD actor, no normals/drape
						 int geographic,                                        // !=0 -> x,y are lon,lat (axis titles "lon"/"lat")
						 const char *title,
						 const char *objname = nullptr,    // Scene Objects label for the surface ("" -> "Surface")
						 bool imageOnly = false,            // bare image: no surface row; readout shows colour
						 const float *gz = nullptr,         // non-null -> TILED plain-grid render (pd ignored)
						 int gnx = 0, int gny = 0) {        // grid dims for the tiled path
	ensureApp();

	Scene *s = new Scene();
	g_scenes.insert(s);                     // register as a live figure handle
	s->imageOnly = imageOnly;               // set BEFORE the Scene Objects panel is built (rebuildSceneObjects)
	s->zmin = zmin; s->zmax = zmax;
	s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1;
	s->xfac = xfac; s->zfac = zfac; s->ve = ve0;

	// --- VTK render window in a Qt widget -----------------------------------
	auto *widget = new GLView();
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	rw->SetMultiSamples(0);                 // no hardware MSAA (FXAA post-pass does the AA, like F3D).
	                                        // 8x MSAA = 8x fragment work every frame -> kills big point clouds.
	widget->setRenderWindow(rw);
	s->widget = widget;
	widget->s = s;                          // GLView handles the middle button itself
	g_lastRW = rw;
	g_lastScene = s;                        // gmtvtk_add_overlay targets the most-recent scene

	vtkNew<vtkNamedColors> nc;
	s->ren = vtkSmartPointer<vtkRenderer>::New();
	s->ren->GradientBackgroundOn();
	s->ren->SetBackground(0.16, 0.18, 0.22);    // bottom (dark slate)
	s->ren->SetBackground2(0.36, 0.42, 0.52);   // top
	rw->AddRenderer(s->ren);

	// Overlay layer (1) for the Z tick billboards. It shares the MAIN camera (so the labels
	// track the same view), keeps the lower layer's colour (transparent except where text is),
	// and clears its own depth so the surface can never occlude the labels. Its default
	// auto-created headlight lights the always-camera-facing text uniformly -> constant
	// brightness at every rotation, fixing the "labels go dark/invisible on some angles" bug.
	rw->SetNumberOfLayers(2);
	s->axesRen = vtkSmartPointer<vtkRenderer>::New();
	s->axesRen->SetLayer(1);
	s->axesRen->InteractiveOff();
	s->axesRen->PreserveColorBufferOn();
	s->axesRen->SetActiveCamera(s->ren->GetActiveCamera());
	rw->AddRenderer(s->axesRen);

	// F3D-style light rig: a 3-point vtkLightKit (key/fill/back/head) instead of
	// the single flat headlight VTK adds by default. This is what gives F3D's
	// relief its form-revealing gradients. (F3D: vtkF3DRenderer::UpdateLights.)
	// Lighting: one user-aimed directional KEY light (azimuth/elevation) + a dim
	// FILL light, both managed by applyShading. Direction is set there from
	// s->lightAz / s->lightEl so the Shading dock can move the "sun" live.
	s->ren->SetAutomaticLightCreation(false);
	s->keyLight = vtkSmartPointer<vtkLight>::New();
	s->keyLight->SetLightTypeToSceneLight();
	s->keyLight->SetPositional(false);          // infinite (directional) light
	s->fillLight = vtkSmartPointer<vtkLight>::New();
	s->fillLight->SetLightTypeToHeadlight();    // fills the camera-facing shadow side
	s->ren->AddLight(s->keyLight);
	s->ren->AddLight(s->fillLight);
	s->envTex = makeSkyEnv();

	// Colour map. A GMT CPT arrives as control nodes (cz[i] -> crgb[i]); a
	// vtkColorTransferFunction maps z to colour at those exact (possibly non-uniform,
	// data-equalized) positions. No CPT -> a plain blue->red ramp (demo).
	vtkSmartPointer<vtkScalarsToColors> lut;
	bool ctfRange = false;
	if (cz && crgb && ncolor > 0) {
		vtkNew<vtkColorTransferFunction> ctf;
		for (int i = 0; i < ncolor; ++i)
			ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
		lut = ctf;
		ctfRange = true;        // the CTF maps absolute z; let the mapper defer to it
	}
	else {
		vtkNew<vtkLookupTable> t;
		t->SetHueRange(0.667, 0.0);         // fallback blue -> red (demo)
		t->SetNumberOfTableValues(256);
		t->SetRampToLinear();
		t->SetTableRange(s->zmin, s->zmax);
		t->Build();
		lut = t;
	}

	// ===== surface: tiled grid (gz) OR single actor (pd) =====================
	// Declared out here so the drape block below (single-actor path) can share them.
	bool hasNormals = false;
	vtkNew<vtkPolyDataNormals> norms;        // polydata-surface path only
	if (gz && gnx > 1 && gny > 1) {
		// Tiled-LOD plain grid: build the quadtree (indices only) + an empty assembly; tile actors
		// are created lazily by the screen-space-error refinement (refineQuadtree) and re-evaluated
		// on every camera move. No giant polydata; resident geometry is bounded by lodBudgetBytes.
		s->surfGroup = vtkSmartPointer<vtkAssembly>::New();
		s->surfLut = lut; s->surfCtfRange = ctfRange; s->surfEdges = edges;
		// Data layer MUST exist before refineQuadtree (tiles sample s->gridZ). Populate it here from
		// gz (the caller fills it only AFTER buildAndShow returns, which would be too late).
		s->gridZ.assign(gz, gz + (size_t)gnx * gny);
		s->gnx = gnx; s->gny = gny;
		s->gx0 = x0; s->gx1 = x1; s->gy0 = y0; s->gy1 = y1;
		s->gdx = (gnx > 1) ? (x1 - x0) / (gnx - 1) : 0.0;
		s->gdy = (gny > 1) ? (y1 - y0) / (gny - 1) : 0.0;
		s->quadRoot = buildQuadNode(0, gnx - 1, 0, gny - 1, 0, x0, s->gdx, y0, s->gdy);
		s->surf = vtkSmartPointer<vtkActor>::New();   // placeholder handle; real geometry = tiles
		s->surfGroup->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		s->ren->AddActor(s->surfGroup);
		refineQuadtree(s);                            // initial (coarse) population so axes/bounds work
		s->lodCmd = vtkSmartPointer<vtkCallbackCommand>::New();
		s->lodCmd->SetCallback(onLodCamera);
		s->lodCmd->SetClientData(s);
		s->ren->GetActiveCamera()->AddObserver(vtkCommand::ModifiedEvent, s->lodCmd);
	}
	else {
	// Smooth shading needs per-vertex normals. A grid polydata arrives with them already baked
	// in (makeGridFromArray) -> feed it straight to the mapper, NO vtkPolyDataNormals second
	// copy. An FV/other surface with none still gets them generated here. Point clouds = unlit.
	hasNormals = pd && pd->GetPointData() && pd->GetPointData()->GetNormals() != nullptr;
	if (!pointCloud && !hasNormals) {
		norms->SetInputData(pd);
		norms->SetFeatureAngle(90.0);
		norms->SplittingOff();
		norms->ConsistencyOn();
	}

	vtkNew<vtkPolyDataMapper> map;
	// Cloud or already-normalled grid -> direct; bare surface -> through the normals filter.
	if (pointCloud || hasNormals) map->SetInputData(pd);
	else                          map->SetInputConnection(norms->GetOutputPort());
	map->SetLookupTable(lut);
	map->SetScalarRange(s->zmin, s->zmax);
	if (ctfRange) map->UseLookupTableScalarRangeOn();  // colours keyed to the CPT's own z nodes
	map->ScalarVisibilityOn();
	map->InterpolateScalarsBeforeMappingOn();   // per-fragment colour -> crisp gradient

	// A point cloud uses a vtkLODActor: while the camera moves it draws a decimated subset
	// (NumberOfCloudPoints), full resolution when still -> interaction stays smooth on huge
	// clouds (a plain actor redraws all N every frame = sluggish). A surface stays a plain actor.
	if (pointCloud) {
		vtkSmartPointer<vtkLODActor> la = vtkSmartPointer<vtkLODActor>::New();
		la->SetNumberOfCloudPoints(50000);   // points drawn during interaction
		s->surf = la;
	}
	else {
		s->surf = vtkSmartPointer<vtkActor>::New();
	}
	s->surf->SetMapper(map);
	s->surf->GetProperty()->SetInterpolationToPBR();  // PBR + IBL = F3D-style shading
	s->surf->GetProperty()->SetMetallic(0.0);         // terrain is dielectric
	s->surf->GetProperty()->SetRoughness(0.45);       // soft sheen, not mirror/not flat
	s->surf->GetProperty()->SetEdgeColor(0.12, 0.12, 0.12);   // wire mesh colour; hidden until 'e'
	s->surf->GetProperty()->SetLineWidth(1.0);
	s->surf->GetProperty()->SetEdgeVisibility(edges);         // initial mesh state (default off)
	s->surf->SetScale(s->xfac, 1.0, s->zfac * s->ve);   // base aspect + unit + initial VE
	s->ren->AddActor(s->surf);
	}

	// --- optional image drape -----------------------------------------------
	// A caller-supplied RGB[A] image is textured over the surface (via the tcoords
	// baked into the grid) instead of the CPT colouring. img is row-major, row 0 =
	// south (VTK texture origin), iw*ih pixels of ibands each.
	bool draped = false;
	if (img && iw > 0 && ih > 0 && ibands > 0) {
		vtkNew<vtkImageData> tex_img;
		tex_img->SetDimensions(iw, ih, 1);
		tex_img->AllocateScalars(VTK_UNSIGNED_CHAR, ibands);
		memcpy(tex_img->GetScalarPointer(), img, (size_t)iw * ih * ibands);
		vtkNew<vtkTexture> tex;
		tex->SetInputData(tex_img);
		tex->InterpolateOn();

		// Image overlay. The canvas spans the WHOLE grid bbox with alpha 0 outside the
		// image footprint, so only the grid ∩ image area is painted; the CPT-coloured
		// base surface shows everywhere else (mirrors GMTF3D drape_to_bbox). A separate
		// actor shares the geometry + tcoords; its RGBA texture has alpha, so VTK runs it
		// in the translucent pass and blends the picture over the base. A polygon offset
		// pulls it toward the camera so it wins the depth tie with the base surface.
		vtkNew<vtkPolyDataMapper> dmap;
		if (hasNormals) dmap->SetInputData(pd);                    // grid: normals baked in
		else            dmap->SetInputConnection(norms->GetOutputPort());
		dmap->ScalarVisibilityOff();        // colour comes from the texture, not the CPT
		vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
		dmap->SetRelativeCoincidentTopologyPolygonOffsetParameters(-1.0, -1.0);
		s->drape = vtkSmartPointer<vtkActor>::New();
		s->drape->SetMapper(dmap);
		s->drape->SetTexture(tex);
		// The drape is a finished PICTURE — show its TRUE pixel colours, NOT re-shaded by the
		// scene. (1) NEVER PBR: VTK's PBR shader only samples SetBaseColorTexture, so a plain
		// SetTexture drape renders flat grey. (2) NOT Phong either: the key/fill sun lights then
		// shade the image (dark gradient = "super shitty"). LightingOff renders the texture flat
		// at full albedo, immune to every light. applyShading() leaves the drape untouched so
		// this sticks; the BASE surface keeps PBR.
		s->drape->GetProperty()->LightingOff();
		s->drape->SetScale(s->xfac, 1.0, s->zfac * s->ve);    // track the base actor's scale/VE
		// GMTF3D :shademesh — draw the grid's cell edges over the drape. The drape canvas is
		// opaque outside_color fill where the image does NOT reach (bridge _drape_to_bbox with
		// outside=:shade/:shademesh), so the uncovered area reads as a shaded MESH; the image
		// area keeps the picture with the same faint wires on top (matches the f3d `_edges` path).
		s->drape->GetProperty()->SetEdgeColor(0.12, 0.12, 0.12);
		s->drape->GetProperty()->SetLineWidth(1.0);
		s->drape->GetProperty()->SetEdgeVisibility(edges);   // tracks the base; toggled by 'e'
		s->ren->AddActor(s->drape);
		draped = true;
	}

	// --- cube axes ----------------------------------------------------------
	s->axes = vtkSmartPointer<vtkCubeAxesActor>::New();
	s->axes->SetCamera(s->ren->GetActiveCamera());
	double b[6]; surfGetBounds(s, b);
	if (b[5] - b[4] <= 0.0) b[5] = b[4] + 1.0;          // zero Z extent (bare image / flat) -> avoid
	                                                    // vtkAxisActor 0/0 label-count crash
	s->axes->SetBounds(b);
	s->axes->SetXAxisRange(s->x0, s->x1);               // TRUE labels despite the actor scale
	s->axes->SetYAxisRange(s->y0, s->y1);
	s->axes->SetZAxisRange(s->zmin, s->zmax);
	s->axes->GetTitleTextProperty(0)->SetColor(0.9, 0.9, 0.9);
	s->axes->GetLabelTextProperty(0)->SetColor(0.8, 0.8, 0.8);
	for (int ax = 0; ax < 3; ++ax) {
		s->axes->GetXAxesLinesProperty()->SetColor(0.7, 0.7, 0.7);
	}
	// Geographic data -> lon/lat axis names; cartesian -> X/Y. Z always "Z". Drawn as overlay
	// billboards (placeAxisTitle), NOT cube-native titles (those don't render in this StaticTriad
	// setup with native labels off). Clear the cube titles so nothing competes.
	s->axes->SetXTitle(" "); s->axes->SetYTitle(" "); s->axes->SetZTitle(" ");  // single space, NOT "" — empty makes vtkVectorText error "Text is not set!" every render
	s->axName[0] = geographic ? "lon" : "X";
	s->axName[1] = geographic ? "lat" : "Y";   // X/Y names only — Z gets NO name title
	for (int i = 0; i < 2; ++i) {
		auto t = vtkSmartPointer<vtkBillboardTextActor3D>::New();
		vtkTextProperty* tp = t->GetTextProperty();
		tp->SetColor(1.0, 1.0, 1.0);
		tp->SetFontFamilyToArial(); tp->BoldOn(); tp->ItalicOff(); tp->ShadowOff();
		tp->SetFontSize(13);                 // a touch larger + bold so the name reads as a title
		tp->SetJustificationToCentered();
		tp->SetVerticalJustificationToCentered();
		t->SetInput(s->axName[i].c_str());
		t->ForceOpaqueOn();
		t->PickableOff();
		t->SetVisibility(0);                 // rebuildAxisLabels positions + shows it
		s->axesRen->AddViewProp(t);          // overlay layer: even brightness, never occluded
		s->axTitle[i] = t;
	}
	s->axes->DrawXGridlinesOn(); s->axes->DrawYGridlinesOn(); s->axes->DrawZGridlinesOn();
	s->axes->SetGridLineLocation(vtkCubeAxesActor::VTK_GRID_LINES_FURTHEST);
	// StaticTriad pins X,Y to the zmin FLOOR (coplanar) — Y/X labels are ALWAYS on the bottom
	// edge, never lifting to a top edge — with native 3-D text (parallel/orthogonal to the axis,
	// reorienting with the view). Mirrors the user's f3d_ext_cube_axes.cxx (his HARD RULE). Z's
	// OWN labels run along the axis -> OFF; Z values are drawn as horizontal billboards (ALWAYS
	// perpendicular to Z) by rebuildAxisLabels().
	s->axes->SetFlyModeToStaticTriad();
	s->axes->SetTickLocationToOutside();
	s->axes->SetScreenSize(13.0);
	s->axes->SetZAxisVisibility(1);          // draw the Z axis LINE (+ gridlines) like X/Y
	// Native value labels AND native ticks OFF on ALL THREE axes. rebuildAxisLabels draws the
	// values as identical freetype billboards AND draws our own SINGLE outward tickmark per label
	// (s->axisTicks) — the cube's native ticks were doubled across the two faces sharing each
	// edge. Only the cube's axis LINES + gridlines remain.
	s->axes->SetXAxisLabelVisibility(0);
	s->axes->SetYAxisLabelVisibility(0);
	s->axes->SetZAxisLabelVisibility(0);
	s->axes->SetXAxisTickVisibility(0);
	s->axes->SetYAxisTickVisibility(0);
	s->axes->SetZAxisTickVisibility(0);
	// MAJOR ticks only on every axis. Minor ticks defaulted ON and made a dense two-directional
	// comb on Z (its range is thousands, so minor=majorDelta/5 packed ~30 marks; X/Y ranges are
	// small so theirs stayed sparse) -> Z now ticks like X/Y.
	s->axes->XAxisMinorTickVisibilityOff();
	s->axes->YAxisMinorTickVisibilityOff();
	s->axes->ZAxisMinorTickVisibilityOff();
	for (int i = 0; i < 3; ++i) {                // white, ARIAL, non-bold -> X/Y/Z share ONE font
		vtkTextProperty* tp = s->axes->GetTitleTextProperty(i);
		tp->SetColor(1.0, 1.0, 1.0); tp->SetFontFamilyToArial(); tp->BoldOff(); tp->ItalicOff(); tp->ShadowOff();
		vtkTextProperty* lp = s->axes->GetLabelTextProperty(i);
		lp->SetColor(1.0, 1.0, 1.0); lp->SetFontFamilyToArial(); lp->BoldOff(); lp->ItalicOff(); lp->ShadowOff();
	}
	s->ren->AddActor(s->axes);

	// Our own SINGLE outward tickmarks (rebuilt every render by rebuildAxisLabels). Unlit grey
	// lines, like the cube's axis lines; the cube's native (doubled) ticks are off.
	s->axisTickPD = vtkSmartPointer<vtkPolyData>::New();
	{
		vtkNew<vtkPolyDataMapper> tm; tm->SetInputData(s->axisTickPD);
		s->axisTicks = vtkSmartPointer<vtkActor>::New();
		s->axisTicks->SetMapper(tm);
		s->axisTicks->GetProperty()->SetColor(0.85, 0.85, 0.85);
		s->axisTicks->GetProperty()->LightingOff();
		s->axisTicks->GetProperty()->SetLineWidth(1.0);
		s->axisTicks->PickableOff();
		s->ren->AddActor(s->axisTicks);
	}
	rebuildAxisLabels(s);                         // billboards (same font/size on X/Y/Z) + single ticks

	// --- scalar bar ---------------------------------------------------------
	// ===== COLORBAR DIMENSIONS & LOCATION (all in NORMALIZED viewport coords [0..1]) =====
	// The coloured strip is drawn by vtkScalarBarActor; the TICK MARKS and NUMBERS are drawn
	// by us (vtkScalarBarActor has NO tick-mark geometry in VTK 9.6, and its built-in labels
	// overlap the strip on a narrow bar). Drawing them ourselves guarantees: short ticks at the
	// strip's left edge + numbers right-justified to the LEFT of the ticks => never overlap.
	const double CB_X0 = 0.93, CB_Y0 = 0.55;   // bottom-left corner of the FRAME
	const double CB_W  = 0.06, CB_H  = 0.40;   // frame size -> top 0.95, right edge 0.99 (upper-right)
	const double CB_BARRATIO = 0.30;           // coloured strip = right 30% of CB_W (~0.018 wide ruler)
	const double CB_TICKLEN  = 0.006;          // SHORT tick length (normalized x), pointing LEFT (halved from 0.012)
	const double CB_LABELGAP = 0.004;          // gap between a tick's outer end and its number
	const double barLeft = CB_X0 + CB_W * (1.0 - CB_BARRATIO);   // left edge of the coloured strip

	if (!imageOnly) {     // bare image -> NO colorbar (only grids / point clouds get one)
	s->bar = vtkSmartPointer<vtkScalarBarActor>::New();
	s->bar->SetLookupTable(lut);
	s->bar->SetTitle("");                   // drop the big 'Z' title
	s->bar->SetDrawTickLabels(false);       // WE draw the numbers (below) — kill the overlapping built-ins
	s->bar->SetTextPositionToPrecedeScalarBar();
	s->bar->SetWidth(CB_W); s->bar->SetHeight(CB_H);
	s->bar->SetPosition(CB_X0, CB_Y0);
	s->bar->SetBarRatio(CB_BARRATIO);
	s->ren->AddActor2D(s->bar);             // CPT legend stays: the base surface shows it
											// wherever the image drape does not cover

	// --- our own tick marks + annotations, aligned to the strip, no overlap ---------------
	// Nice round values (800, 900, ...) at a constant interval; niceNum picks a 1/2/5 x10^n step.
	{
		const double lo = s->zmin, hi = s->zmax;
		const double step = niceNum(niceNum(hi - lo, false) / 5.0, true);
		vtkNew<vtkPoints>    tpts;
		vtkNew<vtkCellArray> tlines;
		for (double v = std::ceil(lo / step) * step; v <= hi + 1e-9 * (hi - lo); v += step) {
			const double frac = (v - lo) / (hi - lo);          // 0 at bottom (zmin) .. 1 at top (zmax)
			const double y    = CB_Y0 + frac * CB_H;
			// short horizontal tick at the strip's left edge, pointing left
			vtkIdType a = tpts->InsertNextPoint(barLeft, y, 0.0);
			vtkIdType b = tpts->InsertNextPoint(barLeft - CB_TICKLEN, y, 0.0);
			tlines->InsertNextCell(2); tlines->InsertCellPoint(a); tlines->InsertCellPoint(b);
			// number, right-justified so its RIGHT edge stops short of the tick => never touches strip
			char buf[32]; snprintf(buf, sizeof buf, "%.0f", v);
			vtkSmartPointer<vtkTextActor> ta = vtkSmartPointer<vtkTextActor>::New();
			ta->SetInput(buf);
			ta->GetTextProperty()->SetColor(0.9, 0.9, 0.9);
			ta->GetTextProperty()->SetFontSize(10);   // colorbar annotation font (smaller, was 13)
			ta->GetTextProperty()->SetJustificationToRight();
			ta->GetTextProperty()->SetVerticalJustificationToCentered();
			ta->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
			ta->SetPosition(barLeft - CB_TICKLEN - CB_LABELGAP, y);
			s->ren->AddActor2D(ta);
		}
		vtkNew<vtkPolyData> tpd; tpd->SetPoints(tpts); tpd->SetLines(tlines);
		vtkNew<vtkCoordinate> nc; nc->SetCoordinateSystemToNormalizedViewport();
		vtkNew<vtkPolyDataMapper2D> tmap; tmap->SetInputData(tpd); tmap->SetTransformCoordinate(nc);
		vtkSmartPointer<vtkActor2D> tact = vtkSmartPointer<vtkActor2D>::New();
		tact->SetMapper(tmap);
		tact->GetProperty()->SetColor(0.9, 0.9, 0.9);
		tact->GetProperty()->SetLineWidth(1.5);
		s->ren->AddActor2D(tact);
	}
	}   // end if(!imageOnly) colorbar guard

	// Default view: world +Z up; azimuth 0 (look north, +Y) and elevation 35deg above
	// horizontal. Camera sits due south of the focal point, raised 35deg. Then zoom in so
	// the relief fills most of the display (ResetCamera alone leaves a wide margin).
	s->ren->ResetCamera();
	{
		vtkCamera* cam = s->ren->GetActiveCamera();
		double fp[3]; cam->GetFocalPoint(fp);
		double dist = cam->GetDistance();
		const double el = 35.0 * vtkMath::Pi() / 180.0;
		cam->SetViewUp(0.0, 0.0, 1.0);
		cam->SetPosition(fp[0],
		                 fp[1] - dist * std::cos(el),
		                 fp[2] + dist * std::sin(el));
		s->ren->ResetCamera();                  // refit distance along the new direction
		cam->Zoom(1.5);                         // fill most of the display area
		s->ren->ResetCameraClippingRange();
	}

	// SSAO sampling radius scales with the scene size; seed it from the bbox
	// diagonal, then build the whole light/material/pass setup via applyShading
	// (the Shading dock re-runs the same function on every slider change).
	{
		double bb[6]; s->ren->ComputeVisiblePropBounds(bb);
		double diag = std::sqrt((bb[1]-bb[0])*(bb[1]-bb[0]) +
		                        (bb[3]-bb[2])*(bb[3]-bb[2]) +
		                        (bb[5]-bb[4])*(bb[5]-bb[4]));
		if (diag > 0.0) { s->ssaoRadius = 0.1 * diag; s->ssaoBias = 1e-4 * diag; }
	}

	// --- picker for coordinate readout --------------------------------------
	// (The hover readout uses a GPU z-buffer pick in onMouseMove — O(1) for any grid size — so no
	// cell locator is built here; vtkCellPicker would be O(cells) brute force and OOM on big grids.)
	s->picker = vtkSmartPointer<vtkCellPicker>::New();
	s->picker->SetTolerance(0.001);

	// --- profile track drape line (Ctrl+left-drag fills it) -----------------
	{
		vtkNew<vtkPolyDataMapper> pm;
		vtkNew<vtkPolyData> empty;
		pm->SetInputData(empty);
		pm->ScalarVisibilityOff();
		vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
		pm->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, -9000.0);  // sit above surface + drape
		s->profLine = vtkSmartPointer<vtkActor>::New();
		s->profLine->SetMapper(pm);
		s->profLine->GetProperty()->LightingOff();
		s->profLine->GetProperty()->SetColor(1.0, 0.15, 0.15);   // red track, Fledermaus-style
		s->profLine->GetProperty()->SetLineWidth(2.5);
		s->profLine->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		s->profLine->SetVisibility(0);
		s->ren->AddActor(s->profLine);
	}

	// --- main window + native menubar ---------------------------------------
	// Heap-allocated + delete-on-close: the function returns immediately (the host
	// pumps the loop), so the window must outlive this stack frame.
	QMainWindow *win = new QMainWindow();
	win->setAttribute(Qt::WA_DeleteOnClose);
	win->setWindowTitle(title ? title : "GMT 3-D Viewer  (Qt + VTK)");
	win->resize(1100, 800);
	win->setCentralWidget(widget);
	win->statusBar()->showMessage("ready");
	enableFileDrops(win, widget, s);        // drop a grid/image/table file onto any window to add it
	s->win = win;
	++g_openWindows;
	QObject::connect(win, &QObject::destroyed, [s]() {
		--g_openWindows;
		if (g_lastScene == s) g_lastScene = nullptr;   // don't let add_overlay touch a freed scene
		g_scenes.erase(s);                             // invalidate any host-held handle to s
		delete s->giz; delete s;
	});

	auto actReset = [s]() {
		s->ren->ResetCamera();
		s->widget->renderWindow()->Render();
	};
	auto actToggleAxes = [s]() {
		s->axes->SetVisibility(!s->axes->GetVisibility());
		s->widget->renderWindow()->Render();
	};
	auto actToggleBar = [s]() {
		if (!s->bar) return;                 // bare image has no colorbar
		s->bar->SetVisibility(!s->bar->GetVisibility());
		s->widget->renderWindow()->Render();
	};
	auto actVE = [s]() {
		bool ok = false;
		double v = QInputDialog::getDouble(s->win, "Vertical exaggeration",
										   "VE factor:", s->ve, 0.01, 1.0e4, 3, &ok);
		if (ok) { s->ve = v; applyVE(s); }
	};
	auto actShot = [s]() {
		QString fn = QFileDialog::getSaveFileName(s->win, "Save screenshot", "gmtvtk.png", "PNG (*.png)");
		if (fn.isEmpty()) return;
		vtkNew<vtkWindowToImageFilter> w2i;
		w2i->SetInput(s->widget->renderWindow());
		w2i->SetScale(2); w2i->Update();
		vtkNew<vtkPNGWriter> wr;
		wr->SetFileName(fn.toLocal8Bit().constData());
		wr->SetInputConnection(w2i->GetOutputPort());
		wr->Write();
	};
	auto actToggleGizmo = [s]() {
		if (s->giz) { setGizmoVisible(*s->giz, !s->giz->visible); s->widget->renderWindow()->Render(); }
	};
	// State-driven flat-2D <-> 3D switch. 2D = collapse VE to a plane, top-down orthographic,
	// rotation/tilt locked (gated in DragCB via s->flat2d), gizmo hidden. The Z tick billboards
	// self-hide when the drawn Z extent is zero (placeTickBillboards d0==d1 guard). Idempotent:
	// calling with the current state is a no-op. The shared act2D checkmark is kept in sync.
	auto setFlat2D = [s](bool on) {
		if (on == s->flat2d) { if (s->act2D) s->act2D->setChecked(on); return; }
		vtkCamera* cam = s->ren->GetActiveCamera();
		s->flat2d = on;
		if (s->flat2d) {
			cam->GetPosition(s->sav_pos);          // save the 3D view to restore later
			cam->GetFocalPoint(s->sav_foc);
			cam->GetViewUp(s->sav_vup);
			s->sav_parallel = cam->GetParallelProjection();

			// 2D = TOP-DOWN ORTHO ONLY. Keep the relief and its PBR lighting exactly as in 3D
			// (illumination must NOT change) — viewed straight down in parallel projection it reads
			// as a shaded-relief map. We do NOT flatten (ve) or touch lighting: flattening kills the
			// hillshade, and LightingOff on PBR renders near-black.
			if (s->giz) setGizmoVisible(*s->giz, false);

			double b[6]; surfGetBounds(s, b);      // north (+Y) up
			const double fp[3] = { 0.5*(b[0]+b[1]), 0.5*(b[2]+b[3]), 0.5*(b[4]+b[5]) };
			cam->SetFocalPoint(fp[0], fp[1], fp[2]);
			cam->SetViewUp(0.0, 1.0, 0.0);
			cam->SetPosition(fp[0], fp[1], b[5] + (b[5]-b[4]) + 1.0);  // above the surface, not inside it
			cam->ParallelProjectionOn();
			s->ren->ResetCameraClippingRange();
			fitSnapView(s, /*topMode=*/true);      // maximize: fill the viewport edge-to-edge
		}
		else {
			cam->SetParallelProjection(s->sav_parallel);
			cam->SetPosition(s->sav_pos);
			cam->SetFocalPoint(s->sav_foc);
			cam->SetViewUp(s->sav_vup);
			if (s->giz) setGizmoVisible(*s->giz, true);
			s->ren->ResetCameraClippingRange();
		}
		if (s->act2D) s->act2D->setChecked(s->flat2d);
		s->widget->renderWindow()->Render();
	};
	auto actToggle2D = [s, setFlat2D]() { setFlat2D(!s->flat2d); };
	auto actAbout = [win]() {
		QMessageBox::about(win, "About",
			"GMT 3-D Viewer\n\nNative Qt UI + VTK 3-D, self-contained.\n\n"
			"Left-drag: horizontal = rotate (azimuth), vertical = tilt.\n"
			"Middle-click: set the centre of rotation to that point.\n"
			"Right-drag / wheel: zoom.\n"
			"Gizmo handles — amber cone: vert. exaggeration;  tip ring: tilt;  "
			"compass ring: azimuth.   'x' hides/shows the gizmo.");
	};

	QMenu *mFile = win->menuBar()->addMenu("&File");
	mFile->addAction("Save &Screenshot…", actShot);
	mFile->addSeparator();
	mFile->addAction("&Close", [win](){ win->close(); }, QKeySequence::Close);

	QMenu *mView = win->menuBar()->addMenu("&View");
	mView->addAction("&Reset Camera", actReset, QKeySequence("R"));
	QAction *aAxes = mView->addAction("Show Cube &Axes", actToggleAxes);
	aAxes->setCheckable(true); aAxes->setChecked(true);
	QAction *aBar = mView->addAction("Show Color &Bar", actToggleBar);
	aBar->setCheckable(true); aBar->setChecked(true);
	QAction *aGiz = mView->addAction("Show &Gizmo", actToggleGizmo);  // 'x' also toggles (VTK)
	aGiz->setCheckable(true); aGiz->setChecked(true);
	// Shared checkable "Flat 2D (map)" action — lives in the View menu AND the toolbar below, so
	// both reflect the same state. actToggle2D authors the checkmark (via setFlat2D).
	s->act2D = mView->addAction("Flat &2D (map)", actToggle2D);
	s->act2D->setCheckable(true); s->act2D->setChecked(false);
	mView->addSeparator();
	mView->addAction("Vertical &Exaggeration…", actVE);

	win->menuBar()->addMenu("&Help")->addAction("&About", actAbout);

	// --- toolbar row (below the menu bar): quick-access buttons (ParaView-style) ------------
	// Open file -> hand the path back to Julia (iview auto-dispatches grid/image/dataset into a
	// NEW window). 2D/3D -> the shared act2D toggle. More buttons can be appended here later.
	QToolBar* tb = win->addToolBar("Main");
	tb->setMovable(false);
	tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	QAction* actOpen = tb->addAction(win->style()->standardIcon(QStyle::SP_DirOpenIcon), "");  // icon only, no text
	actOpen->setToolTip("Open a grid / image / table file in a new window");
	QObject::connect(actOpen, &QAction::triggered, [s, win]() {
		const QString fn = QFileDialog::getOpenFileName(win, "Open file");
		if (fn.isEmpty() || !g_juliaEval) return;
		// Build iview("…") with the path safely quoted (raw string => backslashes are literal).
		std::string cmd = "InteractiveGMT.iview(raw\"" + fn.toStdString() + "\")";
		static std::vector<char> buf(1 << 12);
		g_juliaEval(s, cmd.c_str(), buf.data(), (int)buf.size());
	});
	// 2D/3D toggle: a plain text button showing the CURRENT view ("3D" in 3D, "2D" in flat 2D).
	// Its own QToolButton (not act2D, which keeps the descriptive "Flat 2D (map)" menu text). The
	// label tracks act2D's checked state, so any toggle source (menu, context menu, bare-image init
	// in 90_c_api) keeps it in sync.
	QToolButton* tb2D = new QToolButton(tb);
	tb2D->setText("3D");
	tb2D->setToolTip("Toggle flat 2D map / 3D perspective view");
	QObject::connect(tb2D, &QToolButton::clicked, actToggle2D);
	QObject::connect(s->act2D, &QAction::toggled, tb2D, [tb2D](bool on){ tb2D->setText(on ? "2D" : "3D"); });
	tb->addWidget(tb2D);

	// Polygon tool: a checkable toolbar button (pentagon icon). Checked = draw mode — left-click
	// adds vertices, right-click removes the last, double-left-click closes the polygon. When the
	// button is OFF, a double-click on a finished polygon enters vertex-edit mode (square handles,
	// click-drag a vertex). polygonSetMode lives in 85_polygon.cpp.
	QAction* actPoly = tb->addAction(makePolygonIcon(), "Polygon");   // GRAPHICAL ELEMENT: polygon-draw toggle button
	actPoly->setCheckable(true);
	actPoly->setToolTip("Draw a polygon: left-click adds vertices, right-click undoes one, "
						"double-click closes it. Double-click a polygon to edit its vertices.");
	s->polyAct = actPoly;
	QObject::connect(actPoly, &QAction::toggled, [s](bool on){ polygonSetMode(s, on); });

	// --- native right-click context menu over the 3-D view ------------------
	widget->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(widget, &QWidget::customContextMenuRequested,
		[=](const QPoint& pos) {
			// Ctrl+right is the rubber-band select gesture on a point cloud, not a menu
			// request — swallow it (rbConsume is set by the selection release handler).
			if (s->rbEnabled && (s->rbConsume ||
				(QApplication::keyboardModifiers() & Qt::ControlModifier))) {
				s->rbConsume = false;
				return;
			}
			// While drawing a polygon, right-click means "remove last vertex" (handled by the
			// polygon tool's VTK observer) — never pop the view context menu.
			if (s->polyMode && s->polyDrawing)
				return;
			// If an overlay (GMTdataset line/point) is under the cursor, select it and pop ITS
			// per-element menu. VTK display coords are bottom-up device px; Qt QPoint is top-down.
			{
				const double dpr = widget->devicePixelRatioF();
				const int    Hpx = widget->renderWindow()->GetSize()[1];
				const int    px  = int(pos.x() * dpr), py = int(Hpx - pos.y() * dpr);
				if (profileHitAt(s, px, py)) {   // profile line sits on top -> its menu wins
					popupProfileMenu(s, widget->mapToGlobal(pos));
					return;
				}
				const int pgi = polyHitPolygon(s, px, py, 8.0);   // a drawn polygon under the cursor?
				if (pgi >= 0) {
					popupLineObjectMenu(s, LineRef{ LK_Polygon, s->polys[pgi].line },
										QString::fromStdString(s->polys[pgi].name), widget->mapToGlobal(pos));
					return;
				}
				int ovMode = 1;
				vtkActor* ov = pickOverlayAt(s, px, py, ovMode);
				if (ov) {
					popupOverlayMenu(s, ov, ovMode, widget->mapToGlobal(pos));
					return;
				}
			}
			QMenu m(win);
			m.addAction("Reset Camera", actReset);
			QAction* ca = m.addAction("Cube Axes", actToggleAxes);
			ca->setCheckable(true); ca->setChecked(s->axes->GetVisibility());
			if (s->bar) {                    // no Color Bar entry for bare images
				QAction* cb = m.addAction("Color Bar", actToggleBar);
				cb->setCheckable(true); cb->setChecked(s->bar->GetVisibility());
			}
			QAction* cg = m.addAction("Gizmo", actToggleGizmo);
			cg->setCheckable(true); cg->setChecked(s->giz && s->giz->visible);
			QAction* c2 = m.addAction("2D", actToggle2D);
			c2->setCheckable(true); c2->setChecked(s->flat2d);
			m.addSeparator();
			m.addAction("Vertical Exaggeration…", actVE);
			m.addAction("Save Screenshot…", actShot);
			m.exec(widget->mapToGlobal(pos));
		});

	// --- Shading control dock (live PBR / IBL / post-pass tuning) -----------
	// Every control writes a Scene field and re-runs applyShading(); this is the
	// knob set for matching F3D's look without rebuilding (and lets the look be
	// tuned on a real display, which the headless screenshot path can't show).
	QDockWidget* dock = new QDockWidget("Shading", win);              // GRAPHICAL ELEMENT: the "Shading" dock — foldable side panel
	dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea); // user may drag-fold it to the LEFT or RIGHT window edge
	dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable); // foldable: drag/float/close
	QWidget* panel = new QWidget(dock);                              // container widget that holds all the shading controls
	QFormLayout* form = new QFormLayout(panel);                      // label-on-left / control-on-right rows inside the dock

	// Live tooltip for a slider: maps the raw slider position to the parameter's REAL range
	// [rmin,rmax] and shows "name: value unit  [rmin … rmax]". Updated on every change AND
	// popped at the cursor while dragging so the value is visible without hovering first.
	auto wireTip = [](QSlider* sl, QString name, double rmin, double rmax, QString unit, int dec) {
		auto fmt = [=](int v) {
			double t    = double(v - sl->minimum()) / double(sl->maximum() - sl->minimum());
			double real = rmin + t * (rmax - rmin);
			QString u = unit.isEmpty() ? "" : " " + unit;
			return QString("%1: %2%3   [%4 … %5%3]")
				.arg(name).arg(real, 0, 'f', dec).arg(u)
				.arg(rmin, 0, 'f', dec).arg(rmax, 0, 'f', dec);
		};
		sl->setToolTip(fmt(sl->value()));
		QObject::connect(sl, &QSlider::valueChanged, sl, [sl, fmt](int v) {
			sl->setToolTip(fmt(v));
			QToolTip::showText(QCursor::pos(), fmt(v), sl);
		});
	};

	// Drape blend: actor opacity of the image overlay, so the picture and the PBR-shaded
	// relief can be combined. 100% = opaque image, 0% = image faded out (surface shows
	// through). Only meaningful with a draped image, so the row exists ONLY when s->drape does.
	if (s->drape) {
		QSlider* slDrape = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Drape blend" slider — image-overlay opacity
		slDrape->setRange(0, 100); slDrape->setValue(int(s->drape->GetProperty()->GetOpacity() * 100));
		QObject::connect(slDrape, &QSlider::valueChanged, [s](int v){
			s->drape->GetProperty()->SetOpacity(v / 100.0);
			s->widget->renderWindow()->Render();
		});
		form->addRow("Drape blend", slDrape);
		wireTip(slDrape, "Drape blend", 0, 100, "%", 0);
	}

	QSlider* slRough = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Roughness" slider — PBR surface roughness
	slRough->setRange(0, 100); slRough->setValue(int(s->roughness * 100));
	QObject::connect(slRough, &QSlider::valueChanged, [s](int v){ s->roughness = v / 100.0; applyShading(s); });
	form->addRow("Roughness", slRough);
	wireTip(slRough, "Roughness", 0.0, 1.0, "", 2);

	QSlider* slMetal = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Metallic" slider — PBR metalness
	slMetal->setRange(0, 100); slMetal->setValue(int(s->metallic * 100));
	QObject::connect(slMetal, &QSlider::valueChanged, [s](int v){ s->metallic = v / 100.0; applyShading(s); });
	form->addRow("Metallic", slMetal);
	wireTip(slMetal, "Metallic", 0.0, 1.0, "", 2);

	QSlider* slLight = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Light" slider — key (sun) light intensity
	slLight->setRange(0, 300); slLight->setValue(int(s->lightIntensity * 100));
	QObject::connect(slLight, &QSlider::valueChanged, [s](int v){ s->lightIntensity = v / 100.0; applyShading(s); });
	form->addRow("Light", slLight);
	wireTip(slLight, "Light", 0.0, 3.0, "", 2);

	QSlider* slAz = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Sun azimuth" slider — key-light azimuth (deg from north, CW)
	slAz->setRange(0, 360); slAz->setValue(int(s->lightAz));
	QObject::connect(slAz, &QSlider::valueChanged, [s](int v){ s->lightAz = v; applyShading(s); });
	form->addRow("Sun azimuth", slAz);
	wireTip(slAz, "Sun azimuth", 0, 360, "deg", 0);

	QSlider* slEl = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Sun elevation" slider — key-light elevation above horizon
	slEl->setRange(0, 90); slEl->setValue(int(s->lightEl));
	QObject::connect(slEl, &QSlider::valueChanged, [s](int v){ s->lightEl = v; applyShading(s); });
	form->addRow("Sun elevation", slEl);
	wireTip(slEl, "Sun elevation", 0, 90, "deg", 0);

	QSlider* slFill = new QSlider(Qt::Horizontal, panel); // GRAPHICAL ELEMENT: "Fill" slider — fill-light intensity (shadow-side lift)
	slFill->setRange(0, 100); slFill->setValue(int(s->fillIntensity * 100));
	QObject::connect(slFill, &QSlider::valueChanged, [s](int v){ s->fillIntensity = v / 100.0; applyShading(s); });
	form->addRow("Fill", slFill);
	wireTip(slFill, "Fill", 0.0, 1.0, "", 2);

	QSlider* slEnv = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Env (IBL)" slider — image-based-light intensity
	slEnv->setRange(0, 300); slEnv->setValue(int(s->envIntensity * 100));
	QObject::connect(slEnv, &QSlider::valueChanged, [s](int v){ s->envIntensity = v / 100.0; applyShading(s); });
	form->addRow("Env (IBL)", slEnv);
	wireTip(slEnv, "Env (IBL)", 0.0, 3.0, "", 2);

	const double rad0 = (s->ssaoRadius > 0.0) ? s->ssaoRadius : 0.5;   // slider = 0..200% of seed
	QSlider* slSSAO = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "SSAO radius" slider — ambient-occlusion sampling radius
	slSSAO->setRange(0, 200); slSSAO->setValue(100);
	QObject::connect(slSSAO, &QSlider::valueChanged, [s, rad0](int v){ s->ssaoRadius = rad0 * v / 100.0; applyShading(s); });
	form->addRow("SSAO radius", slSSAO);
	wireTip(slSSAO, "SSAO radius", 0, 200, "%", 0);

	QCheckBox* cbIBL = new QCheckBox(panel); cbIBL->setChecked(s->useIBL);   // GRAPHICAL ELEMENT: "Image-based light" checkbox — toggles IBL
	QObject::connect(cbIBL, &QCheckBox::toggled, [s](bool b){ s->useIBL = b; applyShading(s); });
	form->addRow("Image-based light", cbIBL);

	QCheckBox* cbSSAO = new QCheckBox(panel); cbSSAO->setChecked(s->useSSAO); // GRAPHICAL ELEMENT: "Ambient occlusion" checkbox — toggles SSAO pass
	QObject::connect(cbSSAO, &QCheckBox::toggled, [s](bool b){ s->useSSAO = b; applyShading(s); });
	form->addRow("Ambient occlusion", cbSSAO);

	QCheckBox* cbTone = new QCheckBox(panel); cbTone->setChecked(s->useTone); // GRAPHICAL ELEMENT: "Tone mapping" checkbox — toggles tone-map pass
	QObject::connect(cbTone, &QCheckBox::toggled, [s](bool b){ s->useTone = b; applyShading(s); });
	form->addRow("Tone mapping", cbTone);

	QCheckBox* cbFXAA = new QCheckBox(panel); cbFXAA->setChecked(s->useFXAA); // GRAPHICAL ELEMENT: "FXAA" checkbox — toggles anti-alias post-pass
	QObject::connect(cbFXAA, &QCheckBox::toggled, [s](bool b){ s->useFXAA = b; applyShading(s); });
	form->addRow("FXAA", cbFXAA);

	panel->setLayout(form);
	dock->setWidget(panel);                                  // mount the controls panel into the Shading dock
	win->addDockWidget(Qt::RightDockWidgetArea, dock);       // dock the Shading panel to the RIGHT edge by default
	// Shading only bites on a shaded surface / 3-D body. A bare image (imageOnly) or a
	// Verts-only point cloud has nothing to light, so FOLD the dock by default there; the
	// View menu action still un-folds it on demand.
	const bool hasShadedBody = !imageOnly && !pointCloud;
	dock->setVisible(hasShadedBody);                         // GRAPHICAL ELEMENT: Shading dock initial fold state
	// GRAPHICAL ELEMENT: View menu "Shading Panel" item — folds/un-folds the Shading dock
	QAction* aShade = mView->addAction("Shading &Panel", [dock](){ dock->setVisible(!dock->isVisible()); });
	aShade->setCheckable(true); aShade->setChecked(hasShadedBody);   // menu checkmark tracks the dock's visibility

	// --- Scene Objects dock: Fledermaus-style show/hide checkbox per element -
	// One checkbox for the surface, the image drape (if any), and every line/point
	// overlay. rebuildSceneObjects() repopulates it whenever an overlay is added.
	QDockWidget* objDock = new QDockWidget("Scene Objects", win); // GRAPHICAL ELEMENT: the "Scene Objects" dock — foldable side panel
	objDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea); // user may drag-fold it to the LEFT or RIGHT window edge
	objDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable); // foldable: drag/float/close
	s->objPanel = new QWidget(objDock);                      // container widget; rebuildSceneObjects() fills it with per-object checkboxes
	objDock->setWidget(s->objPanel);                         // mount that container into the Scene Objects dock
	win->addDockWidget(Qt::LeftDockWidgetArea, objDock);     // dock the Scene Objects panel to the LEFT edge by default
	if (objname && objname[0])
		s->surfName = objname;                // named solid -> checkbox shows the solid name
	rebuildSceneObjects(s);                                  // populate the per-object show/hide checkboxes now
	// GRAPHICAL ELEMENT: View menu "Scene Objects Panel" item — folds/un-folds the Scene Objects dock
	QAction* aObjs = mView->addAction("Scene &Objects Panel", [objDock](){ objDock->setVisible(!objDock->isVisible()); });
	aObjs->setCheckable(true); aObjs->setChecked(true);      // menu checkmark tracks the dock's visibility

	// --- FOLD button on the side docks --------------------------------------
	// Qt has no built-in "collapse" affordance, so REPLACE each side dock's default title bar
	// with a FoldTitleBar. Folding hides the body AND shrinks the dock to a thin vertical strip
	// (resizeDocks), so the collapsed dock no longer leaves its full open width as dead space;
	// the strip carries the title rotated 90° down the window edge. Un-folding restores the body
	// and the remembered open width. This is the fold control Qt's default title bar never gave us.
	auto makeFoldable = [win](QDockWidget* d, QWidget* body, const QString& titleText) {
		FoldTitleBar* bar = new FoldTitleBar(titleText, d);  // GRAPHICAL ELEMENT: dock title bar = fold toggle
		d->setTitleBarWidget(bar);                        // swap Qt's default title bar for our fold strip
		bar->onClick = [win, d, body, bar]() {
			const bool fold = body->isVisible();          // visible now -> fold it away
			if (fold) bar->openWidth = d->width();        // remember the open width to restore later
			body->setVisible(!fold);                      // hide body -> dock can shrink to the strip
			bar->folded = fold;
			bar->updateGeometry();                        // sizeHint flips orientation
			bar->update();
			const int w = fold ? bar->sizeHint().width()
							   : (bar->openWidth > 0 ? bar->openWidth : 220);
			win->resizeDocks({d}, {w}, Qt::Horizontal);   // collapse to / expand from the strip width
		};
	};
	makeFoldable(dock,    panel,        "Shading");        // Shading dock fold button
	makeFoldable(objDock, s->objPanel,  "Scene Objects");  // Scene Objects dock fold button

	// --- Bottom tabbed panel: Profile / Julia Console / Data Viewer ----------
	// ONE dock holds a QTabWidget. A "Hide" button in the tab-bar corner collapses the panel
	// BODY down to just the tab strip (so the central 3-D view extends) and toggles to "Show".
	QDockWidget* bottomDock = new QDockWidget("Panels", win);
	bottomDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
	QTabWidget*  tabs = new QTabWidget(bottomDock);
	tabs->setDocumentMode(true);
	bottomDock->setWidget(tabs);
	win->addDockWidget(Qt::BottomDockWidgetArea, bottomDock);
	s->bottomDock = bottomDock;
	s->bottomTabs = tabs;

	// Tab 0 — Profile: 2D (distance, elevation) graph. Ctrl+left-drag a line on the surface
	// fills it (the 3D drape line + this panel update live; GMTF3D / Fledermaus profile track).
	s->prof = new ProfilePanel(tabs);
	tabs->addTab(s->prof, "Profile");

	// Tab 1 — Julia console: the viewer runs in-process in Julia, so a typed command is handed
	// straight back to Julia (g_juliaEval) and eval'd in Main. The callback binds `fig` to THIS
	// window, so `add!(fig, D)`, `view_points(...)`, etc. reach the figure with no handle typing.
	QWidget*     conPanel = new QWidget(tabs);
	QVBoxLayout* conLay   = new QVBoxLayout(conPanel);
	conLay->setContentsMargins(2, 2, 2, 2);
	QPlainTextEdit* conOut = new QPlainTextEdit(conPanel);
	conOut->setReadOnly(true);
	conOut->setFont(QFont("Consolas", 10));
	conOut->setPlaceholderText("Julia output appears here. `fig` is this window. e.g.  add!(fig, [x y z]; mode=:points)");
	QLineEdit* conIn = new QLineEdit(conPanel);
	conIn->setFont(QFont("Consolas", 10));
	conIn->setPlaceholderText("julia>  (Enter to run)");
	conLay->addWidget(conOut, 1);
	conLay->addWidget(conIn, 0);
	conPanel->setLayout(conLay);
	tabs->addTab(conPanel, "Julia Console");
	s->console = conOut;
	QObject::connect(conIn, &QLineEdit::returnPressed, [s, conOut, conIn]() {
		const std::string cmd = conIn->text().toStdString();
		if (cmd.empty())
			return;
		conIn->clear();
		conOut->appendPlainText(QString("julia> ") + QString::fromStdString(cmd));
		if (!g_juliaEval) {
			conOut->appendPlainText("(no Julia eval callback registered — re-include bridge.jl)");
			return;
		}
		static std::vector<char> buf(1 << 16);   // 64 KB result buffer (shared, reused)
		int n = g_juliaEval(s, cmd.c_str(), buf.data(), (int)buf.size());
		if (n > 0)
			conOut->appendPlainText(QString::fromUtf8(buf.data(), n));
	});

	// Tab 2 — Data Viewer: a read-only spreadsheet for a GMTdataset / plain matrix / vector,
	// filled from Julia via gmtvtk_set_table (e.g. show_table(fig, D)).
	s->dataTable = new QTableWidget(tabs);
	s->dataTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	s->dataTable->setAlternatingRowColors(true);
	s->dataTable->horizontalHeader()->setStretchLastSection(true);
	tabs->addTab(s->dataTable, "Data Viewer");

	// "Hide" corner button: collapse the panel body (extend the 3-D view) / restore it.
	QToolButton* hideBtn = new QToolButton(tabs);
	hideBtn->setText("Hide");
	hideBtn->setAutoRaise(true);
	hideBtn->setToolTip("Collapse this panel to extend the 3-D view");
	tabs->setCornerWidget(hideBtn, Qt::TopRightCorner);
	s->bottomHideBtn = hideBtn;
	QObject::connect(hideBtn, &QToolButton::clicked, [s]() { setBottomCollapsed(s, !s->bottomCollapsed); });

	// View-menu items: show the dock, un-collapse it, and bring the matching tab forward.
	auto showTab = [s](QWidget* page) {
		if (s->bottomDock) s->bottomDock->setVisible(true);
		setBottomCollapsed(s, false);
		if (s->bottomTabs) s->bottomTabs->setCurrentWidget(page);
	};
	mView->addAction("&Profile Panel",       [showTab, s]()        { showTab(s->prof); });
	mView->addAction("Julia &Console Panel", [showTab, conPanel]() { showTab(conPanel); });
	mView->addAction("&Data Viewer Panel",   [showTab, s]()        { showTab(s->dataTable); });

	// Default window large enough that the LEFT (Scene Objects) and RIGHT (Shading) side docks
	// both get real width. Without this the window opens at its minimum and the central VTK view
	// squeezes the right dock to ZERO width -> the Shading dock is invisible ("no docks").
	win->show();

	// interactor must be live before we attach observers
	widget->renderWindow()->Render();
	applyShading(s);   // first real lighting/material/pass setup (IBL + PBR + passes)
	auto *iren = widget->interactor();

	// Explicit trackball camera style so right-drag = dolly and wheel = zoom.
	// (Left-drag is owned by the gizmo's DragCB; middle is owned by MiddleCB below.)
	vtkNew<vtkInteractorStyleTrackballCamera> style;
	iren->SetInteractorStyle(style);

	// Middle button (pan on drag, recenter on drag-less click) is handled directly in the
	// GLView widget subclass — VTK's interactor adapter never delivers the middle button to
	// observers here, but Qt delivers it to the widget. MiddleCB/MidPanFilter are dead code.

	// Coordinate readout (default priority). The gizmo's high-priority drag
	// observer aborts the event when it grabs a handle, so this won't double-fire.
	vtkNew<vtkCallbackCommand> moveCB;
	moveCB->SetCallback(onMouseMove);
	moveCB->SetClientData(s);
	iren->AddObserver(vtkCommand::MouseMoveEvent, moveCB);

	// Keep the horizontal Z billboards on the camera-near vertical edge as the view rotates.
	vtkNew<vtkCallbackCommand> axisCB;
	axisCB->SetCallback(AxisLabelCB);
	axisCB->SetClientData(s);
	s->ren->AddObserver(vtkCommand::StartEvent, axisCB);

	// Gizmo: scale cone + tilt ring + compass ring at the rotation centre.
	// Owns its own LeftButton/MouseMove observers at priority 10 and the 'x' toggle.
	s->giz = enableGizmo(s, 0.01);
	// Polygon draw/edit tool: gestures are handled in the GLView widget (mouse*Event overrides,
	// 60_profile.cpp), gated on the tool state, so navigation is untouched when the tool is idle.
	// non-blocking: return now; the host pumps gmtvtk_process_events().
	return s;
}

