struct Gizmo;     // Fledermaus-style scale/tilt/azimuth handle (defined below)
struct QuadNode;  // tiled-LOD pyramid node (defined below Scene)
struct Scene;     // the per-window scene (defined below; forward-declared for the line-tool decls)
struct FoldTitleBar;  // foldable dock title bar (defined in 70_window.cpp; Scene keeps a ptr to fold programmatically)

// A QLabel that opens something on a plain LEFT click — used for the Scene Objects row labels:
// left-clicking the element description pops its properties menu (onClick gets the GLOBAL point).
struct ClickableLabel : QLabel {
	std::function<void(const QPoint&)> onClick;        // LEFT click  -> properties / fold
	std::function<void(const QPoint&)> onRightClick;   // RIGHT click -> context menu (e.g. Save…)
	using QLabel::QLabel;
	void mousePressEvent(QMouseEvent* e) override {
		if (e->button() == Qt::LeftButton && onClick) onClick(e->globalPosition().toPoint());
		else if (e->button() == Qt::RightButton && onRightClick) onRightClick(e->globalPosition().toPoint());
		else QLabel::mousePressEvent(e);
	}
};

// A caller-supplied GMTdataset drawn over the surface as lines or points. Each carries
// its own vtkActor so the right-click context menu can retune its colour / line style /
// point size live. Mode: 0 = points, 1 = lines.
struct Overlay {
	vtkSmartPointer<vtkActor> actor;
	int mode = 1;
	vtkSmartPointer<vtkPolyData> baseLine;   // the line geometry (always the mapper input)
	vtkSmartPointer<vtkTexture>  stripeTex;  // 1-D stipple texture (kept alive) for dashed/dotted
	int lineStyle = 0;                       // 0 solid, 1 dashed, 2 dotted (so colour edits rebuild it)
	std::string name;                        // label shown in the Scene Objects panel
	int    stack = 0;                        // draw-order rank in the shared vector pile (higher = on top)
};

// A generic SCREEN-CONSTANT symbol layer (volcanoes, seismicity, cities, …): N glyphs of one
// shape (GMT symbol code) stamped at N points by a vtkGlyph3D. The glyph source is a UNIT shape
// in the XY plane (radius 0.5); a per-frame observer rescales `glyph->SetScaleFactor` so the
// on-screen size stays `sizePx` pixels at any zoom (same camera math the gizmo uses). Point
// positions are pre-baked with the surface's xfac (x*xfac) so the glyph itself is NOT distorted;
// the actor carries only the z scale (1,1,zfac*ve) so symbols ride VE like the other overlays.
struct SymbolLayer {
	vtkSmartPointer<vtkActor>   actor;
	vtkSmartPointer<vtkGlyph3D> glyph;       // source(unit shape) + input(points); ScaleFactor = world size
	double sizePx = 8.0;                      // requested on-screen size in PIXELS
	bool   filled = true;                     // filled polygon glyph (fill+edge) vs open line glyph (edge only)
	std::string sym  = "c";                   // GMT symbol code (for the Scene Objects label / properties)
	std::string name;                         // label shown in the Scene Objects panel
	std::vector<std::string> info;            // per-point hover text (multi-line); empty = no hover info
	int    stack = 0;                          // draw-order rank in the shared vector pile (higher = on top)
};

// A Fledermaus-style vertical "curtain": a textured wall hung along an XY track.
struct Curtain {
	vtkSmartPointer<vtkActor> actor;
	std::string name;                        // label shown in the Scene Objects panel
};

// An extra dataset dropped into an existing window (a second grid/image surface). Listed in
// the Scene Objects panel with its own show/hide checkbox. `drape` is its optional image actor.
struct ExtraObj {
	vtkSmartPointer<vtkActor> actor;
	vtkSmartPointer<vtkActor> drape;
	vtkSmartPointer<vtkTexture> tex;         // dropped-image texture (reused to rebuild flat plane / drape)
	std::string name;                        // label shown in the Scene Objects panel (file name)
	bool   isImage = false;                  // dropped IMAGE (flat plane / drapeable) vs grid surface
	bool   draped  = false;                  // image currently draped on the host grid (else a flat plane)
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
};

