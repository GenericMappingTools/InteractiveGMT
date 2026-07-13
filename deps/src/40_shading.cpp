static vtkSmartPointer<vtkTexture> makeSkyEnv(double gain = 1.0) {
	const int W = 1024, H = 512;
	const double sunAz = 0.62, sunEl = 0.40;     // sun direction (fraction of 2pi / pi)
	const double sunCx = sunAz * W, sunCy = (1.0 - sunEl) * H, sunR = 70.0;
	vtkNew<vtkImageData> sky;
	sky->SetDimensions(W, H, 1);
	sky->AllocateScalars(VTK_FLOAT, 3);
	float *px = static_cast<float*>(sky->GetScalarPointer());
	for (int y = 0; y < H; ++y) {
		double v = double(y) / (H - 1);          // 0 top .. 1 bottom
		double up = 1.0 - v;                      // 0 horizon-ish .. 1 zenith
		// sky above the mid-line, ground below
		double r, g, b;
		if (up > 0.5) {                           // sky: zenith -> horizon
			double t = (up - 0.5) * 2.0;          // 0 horizon .. 1 zenith
			r = 0.55 + 0.85 * t; g = 0.62 + 1.05 * t; b = 0.78 + 1.35 * t;
		} else {                                  // ground: horizon -> nadir
			double t = up * 2.0;                  // 0 nadir .. 1 horizon
			r = 0.20 + 0.30 * t; g = 0.18 + 0.30 * t; b = 0.16 + 0.32 * t;
		}
		for (int x = 0; x < W; ++x) {
			double rr = r, gg = g, bb = b;
			double dx = x - sunCx, dy = y - sunCy;
			double d = std::sqrt(dx*dx + dy*dy);
			if (d < sunR * 3.0) {                 // soft sun glow + hot core
				double s = std::exp(-(d*d) / (2.0 * sunR * sunR));
				rr += 6.0 * s; gg += 5.6 * s; bb += 4.8 * s;
			}
			*px++ = float(rr * gain); *px++ = float(gg * gain); *px++ = float(bb * gain);
		}
	}
	vtkSmartPointer<vtkTexture> env = vtkSmartPointer<vtkTexture>::New();
	env->SetInputData(sky);
	env->SetColorModeToDirectScalars();
	env->MipmapOn();
	env->InterpolateOn();
	env->UseSRGBColorSpaceOff();
	return env;
}

// GMT gmt_illuminate() port (gmt_support.c): modulate a CPT colour by a hillshade INTENSITY in
// [-1,1] exactly the way grdimage does — in HSV space, preserving hue. Positive intensity lightens
// (slope faces the sun), negative darkens (slope in shade), 0 leaves the colour untouched. GMT
// defaults: COLOR_HSV_MAX_S 0.1, MIN_S 1.0, MAX_V 1.0, MIN_V 0.3. rgb in/out are 0..1.
static void gmtIlluminate(double intensity, double *rgb) {
	if (intensity == 0.0) return;
	if (intensity >  1.0) intensity =  1.0;
	if (intensity < -1.0) intensity = -1.0;
	double hsv[3]; vtkMath::RGBToHSV(rgb, hsv);
	double s = hsv[1], v = hsv[2];
	if (intensity > 0.0) {                            // lighten toward low-saturation white
		const double di = 1.0 - intensity;
		if (s != 0.0) s = di * s + intensity * 0.1;   // COLOR_HSV_MAX_S
		v = di * v + intensity * 1.0;                 // COLOR_HSV_MAX_V
	}
	else {                                            // darken toward grey
		const double di = 1.0 + intensity;
		if (s != 0.0) s = di * s - intensity * 1.0;   // COLOR_HSV_MIN_S
		v = di * v - intensity * 0.3;                 // COLOR_HSV_MIN_V
	}
	hsv[1] = s < 0.0 ? 0.0 : (s > 1.0 ? 1.0 : s);
	hsv[2] = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
	vtkMath::HSVToRGB(hsv, rgb);
}

// ============================================================================
// ONE relief-shade source of truth. The 3-D surface mapper (hillshadeMapper) AND the flat 2-D
// image bake (bakeLayerRGBA) BOTH call applyReliefShade so the SAME grid + sun produces the SAME
// colours whether drawn as a warped surface or a flat picture. Never fork this maths again.
//   (A) Lambert   (hillGrd == false): CPT(z) * (ambient + (1-ambient)*max(0, n'·L)), darken-only,
//                 with the normal VE-corrected onto the displayed relief (n.x/xfac, n.y, n.z/(zfac·ve)).
//   (B) grdimage  (hillGrd == true):  gmt_illuminate(CPT(z), (2/π)·atan(gain·(n·Lg − Lgz))), the GMT
//                 grdimage -I look from the TRUE-coord z-gradient normal (VE-independent).
// ============================================================================
struct ReliefLight {
	double Lx, Ly, Lz;          // sun dir, lit convention (Lambert / PBR key light)
	double LxG, LyG, LzG;       // sun dir, grdimage (inverted elevation)
	double fx, fz;              // Lambert VE-correction factors (1/xfac, 1/(zfac·ve))
	double amb, gain, twoOverPi;
	bool   grd;
	double rough, keyI, fillI;  // PBR bake: roughness, key + fill light intensity (metallic assumed 0)
};
static ReliefLight makeReliefLight(Scene *s) {
	ReliefLight L;
	const double az  = s->lightAz * vtkMath::Pi() / 180.0;
	const double el  = s->lightEl * vtkMath::Pi() / 180.0;
	L.Lx = std::sin(az) * std::cos(el);  L.Ly = std::cos(az) * std::cos(el);  L.Lz = std::sin(el);
	const double elG = (90.0 - s->lightEl) * vtkMath::Pi() / 180.0;   // grdimage inverts elevation
	L.LxG = std::sin(az) * std::cos(elG); L.LyG = std::cos(az) * std::cos(elG); L.LzG = std::sin(elG);
	L.fx = (s->xfac != 0.0) ? 1.0 / s->xfac : 1.0;
	L.fz = (s->zfac * s->ve != 0.0) ? 1.0 / (s->zfac * s->ve) : 1.0;
	L.amb = s->hillAmbient;  L.gain = s->hillGain;  L.twoOverPi = 2.0 / vtkMath::Pi();  L.grd = s->hillGrd;
	L.rough = s->roughness < 0.05 ? 0.05 : s->roughness;   // clamp so the GGX lobe stays finite
	L.keyI = s->lightIntensity;  L.fillI = s->fillIntensity;
	return L;
}

