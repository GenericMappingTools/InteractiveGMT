static vtkSmartPointer<vtkTexture> makeSkyEnv(double gain = 1.0) {
	const int W = 1024, H = 512;
	const double sunAz = 0.62, sunEl = 0.40;     // sun direction (fraction of 2pi / pi)
	const double sunCx = sunAz * W, sunCy = (1.0 - sunEl) * H, sunR = 70.0;
	vtkNew<vtkImageData> sky;
	sky->SetDimensions(W, H, 1);
	sky->AllocateScalars(VTK_FLOAT, 3);
	float* px = static_cast<float*>(sky->GetScalarPointer());
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

// (Re)assemble the post-process pass chain from the Scene's toggles, then apply
// the live material/light values. Called at setup and from the Shading dock.
static void applyShading(Scene* s) {
	// material — PBR on the RELIEF SURFACE ONLY. The drape is a textured picture and MUST
	// stay Phong: VTK's PBR shader samples only SetBaseColorTexture, so a PBR drape ignores
	// its SetTexture and renders flat grey. Do NOT touch s->drape's material here.
	if (s->surf) {
		if (s->matteSurf) {
			// fv colour mesh: PURE-DIFFUSE Phong (no specular, no IBL reflection) so every facet
			// shows its true data colour. Glossy PBR + the bright sky IBL mirrored to a grey
			// specular highlight on up-facing crest facets ("grey top row"). Phong ignores the
			// env texture entirely, so no angle can wash the colour to grey. Must live HERE, not
			// only in gmtvtk_view_fv: applyShading runs at setup AND on every shading slider and
			// would otherwise re-clobber the surface back to glossy PBR.
			s->surf->GetProperty()->SetInterpolationToPhong();
			s->surf->GetProperty()->SetSpecular(0.0);
			s->surf->GetProperty()->SetAmbient(0.25);
			s->surf->GetProperty()->SetDiffuse(0.9);
		}
		else {
			s->surf->GetProperty()->SetInterpolationToPBR();
			s->surf->GetProperty()->SetMetallic(s->metallic);
			s->surf->GetProperty()->SetRoughness(s->roughness);
			s->surf->GetProperty()->SetBaseIOR(s->ior);   // Fresnel rim-sheen on slopes (F3D = 1.5)
		}
	}
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
	// pass chain: steps -> [SSAO] -> [tone] -> [FXAA]
	vtkSmartPointer<vtkRenderPass> chain = vtkSmartPointer<vtkRenderStepsPass>::New();
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
