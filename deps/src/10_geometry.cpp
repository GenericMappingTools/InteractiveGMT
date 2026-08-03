struct Gizmo;     // Fledermaus-style scale/tilt/azimuth handle (defined below)
struct QuadNode;  // tiled-LOD pyramid node (defined below Scene)
struct Scene;     // the per-window scene (defined below; forward-declared for the line-tool decls)

// GRAPHICAL ELEMENT: custom dock title bar that folds the dock HORIZONTALLY.
// Open  -> a normal horizontal strip: "▾ Title" across the top.
// Folded -> a thin vertical strip (~one text-height wide) with "▸" at the top and
// the Title painted rotated 90° (reading bottom->top) down the window edge, so the
// collapsed dock costs only its strip width instead of leaving its full open width
// as dead, unusable space. Clicking anywhere on the bar toggles via onClick.
// (No Q_OBJECT: this TU has no moc — we override virtuals and call a std::function.)
// Defined here (shared, early fragment) so both the 3-D viewer's Scene Objects dock
// (70_window.cpp) and the X,Y plot tool's Object Manager dock (65_xyplot.cpp) fold through
// the SAME mechanism — one dock-fold idiom, not a per-window reimplementation.
struct FoldTitleBar : QWidget {
	QString title;
	bool    folded    = false;
	int     openWidth = 0;            // dock width remembered at fold time, restored on un-fold
	std::function<void()> onClick;
	explicit FoldTitleBar(const QString &t, QWidget *parent = nullptr)
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

// A QLabel that opens something on a plain LEFT click — used for the Scene Objects row labels:
// left-clicking the element description pops its properties menu (onClick gets the GLOBAL point).
struct ClickableLabel : QLabel {
	std::function<void(const QPoint&)> onClick;        // LEFT click  -> properties / fold
	std::function<void(const QPoint&)> onRightClick;   // RIGHT click -> context menu (e.g. Save…)
	std::function<void()>              onDoubleClick;  // LEFT double click -> reopen / raise (parked X,Y plots)
	using QLabel::QLabel;
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && onClick) onClick(e->globalPosition().toPoint());
		else if (e->button() == Qt::RightButton && onRightClick) onRightClick(e->globalPosition().toPoint());
		else QLabel::mousePressEvent(e);
	}
	void mouseDoubleClickEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && onDoubleClick) onDoubleClick();
		else QLabel::mouseDoubleClickEvent(e);
	}
};

// A caller-supplied GMTdataset drawn over the surface as lines or points. Each carries
// its own vtkActor so the right-click context menu can retune its colour / line style /
// point size live. Mode: 0 = points, 1 = lines.
struct Overlay {
	vtkSmartPointer<vtkActor> actor;
	int mode = 1;
	vtkSmartPointer<vtkPolyData> baseLine;   // the mapper input polydata (points always; line- or vert-cells per mode)
	vtkSmartPointer<vtkTexture>  stripeTex;  // 1-D stipple texture (kept alive) for dashed/dotted
	int lineStyle = 0;                       // 0 solid, 1 dashed, 2 dotted (so colour edits rebuild it)
	std::string name;                        // label shown in the Scene Objects panel
	int    stack = 0;                        // draw-order rank in the shared vector pile (higher = on top)
	std::vector<int> segoff;                 // per-segment start offsets (nseg+1 entries) -> rebuild cells on line<->points toggle
	int    nseg = 0;                         // segment count (segoff has nseg+1 entries)
	std::string groupName;                   // Scene Objects group tag ("" = top-level, ungrouped); overlays
	                                          // sharing a non-empty groupName fold under ONE collapsible parent
	                                          // row in rebuildSceneObjects (e.g. Geography > Plate boundaries'
	                                          // 7 boundary-type layers under "Plate boundaries PB")
	std::vector<std::string> info;           // per-SEGMENT hover text (nseg entries, line mode only); empty =
	                                          // no hover info. Looked up by pickOverlayInfoAt via pickOverlayAt's
	                                          // segment index -- same hit-test the context-menu path already uses.
	std::vector<std::string> vertexInfo;     // per-VERTEX hover text (npts entries, line OR points mode); empty =
	                                          // no per-vertex info. The finer-grained twin of `info` above -- a
	                                          // dropped "x y text" table's own gmtread .text column (drop.jl's
	                                          // `_ds_vertex_texts`), one string per point, 1:1 with baseLine's
	                                          // vtkPoints. pickOverlayInfoAt checks this FIRST (via pickOverlayAt's
	                                          // vertex index), falling back to per-segment `info` otherwise.
	bool labelsShown = false;                 // "Show point labels" toggle (55_lineprops.cpp): billboard text
	                                          // labels built from vertexInfo, same mechanism city names use
	                                          // (gmtvtk_add_texts_h). Tracks whether they are currently added.
	std::string labelsGroup;                  // text group tag for the toggled labels (lazily set to
	                                          // "<name> (labels)"), so gmtvtk_remove_overlay_group_h can erase
	                                          // just them without touching this overlay itself.
	std::vector<double> interiorXYZ;         // SHAPENC "bounded ensemble" (OUT polygon + hidden point swarm,
	                                          // Mirone convention): the swarm's x,y,z, stashed here at add-time
	                                          // instead of being added to the scene -- empty = not a bounded
	                                          // OUT polygon, no "Plot interior points" menu item.
	bool interiorAdded = false;              // true once "Plot interior points" has added the swarm as its
	                                          // own overlay (that overlay then owns its own Scene Objects row,
	                                          // checkbox and Delete -- no separate toggle state needed here).
	bool isShapencBoundary = false;          // a SHAPENC OUT/IN polygon (drop.jl's _add_shapenc_bounded):
	                                          // a coverage boundary, not a measurable line -- suppresses
	                                          // "Line length…"/"Azimuth…"/"Convert to points" in its context
	                                          // menu (same kind of source-intrinsic distinction as Polygon's
	                                          // isFault/isSlip/nestKind, not a per-call-site special case).
	bool isShapencInteriorPoints = false;    // the points overlay "Plot interior points" itself added (its
	                                          // OWN row, not the OUT polygon it came from) -- adds "Quick
	                                          // grid" (Auto/Set increments…, gridding not implemented yet)
	                                          // and "Point cloud view" (real 3-D, view_points) to its menu.
	// Display-only holes for annotations (Grid Tools > Contours). `gapAnchors` are GLOBAL vertex
	// indices where a label sits; `gapHalfPx` is how far the hole reaches each side of one, in SCREEN
	// PIXELS. Kept in pixels, not world units, so the hole can be re-cut whenever the zoom changes and
	// stay exactly as wide as the (screen-constant) label that sits in it. The points and segment
	// offsets are never touched — only the drawn line cells (overlayRebuildGapCells, 50_scene.cpp).
	std::vector<int> gapAnchors;
	double gapHalfPx = 0.0;
	// This line stands for ONE value of the window's grid (a contour level), so colouring it through
	// the grid's own colormap is meaningful. Set by Grid Tools > Contours; it is what puts "Color by
	// grid colormap" on the group's properties menu, instead of the menu having to recognise the
	// group by name.
	bool cptColorable = false;
	bool noConvertToPoints = false;          // suppresses ONLY "Convert to points"/"Convert to line" in the
	                                          // context menu, unlike isShapencBoundary which also drops
	                                          // "Line length…"/"Azimuth…" -- for lines where scattering to
	                                          // points makes no sense but length/azimuth still does (e.g.
	                                          // Geography > Magnetic isochrons > GPlates).
	bool zIsPlaceholder = false;             // true when the caller's source data was 2-column (x,y only)
	                                          // and z=0 was a stored-geometry placeholder, NOT real data --
	                                          // showLineDataTable (55_lineprops.cpp) must not invent/show a Z
	                                          // column for these (e.g. Magnetic isochrons > GPlates, a 2-col
	                                          // GPlates export). Default false preserves existing overlays'
	                                          // "always show Z" behaviour (coastlines/plate boundaries/...)
	                                          // until each is confirmed to genuinely have no z of its own.
	bool noDataTable = false;                // suppresses "Show data table…" entirely -- for overlays whose
	                                          // per-vertex table is meaningless to the user (e.g. Geophysics >
	                                          // Magnetics > Import *.gmt/*.nc cruise tracks: thousands of raw
	                                          // nav fixes, no useful per-row content). Default false preserves
	                                          // every existing overlay's table.
};

// A generic SCREEN-CONSTANT symbol layer (volcanoes, seismicity, cities, …): N glyphs of one
// shape (GMT symbol code) stamped at N points by a vtkGlyph3D. The glyph source is a UNIT shape
// in the XY plane (radius 0.5); a per-frame observer rescales `glyph->SetScaleFactor` so the
// on-screen size stays `sizePx` pixels at any zoom (same camera math the gizmo uses). Point
// positions are pre-baked with the surface's xfac (x*xfac) so the glyph itself is NOT distorted;
// the actor carries only the z scale (1,1,zfac*ve) so symbols ride VE like the other overlays.
// Solid 3-D glyphs ("o" sphere / "u" cube) are a true volume, not flat in Z like the rest — the
// actor's (1,1,zfac*ve) scale would otherwise squash them into a near-zero-height pancake (zfac
// converts metres to degree-equivalent world units, typically ~1e-5), so their unit SOURCE is
// pre-scaled by `zfix` (Z *= 1/(zfac*ve), updated per-frame in symbolRescaleCB) to cancel that
// factor out before the actor re-applies it — net effect: the glyph reads as a true screen-
// constant-size ball/box in every axis, same as X/Y, while its CENTRE still sits at the real
// VE-scaled depth.
struct SymbolLayer {
	vtkSmartPointer<vtkActor>   actor;
	vtkSmartPointer<vtkGlyph3D> glyph;       // flat glyphs only: source(unit shape)+input(points); CPU-duplicates
	                                          // the source mesh per point, fine for small/flat shapes but O(N*mesh)
	                                          // memory — NOT used for solid3D (see glyphMapper).
	vtkSmartPointer<vtkGlyph3DMapper> glyphMapper;  // solid3D (sphere/cube) only: GPU-instanced glyphing —
	                                          // renders the SAME small source mesh N times without ever building
	                                          // one combined N*mesh polydata on the CPU. A large seismicity
	                                          // catalog (tens of thousands of events) with vtkGlyph3D's real
	                                          // per-event sphere tessellation was the actual "spheres are slow"
	                                          // bottleneck (point clouds bypass this entirely — just vertices).
	vtkSmartPointer<vtkTransform> zfix;             // solid3D only: Z-cancelling pre-transform on the source
	vtkSmartPointer<vtkTransformPolyDataFilter> zfixFilter;  // solid3D only: applies `zfix` to the unit source
	double sizePx = 8.0;                      // requested on-screen size in PIXELS
	bool   filled = true;                     // filled polygon glyph (fill+edge) vs open line glyph (edge only)
	bool   solid3D = false;                   // true for "o" sphere / "u" cube (needs the zfix counter-scale)
	std::string sym  = "c";                   // GMT symbol code (for the Scene Objects label / properties)
	std::string name;                         // label shown in the Scene Objects panel
	std::vector<std::string> info;            // per-point hover text (multi-line); empty = no hover info
	int    stack = 0;                          // draw-order rank in the shared vector pile (higher = on top)
	bool   oneShot = false;                   // placed by the Symbols draw tool: exactly ONE point, whole-
	                                           // layer double-click-then-drag moves that single point
	                                           // (see symLayerDrag) — false for batch layers (volcanoes etc)
};

// SymbolLayer carries exactly ONE of glyph (flat shapes) / glyphMapper (solid3D sphere/cube) — every
// generic per-layer feature (hover data table, point editing, shape-menu, colour recolouring) goes
// through these two accessors instead of hard-coding `sl.glyph`, so it works for both pipelines.
static vtkPolyData *symInputPD(SymbolLayer &sl) {
	if (sl.glyphMapper) return vtkPolyData::SafeDownCast(sl.glyphMapper->GetInput());
	if (sl.glyph)       return vtkPolyData::SafeDownCast(sl.glyph->GetInput());
	return nullptr;
}
static vtkPolyData *symSourcePD(SymbolLayer &sl) {
	if (sl.glyphMapper) return sl.glyphMapper->GetSource(0);
	if (sl.glyph)       return vtkPolyData::SafeDownCast(sl.glyph->GetSource());
	return nullptr;
}
static void symTouchSource(SymbolLayer &sl) {         // mark the glyph pipeline dirty after an in-place edit
	if (sl.glyphMapper) sl.glyphMapper->Modified();
	if (sl.glyph)       sl.glyph->Modified();
}

// A Fledermaus-style vertical "curtain": a textured wall hung along an XY track.
struct Curtain {
	vtkSmartPointer<vtkActor> actor;
	std::string name;                        // label shown in the Scene Objects panel
};

// An INDEXED image's palette, as a colour bar. The palette IS the legend — one labelled block per
// class (K-means classification's classes, or any indexed raster's colour table), which is why the
// bar is DISCRETE and its labels are the pixel VALUES, not interpolated tick numbers. Carried by
// whichever object holds the image: `Scene::palette` for a window's primary image, `ExtraObj::palette`
// for a dropped/derived one. `n == 0` = this object is not an indexed image and has no legend.
struct PaletteLegend {
	vtkSmartPointer<vtkLookupTable> lut;
	int  n = 0;
	bool show = true;                        // the Color Bar row's per-object "want it shown" intent
};

// ============================ AxesSet =======================================
// SACRED_LAW.md "Raster-own-axes law", in its final and absolute form: EACH RASTER HAS ITS OWN
// AXES. PERIOD. Not one shared window cube re-framed to whoever loaded last -- a real, separate
// set of axes OWNED BY the raster's own master handle container: its own cube, its own tick
// marks, its own X/Y/Z number billboards, its own axis-name titles, and its own FRAME (the true
// data limits, in ITS OWN units, that those numbers annotate). It is STRICTLY PROHIBITED for any
// raster to reuse, inherit, re-frame or hide another raster's axes: the base surface holds
// `Scene::baseAxes`, every dropped/derived grid, image or mesh holds its own `ExtraObj::ax`, and
// a Scene Objects "Axes" row toggles THAT set and nothing else.
//
// Everything the axes need lives HERE, so there is no window-level axis state left for one handle
// to reach into. x0..z1 are TRUE DATA coordinates (never the scaled actor space) -- `axesScaledBox`
// is the ONE place they are turned into the drawn xfac/zfac*ve box, so every consumer of a set's
// geometry reads the same source (SACRED_LAW.md: fix the shared source, never each call site).
struct AxesSet {
	vtkSmartPointer<vtkCubeAxesActor> cube;      // the box + gridlines (native labels/ticks OFF)
	vtkSmartPointer<vtkActor>         ticks;     // our own single outward tickmarks
	vtkSmartPointer<vtkPolyData>      tickPD;    // ... their geometry (rewritten every render)
	std::vector<vtkSmartPointer<vtkBillboardTextActor3D>> xlab, ylab, zlab;  // the value NUMBERS
	vtkSmartPointer<vtkBillboardTextActor3D> title[2];   // the X/Y axis NAME titles
	std::string name[2] = { "X", "Y" };          // ... their text, from THIS raster's own `geog`
	// THIS raster's own frame, in TRUE data coordinates. z0/z1 are in ITS OWN UNITS (a mGal anomaly
	// over a metre bathymetry keeps mGal), which is the whole point of the Z half of the law.
	double x0 = 0, x1 = 1, y0 = 0, y1 = 1, z0 = 0, z1 = 1;
	int    geog  = 0;        // THIS raster's own x,y kind: != 0 -> lon/lat titles, 0 -> X/Y
	bool   shown = true;     // the owning handle's "Axes" checkbox (its OWN intent)
	bool   built = false;    // actors created + added to the renderers
};

// An extra dataset dropped into an existing window (a second grid/image surface). Listed in
// the Scene Objects panel with its own show/hide checkbox. `drape` is its optional image actor.
struct ExtraObj {
	vtkSmartPointer<vtkActor> actor;
	// This raster's OWN axes (Raster-own-axes law above). Built when the extra is adopted; torn
	// down with it. NEVER shared with, and never driven by, any other handle in the window.
	AxesSet ax;
	vtkSmartPointer<vtkActor> drape;
	vtkSmartPointer<vtkTexture> tex;         // dropped-image texture (reused to rebuild flat plane / drape)
	std::string name;                        // label shown in the Scene Objects panel (file name)
	bool   isImage = false;                  // dropped IMAGE (flat plane / drapeable) vs grid surface
	bool   isMesh  = false;                  // a POLYGON MESH layer (a VTK .vtp/.vtu surface, a GMTfv
	                                         // solid) added into this window as an extra. Carries no
	                                         // gridZ and no LUT, so it never resolves as the active
	                                         // grid and its Scene Objects group has no Color Bar row —
	                                         // otherwise an ordinary non-image extra in every respect
	                                         // (own checkbox, own stacking rank, own axes on adoption).
	bool   draped  = false;                  // image currently draped on the host grid (else a flat plane)
	bool   drapeApplied = false;             // the drape's automatic side effects (this image's own Axes
	                                         // off; the HOST GRID unchecked, above all its axes) have
	                                         // already fired for the CURRENT drape episode -- see
	                                         // imageRebuildActor. Latched so the every-rebuild calls
	                                         // (stacking, VE) cannot keep re-clearing what the user has
	                                         // since ticked back on by hand.
	double zpos    = 0.0;                    // flat-plane TRUE z — sits above/below the relief, NEVER at z=0
	double bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;  // image footprint (true coords): tcoords + grid-overlap test
	int    gstack  = 0;                      // GRID draw-order rank in the grid pile (base relief + grids)
	int    tag     = 0;                      // UNIQUE, STABLE group tag (assigned once at creation from
	                                         // Scene::gridTagSeq, never reused). The Color Bar row carries
	                                         // this tag so a recolour always hits THIS grid, regardless of
	                                         // the grid's current index in s->extras (which shifts on delete).
	// Per-grid DATA layer + colour state (grids only). A dropped grid is a first-class layer: it carries
	// its own full-res z (for the hover/coordinate readout when it is the active/displayed grid) and its
	// own LUT + z range (so the single rendered colorbar can be retargeted to it). Mirrors the base
	// surface's Scene-level gridZ / surfLut / zmin-zmax. Empty for images.
	std::vector<float> gridZ;                // full-res z, column-major z[i*gny+j] (same layout as base)
	int    gnx = 0, gny = 0;
	double gx0 = 0, gx1 = 1, gy0 = 0, gy1 = 1;
	double zmin = 0, zmax = 0;               // this grid's own z range (drives its colorbar)
	vtkSmartPointer<vtkScalarsToColors> lut; // this grid's colour map (for the retargeted colorbar)
	bool   showBar = true;                   // user wants this grid's colorbar shown (when it is active)
	int    cubeLayers = 0;                   // >1 iff this grid is a 3-D-cube variable (its menu offers
	                                         // "Cube layers…", opening the slider bound to THIS cube)
	PaletteLegend palette;                   // images only: an indexed image's class legend (see above)
	int    geog = 0;                         // this grid's OWN x,y kind: !=0 -> lon/lat, 0 -> cartesian X/Y.
	                                         // Mirrors the base surface's Scene-level baseGeog. The axis
	                                         // NAME titles follow the ACTIVE grid's flag, so a cartesian
	                                         // derived grid (a gravmag3d anomaly computed with the dialog's
	                                         // "Geographic" UNCHECKED) is never labelled lon/lat just
	                                         // because the window's parent grid happened to be geographic.
};

// A user-drawn polygon (closed polyline) from the toolbar polygon tool. Vertices are kept in
// TRUE coords; the line actor is hung in the surface's scaled space (xfac,1,zfac*ve), so it
// tracks VE like the other overlays. Built by 85_polygon.cpp.
struct Polygon {
	std::vector<std::array<double,3>> v;     // vertices, TRUE coords; closed ring (first == last) when closed
	vtkSmartPointer<vtkActor>    line;       // the polyline actor
	vtkSmartPointer<vtkPolyData> linePD;     // its geometry (rebuilt as vertices move)
	vtkSmartPointer<vtkActor>    fill;       // filled face (closed rects/polygons only); colour+opacity INDEPENDENT of the outline
	vtkSmartPointer<vtkPolyData> fillPD;
	double fillColor[3] = { 1.0, 0.55, 0.0 };// fill colour (default orange, matches outline); editable in Line Properties
	double fillOpacity  = 0.0;               // fill transparency (0 = no fill -> outline-only look preserved by default)
	std::string name;                        // label shown in the Scene Objects panel ("polygon N")
	std::string groupName;                   // when non-empty, polys sharing it fold under ONE collapsible Scene Objects node (e.g. "Slip model" — Import Model Slip patches)
	bool closed = true;                      // closed ring (polygon/rect/circle) vs open chain (polyline)
	bool isRect = false;                     // drawn with a rectangle tool (SH_Rect/SH_RectN): vertex edits stay axis-aligned
	bool isFault = false;                    // drawn with the Draw Fault tool (SH_Fault): props hold the elastic-deformation dialog
	double faultSlip = std::nan("");         // dislocation slip in METERS (set when imported from a sub-fault file; NaN = unknown -> dialog default)
	double faultRake = std::nan("");         // dislocation rake in DEGREES (set on import; NaN = unknown -> dialog default)
	double faultStrike   = std::nan("");     // strike in DEGREES (file mean, set on import; NaN = unknown -> seed from trace azimuth)
	double faultDip      = std::nan("");     // dip in DEGREES (file mean, set on import; NaN = unknown -> dialog default)
	double faultWidth    = std::nan("");     // TOTAL down-dip width ny·Dy (km geog / data units, set on import; NaN = unknown)
	double faultDepthTop = std::nan("");     // depth to top of the shallowest patch in km (set on import; NaN = unknown)
	double faultLength   = std::nan("");     // along-strike patch length Dx (km) — Import Model Slip patches (NaN = unknown)
	bool   isSlip = false;                   // Import Model Slip patch (a rectangular sub-fault): opens the elastic dialog + lists in its Faults combo
	int    slipSeg = -1;                     // fault-segment index of a slip-model patch (-1 = not a slip patch; usually 0)
	vtkSmartPointer<vtkActor>    faultPlane; // gray surface-projection patch of the dipping fault plane (sits BELOW the trace)
	vtkSmartPointer<vtkPolyData> faultPlanePD;
	vtkSmartPointer<vtkActor>    faultPlane3D; // the actual dipping fault plane in 3-D (top buried at the deepest trace point; 3-D-only)
	vtkSmartPointer<vtkPolyData> faultPlane3DPD;
	vtkSmartPointer<vtkActor>    faultArrows; // flat slip-direction arrows imprinted on each face of the 3-D plane (rake / rake+180)
	vtkSmartPointer<vtkPolyData> faultArrowsPD;
	bool   faultPlane3DShown = true;         // user's desired visibility for the buried plane (actual visibility is this AND not flat-2D)
	int    stack = 0;                        // draw-order rank in the shared vector pile (higher = on top)
	bool   isMeca = false;                   // focal-mechanism beachball patch: excluded from the shared vector
	                                          // pile (gatherStackItems) — mecaBuildPatch already gives every
	                                          // patch (fill AND line) its own consistent cross-event rank offset;
	                                          // applyVectorStacking's generic per-item ramp would otherwise
	                                          // overwrite the LINE's offset (it never touches fill) with an
	                                          // unrelated global order, letting an occluded event's outline
	                                          // bleed through the opaque fill of the one covering it.
	int    nestKind = 0;                     // 0 = ordinary shape; 1 = "Nested grids" rectangle (special menu)
	double nestXi = 0, nestYi = 0;           // child cell sizes (0 = inherit parent inc; resolved by nestReflow)
	int    nestReg = 0;                       // 0 grid / 1 pixel registration (carried into COMCOT/NSWING info)
	int    nestIx0 = 0, nestIx1 = 0;          // parent-grid node indices of the snapped W/E edges (1-based on display)
	int    nestIy0 = 0, nestIy1 = 0;          // parent-grid node indices of the snapped S/N edges
};

// Cached compression/dilatation/rim colour + rim width for ONE focal-mechanism batch (keyed by its
// Scene Objects groupName) — lets the group's properties dialog pre-fill from the LAST-applied
// values without asking Julia (the actual re-plot on Apply still round-trips through Julia, since a
// new rim width needs fresh geodesic geometry — see gmtvtk_set_meca_group_props_h).
struct MecaGroupProps {
	std::string name;
	double compColor[3]   = { 0.0, 0.0, 0.0 };
	double dilatColor[3]  = { 1.0, 1.0, 1.0 };
	double rimColor[3]    = { 0.0, 0.0, 0.0 };
	double rimWidthPct    = 1.0;             // percent of disk radius (dialog units; Julia wants a 0..1 fraction)
	// Per-event date label ("Plot event date"), OFF by default — matches the import dialog's own
	// chkPlotEventDate default (unchecked). Font fields mirror TextLabel's so textApplyProps' font
	// family set ("Arial"/"Courier"/"Times") stays the single source of truth for valid values.
	bool   plotDate       = false;
	std::string dateFont  = "Arial";
	int    dateFontSize   = 7;               // small — a catalog's dates sit right above each ball, one per event
	double dateColor[3]   = { 0.0, 0.0, 0.0 };   // black — yellow (TextLabel's own default) washes out
	                                              // against light relief/basemap backgrounds, illegible
	bool   dateBold       = false;
	bool   dateItalic     = false;
	// The group's OWN properties dialog, if currently open (nullptr otherwise). QPointer auto-nulls
	// when the dialog is destroyed (WA_DeleteOnClose) — mecaGroupPropsDialog (50_scene.cpp) checks
	// this before building a new one, so re-clicking the Scene Objects row raises the existing window
	// instead of stacking a fresh duplicate on top of it every time (2026-07-05 bug).
	QPointer<QDialog> propsDlg;
};

