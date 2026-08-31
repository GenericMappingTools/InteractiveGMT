// 58_movieanno.cpp -- movie frame LABELS (GMT movie -L) and PROGRESS INDICATORS (-P) as native
// 2-D overlay elements.
//
// Both kinds are first-class scene elements, not render chrome: each gets a Scene Objects handle with
// its own checkbox, properties menu and Remove (SACRED_LAW.md's Scene Objects registration law), so a
// user can place one, see it live, and only then spend a render on it. `movie()` (src/movie.jl) does
// not create a private overlay of its own -- it pushes a new string / progress fraction into whatever
// annotations the window already carries, through gmtvtk_anno_set_h.
//
// The displayed TEXT is always computed host-side. Only Julia knows the frame table, the elapsed-time
// scale (-L+s) and the C-format (+t) the user asked for; re-deriving any of that here would be a
// second implementation of the same quantity. This file owns placement and drawing, nothing else.
//
// Everything is laid out in DISPLAY (pixel) coordinates, re-run before every render from a renderer
// StartEvent observer -- the same technique symbolRescaleCB uses. Normalized-viewport coordinates
// (what the colour bar uses) would turn every circular indicator into an ellipse the moment the
// window stopped being square.

namespace manno {
	// Fixed colours GMT names as the per-style defaults; X11/GMT values, not approximations.
	constexpr double LIGHTGREEN[3] = {144 / 255.0, 238 / 255.0, 144 / 255.0};
	constexpr double LIGHTRED[3]   = {255 / 255.0, 111 / 255.0, 111 / 255.0};
	constexpr double LIGHTBLUE[3]  = {173 / 255.0, 216 / 255.0, 230 / 255.0};
	constexpr double BLUE[3]       = {0.0, 0.0, 1.0};
	constexpr double DARKRED[3]    = {139 / 255.0, 0.0, 0.0};
	constexpr double RED[3]        = {1.0, 0.0, 0.0};
	constexpr double YELLOW[3]     = {1.0, 1.0, 0.0};
	constexpr double BLACK[3]      = {0.0, 0.0, 0.0};
	constexpr int    NSEG = 96;              // circle tessellation (a 5%-of-canvas disc is small)
}

static void rebuildSceneObjects(Scene *s);          // 50_scene.cpp

static void annoCopyRGB(double *dst, const double *src) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }

// GMT's per-indicator defaults, applied ONLY where the caller supplied nothing (hasFg/hasBg false,
// widths 0). Kept in one function so "what does style c look like" has a single answer, and so a
// style added later cannot quietly inherit another one's look.
//
//   a  static [lightgreen] and moving [lightred] FILL; no label.
//   b  static [lightblue] and moving [blue] PEN; centred label at 30% of the indicator size.
//   c  static [dashed darkred, 1% of size] and moving [red, 5% of size] PEN, circular arrow with a
//      head 20% of the indicator size; centred label.
//   d  static [black] and moving [yellow, 0.5% of length] PEN, rounded line with a cross-mark.
//   e  static [red] and moving [lightgreen] PEN.
//   f  static axis PEN [black] and a moving triangle FILL [red], triangle twice the axis width.
//
// GMT documents a width only for the pens it calls out above; the rest (b's pens, d/e's static, f's
// axis) are set here to a proportion of the indicator size that matches the documented ones' scale.
static void annoApplyStyleDefaults(MovieAnno &a, double W) {
	if (!a.progress) return;
	switch (a.style) {
	case 0:                                                     // a: two fills
		if (!a.hasBg) annoCopyRGB(a.bgrgb, manno::LIGHTGREEN);
		if (!a.hasFg) annoCopyRGB(a.fgrgb, manno::LIGHTRED);
		break;
	case 1:                                                     // b: two pens
		if (!a.hasBg) annoCopyRGB(a.bgrgb, manno::LIGHTBLUE);
		if (!a.hasFg) annoCopyRGB(a.fgrgb, manno::BLUE);
		if (a.bgwidth <= 0.0) a.bgwidth = 0.05 * W;
		if (a.fgwidth <= 0.0) a.fgwidth = 0.05 * W;
		break;
	case 2:                                                     // c: circular arrow
		if (!a.hasBg) annoCopyRGB(a.bgrgb, manno::DARKRED);
		if (!a.hasFg) annoCopyRGB(a.fgrgb, manno::RED);
		if (a.bgwidth <= 0.0) a.bgwidth = 0.01 * W;
		if (a.fgwidth <= 0.0) a.fgwidth = 0.05 * W;
		break;
	case 3:                                                     // d: rounded line + cross-mark
		if (!a.hasBg) annoCopyRGB(a.bgrgb, manno::BLACK);
		if (!a.hasFg) annoCopyRGB(a.fgrgb, manno::YELLOW);
		if (a.bgwidth <= 0.0) a.bgwidth = 0.02 * W;
		if (a.fgwidth <= 0.0) a.fgwidth = 0.005 * W;
		break;
	case 4:                                                     // e: plain axis
		if (!a.hasBg) annoCopyRGB(a.bgrgb, manno::RED);
		if (!a.hasFg) annoCopyRGB(a.fgrgb, manno::LIGHTGREEN);
		if (a.bgwidth <= 0.0) a.bgwidth = 0.02 * W;
		if (a.fgwidth <= 0.0) a.fgwidth = 0.02 * W;
		break;
	default:                                                    // f: axis + moving triangle
		if (!a.hasBg) annoCopyRGB(a.bgrgb, manno::BLACK);
		if (!a.hasFg) annoCopyRGB(a.fgrgb, manno::RED);
		if (a.bgwidth <= 0.0) a.bgwidth = 0.015 * W;
		break;
	}
}

