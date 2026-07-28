// 87_vtkio.cpp — VTK's OWN file formats, read and written natively by VTK.
//
// Everything else in iGMT reaches disk through GMT.jl / GDAL on the Julia side; VTK datasets are the
// one family neither can parse (`gmtread` on a .vtp just errors), so they are caught BEFORE the
// Julia read path (`_open_spec_into`, src/drop.jl) and handled here instead. `.hdf` / `.h5` are NOT
// ours — those stay with GDAL; only VTK's own `.vtkhdf` is claimed.
//
// Read covers every VTK dataset type. Two generic readers do the sniffing (one for the whole XML
// family, one for the legacy family), so there is no per-extension reader table to keep in sync:
//
//   .vti .vtr .vts .vtp .vtu + .pvti/.pvtr/.pvts/.pvtp/.pvtu   vtkXMLGenericDataObjectReader
//   .vtm .vtmb                                                  vtkXMLMultiBlockDataReader
//   .vtk (legacy, any dataset type)                             vtkGenericDataObjectReader
//   .vtkhdf                                                     vtkHDFReader
//
// What arrives is then mapped onto the display paths iGMT ALREADY has — never a new kind of scene
// object (SACRED_LAW.md: same operation, same function):
//
//   axis-aligned, uniformly spaced 2-D structured data  -> a GRID layer (colorbar, shading,
//                                                          hillshade, hover readout — all free)
//   anything with polygon cells                          -> the FV mesh path (gmtvtk_view_fv /
//                                                          gmtvtk_promote_fv_h)
//   line cells only                                      -> a line overlay
//   points only                                          -> a point cloud
//
// A volume, a curvilinear grid or an unstructured mesh is reduced to its bounding SURFACE first
// (vtkDataSetSurfaceFilter) — the polydata every one of those paths speaks.

// What a VTK file turned into, ready to hand to an existing iGMT builder. Exactly one of the four
// kinds is filled; `detail` is a short human description for the status bar.
struct VtkIoLoad {
	enum Kind { None = 0, Grid, Mesh, Lines, Points };
	Kind kind = None;
	// Grid
	std::vector<float> z;
	int    nx = 0, ny = 0;
	double x0 = 0, x1 = 1, y0 = 0, y1 = 1;
	// Mesh / Lines / Points (xyz is shared; mesh adds the cell arrays, lines the segment offsets)
	std::vector<double> xyz;
	std::vector<int>    sides, indices, segoff;
	double z0 = 0, z1 = 1;
	std::string detail;
};

// Is this path one of VTK's own formats? Deliberately EXCLUDES .hdf / .h5 (plain HDF5 rasters keep
// going to GDAL) while claiming VTK's own .vtkhdf.
static bool vtkioIsVtkPath(const std::string &path) {
	const int dot = (int)path.find_last_of('.');
	if (dot < 0) return false;
	QString ext = QString::fromStdString(path.substr(dot + 1)).toLower();
	static const char *kExts[] = { "vtk", "vti", "vtr", "vts", "vtp", "vtu", "vtm", "vtmb",
	                               "pvti", "pvtr", "pvts", "pvtp", "pvtu", "vtkhdf" };
	for (const char *e : kExts) if (ext == QLatin1String(e)) return true;
	return false;
}

// Read the file with the reader that matches its family. Returns null + `err` on failure.
static vtkSmartPointer<vtkDataObject> vtkioReadFile(const std::string &path, std::string &err) {
	const int dot = (int)path.find_last_of('.');
	const QString ext = dot < 0 ? QString() : QString::fromStdString(path.substr(dot + 1)).toLower();
	vtkSmartPointer<vtkDataObject> out;
	if (ext == "vtkhdf") {
		vtkNew<vtkHDFReader> r;
		if (!r->CanReadFile(path.c_str())) { err = "not a readable VTKHDF file"; return nullptr; }
		r->SetFileName(path.c_str());
		r->Update();
		out = r->GetOutputDataObject(0);
	}
	else if (ext == "vtm" || ext == "vtmb") {
		vtkNew<vtkXMLMultiBlockDataReader> r;
		r->SetFileName(path.c_str());
		r->Update();
		out = r->GetOutputDataObject(0);
	}
	else if (ext == "vtk") {
		vtkNew<vtkGenericDataObjectReader> r;      // sniffs the legacy header for the dataset type
		r->SetFileName(path.c_str());
		r->Update();
		out = r->GetOutput();
	}
	else {
		vtkNew<vtkXMLGenericDataObjectReader> r;   // sniffs the XML root element for the dataset type
		r->SetFileName(path.c_str());
		r->Update();
		out = r->GetOutput();
	}
	if (!out) err = "VTK reader produced no dataset";
	return out;
}

