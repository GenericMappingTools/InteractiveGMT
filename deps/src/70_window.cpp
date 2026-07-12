// Busy (indeterminate) progress dialog around a blocking Julia call (Seismicity base-map/fetch
// path). Reuses `g_progress` (30_app.cpp, earlier in this TU) — the same QProgressDialog the
// gmtvtk_progress_* C API (90_c_api.cpp, later in this TU) drives for the Okada patch loop, just
// with range (0,0) for Qt's indeterminate busy bar instead of a counted range.
static void showBusyDialog(const char *title) {
	if (!QApplication::instance()) return;
	if (g_progress) delete g_progress;
	g_progress = new QProgressDialog();
	g_progress->setWindowTitle(title);
	g_progress->setRange(0, 0);
	g_progress->setCancelButton(nullptr);
	g_progress->setWindowModality(Qt::ApplicationModal);
	g_progress->show();
	QApplication::processEvents();
}
static void closeBusyDialog() {
	if (g_progress) { g_progress->close(); delete g_progress; g_progress = nullptr; }
	QApplication::processEvents();
}

// ===== tiled-LOD pyramid: build + per-frame screen-space-error refinement ==================
// Quadtree over the full grid; each node renders its region at a stride chosen so its sampled span
// is <= ~512 (leaf = stride 1 = full res). Per camera move, refineNode keeps each visible branch at
// the coarsest LOD whose node gap still projects to <= tau pixels, building tile actors lazily and
// LRU-evicting offscreen ones past a byte budget. Only the data layer (Scene::gridZ) is always
// resident; render geometry is bounded regardless of grid size.

// ============================================================================================
// World Topo Tiles basemap picker — port of Mirone's bg_map.m (src_figs/bg_map.m).
// A world image (data/etopo4_logo.jpg) overlaid with a 4x8 grid of 45-deg tiles. Clicking a tile
// yields its geographic region; "World Map" mode yields the whole map; the [0 360] radio sets a
// wrap flag. The result "W/E/S/N/wrap" is handed to Julia (g_juliaBaseMap), which crops the big
// data/etopo4.jpg and adds it as a referenced (WGS84) flat image. exec() returns Accepted with
// `region` filled, or Rejected if the user just closed the window.  (Rubber-band sub-region
// selection from bg_map.m's toggle_region is not ported yet — see .wolf/knowledge/mirone-port.md.)
// ============================================================================================
class BaseMapArea : public QWidget {       // the clickable map; no Q_OBJECT (only paint/mouse overrides)
public:
	QPixmap logo;
	std::function<bool()> isTiles;                                            // draw + hit-test grid when true
	std::function<bool()> isRect;                                             // rubber-band sub-region mode
	std::function<void(double,double,double,double,const QString&)> onPick;   // (W,E,S,N, name)
	explicit BaseMapArea(QWidget *p) : QWidget(p) { setMinimumSize(512, 256); }
	// widget pixel (x,y) -> geographic lon/lat over the whole earth [-180 180]/[-90 90] (UL origin)
	double pxLon(double x) const { return -180.0 + x / width()  * 360.0; }
	double pxLat(double y) const { return   90.0 - y / height() * 180.0; }
protected:
	bool   dragging = false;
	QPointF p0, p1;                                                           // rubber-band corners (px)
	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		if (!logo.isNull()) g.drawPixmap(rect(), logo);
		const bool rectMode = isRect && isRect();
		if (!rectMode && isTiles && isTiles()) {
			QPen pen(QColor(230, 230, 230)); pen.setWidth(1); g.setPen(pen);
			const double w = width(), h = height();
			for (int n = 1; n < 8; ++n) g.drawLine(QPointF(n * w / 8, 0), QPointF(n * w / 8, h));
			for (int m = 1; m < 4; ++m) g.drawLine(QPointF(0, m * h / 4), QPointF(w, m * h / 4));
		}
		if (dragging) {                                                       // draw the rubber band
			QPen pen(QColor(255, 80, 80)); pen.setWidth(2); g.setPen(pen);
			g.setBrush(QColor(255, 80, 80, 40));
			g.drawRect(QRectF(p0, p1).normalized());
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (!onPick) return;
		if (isRect && isRect()) { dragging = true; p0 = p1 = e->position(); update(); return; }
		if (isTiles && isTiles()) {
			int col = std::clamp(int(e->position().x() * 8 / width()),  0, 7);   // 0..7 left->right
			int row = std::clamp(int(e->position().y() * 4 / height()), 0, 3);   // 0..3 top->bottom
			double W = -180.0 + col * 45.0, E = W + 45.0;
			double N =   90.0 - row * 45.0, S = N - 45.0;
			onPick(W, E, S, N, QString("%1x%2").arg(row + 1).arg(col + 1));      // name = "row x col" (1-based)
		} else {
			onPick(-180.0, 180.0, -90.0, 90.0, "global");                       // whole world
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (!dragging) return;
		p1 = e->position(); update();
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (!dragging) return;
		dragging = false; p1 = e->position();
		QRectF r = QRectF(p0, p1).normalized();
		if (r.width() < 4 || r.height() < 4) { update(); return; }              // ignore a tap / tiny box
		double W = pxLon(r.left()),   E = pxLon(r.right());
		double N = pxLat(r.top()),    S = pxLat(r.bottom());                    // top px = larger lat
		W = std::clamp(W, -180.0, 180.0); E = std::clamp(E, -180.0, 180.0);
		S = std::clamp(S,  -90.0,  90.0); N = std::clamp(N,  -90.0,  90.0);
		onPick(W, E, S, N, "region");
	}
};

class BaseMapPicker : public QDialog {
public:
	QString region;                            // "W/E/S/N/wrap" once a tile/map is clicked, else empty
	BaseMapPicker(QWidget *parent, const QPixmap &logo) : QDialog(parent) {
		setWindowTitle("World Topo Tiles");
		auto *v   = new QVBoxLayout(this);
		auto *top = new QHBoxLayout();
		auto *rTiles = new QRadioButton("World Map Tiles", this); rTiles->setChecked(true);
		auto *rWorld = new QRadioButton("World Map", this);
		auto *g1 = new QButtonGroup(this); g1->addButton(rTiles); g1->addButton(rWorld);
		auto *r180 = new QRadioButton("[-180 180]", this); r180->setChecked(true);
		auto *r360 = new QRadioButton("[0 360]", this);
		auto *g2 = new QButtonGroup(this); g2->addButton(r180); g2->addButton(r360);
		// middle: a checkable rectangle button -> rubber-band an arbitrary sub-region (Mirone's toggle_region)
		auto *rRect = new QToolButton(this);
		rRect->setCheckable(true);
		rRect->setToolTip("Draw a rectangle to pick an arbitrary region (no tiles)");
		{	QPixmap pm(28, 18); pm.fill(Qt::transparent);
			QPainter ic(&pm); QPen pen(QColor(40, 40, 40)); pen.setWidth(2); ic.setPen(pen);
			ic.drawRect(4, 4, 20, 10); ic.end();
			rRect->setIcon(QIcon(pm)); rRect->setIconSize(QSize(28, 18));
		}
		top->addWidget(rTiles); top->addWidget(rWorld); top->addStretch();
		top->addWidget(rRect);  top->addStretch();
		top->addWidget(r180);   top->addWidget(r360);
		v->addLayout(top);
		auto *map = new BaseMapArea(this);
		map->logo    = logo;
		map->isTiles = [rTiles]() { return rTiles->isChecked(); };
		map->isRect  = [rRect]()  { return rRect->isChecked(); };
		map->onPick  = [this, r360](double W, double E, double S, double N, const QString &name) {
			region = QString("%1/%2/%3/%4/%5/%6").arg(W).arg(E).arg(S).arg(N)
			                                     .arg(r360->isChecked() ? 1 : 0).arg(name);
			accept();
		};
		v->addWidget(map, 1);
		QObject::connect(rTiles, &QRadioButton::toggled, map, [map]() { map->update(); });
		QObject::connect(rRect,  &QToolButton::toggled,  map, [map](bool on) {
			map->setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor); map->update();
		});
		resize(680, 380);
	}
};

// ============================================================================================
// Tiles Tool — port of Mirone's tiles_tool.m (src_figs/tiles_tool.m), MINUS the download/mosaic
// machinery (url2image), which is replaced by GMT.jl's `mosaic`. An interactive world map (the bundled
// data/etopo4.jpg, equirectangular over [-180 180]/[-90 90]) under a refinable web-tile mesh. Raising
// the "Zoom Level" slider zooms the view IN (toward the anchor, else the view centre) and refines the
// mesh to that tile zoom. The user clicks TWO diagonal tiles to bracket a rectangle; GO (green arrow)
// hands that bbox + zoom + provider + cache + Mercator flag to Julia (g_juliaTiles op "go"), which
// builds the final mosaic (GMT.mosaic, two zoom levels coarser) and opens it in a new viewer. Provider
// drop-down replaces the (1)(2)(3) image toggles + tilesServers.txt; Cache / Mercator-Geogs / anchor
// mirror the original. No Q_OBJECT/moc (only paint/mouse overrides; UI wired via lambdas to update()).
// A minimal, crisp anchor drawn from primitives (the font ⚓ glyph rendered fuzzy/ornate). Centred
// at `c`, half-height `h`. Used both for the map marker and the toolbar button icon so they match.
static void paintAnchor(QPainter &g, QPointF c, double h, const QPen &pen) {
	g.setPen(pen); g.setBrush(Qt::NoBrush);
	g.drawEllipse(c + QPointF(0, -h - 1), h * 0.30, h * 0.30);                  // ring
	g.drawLine(c + QPointF(0, -h + 2), c + QPointF(0, h));                      // shank
	g.drawLine(c + QPointF(-h * 0.6, -h + 3), c + QPointF(h * 0.6, -h + 3));    // stock (crossbar)
	g.drawArc(QRectF(c.x() - h, c.y(), 2 * h, h * 1.4), 200 * 16, 140 * 16);    // flukes (curved base)
}

// ============================================================================================
class TilesArea : public QWidget {       // the clickable map: etopo base + refinable tile mesh
public:
	QPixmap world;                                   // full-res equirectangular etopo, covers [-180 180]/[-90 90]
	double  vW = -180, vE = 180, vS = -85, vN = 85;  // current geographic view window
	int     zoom = 1;                                // web-tile zoom level of the mesh
	bool    hasAnchor = false;  double anchorLon = 0, anchorLat = 0;
	bool    anchorMode = false;                      // next click sets the anchor instead of a corner tile
	bool    draggingAnchor = false;                  // grabbed the anchor marker -> drag it with the mouse
	std::vector<QPoint> sel;                         // selected tiles (tile X,Y at `zoom`); a click toggles one
	std::function<void()> onViewChanged;             // notify the picker (zoom label, bg request)
	std::function<void()> onAnchorPlaced;            // notify the picker the anchor was dropped (un-check the button)
	// Phase 2: a sharper coarser-mosaic background fetched (by Julia) for the current view at high zoom,
	// painted over the etopo base and under the mesh. Covers its own geo-extent [bgW..bgE]/[bgS..bgN].
	QPixmap bg;  bool hasBg = false;  double bgW = 0, bgE = 0, bgS = 0, bgN = 0;
	explicit TilesArea(QWidget *p) : QWidget(p) { setMinimumSize(600, 320); }

	void setBg(const QString &path, double W, double E, double S, double N) {
		QPixmap pm(path);
		if (pm.isNull()) return;                     // bad path / unreadable -> keep whatever we had
		bg = pm; bgW = W; bgE = E; bgS = S; bgN = N; hasBg = true; update();
	}
	void clearBg() { if (hasBg) { hasBg = false; bg = QPixmap(); update(); } }

	// Web-Mercator slippy-tile math (matches GMT.mosaic's quadtree): n = 2^zoom tiles per axis.
	static constexpr double PI = 3.14159265358979323846;
	static double tileX2lon(double x, int z) { return x / double(1u << z) * 360.0 - 180.0; }
	static double tileY2lat(double y, int z) { double m = PI * (1.0 - 2.0 * y / double(1u << z)); return std::atan(std::sinh(m)) * 180.0 / PI; }
	static int    lon2tileX(double lon, int z) { return int(std::floor((lon + 180.0) / 360.0 * double(1u << z))); }
	static int    lat2tileY(double lat, int z) { double r = lat * PI / 180.0; return int(std::floor((1.0 - std::asinh(std::tan(r)) / PI) / 2.0 * double(1u << z))); }
	// geographic <-> widget pixel for the current view (equirectangular display)
	double lon2px(double lon) const { return (lon - vW) / (vE - vW) * width(); }
	double lat2py(double lat) const { return (vN - lat) / (vN - vS) * height(); }
	double px2lon(double x)   const { return vW + x / width()  * (vE - vW); }
	double px2lat(double y)   const { return vN - y / height() * (vN - vS); }

	// Re-frame the view for a new tile zoom: keep ~targetTiles across, centred on the anchor (else the
	// current view centre), latitude span following the widget aspect so the map isn't distorted. The
	// view window is shifted (not squashed) when it would overrun the world edges.
	void reframe(int z) {
		zoom = std::clamp(z, 1, 19);
		double cLon = hasAnchor ? anchorLon : (vW + vE) / 2.0;
		double cLat = hasAnchor ? anchorLat : (vS + vN) / 2.0;
		const double targetTiles = 10.0;
		double lonSpan = std::min(360.0, targetTiles * 360.0 / double(1u << zoom));
		double latSpan = std::min(170.0, lonSpan * double(height()) / double(std::max(1, width())));
		vW = cLon - lonSpan / 2; vE = cLon + lonSpan / 2;
		if (vW < -180) { vE += -180 - vW; vW = -180; }   if (vE > 180) { vW -= vE - 180; vE = 180; }
		vW = std::max(vW, -180.0); vE = std::min(vE, 180.0);
		vS = cLat - latSpan / 2; vN = cLat + latSpan / 2;
		if (vS < -85)  { vN += -85 - vS;  vS = -85; }    if (vN > 85)  { vS -= vN - 85;  vN = 85; }
		vS = std::max(vS, -85.0);  vN = std::min(vN, 85.0);
		sel.clear();                                     // a re-zoom invalidates the old tile-index selection
		hasBg = false; bg = QPixmap();                   // and the old background (the picker refetches on release)
		update(); if (onViewChanged) onViewChanged();
	}
	// Pan the view (keeping the current span) so it is centred on (cLon,cLat), clamped inside the world.
	// The scrollbars drive this; the stale background is dropped and the picker refetches it.
	void panTo(double cLon, double cLat) {
		double lonSpan = vE - vW, latSpan = vN - vS;
		double w0 = std::clamp(cLon - lonSpan / 2, -180.0, 180.0 - lonSpan);
		double s0 = std::clamp(cLat - latSpan / 2,  -85.0,  85.0 - latSpan);
		vW = w0; vE = w0 + lonSpan; vS = s0; vN = s0 + latSpan;
		hasBg = false; bg = QPixmap();
		update(); if (onViewChanged) onViewChanged();
	}
protected:
	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		g.setRenderHint(QPainter::Antialiasing, true);       // crisp marker glyph + mesh lines
		g.setRenderHint(QPainter::TextAntialiasing, true);
		g.setRenderHint(QPainter::SmoothPixmapTransform, true);
		g.fillRect(rect(), QColor(20, 30, 50));
		if (!world.isNull()) {                           // etopo cropped to the current view
			QRectF src((vW + 180) / 360.0 * world.width(), (90 - vN) / 180.0 * world.height(),
			           (vE - vW) / 360.0 * world.width(), (vN - vS) / 180.0 * world.height());
			g.drawPixmap(rect(), world, src);
		}
		if (hasBg && !bg.isNull()) {                     // sharper coarser-mosaic bg over its geo-extent
			QRectF tgt(lon2px(bgW), lat2py(bgN), lon2px(bgE) - lon2px(bgW), lat2py(bgS) - lat2py(bgN));
			g.drawPixmap(tgt, bg, QRectF(bg.rect()));
		}
		// the refinable tile mesh: every web-tile boundary intersecting the view at `zoom`
		g.setPen(QPen(QColor(0, 0, 0, 160), 1));
		int x0 = lon2tileX(vW, zoom), x1 = lon2tileX(vE, zoom);
		int y0 = lat2tileY(vN, zoom), y1 = lat2tileY(vS, zoom);     // vN (top) -> smaller tile Y
		for (int tx = x0; tx <= x1 + 1; ++tx) { double X = lon2px(tileX2lon(tx, zoom)); g.drawLine(QPointF(X, 0), QPointF(X, height())); }
		for (int ty = y0; ty <= y1 + 1; ++ty) { double Y = lat2py(tileY2lat(ty, zoom)); g.drawLine(QPointF(0, Y), QPointF(width(), Y)); }
		// selected tiles (a click toggles one): each highlighted yellow. GO uses their union bbox.
		g.setPen(QPen(QColor(255, 210, 0), 2)); g.setBrush(QColor(255, 230, 0, 90));
		for (const QPoint &t : sel) {
			double L = lon2px(tileX2lon(t.x(), zoom)), R = lon2px(tileX2lon(t.x() + 1, zoom));
			double T = lat2py(tileY2lat(t.y(), zoom)), B = lat2py(tileY2lat(t.y() + 1, zoom));
			g.drawRect(QRectF(QPointF(L, T), QPointF(R, B)));
		}
		if (hasAnchor) {                                 // the zoom-anchor marker — same simple anchor as the button
			QPointF p(lon2px(anchorLon), lat2py(anchorLat));
			QPen halo(Qt::white, 3.0); halo.setCapStyle(Qt::RoundCap); halo.setJoinStyle(Qt::RoundJoin);
			QPen ink(Qt::black, 1.5);  ink.setCapStyle(Qt::RoundCap);  ink.setJoinStyle(Qt::RoundJoin);
			paintAnchor(g, p, 4.7, halo);                // white halo for legibility over the map (2/3 of the button size)
			paintAnchor(g, p, 4.7, ink);
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		double lon = px2lon(e->position().x()), lat = px2lat(e->position().y());
		if (anchorMode) { hasAnchor = true; anchorLon = lon; anchorLat = lat; anchorMode = false;
		                  setCursor(Qt::ArrowCursor); if (onAnchorPlaced) onAnchorPlaced(); update(); return; }
		if (hasAnchor) {                               // grab the anchor marker (within 9 px) to drag it
			double ax = lon2px(anchorLon), ay = lat2py(anchorLat);
			if (std::hypot(e->position().x() - ax, e->position().y() - ay) < 9.0) {
				draggingAnchor = true; setCursor(Qt::ClosedHandCursor); return;
			}
		}
		QPoint key(lon2tileX(lon, zoom), lat2tileY(lat, zoom));
		auto it = std::find(sel.begin(), sel.end(), key);
		if (it != sel.end()) sel.erase(it);            // second click on a selected tile -> deselect it
		else                 sel.push_back(key);       // otherwise select it
		update();
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (!draggingAnchor) return;                   // drag the grabbed anchor to follow the cursor
		anchorLon = std::clamp(px2lon(e->position().x()), vW, vE);
		anchorLat = std::clamp(px2lat(e->position().y()), vS, vN);
		update();
	}
	void mouseReleaseEvent(QMouseEvent *) override {
		if (draggingAnchor) { draggingAnchor = false; setCursor(Qt::ArrowCursor); }
	}
};

class TilesPicker : public QDialog {
public:
	Scene        *scene;
	TilesArea    *map;
	QComboBox    *cboProvider;
	QComboBox    *cboCache;                           // editable cache-dir box + remembered MRU
	QRadioButton *rMerc;
	QSlider      *slZoom;
	QLabel       *lblZoom;
	QTimer       *bgTimer;                            // debounces the high-zoom background refetch
	QScrollBar   *hbar, *vbar;                        // pan the view once the mesh is zoomed in
	QPlainTextEdit *dlLog;                            // collapsible per-tile download/cache console
	// Append one line to the Downloads-info console (called from Julia via gmtvtk_tiles_log).
	void logDownload(const QString &line) { if (dlLog) dlLog->appendPlainText(line); }
	// Cache box: the default entry shows ~/.gmt but is sent to GMT as cache="gmt" (-> ~/.gmt/cache_tileserver).
	QString gmtCacheLabel() const { return QDir::homePath() + "/.gmt"; }
	QString cacheSendValue() const {
		QString c = cboCache->currentText().trimmed();
		return (c == gmtCacheLabel()) ? QString("gmt") : c;
	}
	// Persist every cache dir the user picked/typed (except the synthetic ~/.gmt default) across sessions.
	void saveCacheList() {
		QStringList items;
		for (int i = 0; i < cboCache->count(); ++i)
			if (cboCache->itemText(i) != gmtCacheLabel()) items << cboCache->itemText(i);
		QSettings st = igmtSettings();
		st.setValue("tiles/cacheDirs", items);
	}
	void rememberCache(const QString &dir) {
		if (dir.isEmpty() || dir == gmtCacheLabel()) return;
		if (cboCache->findText(dir) < 0) cboCache->insertItem(1, dir);   // keep ~/.gmt first
		cboCache->setCurrentText(dir);
		saveCacheList();
	}
	TilesPicker(QWidget *parent, Scene *s, const QPixmap &world) : QDialog(parent), scene(s) {
		setWindowTitle("Tiles Tool");
		auto *v = new QVBoxLayout(this);
		// --- top toolbar: provider drop-down, anchor, GO, help ---
		auto *top = new QHBoxLayout();
		cboProvider = new QComboBox(this);
		cboProvider->addItems({"Bing", "Google", "OSM", "Esri"});
		cboProvider->setToolTip("Web tile provider for the final mosaic (replaces the (1)(2)(3) image toggles)");
		auto *btnAnchor = new QToolButton(this);
		{                                                // icon = the SAME simple anchor as the map marker
			QPixmap pm(18, 18); pm.fill(Qt::transparent);
			QPainter pg(&pm); pg.setRenderHint(QPainter::Antialiasing, true);
			paintAnchor(pg, QPointF(9, 9), 6.0, QPen(Qt::black, 1.5));
			btnAnchor->setIcon(QIcon(pm));
		}
		btnAnchor->setCheckable(true);                   // stays highlighted while the user picks where to drop it
		btnAnchor->setToolTip("Set zoom anchor point: click this, then click the map");
		auto *btnGo = new QToolButton(this); btnGo->setText(QString::fromUtf8("\xE2\x96\xB6") + " GO");
		btnGo->setToolTip("Build the mosaic for the selected tiles (pick two diagonal corners first)");
		auto *btnHelp = new QToolButton(this); btnHelp->setText("?");
		top->addWidget(new QLabel("Provider", this)); top->addWidget(cboProvider);
		top->addStretch(); top->addWidget(btnAnchor); top->addWidget(btnGo); top->addWidget(btnHelp);
		v->addLayout(top);
		// --- the interactive map + pan scrollbars (vertical at the right, horizontal below) ---
		map = new TilesArea(this); map->world = world;
		hbar = new QScrollBar(Qt::Horizontal, this);
		vbar = new QScrollBar(Qt::Vertical, this);
		auto *mg = new QGridLayout(); mg->setSpacing(0); mg->setContentsMargins(0, 0, 0, 0);
		mg->addWidget(map, 0, 0); mg->addWidget(vbar, 0, 1); mg->addWidget(hbar, 1, 0);
		v->addLayout(mg, 1);
		// --- bottom: zoom slider + level text, Mercator/Geogs, cache directory ---
		auto *bot = new QHBoxLayout();
		bot->addWidget(new QLabel("Zoom Level", this));
		// arrow buttons at the slider tips for fine +/-1 zoom steps (auto-repeat on hold), as in Mirone.
		auto *btnZmDn = new QToolButton(this); btnZmDn->setArrowType(Qt::LeftArrow);  btnZmDn->setAutoRepeat(true);
		auto *btnZmUp = new QToolButton(this); btnZmUp->setArrowType(Qt::RightArrow); btnZmUp->setAutoRepeat(true);
		btnZmDn->setToolTip("Zoom out one level"); btnZmUp->setToolTip("Zoom in one level");
		btnZmDn->setFixedWidth(18); btnZmUp->setFixedWidth(18);
		slZoom = new QSlider(Qt::Horizontal, this); slZoom->setRange(1, 19); slZoom->setValue(1);
		slZoom->setFixedWidth(200);
		lblZoom = new QLabel("1", this); lblZoom->setFixedWidth(24);
		// arrows flush against the slider tips: a zero-spacing sub-layout (the bottom row's spacing
		// would otherwise leave a gap between each arrow and the slider).
		auto *zl = new QHBoxLayout(); zl->setSpacing(0); zl->setContentsMargins(0, 0, 0, 0);
		zl->addWidget(btnZmDn); zl->addWidget(slZoom); zl->addWidget(btnZmUp);
		bot->addLayout(zl); bot->addWidget(lblZoom);
		rMerc = new QRadioButton("Mercator", this);
		auto *rGeog = new QRadioButton("Geogs", this); rGeog->setChecked(true);
		auto *gMode = new QButtonGroup(this); gMode->addButton(rMerc); gMode->addButton(rGeog);
		bot->addSpacing(12); bot->addWidget(rMerc); bot->addWidget(rGeog);
		bot->addStretch();
		bot->addWidget(new QLabel("Cache", this));
		cboCache = new QComboBox(this);
		cboCache->setEditable(true);
		cboCache->setMinimumWidth(300);                  // long box (replaces the narrow line edit)
		cboCache->setInsertPolicy(QComboBox::NoInsert);  // we manage the list ourselves (rememberCache)
		cboCache->addItem(gmtCacheLabel());              // default entry: ~/.gmt (maps to GMT cache="gmt")
		{                                                // restore previously used cache dirs (QSettings MRU)
			QSettings st = igmtSettings();
			for (const QString &d : st.value("tiles/cacheDirs").toStringList())
				if (!d.isEmpty() && cboCache->findText(d) < 0) cboCache->addItem(d);
		}
		cboCache->setCurrentIndex(0);
		cboCache->setToolTip(
			"Directory where downloaded tiles are cached. The default ~/.gmt maps to GMT's "
			"~/.gmt/cache_tileserver. Pick more dirs with '...'; used dirs are remembered across "
			"sessions in this drop-down. Leave empty to use the system TMP directory.");
		auto *btnDir = new QToolButton(this); btnDir->setText("...");  btnDir->setToolTip("Select a cache directory");
		bot->addWidget(cboCache, 1); bot->addWidget(btnDir);
		v->addLayout(bot);
		// --- collapsible "Downloads info" console (folded by default) — the per-tile download / cache
		//     messages routed here from GMT.mosaic, since the iGMT viewer's Errors tab isn't in view. ---
		auto *dlHdr = new QHBoxLayout();
		auto *btnDl = new QToolButton(this);
		btnDl->setCheckable(true); btnDl->setAutoRaise(true);
		btnDl->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		btnDl->setArrowType(Qt::RightArrow); btnDl->setText(" Downloads info");
		btnDl->setToolTip("Show/hide the per-tile download & cache messages");
		auto *btnDlClr = new QToolButton(this); btnDlClr->setText("Clear");
		btnDlClr->setToolTip("Clear the downloads console");
		dlHdr->addWidget(btnDl); dlHdr->addStretch(); dlHdr->addWidget(btnDlClr);
		v->addLayout(dlHdr);
		dlLog = new QPlainTextEdit(this);
		dlLog->setReadOnly(true); dlLog->setMaximumBlockCount(5000); dlLog->setFixedHeight(120);
		dlLog->setVisible(false);                         // folded by default
		{ QFont mono("Consolas"); mono.setStyleHint(QFont::Monospace); mono.setPointSize(8); dlLog->setFont(mono); }
		v->addWidget(dlLog);
		QObject::connect(btnDl, &QToolButton::toggled, this, [this, btnDl](bool on) {
			const int dh = dlLog->height() + 6;          // console height + layout spacing
			btnDl->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
			if (on) { dlLog->setVisible(true);  resize(width(), height() + dh); }   // grow window, keep the map size
			else    { resize(width(), height() - dh); dlLog->setVisible(false); }   // shrink window back
		});
		QObject::connect(btnDlClr, &QToolButton::clicked, this, [this]() { dlLog->clear(); });
		// --- wiring ---
		bgTimer = new QTimer(this); bgTimer->setSingleShot(true);
		QObject::connect(bgTimer, &QTimer::timeout, this, [this]() { requestBg(); });
		// ANY view change (zoom OR pan) updates the zoom label, re-syncs the scrollbars, and (debounced
		// 350 ms) refetches the high-zoom background, so the bg tracks both zoom and pan. reframe()/panTo()
		// drop the stale bg meanwhile, so it never lags behind the view.
		map->onViewChanged = [this]() { lblZoom->setText(QString::number(map->zoom)); syncBars(); bgTimer->start(350); };
		QObject::connect(slZoom, &QSlider::valueChanged, this, [this](int z) { map->reframe(z); });
		QObject::connect(btnZmDn, &QToolButton::clicked, this, [this]() { slZoom->setValue(slZoom->value() - 1); });
		QObject::connect(btnZmUp, &QToolButton::clicked, this, [this]() { slZoom->setValue(slZoom->value() + 1); });
		QObject::connect(hbar, &QScrollBar::valueChanged, this, [this](int val) { onHBar(val); });
		QObject::connect(vbar, &QScrollBar::valueChanged, this, [this](int val) { onVBar(val); });
		// Checkable: stays down (highlighted) while the user decides where to click. Toggle drives the
		// pick mode; dropping the anchor (or toggling off) clears it.
		map->onAnchorPlaced = [btnAnchor]() { btnAnchor->setChecked(false); };
		QObject::connect(btnAnchor, &QToolButton::toggled, this, [this](bool on) {
			map->anchorMode = on; map->setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
		});
		QObject::connect(btnGo, &QToolButton::clicked, this, [this]() { doGo(); });
		QObject::connect(btnHelp, &QToolButton::clicked, this, [this]() {
			QMessageBox::information(this, "Tiles Tool",
				"Raise the Zoom Level slider to refine the tile mesh and zoom the view in. Click tiles to "
				"select them (yellow); click a selected tile again to deselect it. Hit GO to build the "
				"mosaic of the selected tiles' bounding box (GMT.mosaic, two zoom levels coarser) in a new "
				"window. Use the anchor button to fix the zoom centre. Provider, Cache directory and "
				"Mercator/Geogs map to GMT.mosaic options.");
		});
		QObject::connect(btnDir, &QToolButton::clicked, this, [this]() {
			QString start = cacheSendValue(); if (start == "gmt") start = QDir::homePath();
			QString d = QFileDialog::getExistingDirectory(this, "Select cache directory", start);
			if (!d.isEmpty()) rememberCache(d);
		});
		map->reframe(1);
		resize(820, 520);
	}
private:
	static constexpr int BG_ZOOM_MIN = 9;   // fetch a sharper background only past this tile zoom (Mirone: >8)

	// Keep the pan scrollbars in step with the current view (called from onViewChanged). A bar is
	// disabled when the whole world is visible on its axis. blockSignals avoids a pan<->sync loop.
	void syncBars() {
		double lonSpan = map->vE - map->vW, latSpan = map->vN - map->vS;
		hbar->blockSignals(true);
		bool hp = lonSpan < 359.999;
		hbar->setEnabled(hp); hbar->setRange(0, hp ? 1000 : 0);
		hbar->setPageStep(hp ? int(1000.0 * lonSpan / 360.0) : 1000);
		if (hp) { double cLon = (map->vW + map->vE) / 2;
			hbar->setValue(int(std::clamp((cLon - (-180 + lonSpan / 2)) / (360 - lonSpan) * 1000.0, 0.0, 1000.0))); }
		hbar->blockSignals(false);
		vbar->blockSignals(true);
		const double worldLat = 170.0;                  // the picker spans latitude -85..85
		bool vp = latSpan < worldLat - 0.001;
		vbar->setEnabled(vp); vbar->setRange(0, vp ? 1000 : 0);
		vbar->setPageStep(vp ? int(1000.0 * latSpan / worldLat) : 1000);
		if (vp) { double cLat = (map->vS + map->vN) / 2;   // value 0 = north (top of the bar)
			vbar->setValue(int(std::clamp(((85 - latSpan / 2) - cLat) / (worldLat - latSpan) * 1000.0, 0.0, 1000.0))); }
		vbar->blockSignals(false);
	}
	void onHBar(int val) {
		double lonSpan = map->vE - map->vW; if (lonSpan >= 360) return;
		map->panTo((-180 + lonSpan / 2) + val / 1000.0 * (360 - lonSpan), (map->vS + map->vN) / 2);
	}
	void onVBar(int val) {
		double latSpan = map->vN - map->vS; const double worldLat = 170.0; if (latSpan >= worldLat) return;
		map->panTo((map->vW + map->vE) / 2, (85 - latSpan / 2) - val / 1000.0 * (worldLat - latSpan));
	}

	// At high zoom the etopo base is too coarse, so ask Julia (op "bg") for a coarser mosaic (two-to-three
	// zoom levels down) covering the current view; it writes a PNG and pushes it back via gmtvtk_tiles_set_bg.
	// Below the threshold the etopo base suffices, so just drop any stale background. Synchronous: Julia
	// fetches + calls back before returning, so `this` stays valid throughout.
	void requestBg() {
		if (!g_juliaTiles) return;
		if (map->zoom < BG_ZOOM_MIN) { map->clearBg(); return; }
		QString params = QString("bg;%1/%2/%3/%4;%5;%6;%7;%8")
			.arg(map->vW, 0, 'f', 8).arg(map->vE, 0, 'f', 8).arg(map->vS, 0, 'f', 8).arg(map->vN, 0, 'f', 8)
			.arg(map->zoom).arg(cboProvider->currentText())
			.arg(cacheSendValue()).arg(rMerc->isChecked() ? 1 : 0);
		QApplication::processEvents();                   // paint before the blocking call (progress -> Downloads console)
		g_juliaTiles(scene, this, params.toUtf8().constData());
	}
	void doGo() {
		if (map->sel.empty()) {
			QMessageBox::warning(this, "Tiles Tool",
				"Select one or more tiles first (click squares to select; click again to deselect).");
			return;
		}
		int xa = map->sel[0].x(), xb = xa, ya = map->sel[0].y(), yb = ya;
		for (const QPoint &t : map->sel) { xa = std::min(xa, t.x()); xb = std::max(xb, t.x());
		                                   ya = std::min(ya, t.y()); yb = std::max(yb, t.y()); }
		double W = TilesArea::tileX2lon(xa, map->zoom), E = TilesArea::tileX2lon(xb + 1, map->zoom);
		double N = TilesArea::tileY2lat(ya, map->zoom), S = TilesArea::tileY2lat(yb + 1, map->zoom);
		QString params = QString("go;%1/%2/%3/%4;%5;%6;%7;%8")
			.arg(W, 0, 'f', 8).arg(E, 0, 'f', 8).arg(S, 0, 'f', 8).arg(N, 0, 'f', 8)
			.arg(map->zoom).arg(cboProvider->currentText())
			.arg(cacheSendValue()).arg(rMerc->isChecked() ? 1 : 0);
		rememberCache(cboCache->currentText());          // persist a typed cache dir across sessions
		// The fetch blocks the UI thread; note it in the Downloads console + paint before the blocking call
		// (the first run also compiles), so a watcher sees it isn't hung.
		logDownload("Building mosaic — downloading tiles…  (the first run also compiles; please wait)");
		QApplication::processEvents();
		if (g_juliaTiles) g_juliaTiles(scene, this, params.toUtf8().constData());
	}
};

// ============================================================================================
// Background region dialog (File > Background region). A tiny form mirroring Mirone's empty-figure
// limits chooser: a compass-laid-out W/E/S/N (N on top, W/E flanking, S below), an "Is Geographic?"
// checkbox (default on) and OK. exec() returns Accepted with `region` = "W/E/S/N/geographic", which
// the host hands to Julia (g_juliaBgRegion) to open a blank white 2-D map framed to those limits.
// No Q_OBJECT/moc needed (no new signals/slots). Defaults to the whole geographic earth.
// ============================================================================================
class BgRegionDialog : public QDialog {
public:
	QString region;                              // "W/E/S/N/geographic" on OK, else empty
	BgRegionDialog(QWidget *parent) : QDialog(parent) {
		setWindowTitle("Background region");
		auto edit = [this](const QString &val) {
			auto *e = new QLineEdit(val, this);
			e->setValidator(new QDoubleValidator(e));
			e->setAlignment(Qt::AlignHCenter);
			e->setMinimumWidth(110);
			return e;
		};
		QLineEdit *eN = edit("90"), *eS = edit("-90"), *eW = edit("-180"), *eE = edit("180");

		// Compass grid: N row 0 col 1 ; W/E row 1 cols 0/2 ; S row 2 col 1.
		auto *grid = new QGridLayout();
		grid->addWidget(eN, 0, 1);
		grid->addWidget(eW, 1, 0);
		grid->addWidget(eE, 1, 2);
		grid->addWidget(eS, 2, 1);
		grid->setColumnStretch(1, 1);

		auto *geog = new QCheckBox("Is Geographic?", this);
		geog->setChecked(true);

		auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		QObject::connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
		QObject::connect(bb, &QDialogButtonBox::accepted, this, [this, eW, eE, eS, eN, geog]() {
			region = QString("%1/%2/%3/%4/%5")
			             .arg(eW->text().trimmed()).arg(eE->text().trimmed())
			             .arg(eS->text().trimmed()).arg(eN->text().trimmed())
			             .arg(geog->isChecked() ? 1 : 0);
			accept();
		});

		auto *v = new QVBoxLayout(this);
		v->addLayout(grid);
		v->addWidget(geog);
		v->addWidget(bb);
	}
};

// ============================================================================================
// Preferences dialog (File > Preferences). Layout mirrors deps/ui/preferences.ui (fixed-geometry,
// hand-coded to match the Qt Designer file). Values persist via QSettings under the "prefs/" group
// (org "InteractiveGMT", app "i'GMT") and are loaded back on each open. Read a setting anywhere via
// the prefXXX() accessors (defined in 30_app.cpp so every fragment shares them).
// Default directory lives in the dirMRU list (prefStartDir/prefDirMRU/rememberStartDir, 30_app.cpp).
// ============================================================================================

class PreferencesDialog : public QDialog {
public:
	PreferencesDialog(QWidget *parent) : QDialog(parent) {
		setWindowTitle("Preferences");
		setFixedSize(433, 250);

		// ---- Row 1: measure units | dist/azim type | azimuth direction ---------------------
		auto *lblUnits = new QLabel("Measure units", this);
		lblUnits->setGeometry(20, 10, 121, 16);
		cmbMeasureUnits = new QComboBox(this);
		cmbMeasureUnits->setGeometry(20, 30, 121, 24);
		cmbMeasureUnits->addItems({"meters", "kilometers", "nautical miles", "miles", "user-defined"});
		cmbMeasureUnits->setToolTip("Choose the units used in length calculations.");

		auto *lblDA = new QLabel("Dist/Azim type", this);
		lblDA->setGeometry(180, 10, 111, 16);
		cmbDistAzim = new QComboBox(this);
		cmbDistAzim->setGeometry(180, 30, 121, 24);
		cmbDistAzim->addItems({"Ellipsoidal", "Spherical", "Flat Earth"});
		cmbDistAzim->setToolTip("Chose the approximation used in computing distances, areas and azimuths.");

		auto *lblDir = new QLabel("Dir", this);
		lblDir->setGeometry(340, 10, 31, 16);
		cmbAzimDir = new QComboBox(this);
		cmbAzimDir->setGeometry(330, 30, 91, 24);
		cmbAzimDir->addItems({"Forward", "Backward"});
		cmbAzimDir->setToolTip("Choose between Forward and Backward azimuths");

		// ---- Row 2: default directory + browse ---------------------------------------------
		auto *lblDirec = new QLabel("Default directory", this);
		lblDirec->setGeometry(20, 72, 200, 16);
		cmbDefaultDir = new QComboBox(this);
		cmbDefaultDir->setGeometry(20, 90, 365, 24);
		cmbDefaultDir->setEditable(true);
		cmbDefaultDir->setToolTip("Loading and saving files will start at this directory by default. "
		                          "But will change for the used last directory.");
		auto *btnBrowse = new QToolButton(this);
		btnBrowse->setGeometry(392, 90, 28, 24);
		btnBrowse->setText("...");
		QObject::connect(btnBrowse, &QToolButton::clicked, this, [this]() {
			QString start = cmbDefaultDir->currentText().trimmed();
			QString d = QFileDialog::getExistingDirectory(this, "Default directory", start);
			if (!d.isEmpty()) cmbDefaultDir->setEditText(QDir::toNativeSeparators(d));
		});

		// ---- Row 3: default line thickness | line color | coastlines color -----------------
		auto *lblThk = new QLabel("Default line thickness", this);
		lblThk->setGeometry(20, 140, 141, 16);
		cmbLineThickness = new QComboBox(this);
		cmbLineThickness->setGeometry(20, 160, 131, 24);
		cmbLineThickness->addItems({"1 pt", "2 pt", "3 pt", "4 pt"});

		auto *lblCol = new QLabel("Default line color", this);
		lblCol->setGeometry(180, 140, 111, 16);
		cmbLineColor = new QComboBox(this);
		cmbLineColor->setGeometry(180, 160, 111, 24);
		// "Orange" (the program's original default line colour, 1.0/0.55/0.0) leads the list so the
		// familiar look is the default; the rest are the basic named colours (others via Line Properties).
		cmbLineColor->addItems({"Orange", "Black", "Red", "Magenta", "Cyan", "White", "Green", "Blue", "Yellow"});
		cmbLineColor->setToolTip("Line color used when creating lines/polygons.");

		auto *lblCoast = new QLabel("Coastlines color", this);
		lblCoast->setGeometry(320, 140, 111, 16);
		cmbCoastColor = new QComboBox(this);
		cmbCoastColor->setGeometry(320, 160, 101, 24);
		cmbCoastColor->addItems({"Black", "White"});
		cmbCoastColor->setToolTip("Line color used when ploting coastlines and boundaries.");

		// ---- OK -----------------------------------------------------------------------------
		auto *btnOK = new QPushButton("OK", this);
		btnOK->setGeometry(330, 210, 90, 28);
		btnOK->setDefault(true);
		QObject::connect(btnOK, &QPushButton::clicked, this, [this]() { save(); accept(); });

		load();
	}

private:
	QComboBox *cmbMeasureUnits, *cmbDistAzim, *cmbAzimDir, *cmbDefaultDir;
	QComboBox *cmbLineThickness, *cmbLineColor, *cmbCoastColor;

	// Select a combo entry by text; for the editable directory combo just set the edit text.
	static void selectText(QComboBox *c, const QString &txt) {
		int i = c->findText(txt);
		if (i >= 0) c->setCurrentIndex(i);
		else if (c->isEditable() && !txt.isEmpty()) c->setEditText(txt);
	}

	void load() {
		selectText(cmbMeasureUnits,  prefMeasureUnits());
		selectText(cmbDistAzim,      prefDistAzimType());
		selectText(cmbAzimDir,       prefAzimDir());
		// Default directory: offer the whole MRU; the head (most-recent) is the active one.
		cmbDefaultDir->addItems(prefDirMRU());
		cmbDefaultDir->setCurrentIndex(0);
		selectText(cmbLineThickness, prefLineThickness());
		selectText(cmbLineColor,     prefLineColor());
		selectText(cmbCoastColor,    prefCoastColor());
	}

	void save() {
		QSettings st = igmtSettings();
		st.setValue("prefs/measureUnits",  cmbMeasureUnits->currentText());
		st.setValue("prefs/distAzimType",  cmbDistAzim->currentText());
		st.setValue("prefs/azimDir",       cmbAzimDir->currentText());
		st.setValue("prefs/lineThickness", cmbLineThickness->currentText());
		st.setValue("prefs/lineColor",     cmbLineColor->currentText());
		st.setValue("prefs/coastColor",    cmbCoastColor->currentText());
		// Default directory: push the chosen folder to the front of the MRU (also syncs defaultDir).
		prefPushDir(cmbDefaultDir->currentText().trimmed());
	}
};

// ============================================================================================
// Info text popup (toolbar 'i' button). A NON-modal read-only monospace window showing the
// grdinfo / gdalinfo report for the active grid/image, so it can stay open beside the view.
// Self-deletes on close (WA_DeleteOnClose). `title` distinguishes the two reporters.
// ============================================================================================
static void showInfoText(QWidget *parent, const QString &title, const QString &text) {
	QDialog *dlg = new QDialog(parent);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle("Info — " + title);
	dlg->resize(580, 440);
	auto *v = new QVBoxLayout(dlg);
	auto *te = new QPlainTextEdit(dlg);
	te->setReadOnly(true);
	te->setLineWrapMode(QPlainTextEdit::NoWrap);
	QFont f("Consolas"); f.setStyleHint(QFont::Monospace); te->setFont(f);
	te->setPlainText(text.isEmpty() ? "(no output)" : text);
	v->addWidget(te);
	auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
	QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::close);
	v->addWidget(bb);
	dlg->show();
}

static QuadNode *buildQuadNode(int i0, int i1, int j0, int j1, int level,
							   double x0, double dx, double y0, double dy) {
	QuadNode *n = new QuadNode();
	n->level = level; n->i0 = i0; n->i1 = i1; n->j0 = j0; n->j1 = j1;
	const int w = i1 - i0, h = j1 - j0;
	int step = 1; while (w / step > 512 || h / step > 512) step *= 2;   // sampled span <= ~513
	n->step = step;
	n->cx = x0 + 0.5 * (i0 + i1) * dx;
	n->cy = y0 + 0.5 * (j0 + j1) * dy;
	n->worldSpacing = std::max(std::abs(dx), std::abs(dy)) * step;
	const int im = (i0 + i1) / 2, jm = (j0 + j1) / 2;
	if (step == 1 || im <= i0 || jm <= j0) { n->leaf = true; n->step = 1; return n; }
	n->leaf = false;
	n->child[0] = buildQuadNode(i0, im, j0, jm, level+1, x0,dx,y0,dy);
	n->child[1] = buildQuadNode(im, i1, j0, jm, level+1, x0,dx,y0,dy);
	n->child[2] = buildQuadNode(i0, im, jm, j1, level+1, x0,dx,y0,dy);
	n->child[3] = buildQuadNode(im, i1, jm, j1, level+1, x0,dx,y0,dy);
	return n;
}

static void ensureNodeActor(Scene *s, QuadNode *n) {
	if (n->actor) return;
	auto tpd = makeGridTile(s->gridZ.data(), s->gnx, s->gny,
							n->i0, n->i1, n->j0, n->j1, s->gx0, s->gdx, s->gy0, s->gdy, n->step);
	vtkNew<vtkPolyDataMapper> m; m->SetInputData(tpd);
	m->SetLookupTable(s->surfLut); m->SetScalarRange(s->zmin, s->zmax);
	if (s->surfCtfRange) m->UseLookupTableScalarRangeOn();
	m->ScalarVisibilityOn(); m->InterpolateScalarsBeforeMappingOn();
	auto a = vtkSmartPointer<vtkActor>::New(); a->SetMapper(m);
	a->GetProperty()->SetInterpolationToPBR();
	a->GetProperty()->SetMetallic(0.0); a->GetProperty()->SetRoughness(0.45);
	a->GetProperty()->SetEdgeColor(0.12, 0.12, 0.12); a->GetProperty()->SetLineWidth(1.0);
	a->GetProperty()->SetEdgeVisibility(s->surfEdges);
	if (s->useHillshade) applySurfStyle(s, a);   // hillshade on: bake CPT*shade + unlit, like the resident tiles
	const vtkIdType npts = tpd->GetPoints()->GetNumberOfPoints();
	const vtkIdType ncel = tpd->GetPolys()->GetNumberOfCells();
	n->bytes = (size_t)npts * (12 + 4 + 12) + (size_t)ncel * 20;   // pts + z + normal + quad ids
	n->actor = a;
	s->surfGroup->AddPart(a);
	s->tiles.push_back(a);
	s->lodResidentBytes += n->bytes;
}

static void dropNodeActor(Scene *s, QuadNode *n) {
	if (!n->actor) return;
	s->surfGroup->RemovePart(n->actor);
	for (size_t k = 0; k < s->tiles.size(); ++k)
		if (s->tiles[k] == n->actor) { s->tiles.erase(s->tiles.begin() + k); break; }
	s->lodResidentBytes = (s->lodResidentBytes >= n->bytes) ? s->lodResidentBytes - n->bytes : 0;
	n->actor = nullptr; n->bytes = 0;
}

static void dropSubtree(Scene *s, QuadNode *n) {
	if (!n) return;
	dropNodeActor(s, n);
	for (int k = 0; k < 4; ++k) dropSubtree(s, n->child[k]);
}

static void collectResident(QuadNode *n, std::vector<QuadNode*>& out) {
	if (!n) return;
	if (n->actor) out.push_back(n);
	for (int k = 0; k < 4; ++k) collectResident(n->child[k], out);
}

static void evictLRU(Scene *s) {
	std::vector<QuadNode*> res; collectResident(s->quadRoot, res);
	std::sort(res.begin(), res.end(), [](QuadNode *a, QuadNode *b){ return a->lastUsed < b->lastUsed; });
	for (QuadNode *n : res) {
		if (s->lodResidentBytes <= s->lodBudgetBytes) break;
		if (n->lastUsed == s->lodFrame) continue;   // never evict a tile drawn this frame
		dropNodeActor(s, n);
	}
}

static void refineNode(Scene *s, QuadNode *n, vtkCamera *cam, const double camPos[3],
					   double vpH, double tanHalfFov, double parScale, bool parallel, double tau) {
	// node centre in SCALED world (the assembly applies xfac on X, zfac*ve on Z)
	const double zmid = 0.5 * (s->zmin + s->zmax) * s->zfac * s->ve;
	const double pc[3] = { n->cx * s->xfac, n->cy, zmid };
	const double sp = n->worldSpacing * std::max(s->xfac, 1.0);    // scaled node gap
	double px;
	if (parallel) {
		px = (parScale > 0.0) ? sp * vpH / (2.0 * parScale) : 1e9;
	} else {
		const double dvx = pc[0]-camPos[0], dvy = pc[1]-camPos[1], dvz = pc[2]-camPos[2];
		double dist = std::sqrt(dvx*dvx + dvy*dvy + dvz*dvz); if (dist < 1e-6) dist = 1e-6;
		px = sp * vpH / (2.0 * dist * tanHalfFov);
	}
	if (n->leaf || px <= tau) {
		ensureNodeActor(s, n); n->lastUsed = s->lodFrame;   // draw at this LOD
		for (int k = 0; k < 4; ++k) dropSubtree(s, n->child[k]);   // shed finer detail
	} else {
		dropNodeActor(s, n);                                 // too coarse -> recurse
		for (int k = 0; k < 4; ++k) refineNode(s, n->child[k], cam, camPos, vpH, tanHalfFov, parScale, parallel, tau);
	}
}

static void refineQuadtree(Scene *s) {
	if (!s->quadRoot || !s->ren) return;
	vtkCamera *cam = s->ren->GetActiveCamera(); if (!cam) return;
	double camPos[3]; cam->GetPosition(camPos);
	int *sz = s->ren->GetSize(); const double vpH = (sz && sz[1] > 0) ? sz[1] : 600.0;
	const bool parallel = cam->GetParallelProjection() != 0;
	const double tanHalf = std::tan(vtkMath::RadiansFromDegrees(cam->GetViewAngle() * 0.5));
	s->lodFrame++;
	refineNode(s, s->quadRoot, cam, camPos, vpH, tanHalf, cam->GetParallelScale(), parallel, /*tau=*/2.0);
	if (s->lodResidentBytes > s->lodBudgetBytes) evictLRU(s);
}

static void onLodCamera(vtkObject*, unsigned long, void *cd, void*) {
	refineQuadtree(static_cast<Scene*>(cd));
}

// GRAPHICAL ELEMENT: custom dock title bar that folds the dock HORIZONTALLY.
// Open  -> a normal horizontal strip: "▾ Title" across the top.
// Folded -> a thin vertical strip (~one text-height wide) with "▸" at the top and
// the Title painted rotated 90° (reading bottom->top) down the window edge, so the
// collapsed dock costs only its strip width instead of leaving its full open width
// as dead, unusable space. Clicking anywhere on the bar toggles via onClick.
// (No Q_OBJECT: this TU has no moc — we override virtuals and call a std::function.)
struct FoldTitleBar : QWidget {
	QString title;
	bool    folded    = false;
	int     openWidth = 0;            // dock width remembered at fold time, restored on un-fold
	std::function<void()> onClick;
	explicit FoldTitleBar(const QString& t, QWidget *parent = nullptr)
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

// ============================================================================================
// GeoGridGeometry — reusable "Griding Line Geometry" widget (Mirone-style region/spacing table).
// Two rows (X / Y Direction) × four columns (Min, Max, Spacing, # of lines) inside a group box,
// with the cross-field recompute wired through the Julia dim-fun (port of Mirone dim_funs.m), plus
// an optional "OR Ref grid" picker row (gmtread a grid header -> fill the boxes). Self-contained:
// a single addWidget() embeds the whole block. Shared by every dialog that needs a region/spacing
// spec (grdsample, vertical elastic deformation, …). Caller prefills with fillGeometry(), reads
// back with region()/inc()/and the public box pointers, and keeps it in sync with its own grid
// registration via setRegistration().
// ============================================================================================
class GeoGridGeometry : public QWidget {
public:
	QLineEdit *xMin, *xMax, *xInc, *xN;   // X Direction row
	QLineEdit *yMin, *yMax, *yInc, *yN;   // Y Direction row
	QLineEdit *refEdit = nullptr;         // "OR Ref grid" — present only when withRefGrid
	// Source-extent caps for the Min/Max boxes (a sampled grid can't exceed the input). Set by
	// fillGeometry from the loaded/ref grid; empty = unconstrained.
	QString xMinOr, xMaxOr, yMinOr, yMaxOr;
	bool dmsXinc = false, dmsYinc = false;   // last x/y inc typed in dd:mm:ss form (carried to Julia)
	int oneOrZero = 1;                        // 1 gridline / 0 pixel registration (feeds the dim-fun)

	void setRegistration(bool pixel) { oneOrZero = pixel ? 0 : 1; }

	// "W/E/S/N" region string (blank fields kept blank so the caller can detect "no region").
	QString region() const {
		return QString("%1/%2/%3/%4").arg(xMin->text().trimmed()).arg(xMax->text().trimmed())
		                             .arg(yMin->text().trimmed()).arg(yMax->text().trimmed());
	}
	// Increment string: "xinc", or "xinc/yinc" when anisotropic.
	QString inc() const {
		QString xi = xInc->text().trimmed(), yi = yInc->text().trimmed();
		return (!yi.isEmpty() && yi != xi) ? xi + "/" + yi : xi;
	}

	// Fill the boxes from "W/E/S/N/xinc/yinc/nx/ny" (8 slash-separated fields, as returned by
	// g_juliaGridMeta or built from the current scene). Silent no-op on a malformed/empty string so
	// a failed gmtread leaves the user's typed values untouched.
	void fillGeometry(const QString &meta) {
		const QStringList f = meta.split('/');
		if (f.size() < 8) return;
		xMin->setText(f[0]); xMax->setText(f[1]);
		yMin->setText(f[2]); yMax->setText(f[3]);
		xInc->setText(f[4]); yInc->setText(f[5]);
		xN->setText(f[6]);   yN->setText(f[7]);
		// The source extent caps the Min/Max boxes (sampled grid can't exceed the input grid).
		xMinOr = f[0]; xMaxOr = f[1]; yMinOr = f[2]; yMaxOr = f[3];
	}

	// Round-trip the boxes through the Julia dim-fun (port of Mirone's dim_funs.m): hand it which box
	// changed + all current values + the source caps + registration; write back the 8 recomputed
	// fields. Programmatic setText() does NOT re-fire editingFinished, so no recursion.
	void runDimFun(const QString &which) {
		if (!g_juliaDimFun) return;
		QString state = QString("%1/%2/%3/%4/%5/%6/%7/%8/%9/%10/%11/%12/%13/%14")
			.arg(xMin->text().trimmed()).arg(xMax->text().trimmed())
			.arg(yMin->text().trimmed()).arg(yMax->text().trimmed())
			.arg(xInc->text().trimmed()).arg(yInc->text().trimmed())
			.arg(xN->text().trimmed()).arg(yN->text().trimmed())
			.arg(oneOrZero).arg(xMinOr).arg(xMaxOr).arg(yMinOr).arg(yMaxOr)
			.arg(dmsXinc || dmsYinc ? 1 : 0);
		const char *out = g_juliaDimFun(which.toUtf8().constData(), state.toUtf8().constData());
		if (!out) return;
		const QStringList r = QString::fromUtf8(out).split('/');
		if (r.size() < 8) return;
		xMin->setText(r[0]); xMax->setText(r[1]);
		yMin->setText(r[2]); yMax->setText(r[3]);
		xInc->setText(r[4]); yInc->setText(r[5]);
		xN->setText(r[6]);   yN->setText(r[7]);
	}

	GeoGridGeometry(QWidget *parent, bool withRefGrid = true) : QWidget(parent) {
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);

		// --- The 2×4 table inside a group box ---------------------------------------------------
		auto *geoGroup  = new QGroupBox("Griding Line Geometry", this);
		auto *geoLayout = new QGridLayout();
		geoLayout->setHorizontalSpacing(8);
		geoLayout->setVerticalSpacing(4);
		auto makeEdit = [this]() {
			auto *e = new QLineEdit(this);   // no validator: accepts decimal AND dd:mm:ss (Julia validates)
			e->setAlignment(Qt::AlignLeft);
			e->setMinimumWidth(90);
			return e;
		};
		xMin = makeEdit(); xMax = makeEdit(); xInc = makeEdit(); xN = makeEdit();
		yMin = makeEdit(); yMax = makeEdit(); yInc = makeEdit(); yN = makeEdit();

		// Column headers (row 0, cols 1..4), centered over their fields.
		geoLayout->addWidget(new QLabel("Min"),        0, 1, Qt::AlignHCenter);
		geoLayout->addWidget(new QLabel("Max"),        0, 2, Qt::AlignHCenter);
		geoLayout->addWidget(new QLabel("Spacing"),    0, 3, Qt::AlignHCenter);
		geoLayout->addWidget(new QLabel("# of lines"), 0, 4, Qt::AlignHCenter);
		// X Direction row.
		geoLayout->addWidget(new QLabel("X Direction"), 1, 0);
		geoLayout->addWidget(xMin, 1, 1);
		geoLayout->addWidget(xMax, 1, 2);
		geoLayout->addWidget(xInc, 1, 3);
		geoLayout->addWidget(xN,   1, 4);
		// Y Direction row.
		geoLayout->addWidget(new QLabel("Y Direction"), 2, 0);
		geoLayout->addWidget(yMin, 2, 1);
		geoLayout->addWidget(yMax, 2, 2);
		geoLayout->addWidget(yInc, 2, 3);
		geoLayout->addWidget(yN,   2, 4);
		// "?" help button spanning both data rows on the far right.
		auto *helpBtn = new QToolButton(this);
		helpBtn->setText("?");
		helpBtn->setToolTip("Edit any two of Min/Max/Spacing/# and the rest are derived.");
		geoLayout->addWidget(helpBtn, 1, 5, 2, 1);

		geoGroup->setLayout(geoLayout);
		outer->addWidget(geoGroup);

		// Cross-field recompute (Mirone dim_funs.m, now in Julia). Each box recomputes the others on
		// focus-out / Enter.
		QObject::connect(xMin, &QLineEdit::editingFinished, this, [this]{ runDimFun("xMin"); });
		QObject::connect(xMax, &QLineEdit::editingFinished, this, [this]{ runDimFun("xMax"); });
		QObject::connect(yMin, &QLineEdit::editingFinished, this, [this]{ runDimFun("yMin"); });
		QObject::connect(yMax, &QLineEdit::editingFinished, this, [this]{ runDimFun("yMax"); });
		QObject::connect(xInc, &QLineEdit::editingFinished, this, [this]{ runDimFun("xInc"); });
		QObject::connect(yInc, &QLineEdit::editingFinished, this, [this]{ runDimFun("yInc"); });
		QObject::connect(xN,   &QLineEdit::editingFinished, this, [this]{ runDimFun("nCols"); });
		QObject::connect(yN,   &QLineEdit::editingFinished, this, [this]{ runDimFun("nRows"); });

		// --- Optional "OR Ref grid" row: pick a grid/image; gmtread its header to fill the boxes ----
		if (withRefGrid) {
			auto *refRow = new QHBoxLayout();
			refRow->addWidget(new QLabel("OR Ref grid"));
			refEdit = new QLineEdit(this);
			refEdit->setToolTip("Pick a grid/image; its region, spacing and size fill the boxes above.");
			refRow->addWidget(refEdit, 1);
			auto *refBtn = new QToolButton(this);
			refBtn->setText("...");
			refRow->addWidget(refBtn);
			auto loadRef = [this](const QString &path) {
				if (path.isEmpty()) return;
				refEdit->setText(path);
				if (!g_juliaGridMeta) return;
				const char *m = g_juliaGridMeta(path.toUtf8().constData());
				if (m) fillGeometry(QString::fromUtf8(m));
			};
			QObject::connect(refBtn, &QToolButton::clicked, this, [this, loadRef]() {
				QString f = QFileDialog::getOpenFileName(this, "Select reference grid", prefStartDir(),
				                                         "Grid/Image files (*.nc *.grd *.tif *.tiff);;All files (*)");
				if (!f.isEmpty()) rememberStartDir(f);
				loadRef(f);
			});
			// HARD RULE: an edit box must NEVER execute (no editingFinished->module read). The ref grid
			// is loaded ONLY by the "..." picker button above. See only-action-button-executes-dialog.
			outer->addLayout(refRow);
		}
	}
};

// ============================================================================================
// grdsample dialog (GMT > Resample). Port of Mirone's grdsample tool.
// On OK, hands "input;output;I;R;n;r;T" to Julia.
// ============================================================================================
class GrdsampleDialog : public QDialog {
public:
	QLineEdit *inpEdit, *outEdit;
	GeoGridGeometry *geo;             // reusable Griding Line Geometry table (+ OR Ref grid row)
	QComboBox *interpCombo;
	QComboBox *regCombo = nullptr;     // gridline/pixel (kept synced into geo->oneOrZero)
	QCheckBox *clipCheck;
	QCheckBox *toggleCheck;
	QToolButton *inpBtn = nullptr;    // Input grid "..." browse (disabled when a grid is loaded)
	bool    useSelected = false;      // a grid is loaded -> input is the current window's element
	QString srcName;                  // Scene Objects label of the loaded element (suffix source)
	QString params;   // "input;output;I;R;n;r;T;S" on OK  (S = source element name)
	Scene  *scn = nullptr;    // owning window's scene (for Julia call on Apply)

	GrdsampleDialog(QWidget *parent, Scene *scene = nullptr) : QDialog(parent) {
		setWindowTitle("grdsample");
		setMinimumWidth(400);
		scn = scene;

		auto *v = new QVBoxLayout(this);

		// --- Input / Output grid files ---
		auto fileRow = [this](const QString &label, QLineEdit *&edit, QToolButton *&btnOut,
		                      const QString &filter) -> QLayout* {
			auto *h = new QHBoxLayout();
			h->addWidget(new QLabel(label));
			edit = new QLineEdit(this);
			edit->setMinimumWidth(250);
			h->addWidget(edit);
			auto *btn = new QToolButton(this);
			btn->setText("...");
			h->addWidget(btn);
			btnOut = btn;
			QObject::connect(btn, &QToolButton::clicked, this, [this, edit, filter]() {
				QString path = QFileDialog::getOpenFileName(this, "Select grid file", prefStartDir(), filter);
				if (!path.isEmpty()) { edit->setText(path); rememberStartDir(path); }
			});
			return h;
		};

		QToolButton *outBtn = nullptr;
		v->addLayout(fileRow("Input grid:",  inpEdit, inpBtn, "Grid files (*.nc *.grd);;All files (*)"));
		v->addLayout(fileRow("Output grid:", outEdit, outBtn, "Grid files (*.nc *.grd);;All files (*)"));

		// A grid/image is already loaded in this window -> the input IS that element. Show its Scene
		// Objects label grayed ("using <name>") and lock the input row; on Apply we send "selected".
		if (scene && scene->surf && !scene->emptyStart) {
			srcName = scene->surfName.empty()
			            ? QString(scene->imageOnly ? "Image" : "Surface")
			            : QString::fromStdString(scene->surfName);
			useSelected = true;
			inpEdit->setText("using " + srcName);
			inpEdit->setReadOnly(true);
			inpEdit->setEnabled(false);   // grayed
			inpBtn->setEnabled(false);
		}

		// --- Griding Line Geometry (reusable widget: 2×4 table + OR Ref grid row) --------------
		geo = new GeoGridGeometry(this, /*withRefGrid=*/true);
		v->addWidget(geo);

		// Prefill the geometry from the window's currently loaded grid/image. Prefer the full-res
		// data layer (gnx/gdx present); fall back to the render bbox + tile dims. No data -> blank.
		if (scene) {
			if (scene->gnx > 1 && scene->gny > 1) {
				geo->fillGeometry(QString("%1/%2/%3/%4/%5/%6/%7/%8")
					.arg(scene->gx0).arg(scene->gx1).arg(scene->gy0).arg(scene->gy1)
					.arg(scene->gdx).arg(scene->gdy).arg(scene->gnx).arg(scene->gny));
			} else if (scene->x1 > scene->x0 && scene->y1 > scene->y0) {
				geo->fillGeometry(QString("%1/%2/%3/%4////")   // 8 fields: 4 limits + blank inc/size
					.arg(scene->x0).arg(scene->x1).arg(scene->y0).arg(scene->y1));
			}
		}

		// --- Interpolation + Clip ---
		auto *interpRow = new QHBoxLayout();
		interpRow->addWidget(new QLabel("Interpolation:"));
		interpCombo = new QComboBox(this);
		interpCombo->addItem("Nearest neighbor", "nearest");
		interpCombo->addItem("Bilinear", "linear");
		interpCombo->addItem("Bicubic", "cubic");
		interpCombo->addItem("B-spline", "bspline");
		interpCombo->setCurrentIndex(2);  // bicubic default
		interpCombo->setToolTip("Interpolation method: bicubic (smooth), bilinear, nearest neighbor, B-spline");
		interpCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
		interpRow->addWidget(interpCombo);          // sized to its content, not stretched
		clipCheck = new QCheckBox("Clip", this);
		clipCheck->setToolTip("Clip resampled values to input min/max range");
		interpRow->addWidget(clipCheck);
		interpRow->addStretch();
		v->addLayout(interpRow);

		// --- Registration ---
		auto *regComboRow = new QHBoxLayout();
		regComboRow->addWidget(new QLabel("Registration:"));
		regCombo = new QComboBox(this);
		regCombo->addItem("Gridline", "g");
		regCombo->addItem("Pixel", "p");
		regCombo->setToolTip("Grid registration: gridline (node on corners) or pixel (node centered)");
		regCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
		regComboRow->addWidget(regCombo);           // sized to its content, not stretched
		regComboRow->addStretch();
		v->addLayout(regComboRow);
		// Keep the geometry widget's one_or_zero in sync with the chosen registration (gridline=1).
		QObject::connect(regCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			[this]() { geo->setRegistration(regCombo->currentData().toString() == "p"); });

		// --- Toggle ---
		toggleCheck = new QCheckBox("Toggle registration", this);
		toggleCheck->setToolTip("Switch between gridline and pixel registration");
		v->addWidget(toggleCheck);

		// --- Buttons (Apply / Close like Mirone) ---
		auto *btnRow = new QHBoxLayout();
		btnRow->addStretch();
		auto *btnApply = new QPushButton("Apply", this);
		auto *btnClose = new QPushButton("Close", this);
		// HARD RULE: NO edit box may ever execute grdsample. Qt auto-promotes the first QPushButton to
		// the dialog default, so Return in ANY QLineEdit would click Apply and run the module. Disable
		// auto-default on BOTH buttons => Enter in an edit box does nothing but finish that edit.
		btnApply->setAutoDefault(false); btnApply->setDefault(false);
		btnClose->setAutoDefault(false); btnClose->setDefault(false);
		btnRow->addWidget(btnApply);
		btnRow->addWidget(btnClose);
		v->addLayout(btnRow);

		QObject::connect(btnClose, &QPushButton::clicked, this, &QDialog::reject);
		QObject::connect(btnApply, &QPushButton::clicked, this, [this, btnApply]() {
			QString I = geo->inc();     // "xinc" or "xinc/yinc" when anisotropic
			QString R = geo->region();  // "W/E/S/N"
			QString n = interpCombo->currentData().toString();
			if (clipCheck->isChecked()) n += "+c";
			QString r = regCombo->currentData().toString();
			QString T = toggleCheck->isChecked() ? "1" : "0";
			QString in = useSelected ? QString("selected") : inpEdit->text().trimmed();
			params = QString("%1;%2;%3;%4;%5;%6;%7;%8")
				         .arg(in)
				         .arg(outEdit->text().trimmed())
				         .arg(I).arg(R).arg(n).arg(r).arg(T).arg(srcName);
			btnApply->setStyleSheet("background:#d4831a; color:white;");  // busy until Julia returns
			btnApply->setEnabled(false);
			QApplication::processEvents();
			if (g_juliaGrdsample) g_juliaGrdsample(scn, params.toUtf8().constData());
			accept();
		});
	}
};

// ============================================================================================
// IGRF Calculator (Geophysics > Magnetics > IGRF) — port of Mirone's igrf_options.m
// (src_figs/igrf_options.m). Two pieces:
//   IgrfMapArea — a static world image + a click-to-pick marker. Reuses the SAME etopo4_logo.jpg
//   asset and pxLon/pxLat pixel<->geographic mapping as BaseMapArea above (g_basemapLogo, already
//   pushed from Julia via gmtvtk_set_basemap_logo before any window opens), just without
//   BaseMapArea's tile-grid/rubber-band modes — this one only ever picks a single point,
//   mirroring igrf_options.m's axes1 + WindowButtonDownFcn.
//   IgrfDialog — the calculator itself. UNLIKE GrdsampleDialog/ElasticDialog (hand-built C++
//   widget trees), this one is loaded at RUNTIME from deps/ui/igrf_calculator.ui via QUiLoader
//   (same technique as FocalMechanismsDialog below) — the .ui is being actively iterated on in Qt
//   Creator, so hand-porting it desyncs every time it's edited there (see
//   igrf-no-headers-row.md / the aspect-ratio and layout-spacing fights that preceded this
//   rewrite). IgrfUiLoader below is a tiny QUiLoader subclass that knows how to instantiate the
//   one promoted custom widget the .ui references ("IgrfMapWidget" -> IgrfMapArea); every other
//   widget is a plain Qt class QUiLoader already knows how to build. Both compute paths (point
//   readout + grid Compute) go through GMT.jl's magref (g_juliaIgrfPoint/g_juliaIgrfGrid,
//   90_c_api.cpp) instead of Mirone's igrf_m MEX.
// ============================================================================================
class IgrfMapArea : public QWidget {       // the clickable map; no Q_OBJECT (only paint/mouse overrides)
public:
	QPixmap logo;
	double markerLon = -8.0, markerLat = 37.0;
	std::function<void(double, double)> onPick;   // (lon, lat)
	explicit IgrfMapArea(QWidget *p) : QWidget(p) { setMinimumSize(400, 200); }
	// Letterboxed image rect: the logo's OWN aspect ratio, centered inside whatever rect the
	// layout gives this widget — never stretched to fill it (a whole-world image distorted to a
	// non-2:1 widget reads as visibly wrong coastlines). Falls back to the full widget rect when
	// there's no logo yet (flat fill background, no aspect to preserve).
	QRectF imageRect() const {
		if (logo.isNull()) return rect();
		QSizeF fitted = QSizeF(logo.size()).scaled(size(), Qt::KeepAspectRatio);
		QRectF r(QPointF(0, 0), fitted);
		r.moveCenter(rect().center());
		return r;
	}
	double pxLon(double x, const QRectF &ir) const { return -180.0 + (x - ir.left()) / ir.width()  * 360.0; }
	double pxLat(double y, const QRectF &ir) const { return   90.0 - (y - ir.top())  / ir.height() * 180.0; }
	void setMarker(double lon, double lat) { markerLon = lon; markerLat = lat; update(); }
protected:
	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		g.fillRect(rect(), QColor(30, 60, 110));      // letterbox bars when the widget isn't 2:1
		QRectF ir = imageRect();
		if (!logo.isNull()) g.drawPixmap(ir, logo, QRectF(logo.rect()));
		double mx = ir.left() + (markerLon + 180.0) / 360.0 * ir.width();
		double my = ir.top()  + (90.0 - markerLat)  / 180.0 * ir.height();
		QPen pen(Qt::red); pen.setWidth(2); g.setPen(pen);
		g.setBrush(Qt::NoBrush);
		g.drawEllipse(QPointF(mx, my), 5, 5);
		g.drawLine(QPointF(mx - 8, my), QPointF(mx + 8, my));
		g.drawLine(QPointF(mx, my - 8), QPointF(mx, my + 8));
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (!onPick) return;
		QRectF ir = imageRect();
		if (!ir.contains(e->position())) return;      // click landed in a letterbox bar -> ignore
		double lon = std::clamp(pxLon(e->position().x(), ir), -180.0, 180.0);
		double lat = std::clamp(pxLat(e->position().y(), ir),  -90.0,  90.0);
		onPick(lon, lat);
	}
};

// QUiLoader that knows how to instantiate the one promoted custom widget igrf_calculator.ui
// references ("IgrfMapWidget", object name "mapPlot") — every other widget in that .ui is a
// plain Qt class QUiLoader already builds on its own.
class IgrfUiLoader : public QUiLoader {
public:
	QWidget *createWidget(const QString &className, QWidget *parent = nullptr,
	                       const QString &name = QString()) override {
		if (className == "IgrfMapWidget") {
			auto *w = new IgrfMapArea(parent);
			w->setObjectName(name);
			return w;
		}
		return QUiLoader::createWidget(className, parent, name);
	}
};

class IgrfDialog {
public:
	QDialog *dlg = nullptr;
	IgrfMapArea *mapArea = nullptr;
	QLineEdit *latDeg, *latMin, *latSec, *latDec;
	QLineEdit *lonDeg, *lonMin, *lonSec, *lonDec;
	QLineEdit *elevEdit, *dateDay, *dateMonth, *dateYear, *dateDec;
	QLabel *totalFieldVal, *incVal, *decVal, *xVal, *yVal, *zVal, *hVal;
	QLineEdit *xMin, *xMax, *xInc, *xN, *yMin, *yMax, *yInc, *yN, *refEdit;
	QComboBox *fieldCombo;
	QLineEdit *magFile1Edit, *magFile2Edit;
	Scene *scn = nullptr;    // owning window's scene (error reporting only — Compute always opens a NEW window)

	// deg/min/sec (sign on deg) -> decimal degrees, Mirone's edit_LatDeg_CB/edit_LonDeg_CB convention.
	static double dmsToDec(QLineEdit *d, QLineEdit *m, QLineEdit *s) {
		double dv = d->text().toDouble(), mv = m->text().toDouble(), sv = s->text().toDouble();
		return dv < 0 ? dv - mv / 60.0 - sv / 3600.0 : dv + mv / 60.0 + sv / 3600.0;
	}
	// decimal degrees -> deg/min/sec (sign on deg only), Mirone's dec2deg('opt') convention.
	static void decToDms(double v, QLineEdit *d, QLineEdit *m, QLineEdit *s) {
		int deg = (int)v;                                    // truncate toward zero, sign kept
		double frac = std::fabs(v - deg);
		double minF = frac * 60.0;
		int minI = (int)minF;
		double sec = (minF - minI) * 60.0;
		d->setText(QString::number(deg));
		m->setText(QString::number(minI));
		s->setText(QString::number(sec, 'f', 2));
	}
	// D/M/Y edit boxes -> decimal year via QDate (Mirone's dec_year.m, done with the calendar
	// instead of hand-rolled leap-year math). Falls back to the current Decimal box on an invalid
	// date (matches Mirone leaving the box alone rather than crashing).
	double dateDecFromDMY() const {
		int d = dateDay->text().toInt(), m = dateMonth->text().toInt(), y = dateYear->text().toInt();
		QDate dt(y, m, d);
		if (!dt.isValid()) return dateDec->text().toDouble();
		return y + (dt.dayOfYear() - 1.0) / dt.daysInYear();
	}

	// Recompute the Point values from the current Lat/Lon/Elevation/Date boxes (g_juliaIgrfPoint,
	// GMT.jl magref) and refresh the map marker. Wired to every relevant editingFinished, so the
	// dialog stays live the same way Mirone's *_CB handlers do — never on a raw keystroke.
	void recompute() {
		if (!mapArea || !latDec || !lonDec) return;
		double lat = latDec->text().toDouble(), lon = lonDec->text().toDouble();
		mapArea->setMarker(lon, lat);
		if (!g_juliaIgrfPoint) {
			if (scn && scn->win) scn->win->statusBar()->showMessage("IGRF: callback not registered", 3000);
			return;
		}
		double elev = elevEdit->text().toDouble(), date = dateDec->text().toDouble();
		QString state = QString("%1/%2/%3/%4").arg(lon, 0, 'g', 10).arg(lat, 0, 'g', 10)
			                                  .arg(elev, 0, 'g', 10).arg(date, 0, 'g', 10);
		const char *out = g_juliaIgrfPoint(state.toUtf8().constData());
		if (!out) return;
		const QStringList r = QString::fromUtf8(out).split('/');   // copy immediately (Julia-owned buffer)
		if (r.size() < 7) return;
		totalFieldVal->setText(QString::number(r[0].toDouble(), 'f', 0) + " nT");
		hVal->setText(QString::number(r[1].toDouble(), 'f', 0) + " nT");
		xVal->setText(QString::number(r[2].toDouble(), 'f', 0) + " nT");
		yVal->setText(QString::number(r[3].toDouble(), 'f', 0) + " nT");
		zVal->setText(QString::number(r[4].toDouble(), 'f', 0) + " nT");
		decVal->setText(QString::number(r[5].toDouble(), 'f', 1) + QString::fromUtf8(" \xc2\xb0"));
		incVal->setText(QString::number(r[6].toDouble(), 'f', 1) + QString::fromUtf8(" \xc2\xb0"));
	}

	explicit IgrfDialog(QWidget *parent, Scene *scene = nullptr) {
		scn = scene;
		IgrfUiLoader loader;
		QFile f(gmtvtkUiDir() + "/igrf_calculator.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("IgrfDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("IgrfDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;   // local copy — member `dlg` can't be lambda-captured

		// IgrfMapArea has no Q_OBJECT (paint/mouse overrides only, like BaseMapArea) -> qobject_cast
		// can't be used; static_cast is safe since IgrfUiLoader is the only thing that ever
		// constructs an "IgrfMapWidget" instance, and it always builds an IgrfMapArea.
		mapArea = static_cast<IgrfMapArea *>(d->findChild<QWidget *>("mapPlot"));
		if (mapArea && !g_basemapLogo.isEmpty()) mapArea->logo.load(g_basemapLogo);

		latDeg = d->findChild<QLineEdit *>("latDeg"); latMin = d->findChild<QLineEdit *>("latMin");
		latSec = d->findChild<QLineEdit *>("latSec"); latDec = d->findChild<QLineEdit *>("latDec");
		lonDeg = d->findChild<QLineEdit *>("lonDeg"); lonMin = d->findChild<QLineEdit *>("lonMin");
		lonSec = d->findChild<QLineEdit *>("lonSec"); lonDec = d->findChild<QLineEdit *>("lonDec");
		elevEdit  = d->findChild<QLineEdit *>("elevEdit");
		dateDay   = d->findChild<QLineEdit *>("dateDay");
		dateMonth = d->findChild<QLineEdit *>("dateMonth");
		dateYear  = d->findChild<QLineEdit *>("dateYear");
		dateDec   = d->findChild<QLineEdit *>("dateDec");
		totalFieldVal = d->findChild<QLabel *>("totalFieldVal");
		incVal = d->findChild<QLabel *>("incVal"); decVal = d->findChild<QLabel *>("decVal");
		xVal = d->findChild<QLabel *>("xVal"); yVal = d->findChild<QLabel *>("yVal");
		zVal = d->findChild<QLabel *>("zVal"); hVal = d->findChild<QLabel *>("hVal");
		xMin = d->findChild<QLineEdit *>("xMin"); xMax = d->findChild<QLineEdit *>("xMax");
		xInc = d->findChild<QLineEdit *>("xInc"); xN   = d->findChild<QLineEdit *>("xN");
		yMin = d->findChild<QLineEdit *>("yMin"); yMax = d->findChild<QLineEdit *>("yMax");
		yInc = d->findChild<QLineEdit *>("yInc"); yN   = d->findChild<QLineEdit *>("yN");
		refEdit = d->findChild<QLineEdit *>("refEdit");
		fieldCombo = d->findChild<QComboBox *>("fieldCombo");
		magFile1Edit = d->findChild<QLineEdit *>("magFile1Edit");
		magFile2Edit = d->findChild<QLineEdit *>("magFile2Edit");
		auto *helpBtn       = d->findChild<QToolButton *>("helpBtn");
		auto *refBtn        = d->findChild<QToolButton *>("refBtn");
		auto *geoHelpBtn    = d->findChild<QToolButton *>("geoHelpBtn");
		auto *geoComputeBtn = d->findChild<QPushButton *>("geoComputeBtn");
		auto *magHelpBtn    = d->findChild<QToolButton *>("magHelpBtn");
		auto *magFile1Btn   = d->findChild<QToolButton *>("magFile1Btn");
		auto *magFile2Btn   = d->findChild<QToolButton *>("magFile2Btn");
		auto *magComputeBtn = d->findChild<QPushButton *>("magComputeBtn");

		if (helpBtn)    helpBtn->setToolTip("Edit any two of Min/Max/Spacing/# and the rest are derived.");
		if (geoHelpBtn) geoHelpBtn->setToolTip("Compute the selected field component over the grid above.");

		// --- Lat/Lon DMS<->Decimal + Elevation + Date wiring (Mirone's *_CB conventions) ---
		if (latDeg) QObject::connect(latDeg, &QLineEdit::editingFinished, d, [this]() {
			latDec->setText(QString::number(dmsToDec(latDeg, latMin, latSec), 'f', 4)); recompute(); });
		if (latMin) QObject::connect(latMin, &QLineEdit::editingFinished, d, [this]() {
			latDec->setText(QString::number(dmsToDec(latDeg, latMin, latSec), 'f', 4)); recompute(); });
		if (latSec) QObject::connect(latSec, &QLineEdit::editingFinished, d, [this]() {
			latDec->setText(QString::number(dmsToDec(latDeg, latMin, latSec), 'f', 4)); recompute(); });
		if (latDec) QObject::connect(latDec, &QLineEdit::editingFinished, d, [this]() {
			decToDms(latDec->text().toDouble(), latDeg, latMin, latSec); recompute(); });

		if (lonDeg) QObject::connect(lonDeg, &QLineEdit::editingFinished, d, [this]() {
			lonDec->setText(QString::number(dmsToDec(lonDeg, lonMin, lonSec), 'f', 4)); recompute(); });
		if (lonMin) QObject::connect(lonMin, &QLineEdit::editingFinished, d, [this]() {
			lonDec->setText(QString::number(dmsToDec(lonDeg, lonMin, lonSec), 'f', 4)); recompute(); });
		if (lonSec) QObject::connect(lonSec, &QLineEdit::editingFinished, d, [this]() {
			lonDec->setText(QString::number(dmsToDec(lonDeg, lonMin, lonSec), 'f', 4)); recompute(); });
		if (lonDec) QObject::connect(lonDec, &QLineEdit::editingFinished, d, [this]() {
			decToDms(lonDec->text().toDouble(), lonDeg, lonMin, lonSec); recompute(); });

		if (elevEdit) QObject::connect(elevEdit, &QLineEdit::editingFinished, d, [this]() { recompute(); });

		// D/M/Y -> Decimal is ONE-WAY (matches Mirone's edit_DateDD/MM/YY_CB, which sync Decimal but
		// NOT the reverse — edit_DateDec_CB never touches the D/M/Y boxes either).
		auto syncDateDec = [this]() { dateDec->setText(QString::number(dateDecFromDMY(), 'f', 4)); recompute(); };
		if (dateDay)   QObject::connect(dateDay,   &QLineEdit::editingFinished, d, syncDateDec);
		if (dateMonth) QObject::connect(dateMonth, &QLineEdit::editingFinished, d, syncDateDec);
		if (dateYear)  QObject::connect(dateYear,  &QLineEdit::editingFinished, d, syncDateDec);
		if (dateDec)   QObject::connect(dateDec,   &QLineEdit::editingFinished, d, [this]() { recompute(); });

		if (mapArea) mapArea->onPick = [this](double lon, double lat) {
			lonDec->setText(QString::number(lon, 'f', 4));
			latDec->setText(QString::number(lat, 'f', 4));
			decToDms(lon, lonDeg, lonMin, lonSec);
			decToDms(lat, latDeg, latMin, latSec);
			recompute();
		};

		// --- "OR Ref grid" picker — same behavior as GeoGridGeometry's own refRow (grdsample.jl's
		// gmtvtk_set_gridmeta_callback). The .ui declares plain widgets here (not a promoted
		// GeoGridGeometry), so the wiring is inline instead of shared through that C++ class.
		if (refBtn) QObject::connect(refBtn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getOpenFileName(d, "Select reference grid", prefStartDir(),
			                                          "Grid/Image files (*.nc *.grd *.tif *.tiff);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			if (refEdit) refEdit->setText(fn);
			if (!g_juliaGridMeta) return;
			const char *m = g_juliaGridMeta(fn.toUtf8().constData());
			if (!m) return;
			const QStringList meta = QString::fromUtf8(m).split('/');
			if (meta.size() < 8) return;
			xMin->setText(meta[0]); xMax->setText(meta[1]);
			yMin->setText(meta[2]); yMax->setText(meta[3]);
			xInc->setText(meta[4]); yInc->setText(meta[5]);
			xN->setText(meta[6]);   yN->setText(meta[7]);
		});

		// --- Griding Line Geometry Compute: the .ui's fieldCombo order is "Total field /
		// Inclination / Declination / X / Y / Z / H" — NOT Mirone's popup order. Map by combo
		// TEXT, not index, so a future reorder in Designer can't silently desync the field code.
		if (geoComputeBtn) QObject::connect(geoComputeBtn, &QPushButton::clicked, d, [this, d]() {
			if (!g_juliaIgrfGrid) {
				if (scn && scn->win) scn->win->statusBar()->showMessage("IGRF grid: callback not registered", 3000);
				return;
			}
			QString sel = fieldCombo ? fieldCombo->currentText() : "Total field";
			QString code = sel == "Horiz field" ? "H" : sel == "Comp X" ? "X" : sel == "Comp Y" ? "Y" :
			                sel == "Comp Z" ? "Z" : sel == "Declination" ? "D" :
			                sel == "Inclination" ? "I" : "T";
			QString params = QString("%1/%2/%3/%4/%5/%6/%7/%8/%9")
				.arg(xMin->text().trimmed()).arg(xMax->text().trimmed())
				.arg(yMin->text().trimmed()).arg(yMax->text().trimmed())
				.arg(xInc->text().trimmed()).arg(yInc->text().trimmed())
				.arg(elevEdit->text().trimmed()).arg(dateDec->text().trimmed())
				.arg(code);
			g_juliaIgrfGrid(scn, params.toUtf8().constData());
		});

		if (magHelpBtn) QObject::connect(magHelpBtn, &QToolButton::clicked, d, [d]() {
			QMessageBox::information(d, "Input Mag File",
				"Enter a file with total-field measurements to compute the IGRF at those "
				"positions. The minimum required is two columns: longitude and latitude. "
				"Elevation and Date come from the boxes above (a full per-row column selector, "
				"like Mirone's, is not implemented here — every row uses the same Elevation/Date).");
		});
		if (magFile1Btn) QObject::connect(magFile1Btn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getOpenFileName(d, "Select file", prefStartDir(),
			                                          "Mag file (*.dat *.DAT);;All Files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			magFile1Edit->setText(fn);
			QFileInfo fi(fn);
			magFile2Edit->setText(fi.path() + "/" + fi.completeBaseName() + "_igrf.dat");
		});
		if (magFile2Btn) QObject::connect(magFile2Btn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getSaveFileName(d, "Select File name", prefStartDir(),
			                                          "Mag file (*.dat *.DAT);;All Files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			magFile2Edit->setText(fn);
		});
		if (magComputeBtn) QObject::connect(magComputeBtn, &QPushButton::clicked, d, [this, d]() {
			if (!g_juliaIgrfFile) {
				if (scn && scn->win) scn->win->statusBar()->showMessage("IGRF file: callback not registered", 3000);
				return;
			}
			QString in = magFile1Edit->text().trimmed(), out = magFile2Edit->text().trimmed();
			if (in.isEmpty() || out.isEmpty()) return;
			// No Headers?/N-of-headers control in the .ui -> always 0 skipped lines.
			QString params = QString("%1;%2;%3;%4;%5").arg(in).arg(out).arg(QString())
				.arg(elevEdit->text().trimmed()).arg(dateDec->text().trimmed());
			g_juliaIgrfFile(scn, params.toUtf8().constData());
			if (scn && scn->win) scn->win->statusBar()->showMessage("IGRF: file written to " + out, 4000);
		});

		recompute();

		// Non-modal, heap-allocated usage (menu does `new IgrfDialog(...)`): this wrapper is NOT
		// itself a QWidget, so nothing frees it when `dlg` closes (WA_DeleteOnClose only frees the
		// QDialog) — self-delete once the underlying QDialog is destroyed.
		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}
};

// ============================================================================================
// Geomagnetic Bar Code — port of Mirone's magbarcode.m (src_figs\magbarcode.m). Displays
// geomagnetic polarity time scale (black/white bars) with time ruler and geological periods.
// Three sections: chrons (left), time ruler (middle), geological periods (right). Click to add
// red pico markers (draggable). Data from Cande_Kent_95.dat (chron name, age_start, age_end).
// ============================================================================================
class MagBarcodeArea : public QWidget {
public:
	struct Chron {
		QString name;     // chron name (column 1, e.g., "C1n", "C1r.1n")
		QString label;    // age_txt (column 4, e.g., "1", "a", "2")
		double ageStart, ageEnd;
	};
	QList<Chron> chrons;
	double scrollY = 0.0;          // vertical scroll offset (in Ma, from top)
	double barHeight = 40.0;       // px per 1 Ma. Bumped from a straight 0.75cm/Ma port (~26px) so the
	                                // short epoch/period boxes (Bru, Gilbert, Pliocene, ...) have room
	                                // for their rotated label text; see fitRotatedFont for the rest.
	double tmax = 168.0;           // max time (Ma); data now runs to M29n ~164.82 Ma, +margin
	struct Pico { double age; };
	QList<Pico> picos;             // kept sorted by age, like magbarcode.m's UserData ordering
	int draggedPico = -1;
	QScrollBar *scrollBar = nullptr;   // set by MagBarcodeDialog; kept in sync on resize
	QLabel *statusLabel = nullptr;     // set by MagBarcodeDialog; shows the clicked age
	double topMargin = 12.0;           // px gap below the title bar so the 0 Ma tick and the topmost
	                                    // box (Bru) aren't jammed against / clipped by the dialog edge

	// Forward/inverse age<->pixel maps. Every draw AND hit-test goes through these, so the top margin
	// and the scroll offset stay consistent everywhere (change the map once, not in a dozen places).
	double yOf(double age) const { return (age - scrollY) * barHeight + topMargin; }
	double ageOf(double y) const { return (y - topMargin) / barHeight + scrollY; }

	explicit MagBarcodeArea(QWidget *p = nullptr) : QWidget(p) {
		setMouseTracking(true);
		loadData();
	}

	void loadData() {
		QString path = gmtvtkDataDir() + "/Cande_Kent_95.dat";
		QFile f(path);
		if (!f.open(QFile::ReadOnly)) {
			qWarning("MagBarcodeArea: cannot open %s", qUtf8Printable(path));
			return;
		}
		chrons.clear();
		QByteArray data = f.readAll();
		f.close();
		QList<QByteArray> lines = data.split('\n');
		for (const QByteArray &line : lines) {
			if (line.trimmed().isEmpty() || line.startsWith('#')) continue;
			QList<QByteArray> parts = line.simplified().split(' ');
			if (parts.size() < 4) continue;
			Chron c;
			c.name = QString::fromUtf8(parts[0]);   // e.g., "C1n", "C1r.1n"
			c.ageStart = parts[1].toDouble();
			c.ageEnd = parts[2].toDouble();
			c.label = QString::fromUtf8(parts[3]);  // e.g., "1", "a", "2"
			chrons.append(c);
		}
	}

	void setScroll(double y) {
		// Upper bound can go negative when the widget is taller than the whole timescale
		// (std::clamp is UB if hi<lo) - and the view must never scroll past 0 Ma at the top.
		double maxY = std::max(0.0, tmax - height() / barHeight);
		scrollY = std::clamp(y, 0.0, maxY);
		update();
	}

	// Shrinks `g`'s font (down to a small floor) until `text`, drawn horizontally, fits within
	// `availablePx` — used for the rotated period/epoch labels, whose vertical box height varies
	// a lot (Bru is 0.73 Ma tall, Miocene is 19.5 Ma tall). Never skips a label outright; the
	// shortest stages (e.g. Coniacian, 0.55 Ma) may still end up at the size floor.
	static QFont fitRotatedFont(const QPainter &g, const QString &text, double availablePx) {
		QFont f = g.font();
		for (qreal pt = 8.0; pt >= 4.0; pt -= 0.5) {
			f.setPointSizeF(pt);
			if (QFontMetrics(f).horizontalAdvance(text) <= availablePx - 2.0) return f;
		}
		f.setPointSizeF(4.0);
		return f;
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		g.fillRect(rect(), Qt::white);
		int w = width(), h = height();

		// Chrons (left 30%)
		double leftX = 0.1 * w;
		double chronWidth = 0.3 * w;

		for (int i = 0; i < chrons.size(); i++) {
			const Chron &c = chrons[i];
			double yTop = yOf(c.ageStart);
			double yBottom = yOf(c.ageEnd);
			if (yBottom < 0 || yTop > h) continue;

			// Every listed row is a normal-polarity interval (all names end 'n') -> always black,
			// same as magbarcode.m's alternating face-color pattern (odd face = row itself = black,
			// even face = the unlisted reversed gap between rows = white background, no fill).
			// No per-bar outline (patch 'DefaultPatchEdgecolor','none' in the original).
			g.fillRect(QRectF(leftX, yTop, chronWidth, std::max(1.0, yBottom - yTop)), Qt::black);

			// Show label only when age_txt != 'a' (MATLAB: if (~strcmp(age_txt(i),'a')))
			// MATLAB anchors it 'VerticalAlignment','cap' - the TOP of the text sits at yTop, not
			// the baseline (drawText's own y) - so offset the baseline down by the font's ascent.
			if (c.label != "a") {
				g.setPen(Qt::black);
				g.drawText(QPointF(0.42 * w, yTop + g.fontMetrics().ascent()), c.label);
			}
		}

		// Known geomagnetic periods (leftmost: Bru, Mathu, Gau, Gilbert)
		// MATLAB: lines at 0.015 and 0.08 of width, period names at 0.04 — spanning ONLY 0-5.4 Ma,
		// not the whole ruler (unlike the geological-periods brackets below, which span 0-tmax).
		double knownLeft = 0.015 * w, knownRight = 0.08 * w;
		double knownYTop = std::clamp(yOf(0.0), 0.0, (double)h);
		double knownYBot = std::clamp(yOf(5.4), 0.0, (double)h);
		if (knownYBot > knownYTop) {
			g.setPen(QPen(Qt::black, 1));
			g.drawLine(QLineF(knownLeft, knownYTop, knownLeft, knownYBot));
			g.drawLine(QLineF(knownRight, knownYTop, knownRight, knownYBot));
		}

		struct KnownPeriod { QString name; double ageTop, ageBottom; };
		QList<KnownPeriod> known = {
			{"Bru", 0.0, 0.73},
			{"Mathuyama", 0.73, 2.5},
			{"Gauss", 2.5, 3.4},
			{"Gilbert", 3.4, 5.4}
		};

		for (const KnownPeriod &kp : known) {
			double y = yOf(kp.ageTop + (kp.ageBottom - kp.ageTop) / 2.0);
			if (y < 0 || y > h) continue;
			g.setPen(Qt::black);
			g.save();
			QFont f = fitRotatedFont(g, kp.name, (kp.ageBottom - kp.ageTop) * barHeight);
			g.setFont(f);
			// drawText's anchor is the text's START, and after rotate(-90) it extends UPWARD from
			// there — so anchoring at the box's midpoint (as before) put the WHOLE label above the
			// midpoint, overflowing into the box above by ~half the label's length. Shift the
			// anchor down by half the label's rendered width so it's centred on the box instead.
			// Horizontal placement: after rotate(-90) the glyph strip spans [ox-ascent, ox+descent],
			// so anchoring the baseline at a fixed left offset let the ascent spill LEFT past x=0 and
			// get clipped by the window edge. Centre the strip on the column midpoint instead.
			QFontMetrics fm(f);
			double colMid = (knownLeft + knownRight) / 2.0;
			double ox = colMid + (fm.ascent() - fm.descent()) / 2.0;
			double textW = fm.horizontalAdvance(kp.name);
			g.translate(QPointF(ox, y + textW / 2.0));
			g.rotate(-90);
			g.drawText(QPointF(0, 0), kp.name);
			g.restore();

			// Period separator line (except after last)
			double sepY = yOf(kp.ageBottom);
			if (sepY >= 0 && sepY <= h) {
				g.setPen(QPen(Qt::black, 0.5));
				g.drawLine(QLineF(knownLeft, sepY, knownRight, sepY));
			}
		}

		// Time ruler (middle)
		double rulerX = 0.55 * w;
		g.setPen(QPen(Qt::black, 2));
		g.drawLine(QLineF(rulerX, std::max(0.0, yOf(0.0)), rulerX, h));

		for (int i = 0; i <= tmax; i += 5) {
			double y = yOf(i);
			if (y < 0 || y > h) continue;
			g.setPen(QPen(Qt::black, 0.5));
			g.drawLine(QLineF(rulerX, y, 0.58 * w, y));
			g.setPen(Qt::black);
			// Vertically CENTRE the label on its tick: the tick points at the middle of the text, not
			// its top. Baseline = y shifted down by half the text height (ascent-descent)/2.
			QFontMetrics rfm = g.fontMetrics();
			g.drawText(QPointF(0.6 * w, y + (rfm.ascent() - rfm.descent()) / 2.0), QString::number(i) + " Ma");
		}

		for (int i = 0; i <= tmax; i++) {
			double y = yOf(i);
			if (y < 0 || y > h) continue;
			g.setPen(QPen(Qt::black, 0.5));
			g.drawLine(QLineF(rulerX, y, 0.565 * w, y));
		}

		// Geological periods (right). The column (and its Plistocene top box) starts at age 0 —
		// there are no negative times, so clamp the top of the two rails to yOf(0), not y=0.
		double geoLeft = 0.79 * w, geoRight = 0.88 * w;
		double geoTop = std::max(0.0, yOf(0.0));
		g.setPen(QPen(Qt::black, 1));
		g.drawLine(QLineF(geoLeft, geoTop, geoLeft, h));
		g.drawLine(QLineF(geoRight, geoTop, geoRight, h));
		g.drawLine(QLineF(geoLeft, geoTop, geoRight, geoTop));   // cap the box at age 0

		struct Period { QString name; double textAge; double sepAge; };
		QList<Period> periods = {
			{" Plistocene", 1.0, 2.0}, {"Pliocene", 3.5, 5.0},
			{"Miocene", 14.75, 24.5}, {"Oligocene", 31.25, 38.0},
			{"Eocene", 46.5, 55.0}, {"Paleocene", 60.0, 65.0},
			{"Maastrichtian", 69.0, 73.0}, {"Campanian", 78.0, 83.0},
			{"Santonian", 85.2, 87.4}, {"Coniacian", 87.95, 88.5},
			{"Turonian", 89.75, 91.0}, {"Cenomanian", 94.25, 97.5},
			{"Albian", 100.25, 103.0}, {"Aptian", 110.0, 119.0},
			{"Barremian", 122.0, 125.0}, {"Hauterivian", 128.0, 131.0},
			{"Valanginian", 134.5, 138.0}, {"Berriasian", 141.0, 144.0},
			{"Tithonian", 147.0, 150.0}, {"Kimmeridgian", 153.0, 156.0},
			{"Oxfordian", 159.5, 163.0}
		};

		double prevSep = 0.0;   // each period's own box runs [prevSep, sepAge] (textAge is its midpoint)
		for (const Period &p : periods) {
			double boxPx = (p.sepAge - prevSep) * barHeight;
			// Period name (rotated -90 degrees)
			double textY = yOf(p.textAge);
			if (textY >= 0 && textY <= h) {
				g.setPen(Qt::black);
				g.save();
				QFont f = fitRotatedFont(g, p.name, boxPx);
				g.setFont(f);
				// see the KnownPeriod loop above: centre the label on its box instead of anchoring
				// its start at the midpoint (which let it overflow upward into the box above).
				double textW = QFontMetrics(f).horizontalAdvance(p.name);
				g.translate(QPointF(0.84 * w, textY + textW / 2.0));
				g.rotate(-90);
				g.drawText(QPointF(0, 0), p.name);
				g.restore();
			}
			// Separator line after period
			double sepY = yOf(p.sepAge);
			if (sepY >= 0 && sepY <= h) {
				g.setPen(QPen(Qt::black, 0.5));
				g.drawLine(QLineF(geoLeft, sepY, geoRight, sepY));
			}
			prevSep = p.sepAge;
		}

		// Pico markers
		for (const Pico &p : picos) {
			double y = yOf(p.age);
			if (y < 0 || y > h) continue;
			double stickH = 12.0;
			double arrowX = 0.8 * w;
			QPolygonF arrow;
			arrow << QPointF(arrowX, y)
			      << QPointF(arrowX - stickH / 2, y - stickH / 2)
			      << QPointF(arrowX - stickH / 4, y - stickH / 2)
			      << QPointF(arrowX - stickH * 0.55, y)
			      << QPointF(arrowX - stickH / 4, y + stickH / 2)
			      << QPointF(arrowX - stickH / 2, y + stickH / 2);
			g.setBrush(QColor(255, 0, 0, 200));
			g.setPen(Qt::red);
			g.drawPolygon(arrow);
		}
	}

	// Nearest pico to a screen y, within a small pixel tolerance, or -1.
	int picoNear(double py) const {
		for (int i = 0; i < picos.size(); i++) {
			double y = yOf(picos[i].age);
			if (std::abs(py - y) < 10) return i;
		}
		return -1;
	}

	// Age (Ma) at a widget-local y, clamped to the visible timescale for display.
	double ageAtY(double y) const {
		return std::clamp(ageOf(y), 0.0, tmax);
	}

	void showAgeAt(double y) {
		if (statusLabel) statusLabel->setText(QString("Age: %1 Ma").arg(ageAtY(y), 0, 'f', 3));
	}

public:
	// Chron under a widget-local point, restricted to the chrons column (left 30%, see paintEvent's
	// leftX/chronWidth), or nullptr. Public: also used by the gmtvtk_magbarcode_hover_test hook.
	const Chron *chronAt(double x, double y) const {
		double leftX = 0.1 * width(), chronWidth = 0.3 * width();
		if (x < leftX || x > leftX + chronWidth) return nullptr;
		double age = ageOf(y);
		for (const Chron &c : chrons)
			if (age >= c.ageStart && age <= c.ageEnd) return &c;
		return nullptr;
	}
protected:

	// bdn_pico's non-'open' branch: a plain press+drag on an EXISTING marker moves it (never
	// creates one — that's double-click-only, see mouseDoubleClickEvent). Every left click also
	// reads out the clicked age at the bottom of the dialog, per user request.
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) return;
		showAgeAt(e->position().y());
		draggedPico = picoNear(e->position().y());
	}

	// wbm_pico: clamp the drag between the neighbouring picos' ages so markers can't cross. When
	// not dragging, hover shows the chron name/age under the cursor (setMouseTracking is on).
	void mouseMoveEvent(QMouseEvent *e) override {
		if (draggedPico >= 0) {
			double age = ageOf(e->position().y());
			double lo = (draggedPico > 0) ? picos[draggedPico - 1].age : 0.0;
			double hi = (draggedPico < picos.size() - 1) ? picos[draggedPico + 1].age : tmax;
			picos[draggedPico].age = std::clamp(age, lo, hi);
			showAgeAt(e->position().y());
			update();
			return;
		}
		const Chron *c = chronAt(e->position().x(), e->position().y());
		if (c) {
			QToolTip::showText(QCursor::pos(), QString("%1 (%2-%3 Ma)")
			                    .arg(c->name).arg(c->ageStart, 0, 'f', 3).arg(c->ageEnd, 0, 'f', 3), this);
		} else {
			QToolTip::hideText();
		}
	}

	void mouseReleaseEvent(QMouseEvent *) override {
		draggedPico = -1;
	}

	// bdn_MagBar / bdn_pico's 'open' branch: double-click empty space ADDS a marker, double-click
	// an existing marker REMOVES it (a single click only selects/drags, see mousePressEvent).
	void mouseDoubleClickEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) return;
		showAgeAt(e->position().y());
		int hit = picoNear(e->position().y());
		if (hit >= 0) {
			picos.removeAt(hit);
			draggedPico = -1;
			update();
			return;
		}
		double age = ageOf(e->position().y());
		if (age < 0 || age > tmax) return;
		int pos = 0;
		while (pos < picos.size() && picos[pos].age < age) pos++;
		picos.insert(pos, Pico{age});
		update();
	}

	void resizeEvent(QResizeEvent *ev) override {
		QWidget::resizeEvent(ev);
		if (!scrollBar) return;
		int maxVal = std::max(0, (int)(tmax * barHeight - height()));
		scrollBar->setRange(0, maxVal);
		scrollBar->setValue(std::clamp((int)(scrollY * barHeight), 0, maxVal));
	}
};

class MagBarcodeUiLoader : public QUiLoader {
public:
	QWidget *createWidget(const QString &className, QWidget *parent = nullptr,
	                       const QString &name = QString()) override {
		if (className == "MagBarcodeWidget") {
			auto *w = new MagBarcodeArea(parent);
			w->setObjectName(name);
			return w;
		}
		return QUiLoader::createWidget(className, parent, name);
	}
};

class MagBarcodeDialog {
public:
	QDialog *dlg = nullptr;
	MagBarcodeArea *barcodeArea = nullptr;
	QScrollBar *scrollBar = nullptr;

	explicit MagBarcodeDialog(QWidget *parent) {
		MagBarcodeUiLoader loader;
		QFile f(gmtvtkUiDir() + "/magnetic_barcode.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("MagBarcodeDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("MagBarcodeDialog: QUiLoader failed"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);

		barcodeArea = static_cast<MagBarcodeArea *>(dlg->findChild<QWidget *>("barcodeWidget"));
		scrollBar = dlg->findChild<QScrollBar *>("verticalScrollBar");
		if (barcodeArea) barcodeArea->statusLabel = dlg->findChild<QLabel *>("statusLabel");

		if (scrollBar && barcodeArea) {
			barcodeArea->scrollBar = scrollBar;   // resizeEvent keeps the range in sync from here on
			// Start scrolled to the top (0 Ma), like magbarcode.m's initial slider Value=tscal-height
			// (which sets its Y-reversed axes to show [0,height] first). The range set here is a
			// pre-layout best guess; resizeEvent corrects it once the dialog's real size is known.
			scrollBar->setRange(0, std::max(0, (int)(barcodeArea->tmax * barcodeArea->barHeight -
			                                          barcodeArea->height())));
			scrollBar->setValue(0);
			QObject::connect(scrollBar, &QScrollBar::valueChanged, dlg, [this](int value) {
				if (barcodeArea) barcodeArea->setScroll(value / barcodeArea->barHeight);
			});
		}

		QObject::connect(dlg, &QObject::destroyed, dlg, [this]() { delete this; });
	}
};

// ============================================================================================
// BeachballWidget — schematic focal-mechanism "beachball" preview for the elastic-deformation
// dialog. This is NOT yet a full lower-hemisphere double-couple projection (that arrives with the
// deformation compute); it draws two opposing black wedges rotated by the fault strike and
// widened/narrowed by the dip, so the icon updates live as Strike/Dip/Rake are edited. Replace the
// paintEvent body with the proper Aki-Richards projection when the compute math lands.
// ============================================================================================
class BeachballWidget : public QWidget {
public:
	double strike = 0, dip = 45, rake = 90;
	std::function<void()> onClick;   // invoked on click (wired to the Focal Mechanisms demo later)
	bool asCanvas = false;            // true = Focal Meca Studio's big preview: no button frame,
	                                   // real Aki-Richards sectors (via Julia) instead of wedges
	Scene *hostScene = nullptr;       // scene handle for the g_juliaEval round-trip (asCanvas only)

	BeachballWidget(QWidget *parent = nullptr) : QWidget(parent) {
		setMinimumSize(72, 72);
		setCursor(Qt::PointingHandCursor);
		setToolTip("Focal mechanism — click for the Focal Mechanisms demo");
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		QSizePolicy sp = sizePolicy();
		sp.setHeightForWidth(true);
		setSizePolicy(sp);
	}
	void setMechanism(double s, double d, double r) {
		strike = s; dip = d; rake = r;
		if (asCanvas) refreshPrecise();
		update();
	}

	// Forces the LAYOUT to reserve a square rect for this widget (not just the disc it draws
	// inside whatever rect it gets) — the Focal Meca Studio demo dialog stretches this widget to
	// fill the space below the Strike/Dip/Rake sliders, and a non-square reservation reads as a
	// bug (circle floating in a padded rectangle) even though paintEvent already self-centers.
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override { return w; }

protected:
	void mousePressEvent(QMouseEvent *) override { if (onClick) onClick(); }

	// Precise-mode geometry, unit-disk (x,y) origin-centred, y = North — straight from Julia's
	// _focal_demo_sectors (src/focal.jl), which reuses the SAME _focal_patch_meca/_focal_sectors
	// the real catalog beachballs use (_focal_plot). NEVER re-derive this projection here — see
	// .wolf/cerebrum.md "focal-beachball-three-laws" for why that math is one-source-of-truth.
	std::vector<std::pair<QPolygonF, bool>> sectors;   // (polygon, isCompressive)
	QPolygonF nodal1, nodal2;
	bool havePrecise = false;

	static QPolygonF parseCurve(const QString &s) {
		QPolygonF poly;
		const auto pts = s.split(';', Qt::SkipEmptyParts);
		for (const auto &pt : pts) {
			const auto xy = pt.split(',');
			if (xy.size() == 2) poly << QPointF(xy[0].toDouble(), xy[1].toDouble());
		}
		return poly;
	}

	void refreshPrecise() {
		havePrecise = false;
		if (!hostScene || !g_juliaEval) return;
		const QString cmd = QString("InteractiveGMT._focal_demo_sectors(%1,%2,%3)")
								.arg(strike, 0, 'f', 6).arg(dip, 0, 'f', 6).arg(rake, 0, 'f', 6);
		std::vector<char> buf(8192);
		int n = g_juliaEval(hostScene, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
		if (n <= 0) return;
		const QString out = QString::fromUtf8(buf.data(), n);
		const int n1 = out.indexOf("#N1:"), n2 = out.indexOf("#N2:");
		if (n1 < 0 || n2 < 0) return;
		sectors.clear();
		for (const auto &tok : out.left(n1).split('|', Qt::SkipEmptyParts)) {
			const int c = tok.indexOf(':');
			if (c < 0) continue;
			sectors.emplace_back(parseCurve(tok.mid(c + 1)), tok.left(c) == "1");
		}
		nodal1 = parseCurve(out.mid(n1 + 4, n2 - (n1 + 4)));
		nodal2 = parseCurve(out.mid(n2 + 4));
		havePrecise = !sectors.empty();
	}

	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		if (!asCanvas) {
			// Faint button-style frame so it reads as clickable (small icon use only).
			p.setPen(QPen(QColor(150, 150, 150), 1.0));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1.0, height() - 1.0), 4, 4);
		}
		const int side = qMin(width(), height()) - (asCanvas ? 2 : 8);
		QRectF box((width() - side) / 2.0, (height() - side) / 2.0, side, side);

		if (asCanvas && havePrecise) {
			// Unit-disk (x,y), y = North -> screen: cx + x*R, cy - y*R (screen Y is down).
			const double R = side / 2.0, cx = box.center().x(), cy = box.center().y();
			auto toScreen = [&](const QPolygonF &u) {
				QPolygonF s; s.reserve(u.size());
				for (const auto &pt : u) s << QPointF(cx + pt.x() * R, cy - pt.y() * R);
				return s;
			};
			p.setPen(Qt::NoPen);
			for (const auto &[poly, iscomp] : sectors) {
				p.setBrush(iscomp ? Qt::black : Qt::white);
				p.drawPolygon(toScreen(poly));
			}
			// Nodal-plane boundary lines: ALWAYS black, ALWAYS stroked (three-laws rule #2).
			p.setPen(QPen(Qt::black, 1.5));
			p.setBrush(Qt::NoBrush);
			p.drawPolyline(toScreen(nodal1));
			p.drawPolyline(toScreen(nodal2));
			p.drawEllipse(box);
			return;
		}

		// Fallback: schematic wedges (small icon use, or the Julia bridge isn't up yet).
		p.setPen(QPen(Qt::black, 1.5));
		p.setBrush(Qt::white);
		p.drawEllipse(box);
		const double half = qBound(8.0, dip, 90.0);          // each wedge's half-angle (deg)
		const double c    = 90.0 - strike;                   // north-CW strike -> math angle (0 at 3 o'clock)
		p.setBrush(Qt::black);
		p.setPen(Qt::NoPen);
		auto wedge = [&](double centreDeg) {
			p.drawPie(box, int((centreDeg - half) * 16), int((2 * half) * 16));   // Qt angles are 1/16 deg, CCW
		};
		wedge(c);
		wedge(c + 180.0);
		p.setPen(QPen(Qt::black, 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(box);
	}
};

// ============================================================================================
// FocalMecaStudioDialog — "Focal Meca Studio": a standalone Strike/Dip/Rake sandbox for a single
// focal mechanism. Opened by clicking the elastic-deformation dialog's BeachballWidget icon (see
// ElasticDialog's beach->onClick below). Non-modal, one per scene (Scene::focalStudioDlg), same
// lifetime pattern as ElasticDialog itself (see faultRunDialog). The preview reuses
// BeachballWidget in its "asCanvas" precise mode (real Aki-Richards sectors via Julia — see
// .wolf/cerebrum.md "focal-beachball-three-laws"): never a re-derivation of that math here.
// ============================================================================================
class FocalMecaStudioDialog : public QDialog {
public:
	QSlider *sliderStrike, *sliderDip, *sliderRake;
	QLineEdit *editStrike, *editDip, *editRake;
	BeachballWidget *beach;

	FocalMecaStudioDialog(QWidget *parent, Scene *scene, double strike0, double dip0, double rake0)
		: QDialog(parent)
	{
		setWindowTitle("Focal Meca Studio");
		setAttribute(Qt::WA_DeleteOnClose);

		auto makeRow = [this](const QString &label, int lo, int hi, int val,
		                       QSlider *&sliderOut, QLineEdit *&editOut) {
			auto *row = new QHBoxLayout();
			auto *lab = new QLabel(label, this);
			lab->setMinimumWidth(50);
			lab->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			row->addWidget(lab);
			auto *leftBtn = new QToolButton(this);
			leftBtn->setArrowType(Qt::LeftArrow);
			leftBtn->setMaximumSize(20, 20);
			row->addWidget(leftBtn);
			sliderOut = new QSlider(Qt::Horizontal, this);
			sliderOut->setRange(lo, hi);
			sliderOut->setValue(val);
			row->addWidget(sliderOut, 1);
			auto *rightBtn = new QToolButton(this);
			rightBtn->setArrowType(Qt::RightArrow);
			rightBtn->setMaximumSize(20, 20);
			row->addWidget(rightBtn);
			editOut = new QLineEdit(QString::number(val), this);
			editOut->setMaximumWidth(50);
			editOut->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			row->addWidget(editOut);
			QObject::connect(leftBtn, &QToolButton::clicked, sliderOut,
				[sliderOut]{ sliderOut->setValue(sliderOut->value() - 1); });
			QObject::connect(rightBtn, &QToolButton::clicked, sliderOut,
				[sliderOut]{ sliderOut->setValue(sliderOut->value() + 1); });
			return row;
		};

		auto *root = new QVBoxLayout(this);
		auto *paramsCol = new QVBoxLayout();
		paramsCol->addLayout(makeRow("Strike",    0, 360, (int)std::lround(strike0), sliderStrike, editStrike));
		paramsCol->addLayout(makeRow("Dip",       0, 90,  (int)std::lround(dip0),    sliderDip,    editDip));
		paramsCol->addLayout(makeRow("Rake",   -180, 180, (int)std::lround(rake0),   sliderRake,   editRake));
		root->addLayout(paramsCol);

		auto *beachRow = new QHBoxLayout();
		beachRow->addStretch(1);
		beach = new BeachballWidget(this);
		beach->asCanvas = true;
		beach->hostScene = scene;
		beach->setFixedSize(340, 340);
		beachRow->addWidget(beach);
		beachRow->addStretch(1);
		root->addLayout(beachRow);

		auto *btnRow = new QHBoxLayout();
		btnRow->addStretch(1);
		auto *btnGmtComm = new QPushButton("GMT comm", this);
		btnGmtComm->setToolTip("Show the GMT command that reproduces this beachball");
		btnRow->addWidget(btnGmtComm);
		root->addLayout(btnRow);

		// Sliders drive the beachball + the numeric readouts; the readouts drive the sliders back
		// on editingFinished (LOCAL live preview only — no compute/module call from an edit box,
		// per "only the action button executes a dialog" — there is no action button here because
		// there is nothing to commit, this dialog IS the preview).
		auto sync = [this]() {
			beach->setMechanism(sliderStrike->value(), sliderDip->value(), sliderRake->value());
			editStrike->setText(QString::number(sliderStrike->value()));
			editDip->setText(QString::number(sliderDip->value()));
			editRake->setText(QString::number(sliderRake->value()));
		};
		QObject::connect(sliderStrike, &QSlider::valueChanged, this, [sync](int){ sync(); });
		QObject::connect(sliderDip,    &QSlider::valueChanged, this, [sync](int){ sync(); });
		QObject::connect(sliderRake,   &QSlider::valueChanged, this, [sync](int){ sync(); });
		QObject::connect(editStrike, &QLineEdit::editingFinished, this,
			[this]{ sliderStrike->setValue(editStrike->text().toInt()); });
		QObject::connect(editDip, &QLineEdit::editingFinished, this,
			[this]{ sliderDip->setValue(editDip->text().toInt()); });
		QObject::connect(editRake, &QLineEdit::editingFinished, this,
			[this]{ sliderRake->setValue(editRake->text().toInt()); });

		QObject::connect(btnGmtComm, &QPushButton::clicked, this, [this]{
			const QString cmd = QString("GMT.meca((0.0,0.0), strike=%1, dip=%2, rake=%3, mag=5, aki=true)")
									.arg(sliderStrike->value()).arg(sliderDip->value()).arg(sliderRake->value());
			QApplication::clipboard()->setText(cmd);
			QMessageBox box(QMessageBox::Information, "GMT command",
				cmd + "\n\n(copied to clipboard)", QMessageBox::Close, this);
			box.setTextInteractionFlags(Qt::TextSelectableByMouse);
			auto *copyBtn = box.addButton("Copy again", QMessageBox::ActionRole);
			QObject::connect(copyBtn, &QPushButton::clicked, this, [cmd]{ QApplication::clipboard()->setText(cmd); });
			box.exec();
		});

		sync();
	}
};

// Great-circle distance (km) + initial bearing (deg from north, CW) between two lon/lat points.
// Local spherical fallback for the dialog seed when the Julia/GMT host is unavailable (see
// faultLineGeom — the normal path now shares the measure menu's GMT geodesic, so the two agree).
static void geoLineLenAz(double lon1, double lat1, double lon2, double lat2, double& km, double& az) {
	const double D2R = 3.14159265358979323846 / 180.0, R = 6371.0088;
	const double p1 = lat1 * D2R, p2 = lat2 * D2R, dl = (lon2 - lon1) * D2R, dp = p2 - p1;
	const double a = std::sin(dp/2)*std::sin(dp/2) + std::cos(p1)*std::cos(p2)*std::sin(dl/2)*std::sin(dl/2);
	km = 2.0 * R * std::asin(std::min(1.0, std::sqrt(a)));
	const double y = std::sin(dl)*std::cos(p2), x = std::cos(p1)*std::sin(p2) - std::sin(p1)*std::cos(p2)*std::cos(dl);
	az = std::fmod(std::atan2(y, x) / D2R + 360.0, 360.0);     // radians -> deg, wrap to [0,360)
}

// Find the window's Draw-Fault line and report its total length, its first→last strike azimuth, and
// whether it is geographic. Length is km (geographic) or data units (cartesian); strike is deg from
// north, CW. Returns false if there is no fault line. `geog` follows the window CRS when set, else a
// crude lon/lat-range guess (mirrors GMT.guessgeog) so an unreferenced lon/lat fault still reads geo.
static bool faultLineGeom(Scene *s, double& len, double& az, bool& geog) {
	int pi = -1;
	for (size_t i = 0; i < s->polys.size(); ++i) if (s->polys[i].isFault) { pi = (int)i; break; }
	if (pi < 0 || s->polys[pi].v.size() < 2) return false;
	const auto& v = s->polys[pi].v;
	geog = s->crsProj4.find("longlat") != std::string::npos || s->crsProj4.find("latlong") != std::string::npos;
	if (s->crsProj4.empty()) {                                  // unknown CRS -> crude range test
		double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
		for (auto& p : v) { x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]); y0 = std::min(y0, p[1]); y1 = std::max(y1, p[1]); }
		geog = (x0 >= -180 && x1 <= 360 && y0 >= -90 && y1 <= 90);
	}
	// Length + azimuth come from the SAME Julia/GMT geodesic the "Line length…" measure menu uses
	// (_fault_lenaz → _seg_dist_azim → mapproject), so the seeded Length matches what the user measures
	// — no haversine-vs-GMT mismatch. Falls back to the local spherical formula only if the host eval
	// bridge is unavailable.
	if (g_juliaEval) {
		const QString tmp = QDir::tempPath() + "/igmt_faultlen_" +
							QString::number(QDateTime::currentMSecsSinceEpoch()) + ".txt";
		QFile f(tmp);
		if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
			QTextStream ts(&f);
			ts.setRealNumberPrecision(15);
			for (auto& p : v) ts << p[0] << ' ' << p[1] << '\n';
			f.close();
			const QString cmd = QString("InteractiveGMT._fault_lenaz(raw\"%1\",raw\"%2\")")
									.arg(tmp).arg(QString::fromStdString(s->crsProj4));
			std::vector<char> buf(512);
			int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
			QFile::remove(tmp);
			if (n > 0) {
				QStringList p = QString::fromUtf8(buf.data(), n).split('/');
				if (p.size() >= 3) {
					bool a, b; double L = p[0].toDouble(&a), Z = p[1].toDouble(&b);
					if (a && b) { len = L; az = Z; geog = (p[2].toInt() != 0); return true; }
				}
			}
		}
	}

	// Fallback: local spherical haversine (only when the Julia/GMT host is not registered).
	len = 0.0;
	for (size_t k = 1; k < v.size(); ++k) {
		if (geog) { double d, a; geoLineLenAz(v[k-1][0], v[k-1][1], v[k][0], v[k][1], d, a); len += d; }
		else       len += std::hypot(v[k][0] - v[k-1][0], v[k][1] - v[k-1][1]);
	}
	const auto& A = v.front(); const auto& B = v.back();
	if (geog) { double d; geoLineLenAz(A[0], A[1], B[0], B[1], d, az); }
	else        az = std::fmod(std::atan2(B[0] - A[0], B[1] - A[1]) * 180.0 / 3.14159265358979323846 + 360.0, 360.0);
	return true;
}

// Move the fault trace's end vertex so the line matches (strike, len) — port of Mirone's
// edit_FaultStrike_CB / edit_FaultLength_CB. The first vertex is the fixed anchor; the endpoint is
// the direct-geodesic destination for geographic faults (solved in Julia via GMT.geod, like Mirone's
// vreckon) or plain trig for cartesian ones. The line collapses to a clean 2-vertex segment from the
// start, exactly as Mirone sets XData=[x1 lon2], YData=[y1 lat2]. `len` is km (geog) / data units
// (cart). On success returns true and (if requested) the new endpoint. len <= 0 or no fault -> false.
static bool faultApplyGeom(Scene *s, double strike, double len, bool geog,
                           double *lon2o = nullptr, double *lat2o = nullptr) {
	if (!s || len <= 0) return false;
	int pi = -1;
	for (size_t i = 0; i < s->polys.size(); ++i) if (s->polys[i].isFault) { pi = (int)i; break; }
	if (pi < 0 || s->polys[pi].v.empty()) return false;
	Polygon& pg = s->polys[pi];
	const double lon1 = pg.v.front()[0], lat1 = pg.v.front()[1], z0 = pg.v.front()[2];
	double lon2, lat2;
	if (geog) {
		if (!g_juliaFaultGeom) return false;                       // no geodesic solver -> leave trace as-is
		QStringList p = QString::fromUtf8(g_juliaFaultGeom(lon1, lat1, strike, len)).split('/');
		if (p.size() < 2) return false;
		bool a, b; lon2 = p[0].toDouble(&a); lat2 = p[1].toDouble(&b);
		if (!a || !b) return false;
	} else {                                                       // cartesian: azimuth from north, CW
		const double D2R = 3.14159265358979323846 / 180.0;
		lon2 = lon1 + len * std::sin(strike * D2R);
		lat2 = lat1 + len * std::cos(strike * D2R);
	}
	pg.v = { { lon1, lat1, z0 }, { lon2, lat2, z0 } };             // 2-vertex segment from the fixed start
	pg.closed = false;
	polyRebuildLine(s, pg);                                        // re-drapes z + refills the line actor
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
	if (lon2o) *lon2o = lon2;
	if (lat2o) *lat2o = lat2;
	return true;
}

// One filled quad cell over `corners` (no closing dup needed for a polygon cell). VTK triangulates
// the (possibly slightly non-planar, terrain-draped) quad for rendering.
static void faultBuildPlanePD(vtkPolyData *pd, const std::vector<std::array<double,3>>& corners) {
	vtkNew<vtkPoints> pts;
	for (auto& c : corners) pts->InsertNextPoint(c[0], c[1], c[2]);
	vtkNew<vtkCellArray> polys;
	vtkNew<vtkIdList>    ids;
	for (vtkIdType i = 0; i < (vtkIdType)corners.size(); ++i) ids->InsertNextId(i);
	polys->InsertNextCell(ids);
	pd->SetPoints(pts);
	pd->SetPolys(polys);
	pd->Modified();
}

// Build the gray surface-PROJECTION patch as a terrain-DRAPED quad mesh: bilinearly interpolate the
// quad (top edge t0→t1 = the trace, bottom edge b0→b1 = the down-dip projection) into an nu×nv grid
// and sample z on the relief at every node, so the patch HUGS the ground (its top edge follows the
// fault trace draped on the surface) instead of cutting a flat chord through the relief.
static void faultBuildDrapedPatch(Scene *s, vtkPolyData *pd,
								   const std::array<double,3>& t0, const std::array<double,3>& t1,
								   const std::array<double,3>& b0, const std::array<double,3>& b1) {
	double spacing = 0.0;
	const double sx = std::abs(s->gdx), sy = std::abs(s->gdy);
	spacing = (sx > 0 && sy > 0) ? std::min(sx, sy) : std::max(sx, sy);
	const double lenU = std::hypot(t1[0] - t0[0], t1[1] - t0[1]);
	const double lenV = std::hypot(b0[0] - t0[0], b0[1] - t0[1]);
	const int nu = (spacing > 0) ? std::clamp((int)std::ceil(lenU / spacing), 1, 400) : 1;
	const int nv = (spacing > 0) ? std::clamp((int)std::ceil(lenV / spacing), 1, 400) : 1;
	vtkNew<vtkPoints> pts;
	for (int j = 0; j <= nv; ++j) {
		const double v = (double)j / nv;
		for (int i = 0; i <= nu; ++i) {
			const double u = (double)i / nu;
			// bilinear corner blend: top edge t0..t1 at v=0, bottom edge b0..b1 at v=1
			const double x = (1-v)*((1-u)*t0[0] + u*t1[0]) + v*((1-u)*b0[0] + u*b1[0]);
			const double y = (1-v)*((1-u)*t0[1] + u*t1[1]) + v*((1-u)*b0[1] + u*b1[1]);
			double z = (1-v)*((1-u)*t0[2] + u*t1[2]) + v*((1-u)*b0[2] + u*b1[2]);
			const double h = sampleZ(s, x, y); if (!std::isnan(h)) z = h;
			pts->InsertNextPoint(x, y, z);
		}
	}
	vtkNew<vtkCellArray> polys;
	const int stride = nu + 1;
	for (int j = 0; j < nv; ++j) for (int i = 0; i < nu; ++i) {
		const vtkIdType a = j*stride + i, b = a + 1, c = a + stride, d = c + 1;
		vtkNew<vtkIdList> q; q->InsertNextId(a); q->InsertNextId(b); q->InsertNextId(d); q->InsertNextId(c);
		polys->InsertNextCell(q);
	}
	pd->SetPoints(pts);
	pd->SetPolys(polys);
	pd->Modified();
}

// The gray surface-projection patch actor: a filled light-gray quad with a thin black outline. Its
// polygon offset (-22000) lifts it just above the relief but stays BELOW the trace line actor (whose
// line offset is -66000 in polyMakeLineActor), so the orange trace always reads on top of the patch
// — that is how the user tells which long side of the rectangle is the fault trace.
static vtkSmartPointer<vtkActor> faultMakePlaneActor(Scene *s, vtkPolyData *pd) {
	vtkNew<vtkPolyDataMapper> map; map->SetInputData(pd); map->ScalarVisibilityOff();
	vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
	map->SetRelativeCoincidentTopologyPolygonOffsetParameters(0.0, -22000.0);
	auto a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(map);
	a->GetProperty()->SetColor(0.80, 0.80, 0.80);
	a->GetProperty()->EdgeVisibilityOff();     // draped fine mesh: a clean gray fill, not a wireframe
	a->GetProperty()->LightingOff();
	a->PickableOff();
	a->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	return a;
}

// The actual dipping fault plane in 3-D: a solid warm quad with dark edges. It lives in the MAIN
// renderer (normal depth testing) and in the SAME scaled space as the gray patch (xfac,1,zfac*ve),
// with its corners at TRUE buried z (top edge at the deepest trace point, bottom edge W·sin(dip)
// below). Because it sits UNDER the relief it is occluded by the opaque surface from above — visible
// only from below / from angles where the terrain does not block it (hidden entirely in flat-2D).
static vtkSmartPointer<vtkActor> faultMakePlane3DActor(Scene *s, vtkPolyData *pd) {
	vtkNew<vtkPolyDataMapper> map; map->SetInputData(pd); map->ScalarVisibilityOff();
	auto a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(map);
	a->GetProperty()->SetColor(0.85, 0.55, 0.25);
	a->GetProperty()->SetEdgeColor(0.25, 0.12, 0.0);
	a->GetProperty()->EdgeVisibilityOn();
	a->GetProperty()->BackfaceCullingOff();   // a fault plane is two-sided: show it from either face
	a->GetProperty()->LightingOff();
	a->PickableOff();
	a->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	return a;
}

// Flat slip-direction arrows imprinted on the 3-D plane's two faces. A fault is an interface between
// two blocks moving in OPPOSITE senses, so each face carries an arrow: the surface-facing face shows
// the rake direction (slip vector at angle `rake` measured in-plane from strike, up-dip at +90), the
// far face shows rake+180 — the two arrows together read as the relative motion across the plane. The
// actor lives in the SAME scaled space as the plane (xfac,1,zfac·ve); a tiny ±offset along the plane
// normal seats each arrow just off its face so depth-testing occludes the far one from either side.
static vtkSmartPointer<vtkActor> faultMakeArrowsActor(Scene *s, vtkPolyData *pd) {
	vtkNew<vtkPolyDataMapper> map; map->SetInputData(pd); map->ScalarVisibilityOff();
	auto a = vtkSmartPointer<vtkActor>::New();
	a->SetMapper(map);
	a->GetProperty()->SetColor(1.0, 0.92, 0.10);    // bold yellow: pops on the warm plane from either face
	a->GetProperty()->EdgeVisibilityOff();
	a->GetProperty()->BackfaceCullingOff();
	a->GetProperty()->LightingOff();
	a->PickableOff();
	a->SetScale(s->xfac, 1.0, s->zfac * s->ve);
	return a;
}

// Draw / refresh the dipping fault plane — port of Mirone's edit_FaultDip_CB / edit_FaultWidth_CB
// patch update (deform_mansinha.m). The buried 3-D plane is a TRUE dipping rectangle hanging from the
// surface trace: the trace (front→back of the fault polyline) is the top edge; the down-dip edge is
// the down-dip width W decomposed by the dip — horizontal projection off = W·cos(dip) walked along
// azimuth strike+90 (down-dip side), vertical drop = W·sin(dip). So at VE 1 the rendered dip equals
// the true dip and the plate is W wide down-dip (with L:W the trace:down-dip ratio). Geographic faults
// walk the horizontal offset with the SAME geodesic solver as the endpoint move (g_juliaFaultGeom,
// GMT.geod); cartesian ones use plain trig. `strike` = seeded first→last azimuth; `width`/`dip` are km
// & degrees (geographic) / data units & degrees (cartesian). NOTE: this is the geometric plane, NOT
// the Save-fault file boundary (push_save_subfault uses the full-W footprint, a non-geometric Mirone
// representation) — the two are deliberately different.
static void faultUpdatePlane(Scene *s, double width, double dip, double strike, double rake, bool geog, int targetPi = -1) {
	if (!s) return;
	int pi = targetPi;                                        // import targets the just-added fault; dialog uses first isFault
	if (pi < 0) for (size_t i = 0; i < s->polys.size(); ++i) if (s->polys[i].isFault) { pi = (int)i; break; }
	if (pi < 0 || pi >= (int)s->polys.size() || !s->polys[pi].isFault || s->polys[pi].v.size() < 2) return;
	Polygon& pg = s->polys[pi];
	const double D2R = 3.14159265358979323846 / 180.0;
	const double off  = width * std::cos(dip * D2R);               // down-dip horizontal projection (W·cos dip)
	const double vert = width * std::sin(dip * D2R);               // down-dip vertical drop      (W·sin dip)
	const auto& A = pg.v.front();  const auto& B = pg.v.back();     // trace endpoints (the long / top edge)

	bool created = false;   // a plane actor was added this call -> refresh the Scene Objects list at the end
	if (!pg.faultPlanePD) pg.faultPlanePD = vtkSmartPointer<vtkPolyData>::New();
	if (!pg.faultPlane) {
		pg.faultPlane = faultMakePlaneActor(s, pg.faultPlanePD);
		s->ren->AddActor(pg.faultPlane);
		created = true;
	}

	if (!(off > 0) && !(vert > 0)) {                              // zero width: nothing to draw
		pg.faultPlane->VisibilityOff();
		if (pg.faultPlane3D) pg.faultPlane3D->VisibilityOff();
		if (pg.faultArrows) pg.faultArrows->VisibilityOff();
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		return;
	}

	double ox0 = A[0], oy0 = A[1], ox1 = B[0], oy1 = B[1];          // the offset (down-dip) edge endpoints
	if (off > 0) {
		if (geog && g_juliaFaultGeom) {
			QStringList p0 = QString::fromUtf8(g_juliaFaultGeom(A[0], A[1], strike + 90.0, off)).split('/');
			QStringList p1 = QString::fromUtf8(g_juliaFaultGeom(B[0], B[1], strike + 90.0, off)).split('/');
			if (p0.size() >= 2) { ox0 = p0[0].toDouble(); oy0 = p0[1].toDouble(); }
			if (p1.size() >= 2) { ox1 = p1[0].toDouble(); oy1 = p1[1].toDouble(); }
		} else {
			ox0 = A[0] + off * std::cos(strike * D2R);  oy0 = A[1] - off * std::sin(strike * D2R);
			ox1 = B[0] + off * std::cos(strike * D2R);  oy1 = B[1] - off * std::sin(strike * D2R);
		}
	}

	// Draped surface-projection patch: top edge = the trace (A→B), bottom edge = its down-dip
	// projection (ox/oy), the whole patch sampled onto the relief so it hugs the ground. A (near-)
	// vertical fault has zero footprint -> hide the gray patch (the 3-D plane still draws).
	if (off > 0) {
		faultBuildDrapedPatch(s, pg.faultPlanePD,
		                      { A[0], A[1], A[2] }, { B[0], B[1], B[2] },
		                      { ox0, oy0, A[2] }, { ox1, oy1, B[2] });
		pg.faultPlane->VisibilityOn();
	} else {
		pg.faultPlane->VisibilityOff();
	}

	// The buried 3-D dipping plane: a TRUE rectangle. Top edge = the trace (A→B) hung on the trace's
	// own draped relief z (so the plane stays welded to the surface trace); bottom edge = the down-dip
	// offset (ox/oy) dropped by W·sin(dip). The drop is in true grid-z units (km→m ×1000 geographic,
	// data units cartesian). The actor scales z by zfac·ve like the surface, so the plane carries the
	// SAME vertical exaggeration as the relief (true dip at VE 1).
	double zA = sampleZ(s, A[0], A[1]); if (std::isnan(zA)) zA = A[2];
	double zB = sampleZ(s, B[0], B[1]); if (std::isnan(zB)) zB = B[2];
	const double drop = vert * (geog ? 1000.0 : 1.0);
	std::vector<std::array<double,3>> plane3d = {                  // top trace start/end, then bottom off end/start
		{ A[0], A[1], zA }, { B[0], B[1], zB }, { ox1, oy1, zB - drop }, { ox0, oy0, zA - drop } };
	if (!pg.faultPlane3DPD) pg.faultPlane3DPD = vtkSmartPointer<vtkPolyData>::New();
	if (!pg.faultPlane3D) {
		pg.faultPlane3D = faultMakePlane3DActor(s, pg.faultPlane3DPD);
		s->ren->AddActor(pg.faultPlane3D);   // main renderer -> depth-tested -> hidden under the relief
		created = true;
	}
	faultBuildPlanePD(pg.faultPlane3DPD, plane3d);
	pg.faultPlane3D->SetVisibility((pg.faultPlane3DShown && !s->flat2d) ? 1 : 0);

	// Slip arrows imprinted on each face. The basis MUST be built in the actor's SCALED render space
	// (x·xfac, y, z·zfac·ve) — that is where the plane is a true rectangle. Building in raw data coords
	// mixes degrees (x,y) with metres (z) in one "unit" vector and produces a giant, mis-oriented arrow.
	// So: scale the corners to render space, build an orthonormal in-plane basis there (strike Uhat,
	// up-dip Up, normal N forced toward the surface), lay out each flat arrow (shaft quad + head tri) in
	// the (slip, in-plane-perp) frame at angle `rake` (far face gets rake+180), seat it ±e·N off its
	// face, then UN-scale every point back to data coords (the actor re-applies the scale on render).
	if (!pg.faultArrowsPD) pg.faultArrowsPD = vtkSmartPointer<vtkPolyData>::New();
	if (!pg.faultArrows) {
		pg.faultArrows = faultMakeArrowsActor(s, pg.faultArrowsPD);
		s->ren->AddActor(pg.faultArrows);
	}
	{
		const double sx = (s->xfac != 0.0 ? s->xfac : 1.0);
		const double sz = (s->zfac * s->ve != 0.0 ? s->zfac * s->ve : 1.0);
		auto toR = [&](const std::array<double,3>& p, double o[3]){ o[0]=p[0]*sx; o[1]=p[1]; o[2]=p[2]*sz; };
		double R0[3],R1[3],R2[3],R3[3];
		toR(plane3d[0],R0); toR(plane3d[1],R1); toR(plane3d[2],R2); toR(plane3d[3],R3);
		double U[3]  = { R1[0]-R0[0], R1[1]-R0[1], R1[2]-R0[2] };   // strike (along trace A->B), render space
		double Dd[3] = { R3[0]-R0[0], R3[1]-R0[1], R3[2]-R0[2] };   // down-dip (top A -> bottom under A)
		auto nrm = [](double v[3]){ double l = std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if (l>1e-12){v[0]/=l;v[1]/=l;v[2]/=l;} return l; };
		auto dot = [](const double a[3], const double b[3]){ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
		auto crs = [](const double a[3], const double b[3], double o[3]){ o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0]; };
		double Uhat[3] = { U[0], U[1], U[2] };  double Ulen = nrm(Uhat);
		double d = dot(Dd, Uhat);
		double Up[3] = { -(Dd[0]-d*Uhat[0]), -(Dd[1]-d*Uhat[1]), -(Dd[2]-d*Uhat[2]) };  // up-dip (toward surface)
		double Dplen = nrm(Up);
		if (Ulen > 1e-9 && Dplen > 1e-9) {
			double N[3]; crs(Uhat, Up, N); nrm(N); if (N[2] < 0) { N[0]=-N[0]; N[1]=-N[1]; N[2]=-N[2]; }
			const double rr = rake * D2R;
			double slip[3] = { std::cos(rr)*Uhat[0] + std::sin(rr)*Up[0],
			                   std::cos(rr)*Uhat[1] + std::sin(rr)*Up[1],
			                   std::cos(rr)*Uhat[2] + std::sin(rr)*Up[2] };
			double perp[3]; crs(N, slip, perp); nrm(perp);
			double C[3] = { (R0[0]+R1[0]+R2[0]+R3[0])/4.0, (R0[1]+R1[1]+R2[1]+R3[1])/4.0, (R0[2]+R1[2]+R2[2]+R3[2])/4.0 };
			const double L = 0.5 * std::min(Ulen, Dplen);          // fit the smaller plane edge (render space)
			const double e = 0.02 * L;                             // off-face seating along N
			vtkNew<vtkPoints> pts; vtkNew<vtkCellArray> polys;
			// one flat arrow (shaft quad + head triangle), built in render space then un-scaled on insert
			auto addArrow = [&](const double c[3], const double dir[3], const double pp[3]) {
				const double half = 0.5*L, hl = 0.34*L, w = 0.08*L, hw = 0.18*L;
				auto add = [&](double a, double p){ return pts->InsertNextPoint(
					(c[0]+a*dir[0]+p*pp[0]) / sx, (c[1]+a*dir[1]+p*pp[1]), (c[2]+a*dir[2]+p*pp[2]) / sz); };
				vtkIdType s0=add(-half,-w), s1=add(half-hl,-w), s2=add(half-hl,w), s3=add(-half,w);
				vtkNew<vtkIdList> q; q->InsertNextId(s0); q->InsertNextId(s1); q->InsertNextId(s2); q->InsertNextId(s3); polys->InsertNextCell(q);
				vtkIdType h0=add(half-hl,-hw), h1=add(half,0.0), h2=add(half-hl,hw);
				vtkNew<vtkIdList> t; t->InsertNextId(h0); t->InsertNextId(h1); t->InsertNextId(h2); polys->InsertNextCell(t);
			};
			double Cf[3] = { C[0]+e*N[0], C[1]+e*N[1], C[2]+e*N[2] };   // surface-facing face: rake
			double Cb[3] = { C[0]-e*N[0], C[1]-e*N[1], C[2]-e*N[2] };   // far face: rake+180
			double slipB[3] = { -slip[0], -slip[1], -slip[2] };
			double perpB[3]; crs(N, slipB, perpB); nrm(perpB);
			addArrow(Cf, slip,  perp);
			addArrow(Cb, slipB, perpB);
			pg.faultArrowsPD->SetPoints(pts); pg.faultArrowsPD->SetPolys(polys); pg.faultArrowsPD->Modified();
		}
	}
	pg.faultArrows->SetVisibility((pg.faultPlane3DShown && !s->flat2d) ? 1 : 0);

	if (created) rebuildSceneObjects(s);   // a "Fault plane" handle row now exists / must appear
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

// ============================================================================================
// Vertical elastic deformation dialog — Okada (1985) surface-deformation inputs, port of Mirone's
// "Vertical elastic deformation" tool. Fault Geometry (length/width/strike/dip/depth/depth-to-top),
// Dislocation Geometry (strike/rake/slip + N/q discretisation, Hide-fault-planes / SCC toggles, and
// a live Mw magnitude), shear modulus Mu, a coordinate-mode combo, a schematic beachball preview,
// and the shared Griding Line Geometry (GeoGridGeometry, no Ref-grid row). The deformation compute
// is NOT wired yet — Compute / Save fault assemble a `params` string + set `action`, and the dialog
// accept()s; the host menu hook + Julia callback are added later. No Q_OBJECT (lambdas only).
// ============================================================================================
class ElasticDialog : public QDialog {
public:
	QLineEdit *fLen, *fWid, *fStrike, *fDip, *fDepth, *fDepTop;   // Fault Geometry
	QLineEdit *dStrike, *dRake, *dSlip, *dN, *dQ;                 // Dislocation Geometry
	QLineEdit *muEdit;                                            // shear modulus (x10^10)
	QCheckBox *hideCheck, *sccCheck;
	QComboBox *coordCombo;
	QLabel    *mwLabel;
	GeoGridGeometry *geo;
	BeachballWidget *beach;
	Scene *scn = nullptr;   // owning window's scene (for the live fault-trace endpoint update)
	std::function<void(const QString&)> onAction;   // host hook fired by Compute / Save fault (non-modal)

	// ── Import Model Slip support ──────────────────────────────────────────────────────────────
	// A slip model is many rectangular sub-fault patches (s->polys with isSlip), grouped into
	// segments (slipSeg). The dialog gains a Segments + Faults selector: picking a fault loads THAT
	// patch's geometry into the fields; Compute hands the WHOLE model to the host.
	QComboBox *segCombo = nullptr, *faultCombo = nullptr;   // null when not a slip model
	QLabel    *segLab = nullptr, *faultLab = nullptr;
	std::vector<int>              segIds;        // distinct segment ids present, in first-seen order
	std::vector<std::vector<int>> faultsBySeg;  // poly indices per segment (parallel to segIds)
	bool slipMode = false;                       // true when the window holds Import-Model-Slip patches
	bool slipLoading = false;                    // guard: suppress field-edit side effects while loading a patch

	// Collect the window's slip patches into segIds / faultsBySeg (poly indices, in scene order).
	void collectSlip() {
		segIds.clear(); faultsBySeg.clear(); slipMode = false;
		if (!scn) return;
		for (int i = 0; i < (int)scn->polys.size(); ++i) {
			if (!scn->polys[i].isSlip) continue;
			const int sg = scn->polys[i].slipSeg;
			int pos = -1;
			for (int k = 0; k < (int)segIds.size(); ++k) if (segIds[k] == sg) { pos = k; break; }
			if (pos < 0) { pos = (int)segIds.size(); segIds.push_back(sg); faultsBySeg.push_back({}); }
			faultsBySeg[pos].push_back(i);
		}
		slipMode = !segIds.empty();
	}

	// Load one slip patch's dislocation geometry (poly index `pi`) into the dialog fields. Guarded so
	// the field editingFinished handlers don't fire side effects (no fault trace to move in slip mode).
	void loadSlipPatch(int pi) {
		if (!scn || pi < 0 || pi >= (int)scn->polys.size()) return;
		Polygon &pg = scn->polys[pi];
		slipLoading = true;
		if (!std::isnan(pg.faultLength))   fLen->setText(QString::number(pg.faultLength, 'g', 6));
		if (!std::isnan(pg.faultWidth))    fWid->setText(QString::number(pg.faultWidth, 'g', 6));
		if (!std::isnan(pg.faultStrike)) { fStrike->setText(QString::number(pg.faultStrike, 'g', 6));
		                                   dStrike->setText(QString::number(pg.faultStrike, 'g', 6)); }
		if (!std::isnan(pg.faultDip))      fDip->setText(QString::number(pg.faultDip, 'g', 6));
		if (!std::isnan(pg.faultDepthTop)) fDepTop->setText(QString::number(pg.faultDepthTop, 'g', 6));
		if (!std::isnan(pg.faultSlip))     dSlip->setText(QString::number(pg.faultSlip, 'g', 6));
		if (!std::isnan(pg.faultRake))     dRake->setText(QString::number(pg.faultRake, 'g', 6));
		slipLoading = false;
		recomputeDepth(); refreshBeachball(); updateMw();
	}

	// Poly index of the currently-selected fault (or -1). Used by Compute for the start vertex.
	int currentSlipPoly() const {
		if (!slipMode || !segCombo || !faultCombo) return -1;
		const int sp = segCombo->currentIndex(), fp = faultCombo->currentIndex();
		if (sp < 0 || sp >= (int)faultsBySeg.size()) return -1;
		if (fp < 0 || fp >= (int)faultsBySeg[sp].size()) return -1;
		return faultsBySeg[sp][fp];
	}

	// Repopulate the Faults combo from the selected segment, then load the first fault.
	void rebuildFaultCombo() {
		if (!segCombo || !faultCombo) return;
		const int sp = segCombo->currentIndex();
		faultCombo->blockSignals(true);
		faultCombo->clear();
		if (sp >= 0 && sp < (int)faultsBySeg.size())
			for (int j = 0; j < (int)faultsBySeg[sp].size(); ++j)
				faultCombo->addItem(QString("Fault %1").arg(j + 1, 2, 10, QChar('0')));
		faultCombo->setCurrentIndex(0);
		faultCombo->blockSignals(false);
		loadSlipPatch(currentSlipPoly());
	}

	// Assemble the whole model as a patch payload for Compute: patches separated by '|', each
	// "x0/y0/len/wid/strike/dip/depthTop/rake/slip" (x0,y0 = patch's first ring vertex).
	QString slipPayload() const {
		QStringList ps;
		if (scn) for (auto& pg : scn->polys) if (pg.isSlip && !pg.v.empty()) {
			ps << QString("%1/%2/%3/%4/%5/%6/%7/%8/%9")
				.arg(pg.v.front()[0], 0, 'g', 15).arg(pg.v.front()[1], 0, 'g', 15)
				.arg(pg.faultLength).arg(pg.faultWidth).arg(pg.faultStrike).arg(pg.faultDip)
				.arg(pg.faultDepthTop).arg(pg.faultRake).arg(pg.faultSlip);
		}
		return ps.join("|");
	}

	// Live Mw from the seismic moment M0 = mu·L·W·slip (mu in 1e10 Pa, L/W in km -> m, slip in m):
	// Mw = (2/3)·log10(M0) − 6.07. Shown as "--" until L/W/slip/mu are all positive numbers.
	void updateMw() {
		bool a, b, c, d;
		double L = fLen->text().toDouble(&a), W = fWid->text().toDouble(&b);
		double slip = dSlip->text().toDouble(&c), mu = muEdit->text().toDouble(&d);
		if (a && b && c && d && L > 0 && W > 0 && slip > 0 && mu > 0) {
			double M0 = mu * 1e10 * (L * 1e3) * (W * 1e3) * slip;     // N·m
			mwLabel->setText(QString("Mw Magnitude = %1").arg((2.0 / 3.0) * std::log10(M0) - 6.07, 0, 'f', 1));
		} else {
			mwLabel->setText("Mw Magnitude = --");
		}
	}

	void refreshBeachball() {
		beach->setMechanism(fStrike->text().toDouble(), fDip->text().toDouble(), dRake->text().toDouble());
	}

	// Snapshot every field into the Scene so the next open of this (rebuilt) dialog restores them.
	void saveState() {
		if (!scn) return;
		Scene::FaultDlgState &st = scn->faultDlg;
		st.len  = fLen->text();   st.wid    = fWid->text();    st.strike = fStrike->text();
		st.dip  = fDip->text();   st.depth  = fDepth->text();  st.depTop = fDepTop->text();
		st.dStrike = dStrike->text(); st.rake = dRake->text(); st.slip   = dSlip->text();
		st.N    = dN->text();     st.q      = dQ->text();      st.mu     = muEdit->text();
		st.hide = hideCheck->isChecked(); st.scc = sccCheck->isChecked();
		st.coord = coordCombo->currentIndex();
		st.valid = true;
	}

	// Restore the previously-saved non-geometry fields (Length/Strike are re-seeded from the trace).
	void loadState(const Scene::FaultDlgState &st) {
		fLen->setText(st.len);   fWid->setText(st.wid);    fStrike->setText(st.strike);
		fDip->setText(st.dip);   fDepth->setText(st.depth);fDepTop->setText(st.depTop);
		dStrike->setText(st.dStrike); dRake->setText(st.rake); dSlip->setText(st.slip);
		dN->setText(st.N);       dQ->setText(st.q);        muEdit->setText(st.mu);
		hideCheck->setChecked(st.hide); sccCheck->setChecked(st.scc);
		coordCombo->setCurrentIndex(st.coord);
	}

	// Move the fault trace's end vertex to match the typed Strike/Length (delegates to the shared
	// faultApplyGeom core — see below). Geographic vs cartesian is taken from the coordinate combo.
	void applyFaultGeom() {
		if (!scn || slipMode) return;          // slip models have no single trace to move
		bool okS, okL;
		double strike = fStrike->text().toDouble(&okS);
		double len    = fLen->text().toDouble(&okL);
		if (!okS || !okL) return;
		faultApplyGeom(scn, strike, len, coordCombo->currentData().toString() == "geog");
	}

	// Redraw the gray surface-projection patch from the current Width / Dip / Strike. Called whenever
	// any of those (or the trace itself) change, so the patch tracks the fault plane live.
	void updateFaultPlane() {
		if (!scn || slipMode) return;          // slip patches draw no single gray plane preview
		faultUpdatePlane(scn, fWid->text().toDouble(), fDip->text().toDouble(),
						 fStrike->text().toDouble(), dRake->text().toDouble(),
						 coordCombo->currentData().toString() == "geog");
	}

	// Derived bottom Depth = Depth-to-Top + W·sin(dip) (Mirone edit_FaultDip_CB / edit_FaultWidth_CB).
	// Called whenever Dip / Width / Depth-to-Top change so Depth tracks the geometry live.
	void recomputeDepth() {
		const double D2R = 3.14159265358979323846 / 180.0;
		const double w    = fWid->text().toDouble();
		const double dip  = fDip->text().toDouble();
		const double topd = fDepTop->text().toDouble();
		fDepth->setText(QString::number(topd + w * std::sin(dip * D2R), 'g', 6));
	}

	ElasticDialog(QWidget *parent, Scene *scene = nullptr, vtkActor *seedPatch = nullptr) : QDialog(parent) {
		scn = scene;
		collectSlip();          // discover Import-Model-Slip patches (drives the Segments / Faults selectors)
		setWindowTitle("Vertical elastic deformation");
		auto *v = new QVBoxLayout(this);

		// A labelled field with the label centred ABOVE the box (Mirone's table look).
		auto vfield = [this](const QString &lab, QLineEdit *&e, const QString &init) -> QWidget* {
			auto *w  = new QWidget(this);
			auto *vl = new QVBoxLayout(w); vl->setContentsMargins(0, 0, 0, 0); vl->setSpacing(2);
			auto *l  = new QLabel(lab, w); l->setAlignment(Qt::AlignHCenter);
			e = new QLineEdit(init, w); e->setMinimumWidth(80);
			vl->addWidget(l); vl->addWidget(e);
			return w;
		};

		// --- Top row: Fault Geometry | middle column (CONFIRM + Mw + N/q) | Dislocation Geometry ---
		auto *topRow = new QHBoxLayout();

		// Fault Geometry: 2 columns × 3 rows.
		auto *faultGroup = new QGroupBox("Fault Geometry", this);
		auto *fg = new QGridLayout(faultGroup);
		fg->addWidget(vfield("Length", fLen, ""),     0, 0);
		fg->addWidget(vfield("Width",  fWid, ""),     0, 1);
		fg->addWidget(vfield("Strike", fStrike, "0"), 1, 0);
		fg->addWidget(vfield("Dip",    fDip, "25"),   1, 1);
		fg->addWidget(vfield("Depth",  fDepth, ""),   2, 0);
		fg->addWidget(vfield("Depth to Top", fDepTop, "0"), 2, 1);
		topRow->addWidget(faultGroup);

		// Middle column: Segments + Faults selectors (slip models only) then CONFIRM (coordinate mode).
		auto *midCol = new QVBoxLayout();
		midCol->addStretch();
		// Segments / Faults: present only for an Import-Model-Slip window. Picking a fault loads THAT
		// patch's geometry into the fields; Compute hands the whole model to the host.
		segLab = new QLabel("Segments", this);  segLab->setAlignment(Qt::AlignHCenter);
		segCombo = new QComboBox(this);
		faultLab = new QLabel("Faults", this);  faultLab->setAlignment(Qt::AlignHCenter);
		faultCombo = new QComboBox(this);
		midCol->addWidget(segLab);   midCol->addWidget(segCombo);
		midCol->addWidget(faultLab); midCol->addWidget(faultCombo);
		for (QWidget *w : {(QWidget*)segLab, (QWidget*)segCombo, (QWidget*)faultLab, (QWidget*)faultCombo})
			w->setVisible(slipMode);
		auto *confirmLab = new QLabel("CONFIRM", this);
		confirmLab->setStyleSheet("color: red; font-weight: bold;");
		confirmLab->setAlignment(Qt::AlignHCenter);
		midCol->addWidget(confirmLab);
		coordCombo = new QComboBox(this);
		coordCombo->addItem("Geogs", "geog");      // geographic (degrees)
		coordCombo->addItem("Cart",  "cart");      // cartesian (metres / km)
		coordCombo->setToolTip("Coordinate type of the fault position and grid limits");
		midCol->addWidget(coordCombo);
		midCol->addStretch();
		topRow->addLayout(midCol);

		// Dislocation Geometry. Mw lives at the bottom of this box.
		auto *disGroup = new QGroupBox("Dislocation Geometry", this);
		auto *dg = new QGridLayout(disGroup);
		dg->addWidget(vfield("Strike", dStrike, "0"),  0, 0);
		dg->addWidget(vfield("Rake",   dRake, "90"),   0, 1);
		dg->addWidget(vfield("Slip",   dSlip, "1"),    0, 2);
		hideCheck = new QCheckBox("Hide fault planes", disGroup);
		sccCheck  = new QCheckBox("SCC", disGroup);
		sccCheck->setToolTip("Use the SCC (Self-Consistent Crust) Green functions");
		dg->addWidget(hideCheck, 1, 0, 1, 2);
		dg->addWidget(sccCheck,  1, 2);
		dg->addWidget(vfield("N", dN, "20"),  2, 1);     // sub-fault discretisation
		dg->addWidget(vfield("q", dQ, "0.3"), 2, 2);
		mwLabel = new QLabel("Mw Magnitude = --", disGroup);
		dg->addWidget(mwLabel, 3, 0, 1, 3);
		// N/q are only meaningful for the SCC Green functions — disabled until SCC is ticked.
		dN->setEnabled(false); dQ->setEnabled(false);
		QObject::connect(sccCheck, &QCheckBox::toggled, this, [this](bool on) {
			dN->setEnabled(on); dQ->setEnabled(on); });
		topRow->addWidget(disGroup);

		v->addLayout(topRow);

		// --- Mu (shear modulus) row ------------------------------------------------------------
		auto *muRow = new QHBoxLayout();
		muRow->addStretch();
		muRow->addWidget(new QLabel("Mu (x10^10)", this));
		muEdit = new QLineEdit("3.0", this);
		muEdit->setMaximumWidth(80);
		muEdit->setToolTip("Shear modulus / rigidity (×10^10 Pa)");
		muRow->addWidget(muEdit);
		v->addLayout(muRow);

		// --- Griding Line Geometry (reused widget, no Ref-grid row here) + beachball & buttons ----
		auto *botRow = new QHBoxLayout();
		geo = new GeoGridGeometry(this, /*withRefGrid=*/false);
		botRow->addWidget(geo, 1);

		auto *rightCol = new QVBoxLayout();
		beach = new BeachballWidget(this);
		beach->onClick = [this]() {
			if (scn && scn->focalStudioDlg) {
				scn->focalStudioDlg->raise();
				scn->focalStudioDlg->activateWindow();
				return;
			}
			auto *dlg = new FocalMecaStudioDialog(this, scn,
				fStrike->text().toDouble(), fDip->text().toDouble(), dRake->text().toDouble());
			dlg->setAttribute(Qt::WA_DeleteOnClose);
			if (scn) {
				scn->focalStudioDlg = dlg;
				QObject::connect(dlg, &QObject::destroyed, scn->win, [this]{ if (scn) scn->focalStudioDlg = nullptr; });
			}
			dlg->show();
		};
		rightCol->addWidget(beach, 0, Qt::AlignHCenter);
		auto *btnRow = new QHBoxLayout();
		auto *helpBtn    = new QToolButton(this);   helpBtn->setText("?");
		auto *saveBtn    = new QPushButton("Save fault", this);
		auto *computeBtn = new QPushButton("Compute", this);
		// Enter in an edit box must only apply that edit (via editingFinished) — it must NOT trigger a
		// default button (Compute/Save) nor close the dialog. Strip default/auto-default so Return is
		// inert at the dialog level; only an explicit click runs Compute / Save fault.
		for (QPushButton *b : {saveBtn, computeBtn}) { b->setAutoDefault(false); b->setDefault(false); }
		btnRow->addWidget(helpBtn);
		btnRow->addWidget(saveBtn);
		btnRow->addWidget(computeBtn);
		rightCol->addLayout(btnRow);
		botRow->addLayout(rightCol);
		v->addLayout(botRow);

		// Prefill the geometry from the window's loaded grid/image (same logic as grdsample).
		if (scene) {
			if (scene->gnx > 1 && scene->gny > 1) {
				geo->fillGeometry(QString("%1/%2/%3/%4/%5/%6/%7/%8")
					.arg(scene->gx0).arg(scene->gx1).arg(scene->gy0).arg(scene->gy1)
					.arg(scene->gdx).arg(scene->gdy).arg(scene->gnx).arg(scene->gny));
			} else if (scene->x1 > scene->x0 && scene->y1 > scene->y0) {
				geo->fillGeometry(QString("%1/%2/%3/%4////")
					.arg(scene->x0).arg(scene->x1).arg(scene->y0).arg(scene->y1));
			}
			// Restore the user's previously-typed fields (the dialog is rebuilt every open).
			const bool hadState = scene->faultDlg.valid;
			if (hadState) loadState(scene->faultDlg);

			// Seed Length + Strike from the drawn fault line (its raison d'être) — ALWAYS, so a vertex
			// drag since the last open is honoured. Length is km for a geographic fault, data units
			// otherwise; Strike is the first→last azimuth. The combo follows the same geographic guess
			// only on a fresh fault; once the user has a saved state we keep their coordinate choice.
			double flen = 0, faz = 0; bool fgeo = false;
			if (faultLineGeom(scene, flen, faz, fgeo)) {
				fLen->setText(QString::number(flen, 'g', 6));
				fStrike->setText(QString::number(faz, 'g', 6));
				dStrike->setText(QString::number(faz, 'g', 6));
				if (!hadState) {
					coordCombo->setCurrentIndex(fgeo ? 0 : 1);
					// First time only (no saved state): seed Width = Length/4 and the derived Depth,
					// exactly as Mirone's edit_FaultWidth_CB does on a fresh fault. A manual edit later
					// is preserved by the saved state, so this auto-seed never overrides the user.
					const double D2R = 3.14159265358979323846 / 180.0;
					const double w    = flen / 4.0;
					const double dip  = fDip->text().toDouble();       // default 25
					const double topd = fDepTop->text().toDouble();    // default 0
					fWid->setText(QString::number(w, 'g', 6));
					fDepth->setText(QString::number(topd + w * std::cos((90.0 - dip) * D2R), 'g', 6));
				}
			}

			// Seed Slip/Rake AND the fault geometry (Strike/Dip/Width/Depth-to-Top) from the fault if it
			// carries them (set by Import Trace Fault, which reads them from the sub-fault file — slip
			// already converted cm->m, Width = ny·Dy total down-dip). The imported file is the authority
			// for that fault, so these override the trace-seeded / remembered values. NaN = not imported
			// -> leave the dialog's own value untouched. Same fault faultLineGeom picked (first isFault).
			for (auto& pg : scene->polys) if (pg.isFault) {
				if (!std::isnan(pg.faultSlip)) dSlip->setText(QString::number(pg.faultSlip, 'g', 6));
				if (!std::isnan(pg.faultRake)) dRake->setText(QString::number(pg.faultRake, 'g', 6));
				if (!std::isnan(pg.faultStrike)) {
					fStrike->setText(QString::number(pg.faultStrike, 'g', 6));
					dStrike->setText(QString::number(pg.faultStrike, 'g', 6));
				}
				if (!std::isnan(pg.faultDip))      fDip->setText(QString::number(pg.faultDip, 'g', 6));
				if (!std::isnan(pg.faultWidth))    fWid->setText(QString::number(pg.faultWidth, 'g', 6));
				if (!std::isnan(pg.faultDepthTop)) fDepTop->setText(QString::number(pg.faultDepthTop, 'g', 6));
				if (!std::isnan(pg.faultWidth) || !std::isnan(pg.faultDip) || !std::isnan(pg.faultDepthTop))
					recomputeDepth();              // derive bottom Depth from the file's Width/Dip/Depth-to-Top
				refreshBeachball(); updateMw();
				break;
			}
		}

		// Slip-model selectors: populate Segments, then select the patch the user opened the dialog from
		// (its context menu passed the clicked patch's line actor). Picking a different fault loads that
		// patch's geometry; changing segment repopulates the Faults combo.
		if (slipMode && segCombo && faultCombo) {
			segCombo->blockSignals(true);
			for (int k = 0; k < (int)segIds.size(); ++k)
				segCombo->addItem(QString("Segment %1").arg(segIds[k] + 1));
			int seedSeg = 0, seedFault = 0;
			if (seedPatch) for (int k = 0; k < (int)faultsBySeg.size(); ++k)
				for (int j = 0; j < (int)faultsBySeg[k].size(); ++j) {
					const int pi = faultsBySeg[k][j];
					if (pi >= 0 && pi < (int)scn->polys.size() && scn->polys[pi].line.Get() == seedPatch) { seedSeg = k; seedFault = j; }
				}
		// Explicit: always start at Segment 1 (index 0)
			if (segCombo->count() < 1) segCombo->addItem(QString("Segment 1"));
			segCombo->setCurrentIndex(0);
			segCombo->blockSignals(false);
			rebuildFaultCombo();                                   // fills Faults for seedSeg, loads its first
			if (seedFault >= 0 && seedFault < faultCombo->count()) faultCombo->setCurrentIndex(seedFault);
		else faultCombo->setCurrentIndex(0);                   // explicit: always start at Fault 1
			loadSlipPatch(currentSlipPoly());                      // load the exact clicked patch
			QObject::connect(segCombo,   &QComboBox::currentIndexChanged, this, [this]{ rebuildFaultCombo(); });
			QObject::connect(faultCombo, &QComboBox::currentIndexChanged, this, [this]{ loadSlipPatch(currentSlipPoly()); });
		}

		// Live coupling: Strike mirrored between the two boxes; beachball + Mw track their inputs.
		// Editing Strike or Length also moves the fault trace's end vertex (Mirone edit_Fault*_CB).
		// Strike / Length drive ONLY the plane PREVIEW. They must NEVER rewrite the drawn trace's
		// vertices: editingFinished also fires when the dialog loses focus / closes, so calling
		// applyFaultGeom() here silently re-rotated the user's trace on every close. The trace is the
		// user's data — left untouched. (applyFaultGeom stays available for an explicit action only.)
		QObject::connect(fStrike, &QLineEdit::editingFinished, this, [this]{
			dStrike->setText(fStrike->text()); refreshBeachball(); updateFaultPlane(); });
		QObject::connect(dStrike, &QLineEdit::editingFinished, this, [this]{
			fStrike->setText(dStrike->text()); refreshBeachball(); });
		QObject::connect(fLen,  &QLineEdit::editingFinished, this, [this]{ updateFaultPlane(); });
		QObject::connect(fDip,  &QLineEdit::editingFinished, this, [this]{ refreshBeachball(); recomputeDepth(); updateFaultPlane(); });
		QObject::connect(fWid,  &QLineEdit::editingFinished, this, [this]{ recomputeDepth(); updateFaultPlane(); });
		QObject::connect(fDepth, &QLineEdit::editingFinished, this, [this]{ updateFaultPlane(); });
		QObject::connect(fDepTop,&QLineEdit::editingFinished, this, [this]{ recomputeDepth(); updateFaultPlane(); });
		QObject::connect(coordCombo, &QComboBox::currentIndexChanged, this, [this]{ updateFaultPlane(); });
		QObject::connect(dRake, &QLineEdit::editingFinished, this, [this]{ refreshBeachball(); updateFaultPlane(); });
		for (QLineEdit *e : {fLen, fWid, dSlip, muEdit})
			QObject::connect(e, &QLineEdit::editingFinished, this, [this]{ updateMw(); });
		// Persist every field to the Scene on change, so closing + reopening the dialog restores them.
		// (Enter-defocus is handled app-wide by EnterDefocusFilter, installed on the QApplication.)
		for (QLineEdit *e : {fLen, fWid, fStrike, fDip, fDepth, fDepTop, dStrike, dRake, dSlip, dN, dQ, muEdit})
			QObject::connect(e, &QLineEdit::editingFinished, this, [this]{ saveState(); });
		QObject::connect(hideCheck, &QCheckBox::toggled, this, [this]{ saveState(); });
		QObject::connect(sccCheck,  &QCheckBox::toggled, this, [this]{ saveState(); });
		QObject::connect(coordCombo, &QComboBox::currentIndexChanged, this, [this]{ saveState(); });
		saveState();          // snapshot the seeded initial state (so first reopen restores it)
		updateMw();
		refreshBeachball();
		updateFaultPlane();   // draw the gray surface-projection patch for the seeded geometry

		// Help.
		QObject::connect(helpBtn, &QToolButton::clicked, this, [this]{
			QMessageBox::information(this, "Vertical elastic deformation",
				"Okada (1985) elastic surface deformation.\n\n"
				"Fault Geometry: rupture length, width, strike, dip, depth and depth-to-top.\n"
				"Dislocation Geometry: strike, rake and slip; N/q control sub-fault discretisation.\n"
				"Mu: shear modulus (×10^10 Pa). Mw is derived from L·W·slip·Mu.\n"
				"Griding Line Geometry: the output grid region and spacing.");
		});

		// Compute / Save fault: assemble params + fire the host hook. The dialog is NON-MODAL, so it
		// stays open (no accept()/close) — the window keeps working while it is up and the user can
		// keep editing. Compute math + the Julia hook are wired through onAction.
		auto assemble = [this](const QString &act, const QString &savePath) {
			saveState();
			QString params = QString("%1;%2;%3;%4;%5;%6;%7;%8;%9;%10;%11;%12;%13;%14;%15;%16")
				.arg(act).arg(coordCombo->currentData().toString())
				.arg(fLen->text().trimmed()).arg(fWid->text().trimmed())
				.arg(fStrike->text().trimmed()).arg(fDip->text().trimmed())
				.arg(fDepth->text().trimmed()).arg(fDepTop->text().trimmed())
				.arg(dRake->text().trimmed()).arg(dSlip->text().trimmed())
				.arg(hideCheck->isChecked() ? "1" : "0").arg(sccCheck->isChecked() ? "1" : "0")
				.arg(dN->text().trimmed()).arg(dQ->text().trimmed())
				.arg(muEdit->text().trimmed())
				.arg(geo->region() + ";" + geo->inc());   // R then I, tail of the string
			// Append the fault's start vertex (first vertex of the drawn fault trace) — okada needs it as
			// x_start/y_start (UpperLeft corner of the fault plane); it is not otherwise in the dialog.
			double fx0 = std::numeric_limits<double>::quiet_NaN(), fy0 = fx0;
			if (slipMode) {                                   // slip model: start vertex = selected patch's corner
				const int pi = currentSlipPoly();
				if (pi >= 0 && pi < (int)scn->polys.size() && !scn->polys[pi].v.empty()) {
					fx0 = scn->polys[pi].v.front()[0]; fy0 = scn->polys[pi].v.front()[1]; }
			} else if (scn) for (auto& pg : scn->polys) if (pg.isFault && !pg.v.empty()) {
				fx0 = pg.v.front()[0]; fy0 = pg.v.front()[1]; break; }
			params += ";" + QString::number(fx0, 'g', 15) + ";" + QString::number(fy0, 'g', 15);
			params += ";" + savePath;     // field 20: output file for "save" (empty for compute)
			// Field 21 (slip models only): the WHOLE model as "MODELSLIP=" + patch payload, so Compute
			// deforms with every sub-fault, not just the selected one.
			if (slipMode) params += ";MODELSLIP=" + slipPayload();
			if (onAction) onAction(params);
		};
		QObject::connect(computeBtn, &QPushButton::clicked, this, [assemble, computeBtn]() {
			computeBtn->setStyleSheet("background:#d4831a; color:white;");  // busy until Julia returns
			computeBtn->setEnabled(false);
			QApplication::processEvents();
			assemble("compute", QString());
			computeBtn->setStyleSheet("");
			computeBtn->setEnabled(true);
		});
		// Save fault: pick the output .dat first (Mirone's put_or_get_file), then hand the path to Julia
		// which writes the sub-fault format. Cancelling the file dialog aborts — no host hook fired.
		QObject::connect(saveBtn, &QPushButton::clicked, this, [this, assemble, saveBtn]() {
			QString fn = QFileDialog::getSaveFileName(this, "Save fault (sub-fault format)",
				prefStartDir("fault.dat"), "Data file (*.dat *.txt);;All files (*.*)");
			if (fn.isEmpty()) return;     // cancelled
			rememberStartDir(fn);
			saveBtn->setStyleSheet("background:#d4831a; color:white;");  // busy until Julia returns
			saveBtn->setEnabled(false);
			QApplication::processEvents();
			assemble("save", fn);
			saveBtn->setStyleSheet("");
			saveBtn->setEnabled(true);
		});

	}
	// Clear focus from any focused widget when clicking dialog background (safety: prevents accidental typing)
		void mousePressEvent(QMouseEvent *e) override {
			QWidget *w = childAt(e->pos());
			// Clear focus if NOT clicking on a focusable widget (edit box, combo, checkbox, button)
			if (!w || (w->focusPolicy() & Qt::ClickFocus) == 0) {
				if (QWidget *fw = focusWidget()) fw->clearFocus();
			}
			QDialog::mousePressEvent(e);
		}

};

// Open the Vertical elastic deformation dialog for the current window (used by a fault line's first
// property — forward-declared in 55_lineprops.cpp). The dialog prefills its Griding Line Geometry
// from the window's loaded grid/image (same path as grdsample). On accept, hands params to Julia.
static void faultRunDialog(Scene *s, vtkActor *seedPatch) {
	if (!s || !s->win) return;
	// NON-MODAL: show() (not exec()) so the main window stays interactive while the dialog is up —
	// editing Strike/Length must update the trace live, not block the UI. Heap-allocated + delete-on-
	// close so it manages its own lifetime; one dialog per window at a time (reuse if already open).
	if (s->elasticDlg) { s->elasticDlg->raise(); s->elasticDlg->activateWindow(); return; }
	ElasticDialog *dlg = new ElasticDialog(s->win, s, seedPatch);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	s->elasticDlg = dlg;
	// The fault plane (gray surface patch + buried 3-D plane) PERSISTS after the dialog closes — it is
	// a permanent scene element with its own Scene Objects handle, not a dialog-time preview. Only the
	// dialog pointer is cleared here; the plane is removed via its handle's "Remove" or by deleting the
	// fault (polygonEraseOne).
	QObject::connect(dlg, &QObject::destroyed, s->win, [s]{ s->elasticDlg = nullptr; });
	dlg->onAction = [s](const QString& params) {
		if (g_juliaElastic) g_juliaElastic(s, params.toUtf8().constData());
		else s->win->statusBar()->showMessage("Elastic deformation: compute not wired yet", 3000);
	};
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}

// ============================================================================================
// NSWING tsunami modelling — port of Mirone's swan_options.m (src_figs/swan_options.m) driving the
// `nswing` executable. A modal options dialog mirroring the original window: Source / Nest grids +
// nesting level, output target (grids / ANUGA .sww / MOST .nc) + name stem, the per-field outputs
// (surface / total water / Max water -M / 3D netCDF -Z / velocity / momentum), Manning friction -X,
// maregraphs, and the run parameters (cycles -N, jump -J, time step -t, saving step). RUN assembles a
// newline-separated "key=value" block and hands it to Julia (g_juliaNswing), which builds + launches
// the nswing command line. First iteration — semantics will be refined. No Q_OBJECT (lambdas only).
class NswingDialog : public QDialog {
public:
	QString params;                       // "key=value\n…" on RUN, else empty
	QLineEdit *srcEdit, *nestEdit, *nameEdit, *manningEdit;
	QLineEdit *maregInEdit, *maregOutEdit, *cumintEdit;
	QLineEdit *cyclesEdit, *jumpEdit, *dtEdit, *grnEdit;
	QComboBox *levelCombo;
	std::map<int, QString> nestNames;     // level -> in-scene "layerN" name (populateFromScene)
	QRadioButton *rGrids, *rAnuga, *rMost;
	QRadioButton *rSurf, *rTotal;
	QCheckBox *cMax, *cVel, *cMom, *cMareg, *cGeog, *cCoriolis;
	QString bcPath;                       // "Bordering": optional boundary-condition file (-B)
	QPushButton *btnBorder = nullptr;     // label mirrors bcPath ("Bordering" / "Bordering: <file>")
	Scene *scene_ = nullptr;              // owning window's scene (grid inventory + RUN callback target)
	std::map<QLineEdit*, std::function<void()>> fileBrowsers;   // edit -> its "..." browse action (fileRow); double-click runs it too
	bool nestReady_ = false;              // gate: don't run the load-time nest check during construction/seed
	QString lastNestChecked_;             // dedup so the same path isn't re-checked on every keystroke/refresh

	NswingDialog(QWidget *parent, Scene *scene = nullptr) : QDialog(parent), scene_(scene) {
		setWindowTitle("NSWING tsunami options");
		setMinimumWidth(420);
		auto *v = new QVBoxLayout(this);

		// a labelled file row: <label> [lineedit] [...]  (browse with the given filter)
		auto fileRow = [this](const QString &label, QLineEdit *&edit, const QString &filter) -> QLayout* {
			auto *h = new QHBoxLayout();
			auto *lab = new QLabel(label, this); lab->setMinimumWidth(48);
			h->addWidget(lab);
			edit = new QLineEdit(this); edit->setMinimumWidth(240);
			h->addWidget(edit);
			auto *btn = new QToolButton(this); btn->setText("...");
			h->addWidget(btn);
			auto browse = [this, edit, filter]() {
				QString p = QFileDialog::getOpenFileName(this, "Select file", prefStartDir(), filter);
				if (!p.isEmpty()) { edit->setText(p); rememberStartDir(p); }
			};
			QObject::connect(btn, &QToolButton::clicked, this, browse);
			edit->installEventFilter(this);      // double-click on the box itself also opens the picker
			fileBrowsers[edit] = browse;
			return h;
		};

		// --- Input grids: Source + Nest + nesting level -----------------------------------------
		auto *gIn = new QGroupBox("Input grids", this);
		auto *iv  = new QVBoxLayout(gIn);
		iv->addLayout(fileRow("Source", srcEdit,  "Grid files (*.grd *.nc);;All files (*)"));
		iv->addLayout(fileRow("Nest",   nestEdit, "Grid files (*.grd *.nc);;All files (*)"));
		levelCombo = new QComboBox(gIn);
		levelCombo->addItems({"0 -- level ready to use", "1", "2", "3", "4", "5"});
		levelCombo->setToolTip("Nesting level of the Nest grid (0 = no nesting / ready to use)");
		iv->addWidget(levelCombo);
		v->addWidget(gIn);

		// Seed Source + the nest chain from the window's grids: pick an "Okada z" grid as Source and every
		// FILLED (non-all-zero) "layerN" (in N order) as the nesting chain, mirroring how the user built
		// them in this window. Each found "layerN" relabels levelCombo's item N to "N -- level ready to use"; picking that
		// item copies its grid name into the Nest edit box (nestNames, populateFromScene).
		populateFromScene();
		QObject::connect(levelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		                  this, [this](int idx) { showLevel(idx); });
		QObject::connect(nestEdit, &QLineEdit::textChanged, this, [this](const QString &txt) {
			const int lvl = levelCombo->currentIndex();
			if (lvl <= 0) return;               // level 0 has no file, its box is disabled anyway
			nestNames[lvl] = txt.trimmed();
			refreshLevelCombo();
			checkNestFile();                    // validate the moment a nest grid FILE is loaded/typed
		});
		// Start on the next OPEN level (level 1 the first time — nothing filled yet), not level 0, so
		// the Nest box is immediately ready to accept that level's grid name. From here on, only that
		// one open level or an already-filled one (to replace its name) can ever be selected.
		levelCombo->setCurrentIndex(std::min(nextOpenLevel(), levelCombo->count() - 1));
		showLevel(levelCombo->currentIndex());

		// --- Bordering: pick an (experimental) boundary-condition file (-B) ----------------------
		btnBorder = new QPushButton("Bordering", this);
		btnBorder->setToolTip("Select a boundary-condition ASCII file (nswing -O, experimental)");
		QObject::connect(btnBorder, &QPushButton::clicked, this, [this]() {
			QString p = QFileDialog::getOpenFileName(this, "Select boundary-condition file", prefStartDir(),
			                                         "BC files (*.dat *.txt);;All files (*)");
			if (!p.isEmpty()) { bcPath = p; rememberStartDir(p); btnBorder->setText("Bordering: " + QFileInfo(p).fileName()); }
		});
		v->addWidget(btnBorder);

		// --- Output target + name stem ----------------------------------------------------------
		auto *gOut = new QGroupBox("Output", this);
		auto *ov   = new QVBoxLayout(gOut);
		auto *orow = new QHBoxLayout();
		rGrids = new QRadioButton("3D nc", gOut); rGrids->setChecked(true);
		rGrids->setToolTip("Save simulation in a 3D betCDF file");
		rAnuga = new QRadioButton("ANUGA .sww", gOut); rAnuga->setToolTip("Single netCDF in ANUGA .sww format (-A)");
		rMost  = new QRadioButton("MOST .nc", gOut);   rMost->setToolTip("MOST netCDF triplet (-n)");
		auto *gOutMode = new QButtonGroup(this);
		gOutMode->addButton(rGrids); gOutMode->addButton(rAnuga); gOutMode->addButton(rMost);
		orow->addWidget(rGrids); orow->addWidget(rAnuga); orow->addWidget(rMost);
		ov->addLayout(orow);
		auto *nrow = new QHBoxLayout();
		nrow->addWidget(new QLabel("Name", gOut));
		nameEdit = new QLineEdit(gOut);
		nameEdit->setToolTip("Output name stem / file name (grids are numbered using this stem)");
		nrow->addWidget(nameEdit);
		ov->addLayout(nrow);
		v->addWidget(gOut);

		// --- Per-field outputs (active only for the "Output grids" target, as in Mirone) ---------
		auto *gFld = new QGroupBox("Fields", this);
		auto *fg   = new QGridLayout(gFld);
		rSurf  = new QRadioButton("Surface level", gFld); rSurf->setChecked(true);
		rTotal = new QRadioButton("Total water",   gFld); rTotal->setToolTip("Grids with total water depth (-D)");
		auto *gField = new QButtonGroup(this); gField->addButton(rSurf); gField->addButton(rTotal);
		cMax = new QCheckBox("Max water", gFld); cMax->setToolTip("Also write a grid with the max water level (nswing -M)");
		cVel = new QCheckBox("Velocity",  gFld); cVel->setToolTip("Write velocity grids (-S, sufixes _U/_V)");
		cMom = new QCheckBox("Momentum",  gFld); cMom->setToolTip("Write momentum grids (-H)");
		cCoriolis = new QCheckBox("Coriolis", gFld); cCoriolis->setToolTip("Add the Coriolis effect.");
		fg->addWidget(rSurf, 0, 0); fg->addWidget(rTotal, 0, 1); fg->addWidget(cCoriolis, 0, 2);
		fg->addWidget(cVel,  1, 0); fg->addWidget(cMom, 1, 1); fg->addWidget(cMax, 1, 2);
		// Manning friction (-X) — the entry missing from the original window.
		auto *mrow = new QHBoxLayout();
		mrow->addWidget(new QLabel("Manning friction", gFld));
		manningEdit = new QLineEdit(gFld);
		manningEdit->setPlaceholderText("e.g. 0.025  (or comma-separated per level)");
		manningEdit->setToolTip("Manning friction coefficient(s) (nswing -X<manning0[,manning1,…]>)");
		mrow->addWidget(manningEdit);
		fg->addLayout(mrow, 2, 0, 1, 3);
		v->addWidget(gFld);

		// --- Maregraphs -------------------------------------------------------------------------
		auto *gMar = new QGroupBox("Maregraphs", this);
		auto *mv   = new QVBoxLayout(gMar);
		auto *crow = new QHBoxLayout();
		cMareg = new QCheckBox("Maregraphs", gMar);
		cMareg->setToolTip("Compute water height at maregraph locations");
		crow->addWidget(cMareg);
		crow->addStretch();
		crow->addWidget(new QLabel("Saving step", gMar));
		cumintEdit = new QLineEdit("1", gMar); cumintEdit->setFixedWidth(50);
		cumintEdit->setToolTip("Maregraph saving step (time = Time step * this)");
		crow->addWidget(cumintEdit);
		mv->addLayout(crow);
		mv->addLayout(fileRow("In file",  maregInEdit,  "Maregraph (*.dat *.xy);;All files (*)"));
		mv->addLayout(fileRow("Out file", maregOutEdit, "Maregraph (*.dat *.xy);;All files (*)"));
		v->addWidget(gMar);

		// --- Run parameters ---------------------------------------------------------------------
		auto *gRun = new QGroupBox(this);
		auto *rg   = new QGridLayout(gRun);
		auto numEdit = [this](const QString &val) { auto *e = new QLineEdit(val, this); e->setFixedWidth(70); return e; };
		cyclesEdit = numEdit("1010"); jumpEdit = numEdit("0"); dtEdit = numEdit(""); grnEdit = numEdit("10");
		cyclesEdit->setToolTip("Number of cycles (nswing -N)");
		jumpEdit->setToolTip("Do not output before this modeling time, seconds (-P)");
		dtEdit->setToolTip("Time step of the simulation, seconds (-t)");
		grnEdit->setToolTip("Save grids at this cycle interval (the <int> of -G/-Z)");
		rg->addWidget(new QLabel("N\xC2\xBA of cycles", gRun), 0, 0); rg->addWidget(cyclesEdit, 0, 1);
		rg->addWidget(new QLabel("Jump initial", gRun),       0, 2); rg->addWidget(jumpEdit,   0, 3);
		rg->addWidget(new QLabel("Time step (sec)", gRun),    1, 0); rg->addWidget(dtEdit,     1, 1);
		rg->addWidget(new QLabel("Saving step (cycles)", gRun), 1, 2); rg->addWidget(grnEdit,  1, 3);
		cGeog = new QCheckBox("Geographic coordinates", gRun);
		cGeog->setToolTip("Grids are in geographical coordinates (nswing -f)");
		rg->addWidget(cGeog, 2, 0, 1, 4);
		v->addWidget(gRun);

		// --- Prefill Name / Time step / Geographic from the window's OWN bathymetry grid ---------
		// Time step: Mirone's CFL estimate (src_figs/tintol.m L55-58): dtCFL = dx / sqrt(|zmin|*g) / 2,
		// dx = min(x_inc,y_inc) in METRES (degrees * 111000 if geographic). Geographic detection mirrors
		// faultLineGeom's CRS/bbox heuristic above. Name: "tsu" beside the bathymetry's source file.
		if (scene_) {
			bool geog = scene_->crsProj4.find("longlat") != std::string::npos ||
			            scene_->crsProj4.find("latlong") != std::string::npos;
			if (scene_->crsProj4.empty())
				geog = (scene_->gx0 >= -180 && scene_->gx1 <= 360 && scene_->gy0 >= -90 && scene_->gy1 <= 90);
			cGeog->setChecked(geog);

			double dx = std::min(scene_->gdx, scene_->gdy);
			if (geog) dx *= 111000.0;
			double depth = std::fabs(scene_->zmin);
			if (dx > 0 && depth > 0)
				dtEdit->setText(QString::number(dx / std::sqrt(depth * 9.8) / 2.0, 'f', 3));

			if (g_juliaEval) {
				std::vector<char> buf(512);
				int n = g_juliaEval(scene_, "InteractiveGMT._nswing_default_name(fig.h)", buf.data(), (int)buf.size());
				nameEdit->setText(n > 0 ? QString::fromUtf8(buf.data(), n) : "tsu");
			} else {
				nameEdit->setText("tsu");
			}
		}

		// Fields/Manning only make sense for the grids target (-G); grey them out otherwise.
		auto syncFields = [this, gFld]() { gFld->setEnabled(rGrids->isChecked()); };
		QObject::connect(rGrids, &QRadioButton::toggled, this, [syncFields](bool) { syncFields(); });
		syncFields();

		// --- RUN / Save files & RUN -------------------------------------------------------------
		// Plain QPushButtons (not a QDialogButtonBox — there's no Cancel to pair RUN with anymore, and
		// a bare button box gets platform accept-role chrome a sibling QPushButton doesn't, so RUN and
		// "Save files & RUN" would look visually unequal). Neither gets autoDefault/default: RUN
		// launches a real simulation, so pressing Enter in ANY field (Name, dt, Manning, …) must NEVER
		// trigger it — only an explicit click. This dialog is CLOSED ONLY BY THE USER (title-bar X /
		// WA_DeleteOnClose) — no RUN variant ever calls accept()/close(), success or failure alike, so
		// the user can watch progress and fire further runs without reopening it.
		auto *runBtn = new QPushButton("RUN", this);
		runBtn->setAutoDefault(false); runBtn->setDefault(false);
		QObject::connect(runBtn, &QPushButton::clicked, this, [this]() {
			params = collectParams();
			// Synchronous pre-flight (Julia _nswing_check, nswing.jl): a blocking problem (no Source,
			// blank nested layer, …) pops a QMessageBox instead of launching a doomed run silently.
			QString out;
			if (!juliaEvalCall(QString("InteractiveGMT._nswing_check(Ptr{Cvoid}(UInt(%1)),raw\"%2\")")
			                       .arg((qulonglong)reinterpret_cast<uintptr_t>(scene_)).arg(params), out)) {
				QMessageBox::warning(this, "NSWING", out);
				return;
			}
			if (!params.isEmpty() && g_juliaNswing) g_juliaNswing(scene_, params.toUtf8().constData());
			else if (scene_ && scene_->win) scene_->win->statusBar()->showMessage("NSWING: callback not registered", 3000);
		});

		// "Save files & RUN" (lower-left): write every grid this run needs (bathymetry, Source, each
		// nested layer) to disk, then either just show the equivalent `gmt nswing …` command line, or
		// launch it for real as a DETACHED OS PROCESS (nswing.jl _nswing_run_external) — unlike the
		// default RUN button's in-memory worker path, this one's own stdout (-v) feeds the SAME progress
		// bar via the existing _nswing_watch log-tailer.
		auto *saveRunBtn = new QPushButton("Save files && RUN", this);
		saveRunBtn->setAutoDefault(false); saveRunBtn->setDefault(false);
		QObject::connect(saveRunBtn, &QPushButton::clicked, this, [this]() {
			QDialog sub(this);
			sub.setWindowTitle("Save files & RUN");
			auto *sv = new QVBoxLayout(&sub);

			// Save-to directory: remembered across calls (QSettings "nswing/saveDir"); the FIRST time
			// (nothing remembered yet) it defaults to the bathymetry (layer0) grid's own file directory
			// (_nswing_bathy_dir, nswing.jl). User can browse to a different dir; the choice sticks.
			auto *dirRow = new QHBoxLayout();
			dirRow->addWidget(new QLabel("Save to", &sub));
			auto *dirEdit = new QLineEdit(&sub);
			QSettings dst = igmtSettings();
			QString remembered = dst.value("nswing/saveDir").toString();
			if (!remembered.isEmpty()) {
				dirEdit->setText(remembered);
			} else {
				QString bathyDir;
				juliaEvalCall(QString("InteractiveGMT._nswing_bathy_dir(Ptr{Cvoid}(UInt(%1)))")
				                  .arg((qulonglong)reinterpret_cast<uintptr_t>(scene_)), bathyDir);
				dirEdit->setText(bathyDir);
			}
			dirRow->addWidget(dirEdit);
			auto *dirBtn = new QToolButton(&sub); dirBtn->setText("...");
			QObject::connect(dirBtn, &QToolButton::clicked, &sub, [&sub, dirEdit]() {
				QString d = QFileDialog::getExistingDirectory(&sub, "Save files to", dirEdit->text());
				if (!d.isEmpty()) dirEdit->setText(d);
			});
			dirRow->addWidget(dirBtn);
			sv->addLayout(dirRow);

			auto *bShow = new QPushButton("Save files and show GMT command", &sub);
			auto *bRun  = new QPushButton("Save files and RUN", &sub);
			sv->addWidget(bShow);
			sv->addWidget(bRun);

			// Shared by both sub-buttons: build params (+ the chosen save dir), confirm overwrite for
			// any file that already exists, persist the dir choice, then call `juliaFn`. `ok` reports
			// whether the call actually went through (false = aborted or failed, a popup already shown);
			// the return value is `juliaFn`'s printed output either way.
			auto proceed = [this, dirEdit](const QString &juliaFn, bool &ok) -> QString {
				ok = false;
				const QString p = collectParams() + "\nsavedir=" + dirEdit->text().trimmed();
				QString existing;
				if (!juliaEvalCall(QString("InteractiveGMT._nswing_existing_files(Ptr{Cvoid}(UInt(%1)),raw\"%2\")")
				                       .arg((qulonglong)reinterpret_cast<uintptr_t>(scene_)).arg(p), existing)) {
					QMessageBox::warning(this, "NSWING", existing);
					return QString();
				}
				if (!existing.trimmed().isEmpty()) {
					// WHY they're flagged as different (grdinfo -C, disk vs memory, field by field) —
					// shown BEFORE the Yes/No so an overwrite can be checked, not taken on faith.
					QString report;
					juliaEvalCall(QString("InteractiveGMT._nswing_existing_files_report(Ptr{Cvoid}(UInt(%1)),raw\"%2\")")
					                  .arg((qulonglong)reinterpret_cast<uintptr_t>(scene_)).arg(p), report);
					if (!report.trimmed().isEmpty())
						showInfoText(this, "NSWING — why these differ", report.trimmed());
					auto ans = QMessageBox::question(this, "NSWING",
						"These files already exist and will be overwritten:\n\n" + existing,
						QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
					if (ans != QMessageBox::Yes) return QString();
				}
				QSettings st = igmtSettings();
				st.setValue("nswing/saveDir", dirEdit->text().trimmed());
				QString out;
				if (!juliaEvalCall(QString("InteractiveGMT.%1(Ptr{Cvoid}(UInt(%2)),raw\"%3\")")
				                       .arg(juliaFn).arg((qulonglong)reinterpret_cast<uintptr_t>(scene_)).arg(p), out)) {
					QMessageBox::warning(this, "NSWING", out);
					return QString();
				}
				ok = true;
				return out;
			};

			QObject::connect(bShow, &QPushButton::clicked, &sub, [this, &sub, proceed]() {
				bool ok = false;
				QString out = proceed("_nswing_show_cli", ok);
				if (!ok) return;
				sub.accept();
				showInfoText(this, "NSWING — GMT command", out.trimmed());   // dialog stays open: just a preview
			});
			QObject::connect(bRun, &QPushButton::clicked, &sub, [this, &sub, proceed]() {
				bool ok = false;
				proceed("_on_nswing_save_run", ok);
				if (!ok) return;
				sub.accept();   // closes the small picker only; the NSWING dialog itself stays open
			});
			sub.exec();
		});

		// The stock Qt/Windows push button renders nearly borderless on this dialog's palette, so RUN
		// and "Save files & RUN" read as flat text, not buttons. Give both a raised edge, padding and
		// hover/press feedback; RUN gets a green accent (it launches a real simulation) so it's the
		// obvious primary action, Save stays neutral grey.
		runBtn->setMinimumHeight(30);
		runBtn->setStyleSheet(
			"QPushButton { padding: 6px 22px; font-weight: bold; border: 1px solid #1f7a33;"
			" border-radius: 5px; color: white;"
			" background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #4caf50, stop:1 #388e3c); }"
			"QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #5cbf60, stop:1 #43a047); }"
			"QPushButton:pressed { background: #2e7d32; }");
		saveRunBtn->setMinimumHeight(30);
		saveRunBtn->setStyleSheet(
			"QPushButton { padding: 6px 18px; font-weight: bold; border: 1px solid #7a7a7a;"
			" border-radius: 5px;"
			" background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #fbfbfb, stop:1 #dcdcdc); }"
			"QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #ffffff, stop:1 #e8e8e8); }"
			"QPushButton:pressed { background: #cfcfcf; }");

		auto *btnRow = new QHBoxLayout();
		btnRow->addWidget(saveRunBtn);
		btnRow->addStretch();
		btnRow->addWidget(runBtn);
		v->addLayout(btnRow);

		// Restore this window's last-typed fields (scene_->nswingParams, saved below on close) — applied
		// LAST so it overrides every default/scene-derived seed above, verbatim, including Source/Nest/
		// level/bc. QDialog's default closeEvent calls reject() for the title-bar X regardless of
		// show()-vs-exec(), so `rejected` alone catches every close (RUN never calls accept()/close()
		// itself, per this dialog's one-instance-per-window design, comment above).
		if (scene_ && !scene_->nswingParams.isEmpty()) applyParams(scene_->nswingParams);
		nestReady_ = true;                    // seeding done — from here, a nest file load runs the check
		QObject::connect(this, &QDialog::rejected, this, [this]{
			if (scene_) scene_->nswingParams = collectParams();
		});
	}

	// Validate the currently-entered Nest grid the INSTANT its file name is loaded/typed (called from the
	// nestEdit textChanged handler). Only a real, existing file is checked — a partial path being typed, a
	// scene grid NAME, or an empty box is ignored. The daughter is checked against its parent (the window
	// bathymetry for level 1, the previous level otherwise) by nswing's own nesting rule; if it violates,
	// the exact "does not obey to the nesting rules … X_MIN should be …" message pops immediately, so the
	// user learns of it here and not only at RUN. (_nswing_check_nest_file, nswing.jl.)
	void checkNestFile() {
		if (!nestReady_) return;
		const int lvl = levelCombo->currentIndex();
		if (lvl < 1) return;
		const QString path = nestEdit->text().trimmed();
		if (path.isEmpty() || !QFileInfo(path).isFile()) { lastNestChecked_.clear(); return; }
		if (path == lastNestChecked_) return;             // already checked this exact file
		lastNestChecked_ = path;
		const QString parentRef = (lvl >= 2 && nestNames.count(lvl - 1)) ? nestNames[lvl - 1] : QString();
		QString out;
		const QString expr = QString("InteractiveGMT._nswing_check_nest_file(Ptr{Cvoid}(UInt(%1)),%2,raw\"%3\",raw\"%4\")")
			.arg((qulonglong)reinterpret_cast<uintptr_t>(scene_)).arg(lvl).arg(path).arg(parentRef);
		if (!juliaEvalCall(expr, out)) { QMessageBox::warning(this, "NSWING", out); return; }
		if (!out.trimmed().isEmpty())
			QMessageBox::warning(this, "NSWING — nesting rules", out.trimmed());
	}

	// Double-click on a fileRow edit box opens the same "..." picker (fileBrowsers, set up in fileRow).
	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (ev->type() == QEvent::MouseButtonDblClick) {
			auto it = fileBrowsers.find(qobject_cast<QLineEdit*>(obj));
			if (it != fileBrowsers.end()) { it->second(); return true; }
		}
		return QDialog::eventFilter(obj, ev);
	}

	// Run a Julia expression synchronously via the console-eval bridge (g_juliaEval), with `scene_`
	// as the acting window. Fills `out` with printed stdout and returns true on success; on failure
	// (an exception in the evaluated code, or the bridge not registered yet) fills `out` with the
	// error text and returns false. Shared by every synchronous Julia round-trip this dialog makes
	// (pre-flight check, show-command, save+run launch) — one eval helper, not one per caller.
	bool juliaEvalCall(const QString &juliaCall, QString &out) {
		if (!g_juliaEval) { out = "Julia eval bridge not registered"; return false; }
		std::vector<char> buf(1 << 14);
		int n = g_juliaEval(scene_, juliaCall.toStdString().c_str(), buf.data(), (int)buf.size());
		out = QString::fromUtf8(buf.data(), n < 0 ? -n : n);
		return n >= 0;
	}

	// Serialize every dialog field into the "key=value\n…" block _on_nswing/_nswing_check/
	// _nswing_show_cli/_on_nswing_save_run all parse the same way (nswing.jl _nswing_parse). Shared by
	// the main RUN button and both "Save files & RUN" sub-options — one field list, not three.
	QString collectParams() {
		const QString mode = rGrids->isChecked() ? "grids" : (rAnuga->isChecked() ? "anuga" : "most");
		const QString field = rTotal->isChecked() ? "total" : "surface";
		QStringList L;
		auto kv = [&L](const char *k, const QString &val) { L << (QString(k) + "=" + val); };
		kv("source",   srcEdit->text().trimmed());
		kv("nest",     nestEdit->text().trimmed());
		kv("level",    QString::number(levelCombo->currentIndex()));
		// The FULL nest chain (nestNames, every level the user has visited/typed into, not just the one
		// currently showing in the box above) — a run needs every level, not just the last-selected one.
		for (auto &pr : nestNames)
			if (!pr.second.isEmpty()) kv(("nestL" + std::to_string(pr.first)).c_str(), pr.second);
		kv("bc",       bcPath);
		kv("outmode",  mode);
		kv("name",     nameEdit->text().trimmed());
		kv("field",    field);
		kv("max",      cMax->isChecked()   ? "1" : "0");
		kv("velocity", cVel->isChecked()   ? "1" : "0");
		kv("momentum", cMom->isChecked()   ? "1" : "0");
		kv("coriolis", cCoriolis->isChecked() ? "1" : "0");
		kv("manning",  manningEdit->text().trimmed());
		kv("maregs",   cMareg->isChecked() ? "1" : "0");
		kv("maregin",  maregInEdit->text().trimmed());
		kv("maregout", maregOutEdit->text().trimmed());
		kv("cumint",   cumintEdit->text().trimmed());
		kv("ncycles",  cyclesEdit->text().trimmed());
		kv("jump",     jumpEdit->text().trimmed());
		kv("dt",       dtEdit->text().trimmed());
		kv("grn",      grnEdit->text().trimmed());
		kv("geog",     cGeog->isChecked()  ? "1" : "0");
		return L.join("\n");
	}

	// Inverse of collectParams(): push a remembered "key=value\n…" block back into every widget.
	void applyParams(const QString &s) {
		std::map<QString, QString> m;
		for (const QString &line : s.split('\n', Qt::SkipEmptyParts)) {
			int eq = line.indexOf('=');
			if (eq >= 0) m[line.left(eq)] = line.mid(eq + 1);
		}
		auto get = [&](const char *k) { auto it = m.find(k); return it == m.end() ? QString() : it->second; };
		srcEdit->setText(get("source"));
		// Full nest chain FIRST (every "nestL<n>" key, not just the level that happened to be showing
		// when this was saved). MERGED into whatever populateFromScene() already seeded from live scene
		// grids, not cleared first — a remembered typed/browsed name overrides a scene one for the SAME
		// level, but a level populateFromScene found that this save never touched still survives.
		for (auto &[k, v] : m) {
			if (!k.startsWith("nestL") || v.isEmpty()) continue;
			bool ok = false;
			int lvl = k.mid(5).toInt(&ok);
			if (ok) nestNames[lvl] = v;
		}
		refreshLevelCombo();
		int lvl = get("level").toInt();
		if (lvl >= 0 && lvl < levelCombo->count()) levelCombo->setCurrentIndex(lvl);
		showLevel(levelCombo->currentIndex());   // explicit: setCurrentIndex above only re-fires showLevel on an actual index CHANGE
		bcPath = get("bc");
		if (!bcPath.isEmpty()) btnBorder->setText("Bordering: " + QFileInfo(bcPath).fileName());
		nameEdit->setText(get("name"));
		manningEdit->setText(get("manning"));
		maregInEdit->setText(get("maregin"));
		maregOutEdit->setText(get("maregout"));
		cumintEdit->setText(get("cumint"));
		cyclesEdit->setText(get("ncycles"));
		jumpEdit->setText(get("jump"));
		dtEdit->setText(get("dt"));
		grnEdit->setText(get("grn"));
		const QString mode = get("outmode");
		(mode == "anuga" ? rAnuga : mode == "most" ? rMost : rGrids)->setChecked(true);
		(get("field") == "total" ? rTotal : rSurf)->setChecked(true);
		cMax->setChecked(get("max") == "1");
		cVel->setChecked(get("velocity") == "1");
		cMom->setChecked(get("momentum") == "1");
		cCoriolis->setChecked(get("coriolis") == "1");
		cMareg->setChecked(get("maregs") == "1");
		cGeog->setChecked(get("geog") == "1");
	}

	// Seed the Input-grids widgets from the window's live grids (Scene Objects). Source <- the first grid
	// named "Okada z…"; the nesting chain <- every "layerN" (in N order), each shown in the listbox
	// as "name · W/E/S/N · nx×ny". Nest edit gets the first nested grid's name. Grids are in-memory scene
	// objects (names, not file paths) — this is a convenience default; the user can still browse to files.
	void populateFromScene() {
		if (!scene_) return;
		// Source: base surface + any extra grid whose name starts with "Okada z".
		auto isOkada = [](const std::string &n) { return QString::fromStdString(n).startsWith("Okada z", Qt::CaseInsensitive); };
		if (isOkada(scene_->surfName)) srcEdit->setText(QString::fromStdString(scene_->surfName));
		else {
			for (auto &ex : scene_->extras) {
				if (!ex.isImage && isOkada(ex.name)) { srcEdit->setText(QString::fromStdString(ex.name)); break; }
			}
		}
		// Nesting chain: "layerN" grids, ordered by N. Collect (N, &ex) then sort so 1,2,3… line up.
		// A freshly-made "layerN" is a literal all-zero placeholder (nestCreateBlankGrid, 55_lineprops.cpp)
		// until "Transplant 2nd grid…" fills it with real bathymetry — skip those here so the dialog never
		// offers a still-blank layer as a ready nesting level (nswing would silently run over zero bathymetry
		// there; the real guard is _on_nswing's blank check, but the dialog shouldn't seed one as "ready").
		QRegularExpression re("^layer(\\d+)$");
		std::vector<std::pair<int, const ExtraObj *>> nests;
		for (auto &ex : scene_->extras) {
			if (ex.isImage) continue;
			auto m = re.match(QString::fromStdString(ex.name));
			if (!m.hasMatch()) continue;
			if (std::all_of(ex.gridZ.begin(), ex.gridZ.end(), [](float v) { return v == 0.0f; })) continue;
			nests.emplace_back(m.captured(1).toInt(), &ex);
		}
		std::sort(nests.begin(), nests.end(), [](auto &a, auto &b) { return a.first < b.first; });
		for (auto &pr : nests) {
			int level = pr.first;
			if (level < 1 || level >= levelCombo->count()) continue;   // combo only holds levels 0..5
			nestNames[level] = QString::fromStdString(pr.second->name);
		}
		refreshLevelCombo();
	}

	// Highest filled level + 1 — the single level the Nest box currently accepts NEW input for. Shared
	// by refreshLevelCombo (which level to leave enabled) and the constructor (which level to start
	// on), so the two can never disagree about what "open" means.
	int nextOpenLevel() const {
		int maxFilled = 0;
		for (auto &kv : nestNames) if (!kv.second.isEmpty() && kv.first > maxFilled) maxFilled = kv.first;
		return maxFilled + 1;
	}

	// Relabel every levelCombo item from `nestNames` ("N -- level ready to use" once filled, else the
	// bare number) and enable only: level 0 (always — the fixed "in memory" slot), every already-filled
	// level (so the user can reselect one to see/re-pick its grid), and the SINGLE next unfilled level
	// (nesting must be built sequentially — level 3 can't be picked before level 2 exists). Every level
	// beyond that stays disabled/grayed in the dropdown.
	void refreshLevelCombo() {
		const int nextOpen = nextOpenLevel();
		auto *model = qobject_cast<QStandardItemModel *>(levelCombo->model());
		for (int i = 0; i < levelCombo->count(); ++i) {
			const bool filled = i > 0 && nestNames.count(i) && !nestNames[i].isEmpty();
			levelCombo->setItemText(i, i == 0 ? "0 -- level ready to use"
			                              : filled ? QString("%1 -- level ready to use").arg(i)
			                                       : QString::number(i));
			if (model) model->item(i)->setEnabled(i == 0 || filled || i == nextOpen);
		}
	}

	// Reflect the selected level in the Nest box: level 0 needs no file ("In memory grid" placeholder,
	// box disabled); an already-filled level shows its grid name; the (only ever ONE) open level shows
	// an "Enter grid name for level N" placeholder, ready for the user to browse/type into.
	void showLevel(int idx) {
		if (idx <= 0) {
			nestEdit->setEnabled(false);
			nestEdit->clear();
			nestEdit->setPlaceholderText("In memory grid");
			return;
		}
		nestEdit->setEnabled(true);
		auto it = nestNames.find(idx);
		if (it != nestNames.end() && !it->second.isEmpty()) {
			nestEdit->setPlaceholderText(QString());
			nestEdit->setText(it->second);
		} else {
			nestEdit->clear();
			nestEdit->setPlaceholderText(QString("Enter grid name for level %1").arg(idx));
		}
	}

protected:
	// Enter/Return anywhere in this dialog (any QLineEdit, e.g. Name) must NEVER fire anything except
	// an explicit RUN click — same law as every other action-button dialog here. RUN has
	// autoDefault/default set false above, but QDialog's own built-in Enter handling (search for
	// a "default" push button and click it) still ends up accepting the dialog regardless — confirmed
	// LIVE via the gmtvtk_nswing_enter_test hook (90_c_api.cpp): a synthetic Return in the Name field
	// accepted the dialog before this override existed. Fix: swallow Return/Enter unconditionally at
	// the dialog level instead of trusting QDialog's default-button search.
	void keyPressEvent(QKeyEvent *e) override {
		if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { e->accept(); return; }
		QDialog::keyPressEvent(e);
	}
};

// Plot seismicity — port of Mirone's earthquakes.m (src_figs/earthquakes.m). The layout is a
// FAITHFUL reproduction of deps/ui/plot_seismicity.ui: fixed 520x540 dialog, every widget at
// its .ui rect (setGeometry, NO Qt layouts — the .ui was arranged by hand and its geometry is
// the spec). Catalog source list (USGS web query / ISF / two plain-column layouts / Posit) +
// "..." file picker, a date range, and the magnitude / depth filter groups. "Use different
// sizes" maps the six magnitude intervals (<3, 3-5, 5-6, 6-7, 7-8, >=8) to symbol sizes; "Use
// different colors" maps the five depth intervals (0-33, 33-70, 70-150, 150-300, >300 km) to
// symbol colours; "All magnitudes"/"All depths" re-include NaN-valued events (only meaningful
// with the interval mode on, as in Mirone). OK assembles a newline-separated "key=value" block
// for Julia (g_juliaSeismicity), which reads, filters and stamps the events as screen-constant
// symbol layers. No Q_OBJECT (lambdas only).
class PlotSeismicityDialog : public QDialog {
public:
	QString params;                        // "key=value\n…" on OK, else empty
	QString filePath;                      // catalog file (empty for the USGS web query)
	bool builtin = false;                  // "Global seismicity (1990-2009)": the shipped quakes.dat
	QListWidget *fmtList;
	QLineEdit *syEdit, *smEdit, *sdEdit, *eyEdit, *emEdit, *edEdit;
	QLineEdit *magMinEdit, *magMaxEdit, *depMinEdit, *depMaxEdit;
	QCheckBox *cAllMags, *cMagSizes, *cAllDeps, *cDepColors;
	QComboBox *sizeCombo[6], *colorCombo[5];

	explicit PlotSeismicityDialog(QWidget *parent, bool builtin_ = false)
		: QDialog(parent), builtin(builtin_) {
		setWindowTitle("Plot seismicity");
		setFixedSize(520, 540);                                        // .ui: 520 x 540

		auto label = [](QWidget *parent, const char *txt, int x, int y, int w, int h) {
			auto *l = new QLabel(txt, parent); l->setGeometry(x, y, w, h); return l;
		};
		auto edit = [](QWidget *parent, int x, int y, int w, int h, const char *text = "") {
			auto *e = new QLineEdit(text, parent); e->setGeometry(x, y, w, h); return e;
		};

		// --- Catalog source + "..." file picker (the USGS web query needs no file) ---------------
		fmtList = new QListWidget(this);
		fmtList->setGeometry(16, 12, 463, 71);
		fmtList->addItems({"USGS Current seismicity", "ISF formated catalog (ascii)",
		                   "lon,lat,mag,dep,yy,mm,dd,hh,mm,ss", "lon,lat,dep,mag,yy,mm,dd", "Posit file"});
		fmtList->setCurrentRow(0);
		auto *btnFile = new QToolButton(this);
		btnFile->setGeometry(480, 31, 32, 32);
		btnFile->setText("...");
		btnFile->setToolTip("Select the catalog file (not needed for the USGS web query)");
		QObject::connect(btnFile, &QToolButton::clicked, this, [this]() {
			static const char *filters[5] = {
				"All files (*)",
				"ISF catalogs (*.isf *.ISF);;All files (*)",
				"Data files (*.dat *.DAT);;All files (*)",
				"Data files (*.dat *.DAT);;All files (*)",
				"Posit files (*.posit *.POSIT);;All files (*)" };
			const int row = qBound(0, fmtList->currentRow(), 4);
			QString p = QFileDialog::getOpenFileName(this, "Select earthquakes file", prefStartDir(), filters[row]);
			if (p.isEmpty()) return;
			filePath = p; rememberStartDir(p);
			setWindowTitle("Plot seismicity — " + QFileInfo(p).fileName());
		});
		QObject::connect(fmtList, &QListWidget::currentRowChanged, this,
		                 [btnFile](int row) { btnFile->setEnabled(row != 0); });
		btnFile->setEnabled(false);                            // row 0 (USGS) is the initial pick

		// --- Date range ---------------------------------------------------------------------------
		label(this, "Start\nyear",  16,  104, 31, 28);  syEdit = edit(this,  60, 104, 90, 28);
		label(this, "Start\nmonth", 176, 104, 41, 28);  smEdit = edit(this, 230, 104, 90, 28);
		label(this, "Start\nday",   347, 104, 41, 28);  sdEdit = edit(this, 390, 104, 90, 28);
		label(this, "End\nyear",    16,  140, 31, 28);  eyEdit = edit(this,  60, 140, 90, 28);
		label(this, "End\nmonth",   176, 140, 41, 28);  emEdit = edit(this, 230, 140, 90, 28);
		label(this, "End\nday",     347, 140, 41, 28);  edEdit = edit(this, 390, 140, 90, 28);
		for (auto *e : { syEdit, smEdit, sdEdit, eyEdit, emEdit, edEdit })
			e->setToolTip("empty = no bound");
		if (builtin) {
			// "Global seismicity (1990-2009)": the shipped data/quakes.dat — no catalog picking
			// (Mirone earthquakes.m nargin==1: listbox "Not useful here" + hidden file button) and
			// the date fields pre-filled to the dataset's span.
			fmtList->clear();
			fmtList->addItem("Not useful here");
			fmtList->setEnabled(false);
			btnFile->hide();
			syEdit->setText("1990"); smEdit->setText("1");  sdEdit->setText("1");
			eyEdit->setText("2009"); emEdit->setText("12"); edEdit->setText("31");
		}

		// --- Magnitude group: min/max filter + per-interval sizes (untitled frame, as in the .ui) --
		auto *gMag = new QGroupBox(this);
		gMag->setGeometry(16, 190, 501, 148);
		label(gMag, "Minimum\nmagnitude", 12,  15, 65, 28);  magMinEdit = edit(gMag,  83, 15, 86, 28);
		label(gMag, "Maximum\nmagnitude", 189, 15, 65, 28);  magMaxEdit = edit(gMag, 260, 15, 85, 28);
		cAllMags = new QCheckBox("All magnitudes", gMag);
		cAllMags->setGeometry(360, 19, 114, 19);
		cAllMags->setEnabled(false);
		cAllMags->setToolTip("Also plot events with an unknown (NaN) magnitude");
		cMagSizes = new QCheckBox("Use different sizes for magnitude intervals", gMag);
		cMagSizes->setGeometry(12, 59, 477, 19);
		static const int   magX[6]   = { 12, 94, 176, 257, 339, 421 };
		static const char *magLab[6] = { "0-3", "3-5", "5-6", "6-7", "7-8", "> 8" };
		static const char *magDef[6] = { "4", "6", "8", "10", "12", "15" };
		for (int k = 0; k < 6; ++k) {
			label(gMag, magLab[k], magX[k], 94, 68, 14);
			auto *cb = new QComboBox(gMag);
			cb->setGeometry(magX[k], 114, 65, 22);
			cb->setEditable(true);
			cb->addItem(magDef[k]);                            // one default item, per the .ui
			cb->setEnabled(false);
			sizeCombo[k] = cb;
		}
		QObject::connect(cMagSizes, &QCheckBox::toggled, this, [this](bool on) {
			for (auto *cb : sizeCombo) cb->setEnabled(on);
			cAllMags->setEnabled(on);
			if (!on) cAllMags->setChecked(false);
		});

		// --- Depth group: min/max filter + per-interval colours -----------------------------------
		auto *gDep = new QGroupBox(this);
		gDep->setGeometry(16, 348, 501, 148);
		label(gDep, "Minimum\ndepth", 12,  15, 56, 28);  depMinEdit = edit(gDep,  74, 15, 90, 28, "0");
		label(gDep, "Maximum\ndepth", 194, 15, 60, 28);  depMaxEdit = edit(gDep, 260, 15, 85, 28);
		cAllDeps = new QCheckBox("All depths", gDep);
		cAllDeps->setGeometry(360, 19, 84, 19);
		cAllDeps->setEnabled(false);
		cAllDeps->setToolTip("Also plot events with an unknown (NaN) depth");
		cDepColors = new QCheckBox("Use different colors for depth intervals", gDep);
		cDepColors->setGeometry(12, 59, 477, 19);
		static const int   depX[5]   = { 12, 110, 208, 307, 405 };
		static const int   depW[5]   = { 84, 84, 85, 84, 84 };
		static const char *depLab[5] = { "0-33 km", "33-70 km", "70-150 km", "150-300 km", "> 300 km" };
		static const char *depDef[5] = { "red", "green", "blue", "cyan", "yellow" };
		for (int k = 0; k < 5; ++k) {
			label(gDep, depLab[k], depX[k], 94, depW[k], 14);
			auto *cb = new QComboBox(gDep);
			cb->setGeometry(depX[k], 114, 80, 22);
			cb->setEditable(true);
			cb->addItem(depDef[k]);                            // one default item, per the .ui
			cb->setEnabled(false);
			colorCombo[k] = cb;
		}
		QObject::connect(cDepColors, &QCheckBox::toggled, this, [this](bool on) {
			for (auto *cb : colorCombo) cb->setEnabled(on);
			cAllDeps->setEnabled(on);
			if (!on) cAllDeps->setChecked(false);
		});

		// --- OK (the only action button, per the .ui; Esc rejects) --------------------------------
		auto *ok = new QPushButton("OK", this);
		ok->setGeometry(410, 500, 100, 32);
		ok->setDefault(true);
		QObject::connect(ok, &QPushButton::clicked, this, [this]() {
			const int fmt = builtin ? 6 : qBound(0, fmtList->currentRow(), 4) + 1;
			if (fmt >= 2 && fmt <= 5 && filePath.isEmpty()) {
				QMessageBox::warning(this, "Plot seismicity",
				                     "This catalog format needs a file — pick one with the \"...\" button.");
				return;
			}
			QStringList L;
			auto kv = [&L](const QString &k, const QString &val) { L << k + "=" + val; };
			kv("format",    QString::number(fmt));
			kv("file",      filePath);
			kv("syear",     syEdit->text().trimmed());
			kv("smonth",    smEdit->text().trimmed());
			kv("sday",      sdEdit->text().trimmed());
			kv("eyear",     eyEdit->text().trimmed());
			kv("emonth",    emEdit->text().trimmed());
			kv("eday",      edEdit->text().trimmed());
			kv("magmin",    magMinEdit->text().trimmed());
			kv("magmax",    magMaxEdit->text().trimmed());
			kv("allmags",   cAllMags->isChecked()   ? "1" : "0");
			kv("magsizes",  cMagSizes->isChecked()  ? "1" : "0");
			for (int k = 0; k < 6; ++k) kv(QString("s%1").arg(k + 1), sizeCombo[k]->currentText().trimmed());
			kv("depmin",    depMinEdit->text().trimmed());
			kv("depmax",    depMaxEdit->text().trimmed());
			kv("alldeps",   cAllDeps->isChecked()   ? "1" : "0");
			kv("depcolors", cDepColors->isChecked() ? "1" : "0");
			for (int k = 0; k < 5; ++k) kv(QString("c%1").arg(k + 1), colorCombo[k]->currentText().trimmed());
			params = L.join("\n");
			accept();
		});
	}
};

// Focal mechanisms — loads deps/ui/focal_mechanisms.ui at RUNTIME via QUiLoader instead of
// hand-porting it into C++ widget calls. Every other .ui in deps/ui/ is a spec that gets
// hand-ported (see the "Do-Not-Repeat .ui geometry is LAW" note above) — that split caused
// repeated user frustration when a `.ui` edit in Qt Creator didn't show up in the app because
// the hand-port wasn't manually resynced. For this dialog the .ui IS the running dialog: edit
// it in Qt Creator, relaunch, done — no C++ resync, ever. Behavior is wired generically by
// objectName via findChild, so most .ui edits (reflow, spacing, new default) need no C++ change
// at all; only adding/renaming/removing a NAMED field that OK reads would. OK packs every field
// into `params` ("key=value\n…", the Geophysics menu appends "region=W/E/S/N"); Julia (g_juliaFocal,
// src/focal.jl) does the catalog read + beachball plotting.
class FocalMechanismsDialog {
public:
	QDialog *dlg = nullptr;
	QString params;                        // "key=value\n…" on OK, else empty
	QString filePath;                      // catalog file

	explicit FocalMechanismsDialog(QWidget *parent, Scene *scene = nullptr) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/focal_mechanisms.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("FocalMechanismsDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("FocalMechanismsDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		QDialog *d = dlg;                      // local copy — member `dlg` can't be lambda-captured

		auto *catalogList = d->findChild<QListWidget *>("catalogFormatList");
		// Live prefill of Min/Max magnitude and Min/Max depth from the CHOSEN FILE's own values
		// (read AFTER the catalog is picked, not the window's possibly placeholder/unrelated
		// visible region), plus a reset of "Magnitude 5 size" back to Mirone's fixed 0.8 cm
		// reference (not data-derived — see _focal_peek_and_frame). Best-effort: g_juliaEval
		// round-trips a plain Julia call (_focal_peek_and_frame, src/focal.jl) that prints
		// "mag5/minmag/maxmag/mindepth/maxdepth" on success, nothing on any read failure (wrong
		// format for this file yet, bad path) — the fields are simply left alone in that case.
		auto updateFieldsFromData = [this, d, catalogList, scene]() {
			if (filePath.isEmpty() || !g_juliaEval) return;
			const int fmt = (catalogList ? catalogList->currentRow() : 0) + 1;
			// Pass the window's own Scene* as a raw Julia pointer literal (not through the `fig`
			// binding g_juliaEval offers) — _focal_peek_and_frame needs the handle itself to call
			// gmtvtk_has_surface / _on_basemap directly, and works whether or not this scene has a
			// registered QtFigure.
			char ptrbuf[32];
			snprintf(ptrbuf, sizeof ptrbuf, "Ptr{Cvoid}(0x%llx)", (unsigned long long)(uintptr_t)scene);
			const std::string cmd = "InteractiveGMT._focal_peek_and_frame(" + std::string(ptrbuf) +
				", raw\"" + filePath.toStdString() + "\", " + std::to_string(fmt) + ")";
			std::vector<char> buf(256);
			int n = g_juliaEval(scene, cmd.c_str(), buf.data(), (int)buf.size());
			if (n <= 0) return;
			const QStringList p = QString::fromUtf8(buf.data(), n).trimmed().split('/');
			if (p.size() < 5) return;
			auto setIfValid = [d](const char *name, const QString &s, bool round) {
				bool ok = false;
				const double v = s.toDouble(&ok);
				if (!ok) return;
				auto *e = d->findChild<QLineEdit *>(name);
				if (!e) return;
				e->setText(round ? QString::number(std::max(1.0, std::round(v))) : QString::number(v));
			};
			setIfValid("editMag5Size",  p[0], false);
			setIfValid("editMinMag",    p[1], false);
			setIfValid("editMaxMag",    p[2], false);
			setIfValid("editMinDepth",  p[3], false);
			setIfValid("editMaxDepth",  p[4], false);
		};
		auto openFileDialog = [this, d, updateFieldsFromData]() {
			QString p = QFileDialog::getOpenFileName(d, "Select focal mechanisms file", prefStartDir());
			if (p.isEmpty()) return;
			filePath = p; rememberStartDir(p);
			d->setWindowTitle("Focal mechanisms — " + QFileInfo(p).fileName());
			updateFieldsFromData();
		};
		if (auto *btnOpenFile = d->findChild<QToolButton *>("btnOpenFile"))
			QObject::connect(btnOpenFile, &QToolButton::clicked, d, openFileDialog);
		// Double-click a catalog format in the list = same as Browse: pick the file for that format.
		if (catalogList)
			QObject::connect(catalogList, &QListWidget::itemDoubleClicked, d, [openFileDialog](QListWidgetItem *) { openFileDialog(); });

		// Plot event date is only meaningful for catalog formats that actually CARRY a date (ISF
		// row 0, CMT .ndk row 3) — Aki & Richards / plain Harvard CMT column files (rows 1,2) have
		// none, so the box is disabled + unchecked there (mirrors Mirone's push_readFile_CB, which
		// enables check_plotDate only after a successful ISF/.ndk read).
		if (auto *dateCheck = d->findChild<QCheckBox *>("chkPlotEventDate")) {
			QObject::connect(catalogList, &QListWidget::currentRowChanged, d, [dateCheck, updateFieldsFromData](int row) {
				const bool hasDate = (row == 0 || row == 3);
				dateCheck->setEnabled(hasDate);
				if (!hasDate) dateCheck->setChecked(false);
				updateFieldsFromData();          // format changed -> re-peek the SAME file under the new format
			});
		}

		static const char *comboNames[5] = { "cmbDepthColor0_33", "cmbDepthColor33_70",
			"cmbDepthColor70_150", "cmbDepthColor150_300", "cmbDepthColorGT300" };
		static const char *labelNames[5] = { "lblDepth0_33", "lblDepth33_70",
			"lblDepth70_150", "lblDepth150_300", "lblDepthGT300" };
		if (auto *depthColorsCheck = d->findChild<QCheckBox *>("chkDepthColors")) {
			QObject::connect(depthColorsCheck, &QCheckBox::toggled, d, [d](bool on) {
				for (const char *n : comboNames) if (auto *w = d->findChild<QWidget *>(n)) w->setEnabled(on);
				for (const char *n : labelNames) if (auto *w = d->findChild<QWidget *>(n)) w->setEnabled(on);
			});
		}

		if (auto *btnOK = d->findChild<QPushButton *>("btnOK")) {
			QObject::connect(btnOK, &QPushButton::clicked, d, [this, d, catalogList]() {
				if (filePath.isEmpty()) {
					QMessageBox::warning(d, "Focal mechanisms", "Select a catalogue file first.");
					return;
				}
				auto text = [d](const char *name) {
					auto *e = d->findChild<QLineEdit *>(name);
					return e ? e->text().trimmed() : QString();
				};
				auto checked = [d](const char *name) {
					auto *c = d->findChild<QCheckBox *>(name);
					return c && c->isChecked();
				};
				QStringList L;
				auto kv = [&L](const QString &k, const QString &val) { L << k + "=" + val; };
				kv("format",    QString::number((catalogList ? catalogList->currentRow() : 0) + 1));
				kv("file",      filePath);
				kv("magmin",    text("editMinMag"));
				kv("magmax",    text("editMaxMag"));
				kv("mag5size",  text("editMag5Size"));
				kv("depmin",    text("editMinDepth"));
				kv("depmax",    text("editMaxDepth"));
				kv("depcolors", checked("chkDepthColors") ? "1" : "0");
				kv("plotdate",  checked("chkPlotEventDate") ? "1" : "0");
				for (int k = 0; k < 5; ++k) {
					auto *cb = d->findChild<QComboBox *>(comboNames[k]);
					kv(QString("c%1").arg(k + 1), cb ? cb->currentText().trimmed() : QString());
				}
				params = L.join("\n");
				d->accept();
			});
		}
	}

	int exec() { return dlg ? dlg->exec() : QDialog::Rejected; }
};

// Fold / un-fold the Shading dock programmatically (Surface row click in the Scene Objects panel).
// Lives here because FoldTitleBar is complete only in this TU fragment; 50_scene.cpp forward-decls it.
static void toggleShadingFold(Scene *s) {
	if (s && s->shadeFoldBar && s->shadeFoldBar->onClick)
		s->shadeFoldBar->onClick();
}

// Reveal + UN-FOLD the Scene Objects dock (called when the first nested rectangle lands, so the user
// sees where the rectangle / its grids show up). Only un-folds if currently folded; no-op otherwise.
static void unfoldSceneObjects(Scene *s) {
	if (!s) return;
	if (s->objDock) { s->objDock->show(); s->objDock->raise(); }
	if (s->objFoldBar && s->objFoldBar->folded && s->objFoldBar->onClick)
		s->objFoldBar->onClick();
}

// Polygon draw/edit tool (defined in 85_polygon.cpp, #included after this file). The toolbar
// button toggles draw mode via polygonSetMode; the mouse gestures are driven from GLView.
static void polygonSetMode(Scene *s, bool on);
static void polygonToolToggled(Scene *s, QAction *act, Scene::ShapeKind shape, bool on);
static QIcon makePolygonIcon();
static QIcon makePolylineIcon();
static QIcon makeLineIcon();
static QIcon makeRectIcon();
static QIcon makeNestedRectIcon();
static QIcon makeCircleIcon();
static QIcon makeSymCircleIcon();   // Symbols flyout glyphs (85_polygon.cpp)
static QIcon makeSymSquareIcon();
static QIcon makeSymStarIcon();
static QIcon makeTextIcon();
static QIcon makeCubeIcon();        // 3-D Bodies flyout glyphs (85_polygon.cpp)
static QIcon makeSphereIcon();
static QIcon makeTorusIcon();
static QIcon makeCylinderIcon();
static QIcon makePolyhedronIcon();
static QIcon makeViewModeIcon(bool twoD);   // "2D"/"3D" glyph for the icon-only view-toggle button
static QIcon makeInfoIcon();                // stylised 'i' glyph for the grdinfo/gdalinfo flyout
static int  polyHitText(Scene *s, int x, int y, double tol);   // text label under the cursor (85_polygon.cpp)


// ============================================================================
//  Recent files — a persistent (QSettings) MRU of the last kRecentMax opened
//  files, each tagged by category (0=grid, 1=image, 2=dataset). Julia calls
//  gmtvtk_add_recent(path,cat) after every successful open; the File > Recent
//  Files submenu rebuilds from this list on aboutToShow (so all windows stay in
//  sync) and re-opens a pick via iview("path"). Shared process-wide.
// ============================================================================
struct RecentItem { QString path; int cat; };
static std::vector<RecentItem> g_recent;
static bool g_recentLoaded = false;
static const int kRecentMax = 21;

static void loadRecent() {
	if (g_recentLoaded) return;
	g_recentLoaded = true;
	QSettings st = igmtSettings();
	const QStringList paths = st.value("recent/paths").toStringList();
	const QVariantList cats  = st.value("recent/cats").toList();
	for (int i = 0; i < paths.size(); ++i)
		g_recent.push_back({ paths[i], (i < cats.size()) ? cats[i].toInt() : 2 });
}

static void saveRecent() {
	QStringList paths; QVariantList cats;
	for (const RecentItem& r : g_recent) { paths << r.path; cats << r.cat; }
	QSettings st = igmtSettings();
	st.setValue("recent/paths", paths);
	st.setValue("recent/cats", cats);
}

// Promote a freshly-opened file to the front of the MRU (de-dup, cap, persist).
static void addRecentFile(const char *cpath, int cat) {
	if (!cpath || !*cpath) return;
	loadRecent();
	const QString p = QString::fromUtf8(cpath);
	for (int i = (int)g_recent.size() - 1; i >= 0; --i)            // drop any prior entry for this path
		if (QString::compare(g_recent[i].path, p, Qt::CaseInsensitive) == 0)
			g_recent.erase(g_recent.begin() + i);
	g_recent.insert(g_recent.begin(), { p, (cat >= 0 && cat <= 2) ? cat : 2 });
	if ((int)g_recent.size() > kRecentMax) g_recent.resize(kRecentMax);
	saveRecent();
}

// Rebuild the Recent Files submenu, grouped Grids / Images / Datasets. Each entry shows the file
// name (full path on hover) and re-opens via the drop path (into THIS window); Clear wipes list.
static void populateRecentMenu(QMenu *menu, Scene *s) {
	loadRecent();
	menu->clear();
	static const char *kCatName[3] = { "Grids", "Images", "Datasets" };
	bool any = false;
	for (int c = 0; c < 3; ++c) {
		bool header = false;
		for (const RecentItem& r : g_recent) {
			if (r.cat != c) continue;
			if (!header) { QAction *h = menu->addAction(kCatName[c]); h->setEnabled(false); header = true; }
			const QString full = r.path;
			QAction *act = menu->addAction(QFileInfo(full).fileName());
			act->setToolTip(full); act->setStatusTip(full);
			QObject::connect(act, &QAction::triggered, [s, full]() {
				if (!g_juliaDrop) return;
				// Route through the drop path so the file opens INTO this window
				// (or promotes an empty launcher) instead of spawning a new window.
				g_juliaDrop(s, full.toStdString().c_str());
			});
			any = true;
		}
		if (header) menu->addSeparator();
	}
	if (!any) { QAction *none = menu->addAction("(no recent files)"); none->setEnabled(false); }
	else      { menu->addAction("&Clear Recent Files", []() { g_recent.clear(); saveRecent(); }); }
}

// ── Per-data scene content ──────────────────────────────────────────────────────────────────
// Builds EVERYTHING that depends on the data — LUT, surface (tiled or single actor), optional
// image drape, cube axes + titles/ticks, colorbar, the default 3-D view, the SSAO radius seed,
// the readout picker and the profile-track line — onto an ALREADY-constructed Scene `s` (its
// renderer, overlay renderer, lights and env map already exist). Called by buildAndShow for a
// fresh window AND by gmtvtk_promote_surface_h to turn an empty launcher into a real grid window
// IN THE SAME window. Because both go through here there is ONE build path and nothing to drift.
// Self-cleaning: every content actor it is about to (re)create is removed first, so it is
// idempotent — a fresh Scene has none of them yet and the removals are harmless no-ops.
//
// The CALLER must already have set on `s`: imageOnly, x0/x1/y0/y1, zmin/zmax, xfac/zfac/ve.
static void buildSceneContent(Scene *s, vtkSmartPointer<vtkPolyData> pd,
                              double x0, double x1, double y0, double y1,
                              const double *cz, const double *crgb, int ncolor,
                              const unsigned char *img, int iw, int ih, int ibands,
                              int edges, bool pointCloud, int geographic,
                              const float *gz, int gnx, int gny, bool blankStart) {
	// Drop any previous content first (promotion rebuilds into an existing scene; a fresh scene has
	// none of these so every removal is a no-op). RemoveActor on an actor not in the renderer is safe.
	if (s->lodCmd && s->ren->GetActiveCamera()) s->ren->GetActiveCamera()->RemoveObserver(s->lodCmd);
	s->lodCmd = nullptr; s->quadRoot = nullptr; s->tiles.clear();
	if (s->surfGroup) s->ren->RemoveActor(s->surfGroup);
	if (s->surf)      s->ren->RemoveActor(s->surf);
	if (s->drape)     s->ren->RemoveActor(s->drape);
	if (s->axes)      s->ren->RemoveActor(s->axes);
	if (s->axisTicks) s->ren->RemoveActor(s->axisTicks);
	if (s->profLine)  s->ren->RemoveActor(s->profLine);
	for (int i = 0; i < 2; ++i) if (s->axTitle[i]) { s->axesRen->RemoveViewProp(s->axTitle[i]); s->axTitle[i] = nullptr; }
	if (s->bar)      s->ren->RemoveActor2D(s->bar);
	if (s->barTicks) s->ren->RemoveActor2D(s->barTicks);
	for (auto& ta : s->barLabels) if (ta) s->ren->RemoveActor2D(ta);
	s->barLabels.clear(); s->barValues.clear();
	s->surfGroup = nullptr; s->drape = nullptr; s->bar = nullptr; s->barTicks = nullptr;

	// Colour map. A GMT CPT arrives as control nodes (cz[i] -> crgb[i]); a
	// vtkColorTransferFunction maps z to colour at those exact (possibly non-uniform,
	// data-equalized) positions. No CPT -> a plain blue->red ramp (demo).
	vtkSmartPointer<vtkScalarsToColors> lut;
	bool ctfRange = false;
	if (cz && crgb && ncolor > 0) {
		vtkNew<vtkColorTransferFunction> ctf;
		for (int i = 0; i < ncolor; ++i)
			ctf->AddRGBPoint(cz[i], crgb[3*i], crgb[3*i+1], crgb[3*i+2]);
		lut = ctf;
		ctfRange = true;        // the CTF maps absolute z; let the mapper defer to it
	}
	else {
		// Fallback ramp as a vtkColorTransferFunction (NOT a plain LUT) so s->surfLut is ALWAYS a
		// CTF -> the runtime colormap chooser (gmtvtk_set_cpt) can recolour by mutating its nodes,
		// which every mapper + the colorbar share by pointer. Blue->red, same look as the old LUT.
		vtkNew<vtkColorTransferFunction> t;
		t->SetColorSpaceToHSV();
		t->HSVWrapOff();
		t->AddHSVPoint(s->zmin, 0.667, 1.0, 1.0);   // blue (low)
		t->AddHSVPoint(s->zmax, 0.0,   1.0, 1.0);   // red  (high)
		lut = t;
		ctfRange = true;
	}
	s->surfLut = lut; s->surfCtfRange = ctfRange;   // shared by surface, LOD tiles AND the colorbar

	// ===== surface: tiled grid (gz) OR single actor (pd) =====================
	// Declared out here so the drape block below (single-actor path) can share them.
	bool hasNormals = false;
	vtkNew<vtkPolyDataNormals> norms;        // polydata-surface path only
	if (gz && gnx > 1 && gny > 1) {
		// Tiled-LOD plain grid: build the quadtree (indices only) + an empty assembly; tile actors
		// are created lazily by the screen-space-error refinement (refineQuadtree) and re-evaluated
		// on every camera move. No giant polydata; resident geometry is bounded by lodBudgetBytes.
		s->surfGroup = vtkSmartPointer<vtkAssembly>::New();
		s->surfLut = lut; s->surfCtfRange = ctfRange; s->surfEdges = edges;
		// Data layer MUST exist before refineQuadtree (tiles sample s->gridZ). Populate it here from
		// gz (the caller fills it only AFTER buildAndShow returns, which would be too late).
		s->gridZ.assign(gz, gz + (size_t)gnx * gny);
		s->gnx = gnx; s->gny = gny;
		s->gx0 = x0; s->gx1 = x1; s->gy0 = y0; s->gy1 = y1;
		s->gdx = (gnx > 1) ? (x1 - x0) / (gnx - 1) : 0.0;
		s->gdy = (gny > 1) ? (y1 - y0) / (gny - 1) : 0.0;
		s->quadRoot = buildQuadNode(0, gnx - 1, 0, gny - 1, 0, x0, s->gdx, y0, s->gdy);
		s->surf = vtkSmartPointer<vtkActor>::New();   // placeholder handle; real geometry = tiles
		s->surfGroup->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		s->ren->AddActor(s->surfGroup);
		refineQuadtree(s);                            // initial (coarse) population so axes/bounds work
		s->lodCmd = vtkSmartPointer<vtkCallbackCommand>::New();
		s->lodCmd->SetCallback(onLodCamera);
		s->lodCmd->SetClientData(s);
		s->ren->GetActiveCamera()->AddObserver(vtkCommand::ModifiedEvent, s->lodCmd);
	}
	else {
	// Smooth shading needs per-vertex normals. A grid polydata arrives with them already baked
	// in (makeGridFromArray) -> feed it straight to the mapper, NO vtkPolyDataNormals second
	// copy. An FV/other surface with none still gets them generated here. Point clouds = unlit.
	hasNormals = pd && pd->GetPointData() && pd->GetPointData()->GetNormals() != nullptr;
	if (!pointCloud && !hasNormals) {
		norms->SetInputData(pd);
		norms->SetFeatureAngle(90.0);
		norms->SplittingOff();
		norms->ConsistencyOn();
	}

	vtkNew<vtkPolyDataMapper> map;
	// Cloud or already-normalled grid -> direct; bare surface -> through the normals filter.
	if (pointCloud || hasNormals) map->SetInputData(pd);
	else                          map->SetInputConnection(norms->GetOutputPort());
	map->SetLookupTable(lut);
	map->SetScalarRange(s->zmin, s->zmax);
	if (ctfRange) map->UseLookupTableScalarRangeOn();  // colours keyed to the CPT's own z nodes
	map->ScalarVisibilityOn();
	map->InterpolateScalarsBeforeMappingOn();   // per-fragment colour -> crisp gradient

	// A point cloud uses a vtkLODActor: while the camera moves it draws a decimated subset
	// (NumberOfCloudPoints), full resolution when still -> interaction stays smooth on huge
	// clouds (a plain actor redraws all N every frame = sluggish). A surface stays a plain actor.
	if (pointCloud) {
		vtkSmartPointer<vtkLODActor> la = vtkSmartPointer<vtkLODActor>::New();
		la->SetNumberOfCloudPoints(50000);   // points drawn during interaction
		s->surf = la;
	}
	else {
		s->surf = vtkSmartPointer<vtkActor>::New();
	}
	s->surf->SetMapper(map);
	s->surf->GetProperty()->SetInterpolationToPBR();  // PBR + IBL = F3D-style shading
	s->surf->GetProperty()->SetMetallic(0.0);         // terrain is dielectric
	s->surf->GetProperty()->SetRoughness(0.45);       // soft sheen, not mirror/not flat
	s->surf->GetProperty()->SetEdgeColor(0.12, 0.12, 0.12);   // wire mesh colour; hidden until 'e'
	s->surf->GetProperty()->SetLineWidth(1.0);
	s->surf->GetProperty()->SetEdgeVisibility(edges);         // initial mesh state (default off)
	s->surf->SetScale(s->xfac, 1.0, s->zfac * s->ve);   // base aspect + unit + initial VE
	s->ren->AddActor(s->surf);
	}

	// --- optional image drape -----------------------------------------------
	// A caller-supplied RGB[A] image is textured over the surface (via the tcoords
	// baked into the grid) instead of the CPT colouring. img is row-major, row 0 =
	// south (VTK texture origin), iw*ih pixels of ibands each.
	bool draped = false;
	if (img && iw > 0 && ih > 0 && ibands > 0) {
		vtkNew<vtkImageData> tex_img;
		tex_img->SetDimensions(iw, ih, 1);
		tex_img->AllocateScalars(VTK_UNSIGNED_CHAR, ibands);
		memcpy(tex_img->GetScalarPointer(), img, (size_t)iw * ih * ibands);
		vtkNew<vtkTexture> tex;
		tex->SetInputData(tex_img);
		tex->InterpolateOn();

		// Image overlay. The canvas spans the WHOLE grid bbox with alpha 0 outside the
		// image footprint, so only the grid ∩ image area is painted; the CPT-coloured
		// base surface shows everywhere else (mirrors GMTF3D drape_to_bbox). A separate
		// actor shares the geometry + tcoords; its RGBA texture has alpha, so VTK runs it
		// in the translucent pass and blends the picture over the base. A polygon offset
		// pulls it toward the camera so it wins the depth tie with the base surface.
		vtkNew<vtkPolyDataMapper> dmap;
		if (hasNormals) dmap->SetInputData(pd);                    // grid: normals baked in
		else            dmap->SetInputConnection(norms->GetOutputPort());
		dmap->ScalarVisibilityOff();        // colour comes from the texture, not the CPT
		vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
		dmap->SetRelativeCoincidentTopologyPolygonOffsetParameters(-1.0, -1.0);
		s->drape = vtkSmartPointer<vtkActor>::New();
		s->drape->SetMapper(dmap);
		s->drape->SetTexture(tex);
		// The drape is a finished PICTURE — show its TRUE pixel colours, NOT re-shaded by the
		// scene. (1) NEVER PBR: VTK's PBR shader only samples SetBaseColorTexture, so a plain
		// SetTexture drape renders flat grey. (2) NOT Phong either: the key/fill sun lights then
		// shade the image (dark gradient = "super shitty"). LightingOff renders the texture flat
		// at full albedo, immune to every light. applyShading() leaves the drape untouched so
		// this sticks; the BASE surface keeps PBR.
		s->drape->GetProperty()->LightingOff();
		s->drape->SetScale(s->xfac, 1.0, s->zfac * s->ve);    // track the base actor's scale/VE
		// GMTF3D :shademesh — draw the grid's cell edges over the drape. The drape canvas is
		// opaque outside_color fill where the image does NOT reach (bridge _drape_to_bbox with
		// outside=:shade/:shademesh), so the uncovered area reads as a shaded MESH; the image
		// area keeps the picture with the same faint wires on top (matches the f3d `_edges` path).
		s->drape->GetProperty()->SetEdgeColor(0.12, 0.12, 0.12);
		s->drape->GetProperty()->SetLineWidth(1.0);
		s->drape->GetProperty()->SetEdgeVisibility(edges);   // tracks the base; toggled by 'e'
		s->ren->AddActor(s->drape);
		draped = true;
	}

	// --- cube axes ----------------------------------------------------------
	s->axes = vtkSmartPointer<vtkCubeAxesActor>::New();
	s->axes->SetCamera(s->ren->GetActiveCamera());
	double b[6]; surfGetBounds(s, b);
	if (b[5] - b[4] <= 0.0) b[5] = b[4] + 1.0;          // zero Z extent (bare image / flat) -> avoid
	                                                    // vtkAxisActor 0/0 label-count crash
	s->axes->SetBounds(b);
	s->axes->SetXAxisRange(s->x0, s->x1);               // TRUE labels despite the actor scale
	s->axes->SetYAxisRange(s->y0, s->y1);
	s->axes->SetZAxisRange(s->zmin, s->zmax);
	s->axes->GetTitleTextProperty(0)->SetColor(0.9, 0.9, 0.9);
	s->axes->GetLabelTextProperty(0)->SetColor(0.8, 0.8, 0.8);
	for (int ax = 0; ax < 3; ++ax) {
		s->axes->GetXAxesLinesProperty()->SetColor(0.7, 0.7, 0.7);
	}
	// Geographic data -> lon/lat axis names; cartesian -> X/Y. Z always "Z". Drawn as overlay
	// billboards (placeAxisTitle), NOT cube-native titles (those don't render in this StaticTriad
	// setup with native labels off). Clear the cube titles so nothing competes.
	s->axes->SetXTitle(" "); s->axes->SetYTitle(" "); s->axes->SetZTitle(" ");  // single space, NOT "" — empty makes vtkVectorText error "Text is not set!" every render
	s->axName[0] = geographic ? "lon" : "X";
	s->axName[1] = geographic ? "lat" : "Y";   // X/Y names only — Z gets NO name title
	for (int i = 0; i < 2; ++i) {
		auto t = vtkSmartPointer<vtkBillboardTextActor3D>::New();
		vtkTextProperty *tp = t->GetTextProperty();
		tp->SetColor(1.0, 1.0, 1.0);
		tp->SetFontFamilyToArial(); tp->BoldOn(); tp->ItalicOff(); tp->ShadowOff();
		tp->SetFontSize(13);                 // a touch larger + bold so the name reads as a title
		tp->SetJustificationToCentered();
		tp->SetVerticalJustificationToCentered();
		t->SetInput(s->axName[i].c_str());
		t->ForceOpaqueOn();
		t->PickableOff();
		t->SetVisibility(0);                 // rebuildAxisLabels positions + shows it
		s->axesRen->AddViewProp(t);          // overlay layer: even brightness, never occluded
		s->axTitle[i] = t;
	}
	s->axes->DrawXGridlinesOn(); s->axes->DrawYGridlinesOn(); s->axes->DrawZGridlinesOn();
	s->axes->SetGridLineLocation(vtkCubeAxesActor::VTK_GRID_LINES_FURTHEST);
	// StaticTriad pins X,Y to the zmin FLOOR (coplanar) — Y/X labels are ALWAYS on the bottom
	// edge, never lifting to a top edge — with native 3-D text (parallel/orthogonal to the axis,
	// reorienting with the view). Mirrors the user's f3d_ext_cube_axes.cxx (his HARD RULE). Z's
	// OWN labels run along the axis -> OFF; Z values are drawn as horizontal billboards (ALWAYS
	// perpendicular to Z) by rebuildAxisLabels().
	s->axes->SetFlyModeToStaticTriad();
	s->axes->SetTickLocationToOutside();
	s->axes->SetScreenSize(13.0);
	s->axes->SetZAxisVisibility(1);          // draw the Z axis LINE (+ gridlines) like X/Y
	// Native value labels AND native ticks OFF on ALL THREE axes. rebuildAxisLabels draws the
	// values as identical freetype billboards AND draws our own SINGLE outward tickmark per label
	// (s->axisTicks) — the cube's native ticks were doubled across the two faces sharing each
	// edge. Only the cube's axis LINES + gridlines remain.
	s->axes->SetXAxisLabelVisibility(0);
	s->axes->SetYAxisLabelVisibility(0);
	s->axes->SetZAxisLabelVisibility(0);
	s->axes->SetXAxisTickVisibility(0);
	s->axes->SetYAxisTickVisibility(0);
	s->axes->SetZAxisTickVisibility(0);
	// MAJOR ticks only on every axis. Minor ticks defaulted ON and made a dense two-directional
	// comb on Z (its range is thousands, so minor=majorDelta/5 packed ~30 marks; X/Y ranges are
	// small so theirs stayed sparse) -> Z now ticks like X/Y.
	s->axes->XAxisMinorTickVisibilityOff();
	s->axes->YAxisMinorTickVisibilityOff();
	s->axes->ZAxisMinorTickVisibilityOff();
	for (int i = 0; i < 3; ++i) {                // white, ARIAL, non-bold -> X/Y/Z share ONE font
		vtkTextProperty *tp = s->axes->GetTitleTextProperty(i);
		tp->SetColor(1.0, 1.0, 1.0); tp->SetFontFamilyToArial(); tp->BoldOff(); tp->ItalicOff(); tp->ShadowOff();
		vtkTextProperty *lp = s->axes->GetLabelTextProperty(i);
		lp->SetColor(1.0, 1.0, 1.0); lp->SetFontFamilyToArial(); lp->BoldOff(); lp->ItalicOff(); lp->ShadowOff();
	}
	// Empty launcher (blankStart): the cube axes + tick/label billboards are NEVER added to the
	// renderer and the initial label build is skipped, so the blank window can't flash an axis box
	// with numbers for a frame. A dropped file PROMOTES this same window via buildSceneContent.
	if (!blankStart) s->ren->AddActor(s->axes);

	// Our own SINGLE outward tickmarks (rebuilt every render by rebuildAxisLabels). Unlit grey
	// lines, like the cube's axis lines; the cube's native (doubled) ticks are off.
	s->axisTickPD = vtkSmartPointer<vtkPolyData>::New();
	{
		vtkNew<vtkPolyDataMapper> tm; tm->SetInputData(s->axisTickPD);
		s->axisTicks = vtkSmartPointer<vtkActor>::New();
		s->axisTicks->SetMapper(tm);
		s->axisTicks->GetProperty()->SetColor(0.85, 0.85, 0.85);
		s->axisTicks->GetProperty()->LightingOff();
		s->axisTicks->GetProperty()->SetLineWidth(1.0);
		s->axisTicks->PickableOff();
		if (!blankStart) s->ren->AddActor(s->axisTicks);
	}
	if (!blankStart) rebuildAxisLabels(s);        // billboards (same font/size on X/Y/Z) + single ticks

	// --- scalar bar ---------------------------------------------------------
	// Coloured strip (vtkScalarBarActor) + our own tick marks / numbers, all positioned from
	// s->barX0/barY0 so the assembly toggles and DRAGS as one unit. Bare images get no colorbar.
	buildColorbar(s, lut, s->zmin, s->zmax);
	s->actZ = &s->gridZ; s->actNx = s->gnx; s->actNy = s->gny;   // base relief is the initial active grid
	s->actX0 = s->gx0; s->actX1 = s->gx1; s->actY0 = s->gy0; s->actY1 = s->gy1;

	// Default view: world +Z up; azimuth 0 (look north, +Y) and elevation 35deg above
	// horizontal. Camera sits due south of the focal point, raised 35deg. Then zoom in so
	// the relief fills most of the display (ResetCamera alone leaves a wide margin).
	s->ren->ResetCamera();
	{
		vtkCamera *cam = s->ren->GetActiveCamera();
		double fp[3]; cam->GetFocalPoint(fp);
		double dist = cam->GetDistance();
		const double el = 35.0 * vtkMath::Pi() / 180.0;
		cam->SetViewUp(0.0, 0.0, 1.0);
		cam->SetPosition(fp[0],
		                 fp[1] - dist * std::cos(el),
		                 fp[2] + dist * std::sin(el));
		s->ren->ResetCamera();                  // refit distance along the new direction
		cam->Zoom(1.5);                         // fill most of the display area
		s->ren->ResetCameraClippingRange();
	}

	// SSAO sampling radius scales with the scene size; seed it from the bbox
	// diagonal, then build the whole light/material/pass setup via applyShading
	// (the Shading dock re-runs the same function on every slider change).
	{
		double bb[6]; s->ren->ComputeVisiblePropBounds(bb);
		double diag = std::sqrt((bb[1]-bb[0])*(bb[1]-bb[0]) +
		                        (bb[3]-bb[2])*(bb[3]-bb[2]) +
		                        (bb[5]-bb[4])*(bb[5]-bb[4]));
		if (diag > 0.0) { s->ssaoRadius = 0.1 * diag; s->ssaoBias = 1e-4 * diag; }
	}

	// --- picker for coordinate readout --------------------------------------
	// (The hover readout uses a GPU z-buffer pick in onMouseMove — O(1) for any grid size — so no
	// cell locator is built here; vtkCellPicker would be O(cells) brute force and OOM on big grids.)
	s->picker = vtkSmartPointer<vtkCellPicker>::New();
	s->picker->SetTolerance(0.001);

	// --- profile track drape line (Ctrl+left-drag fills it) -----------------
	{
		vtkNew<vtkPolyDataMapper> pm;
		vtkNew<vtkPolyData> empty;
		pm->SetInputData(empty);
		pm->ScalarVisibilityOff();
		vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
		pm->SetRelativeCoincidentTopologyLineOffsetParameters(0.0, -9000.0);  // sit above surface + drape
		s->profLine = vtkSmartPointer<vtkActor>::New();
		s->profLine->SetMapper(pm);
		s->profLine->GetProperty()->LightingOff();
		s->profLine->GetProperty()->SetColor(1.0, 0.15, 0.15);   // red track, Fledermaus-style
		s->profLine->GetProperty()->SetLineWidth(2.5);
		s->profLine->SetScale(s->xfac, 1.0, s->zfac * s->ve);
		s->profLine->SetVisibility(0);
		s->ren->AddActor(s->profLine);
	}
}

// ============================================================================
// Earth Tides dialog (Geography > Earth Tides) — port of Mirone's earth_tides.
// ============================================================================

// One-shot map-pick: when the user hits "Click point on map" the dialog hides and installs this
// filter on the render widget; the NEXT left-click (no drag) is converted to lon/lat (ray to the
// z=0 map plane, undo the X aspect scale) and handed back via cb, then the filter removes itself.
// No Q_OBJECT/moc needed (no signals/slots). Mirrors the readout math in onMouseMove (10_geometry).
class MapPickFilter : public QObject {
public:
	Scene *s = nullptr;
	std::function<void(double, double)> cb;
	bool down = false, moved = false; double px = 0, py = 0;
	MapPickFilter(Scene *sc, QObject *parent, std::function<void(double, double)> f)
		: QObject(parent), s(sc), cb(std::move(f)) {}
protected:
	bool eventFilter(QObject *obj, QEvent *ev) override {
		const QEvent::Type t = ev->type();
		if (t == QEvent::MouseButtonPress) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (me->button() == Qt::LeftButton) {
				down = true; moved = false; px = me->position().x(); py = me->position().y();
				return true;                                  // swallow so VTK doesn't start a rotate
			}
		} else if (t == QEvent::MouseMove && down) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (std::abs(me->position().x() - px) > 3 || std::abs(me->position().y() - py) > 3) moved = true;
			return true;
		} else if (t == QEvent::MouseButtonRelease && down) {
			QMouseEvent *me = static_cast<QMouseEvent*>(ev);
			if (me->button() == Qt::LeftButton) {
				down = false;
				if (!moved && s->ren && s->widget && s->widget->renderWindow()) {
					const double r = s->widget->devicePixelRatioF();
					const int    H = s->widget->renderWindow()->GetSize()[1];
					const double mx = me->position().x() * r, my = H - me->position().y() * r;
					double nr[4], fr[4];
					s->ren->SetDisplayPoint(mx, my, 0.0); s->ren->DisplayToWorld();
					for (int i = 0; i < 4; ++i) nr[i] = s->ren->GetWorldPoint()[i];
					s->ren->SetDisplayPoint(mx, my, 1.0); s->ren->DisplayToWorld();
					for (int i = 0; i < 4; ++i) fr[i] = s->ren->GetWorldPoint()[i];
					if (nr[3] != 0.0) { nr[0] /= nr[3]; nr[1] /= nr[3]; nr[2] /= nr[3]; }
					if (fr[3] != 0.0) { fr[0] /= fr[3]; fr[1] /= fr[3]; fr[2] /= fr[3]; }
					const double dz = fr[2] - nr[2];
					if (dz != 0.0) {
						const double t0 = -nr[2] / dz;
						const double gx = (s->xfac != 0.0) ? s->xfac : 1.0;
						const double lon = (nr[0] + t0 * (fr[0] - nr[0])) / gx;
						const double lat =  nr[1] + t0 * (fr[1] - nr[1]);
						if (cb) cb(lon, lat);
					}
				}
				if (s->widget) s->widget->removeEventFilter(this);
				deleteLater();
				return true;
			}
		}
		return QObject::eventFilter(obj, ev);
	}
};

// Build + run the modeless Earth Tides dialog. (cW..cN) is the current visible region, used to seed
// Lon/Lat (its centre) and, in grid mode, the computed -R. On OK the chosen settings are packed into
// the request string and handed to Julia (g_juliaEarthTide). Modeless so "Click point on map" can
// reach the map: the dialog hides, MapPickFilter grabs the next click, refills Lon/Lat, reshows.
static void showEarthTidesDialog(Scene *s, double cW, double cE, double cS, double cN) {
	if (!g_juliaEarthTide) {
		if (s->win) s->win->statusBar()->showMessage("Earth Tides: callback not registered", 3000);
		return;
	}
	const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
	QDialog *dlg = new QDialog(s->win);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle("Earth Tides");

	QDateTimeEdit *eStart = new QDateTimeEdit(nowUtc, dlg);
	QDateTimeEdit *eEnd   = new QDateTimeEdit(nowUtc.addDays(10), dlg);
	for (QDateTimeEdit *e : { eStart, eEnd }) {
		e->setDisplayFormat("dd-MMM-yyyy HH:mm:ss"); e->setCalendarPopup(true); e->setTimeSpec(Qt::UTC);
	}
	QDoubleSpinBox *eLon = new QDoubleSpinBox(dlg);
	eLon->setRange(-360.0, 360.0); eLon->setDecimals(4); eLon->setValue(0.5 * (cW + cE));
	QDoubleSpinBox *eLat = new QDoubleSpinBox(dlg);
	eLat->setRange(-90.0, 90.0); eLat->setDecimals(4); eLat->setValue(0.5 * (cS + cN));

	QRadioButton *rSeries = new QRadioButton("Time series", dlg); rSeries->setChecked(true);
	QRadioButton *rGrid   = new QRadioButton("Grid(s)", dlg);
	QButtonGroup *mode = new QButtonGroup(dlg);
	mode->addButton(rSeries); mode->addButton(rGrid);
	// Grid(s) uses a single instant (Start date) over a global region, so freeze End date then; a
	// time series spans Start->End, so unfreeze it for "Time series".
	QObject::connect(rGrid, &QRadioButton::toggled, dlg, [eEnd](bool on) { eEnd->setEnabled(!on); });
	eEnd->setEnabled(!rGrid->isChecked());                 // initial state (series default -> enabled)

	QCheckBox *cV = new QCheckBox("Vertical", dlg); cV->setChecked(true);
	QCheckBox *cE2 = new QCheckBox("East", dlg);
	QCheckBox *cN2 = new QCheckBox("North", dlg);

	QPushButton *bPick = new QPushButton("Click point on map", dlg);

	// Grid spacing (degrees) for Grid(s) mode; relevant only when gridding -> enabled with rGrid.
	QDoubleSpinBox *eInc = new QDoubleSpinBox(dlg);
	eInc->setRange(0.05, 10.0); eInc->setDecimals(2); eInc->setSingleStep(0.25); eInc->setValue(0.5);
	QObject::connect(rGrid, &QRadioButton::toggled, dlg, [eInc](bool on) { eInc->setEnabled(on); });
	eInc->setEnabled(rGrid->isChecked());                  // disabled in the default Time-series mode

	// Layout: left column = dates + mode + components; right column = lon/lat + grid inc + pick + OK.
	QFormLayout *left = new QFormLayout;
	left->addRow("Start date:", eStart);
	left->addRow("End date:",   eEnd);
	left->addRow(rSeries);
	left->addRow(rGrid);
	left->addRow(cV);
	left->addRow(cE2);
	left->addRow(cN2);

	QFormLayout *right = new QFormLayout;
	right->addRow("Lon:", eLon);
	right->addRow("Lat:", eLat);
	right->addRow("Grid inc (°):", eInc);
	right->addRow(bPick);
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
	right->addRow(bb);

	QHBoxLayout *cols = new QHBoxLayout(dlg);
	cols->addLayout(left); cols->addSpacing(16); cols->addLayout(right);

	// "Click point on map": hide the dialog, arm a one-shot pick on the map widget; on click refill
	// Lon/Lat and reshow. (Grid region still comes from the visible extent captured at menu time.)
	QObject::connect(bPick, &QPushButton::clicked, dlg, [s, dlg, eLon, eLat]() {
		if (!s->widget) return;
		dlg->lower();                                       // keep visible (don't hide), just out of the way
		if (s->win) s->win->statusBar()->showMessage("Earth Tides: click a point on the map…", 5000);
		MapPickFilter *f = new MapPickFilter(s, s->widget, [dlg, eLon, eLat](double lon, double lat) {
			eLon->setValue(lon); eLat->setValue(lat);
			dlg->raise(); dlg->activateWindow();
		});
		s->widget->installEventFilter(f);
	});
	QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
	QObject::connect(bb, &QDialogButtonBox::accepted, dlg,
	                 [s, dlg, eStart, eEnd, eLon, eLat, eInc, rGrid, cV, cE2, cN2, cW, cE, cS, cN]() {
		QString comp;
		if (cV->isChecked())  comp += 'V';
		if (cE2->isChecked()) comp += 'E';
		if (cN2->isChecked()) comp += 'N';
		if (comp.isEmpty()) {
			if (s->win) s->win->statusBar()->showMessage("Earth Tides: pick at least one component", 3000);
			return;
		}
		const char *m = rGrid->isChecked() ? "grid" : "series";
		// req = mode/start/end/lon/lat/comp/inc/W/E/S/N. inc = grid spacing (deg); region fields are
		// kept for layout stability but ignored by the (always-global) grid path.
		const QString req = QString("%1/%2/%3/%4/%5/%6/%7/%8/%9/%10/%11").arg(m)
			.arg(eStart->dateTime().toString("yyyy-MM-ddTHH:mm:ss"))
			.arg(eEnd->dateTime().toString("yyyy-MM-ddTHH:mm:ss"))
			.arg(eLon->value(), 0, 'f', 4).arg(eLat->value(), 0, 'f', 4).arg(comp)
			.arg(eInc->value(), 0, 'f', 4)
			.arg(cW, 0, 'f', 6).arg(cE, 0, 'f', 6).arg(cS, 0, 'f', 6).arg(cN, 0, 'f', 6);
		if (s->win) s->win->statusBar()->showMessage("Earth Tides: computing…", 3000);
		g_juliaEarthTide(s, req.toUtf8().constData());     // keep the dialog open for repeated runs
	});
	dlg->show();
}

// SINGLE source of truth for the flat-2D <-> 3D view switch. The toolbar flyout, the View menu, the
// context menu AND the grid/image init in 90_c_api ALL go through here — never re-implement the
// camera math. 2D = top-down orthographic over the surface bounds, rotation/tilt locked (gated via
// s->flat2d), gizmo hidden; the 3-D camera is saved on ENTER so leaving 2D restores it. Idempotent:
// a no-op (bar the act2D checkmark) when already in the requested mode — so a caller that needs to
// FORCE the 2D camera after rebuilding the scene must reset s->flat2d=false first (the rebuilt scene
// left the 3-D camera, but the flag may still read 2D from the launcher).
static void sceneSetFlat2D(Scene *s, bool on) {
	if (!s || !s->ren) return;
	if (on == s->flat2d) { if (s->act2D) s->act2D->setChecked(on); return; }
	vtkCamera *cam = s->ren->GetActiveCamera();
	s->flat2d = on;
	if (s->flat2d) {
		cam->GetPosition(s->sav_pos);          // save the 3D view to restore later
		cam->GetFocalPoint(s->sav_foc);
		cam->GetViewUp(s->sav_vup);
		s->sav_parallel = cam->GetParallelProjection();
		// 2D = TOP-DOWN ORTHO ONLY. Keep the relief and its PBR lighting exactly as in 3D
		// (illumination must NOT change) — viewed straight down in parallel projection it reads
		// as a shaded-relief map. We do NOT flatten (ve) or touch lighting.
		if (s->giz) setGizmoVisible(*s->giz, false);
		double b[6]; surfGetBounds(s, b);      // north (+Y) up
		const double fp[3] = { 0.5*(b[0]+b[1]), 0.5*(b[2]+b[3]), 0.5*(b[4]+b[5]) };
		cam->SetFocalPoint(fp[0], fp[1], fp[2]);
		cam->SetViewUp(0.0, 1.0, 0.0);
		cam->SetPosition(fp[0], fp[1], b[5] + (b[5]-b[4]) + 1.0);  // above the surface, not inside it
		cam->ParallelProjectionOn();
		s->ren->ResetCameraClippingRange();
		fitSnapView(s, /*topMode=*/true);      // maximize: fill the viewport edge-to-edge
	}
	else {
		cam->SetParallelProjection(s->sav_parallel);
		cam->SetPosition(s->sav_pos);
		cam->SetFocalPoint(s->sav_foc);
		cam->SetViewUp(s->sav_vup);
		if (s->giz) setGizmoVisible(*s->giz, true);
		s->ren->ResetCameraClippingRange();
	}
	// The buried 3-D fault plane is meaningless top-down — show it only off flat-2D AND when the user
	// has not hidden it via its handle (faultPlane3DShown).
	for (auto& pg : s->polys) if (pg.isFault && pg.faultPlane3D) {
		pg.faultPlane3D->SetVisibility((pg.faultPlane3DShown && !s->flat2d) ? 1 : 0);
		if (pg.faultArrows) pg.faultArrows->SetVisibility((pg.faultPlane3DShown && !s->flat2d) ? 1 : 0);
	}
	// solid3D symbol layers (sphere/cube, e.g. buried seismicity) need real depth-tested occlusion in
	// 3-D but must always show as flat map pins in 2-D (see applyStacking) — re-derive their overlay-
	// layer placement for the mode we just entered.
	applyVectorStacking(s);
	if (s->act2D) s->act2D->setChecked(s->flat2d);
	if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
}

static Scene *buildAndShow(vtkSmartPointer<vtkPolyData> pd,
						 double x0, double x1, double y0, double y1,
						 double zmin, double zmax,
						 double xfac, double zfac, double ve0,
						 const double *cz, const double *crgb, int ncolor,  // CPT nodes: cz[n] + crgb[n*3] 0..1; 0 = default
						 const unsigned char *img, int iw, int ih, int ibands,  // optional drape: RGB[A] iw*ih*ibands, row 0 = south
						 int edges,                                             // !=0 -> draw mesh edges (GMTF3D :shademesh)
						 bool pointCloud,                                       // true -> Verts-only cloud: LOD actor, no normals/drape
						 int geographic,                                        // !=0 -> x,y are lon,lat (axis titles "lon"/"lat")
						 const char *title,
						 const char *objname = nullptr,    // Scene Objects label for the surface ("" -> "Surface")
						 bool imageOnly = false,            // bare image: no surface row; readout shows colour
						 const float *gz = nullptr,         // non-null -> TILED plain-grid render (pd ignored)
						 int gnx = 0, int gny = 0,          // grid dims for the tiled path
						 bool blankStart = false,           // empty launcher: open as a clean dark canvas (no axes flash)
						 bool openFlat2D = false) {         // open in flat-2D from the FIRST frame (grids) — no 3-D flash
	ensureApp();

	Scene *s = new Scene();
	g_scenes.insert(s);                     // register as a live figure handle
	s->imageOnly = imageOnly;               // set BEFORE the Scene Objects panel is built (rebuildSceneObjects)
	if (imageOnly) {                        // bare image: unlit picture, nothing PBR/lit to tone-map or occlude.
		// Tone mapping + SSAO are screen-space passes applied to the WHOLE framebuffer, so even though
		// the image actor itself is LightingOff (imageRebuildActor), vtkToneMappingPass still remapped
		// its raw sRGB texture colours as if they were linear HDR radiance -> visibly DARKENED the
		// picture (e.g. Base Map's etopo4.jpg import). The Shading dock is already folded here because
		// "nothing to light" (see hasShadedBody below) -- match that intent in the actual pass chain too.
		s->useTone = false;
		s->useSSAO = false;
	}
	s->zmin = zmin; s->zmax = zmax;
	s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1;
	s->xfac = xfac; s->zfac = zfac; s->ve = ve0;

	// --- VTK render window in a Qt widget -----------------------------------
	auto *widget = new GLView();
	vtkNew<vtkGenericOpenGLRenderWindow> rw;
	rw->SetMultiSamples(0);                 // no hardware MSAA (FXAA post-pass does the AA, like F3D).
	                                        // 8x MSAA = 8x fragment work every frame -> kills big point clouds.
	widget->setRenderWindow(rw);
	s->widget = widget;
	widget->s = s;                          // GLView handles the middle button itself
	g_lastRW = rw;
	g_lastScene = s;                        // gmtvtk_add_overlay targets the most-recent scene

	vtkNew<vtkNamedColors> nc;
	s->ren = vtkSmartPointer<vtkRenderer>::New();
	s->ren->GradientBackgroundOn();
	s->ren->SetBackground(0.16, 0.18, 0.22);    // bottom (dark slate)
	s->ren->SetBackground2(0.36, 0.42, 0.52);   // top
	rw->AddRenderer(s->ren);

	// Overlay layer (1) for the Z tick billboards. It shares the MAIN camera (so the labels
	// track the same view), keeps the lower layer's colour (transparent except where text is),
	// and clears its own depth so the surface can never occlude the labels. Its default
	// auto-created headlight lights the always-camera-facing text uniformly -> constant
	// brightness at every rotation, fixing the "labels go dark/invisible on some angles" bug.
	rw->SetNumberOfLayers(2);
	s->axesRen = vtkSmartPointer<vtkRenderer>::New();
	s->axesRen->SetLayer(1);
	s->axesRen->InteractiveOff();
	s->axesRen->PreserveColorBufferOn();
	s->axesRen->SetActiveCamera(s->ren->GetActiveCamera());
	rw->AddRenderer(s->axesRen);

	// F3D-style light rig: a 3-point vtkLightKit (key/fill/back/head) instead of
	// the single flat headlight VTK adds by default. This is what gives F3D's
	// relief its form-revealing gradients. (F3D: vtkF3DRenderer::UpdateLights.)
	// Lighting: one user-aimed directional KEY light (azimuth/elevation) + a dim
	// FILL light, both managed by applyShading. Direction is set there from
	// s->lightAz / s->lightEl so the Shading dock can move the "sun" live.
	s->ren->SetAutomaticLightCreation(false);
	s->keyLight = vtkSmartPointer<vtkLight>::New();
	s->keyLight->SetLightTypeToSceneLight();
	s->keyLight->SetPositional(false);          // infinite (directional) light
	s->fillLight = vtkSmartPointer<vtkLight>::New();
	s->fillLight->SetLightTypeToHeadlight();    // fills the camera-facing shadow side
	s->ren->AddLight(s->keyLight);
	s->ren->AddLight(s->fillLight);
	s->envTex = makeSkyEnv();

	buildSceneContent(s, pd, x0, x1, y0, y1, cz, crgb, ncolor, img, iw, ih, ibands,
	                  edges, pointCloud, geographic, gz, gnx, gny, blankStart);

	// --- main window + native menubar ---------------------------------------
	// Heap-allocated + delete-on-close: the function returns immediately (the host
	// pumps the loop), so the window must outlive this stack frame.
	QMainWindow *win = new QMainWindow();
	win->setAttribute(Qt::WA_DeleteOnClose);
	win->setWindowTitle(title ? title : "i'GMT");
	win->setWindowIcon(appIcon());          // per-window titlebar icon (matches the app-wide icon)
	win->resize(1100, 800);
	win->setCentralWidget(widget);
	win->statusBar()->showMessage("ready");
	enableFileDrops(win, widget, s);        // drop a grid/image/table file onto any window to add it
	s->win = win;
	++g_openWindows;
	QObject::connect(win, &QObject::destroyed, [s, rwp = rw.Get()]() {
		--g_openWindows;
		if (g_lastScene == s) g_lastScene = nullptr;   // don't let add_overlay touch a freed scene
		if (g_lastRW == rwp) g_lastRW = nullptr;       // don't let gmtvtk_save_png capture a freed window (crash)
		g_scenes.erase(s);                             // invalidate any host-held handle to s
		delete s->giz; delete s;
	});

	auto actReset = [s]() {
		s->ren->ResetCamera();
		s->widget->renderWindow()->Render();
	};
	auto actToggleAxes = [s]() {
		s->axes->SetVisibility(!s->axes->GetVisibility());
		s->widget->renderWindow()->Render();
	};
	auto actToggleBar = [s]() {
		if (!s->bar) return;                 // bare image has no colorbar
		setColorbarVisible(s, !colorbarVisible(s));
		s->widget->renderWindow()->Render();
	};
	auto actVE = [s]() {
		bool ok = false;
		double v = QInputDialog::getDouble(s->win, "Vertical exaggeration",
										   "VE factor:", s->ve, 0.01, 1.0e4, 3, &ok);
		if (ok) { s->ve = v; applyVE(s); }
	};
	auto actShot = [s]() {
		QString fn = QFileDialog::getSaveFileName(s->win, "Save screenshot", prefStartDir("gmtvtk.png"), "PNG (*.png)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		vtkNew<vtkWindowToImageFilter> w2i;
		w2i->SetInput(s->widget->renderWindow());
		w2i->SetScale(2); w2i->Update();
		vtkNew<vtkPNGWriter> wr;
		wr->SetFileName(fn.toLocal8Bit().constData());
		wr->SetInputConnection(w2i->GetOutputPort());
		wr->Write();
	};
	// Screenshot-GeoTIFF capture: fit the primary raster edge-to-edge in flat top-down 2-D (the
	// only camera state where screen pixels map affinely to world coordinates), hide the cube axes
	// + colour bar (decoration, not data), grab the frame, then crop to exactly the axes interior
	// (the data bbox's own on-screen rectangle) — same NDC projection fitSnapView uses to FIT the
	// view, run here to READ the pixel rect back. Restores every toggled state before returning.
	// Returns the cropped RGB pixels directly in `outRgb`/`outW`/`outH` — no PNG encode, no temp
	// file. Rows are flipped to top-first while copying (VTK's vtkImageData is bottom-up/y-up;
	// mat2img on the Julia side defaults to top-first, matching what a re-read PNG would have
	// given it) so Julia can wrap the buffer straight into a GMTimage. This function only owns the
	// render — no GMT/GDAL calls belong in the C++ side.
	auto captureAxesInteriorRGB = [s](std::vector<unsigned char> &outRgb, int &outW, int &outH) -> bool {
		if (!s->ren || !s->surf || !s->widget) return false;
		const bool wasFlat = s->flat2d;
		if (!wasFlat) sceneSetFlat2D(s, true);
		fitSnapView(s, /*topMode=*/true);        // guarantee the data fills edge-to-edge, no stale pan/zoom
		const int  axesVis = s->axes ? s->axes->GetVisibility() : 0;
		const bool barVis  = colorbarVisible(s);
		if (s->axes) s->axes->SetVisibility(0);  // decoration only — never part of the georeferenced pixels
		if (s->bar)  setColorbarVisible(s, false);
		s->widget->renderWindow()->Render();

		vtkNew<vtkWindowToImageFilter> w2i;
		w2i->SetInput(s->widget->renderWindow());
		w2i->SetScale(2); w2i->Update();
		vtkImageData *full = w2i->GetOutput();
		int dims[3]; full->GetDimensions(dims);

		// Project the data bbox through the (now edge-to-edge-fitted) camera to find its own pixel
		// rectangle within the captured frame.
		vtkCamera *cam = s->ren->GetActiveCamera();
		double b[6]; surfGetBounds(s, b);
		const double aspect = (dims[1] > 0) ? double(dims[0]) / double(dims[1]) : 1.0;
		vtkMatrix4x4 *M = cam->GetCompositeProjectionTransformMatrix(aspect, -1.0, 1.0);
		double nx0=1e300, nx1=-1e300, ny0=1e300, ny1=-1e300;
		for (double cx : { b[0], b[1] })
			for (double cy : { b[2], b[3] })
				for (double cz : { b[4], b[5] }) {
					double p[4] = { cx, cy, cz, 1.0 }, o[4];
					M->MultiplyPoint(p, o);
					if (o[3] != 0.0) {
						const double ndcx = o[0]/o[3], ndcy = o[1]/o[3];
						nx0 = std::min(nx0, ndcx); nx1 = std::max(nx1, ndcx);
						ny0 = std::min(ny0, ndcy); ny1 = std::max(ny1, ndcy);
					}
				}
		auto ndcToPix = [](double n, int size) { return std::clamp(int(std::round((n*0.5+0.5) * (size-1))), 0, size-1); };
		const int px0 = ndcToPix(nx0, dims[0]), px1 = ndcToPix(nx1, dims[0]);
		const int py0 = ndcToPix(ny0, dims[1]), py1 = ndcToPix(ny1, dims[1]);
		const bool ok = (px1 > px0 && py1 > py0);
		if (ok) {
			const int cw = px1 - px0 + 1, ch = py1 - py0 + 1;
			outW = cw; outH = ch;
			outRgb.resize(size_t(cw) * size_t(ch) * 3);
			for (int row = 0; row < ch; ++row) {
				auto *src = static_cast<unsigned char*>(full->GetScalarPointer(px0, py0 + row, 0));
				unsigned char *dst = outRgb.data() + size_t(ch - 1 - row) * size_t(cw) * 3;  // bottom-up -> top-first
				std::memcpy(dst, src, size_t(cw) * 3);
			}
		}

		if (s->axes) s->axes->SetVisibility(axesVis);
		if (s->bar)  setColorbarVisible(s, barVis);
		if (!wasFlat) sceneSetFlat2D(s, false);
		s->widget->renderWindow()->Render();
		return ok;
	};
	// Save Screenshot GeoTIFF: only meaningful for a grid/image with a known reference system (the
	// Geography-menu gate, s->hasCRS()) — an unreferenced scene, or a bare solid/mesh, has no W/E/S/N
	// to write into a geotransform. Capture happens entirely in C++ (captureAxesInteriorRGB); the
	// pixels are handed to Julia in memory (g_juliaSaveGeoTiff) — no temp file, no PNG round-trip.
	auto actShotGeoTiff = [s, captureAxesInteriorRGB]() {
		if (!s->hasCRS() || !(sceneHasGrid(s) || sceneHasImage(s))) {
			if (s->win) s->win->statusBar()->showMessage(
				"Save Screenshot GeoTIFF: needs a grid/image with a known reference system", 4000);
			return;
		}
		QString fn = QFileDialog::getSaveFileName(s->win, "Save screenshot as GeoTIFF",
												   prefStartDir("gmtvtk.tif"), "GeoTIFF (*.tif *.tiff)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		if (!g_juliaSaveGeoTiff) {
			if (s->win) s->win->statusBar()->showMessage("Save Screenshot GeoTIFF: callback not registered", 3000);
			return;
		}
		std::vector<unsigned char> rgb;
		int w = 0, h = 0;
		if (!captureAxesInteriorRGB(rgb, w, h)) {
			if (s->win) s->win->statusBar()->showMessage("Save Screenshot GeoTIFF: capture failed", 4000);
			return;
		}
		g_juliaSaveGeoTiff(s, rgb.data(), w, h, fn.toUtf8().constData(),
						   s->x0, s->x1, s->y0, s->y1,
						   s->crsProj4.c_str(), s->crsWkt.c_str());
	};
	auto actToggleGizmo = [s]() {
		if (s->giz) { setGizmoVisible(*s->giz, !s->giz->visible); s->widget->renderWindow()->Render(); }
	};
	// Flat-2D <-> 3D switch — the camera math lives in the file-scope sceneSetFlat2D (the SINGLE
	// source of truth, shared with the grid/image init in 90_c_api). These are just the UI handles.
	auto setFlat2D   = [s](bool on) { sceneSetFlat2D(s, on); };
	auto actToggle2D = [s]() { sceneSetFlat2D(s, !s->flat2d); };
	auto actAbout = [win]() {
		QMessageBox::about(win, "About",
			"i'GMT 3-D Viewer\n\nNative Qt UI + VTK 3-D, self-contained.\n\n"
			"Left-drag: horizontal = rotate (azimuth), vertical = tilt.\n"
			"Middle-click: set the centre of rotation to that point.\n"
			"Right-drag / wheel: zoom.\n"
			"Gizmo handles — amber cone: vert. exaggeration;  tip ring: tilt;  "
			"compass ring: azimuth.   'x' hides/shows the gizmo.");
	};

	QMenu *mFile = win->menuBar()->addMenu("&File");
	// Preferences: settings dialog (deps/ui/preferences.ui). Values persist via QSettings.
	mFile->addAction("&Preferences…", [win]() { PreferencesDialog(win).exec(); });
	mFile->addSeparator();
	// New Window: open a fresh empty iGMT launcher. Routed through Julia (g_juliaNewWindow) so the
	// new window is tracked in the Julia figure registry — the basis for future inter-window data
	// exchange. Reports if the callback is not wired.
	mFile->addAction("&New Window", [s]() {
		if (g_juliaNewWindow) g_juliaNewWindow(s);
		else if (s->win) s->win->statusBar()->showMessage("New Window: callback not registered", 3000);
	});
	mFile->addSeparator();
	mFile->addAction("Save Screenshot &img…", actShot);
	// Save Screenshot GeoTIFF: greyed out unless the window has a grid/image with a known reference
	// system (s->hasCRS()) — refreshed on every menu open, same gating rule as aSaveGrid/aSaveImage below.
	QAction *aShotGeoTiff = mFile->addAction("Save Screenshot &GeoTIFF…", actShotGeoTiff);
	// Save Grid / Save Image: each opens the format-picker dialog (saveObjectDialog) for the window's
	// primary object (empty name). Greyed out when the window holds no grid / no image — refreshed on
	// every menu open so it tracks drops, basemap tiles, etc.
	QAction *aSaveGrid  = mFile->addAction("Save &Grid…",  [s]() { saveObjectDialog(s, "grid",  QString()); });
	QAction *aSaveImage = mFile->addAction("Save &Image…", [s]() { saveObjectDialog(s, "image", QString()); });
	// Background region: open a blank white 2-D map framed to W/E/S/N (default the whole geographic
	// earth). The dialog hands "W/E/S/N/geographic" to Julia (g_juliaBgRegion), which opens a fresh
	// window — ready to drop coastlines / overlays onto. Reports if the callback is not wired.
	QAction *aBgRegion = mFile->addAction("&Background region…", [win, s]() {
		BgRegionDialog dlg(win);
		if (dlg.exec() != QDialog::Accepted || dlg.region.isEmpty()) return;
		if (g_juliaBgRegion) g_juliaBgRegion(s, dlg.region.toUtf8().constData());
		else if (s->win) s->win->statusBar()->showMessage("Background region: callback not registered", 3000);
	});
	// Per-open gating: Save entries reflect what's loaded; Background region is hidden for good once the
	// window holds ANY content (a grid, an image, or any dropped extra) — it only makes sense on a bare
	// launcher whose limits are still up for grabs.
	QObject::connect(mFile, &QMenu::aboutToShow, [s, aShotGeoTiff, aSaveGrid, aSaveImage, aBgRegion]() {
		aShotGeoTiff->setEnabled(s->hasCRS() && (sceneHasGrid(s) || sceneHasImage(s)));
		aSaveGrid->setEnabled(sceneHasGrid(s));
		aSaveImage->setEnabled(sceneHasImage(s));
		const bool hasContent = (s->surf && !s->emptyStart) || !s->extras.empty();
		aBgRegion->setVisible(!hasContent);
	});
	mFile->addSeparator();
	// Open known file types: file picker that uses same auto-detect logic as drag-and-drop.
	// Opens into THIS window (or promotes empty launcher) via g_juliaDrop.
	mFile->addAction("Open &known file types…", [win, s]() {
		if (!g_juliaDrop) {
			if (s->win) s->win->statusBar()->showMessage("Open: callback not registered", 3000);
			return;
		}
		const QStringList files = QFileDialog::getOpenFileNames(win, "Open File", prefStartDir());
		if (!files.isEmpty()) {
			rememberStartDir(files.first());               // push chosen dir to MRU
			for (const QString& f : files) {
				const QByteArray utf8 = f.toUtf8();        // keep buffer alive across call
				g_juliaDrop(s, utf8.constData());
			}
		}
	});
	mFile->addSeparator();
	// Recent Files: persistent MRU, grouped Grids/Images/Datasets, rebuilt each time it opens so a
	// file opened in any window shows up here too. Re-opens a pick in a NEW window via iview().
	QMenu *mRecent = mFile->addMenu("Recent &Files");
	mRecent->setToolTipsVisible(true);                       // show the full path on hover
	QObject::connect(mRecent, &QMenu::aboutToShow, [mRecent, s]() { populateRecentMenu(mRecent, s); });
	mFile->addSeparator();
	// Save / Load Session (.igmtz): Save writes THIS window's state (recipes + generated data) to a
		// single zip; Load rebuilds a window from one. Julia does the work (session.jl); a missing
		// callback reports in the status bar rather than doing nothing.
		mFile->addAction("Save &Session…", [win, s]() {
			if (!g_juliaSaveSession) {
				if (s->win) s->win->statusBar()->showMessage("Save Session: callback not registered", 3000);
				return;
			}
			QString f = QFileDialog::getSaveFileName(win, "Save Session", prefStartDir(), "iGMT Session (*.igmtz)");
			if (f.isEmpty()) return;
			if (!f.endsWith(".igmtz", Qt::CaseInsensitive)) f += ".igmtz";
			rememberStartDir(f);
			const QByteArray utf8 = f.toUtf8();
			g_juliaSaveSession(s, utf8.constData());
		});
		mFile->addAction("&Load Session…", [win, s]() {
			if (!g_juliaLoadSession) {
				if (s->win) s->win->statusBar()->showMessage("Load Session: callback not registered", 3000);
				return;
			}
			QString f = QFileDialog::getOpenFileName(win, "Load Session", prefStartDir(), "iGMT Session (*.igmtz)");
			if (f.isEmpty()) return;
			rememberStartDir(f);
			const QByteArray utf8 = f.toUtf8();
			g_juliaLoadSession(s, utf8.constData());
		});
		mFile->addSeparator();
		mFile->addAction("&Close", [win](){ win->close(); }, QKeySequence::Close);

	QMenu *mView = win->menuBar()->addMenu("&View");
	mView->addAction("&Reset Camera", actReset, QKeySequence("R"));
	QAction *aAxes = mView->addAction("Show Cube &Axes", actToggleAxes);
	aAxes->setCheckable(true); aAxes->setChecked(true);
	QAction *aBar = mView->addAction("Show Color &Bar", actToggleBar);
	aBar->setCheckable(true); aBar->setChecked(true);
	QAction *aGiz = mView->addAction("Show &Gizmo", actToggleGizmo);  // 'x' also toggles (VTK)
	aGiz->setCheckable(true); aGiz->setChecked(true);
	// Shared checkable "Flat 2D (map)" action — lives in the View menu AND the toolbar below, so
	// both reflect the same state. actToggle2D authors the checkmark (via setFlat2D).
	s->act2D = mView->addAction("Flat &2D (map)", actToggle2D);
	s->act2D->setCheckable(true); s->act2D->setChecked(false);
	mView->addSeparator();
	mView->addAction("Vertical &Exaggeration…", actVE);

	// --- Geography menu (mirrors Mirone's Geography menu, mirone_uis.m lines 459-575) ----------
	// Plots GMT/GSHHG geographic data (coastlines, borders, rivers) + point datasets (seismicity,
	// volcanoes, …). Leaf actions are wired to a stub for now; the real plotting lands later.
	auto geoTODO = [s](const QString &what) {
		return [s, what]() {
			if (s->win) s->win->statusBar()->showMessage("Geography: " + what + " — not implemented yet", 3000);
		};
	};
	// Current visible geographic region (W/E/S/N in TRUE data coords) = the part of the map on
	// screen at the current zoom. Project the 4 viewport corners onto the z=0 plane (the flat map),
	// undo the X aspect scale (xfac), take the bbox, and clamp to the data frame. Mirrors the hover
	// readout's DisplayToWorld ray-to-plane math (10_geometry.cpp). false if no renderer/window.
	auto visibleRegion = [s](double &W, double &E, double &S, double &N) -> bool {
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
			if (dirz == 0.0) continue;                          // ray parallel to the map plane
			const double t0 = -nr[2] / dirz;
			const double tx = (nr[0] + t0 * (fr[0] - nr[0])) / gx;
			const double ty =  nr[1] + t0 * (fr[1] - nr[1]);
			if (!any) { W = E = tx; S = N = ty; any = true; }
			else { W = std::min(W, tx); E = std::max(E, tx); S = std::min(S, ty); N = std::max(N, ty); }
		}
		if (!any) return false;
		W = std::max(W, s->x0); E = std::min(E, s->x1);         // never exceed the data frame
		S = std::max(S, s->y0); N = std::min(N, s->y1);
		return (E > W && N > S);
	};
	// The seismicity events need a base to land on. On an EMPTY launcher, load the whole-world
	// Base Map IN PLACE (the SAME "global" request the Base Maps picker sends -> _on_basemap
	// promotes the launcher + adds the etopo4 image) instead of refusing. Runs synchronously, so
	// on return s->x0..y1 already frame the world. false only if the basemap callback is missing.
	auto ensureSeisBase = [s]() -> bool {
		if (!s->emptyStart) return true;
		if (!g_juliaBaseMap) {
			if (s->win) s->win->statusBar()->showMessage("Seismicity: Base Map callback not registered", 3000);
			return false;
		}
		if (s->win) s->win->statusBar()->showMessage("Seismicity: loading base map…");
		showBusyDialog("Base Map");              // indeterminate busy bar (first-run GMT compile)
		g_juliaBaseMap(s, "-180/180/-90/90/0/global");
		closeBusyDialog();
		return true;
	};
	// Hand a seismicity request to Julia: make sure there is a base map, append the visible map
	// region (the in-map event crop + the USGS query bbox, like Mirone's in_map_region), send.
	auto sendSeismicity = [s, visibleRegion, ensureSeisBase](const QString &params) {
		if (!ensureSeisBase()) return;
		// Paint the base map (just promoted, or already there) BEFORE the blocking catalog fetch,
		// so the user sees the world map right away instead of a frozen window. Then show a busy
		// (indeterminate) progress dialog for the fetch itself, which can take a while on the first
		// call of the session (GMT.seismicity/gmtisf/gmtread JIT compilation) or over a slow link.
		if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		if (s->win) s->win->statusBar()->showMessage("Seismicity: fetching catalog…  (first run this session also compiles; please wait)");
		QApplication::processEvents();
		double W = s->x0, E = s->x1, So = s->y0, No = s->y1;
		visibleRegion(W, E, So, No);
		const QString p = params + QString("\nregion=%1/%2/%3/%4")
			.arg(W, 0, 'f', 6).arg(E, 0, 'f', 6).arg(So, 0, 'f', 6).arg(No, 0, 'f', 6);
		showBusyDialog("Seismicity");             // indeterminate busy bar for the blocking fetch
		g_juliaSeismicity(s, p.toUtf8().constData());
		closeBusyDialog();
		// The freshly-plotted solid3D event spheres are meant to show unconditionally in flat-2D (map
		// pins, see applyStacking), but the FIRST time they land in a 2D scene most render invisible;
		// a manual 2D->3D->2D round-trip through sceneSetFlat2D reliably fixes it (confirmed). Rather
		// than chase the exact VTK state that trip resets, just DO that round-trip here automatically
		// so seismicity always comes up fully visible in 2D without the user having to know the trick.
		if (s->flat2d) { sceneSetFlat2D(s, false); sceneSetFlat2D(s, true); }
	};
	// Open the Plot seismicity dialog (Seismology > "Seismicity…" and, with builtin=true,
	// "Global seismicity (1990-2009)" = the shipped data/quakes.dat — same dialog, same Julia
	// pipeline).
	auto openSeismicity = [win, s, sendSeismicity](bool builtin) {
		if (!g_juliaSeismicity) {
			if (s->win) s->win->statusBar()->showMessage("Seismicity: callback not registered", 3000);
			return;
		}
		PlotSeismicityDialog dlg(win, builtin);
		if (dlg.exec() != QDialog::Accepted || dlg.params.isEmpty()) return;
		sendSeismicity(dlg.params);
	};
	// A leaf that fetches a GSHHG feature for the current view: compute the visible region, hand
	// "<kind>/<res>/W/E/S/N" to Julia, which calls GMT.coast(R=…, D=res, M=true) and adds the lines.
	auto geoPlot = [s, visibleRegion](const QString &kind, const char *res) {
		return [s, kind, res, visibleRegion]() {
			if (!g_juliaGeo) {
				if (s->win) s->win->statusBar()->showMessage("Geography: callback not registered", 3000);
				return;
			}
			double W, E, S, N;
			if (!visibleRegion(W, E, S, N)) return;
			// Trailing field = Preferences "Coastlines color" (Black|White) for the line features
			// (coast/borders/rivers); point datasets ignore it and keep their own symbol colours.
			const QString req = QString("%1/%2/%3/%4/%5/%6/%7").arg(kind).arg(res)
				.arg(W, 0, 'f', 6).arg(E, 0, 'f', 6).arg(S, 0, 'f', 6).arg(N, 0, 'f', 6)
				.arg(prefCoastColor());
			g_juliaGeo(s, req.toUtf8().constData());
		};
	};
	// GSHHG features come at 4 resolutions (low/intermediate/high/full) — one submenu does all four.
	// `kind` non-empty wires the leaves to the real geography callback; empty -> the TODO stub.
	auto addResMenu = [&](QMenu *parent, const QString &label, const QString &kind = QString()) {
		QMenu *m = parent->addMenu(label);
		auto leaf = [&](const char *txt, const char *word, const char *res) {
			if (kind.isEmpty()) m->addAction(txt, geoTODO(label + " (" + word + ")"));
			else                m->addAction(txt, geoPlot(kind, res));
		};
		leaf("Low resolution",          "low",          "l");
		leaf("Intermediate resolution", "intermediate", "i");
		leaf("High resolution",         "high",         "h");
		leaf("Full resolution",         "full",         "f");
		leaf("Automatic resolution",    "auto",         "a");
		return m;
	};

	QMenu *mGeo = win->menuBar()->addMenu("&Geography");
	s->geoMenu = mGeo;                              // gmtvtk_set_crs enables it once the data has a CRS
	mGeo->menuAction()->setEnabled(false);         // disabled until a referencing system is known
	addResMenu(mGeo, "Plot coastline", "coast");

	QMenu *mPB = mGeo->addMenu("Plot political boundaries");
	addResMenu(mPB, "National boundaries");
	addResMenu(mPB, "State boundaries (US)");
	addResMenu(mPB, "All boundaries");

	QMenu *mRiv = mGeo->addMenu("Plot rivers");
	addResMenu(mRiv, "Permanent major rivers");
	addResMenu(mRiv, "Additional major rivers");
	addResMenu(mRiv, "Additional rivers");
	addResMenu(mRiv, "Intermittent rivers - major");
	addResMenu(mRiv, "Intermittent rivers - additional");
	addResMenu(mRiv, "Intermittent rivers - minor");
	addResMenu(mRiv, "All rivers and canals");
	addResMenu(mRiv, "All permanent rivers");
	addResMenu(mRiv, "All intermittent rivers");

	mGeo->addSeparator();
	mGeo->addAction("Global seismicity (1990-2009)", [openSeismicity]() { openSeismicity(true); });
	mGeo->addAction("Hotspot locations",             geoTODO("Hotspot locations"));
	mGeo->addAction("Magnetic isochrons",            geoTODO("Magnetic isochrons"));
	mGeo->addAction("Volcanoes",                     geoPlot("volcano", ""));
	mGeo->addAction("Meteorite impacts",             geoPlot("meteorite", ""));
	mGeo->addAction("Hydrothermal sites",            geoPlot("hydro", ""));
	mGeo->addAction("Tide Stations",                 geoTODO("Tide Stations"));
	mGeo->addAction("Tides (download)",              geoPlot("tides", ""));
	mGeo->addAction("Earth Tides", [s, visibleRegion]() {
		double W = -180, E = 180, S = -90, N = 90;          // whole-earth fallback if no view region
		visibleRegion(W, E, S, N);
		showEarthTidesDialog(s, W, E, S, N);
	});
	mGeo->addAction("Fracture Zones",                geoTODO("Fracture Zones"));
	mGeo->addAction("Plate boundaries",              geoTODO("Plate boundaries"));

	QMenu *mCit = mGeo->addMenu("Cities");
	mCit->addAction("Major cities", geoTODO("Major cities"));
	mCit->addAction("Other cities", geoTODO("Other cities"));

	QMenu *mODP = mGeo->addMenu("DSDP/ODP/IODP sites");
	mODP->addAction("DSDP",          geoTODO("DSDP"));
	mODP->addAction("ODP",           geoTODO("ODP"));
	mODP->addAction("IODP",          geoTODO("IODP"));
	mODP->addAction("DSDP+ODP+IODP", geoTODO("DSDP+ODP+IODP"));

	// --- Geophysics: a switchable discipline group (native rotating menu) -----------------------
	// A stock top-level menu (always visible, unlike a widget jammed in the bar). Its title shows a
	// ▾ to hint it is a switcher. Opening it lists the disciplines (Tsunamis / Seismology); picking
	// one rotates the menu title to that discipline and repopulates it with that discipline's items
	// (plus a "‹ Disciplines" entry to switch back), then re-opens so the content shows at once. The
	// menu is rebuilt in place via three mutually-recursive std::functions (heap, window-lifetime).
	QMenu *mGphy = win->menuBar()->addMenu("Geophysics ▾");

	// Elastic deformation submenu — built once and re-attached on each Seismology rebuild (parented
	// to win so the menu's clear() does not delete it). CRS gate relaxed for now: many geographic
	// grids carry no explicit referencing system yet are valid inputs, so the submenu stays enabled
	// (the Draw Fault tool still guards the empty launcher).
	QMenu *mElastic = new QMenu("Elastic deformation", win);
	s->elasticMenu = mElastic;

	// Draw Fault — a draw-mode tool (a two-point line, like the Line family member) routed through
	// polygonToolToggled with the SH_Fault kind. The finished fault carries isFault, so its Scene
	// Objects context menu leads with "Vertical elastic deformation". Joined to the shared exclusive
	// draw-tool group (s->shapeActs) so it untoggles the toolbar shape tools and vice-versa.
	QAction *actDrawFault = mElastic->addAction("Draw Fault");
	actDrawFault->setCheckable(true);
	actDrawFault->setToolTip("Draw a fault line: click the start point, then the end (double-click ends it). "
	                         "Its properties hold the Vertical elastic deformation dialog.");
	QObject::connect(actDrawFault, &QAction::toggled,
		[s, actDrawFault](bool on) { polygonToolToggled(s, actDrawFault, Scene::SH_Fault, on); });
	s->shapeActs.push_back(actDrawFault);

	// Import Trace Fault — port of Mirone's fault_models.m subfault. Pick a sub-fault-format file;
	// Julia (g_juliaImportFault) reads it, rebuilds the surface fault trace of every downdip row and
	// adds each as a Draw-Fault line (gmtvtk_add_fault_h) so it carries the elastic-deformation props.
	mElastic->addAction("Import Trace Fault", [win, s]() {
		if (!g_juliaImportFault) {
			if (s->win) s->win->statusBar()->showMessage("Import Trace Fault: callback not registered", 3000);
			return;
		}
		QString fn = QFileDialog::getOpenFileName(win, "Select sub-fault format file", prefStartDir(),
		                                          "Sub-fault data (*.dat *.DAT);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		g_juliaImportFault(s, fn.toUtf8().constData());
	});
	// Import Model Slip — port of Mirone's fault_models.m subfault, full slip model. Same sub-fault
	// file format as Import Trace Fault, but Julia (g_juliaModelSlip) builds EVERY patch's surface-
	// projection quad and adds them as filled, slip-coloured polygons (gmtvtk_add_slip_patches_h) —
	// no dipping 3-D planes (surface projections only).
	mElastic->addAction("Import Model Slip", [win, s]() {
		if (!g_juliaModelSlip) {
			if (s->win) s->win->statusBar()->showMessage("Import Model Slip: callback not registered", 3000);
			return;
		}
		QString fn = QFileDialog::getOpenFileName(win, "Select sub-fault format file", prefStartDir(),
		                                          "Sub-fault data (*.dat *.DAT);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		g_juliaModelSlip(s, fn.toUtf8().constData());
	});

	// Nested grids — a COPY of the shapes-flyout tool (toolbar keeps its own copy too), reachable
	// straight from the Tsunamis menu. Built ONCE (parented to win, like mElastic) and just re-added
	// to mGphy inside *fTsu below — mGphy->clear() runs on every discipline switch, so an action
	// created fresh inside that lambda would leave stale pointers in s->shapeActs once clear()
	// deletes the old one. Same SH_RectN kind, same shared exclusive draw-tool group (s->shapeActs),
	// routed through the same polygonToolToggled.
	QAction *actNestedGridsTsu = new QAction(makeNestedRectIcon(), "Nested grids", win);
	actNestedGridsTsu->setCheckable(true);
	actNestedGridsTsu->setToolTip("Draw a nested-grids rectangle (constrained dimensions + custom "
	                              "context menus): click one corner, then the opposite corner.");
	QObject::connect(actNestedGridsTsu, &QAction::toggled,
		[s, actNestedGridsTsu](bool on) { polygonToolToggled(s, actNestedGridsTsu, Scene::SH_RectN, on); });
	s->shapeActs.push_back(actNestedGridsTsu);

	auto *fGroup = new std::function<void()>();   // show the discipline chooser
	auto *fTsu   = new std::function<void()>();    // show Tsunamis
	auto *fSeis  = new std::function<void()>();    // show Seismology
	auto *fMag   = new std::function<void()>();    // show Magnetics

	// Re-open the menu at its menubar slot after a rotate (deferred so it runs once the triggering
	// click has finished closing the menu).
	auto reopen = [win, mGphy]() {
		QTimer::singleShot(0, mGphy, [win, mGphy]() {
			QRect r = win->menuBar()->actionGeometry(mGphy->menuAction());
			mGphy->popup(win->menuBar()->mapToGlobal(r.bottomLeft()));
		});
	};

	*fGroup = [mGphy, fTsu, fSeis, fMag]() {
		mGphy->clear();
		mGphy->setTitle("Geophysics ▾");
		mGphy->addAction("Tsunamis",   [fTsu]()  { (*fTsu)(); });
		mGphy->addAction("Seismology", [fSeis]() { (*fSeis)(); });
		mGphy->addAction("Magnetics",  [fMag]()  { (*fMag)(); });
	};
	auto backItem = [mGphy, fGroup, reopen]() {            // "‹ Disciplines" — return to the chooser
		mGphy->addAction("‹ Disciplines", [fGroup, reopen]() { (*fGroup)(); reopen(); });
		mGphy->addSeparator();
	};

	// Tsunamis discipline — currently just NSWING (port of Mirone's swan_options.m).
	*fTsu = [mGphy, win, s, backItem, reopen, actNestedGridsTsu]() {
		mGphy->clear();
		mGphy->setTitle("Tsunamis ▾");
		backItem();
		mGphy->addAction("NSWING tsunami…", [win, s]() {
			// Non-modal: the 3-D view stays interactive while options are picked, and the dialog itself
			// stays open across any number of RUN clicks (see its RUN/Save-files&RUN handlers) — closed
			// only by the user, never by a run. Window flags + explicit NonModal (not just show(), which
			// alone still leaves a parented QDialog feeling tied-above its parent on some platforms) —
			// same combo as the other non-modal dialogs in this file (e.g. IgrfDialog).
			auto *dlg = new NswingDialog(win, s);
			dlg->setAttribute(Qt::WA_DeleteOnClose);
			dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
			dlg->setWindowModality(Qt::NonModal);
			dlg->show();
		});
		mGphy->addAction(actNestedGridsTsu);
		reopen();
	};

	// Magnetics discipline — IGRF Calculator (port of Mirone's igrf_options.m, GMT.jl magref).
	*fMag = [mGphy, win, s, backItem, reopen]() {
		mGphy->clear();
		mGphy->setTitle("Magnetics ▾");
		backItem();
		mGphy->addAction("IGRF", [win, s]() {
			auto *w = new IgrfDialog(win, s);   // self-deletes when its QDialog closes (WA_DeleteOnClose)
			if (w->dlg) w->dlg->show();
		});
		mGphy->addAction("Geomagnetic Bar Code", [win]() {
			auto *w = new MagBarcodeDialog(win);
			if (w->dlg) w->dlg->show();
		});
		reopen();
	};

	// Seismology discipline — Seismicity (earthquakes.m port) + TODO stubs (geoTODO) + the
	// Elastic deformation submenu.
	*fSeis = [mGphy, mElastic, geoTODO, backItem, reopen, s, win, openSeismicity, sendSeismicity, visibleRegion]() {
		mGphy->clear();
		mGphy->setTitle("Seismology ▾");
		backItem();
		mGphy->addAction("Seismicity…", [openSeismicity]() { openSeismicity(false); });
		mGphy->addAction("Focal mechanisms", [win, s, visibleRegion]() {
			if (!g_juliaFocal) {
				if (s->win) s->win->statusBar()->showMessage("Focal mechanisms: callback not registered", 3000);
				return;
			}
			FocalMechanismsDialog dlg(win, s);
			// The .ui ships Mirone's historical "0.8" — a PRINTED-cm Mag-5 beachball size, same
			// unit and same default Mirone itself used. No pre-fill override needed: focal.jl
			// converts cm -> world size itself (mag5size is CENTIMETRES, scaled against the
			// visible region so 0.8 cm never plots a sub-pixel ball).
			if (dlg.exec() != QDialog::Accepted || dlg.params.isEmpty()) return;
			// Derive the region AFTER the dialog closes: on an empty launcher, picking a file
			// inside the dialog already promoted/framed the window to the catalog's own extent
			// (_focal_peek_and_frame, src/focal.jl) — an EARLIER snapshot (on the still-empty
			// placeholder) would crop the read at OK time against a region that no longer
			// describes this window, dropping every real event ("catalog returned no events"
			// despite the file being fine and already read once).
			double W, E, S, N;
			if (!visibleRegion(W, E, S, N)) { W = -180; E = 180; S = -90; N = 90; }
			const QString p = dlg.params + QString("\nregion=%1/%2/%3/%4")
				.arg(W, 0, 'f', 6).arg(E, 0, 'f', 6).arg(S, 0, 'f', 6).arg(N, 0, 'f', 6);
			if (s->win) s->win->statusBar()->showMessage("Focal mechanisms: plotting…");
			showBusyDialog("Focal mechanisms");   // catalog read + per-event geodesic batch can take many seconds
			g_juliaFocal(s, p.toUtf8().constData());
			closeBusyDialog();
		});
		mGphy->addAction("Focal Mechanisms demo", [win, s]() {
			if (s->focalStudioDlg) { s->focalStudioDlg->raise(); s->focalStudioDlg->activateWindow(); return; }
			auto *dlg = new FocalMecaStudioDialog(win, s, 0.0, 90.0, 180.0);
			dlg->setAttribute(Qt::WA_DeleteOnClose);
			s->focalStudioDlg = dlg;
			QObject::connect(dlg, &QObject::destroyed, s->win, [s]{ s->focalStudioDlg = nullptr; });
			dlg->show();
		});
		mGphy->addAction("CMT Catalog (Web download)",    geoTODO("CMT Catalog"));
		mGphy->addAction("Global seismicity (1990-2009)", [openSeismicity]() { openSeismicity(true); });
		// Direct plot, no dialog: format=1 with no bounds -> GMT.seismicity's own defaults
		// (events of the last 30 days, M >= 3) over the visible region.
		mGphy->addAction("USGS recent seismicity", [s, sendSeismicity]() {
			if (!g_juliaSeismicity) {
				if (s->win) s->win->statusBar()->showMessage("Seismicity: callback not registered", 3000);
				return;
			}
			sendSeismicity("format=1");
		});
		mGphy->addAction("Ground motions",                geoTODO("Ground motions"));
		mGphy->addSeparator();
		mGphy->addMenu(mElastic);
		reopen();
	};

	(*fGroup)();   // initial population: the discipline chooser

	// --- Tools menu: open the standalone X,Y plot tool (blank; ready for File>Open or Julia) ----
	QMenu *mTools = win->menuBar()->addMenu("&Tools");
	mTools->addAction("X,Y plot", [] { xyOpenBlankFromHost(); });
	// Tiles Tool (port of Mirone's tiles_tool.m): an interactive world map + refinable web-tile mesh.
	// Pick two diagonal tiles, hit GO -> Julia builds the mosaic (GMT.mosaic) in a new viewer. Non-modal
	// (stays open for repeated picks); WA_DeleteOnClose frees it (and its world pixmap) when closed.
	mTools->addAction("Tiles Tool", [win, s]() {
		QPixmap world;
		if (!g_tilesWorld.isEmpty()) world.load(g_tilesWorld);
		TilesPicker *dlg = new TilesPicker(win, s, world);
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->show();
	});

	// --- GMT menu: helper windows to drive GMT modules (TODO: populate with module tools) ----
	QMenu *mGMT = win->menuBar()->addMenu("&GMT");
	mGMT->addAction("grdsample", [win, s]() {
		GrdsampleDialog dlg(win, s);
		dlg.exec();   // Julia is invoked inside the dialog on Apply
	});

	// --- Grid Tools menu: operations that combine / modify the window's host grid ------------
	// "Transplant 2nd grid" (port of Mirone utils/transplants.m, IMPLANTGRID mode): pick an external
	// grid and implant it into THIS window's grid with a smooth seam. A rectangle is NOT required from
	// here (the rectangle-handle path is the same action wired into a rectangle's context menu, which
	// passes its W/E/S/N as a clip). The submenu's two entries are the -res choice (host vs implant
	// resolution). Both hand params to Julia's _on_transplant via g_juliaEval, like Extract profile.
	QMenu *mGridTools  = win->menuBar()->addMenu("Grid T&ools");
	QMenu *mTransplant = mGridTools->addMenu("Transplant 2nd grid");
	auto runTransplant = [win, s](int res) {
		if (!g_juliaEval) {
			QMessageBox::warning(win, "Transplant 2nd grid", "This computation needs the Julia/GMT host.");
			return;
		}
		const QString fn = QFileDialog::getOpenFileName(win, "Select grid to implant", prefStartDir(),
			"Grids (*.grd *.nc *.tif *.tiff *.img);;All files (*)");
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		const QString cmd = QString("InteractiveGMT._on_transplant(Ptr{Cvoid}(UInt(%1)),raw\"%2\",%3,\"\")")
								.arg((qulonglong)reinterpret_cast<uintptr_t>(s)).arg(fn).arg(res);
		static std::vector<char> buf(1 << 12);
		int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
		if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));   // Julia threw -> Errors tab
	};
	mTransplant->addAction("Keep host resolution",     [runTransplant]() { runTransplant(1); });
	mTransplant->addAction("Adopt implant resolution", [runTransplant]() { runTransplant(0); });

	// Ctrl+Z undoes the last transplant (restores the original grid kept on the Julia side). The undo
	// is also offered on the rectangle's context menu (55_lineprops.cpp).
	QShortcut *scUndoTransplant = new QShortcut(QKeySequence::Undo, win);
	QObject::connect(scUndoTransplant, &QShortcut::activated, [win, s]() {
		if (!g_juliaEval) return;
		const QString cmd = QString("InteractiveGMT._on_transplant_undo(Ptr{Cvoid}(UInt(%1)))")
								.arg((qulonglong)reinterpret_cast<uintptr_t>(s));
		static std::vector<char> buf(1 << 12);
		int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
		if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));
	});

	win->menuBar()->addMenu("&Help")->addAction("&About", actAbout);

	// --- toolbar row (below the menu bar): quick-access buttons (ParaView-style) ------------
	// Open file -> hand the path back to Julia (iview auto-dispatches grid/image/dataset into a
	// NEW window). 2D/3D -> the shared act2D toggle. More buttons can be appended here later.
	QToolBar *tb = win->addToolBar("Main");
	tb->setMovable(false);
	tb->setToolButtonStyle(Qt::ToolButtonIconOnly);   // icon-only toolbar — no text labels on any button
	QAction *actOpen = tb->addAction(win->style()->standardIcon(QStyle::SP_DirOpenIcon), "");  // icon only, no text
	actOpen->setToolTip("Open a grid / image / table file in a new window");
	QObject::connect(actOpen, &QAction::triggered, [s, win]() {
		const QString fn = QFileDialog::getOpenFileName(win, "Open file", prefStartDir());
		if (fn.isEmpty() || !g_juliaEval) return;
		rememberStartDir(fn);
		// Build iview("…") with the path safely quoted (raw string => backslashes are literal).
		std::string cmd = "InteractiveGMT.iview(raw\"" + fn.toStdString() + "\")";
		static std::vector<char> buf(1 << 12);
		int n = g_juliaEval(s, cmd.c_str(), buf.data(), (int)buf.size());
		if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));   // open failed -> Errors tab
	});
	// Info flyout: a stylish 'i' that reports on the active grid/image. Slot click runs the current
	// reporter (grdinfo by default); the 'v' dropdown switches between GMT.grdinfo and GMT.gdalinfo
	// (and runs it). Both go through g_juliaEval -> InteractiveGMT._info_text(fig, mode), whose
	// printed report is shown in a read-only text popup (showInfoText). Mirrors the 2D flyout shape.
	QToolButton *tbInfo = new QToolButton(tb);
	tbInfo->setPopupMode(QToolButton::MenuButtonPopup);   // click icon = run current reporter; click 'v' = pick one
	tbInfo->setToolButtonStyle(Qt::ToolButtonIconOnly);
	tbInfo->setIcon(makeInfoIcon());                      // glyph stays 'i' regardless of mode
	tbInfo->setToolTip("Info: report on the active grid / image (grdinfo / gdalinfo)");
	QMenu *infoMenu = new QMenu(tbInfo);
	QAction *aGrdinfo  = infoMenu->addAction("grdinfo");
	QAction *aGdalinfo = infoMenu->addAction("gdalinfo");
	aGrdinfo->setCheckable(true); aGdalinfo->setCheckable(true);
	aGrdinfo->setToolTip("GMT.grdinfo — grid header / range report");
	aGdalinfo->setToolTip("GMT.gdalinfo — GDAL dataset report");
	auto *infoGroup = new QActionGroup(tbInfo);           // exclusive: exactly one reporter active
	infoGroup->addAction(aGrdinfo); infoGroup->addAction(aGdalinfo);
	aGrdinfo->setChecked(true);                           // default = grdinfo
	tbInfo->setMenu(infoMenu);
	auto runInfo = [s, win, aGdalinfo]() {
		if (!g_juliaEval) { if (s->win) s->win->statusBar()->showMessage("Info: Julia eval not registered", 3000); return; }
		const char *mode = aGdalinfo->isChecked() ? "gdalinfo" : "grdinfo";
		std::string cmd = std::string("InteractiveGMT._info_text(fig, \"") + mode + "\")";
		static std::vector<char> buf(1 << 16);
		int n = g_juliaEval(s, cmd.c_str(), buf.data(), (int)buf.size());
		QString txt = QString::fromUtf8(buf.data(), n < 0 ? -n : n);
		showInfoText(win, QString::fromUtf8(mode), txt);
	};
	QObject::connect(tbInfo, &QToolButton::clicked, runInfo);
	QObject::connect(aGrdinfo,  &QAction::triggered, runInfo);   // picking a reporter also runs it
	QObject::connect(aGdalinfo, &QAction::triggered, runInfo);
	// (tbInfo is added to the toolbar at the very END of the row — see after the 3-D Bodies flyout.)
	// 2D/3D view-mode flyout: a sibling of the shapes / 3-D Bodies families — an icon-only QToolButton
	// whose glyph shows the CURRENT view ("2D" flat map / "3D" perspective) and whose dropdown arrow
	// ('v') lists the two modes. Picking one switches via the shared setFlat2D; the slot's glyph +
	// the active-mode checkmark track act2D's checked state, so EVERY toggle source (this flyout,
	// the View menu, the context menu, the 2D bare-image / grid init in 90_c_api) keeps it in sync.
	// (iGMT opens grid windows in 2D — see the grid init in gmtvtk_view_grid, 90_c_api.cpp.)
	QToolButton *tb2D = new QToolButton(tb);
	tb2D->setPopupMode(QToolButton::MenuButtonPopup);   // click icon = re-apply current mode; click 'v' = pick mode
	tb2D->setToolButtonStyle(Qt::ToolButtonIconOnly);
	QMenu *viewModeMenu = new QMenu(tb2D);              // the dropdown list: 2D / 3D (text only — no glyph,
	QAction *actMode2D = viewModeMenu->addAction("2D"); // the slot below carries the glyph icon)
	QAction *actMode3D = viewModeMenu->addAction("3D");
	actMode2D->setCheckable(true); actMode3D->setCheckable(true);
	actMode2D->setToolTip("Flat 2D map (top-down shaded relief)");
	actMode3D->setToolTip("3D perspective view");
	QObject::connect(actMode2D, &QAction::triggered, [setFlat2D]() { setFlat2D(true);  });
	QObject::connect(actMode3D, &QAction::triggered, [setFlat2D]() { setFlat2D(false); });
	tb2D->setMenu(viewModeMenu);
	tb2D->setToolTip("View mode: flat 2D map / 3D perspective");
	// Slot click toggles 2D<->3D; the 'v' dropdown picks a mode. NOT setDefaultAction (that would tie
	// the button's sunken/checked look to the always-checked menu entry — leaving it permanently
	// highlighted). Drive the glyph icon ourselves; the checked entry just marks the active mode.
	QObject::connect(tb2D, &QToolButton::clicked, actToggle2D);
	auto syncViewMode = [tb2D, actMode2D, actMode3D](bool on) {
		actMode2D->setChecked(on); actMode3D->setChecked(!on);   // dropdown checkmark on the active mode
		tb2D->setIcon(makeViewModeIcon(on));                     // glyph shows the CURRENT mode
	};
	QObject::connect(s->act2D, &QAction::toggled, tb2D, [syncViewMode](bool on){ syncViewMode(on); });
	syncViewMode(s->flat2d);
	tb->addWidget(tb2D);

	tb->addSeparator();

	// Base Map: opens the World Topo Tiles picker (port of Mirone's bg_map.m). A clicked tile's
	// region "W/E/S/N/wrap/name" is handed to Julia (g_juliaBaseMap), which crops data/etopo4.jpg
	// and adds it as a referenced flat image (Julia frames an empty launcher to a 2-D map; a window
	// already showing data just gets the basemap added on top). Sits right BEFORE the polygon tool.
	// Prefer the bundled world icon (data/basemap_icon.png, pushed via gmtvtk_set_basemap_icon);
	// fall back to a hand-painted tile-grid glyph if that path is unset or fails to load.
	QIcon baseMapIcon;
	if (!g_basemapIcon.isEmpty()) {
		QPixmap pm(g_basemapIcon);
		if (!pm.isNull()) baseMapIcon = QIcon(pm);
	}
	if (baseMapIcon.isNull()) {
		QPixmap pm(16, 16); pm.fill(Qt::transparent);
		QPainter p(&pm); p.setPen(QColor(60, 110, 180));
		p.drawRect(1, 3, 13, 9);
		for (int x = 4; x < 14; x += 3) p.drawLine(x, 3, x, 12);    // tile grid columns
		for (int y = 6; y < 12; y += 3) p.drawLine(1, y, 14, y);    // tile grid rows
		baseMapIcon = QIcon(pm);
	}
	QAction *actBaseMap = tb->addAction(baseMapIcon, "");
	actBaseMap->setToolTip("Base Map: pick a world topo tile to load as a referenced image");
	QObject::connect(actBaseMap, &QAction::triggered, [win, s]() {
		QPixmap logo;
		if (!g_basemapLogo.isEmpty()) logo.load(g_basemapLogo);
		BaseMapPicker dlg(win, logo);
		if (dlg.exec() == QDialog::Accepted && !dlg.region.isEmpty() && g_juliaBaseMap)
			g_juliaBaseMap(s, dlg.region.toUtf8().constData());
	});

	// --- draw tools: an Illustrator-style flyout (shapes) + a standalone Text button -----------
	// The four shape tools share ONE toolbar slot (a plain QToolButton in MenuButtonPopup mode): the
	// slot shows the active tool; its native dropdown arrow opens the family flyout. Each tool is
	// checkable QAction routed through polygonToolToggled (85_polygon.cpp), which sets the active
	// ShapeKind, untoggles the others via s->shapeActs, and enters/leaves draw mode — the flyout
	// only changes how four of them are PRESENTED. A drawn shape is a `Polygon` (vertex ring) so all
	// share preview/edit/delete. When all tools are OFF, double-clicking a finished polygon enters
	// vertex-edit mode (square handles).
	//
	//   Polygon  — left-click adds vertices, right-click undoes, double-click closes (>=3).
	//   Polyline — same, but double-click ends an OPEN chain (>=2).
	//   Rectangle/Circle — two clicks (first corner/centre, then opposite corner/edge); a live
	//                      preview trails the cursor between them.
	//   Text     — one click on the scene, then a dialog asks for the string (own button below).
	struct ToolDef { QIcon icon; const char *name; const char *tip; Scene::ShapeKind kind; };
	const ToolDef flyoutTools[] = {
		{ makePolygonIcon(),  "Polygon",   "Draw a polygon: left-click adds vertices, right-click undoes one, "
		                                   "double-click closes it. Double-click a polygon to edit its vertices.", Scene::SH_Polygon  },
		{ makePolylineIcon(), "Polyline",  "Draw a polyline: left-click adds vertices, right-click undoes one, "
		                                   "double-click ends the open line.",                                     Scene::SH_Polyline },
		{ makeLineIcon(),     "Line",      "Draw a straight line: click the start point, then the end point "
		                                   "(later clicks move the end); double-click ends it.",                   Scene::SH_Line     },
		{ makeRectIcon(),     "Rectangle", "Draw a rectangle: click one corner, then the opposite corner.",        Scene::SH_Rect     },
		{ makeNestedRectIcon(), "Nested grids", "Draw a nested-grids rectangle (constrained dimensions + custom "
		                                   "context menus): click one corner, then the opposite corner.",          Scene::SH_RectN    },
		{ makeCircleIcon(),   "Circle",    "Draw a circle: click the centre, then a point on the edge.",           Scene::SH_Circle   },
	};
	QToolButton *flyout = new QToolButton(tb);           // the shared shape slot
	flyout->setPopupMode(QToolButton::MenuButtonPopup);  // click icon = use tool; click arrow = flyout
	flyout->setToolButtonStyle(Qt::ToolButtonIconOnly);
	QMenu *shapeMenu = new QMenu(flyout);                // the dropdown flyout list
	QAction *defaultShape = nullptr;                     // the tool the slot starts on (Line)
	for (const ToolDef& td : flyoutTools) {
		QAction *act = shapeMenu->addAction(td.icon, td.name);   // icon + label (the slot itself stays icon-only)
		act->setCheckable(true);
		act->setToolTip(td.tip);
		const Scene::ShapeKind kind = td.kind;
		QObject::connect(act, &QAction::toggled, [s, act, kind](bool on){ polygonToolToggled(s, act, kind, on); });
		s->shapeActs.push_back(act);
		if (kind == Scene::SH_Line) defaultShape = act;
	}
	flyout->setMenu(shapeMenu);
	// Start on Line (icon + tooltip mirror it); fall back to the first entry if Line ever goes away.
	flyout->setDefaultAction(defaultShape ? defaultShape : shapeMenu->actions().first());
	// Picking a sibling from the flyout makes it the slot's current tool (Illustrator behaviour): the
	// chosen action toggles on (its connection enters draw mode) and becomes the button's default.
	QObject::connect(shapeMenu, &QMenu::triggered, flyout, [flyout](QAction *a){ flyout->setDefaultAction(a); });
	tb->addWidget(flyout);

	// --- Symbols flyout: circle/square/star, placed by ONE click (not drag-to-size) -----------
	// Same ToolDef/QToolButton/QMenu machinery as the shapes flyout above, routed through the SAME
	// polygonToolToggled (so it shares s->shapeActs mutual exclusion with every other draw tool). A
	// placed symbol is a NATIVE `SymbolLayer` (addSymbols, oneShot=true, 50_scene.cpp) — the SAME
	// screen-constant-size vtkGlyph3D system volcanoes/seismicity use, NOT a drawn Polygon ring (a
	// baked-vertex ring visibly deforms on geographic maps where x/y scale unequally; a glyph never
	// does). Double-click then drag (polygonHandleDblClick/Move, 85_polygon.cpp) moves the layer's
	// one point; size/fill/edge-color/width are already generic via the existing symbolLayerMenu.
	struct SymToolDef { QIcon icon; const char *name; const char *tip; Scene::ShapeKind kind; };
	const SymToolDef symTools[] = {
		{ makeSymCircleIcon(), "Circle", "Place a circle symbol: click to drop it. Double-click then drag "
		                                 "to move it; right-click for size/fill/outline.", Scene::SH_SymCircle },
		{ makeSymSquareIcon(), "Square", "Place a square symbol: click to drop it. Double-click then drag "
		                                 "to move it; right-click for size/fill/outline.", Scene::SH_SymSquare },
		{ makeSymStarIcon(),   "Star",   "Place a star symbol: click to drop it. Double-click then drag "
		                                 "to move it; right-click for size/fill/outline.",   Scene::SH_SymStar },
	};
	QToolButton *symFlyout = new QToolButton(tb);            // the shared symbol slot
	symFlyout->setPopupMode(QToolButton::MenuButtonPopup);
	symFlyout->setToolButtonStyle(Qt::ToolButtonIconOnly);
	QMenu *symMenu = new QMenu(symFlyout);
	QAction *defaultSym = nullptr;
	for (const SymToolDef& td : symTools) {
		QAction *act = symMenu->addAction(td.icon, td.name);
		act->setCheckable(true);
		act->setToolTip(td.tip);
		const Scene::ShapeKind kind = td.kind;
		QObject::connect(act, &QAction::toggled, [s, act, kind](bool on){ polygonToolToggled(s, act, kind, on); });
		s->shapeActs.push_back(act);
		if (!defaultSym) defaultSym = act;
	}
	symFlyout->setMenu(symMenu);
	symFlyout->setDefaultAction(defaultSym);
	QObject::connect(symMenu, &QMenu::triggered, symFlyout, [symFlyout](QAction *a){ symFlyout->setDefaultAction(a); });
	tb->addWidget(symFlyout);

	// Text — its own icon-only toggle (not a "drawn shape" family member, but shares the exclusive
	// s->shapeActs group so selecting it untoggles the active shape tool and vice-versa).
	QAction *actText = tb->addAction(makeTextIcon(), "");
	actText->setCheckable(true);
	actText->setToolTip("Place a text label: click a point on the scene, then type the text.");
	QObject::connect(actText, &QAction::toggled, [s, actText](bool on){ polygonToolToggled(s, actText, Scene::SH_Text, on); });
	s->shapeActs.push_back(actText);

	// --- 3-D Bodies: a flyout that builds GMT.jl solids (cube/sphere/torus/cylinder/…) ----------
	// Sibling to the shapes flyout, but every entry is a ONE-SHOT action (NOT a draw-mode toggle):
	// clicking it hands the solid's GMT name to Julia (g_juliaSolid, 30_app.cpp), which builds the
	// named GMTfv via the SOLIDS catalogue (fv.jl) and opens it in its own FV viewer window (where the
	// existing view_fv path already gives it full Scene Objects properties). Closed primitives first,
	// then the parametric generators (revolve/loft/extrude render demo curves). Slot click = build the
	// current body; the dropdown arrow opens the flyout; picking a sibling makes it the new default.
	struct BodyDef { QIcon icon; const char *name; const char *label; const char *tip; };
	const BodyDef bodyTools[] = {
		{ makeCubeIcon(),       "cube",         "Cube",         "Create a cube solid."                        },
		{ makeSphereIcon(),     "sphere",       "Sphere",       "Create a sphere solid."                      },
		{ makeTorusIcon(),      "torus",        "Torus",        "Create a torus (donut) solid."               },
		{ makeCylinderIcon(),   "cylinder",     "Cylinder",     "Create a cylinder solid."                    },
		{ makePolyhedronIcon(), "tetrahedron",  "Tetrahedron",  "Create a tetrahedron solid."                 },
		{ makePolyhedronIcon(), "octahedron",   "Octahedron",   "Create an octahedron solid."                 },
		{ makePolyhedronIcon(), "dodecahedron", "Dodecahedron", "Create a dodecahedron solid."                },
		{ makePolyhedronIcon(), "icosahedron",  "Icosahedron",  "Create an icosahedron solid."                },
		{ makePolyhedronIcon(), "revolve",      "Revolve",      "Surface of revolution (demo profile curve)." },
		{ makePolyhedronIcon(), "loft",         "Loft",         "Loft between two curves (demo curves)."      },
		{ makePolyhedronIcon(), "extrude",      "Extrude",      "Extrude a 2-D shape (demo star outline)."    },
	};
	QToolButton *bodyFlyout = new QToolButton(tb);
	bodyFlyout->setPopupMode(QToolButton::MenuButtonPopup);
	bodyFlyout->setToolButtonStyle(Qt::ToolButtonIconOnly);
	bodyFlyout->setToolTip("3-D Bodies: build a GMT solid (cube, sphere, torus, cylinder, …)");
	QMenu *bodyMenu = new QMenu(bodyFlyout);
	for (const BodyDef& bd : bodyTools) {
		QAction *act = bodyMenu->addAction(bd.icon, bd.label);
		act->setToolTip(bd.tip);
		const QByteArray nm = bd.name;                          // capture the solid name by value
		QObject::connect(act, &QAction::triggered, [s, nm]() {
			if (g_juliaSolid) g_juliaSolid(s, nm.constData());   // nullptr -> not wired; silently ignore
		});
	}
	bodyFlyout->setMenu(bodyMenu);
	bodyFlyout->setDefaultAction(bodyMenu->actions().first());   // slot starts on Cube (icon + tooltip mirror it)
	QObject::connect(bodyMenu, &QMenu::triggered, bodyFlyout, [bodyFlyout](QAction *a){ bodyFlyout->setDefaultAction(a); });
	tb->addSeparator();
	tb->addWidget(bodyFlyout);

	// Info flyout sits LAST on the toolbar row (built earlier, added here so it's the rightmost item).
	tb->addSeparator();
	tb->addWidget(tbInfo);

	// --- native right-click context menu over the 3-D view ------------------
	widget->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(widget, &QWidget::customContextMenuRequested,
		[=](const QPoint& pos) {
			// Ctrl+right is the rubber-band select gesture on a point cloud, not a menu
			// request — swallow it (rbConsume is set by the selection release handler).
			if (s->rbEnabled && (s->rbConsume ||
				(QApplication::keyboardModifiers() & Qt::ControlModifier))) {
				s->rbConsume = false;
				return;
			}
			// While drawing a polygon, right-click means "remove last vertex" (handled by the
			// polygon tool's VTK observer) — never pop the view context menu.
			if (s->polyMode && s->polyDrawing)
				return;
			// If an overlay (GMTdataset line/point) is under the cursor, select it and pop ITS
			// per-element menu. VTK display coords are bottom-up device px; Qt QPoint is top-down.
			{
				const double dpr = widget->devicePixelRatioF();
				const int    Hpx = widget->renderWindow()->GetSize()[1];
				const int    px  = int(pos.x() * dpr), py = int(Hpx - pos.y() * dpr);
				if (profileHitAt(s, px, py)) {   // profile line sits on top -> its menu wins
					popupProfileMenu(s, widget->mapToGlobal(pos));
					return;
				}
				const int pgi = polyHitPolygon(s, px, py, 8.0);   // a drawn polygon under the cursor?
				if (pgi >= 0) {
					popupLineObjectMenu(s, LineRef{ LK_Polygon, s->polys[pgi].line },
										QString::fromStdString(s->polys[pgi].name), widget->mapToGlobal(pos));
					return;
				}
				const int tHit = polyHitText(s, px, py, 14.0);    // a text label under the cursor?
				if (tHit >= 0) {
					// polyHitText only ever returns ungrouped labels (plain vtkTextActor3D) — see its
					// own comment (85_polygon.cpp) for why grouped/billboard labels are excluded there.
					textLabelMenu(s, vtkTextActor3D::SafeDownCast(s->texts[tHit].actor), widget->mapToGlobal(pos));
					return;
				}
				if (vtkActor *sym = pickSymbolAt(s, px, py)) {    // symbol layers sit on top
					symbolLayerMenu(s, sym, widget->mapToGlobal(pos));
					return;
				}
				int ovMode = 1;
				vtkActor *ov = pickOverlayAt(s, px, py, ovMode);
				if (ov) {
					popupOverlayMenu(s, ov, ovMode, widget->mapToGlobal(pos));
					return;
				}
			}
			QMenu m(win);
			m.addAction("Reset Camera", actReset);
			QAction *ca = m.addAction("Cube Axes", actToggleAxes);
			ca->setCheckable(true); ca->setChecked(s->axes->GetVisibility());
			if (s->bar) {                    // no Color Bar entry for bare images
				QAction *cb = m.addAction("Color Bar", actToggleBar);
				cb->setCheckable(true); cb->setChecked(colorbarVisible(s));
			}
			QAction *cg = m.addAction("Gizmo", actToggleGizmo);
			cg->setCheckable(true); cg->setChecked(s->giz && s->giz->visible);
			QAction *c2 = m.addAction("2D", actToggle2D);
			c2->setCheckable(true); c2->setChecked(s->flat2d);
			m.addSeparator();
			m.addAction("Vertical Exaggeration…", actVE);
			m.addAction("Save Screenshot…", actShot);
			m.exec(widget->mapToGlobal(pos));
		});

	// --- Shading control dock (live PBR / IBL / post-pass tuning) -----------
	// Every control writes a Scene field and re-runs applyShading(); this is the
	// knob set for matching F3D's look without rebuilding (and lets the look be
	// tuned on a real display, which the headless screenshot path can't show).
	QDockWidget *dock = new QDockWidget("Shading", win);              // GRAPHICAL ELEMENT: the "Shading" dock — foldable side panel
	dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea); // user may drag-fold it to the LEFT or RIGHT window edge
	dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable); // foldable: drag/float/close
	QWidget *panel = new QWidget(dock);                              // container widget that holds all the shading controls
	QFormLayout *form = new QFormLayout(panel);                      // label-on-left / control-on-right rows inside the dock

	// Live tooltip for a slider: maps the raw slider position to the parameter's REAL range
	// [rmin,rmax] and shows "name: value unit  [rmin … rmax]". Updated on every change AND
	// popped at the cursor while dragging so the value is visible without hovering first.
	auto wireTip = [](QSlider *sl, QString name, double rmin, double rmax, QString unit, int dec) {
		auto fmt = [=](int v) {
			double t    = double(v - sl->minimum()) / double(sl->maximum() - sl->minimum());
			double real = rmin + t * (rmax - rmin);
			QString u = unit.isEmpty() ? "" : " " + unit;
			return QString("%1: %2%3   [%4 … %5%3]")
				.arg(name).arg(real, 0, 'f', dec).arg(u)
				.arg(rmin, 0, 'f', dec).arg(rmax, 0, 'f', dec);
		};
		sl->setToolTip(fmt(sl->value()));
		QObject::connect(sl, &QSlider::valueChanged, sl, [sl, fmt](int v) {
			sl->setToolTip(fmt(v));
			QToolTip::showText(QCursor::pos(), fmt(v), sl);
		});
	};

	// Drape blend: actor opacity of the image overlay, so the picture and the PBR-shaded
	// relief can be combined. 100% = opaque image, 0% = image faded out (surface shows
	// through). Only meaningful with a draped image, so the row exists ONLY when s->drape does.
	if (s->drape) {
		QSlider *slDrape = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Drape blend" slider — image-overlay opacity
		slDrape->setRange(0, 100); slDrape->setValue(int(s->drape->GetProperty()->GetOpacity() * 100));
		QObject::connect(slDrape, &QSlider::valueChanged, [s](int v){
			s->drape->GetProperty()->SetOpacity(v / 100.0);
			s->widget->renderWindow()->Render();
		});
		form->addRow("Drape blend", slDrape);
		wireTip(slDrape, "Drape blend", 0, 100, "%", 0);
	}

	QSlider *slRough = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Roughness" slider — PBR surface roughness
	slRough->setRange(0, 100); slRough->setValue(int(s->roughness * 100));
	QObject::connect(slRough, &QSlider::valueChanged, [s](int v){ s->roughness = v / 100.0; applyShading(s); });
	form->addRow("Roughness", slRough);
	wireTip(slRough, "Roughness", 0.0, 1.0, "", 2);

	QSlider *slMetal = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Metallic" slider — PBR metalness
	slMetal->setRange(0, 100); slMetal->setValue(int(s->metallic * 100));
	QObject::connect(slMetal, &QSlider::valueChanged, [s](int v){ s->metallic = v / 100.0; applyShading(s); });
	form->addRow("Metallic", slMetal);
	wireTip(slMetal, "Metallic", 0.0, 1.0, "", 2);

	QSlider *slLight = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Light" slider — key (sun) light intensity
	slLight->setRange(0, 300); slLight->setValue(int(s->lightIntensity * 100));
	QObject::connect(slLight, &QSlider::valueChanged, [s](int v){ s->lightIntensity = v / 100.0; applyShading(s); });
	form->addRow("Light", slLight);
	wireTip(slLight, "Light", 0.0, 3.0, "", 2);

	QSlider *slAz = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Sun azimuth" slider — key-light azimuth (deg from north, CW)
	slAz->setRange(0, 360); slAz->setValue(int(s->lightAz));
	QObject::connect(slAz, &QSlider::valueChanged, [s](int v){ s->lightAz = v; applyShading(s); });
	form->addRow("Sun azimuth", slAz);
	wireTip(slAz, "Sun azimuth", 0, 360, "deg", 0);

	QSlider *slEl = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Sun elevation" slider — key-light elevation above horizon
	slEl->setRange(0, 90); slEl->setValue(int(s->lightEl));
	QObject::connect(slEl, &QSlider::valueChanged, [s](int v){ s->lightEl = v; applyShading(s); });
	form->addRow("Sun elevation", slEl);
	wireTip(slEl, "Sun elevation", 0, 90, "deg", 0);

	QSlider *slFill = new QSlider(Qt::Horizontal, panel); // GRAPHICAL ELEMENT: "Fill" slider — fill-light intensity (shadow-side lift)
	slFill->setRange(0, 100); slFill->setValue(int(s->fillIntensity * 100));
	QObject::connect(slFill, &QSlider::valueChanged, [s](int v){ s->fillIntensity = v / 100.0; applyShading(s); });
	form->addRow("Fill", slFill);
	wireTip(slFill, "Fill", 0.0, 1.0, "", 2);

	QSlider *slEnv = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Env (IBL)" slider — image-based-light intensity
	slEnv->setRange(0, 300); slEnv->setValue(int(s->envIntensity * 100));
	QObject::connect(slEnv, &QSlider::valueChanged, [s](int v){ s->envIntensity = v / 100.0; applyShading(s); });
	form->addRow("Env (IBL)", slEnv);
	wireTip(slEnv, "Env (IBL)", 0.0, 3.0, "", 2);

	const double rad0 = (s->ssaoRadius > 0.0) ? s->ssaoRadius : 0.5;   // slider = 0..200% of seed
	QSlider *slSSAO = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "SSAO radius" slider — ambient-occlusion sampling radius
	slSSAO->setRange(0, 200); slSSAO->setValue(100);
	QObject::connect(slSSAO, &QSlider::valueChanged, [s, rad0](int v){ s->ssaoRadius = rad0 * v / 100.0; applyShading(s); });
	form->addRow("SSAO radius", slSSAO);
	wireTip(slSSAO, "SSAO radius", 0, 200, "%", 0);

	QCheckBox *cbIBL = new QCheckBox(panel); cbIBL->setChecked(s->useIBL);   // GRAPHICAL ELEMENT: "Image-based light" checkbox — toggles IBL
	QObject::connect(cbIBL, &QCheckBox::toggled, [s](bool b){ s->useIBL = b; applyShading(s); });
	form->addRow("Image-based light", cbIBL);

	QCheckBox *cbSSAO = new QCheckBox(panel); cbSSAO->setChecked(s->useSSAO); // GRAPHICAL ELEMENT: "Ambient occlusion" checkbox — toggles SSAO pass
	QObject::connect(cbSSAO, &QCheckBox::toggled, [s](bool b){ s->useSSAO = b; applyShading(s); });
	form->addRow("Ambient occlusion", cbSSAO);

	QCheckBox *cbTone = new QCheckBox(panel); cbTone->setChecked(s->useTone); // GRAPHICAL ELEMENT: "Tone mapping" checkbox — toggles tone-map pass
	QObject::connect(cbTone, &QCheckBox::toggled, [s](bool b){ s->useTone = b; applyShading(s); });
	form->addRow("Tone mapping", cbTone);

	QCheckBox *cbFXAA = new QCheckBox(panel); cbFXAA->setChecked(s->useFXAA); // GRAPHICAL ELEMENT: "FXAA" checkbox — toggles anti-alias post-pass
	QObject::connect(cbFXAA, &QCheckBox::toggled, [s](bool b){ s->useFXAA = b; applyShading(s); });
	form->addRow("FXAA", cbFXAA);

	// Three ALTERNATIVE relief looks — Cast shadows (lit self-shadowing), Hillshade/Lambert
	// (VE-corrected mesh-normal shade) and Hillshade/grdimage (VE-independent z-gradient + GMT
	// HSV illuminate) — are MUTUALLY EXCLUSIVE but all three may be off. Each toggled handler:
	// when turned ON, uncheck the other two (QSignalBlocker stops their handlers re-firing), then
	// re-derive ALL Scene flags from the live checkbox states (so an off-handler never wrongly
	// clears a flag the just-checked box set). hillGrd selects the Lambert vs grdimage style.
	QCheckBox *cbShadow = new QCheckBox(panel); cbShadow->setChecked(s->useShadows);                  // GRAPHICAL ELEMENT: "Cast shadows" checkbox
	QCheckBox *cbHillL  = new QCheckBox(panel); cbHillL->setChecked(s->useHillshade && !s->hillGrd);  // GRAPHICAL ELEMENT: "Hillshade (Lambert)" checkbox
	QCheckBox *cbHillG  = new QCheckBox(panel); cbHillG->setChecked(s->useHillshade &&  s->hillGrd);  // GRAPHICAL ELEMENT: "Hillshade (grdimage)" checkbox
	auto syncShade = [s, cbShadow, cbHillL, cbHillG]() {
		s->useShadows   = cbShadow->isChecked();
		s->useHillshade = cbHillL->isChecked() || cbHillG->isChecked();
		s->hillGrd      = cbHillG->isChecked();
		applyShading(s);
	};
	QObject::connect(cbShadow, &QCheckBox::toggled, [=](bool b){
		if (b) { QSignalBlocker bl(cbHillL), bg(cbHillG); cbHillL->setChecked(false); cbHillG->setChecked(false); }
		syncShade();
	});
	form->addRow("Cast shadows", cbShadow);

	QObject::connect(cbHillL, &QCheckBox::toggled, [=](bool b){
		if (b) { QSignalBlocker bs(cbShadow), bg(cbHillG); cbShadow->setChecked(false); cbHillG->setChecked(false); }
		syncShade();
	});
	form->addRow("Hillshade (Lambert)", cbHillL);

	QObject::connect(cbHillG, &QCheckBox::toggled, [=](bool b){
		if (b) { QSignalBlocker bs(cbShadow), bl(cbHillL); cbShadow->setChecked(false); cbHillL->setChecked(false); }
		syncShade();
	});
	form->addRow("Hillshade (grdimage)", cbHillG);

	panel->setLayout(form);
	dock->setWidget(panel);                                  // mount the controls panel into the Shading dock
	win->addDockWidget(Qt::RightDockWidgetArea, dock);       // dock the Shading panel to the RIGHT edge by default
	// Shading only bites on a shaded surface / 3-D body. A bare image (imageOnly) or a
	// Verts-only point cloud has nothing to light, so FOLD the dock by default there; the
	// View menu action still un-folds it on demand.
	const bool hasShadedBody = !imageOnly && !pointCloud;
	s->shadeDock = dock;                                     // keep it so a promoted launcher can re-show + fold it
	dock->setVisible(hasShadedBody);                         // GRAPHICAL ELEMENT: Shading dock initial fold state
	// GRAPHICAL ELEMENT: View menu "Shading Panel" item — folds/un-folds the Shading dock
	QAction *aShade = mView->addAction("Shading &Panel", [dock](){ dock->setVisible(!dock->isVisible()); });
	aShade->setCheckable(true); aShade->setChecked(hasShadedBody);   // menu checkmark tracks the dock's visibility

	// --- Scene Objects dock: Fledermaus-style show/hide checkbox per element -
	// One checkbox for the surface, the image drape (if any), and every line/point
	// overlay. rebuildSceneObjects() repopulates it whenever an overlay is added.
	QDockWidget *objDock = new QDockWidget("Scene Objects", win); // GRAPHICAL ELEMENT: the "Scene Objects" dock — foldable side panel
	objDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea); // user may drag-fold it to the LEFT or RIGHT window edge
	objDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable); // foldable: drag/float/close
	s->objPanel = new QWidget(objDock);                      // container widget; rebuildSceneObjects() fills it with per-object checkboxes
	s->objDock  = objDock;                                   // keep a handle so the first nested rect can re-show it
	objDock->setWidget(s->objPanel);                         // mount that container into the Scene Objects dock
	win->addDockWidget(Qt::LeftDockWidgetArea, objDock);     // dock the Scene Objects panel to the LEFT edge by default
	if (objname && objname[0])
		s->surfName = objname;                // named solid -> checkbox shows the solid name
	rebuildSceneObjects(s);                                  // populate the per-object show/hide checkboxes now
	// GRAPHICAL ELEMENT: View menu "Scene Objects Panel" item — folds/un-folds the Scene Objects dock
	QAction *aObjs = mView->addAction("Scene &Objects Panel", [objDock](){ objDock->setVisible(!objDock->isVisible()); });
	aObjs->setCheckable(true); aObjs->setChecked(true);      // menu checkmark tracks the dock's visibility

	// --- FOLD button on the side docks --------------------------------------
	// Qt has no built-in "collapse" affordance, so REPLACE each side dock's default title bar
	// with a FoldTitleBar. Folding hides the body AND shrinks the dock to a thin vertical strip
	// (resizeDocks), so the collapsed dock no longer leaves its full open width as dead space;
	// the strip carries the title rotated 90° down the window edge. Un-folding restores the body
	// and the remembered open width. This is the fold control Qt's default title bar never gave us.
	auto makeFoldable = [win](QDockWidget *d, QWidget *body, const QString& titleText) -> FoldTitleBar* {
		FoldTitleBar *bar = new FoldTitleBar(titleText, d);  // GRAPHICAL ELEMENT: dock title bar = fold toggle
		d->setTitleBarWidget(bar);                        // swap Qt's default title bar for our fold strip
		bar->onClick = [win, d, body, bar]() {
			const bool fold = body->isVisible();          // visible now -> fold it away
			if (fold) bar->openWidth = d->width();        // remember the open width to restore later
			body->setVisible(!fold);                      // hide body -> dock can shrink to the strip
			bar->folded = fold;
			bar->updateGeometry();                        // sizeHint flips orientation
			bar->update();
			const int w = fold ? bar->sizeHint().width()
							   : (bar->openWidth > 0 ? bar->openWidth : 220);
			win->resizeDocks({d}, {w}, Qt::Horizontal);   // collapse to / expand from the strip width
		};
		return bar;
	};
	s->shadeFoldBar = makeFoldable(dock, panel, "Shading");   // keep the bar so the Surface row can fold/un-fold it
	s->objFoldBar = makeFoldable(objDock, s->objPanel, "Scene Objects");  // keep the bar so an empty launcher can start folded

	// A grid opens with the Shading dock FOLDED to the side strip (it stays one click away on the
	// Surface row / View menu). Pre-fold BEFORE the first paint so it never flashes open; the
	// strip-width resizeDocks is deferred to just after win->show() (only bites once laid out).
	if (hasShadedBody && s->shadeFoldBar) {
		s->shadeFoldBar->openWidth = 240;       // width to restore when un-folded
		panel->setVisible(false);               // hide body -> dock shrinks to the strip
		s->shadeFoldBar->folded = true;
		s->shadeFoldBar->updateGeometry();      // sizeHint flips to the thin vertical strip
	}

	// --- Bottom tabbed panel: Profile / Julia Console / Data Viewer ----------
	// ONE dock holds a QTabWidget. A "Hide" button in the tab-bar corner collapses the panel
	// BODY down to just the tab strip (so the central 3-D view extends) and toggles to "Show".
	QDockWidget *bottomDock = new QDockWidget("Panels", win);
	bottomDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
	QTabWidget *tabs = new QTabWidget(bottomDock);
	tabs->setDocumentMode(true);
	bottomDock->setWidget(tabs);
	win->addDockWidget(Qt::BottomDockWidgetArea, bottomDock);
	s->bottomDock = bottomDock;
	s->bottomTabs = tabs;

	// Tab 0 — Profile: 2D (distance, elevation) graph. Ctrl+left-drag a line on the surface
	// fills it (the 3D drape line + this panel update live; GMTF3D / Fledermaus profile track).
	s->prof = new ProfilePanel(tabs);
	tabs->addTab(s->prof, "Profile");

	// Tab 1 — Julia console: the viewer runs in-process in Julia, so a typed command is handed
	// straight back to Julia (g_juliaEval) and eval'd in Main. The callback binds `fig` to THIS
	// window, so `add!(fig, D)`, `view_points(...)`, etc. reach the figure with no handle typing.
	QWidget     *conPanel = new QWidget(tabs);
	QVBoxLayout *conLay   = new QVBoxLayout(conPanel);
	conLay->setContentsMargins(2, 2, 2, 2);
	QPlainTextEdit *conOut = new QPlainTextEdit(conPanel);
	conOut->setReadOnly(true);
	conOut->setFont(QFont("Consolas", 10));
	conOut->setPlaceholderText("Julia output appears here. `fig` is this window. e.g.  add!(fig, [x y z]; mode=:points)");
	QLineEdit *conIn = new QLineEdit(conPanel);
	conIn->setFont(QFont("Consolas", 10));
	conIn->setPlaceholderText("julia>  (Enter to run)");
	conLay->addWidget(conOut, 1);
	conLay->addWidget(conIn, 0);
	conPanel->setLayout(conLay);
	tabs->addTab(conPanel, "Julia Console");
	s->console = conOut;
	QObject::connect(conIn, &QLineEdit::returnPressed, [s, conOut, conIn]() {
		const std::string cmd = conIn->text().toStdString();
		if (cmd.empty())
			return;
		conIn->clear();
		conOut->appendPlainText(QString("julia> ") + QString::fromStdString(cmd));
		if (!g_juliaEval) {
			conOut->appendPlainText("(no Julia eval callback registered — re-include bridge.jl)");
			return;
		}
		static std::vector<char> buf(1 << 16);   // 64 KB result buffer (shared, reused)
		// _console_eval returns the byte count; NEGATIVE flags a Julia error (still |n| bytes of text).
		int n   = g_juliaEval(s, cmd.c_str(), buf.data(), (int)buf.size());
		int len = n < 0 ? -n : n;
		if (len > 0)
			conOut->appendPlainText(QString::fromUtf8(buf.data(), len));
	});

	// Tab 2 — Errors: a READ-ONLY sink for execution errors from background callbacks (drop, coastlines,
	// basemap, tides, recolour, …). Those used to vanish into the REPL's stderr; Julia now also ccalls
	// gmtvtk_log_error -> here, raising this tab so a failure can't pass unseen. Typed-command errors stay
	// inline in the Julia Console tab; THIS tab is the program-side error log.
	QPlainTextEdit *errOut = new QPlainTextEdit(tabs);
	errOut->setReadOnly(true);
	errOut->setMaximumBlockCount(2000);
	errOut->setFont(QFont("Consolas", 10));
	errOut->setPlaceholderText("Execution errors from menu actions / background callbacks appear here.");
	tabs->addTab(errOut, "Errors");
	s->errConsole = errOut;

	// Tab 3 — Data Viewer: a read-only spreadsheet for a GMTdataset / plain matrix / vector,
	// filled from Julia via gmtvtk_set_table (e.g. show_table(fig, D)).
	s->dataTable = new QTableWidget(tabs);
	s->dataTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	s->dataTable->setAlternatingRowColors(true);
	s->dataTable->horizontalHeader()->setStretchLastSection(true);
	tabs->addTab(s->dataTable, "Data Viewer");

	// Custom dock title bar: a fold triangle sitting RIGHT BESIDE the "Panels" word (matching the
	// Scene Objects / Shading docks), instead of the old hide button lost in the tab-strip corner.
	// The triangle collapses the panel body (extend the 3-D view) / restores it; glyph swapped by
	// setBottomCollapsed (▸ collapsed, ▾ open).
	QWidget *titleBar = new QWidget(bottomDock);
	QHBoxLayout *titleLay = new QHBoxLayout(titleBar);
	titleLay->setContentsMargins(6, 2, 6, 2);
	titleLay->setSpacing(4);
	QToolButton *hideBtn = new QToolButton(titleBar);
	hideBtn->setText(QString::fromUtf8("\xE2\x96\xBE"));   // ▾ open
	hideBtn->setAutoRaise(true);
	hideBtn->setCursor(Qt::PointingHandCursor);
	hideBtn->setToolTip("Collapse this panel to extend the 3-D view");
	titleLay->addWidget(hideBtn);
	titleLay->addWidget(new QLabel("Panels", titleBar));
	titleLay->addStretch(1);
	// Float/dock button: a custom titleBarWidget suppresses Qt's native drag-to-undock, so we
	// restore undocking explicitly — toggles the dock between floating and docked (DockWidgetFloatable
	// is on by default). Once floating, the OS window frame lets the user move it / drag it back to dock.
	QToolButton *floatBtn = new QToolButton(titleBar);
	floatBtn->setText(QString::fromUtf8("\xE2\x9D\x90"));   // ❐ float / re-dock
	floatBtn->setAutoRaise(true);
	floatBtn->setCursor(Qt::PointingHandCursor);
	floatBtn->setToolTip("Undock this panel to a floating window / dock it back");
	titleLay->addWidget(floatBtn);
	bottomDock->setTitleBarWidget(titleBar);
	s->bottomHideBtn = hideBtn;
	QObject::connect(hideBtn,  &QToolButton::clicked, [s]() { setBottomCollapsed(s, !s->bottomCollapsed); });
	QObject::connect(floatBtn, &QToolButton::clicked, [bottomDock]() { bottomDock->setFloating(!bottomDock->isFloating()); });

	// View-menu items: show the dock, un-collapse it, and bring the matching tab forward.
	auto showTab = [s](QWidget *page) {
		if (s->bottomDock) s->bottomDock->setVisible(true);
		setBottomCollapsed(s, false);
		if (s->bottomTabs) s->bottomTabs->setCurrentWidget(page);
	};
	mView->addAction("&Profile Panel",       [showTab, s]()        { showTab(s->prof); });
	mView->addAction("Julia &Console Panel", [showTab, conPanel]() { showTab(conPanel); });
	mView->addAction("&Data Viewer Panel",   [showTab, s]()        { showTab(s->dataTable); });

	// Empty launcher / blank start: hide the surface, cube axes and gizmo BEFORE the first paint so
	// the window opens as a clean dark canvas instead of flashing an empty blue cube-axes box for one
	// frame (the caller's post-show hides would otherwise only bite on the NEXT render).
	if (blankStart) {
		if (s->surf) s->surf->SetVisibility(0);
		if (s->axes) s->axes->SetVisibility(0);
		if (s->giz)  setGizmoVisible(*s->giz, false);
		// Start the Scene Objects dock FOLDED with no open->fold flash: hide the body and flip the
		// fold-bar state BEFORE the first paint (so it never renders expanded for a frame); the
		// strip-width resizeDocks is deferred to just after win->show() (only bites once laid out).
		if (s->objFoldBar) {
			s->objFoldBar->openWidth = 220;        // width to restore when the user un-folds
			s->objPanel->setVisible(false);        // hide body -> dock can shrink to the strip
			s->objFoldBar->folded = true;
			s->objFoldBar->updateGeometry();       // sizeHint flips to the thin vertical strip
		}
	}

	// Start the "Panels" dock minimized to its tab strip BEFORE the first paint, so it never
	// flashes full-height then collapses (setBottomCollapsed clamps maxHeight from the tab-bar
	// sizeHint, no post-show geometry needed). show_table / profile track / View-menu actions
	// un-collapse it on demand (setBottomCollapsed(s,false)).
	setBottomCollapsed(s, true);

	// Default window large enough that the LEFT (Scene Objects) and RIGHT (Shading) side docks
	// both get real width. Without this the window opens at its minimum and the central VTK view
	// squeezes the right dock to ZERO width -> the Shading dock is invisible ("no docks").
	// Open in flat-2D from the FIRST painted frame (grids): switch the camera to top-down ortho BEFORE
	// the window is shown, so the 3-D oblique view buildSceneContent set never flashes on screen. ONE
	// shared switch (sceneSetFlat2D saves that 3-D camera for a later toggle back). The gizmo is built
	// further down and hidden there when flat2d is set.
	if (openFlat2D) sceneSetFlat2D(s, true);

	win->show();

	// Empty launcher: now that the layout has real geometry, shrink the pre-folded Scene Objects
	// dock to its strip width (resizeDocks only bites after show()).
	if (blankStart && s->objFoldBar)
		win->resizeDocks({objDock}, {s->objFoldBar->sizeHint().width()}, Qt::Horizontal);

	// Shrink the pre-folded Shading dock to its strip width (resizeDocks only bites after show()).
	if (hasShadedBody && s->shadeFoldBar && s->shadeFoldBar->folded)
		win->resizeDocks({dock}, {s->shadeFoldBar->sizeHint().width()}, Qt::Horizontal);

	// interactor must be live before we attach observers
	widget->renderWindow()->Render();
	applyShading(s);   // first real lighting/material/pass setup (IBL + PBR + passes)
	auto *iren = widget->interactor();

	// Explicit trackball camera style so right-drag = dolly and wheel = zoom.
	// (Left-drag is owned by the gizmo's DragCB; middle is owned by MiddleCB below.)
	vtkNew<vtkInteractorStyleTrackballCamera> style;
	iren->SetInteractorStyle(style);

	// Middle button (pan on drag, recenter on drag-less click) is handled directly in the
	// GLView widget subclass — VTK's interactor adapter never delivers the middle button to
	// observers here, but Qt delivers it to the widget. MiddleCB/MidPanFilter are dead code.

	// Coordinate readout (default priority). The gizmo's high-priority drag
	// observer aborts the event when it grabs a handle, so this won't double-fire.
	vtkNew<vtkCallbackCommand> moveCB;
	moveCB->SetCallback(onMouseMove);
	moveCB->SetClientData(s);
	iren->AddObserver(vtkCommand::MouseMoveEvent, moveCB);

	// Keep the horizontal Z billboards on the camera-near vertical edge as the view rotates.
	vtkNew<vtkCallbackCommand> axisCB;
	axisCB->SetCallback(AxisLabelCB);
	axisCB->SetClientData(s);
	s->ren->AddObserver(vtkCommand::StartEvent, axisCB);

	// Gizmo: scale cone + tilt ring + compass ring at the rotation centre.
	// Owns its own LeftButton/MouseMove observers at priority 10 and the 'x' toggle.
	s->giz = enableGizmo(s, 0.01);
	if (s->flat2d && s->giz) setGizmoVisible(*s->giz, false);   // 2D map: gizmo hidden (camera already top-down)
	// Polygon draw/edit tool: gestures are handled in the GLView widget (mouse*Event overrides,
	// 60_profile.cpp), gated on the tool state, so navigation is untouched when the tool is idle.
	// non-blocking: return now; the host pumps gmtvtk_process_events().
	return s;
}

