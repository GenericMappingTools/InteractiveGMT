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
//   (B) grdimage  (hillGrd == true):  gmt_illuminate(CPT(z), (2/Ï€)·atan(gain·(n·Lg − Lgz))), the GMT
//                 grdimage -I look from the TRUE-coord z-gradient normal (VE-independent).
// ============================================================================
// --- the Hillshade tool's externally-computed reflectance -------------------------------------
// Present only while an illumination model from View > "Illumination (Hillshade)…" is loaded.
// Sampled by WORLD position so every consumer (surface, LOD tile, flat bake, Aquamoto) reads the
// same grid through the same call, whatever its own resolution or point ordering.
static inline bool haveExternShade(const ExternShade &e) {
	return e.nx > 1 && e.ny > 1 && (int)e.inten.size() == e.nx * e.ny;
}
static inline bool haveExternShade(const Scene *s) {
	return s && haveExternShade(s->shadeIn);
}
// Forget the loaded model. Any Shading-dock control that AIMS or STYLES the light (sun azimuth /
// elevation, the four relief looks) calls this first: asking the dock for a different light means
// the user wants the dock's own light back, so the control must bite instead of sitting inert
// under a baked GMT reflectance (SACRED_LAW: a shared control never becomes a no-op). The caller
// re-runs applyShading itself.
static inline void dropExternShade(Scene *s) {
	if (!s) return;
	s->shadeIn     = ExternShade();
	s->shadeInLand = ExternShade();   // both sides, or the dock would move one image and not the other
	activeLook(s).noShade = false;   // asking for a light ends "Remove illumination" on THAT layer
}
// …the same, for ONE Aquamoto side. An Aquamoto layer's two sides each hold their own reflectance,
// so a method chosen for one side replaces THAT side's model and must leave the other's standing.
// `side`: 1 = LAND (the static bathymetry), anything else = WATER (the live stage). `noShade` is the
// window's, not a side's, so it is cleared here too: asking for a light ends "Remove illumination".
static inline void dropExternShadeSide(Scene *s, int side) {
	if (!s) return;
	((side == 1) ? s->shadeInLand : s->shadeIn) = ExternShade();
	activeLook(s).noShade = false;
}
// Reflectance at TRUE-coord (x,y), or NaN outside the grid / on a NaN node.
// Deliberately NOT longitude-aware: a reflectance arrives already in the frame of the grid it lights
// (hillshade.jl rolls a GMT module's re-wrapped output back before pushing it), so a wrap-around
// retry here would only be a second, sampler-side opinion about the same question — and one that
// changes how EVERY model is sampled, including the ones that were never in the wrong frame.
static inline double externShadeAt(const ExternShade &e, double x, double y) {
	return sampleGrid(e.inten.data(), e.nx, e.ny, e.x0, e.x1, e.y0, e.y1, x, y);
}
static inline double externShadeAt(const Scene *s, double x, double y) {
	return externShadeAt(s->shadeIn, x, y);
}

// EACH LAYER IS LIT BY ITS OWN REFLECTANCE AND BY NOTHING ELSE. `layer` is a Scene Objects name
// ("" = the base surface); the answer is true only when the loaded reflectance was computed FOR that
// layer. Every consumer of the extern shade asks this first, so illuminating one grid can no longer
// change how any other grid in the window is drawn (2026-09-07: reported as layer0's illumination
// changing when another layer arrived). A reflectance with no owner (an older state) belongs to the
// base surface, which is where the single-layer windows that predate this always applied it.
static inline bool externShadeOwns(const Scene *s, const std::string &layer) {
	if (!s || !haveExternShade(s->shadeIn)) return false;
	return s->shadeIn.owner.empty() ? layer.empty() : (s->shadeIn.owner == layer);
}

// The Scene Objects name of the layer an actor belongs to: an extra by its own name, anything else
// (the base surface and every LOD tile of it) the base's. ONE resolver, so "which layer is this
// actor?" has one answer wherever it is asked.
static inline std::string layerNameOfActor(Scene *s, vtkActor *a) {
	const int idx = a ? extraIndexOfActor(s, a) : -1;
	return (idx >= 0) ? s->extras[idx].name : s->surfName;
}

struct ReliefLight {
	double Lx, Ly, Lz;          // sun dir, lit convention (Lambert / PBR key light)
	double LxG, LyG, LzG;       // sun dir, grdimage (inverted elevation)
	double fx, fz, fzRef;       // normal-correction: 1/xfac, 1/(zfacShade·ve), 1/zfacShade (ve = 1)
	double amb, gain, twoOverPi;
	bool   grd;
	double rough, metal, keyI, fillI;   // PBR bake: roughness, metalness, key + fill light intensity
	double baseF0;                      // PBR bake: dielectric reflectance at normal incidence, from the IOR
	double Gx, Gy, Gz, gizI;            // PBR bake: the gizmo's own scene light (gizI 0 = none / switched off)
};

// THE REST OF METHOD 1's LIGHT RIG, read off the live renderer objects so the CPU bake is lit by the
// same lights the VTK render is. The key (sun) and the headlight fill are already in ReliefLight; the
// third light is the gizmo's (20_gizmo.cpp enableGizmo: a directional scene light at 1.4, brighter
// than the sun), which is on whenever the gizmo exists and cast shadows is off. Only READ here.
static void reliefLightRig(Scene *s, double ior, ReliefLight &L) {
	const double r = (ior > 0.0) ? (ior - 1.0) / (ior + 1.0) : 0.2;
	L.baseF0 = r * r;                                        // vtkProperty::ComputeReflectanceFromIOR
	L.Gx = 0.0;  L.Gy = 0.0;  L.Gz = 1.0;  L.gizI = 0.0;
	if (!s || !s->giz || !s->giz->light || !s->giz->light->GetSwitch()) return;
	double p[3], f[3];
	s->giz->light->GetPosition(p);
	s->giz->light->GetFocalPoint(f);
	double d[3] = { p[0] - f[0], p[1] - f[1], p[2] - f[2] };
	const double len = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
	if (!(len > 0.0)) return;
	L.Gx = d[0] / len;  L.Gy = d[1] / len;  L.Gz = d[2] / len;
	L.gizI = s->giz->light->GetIntensity();
}
// `ve` is THE LAYER BEING SHADED's own exaggeration (ExtraObj::ve, or Scene::ve for the base), never
// a window-wide one: a grid drawn at its own VE has to be lit at that same VE or its slopes get
// corrected by a number belonging to a different layer.
// `zfac` is THIS layer's own axis mapping (sceneZRefFor over its own z range) and `ve` its own
// exaggeration, so the normal correction describes the relief exactly AS DRAWN -- which, the VE being
// defined against the displayed dimensions, is also the relief the eye sees. One pair, one layer.
static ReliefLight makeReliefLight(Scene *s, const LayerShade &lk, double zfac, double ve) {
	ReliefLight L;
	const double az  = lk.lightAz * vtkMath::Pi() / 180.0;
	const double el  = lk.lightEl * vtkMath::Pi() / 180.0;
	L.Lx = std::sin(az) * std::cos(el);  L.Ly = std::cos(az) * std::cos(el);  L.Lz = std::sin(el);
	const double elG = (90.0 - lk.lightEl) * vtkMath::Pi() / 180.0;   // grdimage inverts elevation
	L.LxG = std::sin(az) * std::cos(elG); L.LyG = std::cos(az) * std::cos(elG); L.LzG = std::sin(elG);
	L.fx    = (s->xfac != 0.0) ? 1.0 / s->xfac : 1.0;
	L.fz    = (zfac * ve != 0.0) ? 1.0 / (zfac * ve) : 1.0;
	L.fzRef = (zfac != 0.0) ? 1.0 / zfac : 1.0;            // same, at the reference VE (ve = 1)
	L.amb = lk.hillAmbient;  L.gain = lk.hillGain;  L.twoOverPi = 2.0 / vtkMath::Pi();  L.grd = lk.hillGrd;
	L.rough = lk.roughness < 0.05 ? 0.05 : lk.roughness;   // clamp so the GGX lobe stays finite
	L.metal = lk.metallic < 0.0 ? 0.0 : (lk.metallic > 1.0 ? 1.0 : lk.metallic);
	L.keyI = s->lightIntensity;  L.fillI = s->fillIntensity;
	reliefLightRig(s, lk.ior, L);
	return L;
}

// A data-space normal turned into the normal of the relief AS DRAWN. THE one place that conversion
// happens, and every look goes through it — because a relief shade computed from the raw data-space
// normal is UNIT-DEPENDENT: the same terrain in degrees and in metres has wildly different dz/dx, so
// it shaded completely differently (a reprojected grid's land saturated to flat white while the very
// same data in degrees beside it shaded correctly). What the user sees is the drawn surface, so that
// is what gets shaded — and the drawn surface is unit-free by construction (Scene::ve / sceneZRef).
// `withVE` = follow the user's exaggeration slider (Lambert does, by design); false = the reference
// exaggeration, so the look stays VE-independent as documented, without being unit-dependent.
static inline void reliefDrawnNormal(const ReliefLight &L, const double nv[3], bool withVE, double o[3]) {
	o[0] = nv[0] * L.fx;
	o[1] = nv[1];
	o[2] = nv[2] * (withVE ? L.fz : L.fzRef);
	const double len = std::sqrt(o[0]*o[0] + o[1]*o[1] + o[2]*o[2]);
	if (len > 0.0) { o[0] /= len; o[1] /= len; o[2] /= len; }
}