// A user-drawn polygon (closed polyline) from the toolbar polygon tool. Vertices are kept in
// TRUE coords; the line actor is hung in the surface's scaled space (xfac,1,zfac*ve), so it
// tracks VE like the other overlays. Built by 85_polygon.cpp.
struct Polygon {
	std::vector<std::array<double,3>> v;     // vertices, TRUE coords; closed ring (first == last) when closed
	vtkSmartPointer<vtkActor>    line;       // the polyline actor
	vtkSmartPointer<vtkPolyData> linePD;     // its geometry (rebuilt as vertices move)
	std::string name;                        // label shown in the Scene Objects panel ("polygon N")
	bool closed = true;                      // closed ring (polygon/rect/circle) vs open chain (polyline)
	bool isRect = false;                     // drawn with a rectangle tool (SH_Rect/SH_RectN): vertex edits stay axis-aligned
	bool isFault = false;                    // drawn with the Draw Fault tool (SH_Fault): props hold the elastic-deformation dialog
	vtkSmartPointer<vtkActor>    faultPlane; // gray surface-projection patch of the dipping fault plane (sits BELOW the trace)
	vtkSmartPointer<vtkPolyData> faultPlanePD;
	int    stack = 0;                        // draw-order rank in the shared vector pile (higher = on top)
	int    nestKind = 0;                     // 0 = ordinary shape; 1 = "Nested grids" rectangle (special menu)
	double nestXi = 0, nestYi = 0;           // child cell sizes (0 = inherit parent inc; resolved by nestReflow)
	int    nestReg = 0;                       // 0 grid / 1 pixel registration (carried into COMCOT/NSWING info)
	int    nestIx0 = 0, nestIx1 = 0;          // parent-grid node indices of the snapped W/E edges (1-based on display)
	int    nestIy0 = 0, nestIy1 = 0;          // parent-grid node indices of the snapped S/N edges
};

// A user-placed text label from the toolbar text tool. Lies FLAT on the z=0 (XY) plane: a
// vtkTextActor3D renders the text into its local XY plane, anchored at (x,y,0). Stored in TRUE
// coords (x,y); the actor sits in the surface's scaled space (x*xfac). Left-click-drag moves it on
// the plane; its font (family/size/colour/bold/italic) is editable from the Scene Objects menu.
struct TextLabel {
	std::array<double,3> pos;                // anchor on the XY plane, TRUE coords (z always 0)
	vtkSmartPointer<vtkTextActor3D> actor;
	std::string text;                        // the shown string (rendered in the scene)
	std::string name;                        // short Scene Objects label ("Text N")
	std::string font  = "Arial";             // VTK font family: "Arial" / "Courier" / "Times"
	int    size  = 18;
	double color[3] = { 1.0, 1.0, 0.2 };     // default: yellow (readable over relief)
	bool   bold = false, italic = false;
};