static bool annoIsCircular(const MovieAnno &a) { return a.progress && a.style <= 2; }

// The reference point named by GMT's +j, in pixels, plus the INWARD direction that +o offsets along.
// just: 0..8 = TL TC TR ML MC MR BL BC BR.
static void annoRefPoint(int just, int W, int H, double *px, double *py, double *inx, double *iny) {
	const int col = just % 3;                                   // 0 left, 1 centre, 2 right
	const int row = just / 3;                                   // 0 top,  1 middle, 2 bottom
	*px = (col == 0) ? 0.0 : (col == 1) ? W * 0.5 : (double)W;
	*py = (row == 0) ? (double)H : (row == 1) ? H * 0.5 : 0.0;
	*inx = (col == 0) ? 1.0 : (col == 2) ? -1.0 : 0.0;
	*iny = (row == 0) ? -1.0 : (row == 2) ? 1.0 : 0.0;
}

// A 2-D actor over `pd`, drawn in DISPLAY coordinates. `filled` picks polygons over lines only in the
// sense of which property fields matter -- the polydata itself carries whichever cells were built.
static vtkSmartPointer<vtkActor2D> annoMakeActor2D(vtkPolyData *pd, const double *rgb, double lineWidth)
{
	vtkNew<vtkCoordinate> co;
	co->SetCoordinateSystemToDisplay();
	vtkNew<vtkPolyDataMapper2D> map;
	map->SetInputData(pd);
	map->SetTransformCoordinate(co);
	vtkSmartPointer<vtkActor2D> act = vtkSmartPointer<vtkActor2D>::New();
	act->SetMapper(map);
	act->GetProperty()->SetColor(rgb[0], rgb[1], rgb[2]);
	if (lineWidth > 0.0) act->GetProperty()->SetLineWidth(lineWidth);
	return act;
}

// Append an arc (or full circle) as a POLYLINE. Angles in degrees, measured the way a progress
// indicator reads: 0 = 12 o'clock, growing CLOCKWISE, which is what GMT's indicators do.
static void annoAddArc(vtkPoints *pts, vtkCellArray *lines, double cx, double cy, double r,
                       double a0, double a1, int nseg)
{
	if (nseg < 2) nseg = 2;
	lines->InsertNextCell(nseg + 1);
	for (int i = 0; i <= nseg; ++i) {
		const double t   = a0 + (a1 - a0) * (double)i / (double)nseg;
		const double rad = (90.0 - t) * vtkMath::Pi() / 180.0;   // clockwise from north
		lines->InsertCellPoint(pts->InsertNextPoint(cx + r * std::cos(rad), cy + r * std::sin(rad), 0.0));
	}
}

// Append a filled wedge (centre + arc) as a POLYGON, same angle convention as annoAddArc.
static void annoAddWedge(vtkPoints *pts, vtkCellArray *polys, double cx, double cy, double r,
                         double a0, double a1, int nseg)
{
	if (nseg < 2) nseg = 2;
	polys->InsertNextCell(nseg + 2);
	polys->InsertCellPoint(pts->InsertNextPoint(cx, cy, 0.0));
	for (int i = 0; i <= nseg; ++i) {
		const double t   = a0 + (a1 - a0) * (double)i / (double)nseg;
		const double rad = (90.0 - t) * vtkMath::Pi() / 180.0;
		polys->InsertCellPoint(pts->InsertNextPoint(cx + r * std::cos(rad), cy + r * std::sin(rad), 0.0));
	}
}