// METHOD 7: method 1's picture computed on the CPU, per flat-image pixel, from the data-space normal
// nv. rgb is 0..1, the albedo in and the shaded colour out.
//
// IT IS VTK 9.6's OWN PBR FRAGMENT SHADER, TERM FOR TERM — not a lookalike Cook-Torrance. The
// sources are vtkOpenGLPolyDataMapper.cxx (ReplaceShaderLight), glsl/vtkPBRFunctions.glsl and
// vtkToneMappingPass.cxx (NeutralPBR). The first version of this bake was a textbook BRDF with its
// own fill and a 0.12 ambient floor, and it differed from the render in every stage, which is why
// it looked nothing like method 1:
//   * ALBEDO. VTK takes the CPT colour as LINEAR light ("VTK colors are expressed in linear color
//     space") and gamma-encodes the result, pow(1/2.2). The old bake stayed in screen values with no
//     encode, so it came out much darker and more saturated than the render.
//   * DIFFUSE is Lambert's albedo/PI, times (1 - F) * (1 - metallic). The old one had no 1/PI.
//   * SPECULAR is GGX D * the height-correlated Smith visibility * Schlick F with VTK's F90 (specular
//     occlusion) and HdL, not V·H and the separable Smith G over 4·NdV·NdL.
//   * THE LIGHTS are method 1's rig as the renderer holds it: the key (sun), the HEADLIGHT fill
//     (L = V: straight down in a top-down map; VTK passes HdL = 1 for it), and the gizmo's own 1.4
//     scene light whenever it is switched on (reliefLightRig). The old bake had a hemispherical fill
//     and an invented ambient floor instead, and no gizmo light, which is the brightest of the three.
//   * THE NORMAL is the relief AS DRAWN, at the window's own VE: the actor is scaled by
//     (xfac, 1, zfac*ve) and VTK's normal matrix carries exactly that onto the mesh normals.
//   * TONE. Method 1 draws through the NeutralPBR tone-mapping pass (on by default; the pass
//     linearises the frame, compresses the highlights with a slight desaturation, and re-encodes),
//     so the same curve is the last stage here. SSAO, FXAA and the IBL sky are screen/environment
//     passes this bake has no scene for; with IBL off (the default) method 1 has no environment term
//     either, so there is nothing to stand in for it.
// A 2-D map is viewed straight down, so V = +Z.
static inline void applyPBRShade(const ReliefLight &L, const double nvRaw[3], double rgb[3]) {
	double N[3];
	reliefDrawnNormal(L, nvRaw, /*withVE=*/true, N);
	auto clampd = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
	const double NdV = clampd(N[2], 1e-5, 1.0);                           // V = +Z
	const double rough = L.rough, met = L.metal;
	const double recPI = 1.0 / vtkMath::Pi();
	double alb[3], F0[3], F90[3];
	for (int i = 0; i < 3; ++i) {
		alb[i] = clampd(rgb[i], 0.0, 1.0);
		F0[i]  = L.baseF0 + (alb[i] - L.baseF0) * met;                    // mix(baseF0, albedo, metallic)
	}
	// Specular occlusion: an F0 under 2% loses its grazing lobe; edgeTint is VTK's default white.
	const double f90 = clampd((F0[0] + F0[1] + F0[2]) * (50.0 * 0.33), 0.0, 1.0);
	for (int i = 0; i < 3; ++i) F90[i] = f90 + (1.0 - f90) * met;
	// D_GGX and V_SmithCorrelated exactly as vtkPBRFunctions.glsl writes them (the latter takes
	// roughness^2 as its a2, which is VTK's convention and is kept so the lobe is VTK's lobe).
	const double a  = rough * rough, a2 = a * a;
	double Lo[3] = { 0.0, 0.0, 0.0 };
	auto addLight = [&](double NdH, double NdL, double HdL, double radiance) {
		if (!(radiance > 0.0)) return;
		const double d    = (NdH * a2 - NdH) * NdH + 1.0;
		const double D    = a2 / (vtkMath::Pi() * d * d);
		const double ggxV = NdL * std::sqrt(a + NdV * (NdV - a * NdV));
		const double ggxL = NdV * std::sqrt(a + NdL * (NdL - a * NdL));
		const double Vis  = 0.5 / (ggxV + ggxL);
		const double fw   = std::pow(1.0 - HdL, 5.0);
		for (int i = 0; i < 3; ++i) {
			const double F    = F0[i] + (F90[i] - F0[i]) * fw;
			const double spec = D * Vis * F;
			const double diff = (1.0 - met) * (1.0 - F) * alb[i] * recPI;
			Lo[i] += radiance * (diff + spec) * NdL;
		}
	};
	auto addDirectional = [&](double lx, double ly, double lz, double radiance) {
		double H[3] = { lx, ly, lz + 1.0 };                                // normalize(V + L), V = +Z
		const double hl = std::sqrt(H[0]*H[0] + H[1]*H[1] + H[2]*H[2]);
		if (hl > 0.0) { H[0] /= hl; H[1] /= hl; H[2] /= hl; }
		const double HdL = clampd(H[0]*lx + H[1]*ly + H[2]*lz, 1e-5, 1.0);
		const double NdL = clampd(N[0]*lx + N[1]*ly + N[2]*lz, 1e-5, 1.0);
		const double NdH = clampd(N[0]*H[0] + N[1]*H[1] + N[2]*H[2], 1e-5, 1.0);
		addLight(NdH, NdL, HdL, radiance);
	};
	addDirectional(L.Lx, L.Ly, L.Lz, L.keyI);                             // the sun
	addDirectional(L.Gx, L.Gy, L.Gz, L.gizI);                             // the gizmo's light (0 = none)
	addLight(NdV, NdV, 1.0, L.fillI);                                     // the headlight: L = H = V
	// NeutralPBR (Khronos PBR Neutral), then the sRGB encode — vtkToneMappingPass verbatim.
	const double startCompression = 0.8 - 0.04, desaturation = 0.15;
	const double x = std::min(Lo[0], std::min(Lo[1], Lo[2]));
	const double offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	double t[3] = { Lo[0] - offset, Lo[1] - offset, Lo[2] - offset };
	const double peak = std::max(t[0], std::max(t[1], t[2]));
	if (peak >= startCompression) {
		const double d = 1.0 - startCompression;
		const double newPeak = 1.0 - d * d / (peak + d - startCompression);
		const double g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
		for (int i = 0; i < 3; ++i) {
			t[i] *= newPeak / peak;
			t[i] = t[i] + (newPeak - t[i]) * g;
		}
	}
	for (int i = 0; i < 3; ++i)
		rgb[i] = std::pow(clampd(t[i], 0.0, 1.0), 1.0 / 2.2);
}
// Modulate rgb (0..1, in/out) by the relief shade for a TRUE-coord surface normal nv.
// `externI`, when given, is a reflectance somebody else already computed for THIS point — the GMT
// grdgradient intensity the Hillshade tool pushes down (Scene::shadeInten, see hillshade.jl). It
// replaces the intensity this function would derive from nv; the modulation is the SAME
// gmtIlluminate() every other path ends in, which is exactly what Mirone's mex_illuminate does.
static inline void applyReliefShade(const ReliefLight &L, const double nv[3], double rgb[3],
                                    const double *externI = nullptr) {
	if (externI) { gmtIlluminate(*externI, rgb); return; }
	double c[3];
	if (!L.grd) {                                             // (A) Lambert, VE-corrected, darken-only
		reliefDrawnNormal(L, nv, /*withVE=*/true, c);
		double sh = c[0]*L.Lx + c[1]*L.Ly + c[2]*L.Lz;
		if (sh < 0.0) sh = 0.0;
		const double I = L.amb + (1.0 - L.amb) * sh;
		rgb[0] = std::min(1.0, rgb[0]*I); rgb[1] = std::min(1.0, rgb[1]*I); rgb[2] = std::min(1.0, rgb[2]*I);
		return;
	}
	reliefDrawnNormal(L, nv, /*withVE=*/false, c);             // (B) grdimage -I, VE-independent
	const double raw   = c[0]*L.LxG + c[1]*L.LyG + c[2]*L.LzG - L.LzG;   // slope-toward-sun; 0 on flat
	const double inten = L.twoOverPi * std::atan(L.gain * raw);
	gmtIlluminate(inten, rgb);
}

// ============================================================================
// DAY / NIGHT — the night side darkened, as a FACTOR on the colour a bake has already decided.
//
// It is a second modulation standing BESIDE the relief shade, never on top of its data: nothing here
// reads or writes Scene::shadeInten, the ExternShade snapshots or any Shading-dock field, so the dock
// keeps sole ownership of what it owns and the two factors simply multiply. Every call is gated on
// `s->dayNight.on`, which is false until a host asks for it — with it off, `applyDayNight` returns
// before touching anything and the bakes are bit-identical to what they were.
//
// The geometry is one dot product: the cosine of the solar zenith angle at (lon, lat), i.e. the sine
// of the sun's elevation there. WHERE THE SUN IS comes from the host (GMT.solar, the one source of
// truth this app has for that); this end only asks "how high is it here".
static inline double dayNightFactor(const Scene *s, double lon, double lat) {
	const Scene::DayNightShade &dn = s->dayNight;
	if (!dn.on) return 1.0;
	const double d2r = vtkMath::Pi() / 180.0;
	const double sinE = std::sin(lat * d2r) * std::sin(dn.sunLat * d2r) +
	                    std::cos(lat * d2r) * std::cos(dn.sunLat * d2r) *
	                    std::cos((lon - dn.sunLon) * d2r);
	const double elev = std::asin(sinE > 1.0 ? 1.0 : (sinE < -1.0 ? -1.0 : sinE)) / d2r;
	// Smoothstep across [-twilight, +twilight] of solar elevation: full day above it, `night` below,
	// and a soft band between — a hard cut reads as a drawn line rather than as dusk.
	const double w = (dn.twilight > 1e-6) ? dn.twilight : 1e-6;
	double t = (elev + w) / (2.0 * w);
	t = (t < 0.0) ? 0.0 : (t > 1.0 ? 1.0 : t);
	t = t * t * (3.0 - 2.0 * t);
	return dn.night + (1.0 - dn.night) * t;
}

// THE one place the factor is applied. `dn >= 1` is the full-daylight / feature-off case and returns
// without doing arithmetic at all, which is what makes "off" cost nothing and change nothing.
static inline void applyDayNight(double dn, double rgb[3]) {
	if (dn >= 1.0) return;
	rgb[0] *= dn;  rgb[1] *= dn;  rgb[2] *= dn;
}