// A handle to one line-like scene object for the shared Line Properties tool (55_lineprops.cpp).
// `kind` selects how style (solid/dashed/dotted) is applied (each line type stipples differently);
// `actor` is the renderable. Reachable by right-click on the line OR on its Scene Objects row.
enum LineKind { LK_Profile, LK_Overlay, LK_Polygon };
struct LineRef {
	LineKind  kind;
	vtkActor* actor = nullptr;
};
static void showLineProperties(Scene* s, const LineRef& lr);                 // the properties dialog
static void popupLineObjectMenu(Scene* s, const LineRef& lr, const QString& name, const QPoint& gp);
static void applyVectorStacking(Scene* s);                      // shared vector-pile draw-order (50_scene.cpp)
static void restackVector(Scene* s, int* stackPtr, int op);    // move one vector element through the pile
static void applyGridStacking(Scene* s);                        // grid-pile draw-order: base relief + grids (50_scene.cpp)
static void restackGrid(Scene* s, int* stackPtr, int op);      // move one grid through the grid pile
static void lineApplyStyle(Scene* s, const LineRef& lr, int style);
static int  lineCurrentStyle(Scene* s, const LineRef& lr);
static void polygonDelete(Scene* s, vtkActor* lineActor);                    // remove a finished polygon
static void overlayDelete(Scene* s, vtkActor* a);                            // remove an overlay line/point (50)
static void polyRebuildLine(Scene* s, Polygon& pg);                         // rebuild a polygon actor from pg.v (85)
static int  polyHitPolygon(Scene* s, int x, int y, double tol);             // polygon under cursor? (85)
static void nestReflow(Scene* s);                                           // re-quantize "Nested grids" chain (85)
static void nestNewChild(Scene* s);                                         // append a refined nested child (85)

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
	vtkSmartPointer<vtkCubeAxesActor>     axes;
	vtkSmartPointer<vtkRenderer>          axesRen;  // overlay layer (1) for the Z tick billboards: own headlight (even, view-independent text brightness) + own depth (never occluded by the surface); shares the main camera
	vtkSmartPointer<vtkActor>             axisTicks; // our OWN single outward tickmarks (cube native ticks are doubled across two faces -> off)
	vtkSmartPointer<vtkPolyData>          axisTickPD;
	std::vector<vtkSmartPointer<vtkBillboardTextActor3D>> xlabels, ylabels, zlabels;  // tick labels drawn on the camera-near edges (never inside the cube)
	vtkSmartPointer<vtkBillboardTextActor3D> axTitle[3];   // axis NAME titles (lon/lat/Z or X/Y/Z) as overlay billboards (cube-native titles don't render here)
	std::string axName[3];                                 // the three axis names, picked by `geographic`
	vtkSmartPointer<vtkScalarBarActor>    bar;       // coloured strip only
	vtkSmartPointer<vtkActor2D>           barTicks;  // our own tick-mark lines (strip has none in VTK 9.6)
	std::vector<vtkSmartPointer<vtkTextActor>> barLabels;  // our own tick numbers
	std::vector<double>                   barValues; // value at each tick/label
	vtkSmartPointer<vtkPoints>            barTickPts;// tick endpoints (rewritten in place on drag)
	double barX0 = 0.93, barY0 = 0.55;               // colorbar frame bottom-left, normalized (draggable)
	bool   barDragging = false;
	double barGrabX = 0, barGrabY = 0;               // mouse-to-origin offset while dragging
	vtkSmartPointer<vtkCellPicker>        picker;
	QVTKOpenGLNativeWidget*               widget = nullptr;
	QMainWindow*                          win    = nullptr;
	double ve = 1.0;            // vertical exaggeration (gizmo factor, 1 = true scale)
	double zmin = 0, zmax = 0;  // true (unscaled) z range
	double x0 = 0, x1 = 1;      // true x range (for cube-axis labels / readout)
	double y0 = 0, y1 = 1;      // true y range
	double xfac = 1.0;          // base X actor scale (cos(midlat) for geographic)
	double zfac = 1.0;          // base Z actor scale (true-scale unit conversion)

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
	const std::vector<float>* actZ = nullptr;
	int    actNx = 0, actNy = 0;
	double actX0 = 0, actX1 = 1, actY0 = 0, actY1 = 1;
	// The colorbar's CURRENT display range — decoupled from zmin/zmax (which stay the base relief's range
	// for the cube axes / VE). buildColorbar sets these to the active grid's range; layoutColorbar reads
	// them so ticks place correctly when the bar is retargeted to a dropped grid.
	double barLo = 0, barHi = 1;
	bool   surfShowBar = true;              // base relief: user wants its colorbar shown (when active)

	// --- tiled-LOD pyramid (plain grid) -------------------------------------
	// Quadtree of tiles; coarse near root, refined per-frame by screen-space error so only the
	// visible region at the needed resolution is resident. surfGroup holds the live tile actors.
	QuadNode* quadRoot = nullptr;
	vtkSmartPointer<vtkScalarsToColors> surfLut;     // shared LUT for lazily-built tiles
	bool     surfCtfRange = false;
	int      surfEdges = 0;                           // current wire-edge state (applied to new tiles)
	uint64_t lodFrame = 0;                            // bumped each refine; tiles store lastUsed
	size_t   lodResidentBytes = 0;                    // approx resident tile geometry bytes
	size_t   lodBudgetBytes = (size_t)1 << 30;        // ~1 GiB cap; LRU-evict offscreen tiles past it
	vtkSmartPointer<vtkCallbackCommand> lodCmd;       // camera-modified observer (drives refine)

	// --- flat-2D (top-down ortho map) toggle --------------------------------
	// One-button switch to a true planimetric map: VE collapsed to 0 (relief flat,
	// z carried by colour only), orthographic top-down camera, rotation/tilt locked.
	// The 3D camera + VE are saved here and restored on toggle back.
	bool   flat2d = false;
	bool   imageOnly = false;   // loaded as a bare image (no elevation): readout shows pixel colour, not z
	bool   emptyStart = false;  // full-chrome launcher with no data yet (hidden placeholder); drop -> promote
	bool   gridAdopted = false; // a real grid was dropped onto an imageOnly canvas (Background region /
	                            // bare image) and adopted as the hover heightfield -> readout shows z,
	                            // NOT pixel colour, even though the canvas itself stays imageOnly
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
	QMenu*      geoMenu = nullptr;   // the Geography menu (built disabled; enabled once a CRS is set)
	QMenu*      elasticMenu = nullptr;   // Seismology > Elastic deformation (disabled until a CRS is set)
	bool hasCRS() const { return !crsProj4.empty() || !crsWkt.empty() || crsEpsg != 0; }

	QAction* act2D = nullptr;        // shared checkable "Flat 2D (map)" action (toolbar + View menu)
	QWidget* objPanel = nullptr;     // Scene Objects dock content (rebuilt when overlays change)
	QDockWidget* objDock = nullptr;  // the Scene Objects dock itself (re-shown when the first nested rect lands)
	FoldTitleBar* objFoldBar = nullptr;  // Scene Objects dock fold toggle (call ->onClick() to fold/unfold programmatically)
	FoldTitleBar* shadeFoldBar = nullptr; // Shading dock fold toggle (Surface row click folds/un-folds it via toggleShadingFold)
	QDockWidget*  shadeDock    = nullptr;  // the Shading dock itself (re-shown when an empty launcher is promoted to a grid)
	std::string surfName;            // Scene Objects label for s->surf ("" -> "Surface"; named solids set it)
	QPlainTextEdit* console = nullptr;   // Julia console dock output (commands eval'd in Main via g_juliaEval)
	QPlainTextEdit* errConsole = nullptr; // read-only Errors tab: execution errors from background callbacks (gmtvtk_log_error)

	// --- bottom tabbed panel (Profile / Julia Console / Data Viewer) --------
	QDockWidget*  bottomDock    = nullptr;   // the single bottom dock holding the tab widget
	QTabWidget*   bottomTabs    = nullptr;   // its QTabWidget; the corner "Hide" collapses the body
	QTableWidget* dataTable     = nullptr;   // Data Viewer spreadsheet (filled by gmtvtk_set_table)
	QToolButton*  bottomHideBtn = nullptr;   // tab-bar corner Hide/Show toggle
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
	class ProfilePanel*             prof     = nullptr; // 2D (s,z) panel (a tab in the bottom dock)
	int    profStyle = 0;                              // 0 solid, 1 dashed, 2 dotted
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
	enum ShapeKind { SH_Polygon, SH_Polyline, SH_Line, SH_Rect, SH_Circle, SH_Text, SH_RectN, SH_Fault };
	ShapeKind polyShape = SH_Polygon;                  // active tool while polyMode is on
	std::vector<Polygon> polys;                        // finished polygons / polylines / rects / circles
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
	int    textDrag     = -1;                          // index into texts being click-dragged (-1 = none)
	vtkSmartPointer<vtkActor>    polyHandles;          // square vertex handles for the edited polygon
	vtkSmartPointer<vtkPolyData> polyHandlePD;
	qint64 polyLastClickMs = -10000;                   // last left-press time (double-click detect)
	int    polyLastClickX = 0, polyLastClickY = 0;     // last left-press position (px)
	vtkSmartPointer<vtkCallbackCommand> polyCmd;       // mouse observers (priority above the gizmo)
	QAction* polyAct = nullptr;                         // active draw toggle action — set on the checked tool
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
	QWidget* elasticDlg = nullptr;                      // open (non-modal) Vertical elastic deformation dialog, if any
};