// One focal-mechanism beachball's DRAG state. A ball = 2 filled sector actors (comp/dilat) + 1
// stroked rim/nodal-line actor, grouped by EVENT (evid/3 — see gmtvtk_add_meca_h/mecaBuildPatch,
// evid = ei*3+role). Dragging must move all of an event's actors together and leave a thin line
// from the ball's ORIGINAL plotted position to wherever it was dropped — the same epicenter-to-
// symbol convention _focal_plot already draws statically for lon0/lat0 anchor columns, here drawn
// live from a mouse drag instead of read from a file column. `actors` are RAW, non-owning pointers
// (the owning Polygon in s->polys keeps them alive) — never a cached index into s->polys, which can
// shift under an unrelated erase elsewhere (see polyIndexOfActor's "never cache" convention).
struct MecaBall {
	std::string groupName;              // batch name ("Focal mechanisms"), matches Polygon::groupName
	int    event = -1;                  // ei (evid/3)
	double x0 = 0, y0 = 0;               // ORIGINAL plotted centre (same coord convention as Polygon::v)
	double offX = 0, offY = 0;          // cumulative drag offset from x0/y0 (same convention); 0,0 = never moved
	double radius = 0;                  // on-screen radius reference (same convention), for hit-testing
	double zLow = 1e30;                 // MIN baked Z over every rank this event contributed (mecaBuildPatch/
	                                     // Lines' z0+rank*kMecaRankZStep) — the anchor line/dot render a shade
	                                     // BELOW this so the ball's own opaque fill always occludes them where
	                                     // it currently sits (real depth test, no polygon-offset — matches
	                                     // mecaBuildLines' own cross-primitive-safe technique, not
	                                     // polyMakeLineActor's terrain line-offset).
	std::string info;                   // hover metadata (date/magnitude/depth), gmtvtk_set_meca_infos_h
	std::vector<vtkActor*> actors;      // this event's fill(s) + line actor
	vtkSmartPointer<vtkActor>    anchor;     // drag-trail LINE, built lazily on first drag
	vtkSmartPointer<vtkPolyData> anchorPD;
	vtkSmartPointer<vtkActor>    anchorDot;  // small filled dot marking the ORIGINAL point (never moves)
	vtkProp3D *dateLabel = nullptr;          // this event's "Plot event date" label (TextLabel::mecaEvent
	                                         // links it back here — gmtvtk_add_meca_h wires this pointer),
	                                         // nullptr if plotdate was off. RAW, non-owning (s->texts owns
	                                         // it); mecaDragTo repositions it alongside the ball's own actors.
	                                         // Only SetPosition (vtkProp3D) is ever called on it, so the
	                                         // billboard-vs-flat concrete type underneath never matters here.
};

// A text label: either a user-placed toolbar annotation (the "T" Text tool), or a batch-owned
// label (Focal mechanisms' per-event date, Geography > Cities' name labels). ALWAYS a
// vtkBillboardTextActor3D — always faces the camera at a constant screen size, like the cube's
// tick-number labels (STANDING RULE, 2026-07-24: the old plain/flat vtkTextActor3D rendering, which
// lay flat on the z=0 plane and world-scaled with view extent, is RETIRED for every text label, no
// exceptions — it read as if painted into the terrain/basemap texture from most view angles, and
// went huge/distorted at a whole-earth zoom; see .wolf/cerebrum.md's Decision Log for the history).
// `groupName` (non-empty = owned by a batch) governs BATCH membership only now — bulk find/erase,
// folding its Scene Objects row under the batch's own row instead of listing one row per label
// (rebuildSceneObjects), and bottom- vs centre-vertical-justification (textApplyProps) — it no
// longer selects the render kind. Every label (grouped or not) is directly click-draggable on the
// plane (polyHitText) and gets its own right-click properties (textLabelMenu for standalone labels,
// batchTextLabelsDialog — with a this-one/whole-group scope choice — for batch-owned ones). Stored
// in TRUE coords (x,y); the actor sits in the surface's scaled space (x*xfac).
//
// ONE EXCEPTION to the billboard rule, added 2026-07-27 on explicit instruction: a CONTOUR label
// must READ ALONG ITS CONTOUR (rotated to the local line direction) and sit at the contour's own
// height — neither of which a billboard can do, since it is by definition always upright and always
// facing the camera. Those labels set `flat` and are rendered by vtkTextActor3D lying in the XY
// plane, rotated by `angle` about Z, positioned at the real `pos[2]`, and world-scaled so their size
// matches the gap cut for them in the line. Nothing else may set `flat`: the standing rule above
// still governs every user-placed and batch label.
struct TextLabel {
	std::array<double,3> pos;                // anchor, TRUE coords; z = 0 for plane labels, the annotated
	                                          // height for a contour label (applyVE scales it by zfac*ve)
	vtkSmartPointer<vtkProp3D> actor;         // vtkBillboardTextActor3D, or vtkTextActor3D when `flat`
	double angle = 0.0;                      // `flat` only: rotation about Z in degrees, from the line's own
	                                          // direction, so the text reads along what it annotates
	double wscale = 0.0;                     // `flat` only: world units per screen pixel at build time — the
	                                          // actor's uniform scale, which is what makes a pixel-designed
	                                          // font come out the size the caller measured in world units
	bool   flat = false;                     // see the EXCEPTION above; never set for a normal label
	double offX = 0.0, offY = 0.0;           // `flat` only: shift from the anchor to the actor's own origin,
	                                          // so the glyphs end up CENTRED on the anchor whatever corner
	                                          // vtkTextActor3D measures itself from (textApplyProps measures
	                                          // the real quad and fills this in; applyVE reuses it)
	std::string text;                        // the shown string (rendered in the scene)
	std::string name;                        // short Scene Objects label ("Text N")
	std::string font  = "Arial";             // VTK font family: "Arial" / "Courier" / "Times"
	int    size  = 18;
	double color[3] = { 0.0, 0.0, 0.0 };     // default: black
	bool   bold = false, italic = false;
	std::string groupName;                   // non-empty = owned by a batch (e.g. focal-mechanism date
	                                          // labels); tags it for bulk find/erase (deleteMecaGroup) and
	                                          // folds its Scene Objects row under the batch's own row
	                                          // instead of listing one row per label (rebuildSceneObjects)
	bool   vcenter = false;                  // force CENTRED vertical justification even for a batch-owned
	                                          // label (textApplyProps otherwise bottom-justifies those, so
	                                          // a focal-mechanism date grows upward off its ball). A contour
	                                          // annotation has to straddle its line, not sit above it.
	int mecaEvent = -1;                      // valid iff groupName non-empty: the 0-based event index
	                                          // (evid/3) this label belongs to — gmtvtk_add_meca_h uses it
	                                          // to wire MecaBall::dateLabel so a drag carries the label along
};

// A handle to one line-like scene object for the shared Line Properties tool (55_lineprops.cpp).
// `kind` selects how style (solid/dashed/dotted) is applied (each line type stipples differently);
// `actor` is the renderable. Reachable by right-click on the line OR on its Scene Objects row.
enum LineKind { LK_Profile, LK_Overlay, LK_Polygon };
struct LineRef {
	LineKind  kind;
	vtkActor *actor = nullptr;
};
static void showLineProperties(Scene *s, const LineRef &lr);                 // the properties dialog
static void popupLineObjectMenu(Scene *s, const LineRef &lr, const QString &name, const QPoint &gp);
static void applyVectorStacking(Scene *s);                      // shared vector-pile draw-order (50_scene.cpp)
static void restackVector(Scene *s, int *stackPtr, int op);    // move one vector element through the pile
static void applyGridStacking(Scene *s);                        // grid-pile draw-order: base relief + grids (50_scene.cpp)
static void restackGrid(Scene *s, int *stackPtr, int op);      // move one grid through the grid pile
static void lineApplyStyle(Scene *s, const LineRef &lr, int style);
static int  lineCurrentStyle(Scene *s, const LineRef &lr);
static void polygonDelete(Scene *s, vtkActor *lineActor);                    // remove a finished polygon
static void overlayDelete(Scene *s, vtkActor *a);                            // remove an overlay line/point (50)
static void overlayDeleteGroup(Scene *s, const std::string &groupName);      // remove every overlay tagged with groupName (50)
static int addTextsBatch(Scene *s, const double *xy, const char *texts, int n,
                          double r, double g, double b, int size, const char *font,
                          int bold, int italic, const char *groupName, const int *eventIdx,
                          int vcenter, const double *z, const double *angleDeg, int flat);  // batch text labels (90)
static void polyRebuildLine(Scene *s, Polygon &pg);                         // rebuild a polygon actor from pg.v (85)
static void polyRebuildFill(Scene *s, Polygon &pg);                         // rebuild a closed polygon's filled face (85)
static int  polyIndexOfActor(Scene *s, vtkActor *a);                        // index of polygon whose line==a, or -1 (55)
static bool lineClosedRing(Scene *s, const LineRef &lr);                    // closed polygon ring? (55)
static int  polyHitPolygon(Scene *s, int x, int y, double tol);             // polygon under cursor? (85)
static void nestReflow(Scene *s, bool snap = true);                         // re-quantize "Nested grids" chain (85); snap=false = don't move verts, only recompute indices (restore)
static void nestNewChild(Scene *s);                                         // append a refined nested child (85)
static void nestSetRect(Scene *s, Polygon &pg, double x0, double x1, double y0, double y1);  // force a rect's ring to these axis-aligned limits + rebuild (85)

// Per-side illumination snapshot for an Aquamoto layer. WATER and LAND are two SEPARATE images, each
// with its OWN light: editing the selected side updates only ITS snapshot, and bakeAquaShade re-bakes
// the OTHER side from its own (unchanged) snapshot -- so a water edit changes NOTHING of the land
// (no colour, no light), and vice versa. Only geometry (xfac/zfac/ve) is shared (read live from Scene).
struct AquaSideShade {
	bool   valid = false;
	bool   useHillshade = true, hillGrd = true, litBake = false;
	double lightAz = 315.0, lightEl = 45.0, hillAmbient = 0.3, hillGain = 2.0;
	double roughness = 0.45, lightIntensity = 1.0, fillIntensity = 0.4;
};

// ---- scene we hang onto for the callbacks / menu actions --------------------
struct Scene {
	vtkSmartPointer<vtkRenderer>          ren;
	vtkSmartPointer<vtkActor>             surf;
	// Tiled render (plain grid): the surface is split into tile actors held by `surfGroup`
	// (a vtkAssembly) so ONE transform (base scale + VE) drives them all and GetBounds unions
	// them. `tiles` lists the parts for per-actor ops (material / edges / colour map). Empty +
	// null for the single-actor paths (cloud / FV / drape / image) -> helpers fall back to surf.
	vtkSmartPointer<vtkAssembly>          surfGroup;
	std::vector<vtkSmartPointer<vtkActor>> tiles;
	vtkSmartPointer<vtkActor>             drape;   // optional image overlay (CPT base shows under transparent texels)
	// The NaN backdrop. A NaN is a DATA HOLE: the mesh builders emit no cell there, so the surface has
	// a real gap and nothing is left for the light to illuminate. This flat, UNLIT quad sits just under
	// the grid floor in the Preferences NaN fill colour, so a hole reads as that colour instead of as
	// the window background — and, being real geometry, it is what a click in a hole lands on.
	vtkSmartPointer<vtkActor>             nanPlane;
	double nanPlaneBox[6] = { 0, 0, 0, 0, 0, 0 };  // bounds it was last built for (skip idle rebuilds)
	// The BASE surface's OWN axes (SACRED_LAW.md Raster-own-axes law -- see AxesSet above). This is
	// the PRIMARY raster's set, not "the window's axes": it is owned by the base surface's own master
	// handle exactly as every ExtraObj owns `ex.ax`, and nothing else in the window may frame, hide or
	// re-label it. There is deliberately NO window-level axes state left here for a handle to reach.
	AxesSet baseAxes;
	// 3-D cube: pin the vertical axis box + Z tick labels to the WHOLE cube's z-range so the axes do
	// not shift as the user switches layers (each layer's own min/max differs). Set once per cube via
	// gmtvtk_set_cube_axes_zrange; cubeZMin/Max are UNSCALED data values (scaled by zfac*ve on use).
	bool   cubeZLock = false;
	double cubeZMin = 0.0, cubeZMax = 0.0;
	vtkSmartPointer<vtkRenderer>          axesRen;  // overlay layer (1) shared by ALL text/vector billboards (every raster's tick labels, the polygon handles, meca anchors): own headlight (even, view-independent text brightness) + own depth (never occluded by the surface); shares the main camera. A RENDER LAYER, not axis state.
	vtkSmartPointer<vtkScalarBarActor>    bar;       // coloured strip only
	vtkSmartPointer<vtkActor2D>           barTicks;  // our own tick-mark lines (strip has none in VTK 9.6)
	std::vector<vtkSmartPointer<vtkTextActor>> barLabels;  // our own tick numbers
	std::vector<double>                   barValues; // value at each tick/label
	vtkSmartPointer<vtkPoints>            barTickPts;// tick endpoints (rewritten in place on drag)
	double barX0 = 0.93, barY0 = 0.55;               // colorbar frame bottom-left, normalized (draggable)
	bool   barDragging = false;
	double barGrabX = 0, barGrabY = 0;               // mouse-to-origin offset while dragging
	vtkSmartPointer<vtkCellPicker>        picker;
	vtkSmartPointer<vtkPointPicker>       pointPicker;   // coordinate-readout fallback for a Verts-only
	                                                      // point cloud (view_points, "Point cloud view")
	                                                      // -- vtkCellPicker's ray-cell intersection never
	                                                      // hits a zero-area vertex cell, so a plain point
	                                                      // cloud got NO hover readout at all through
	                                                      // `picker` alone (SACRED_LAW.md: the readout must
	                                                      // work the same everywhere; this is the missing
	                                                      // hit-test for the one geometry kind that needs a
	                                                      // genuinely different picker, not a special case).
	QVTKOpenGLNativeWidget *widget = nullptr;
	QMainWindow *win    = nullptr;
	double ve = 1.0;            // vertical exaggeration (gizmo factor, 1 = true scale)
	double zmin = 0, zmax = 0;  // true (unscaled) z range
	double x0 = 0, x1 = 1;      // true x range (for cube-axis labels / readout)
	double y0 = 0, y1 = 1;      // true y range
	double xfac = 1.0;          // base X actor scale (cos(midlat) for geographic)
	double zfac = 1.0;          // base Z actor scale (true-scale unit conversion)
	// SACRED_LAW.md "derived-variable axes law": once a crop/derive reframes the window onto a
	// SUBREGION (gmtvtk_reframe_h), surfGetBounds() must report THAT subregion instead of the
	// primary surface's own full bounds — every bounds-driven function (applyVE's axes-cube resize,
	// fitSnapView's camera fit, rebuildAxisLabels' custom tick-label billboards — ALL THREE read
	// surfGetBounds, never s->baseAxes.cube directly) then automatically stays consistent with the reframe
	// with ZERO changes to any of them. false = untouched, normal behaviour (report the real actor).
	bool   viewBoundsOverride = false;
	double viewBounds[6] = { 0,0,0,0,0,0 };

	// --- full-resolution DATA layer (decoupled from the render geometry) -----
	// The grid's z kept once at full res, column-major z[i*gny + j] (GMT layout). Hover readout
	// and profile cross-sections sample THIS (bilinear), so they stay full-res no matter what the
	// render path draws (future tiled-LOD pyramid) — and need no vtkCellLocator over the surface.
	std::vector<float> gridZ;
	int    gnx = 0, gny = 0;
	double gx0 = 0, gx1 = 1, gy0 = 0, gy1 = 1;
	double gdx = 0, gdy = 0;                          // true-coord node spacing (tile build + SSE)

	// --- ACTIVE grid routing (multi-grid windows) ---------------------------
	// A window can hold several grids (base relief + dropped/computed grids). The "active" grid is the
	// topmost VISIBLE one; it drives the hover/coordinate readout and is the only grid whose colorbar is
	// shown. These point the readout at the active grid's data layer (null -> use the base gridZ above).
	// resolveActiveGrid()/refreshGridColorbar() (50_scene.cpp) set them on every add / toggle / restack.
	const std::vector<float> *actZ = nullptr;
	int    actNx = 0, actNy = 0;
	double actX0 = 0, actX1 = 1, actY0 = 0, actY1 = 1;
	// The colorbar's CURRENT display range — decoupled from zmin/zmax (which stay the base relief's range
	// for the cube axes / VE). buildColorbar sets these to the active grid's range; layoutColorbar reads
	// them so ticks place correctly when the bar is retargeted to a dropped grid.
	double barLo = 0, barHi = 1;
	bool   surfShowBar = true;              // base relief: user wants its colorbar shown (when active)
	bool   surfCloud = false;               // the PRIMARY surface is a point cloud (view_points /
	                                         // "Point cloud view" / a dropped .laz), NOT a heightfield:
	                                         // it colours by z through surfLut and owns a colorbar +
	                                         // Z axis exactly like a grid, but carries no gridZ layer.
	                                         // resolveActiveGrid honours it so the bar/axes/readout all
	                                         // describe it (a cloud used to resolve to "no active grid",
	                                         // and every refreshGridColorbar destroyed its colour bar).

	// --- Aquamoto dual colorbar (water / land) ------------------------------
	// A tsunami netCDF layer (customLayerTexture) needs TWO colour scales at once: `bar` above
	// (built exactly like any other grid's colorbar by showLayerImageTail) serves as the WATER bar;
	// this is a SEPARATE, persistent LAND bar for the (static, file-open-time) bathymetry range.
	// Only one is ever visible at a time, gated by aquaShowWater (mirrors the Aquamoto dialog's
	// Shade Water/Land radio) ANDed with each side's own Scene-Objects checkbox.
	vtkSmartPointer<vtkScalarsToColors>    aquaLandLut;      // keeps the land CTF alive (mirrors surfLut)
	vtkSmartPointer<vtkScalarBarActor>    aquaLandBar;
	vtkSmartPointer<vtkActor2D>           aquaLandBarTicks;
	std::vector<vtkSmartPointer<vtkTextActor>> aquaLandBarLabels;
	std::vector<double>                   aquaLandBarValues;
	vtkSmartPointer<vtkPoints>            aquaLandBarTickPts;
	double aquaLandBarLo = 0, aquaLandBarHi = 1;
	bool   aquaLandShowBar = true;           // Scene-Objects "Color Bar Land" checkbox intent
	bool   aquaShowWater = true;             // which side is ACTIVE; default water (per spec)
	std::string aquaVarLabel;                // Scene Objects label for the composited surface's OWN
	                                          // group = the active variable's real name, whatever the
	                                          // file itself calls it (gmtvtk_aqua_set_var_label_h);
	                                          // empty -> rebuildSceneObjects falls back to surfName.

	// --- tiled-LOD pyramid (plain grid) -------------------------------------
	// Quadtree of tiles; coarse near root, refined per-frame by screen-space error so only the
	// visible region at the needed resolution is resident. surfGroup holds the live tile actors.
	QuadNode *quadRoot = nullptr;
	vtkSmartPointer<vtkScalarsToColors> surfLut;     // shared LUT for lazily-built tiles
	bool     surfCtfRange = false;
	double   nanColor[3] = { 1.0, 1.0, 1.0 };         // Preferences "NaN fill colour" (seeded from QSettings at build)
	int      surfEdges = 0;                           // current wire-edge state (applied to new tiles)
	uint64_t lodFrame = 0;                            // bumped each refine; tiles store lastUsed
	size_t   lodResidentBytes = 0;                    // approx resident tile geometry bytes
	size_t   lodBudgetBytes = (size_t)1 << 30;        // ~1 GiB cap; LRU-evict offscreen tiles past it
	vtkSmartPointer<vtkCallbackCommand> lodCmd;       // camera-modified observer (drives refine)
	// MESHING BUDGET PER CAMERA EVENT. Refinement runs inside the camera callback, i.e. before the
	// frame is allowed to finish, so a zoom that crosses a level threshold used to mesh that ENTIRE
	// level -- hundreds of makeGridTile calls -- with the UI frozen until the last one. The budget
	// caps how much may be meshed in one pass; whatever is left over is covered by its COARSER parent
	// tile (already built, so free) and finished on a short timer. Blur-then-sharpen, never a freeze.
	// Bumped by applyShading, i.e. whenever ANY shading input changes (mode, sun, sliders, the
	// Hillshade tool's reflectance). A tile's colours are valid exactly while its own stamp matches.
	long styleGen      = 1;
	long lodCellsLeft  = 0;                           // sampled cells still meshable in THIS pass
	bool lodPending    = false;                       // a pass ran short -> more detail still owed
	bool lodTimerArmed = false;                       // one catch-up timer in flight, never a storm

	// --- flat-2D (top-down ortho map) toggle --------------------------------
	// One-button switch to a true planimetric map: VE collapsed to 0 (relief flat,
	// z carried by colour only), orthographic top-down camera, rotation/tilt locked.
	// The 3D camera + VE are saved here and restored on toggle back.
	bool   flat2d = false;
	bool   imageOnly = false;   // loaded as a bare image (no elevation): readout shows pixel colour, not z
	PaletteLegend palette;      // the PRIMARY image's class legend, when that image is indexed (see
	                            // PaletteLegend); an extra/derived image carries its own on its ExtraObj
	// Images whose FULL-PRECISION (UInt16) source the host still holds (_IMG_ORIG, savefile.jl), by
	// Scene Objects name ("" = the primary image). "Auto histogram stretch (new image)" re-derives an
	// 8-bit image from that source, so the entry only appears for an image that HAS one — on a plain
	// 8-bit image there is nothing wider to stretch from. Set from Julia (gmtvtk_image_set_has_orig_h).
	std::set<std::string> imgHasOrig;
	// Images that are genuine 3-band RGB, by the same naming. "Explore RGB" splits an image into its
	// colour components, so it is offered ONLY for one that HAS them — Mirone hides the entry for any
	// non-RGB image (mirone.m: `if (ndims(img) < 3), return, end`). Set from Julia
	// (gmtvtk_image_set_rgb_h), which is the only side that knows how many bands an image really has:
	// every image reaches the viewer as an RGBA texture, indexed and grey ones included.
	std::set<std::string> imgRGB;
	bool   emptyStart = false;  // full-chrome launcher with no data yet (hidden placeholder); drop -> promote
	bool   gridAdopted = false; // a real grid was dropped onto an imageOnly canvas (Background region /
	                            // bare image) and adopted as the hover heightfield -> readout shows z,
	                            // NOT pixel colour, even though the canvas itself stays imageOnly
	bool   layerImgMode = false;// base is a grid shown as a FLAT illuminated IMAGE (3-D-cube layer scrub,
	                            // gmtvtk_show_layer_image_h): a flat quad + hillshade texture, full-res z in
	                            // gridZ for the readout. A same-size layer switch just repaints the texture
	                            // (no geometry rebuild). Cleared by buildSceneContent (any real surface build).
	int    layerTexW = 0, layerTexH = 0;   // baked hillshade texture size (<= grid size; capped so a huge
	                            // cube's per-layer bake stays cheap). Fast-repaint reuses the texture iff these match.
	bool   customLayerTexture = false;  // the drape texture was supplied ALREADY COMPOSITED by the host
	                            // (gmtvtk_show_layer_rgba_h, e.g. Aquamoto's dry/wet blend) instead of baked
	                            // here from a single CPT + illumination -- rebakeLayerImage/refineLayerDetail
	                            // must never regenerate it from gridZ+cpt, or they would silently overwrite it.
	// Aquamoto hillshade: shade the host-composited texture through the SAME applyReliefShade the whole
	// app uses (bakeAquaShade), so the Shading dock's Hillshade drives the tsunami too. aquaBaseRGBA is
	// the UNSHADED composite (row-major, row0=south, nx*ny*4) re-shaded on every dock change; aquaBathyZ
	// is the static bathymetry (column-major, like gridZ) = the LAND surface, while gridZ carries the
	// per-slice stage = the WATER surface. Empty -> not an Aquamoto layer, bakeAquaShade is a no-op.
	std::vector<unsigned char> aquaBaseRGBA;
	std::vector<float>         aquaBathyZ;
	AquaSideShade aquaWaterShade;   // WATER image's OWN light (updated only when Water is the selected side)
	AquaSideShade aquaLandShade;    // LAND  image's OWN light (updated only when Land  is the selected side)
	bool aquaShadeSelWater = true;  // which side the Shading dock edits (Shade Water/Land radio). PURE
	                                // selector: flipping it changes NOTHING visible, NOT the colorbar --
	                                // it only routes the NEXT shading edit to water (true) or land (false).
	// 3-D-cube shading selection. A cube layer can be shown either as the fast flat shaded IMAGE (the new
	// "Shaded image (2-D)" algorithm) or, if the user picks one of the surface looks (Cast shadows /
	// Hillshade Lambert / grdimage) in the Shading dock, as a real 3-D surface with that look. The dock
	// re-renders the CURRENT layer through g_juliaCubeLayer when the choice changes.
	bool   isCube = false;      // this window is showing a 3-D-cube layer (cube layer switches read cubeFlatImg)
	int    cubeNLayers = 0;     // >1 iff the BASE surface is a 3-D-cube variable (its menu offers "Cube layers…")
	bool   cubeFlatImg = true;  // render the base grid as the flat shaded IMAGE (else a real 3-D surface)
	int    cubeLayerCur = 0;    // current cube layer index (0-based) + colour-range choice (bookkeeping)
	int    cubeUseGlobal = 0;
	QCheckBox *cbFlat = nullptr, *cbShadow = nullptr, *cbHillL = nullptr, *cbHillG = nullptr, *cbPBR = nullptr;   // Shading dock checkboxes
	std::function<void()> syncFlatEnable;   // grey out the Shading controls that do nothing on a flat baked image
	// Base grid's CPT (control nodes) + geographic flag, kept so the Shading dock can rebuild the base as
	// a flat IMAGE or a real SURFACE on demand (rebuildBaseFromStored) from s->gridZ without the host.
	std::vector<double> baseCz, baseCrgb;
	int    baseGeog = 0;
	// Hi-res zoom detail tile: on camera-settle a sharper texture of just the visible region is baked and
	// laid over the base drape, so a deep zoom is crisp without paying for it during scrubbing.
	vtkSmartPointer<vtkActor>     layerDetail;
	vtkSmartPointer<vtkImageData> layerDetailImg;
	vtkSmartPointer<vtkCallbackCommand> layerCamCmd;   // camera observer -> schedules a settle refine
	QTimer *layerDetailTimer = nullptr;                // debounce (bake only after the camera stops)
	double  layerDetailReg[4] = { 0, 0, 0, 0 };        // baked tile region (true W,E,S,N); skip if unchanged
	bool   fvSolid = false;     // window's content is a body-button GMT solid (cube/sphere/torus/…) built
	                            // in place by gmtvtk_promote_fv_h -> a later body click REPLACES it here
	double sav_pos[3] = {0, 0, 0};
	double sav_foc[3] = {0, 0, 0};
	double sav_vup[3] = {0, 0, 1};
	int    sav_parallel = 0;
	double sav_ve = 1.0;
	bool   sav_surfLit = true;