// DAY / NIGHT on a textured IMAGE layer. `applyShading` deliberately leaves image extras alone —
// they are pictures, not shaded surfaces — so the factor cannot reach them through any bake. It
// reaches them here, by rewriting the texture itself. Called for every image extra on every shading
// pass; with day/night off and never used on this layer it returns on the first line.
static void dayNightImageLayer(Scene *s, ExtraObj &ex) {
	if (!ex.isImage || !ex.tex) return;
	if (!s->dayNight.on) {
		// Put the picture back exactly as it arrived — the copy is dropped, so nothing is kept for a
		// layer that is not being darkened.
		if (ex.dnPristine) {
			ex.tex->SetInputData(ex.dnPristine);
			ex.tex->Modified();
			ex.dnPristine = nullptr;
		}
		return;
	}
	if (!ex.dnPristine) {
		ex.dnPristine = vtkImageData::SafeDownCast(ex.tex->GetInput());
		if (!ex.dnPristine) return;
	}
	vtkImageData *src = ex.dnPristine;
	vtkUnsignedCharArray *sa = vtkUnsignedCharArray::SafeDownCast(src->GetPointData()->GetScalars());
	if (!sa) return;
	int dims[3];  src->GetDimensions(dims);
	const int W = dims[0], H = dims[1], nc = sa->GetNumberOfComponents();
	if (W < 1 || H < 1 || nc < 3) return;
	vtkNew<vtkImageData> dst;
	dst->DeepCopy(src);
	vtkUnsignedCharArray *da = vtkUnsignedCharArray::SafeDownCast(dst->GetPointData()->GetScalars());
	if (!da) return;
	// Texel -> true coords across the layer's own footprint. Row 0 is the SOUTH edge: that is the
	// tcoord convention the image plane is built with (by0 at v = 0), so this reads the same way up
	// as the picture is drawn.
	const double dx = (W > 1) ? (ex.bx1 - ex.bx0) / (W - 1) : 0.0;
	const double dy = (H > 1) ? (ex.by1 - ex.by0) / (H - 1) : 0.0;
	vtkSMPTools::For(0, H, [&](int jBeg, int jEnd) {
	for (int j = jBeg; j < jEnd; ++j) {
		const double lat = ex.by0 + j * dy;
		unsigned char *row = da->GetPointer((vtkIdType)j * W * nc);
		for (int i = 0; i < W; ++i) {
			const double f = dayNightFactor(s, ex.bx0 + i * dx, lat);
			if (f >= 1.0) continue;
			unsigned char *p = row + (vtkIdType)i * nc;
			p[0] = (unsigned char)(p[0] * f + 0.5);
			p[1] = (unsigned char)(p[1] * f + 0.5);
			p[2] = (unsigned char)(p[2] * f + 0.5);
		}
	}
	});
	ex.tex->SetInputData(dst);
	ex.tex->Modified();
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
// `baseRGBA` (optional, node-resolution nx*ny*4, row-major row0=south) overrides the CPT lookup: when
// given, each pixel's ALBEDO is the host composite (e.g. Aquamoto's dry/wet blend) instead of CPT(z),
// and everything else — the SAME gradient, normal and applyReliefShade/applyPBRShade — is identical.
// This is how the tsunami shades through the ONE bake function instead of a fork; grids pass nullptr.
static void bakeLayerRGBA(Scene *s, const float *z, int nx, int ny, double gx0, double gy0,
                          double dx, double dy, vtkColorTransferFunction *ctf, double lo, double hi,
                          double wx0, double wx1, double wy0, double wy1,
                          int txW, int txH, std::vector<unsigned char> &out,
                          const unsigned char *baseRGBA = nullptr,
                          int zlayout = 0) {   // z layout: 0 = "BCB" (s->gridZ), !=0 = a caller's "TRB" buffer
	out.assign((size_t)txW * txH * 4, 0);
	if ((!ctf && !baseRGBA) || dx == 0.0 || dy == 0.0) return;
	// discretize the CPT once (skipped when a host composite supplies the albedo)
	const int NT = 1024;
	std::vector<unsigned char> tbl(baseRGBA ? 0 : (size_t)NT * 3);
	if (!baseRGBA) {
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
	const bool   pbr   = !s->look.useHillshade && s->look.litBake;   // flat PBR bake (approximates the lit surface)
	// Hillshade tool: GMT-computed reflectance — consumed only when it is THIS layer's own. This bake
	// paints the BASE surface's drape (its callers pass s->gridZ), so the owner asked about is the base.
	const bool   ext   = externShadeOwns(s, s->surfName);
	const bool   shade = s->look.useHillshade || pbr;           // any per-pixel shade (hillshade or PBR)
	// the BASE relief's own look, own axis mapping (Scene::zfac) and own VE
	const ReliefLight L = makeReliefLight(s, s->look, s->zfac, s->ve);
	const GridLay zlay = gridLay(nx, ny, zlayout);                               // THE layout resolver (10_geometry.cpp)
	auto Zc = [&](int ix, int iy) -> double { return zlay.at(z, ix, iy); };
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
			unsigned char cr, cg, cb;
			if (baseRGBA) {                       // albedo from the host composite (node-aligned, row0=south)
				const size_t bi = ((size_t)iy * nx + ix) * 4;
				cr = baseRGBA[bi]; cg = baseRGBA[bi+1]; cb = baseRGBA[bi+2];
			} else {
				if (std::isnan(zc)) {   // paint NaN with the Preferences NaN fill colour (opaque)
					p[0] = (unsigned char)(s->nanColor[0]*255.0+0.5);
					p[1] = (unsigned char)(s->nanColor[1]*255.0+0.5);
					p[2] = (unsigned char)(s->nanColor[2]*255.0+0.5);
					p[3] = 255; continue;
				}
				int ti = (int)((zc - lo) * invspan); if (ti < 0) ti = 0; else if (ti > NT - 1) ti = NT - 1;
				const unsigned char *rgb8 = &tbl[3 * ti];
				cr = rgb8[0]; cg = rgb8[1]; cb = rgb8[2];
			}
			if (!shade) { p[0] = cr; p[1] = cg; p[2] = cb; p[3] = 255; continue; }
			// Hillshade tool: the reflectance is already known for this world position -> modulate
			// with it and skip the gradient entirely (the SAME gmtIlluminate the normal path ends in).
			if (ext) {
				const double ei = externShadeAt(s, tx, ty);
				if (!std::isnan(ei)) {
					double c[3] = { cr / 255.0, cg / 255.0, cb / 255.0 };
					applyReliefShade(L, nullptr, c, &ei);
					p[0] = (unsigned char)std::min(255.0, c[0] * 255.0 + 0.5);
					p[1] = (unsigned char)std::min(255.0, c[1] * 255.0 + 0.5);
					p[2] = (unsigned char)std::min(255.0, c[2] * 255.0 + 0.5);
					p[3] = 255; continue;
				}
			}
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
			double c[3] = { cr / 255.0, cg / 255.0, cb / 255.0 };
			if (pbr) applyPBRShade(L, nv, c);                    // PBR lit look (no hillshade selected)
			else     applyReliefShade(L, nv, c);                 // SHARED hillshade (grdimage or Lambert), matches the surface
			p[0] = (unsigned char)std::min(255.0, c[0] * 255.0 + 0.5);
			p[1] = (unsigned char)std::min(255.0, c[1] * 255.0 + 0.5);
			p[2] = (unsigned char)std::min(255.0, c[2] * 255.0 + 0.5);
			p[3] = 255;
		}
	}
	});

	// DAY / NIGHT, as its own pass over the finished texture. Deliberately NOT folded into the loop
	// above: that loop has three different exits (no shade, extern reflectance, gradient shade) and a
	// factor applied at three places is three places to get it wrong. One pass, one condition, and it
	// does not run at all unless a host has asked for it — so this cannot alter any existing bake.
	if (s->dayNight.on) {
		const double txdx = (txW > 1) ? (wx1 - wx0) / (txW - 1) : 0.0;
		const double txdy = (txH > 1) ? (wy1 - wy0) / (txH - 1) : 0.0;
		vtkSMPTools::For(0, txH, [&](int jBeg, int jEnd) {
		for (int j = jBeg; j < jEnd; ++j) {
			const double ty = wy0 + j * txdy;
			unsigned char *row = &out[(size_t)j * txW * 4];
			for (int i = 0; i < txW; ++i) {
				unsigned char *p = row + (size_t)i * 4;
				if (p[3] == 0) continue;                    // untouched texel: nothing to darken
				const double f = dayNightFactor(s, wx0 + i * txdx, ty);
				if (f >= 1.0) continue;
				p[0] = (unsigned char)(p[0] * f + 0.5);
				p[1] = (unsigned char)(p[1] * f + 0.5);
				p[2] = (unsigned char)(p[2] * f + 0.5);
			}
		}
		});
	}
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