// The dataset to display. A multiblock file yields its first non-empty leaf (a whole composite tree
// has no single sensible iGMT representation; the leaf count is reported so the user knows).
static vtkSmartPointer<vtkDataSet> vtkioFirstDataSet(vtkDataObject *obj, int &nblocks) {
	nblocks = 0;
	if (!obj) return nullptr;
	if (vtkDataSet *ds = vtkDataSet::SafeDownCast(obj)) { nblocks = 1; return ds; }
	vtkMultiBlockDataSet *mb = vtkMultiBlockDataSet::SafeDownCast(obj);
	if (!mb) return nullptr;
	vtkSmartPointer<vtkDataSet> first;
	vtkSmartPointer<vtkCompositeDataIterator> it = mb->NewIterator();
	for (it->InitTraversal(); !it->IsDoneWithTraversal(); it->GoToNextItem()) {
		vtkDataSet *ds = vtkDataSet::SafeDownCast(it->GetCurrentDataObject());
		if (!ds || ds->GetNumberOfPoints() == 0) continue;
		++nblocks;
		if (!first) first = ds;
	}
	return first;
}

// Is `c` (n values along one axis) uniformly spaced? A rectilinear/structured axis that is not
// tells us the data is NOT an iGMT grid (whose x/y are implied by a range + node count), so it must
// go down the mesh path instead of being silently resampled onto even spacing.
static bool vtkioAxisUniform(const double *c, int n) {
	if (n < 3) return true;
	const double step = c[1] - c[0];
	if (step == 0.0) return false;
	const double tol = std::fabs(step) * 1e-4;
	for (int i = 2; i < n; ++i)
		if (std::fabs((c[i] - c[i - 1]) - step) > tol) return false;
	return true;
}

// Pull the scalar the file marks active, falling back to the first point array. Null when the
// dataset carries no point data at all (pure geometry) -> the caller uses the mesh path.
static vtkDataArray *vtkioPointScalars(vtkDataSet *ds) {
	if (!ds || !ds->GetPointData()) return nullptr;
	if (vtkDataArray *a = ds->GetPointData()->GetScalars()) return a;
	for (int i = 0; i < ds->GetPointData()->GetNumberOfArrays(); ++i)
		if (vtkDataArray *a = ds->GetPointData()->GetArray(i)) return a;
	return nullptr;
}

// Try to read `ds` as an iGMT GRID: 2-D, axis-aligned, uniformly spaced, carrying a point scalar.
// vtkImageData is uniform by construction; a rectilinear grid must pass the spacing test; a
// structured grid is curvilinear in general and is left to the mesh path. false -> not a grid.
static bool vtkioAsGrid(vtkDataSet *ds, VtkIoLoad &out) {
	int dims[3] = { 0, 0, 0 };
	double coordX[2] = { 0, 1 }, coordY[2] = { 0, 1 };
	std::vector<double> cx, cy;
	if (vtkImageData *im = vtkImageData::SafeDownCast(ds)) {
		im->GetDimensions(dims);
		double org[3], sp[3];
		im->GetOrigin(org); im->GetSpacing(sp);
		if (dims[2] != 1 || dims[0] < 2 || dims[1] < 2) return false;
		coordX[0] = org[0]; coordX[1] = org[0] + sp[0] * (dims[0] - 1);
		coordY[0] = org[1]; coordY[1] = org[1] + sp[1] * (dims[1] - 1);
	}
	else if (vtkRectilinearGrid *rg = vtkRectilinearGrid::SafeDownCast(ds)) {
		rg->GetDimensions(dims);
		if (dims[2] != 1 || dims[0] < 2 || dims[1] < 2) return false;
		vtkDataArray *ax = rg->GetXCoordinates(), *ay = rg->GetYCoordinates();
		if (!ax || !ay) return false;
		cx.resize(dims[0]); cy.resize(dims[1]);
		for (int i = 0; i < dims[0]; ++i) cx[i] = ax->GetComponent(i, 0);
		for (int j = 0; j < dims[1]; ++j) cy[j] = ay->GetComponent(j, 0);
		if (!vtkioAxisUniform(cx.data(), dims[0]) || !vtkioAxisUniform(cy.data(), dims[1]))
			return false;                       // genuinely non-uniform -> mesh, never a fake resample
		coordX[0] = cx.front(); coordX[1] = cx.back();
		coordY[0] = cy.front(); coordY[1] = cy.back();
	}
	else {
		return false;                            // structured (curvilinear) / unstructured -> mesh
	}
	vtkDataArray *sc = vtkioPointScalars(ds);
	if (!sc || sc->GetNumberOfTuples() < (vtkIdType)dims[0] * dims[1]) return false;

	out.kind = VtkIoLoad::Grid;
	out.nx = dims[0]; out.ny = dims[1];
	out.x0 = coordX[0]; out.x1 = coordX[1];
	out.y0 = coordY[0]; out.y1 = coordY[1];
	// iGMT's grid z is column-major z[i*ny + j] (GMT layout); a VTK structured point id runs x
	// fastest, so id = i + j*nx.
	out.z.resize((size_t)out.nx * out.ny);
	for (int j = 0; j < out.ny; ++j)
		for (int i = 0; i < out.nx; ++i)
			out.z[(size_t)i * out.ny + j] = (float)sc->GetComponent((vtkIdType)j * out.nx + i, 0);
	out.detail = QString("%1 grid %2x%3, scalar \"%4\"")
	             .arg(vtkImageData::SafeDownCast(ds) ? "image" : "rectilinear")
	             .arg(out.nx).arg(out.ny)
	             .arg(sc->GetName() ? sc->GetName() : "(unnamed)").toStdString();
	return true;
}