	// --- shading state, live-tunable from the Shading dock ------------------
	// A controllable directional KEY light (azimuth/elevation, like a GMT hillshade
	// sun) + a dim FILL so shadowed slopes aren't pure black. No LightKit (its angles
	// are camera-relative and not user-controllable) and no IBL by default.
	vtkSmartPointer<vtkLight>     keyLight, fillLight;
	vtkSmartPointer<vtkTexture>   envTex;
	vtkSmartPointer<vtkSSAOPass>          ssao;   // persistent passes: rebuilding them every
	vtkSmartPointer<vtkToneMappingPass>   tone;   // applyShading() leaks their GPU FBOs (VTK warns
	vtkSmartPointer<vtkOpenGLFXAAPass>    fxaa;   // "FrameBufferObject should have been deleted")
	vtkSmartPointer<vtkCameraPass>        shadowCam;   // cached cast-shadow opaque sequence (sun self-shadowing terrain)
	vtkSmartPointer<vtkShadowMapBakerPass> shadowBaker; // its depth-map baker (resolution lives here)
	double ssaoRadius = 0.5, ssaoBias = 1e-4;
	double lightAz = 315.0, lightEl = 45.0;   // sun azimuth (deg from north, CW) + elevation
	// F3D material defaults (vtkF3DGenericImporter): roughness 0.3, IOR 1.5, PBR, metallic 0.
	double roughness = 0.3, metallic = 0.0, ior = 1.5;
	double lightIntensity = 1.0, fillIntensity = 0.35, envIntensity = 1.0;
	bool   useSSAO = true, useTone = true, useFXAA = true, useIBL = false;
	bool   useShadows = false;        // sun cast-shadows (terrain self-shadowing) — OFF by default (opt-in; mutually exclusive with useHillshade)
	int    shadowRes  = 2048;         // shadow depth-map resolution (higher = crisper shadow edges)
	bool   useHillshade = false;      // baked hillshade master on/off; rendered UNLIT so relief reads
									  // even flat-on (2-D map). Alt to lit/PBR. Two styles (s->hillGrd):
	bool   hillGrd      = false;      //   false = Lambert (mesh-normal N.L, VE-corrected, darken-only),
									  //   true  = GMT grdimage (z-gradient, VE-independent, HSV illuminate).
	double hillAmbient  = 0.25;       // Lambert hillshade shadow floor (0 = black valleys, 1 = no shade)
	double hillGain     = 2.0;        // grdimage relief contrast: atan slope on the z-gradient signal (grdgradient -Nt amp)
	bool   litBake      = true;       // FLAT image only: bake a CPU PBR shade (approximates the lit 3-D
	                                  // surface) so "Shaded image" alone reproduces the loaded-grid look.
	                                  // Mutually exclusive with useHillshade; both off (flat) = plain CPT.
	// --- EXTERNAL illumination (View > "Illumination (Hillshade)…", port of Mirone shading_params.m).
	// A per-node REFLECTANCE grid computed by GMT grdgradient in Julia (src/hillshade.jl) and pushed
	// down by gmtvtk_set_shade_intensity_h. When it is present the shade engine takes the intensity
	// from HERE instead of deriving one from the surface normal — the modulation itself stays
	// gmtIlluminate(), the one HSV modulator (SACRED_LAW: same operation, same function). Sampled by
	// WORLD POSITION (sampleGrid), so it serves the surface, the LOD tiles, the flat-image bake and
	// the Aquamoto composite alike, at any resolution and with no index bookkeeping.
	std::vector<float> shadeInten;   // column-major inten[ix*shadeInY + iy], the gridZ layout
	int    shadeInX = 0, shadeInY = 0;
	double shadeInX0 = 0.0, shadeInX1 = 0.0, shadeInY0 = 0.0, shadeInY1 = 0.0;
	int    shadeInModel = 0;         // the Mirone illum_model that produced it (0 = none loaded)
	// "Remove illumination" (the tool's ✕, Mirone's ImageResetOrigImg_CB): NO light at all — the
	// surface renders UNLIT with its plain CPT colours. Distinct from every look toggle, because
	// "no hillshade" still leaves the PBR scene lights on and the grid still looks illuminated.
	// Cleared by anything that turns a light back on (the dock's looks, the sun sliders, a new model).
	bool   noShade = false;

	bool   matteSurf = false;        // fv colour mesh: keep s->surf MATTE (Phong, no specular/IBL) so the
									 // data colour reads true; glossy PBR mirrored the bright sky env to grey
									 // on up-facing facets. applyShading honours this (else it re-clobbers it).

	std::vector<Overlay> overlays;   // GMTdataset line/point overlays (per-element context menus)
	std::vector<Curtain> curtains;   // Fledermaus vertical image curtains hung along an XY track
	std::vector<ExtraObj> extras;    // grids/images dropped into this window after it opened
	std::vector<SymbolLayer> symbols;       // screen-constant glyph layers (volcanoes, seismicity, …)
	vtkSmartPointer<vtkCommand> symSizeCmd; // per-frame rescale observer, installed once (addSymbols)

	// --- coordinate reference system (CRS) ----------------------------------
	// The single per-window store of the data's georeferencing, pushed down from Julia
	// (gmtvtk_set_crs) which resolves all three interchangeable forms via GMT.jl. An empty CRS
	// (no proj4/wkt and epsg==0) means UNREFERENCED data -> the Geography menu stays disabled, since
	// placing GSHHG coastlines/borders/rivers needs a reference frame.
	std::string crsProj4, crsWkt;
	int         crsEpsg = 0;
	QMenu *geoMenu = nullptr;   // the Geography menu (built disabled; enabled once a CRS is set)
	QMenu *elasticMenu = nullptr;   // Seismology > Elastic deformation (disabled until a CRS is set)
	bool hasCRS() const { return !crsProj4.empty() || !crsWkt.empty() || crsEpsg != 0; }

	// --- Swipe / Link tool (two ways to compare two rasters) -----------------
	// One toolbar slot, two modes (57_swipe.cpp) — `swipeToolMode` picks which one `swipeAct`'s
	// toggle drives, mirroring the 2D/3D flyout's shape (icon + dropdown) but each mode needs its own
	// partner-resolution flow before it can turn on, so the dropdown only SELECTS a mode; the slot
	// click is what toggles the selected mode on/off.
	enum class ToolMode { Swipe, Link };
	ToolMode swipeToolMode = ToolMode::Swipe;

	// Swipe: ONE camera-aligned cut plane, given to both paired layers of THIS window with opposite
	// normals: A keeps the half LEFT of the divider, B the half right of it. The props are RAW
	// (non-owning) identity handles — every use re-validates them against the live layer list, so a
	// layer deleted while the tool is on simply drops out. `swipeSavedVis` is the visibility of every
	// raster before the tool hid the non-paired ones; it is replayed when the tool switches off.
	bool   swipeOn   = false;
	double swipeFrac = 0.5;          // divider position as a fraction of the render-window width
	vtkProp3D *swipeAProp = nullptr;
	vtkProp3D *swipeBProp = nullptr;
	vtkSmartPointer<vtkPlane> swipePlaneA, swipePlaneB;
	vtkSmartPointer<vtkCallbackCommand> swipeCamCmd;   // camera-modified: keep the cut screen-vertical
	QAction *swipeAct = nullptr;      // the shared toggle (enabled per the ACTIVE mode's own usability)
	QToolButton *swipeToolBtn = nullptr;  // the VISIBLE toolbar button -- swipeAct is never added to the
	                                     // toolbar itself (see 70_window.cpp), so its enabled/checked
	                                     // state does not reach the screen on its own; every setter
	                                     // that touches swipeAct's enabled state must ALSO touch this.
	QWidget *swipeBar = nullptr;      // the draggable divider overlay, parented on `widget`
	std::vector<std::pair<vtkProp3D*, int>> swipeSavedVis;

	// "Download grid" button pinned to the bottom-centre of an Ocean Color browse image (the L4 PNG a
	// double-click in the browser placed here). It is a real QPushButton parented on `widget`, like
	// swipeBar above, repositioned from the image actor's own bounds before every render — so it
	// tracks pan/zoom/rotate and disappears with the image. `ocBtnOwner` is the ExtraObj name it
	// belongs to and `ocBtnUrl` the L3 netCDF address it downloads (see src/oceancolor.jl).
	QPushButton *ocBtn = nullptr;
	std::string  ocBtnOwner, ocBtnUrl;

	// Link shows ONE of a pair at a time instead of splitting them across a divider: a right-click
	// swaps which one you are looking at, at whatever zoom you are already at. Two flavours, picked
	// by what the window actually holds (57_swipe.cpp):
	//   IN-WINDOW  — this window holds >=2 rasters: the SAME pairing swipeLayers/swipePartnerDialog
	//     give Swipe, and the right-click just flips which of the two is visible. The camera is NEVER
	//     touched, which is exactly what "respecting the current zoom" means — nothing about the view
	//     moves, only which raster is drawn. `linkAProp`/`linkBProp` are RAW, non-owning identity
	//     handles re-validated against the live layer list on every use (same convention as Swipe's
	//     own aProp/bProp); `linkSavedVis` is every raster's visibility before Link hid the non-paired
	//     ones, replayed when Link switches off.
	//   CROSS-WINDOW — this window holds only ONE raster: the partner is ANOTHER OPEN WINDOW whose
	//     data frame overlaps this one's, and the right-click raises it framed on the region THIS
	//     window is currently showing. `linkPartnerScene` holds it, set on BOTH scenes so the gesture
	//     works from either side; it is a RAW handle too, always re-validated with sceneAlive() before
	//     use (a partner window closed meanwhile is simply dropped, never dereferenced).
	// The gesture is PRESS-AND-HOLD, not a toggle: hold the right button to peek at the other one,
	// release to snap back to the one you started on. `linkPeeking` is that held state (so a peek
	// interrupted by the tool switching off, or by a layer disappearing, still snaps back);
	// `linkPeekFilter` is the QObject event filter on `widget` that owns the raw press/release —
	// installed while Link is on, removed when it is off, so right-drag zoom behaves normally again.
	bool   linkOn = false;
	bool   linkPeeking = false;
	QObject *linkPeekFilter = nullptr;
	vtkProp3D *linkAProp = nullptr;
	vtkProp3D *linkBProp = nullptr;
	Scene *linkPartnerScene = nullptr;
	std::vector<std::pair<vtkProp3D*, int>> linkSavedVis;

	QAction *act2D = nullptr;        // shared checkable "Flat 2D (map)" action (toolbar + View menu)
	QWidget *objPanel = nullptr;     // Scene Objects dock content (rebuilt when overlays change)
	QDockWidget *objDock = nullptr;  // the Scene Objects dock itself (re-shown when the first nested rect lands)
	FoldTitleBar *objFoldBar = nullptr;  // Scene Objects dock fold toggle (call ->onClick() to fold/unfold programmatically)
	FoldTitleBar *shadeFoldBar = nullptr; // Shading dock fold toggle (Surface row click folds/un-folds it via toggleShadingFold)
	QDockWidget *shadeDock    = nullptr;  // the Shading dock itself (re-shown when an empty launcher is promoted to a grid)
	std::string surfName;            // Scene Objects label for s->surf ("" -> "Surface"; named solids set it)
	bool transplantUndoAvail = false; // a transplant is applied + not yet undone -> offer "Undo transplant" (Julia toggles this via gmtvtk_set_transplant_undo)
	QPlainTextEdit *console = nullptr;   // Julia console dock output (commands eval'd in Main via g_juliaEval)
	QPlainTextEdit *errConsole = nullptr; // read-only Errors tab: execution errors from background callbacks (gmtvtk_log_error)

	// --- bottom tabbed panel (Profile / Julia Console / Errors) -------------
	QDockWidget *bottomDock    = nullptr;   // the single bottom dock holding the tab widget
	QTabWidget *bottomTabs    = nullptr;   // its QTabWidget; the corner "Hide" collapses the body
	QTableWidget *dataTable     = nullptr;   // LAST floating table popped for this window by
	                                         // gmtvtk_set_table (show_table). Kept only so
	                                         // gmtvtk_scene_state can report its row count; the dialog
	                                         // itself is parentless and self-deleting (nulls this).
	QToolButton *bottomHideBtn = nullptr;   // tab-bar corner Hide/Show toggle
	bool          bottomCollapsed = false;   // panel body collapsed to the tab strip?

	// --- point-cloud rubber-band selection (Ctrl+right-drag) ----------------
	// Ported from GMTF3D f3d_ext_interactor.cxx: drag a box to (de)select points; the
	// picked set is highlighted (caller colour) and kept for the host to read back.
	vtkSmartPointer<vtkPolyData>  cloudPD;        // the point cloud (set by view_points; null for grids)
	std::string hoverInfo;                        // text of the symbol tooltip currently shown ("" = none)
	bool   rbEnabled   = false;                   // rubber-band selection active (point clouds only)
	bool   rbSelecting = false;                   // mid Ctrl+right-drag
	bool   rbConsume   = false;                   // swallow the context menu this right-release triggers
	int    rbX0 = 0, rbY0 = 0;                    // drag start (VTK display px)
	double rbR = 0.83, rbG = 0.83, rbB = 0.83;    // highlight colour for the picked points
	vtkSmartPointer<vtkActor2D>   rbBox;          // the 2D selection rectangle overlay
	vtkSmartPointer<vtkPoints>    rbBoxPts;
	vtkSmartPointer<vtkActor>     rbHL;           // highlight overlay (selected points drawn on top)
	vtkSmartPointer<vtkPoints>    rbHLPts;
	vtkSmartPointer<vtkPolyData>  rbHLPoly;
	std::set<vtkIdType> rbSel;                    // current selection (point ids into cloudPD)
	std::vector<std::vector<vtkIdType>> rbUndo;   // prior selection states (Ctrl+Z undo)
	vtkSmartPointer<vtkCallbackCommand> rbCmd;

	Gizmo *giz = nullptr;       // interaction gizmo (owns its own drag observers)

	// middle-button: pan while dragging, recenter rotation centre on a click (no drag)
	vtkSmartPointer<vtkCallbackCommand> midCmd;
	bool   midDown = false, midMoved = false, midPicked = false;
	int    midX = 0, midY = 0;
	double midPick[3] = {0, 0, 0};

	// --- profile track (Ctrl+left-drag): sample surface elevation along a line --
	vtkSmartPointer<vtkActor>       profLine;          // 3D drape polyline laid on the relief
	vtkSmartPointer<vtkPolyData>    profPD;            // its geometry (TRUE coords) — for restyle + save
	vtkSmartPointer<vtkTexture>     profStripe;        // 1-D stipple texture for dashed/dotted style
	vtkSmartPointer<vtkCellLocator> surfLoc;           // built lazily from surf polydata (TRUE coords)
	class ProfilePanel *prof     = nullptr; // 2D (s,z) panel (a tab in the bottom dock)
	int    profStyle = 0;                              // 0 solid, 1 dashed, 2 dotted
	int    profStack = 0;                              // its rank in the SHARED draw-order pile: the track is
	                                                   // a vector like any other, so it obeys the same
	                                                   // "vectors above every raster" law (gatherStackItems)
	bool   profiling = false;
	double track0[2] = {0, 0};                         // press point in TRUE (x,y)
	std::vector<double> profS, profZ;                  // last profile (along-track distance, elevation)

	// --- polygon draw / edit tool (toolbar polygon button) ------------------
	// Draw mode (polyMode, toolbar toggle on): left-click adds a vertex, right-click removes the
	// last, double-left-click closes the polygon. Idle (polyMode off): double-click ON a finished
	// polygon enters edit mode (polyEdit) — square handles at the vertices, click-drag moves one.
	// The toolbar offers five draw tools, all routed through this one machinery. Polygon, polyline,
	// rectangle and circle all finalize into a `Polygon` (a vertex ring; polyline is the only open
	// one) and so share preview / edit / delete / Scene-Objects / Line-Properties code. Text places
	// a billboard label instead. polyShape selects which tool the active (checked) button drives.
	enum ShapeKind { SH_Polygon, SH_Polyline, SH_Line, SH_Rect, SH_Circle, SH_Text, SH_RectN, SH_Fault,
	                 SH_SymCircle, SH_SymSquare, SH_SymStar };   // Symbols flyout: one-click regular shapes
	ShapeKind polyShape = SH_Polygon;                  // active tool while polyMode is on
	std::vector<Polygon> polys;                        // finished polygons / polylines / rects / circles
	// "Point at a line" pick a tool can ARM (Plates > Euler rotations' "Pick in view" / "Rect
	// select"). Deliberately generic — any later tool that needs the user to point at lines reuses
	// this one mechanism, not its own.
	//   1 = CLICK: each left click resolves the line under the cursor through the SAME hit tests the
	//       double-click edit path uses (polyHitPolygon, then pickOverlayAt). Stays armed, so several
	//       lines can be collected; the tool disarms it.
	//   2 = RECT: two clicks (anchor, opposite corner) rubber-band a rectangle — drawn by the SAME
	//       preview the rectangle DRAW tool uses (polyRebuildPreview) — and every line with a vertex
	//       inside it answers at once. One-shot: it disarms itself on the second click.
	// `vectorPickCB` gets the Scene Objects labels, one per line, '\n'-separated ("" = nothing hit).
	int  vectorPickMode = 0;
	// CLICK mode only: fire on a LEFT DOUBLE-click instead of a single click (Compute Euler pole's
	// "Pick lines from Figure"). Same hit test, same callback — vectorPickFire is the one resolver;
	// this only says WHICH mouse event calls it.
	bool vectorPickDbl = false;
	bool vectorPickDrawing = false;                    // RECT: the anchor click has happened
	std::array<double,3> vectorPickAnchor{ 0, 0, 0 };  // RECT: that anchor, in TRUE world coords
	int  vectorPickPrevShape = 0;                      // RECT: polyShape before the preview borrowed it
	std::function<void(const std::string &)> vectorPickCB;
	// vectorPickMode == 3: a POINT pick — the click answers with the world (x,y) it landed on rather
	// than with whatever element sits there. The Shape detector (floodfill.m's magic wand) needs the
	// seed point itself, not a line. Same arming/disarming and the same left-click consumption as the
	// element picks above, so an armed seed pick can never leak into a camera rotate either.
	std::function<void(double, double)> seedPickCB;
	// …and how the user says "that's all of them": a right-click or a double-click, both routed here
	// (the tool's own button works too). Only the multi-seed collector sets this.
	std::function<void()> seedPickEndCB;
	std::vector<MecaGroupProps> mecaGroups;            // one entry per focal-mechanism batch groupName
	std::vector<MecaBall> mecaBalls;                   // one entry per plotted event (drag + anchor line state)
	int    mecaDrag = -1;                               // index into mecaBalls being click-dragged (-1 = none)
	// Scene Objects group rows the USER has opened by hand, keyed by their label. rebuildSceneObjects
	// throws the whole tree away and builds a new one on every change, so without this every manual
	// unfold would snap shut again on the next rebuild. A grid group starts FOLDED (only the container
	// row shows, not Surface/Color Bar/Axes) and appears here once opened.
	std::unordered_set<std::string> objExpanded;
	int    vecSeq = 0;                                  // monotonic seed for shared vector-pile stack ranks
	int    surfStack = 0;                               // base relief's rank in the GRID pile (base + grids)
	int    gridSeq   = 0;                               // monotonic seed for grid-pile ranks (newest on top)
	int    gridTagSeq = 0;                              // monotonic seed for UNIQUE grid GROUP tags (never reused;
	                                                    // -1 is reserved for the base relief grid)
	std::vector<TextLabel> texts;                      // user-placed text labels
	bool   polyMode    = false;                        // draw-mode button toggled on
	bool   polyDrawing = false;                        // mid-building the current polygon
	std::vector<std::array<double,3>> polyCur;         // in-progress vertices (TRUE coords)
	vtkSmartPointer<vtkActor>    polyPreview;          // rubber preview: placed verts + segment to cursor
	vtkSmartPointer<vtkPolyData> polyPreviewPD;
	int    polyEdit     = -1;                          // index into polys being edited (-1 = none)
	int    polyDragVert = -1;                          // vertex index being click-dragged (-1 = none)
	bool   polyDragWhole = false;                       // Shift+drag in edit mode: translate the WHOLE element
	double polyDragLastW[2] = {0.0, 0.0};              // last picked world (x,y) for the incremental whole-drag delta
	int    textDrag     = -1;                          // index into texts being click-dragged (-1 = none)
	int    symArmed     = -1;                          // index into symbols (an oneShot one) armed for drag by
	                                                    // a double-click — PERSISTS like polyEdit, until toggled
	                                                    // off by another double-click (-1 = none)
	int    symLayerDrag = -1;                          // index into symbols actively being click-dragged RIGHT
	                                                    // NOW — transient, like polyDragVert (-1 = none)
	int    symDragPressX = 0, symDragPressY = 0;       // press-point (px) for symLayerDrag's threshold gate —
	                                                    // real movement doesn't commit until past a few px, so a
	                                                    // plain click on an armed symbol can't nudge its position
	int    symPtDrag = -1;                             // BATCH symbol layer (index into symbols, oneShot==false)
	                                                    // with a single POINT being click-dragged RIGHT NOW, idle
	                                                    // (no arm/double-click step, unlike symLayerDrag/symArmed —
	                                                    // a batch layer has many points, so press-and-drag picks
	                                                    // the nearest one directly). -1 = none.
	int    symPtIdx  = -1;                             // which point within symbols[symPtDrag] (-1 = none)
	double symPtDragLastW[2] = {0.0, 0.0};             // last picked world (x,y) for the incremental drag delta —
	                                                    // same convention as polyDragLastW
	// "Copy me" (line/polyline/polygon context menu, 85_polygon.cpp copyMeStart/End): a clone follows
	// the cursor, attached to no button, until a left click drops it. The clone lives HERE as its own
	// floating Polygon (never pushed into `polys` until commit) and is redrawn every move through the
	// SAME polyRebuildLine every drawn shape uses; it commits through the SAME polyFinalize a draw
	// tool's double-click uses — no parallel geometry or finalize path (SACRED_LAW).
	bool    copyDragging = false;                      // true from "Copy me" click to the commit/left-click
	Polygon copyGhost;                                  // the floating clone (its own line/fill actors)
	std::array<double,3> copyAnchorW{ 0, 0, 0 };        // last picked world (x,y) — incremental drag delta, same convention as polyDragLastW
	vtkSmartPointer<vtkActor>    polyHandles;          // square vertex handles for the edited polygon
	vtkSmartPointer<vtkPolyData> polyHandlePD;
	vtkSmartPointer<vtkActor>    symHandle;            // yellow handle on the armed symbol (symArmed) —
	vtkSmartPointer<vtkPolyData> symHandlePD;          // visible "selected" feedback + a comfortable drag target
	qint64 polyLastClickMs = -10000;                   // last left-press time (double-click detect)
	int    polyLastClickX = 0, polyLastClickY = 0;     // last left-press position (px)
	vtkSmartPointer<vtkCallbackCommand> polyCmd;       // mouse observers (priority above the gizmo)
	QAction *polyAct = nullptr;                         // active draw toggle action — set on the checked tool
	std::vector<QAction*> shapeActs;                    // all five draw-tool buttons (for mutual untoggle)

	// Vertical elastic deformation dialog state — persisted here so reopening the dialog (it is
	// rebuilt from scratch each time) restores the user's last-typed Fault/Dislocation values. The
	// trace geometry (Length/Strike) is always re-seeded from the live fault polyline so a vertex
	// drag is honoured; these non-geometry fields are restored from memory. Only single faults are
	// editable, so one slot suffices.
	struct FaultDlgState {
		bool valid = false;
		QString len, wid, strike, dip, depth, depTop;   // Fault Geometry boxes
		QString dStrike, rake, slip, N, q, mu;          // Dislocation Geometry boxes
		bool hide = false, scc = false;
		int  coord = 0;                                  // coordCombo index: 0 = Geogs, 1 = Cart
	};
	FaultDlgState faultDlg;
	QString nswingParams;                               // NSWING dialog fields, saved on close, restored on reopen (raw "key=value\n…" block)
	QWidget *elasticDlg = nullptr;                      // open (non-modal) Vertical elastic deformation dialog, if any
	QWidget *focalStudioDlg = nullptr;                  // open (non-modal) Focal Meca Studio demo dialog, if any
	QWidget *cubeDlg = nullptr;                         // open (non-modal) 3-D cube layer selector dialog, if any

