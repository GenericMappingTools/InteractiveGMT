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
	void mousePressEvent(QMouseEvent *e) override {
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
	int    cubeLayers = 0;                   // >1 iff this grid is a 3-D-cube variable (its menu offers
	                                         // "Cube layers…", opening the slider bound to THIS cube)
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

// A text label: either a user-placed toolbar annotation, or a batch-owned label (Focal
// mechanisms' per-event date). The two render DIFFERENTLY, keyed by `groupName`:
//  - ungrouped (Text tool): lies FLAT on the z=0 (XY) plane — a plain vtkTextActor3D rendered into
//    its local XY plane, a map "sticker" that tilts/rotates with the terrain.
//  - grouped (groupName set): a vtkBillboardTextActor3D — always faces the camera at a constant
//    screen size, like the cube's tick-number labels. A flat decal here read as if the date had
//    been painted into the terrain/basemap texture from most view angles (2026-07-04 bug).
// Both are stored in TRUE coords (x,y); the actor sits in the surface's scaled space (x*xfac).
// Ungrouped labels are left-click-draggable on the plane; font (family/size/colour/bold/italic) is
// editable from the Scene Objects menu (ungrouped) or the owning batch's properties dialog (grouped).
struct TextLabel {
	std::array<double,3> pos;                // anchor on the XY plane, TRUE coords (z always 0)
	// vtkBillboardTextActor3D is NOT a vtkTextActor3D subclass (both derive independently from
	// vtkProp3D, each with their own SetInput/GetTextProperty) — the field holds the common base;
	// textApplyProps downcasts to whichever concrete type this label's `groupName` says it built.
	vtkSmartPointer<vtkProp3D> actor;
	std::string text;                        // the shown string (rendered in the scene)
	std::string name;                        // short Scene Objects label ("Text N")
	std::string font  = "Arial";             // VTK font family: "Arial" / "Courier" / "Times"
	int    size  = 18;
	double color[3] = { 1.0, 1.0, 0.2 };     // default: yellow (readable over relief)
	bool   bold = false, italic = false;
	std::string groupName;                   // non-empty = owned by a batch (e.g. focal-mechanism date
	                                          // labels); tags it for bulk find/erase (deleteMecaGroup) and
	                                          // folds its Scene Objects row under the batch's own row
	                                          // instead of listing one row per label (rebuildSceneObjects)
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
static void showLineProperties(Scene *s, const LineRef& lr);                 // the properties dialog
static void popupLineObjectMenu(Scene *s, const LineRef& lr, const QString& name, const QPoint& gp);
static void applyVectorStacking(Scene *s);                      // shared vector-pile draw-order (50_scene.cpp)
static void restackVector(Scene *s, int *stackPtr, int op);    // move one vector element through the pile
static void applyGridStacking(Scene *s);                        // grid-pile draw-order: base relief + grids (50_scene.cpp)
static void restackGrid(Scene *s, int *stackPtr, int op);      // move one grid through the grid pile
static void lineApplyStyle(Scene *s, const LineRef& lr, int style);
static int  lineCurrentStyle(Scene *s, const LineRef& lr);
static void polygonDelete(Scene *s, vtkActor *lineActor);                    // remove a finished polygon
static void overlayDelete(Scene *s, vtkActor *a);                            // remove an overlay line/point (50)
static void overlayDeleteGroup(Scene *s, const std::string& groupName);      // remove every overlay tagged with groupName (50)
static void polyRebuildLine(Scene *s, Polygon& pg);                         // rebuild a polygon actor from pg.v (85)
static void polyRebuildFill(Scene *s, Polygon& pg);                         // rebuild a closed polygon's filled face (85)
static int  polyIndexOfActor(Scene *s, vtkActor *a);                        // index of polygon whose line==a, or -1 (55)
static bool lineClosedRing(Scene *s, const LineRef& lr);                    // closed polygon ring? (55)
static int  polyHitPolygon(Scene *s, int x, int y, double tol);             // polygon under cursor? (85)
static void nestReflow(Scene *s, bool snap = true);                         // re-quantize "Nested grids" chain (85); snap=false = don't move verts, only recompute indices (restore)
static void nestNewChild(Scene *s);                                         // append a refined nested child (85)

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
	vtkSmartPointer<vtkCubeAxesActor>     axes;
	// 3-D cube: pin the vertical axis box + Z tick labels to the WHOLE cube's z-range so the axes do
	// not shift as the user switches layers (each layer's own min/max differs). Set once per cube via
	// gmtvtk_set_cube_axes_zrange; cubeZMin/Max are UNSCALED data values (scaled by zfac*ve on use).
	bool   cubeZLock = false;
	double cubeZMin = 0.0, cubeZMax = 0.0;
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
	// surfGetBounds, never s->axes directly) then automatically stays consistent with the reframe
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
	const std::vector<float>* actZ = nullptr;
	int    actNx = 0, actNy = 0;
	double actX0 = 0, actX1 = 1, actY0 = 0, actY1 = 1;
	// The colorbar's CURRENT display range — decoupled from zmin/zmax (which stay the base relief's range
	// for the cube axes / VE). buildColorbar sets these to the active grid's range; layoutColorbar reads
	// them so ticks place correctly when the bar is retargeted to a dropped grid.
	double barLo = 0, barHi = 1;
	bool   surfShowBar = true;              // base relief: user wants its colorbar shown (when active)

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

	// --- bottom tabbed panel (Profile / Julia Console / Data Viewer) --------
	QDockWidget *bottomDock    = nullptr;   // the single bottom dock holding the tab widget
	QTabWidget *bottomTabs    = nullptr;   // its QTabWidget; the corner "Hide" collapses the body
	QTableWidget *dataTable     = nullptr;   // Data Viewer spreadsheet (filled by gmtvtk_set_table)
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
	std::vector<MecaGroupProps> mecaGroups;            // one entry per focal-mechanism batch groupName
	std::vector<MecaBall> mecaBalls;                   // one entry per plotted event (drag + anchor line state)
	int    mecaDrag = -1;                               // index into mecaBalls being click-dragged (-1 = none)
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
static inline void surfGetBounds(Scene *s, double b[6]) {
	if (s->viewBoundsOverride) { for (int i = 0; i < 6; ++i) b[i] = s->viewBounds[i]; return; }
	if (vtkProp3D *p = surfProp(s)) p->GetBounds(b);
}
static inline void surfSetVisibility(Scene *s, int v) {
	if (vtkProp3D *p = surfProp(s)) p->SetVisibility(v);
}
// The renderable actors carrying material / mapper / edges. Tiles when tiled, else the surf.
static inline std::vector<vtkActor*> surfActors(Scene *s) {
	std::vector<vtkActor*> v;
	if (!s->tiles.empty()) { for (auto& a : s->tiles) v.push_back(a.Get()); }
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
	for (auto& ex : s->extras) {
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
	vtkSmartPointer<vtkActor> actor;  // null = not resident
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
static void popupProfileMenu(Scene *s, const QPoint& globalPos); // its right-click menu
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
			// NaN corners are no longer skipped — the quad is emitted and its NaN nodes render in the
			// NaN fill colour (z scalar NaN -> CTF NanColor), so the hole is painted instead of void.
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
		const int jd = ys[jj > 0 ? jj-1 : jj], ju = ys[jj < th-1 ? jj+1 : jj];  // sampled neighbours
		for (int ii = 0; ii < tw; ++ii) {
			const int i = xs[ii]; const double x = x0 + i * dx;
			const float zz = z[(vtkIdType)i * ny + j];
			// NaN node: geometry z pinned to the floor (valid mesh, flat hole), z SCALAR kept NaN so
			// the CTF paints it with the NaN fill colour.
			pts->InsertNextPoint(x, y, std::isnan(zz) ? fillZ : zz);
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
			// NaN corners no longer skip the quad — the hole is painted (NaN scalar -> CTF NanColor).
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
static vtkSmartPointer<vtkPolyData> makeFvMesh(const double *xyz, int nv,
											   const int *sides, int nfaces, const int *indices,
											   const unsigned char *facergb, const double *facez,
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

static void rebuildAxisLabels(Scene *s) {
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
	pinCubeAxisZ(s, b);                          // cube: hold the Z box to the whole cube's range
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
	placeTickBillboards(s, s->xlabels, s->x0, s->x1, b[0], b[1], 0, xEdgeY, b[4], ctr, tp, tl, tickLen);
	placeTickBillboards(s, s->ylabels, s->y0, s->y1, b[2], b[3], 1, yEdgeX, b[4], ctr, tp, tl, tickLen);
	if (s->flat2d) {
		// Complete the frame: plain (un-numbered) ticks on the FAR edge from each annotated one --
		// south got the real X ticks above, so north gets the mirror; west got the real Y ticks,
		// so east gets the mirror. SACRED_LAW.md: axes on all 4 sides, always.
		const double xFar = (xEdgeY == b[2]) ? b[3] : b[2];
		const double yFar = (yEdgeX == b[0]) ? b[1] : b[0];
		placeTickMarksOnly(s->x0, s->x1, b[0], b[1], 0, xFar, b[4], ctr, tp, tl, tickLen);
		placeTickMarksOnly(s->y0, s->y1, b[2], b[3], 1, yFar, b[4], ctr, tp, tl, tickLen);
	}
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
	// Flat map (no Z relief): the X/Y gridlines lie coplanar with the image, drawing a graticule
	// mesh over the map (and thin coplanar lines FXAA then re-thicknesses). Drop them when flat;
	// keep them in 3-D where they sit on the far box walls as a depth reference.
	if (zHide) { s->axes->DrawXGridlinesOff(); s->axes->DrawYGridlinesOff(); }
	else       { s->axes->DrawXGridlinesOn();  s->axes->DrawYGridlinesOn();  }
	if (zHide) {
		for (auto& l : s->zlabels) l->SetVisibility(0);
	} else {
		// Cube: label the Z axis with the WHOLE cube's range (matching the pinned box) so the numbers
		// are identical on every layer; otherwise this layer's own data range.
		const double zlo = s->cubeZLock ? s->cubeZMin : s->zmin;
		const double zhi = s->cubeZLock ? s->cubeZMax : s->zmax;
		placeTickBillboards(s, s->zlabels, zlo, zhi, b[4], b[5], 2, zx, zy, ctr, tp, tl, tickLen);
	}
	if (s->flat2d) {
		// The actual 4-side BORDER: a closed rectangle connecting the 4 corners, not just interval
		// ticks -- SACRED_LAW.md "all mapping displays must have axes on all 4 sides" means a real
		// frame, same as any GMT map border. Same axisTickPD line pipeline the ticks already use.
		vtkIdType c0 = tp->InsertNextPoint(b[0], b[2], b[4]);
		vtkIdType c1 = tp->InsertNextPoint(b[1], b[2], b[4]);
		vtkIdType c2 = tp->InsertNextPoint(b[1], b[3], b[4]);
		vtkIdType c3 = tp->InsertNextPoint(b[0], b[3], b[4]);
		tl->InsertNextCell(5);
		tl->InsertCellPoint(c0); tl->InsertCellPoint(c1); tl->InsertCellPoint(c2);
		tl->InsertCellPoint(c3); tl->InsertCellPoint(c0);
	}
	if (s->axisTickPD) {
		s->axisTickPD->SetPoints(tp);
		s->axisTickPD->SetLines(tl);
		s->axisTickPD->Modified();
	}
}

// Renderer StartEvent -> keep the axis labels on the camera-near edges as the view rotates.
static void AxisLabelCB(vtkObject*, unsigned long, void *cd, void*) {
	rebuildAxisLabels(static_cast<Scene*>(cd));
}

// Apply vertical exaggeration. The actor carries the base scale (xfac aspect +
// zfac unit conversion); the gizmo factor `ve` multiplies the Z. Cube-axis labels
// stay TRUE because their ranges are pinned to the data ranges, not the bounds.
static void applyVE(Scene *s) {
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
	for (auto& pg : s->polys) {                                            // user polygons hang in the scaled space
		if (pg.line)        pg.line->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		if (pg.fill)        pg.fill->SetScale(s->xfac, 1.0, s->zfac * s->ve);          // filled face rides VE with its outline
		if (pg.faultPlane)  pg.faultPlane->SetScale(s->xfac, 1.0, s->zfac * s->ve);   // gray patch rides VE
		if (pg.faultPlane3D) pg.faultPlane3D->SetScale(s->xfac, 1.0, s->zfac * s->ve);// buried plane rides VE too
		if (pg.faultArrows) pg.faultArrows->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // slip arrows ride VE with the plane
	}
	for (auto& mb : s->mecaBalls) {                                        // drag-anchor line + dot ride VE too
		if (mb.anchor)    mb.anchor->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		if (mb.anchorDot) mb.anchorDot->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	}
	for (auto& tl : s->texts)                                              // text labels lie flat on z=0 (XY plane)
		if (tl.actor) tl.actor->SetPosition(tl.pos[0] * s->xfac, tl.pos[1], 0.0);
	for (auto& sl : s->symbols)                                            // symbol depth (z) rides VE too
		if (sl.actor) sl.actor->SetScale(1.0, 1.0, s->zfac * s->ve);      // x already baked into the points
	if (s->polyPreview) s->polyPreview->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // in-progress draw preview
	if (s->polyHandles) s->polyHandles->SetScale(s->xfac, 1.0, s->zfac * s->ve);  // edit-mode vertex handles
	double b[6]; surfGetBounds(s, b);            // bounds already include the scale
	pinCubeAxisZ(s, b);                          // cube: hold the Z box to the whole cube's range
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
	// Flat map: X/Y gridlines go coplanar with the image (a mesh over the map) -> drop them; 3-D
	// keeps them on the far box walls (see rebuildAxisLabels).
	if (flatZ) { s->axes->DrawXGridlinesOff(); s->axes->DrawYGridlinesOff(); }
	else       { s->axes->DrawXGridlinesOn();  s->axes->DrawYGridlinesOn();  }
	s->axes->SetCamera(s->ren->GetActiveCamera());
	rebuildAxisLabels(s);                        // Z billboards follow the new drawn extent
	s->widget->renderWindow()->Render();
}

// Build + exec the per-element context menu for an overlay (defined after addOverlay,
// near the Qt window code). Forward-declared so the gizmo's left-click handler can call it.
static void popupOverlayMenu(Scene *s, vtkActor *a, int mode, const QPoint& globalPos);
static void symbolLayerMenu(Scene *s, vtkActor *act, const QPoint& gp);   // symbol-layer menu (50_scene.cpp)

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
// px, or nullptr. Deterministic, no render-pass pick. `outSeg` (optional): for a line-mode hit,
// the index into that overlay's own segoff/nseg (which stored polyline this point index falls
// in) — used by the double-click "promote to editable Polygon" path (85_polygon.cpp) to isolate
// just the clicked segment. -1 if not applicable (points mode, or no hit).
static vtkActor *pickOverlayAt(Scene *s, int dx, int dy, int& outMode, int *outSeg = nullptr) {
	if (outSeg) *outSeg = -1;
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

	for (auto& ov : s->overlays) {
		if (!ov.actor || !ov.actor->GetVisibility())
			continue;
		vtkPolyDataMapper *m = vtkPolyDataMapper::SafeDownCast(ov.actor->GetMapper());
		if (!m) continue;
		vtkPolyData *pd = m->GetInput();
		if (!pd || !pd->GetPoints()) continue;
		double sc[3]; ov.actor->GetScale(sc);
		vtkPoints *pts = pd->GetPoints();
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
			vtkCellArray *lines = pd->GetLines();
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
					if (dd < best) { best = dd; bestA = ov.actor; bestMode = 1; bestOv = &ov; bestI0 = i0; }
				}
			}
		}
		else {                           // points: nearest vertex
			for (vtkIdType i = 0; i < np; ++i) {
				const double ex = px[i]-dx, ey = py[i]-dy;
				const double dd = ex*ex + ey*ey;
				if (dd < trueBest) trueBest = dd;
				if (dd < best) { best = dd; bestA = ov.actor; bestMode = 0; bestOv = &ov; bestI0 = -1; }
			}
		}
	}
	(void)trueBest;
	if (bestA) outMode = bestMode;
	if (outSeg && bestOv && bestI0 >= 0) {
		for (int k = 0; k < bestOv->nseg; ++k)
			if (bestI0 >= bestOv->segoff[k] && bestI0 < bestOv->segoff[k+1]) { *outSeg = k; break; }
	}
	return bestA;
}

// Nearest OVERLAY SEGMENT under the cursor that carries hover info (e.g. a plate-boundary
// velocity/plate-pair block). Reuses pickOverlayAt -- the SAME hit-test the context-menu /
// "promote clicked segment" paths already use, never a second parallel picker for the same
// quantity -- to find the nearest line + its segment index, then looks up that Overlay's own
// info[] by segment. Only line-mode overlays carry per-segment info. Used by onMouseMove to pop
// a tooltip when hovering e.g. a plate boundary segment.
static bool pickOverlayInfoAt(Scene *s, int dx, int dy, std::string& out) {
	int mode = 1, seg = -1;
	vtkActor *a = pickOverlayAt(s, dx, dy, mode, &seg);
	if (!a || mode != 1 || seg < 0)
		return false;
	for (auto& ov : s->overlays) {
		if (ov.actor.Get() != a)
			continue;
		if (seg >= (int)ov.info.size())
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
	for (auto& sl : s->symbols) {
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
static bool pickSymbolInfoAt(Scene *s, int dx, int dy, std::string& out) {
	if (!s || s->symbols.empty())
		return false;
	vtkRenderer *ren = s->ren;
	double best = 1e30; const std::string *bestInfo = nullptr;
	for (auto& sl : s->symbols) {
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
                   const double a[3], const double b[3], const double c[3], double& t) {
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
static bool pickFaultPlaneAt(Scene *s, const double o[3], const double d[3], double wOut[3], double& tOut) {
	if (s->flat2d) return false;
	const double zsc = s->zfac * s->ve, gx = (s->xfac != 0.0) ? s->xfac : 1.0;
	bool got = false; double best = 1e300;
	for (auto& pg : s->polys) {
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