// --- surface accessors: one actor (cloud/FV/drape/image) or a tiled grid -----------------
// When the grid is tiled, the transform/bounds/visibility live on the vtkAssembly `surfGroup`
// and the renderable parts are `tiles`; otherwise everything is the single actor `surf`. These
// helpers hide that split so call sites stay uniform. (Group null + tiles empty -> surf.)
static inline vtkProp3D* surfProp(Scene* s) {
	if (s->surfGroup) return s->surfGroup.Get();
	return s->surf.Get();
}
static inline void surfSetScale(Scene* s, double x, double y, double z) {
	if (vtkProp3D* p = surfProp(s)) p->SetScale(x, y, z);
}
static inline void surfGetScale(Scene* s, double sc[3]) {
	if (vtkProp3D* p = surfProp(s)) p->GetScale(sc); else { sc[0]=sc[1]=sc[2]=1.0; }
}
static inline void surfGetBounds(Scene* s, double b[6]) {
	if (vtkProp3D* p = surfProp(s)) p->GetBounds(b);
}
static inline void surfSetVisibility(Scene* s, int v) {
	if (vtkProp3D* p = surfProp(s)) p->SetVisibility(v);
}
// The renderable actors carrying material / mapper / edges. Tiles when tiled, else the surf.
static inline std::vector<vtkActor*> surfActors(Scene* s) {
	std::vector<vtkActor*> v;
	if (!s->tiles.empty()) { for (auto& a : s->tiles) v.push_back(a.Get()); }
	else if (s->surf)        v.push_back(s->surf.Get());
	return v;
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
	QuadNode* child[4] = { nullptr, nullptr, nullptr, nullptr };
	vtkSmartPointer<vtkActor> actor;  // null = not resident
	uint64_t lastUsed = 0;
	size_t   bytes = 0;
};