	// A tool window closed with its X does NOT die: it hides and PARKS as a handle in the bottom strip
	// of THIS window's Scene Objects dock, where a double-click brings it back and its own menu holds
	// the real delete. ONE list and ONE row builder (rebuildSceneObjects, 50_scene.cpp) for every kind
	// of parkable tool — X,Y plots, the Contours dialog, whatever comes next — never a per-tool strip.
	// The owning tool supplies the label/icon/tooltip and the two actions, so parking adds no
	// knowledge of any particular tool here.
	struct ParkedTool {
		QWidget *win = nullptr;                          // the hidden window: also the entry's identity
		QString  label, tip;
		int      icon = 0;                                // ObjIcon (50_scene.cpp), int here: not visible yet
		std::function<void()>               unpark;       // double-click, or ticking the row's checkbox
		std::function<void(const QPoint &)> menu;         // properties AND context menu: ONE lambda, both buttons
	};
	std::vector<ParkedTool> parkedTools;

	// IN-PLACE editing of an OVERLAY line (a contour, a coastline island, a dropped .xy track): the
	// vertex handles hang off the overlay's own points, so nothing is converted, copied or added —
	// no new element, no new Scene Objects row. It is the SAME edit gesture and the SAME handle /
	// hit-test / drag code a drawn polygon uses; `editVerts` (85_polygon.cpp) is the one view both
	// go through. -1 = not editing an overlay (a drawn polygon may still be under edit via polyEdit;
	// the two are mutually exclusive).
	int ovEdit    = -1;                                 // index into `overlays`
	int ovEditSeg = -1;                                 // which segment of it

	double annotWpp = 0.0;                              // world-per-pixel the screen-constant contour
	                                                     // annotations were last sized/cut for
	                                                     // (followZoomAnnotations, 50_scene.cpp)
};

// --- surface accessors: one actor (cloud/FV/drape/image) or a tiled grid -----------------
// When the grid is tiled, the transform/bounds/visibility live on the vtkAssembly `surfGroup`
// and the renderable parts are `tiles`; otherwise everything is the single actor `surf`. These
// helpers hide that split so call sites stay uniform. (Group null + tiles empty -> surf.)
static inline vtkProp3D *surfProp(Scene *s) {
	if (s->surfGroup) return s->surfGroup.Get();
	return s->surf.Get();
}
static inline void surfSetScale(Scene *s, double x, double y, double z) {
	if (vtkProp3D *p = surfProp(s)) p->SetScale(x, y, z);
}
static inline void surfGetScale(Scene *s, double sc[3]) {
	if (vtkProp3D *p = surfProp(s)) p->GetScale(sc); else { sc[0]=sc[1]=sc[2]=1.0; }
}
// The ACTIVE grid layer's own (unscaled) data z range — defined next to resolveActiveGrid
// (50_scene.cpp), which is also what the readout and the colorbar resolve through, so all three
// always describe the SAME layer. false = no grid layer at all (point cloud, solid, bare image).
static bool activeGridZRange(Scene *s, double &zlo, double &zhi);
static int  activeGridGeog(Scene *s);

static inline void surfGetBounds(Scene *s, double b[6]) {
	if (s->viewBoundsOverride) { for (int i = 0; i < 6; ++i) b[i] = s->viewBounds[i]; }
	else if (vtkProp3D *p = surfProp(s)) p->GetBounds(b);
	// SACRED_LAW.md "derived-variable axes law", Z half: a NEW grid is a NEW quantity with its OWN Z
	// axis and, more likely than not, its OWN UNITS (a gravity anomaly in mGal computed over a
	// bathymetry grid in m). It must never wear the parent's Z frame. The X/Y half already reframes
	// (viewBounds above); Z is taken here, at the ONE source every bounds-driven function reads
	// (applyVE's axes box, fitSnapView's camera fit, rebuildAxisLabels' tick billboards, the gizmo),
	// so they all follow the active layer automatically. When the base surface IS the active grid this
	// returns its own zmin/zmax = the actor bounds, i.e. no change to the ordinary single-grid case.
	double zlo, zhi;
	if (activeGridZRange(s, zlo, zhi)) {
		const double zs = s->zfac * s->ve;
		b[4] = zlo * zs; b[5] = zhi * zs;
	}
}
static inline void surfSetVisibility(Scene *s, int v) {
	if (vtkProp3D *p = surfProp(s)) p->SetVisibility(v);
}
// The renderable actors carrying material / mapper / edges. Tiles when tiled, else the surf.
static inline std::vector<vtkActor*> surfActors(Scene *s) {
	std::vector<vtkActor*> v;
	if (!s->tiles.empty()) { for (auto &a : s->tiles) v.push_back(a.Get()); }
	else if (s->surf)        v.push_back(s->surf.Get());
	return v;
}

// Topmost VISIBLE raster (base relief or a dropped grid) — target of the 'e' mesh toggle and other
// per-active-raster ops. Same "highest grid-pile rank wins" rule as resolveActiveGrid (images stack
// by zpos, not the pile, so they are not raster-mesh targets). `actors` already includes the optional
// image drape so the wire toggles on both layers together. `edgeState` points at the int holding the
// current wire state (base relief: s->surfEdges, also stamped on new tiles by buildTileActor); null
// for a dropped grid (single actor — read EdgeVisibility straight off it).
struct TopRaster { std::vector<vtkActor*> actors; int *edgeState = nullptr; bool valid = false; };

static inline TopRaster resolveTopRaster(Scene *s) {
	TopRaster tr;
	int bestStack = 0; bool have = false;
	std::vector<vtkActor*> base = surfActors(s);
	vtkProp3D *sp = surfProp(s);
	if (!base.empty() && sp && sp->GetVisibility()) {
		tr.actors = base;
		if (s->drape) tr.actors.push_back(s->drape.Get());
		tr.edgeState = &s->surfEdges; tr.valid = true;
		bestStack = s->surfStack; have = true;
	}
	for (auto &ex : s->extras) {
		if (ex.isImage || !ex.actor || !ex.actor->GetVisibility()) continue;
		if (!have || ex.gstack >= bestStack) {             // ties impossible (ranks normalized unique)
			bestStack = ex.gstack; have = true; tr.valid = true;
			tr.actors = { ex.actor.Get() };
			if (ex.drape) tr.actors.push_back(ex.drape.Get());
			tr.edgeState = nullptr;
		}
	}
	return tr;
}

// One node of the tiled-LOD quadtree. Covers the inclusive full-grid index region [i0..i1]x[j0..j1],
// rendered at stride `step` (1 = full res = leaf). `actor` is built lazily and lives in surfGroup
// while resident; `lastUsed` drives LRU eviction. worldSpacing = true-coord node gap at this step
// (feeds the screen-space-error test).
struct QuadNode {
	int level = 0;
	int i0 = 0, i1 = 0, j0 = 0, j1 = 0;
	int step = 1;
	bool leaf = true;
	double cx = 0, cy = 0;            // region centre (true coords)
	double worldSpacing = 0;          // true-coord node gap at this node's step
	QuadNode *child[4] = { nullptr, nullptr, nullptr, nullptr };
	vtkSmartPointer<vtkActor> actor;  // built geometry; null = not resident (nothing cached)
	// Is this node's actor currently PART OF THE SCENE? Distinct from `actor != null`, which only
	// says the geometry exists. Coarsening takes a tile OUT of the assembly but KEEPS it built, so
	// zooming back in re-adds it instead of re-meshing it (see dropNodeActor / ensureNodeActor).
	bool     inScene = false;
	// Shading state this tile's colours were baked under (Scene::styleGen). Re-baking is expensive —
	// hillshadeMapper maps every point through the CPT and shades it — so it must happen when the
	// shading actually CHANGED, not every time a cached tile comes back on screen.
	long     styleGen = 0;
	uint64_t lastUsed = 0;
	size_t   bytes = 0;
};

// Collapse / restore the bottom tabbed panel's BODY, leaving the tab strip (+ the Hide
// button) visible. Collapsing hides the QTabWidget's page stack and clamps the widget to
// the tab-bar height, so the dock shrinks and the 3-D view extends; restore reverses it.
static void setBottomCollapsed(Scene *s, bool collapse) {
	if (!s || !s->bottomTabs)
		return;
	if (QStackedWidget *body = s->bottomTabs->findChild<QStackedWidget*>("qt_tabwidget_stackedwidget"))
		body->setVisible(!collapse);
	s->bottomTabs->setMaximumHeight(collapse ? s->bottomTabs->tabBar()->sizeHint().height() + 6
											  : QWIDGETSIZE_MAX);
	s->bottomCollapsed = collapse;
	if (s->bottomHideBtn) {
		// Triangle fold affordance, matching the Scene Objects dock: ▸ collapsed, ▾ open.
		s->bottomHideBtn->setText(collapse ? QString::fromUtf8("\xE2\x96\xB8")   // ▸
										   : QString::fromUtf8("\xE2\x96\xBE"));  // ▾
		s->bottomHideBtn->setToolTip(collapse ? "Expand this panel"
											  : "Collapse this panel to extend the 3-D view");
	}
}

// Profile-track helpers (defined after ProfilePanel, below). DragCB drives these on Ctrl+drag.
// profilerBegin returns false when the cursor misses a data surface (grid/image) — vector-only
// or an empty Background region has nothing to sample, so the Ctrl+drag must not arm profiling.
static bool profilerBegin(Scene *s, int dx, int dy);
static void profilerDrag(Scene *s, int dx, int dy);
static void profilerEnd(Scene *s);
static bool profileHitAt(Scene *s, int dx, int dy);              // cursor near the profile line?
static void popupProfileMenu(Scene *s, const QPoint &globalPos); // its right-click menu
static void profileClear(Scene *s);                              // wipe the line + 2D panel

// MATLAB "peaks" — a recognizable relief surface.
static double peaks(double x, double y) {
	return  3.0 * std::pow(1 - x, 2) * std::exp(-(x * x) - std::pow(y + 1, 2))
		  - 10.0 * (x / 5.0 - std::pow(x, 3) - std::pow(y, 5)) * std::exp(-(x * x) - (y * y))
		  - (1.0 / 3.0) * std::exp(-std::pow(x + 1, 2) - (y * y));
}

// Build a structured grid surface as vtkPolyData (points + quads + z scalar),
// mirroring how grid2fv_direct emits a GMT grid.
static vtkSmartPointer<vtkPolyData> makeGridSurface(int nx, int ny, double x0, double x1, double y0, double y1,
                                                    double &zmin, double &zmax) {
	vtkNew<vtkPoints>     pts;   pts->SetDataTypeToFloat();
	vtkNew<vtkFloatArray> zval;  zval->SetName("z");
	pts->Allocate(nx * ny);
	zval->Allocate(nx * ny);

	const double dx = (x1 - x0) / (nx - 1);
	const double dy = (y1 - y0) / (ny - 1);
	zmin =  1e30; zmax = -1e30;
	for (int j = 0; j < ny; ++j) {
		double y = y0 + j * dy;
		for (int i = 0; i < nx; ++i) {
			double x = x0 + i * dx;
			double z = peaks(x, y);
			pts->InsertNextPoint(x, y, z);
			zval->InsertNextValue(z);
			if (z < zmin) zmin = z;
			if (z > zmax) zmax = z;
		}
	}

	vtkNew<vtkCellArray> quads;
	for (int j = 0; j < ny - 1; ++j) {
		for (int i = 0; i < nx - 1; ++i) {
			vtkIdType a =  j      * nx + i;
			vtkIdType b =  j      * nx + i + 1;
			vtkIdType c = (j + 1) * nx + i + 1;
			vtkIdType d = (j + 1) * nx + i;
			vtkIdType cell[4] = { a, b, c, d };
			quads->InsertNextCell(4, cell);
		}
	}

	vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
	pd->SetPoints(pts);
	pd->SetPolys(quads);
	pd->GetPointData()->SetScalars(zval);
	return pd;
}

// Paint NaN-scalar nodes/cells with the Preferences "NaN fill colour". A vtkColorTransferFunction
// maps a NaN scalar to its NanColor; the grid builders keep NaN nodes' z SCALAR = NaN (only their
// geometry z is filled to a finite floor), so those cells render in this colour. No-op for a plain
// LUT (grids always carry a CTF here).
static void applyNanColorToLut(vtkScalarsToColors *lut, const double nanRGB[3]) {
	if (auto *ctf = vtkColorTransferFunction::SafeDownCast(lut)) {
		ctf->SetNanColor(nanRGB[0], nanRGB[1], nanRGB[2]);
		ctf->SetNanOpacity(1.0);
	}
	else if (auto *tbl = vtkLookupTable::SafeDownCast(lut)) {
		tbl->SetNanColor(nanRGB[0], nanRGB[1], nanRGB[2], 1.0);
	}
}

// THE one place a grid's colour transfer function is built from a CPT (cz/crgb node arrays, the
// shape every Julia colour path already hands over).
//
// SACRED_LAW.md, "same operation, ALWAYS same function" — and its own lesson that when a value has
// more than one consumer you fix the SHARED SOURCE, not each call site. Building the CTF and
// applying the Preferences NaN fill colour were separate steps, so of the six grid-LUT builders in
// this code base only three remembered the second step: the primary surface, an added surface and
// the Preferences re-apply had the NaN colour; the cube-layer, Aquamoto-land and mesh builders
// silently dropped it. Same control, different behaviour per element type = the violation.
//
// Applying it HERE makes that impossible to get wrong: a grid LUT that exists has the NaN colour on
// it by construction, and a new builder cannot forget a step it never has to take.
// THE one place a grid mapper's scalar->colour mapping is configured.
//
// `InterpolateScalarsBeforeMapping` is deliberately OFF, and that is the whole point of this
// function. With it ON, VTK stops asking the LUT for a colour per fragment: it bakes the LUT into a
// 1-D colour TEXTURE and turns every scalar into a texture COORDINATE. A NaN scalar becomes a NaN
// coordinate, which samples the clamped edge of that texture — the vtkColorTransferFunction's
// NanColor is never consulted at all. So NaN cells came out in the ramp's end colour on every grid
// mapper that had it on, no matter how correctly the Preferences NaN fill colour had been applied
// to the LUT. Setting the colour and being able to SEE it are two different things, and the fix for
// the second one lives here.
//
// It costs a slightly less smooth gradient across a cell. Correct NaN wins.
static void configureGridMapper(vtkMapper *m, vtkScalarsToColors *lut,
                                double zmin, double zmax, bool ctfRange) {
	if (!m) return;
	m->SetLookupTable(lut);
	m->SetScalarRange(zmin, zmax);
	if (ctfRange) m->UseLookupTableScalarRangeOn();		// colours keyed to the CPT's own z nodes
	m->ScalarVisibilityOn();
	m->InterpolateScalarsBeforeMappingOff();
}

static vtkSmartPointer<vtkColorTransferFunction>
makeGridCTF(const Scene *s, const double *cz, const double *crgb, int ncolor) {
	vtkSmartPointer<vtkColorTransferFunction> ctf = vtkSmartPointer<vtkColorTransferFunction>::New();
	for (int i = 0; i < ncolor; ++i)
		ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
	if (s) applyNanColorToLut(ctf, s->nanColor);
	return ctf;
}

// Build a surface from a caller-supplied grid (GMT.jl layout): z is column-major,
// ny rows x nx cols, element (iy,ix) at offset ix*ny+iy, mapping to (x[ix], y[iy])
// with y ascending. NaN cells are PAINTED (not dropped): every quad is emitted; a NaN node keeps its
// z SCALAR = NaN (so the CTF paints it with its NanColor = the Preferences NaN fill colour) but its
// geometry z is pinned to the grid floor (zmin) so the mesh stays valid (flat filled hole).
static vtkSmartPointer<vtkPolyData> makeGridFromArray(const float *z, int nx, int ny,
													  double x0, double x1,
													  double y0, double y1,
													  double &zmin, double &zmax,
													  bool triangulate = true,
													  bool wantTC = true) {   // texture coords only needed for image drape
	vtkNew<vtkPoints>     pts;   pts->SetDataTypeToFloat();
	vtkNew<vtkFloatArray> zval;  zval->SetName("z");
	vtkNew<vtkFloatArray> tcoord; tcoord->SetNumberOfComponents(2); tcoord->SetName("tc");
	pts->Allocate(nx * ny);
	zval->Allocate(nx * ny);
	if (wantTC) tcoord->Allocate(2 * nx * ny);

	const double dx = (nx > 1) ? (x1 - x0) / (nx - 1) : 0.0;
	const double dy = (ny > 1) ? (y1 - y0) / (ny - 1) : 0.0;
	const double xspan = (x1 != x0) ? (x1 - x0) : 1.0;
	const double yspan = (y1 != y0) ? (y1 - y0) : 1.0;
	zmin =  1e30; zmax = -1e30;
	for (int j = 0; j < ny; ++j) {            // j = y row
		double y = y0 + j * dy;
		for (int i = 0; i < nx; ++i) {        // i = x col
			double x = x0 + i * dx;
			double zz = z[(vtkIdType)i * ny + j];   // column-major
			pts->InsertNextPoint(x, y, zz);
			zval->InsertNextValue(zz);
			if (wantTC) {
				tcoord->InsertNextValue((x - x0) / xspan);   // u: west->east
				tcoord->InsertNextValue((y - y0) / yspan);   // v: south->north (texture is south-first)
			}
			if (!std::isnan(zz)) {
				if (zz < zmin) zmin = zz;
				if (zz > zmax) zmax = zz;
			}
		}
	}
	if (zmin > zmax) { zmin = 0.0; zmax = 1.0; }   // all-NaN guard

	// Pin NaN nodes' GEOMETRY z to the grid floor so a filled hole is a flat sheet (the z SCALAR
	// stays NaN, so the CTF still paints it with the NaN fill colour). Same id order as the insert
	// loop above (id = j*nx + i). A NaN coordinate would otherwise corrupt any quad referencing it.
	for (int j = 0; j < ny; ++j) {
		for (int i = 0; i < nx; ++i) {
			if (std::isnan(z[(vtkIdType)i * ny + j]))
				pts->SetPoint((vtkIdType)j * nx + i, x0 + i * dx, y0 + j * dy, zmin);
		}
	}

	vtkNew<vtkCellArray> cells;
	// vtkCellArray defaults to 64-bit ids on win64. A grid id is < nx*ny; when that fits in
	// int32 use 32-bit connectivity storage -> halve the cell-array RAM (huge on big grids).
	if ((double)nx * (double)ny < 2.0e9) cells->Use32BitStorage();
	for (int j = 0; j < ny - 1; ++j) {
		for (int i = 0; i < nx - 1; ++i) {
			vtkIdType a =  j      * nx + i;
			vtkIdType b =  j      * nx + i + 1;
			vtkIdType c = (j + 1) * nx + i + 1;
			vtkIdType d = (j + 1) * nx + i;
			// A NaN IS A DATA HOLE. No cell is emitted where any corner is NaN, so the hole is a real
			// gap in the mesh: no geometry, no normal, and above all NOTHING FOR THE LIGHT TO HIT.
			// Emitting the quad and relying on the NaN scalar -> CTF NanColor (what this did before)
			// paints the right colour and then lets the PBR light multiply it — white NaN came out
			// beige. A hole cannot be illuminated. The NaN fill colour is shown by the flat unlit
			// backdrop behind the surface (nanPlaneUpdate), which is also what a click lands on.
			if (std::isnan(z[(vtkIdType) i     *ny + j    ]) || std::isnan(z[(vtkIdType)(i+1)*ny + j    ]) ||
			    std::isnan(z[(vtkIdType)(i+1)*ny + j + 1]) || std::isnan(z[(vtkIdType) i     *ny + j + 1]))
				continue;
			if (triangulate) {
				vtkIdType t1[3] = { a, b, c };   // lower-right tri
				vtkIdType t2[3] = { a, c, d };   // upper-left  tri
				cells->InsertNextCell(3, t1);
				cells->InsertNextCell(3, t2);
			}
			else {
				vtkIdType quad[4] = { a, b, c, d };
				cells->InsertNextCell(4, quad);
			}
		}
	}

	// Per-vertex normals computed INLINE from the height field (central differences of z),
	// so this single polydata feeds the mapper directly and we SKIP vtkPolyDataNormals -> no
	// full second copy of points+cells+scalars (the dominant grid-RAM waste). Point id order
	// matches the insert loop above: id = j*nx + i. NaN node / NaN neighbour -> flat up normal.
	vtkNew<vtkFloatArray> norm; norm->SetNumberOfComponents(3); norm->SetName("Normals");
	norm->SetNumberOfTuples((vtkIdType)nx * ny);
	for (int j = 0; j < ny; ++j) {
		for (int i = 0; i < nx; ++i) {
			float nxv = 0.f, nyv = 0.f, nzv = 1.f;
			const float zc = z[(vtkIdType)i * ny + j];
			if (!std::isnan(zc)) {
				const int il = i > 0 ? i-1 : i,  ir = i < nx-1 ? i+1 : i;
				const int jd = j > 0 ? j-1 : j,  ju = j < ny-1 ? j+1 : j;
				const float zl = z[(vtkIdType)il*ny + j],  zr = z[(vtkIdType)ir*ny + j];
				const float zb = z[(vtkIdType)i*ny + jd],  zt = z[(vtkIdType)i*ny + ju];
				if (!std::isnan(zl) && !std::isnan(zr) && !std::isnan(zb) && !std::isnan(zt)) {
					const double ddx = (ir - il) * dx, ddy = (ju - jd) * dy;
					const double gx = ddx != 0.0 ? (zr - zl) / ddx : 0.0;
					const double gy = ddy != 0.0 ? (zt - zb) / ddy : 0.0;
					double vx = -gx, vy = -gy, vz = 1.0;
					const double inv = 1.0 / std::sqrt(vx*vx + vy*vy + vz*vz);
					nxv = (float)(vx*inv); nyv = (float)(vy*inv); nzv = (float)(vz*inv);
				}
			}
			const vtkIdType id = (vtkIdType)j*nx + i;
			norm->SetTypedComponent(id, 0, nxv);
			norm->SetTypedComponent(id, 1, nyv);
			norm->SetTypedComponent(id, 2, nzv);
		}
	}

	vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
	pd->SetPoints(pts);
	pd->SetPolys(cells);
	pd->GetPointData()->SetScalars(zval);
	pd->GetPointData()->SetNormals(norm);     // baked normals -> buildAndShow skips vtkPolyDataNormals
	if (wantTC) pd->GetPointData()->SetTCoords(tcoord);   // for optional image drape
	return pd;
}