// CPU approximation of the GPU PBR SURFACE look, baked per flat-image pixel so "Shaded image" alone
// reproduces the shaded relief a freshly loaded 3-D grid shows — WITHOUT triangulating the grid
// (the GPU shader rasterises triangles; this shades straight from the data-space normal nv). A 2-D
// map is viewed straight down, so the view vector is +Z. Metallic-0 dielectric Cook-Torrance:
// GGX distribution + Smith geometry + Schlick Fresnel for the key light, plus a soft hemispherical
// fill and a small ambient floor so the shadow side matches the lit surface (which the fill + scene
// light also lift). Not pixel-identical to the GPU pass — the live Light/Fill/Roughness sliders tune
// it. rgb (0..1, in albedo / out shaded).
static inline void applyPBRShade(const ReliefLight &L, const double nv[3], double rgb[3]) {
	const double Lk[3] = { L.Lx, L.Ly, L.Lz };
	double NdotL = nv[0]*Lk[0] + nv[1]*Lk[1] + nv[2]*Lk[2]; if (NdotL < 0.0) NdotL = 0.0;
	double NdotV = nv[2] < 0.0 ? 0.0 : nv[2];                             // V = +Z
	double H[3] = { Lk[0], Lk[1], Lk[2] + 1.0 };                          // half-vector of Lk and +Z
	const double hl = std::sqrt(H[0]*H[0] + H[1]*H[1] + H[2]*H[2]);
	if (hl > 0.0) { H[0] /= hl; H[1] /= hl; H[2] /= hl; }
	double NdotH = nv[0]*H[0] + nv[1]*H[1] + nv[2]*H[2]; if (NdotH < 0.0) NdotH = 0.0;
	const double VdotH = H[2] < 0.0 ? 0.0 : H[2];                         // V·H = H.z (V = +Z)
	const double a2 = L.rough*L.rough*L.rough*L.rough;                    // a = rough^2, a2 = a^2
	const double dn = NdotH*NdotH*(a2 - 1.0) + 1.0;
	const double D  = a2 / (vtkMath::Pi()*dn*dn + 1e-9);
	const double k  = L.rough*L.rough*0.5;
	const double G  = (NdotV/(NdotV*(1.0-k)+k+1e-9)) * (NdotL/(NdotL*(1.0-k)+k+1e-9));
	const double F  = 0.04 + 0.96*std::pow(1.0 - VdotH, 5.0);
	const double spec = (D*G*F) / (4.0*NdotV*NdotL + 1e-4);
	const double kd = 1.0 - F;
	for (int i = 0; i < 3; ++i) {
		const double lit  = (kd*rgb[i] + spec) * L.keyI * NdotL;         // key light (diffuse + specular)
		const double fill = rgb[i] * L.fillI * (0.5 + 0.5*nv[2]);        // hemispherical fill from above
		const double amb  = rgb[i] * 0.12;                              // scene-light floor (no black valleys)
		const double v = lit + fill + amb;
		rgb[i] = v > 1.0 ? 1.0 : v;
	}
}
// Modulate rgb (0..1, in/out) by the relief shade for a TRUE-coord surface normal nv.
static inline void applyReliefShade(const ReliefLight &L, const double nv[3], double rgb[3]) {
	if (!L.grd) {                                             // (A) Lambert, VE-corrected, darken-only
		double cx = nv[0]*L.fx, cy = nv[1], cz = nv[2]*L.fz;
		const double len = std::sqrt(cx*cx + cy*cy + cz*cz);
		if (len > 0.0) { cx /= len; cy /= len; cz /= len; }
		double sh = cx*L.Lx + cy*L.Ly + cz*L.Lz;
		if (sh < 0.0) sh = 0.0;
		const double I = L.amb + (1.0 - L.amb) * sh;
		rgb[0] = std::min(1.0, rgb[0]*I); rgb[1] = std::min(1.0, rgb[1]*I); rgb[2] = std::min(1.0, rgb[2]*I);
		return;
	}
	const double raw   = nv[0]*L.LxG + nv[1]*L.LyG + nv[2]*L.LzG - L.LzG;   // slope-toward-sun; 0 on flat
	const double inten = L.twoOverPi * std::atan(L.gain * raw);
	gmtIlluminate(inten, rgb);
}