// Reduce any dataset to the polydata every mesh/line/point path speaks. A polydata passes through
// (triangulated only when it carries strips, which the flat sides/indices encoding cannot express).
static vtkSmartPointer<vtkPolyData> vtkioAsPolyData(vtkDataSet *ds) {
	vtkSmartPointer<vtkPolyData> pd = vtkPolyData::SafeDownCast(ds);
	if (!pd) {
		vtkNew<vtkDataSetSurfaceFilter> surf;   // volume / unstructured / curvilinear -> its skin
		surf->SetInputData(ds);
		surf->Update();
		pd = surf->GetOutput();
	}
	if (pd && pd->GetNumberOfStrips() > 0) {
		vtkNew<vtkTriangleFilter> tri;
		tri->SetInputData(pd);
		tri->Update();
		pd = tri->GetOutput();
	}
	return pd;
}

// Fill `out` from a polydata: polygon cells -> Mesh, else line cells -> Lines, else -> Points.
static bool vtkioFromPolyData(vtkPolyData *pd, VtkIoLoad &out, std::string &err) {
	if (!pd || pd->GetNumberOfPoints() == 0) { err = "dataset has no points"; return false; }
	const vtkIdType np = pd->GetNumberOfPoints();
	out.xyz.resize((size_t)np * 3);
	for (vtkIdType i = 0; i < np; ++i) {
		double p[3];
		pd->GetPoint(i, p);
		out.xyz[(size_t)i * 3 + 0] = p[0];
		out.xyz[(size_t)i * 3 + 1] = p[1];
		out.xyz[(size_t)i * 3 + 2] = p[2];
	}
	double b[6];
	pd->GetBounds(b);
	out.x0 = b[0]; out.x1 = b[1];            // the grid fields double as the mesh/overlay bbox
	out.y0 = b[2]; out.y1 = b[3];
	out.z0 = b[4]; out.z1 = b[5];
	// A flat sheet (a plane of quads at constant x, a mesh with no relief) has a zero-span axis; give
	// the DECLARED data range one so the axis annotates sensibly. The axes CUBE has its own guard for
	// the same case at the one box setter every caller shares (axesSetBounds, 10_geometry.cpp).
	if (!(out.x1 > out.x0)) { out.x0 -= 0.5; out.x1 += 0.5; }
	if (!(out.y1 > out.y0)) { out.y0 -= 0.5; out.y1 += 0.5; }
	if (!(out.z1 > out.z0)) { out.z0 -= 0.5; out.z1 += 0.5; }

	if (pd->GetNumberOfPolys() > 0) {
		auto it = vtk::TakeSmartPointer(pd->GetPolys()->NewIterator());
		for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell()) {
			vtkIdList *ids = it->GetCurrentCell();
			const int n = (int)ids->GetNumberOfIds();
			if (n < 3) continue;
			out.sides.push_back(n);
			for (int k = 0; k < n; ++k) out.indices.push_back((int)ids->GetId(k));
		}
	}
	if (!out.sides.empty()) {
		out.kind = VtkIoLoad::Mesh;
		out.detail = QString("polygon mesh: %1 vertices, %2 faces")
		             .arg((long long)np).arg((long long)out.sides.size()).toStdString();
		return true;
	}
	if (pd->GetNumberOfLines() > 0) {
		// The overlay encoding is one flat point list + segment start offsets, so the points are
		// RE-EMITTED in cell order (a VTK line cell indexes shared points in any order).
		std::vector<double> lx;
		auto it = vtk::TakeSmartPointer(pd->GetLines()->NewIterator());
		for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell()) {
			vtkIdList *ids = it->GetCurrentCell();
			if (ids->GetNumberOfIds() < 2) continue;
			out.segoff.push_back((int)(lx.size() / 3));
			for (vtkIdType k = 0; k < ids->GetNumberOfIds(); ++k) {
				double p[3];
				pd->GetPoint(ids->GetId(k), p);
				lx.push_back(p[0]); lx.push_back(p[1]); lx.push_back(p[2]);
			}
		}
		if (!out.segoff.empty()) {
			out.segoff.push_back((int)(lx.size() / 3));      // trailing sentinel: addOverlay's contract
			out.xyz.swap(lx);
			out.kind = VtkIoLoad::Lines;
			out.detail = QString("%1 polylines, %2 vertices")
			             .arg((long long)out.segoff.size() - 1)
			             .arg((long long)(out.xyz.size() / 3)).toStdString();
			return true;
		}
	}
	out.kind = VtkIoLoad::Points;
	out.detail = QString("point cloud: %1 points").arg((long long)np).toStdString();
	return true;
}