// Build ONE tile of a grid: the inclusive node block [i0..i1] x [j0..j1] of the full column-major
// z[i*ny + j], as a small quad polydata with baked per-vertex normals (z-scalar for CPT colour).
// Adjacent tiles SHARE their boundary row/col (i1 == next i0) so the seam has no gap. Normals are
// computed from the GLOBAL neighbours, so a border vertex's normal matches the neighbouring tile
// (no lighting seam). No tcoords (the tiled path is the plain CPT grid, never draped). Local point
// id = (j-j0)*tw + (i-i0). The basis for the tiled-LOD render (P1/P2).
static vtkSmartPointer<vtkPolyData> makeGridTile(const float *z, int nx, int ny,
												 int i0, int i1, int j0, int j1,
												 double x0, double dx, double y0, double dy,
												 double fillZ,       // NaN nodes' geometry z (grid floor) -> painted flat hole
												 int step = 1) {     // step>1 -> coarse LOD tile (sub-sampled)
	if (step < 1) step = 1;
	// Sampled global indices for this tile, ALWAYS including the far edge i1/j1 so a coarse tile
	// still spans its whole region and meets neighbours. step=1 -> every node (full res).
	std::vector<int> xs, ys;
	for (int i = i0; i < i1; i += step) xs.push_back(i);  xs.push_back(i1);
	for (int j = j0; j < j1; j += step) ys.push_back(j);  ys.push_back(j1);
	const int tw = (int)xs.size(), th = (int)ys.size();

	vtkNew<vtkPoints>     pts;  pts->SetDataTypeToFloat(); pts->Allocate(tw * th);
	vtkNew<vtkFloatArray> zval; zval->SetName("z");        zval->Allocate(tw * th);
	vtkNew<vtkFloatArray> nrm;  nrm->SetNumberOfComponents(3); nrm->SetName("Normals");
	nrm->SetNumberOfTuples((vtkIdType)tw * th);

	for (int jj = 0; jj < th; ++jj) {
		const int j = ys[jj]; const double y = y0 + j * dy;
		for (int ii = 0; ii < tw; ++ii) {
			const int i = xs[ii]; const double x = x0 + i * dx;
			const float zz = z[(vtkIdType)i * ny + j];
			// NaN node: geometry z pinned to the floor (valid mesh, flat hole), z SCALAR kept NaN so
			// the CTF paints it with the NaN fill colour.
			pts->InsertNextPoint(x, y, std::isnan(zz) ? fillZ : zz);
			zval->InsertNextValue(zz);
			// ILLUMINATION COMES FROM THE GRID, NOT FROM THE MESH. The normal is computed from this
			// node's TRUE FULL-RESOLUTION neighbours (i±1, j±1), never from the LOD-sampled ones, so
			// one node has ONE normal at every zoom level -- which is the only way the hillshade can
			// be the same illumination at every zoom step.
			//
			// It used to read the SAMPLED neighbours (step nodes away), so a node's normal changed
			// with the LOD: fine detail at step 1, progressively smoothed as the view pulled back, and
			// re-baked by hillshadeMapper on every refinement. That is the shifting, re-generating
			// illumination the user saw while zooming -- and it also disagreed with makeGridFromArray,
			// the non-tiled builder, which has always used the full-resolution neighbours.
			const int il = i > 0 ? i-1 : i, ir = i < nx-1 ? i+1 : i;
			const int jdF = j > 0 ? j-1 : j, juF = j < ny-1 ? j+1 : j;
			float nxv = 0.f, nyv = 0.f, nzv = 1.f;
			if (!std::isnan(zz)) {
				const float zl = z[(vtkIdType)il*ny + j],   zr = z[(vtkIdType)ir*ny + j];
				const float zb = z[(vtkIdType)i*ny + jdF],  zt = z[(vtkIdType)i*ny + juF];
				if (!std::isnan(zl) && !std::isnan(zr) && !std::isnan(zb) && !std::isnan(zt)) {
					const double ddx = (ir - il) * dx, ddy = (juF - jdF) * dy;   // full-res spans too
					const double gx = ddx != 0.0 ? (zr - zl) / ddx : 0.0;
					const double gy = ddy != 0.0 ? (zt - zb) / ddy : 0.0;
					double vx = -gx, vy = -gy, vz = 1.0;
					const double inv = 1.0 / std::sqrt(vx*vx + vy*vy + vz*vz);
					nxv = (float)(vx*inv); nyv = (float)(vy*inv); nzv = (float)(vz*inv);
				}
			}
			const vtkIdType lid = (vtkIdType)jj * tw + ii;
			nrm->SetTypedComponent(lid, 0, nxv);
			nrm->SetTypedComponent(lid, 1, nyv);
			nrm->SetTypedComponent(lid, 2, nzv);
		}
	}

	vtkNew<vtkCellArray> cells;
	if ((double)tw * th < 2.0e9) cells->Use32BitStorage();
	for (int jj = 0; jj < th - 1; ++jj) {
		for (int ii = 0; ii < tw - 1; ++ii) {
			const vtkIdType a = (vtkIdType)jj    * tw + ii;
			const vtkIdType b = (vtkIdType)jj    * tw + ii + 1;
			const vtkIdType c = (vtkIdType)(jj+1)* tw + ii + 1;
			const vtkIdType d = (vtkIdType)(jj+1)* tw + ii;
			// A NaN IS A DATA HOLE — same rule as makeGridFromArray above, same reason (a hole has
			// no geometry, so no light can reach it). The sampled corner nodes are the ones actually
			// meshed at this LOD step, so a coarse tile's hole follows what it draws.
			if (std::isnan(z[(vtkIdType)xs[ii  ]*ny + ys[jj  ]]) || std::isnan(z[(vtkIdType)xs[ii+1]*ny + ys[jj  ]]) ||
			    std::isnan(z[(vtkIdType)xs[ii+1]*ny + ys[jj+1]]) || std::isnan(z[(vtkIdType)xs[ii  ]*ny + ys[jj+1]]))
				continue;
			const vtkIdType quad[4] = { a, b, c, d };
			cells->InsertNextCell(4, quad);
		}
	}

	vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
	pd->SetPoints(pts);
	pd->SetPolys(cells);
	pd->GetPointData()->SetScalars(zval);
	pd->GetPointData()->SetNormals(nrm);
	return pd;
}

// Build a point cloud as vtkPolyData: N vertices (one Verts cell each) coloured by their
// z scalar. A Verts-only polydata renders as points; the rubber-band selector indexes
// these point ids. Mirrors GMTF3D view_points' EMPTY-sided mesh (a pure point cloud).
static vtkSmartPointer<vtkPolyData> makePointCloud(const double *xyz, int npts,
												   double &zmin, double &zmax) {
	vtkNew<vtkPoints>     pts;  pts->SetDataTypeToDouble(); pts->Allocate(npts);
	vtkNew<vtkFloatArray> zval; zval->SetName("z");        zval->Allocate(npts);
	vtkNew<vtkCellArray>  verts;
	zmin = 1e30; zmax = -1e30;
	for (int i = 0; i < npts; ++i) {
		const double x = xyz[3*i], y = xyz[3*i+1], z = xyz[3*i+2];
		const vtkIdType id = pts->InsertNextPoint(x, y, z);
		zval->InsertNextValue(z);
		verts->InsertNextCell(1, &id);
		if (z < zmin) zmin = z;
		if (z > zmax) zmax = z;
	}
	if (zmin > zmax) { zmin = 0.0; zmax = 1.0; }   // all-equal / empty guard
	vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
	pd->SetPoints(pts);
	pd->SetVerts(verts);
	pd->GetPointData()->SetScalars(zval);
	return pd;
}

// Build an arbitrary FV mesh (GMTfv solids / polygons) as vtkPolyData: `nv` SHARED vertices
// (xyz triples), `nfaces` polygon cells (corner counts in sides[], flat 0-based corner ids in
// indices[] = sum(sides) entries). An optional per-face RGB array (facergb[nfaces*3], 0..255)
// is attached as DIRECT cell-data colours (flat per-face shading, explicit/categorical). Else
// when `facez` (nfaces) is given it is attached as a per-FACE z SCALAR -> faceted colouring
// through the CPT/CTF that ALSO feeds the colorbar (so the two MATCH). Else a per-vertex z
// scalar drives smooth CPT colouring. Fills zmin/zmax from the vertex z. Mirrors GMTF3D
// fv_to_mesh's shared-vertex packing (sides/indices), but lets VTK tessellate n-gons + compute
// normals (no Julia-side normal pass).
static vtkSmartPointer<vtkPolyData> makeFvMesh(const double *xyz, int nv,
											   const int *sides, int nfaces, const int *indices,
											   const unsigned char *facergb, const double *facez,
											   double &zmin, double &zmax) {
	vtkNew<vtkPoints>     pts;  pts->SetDataTypeToDouble(); pts->SetNumberOfPoints(nv);
	vtkNew<vtkFloatArray> zval; zval->SetName("z"); zval->SetNumberOfComponents(1); zval->SetNumberOfTuples(nv);
	zmin = 1e30; zmax = -1e30;
	for (int i = 0; i < nv; ++i) {
		const double x = xyz[3*i], y = xyz[3*i+1], z = xyz[3*i+2];
		pts->SetPoint(i, x, y, z);
		zval->SetValue(i, (float)z);
		if (z < zmin) zmin = z;
		if (z > zmax) zmax = z;
	}
	if (zmin > zmax) { zmin = 0.0; zmax = 1.0; }   // all-equal / empty guard

	vtkNew<vtkCellArray> cells;
	int off = 0;
	for (int f = 0; f < nfaces; ++f) {
		const int np = sides[f];
		cells->InsertNextCell(np);
		for (int a = 0; a < np; ++a)
			cells->InsertCellPoint(indices[off + a]);
		off += np;
	}

	vtkSmartPointer<vtkPolyData> pd = vtkSmartPointer<vtkPolyData>::New();
	pd->SetPoints(pts);
	pd->SetPolys(cells);
	pd->GetPointData()->SetScalars(zval);

	if (facergb) {
		vtkNew<vtkUnsignedCharArray> col;
		col->SetName("facecolors");
		col->SetNumberOfComponents(3);
		col->SetNumberOfTuples(nfaces);
		for (int f = 0; f < nfaces; ++f)
			col->SetTuple3(f, facergb[3*f], facergb[3*f+1], facergb[3*f+2]);
		pd->GetCellData()->SetScalars(col);
	}
	else if (facez) {
		vtkNew<vtkFloatArray> fz;
		fz->SetName("facez");
		fz->SetNumberOfComponents(1);
		fz->SetNumberOfTuples(nfaces);
		for (int f = 0; f < nfaces; ++f)
			fz->SetValue(f, (float)facez[f]);
		pd->GetCellData()->SetScalars(fz);
	}
	return pd;
}

// "Nice" axis step (Heckbert) — round 1/2/5 ×10^n covering the range.
static double niceNum(double range, bool round) {
	if (range <= 0.0)
		return 1.0;
	double expv = std::floor(std::log10(range));
	double f = range / std::pow(10.0, expv);
	double nf;
	if (round)
		nf = (f < 1.5) ? 1.0 : (f < 3.0) ? 2.0 : (f < 7.0) ? 5.0 : 10.0;
	else
		nf = (f <= 1.0) ? 1.0 : (f <= 2.0) ? 2.0 : (f <= 5.0) ? 5.0 : 10.0;
	return nf * std::pow(10.0, expv);
}

// Same tick geometry as placeTickBillboards below (kept in lockstep on purpose — same spacing via
// niceNum, same outward-direction math), but NO number billboards: just the outward tick segments,
// appended to tpts/tlines. Used to complete a flat-2-D map's frame on the far (un-annotated) edge
// once the near edge already got the real, numbered ticks — SACRED_LAW.md: every mapping display
// needs axes on all 4 sides, annotations belong on the near (south/west) pair only.
static void placeTickMarksOnly(double v0, double v1, double d0, double d1,
		int axis, double fixedA, double fixedB, const double ctr[3],
		vtkPoints *tpts, vtkCellArray *tlines, double tickLen) {
	const double vspan = v1 - v0;
	if (std::abs(vspan) <= 1e-12 || std::abs(d1 - d0) <= 1e-12 || !tpts || !tlines) return;
	const double range = niceNum(std::abs(vspan), false);
	const double step  = niceNum(range / 4.0, true);
	const double lo = std::min(v0, v1), hi = std::max(v0, v1);
	const double eps = 1e-9 * std::abs(vspan);
	for (double v = std::ceil(lo / step) * step; v <= hi + eps; v += step) {
		if (v < lo - eps) continue;
		const double frac = (v - v0) / vspan;
		const double dpos = d0 + frac * (d1 - d0);
		double p[3];
		if      (axis == 0) { p[0] = dpos;   p[1] = fixedA; p[2] = fixedB; }
		else if (axis == 1) { p[0] = fixedA; p[1] = dpos;   p[2] = fixedB; }
		else                { p[0] = fixedA; p[1] = fixedB; p[2] = dpos;   }
		double wo[3] = { p[0]-ctr[0], p[1]-ctr[1], p[2]-ctr[2] };
		wo[axis] = 0.0;
		double wl = std::sqrt(wo[0]*wo[0] + wo[1]*wo[1] + wo[2]*wo[2]);
		if (wl > 1e-9) { wo[0]/=wl; wo[1]/=wl; wo[2]/=wl; }
		else { wo[0] = (axis==0?0.0:-1.0); wo[1] = (axis==1?0.0:-1.0); wo[2] = 0.0; }
		const double q[3] = { p[0]+wo[0]*tickLen, p[1]+wo[1]*tickLen, p[2]+wo[2]*tickLen };
		vtkIdType ia = tpts->InsertNextPoint(p);
		vtkIdType ib = tpts->InsertNextPoint(q);
		tlines->InsertNextCell(2); tlines->InsertCellPoint(ia); tlines->InsertCellPoint(ib);
	}
}

// Lay one axis' value labels along the chosen edge as horizontal screen-facing billboards, with
// ONE clean tickmark per label (appended to tpts/tlines). The tick points OUTWARD in world space
// (perpendicular to the axis, away from the cube centre) — a SINGLE mark, never the cube's
// two-faces double. The label is then pushed in screen space PAST the tick's end so it never sits
// on the axis or the tick. Values are TRUE (v0..v1); positions follow the DRAWN coords (d0..d1).
// Actors pooled + reused. axis: 0=x varies, 1=y varies, 2=z varies; other two coords pinned to
// the edge (fixedA, fixedB). tickLen is the tick length in world units.
static void placeTickBillboards(Scene *s,
		std::vector<vtkSmartPointer<vtkBillboardTextActor3D>> &pool,
		double v0, double v1, double d0, double d1,
		int axis, double fixedA, double fixedB, const double ctr[3],
		vtkPoints *tpts, vtkCellArray *tlines, double tickLen) {
	size_t used = 0;
	const double vspan = v1 - v0;
	if (std::abs(vspan) > 1e-12 && std::abs(d1 - d0) > 1e-12) {
		const double range = niceNum(std::abs(vspan), false);
		const double step  = niceNum(range / 4.0, true);   // ~5 ticks
		const double lo = std::min(v0, v1), hi = std::max(v0, v1);
		const double eps = 1e-9 * std::abs(vspan);
		for (double v = std::ceil(lo / step) * step; v <= hi + eps; v += step) {
			if (v < lo - eps) continue;
			const double frac = (v - v0) / vspan;
			const double dpos = d0 + frac * (d1 - d0);
			double p[3];
			if      (axis == 0) { p[0] = dpos;   p[1] = fixedA; p[2] = fixedB; }
			else if (axis == 1) { p[0] = fixedA; p[1] = dpos;   p[2] = fixedB; }
			else                { p[0] = fixedA; p[1] = fixedB; p[2] = dpos;   }

			// World outward = (edge - centre) with the along-axis part removed -> perpendicular to
			// the axis, pointing away from the cube. ONE direction -> ONE tick (no double).
			double wo[3] = { p[0]-ctr[0], p[1]-ctr[1], p[2]-ctr[2] };
			wo[axis] = 0.0;
			double wl = std::sqrt(wo[0]*wo[0] + wo[1]*wo[1] + wo[2]*wo[2]);
			if (wl > 1e-9) { wo[0]/=wl; wo[1]/=wl; wo[2]/=wl; }
			else { wo[0] = (axis==0?0.0:-1.0); wo[1] = (axis==1?0.0:-1.0); wo[2] = 0.0; }
			const double q[3] = { p[0]+wo[0]*tickLen, p[1]+wo[1]*tickLen, p[2]+wo[2]*tickLen };
			if (tpts && tlines) {                 // emit the single outward tick segment
				vtkIdType ia = tpts->InsertNextPoint(p);
				vtkIdType ib = tpts->InsertNextPoint(q);
				tlines->InsertNextCell(2); tlines->InsertCellPoint(ia); tlines->InsertCellPoint(ib);
			}

			if (used >= pool.size()) {            // grow the pool on demand
				vtkSmartPointer<vtkBillboardTextActor3D> nt = vtkSmartPointer<vtkBillboardTextActor3D>::New();
				vtkTextProperty *tp = nt->GetTextProperty();
				tp->SetColor(1.0, 1.0, 1.0);
				tp->SetFontFamilyToArial();          // SAME font + size on X/Y/Z (all billboards)
				tp->BoldOff(); tp->ItalicOff(); tp->ShadowOff();
				tp->SetFontSize(10);
				tp->SetJustificationToCentered();
				tp->SetVerticalJustificationToCentered();
				nt->ForceOpaqueOn();                 // never sorted/faded as translucent
				nt->PickableOff();
				// Live in the overlay layer, NOT s->ren: its headlight keeps the camera-facing
				// text equally bright at every view angle, and its own depth buffer stops the
				// surface from occluding the labels.
				(s->axesRen ? s->axesRen : s->ren)->AddViewProp(nt);
				pool.push_back(nt);
			}
			vtkBillboardTextActor3D *t = pool[used];
			char buf[64]; std::snprintf(buf, sizeof(buf), "%g", v);
			t->SetInput(buf);
			t->SetPosition(p[0], p[1], p[2]);
			// Offset the label in SCREEN space along the tick direction, PAST the tick's end:
			// project the edge point p and the tick end q, then push by |q-p|_screen + a gap.
			double sp[2], sq[2];
			s->ren->SetWorldPoint(p[0], p[1], p[2], 1.0); s->ren->WorldToDisplay();
			{ double *dd = s->ren->GetDisplayPoint(); sp[0] = dd[0]; sp[1] = dd[1]; }
			s->ren->SetWorldPoint(q[0], q[1], q[2], 1.0); s->ren->WorldToDisplay();
			{ double *dd = s->ren->GetDisplayPoint(); sq[0] = dd[0]; sq[1] = dd[1]; }
			double ox = sq[0]-sp[0], oy = sq[1]-sp[1];
			double tl = std::sqrt(ox*ox + oy*oy);
			if (tl > 1e-6) { ox /= tl; oy /= tl; } else { ox = 0; oy = -1; }
			const int off = int(tl) + 16;          // sit just past the tick end (+16 px gap)
			t->SetDisplayOffset(int(ox * off), int(oy * off));
			// Anchor the text's INNER edge at the offset point and let it grow OUTWARD only, so a
			// long label never spills back inside the cube. The outward screen dir (ox,oy) chooses
			// the justification: e.g. outward up-right -> anchor bottom-left -> text extends up-right.
			vtkTextProperty *jp = t->GetTextProperty();
			if      (ox >  0.30) jp->SetJustificationToLeft();
			else if (ox < -0.30) jp->SetJustificationToRight();
			else                 jp->SetJustificationToCentered();
			if      (oy >  0.30) jp->SetVerticalJustificationToBottom();
			else if (oy < -0.30) jp->SetVerticalJustificationToTop();
			else                 jp->SetVerticalJustificationToCentered();
			t->SetVisibility(1);
			++used;
		}
	}
	for (size_t i = used; i < pool.size(); ++i) pool[i]->SetVisibility(0);   // hide unused
}

// Place one axis NAME title billboard at the midpoint of the axis' camera-near edge, pushed
// OUTWARD in screen space past the number labels so it never overlaps them or the cube. Same
// overlay-billboard mechanism as the tick numbers (cube-native titles don't render in this setup).
static void placeAxisTitle(Scene *s, vtkBillboardTextActor3D *t, int axis,
						   double dmid, double fixedA, double fixedB,
						   const double ctr[3], double tickLen) {
	if (!t) return;
	double p[3];
	if      (axis == 0) { p[0] = dmid;   p[1] = fixedA; p[2] = fixedB; }
	else if (axis == 1) { p[0] = fixedA; p[1] = dmid;   p[2] = fixedB; }
	else                { p[0] = fixedA; p[1] = fixedB; p[2] = dmid;   }
	double wo[3] = { p[0]-ctr[0], p[1]-ctr[1], p[2]-ctr[2] };
	wo[axis] = 0.0;
	double wl = std::sqrt(wo[0]*wo[0] + wo[1]*wo[1] + wo[2]*wo[2]);
	if (wl > 1e-9) { wo[0]/=wl; wo[1]/=wl; wo[2]/=wl; }
	else { wo[0] = (axis==0?0.0:-1.0); wo[1] = (axis==1?0.0:-1.0); wo[2] = 0.0; }
	const double q[3] = { p[0]+wo[0]*tickLen, p[1]+wo[1]*tickLen, p[2]+wo[2]*tickLen };
	t->SetPosition(p[0], p[1], p[2]);
	double sp[2], sq[2];
	s->ren->SetWorldPoint(p[0], p[1], p[2], 1.0); s->ren->WorldToDisplay();
	{ double *dd = s->ren->GetDisplayPoint(); sp[0] = dd[0]; sp[1] = dd[1]; }
	s->ren->SetWorldPoint(q[0], q[1], q[2], 1.0); s->ren->WorldToDisplay();
	{ double *dd = s->ren->GetDisplayPoint(); sq[0] = dd[0]; sq[1] = dd[1]; }
	double ox = sq[0]-sp[0], oy = sq[1]-sp[1];
	double tl = std::sqrt(ox*ox + oy*oy);
	if (tl > 1e-6) { ox /= tl; oy /= tl; } else { ox = 0; oy = -1; }
	const int off = int(tl) + 96;          // sit WELL past the number labels (they use +16)
	t->SetDisplayOffset(int(ox * off), int(oy * off));
	// Anchor so the title grows OUTWARD only (never back over the numbers), same as the numbers.
	vtkTextProperty *jp = t->GetTextProperty();
	if      (ox >  0.30) jp->SetJustificationToLeft();
	else if (ox < -0.30) jp->SetJustificationToRight();
	else                 jp->SetJustificationToCentered();
	if      (oy >  0.30) jp->SetVerticalJustificationToBottom();
	else if (oy < -0.30) jp->SetVerticalJustificationToTop();
	else                 jp->SetVerticalJustificationToCentered();
	t->SetVisibility(1);
}

// Z tick labels are horizontal screen-facing billboards (perpendicular to Z) on the camera-nearest
// vertical edge; X/Y tick numbers are billboards on the nearer floor edges. Axis NAME titles are
// also billboards (placeAxisTitle). All recomputed every render as the near edges change with view.
// 3-D cube: overwrite the (VE-scaled) Z bounds with the whole cube's pinned z-range so the axis
// box is identical on every layer. No-op unless the scene has a cube z-lock. Scales the stored
// (unscaled) data range by the current zfac*ve, matching surfGetBounds' scaled space.
static inline void pinCubeAxisZ(Scene *s, double b[6]) {
	if (!s->cubeZLock) return;
	const double zs = s->zfac * s->ve;
	b[4] = s->cubeZMin * zs;
	b[5] = s->cubeZMax * zs;
}

// Put the axes CUBE's box on `bIn`. ONE function for it: applyVE calls it when the geometry/VE
// changed, rebuildAxisLabels calls it every render — so when the ACTIVE grid changes (a derived
// variable in different units becomes the visible one) the box re-fits on its own, with no call
// site to hunt down. Takes a COPY: the degenerate-Z guard (a zero range makes vtkCubeAxesActor
// compute a NaN label count and abort the render) must not leak back into the caller's bounds,
// which drive the flat-map Z-hide test. Only touches the actor when the box actually moved —
// SetBounds always Modified()s, and this runs inside the render's StartEvent.
static inline void axesSetBounds(Scene *s, AxesSet &A, const double bIn[6]) {
	if (!A.cube) return;
	double b[6];
	for (int i = 0; i < 6; ++i) b[i] = bIn[i];
	if (b[5] <= b[4]) b[5] = b[4] + 1.0;         // flat-Z (flat-2D collapses VE to 0) — unchanged rule
	// X or Y degenerate too: any flat SHEET standing on edge has one (a plane of quads at constant x,
	// a single-row grid). vtkAxisActor turns a zero span into an invalid tick count ("Number of labels
	// -2147483647"), so give that axis a span proportionate to the ones that do have extent. Guarded
	// HERE, at the one axes-cube box setter every caller goes through (applyVE, rebuildAxisLabels,
	// gmtvtk_reframe_h), so it holds for every data source at once — SACRED_LAW.md's own lesson: fix
	// the shared source a bounds value has many consumers of, never each call site.
	for (int k = 0; k < 4; k += 2) {
		if (b[k + 1] > b[k]) continue;
		double pad = 0.0;
		for (int j = 0; j < 6; j += 2) pad = std::max(pad, b[j + 1] - b[j]);
		pad = (pad > 0.0) ? pad * 0.5 : 1.0;
		b[k] -= pad; b[k + 1] += pad;
	}
	double cur[6]; A.cube->GetBounds(cur);
	for (int i = 0; i < 6; ++i)
		if (std::abs(cur[i] - b[i]) > 1e-9 * (1.0 + std::abs(b[i]))) { A.cube->SetBounds(b); return; }
}

// ===================== per-raster axes: the ONE set of primitives ============
// SACRED_LAW.md Raster-own-axes law. Every raster's axes go through THESE and only these, so a set
// can only ever be built, framed, boxed, labelled or torn down as a WHOLE, by its OWN owner.

// A set's drawn (xfac / zfac*ve scaled) box, from ITS OWN true-data frame. The ONE place a set's
// data limits become world coordinates — the cube box, the tick billboards and the camera fit all
// read it, so they can never disagree about where a raster's axes are.
static inline void axesScaledBox(Scene *s, const AxesSet &A, double b[6]) {
	const double zs = s->zfac * s->ve;
	b[0] = A.x0 * s->xfac; b[1] = A.x1 * s->xfac;
	b[2] = A.y0;           b[3] = A.y1;
	b[4] = A.z0 * zs;      b[5] = A.z1 * zs;
}

// Point a set at ITS OWN raster's limits. THE only way a frame is ever set: a caller names the set
// it owns and hands it that raster's own numbers — there is no call that can re-frame "the window",
// so no handle can move another's axes.
static inline void axesSetFrame(AxesSet &A, double x0, double x1, double y0, double y1,
                                double z0, double z1, int geog) {
	A.x0 = x0; A.x1 = x1; A.y0 = y0; A.y1 = y1; A.z0 = z0; A.z1 = z1; A.geog = geog;
}