// ============================================================================
// Flat illuminated IMAGE bake (3-D-cube layer scrubbing). A cube layer is drawn as a flat quad
// carrying a hillshade texture instead of a warped surface (see gmtvtk_show_layer_image_h,
// 90_c_api.cpp). The SAME illumination the 3-D relief uses is baked per texture pixel here, honouring
// the SAME Shading-dock toggles (useHillshade master, hillGrd = grdimage vs Lambert, az/el/gain/
// ambient) so the dock drives a cube layer exactly like a surface — the only difference is the result
// is written into the drape texture (rebakeLayerImage) rather than a per-vertex colour array.
// Cast-shadows does NOT apply (a flat plane has no relief to self-shadow); it stays 3-D-only.
// ============================================================================

// Cap the baked texture to this many pixels. A cube layer's hillshade is a screen picture, never
// larger than a display needs, so a heavy cube subsamples to here — the per-layer bake cost is then
// bounded no matter how big the grid is (the full-res z still lives in s->gridZ for the readout).
static const size_t kLayerTexMaxPix = 1500000;

// Pick the baked texture size (txW,txH) for a grid of nx*ny — the grid size, subsampled uniformly
// (aspect preserved) once it exceeds kLayerTexMaxPix.
static void layerTexSize(int nx, int ny, int &txW, int &txH) {
	txW = nx; txH = ny;
	const double npix = (double)nx * ny;
	if (npix > (double)kLayerTexMaxPix) {
		const double sc = std::sqrt((double)kLayerTexMaxPix / npix);
		txW = std::max(2, (int)std::lround(nx * sc));
		txH = std::max(2, (int)std::lround(ny * sc));
	}
}

// Bake a txW*txH RGBA texture (row 0 = south, matching the drape origin) from grid z, a CPT and the
// scene's shading state. The CPT is discretized into a 1024-entry table ONCE (per-pixel
// vtkColorTransferFunction::GetColor was the cube-scrub stall). Grid is subsampled to the texture
// size (nearest) but the slope uses full-res neighbours so the hillshade stays crisp. Colour per
// pixel follows the Shading dock, mirroring hillshadeMapper. In flat-image mode the two Hillshade
// boxes are the ONLY illumination control (PBR lights don't touch a baked texture); with BOTH off
// the image is drawn as plain CPT colour, no shade — a deliberately available "flat map" look.
//   * hillshade OFF + litBake   -> PBR (C): CPU Cook-Torrance, the lit-surface look (applyPBRShade).
//   * hillshade OFF + !litBake  -> plain CPT(z), no shade.
//   * useHillshade + hillGrd    -> grdimage (B): data-gradient normal, atan soft-clip, gmt_illuminate.
//   * useHillshade + !hillGrd   -> Lambert (A): CPT(z) * (ambient + (1-ambient)*max(0, n.L)),
//                                  darken-only. Uses the DATA-space normal (VE-independent — a flat
//                                  image has no displayed relief to VE-correct onto, unlike the 3-D
//                                  surface Lambert).
// NaN z -> the Preferences NaN fill colour (opaque). Bakes an arbitrary TRUE-coord WINDOW [wx0,wx1]x[wy0,wy1] of the grid (the
// whole extent for the base texture, a zoomed sub-rectangle for the detail tile), so the same code
// serves both the full-map bake and the hi-res zoom refine. This loop IS the cost of a layer switch /
// relight / zoom refine: no VTK geometry.
static void bakeLayerRGBA(Scene *s, const float *z, int nx, int ny, double gx0, double gy0,
                          double dx, double dy, vtkColorTransferFunction *ctf, double lo, double hi,
                          double wx0, double wx1, double wy0, double wy1,
                          int txW, int txH, std::vector<unsigned char> &out) {
	out.assign((size_t)txW * txH * 4, 0);
	if (!ctf || dx == 0.0 || dy == 0.0) return;
	// discretize the CPT once
	const int NT = 1024;
	std::vector<unsigned char> tbl((size_t)NT * 3);
	{
		const double span = (hi > lo) ? (hi - lo) : 1.0;
		double c[3];
		for (int i = 0; i < NT; ++i) {
			ctf->GetColor(lo + span * i / (NT - 1), c);
			tbl[3*i+0] = (unsigned char)(c[0]*255.0+0.5);
			tbl[3*i+1] = (unsigned char)(c[1]*255.0+0.5);
			tbl[3*i+2] = (unsigned char)(c[2]*255.0+0.5);
		}
	}
	const double invspan = (hi > lo) ? (NT - 1) / (hi - lo) : 0.0;
	const bool   pbr   = !s->useHillshade && s->litBake;   // flat PBR bake (approximates the lit surface)
	const bool   shade = s->useHillshade || pbr;           // any per-pixel shade (hillshade or PBR)
	const ReliefLight L = makeReliefLight(s);      // SAME light/style the 3-D surface uses (one source of truth)
	auto Zc = [&](int ix, int iy) -> double { return z[(size_t)ix * ny + iy]; };   // column-major z[ix*ny+iy]
	auto clampi = [](int v, int hi2) { return v < 0 ? 0 : (v > hi2 ? hi2 : v); };
	// Per-row parallel: every output row is a disjoint slice of `out`, and every read (z, tbl LUT,
	// light L) is shared read-only, so no locks. vtkSMPTools runs on VTK's SMP backend (TBB here).
	vtkSMPTools::For(0, txH, [&](vtkIdType rBeg, vtkIdType rEnd) {
	for (int r = (int)rBeg; r < (int)rEnd; ++r) {
		const double ty = (txH > 1) ? wy0 + (wy1 - wy0) * r / (txH - 1) : wy0;   // row r=0 -> south
		const int iy  = clampi((int)std::lround((ty - gy0) / dy), ny - 1);
		const int iym = iy > 0 ? iy - 1 : iy, iyp = iy < ny - 1 ? iy + 1 : iy;
		for (int col = 0; col < txW; ++col) {
			const double tx = (txW > 1) ? wx0 + (wx1 - wx0) * col / (txW - 1) : wx0;
			const int ix = clampi((int)std::lround((tx - gx0) / dx), nx - 1);
			const double zc = Zc(ix, iy);
			unsigned char *p = out.data() + ((size_t)r * txW + col) * 4;
			if (std::isnan(zc)) {   // paint NaN with the Preferences NaN fill colour (opaque)
				p[0] = (unsigned char)(s->nanColor[0]*255.0+0.5);
				p[1] = (unsigned char)(s->nanColor[1]*255.0+0.5);
				p[2] = (unsigned char)(s->nanColor[2]*255.0+0.5);
				p[3] = 255; continue;
			}
			int ti = (int)((zc - lo) * invspan); if (ti < 0) ti = 0; else if (ti > NT - 1) ti = NT - 1;
			const unsigned char *rgb8 = &tbl[3 * ti];
			if (!shade) { p[0] = rgb8[0]; p[1] = rgb8[1]; p[2] = rgb8[2]; p[3] = 255; continue; }
			// central-difference gradient (full-res neighbours), edge-clamped; NaN neighbour -> flat.
			const int ixm = ix > 0 ? ix - 1 : ix, ixp = ix < nx - 1 ? ix + 1 : ix;
			const double za = Zc(ixp, iy), zb = Zc(ixm, iy);
			const double zu = Zc(ix, iyp), zd = Zc(ix, iym);
			const double dzdx = (ixp == ixm || std::isnan(za) || std::isnan(zb)) ? 0.0 : (za - zb) / ((ixp - ixm) * dx);
			const double dzdy = (iyp == iym || std::isnan(zu) || std::isnan(zd)) ? 0.0 : (zu - zd) / ((iyp - iym) * dy);
			double n0 = -dzdx, n1 = -dzdy, n2 = 1.0;
			const double len = std::sqrt(n0*n0 + n1*n1 + n2*n2);
			if (len > 0.0) { n0 /= len; n1 /= len; n2 /= len; }
			const double nv[3] = { n0, n1, n2 };
			double c[3] = { rgb8[0] / 255.0, rgb8[1] / 255.0, rgb8[2] / 255.0 };
			if (pbr) applyPBRShade(L, nv, c);                    // PBR lit look (no hillshade selected)
			else     applyReliefShade(L, nv, c);                 // SHARED hillshade (grdimage or Lambert), matches the surface
			p[0] = (unsigned char)std::min(255.0, c[0] * 255.0 + 0.5);
			p[1] = (unsigned char)std::min(255.0, c[1] * 255.0 + 0.5);
			p[2] = (unsigned char)std::min(255.0, c[2] * 255.0 + 0.5);
			p[3] = 255;
		}
	}
	});
}

