// ============================================================================
//  Point-cloud rubber-band selection (Ctrl+right-drag) — ported from GMTF3D
//  f3d_ext_interactor.cxx. Ctrl+right-drag a box to (de)select points (TOGGLE: a
//  point already selected and re-dragged is removed). Ctrl+Z undoes the last change.
//  Plain right-drag stays the normal dolly. The picked set is highlighted (caller
//  colour) and kept for the host to read back via gmtvtk_get_selection.
//
//  Area pick is SCREEN-SPACE (project every point with the actor scale, test box
//  containment) — hardware pickers miss points (same lesson as pickOverlayAt).
// ============================================================================
static void rbSetBox(Scene* s, int x0, int y0, int x1, int y1) {
	s->rbBoxPts->SetPoint(0, x0, y0, 0.0);
	s->rbBoxPts->SetPoint(1, x1, y0, 0.0);
	s->rbBoxPts->SetPoint(2, x1, y1, 0.0);
	s->rbBoxPts->SetPoint(3, x0, y1, 0.0);
	s->rbBoxPts->Modified();
}

// Rebuild the highlight overlay (selected points drawn on top) from the current set.
static void rbRebuildHighlight(Scene* s) {
	s->rbHLPts->Reset();
	vtkNew<vtkCellArray> verts;
	if (s->cloudPD && !s->rbSel.empty()) {
		vtkPoints* src = s->cloudPD->GetPoints();
		const vtkIdType np = src ? src->GetNumberOfPoints() : 0;
		for (vtkIdType id : s->rbSel) {
			if (id < np) {
				double p[3]; src->GetPoint(id, p);
				const vtkIdType nid = s->rbHLPts->InsertNextPoint(p);
				verts->InsertNextCell(1, &nid);
			}
		}
	}
	s->rbHLPoly->SetVerts(verts);
	s->rbHLPoly->Modified();
	s->rbHL->SetVisibility(s->rbSel.empty() ? 0 : 1);
}

// Screen-space area pick: every cloud point projected to display (with the surf actor
// scale, no rot/trans on the cloud), kept if inside the drag rectangle.
static void rbAreaPick(Scene* s, int x0, int y0, int x1, int y1, std::vector<vtkIdType>& out) {
	out.clear();
	if (!s->cloudPD || !s->cloudPD->GetPoints())
		return;
	vtkRenderer* ren = s->ren;
	const double xlo = std::min(x0, x1), xhi = std::max(x0, x1);
	const double ylo = std::min(y0, y1), yhi = std::max(y0, y1);
	double sc[3]; s->surf->GetScale(sc);
	vtkPoints* pts = s->cloudPD->GetPoints();
	const vtkIdType np = pts->GetNumberOfPoints();
	for (vtkIdType i = 0; i < np; ++i) {
		double p[3]; pts->GetPoint(i, p);
		ren->SetWorldPoint(p[0]*sc[0], p[1]*sc[1], p[2]*sc[2], 1.0);
		ren->WorldToDisplay();
		double d[3]; ren->GetDisplayPoint(d);
		if (d[0] >= xlo && d[0] <= xhi && d[1] >= ylo && d[1] <= yhi)
			out.push_back(i);
	}
}

static void RubberCB(vtkObject* caller, unsigned long eid, void* clientData, void*) {
	Scene* s = static_cast<Scene*>(clientData);
	vtkRenderWindowInteractor* rwi = vtkRenderWindowInteractor::SafeDownCast(caller);
	if (!s || !rwi || !s->rbEnabled)
		return;
	bool handled = false;

	if (eid == vtkCommand::RightButtonPressEvent) {
		// Hijack the right button ONLY when Ctrl is held: Ctrl+right-drag = box select
		// (mirrors Ctrl+left-drag = profile). Plain right-drag stays the dolly, so the
		// selector is always available yet never triggers navigation by accident.
		if (rwi->GetControlKey()) {
			s->rbX0 = rwi->GetEventPosition()[0];
			s->rbY0 = rwi->GetEventPosition()[1];
			s->rbSelecting = true;
			rbSetBox(s, s->rbX0, s->rbY0, s->rbX0, s->rbY0);
			s->rbBox->SetVisibility(1);
			rwi->Render();
			handled = true;
		}
	}
	else if (eid == vtkCommand::MouseMoveEvent) {
		if (s->rbSelecting) {
			rbSetBox(s, s->rbX0, s->rbY0, rwi->GetEventPosition()[0], rwi->GetEventPosition()[1]);
			rwi->Render();
			handled = true;
		}
	}
	else if (eid == vtkCommand::RightButtonReleaseEvent) {
		if (s->rbSelecting) {
			s->rbSelecting = false;
			const int x1 = rwi->GetEventPosition()[0], y1 = rwi->GetEventPosition()[1];
			s->rbBox->SetVisibility(0);
			std::vector<vtkIdType> hit;
			rbAreaPick(s, s->rbX0, s->rbY0, x1, y1, hit);
			// Save state for undo, then TOGGLE the newly picked ids in/out of the set.
			s->rbUndo.push_back(std::vector<vtkIdType>(s->rbSel.begin(), s->rbSel.end()));
			for (vtkIdType id : hit) {
				auto it = s->rbSel.find(id);
				if (it == s->rbSel.end()) s->rbSel.insert(id);
				else                      s->rbSel.erase(it);
			}
			rbRebuildHighlight(s);
			rwi->Render();
			s->rbConsume = true;   // the context menu this right-release also triggers is swallowed
			handled = true;
		}
	}
	else if (eid == vtkCommand::KeyPressEvent) {
		const char* sym = rwi->GetKeySym();
		if (rwi->GetControlKey() && sym && (sym[0] == 'z' || sym[0] == 'Z') && sym[1] == '\0') {
			if (!s->rbUndo.empty()) {
				s->rbSel = std::set<vtkIdType>(s->rbUndo.back().begin(), s->rbUndo.back().end());
				s->rbUndo.pop_back();
				rbRebuildHighlight(s);
				rwi->Render();
			}
			handled = true;
		}
	}

	if (s->rbCmd)
		s->rbCmd->SetAbortFlagOnExecute(handled ? 1 : 0);
}