// Build a raster's axes: its own cube, its own tickmark actor, its own two axis-NAME billboards.
// ONE builder for every raster in the window — the base surface and every dropped/derived grid,
// image or mesh all get an IDENTICAL, independent set (SACRED_LAW.md: same operation, same
// function; never a second axes-construction path for "extras"). Idempotent: a set already built
// is left alone. `addToRen` false = an empty launcher, which must not flash an axis box before it
// holds data; the set is completed and shown by axesShow once the raster arrives.
static void axesBuild(Scene *s, AxesSet &A, bool addToRen) {
	if (A.built || !s || !s->ren) return;
	A.cube = vtkSmartPointer<vtkCubeAxesActor>::New();
	A.cube->SetCamera(s->ren->GetActiveCamera());
	double b[6]; axesScaledBox(s, A, b);
	if (b[5] - b[4] <= 0.0) b[5] = b[4] + 1.0;      // zero Z extent (bare image / flat) -> avoid
	                                                 // vtkAxisActor 0/0 label-count crash
	A.cube->SetBounds(b);
	A.cube->SetXAxisRange(A.x0, A.x1);               // TRUE labels despite the actor scale
	A.cube->SetYAxisRange(A.y0, A.y1);
	A.cube->SetZAxisRange(A.z0, A.z1);
	A.cube->GetTitleTextProperty(0)->SetColor(0.9, 0.9, 0.9);
	A.cube->GetLabelTextProperty(0)->SetColor(0.8, 0.8, 0.8);
	A.cube->GetXAxesLinesProperty()->SetColor(0.7, 0.7, 0.7);
	// Geographic data -> lon/lat axis names; cartesian -> X/Y. Z always unnamed. Drawn as overlay
	// billboards (placeAxisTitle), NOT cube-native titles (those don't render in this StaticTriad
	// setup with native labels off). Clear the cube titles so nothing competes.
	A.cube->SetXTitle(" "); A.cube->SetYTitle(" "); A.cube->SetZTitle(" ");  // single space, NOT "" — empty makes vtkVectorText error "Text is not set!" every render
	A.name[0] = A.geog ? "lon" : "X";
	A.name[1] = A.geog ? "lat" : "Y";                // X/Y names only — Z gets NO name title
	for (int i = 0; i < 2; ++i) {
		auto t = vtkSmartPointer<vtkBillboardTextActor3D>::New();
		vtkTextProperty *tp = t->GetTextProperty();
		tp->SetColor(1.0, 1.0, 1.0);
		tp->SetFontFamilyToArial(); tp->BoldOn(); tp->ItalicOff(); tp->ShadowOff();
		tp->SetFontSize(13);                         // a touch larger + bold so the name reads as a title
		tp->SetJustificationToCentered();
		tp->SetVerticalJustificationToCentered();
		t->SetInput(A.name[i].c_str());
		t->ForceOpaqueOn();
		t->PickableOff();
		t->SetVisibility(0);                         // rebuildAxesFor positions + shows it
		(s->axesRen ? s->axesRen : s->ren)->AddViewProp(t);   // overlay layer: even brightness, never occluded
		A.title[i] = t;
	}
	A.cube->DrawXGridlinesOn(); A.cube->DrawYGridlinesOn(); A.cube->DrawZGridlinesOn();
	A.cube->SetGridLineLocation(vtkCubeAxesActor::VTK_GRID_LINES_FURTHEST);
	// StaticTriad pins X,Y to the zmin FLOOR (coplanar) — Y/X labels are ALWAYS on the bottom
	// edge, never lifting to a top edge — with native 3-D text (parallel/orthogonal to the axis,
	// reorienting with the view). Mirrors the user's f3d_ext_cube_axes.cxx (his HARD RULE). Z's
	// OWN labels run along the axis -> OFF; Z values are drawn as horizontal billboards (ALWAYS
	// perpendicular to Z) by rebuildAxesFor().
	A.cube->SetFlyModeToStaticTriad();
	A.cube->SetTickLocationToOutside();
	A.cube->SetScreenSize(13.0);
	A.cube->SetZAxisVisibility(1);                   // draw the Z axis LINE (+ gridlines) like X/Y
	// Native value labels AND native ticks OFF on ALL THREE axes. rebuildAxesFor draws the values as
	// identical freetype billboards AND draws our own SINGLE outward tickmark per label (A.ticks) —
	// the cube's native ticks were doubled across the two faces sharing each edge. Only the cube's
	// axis LINES + gridlines remain.
	A.cube->SetXAxisLabelVisibility(0);
	A.cube->SetYAxisLabelVisibility(0);
	A.cube->SetZAxisLabelVisibility(0);
	A.cube->SetXAxisTickVisibility(0);
	A.cube->SetYAxisTickVisibility(0);
	A.cube->SetZAxisTickVisibility(0);
	// MAJOR ticks only on every axis. Minor ticks defaulted ON and made a dense two-directional
	// comb on Z (its range is thousands, so minor=majorDelta/5 packed ~30 marks; X/Y ranges are
	// small so theirs stayed sparse) -> Z now ticks like X/Y.
	A.cube->XAxisMinorTickVisibilityOff();
	A.cube->YAxisMinorTickVisibilityOff();
	A.cube->ZAxisMinorTickVisibilityOff();
	for (int i = 0; i < 3; ++i) {                    // white, ARIAL, non-bold -> X/Y/Z share ONE font
		vtkTextProperty *tp = A.cube->GetTitleTextProperty(i);
		tp->SetColor(1.0, 1.0, 1.0); tp->SetFontFamilyToArial(); tp->BoldOff(); tp->ItalicOff(); tp->ShadowOff();
		vtkTextProperty *lp = A.cube->GetLabelTextProperty(i);
		lp->SetColor(1.0, 1.0, 1.0); lp->SetFontFamilyToArial(); lp->BoldOff(); lp->ItalicOff(); lp->ShadowOff();
	}
	// Our own SINGLE outward tickmarks (rebuilt every render by rebuildAxesFor). Unlit grey lines,
	// like the cube's axis lines; the cube's native (doubled) ticks are off.
	A.tickPD = vtkSmartPointer<vtkPolyData>::New();
	vtkNew<vtkPolyDataMapper> tm; tm->SetInputData(A.tickPD);
	A.ticks = vtkSmartPointer<vtkActor>::New();
	A.ticks->SetMapper(tm);
	A.ticks->GetProperty()->SetColor(0.85, 0.85, 0.85);
	A.ticks->GetProperty()->LightingOff();
	A.ticks->GetProperty()->SetLineWidth(1.0);
	A.ticks->PickableOff();
	if (addToRen) { s->ren->AddActor(A.cube); s->ren->AddActor(A.ticks); }
	A.built = addToRen;      // not yet in the renderer -> not finished; axesShow completes it
}

// Put a built-but-not-yet-added set on screen (the empty-launcher promote path). Separate from
// axesBuild only so a blank window can hold a set that draws nothing until its raster arrives.
static inline void axesShow(Scene *s, AxesSet &A) {
	if (!s || !s->ren || !A.cube || A.built) return;
	s->ren->AddActor(A.cube);
	if (A.ticks) s->ren->AddActor(A.ticks);
	A.built = true;
}

// Tear a raster's axes down WITH the raster. Removing an object must take its axes with it — the
// set is the handle's property, so nothing of it may outlive the handle (SACRED_LAW.md "removal
// undoes what add did"; the user's own repro was a deleted crop leaving behind an axes box no row
// could ever clear again).
static void axesDestroy(Scene *s, AxesSet &A) {
	if (!s) return;
	vtkRenderer *ov = (s->axesRen ? s->axesRen.Get() : s->ren.Get());
	if (s->ren) {
		if (A.cube)  s->ren->RemoveActor(A.cube);
		if (A.ticks) s->ren->RemoveActor(A.ticks);
	}
	if (ov) {
		for (auto &t : A.title) if (t) ov->RemoveViewProp(t);
		for (auto &l : A.xlab)  if (l) ov->RemoveViewProp(l);
		for (auto &l : A.ylab)  if (l) ov->RemoveViewProp(l);
		for (auto &l : A.zlab)  if (l) ov->RemoveViewProp(l);
	}
	A.title[0] = A.title[1] = nullptr;
	A.xlab.clear(); A.ylab.clear(); A.zlab.clear();
	A.cube = nullptr; A.ticks = nullptr; A.tickPD = nullptr;
	A.built = false;
}

// Hide every renderable piece of a set in one go — used when the owning handle is unchecked, or the
// set's own "Axes" row is. A set is all-or-nothing: box, ticks, numbers and titles share the owner's
// fate, so a hidden raster can never leave stray axis text on screen.
static inline void axesHideAll(AxesSet &A) {
	if (A.cube)  A.cube->SetVisibility(0);
	if (A.ticks) A.ticks->SetVisibility(0);
	for (auto &l : A.xlab)  if (l) l->SetVisibility(0);
	for (auto &l : A.ylab)  if (l) l->SetVisibility(0);
	for (auto &l : A.zlab)  if (l) l->SetVisibility(0);
	for (auto &t : A.title) if (t) t->SetVisibility(0);
}

// Current visible world region (W/E/S/N in TRUE data coords) = the part of the map on screen at the
// CURRENT camera/zoom: project the 4 viewport corners onto the z=0 plane (the flat map), undo the X
// aspect scale (xfac), take the bbox, clamp to the data frame. The ONE source of "what's on screen
// right now" — buildAndShow's own `visibleRegion` lambda (70_window.cpp, used by every Geography/
// Seismology leaf) is a one-line forwarder to this; the Link tool's cross-window camera sync
// (57_swipe.cpp) reads the SAME thing off the SOURCE window before pointing the partner at it. false
// if there is no renderer/window yet (an empty launcher with no camera set up).
static bool sceneVisibleRegion(Scene *s, double &W, double &E, double &S, double &N) {
	if (!s || !s->ren || !s->widget || !s->widget->renderWindow()) return false;
	const int *sz = s->widget->renderWindow()->GetSize();
	const double w = sz[0], h = sz[1];
	const double gx = (s->xfac != 0.0) ? s->xfac : 1.0;
	const double corners[4][2] = { {0,0}, {w,0}, {0,h}, {w,h} };
	bool any = false;
	for (const auto &c : corners) {
		double nr[4], fr[4];
		s->ren->SetDisplayPoint(c[0], c[1], 0.0); s->ren->DisplayToWorld();
		for (int i = 0; i < 4; ++i) nr[i] = s->ren->GetWorldPoint()[i];
		s->ren->SetDisplayPoint(c[0], c[1], 1.0); s->ren->DisplayToWorld();
		for (int i = 0; i < 4; ++i) fr[i] = s->ren->GetWorldPoint()[i];
		if (nr[3] != 0.0) { nr[0] /= nr[3]; nr[1] /= nr[3]; nr[2] /= nr[3]; }
		if (fr[3] != 0.0) { fr[0] /= fr[3]; fr[1] /= fr[3]; fr[2] /= fr[3]; }
		const double dirz = fr[2] - nr[2];
		if (dirz == 0.0) continue;                          // ray parallel to the map plane
		const double t0 = -nr[2] / dirz;
		const double tx = (nr[0] + t0 * (fr[0] - nr[0])) / gx;
		const double ty =  nr[1] + t0 * (fr[1] - nr[1]);
		if (!any) { W = E = tx; S = N = ty; any = true; }
		else { W = std::min(W, tx); E = std::max(E, tx); S = std::min(S, ty); N = std::max(N, ty); }
	}
	if (!any) return false;
	// Never exceed the data frame — of the raster ACTUALLY ON DISPLAY, which is the one the caller
	// means by "what's on screen right now". s->x0..y1 is the BASE raster's own frame and would clamp
	// a crop/derived layer back to its parent's extent (SACRED_LAW.md Raster-own-axes law: no reading
	// another raster's limits). surfGetBounds carries the same answer in scaled space.
	double fb[6]; surfGetBounds(s, fb);
	W = std::max(W, fb[0] / gx); E = std::min(E, fb[1] / gx);
	S = std::max(S, fb[2]);      N = std::min(N, fb[3]);
	return (E > W && N > S);
}

// Point the camera at `b` (an already xfac/zfac/ve-SCALED world bbox) using the SAME re-center +
// NDC-fit-zoom technique gmtvtk_reframe_z_h/fitSnapView use — WITHOUT touching the axes cube or the
// scene's own x0/x1/y0/y1 bookkeeping. gmtvtk_reframe_z_h (90_c_api.cpp) is this PLUS that axes/frame
// bookkeeping, extracted here so a caller that wants to LOOK AT a region without redefining what this
// window's data frame IS can reuse the exact same maths (SACRED_LAW: fix/share the source, never
// refork it) — the Link tool's cross-window camera sync (57_swipe.cpp) is exactly that caller: it
// must never touch the PARTNER window's own axes/frame, only point its camera.
static void cameraFitToScaledBBox(Scene *s, const double b[6], bool keepMargin) {
	if (!s || !s->ren || !s->widget || !s->widget->renderWindow()) return;
	vtkRenderer *ren = s->ren;
	vtkCamera *cam = ren->GetActiveCamera();
	{
		const double sc[3] = { 0.5*(b[0]+b[1]), 0.5*(b[2]+b[3]), 0.5*(b[4]+b[5]) };
		double pos[3], foc[3]; cam->GetPosition(pos); cam->GetFocalPoint(foc);
		const double d[3] = { pos[0]-foc[0], pos[1]-foc[1], pos[2]-foc[2] };
		cam->SetFocalPoint(sc);
		cam->SetPosition(sc[0]+d[0], sc[1]+d[1], sc[2]+d[2]);
	}
	const int *sz = s->widget->renderWindow()->GetSize();
	const double aspect = (sz && sz[1] > 0) ? double(sz[0]) / double(sz[1]) : 1.0;
	const double targetFill = !s->flat2d ? 0.88 : (keepMargin ? 0.84 : 1.0);
	for (int pass = 0; pass < 2; ++pass) {
		vtkMatrix4x4 *M = cam->GetCompositeProjectionTransformMatrix(aspect, -1.0, 1.0);
		double nx0=1e300, nx1=-1e300, ny0=1e300, ny1=-1e300;
		for (double cx : { b[0], b[1] })
			for (double cy : { b[2], b[3] })
				for (double cz : { b[4], b[5] }) {
					double p[4] = { cx, cy, cz, 1.0 }, o[4];
					M->MultiplyPoint(p, o);
					if (o[3] == 0.0) continue;
					const double ndcx = o[0]/o[3], ndcy = o[1]/o[3];
					nx0 = std::min(nx0, ndcx); nx1 = std::max(nx1, ndcx);
					ny0 = std::min(ny0, ndcy); ny1 = std::max(ny1, ndcy);
				}
		const double wfrac = (nx1 - nx0) / 2.0, hfrac = (ny1 - ny0) / 2.0;
		const double frac = s->flat2d ? std::max(wfrac, hfrac) : wfrac;
		if (frac <= 1e-6) break;
		cam->Zoom(targetFill / frac);
	}
	ren->ResetCameraClippingRange();
}

// The X/Y axis NAME titles, from the ACTIVE layer's own x,y kind. Geographic -> "lon"/"lat";
// cartesian -> "X"/"Y". ONE function for it (buildAndShow seeds the billboards, rebuildAxisLabels
// calls this every render), so the names re-follow the layer the moment the active one changes —
// a cartesian derived grid dropped into a geographic window must NOT keep saying lon/lat. Only
// touches the actors when the name actually changed (this runs inside the render's StartEvent).
// Names come from the SET's OWN `geog`, never from "the active layer": a cartesian derived grid is
// labelled X/Y and the geographic parent it was computed over stays lon/lat, at the same time, on
// their own axes. Only touches the actors when the name actually changed (runs inside the render's
// StartEvent).
static inline void syncAxisNames(Scene *s, AxesSet &A) {
	(void)s;
	const char *want[2] = { A.geog ? "lon" : "X", A.geog ? "lat" : "Y" };
	for (int i = 0; i < 2; ++i) {
		if (A.name[i] == want[i]) continue;
		A.name[i] = want[i];
		if (A.title[i]) A.title[i]->SetInput(A.name[i].c_str());
	}
}

// Draw ONE raster's axes, from ITS OWN frame. Nothing in here reads the window's primary surface,
// the "active" grid, or any other handle's state — the entire geometry comes from `A` (SACRED_LAW.md
// Raster-own-axes law: axes cannot be shared between rasters). `visible` is the owning handle's
// verdict: its container checkbox AND the set's own Axes row.
static void rebuildAxesFor(Scene *s, AxesSet &A, bool visible, bool isBase) {
	if (!s->ren || !s->ren->GetActiveCamera() || !A.cube) return;
	// Hidden owner (or its own Axes row unchecked): the WHOLE set goes dark. This callback fires
	// every render and would otherwise re-show the billboards, so honour the verdict here.
	if (!visible) { axesHideAll(A); return; }
	A.cube->SetVisibility(1);
	if (A.ticks) A.ticks->SetVisibility(1);
	double b[6]; axesScaledBox(s, A, b);         // THIS raster's own drawn (VE-scaled) bounds
	if (isBase) pinCubeAxisZ(s, b);              // 3-D cube: hold the BASE set's Z box to the whole
	                                             // cube's range (the cube variable IS the base surface)
	axesSetBounds(s, A, b);                      // re-box if VE / the raster's own frame moved
	const double ctr[3] = { 0.5*(b[0]+b[1]), 0.5*(b[2]+b[3]), 0.5*(b[4]+b[5]) };
	double cam[3]; s->ren->GetActiveCamera()->GetPosition(cam);

	// ALL THREE axes' value labels are IDENTICAL freetype billboards (same font + size on X/Y/Z;
	// the cube's native labels use a different text engine, so they are off). Each axis is labelled
	// on the box edge CLOSEST TO THE CAMERA so the labels stay in front and readable as the view
	// rotates: X on the nearer front/back floor edge (y=ymin|ymax), Y on the nearer left/right
	// floor edge (x=xmin|xmax), Z up the nearest vertical corner. We also draw our OWN single
	// outward tickmark per label (the cube's native ticks were doubled across two faces -> off).
	const double diag = std::sqrt((b[1]-b[0])*(b[1]-b[0]) + (b[3]-b[2])*(b[3]-b[2]) + (b[5]-b[4])*(b[5]-b[4]));
	// ===== TICKMARK LENGTH ===== world length of every axis tick = this fraction of the bbox
	// diagonal. Lower it for shorter ticks, raise it for longer.
	const double tickLen = 0.00625 * diag;
	// Pick the candidate value nearer the camera along one coordinate.
	auto nearer = [](double a, double c, double camc) { return std::abs(camc-a) <= std::abs(camc-c) ? a : c; };
	double xEdgeY = nearer(b[2], b[3], cam[1]);   // X labels on nearer y (front/back) floor edge
	double yEdgeX = nearer(b[0], b[1], cam[0]);   // Y labels on nearer x (left/right) floor edge
	// Top-down view ('2' snap, +Y up): pin the Y (north) annotations to the screen-left (xmin)
	// edge instead of the camera-near edge, so north labels always sit on the left of the map.
	double dop[3]; s->ren->GetActiveCamera()->GetDirectionOfProjection(dop);
	if (dop[2] < -0.999) yEdgeX = b[0];
	// Flat-2-D is a FIXED top-down view, never rotated -- "nearer the camera" is meaningless there
	// and, worse, unstable: an orthographic top-down camera's Y position often sits exactly on the
	// bbox's vertical centre, so `nearer(b[2],b[3],cam[1])` is a near-tie decided by float noise
	// (proven live: the SAME scene showed X annotations on the bottom edge on one run and not at
	// all on another). Pin X to south (b[2]) deterministically, same spirit as the existing Y pin
	// two lines up. SACRED_LAW.md: "all mapping displays must have axes on all 4 sides" -- the
	// mirrored tick-only marks on the far edges (below) complete the frame.
	if (s->flat2d) xEdgeY = b[2];
	// Z: nearest of the 4 vertical edges (compared at mid-height).
	double zx = b[0], zy = b[2], zbest = 1e300;
	for (double cx : { b[0], b[1] })
		for (double cy : { b[2], b[3] }) {
			const double dd = (cx-cam[0])*(cx-cam[0]) + (cy-cam[1])*(cy-cam[1]) + (ctr[2]-cam[2])*(ctr[2]-cam[2]);
			if (dd < zbest) { zbest = dd; zx = cx; zy = cy; }
		}
	vtkNew<vtkPoints> tp; vtkNew<vtkCellArray> tl;
	// Terminology: ANNOTATIONS = the coordinate NUMBERS (xlabels/ylabels billboards). LABELS =
	// the axis NAME titles (axTitle, e.g. "lon"/"lat"). Flat-2D is a top-down lon/lat MAP: it
	// KEEPS the X/Y coordinate numbers (like the imageOnly map) and only hides the axis-NAME
	// titles for a clean look. The Z axis is perpendicular to the screen in top-down view, so it
	// (line + numbers) is hidden below.
	const bool hideNames = s->flat2d || s->imageOnly;
	placeTickBillboards(s, A.xlab, A.x0, A.x1, b[0], b[1], 0, xEdgeY, b[4], ctr, tp, tl, tickLen);
	placeTickBillboards(s, A.ylab, A.y0, A.y1, b[2], b[3], 1, yEdgeX, b[4], ctr, tp, tl, tickLen);
	if (s->flat2d) {
		// Complete the frame: plain (un-numbered) ticks on the FAR edge from each annotated one --
		// south got the real X ticks above, so north gets the mirror; west got the real Y ticks,
		// so east gets the mirror. SACRED_LAW.md: axes on all 4 sides, always.
		const double xFar = (xEdgeY == b[2]) ? b[3] : b[2];
		const double yFar = (yEdgeX == b[0]) ? b[1] : b[0];
		placeTickMarksOnly(A.x0, A.x1, b[0], b[1], 0, xFar, b[4], ctr, tp, tl, tickLen);
		placeTickMarksOnly(A.y0, A.y1, b[2], b[3], 1, yFar, b[4], ctr, tp, tl, tickLen);
	}
	if (hideNames) {
		if (A.title[0]) A.title[0]->SetVisibility(0);
		if (A.title[1]) A.title[1]->SetVisibility(0);
	}
	else {
		// The NAMES come from THIS raster's own x,y kind — never from the window or from whatever
		// layer happens to be "active": a cartesian derived grid (a gravmag3d anomaly computed with
		// the dialog's "Geographic" unchecked -> its x,y are metres) reads "X"/"Y" on ITS axes while
		// the geographic parent keeps "lon"/"lat" on ITS OWN, simultaneously.
		syncAxisNames(s, A);
		// X/Y NAME labels at the midpoint of each floor edge, pushed well past the numbers. No Z name.
		placeAxisTitle(s, A.title[0], 0, 0.5*(b[0]+b[1]), xEdgeY, b[4], ctr, tickLen);
		placeAxisTitle(s, A.title[1], 1, 0.5*(b[2]+b[3]), yEdgeX, b[4], ctr, tickLen);
	}
	// Z axis: hide in flat-2D (top-down map -> Z points at the camera, meaningless) or when the
	// drawn Z extent is degenerate. Drives the cube Z LINE/gridlines + the Z number billboards so
	// the toggle is self-correcting every render (no stale state on 2D<->3D switch).
	const bool zHide = s->flat2d || (b[5] - b[4]) <= 0.0;
	A.cube->SetZAxisVisibility(zHide ? 0 : 1);
	if (zHide) A.cube->DrawZGridlinesOff(); else A.cube->DrawZGridlinesOn();
	// Flat map (no Z relief): the X/Y gridlines lie coplanar with the image, drawing a graticule
	// mesh over the map (and thin coplanar lines FXAA then re-thicknesses). Drop them when flat;
	// keep them in 3-D where they sit on the far box walls as a depth reference.
	if (zHide) { A.cube->DrawXGridlinesOff(); A.cube->DrawYGridlinesOff(); }
	else       { A.cube->DrawXGridlinesOn();  A.cube->DrawYGridlinesOn();  }
	if (zHide) {
		for (auto &l : A.zlab) l->SetVisibility(0);
	}
	else {
		// The Z NUMBERS come from THIS raster's OWN z range, in ITS OWN UNITS — the third layer of
		// the derived-variable axes law, now structural: a mGal anomaly can no longer be numbered in
		// its parent's metres because it does not share, and cannot reach, the parent's axes. The
		// only override is the 3-D cube's z-lock, which belongs to the BASE surface's own set (the
		// cube variable IS the base) so every layer of one cube is boxed alike.
		double zlo = A.z0, zhi = A.z1;
		if (isBase && s->cubeZLock) { zlo = s->cubeZMin; zhi = s->cubeZMax; }
		placeTickBillboards(s, A.zlab, zlo, zhi, b[4], b[5], 2, zx, zy, ctr, tp, tl, tickLen);
	}
	if (s->flat2d) {
		// The actual 4-side BORDER: a closed rectangle connecting the 4 corners, not just interval
		// ticks -- SACRED_LAW.md "all mapping displays must have axes on all 4 sides" means a real
		// frame, same as any GMT map border. Same tickPD line pipeline the ticks already use.
		vtkIdType c0 = tp->InsertNextPoint(b[0], b[2], b[4]);
		vtkIdType c1 = tp->InsertNextPoint(b[1], b[2], b[4]);
		vtkIdType c2 = tp->InsertNextPoint(b[1], b[3], b[4]);
		vtkIdType c3 = tp->InsertNextPoint(b[0], b[3], b[4]);
		tl->InsertNextCell(5);
		tl->InsertCellPoint(c0); tl->InsertCellPoint(c1); tl->InsertCellPoint(c2);
		tl->InsertCellPoint(c3); tl->InsertCellPoint(c0);
	}
	if (A.tickPD) {
		A.tickPD->SetPoints(tp);
		A.tickPD->SetLines(tl);
		A.tickPD->Modified();
	}
}

// The View menu's "Axes cube" entry is a WINDOW-WIDE VIEW COMMAND, not a handle: it flips every
// raster's OWN intent at once (and the screenshot paths use it to take the decoration out of the
// captured pixels and put it back). That is not sharing — each set keeps its own `shown` flag and
// its own frame; this just sets them all, the way "hide all overlays" would. NOTHING here reads or
// writes another handle's frame, which is what the law forbids.
static inline bool sceneAxesShown(Scene *s) {
	if (!s) return false;
	if (s->baseAxes.shown) return true;
	for (auto &ex : s->extras) if (ex.ax.shown) return true;
	return false;
}
// Is ANY raster's axes actually ON SCREEN right now? Distinct from sceneAxesShown above, which is
// the user's INTENT: an empty launcher's set is never added to the renderer (axesBuild's blankStart
// path) yet its `shown` still defaults true, and a hidden raster's set is intended-on but drawn off.
// The scene-state dump reports THIS, since a test asking "are there axes" means on screen.
static inline bool sceneAxesOnScreen(Scene *s) {
	if (!s) return false;
	auto live = [](const AxesSet &A) { return A.built && A.cube && A.cube->GetVisibility() != 0; };
	if (live(s->baseAxes)) return true;
	for (auto &ex : s->extras) if (live(ex.ax)) return true;
	return false;
}