// Append a quad (x0,y0)-(x1,y1) as a filled POLYGON.
static void annoAddQuad(vtkPoints *pts, vtkCellArray *polys, double x0, double y0, double x1, double y1)
{
	polys->InsertNextCell(4);
	polys->InsertCellPoint(pts->InsertNextPoint(x0, y0, 0.0));
	polys->InsertCellPoint(pts->InsertNextPoint(x1, y0, 0.0));
	polys->InsertCellPoint(pts->InsertNextPoint(x1, y1, 0.0));
	polys->InsertCellPoint(pts->InsertNextPoint(x0, y1, 0.0));
}

static void annoAddSegment(vtkPoints *pts, vtkCellArray *lines, double x0, double y0, double x1, double y1)
{
	lines->InsertNextCell(2);
	lines->InsertCellPoint(pts->InsertNextPoint(x0, y0, 0.0));
	lines->InsertCellPoint(pts->InsertNextPoint(x1, y1, 0.0));
}

// Shift every point of one or two arrays so their COMBINED bounding box lies inside the viewport,
// and report the shift so a text actor placed against the same geometry moves with it.
//
// GMT's +o offsets are measured against a canvas whose plot already sits inside a margin; this
// viewer's render fills the window edge to edge, so the same numbers put an element flush against the
// border — and anything that grows outward from its anchor (a label's textbox, a pen's own width)
// crosses it. Clamping is applied to the finished geometry rather than to each builder, so one rule
// covers every kind and no future style can be added without it.
static void annoClampInside(vtkPoints *a, vtkPoints *b, int W, int H, double pad, double *dx, double *dy)
{
	*dx = 0.0; *dy = 0.0;
	double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
	vtkPoints *sets[2] = {a, b};
	for (int k = 0; k < 2; ++k) {
		if (!sets[k]) continue;
		for (vtkIdType i = 0; i < sets[k]->GetNumberOfPoints(); ++i) {
			double q[3];
			sets[k]->GetPoint(i, q);
			for (int c = 0; c < 2; ++c) {
				if (q[c] < lo[c]) lo[c] = q[c];
				if (q[c] > hi[c]) hi[c] = q[c];
			}
		}
	}
	if (lo[0] > hi[0]) return;                                   // nothing was built
	const double lim[2] = {(double)W, (double)H};
	double d[2] = {0.0, 0.0};
	for (int c = 0; c < 2; ++c) {
		if (lo[c] - pad < 0.0)             d[c] = pad - lo[c];
		else if (hi[c] + pad > lim[c])     d[c] = lim[c] - pad - hi[c];
		// A thing wider than the viewport cannot be fitted; pinning its low edge is the readable
		// failure (its start is visible), and clamping twice would just undo the first move.
		if (hi[c] - lo[c] + 2 * pad > lim[c]) d[c] = pad - lo[c];
	}
	*dx = d[0]; *dy = d[1];
	if (d[0] == 0.0 && d[1] == 0.0) return;
	for (int k = 0; k < 2; ++k) {
		if (!sets[k]) continue;
		for (vtkIdType i = 0; i < sets[k]->GetNumberOfPoints(); ++i) {
			double q[3];
			sets[k]->GetPoint(i, q);
			sets[k]->SetPoint(i, q[0] + d[0], q[1] + d[1], q[2]);
		}
		sets[k]->Modified();
	}
}