// The visible TRUE-coord rectangle (W,E,S,N) = the part of the flat map on screen at the current
// zoom. Project the 4 viewport corners onto the z=0 plane, undo the X aspect scale (xfac), take the
// bbox, clamp to the data frame. Mirrors the hover readout / Geography visibleRegion math. false if
// nothing visible.
static bool layerVisibleRegion(Scene *s, double &W, double &E, double &S, double &N) {
	if (!s->ren || !s->widget || !s->widget->renderWindow()) return false;
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
		if (dirz == 0.0) continue;
		const double t0 = -nr[2] / dirz;
		const double tx = (nr[0] + t0 * (fr[0] - nr[0])) / gx;
		const double ty =  nr[1] + t0 * (fr[1] - nr[1]);
		if (!any) { W = E = tx; S = N = ty; any = true; }
		else { W = std::min(W, tx); E = std::max(E, tx); S = std::min(S, ty); N = std::max(N, ty); }
	}
	if (!any) return false;
	W = std::max(W, s->gx0); E = std::min(E, s->gx1);
	S = std::max(S, s->gy0); N = std::min(N, s->gy1);
	return (E > W && N > S);
}

// Re-bake the current cube layer's BASE drape texture (whole extent) in place from s->gridZ +
// s->surfLut + the current Shading state. Called from applyShading, so EVERY Shading-dock change
// (sun az/el, gain, Lambert/grdimage/off) relights the flat image live — the image-mode counterpart
// of hillshadeMapper re-colouring a surface. No-op unless the window is in cube-image mode.
static void invalidateLayerDetail(Scene *s);   // fwd (defined below)
static void rebakeLayerImage(Scene *s) {
	if (!s || !s->layerImgMode || !s->drape || s->gridZ.empty() || s->gnx < 2 || s->gny < 2) return;
	vtkColorTransferFunction *ctf = vtkColorTransferFunction::SafeDownCast(s->surfLut);
	if (!ctf) return;
	vtkTexture *tx = s->drape->GetTexture();
	vtkImageData *id = tx ? vtkImageData::SafeDownCast(tx->GetInput()) : nullptr;
	if (!id) return;
	int dims[3] = { 0, 0, 0 }; id->GetDimensions(dims);
	if (dims[0] != s->layerTexW || dims[1] != s->layerTexH) return;
	std::vector<unsigned char> rgba;
	bakeLayerRGBA(s, s->gridZ.data(), s->gnx, s->gny, s->gx0, s->gy0, s->gdx, s->gdy, ctf,
	              s->zmin, s->zmax, s->gx0, s->gx1, s->gy0, s->gy1, s->layerTexW, s->layerTexH, rgba);
	if (rgba.size() != (size_t)dims[0] * dims[1] * 4) return;
	memcpy(id->GetScalarPointer(), rgba.data(), rgba.size());
	id->Modified(); tx->Modified();
	invalidateLayerDetail(s);   // shading changed -> the zoom detail tile is stale; refresh on settle
}