static inline void sceneAxesSetShown(Scene *s, bool on) {
	if (!s) return;
	s->baseAxes.shown = on;
	for (auto &ex : s->extras) ex.ax.shown = on;
	if (!on) {                      // hide immediately; rebuildAxisLabels re-shows the visible ones
		axesHideAll(s->baseAxes);
		for (auto &ex : s->extras) axesHideAll(ex.ax);
	}
}

// Is a raster's OWN handle showing? An axes set is a property of its container, so it lives and dies
// with it — the group-uncheck law, applied to the one child row that used to escape it because the
// axes were window-level and there was only one of them to gate.
static inline bool extraVisible(const ExtraObj &ex) {
	if (ex.actor && ex.actor->GetVisibility()) return true;
	if (ex.drape && ex.drape->GetVisibility()) return true;
	return false;
}

// Keep the NaN backdrop under whatever raster is on display. Built lazily, refreshed only when the
// drawn bounds actually move (VE, a re-frame, a new active layer), so the per-render caller costs a
// bounds compare. Unlit and unscaled: it is built directly in the same scaled world the surface
// actors are drawn in, and it must show the NaN fill colour EXACTLY as the user set it -- a lit plane
// would tint it the same way the lit NaN quads used to.
static void nanPlaneUpdate(Scene *s) {
	if (!s || !s->ren) return;
	// Only a GRID can have NaNs. No grid layer on display -> nothing to back.
	double zlo, zhi;
	const bool haveGrid = activeGridZRange(s, zlo, zhi);
	if (!haveGrid) { if (s->nanPlane) s->nanPlane->SetVisibility(0); return; }
	double b[6]; surfGetBounds(s, b);
	const double diag = std::sqrt((b[1]-b[0])*(b[1]-b[0]) + (b[3]-b[2])*(b[3]-b[2]));
	const double zplane = b[4] - 1e-3 * ((b[5] > b[4]) ? (b[5]-b[4]) : (diag > 0 ? diag : 1.0));
	if (!s->nanPlane) {
		vtkNew<vtkPlaneSource> ps;
		vtkNew<vtkPolyDataMapper> m; m->SetInputConnection(ps->GetOutputPort());
		m->ScalarVisibilityOff();
		s->nanPlane = vtkSmartPointer<vtkActor>::New();
		s->nanPlane->SetMapper(m);
		s->nanPlane->GetProperty()->LightingOff();     // a hole is not illuminated. That is the point.
		s->ren->AddActor(s->nanPlane);
		s->nanPlaneBox[0] = s->nanPlaneBox[1] = 1e300;  // force the first build below
	}
	s->nanPlane->SetVisibility(1);
	s->nanPlane->GetProperty()->SetColor(s->nanColor[0], s->nanColor[1], s->nanColor[2]);
	const double want[6] = { b[0], b[1], b[2], b[3], zplane, zplane };
	bool same = true;
	for (int i = 0; i < 6; ++i)
		if (std::abs(want[i] - s->nanPlaneBox[i]) > 1e-9 * (1.0 + std::abs(want[i]))) { same = false; break; }
	if (same) return;
	for (int i = 0; i < 6; ++i) s->nanPlaneBox[i] = want[i];
	vtkPolyDataMapper *m = vtkPolyDataMapper::SafeDownCast(s->nanPlane->GetMapper());
	if (vtkPlaneSource *ps = m ? vtkPlaneSource::SafeDownCast(m->GetInputAlgorithm()) : nullptr) {
		ps->SetOrigin(b[0], b[2], zplane);
		ps->SetPoint1(b[1], b[2], zplane);
		ps->SetPoint2(b[0], b[3], zplane);
		ps->Update();
	}
}

// Every raster's axes, redrawn from ITS OWN frame. This is the whole of the window's axis work: a
// loop over independent sets, with NO window-level box, NO shared frame and NO "active layer" — two
// visible rasters draw two sets of axes, each fitted to and numbered in its own limits and units.
static void rebuildAxisLabels(Scene *s) {
	if (!s || !s->ren || !s->ren->GetActiveCamera()) return;
	// The BASE raster is its surface OR, on a bare-image window, its drape — the picture IS the
	// raster there. Same test shape as extraVisible, so base and extra are judged by one rule.
	vtkProp3D *sp = surfProp(s);
	const bool baseVis = (sp && sp->GetVisibility() != 0) ||
	                     (s->drape && s->drape->GetVisibility() != 0);
	rebuildAxesFor(s, s->baseAxes, s->baseAxes.shown && baseVis, true);
	for (auto &ex : s->extras)
		rebuildAxesFor(s, ex.ax, ex.ax.shown && extraVisible(ex), false);
	nanPlaneUpdate(s);       // the data holes' backdrop follows the same bounds the axes just did
}

// Renderer StartEvent -> keep the axis labels on the camera-near edges as the view rotates.
static void followZoomAnnotations(Scene *s);   // 50_scene.cpp: keep screen-constant contour labels + their line holes sized to the view

static void AxisLabelCB(vtkObject*, unsigned long, void *cd, void*) {
	Scene *s = static_cast<Scene*>(cd);
	rebuildAxisLabels(s);
	followZoomAnnotations(s);                  // cheap: gated on a real change in world-per-pixel
}

// Apply vertical exaggeration. The actor carries the base scale (xfac aspect +
// zfac unit conversion); the gizmo factor `ve` multiplies the Z. Cube-axis labels
// stay TRUE because their ranges are pinned to the data ranges, not the bounds.
static void applyVE(Scene *s) {
	surfSetScale(s, s->xfac, 1.0, s->zfac * s->ve);
	if (s->drape) s->drape->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // overlay tracks the base
	for (auto &ov : s->overlays)                                       // line/point overlays track the base too
		if (ov.actor) ov.actor->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	for (auto &cu : s->curtains)                                       // curtains hang in the same scaled space
		if (cu.actor) cu.actor->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	for (auto &ex : s->extras) {                                       // dropped grids/images track the base scale + VE
		if (ex.actor) ex.actor->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // (flat image z=zpos is baked in geometry -> scale carries VE)
		if (ex.drape) ex.drape->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	}
	if (s->profLine) s->profLine->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // profile drape tracks the base
	if (s->rbHL)     s->rbHL->SetScale(s->xfac, 1.0, s->zfac * s->ve);      // selection highlight tracks the cloud
	for (auto &pg : s->polys) {                                            // user polygons hang in the scaled space
		if (pg.line)        pg.line->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		if (pg.fill)        pg.fill->SetScale(s->xfac, 1.0, s->zfac * s->ve);          // filled face rides VE with its outline
		if (pg.faultPlane)  pg.faultPlane->SetScale(s->xfac, 1.0, s->zfac * s->ve);   // gray patch rides VE
		if (pg.faultPlane3D) pg.faultPlane3D->SetScale(s->xfac, 1.0, s->zfac * s->ve);// buried plane rides VE too
		if (pg.faultArrows) pg.faultArrows->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // slip arrows ride VE with the plane
	}
	for (auto &mb : s->mecaBalls) {                                        // drag-anchor line + dot ride VE too
		if (mb.anchor)    mb.anchor->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		if (mb.anchorDot) mb.anchorDot->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	}
	// Text labels sit on the XY plane (pos[2] = 0) unless they annotate something at a real height —
	// a contour label rides at its own contour's z, so it must follow VE like every other z-bearing
	// actor above (or it drifts away from the line it belongs to as soon as VE or the view changes).
	for (auto &tl : s->texts)
		if (tl.actor) tl.actor->SetPosition(tl.pos[0] * s->xfac - tl.offX, tl.pos[1] - tl.offY,
		                                    tl.pos[2] * s->zfac * s->ve);
	for (auto &sl : s->symbols)                                            // symbol depth (z) rides VE too
		if (sl.actor) sl.actor->SetScale(1.0, 1.0, s->zfac * s->ve);      // x already baked into the points
	if (s->polyPreview) s->polyPreview->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // in-progress draw preview
	if (s->polyHandles) s->polyHandles->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // edit-mode vertex handles
	// EVERY raster's axes ride VE, each from its OWN frame — there is no window box to resize. The
	// per-set work (box + degenerate-Z guard + gridline/Z-axis toggles + the billboards) is exactly
	// what rebuildAxisLabels already does for all of them, so VE only has to re-point the cameras and
	// let that ONE path run (SACRED_LAW.md: same operation, same function — never a second axes-fitting
	// implementation living in applyVE).
	vtkCamera *cam = s->ren->GetActiveCamera();
	if (s->baseAxes.cube) s->baseAxes.cube->SetCamera(cam);
	for (auto &ex : s->extras) if (ex.ax.cube) ex.ax.cube->SetCamera(cam);
	rebuildAxisLabels(s);                        // every set re-boxed + re-labelled from its own limits
	s->widget->renderWindow()->Render();
}

// SACRED_LAW.md derived-variable axes law, the UNDO half. A derived variable (a crop, an RTP, any
// "compute X from Y" result) re-frames the window onto ITS OWN limits through `gmtvtk_reframe_h`,
// which PINS `viewBoundsOverride` so every bounds consumer (axes cube, tick billboards, camera fit,
// gizmo) follows it. Removing that object must RELEASE the pin: otherwise the axes cube and its tick
// text stay boxed on a bbox nothing in the scene occupies any more — and since the object's own
// handle is gone with it, no Scene Objects row can ever clear them again (the user's own repro:
// "remove the cropped image and it leaves behind its axes that can't be deleted"). Clearing the flag
// makes `surfGetBounds` fall back to the real remaining content; `applyVE` then re-boxes the cube and
// rebuilds the labels from it — the SAME path every other frame change goes through, no second
// axes-fitting implementation. Call from EVERY object-removal site.
// Re-frame axes + camera onto whatever the scene actually holds now (defined in 90_c_api.cpp, next to
// the gmtvtk_reframe_* exports it is built from — this fragment is #included long before them).
static void sceneReframeToContent(Scene *s);

static void sceneClearViewOverride(Scene *s) {
	if (!s || !s->viewBoundsOverride) return;
	s->viewBoundsOverride = false;
	// With the pin released, surfGetBounds reports the REAL remaining content again. Re-frame onto it
	// through the SAME entry point the crop used to frame itself — axes box, X/Y ranges, the s->x0..y1
	// bookkeeping (which the crop SHRANK to its own limits) and the camera all come back together,
	// instead of an axes-only fix that would leave the camera parked on the deleted object.
	sceneReframeToContent(s);
	applyVE(s);
}

// Everything that must happen after ANY raster object is removed from the scene. ONE function, called
// from every removal site (the image row's Remove, the grid row's Remove/Move, gmtvtk_remove_grid_h),
// so a deletion can never leave a different kind of wreckage depending on which row it went through.
//
// (1) RESTORE THE SOURCE. A derived variable is added CHECKED with its source UNCHECKED
//     (SACRED_LAW.md derived-variable display law). Deleting the derived result therefore tends to
//     leave a window in which NOTHING is checked: the source is still hidden and the thing that
//     replaced it is gone. Same rule as "Remove illumination restores the original" — removing a
//     derived product must un-hide what it displaced, not just delete itself. The blank scaffold
//     plane of an image-canvas window (imageOnly) is NOT content and is never restored — it is an
//     opaque sheet that would cover the map.
// (2) RELEASE THE FRAME. See sceneClearViewOverride: the axes cube + tick billboards would otherwise
//     stay boxed on the removed object's bbox, with its handle gone and no row left to clear them.
static void sceneAfterObjectRemoved(Scene *s) {
	if (!s) return;
	bool anyVisible = false;
	for (auto &ex : s->extras)
		if (ex.actor && ex.actor->GetVisibility()) { anyVisible = true; break; }
	if (!anyVisible && !s->imageOnly) {
		if (vtkProp3D *sp = surfProp(s)) anyVisible = sp->GetVisibility() != 0;
	}
	if (!anyVisible) {
		// Topmost survivor first (extras are the pile, last = most recently added), else the base surface.
		bool restored = false;
		for (size_t i = s->extras.size(); i-- > 0; ) {
			if (!s->extras[i].actor) continue;
			s->extras[i].actor->SetVisibility(1);
			if (s->extras[i].drape) s->extras[i].drape->SetVisibility(1);
			restored = true;
			break;
		}
		if (!restored && !s->imageOnly) surfSetVisibility(s, 1);
	}
	sceneClearViewOverride(s);
}

// Build + exec the per-element context menu for an overlay (defined after addOverlay,
// near the Qt window code). Forward-declared so the gizmo's left-click handler can call it.
static void popupOverlayMenu(Scene *s, vtkActor *a, int mode, const QPoint &globalPos);
static void symbolLayerMenu(Scene *s, vtkActor *act, const QPoint &gp);   // symbol-layer menu (50_scene.cpp)

// Squared distance from point (px,py) to segment [a,b] (all display coords).
static double segDist2(double px, double py, const double a[2], const double b[2]) {
	const double vx = b[0]-a[0], vy = b[1]-a[1];
	const double L2 = vx*vx + vy*vy;
	double t = (L2 > 1e-12) ? ((px-a[0])*vx + (py-a[1])*vy) / L2 : 0.0;
	t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
	const double cx = a[0]+t*vx, cy = a[1]+t*vy;
	const double ex = px-cx, ey = py-cy;
	return ex*ex + ey*ey;
}

// World -> display projection as a plain 4x4 multiply. vtkRenderer::WorldToDisplay computes the
// SAME thing, but rebuilds the camera's composite transform and round-trips through the renderer
// for EVERY point — fine for a handful of vertices, ruinous for the million-vertex overlays a
// dropped LiDAR/point-cloud file produces (pickOverlayAt runs on every mouse-move). Built once
// per pick, then it is pure arithmetic per point.
struct DisplayProjector {
	vtkNew<vtkMatrix4x4> M;
	double ox = 0.0, oy = 0.0, sx = 0.0, sy = 0.0;
	bool ok = false;
	explicit DisplayProjector(vtkRenderer *ren) {
		if (!ren || !ren->GetActiveCamera()) return;
		M->DeepCopy(ren->GetActiveCamera()->GetCompositeProjectionTransformMatrix(
		                ren->GetTiledAspectRatio(), 0.0, 1.0));
		const int *sz = ren->GetSize(), *org = ren->GetOrigin();
		if (!sz || sz[0] <= 0 || sz[1] <= 0) return;
		ox = org ? org[0] : 0.0; oy = org ? org[1] : 0.0;
		sx = 0.5 * sz[0];        sy = 0.5 * sz[1];
		ok = true;
	}
	// (x,y,z) in SCALED world coords -> device px (bottom-up), matching WorldToDisplay's convention.
	inline void project(double x, double y, double z, double &dx, double &dy) const {
		const double *m = M->GetData();
		const double wx = m[0]*x + m[1]*y + m[2]*z  + m[3];
		const double wy = m[4]*x + m[5]*y + m[6]*z  + m[7];
		const double ww = m[12]*x + m[13]*y + m[14]*z + m[15];
		const double iw = (ww != 0.0) ? 1.0 / ww : 0.0;
		dx = ox + sx * (wx * iw + 1.0);
		dy = oy + sy * (wy * iw + 1.0);
	}
};

// Vertices a single overlay may contribute to ONE hit-test. A point cloud (a dropped .laz is
// millions of points) is scanned with a stride so the cost per mouse-move stays bounded; at the
// 12 px tolerance below, a dense cloud still answers the pick with a neighbouring point, which is
// the same actor and the same Scene Objects row. Without this cap the hover readout (onMouseMove
// -> pickOverlayInfoAt) projected every vertex of every overlay on EVERY motion event and the
// window froze solid the moment the user tried to rotate.
static const vtkIdType kPickVertexBudget = 40000;

// Vertex count of the window's PRIMARY surface actor (0 when it has no polydata mapper input, e.g.
// the tiled-LOD grid path, whose readout never goes through a picker anyway). Used to recognise a
// big point cloud before running an O(N)-per-pick VTK picker on it.
static vtkIdType surfPointCount(const Scene *s) {
	if (!s || !s->surf) return 0;
	vtkPolyDataMapper *m = vtkPolyDataMapper::SafeDownCast(s->surf->GetMapper());
	if (!m) return 0;
	vtkPolyData *pd = m->GetInput();
	return (pd && pd->GetPoints()) ? pd->GetNumberOfPoints() : 0;
}

// Nearest point of the primary surface's cloud under the cursor (device px), in SCALED world
// coords -- the coordinate-readout hit-test for a cloud too big for vtkCellPicker/vtkPointPicker.
// Strided screen-space scan, bounded by kPickVertexBudget, same technique as pickOverlayAt.
static bool pickCloudPointAt(Scene *s, int dx, int dy, double w[3]) {
	if (!s || !s->surf) return false;
	vtkPolyDataMapper *m = vtkPolyDataMapper::SafeDownCast(s->surf->GetMapper());
	vtkPolyData *pd = m ? m->GetInput() : nullptr;
	if (!pd || !pd->GetPoints()) return false;
	vtkPoints *pts = pd->GetPoints();
	const vtkIdType np = pts->GetNumberOfPoints();
	if (np <= 0) return false;
	const DisplayProjector proj(s->ren);
	if (!proj.ok) return false;
	double sc[3]; s->surf->GetScale(sc);
	const double tol = 12.0;
	double best = tol * tol;
	bool found = false;
	const vtkIdType step = (np > kPickVertexBudget) ? (np / kPickVertexBudget + 1) : 1;
	for (vtkIdType i = 0; i < np; i += step) {
		double p[3]; pts->GetPoint(i, p);
		const double X = p[0]*sc[0], Y = p[1]*sc[1], Z = p[2]*sc[2];
		double ppx, ppy; proj.project(X, Y, Z, ppx, ppy);
		const double ex = ppx-dx, ey = ppy-dy;
		const double dd = ex*ex + ey*ey;
		if (dd < best) { best = dd; w[0] = X; w[1] = Y; w[2] = Z; found = true; }
	}
	return found;
}

// Pick the overlay nearest the cursor at VTK display coords (dx,dy, bottom-up device px).
// vtkPropPicker/vtkCellPicker miss thin 1-2px lines, so this projects every overlay vertex
// to the screen (applying the actor's scale) and measures the cursor's pixel distance to the
// line segments (lines) or to the points (points). Returns the closest overlay within `tol`
// px, or nullptr. Deterministic, no render-pass pick. `outSeg` (optional): for a line-mode hit,
// the index into that overlay's own segoff/nseg (which stored polyline this point index falls
// in) — used by the double-click "promote to editable Polygon" path (85_polygon.cpp) to isolate
// just the clicked segment. -1 if not applicable (points mode, or no hit).
static vtkActor *pickOverlayAt(Scene *s, int dx, int dy, int &outMode, int *outSeg = nullptr, int *outVertex = nullptr) {
	if (outSeg) *outSeg = -1;
	if (outVertex) *outVertex = -1;
	if (!s || s->overlays.empty())
		return nullptr;
	vtkRenderer *ren = s->ren;
	const double tol = 12.0;             // pick radius in device px
	double best = tol * tol;             // squared
	double trueBest = 1e30;              // uncapped nearest (for diagnostics)
	vtkActor *bestA = nullptr;
	int bestMode = 1;
	Overlay *bestOv = nullptr;
	vtkIdType bestI0 = -1;

	const DisplayProjector proj(ren);
	if (!proj.ok) return nullptr;

	for (auto &ov : s->overlays) {
		if (!ov.actor || !ov.actor->GetVisibility())
			continue;
		vtkPolyDataMapper *m = vtkPolyDataMapper::SafeDownCast(ov.actor->GetMapper());
		if (!m) continue;
		vtkPolyData *pd = m->GetInput();
		if (!pd || !pd->GetPoints()) continue;
		double sc[3]; ov.actor->GetScale(sc);
		vtkPoints *pts = pd->GetPoints();
		const vtkIdType np = pts->GetNumberOfPoints();
		if (np <= 0) continue;

		// Cheap whole-overlay reject FIRST: the screen box of the actor's 8 bounds corners contains
		// every projected vertex, so a cursor outside it (+tol) cannot hit this overlay and its
		// vertices are never touched. One 8-point projection instead of np.
		{
			double b[6]; ov.actor->GetBounds(b);
			double bx0 = 1e300, bx1 = -1e300, by0 = 1e300, by1 = -1e300;
			for (int c = 0; c < 8; ++c) {
				double cx, cy;
				proj.project(b[(c & 1) ? 1 : 0], b[(c & 2) ? 3 : 2], b[(c & 4) ? 5 : 4], cx, cy);
				bx0 = std::min(bx0, cx); bx1 = std::max(bx1, cx);
				by0 = std::min(by0, cy); by1 = std::max(by1, cy);
			}
			if (dx < bx0 - tol || dx > bx1 + tol || dy < by0 - tol || dy > by1 + tol)
				continue;
		}

		// A vertex count over the budget (a point cloud, or any overlay big enough to stall the
		// per-mouse-move hover readout) is scanned strided, as nearest VERTEX, in either mode: the
		// answer is the same actor and — via bestI0 -> segoff below — the same segment, at bounded
		// cost. Only a within-budget line overlay pays the exact nearest-SEGMENT test.
		const bool strided = np > kPickVertexBudget;
		vtkCellArray *lines = (ov.mode == 1) ? pd->GetLines() : nullptr;
		if (ov.mode == 1 && !lines) continue;

		if (strided || ov.mode != 1) {   // points, or an oversized overlay: nearest vertex
			const vtkIdType step = strided ? (np / kPickVertexBudget + 1) : 1;
			for (vtkIdType i = 0; i < np; i += step) {
				double p[3]; pts->GetPoint(i, p);
				double ppx, ppy; proj.project(p[0]*sc[0], p[1]*sc[1], p[2]*sc[2], ppx, ppy);
				const double ex = ppx-dx, ey = ppy-dy;
				const double dd = ex*ex + ey*ey;
				if (dd < trueBest) trueBest = dd;
				if (dd < best) {
					best = dd; bestA = ov.actor; bestMode = ov.mode == 1 ? 1 : 0; bestOv = &ov;
					bestI0 = i;   // vertex index in EITHER mode -- points-mode overlays have no
					              // segoff/outSeg use for it, but vertexInfo hover (pickOverlayInfoAt)
					              // needs it regardless of mode; previously forced -1 for points,
					              // which silently killed vertex hover for every points-mode overlay.
				}
			}
			continue;
		}

		// project all points to display once (apply the actor's scale; no rot/trans on overlays)
		std::vector<double> px(np), py(np);
		for (vtkIdType i = 0; i < np; ++i) {
			double p[3]; pts->GetPoint(i, p);
			proj.project(p[0]*sc[0], p[1]*sc[1], p[2]*sc[2], px[i], py[i]);
		}

		{                                // lines: nearest segment
			vtkNew<vtkIdList> idl;
			lines->InitTraversal();
			while (lines->GetNextCell(idl)) {
				const vtkIdType n = idl->GetNumberOfIds();
				for (vtkIdType k = 0; k + 1 < n; ++k) {
					const vtkIdType i0 = idl->GetId(k), i1 = idl->GetId(k+1);
					const double a[2] = { px[i0], py[i0] };
					const double b[2] = { px[i1], py[i1] };
					const double dd = segDist2((double)dx, (double)dy, a, b);
					if (dd < trueBest) trueBest = dd;
					if (dd < best) { best = dd; bestA = ov.actor; bestMode = 1; bestOv = &ov; bestI0 = i0; }
				}
			}
		}
	}
	(void)trueBest;
	if (bestA) outMode = bestMode;
	if (outSeg && bestOv && bestI0 >= 0) {
		for (int k = 0; k < bestOv->nseg; ++k)
			if (bestI0 >= bestOv->segoff[k] && bestI0 < bestOv->segoff[k+1]) { *outSeg = k; break; }
	}
	if (outVertex && bestI0 >= 0) *outVertex = (int)bestI0;
	return bestA;
}

// Nearest OVERLAY SEGMENT/VERTEX under the cursor that carries hover info (e.g. a plate-boundary
// velocity/plate-pair block, or a dropped table's per-row text). Reuses pickOverlayAt -- the SAME
// hit-test the context-menu / "promote clicked segment" paths already use, never a second parallel
// picker for the same quantity -- to find the nearest overlay + its vertex (either mode) and segment
// (line mode only) index. Per-VERTEX info (finer-grained: one entry per point, e.g. a dropped
// "x y text" table, LINE or POINTS mode alike) is checked FIRST; per-SEGMENT info (e.g. plate-
// boundary blocks, line mode only) is the fallback. Used by onMouseMove to pop a tooltip when
// hovering e.g. a plate boundary segment or a dropped table's own point/vertex.
static bool pickOverlayInfoAt(Scene *s, int dx, int dy, std::string &out) {
	int mode = 1, seg = -1, vtx = -1;
	vtkActor *a = pickOverlayAt(s, dx, dy, mode, &seg, &vtx);
	if (!a)
		return false;
	for (auto &ov : s->overlays) {
		if (ov.actor.Get() != a)
			continue;
		if (vtx >= 0 && vtx < (int)ov.vertexInfo.size() && !ov.vertexInfo[vtx].empty()) {
			out = ov.vertexInfo[vtx];
			return true;
		}
		if (mode != 1 || seg < 0 || seg >= (int)ov.info.size())
			return false;
		out = ov.info[seg];
		return !out.empty();
	}
	return false;
}

static double sampleZ(const Scene *s, double x, double y);   // defined below (base relief height sampler)