// Snapshot the live Shading-dock illumination into a per-side struct (one side's OWN light).
static AquaSideShade snapshotShade(Scene *s) {
	AquaSideShade a; a.valid = true;
	a.useHillshade = s->look.useHillshade; a.hillGrd = s->look.hillGrd; a.litBake = s->look.litBake;
	a.lightAz = s->look.lightAz; a.lightEl = s->look.lightEl; a.hillAmbient = s->look.hillAmbient; a.hillGain = s->look.hillGain;
	a.roughness = s->look.roughness; a.metallic = s->look.metallic;
	a.lightIntensity = s->lightIntensity; a.fillIntensity = s->fillIntensity;
	return a;
}
// makeReliefLight, but with the light/style taken from a per-side snapshot (geometry xfac/zfac/ve still
// live from the Scene). Lets WATER and LAND shade with independent suns through the SAME applyReliefShade.
static ReliefLight makeReliefLightSide(Scene *s, const AquaSideShade &a) {
	const double ve = s->ve;                      // the tank IS the base relief -- its own VE and look
	const LayerShade &lk = s->look;
	ReliefLight L;
	const double az  = a.lightAz * vtkMath::Pi() / 180.0;
	const double el  = a.lightEl * vtkMath::Pi() / 180.0;
	L.Lx = std::sin(az) * std::cos(el);  L.Ly = std::cos(az) * std::cos(el);  L.Lz = std::sin(el);
	const double elG = (90.0 - a.lightEl) * vtkMath::Pi() / 180.0;
	L.LxG = std::sin(az) * std::cos(elG); L.LyG = std::cos(az) * std::cos(elG); L.LzG = std::sin(elG);
	L.fx    = (s->xfac != 0.0) ? 1.0 / s->xfac : 1.0;
	// The LIGHTING reference, like every other shade path (makeReliefLight) — never the geometry's
	// horizontal-scale normaliser, or the tank's water and land both wash out to flat colour.
	const double zsh = s->zfac;
	L.fz    = (zsh * ve != 0.0) ? 1.0 / (zsh * ve) : 1.0;
	L.fzRef = (zsh != 0.0) ? 1.0 / zsh : 1.0;              // same, at the reference VE (ve = 1)
	L.amb = a.hillAmbient;  L.gain = a.hillGain;  L.twoOverPi = 2.0 / vtkMath::Pi();  L.grd = a.hillGrd;
	L.rough = a.roughness < 0.05 ? 0.05 : a.roughness;
	L.metal = a.metallic < 0.0 ? 0.0 : (a.metallic > 1.0 ? 1.0 : a.metallic);
	L.keyI = a.lightIntensity;  L.fillI = a.fillIntensity;
	reliefLightRig(s, lk.ior, L);
	return L;
}

// Aquamoto hillshade: re-light the host-composited tsunami texture through the SAME illumination the
// whole app uses (applyReliefShade / applyPBRShade — ONE shading source of truth; never fork). The
// colour is the host's dry/wet composite (aquaBaseRGBA, unshaded). WATER and LAND are TWO SEPARATE
// images with INDEPENDENT lights: water pixels shade from the per-slice stage (s->gridZ) with the
// WATER snapshot, land pixels from the static bathymetry (s->aquaBathyZ) with the LAND snapshot.
// Because each side re-bakes from its OWN snapshot, editing one side (only its snapshot changes, see
// rebakeLayerImage) leaves the OTHER side pixel-identical — no colour, no light of the other touched.
// IGMT_TRACE_AQUA, read the live process block. Not getenv(): on Windows the CRT keeps its own
// snapshot taken at start-up, so a variable the host sets afterwards (Julia's ENV[...], which calls
// SetEnvironmentVariableW) never appears in it — the same trap 70_window.cpp's envFlag documents.
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall
GetEnvironmentVariableA(const char *name, char *buf, unsigned long size);
static bool aquaTraceOn() { char b[8]; return GetEnvironmentVariableA("IGMT_TRACE_AQUA", b, sizeof b) > 0; }
#else
static bool aquaTraceOn() { const char *v = std::getenv("IGMT_TRACE_AQUA"); return v && *v; }
#endif

// DAY / NIGHT on the Aquamoto composite. The tsunami layer is a host-composited TEXTURE — no CPT,
// no node bake — so the night factor reaches it HERE, in the same bake that decides its pixels, and
// nowhere else (`dayNightNodeBake` keeps the per-node path off this quad; a bake there would tint
// the whole picture with one flat CPT colour). Applied AFTER the two-sided relief shade, as the
// second factor it is: land and water are darkened alike because the terminator is not a property
// of the surface, and each side keeps the light its own snapshot gave it.
// Texel (r = south..north, col) -> true coords, the SAME mapping the shading loop below uses.
static void dayNightAquaPass(Scene *s, unsigned char *out, int nx, int ny) {
	if (!s->dayNight.on) return;
	const double dx = s->gdx != 0.0 ? s->gdx : 1.0, dy = s->gdy != 0.0 ? s->gdy : 1.0;
	vtkSMPTools::For(0, ny, [&](vtkIdType rBeg, vtkIdType rEnd) {
	for (int r = (int)rBeg; r < (int)rEnd; ++r) {
		const double lat = s->gy0 + r * dy;
		for (int col = 0; col < nx; ++col) {
			const double f = dayNightFactor(s, s->gx0 + col * dx, lat);
			if (f >= 1.0) continue;
			unsigned char *p = out + ((size_t)r * nx + col) * 4;
			p[0] = (unsigned char)(p[0] * f + 0.5);
			p[1] = (unsigned char)(p[1] * f + 0.5);
			p[2] = (unsigned char)(p[2] * f + 0.5);
		}
	}
	});
}