// Collapse / restore the bottom tabbed panel's BODY, leaving the tab strip (+ the Hide
// button) visible. Collapsing hides the QTabWidget's page stack and clamps the widget to
// the tab-bar height, so the dock shrinks and the 3-D view extends; restore reverses it.
static void setBottomCollapsed(Scene* s, bool collapse) {
	if (!s || !s->bottomTabs)
		return;
	if (QStackedWidget* body = s->bottomTabs->findChild<QStackedWidget*>("qt_tabwidget_stackedwidget"))
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
static void profilerBegin(Scene* s, int dx, int dy);
static void profilerDrag(Scene* s, int dx, int dy);
static void profilerEnd(Scene* s);
static bool profileHitAt(Scene* s, int dx, int dy);              // cursor near the profile line?
static void popupProfileMenu(Scene* s, const QPoint& globalPos); // its right-click menu
static void profileClear(Scene* s);                              // wipe the line + 2D panel

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

// Build a surface from a caller-supplied grid (GMT.jl layout): z is column-major,
// ny rows x nx cols, element (iy,ix) at offset ix*ny+iy, mapping to (x[ix], y[iy])
// with y ascending. NaN cells are dropped: their points are still emitted (cheap,
// harmless orphans) but any quad touching a NaN corner is skipped.
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
			if (std::isnan(zval->GetValue(a)) || std::isnan(zval->GetValue(b)) ||
				std::isnan(zval->GetValue(c)) || std::isnan(zval->GetValue(d)))
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
		const int jd = ys[jj > 0 ? jj-1 : jj], ju = ys[jj < th-1 ? jj+1 : jj];  // sampled neighbours
		for (int ii = 0; ii < tw; ++ii) {
			const int i = xs[ii]; const double x = x0 + i * dx;
			const float zz = z[(vtkIdType)i * ny + j];
			pts->InsertNextPoint(x, y, zz);
			zval->InsertNextValue(zz);
			// normal from the DISPLAYED (sampled) neighbours -> matches this tile's surface, and
			// neighbour tiles sharing the edge sample the same nodes -> no lighting seam.
			const int il = xs[ii > 0 ? ii-1 : ii], ir = xs[ii < tw-1 ? ii+1 : ii];
			float nxv = 0.f, nyv = 0.f, nzv = 1.f;
			if (!std::isnan(zz)) {
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
			if (std::isnan(zval->GetValue(a)) || std::isnan(zval->GetValue(b)) ||
				std::isnan(zval->GetValue(c)) || std::isnan(zval->GetValue(d)))
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
static vtkSmartPointer<vtkPolyData> makePointCloud(const double* xyz, int npts,
												   double& zmin, double& zmax) {
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
static vtkSmartPointer<vtkPolyData> makeFvMesh(const double* xyz, int nv,
											   const int* sides, int nfaces, const int* indices,
											   const unsigned char* facergb, const double* facez,
											   double& zmin, double& zmax) {
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
				vtkTextProperty* tp = nt->GetTextProperty();
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
			vtkBillboardTextActor3D* t = pool[used];
			char buf[64]; std::snprintf(buf, sizeof(buf), "%g", v);
			t->SetInput(buf);
			t->SetPosition(p[0], p[1], p[2]);
			// Offset the label in SCREEN space along the tick direction, PAST the tick's end:
			// project the edge point p and the tick end q, then push by |q-p|_screen + a gap.
			double sp[2], sq[2];
			s->ren->SetWorldPoint(p[0], p[1], p[2], 1.0); s->ren->WorldToDisplay();
			{ double* dd = s->ren->GetDisplayPoint(); sp[0] = dd[0]; sp[1] = dd[1]; }
			s->ren->SetWorldPoint(q[0], q[1], q[2], 1.0); s->ren->WorldToDisplay();
			{ double* dd = s->ren->GetDisplayPoint(); sq[0] = dd[0]; sq[1] = dd[1]; }
			double ox = sq[0]-sp[0], oy = sq[1]-sp[1];
			double tl = std::sqrt(ox*ox + oy*oy);
			if (tl > 1e-6) { ox /= tl; oy /= tl; } else { ox = 0; oy = -1; }
			const int off = int(tl) + 16;          // sit just past the tick end (+16 px gap)
			t->SetDisplayOffset(int(ox * off), int(oy * off));
			// Anchor the text's INNER edge at the offset point and let it grow OUTWARD only, so a
			// long label never spills back inside the cube. The outward screen dir (ox,oy) chooses
			// the justification: e.g. outward up-right -> anchor bottom-left -> text extends up-right.
			vtkTextProperty* jp = t->GetTextProperty();
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
static void placeAxisTitle(Scene* s, vtkBillboardTextActor3D* t, int axis,
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
	{ double* dd = s->ren->GetDisplayPoint(); sp[0] = dd[0]; sp[1] = dd[1]; }
	s->ren->SetWorldPoint(q[0], q[1], q[2], 1.0); s->ren->WorldToDisplay();
	{ double* dd = s->ren->GetDisplayPoint(); sq[0] = dd[0]; sq[1] = dd[1]; }
	double ox = sq[0]-sp[0], oy = sq[1]-sp[1];
	double tl = std::sqrt(ox*ox + oy*oy);
	if (tl > 1e-6) { ox /= tl; oy /= tl; } else { ox = 0; oy = -1; }
	const int off = int(tl) + 96;          // sit WELL past the number labels (they use +16)
	t->SetDisplayOffset(int(ox * off), int(oy * off));
	// Anchor so the title grows OUTWARD only (never back over the numbers), same as the numbers.
	vtkTextProperty* jp = t->GetTextProperty();
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
static void rebuildAxisLabels(Scene* s) {
	if (!s->surf || !s->ren || !s->ren->GetActiveCamera())
		return;
	// Z labels belong to the Axes Cube: when it is hidden they must hide too. This callback
	// fires every render and would otherwise re-show them, so honour the cube's visibility here.
	if (s->axes && !s->axes->GetVisibility()) {
		for (auto& l : s->xlabels) l->SetVisibility(0);
		for (auto& l : s->ylabels) l->SetVisibility(0);
		for (auto& l : s->zlabels) l->SetVisibility(0);
		for (auto& t : s->axTitle) if (t) t->SetVisibility(0);
		if (s->axisTicks) s->axisTicks->SetVisibility(0);
		return;
	}
	if (s->axisTicks) s->axisTicks->SetVisibility(1);
	double b[6]; surfGetBounds(s, b);            // drawn (VE-scaled) bounds
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
	const double tickLen = 0.0125 * diag;
	// Pick the candidate value nearer the camera along one coordinate.
	auto nearer = [](double a, double c, double camc) { return std::abs(camc-a) <= std::abs(camc-c) ? a : c; };
	const double xEdgeY = nearer(b[2], b[3], cam[1]);   // X labels on nearer y (front/back) floor edge
	double       yEdgeX = nearer(b[0], b[1], cam[0]);   // Y labels on nearer x (left/right) floor edge
	// Top-down view ('2' snap, +Y up): pin the Y (north) annotations to the screen-left (xmin)
	// edge instead of the camera-near edge, so north labels always sit on the left of the map.
	double dop[3]; s->ren->GetActiveCamera()->GetDirectionOfProjection(dop);
	if (dop[2] < -0.999) yEdgeX = b[0];
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
	placeTickBillboards(s, s->xlabels, s->x0, s->x1, b[0], b[1], 0, xEdgeY, b[4], ctr, tp, tl, tickLen);
	placeTickBillboards(s, s->ylabels, s->y0, s->y1, b[2], b[3], 1, yEdgeX, b[4], ctr, tp, tl, tickLen);
	if (hideNames) {
		if (s->axTitle[0]) s->axTitle[0]->SetVisibility(0);
		if (s->axTitle[1]) s->axTitle[1]->SetVisibility(0);
	} else {
		// X/Y NAME labels at the midpoint of each floor edge, pushed well past the numbers. No Z name.
		placeAxisTitle(s, s->axTitle[0], 0, 0.5*(b[0]+b[1]), xEdgeY, b[4], ctr, tickLen);
		placeAxisTitle(s, s->axTitle[1], 1, 0.5*(b[2]+b[3]), yEdgeX, b[4], ctr, tickLen);
	}
	// Z axis: hide in flat-2D (top-down map -> Z points at the camera, meaningless) or when the
	// drawn Z extent is degenerate. Drives the cube Z LINE/gridlines + the Z number billboards so
	// the toggle is self-correcting every render (no stale state on 2D<->3D switch).
	const bool zHide = s->flat2d || (b[5] - b[4]) <= 0.0;
	s->axes->SetZAxisVisibility(zHide ? 0 : 1);
	if (zHide) s->axes->DrawZGridlinesOff(); else s->axes->DrawZGridlinesOn();
	if (zHide) {
		for (auto& l : s->zlabels) l->SetVisibility(0);
	} else {
		placeTickBillboards(s, s->zlabels, s->zmin, s->zmax, b[4], b[5], 2, zx, zy, ctr, tp, tl, tickLen);
	}
	if (s->axisTickPD) {
		s->axisTickPD->SetPoints(tp);
		s->axisTickPD->SetLines(tl);
		s->axisTickPD->Modified();
	}
}

// Renderer StartEvent -> keep the axis labels on the camera-near edges as the view rotates.
static void AxisLabelCB(vtkObject*, unsigned long, void* cd, void*) {
	rebuildAxisLabels(static_cast<Scene*>(cd));
}

// Apply vertical exaggeration. The actor carries the base scale (xfac aspect +
// zfac unit conversion); the gizmo factor `ve` multiplies the Z. Cube-axis labels
// stay TRUE because their ranges are pinned to the data ranges, not the bounds.
static void applyVE(Scene* s) {
	surfSetScale(s, s->xfac, 1.0, s->zfac * s->ve);
	if (s->drape) s->drape->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // overlay tracks the base
	for (auto& ov : s->overlays)                                       // line/point overlays track the base too
		if (ov.actor) ov.actor->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	for (auto& cu : s->curtains)                                       // curtains hang in the same scaled space
		if (cu.actor) cu.actor->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	for (auto& ex : s->extras) {                                       // dropped grids/images track the base scale + VE
		if (ex.actor) ex.actor->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // (flat image z=zpos is baked in geometry -> scale carries VE)
		if (ex.drape) ex.drape->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	}
	if (s->profLine) s->profLine->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // profile drape tracks the base
	if (s->rbHL)     s->rbHL->SetScale(s->xfac, 1.0, s->zfac * s->ve);      // selection highlight tracks the cloud
	for (auto& pg : s->polys)                                               // user polygons hang in the scaled space
		if (pg.line) pg.line->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	for (auto& tl : s->texts)                                              // text labels lie flat on z=0 (XY plane)
		if (tl.actor) tl.actor->SetPosition(tl.pos[0] * s->xfac, tl.pos[1], 0.0);
	if (s->polyPreview) s->polyPreview->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // in-progress draw preview
	if (s->polyHandles) s->polyHandles->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // edit-mode vertex handles
	double b[6]; surfGetBounds(s, b);            // bounds already include the scale
	// Flat map (VE collapsed to 0 -> zero Z extent): vtkCubeAxesActor computes EACH axis's
	// label/gridline count even for hidden axes, so a zero Z range gives range/step = 0/0 ->
	// NaN -> INT_MIN ("Number of labels -2147483647 is invalid"), aborting the render (blank
	// window). Hiding the Z axis is not enough. Feed the axes a tiny non-zero Z range so the
	// count stays finite; the Z axis line + gridlines are hidden anyway, so nothing shows.
	const bool flatZ = (b[5] - b[4]) <= 0.0;
	if (flatZ) b[5] = b[4] + 1.0;                // non-degenerate range for the (hidden) Z axis
	s->axes->SetBounds(b);
	s->axes->SetZAxisVisibility(flatZ ? 0 : 1);
	if (flatZ) s->axes->DrawZGridlinesOff(); else s->axes->DrawZGridlinesOn();
	s->axes->SetCamera(s->ren->GetActiveCamera());
	rebuildAxisLabels(s);                        // Z billboards follow the new drawn extent
	s->widget->renderWindow()->Render();
}

// Build + exec the per-element context menu for an overlay (defined after addOverlay,
// near the Qt window code). Forward-declared so the gizmo's left-click handler can call it.
static void popupOverlayMenu(Scene* s, vtkActor* a, int mode, const QPoint& globalPos);
static void symbolLayerMenu(Scene* s, vtkActor* act, const QPoint& gp);   // symbol-layer menu (50_scene.cpp)

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

// Pick the overlay nearest the cursor at VTK display coords (dx,dy, bottom-up device px).
// vtkPropPicker/vtkCellPicker miss thin 1-2px lines, so this projects every overlay vertex
// to the screen (applying the actor's scale) and measures the cursor's pixel distance to the
// line segments (lines) or to the points (points). Returns the closest overlay within `tol`
// px, or nullptr. Deterministic, no render-pass pick.
static vtkActor* pickOverlayAt(Scene* s, int dx, int dy, int& outMode) {
	if (!s || s->overlays.empty())
		return nullptr;
	vtkRenderer* ren = s->ren;
	const double tol = 12.0;             // pick radius in device px
	double best = tol * tol;             // squared
	double trueBest = 1e30;              // uncapped nearest (for diagnostics)
	vtkActor* bestA = nullptr;
	int bestMode = 1;

	for (auto& ov : s->overlays) {
		if (!ov.actor || !ov.actor->GetVisibility())
			continue;
		vtkPolyDataMapper* m = vtkPolyDataMapper::SafeDownCast(ov.actor->GetMapper());
		if (!m) continue;
		vtkPolyData* pd = m->GetInput();
		if (!pd || !pd->GetPoints()) continue;
		double sc[3]; ov.actor->GetScale(sc);
		vtkPoints* pts = pd->GetPoints();
		const vtkIdType np = pts->GetNumberOfPoints();

		// project all points to display once (apply the actor's scale; no rot/trans on overlays)
		std::vector<double> px(np), py(np);
		for (vtkIdType i = 0; i < np; ++i) {
			double p[3]; pts->GetPoint(i, p);
			ren->SetWorldPoint(p[0]*sc[0], p[1]*sc[1], p[2]*sc[2], 1.0);
			ren->WorldToDisplay();
			double d[3]; ren->GetDisplayPoint(d);
			px[i] = d[0]; py[i] = d[1];
		}

		if (ov.mode == 1) {              // lines: nearest segment
			vtkCellArray* lines = pd->GetLines();
			if (!lines) continue;
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
					if (dd < best) { best = dd; bestA = ov.actor; bestMode = 1; }
				}
			}
		}
		else {                           // points: nearest vertex
			for (vtkIdType i = 0; i < np; ++i) {
				const double ex = px[i]-dx, ey = py[i]-dy;
				const double dd = ex*ex + ey*ey;
				if (dd < trueBest) trueBest = dd;
				if (dd < best) { best = dd; bestA = ov.actor; bestMode = 0; }
			}
		}
	}
	(void)trueBest;
	if (bestA) outMode = bestMode;
	return bestA;
}

// Nearest SYMBOL layer under the cursor (device px). Symbols sit ON TOP of overlays, so the click
// dispatcher tests this first. Projects each glyph's anchor point (x already xfac-baked; the actor
// carries the z scale) to display and takes the nearest within a size-aware tolerance, so big
// symbols are easy to hit. Returns the layer's actor (-> symbolLayerMenu) or nullptr.
static vtkActor* pickSymbolAt(Scene* s, int dx, int dy) {
	if (!s || s->symbols.empty())
		return nullptr;
	vtkRenderer* ren = s->ren;
	vtkActor* bestA = nullptr;
	double best = 1e30;
	for (auto& sl : s->symbols) {
		if (!sl.actor || !sl.actor->GetVisibility() || !sl.glyph)
			continue;
		vtkPolyData* pd = vtkPolyData::SafeDownCast(sl.glyph->GetInput());
		if (!pd || !pd->GetPoints())
			continue;
		double sc[3]; sl.actor->GetScale(sc);
		const double tol2 = std::max(12.0, sl.sizePx * 0.6) * std::max(12.0, sl.sizePx * 0.6);
		vtkPoints* pts = pd->GetPoints();
		const vtkIdType np = pts->GetNumberOfPoints();
		for (vtkIdType i = 0; i < np; ++i) {
			double p[3]; pts->GetPoint(i, p);
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
static bool pickSymbolInfoAt(Scene* s, int dx, int dy, std::string& out) {
	if (!s || s->symbols.empty())
		return false;
	vtkRenderer* ren = s->ren;
	double best = 1e30; const std::string* bestInfo = nullptr;
	for (auto& sl : s->symbols) {
		if (sl.info.empty() || !sl.actor || !sl.actor->GetVisibility() || !sl.glyph)
			continue;
		vtkPolyData* pd = vtkPolyData::SafeDownCast(sl.glyph->GetInput());
		if (!pd || !pd->GetPoints())
			continue;
		double sc[3]; sl.actor->GetScale(sc);
		const double tol = std::max(12.0, sl.sizePx * 0.6);
		const double tol2 = tol * tol;
		vtkPoints* pts = pd->GetPoints();
		const vtkIdType np = pts->GetNumberOfPoints();
		for (vtkIdType i = 0; i < np; ++i) {
			if ((size_t)i >= sl.info.size()) break;        // info must align 1:1 with points
			double p[3]; pts->GetPoint(i, p);
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

// Is the cursor (VTK display px dx,dy) on the profile line? Same screen-space segment
// distance test as pickOverlayAt (thin lines miss hardware pickers), on s->profPD.
static bool profileHitAt(Scene* s, int dx, int dy) {
	if (!s || !s->profLine || !s->profLine->GetVisibility() || !s->profPD || !s->profPD->GetPoints())
		return false;
	vtkRenderer* ren = s->ren;
	vtkPoints* pts = s->profPD->GetPoints();
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
static double sampleGrid(const float* Z, int nx, int ny, double gx0, double gx1, double gy0, double gy1,
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
static double sampleZ(const Scene* s, double x, double y) {
	if (s->gridZ.empty()) return std::numeric_limits<double>::quiet_NaN();
	return sampleGrid(s->gridZ.data(), s->gnx, s->gny, s->gx0, s->gx1, s->gy0, s->gy1, x, y);
}

// ACTIVE grid data layer — what the hover/coordinate readout reports. Falls back to the base relief
// when no dropped grid is active (actZ null), so a single-grid window behaves exactly as before.
static double sampleActiveZ(const Scene* s, double x, double y) {
	if (s->actZ && !s->actZ->empty())
		return sampleGrid(s->actZ->data(), s->actNx, s->actNy, s->actX0, s->actX1, s->actY0, s->actY1, x, y);
	return sampleZ(s, x, y);
}

// Mouse move (default priority): live coordinate readout. Runs only when the gizmo
// did not grab the drag (the gizmo's high-priority observer aborts the event then).
static void onMouseMove(vtkObject*, unsigned long, void* clientData, void* /*cd*/) {
	Scene* s = static_cast<Scene*>(clientData);
	// Skip the readout while rubber-band selection is armed (Ctrl+right-drag owns the cursor).
	if (s->rbEnabled)
		return;
	int* p = s->widget->interactor()->GetEventPosition();   // device px, bottom-up
	const int mx = p[0], my = p[1];
	// Per-symbol hover info: if the cursor is over a symbol that carries metadata (e.g. a volcano),
	// pop its multi-line text as a tooltip. Anti-flicker: only call showText when the hovered TEXT
	// CHANGES (Qt keeps a same-text tip fixed, but re-issuing it every move + hideText on near-miss
	// frames made it strobe), and only hideText on a real hit->miss transition. Offset the tip off
	// the cursor (+18,+18) so it never sits under the pointer (self-occlusion also caused flicker).
	{
		std::string sinfo;
		if (pickSymbolInfoAt(s, mx, my, sinfo)) {
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
		auto eval = [&](double t, double& fval) -> bool {
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
	} else if (s->picker) {
		if (s->picker->Pick((double)mx, (double)my, 0.0, s->ren) && s->picker->GetCellId() >= 0) {
			s->picker->GetPickPosition(w); hit = true;
		}
	}
	if (hit) {
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
			if (haveActive && !std::isnan(ztrue = sampleActiveZ(s, truex, truey))) {
				// got it from the active grid's data layer
			} else {
				const double zsc = s->zfac * s->ve;
				ztrue = (zsc != 0.0) ? w[2] / zsc : 0.0;
			}
			s->win->statusBar()->showMessage(                  // true coords
				QString("x = %1    y = %2    z = %3   (VE ×%4)")
					.arg(truex, 0, 'f', 3).arg(truey, 0, 'f', 3)
					.arg(ztrue, 0, 'f', 3).arg(s->ve, 0, 'f', 2));
		}
	} else {
		s->win->statusBar()->showMessage("ready");
	}
}