// True (a solid3D glyph carries genuine depth, e.g. a buried earthquake) when `trueZ` sits BELOW
// the base relief's own height at (trueX,trueY) — i.e. the terrain that visually occludes it (see
// applyStacking) also occludes it for picking. GetZbufferDataAtPoint can't be used here: it always
// reads back 1.0 (far plane) through this app's QVTKOpenGLNativeWidget FBO (see the hover-readout
// ray-march comment below) — sampleZ against the resident heightfield is the same workaround this
// file already relies on for the coordinate readout. No base grid (NaN) -> never treat as buried.
// `imageOnly` windows (bare basemap picture, e.g. Seismicity's empty-launcher flow) carry only a
// HIDDEN FLAT z=0 placeholder plane, not real elevation — comparing against it made every event
// with any real depth (trueZ < 0) read as "buried", killing 3-D hover for every non-zero-depth
// event (only dep==0 events ever showed a tooltip). That placeholder is not terrain, skip it.
static bool solid3DBuried(const Scene *s, double trueX, double trueY, double trueZ) {
	if (s->imageOnly) return false;
	const double h = sampleZ(s, trueX, trueY);
	return !std::isnan(h) && trueZ < h;
}

// Nearest SYMBOL layer under the cursor (device px). Symbols sit ON TOP of overlays, so the click
// dispatcher tests this first. Projects each glyph's anchor point (x already xfac-baked; the actor
// carries the z scale) to display and takes the nearest within a size-aware tolerance, so big
// symbols are easy to hit. Returns the layer's actor (-> symbolLayerMenu) or nullptr.
static vtkActor *pickSymbolAt(Scene *s, int dx, int dy) {
	if (!s || s->symbols.empty())
		return nullptr;
	vtkRenderer *ren = s->ren;
	vtkActor *bestA = nullptr;
	double best = 1e30;
	for (auto &sl : s->symbols) {
		if (!sl.actor || !sl.actor->GetVisibility())
			continue;
		vtkPolyData *pd = symInputPD(sl);
		if (!pd || !pd->GetPoints())
			continue;
		double sc[3]; sl.actor->GetScale(sc);
		const double tol2 = std::max(12.0, sl.sizePx * 0.6) * std::max(12.0, sl.sizePx * 0.6);
		vtkPoints *pts = pd->GetPoints();
		const vtkIdType np = pts->GetNumberOfPoints();
		const double xfacInv = (s->xfac != 0.0) ? 1.0 / s->xfac : 1.0;
		for (vtkIdType i = 0; i < np; ++i) {
			double p[3]; pts->GetPoint(i, p);
			if (sl.solid3D && !s->flat2d && solid3DBuried(s, p[0]*xfacInv, p[1], p[2]))
				continue;                                    // hidden behind real terrain -> not pickable
			ren->SetWorldPoint(p[0]*sc[0], p[1]*sc[1], p[2]*sc[2], 1.0);
			ren->WorldToDisplay();
			double d[3]; ren->GetDisplayPoint(d);
			const double ex = d[0]-dx, ey = d[1]-dy, dd = ex*ex + ey*ey;
			if (dd <= tol2 && dd < best) { best = dd; bestA = sl.actor; }
		}
	}
	return bestA;
}

// Nearest SYMBOL POINT under the cursor that carries hover info (device px). Mirrors pickSymbolAt
// but tracks the individual point index so we can fetch its per-point text, and only considers
// layers that actually have info. On a hit, writes that point's multi-line text to `out` and
// returns true. Used by onMouseMove to pop a tooltip when hovering e.g. a volcano symbol.
static bool pickSymbolInfoAt(Scene *s, int dx, int dy, std::string &out) {
	if (!s || s->symbols.empty())
		return false;
	vtkRenderer *ren = s->ren;
	double best = 1e30; const std::string *bestInfo = nullptr;
	for (auto &sl : s->symbols) {
		if (sl.info.empty() || !sl.actor || !sl.actor->GetVisibility())
			continue;
		vtkPolyData *pd = symInputPD(sl);
		if (!pd || !pd->GetPoints())
			continue;
		double sc[3]; sl.actor->GetScale(sc);
		const double tol = std::max(12.0, sl.sizePx * 0.6);
		const double tol2 = tol * tol;
		vtkPoints *pts = pd->GetPoints();
		const vtkIdType np = pts->GetNumberOfPoints();
		const double xfacInv = (s->xfac != 0.0) ? 1.0 / s->xfac : 1.0;
		for (vtkIdType i = 0; i < np; ++i) {
			if ((size_t)i >= sl.info.size()) break;        // info must align 1:1 with points
			double p[3]; pts->GetPoint(i, p);
			if (sl.solid3D && !s->flat2d && solid3DBuried(s, p[0]*xfacInv, p[1], p[2]))
				continue;                                    // hidden behind real terrain -> no tooltip
			ren->SetWorldPoint(p[0]*sc[0], p[1]*sc[1], p[2]*sc[2], 1.0);
			ren->WorldToDisplay();
			double d[3]; ren->GetDisplayPoint(d);
			const double ex = d[0]-dx, ey = d[1]-dy, dd = ex*ex + ey*ey;
			if (dd <= tol2 && dd < best) { best = dd; bestInfo = &sl.info[i]; }
		}
	}
	if (bestInfo) { out = *bestInfo; return true; }
	return false;
}

// Nearest POINT within a BATCH symbol layer (sl.oneShot == false) under the cursor — the pick half
// of the per-point drag gesture (polygonHandlePress/Move, 85_polygon.cpp). oneShot symbols (the
// standalone Draggable Symbol tool) are skipped here: they already have their own double-click-arm-
// then-drag flow (symArmed/symLayerDrag) with exactly one point, no picking needed. Same projection
// + tolerance convention as pickSymbolAt/pickSymbolInfoAt. Writes the layer + point index on a hit.
static bool pickSymbolPointAt(Scene *s, int dx, int dy, int &outLayer, int &outPoint) {
	if (!s || s->symbols.empty())
		return false;
	vtkRenderer *ren = s->ren;
	double best = 1e30; int bestLayer = -1, bestPoint = -1;
	for (size_t li = 0; li < s->symbols.size(); ++li) {
		SymbolLayer &sl = s->symbols[li];
		if (sl.oneShot || !sl.actor || !sl.actor->GetVisibility())
			continue;
		vtkPolyData *pd = symInputPD(sl);
		if (!pd || !pd->GetPoints())
			continue;
		double sc[3]; sl.actor->GetScale(sc);
		const double tol = std::max(12.0, sl.sizePx * 0.6);
		const double tol2 = tol * tol;
		vtkPoints *pts = pd->GetPoints();
		const vtkIdType np = pts->GetNumberOfPoints();
		const double xfacInv = (s->xfac != 0.0) ? 1.0 / s->xfac : 1.0;
		for (vtkIdType i = 0; i < np; ++i) {
			double p[3]; pts->GetPoint(i, p);
			if (sl.solid3D && !s->flat2d && solid3DBuried(s, p[0]*xfacInv, p[1], p[2]))
				continue;
			ren->SetWorldPoint(p[0]*sc[0], p[1]*sc[1], p[2]*sc[2], 1.0);
			ren->WorldToDisplay();
			double d[3]; ren->GetDisplayPoint(d);
			const double ex = d[0]-dx, ey = d[1]-dy, dd = ex*ex + ey*ey;
			if (dd <= tol2 && dd < best) { best = dd; bestLayer = (int)li; bestPoint = (int)i; }
		}
	}
	if (bestLayer < 0) return false;
	outLayer = bestLayer; outPoint = bestPoint;
	return true;
}

// Is the cursor (VTK display px dx,dy) on the profile line? Same screen-space segment
// distance test as pickOverlayAt (thin lines miss hardware pickers), on s->profPD.
static bool profileHitAt(Scene *s, int dx, int dy) {
	if (!s || !s->profLine || !s->profLine->GetVisibility() || !s->profPD || !s->profPD->GetPoints())
		return false;
	vtkRenderer *ren = s->ren;
	vtkPoints *pts = s->profPD->GetPoints();
	const vtkIdType np = pts->GetNumberOfPoints();
	if (np < 2) return false;
	double sc[3]; s->profLine->GetScale(sc);
	std::vector<double> px(np), py(np);
	for (vtkIdType i = 0; i < np; ++i) {
		double p[3]; pts->GetPoint(i, p);
		ren->SetWorldPoint(p[0]*sc[0], p[1]*sc[1], p[2]*sc[2], 1.0);
		ren->WorldToDisplay();
		double d[3]; ren->GetDisplayPoint(d);
		px[i] = d[0]; py[i] = d[1];
	}
	const double tol2 = 12.0 * 12.0;
	for (vtkIdType i = 0; i + 1 < np; ++i) {
		const double a[2] = { px[i], py[i] }, b[2] = { px[i+1], py[i+1] };
		if (segDist2((double)dx, (double)dy, a, b) <= tol2)
			return true;
	}
	return false;
}

// Bilinear sample of the full-res data layer at TRUE coords (x,y). Returns NaN outside the grid
// or when any of the four corners is NaN (so callers skip it). O(1), no locator, render-LOD
// independent — the basis for full-res hover + profile under the tiled-LOD render path.
static double sampleGrid(const float *Z, int nx, int ny, double gx0, double gx1, double gy0, double gy1,
                         double x, double y) {
	if (!Z || nx < 2 || ny < 2)
		return std::numeric_limits<double>::quiet_NaN();
	const double dx = (gx1 - gx0) / (nx - 1);
	const double dy = (gy1 - gy0) / (ny - 1);
	if (dx == 0.0 || dy == 0.0)
		return std::numeric_limits<double>::quiet_NaN();
	const double fx = (x - gx0) / dx, fy = (y - gy0) / dy;
	if (fx < 0.0 || fy < 0.0 || fx > nx - 1 || fy > ny - 1)
		return std::numeric_limits<double>::quiet_NaN();
	const int i0 = std::min((int)fx, nx - 2), j0 = std::min((int)fy, ny - 2);
	const double tx = fx - i0, ty = fy - j0;
	const double z00 = Z[(size_t)i0     * ny + j0    ];   // column-major: Z[i*ny + j]
	const double z10 = Z[(size_t)(i0+1) * ny + j0    ];
	const double z01 = Z[(size_t)i0     * ny + j0 + 1];
	const double z11 = Z[(size_t)(i0+1) * ny + j0 + 1];
	if (std::isnan(z00) || std::isnan(z10) || std::isnan(z01) || std::isnan(z11))
		return std::numeric_limits<double>::quiet_NaN();
	return (1-tx)*(1-ty)*z00 + tx*(1-ty)*z10 + (1-tx)*ty*z01 + tx*ty*z11;
}

// Base relief data layer (profiles / cross-sections sample THIS — unchanged by multi-grid routing).
static double sampleZ(const Scene *s, double x, double y) {
	if (s->gridZ.empty()) return std::numeric_limits<double>::quiet_NaN();
	return sampleGrid(s->gridZ.data(), s->gnx, s->gny, s->gx0, s->gx1, s->gy0, s->gy1, x, y);
}

// ACTIVE grid data layer — what the hover/coordinate readout reports. Falls back to the base relief
// when no dropped grid is active (actZ null), so a single-grid window behaves exactly as before.
static double sampleActiveZ(const Scene *s, double x, double y) {
	if (s->actZ && !s->actZ->empty())
		return sampleGrid(s->actZ->data(), s->actNx, s->actNy, s->actX0, s->actX1, s->actY0, s->actY1, x, y);
	return sampleZ(s, x, y);
}

// Ray vs one triangle (Möller–Trumbore), all in SCALED world space. dir need not be unit; t is the
// same near->far parameter as the unproject ray. Returns true + t on a front/back hit (two-sided).
static bool rayTri(const double o[3], const double d[3],
                   const double a[3], const double b[3], const double c[3], double &t) {
	const double e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
	const double e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
	const double pq[3] = { d[1]*e2[2]-d[2]*e2[1], d[2]*e2[0]-d[0]*e2[2], d[0]*e2[1]-d[1]*e2[0] };
	const double det = e1[0]*pq[0] + e1[1]*pq[1] + e1[2]*pq[2];
	if (std::fabs(det) < 1e-30) return false;
	const double inv = 1.0/det;
	const double tv[3] = { o[0]-a[0], o[1]-a[1], o[2]-a[2] };
	const double u = (tv[0]*pq[0] + tv[1]*pq[1] + tv[2]*pq[2]) * inv;
	if (u < 0.0 || u > 1.0) return false;
	const double qv[3] = { tv[1]*e1[2]-tv[2]*e1[1], tv[2]*e1[0]-tv[0]*e1[2], tv[0]*e1[1]-tv[1]*e1[0] };
	const double v = (d[0]*qv[0] + d[1]*qv[1] + d[2]*qv[2]) * inv;
	if (v < 0.0 || u + v > 1.0) return false;
	t = (e2[0]*qv[0] + e2[1]*qv[1] + e2[2]*qv[2]) * inv;
	return t >= 0.0;
}

// Hover pick against the buried 3-D fault plane(s). The plane's 4 corners live in the polydata in
// RAW (true) coords; scale them (xfac,1,zfac*ve) into the same world the unproject ray lives in,
// ray-cast the two triangles, keep the nearest. On a hit returns the SCALED hit point in wOut and
// its ray parameter in tOut so the caller can compare depth against the surface hit.
static bool pickFaultPlaneAt(Scene *s, const double o[3], const double d[3], double wOut[3], double &tOut) {
	if (s->flat2d) return false;
	const double zsc = s->zfac * s->ve, gx = (s->xfac != 0.0) ? s->xfac : 1.0;
	bool got = false; double best = 1e300;
	for (auto &pg : s->polys) {
		if (!pg.isFault || !pg.faultPlane3D || !pg.faultPlane3D->GetVisibility()) continue;
		vtkPoints *P = pg.faultPlane3DPD ? pg.faultPlane3DPD->GetPoints() : nullptr;
		if (!P || P->GetNumberOfPoints() < 4) continue;
		double c[4][3];
		for (int i = 0; i < 4; ++i) {
			double r[3]; P->GetPoint(i, r);
			c[i][0] = r[0]*gx; c[i][1] = r[1]; c[i][2] = r[2]*zsc;
		}
		double t;
		if ((rayTri(o, d, c[0], c[1], c[2], t) || rayTri(o, d, c[0], c[2], c[3], t)) && t < best) {
			best = t; got = true;
		}
	}
	if (!got) return false;
	wOut[0] = o[0] + best*d[0]; wOut[1] = o[1] + best*d[1]; wOut[2] = o[2] + best*d[2];
	tOut = best;
	return true;
}

static int mecaHitAt(Scene *s, int x, int y);   // beachball under cursor (defined in 85_polygon.cpp)

// Mouse move (default priority): live coordinate readout. Runs only when the gizmo
// did not grab the drag (the gizmo's high-priority observer aborts the event then).
static void onMouseMove(vtkObject*, unsigned long, void *clientData, void* /*cd*/) {
	Scene *s = static_cast<Scene*>(clientData);
	// Skip the readout while rubber-band selection is armed (Ctrl+right-drag owns the cursor).
	if (s->rbEnabled)
		return;
	int *p = s->widget->interactor()->GetEventPosition();   // device px, bottom-up
	const int mx = p[0], my = p[1];
	// Per-symbol hover info: if the cursor is over a symbol that carries metadata (e.g. a volcano),
	// pop its multi-line text as a tooltip. Anti-flicker: only call showText when the hovered TEXT
	// CHANGES (Qt keeps a same-text tip fixed, but re-issuing it every move + hideText on near-miss
	// frames made it strobe), and only hideText on a real hit->miss transition. Offset the tip off
	// the cursor (+18,+18) so it never sits under the pointer (self-occlusion also caused flicker).
	{
		std::string sinfo;
		bool haveInfo = pickSymbolInfoAt(s, mx, my, sinfo);
		if (!haveInfo) {                 // focal-mechanism beachball metadata (gmtvtk_set_meca_infos_h)
			const int bi = mecaHitAt(s, mx, my);
			if (bi >= 0 && !s->mecaBalls[bi].info.empty()) { sinfo = s->mecaBalls[bi].info; haveInfo = true; }
		}
		if (!haveInfo)                   // per-segment overlay metadata (e.g. plate boundaries)
			haveInfo = pickOverlayInfoAt(s, mx, my, sinfo);
		if (haveInfo) {
			if (sinfo != s->hoverInfo) {
				QToolTip::showText(QCursor::pos() + QPoint(18, 18),
				                   QString::fromStdString(sinfo), s->widget);
				s->hoverInfo = sinfo;
			}
		} else if (!s->hoverInfo.empty()) {
			QToolTip::hideText();
			s->hoverInfo.clear();
		}
	}
	// GPU z-buffer pick: read the depth under the cursor and unproject it. O(1) regardless of grid
	// size — no software cell traversal — so the readout never stalls, needs no cell locator, and
	// can't OOM on a 200 MB grid (the old vtkCellPicker path did all three). One-pixel glReadPixels.
	// Cursor -> world WITHOUT the GPU z-buffer: GetZbufferDataAtPoint returns 1.0 (far plane)
	// through the QVTKOpenGLNativeWidget FBO, so the old depth-read never resolved a hit. Build
	// this pixel's world ray by unprojecting the near (depth 0) and far (depth 1) planes — camera
	// inverse only, no buffer read — then intersect it with the scene in SCALED world space:
	//   - grid (gridZ): march the ray against the full-res heightfield (sampleZ), LOD-independent;
	//   - bare image (imageOnly): hit the flat z=0 drape plane;
	//   - FV mesh / point cloud: software ray-cast with the resident vtkCellPicker (bounded geom).
	// Produces a scaled world point w[3] + hit flag; the readout below consumes it unchanged.
	double w[3] = { 0.0, 0.0, 0.0 };
	bool hit = false;
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
	// March against the ACTIVE (topmost-visible) grid so the readout tracks the grid actually shown.
	const bool haveActive = (s->actZ && !s->actZ->empty()) || !s->gridZ.empty();
	if (haveActive) {
		// g(t) = Pz(t) - sampleActiveZ(truex,truey)*zsc; first sign change along the ray = nearest
		// surface crossing, then bisect. NaN (off-grid) segments are skipped.
		auto eval = [&](double t, double &fval) -> bool {
			const double X = nr[0] + t*dirx, Y = nr[1] + t*diry, Z = nr[2] + t*dirz;
			const double h = sampleActiveZ(s, X / gx, Y);
			if (std::isnan(h)) return false;
			fval = Z - h * zsc; return true;
		};
		const int NS = 512;
		double pt = 0.0, pf = 0.0; bool have = false;
		for (int k = 0; k <= NS && !hit; ++k) {
			const double t = (double)k / NS; double fv;
			if (!eval(t, fv)) { have = false; continue; }
			if (have && ((pf <= 0.0 && fv >= 0.0) || (pf >= 0.0 && fv <= 0.0))) {
				double a = pt, b = t, fa = pf;
				for (int it = 0; it < 40; ++it) {
					const double m = 0.5*(a+b); double fm;
					if (!eval(m, fm)) break;
					if ((fa <= 0.0 && fm <= 0.0) || (fa >= 0.0 && fm >= 0.0)) { a = m; fa = fm; } else b = m;
				}
				const double t0 = 0.5*(a+b);
				w[0] = nr[0] + t0*dirx; w[1] = nr[1] + t0*diry; w[2] = nr[2] + t0*dirz; hit = true;
			}
			pt = t; pf = fv; have = true;
		}
	} else if (s->imageOnly) {
		if (dirz != 0.0) {
			const double t0 = -nr[2] / dirz;
			if (t0 >= 0.0 && t0 <= 1.0) { w[0] = nr[0] + t0*dirx; w[1] = nr[1] + t0*diry; w[2] = 0.0; hit = true; }
		}
	} else if (s->picker && surfPointCount(s) > kPickVertexBudget) {
		// A BIG cloud (a .laz is millions of points): both vtkCellPicker and vtkPointPicker walk
		// every point on every mouse-move -- that is what made rotating a dropped point cloud hang.
		// Same answer (nearest point under the cursor) at bounded cost: the SAME strided screen-space
		// scan pickOverlayAt uses, so there is one hit-test technique for big clouds, not two.
		hit = pickCloudPointAt(s, mx, my, w);
	} else if (s->picker) {
		if (s->picker->Pick((double)mx, (double)my, 0.0, s->ren) && s->picker->GetCellId() >= 0) {
			s->picker->GetPickPosition(w); hit = true;
		}
		// The cell picker never hits a Verts-only point cloud (zero-area cells) -- fall back to
		// nearest-point picking so a plain point cloud (view_points/"Point cloud view") gets the
		// SAME LL-corner coordinate readout every other scene kind already has.
		else if (s->pointPicker && s->pointPicker->Pick((double)mx, (double)my, 0.0, s->ren) &&
		         s->pointPicker->GetPointId() >= 0) {
			s->pointPicker->GetPickPosition(w); hit = true;
		}
	}
	// Over a grid but the ray hit a NaN hole (no surface there) — the march above found no crossing.
	// Coordinates must NEVER go blank: intersect the ray with the base map plane (z=0) to recover x,y
	// (z is then sampled as NaN below and printed literally). Accept only inside the grid footprint so
	// pointing at empty sky still reads "ready".
	if (!hit && haveActive && dirz != 0.0) {
		const double t0 = -nr[2] / dirz;
		if (t0 >= 0.0) {
			const double X = nr[0] + t0 * dirx, Y = nr[1] + t0 * diry;
			const bool useAct = (s->actZ && !s->actZ->empty());
			const double bx0 = useAct ? s->actX0 : s->gx0, bx1 = useAct ? s->actX1 : s->gx1;
			const double by0 = useAct ? s->actY0 : s->gy0, by1 = useAct ? s->actY1 : s->gy1;
			const double tx = X / gx;
			if (tx >= std::min(bx0, bx1) && tx <= std::max(bx0, bx1) &&
				Y  >= std::min(by0, by1) && Y  <= std::max(by0, by1)) {
				w[0] = X; w[1] = Y; w[2] = 0.0; hit = true;
			}
		}
	}
	// Buried 3-D fault plane: cast the same ray at its quad. If it is nearer to the camera than the
	// surface hit (or the surface missed), report the PLANE's own x,y,z so the user can verify the
	// plane geometry directly (otherwise the plane never shows coordinates at all).
	bool onPlane = false;
	{
		const double o[3]  = { nr[0], nr[1], nr[2] };
		const double dd[3] = { dirx, diry, dirz };
		double tHit = 1e300;
		if (hit) {
			const double dlen2 = dirx*dirx + diry*diry + dirz*dirz;
			if (dlen2 > 0.0)
				tHit = ((w[0]-nr[0])*dirx + (w[1]-nr[1])*diry + (w[2]-nr[2])*dirz) / dlen2;
		}
		double wp[3], tp;
		if (pickFaultPlaneAt(s, o, dd, wp, tp) && (!hit || tp <= tHit)) {
			w[0] = wp[0]; w[1] = wp[1]; w[2] = wp[2]; hit = true; onPlane = true;
		}
	}
	if (onPlane) {
		// Plane hit: z is the plane's OWN depth (undo the actor's z scale), not the surface elevation.
		const double zsc = s->zfac * s->ve;
		s->win->statusBar()->showMessage(
			QString("fault plane:  x = %1    y = %2    z = %3   (VE ×%4)")
				.arg(w[0] / s->xfac, 0, 'f', 3).arg(w[1], 0, 'f', 3)
				.arg((zsc != 0.0) ? w[2] / zsc : 0.0, 0, 'f', 3).arg(s->ve, 0, 'f', 2));
	} else if (hit) {
		if (s->imageOnly && !s->gridAdopted) {
			// Bare image: no elevation -> show the pixel COLOUR under the cursor instead of z.
			// Read it straight from the framebuffer (the drape is unlit, so the pixel is the
			// image's true albedo). p is bottom-up display pixels, matching GetPixelData.
			vtkNew<vtkUnsignedCharArray> px;
			s->widget->renderWindow()->GetPixelData(p[0], p[1], p[0], p[1], 1, px.GetPointer());
			int r = 0, g = 0, b = 0;
			if (px->GetNumberOfTuples() >= 1 && px->GetNumberOfComponents() >= 3) {
				r = px->GetValue(0); g = px->GetValue(1); b = px->GetValue(2);
			}
			s->win->statusBar()->showMessage(
				QString("x = %1    y = %2    rgb = %3 %4 %5")
					.arg(w[0] / s->xfac, 0, 'f', 3).arg(w[1], 0, 'f', 3)
					.arg(r).arg(g).arg(b));
		} else {
			const double truex = w[0] / s->xfac, truey = w[1];
			// z from the full-res DATA layer (render-LOD independent). Fall back to the unprojected
			// depth z (undo base/VE actor scale zfac*ve) for surfaces with no data layer (FV mesh /
			// point cloud), and where the sample misses (off-grid / NaN). flat-2D: zsc=0 -> z 0.
			double ztrue;
			bool zknown = true;
			if (haveActive) {
				ztrue = sampleActiveZ(s, truex, truey);
				zknown = !std::isnan(ztrue);   // NaN hole in the grid -> report z = NaN, never blank
			}
			else {
				const double zsc = s->zfac * s->ve;
				ztrue = (zsc != 0.0) ? w[2] / zsc : 0.0;
			}
			const QString zstr = zknown ? QString::number(ztrue, 'f', 3) : QStringLiteral("NaN");
			s->win->statusBar()->showMessage(                  // true coords
				QString("x = %1    y = %2    z = %3   (VE ×%4)")
					.arg(truex, 0, 'f', 3).arg(truey, 0, 'f', 3)
					.arg(zstr).arg(s->ve, 0, 'f', 2));
		}
	} else {
		s->win->statusBar()->showMessage("ready");
	}
}