// Bake a HI-RES detail tile over the currently-visible sub-rectangle and lay it over the base drape,
// so a deep zoom shows crisp relief instead of the magnified base texels. Cheap because it covers
// only the visible window (bounded by the same kLayerTexMaxPix cap) and fires only when the camera
// SETTLES (layerDetailTimer), never during interaction or per layer switch. When zoomed out enough
// that the base texture already resolves the view, the tile is dropped.
static void refineLayerDetail(Scene *s) {
	if (!s || !s->layerImgMode || s->gridZ.empty() || s->gnx < 2 || s->gny < 2) return;
	if (!s->ren || !s->widget || !s->widget->renderWindow()) return;
	vtkColorTransferFunction *ctf = vtkColorTransferFunction::SafeDownCast(s->surfLut);
	if (!ctf) return;
	double W, E, S, N;
	if (!layerVisibleRegion(s, W, E, S, N)) return;
	const double fullx = s->gx1 - s->gx0, fully = s->gy1 - s->gy0;
	if (fullx <= 0.0 || fully <= 0.0) return;
	// Not zoomed in enough -> the base texture is fine; drop any existing tile.
	if ((E - W) / fullx > 0.55 && (N - S) / fully > 0.55) {
		if (s->layerDetail) { s->ren->RemoveActor(s->layerDetail); s->layerDetail = nullptr; s->layerDetailImg = nullptr; }
		s->layerDetailReg[0] = s->layerDetailReg[1] = 0.0;
		return;
	}
	// Already have (nearly) this region -> nothing to do.
	auto nearv = [](double a, double b, double span) { return std::abs(a - b) < 0.03 * span; };
	if (s->layerDetail && s->layerDetail->GetVisibility() &&
	    nearv(s->layerDetailReg[0], W, fullx) && nearv(s->layerDetailReg[1], E, fullx) &&
	    nearv(s->layerDetailReg[2], S, fully) && nearv(s->layerDetailReg[3], N, fully))
		return;
	// Texture: as many texels as the window has grid nodes, capped (crisp to the data limit).
	const int wnx = std::max(2, (int)std::lround((E - W) / s->gdx) + 1);
	const int wny = std::max(2, (int)std::lround((N - S) / s->gdy) + 1);
	int txW, txH; layerTexSize(wnx, wny, txW, txH);
	std::vector<unsigned char> rgba;
	bakeLayerRGBA(s, s->gridZ.data(), s->gnx, s->gny, s->gx0, s->gy0, s->gdx, s->gdy, ctf,
	              s->zmin, s->zmax, W, E, S, N, txW, txH, rgba);
	vtkSmartPointer<vtkImageData> id = vtkSmartPointer<vtkImageData>::New();
	id->SetDimensions(txW, txH, 1);
	id->AllocateScalars(VTK_UNSIGNED_CHAR, 4);
	memcpy(id->GetScalarPointer(), rgba.data(), rgba.size());
	vtkNew<vtkTexture> tex; tex->SetInputData(id); tex->InterpolateOn();
	// flat quad over [W,E]x[S,N], z=0, full-texture tcoords (row 0 = south).
	vtkNew<vtkPoints> pts; pts->SetDataTypeToFloat();
	pts->InsertNextPoint(W, S, 0); pts->InsertNextPoint(E, S, 0); pts->InsertNextPoint(E, N, 0); pts->InsertNextPoint(W, N, 0);
	vtkNew<vtkFloatArray> tc; tc->SetNumberOfComponents(2); tc->SetName("tc");
	tc->InsertNextTuple2(0, 0); tc->InsertNextTuple2(1, 0); tc->InsertNextTuple2(1, 1); tc->InsertNextTuple2(0, 1);
	vtkNew<vtkCellArray> cells; vtkIdType q[4] = { 0, 1, 2, 3 }; cells->InsertNextCell(4, q);
	vtkNew<vtkPolyData> pd; pd->SetPoints(pts); pd->SetPolys(cells); pd->GetPointData()->SetTCoords(tc);
	vtkNew<vtkPolyDataMapper> map; map->SetInputData(pd); map->ScalarVisibilityOff();
	vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
	map->SetRelativeCoincidentTopologyPolygonOffsetParameters(-2.0, -2.0);   // beat the base drape (-1) on the z-tie
	if (s->layerDetail) s->ren->RemoveActor(s->layerDetail);
	s->layerDetail = vtkSmartPointer<vtkActor>::New();
	s->layerDetail->SetMapper(map);
	s->layerDetail->SetTexture(tex);
	s->layerDetail->GetProperty()->LightingOff();
	s->layerDetail->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	s->layerDetailImg = id;
	s->layerDetailReg[0] = W; s->layerDetailReg[1] = E; s->layerDetailReg[2] = S; s->layerDetailReg[3] = N;
	s->ren->AddActor(s->layerDetail);
	if (s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// The detail tile is now stale (layer switched / relit): hide it and schedule a fresh refine once
// the user pauses. Debounced, so rapid scrubbing never bakes a tile mid-flight.
static void invalidateLayerDetail(Scene *s) {
	if (!s || !s->layerImgMode) return;
	if (s->layerDetail) s->layerDetail->SetVisibility(0);
	s->layerDetailReg[0] = s->layerDetailReg[1] = 0.0;   // force a rebake
	if (s->layerDetailTimer) s->layerDetailTimer->start(180);
}

// Camera-modified observer: (re)start the settle timer so the detail tile refines only once the
// camera stops moving, never every frame during a pan/zoom.
static void onLayerCamera(vtkObject*, unsigned long, void *cd, void*) {
	Scene *s = static_cast<Scene*>(cd);
	if (s && s->layerImgMode && s->layerDetailTimer) s->layerDetailTimer->start(180);
}

// Mirror the Scene's shading state onto the four Shading-dock checkboxes without re-firing their
// handlers. "Shaded image (2-D)" is the base GEOMETRY toggle (flat image vs surface) — checked when
// the base is currently a flat image, enabled whenever there is a grid to flip. The three hillshade
// looks (Cast shadows / Lambert / grdimage) are the ILLUMINATION, independent of the geometry toggle.
// Called at the end of applyShading so the dock always reflects the live state.
static void syncShadeChecks(Scene *s) {
	if (!s || !s->cbFlat) return;                       // dock not built yet
	QSignalBlocker b0(s->cbFlat), b1(s->cbShadow), b2(s->cbHillL), b3(s->cbHillG), b4(s->cbPBR);
	s->cbFlat->setEnabled(s->gnx > 1 && !s->gridZ.empty());
	s->cbFlat->setChecked(s->layerImgMode);
	s->cbShadow->setChecked(s->useShadows);
	s->cbHillL->setChecked(s->useHillshade && !s->hillGrd);
	s->cbHillG->setChecked(s->useHillshade &&  s->hillGrd);
	// PBR = the lit look with no hillshade / shadows. On a 3-D surface that IS the default (all off);
	// on a flat image it means the PBR bake is on (litBake). Reflect both so the box tracks reality.
	s->cbPBR->setChecked(!s->useHillshade && !s->useShadows && (s->layerImgMode ? s->litBake : true));
	if (s->syncFlatEnable) s->syncFlatEnable();   // grey the flat-dead controls when a layer enters image mode
}

// Baked hillshade for ONE surface mapper (single actor or a LOD tile). Two ALTERNATIVE styles,
// selected by s->hillGrd; both bake a per-point RGB "hillshade" field (active z scalars untouched,
// so the colour bar still maps z) and the caller renders the surface UNLIT. When off: revert to
// live CPT scalar colouring.
//
//  (A) s->hillGrd == false — LAMBERT (the original look): per-node colour = CPT(z) * Lambert shade.
//      The mesh normal is VE-CORRECTED to the displayed relief — normalize(n.x/xfac, n.y,
//      n.z/(zfac*ve)) — dotted with the sun, with a hillAmbient floor so valleys aren't pure black.
//      Darken-only (multiply). Shade tracks the on-screen exaggeration.
//
//  (B) s->hillGrd == true  — GMT grdimage from the z-GRADIENT (VE-independent): the baked normal is
//      already n = normalize(-dz/dx, -dz/dy, 1) in TRUE DATA units, so it IS the z-gradient. With
//      sun L (az from north CW, el above horizon) the Lambertian reflectance n.L is recentred to a
//      signed relief signal  raw = 2*(n.L) - 1  (so a sun OVERHEAD, el=90, lights flat ground
//      brightest and grazing sun darkens — el behaves like a real sun), soft-clipped to (-1,1) by
//      an atan (grdgradient -Nt style, amp = s->hillGain), then gmt_illuminate() blends it into the
//      CPT colour the way grdimage -I does (lightens AND darkens, hue preserved).
static void hillshadeMapper(Scene *s, vtkActor *act) {
	if (!act) return;
	vtkPolyDataMapper *m = vtkPolyDataMapper::SafeDownCast(act->GetMapper());
	if (!m) return;

	if (!s->useHillshade) {                       // revert to live CPT colouring
		m->SetScalarModeToUsePointData();         // colour from the active scalars (z)
		m->SetColorModeToMapScalars();            // through the LUT again
		m->ScalarVisibilityOn();
		return;
	}

	vtkPolyData *pd = vtkPolyData::SafeDownCast(m->GetInput());
	if (!pd) return;
	vtkDataArray *nrm = pd->GetPointData()->GetNormals();
	vtkDataArray *zs  = pd->GetPointData()->GetScalars();
	// Prefer the actor's OWN mapper LUT (a dropped grid extra carries its own CPT); fall back to the
	// scene's primary LUT. For the primary surface/tiles these are the same object, so this is a no-op
	// there but lets an extra grid hillshade with its true colours instead of the canvas LUT.
	vtkScalarsToColors *lut = m->GetLookupTable() ? m->GetLookupTable() : (s->surfLut ? s->surfLut.Get() : nullptr);
	if (!nrm || !zs || !lut) return;              // no normals/scalars/LUT -> leave as-is

	const ReliefLight L = makeReliefLight(s);      // SAME light/style the flat 2-D image bake uses (one source of truth)

	const vtkIdType n = pd->GetNumberOfPoints();
	// Map EVERY z to its CPT colour in one serial batch: vtkScalarsToColors::MapValue is NOT
	// thread-safe (returns a pointer into a shared internal buffer), so it can't be called from the
	// parallel loop. MapScalars is a vectorised, thread-safe alternative that returns an owned RGBA
	// array; the parallel loop then only does the (stateless) shade maths from those bytes + normals.
	vtkSmartPointer<vtkUnsignedCharArray> mapped =
		vtkSmartPointer<vtkUnsignedCharArray>::Take(lut->MapScalars(zs, VTK_COLOR_MODE_MAP_SCALARS, 0));
	if (!mapped) return;
	const int mc = mapped->GetNumberOfComponents();      // RGBA = 4
	vtkSmartPointer<vtkUnsignedCharArray> col = vtkSmartPointer<vtkUnsignedCharArray>::New();
	col->SetName("hillshade");
	col->SetNumberOfComponents(3);
	col->SetNumberOfTuples(n);
	// Per-point parallel: disjoint output tuples, all reads (mapped colours, normals, light L) shared
	// read-only. GetTuple(i, buf) writes the caller's own buffer, so it is safe under concurrent reads.
	vtkSMPTools::For(0, n, [&](vtkIdType iBeg, vtkIdType iEnd) {
	for (vtkIdType i = iBeg; i < iEnd; ++i) {
		double nv[3]; nrm->GetTuple(i, nv);
		const unsigned char *rgb8 = mapped->GetPointer(i * mc);     // CPT colour for this z (pre-mapped)
		double c[3] = { rgb8[0] / 255.0, rgb8[1] / 255.0, rgb8[2] / 255.0 };
		applyReliefShade(L, nv, c);                                 // SHARED shade (grdimage or Lambert)
		col->SetTypedComponent(i, 0, (unsigned char)(c[0] * 255.0 + 0.5));
		col->SetTypedComponent(i, 1, (unsigned char)(c[1] * 255.0 + 0.5));
		col->SetTypedComponent(i, 2, (unsigned char)(c[2] * 255.0 + 0.5));
	}
	});
	pd->GetPointData()->AddArray(col);
	m->SetScalarModeToUsePointFieldData();
	m->SelectColorArray("hillshade");
	m->SetColorModeToDirectScalars();               // use the baked RGB verbatim (no LUT)
	m->ScalarVisibilityOn();
}

// Apply the per-surface MATERIAL + hillshade colouring for one actor, honouring the Scene's
// style toggles. Shared by applyShading (all actors) and ensureNodeActor (each new LOD tile),
// so a tile built mid-flight matches the rest.
static void applySurfStyle(Scene *s, vtkActor *a) {
	vtkProperty *prop = a->GetProperty();
	if (s->useHillshade) {
		// Baked shade IS the shading -> render UNLIT (flat ambient) so colours show verbatim.
		prop->SetInterpolationToFlat();
		prop->SetAmbient(1.0); prop->SetDiffuse(0.0); prop->SetSpecular(0.0);
		prop->SetAmbientColor(1.0, 1.0, 1.0);
	}
	else if (s->matteSurf) {
		prop->SetInterpolationToPhong();
		prop->SetSpecular(0.0); prop->SetAmbient(0.25); prop->SetDiffuse(0.9);
	}
	else {
		// Restore VTK property defaults first: the hillshade branch leaves Diffuse=0/Ambient=1,
		// and PBR albedo = Diffuse-coeff * DiffuseColor, so a leftover Diffuse=0 renders the
		// surface near-black. Reset before re-asserting PBR.
		prop->SetAmbient(0.0); prop->SetDiffuse(1.0); prop->SetSpecular(0.0);
		prop->SetAmbientColor(1.0, 1.0, 1.0);
		prop->SetInterpolationToPBR();
		prop->SetMetallic(s->metallic);
		prop->SetRoughness(s->roughness);
		prop->SetBaseIOR(s->ior);
	}
	hillshadeMapper(s, a);   // bake or revert the per-node colours to match the material
}

// (Re)assemble the post-process pass chain from the Scene's toggles, then apply
// the live material/light values. Called at setup and from the Shading dock.
static void applyShading(Scene *s) {
	// material — PBR on the RELIEF SURFACE ONLY. The drape is a textured picture and MUST
	// stay Phong: VTK's PBR shader samples only SetBaseColorTexture, so a PBR drape ignores
	// its SetTexture and renders flat grey. Do NOT touch s->drape's material here.
	// Per-surface material + hillshade colouring. matteSurf (fv colour mesh) stays PURE-DIFFUSE
	// Phong (glossy PBR + bright sky IBL mirrored a grey specular highlight onto up-facing crest
	// facets — the "grey top row"); useHillshade renders UNLIT with baked CPT*shade colours; else
	// PBR. applySurfStyle runs at setup AND on every shading slider, so it must re-assert the
	// material each call or a slider would re-clobber it.
	for (vtkActor *a : surfActors(s))     // all tiles (tiled grid) or the single surface
		applySurfStyle(s, a);
	// Dropped GRID surfaces (extras) are shaded too, so the Shading dock controls them like the
	// primary relief — each hillshades with its own CPT (hillshadeMapper prefers the actor's LUT).
	// Image extras are textured pictures (kept unlit) and are left untouched.
	for (auto& ex : s->extras)
		if (!ex.isImage && ex.actor) applySurfStyle(s, ex.actor.Get());
	// key light: aim from azimuth (deg from north, clockwise) + elevation.
	// dir points FROM the scene TO the sun; for a directional light only the
	// Position-minus-FocalPoint direction matters.
	{
		const double az = s->lightAz * vtkMath::Pi() / 180.0;
		const double el = s->lightEl * vtkMath::Pi() / 180.0;
		const double dx = std::sin(az) * std::cos(el);   // east
		const double dy = std::cos(az) * std::cos(el);   // north
		const double dz = std::sin(el);                  // up
		s->keyLight->SetFocalPoint(0.0, 0.0, 0.0);
		s->keyLight->SetPosition(dx, dy, dz);
		s->keyLight->SetIntensity(s->lightIntensity);
		s->fillLight->SetIntensity(s->fillIntensity);
	}
	// The gizmo adds its OWN bright (1.4) scene light to s->ren (20_gizmo enableGizmo). It is a
	// shadow-caster (non-headlight, non-positional) brighter than the sun, shining from a fixed
	// near-overhead direction, so it floods the sun's cast shadows and bakes a competing second
	// shadow map -> toggling "Cast shadows" looked like it did nothing. Cast-shadows and the gizmo
	// light are alternatives: mute the gizmo light while shadows are on, restore it when off.
	if (s->giz && s->giz->light)
		s->useShadows ? s->giz->light->SwitchOff() : s->giz->light->SwitchOn();
	// IBL — vtkRenderer (9.6) has no env-intensity setter, so bake the gain into
	// the texture (rebuilt only when the value actually moved, to keep sliders cheap).
	if (s->useIBL) {
		static double lastGain = -1.0;
		if (!s->envTex || s->envIntensity != lastGain) {
			s->envTex = makeSkyEnv(s->envIntensity);
			lastGain = s->envIntensity;
		}
		s->ren->UseImageBasedLightingOn();
		s->ren->SetEnvironmentTexture(s->envTex);
	} else {
		s->ren->UseImageBasedLightingOff();
		s->ren->SetEnvironmentTexture(nullptr);
	}
	// pass chain: base -> [SSAO] -> [tone] -> [FXAA]
	// base is either the cast-shadow camera pass (sun self-shadowing) or the plain render steps.
	// The shadow graph is built ONCE and cached on the Scene (rebuilding passes leaks GPU FBOs):
	// only keyLight casts (fillLight is a headlight, which the baker skips), so terrain shadows
	// fall along the sun azimuth/elevation and track the light sliders for free.
	vtkSmartPointer<vtkRenderPass> chain;
	if (s->useShadows) {
		if (!s->shadowCam) {
			// shared opaque sequence (lights + opaque), used by BOTH the depth-map baker and the
			// shadowed main pass so they rasterise identical geometry.
			vtkNew<vtkLightsPass>           lights;
			vtkNew<vtkOpaquePass>           opaque;
			vtkNew<vtkSequencePass>         opaqueSeq;
			vtkNew<vtkRenderPassCollection> opaquePasses;
			opaquePasses->AddItem(lights);
			opaquePasses->AddItem(opaque);
			opaqueSeq->SetPasses(opaquePasses);

			s->shadowBaker = vtkSmartPointer<vtkShadowMapBakerPass>::New();
			s->shadowBaker->SetOpaqueSequence(opaqueSeq);
			s->shadowBaker->SetResolution(s->shadowRes);

			vtkNew<vtkShadowMapPass> shadow;
			shadow->SetShadowMapBakerPass(s->shadowBaker);
			shadow->SetOpaqueSequence(opaqueSeq);

			// full scene sequence: lights -> shadowed opaque -> translucent -> volumetric -> overlay.
			vtkNew<vtkTranslucentPass>      translucent;
			vtkNew<vtkVolumetricPass>       volumetric;
			vtkNew<vtkOverlayPass>          overlay;
			vtkNew<vtkSequencePass>         seq;
			vtkNew<vtkRenderPassCollection> passes;
			passes->AddItem(lights);
			passes->AddItem(shadow);
			passes->AddItem(translucent);
			passes->AddItem(volumetric);
			passes->AddItem(overlay);
			seq->SetPasses(passes);

			s->shadowCam = vtkSmartPointer<vtkCameraPass>::New();   // owns the whole graph above
			s->shadowCam->SetDelegatePass(seq);
		}
		s->shadowBaker->SetResolution(s->shadowRes);
		chain = s->shadowCam;
	} else {
		chain = vtkSmartPointer<vtkRenderStepsPass>::New();
	}
	if (s->useSSAO) {
		if (!s->ssao) s->ssao = vtkSmartPointer<vtkSSAOPass>::New();
		s->ssao->SetRadius(s->ssaoRadius);
		s->ssao->SetBias(s->ssaoBias);
		s->ssao->SetKernelSize(256);
		s->ssao->BlurOn();
		s->ssao->SetDelegatePass(chain);
		chain = s->ssao;
	}
	if (s->useTone) {
		if (!s->tone) s->tone = vtkSmartPointer<vtkToneMappingPass>::New();
		s->tone->SetToneMappingType(vtkToneMappingPass::NeutralPBR);
		s->tone->SetDelegatePass(chain);
		chain = s->tone;
	}
	if (s->useFXAA) {
		if (!s->fxaa) s->fxaa = vtkSmartPointer<vtkOpenGLFXAAPass>::New();
		s->fxaa->SetDelegatePass(chain);
		chain = s->fxaa;
	}
	s->ren->SetPass(chain);
	rebakeLayerImage(s);   // flat-image mode: relight the drape texture to match the new state (shared shade)
	syncShadeChecks(s);    // keep the Shading-dock checkboxes in sync with the live state
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// Add a GMTdataset overlay (lines or points) to an existing scene. `xyz` is npts
// triples (x,y,z) in TRUE data coords; `segoff` holds nseg+1 offsets so segment k is
// points [segoff[k], segoff[k+1]). mode 0 = points, 1 = polylines. The actor gets the
// surface's base scale (xfac/zfac/VE) so it sits in register, and a coincident-topology
// offset toward the camera so it wins the depth tie with the surface (draws on top).
// Rebuild the Scene Objects panel: one checkbox per scene element (surface, image
// drape, each line/point overlay) that toggles the actor's visibility. Re-called
// whenever the overlay set changes, since overlays are added after the window shows.