// Lay out ONE annotation for the current render size. Rebuilds its polydata in place (the actors
// themselves are created once, in movieAnnoAdd) so a per-render call costs no allocation churn in
// VTK's pipeline beyond the point arrays.
static void annoLayoutOne(Scene *s, MovieAnno &a, int W, int H)
{
	if (!s || W <= 1 || H <= 1) return;
	const int vis = a.visible ? 1 : 0;
	if (a.label)       a.label->SetVisibility(vis);
	if (a.boxActor)    a.boxActor->SetVisibility(vis);
	if (a.boxLine)     a.boxLine->SetVisibility(vis);
	if (a.staticActor) a.staticActor->SetVisibility(vis);
	if (a.movingActor) a.movingActor->SetVisibility(vis);
	if (!a.visible) return;

	// "auto" font: GMT scales its label font with the canvas, so a movie rendered at 4K does not get
	// the same pixel height as one at 480p. 3% of the smaller dimension matches GMT's own look.
	const double fs   = (a.fontsize > 0.0) ? a.fontsize : std::max(8.0, 0.03 * std::min(W, H));
	const double clr  = (a.clearance > 0.0) ? a.clearance : 0.15 * fs;
	// GMT's default +o is 20% of the font size, measured against a canvas whose plot already sits
	// inside a margin. This viewer's render fills the window edge to edge, so that number alone leaves
	// an annotation touching the border (and a textbox, which grows outward from its anchor by its
	// clearance, crossing it). The default is therefore the LARGER of GMT's number and 1% of the
	// smaller render dimension -- a deliberate divergence, and only a default: an explicit +o is
	// honoured exactly as given.
	const double offd = std::max(0.2 * fs, 0.01 * std::min(W, H));
	const double ox   = (a.offx != 0.0) ? a.offx : offd;
	const double oy   = (a.offy != 0.0) ? a.offy : offd;

	double rx = 0.0, ry = 0.0, inx = 0.0, iny = 0.0;
	annoRefPoint(a.just, W, H, &rx, &ry, &inx, &iny);
	rx += inx * ox;
	ry += iny * oy;

	if (!a.progress) {                                           // ---------------- -L label
		if (!a.label) return;
		a.label->SetInput(a.text.c_str());
		vtkTextProperty *tp = a.label->GetTextProperty();
		tp->SetFontSize((int)std::lround(fs));
		tp->SetColor(a.fontrgb[0], a.fontrgb[1], a.fontrgb[2]);
		// The text actor anchors itself the same way GMT's +j does, so the label grows INTO the frame
		// from its reference point instead of off the edge.
		const int col = a.just % 3, row = a.just / 3;
		if (col == 0) tp->SetJustificationToLeft();
		else if (col == 1) tp->SetJustificationToCentered();
		else tp->SetJustificationToRight();
		if (row == 0) tp->SetVerticalJustificationToTop();
		else if (row == 1) tp->SetVerticalJustificationToCentered();
		else tp->SetVerticalJustificationToBottom();
		a.label->GetPositionCoordinate()->SetCoordinateSystemToDisplay();
		a.label->SetPosition(rx, ry);

		// The box (+g fill / +p outline) is sized from what the text actor actually measures, so it
		// fits the string on screen rather than an estimate of it.
		if (a.boxPD && (a.hasFill || a.hasPen)) {
			double tsz[2] = {0.0, 0.0};
			a.label->GetSize(s->ren, tsz);
			const double tw = (double)tsz[0], th = (double)tsz[1];
			const double x0 = ((col == 0) ? rx : (col == 1) ? rx - tw * 0.5 : rx - tw) - clr;
			const double y0 = ((row == 0) ? ry - th : (row == 1) ? ry - th * 0.5 : ry) - clr;
			const double x1 = x0 + tw + 2 * clr, y1 = y0 + th + 2 * clr;
			vtkNew<vtkPoints> pts; vtkNew<vtkCellArray> polys; vtkNew<vtkCellArray> lines;
			if (a.rounded) {
				// +r: a rectangle with quarter-circle corners, radius capped so it never eats the box.
				// The declared cell size must be EXACTLY the number of points then inserted -- VTK
				// takes InsertNextCell's count as a promise and reads that many ids when it
				// triangulates, so an over-declared cell walks off the end of the array.
				const double r = std::min(clr * 2.0, std::min(x1 - x0, y1 - y0) * 0.5);
				const int    nq = manno::NSEG / 8;                  // segments per quarter turn
				polys->InsertNextCell(4 * (nq + 1));
				// Corner centres in counter-clockwise order from the top-right, and the quarter turn
				// each one sweeps. Tracing the ring in ONE direction is what keeps the polygon simple;
				// mixing directions folds it and the triangulation of a folded ring is garbage.
				const double cxs[4] = {x1 - r, x0 + r, x0 + r, x1 - r};
				const double cys[4] = {y1 - r, y1 - r, y0 + r, y0 + r};
				for (int q = 0; q < 4; ++q) {
					const double base = 90.0 * q;
					for (int i = 0; i <= nq; ++i) {
						const double t = (base + 90.0 * (double)i / (double)nq) * vtkMath::Pi() / 180.0;
						polys->InsertCellPoint(pts->InsertNextPoint(cxs[q] + r * std::cos(t), cys[q] + r * std::sin(t), 0.0));
					}
				}
			}
			else {
				annoAddQuad(pts, polys, x0, y0, x1, y1);
			}
			// The box grows OUTWARD from the text's anchor by its clearance, so at a corner it is the
			// box, not the glyphs, that crosses the border first. Clamp the finished rectangle and move
			// the text by the same delta, so the two can never come apart.
			double sdx = 0.0, sdy = 0.0;
			annoClampInside(pts, nullptr, W, H, 1.0, &sdx, &sdy);
			if (sdx != 0.0 || sdy != 0.0) a.label->SetPosition(rx + sdx, ry + sdy);
			// The ring is always stored; whether it PAINTS is the actor's visibility, set below. A
			// ternary that swapped the cell array for null would just be a second way to say the same
			// thing, one that also has to be undone when the user turns the fill back on.
			a.boxPD->SetPoints(pts);
			a.boxPD->SetPolys(polys);
			a.boxPD->Modified();
			if (a.boxActor) {
				a.boxActor->SetVisibility(a.hasFill ? 1 : 0);
				a.boxActor->GetProperty()->SetColor(a.fillrgb[0], a.fillrgb[1], a.fillrgb[2]);
			}
			if (a.boxLinePD && a.boxLine) {
				// The outline reuses the fill's ring as a closed polyline, so fill and outline can never
				// describe two different boxes.
				vtkNew<vtkCellArray> ring;
				const vtkIdType n = pts->GetNumberOfPoints();
				if (n > 1) {
					ring->InsertNextCell(n + 1);
					for (vtkIdType i = 0; i < n; ++i) ring->InsertCellPoint(i);
					ring->InsertCellPoint(0);
				}
				vtkNew<vtkPoints> cp; cp->DeepCopy(pts);
				a.boxLinePD->SetPoints(cp);
				a.boxLinePD->SetLines(ring);
				a.boxLinePD->Modified();
				a.boxLine->SetVisibility(a.hasPen ? 1 : 0);
				a.boxLine->GetProperty()->SetColor(a.penrgb[0], a.penrgb[1], a.penrgb[2]);
				a.boxLine->GetProperty()->SetLineWidth(a.penwidth > 0.0 ? a.penwidth : 1.0);
			}
		}
		else {
			if (a.boxActor) a.boxActor->SetVisibility(0);
			if (a.boxLine)  a.boxLine->SetVisibility(0);
			// No box, but the glyphs themselves still have to clear the border. Same clamp, over a
			// rectangle standing in for the text, so a boxed and an unboxed label sit the same way.
			double tsz[2] = {0.0, 0.0};
			a.label->GetSize(s->ren, tsz);
			const int col2 = a.just % 3, row2 = a.just / 3;
			const double tx0 = (col2 == 0) ? rx : (col2 == 1) ? rx - tsz[0] * 0.5 : rx - tsz[0];
			const double ty0 = (row2 == 0) ? ry - tsz[1] : (row2 == 1) ? ry - tsz[1] * 0.5 : ry;
			vtkNew<vtkPoints> tp2;
			tp2->InsertNextPoint(tx0, ty0, 0.0);
			tp2->InsertNextPoint(tx0 + tsz[0], ty0 + tsz[1], 0.0);
			double sdx = 0.0, sdy = 0.0;
			annoClampInside(tp2, nullptr, W, H, 1.0, &sdx, &sdy);
			if (sdx != 0.0 || sdy != 0.0) a.label->SetPosition(rx + sdx, ry + sdy);
		}
		return;
	}

	// ---------------- -P progress indicator
	// GMT's default sizes: 5% of the larger canvas dimension for the circular ones, 60% of the
	// relevant dimension for the axis-like ones.
	const bool circ = annoIsCircular(a);
	double Wd = a.width;
	if (Wd <= 0.0) Wd = circ ? 0.05 * std::max(W, H) : 0.60 * (double)W;
	annoApplyStyleDefaults(a, Wd);

	const double frac = (a.frac < 0.0) ? 0.0 : (a.frac > 1.0 ? 1.0 : a.frac);
	vtkNew<vtkPoints> spts; vtkNew<vtkCellArray> spolys; vtkNew<vtkCellArray> slines;
	vtkNew<vtkPoints> mpts; vtkNew<vtkCellArray> mpolys; vtkNew<vtkCellArray> mlines;
	double labx = 0.0, laby = 0.0, labfs = fs;

	if (circ) {
		const double R  = Wd * 0.5;
		const int col = a.just % 3, row = a.just / 3;
		const double cx = rx + ((col == 0) ? R : (col == 2) ? -R : 0.0);
		const double cy = ry + ((row == 0) ? -R : (row == 2) ? R : 0.0);
		labx = cx; laby = cy;
		labfs = 0.30 * Wd;                                        // GMT: 30% of indicator size
		const double sweep = 360.0 * frac;
		if (a.style == 0) {                                       // a: filled disc + filled wedge
			annoAddWedge(spts, spolys, cx, cy, R, 0.0, 360.0, manno::NSEG);
			if (sweep > 0.0) annoAddWedge(mpts, mpolys, cx, cy, R, 0.0, sweep,
			                              std::max(2, (int)std::lround(manno::NSEG * frac)));
		}
		else {                                                    // b, c: ring outline + growing arc
			annoAddArc(spts, slines, cx, cy, R, 0.0, 360.0, manno::NSEG);
			if (sweep > 0.0) annoAddArc(mpts, mlines, cx, cy, R, 0.0, sweep,
			                            std::max(2, (int)std::lround(manno::NSEG * frac)));
			if (a.style == 2 && sweep > 0.0) {
				// c is a circular ARROW: a filled head, 20% of the indicator size, at the leading end,
				// pointing along the arc (i.e. perpendicular to the radius, clockwise).
				const double head = 0.20 * Wd;
				const double rad  = (90.0 - sweep) * vtkMath::Pi() / 180.0;
				const double hx = cx + R * std::cos(rad), hy = cy + R * std::sin(rad);
				const double tx = std::sin(rad), ty = -std::cos(rad);     // clockwise tangent
				const double nx = std::cos(rad), ny = std::sin(rad);      // outward radial
				mpolys->InsertNextCell(3);
				mpolys->InsertCellPoint(mpts->InsertNextPoint(hx + tx * head * 0.5, hy + ty * head * 0.5, 0.0));
				mpolys->InsertCellPoint(mpts->InsertNextPoint(hx - tx * head * 0.25 + nx * head * 0.4,
				                                              hy - ty * head * 0.25 + ny * head * 0.4, 0.0));
				mpolys->InsertCellPoint(mpts->InsertNextPoint(hx - tx * head * 0.25 - nx * head * 0.4,
				                                              hy - ty * head * 0.25 - ny * head * 0.4, 0.0));
			}
		}
	}
	else {
		// Linear: a horizontal bar of length Wd, anchored by +j the same way the label is, with the
		// moving part growing left -> right.
		const int col = a.just % 3, row = a.just / 3;
		const double x0 = rx + ((col == 0) ? 0.0 : (col == 1) ? -Wd * 0.5 : -Wd);
		const double x1 = x0 + Wd;
		const double thick = std::max(a.bgwidth, a.fgwidth);
		const double y  = ry + ((row == 0) ? -thick : (row == 2) ? thick : 0.0);
		const double xm = x0 + Wd * frac;
		labx = xm; laby = y - thick * 2.0;                        // under the bar, at the moving end
		if (a.style == 3) {                                       // d: rounded line + cross-mark
			annoAddSegment(spts, slines, x0, y, x1, y);
			// "Rounded": VTK 2-D lines have no end caps, so the round ends are two small discs of the
			// pen's own radius -- the same look, built from geometry this file already draws.
			annoAddWedge(spts, spolys, x0, y, a.bgwidth * 0.5, 0.0, 360.0, manno::NSEG / 4);
			annoAddWedge(spts, spolys, x1, y, a.bgwidth * 0.5, 0.0, 360.0, manno::NSEG / 4);
			if (frac > 0.0) annoAddSegment(mpts, mlines, x0, y, xm, y);
			const double cross = std::max(3.0, a.bgwidth * 1.5);  // the cross-mark at the moving end
			annoAddSegment(mpts, mlines, xm, y - cross, xm, y + cross);
			labfs = 2.0 * a.bgwidth;
		}
		else if (a.style == 4) {                                  // e: plain axis, growing overlay
			annoAddSegment(spts, slines, x0, y, x1, y);
			if (frac > 0.0) annoAddSegment(mpts, mlines, x0, y, xm, y);
			labfs = 2.0 * a.bgwidth;
		}
		else {                                                    // f: axis + moving triangle
			annoAddSegment(spts, slines, x0, y, x1, y);
			const double t = 2.0 * a.bgwidth;                     // GMT: twice the axis width
			mpolys->InsertNextCell(3);
			mpolys->InsertCellPoint(mpts->InsertNextPoint(xm, y, 0.0));
			mpolys->InsertCellPoint(mpts->InsertNextPoint(xm - t, y + t * 1.6, 0.0));
			mpolys->InsertCellPoint(mpts->InsertNextPoint(xm + t, y + t * 1.6, 0.0));
			labfs = 3.0 * a.bgwidth;
		}
	}

	// A pen has WIDTH, and half of it hangs outside the geometry it is drawn over, so an indicator
	// placed at GMT's small default offset would still touch the border. Pad the clamp by the widest
	// pen this indicator uses; the label rides along on the same delta.
	{
		double sdx = 0.0, sdy = 0.0;
		annoClampInside(spts, mpts, W, H, 1.0 + 0.5 * std::max(a.bgwidth, a.fgwidth), &sdx, &sdy);
		labx += sdx;  laby += sdy;
	}
	// Empty cell arrays are stored as they are: a polydata with zero polys draws no polys, so there is
	// nothing a null would express that an empty array does not.
	if (a.staticPD) {
		a.staticPD->SetPoints(spts);
		a.staticPD->SetPolys(spolys);
		a.staticPD->SetLines(slines);
		a.staticPD->Modified();
	}
	if (a.movingPD) {
		a.movingPD->SetPoints(mpts);
		a.movingPD->SetPolys(mpolys);
		a.movingPD->SetLines(mlines);
		a.movingPD->Modified();
	}
	if (a.staticActor) {
		a.staticActor->GetProperty()->SetColor(a.bgrgb[0], a.bgrgb[1], a.bgrgb[2]);
		a.staticActor->GetProperty()->SetLineWidth(std::max(1.0, a.bgwidth));
		// c's static ring is DASHED, which is the one place a style needs a stipple.
		a.staticActor->GetProperty()->SetLineStipplePattern(a.style == 2 ? 0xF0F0 : 0xFFFF);
	}
	if (a.movingActor) {
		a.movingActor->GetProperty()->SetColor(a.fgrgb[0], a.fgrgb[1], a.fgrgb[2]);
		a.movingActor->GetProperty()->SetLineWidth(std::max(1.0, a.fgwidth));
	}
	// +a: the indicator carries its own centred label. Style a has no label option in GMT, so it never
	// gets one here either.
	if (a.label) {
		const bool wantLabel = a.annot && a.style != 0 && !a.text.empty();
		a.label->SetVisibility(wantLabel ? 1 : 0);
		if (wantLabel) {
			a.label->SetInput(a.text.c_str());
			vtkTextProperty *tp = a.label->GetTextProperty();
			tp->SetFontSize((int)std::lround(std::max(8.0, labfs)));
			tp->SetColor(a.fontrgb[0], a.fontrgb[1], a.fontrgb[2]);
			tp->SetJustificationToCentered();
			tp->SetVerticalJustificationToCentered();
			a.label->GetPositionCoordinate()->SetCoordinateSystemToDisplay();
			// A centred annotation is easily wider than the indicator it sits in (GMT sizes it from the
			// indicator, not from the string), so it gets the same clamp the geometry just had -- an
			// indicator parked against an edge must not push its own number off the frame.
			double tsz[2] = {0.0, 0.0};
			a.label->SetPosition(labx, laby);
			a.label->GetSize(s->ren, tsz);
			vtkNew<vtkPoints> lp;
			lp->InsertNextPoint(labx - tsz[0] * 0.5, laby - tsz[1] * 0.5, 0.0);
			lp->InsertNextPoint(labx + tsz[0] * 0.5, laby + tsz[1] * 0.5, 0.0);
			double ldx = 0.0, ldy = 0.0;
			annoClampInside(lp, nullptr, W, H, 1.0, &ldx, &ldy);
			if (ldx != 0.0 || ldy != 0.0) a.label->SetPosition(labx + ldx, laby + ldy);
		}
	}
}

