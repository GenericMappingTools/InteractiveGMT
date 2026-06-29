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

	// sun direction (points FROM scene TO sun): az from north CW, el above horizon.
	const double az = s->lightAz * vtkMath::Pi() / 180.0;
	const double el = s->lightEl * vtkMath::Pi() / 180.0;
	const double Lx = std::sin(az) * std::cos(el);
	const double Ly = std::cos(az) * std::cos(el);
	const double Lz = std::sin(el);
	// Lambert (A): VE-correct the baked TRUE-coord normal onto the displayed relief (xfac,1,zfac*ve).
	const double fx  = (s->xfac != 0.0) ? 1.0 / s->xfac : 1.0;
	const double fz  = (s->zfac * s->ve != 0.0) ? 1.0 / (s->zfac * s->ve) : 1.0;
	const double amb = s->hillAmbient;
	const double gain = s->hillGain;              // grdimage relief contrast = grdgradient -Nt amp
	const double twoOverPi = 2.0 / vtkMath::Pi();
	// grdimage (B): the elevation angle is INVERTED vs the Lambert/lit convention — flip it (90-el).
	const double elG = (90.0 - s->lightEl) * vtkMath::Pi() / 180.0;
	const double LxG = std::sin(az) * std::cos(elG);
	const double LyG = std::cos(az) * std::cos(elG);
	const double LzG = std::sin(elG);

	const vtkIdType n = pd->GetNumberOfPoints();
	vtkSmartPointer<vtkUnsignedCharArray> col = vtkSmartPointer<vtkUnsignedCharArray>::New();
	col->SetName("hillshade");
	col->SetNumberOfComponents(3);
	col->SetNumberOfTuples(n);
	for (vtkIdType i = 0; i < n; ++i) {
		double nv[3]; nrm->GetTuple(i, nv);
		const unsigned char *rgb8 = lut->MapValue(zs->GetComponent(i, 0));   // CPT colour for this z
		if (!s->hillGrd) {                                           // (A) Lambert, VE-corrected, darken-only
			double cx = nv[0]*fx, cy = nv[1], cz = nv[2]*fz;
			const double len = std::sqrt(cx*cx + cy*cy + cz*cz);
			if (len > 0.0) { cx /= len; cy /= len; cz /= len; }
			double sh = cx*Lx + cy*Ly + cz*Lz;                      // Lambert N.L
			if (sh < 0.0) sh = 0.0;
			const double I = amb + (1.0 - amb) * sh;                // ambient floor
			col->SetTypedComponent(i, 0, (unsigned char)std::min(255.0, rgb8[0] * I));
			col->SetTypedComponent(i, 1, (unsigned char)std::min(255.0, rgb8[1] * I));
			col->SetTypedComponent(i, 2, (unsigned char)std::min(255.0, rgb8[2] * I));
			continue;
		}
		// (B) GMT grdimage from the z-gradient (raw TRUE-coord normal), HSV illuminate.
		const double raw   = nv[0]*LxG + nv[1]*LyG + nv[2]*LzG - LzG;   // slope-toward-sun signal (inverted el); 0 on flat
		const double inten = twoOverPi * std::atan(gain * raw);     // soft-clip to (-1,1)
		double c[3] = { rgb8[0] / 255.0, rgb8[1] / 255.0, rgb8[2] / 255.0 };
		gmtIlluminate(inten, c);                                    // GMT grdimage HSV modulation
		col->SetTypedComponent(i, 0, (unsigned char)(c[0] * 255.0 + 0.5));
		col->SetTypedComponent(i, 1, (unsigned char)(c[1] * 255.0 + 0.5));
		col->SetTypedComponent(i, 2, (unsigned char)(c[2] * 255.0 + 0.5));
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