// Enable Ctrl+right-drag rubber-band selection on a point-cloud figure. `cloud` is the
// cloud polydata whose point ids the selection indexes; r,g,b = highlight colour. The
// interactor must already be live (call after buildAndShow).
static void enableRubberBand(Scene* s, vtkSmartPointer<vtkPolyData> cloud, double r, double g, double b) {
	if (!s || !s->widget || !s->widget->interactor())
		return;
	s->cloudPD = cloud;
	s->rbR = r; s->rbG = g; s->rbB = b;

	// 2D selection rectangle (display coords).
	vtkNew<vtkPoints> bpts; bpts->SetNumberOfPoints(4);
	vtkNew<vtkCellArray> bl; const vtkIdType loop[5] = { 0, 1, 2, 3, 0 }; bl->InsertNextCell(5, loop);
	vtkNew<vtkPolyData> bpd; bpd->SetPoints(bpts); bpd->SetLines(bl);
	vtkNew<vtkCoordinate> coord; coord->SetCoordinateSystemToDisplay();
	vtkNew<vtkPolyDataMapper2D> bmap; bmap->SetInputData(bpd); bmap->SetTransformCoordinate(coord);
	s->rbBox = vtkSmartPointer<vtkActor2D>::New();
	s->rbBox->SetMapper(bmap);
	s->rbBox->GetProperty()->SetColor(1.0, 1.0, 0.0);   // yellow marquee
	s->rbBox->GetProperty()->SetLineWidth(1.5);
	s->rbBox->SetVisibility(0);
	s->ren->AddViewProp(s->rbBox);
	s->rbBoxPts = bpts;

	// Highlight overlay for the selected points (drawn on top, caller colour).
	vtkNew<vtkPoints> hpts;
	vtkNew<vtkPolyData> hpoly; hpoly->SetPoints(hpts);
	vtkNew<vtkPolyDataMapper> hmap; hmap->SetInputData(hpoly); hmap->ScalarVisibilityOff();
	vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
	hmap->SetRelativeCoincidentTopologyPointOffsetParameter(-9000.0);   // sit above the cloud
	s->rbHL = vtkSmartPointer<vtkActor>::New();
	s->rbHL->SetMapper(hmap);
	s->rbHL->GetProperty()->SetColor(r, g, b);
	s->rbHL->GetProperty()->SetPointSize(10.0);
	s->rbHL->GetProperty()->SetRenderPointsAsSpheres(true);
	s->rbHL->GetProperty()->LightingOff();
	s->rbHL->PickableOff();
	s->rbHL->SetScale(s->xfac, 1.0, s->zfac * s->ve);   // track the cloud's scale / VE
	s->rbHL->SetVisibility(0);
	s->ren->AddActor(s->rbHL);
	s->rbHLPts = hpts; s->rbHLPoly = hpoly;

	vtkNew<vtkCallbackCommand> cmd;
	cmd->SetCallback(RubberCB);
	cmd->SetClientData(s);
	s->rbCmd = cmd;
	vtkRenderWindowInteractor* rwi = s->widget->interactor();
	rwi->AddObserver(vtkCommand::RightButtonPressEvent, cmd, 10.0);
	rwi->AddObserver(vtkCommand::MouseMoveEvent, cmd, 10.0);
	rwi->AddObserver(vtkCommand::RightButtonReleaseEvent, cmd, 10.0);
	rwi->AddObserver(vtkCommand::KeyPressEvent, cmd, 10.0);
	s->rbEnabled = true;
}