static void layoutMovieAnnos(Scene *s)
{
	if (!s || !s->ren || s->movieAnnos.empty()) return;
	const int *sz = s->ren->GetSize();
	if (!sz) return;
	for (auto &a : s->movieAnnos) annoLayoutOne(s, a, sz[0], sz[1]);
}

// Renderer StartEvent: re-lay every annotation for the size this frame is about to be drawn at. Same
// hook symbolRescaleCB uses, so a resize needs no observer of its own.
static void movieAnnoRelayoutCB(vtkObject *, unsigned long, void *clientData, void *)
{
	layoutMovieAnnos(static_cast<Scene *>(clientData));
}

static MovieAnno *movieAnnoFind(Scene *s, int id)
{
	if (!s) return nullptr;
	for (auto &a : s->movieAnnos)
		if (a.id == id) return &a;
	return nullptr;
}

// Create the actors for one annotation and hand back its id. The caller has already filled in every
// look/placement field; this only builds the VTK side and registers the element.
static int movieAnnoAdd(Scene *s, MovieAnno proto)
{
	if (!s || !s->ren) return 0;
	proto.id = ++s->movieAnnoSeq;

	proto.label = vtkSmartPointer<vtkTextActor>::New();
	proto.label->GetTextProperty()->SetFontFamilyToArial();
	s->ren->AddViewProp(proto.label);

	if (!proto.progress) {
		proto.boxPD     = vtkSmartPointer<vtkPolyData>::New();
		proto.boxLinePD = vtkSmartPointer<vtkPolyData>::New();
		proto.boxActor  = annoMakeActor2D(proto.boxPD, proto.fillrgb, 0.0);
		proto.boxLine   = annoMakeActor2D(proto.boxLinePD, proto.penrgb, proto.penwidth);
		// The box goes UNDER its text: 2-D props draw in the order they were added, and a fill added
		// after the label would paint over the very string it is meant to back.
		s->ren->RemoveViewProp(proto.label);
		s->ren->AddViewProp(proto.boxActor);
		s->ren->AddViewProp(proto.boxLine);
		s->ren->AddViewProp(proto.label);
	}
	else {
		proto.staticPD    = vtkSmartPointer<vtkPolyData>::New();
		proto.movingPD    = vtkSmartPointer<vtkPolyData>::New();
		proto.staticActor = annoMakeActor2D(proto.staticPD, proto.bgrgb, proto.bgwidth);
		proto.movingActor = annoMakeActor2D(proto.movingPD, proto.fgrgb, proto.fgwidth);
		s->ren->RemoveViewProp(proto.label);
		s->ren->AddViewProp(proto.staticActor);          // static first: the moving part rides on top
		s->ren->AddViewProp(proto.movingActor);
		s->ren->AddViewProp(proto.label);
	}

	s->movieAnnos.push_back(proto);

	if (!s->movieAnnoCmd) {                             // install the per-render layout once per scene
		vtkSmartPointer<vtkCallbackCommand> cmd = vtkSmartPointer<vtkCallbackCommand>::New();
		cmd->SetCallback(movieAnnoRelayoutCB);
		cmd->SetClientData(s);
		s->ren->AddObserver(vtkCommand::StartEvent, cmd);
		s->movieAnnoCmd = cmd;
	}
	layoutMovieAnnos(s);                                // prime placement for the first frame
	rebuildSceneObjects(s);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	return proto.id;
}