// Read `path` and classify it. false + `err` on any failure.
static bool vtkioLoad(const std::string &path, VtkIoLoad &out, std::string &err) {
	vtkSmartPointer<vtkDataObject> obj = vtkioReadFile(path, err);
	if (!obj) return false;
	int nblocks = 0;
	vtkSmartPointer<vtkDataSet> ds = vtkioFirstDataSet(obj, nblocks);
	if (!ds || ds->GetNumberOfPoints() == 0) { err = "VTK file holds no point data"; return false; }
	if (vtkioAsGrid(ds, out)) {
		if (nblocks > 1) out.detail += QString(" (block 1 of %1)").arg(nblocks).toStdString();
		return true;
	}
	vtkSmartPointer<vtkPolyData> pd = vtkioAsPolyData(ds);
	if (!vtkioFromPolyData(pd, out, err)) return false;
	if (nblocks > 1) out.detail += QString(" (block 1 of %1)").arg(nblocks).toStdString();
	return true;
}

// ── writing ────────────────────────────────────────────────────────────────────────────────────
// The Save dialog's VTK entries (kGridFmts / kImageFmts, 30_app.cpp) never reach Julia: GMT and GDAL
// cannot write these, so saveObjectDialog routes the vtk* format codes straight here.

// The grid layer the Save request names: empty name = the window's base relief, otherwise the
// matching extra. Fills the caller's pointers with that layer's own data. false = no such grid.
static bool vtkioResolveGrid(Scene *s, const std::string &name, const std::vector<float> *&z,
                             int &nx, int &ny, double &x0, double &x1, double &y0, double &y1) {
	if (!s) return false;
	if (name.empty() || name == s->surfName) {
		if (s->gridZ.empty()) return false;
		z = &s->gridZ; nx = s->gnx; ny = s->gny;
		x0 = s->gx0; x1 = s->gx1; y0 = s->gy0; y1 = s->gy1;
		return true;
	}
	for (auto &ex : s->extras) {
		if (ex.name != name || ex.gridZ.empty()) continue;
		z = &ex.gridZ; nx = ex.gnx; ny = ex.gny;
		x0 = ex.gx0; x1 = ex.gx1; y0 = ex.gy0; y1 = ex.gy1;
		return true;
	}
	return false;
}

// A grid layer as a vtkImageData: one float point-scalar array named after the layer, x fastest.
static vtkSmartPointer<vtkImageData> vtkioGridToImage(const std::vector<float> &z, int nx, int ny,
                                                      double x0, double x1, double y0, double y1,
                                                      const std::string &name) {
	vtkNew<vtkImageData> im;
	im->SetDimensions(nx, ny, 1);
	im->SetOrigin(x0, y0, 0.0);
	im->SetSpacing(nx > 1 ? (x1 - x0) / (nx - 1) : 1.0, ny > 1 ? (y1 - y0) / (ny - 1) : 1.0, 1.0);
	vtkNew<vtkFloatArray> arr;
	arr->SetName(name.empty() ? "z" : name.c_str());
	arr->SetNumberOfComponents(1);
	arr->SetNumberOfTuples((vtkIdType)nx * ny);
	for (int j = 0; j < ny; ++j)
		for (int i = 0; i < nx; ++i)
			arr->SetValue((vtkIdType)j * nx + i, z[(size_t)i * ny + j]);   // GMT column-major -> VTK x-fastest
	im->GetPointData()->SetScalars(arr);
	return im;
}