static void bakeAquaShade(Scene *s) {
	if (!s || !s->layerImgMode || !s->customLayerTexture || !s->drape) return;
	const int nx = s->gnx, ny = s->gny;
	if (nx < 2 || ny < 2) return;
	if ((int)s->aquaBaseRGBA.size() != nx * ny * 4) return;   // no base composite -> nothing to shade
	const bool haveStage = (int)s->gridZ.size()    == nx * ny;
	const bool haveBathy = (int)s->aquaBathyZ.size() == nx * ny;
	vtkTexture   *tx = s->drape->GetTexture();
	vtkImageData *id = tx ? vtkImageData::SafeDownCast(tx->GetInput()) : nullptr;
	if (!id) return;
	int dims[3] = { 0, 0, 0 }; id->GetDimensions(dims);
	if (dims[0] != nx || dims[1] != ny) return;
	unsigned char *out = static_cast<unsigned char*>(id->GetScalarPointer());
	const unsigned char *base = s->aquaBaseRGBA.data();

	// Each side uses its OWN light snapshot (fall back to the live dock only the first time, before either
	// side has ever been set). Water shades from the stage, land from the bathymetry — fully independent.
	const AquaSideShade wS = s->aquaWaterShade.valid ? s->aquaWaterShade : snapshotShade(s);
	const AquaSideShade lS = s->aquaLandShade.valid  ? s->aquaLandShade  : snapshotShade(s);
	const ReliefLight Lw = makeReliefLightSide(s, wS);
	const ReliefLight Ll = makeReliefLightSide(s, lS);
	// Hillshade tool: a GMT-computed reflectance, ONE PER SIDE. Water's was computed from the live
	// stage and land's from the static bathymetry, exactly the two surfaces this function shades from
	// below -- so the tool splits dry from wet the same way the composite and the dock already do.
	const bool wExt = haveExternShade(s->shadeIn), lExt = haveExternShade(s->shadeInLand);
	const bool wPbr = !wS.useHillshade && wS.litBake, wShade = ((wS.useHillshade || wPbr) && haveStage) || wExt;
	const bool lPbr = !lS.useHillshade && lS.litBake, lShade = (lS.useHillshade || lPbr) || lExt;
	if (!wShade && !lShade) {                            // neither side shades -> the composite verbatim
		memcpy(out, base, (size_t)nx * ny * 4);
		dayNightAquaPass(s, out, nx, ny);                // …still night where it is night
		id->Modified(); tx->Modified();
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return;
	}
	const float *stage = haveStage ? s->gridZ.data() : nullptr;
	const float *bathy = haveBathy ? s->aquaBathyZ.data() : nullptr;
	const bool haveMask = ((int)s->aquaLandMask.size() == nx * ny);
	const unsigned char *mask = haveMask ? s->aquaLandMask.data() : nullptr;
	// TRACE (IGMT_TRACE_AQUA): what actually decides each pixel's side and light. Set the variable and
	// the numbers say whether a land pixel is being SEEN as land, and whether the colour it starts
	// from is already the water CPT's red or is turned red by the shading.
	if (aquaTraceOn()) {
		long nland = 0;
		if (mask) for (long i = 0; i < (long)nx * ny; ++i) nland += (mask[i] != 0);
		long li = -1;
		if (mask) for (long i = 0; i < (long)nx * ny; ++i) if (mask[i] != 0) { li = i; break; }
		fprintf(stdout, "[aqua] %dx%d stage=%d bathy=%d mask=%d landpx=%ld | water(hill=%d pbr=%d ext=%d shade=%d)"
		                " land(hill=%d pbr=%d ext=%d shade=%d)",
		        nx, ny, (int)haveStage, (int)haveBathy, (int)haveMask, nland,
		        (int)wS.useHillshade, (int)wPbr, (int)wExt, (int)wShade,
		        (int)lS.useHillshade, (int)lPbr, (int)lExt, (int)lShade);
		if (li >= 0) fprintf(stdout, " | first land texel base RGB = %3d,%3d,%3d",
		                     base[(size_t)li * 4], base[(size_t)li * 4 + 1], base[(size_t)li * 4 + 2]);
		fprintf(stdout, "\n");
		fflush(stdout);
	}
	const double dx = s->gdx != 0.0 ? s->gdx : 1.0, dy = s->gdy != 0.0 ? s->gdy : 1.0;
	auto at = [](const float *z, int ix, int iy, int gny) -> double { return z[(size_t)ix * gny + iy]; };
	// Per-row parallel: texel (row r = south..north, col) <-> grid (ix=col, iy=r); z is column-major
	// z[ix*ny+iy] (same layout as gridZ), the composite RGBA is row-major row0=south (aqua_pack_rgba).
	vtkSMPTools::For(0, ny, [&](vtkIdType rBeg, vtkIdType rEnd) {
	for (int r = (int)rBeg; r < (int)rEnd; ++r) {
		const int iy = r;
		const int iym = iy > 0 ? iy - 1 : iy, iyp = iy < ny - 1 ? iy + 1 : iy;
		for (int col = 0; col < nx; ++col) {
			const int ix = col;
			const size_t t = ((size_t)r * nx + col) * 4;
			const double sz = stage ? at(stage, ix, iy, ny) : std::numeric_limits<double>::quiet_NaN();
			// LAND or WATER: read off the host's mask, the SAME one that decided how this very pixel was
			// coloured (`_aqua_indland`, pushed with the composite). Never re-derived here — that was the
			// second implementation of one operation, and a disagreement between the two lit land pixels
			// with the water light and wiped out the dry/wet split. `bathy` is still required, since the
			// land side shades FROM it.
			const bool land = bathy && haveMask && mask[(size_t)r * nx + col] != 0;
			const bool shadeThis = land ? lShade : wShade;
			if (!shadeThis) {                            // this side's light is off -> its colour verbatim
				out[t] = base[t]; out[t+1] = base[t+1]; out[t+2] = base[t+2]; out[t+3] = base[t+3];
				continue;
			}
			const ReliefLight &L = land ? Ll : Lw;
			// Hillshade tool: an externally computed reflectance covers EVERY element type, the
			// tsunami composite included (SACRED_LAW: no element type opts out of a shared operation).
			// THIS SIDE's reflectance, never the other's: a single grid smeared over both lit the sea
			// with the land's relief (and the reverse), which is the dry/wet split vanishing again.
			if (land ? lExt : wExt) {
				const double ei = externShadeAt(land ? s->shadeInLand : s->shadeIn,
				                                s->gx0 + ix * dx, s->gy0 + iy * dy);
				if (!std::isnan(ei)) {
					double c[3] = { base[t] / 255.0, base[t+1] / 255.0, base[t+2] / 255.0 };
					applyReliefShade(L, nullptr, c, &ei);
					out[t]   = (unsigned char)std::min(255.0, c[0] * 255.0 + 0.5);
					out[t+1] = (unsigned char)std::min(255.0, c[1] * 255.0 + 0.5);
					out[t+2] = (unsigned char)std::min(255.0, c[2] * 255.0 + 0.5);
					out[t+3] = base[t+3];
					continue;
				}
			}
			const bool pbr = land ? lPbr : wPbr;
			const float *z = land ? bathy : stage;
			const int ixm = ix > 0 ? ix - 1 : ix, ixp = ix < nx - 1 ? ix + 1 : ix;
			const double za = at(z, ixp, iy, ny), zb = at(z, ixm, iy, ny);
			const double zu = at(z, ix, iyp, ny), zd = at(z, ix, iym, ny);
			const double dzdx = (ixp == ixm || std::isnan(za) || std::isnan(zb)) ? 0.0 : (za - zb) / ((ixp - ixm) * dx);
			const double dzdy = (iyp == iym || std::isnan(zu) || std::isnan(zd)) ? 0.0 : (zu - zd) / ((iyp - iym) * dy);
			double n0 = -dzdx, n1 = -dzdy, n2 = 1.0;
			const double len = std::sqrt(n0*n0 + n1*n1 + n2*n2);
			if (len > 0.0) { n0 /= len; n1 /= len; n2 /= len; }
			const double nv[3] = { n0, n1, n2 };
			double c[3] = { base[t] / 255.0, base[t+1] / 255.0, base[t+2] / 255.0 };
			if (pbr) applyPBRShade(L, nv, c);            // SAME PBR bake as the flat CPT image
			else     applyReliefShade(L, nv, c);         // SAME grdimage/Lambert shade as every surface
			out[t]   = (unsigned char)std::min(255.0, c[0] * 255.0 + 0.5);
			out[t+1] = (unsigned char)std::min(255.0, c[1] * 255.0 + 0.5);
			out[t+2] = (unsigned char)std::min(255.0, c[2] * 255.0 + 0.5);
			out[t+3] = base[t+3];
		}
	}
	});
	dayNightAquaPass(s, out, nx, ny);                    // the second factor, over the finished composite
	id->Modified(); tx->Modified();
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

static void rebakeLayerImage(Scene *s) {
	if (!s || !s->layerImgMode || !s->drape || s->gridZ.empty() || s->gnx < 2 || s->gny < 2) return;
	if (s->customLayerTexture) {   // Aquamoto: shade via the SHARED engine, ONE SIDE AT A TIME
		// A BAKE IS NOT AN EDIT. It re-draws what the two sides already say; it may not WRITE either
		// side's light. This used to snapshot the live look into whichever side the Shade Water/Land
		// radio happened to select, at EVERY bake -- and a bake runs for reasons that have nothing to
		// do with editing a side: every water re-light (one per timestep), every reflectance push,
		// every applyShading. So lighting the WATER re-stamped the LAND's snapshot with the live look
		// whenever Land was the selected radio, and that is exactly "changing the water changed the
		// land". WATER AND LAND ARE INDEPENDENT, FULL STOP.
		//
		// A side's light is now written ONLY by the act that aims that side: sceneSetReliefLookAquaSide
		// (the two Illumination buttons beside Shade Water / Shade Land) and sceneSetReliefLook (the
		// window-wide pick, which is the whole window's choice and says so). One writer per fact.
		bakeAquaShade(s);
		return;
	}
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
	if (s->customLayerTexture) return;   // host-composited texture (Aquamoto) -- no CPT to bake a hi-res tile from
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
	const LayerShade &lkA = activeLook(s);        // the dock shows the ACTIVE layer's own look
	s->cbHillL->setChecked(lkA.useHillshade && !lkA.hillGrd);
	s->cbHillG->setChecked(lkA.useHillshade &&  lkA.hillGrd);
	// PBR = the lit look with no hillshade / shadows. On a 3-D surface that IS the default (all off);
	// on a flat image it means the PBR bake is on (litBake). Reflect both so the box tracks reality.
	s->cbPBR->setChecked(!lkA.useHillshade && !s->useShadows && (s->layerImgMode ? lkA.litBake : true));
	if (s->syncFlatEnable) s->syncFlatEnable();   // grey the flat-dead controls when a layer enters image mode
}

// Baked hillshade for ONE surface mapper (single actor or a LOD tile). Two ALTERNATIVE styles,
// selected by s->look.hillGrd; both bake a per-point RGB "hillshade" field (active z scalars untouched,
// so the colour bar still maps z) and the caller renders the surface UNLIT. When off: revert to
// live CPT scalar colouring.
//
//  (A) s->look.hillGrd == false — LAMBERT (the original look): per-node colour = CPT(z) * Lambert shade.
//      The mesh normal is VE-CORRECTED to the displayed relief — normalize(n.x/xfac, n.y,
//      n.z/(zfac*ve)) — dotted with the sun, with a hillAmbient floor so valleys aren't pure black.
//      Darken-only (multiply). Shade tracks the on-screen exaggeration.
//
//  (B) s->look.hillGrd == true  — GMT grdimage from the z-GRADIENT (VE-independent): the baked normal is
//      already n = normalize(-dz/dx, -dz/dy, 1) in TRUE DATA units, so it IS the z-gradient. With
//      sun L (az from north CW, el above horizon) the Lambertian reflectance n.L is recentred to a
//      signed relief signal  raw = 2*(n.L) - 1  (so a sun OVERHEAD, el=90, lights flat ground
//      brightest and grazing sun darkens — el behaves like a real sun), soft-clipped to (-1,1) by
//      an atan (grdgradient -Nt style, amp = s->look.hillGain), then gmt_illuminate() blends it into the
//      CPT colour the way grdimage -I does (lightens AND darkens, hue preserved).
// THE LAND BAR PAINTS THE LAND — ON THE 3-D SURFACE TOO.
//
// An Aquamoto layer is not one quantity with one colour scale. Its WET nodes are water heights and
// belong to the water bar; its DRY nodes are ground elevations and belong to the LAND bar
// (`gmtvtk_aqua_set_land_cpt_h` -> `Scene::aquaLandLut`), and the dry/wet line is the host's own mask,
// the SAME one the flat composite was painted with and `bakeAquaShade` splits its two lights by
// (`Scene::aquaLandMask`). The flat 2-D image has always been built that way host-side. The 3-D
// surface was not: it mapped the WHOLE stage through the water CPT alone, so every dry node came out
// at the top of :polar — a solid red continent standing over the sea, with the land colorbar hanging
// beside it describing colours that were nowhere on screen.
//
// So the node's colour is looked up in the node's OWN bar, at the node's OWN quantity: the stage z
// through the water LUT for water, the BATHYMETRY (`Scene::aquaBathyZ`, the land side's surface — and
// on a dry node the stage equals it) through the land LUT for land. Nothing else changes: the shade
// that multiplies these colours, the normals it uses, and the light it uses are the existing ones.
//
// Returns the per-point land colours (RGBA tuples, same indexing as the caller's `mapped`) and fills
// `isLand` with one flag per point, or nullptr when this is not an Aquamoto layer — in which case
// there is one bar, and the caller's own lookup is the whole answer.
static vtkSmartPointer<vtkUnsignedCharArray> aquaLandColors(Scene *s, vtkPolyData *pd,
                                                            std::vector<unsigned char> &isLand) {
	isLand.clear();
	if (!s || !pd || !s->aquaLandLut) return nullptr;
	const int nx = s->gnx, ny = s->gny;
	if (nx < 2 || ny < 2) return nullptr;
	if ((int)s->aquaLandMask.size() != nx * ny || (int)s->aquaBathyZ.size() != nx * ny) return nullptr;
	vtkPoints *pts = pd->GetPoints();
	if (!pts) return nullptr;
	const vtkIdType n = pd->GetNumberOfPoints();
	if (n < 1) return nullptr;
	const double dx = s->gdx != 0.0 ? s->gdx : 1.0, dy = s->gdy != 0.0 ? s->gdy : 1.0;
	isLand.assign((size_t)n, 0);
	// The land quantity, per point: the bathymetry under it. A wet point is given the same value as its
	// stage lookup would use -- it is never read back (isLand decides), it only keeps the array dense.
	vtkNew<vtkFloatArray> lz;
	lz->SetNumberOfComponents(1);
	lz->SetNumberOfTuples(n);
	for (vtkIdType i = 0; i < n; ++i) {
		double p[3]; pts->GetPoint(i, p);
		int ix = (int)std::lround((p[0] - s->gx0) / dx);
		int iy = (int)std::lround((p[1] - s->gy0) / dy);
		ix = std::min(std::max(ix, 0), nx - 1);
		iy = std::min(std::max(iy, 0), ny - 1);
		const bool land = s->aquaLandMask[(size_t)iy * nx + ix] != 0;   // mask: row-major, row 0 = south
		isLand[(size_t)i] = land ? 1 : 0;
		lz->SetTypedComponent(i, 0, s->aquaBathyZ[(size_t)ix * ny + iy]);   // bathy: column-major, like gridZ
	}
	// NO "this piece has no land, so skip it". The layer decides, never the piece: the base surface is a
	// pyramid of LOD TILES, and a tile that happens to hold only sea would have taken a different
	// colouring path from its neighbours -- the caller's plain-LUT branch instead of the baked one. That
	// is one layer painted two ways, and it showed as exactly what it is: a rectangle of visibly
	// different water, tile-shaped, hanging in the middle of the map (the SW quadrant of tsu_time.nc,
	// the one quadrant with no coast in it). Every tile of an Aquamoto layer goes through here; the
	// all-water ones simply come back with `isLand` all zeros and are painted by the water bar alone.
	// MapScalars is the thread-safe, vectorised lookup (MapValue is not) -- the same call the water side
	// above is served by, so both bars are read exactly one way.
	// …AND IT MUST BE BUILT FIRST. `makeGridCTF` returns a vtkDiscretizableColorTransferFunction, which
	// maps an ARRAY through an internal 1024-entry table; that table is filled by Build(). The water LUT
	// is a mapper's LUT, so the render pass builds it and the call below just works. This one belongs to
	// no mapper — it only ever paints the land colorbar — so unbuilt it mapped EVERY node to black, land
	// coming out a black continent instead of the geo ramp the bar beside it was showing.
	s->aquaLandLut->Build();
	vtkSmartPointer<vtkUnsignedCharArray> out = vtkSmartPointer<vtkUnsignedCharArray>::Take(
		s->aquaLandLut->MapScalars(lz, VTK_COLOR_MODE_MAP_SCALARS, 0));
	if (aquaTraceOn()) {
		long nl = 0; for (vtkIdType i = 0; i < n; ++i) nl += isLand[(size_t)i];
		double lo = 1e30, hi = -1e30;
		for (vtkIdType i = 0; i < n; ++i) {
			if (!isLand[(size_t)i]) continue;
			const double v = lz->GetTypedComponent(i, 0);
			if (v < lo) lo = v;  if (v > hi) hi = v;
		}
		vtkIdType li = -1; for (vtkIdType i = 0; i < n; ++i) if (isLand[(size_t)i]) { li = i; break; }
		fprintf(stdout, "[aqua-land] pts=%lld land=%ld bathy(land) %.2f..%.2f | mapped=%d comps=%d",
		        (long long)n, nl, lo, hi, (int)(out != nullptr), out ? out->GetNumberOfComponents() : 0);
		if (out && li >= 0) {
			const unsigned char *p = out->GetPointer(li * out->GetNumberOfComponents());
			fprintf(stdout, " | first land RGB = %3d,%3d,%3d at z=%.2f", p[0], p[1], p[2],
			        lz->GetTypedComponent(li, 0));
		}
		fprintf(stdout, "\n"); fflush(stdout);
	}
	return out;
}

// MAY day/night bake per-NODE colours onto this actor? ONLY when the actor's colour really is the
// CPT's — i.e. when a node bake is what decides the pixel. Everything else wears a picture or its
// own colours, and writing an RGB array onto it does not darken it, it REPLACES what it looks like:
//   * a TEXTURED actor (a drape, and the flat quad a grid-shown-as-an-image rides on) has VTK
//     MODULATE the texture by the vertex colour, so a bake tints the picture. The Aquamoto quad is
//     the worst case: its geometry is a flat z=0 rectangle, so every node maps to the SAME CPT
//     colour and the whole tsunami composite — land included — came out washed in that one colour.
//     Those layers are darkened in their OWN bake (bakeLayerRGBA / bakeAquaShade / dayNightImageLayer),
//     which is where their pixels are decided.
//   * a mesh with its own colours (per-vertex/per-face RGB, or per-face z) would have them pushed
//     through the LUT by MAGNITUDE — the flat-red failure `fvRestoreColorMode` exists to stop.
// That last question is asked of `fvMeshColorMode`, the ONE function that answers "what colouring
// does this geometry really have" (10_geometry.cpp) — never of the mapper's live colour mode, which
// this very function sets to DirectScalars on every successful bake and would therefore stop
// recognising its own grids from the second pass on.
// This is a QUESTION, not a change: it only ever gates the day/night-only path, so with the feature
// off nothing here can run at all.
static bool dayNightNodeBake(Scene *s, vtkActor *act, vtkPolyDataMapper *m) {
	if (!act || !m) return false;
	if (act->GetTexture()) return false;                       // a picture rides on this actor
	if (s->layerImgMode) {                                      // flat-image quad (cube layer / Aquamoto):
		for (vtkActor *a : surfActors(s)) if (a == act) return false;   // its pixels are the drape texture
	}
	bool fRGB = false, vRGB = false, cZ = false;
	if (fvMeshColorMode(m, fRGB, vRGB, cZ)) return false;      // mesh's own colours, not LUT values
	return true;
}

static void hillshadeMapper(Scene *s, vtkActor *act) {
	if (!act) return;
	const LayerShade &lk = lookOfActor(s, act);   // THIS layer's own look, never the window's
	vtkPolyDataMapper *m = vtkPolyDataMapper::SafeDownCast(act->GetMapper());
	if (!m) return;

	// PULL THE PIPELINE FIRST. A mapper wired with SetInputConnection (a grid added as an EXTRA goes
	// through a vtkPolyDataNormals filter) has no computed output until something asks for it — so
	// `GetInput()` hands back an empty polydata with no normals, and the `return` below then skipped
	// the bake ENTIRELY for that layer. It stayed lit by raw PBR + the sky IBL instead, which blows
	// steep ground out to flat grey-white while the base surface beside it is correctly shaded: the
	// same operation producing a different result for one kind of layer, i.e. the violation. Never
	// let this function no-op its way out of shading something.
	// …AND PULL IT FROM THE RIGHT END OF THE PIPELINE. On a globe or a cube, globeAttachActor splices
	// a vtkTransformPolyDataFilter BETWEEN the actor's polydata and its mapper, so `m->GetInput()`
	// stops being lon/lat/z and becomes WORLD XYZ on the body. Everything below then went wrong at
	// once, and BAKED the result into the vertex colours, which is why no change to the scene lights
	// could move it: the normals handed to applyReliefShade were world normals being VE-corrected as
	// if they were flat-map ones, so every cube face got a different, wrong illumination and any face
	// whose world normal opposed the sun baked to solid BLACK; and the extern-shade sampler below
	// read those world XYZ as if they were true x/y, landing in the wrong part of the reflectance
	// grid and clamping off its edge — the straight-edged rectangle across a face.
	// The bake belongs on the SOURCE geometry, in true coords, exactly as on the flat map: one bake,
	// same inputs, whatever body the window is wearing (SACRED_LAW.md). The transform filter carries
	// the colour array through to the mapper on its own, so nothing downstream needs to know.
	vtkPolyData *pd = nullptr;
	auto hk = s->globeHooks.find(act);
	if (hk != s->globeHooks.end() && hk->second.filt) {
		if (vtkAlgorithm *up = hk->second.filt->GetInputAlgorithm()) up->Update();
		pd = vtkPolyData::SafeDownCast(hk->second.filt->GetInput());
	}
	if (!pd) {
		if (m->GetNumberOfInputConnections(0) > 0) m->Update();
		pd = vtkPolyData::SafeDownCast(m->GetInput());
	}
	// THE SECOND BAR, if this layer has one (an Aquamoto land/water surface). It is asked for BEFORE
	// the hillshade question, because it is not a light: a tsunami's dry nodes must wear the land bar's
	// colours whether or not the window is currently hillshading. Nothing else in this app gets a
	// non-null here.
	std::vector<unsigned char> isLand;
	vtkSmartPointer<vtkUnsignedCharArray> landCol = pd ? aquaLandColors(s, pd, isLand) : nullptr;

	// DAY / NIGHT keeps the bake alive when no relief shade is selected: the night side still has to
	// be darkened, and the only place a surface's colour can be modulated per NODE is here. The whole
	// condition is false unless a host has asked for day/night, so with the feature off every line
	// below behaves exactly as it always has.
	//
	// ...and ONLY for an actor whose colour really is the LUT's. `dayNightNodeBake` is what says so.
	const bool dnOn = s->dayNight.on && dayNightNodeBake(s, act, m);
	if (!lk.useHillshade && !landCol && !dnOn) {  // revert to whatever this geometry's colouring IS
		// NOT hard-coded to "point data, through the LUT" any more. That is right for a grid and
		// wrong for every MESH: a per-vertex RGB array pushed through a LUT is mapped by its
		// MAGNITUDE, so a magenta model rendered as one flat red off the top of the ramp, and a
		// solid's per-FACE colours were reverted onto point data they do not live on. The mesh doors
		// set an explicit mode and this ran after them, so it silently won.
		fvRestoreColorMode(m);                    // 10_geometry.cpp — one decision, every actor
		return;
	}
	if (!pd) return;
	vtkDataArray *nrm = pd->GetPointData()->GetNormals();
	vtkDataArray *zs  = pd->GetPointData()->GetScalars();
	// Prefer the actor's OWN mapper LUT (a dropped grid extra carries its own CPT); fall back to the
	// scene's primary LUT. For the primary surface/tiles these are the same object, so this is a no-op
	// there but lets an extra grid hillshade with its true colours instead of the canvas LUT.
	vtkScalarsToColors *lut = m->GetLookupTable() ? m->GetLookupTable() : (s->surfLut ? s->surfLut.Get() : nullptr);
	if (!zs || !lut)   return;                    // no scalars/LUT -> leave as-is
	// …normals only matter to the shade itself — and an Aquamoto surface shades from its two sides'
	// own lights even when the window look is off, so it needs them too.
	if (!nrm && (lk.useHillshade || landCol)) return;

	double lzf = 1.0, lve = 1.0;
	layerZOf(s, act, lzf, lve);                    // THIS actor's own normaliser + VE (VE is what we want)
	// ...and THIS actor's own LIGHTING reference, from its own z range, in place of the geometry
	// normaliser: see makeReliefLight. Each layer lights like terrain from its own numbers.
	const ReliefLight L = makeReliefLight(s, lookOfActor(s, act), lzf, lve);

	const vtkIdType n = pd->GetNumberOfPoints();
	// Map EVERY z to its CPT colour in one serial batch: vtkScalarsToColors::MapValue is NOT
	// thread-safe (returns a pointer into a shared internal buffer), so it can't be called from the
	// parallel loop. MapScalars is a vectorised, thread-safe alternative that returns an owned RGBA
	// array; the parallel loop then only does the (stateless) shade maths from those bytes + normals.
	vtkSmartPointer<vtkUnsignedCharArray> mapped =
		vtkSmartPointer<vtkUnsignedCharArray>::Take(lut->MapScalars(zs, VTK_COLOR_MODE_MAP_SCALARS, 0));
	if (!mapped) return;
	const int mc = mapped->GetNumberOfComponents();      // RGBA = 4
	// The land bar's own lookup, same shape, same indexing (see aquaLandColors). Dropped if it did not
	// come back with one tuple per point -- a colour taken from the wrong index is worse than one bar.
	const bool twoBar = landCol && landCol->GetNumberOfTuples() == pd->GetNumberOfPoints() &&
	                    isLand.size() == (size_t)pd->GetNumberOfPoints();
	const int lc = twoBar ? landCol->GetNumberOfComponents() : 0;
	// Hillshade tool: an externally computed reflectance, sampled at each point's TRUE-coord (x,y).
	// The polydata carries true data coords (the actor holds xfac/ve), so this works for the single
	// surface and for every LOD tile without either of them knowing its own grid index range.
	// …and ONLY if that reflectance was computed for THIS actor's layer. Any other layer's light is
	// not this layer's business (see externShadeOwns).
	const bool ext = externShadeOwns(s, layerNameOfActor(s, act));
	// AN AQUAMOTO SURFACE HAS TWO SIDES HERE TOO. `twoBar` already says, per node, which side a point
	// belongs to -- it is how the two colour bars are applied -- so the LIGHT follows the same split:
	// water nodes from the WATER snapshot and the water reflectance, land nodes from the LAND ones.
	// Without this the 3-D surface had a SINGLE light for the whole tank (s->shadeIn + the window
	// look), which is the flat-image path's two-surface law broken on the other geometry: choosing a
	// method for the water lit the land with it, and the reverse. Non-Aquamoto actors never get a
	// `landCol`, so `aqua` is false for them and every line below behaves exactly as before.
	const bool aqua = twoBar;
	const AquaSideShade wS = s->aquaWaterShade.valid ? s->aquaWaterShade : snapshotShade(s);
	const AquaSideShade lS = s->aquaLandShade.valid  ? s->aquaLandShade  : snapshotShade(s);
	const ReliefLight Lw = aqua ? makeReliefLightSide(s, wS) : L;   // SAME per-side light builder the
	const ReliefLight Ll = aqua ? makeReliefLightSide(s, lS) : L;   // composited path uses -- never a copy
	const bool wExt = aqua && haveExternShade(s->shadeIn);
	const bool lExt = aqua && haveExternShade(s->shadeInLand);
	// The points are needed by the extern reflectance samplers AND by day/night, which asks each node
	// where it is on the Earth. Same array, one reason more to fetch it.
	vtkPoints *pts = (ext || wExt || lExt || dnOn) ? pd->GetPoints() : nullptr;
	if ((ext || wExt || lExt || dnOn) && !pts) return;
	vtkSmartPointer<vtkUnsignedCharArray> col = vtkSmartPointer<vtkUnsignedCharArray>::New();
	col->SetName("hillshade");
	col->SetNumberOfComponents(3);
	col->SetNumberOfTuples(n);
	// Per-point parallel: disjoint output tuples, all reads (mapped colours, normals, light L) shared
	// read-only. GetTuple(i, buf) writes the caller's own buffer, so it is safe under concurrent reads.
	vtkSMPTools::For(0, n, [&](vtkIdType iBeg, vtkIdType iEnd) {
	for (vtkIdType i = iBeg; i < iEnd; ++i) {
		double nv[3] = { 0.0, 0.0, 1.0 };
		if (nrm) nrm->GetTuple(i, nv);
		// EACH NODE FROM ITS OWN BAR: a dry node's ground elevation through the LAND lut, a wet node's
		// stage through the water one. One bar -> `twoBar` is false and this is the lookup it always was.
		const bool land = twoBar && isLand[(size_t)i] != 0;
		const unsigned char *rgb8 = land ? landCol->GetPointer(i * lc)    // land bar, at the bathymetry
		                                 : mapped->GetPointer(i * mc);    // water bar, at the stage z
		double c[3] = { rgb8[0] / 255.0, rgb8[1] / 255.0, rgb8[2] / 255.0 };
		double ei = std::numeric_limits<double>::quiet_NaN();
		if (aqua) {
			// THIS NODE'S SIDE, and nothing of the other's: its own reflectance, its own light, its own
			// on/off. The two sides are independent, so a side whose light is off keeps its colour
			// verbatim while the other shades.
			const bool sideExt = land ? lExt : wExt;
			if (sideExt) { double p[3]; pts->GetPoint(i, p); ei = externShadeAt(land ? s->shadeInLand : s->shadeIn, p[0], p[1]); }
			const AquaSideShade &A = land ? lS : wS;
			const ReliefLight   &Ls = land ? Ll : Lw;
			if (A.useHillshade || sideExt)
				applyReliefShade(Ls, nv, c, std::isnan(ei) ? nullptr : &ei);
			else if (A.litBake)
				applyPBRShade(Ls, nv, c);
		}
		else if (ext || lk.useHillshade) {
			if (ext) { double p[3]; pts->GetPoint(i, p); ei = externShadeAt(s, p[0], p[1]); }
			if (lk.useHillshade)                                         // colour only when the look is unlit
				applyReliefShade(L, nv, c, std::isnan(ei) ? nullptr : &ei);  // SHARED shade (extern / grdimage / Lambert)
		}
		// …and THEN the night, as the second factor on whatever colour the lines above decided. Never
		// instead of them: with no relief shade selected the CPT colour is what this layer looks like
		// in daylight, and this multiply is the whole of the change.
		if (dnOn) { double pw[3]; pts->GetPoint(i, pw); applyDayNight(dayNightFactor(s, pw[0], pw[1]), c); }
		col->SetTypedComponent(i, 0, (unsigned char)(c[0] * 255.0 + 0.5));
		col->SetTypedComponent(i, 1, (unsigned char)(c[1] * 255.0 + 0.5));
		col->SetTypedComponent(i, 2, (unsigned char)(c[2] * 255.0 + 0.5));
	}
	});
	if (aquaTraceOn()) {
		double b[6]; act->GetBounds(b);
		fprintf(stdout, "[bake] x %8.3f..%8.3f y %8.3f..%8.3f | hill=%d grd=%d twoBar=%d ext=%d n=%lld\n",
		        b[0], b[1], b[2], b[3], (int)lk.useHillshade, (int)lk.hillGrd, (int)twoBar, (int)ext,
		        (long long)n);
		fflush(stdout);
	}
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
	const LayerShade &lk = lookOfActor(s, a);   // THIS layer's own look, never the window's
	vtkProperty *prop = a->GetProperty();
	// AN AQUAMOTO SURFACE IS ALWAYS UNLIT. Its colours are baked PER NODE, per side, by
	// hillshadeMapper, so the renderer must show them verbatim — and, decisively, the material may not
	// depend on the window look: that look is written when a side is aimed, and flipping the actor
	// between lit and unlit re-rendered BOTH halves of the tank on a one-side act. Keyed off the layer
	// having two surfaces (`aquaBathyZ`), never off a light.
	if (!s->aquaBathyZ.empty() && !s->aquaLandMask.empty()) {
		prop->SetInterpolationToFlat();
		prop->SetAmbient(1.0); prop->SetDiffuse(0.0); prop->SetSpecular(0.0);
		prop->SetAmbientColor(1.0, 1.0, 1.0);
		hillshadeMapper(s, a);
		return;
	}
	if (lk.noShade) {
		// No illumination at all: flat, fully ambient, so the CPT colour shows exactly as the colour
		// bar says. hillshadeMapper below reverts the mapper to live CPT scalars (useHillshade is off
		// whenever this is on), so nothing modulates the colour either.
		prop->SetInterpolationToFlat();
		prop->SetAmbient(1.0); prop->SetDiffuse(0.0); prop->SetSpecular(0.0);
		prop->SetAmbientColor(1.0, 1.0, 1.0);
	}
	else if (lk.useHillshade) {
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
		prop->SetMetallic(lk.metallic);
		prop->SetRoughness(lk.roughness);
		prop->SetBaseIOR(lk.ior);
	}
	hillshadeMapper(s, a);   // bake or revert the per-node colours to match the material
}

// (Re)assemble the post-process pass chain from the Scene's toggles, then apply
// the live material/light values. Called at setup and from the Shading dock.
static void applyShading(Scene *s) {
	// ANY shading input just changed (mode, sun, sliders, the Hillshade tool's reflectance). Bumping
	// this invalidates every cached LOD tile's colours exactly once — a tile compares its own stamp on
	// its way back into the scene, so it re-bakes when the look really changed and never otherwise.
	++s->styleGen;
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
	for (auto &ex : s->extras)
		if (!ex.isImage && ex.actor) applySurfStyle(s, ex.actor.Get());
	// …and the IMAGE extras, which no bake reaches, get the day/night factor written into their own
	// texture. A no-op unless day/night is on (or was, on this layer — then it restores it).
	for (auto &ex : s->extras)
		if (ex.isImage) dayNightImageLayer(s, ex);
	// key light: aim from azimuth (deg from north, clockwise) + elevation.
	// dir points FROM the scene TO the sun; for a directional light only the
	// Position-minus-FocalPoint direction matters.
	{
		// The renderer holds ONE light rig -- OpenGL cannot give each raster its own -- so it is aimed
		// by the ACTIVE layer's sun, the same layer the dock is editing. The BAKED relief shade, which is
		// what a grid's look actually is, is computed per layer on the CPU and does follow each layer's own.
		const LayerShade &lkS = activeLook(s);
		const double az = lkS.lightAz * vtkMath::Pi() / 180.0;
		const double el = lkS.lightEl * vtkMath::Pi() / 180.0;
		const double dx = std::sin(az) * std::cos(el);   // east
		const double dy = std::cos(az) * std::cos(el);   // north
		const double dz = std::sin(el);                  // up
		s->keyLight->SetFocalPoint(0.0, 0.0, 0.0);
		s->keyLight->SetPosition(dx, dy, dz);
		s->keyLight->SetIntensity(s->lightIntensity);
		s->fillLight->SetIntensity(s->fillIntensity);
		// ON A 3-D BODY ONLY, the same direction is read in the CAMERA's frame instead of the world's.
		// Everything above is untouched for a flat map — this branch cannot be reached unless s->globe
		// is up, so an ordinary grid's lighting is exactly what it always was.
		// Why: azimuth/elevation is a MAP-frame direction. On the flat map every normal is +Z, so one
		// fixed world direction lights the whole scene. A sphere or a cube has no single map frame, so
		// that same fixed direction leaves whole faces (and the globe's trailing limb) on the ambient
		// floor alone. A CAMERA light takes its position in camera coordinates (+x right, +y up, +z
		// toward the viewer) and VTK re-derives it every frame, so az/el keeps meaning "from the left /
		// above / in front of me" however the trackball has turned the planet, with no per-frame code.
		// This is the LIT (PBR / VTK illumination) half of the problem only. The BAKED half — a
		// hillshade computed from world normals because hillshadeMapper was reading the transformed
		// end of the pipeline — is fixed at its own source, in hillshadeMapper below; a baked colour
		// cannot be rescued by any light, which is exactly how the two were told apart.
		// The gizmo's light is included because it is the one that decides the picture: enableGizmo
		// (20_gizmo.cpp) adds a SECOND scene light at intensity 1.4, brighter than the key light, fixed
		// at world (0.6, 0.4, 1.0). Aiming only the key light left that one still blacking out whatever
		// face had turned away from it. Its direction is kept verbatim — this does not re-tune the
		// gizmo's rig, only the frame that direction is expressed in. (20_gizmo.cpp is not edited;
		// applyShading already reaches into s->giz->light for the cast-shadow toggle.)
		const int lt = s->globe ? VTK_LIGHT_TYPE_CAMERA_LIGHT : VTK_LIGHT_TYPE_SCENE_LIGHT;
		s->keyLight->SetLightType(lt);
		if (s->giz && s->giz->light) {
			s->giz->light->SetLightType(lt);
			s->giz->light->SetPosition(0.6, 0.4, 1.0);
			s->giz->light->SetFocalPoint(0.0, 0.0, 0.0);
		}
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

// WHICH RELIEF LOOK THE WINDOW WEARS — the one place that decides it, for every caller.
//
// The four looks are alternatives, so choosing one is a single act: drop whatever external
// reflectance was loaded (a look computed here REPLACES a loaded Illumination model), set the flags
// that `applyShading` / `applyReliefShade` read, and re-shade. The Shading dock's checkboxes used to
// write those flags themselves, inside a lambda captured over the boxes — which meant the maths was
// reachable ONLY by clicking that dock. The Illumination dialog's methods 4 (grdimage) and 5
// (Lambert) ARE these two looks, so they come through here rather than growing a second copy of a
// reflectance this file already owns (SACRED_LAW.md: same operation, always the same function).
//
// It deliberately does NOT touch a checkbox: `applyShading` ends in `syncShadeChecks`, which derives
// the dock from the live flags. So the dock follows whoever set the look, and the day the dock's two
// hillshade boxes are removed this function and its callers are unaffected — nothing here knows a
// checkbox exists.
// CAST SHADOWS IS NOT A LOOK. It is a render PASS on the VTK path — the sun's own self-shadowing,
// a sibling of SSAO, tone mapping and FXAA, not a way of deriving a reflectance. The Shading dock
// used to put it in the exclusive group with the three real looks, which is why it was mistaken for
// one; `Scene::useShadows` is an independent flag, owned by the VTK (PBR) method's own checkbox.
enum ReliefLook { RL_None = 0, RL_PBR, RL_HillLambert, RL_HillGrdimage };

// `keepExternShade` exists for exactly one caller: RESTORING a saved snapshot (Load Session /
// movie's restore_view, through gmtvtk_apply_scene_state). Picking a look is a user action that
// replaces whatever Illumination model was loaded; putting a snapshot back is not -- the session
// restores the model itself (its own :illum recipe) and this must not throw it away a moment later.
// Same function, one parameter, never a second look setter for the restore path.
static void sceneSetReliefLook(Scene *s, int look, bool keepExternShade = false) {
	if (!s) return;
	if (!keepExternShade)
		dropExternShade(s);             // a look picked here replaces a loaded Illumination model
	// The look lands on the ACTIVE layer -- picking "Lambert" over a gravity anomaly must not
	// re-shade the bathymetry underneath it (SACRED_LAW.md: each grid independent of any other).
	LayerShade &lkA = activeLook(s);
	lkA.useHillshade = (look == RL_HillLambert || look == RL_HillGrdimage);
	lkA.hillGrd      = (look == RL_HillGrdimage);
	lkA.litBake      = (look == RL_PBR);
	// A LOOK IS THE WINDOW'S, NOT ONE SIDE'S. An Aquamoto layer keeps a light snapshot per side so the
	// dock can edit water and land independently — but the LOOK (VTK PBR / grdimage / Lambert / none)
	// is the whole window's choice, and it must land on BOTH. It used to reach only the side the Shade
	// Water/Land radio happened to select (rebakeLayerImage snapshots that one), so picking "VTK (PBR)"
	// lit the water with it and left the land on whatever it had — the two halves permanently out of
	// step, and the method the user chose never applied to land at all.
	// Only the three LOOK flags are copied: each side's own sun, gain and ambience stay its own.
	for (AquaSideShade *A : { &s->aquaWaterShade, &s->aquaLandShade })
		if (A->valid) { A->useHillshade = lkA.useHillshade; A->hillGrd = lkA.hillGrd; A->litBake = lkA.litBake; }
	applyShading(s);                     // …which re-syncs the dock and re-bakes a flat image
	if (s->syncFlatEnable) s->syncFlatEnable();   // which sliders are live depends on the chosen look
}

// THE SAME ACT, AIMED AT ONE AQUAMOTO SIDE. A tsunami layer is two images standing on two surfaces
// (SACRED_LAW.md, two-surface illumination law), so water and land each get their OWN method — the
// two palette buttons beside Shade Water / Shade Land aim the Illumination dialog at one of them.
// `side`: 1 = LAND, anything else = WATER.
//
// The look flags are written where every look is written (the live `activeLook`), and the side's own
// snapshot is then taken by `snapshotShade` — the VERY function a Shading-dock edit snapshots with, so
// a side aimed here and a side edited in the dock are described by one piece of code, never two. The
// OTHER side's snapshot is not touched, which is what leaves its own method standing.
static void sceneSetReliefLookAquaSide(Scene *s, int look, int side, bool keepExternShade = false) {
	if (!s) return;
	if (!keepExternShade)
		dropExternShadeSide(s, side);    // a look picked here replaces THIS side's loaded model
	// THE WINDOW LOOK IS NOT TOUCHED. Writing the three flags into `activeLook` was the last piece of
	// this operation that both sides could see: `applySurfStyle` keys the actor's MATERIAL off them,
	// so aiming the water flipped the whole tank between lit and unlit and the land visibly changed
	// with it. The choice belongs to the side that was aimed, and nowhere else.
	// `aquaShadeSelWater` is NOT touched either: it is a SELECTOR (which side the colour bar shows /
	// the radio names), and aiming one side's light may not move what the other side displays.
	AquaSideShade &A = (side == 1) ? s->aquaLandShade : s->aquaWaterShade;
	A = snapshotShade(s);                // the live sun / gain / material the dialog just wrote…
	A.useHillshade = (look == RL_HillLambert || look == RL_HillGrdimage);   // …with THIS side's look
	A.hillGrd      = (look == RL_HillGrdimage);
	A.litBake      = (look == RL_PBR);
	applyShading(s);
	if (s->syncFlatEnable) s->syncFlatEnable();
}

// Add a GMTdataset overlay (lines or points) to an existing scene. `xyz` is npts
// triples (x,y,z) in TRUE data coords; `segoff` holds nseg+1 offsets so segment k is
// points [segoff[k], segoff[k+1]). mode 0 = points, 1 = polylines. The actor gets the
// surface's base scale (xfac/zfac/VE) so it sits in register, and a coincident-topology
// offset toward the camera so it wins the depth tie with the surface (draws on top).
// Rebuild the Scene Objects panel: one checkbox per scene element (surface, image
// drape, each line/point overlay) that toggles the actor's visibility. Re-called
// whenever the overlay set changes, since overlays are added after the window shows.
// `zfac` is THIS layer's own axis mapping (sceneZRefFor over its own z range) and `ve` its own
// exaggeration, so the normal correction describes the relief exactly AS DRAWN -- which, the VE
// being defined against the displayed dimensions, is also the relief the eye sees.