// Remove one annotation and every actor it owns. Removal undoes exactly what the add did -- no actor
// is left parented to the renderer after its element is gone.
static bool movieAnnoRemove(Scene *s, int id)
{
	if (!s || !s->ren) return false;
	for (size_t i = 0; i < s->movieAnnos.size(); ++i) {
		if (s->movieAnnos[i].id != id) continue;
		MovieAnno &a = s->movieAnnos[i];
		if (a.label)       s->ren->RemoveViewProp(a.label);
		if (a.boxActor)    s->ren->RemoveViewProp(a.boxActor);
		if (a.boxLine)     s->ren->RemoveViewProp(a.boxLine);
		if (a.staticActor) s->ren->RemoveViewProp(a.staticActor);
		if (a.movingActor) s->ren->RemoveViewProp(a.movingActor);
		s->movieAnnos.erase(s->movieAnnos.begin() + i);
		rebuildSceneObjects(s);
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return true;
	}
	return false;
}

// Push this frame's string and/or progress into one annotation (id > 0) or into EVERY one (id <= 0,
// which is what movie() uses: one call per frame updates the whole set).
static bool movieAnnoSet(Scene *s, int id, const char *text, double frac, bool haveText, bool haveFrac)
{
	if (!s) return false;
	bool any = false;
	for (auto &a : s->movieAnnos) {
		if (id > 0 && a.id != id) continue;
		if (haveText && text) a.text = text;
		if (haveFrac) a.frac = frac;
		any = true;
	}
	if (any) layoutMovieAnnos(s);
	return any;
}

// The human-readable name of what a label is showing, for the Scene Objects row and its properties.
static const char *annoSourceName(int source)
{
	switch (source) {
	case 0:  return "elapsed time";
	case 1:  return "frame number";
	case 2:  return "percent complete";
	case 3:  return "fixed text";
	case 4:  return "table column";
	default: return "table word";
	}
}