// The polydata actually being rendered for the named object — what a .vtp save must write. Uses the
// SAME name resolution as the grid case above, then the actor's mapper input (the mesh/cloud/surface
// as built, so what lands on disk is what is on screen).
static vtkSmartPointer<vtkPolyData> vtkioResolvePolyData(Scene *s, const std::string &name) {
	if (!s) return nullptr;
	vtkActor *a = nullptr;
	if (name.empty() || name == s->surfName) {
		if (!s->tiles.empty()) a = s->tiles.front();     // tiled base: only the resident tiles exist
		else if (s->surf)      a = s->surf;
	}
	else {
		for (auto &ex : s->extras)
			if (ex.name == name && ex.actor) { a = ex.actor; break; }
	}
	if (!a || !a->GetMapper()) return nullptr;
	return vtkPolyData::SafeDownCast(a->GetMapper()->GetInput());
}

// The RGB(A) raster behind an IMAGE layer, straight off the texture it is drawn with — already a
// vtkImageData, so a .vti save writes exactly the pixels on screen. Null when `name` is not an image.
static vtkSmartPointer<vtkImageData> vtkioResolveImage(Scene *s, const std::string &name) {
	if (!s) return nullptr;
	vtkTexture *t = nullptr;
	if (name.empty() || name == s->surfName) {
		if (s->drape) t = s->drape->GetTexture();
	}
	else {
		for (auto &ex : s->extras) {
			if (ex.name != name) continue;
			t = ex.tex ? ex.tex.Get() : (ex.actor ? ex.actor->GetTexture() : nullptr);
			break;
		}
	}
	return t ? vtkImageData::SafeDownCast(t->GetInput()) : nullptr;
}

// Write the named object in the requested VTK format. `code` is the Save dialog's format code
// ("vtk_vti" / "vtk_vtp" / "vtk_legacy"). false + `err` when there is nothing of that shape to write.
static bool vtkioSaveObject(Scene *s, const std::string &name, const std::string &code,
                            const std::string &path, std::string &err) {
	const std::vector<float> *z = nullptr;
	int nx = 0, ny = 0;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	const bool haveGrid = vtkioResolveGrid(s, name, z, nx, ny, x0, x1, y0, y1);

	if (code == "vtk_vti") {
		// A grid layer writes its z as a float scalar; an IMAGE layer writes its texture's own RGB(A)
		// raster. Both are vtkImageData, so one format entry covers the grid and the image Save lists.
		vtkSmartPointer<vtkImageData> im = haveGrid ? vtkioGridToImage(*z, nx, ny, x0, x1, y0, y1, name)
		                                            : vtkioResolveImage(s, name);
		if (!im) { err = "no grid or image raster in this layer (a mesh saves as .vtp)"; return false; }
		vtkNew<vtkXMLImageDataWriter> w;
		w->SetFileName(path.c_str());
		w->SetInputData(im);
		w->SetDataModeToBinary();
		return w->Write() != 0 ? true : (err = "vtkXMLImageDataWriter failed", false);
	}
	if (code == "vtk_vtp") {
		vtkSmartPointer<vtkPolyData> pd = vtkioResolvePolyData(s, name);
		if (!pd) { err = "this layer has no renderable polydata to write"; return false; }
		vtkNew<vtkXMLPolyDataWriter> w;
		w->SetFileName(path.c_str());
		w->SetInputData(pd);
		w->SetDataModeToBinary();
		return w->Write() != 0 ? true : (err = "vtkXMLPolyDataWriter failed", false);
	}
	if (code == "vtk_legacy") {
		vtkNew<vtkDataSetWriter> w;                       // legacy .vtk takes either dataset kind
		w->SetFileName(path.c_str());
		w->SetFileTypeToBinary();
		vtkSmartPointer<vtkImageData> img = haveGrid ? nullptr : vtkioResolveImage(s, name);
		if (haveGrid)   w->SetInputData(vtkioGridToImage(*z, nx, ny, x0, x1, y0, y1, name));
		else if (img)   w->SetInputData(img);
		else {
			vtkSmartPointer<vtkPolyData> pd = vtkioResolvePolyData(s, name);
			if (!pd) { err = "nothing renderable in this layer to write"; return false; }
			w->SetInputData(pd);
		}
		return w->Write() != 0 ? true : (err = "vtkDataSetWriter failed", false);
	}
	err = "unknown VTK format code";
	return false;
}

// Does this Save format code belong to us (rather than Julia's GMT/GDAL writers)?
static bool vtkioIsVtkSaveCode(const QString &code) { return code.startsWith(QLatin1String("vtk_")); }
