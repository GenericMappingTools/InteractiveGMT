// Busy (indeterminate) progress dialog around a blocking Julia call (Seismicity base-map/fetch
// path). Reuses `g_progress` (30_app.cpp, earlier in this TU) — the same QProgressDialog the
// gmtvtk_progress_ *C API (90_c_api.cpp, later in this TU) drives for the Okada patch loop, just
// with range (0,0) for Qt's indeterminate busy bar instead of a counted range.
// showBusyDialog / closeBusyDialog moved to 30_app.cpp: the file-open path (DropFilter and
// juliaOpenFile) lives there, earlier in this translation unit, and must be able to put the dialog up
// BEFORE it calls into Julia.

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
// LIDAR2011 PT — port of Mirone's cartas_militares.m in its second mode (menu entry
// "Tools -> Misc Tools -> LIDAR2011 PT" calls cartas_militares(handles,'nikles'), whose 2-arg branch
// hands the figure straight to the local `lidarPT` function and returns, so NONE of the 1:25000
// "Cartas Militares" tile matrix is built). What remains is a mosaic picker over the LIDAR2011 PT
// survey: a background image of mainland Portugal in the survey's metric frame, the survey's
// 1600 x 1000 m tile matrix drawn as ONE mesh (only the cells that actually have data), click a cell
// to select it (red) / click it again to drop it, then "Faz Mosaico" builds the grid spanning the
// bounding box of everything selected — inner unselected cells included, exactly as the original.
//
// Geometry is a literal transcription of cartas_militares_LayoutFcn (absolute pixel positions,
// y_qt = FigH - y_matlab - h; the figure is 'Resize','off' and sized from the screen: FigH =
// screenH-65, FigW = round(FigH/2), axes = [0 60 FigW FigH-60]) plus lidarPT()'s own additions
// (resolution popup + "Faz Mosaico" made visible, In-Web radio disabled, title "LIDAR2011",
// addHelpLegend's text panel, setSliders' pan sliders).
//
// The tile table itself is NOT hard-coded here: Julia reads data/lidarPT.dat with gmtread and pushes
// it in via gmtvtk_lidar_set_tiles when the dialog opens (op "init") — the same rows Mirone loaded
// from data/lidarPT.mat (row 1 = the global bounding box, rows 2..N = one 1600x1000 m tile each,
// with the tile's name in the trailing text field).
// ============================================================================================
class LidarArea : public QWidget {       // the clickable map: PT background image + survey tile mesh
public:
	static constexpr double XINC = 1600.0;   // LIDAR2011 tile size, metres (lidarPT.m x_inc/y_inc)
	static constexpr double YINC = 1000.0;
	// The background image's extent, hard-coded exactly as in lidarPT() ("Hard code this limits.
	// This is very bug prone solution I know but avoids having to have a .jgw file").
	static constexpr double BGX0 = -123787.423169315, BGX1 = 92138.1835464904;
	static constexpr double BGY0 = -303489.079090607, BGY1 = 261783.41534662;

	QPixmap bg;                                        // data/PTimg_lidar.jpg
	double  dx0 = 0, dx1 = 0, dy0 = 0, dy1 = 0;        // the survey's global bounding box (mosaico row 1)
	int     nColsQ = 0;                                // (dx1-dx0)/XINC + 1, the row-major index stride
	std::map<int, QString> cells;                      // linear cell index -> tile name (the data cells)
	std::set<int>          sel;                        // selected cells (a click toggles one)
	double  vX0 = 0, vX1 = 1, vY0 = 0, vY1 = 1;        // current view window, map metres
	double  fX0 = 0, fX1 = 1, fY0 = 0, fY1 = 1;        // the full ("zoomed out") view, for clamping
	std::function<void()> onViewChanged;               // notify the picker (re-sync the pan sliders)
	// Left-drag rubber-band zoom (a plain click without movement still toggles a cell); a double-click
	// zooms back out to the full extent, undoing the cell its first click had just toggled.
	QPoint  pressPx;  bool dragging = false;  QRect band;
	int     lastToggled = 0;
	bool    swallowRelease = false;      // Qt sends a release AFTER a double-click — it must not select

	explicit LidarArea(QWidget *p) : QWidget(p) { setFocusPolicy(Qt::StrongFocus); }

	// Install the tile table pushed from Julia. `rects` is 4*n doubles (x0,x1,y0,y1 per row) and
	// `names` the matching newline-joined names; row 0 is the survey's global bbox (its name unused),
	// rows 1..n-1 are the tiles. Index arithmetic is Mirone's verbatim (lidarPT(): col/row from the
	// tile's lower-left corner, ind = (row-1)*nColsQ + col), so C++ and Julia agree on every cell id.
	void setTiles(const double *rects, const QStringList &names, int n) {
		cells.clear(); sel.clear();
		if (!rects || n < 1) return;
		dx0 = rects[0]; dx1 = rects[1]; dy0 = rects[2]; dy1 = rects[3];
		nColsQ = int(std::lround((dx1 - dx0) / XINC)) + 1;
		for (int k = 1; k < n; ++k) {
			int col = int(std::lround((rects[4 * k + 0] - dx0) / XINC)) + 1;
			int row = int(std::lround((rects[4 * k + 2] - dy0) / YINC)) + 1;
			cells[(row - 1) * nColsQ + col] = (k < names.size()) ? names[k] : QString();
		}
		resetView();
	}
	// lidarPT()'s initial limits: the data bbox with a wide left/right margin (so the whole background
	// image fits) and a two-cell margin top/bottom.
	void resetView() {
		fX0 = dx0 - 35 * XINC; fX1 = dx1 + 30 * XINC;
		fY0 = dy0 -  2 * YINC; fY1 = dy1 +  2 * YINC;
		vX0 = fX0; vX1 = fX1; vY0 = fY0; vY1 = fY1;
		fitAspect(); update(); if (onViewChanged) onViewChanged();
	}
	// 'DataAspectRatio',[1 1 1] — metres must be square on screen, so grow whichever axis is short.
	void fitAspect() {
		if (width() < 2 || height() < 2) return;
		double sx = (vX1 - vX0) / width(), sy = (vY1 - vY0) / height();
		double s = std::max(sx, sy);
		double cx = (vX0 + vX1) / 2, cy = (vY0 + vY1) / 2;
		double hw = s * width() / 2, hh = s * height() / 2;
		vX0 = cx - hw; vX1 = cx + hw; vY0 = cy - hh; vY1 = cy + hh;
	}
	double x2px(double x) const { return (x - vX0) / (vX1 - vX0) * width(); }
	double y2py(double y) const { return (vY1 - y) / (vY1 - vY0) * height(); }
	double px2x(double px) const { return vX0 + px / std::max(1, width())  * (vX1 - vX0); }
	double py2y(double py) const { return vY1 - py / std::max(1, height()) * (vY1 - vY0); }

	// '+'/'-' keys, as in figure1_KeyPressFcn (zoom_j 2 / 0.5): scale the span about the view centre,
	// never past the full extent.
	void zoomBy(double f) {
		double cx = (vX0 + vX1) / 2, cy = (vY0 + vY1) / 2;
		double hw = (vX1 - vX0) / (2 * f), hh = (vY1 - vY0) / (2 * f);
		double maxHw = (fX1 - fX0) / 2, maxHh = (fY1 - fY0) / 2;
		if (hw > maxHw || hh > maxHh) { hw = maxHw; hh = maxHh; cx = (fX0 + fX1) / 2; cy = (fY0 + fY1) / 2; }
		vX0 = cx - hw; vX1 = cx + hw; vY0 = cy - hh; vY1 = cy + hh;
		fitAspect(); clampView(); update(); if (onViewChanged) onViewChanged();
	}
	// Centre the view on (cx,cy) keeping its span (the arrow keys and the pan sliders drive this).
	void panTo(double cx, double cy) {
		double hw = (vX1 - vX0) / 2, hh = (vY1 - vY0) / 2;
		vX0 = cx - hw; vX1 = cx + hw; vY0 = cy - hh; vY1 = cy + hh;
		clampView(); update(); if (onViewChanged) onViewChanged();
	}
	void panBy(double fx, double fy) {           // fractions of the current span (arrow keys)
		panTo((vX0 + vX1) / 2 + fx * (vX1 - vX0), (vY0 + vY1) / 2 + fy * (vY1 - vY0));
	}
	void clampView() {                            // keep the view inside the full extent
		double w = vX1 - vX0, h = vY1 - vY0;
		if (w < fX1 - fX0) { if (vX0 < fX0) { vX0 = fX0; vX1 = fX0 + w; }  if (vX1 > fX1) { vX1 = fX1; vX0 = fX1 - w; } }
		if (h < fY1 - fY0) { if (vY0 < fY0) { vY0 = fY0; vY1 = fY0 + h; }  if (vY1 > fY1) { vY1 = fY1; vY0 = fY1 - h; } }
	}
	// The cell rectangle (map metres) of a linear cell index, and the reverse look-up from a point.
	QRectF cellRect(int ind) const {
		int col = (ind - 1) % nColsQ + 1, row = (ind - 1) / nColsQ + 1;
		double x = dx0 + (col - 1) * XINC, y = dy0 + (row - 1) * YINC;
		return QRectF(x, y, XINC, YINC);
	}
	int cellAt(double x, double y) const {
		if (nColsQ <= 0) return 0;
		int col = int(std::floor((x - dx0) / XINC)) + 1;
		int row = int(std::floor((y - dy0) / YINC)) + 1;
		if (col < 1 || col > nColsQ || row < 1) return 0;
		return (row - 1) * nColsQ + col;
	}
protected:
	void resizeEvent(QResizeEvent *) override { fitAspect(); clampView(); if (onViewChanged) onViewChanged(); }

	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		g.setRenderHint(QPainter::SmoothPixmapTransform, true);
		g.fillRect(rect(), QColor(255, 255, 255));
		if (!bg.isNull()) {                      // the PT background over its hard-coded extent
			QRectF tgt(x2px(BGX0), y2py(BGY1), x2px(BGX1) - x2px(BGX0), y2py(BGY0) - y2py(BGY1));
			g.drawPixmap(tgt, bg, QRectF(bg.rect()));
		}
		// the survey mesh: one rectangle per data cell, 'EdgeColor','y','FaceColor','none'
		const double cw = XINC / (vX1 - vX0) * width();          // a cell's on-screen width
		g.setPen(QPen(QColor(230, 200, 0), 1));
		g.setBrush(Qt::NoBrush);
		if (cw >= 1.5) {                                          // below ~1 px the mesh is just noise
			for (const auto &kv : cells) {
				QRectF r = cellRect(kv.first);
				if (r.right() < vX0 || r.left() > vX1 || r.top() > vY1 || r.bottom() < vY0) continue;
				g.drawRect(QRectF(x2px(r.left()), y2py(r.top() + r.height()),
				                  x2px(r.right()) - x2px(r.left()),
				                  y2py(r.top()) - y2py(r.top() + r.height())));
			}
		}
		else {                                                    // zoomed out: a solid coverage blob
			g.setPen(Qt::NoPen); g.setBrush(QColor(230, 200, 0, 120));
			for (const auto &kv : cells) {
				QRectF r = cellRect(kv.first);
				g.drawRect(QRectF(x2px(r.left()), y2py(r.top() + r.height()),
				                  std::max(1.0, x2px(r.right()) - x2px(r.left())),
				                  std::max(1.0, y2py(r.top()) - y2py(r.top() + r.height()))));
			}
		}
		// selected cells — bdnLidar's red patch ('FaceColor','r'), click again to delete it
		g.setPen(QPen(QColor(180, 0, 0), 1)); g.setBrush(QColor(255, 0, 0, 170));
		for (int ind : sel) {
			QRectF r = cellRect(ind);
			g.drawRect(QRectF(x2px(r.left()), y2py(r.top() + r.height()),
			                  std::max(1.0, x2px(r.right()) - x2px(r.left())),
			                  std::max(1.0, y2py(r.top()) - y2py(r.top() + r.height()))));
		}
		if (dragging && band.width() > 1 && band.height() > 1) {  // the rubber-band zoom rectangle
			g.setPen(QPen(QColor(0, 0, 0), 1, Qt::DashLine));
			g.setBrush(QColor(0, 90, 200, 40));
			g.drawRect(band);
		}
	}
	// Toggle the cell under a point — the plain-click selection (bdnLidar's red patch, and its
	// "delete(gco)" when the patch itself is clicked again).
	void toggleAt(double x, double y) {
		int ind = cellAt(x, y);
		lastToggled = 0;
		if (ind <= 0 || cells.find(ind) == cells.end()) return;   // outside the surveyed area
		auto it = sel.find(ind);
		if (it != sel.end()) sel.erase(it);
		else                 sel.insert(ind);
		lastToggled = ind;                        // so a double-click can undo this first click
		update();
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) return;
		pressPx = e->position().toPoint();
		dragging = false;
		band = QRect(pressPx, pressPx);
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (!(e->buttons() & Qt::LeftButton)) return;
		QPoint p = e->position().toPoint();
		if (!dragging && (p - pressPx).manhattanLength() < 5) return;   // still a click, not a drag
		dragging = true;
		setCursor(Qt::SizeAllCursor);
		band = QRect(pressPx, p).normalized();
		update();
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) return;
		// A double-click arrives as press / release / DoubleClick / release: this trailing release must
		// NOT be read as a fresh click, or every zoom-out would select whatever square sits under it.
		if (swallowRelease) { swallowRelease = false; return; }
		if (!dragging) { toggleAt(px2x(e->position().x()), py2y(e->position().y())); return; }
		dragging = false;
		unsetCursor();
		QRect b = band; band = QRect();
		update();
		if (b.width() < 5 || b.height() < 5) return;                    // a slip of the hand, not a zoom
		double x0 = px2x(b.left()), x1 = px2x(b.right());
		double y1 = py2y(b.top()),  y0 = py2y(b.bottom());
		vX0 = x0; vX1 = x1; vY0 = y0; vY1 = y1;
		fitAspect(); clampView(); update(); if (onViewChanged) onViewChanged();
	}
	// Double-click zooms back out to the whole survey AND clears the selection — including the cell the
	// double-click's own first click had just toggled, which is why nothing needs undoing here.
	void mouseDoubleClickEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) return;
		swallowRelease = true;                 // eat the release Qt sends right after this event
		dragging = false; band = QRect();
		lastToggled = 0;
		sel.clear();
		resetView();
	}
};

// One LIDAR2011 picker per window, alive while parked. Closing it with the X does NOT destroy it: it
// hides and PARKS as a handle at the bottom of that window's Scene Objects dock, exactly like a closed
// X,Y plot / Contours / Illumination dialog — same Scene::parkedTools list, same parkTool/unparkTool
// pair, same row builder. Re-picking the menu entry brings the SAME dialog back (selection and chosen
// directory intact), never a second one.
class LidarPicker;
static std::map<Scene *, LidarPicker *> g_lidarDlgs;

class LidarPicker : public QDialog {
public:
	Scene       *scene;
	LidarArea   *map;
	QComboBox   *cboDir;                      // popup_directory_list — where the *-mis_orto.* tiles live
	QComboBox   *cboRes;                      // popup_resolution {2 10 20 50} metres
	QScrollBar  *hbar, *vbar;                 // setSliders' pan pair
	QLabel      *status;                      // replaces aguentabar ("A ler os fiches")
	bool         reallyClose = false;         // set by the parked row's "Delete": let the next close through

	void setStatus(const QString &t) { status->setText(t); status->setVisible(!t.isEmpty()); }

	// Bring the dialog back from the dock (double-click, the row's checkbox, its "Show" item). ONE
	// function for every way back in, like xyUnpark / ContourDialog::unpark.
	void unpark() {
		unparkTool(scene, this);
		setWindowState(windowState() & ~Qt::WindowMinimized);
		showNormal();
		raise();
		activateWindow();
	}
	// The parked row's menu — properties button and context menu are the same lambda, never two.
	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel  = m.addAction("Delete");
			QAction *pick  = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scene, this);
				deleteLater();               // destroyed -> the row and this object go with it
			}
		};
	}
	// Drop the registry entry by VALUE, not by `scene`: when the owning viewer window is torn down the
	// dialog dies with it and the Scene may already be gone, so the key cannot be trusted.
	~LidarPicker() override {
		for (auto it = g_lidarDlgs.begin(); it != g_lidarDlgs.end(); )
			it = (it->second == this) ? g_lidarDlgs.erase(it) : std::next(it);
	}

	LidarPicker(QWidget *parent, Scene *s, const QPixmap &img) : QDialog(parent), scene(s) {
		// --- the figure itself: sized off the screen, fixed (cartas_militares_LayoutFcn) ---
		const int screenH = QApplication::primaryScreen() ? QApplication::primaryScreen()->availableGeometry().height() : 900;
		const int H  = screenH - 65;
		const int W  = int(std::lround(H * 0.5));
		const int AH = H - 60;                 // axes1 height
		setWindowTitle("LIDAR2011");           // lidarPT(): set(handles.figure1,'Name','LIDAR2011')
		setFixedSize(W, H);                    // 'Resize','off'

		map = new LidarArea(this); map->bg = img;
		map->setGeometry(0, 0, W, AH);         // axes1 = [0 60 FigW AxHeight]

		// addHelpLegend's instruction panel — Mirone paints it permanently over the map's upper-right
		// quadrant; here it is behind a "?" button instead (in English, and covering the mouse zoom
		// this port adds), so it never sits on top of the tiles you are trying to click.

		// setSliders(handles, 9, 50): vertical slider down the axes' right edge, horizontal one below it.
		vbar = new QScrollBar(Qt::Vertical, this);   vbar->setGeometry(W - 9, 0, 9, AH);
		hbar = new QScrollBar(Qt::Horizontal, this); hbar->setGeometry(0, H - 59, W, 9);

		// --- the bottom control strip ---
		cboDir = new QComboBox(this);
		cboDir->setGeometry(10, H - 25, W - 40, 22);              // [10 3 FigW-40 22]
		cboDir->setEditable(true);
		cboDir->setToolTip("Select the directory where the LIDAR2011 PT LAZ files reside");
		{   // Mirone seeds this from mirone_pref.mat's directory_list, with the remembered lidarPT_dir
			// on top. Ours is the shared iGMT directory MRU (prefs/dirMRU) plus our own saved lidar dir.
			QSettings st = igmtSettings();
			QString last = st.value("lidar/dir").toString();
			if (!last.isEmpty()) cboDir->addItem(last);
			for (const QString &d : prefDirMRU())
				if (!d.isEmpty() && cboDir->findText(d) < 0) cboDir->addItem(d);
			cboDir->setCurrentIndex(0);
		}
		QToolButton *btnDir = new QToolButton(this);
		btnDir->setGeometry(W - 31, H - 26, 21, 23);              // [FigWidth-31 3 21 23]
		btnDir->setText("...");
		{ QFont f = btnDir->font(); f.setPointSize(10); f.setBold(true); btnDir->setFont(f); }
		btnDir->setToolTip("Select a different directory");

		QRadioButton *rInLoco = new QRadioButton("In loco", this);
		rInLoco->setGeometry(10, H - 45, 75, 15);                 // [10 30 61 15] (widened: Qt font)
		rInLoco->setChecked(true);
		rInLoco->setToolTip("When you have the files on disk");
		rInLoco->setVisible(false);                               // hidden, like its In-Web twin below
		QRadioButton *rInWeb = new QRadioButton("In Web", this);
		rInWeb->setGeometry(130, H - 45, 85, 15);                 // [130 30 71 15]
		rInWeb->setEnabled(false);                                // lidarPT(): set(radio_inWeb,'Enable','off')
		rInWeb->setVisible(false);                                // ...and hidden here: disk is the only source
		QButtonGroup *gWhere = new QButtonGroup(this);
		gWhere->addButton(rInLoco); gWhere->addButton(rInWeb);

		QLabel *txtRes = new QLabel("Resolution ", this);          // Mirone: "Resolução"
		txtRes->setGeometry(W - 260, H - 42, 80, 16);             // [FigWidth-260 26 80 16]
		txtRes->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		cboRes = new QComboBox(this);
		cboRes->setGeometry(W - 180, H - 47, 62, 21);             // [FigWidth-180 27 40 19], widened:
		cboRes->addItems({"2", "10", "20", "50"});                // 40 px left no room for the number
		cboRes->setToolTip("Select resolution of final DTM (in meters)");
		QPushButton *btnGo = new QPushButton("Do Mosaic", this);   // Mirone: "Faz Mosaico"
		btnGo->setGeometry(W - 110, H - 47, 100, 21);             // [FigWidth-110 26 100 21]
		{ QFont f = btnGo->font(); f.setPointSize(10); f.setBold(true); btnGo->setFont(f); }

		// "?" — addHelpLegend's text, on demand. Sits at the left end of the strip, where the two
		// hidden radios used to be (the right half of that band holds resolution + "Do Mosaic").
		QToolButton *btnHelp = new QToolButton(this);
		btnHelp->setGeometry(10, H - 52, 26, 26);
		btnHelp->setText("?");
		{ QFont f = btnHelp->font(); f.setPointSize(12); f.setBold(true); btnHelp->setFont(f); }
		btnHelp->setToolTip("How to use this window");

		// Progress line (Mirone shows an aguentabar while reading the files). Overlaid on the map's
		// bottom-left so the transcribed control strip keeps its geometry; hidden when idle.
		status = new QLabel(this);
		status->setGeometry(6, AH - 24, W - 20, 18);
		status->setStyleSheet("background: rgba(255,255,255,220); color: #202020; border: 1px solid #808080;");
		status->setVisible(false);

		// --- wiring ---
		map->onViewChanged = [this]() { syncBars(); };
		QObject::connect(hbar, &QScrollBar::valueChanged, this, [this](int v) { onHBar(v); });
		QObject::connect(vbar, &QScrollBar::valueChanged, this, [this](int v) { onVBar(v); });
		QObject::connect(btnDir, &QToolButton::clicked, this, [this]() {
			QString start = cboDir->currentText().trimmed();
			if (start.isEmpty()) start = prefStartDir();
			QString d = QFileDialog::getExistingDirectory(this, "Select a directory", start);
			if (d.isEmpty()) return;
			if (cboDir->findText(d) < 0) cboDir->insertItem(0, d);
			cboDir->setCurrentText(d);
			igmtSettings().setValue("lidar/dir", d);     // Mirone saves lidarPT_dir in mirone_pref.mat
			prefPushDir(d);
		});
		QObject::connect(btnGo, &QPushButton::clicked, this, [this]() { doMosaic(); });
		QObject::connect(btnHelp, &QToolButton::clicked, this, [this]() {
			// addHelpLegend's text, translated literally, plus the two mouse-zoom lines this port adds.
			QMessageBox::information(this, "LIDAR2011 PT",
				"Select the directory holding the LIDAR2011 tiles.\n\n"
				"For zoom, use the +/- keys.\nUse the arrows to move horizontally and vertically.\n"
				"Left-click-drag defines the zoom window.\nDouble left-click unzooms to the full extent.\n\n"
				"Click a little square to select it. More than one\nselects a rectangle.\n\n"
				"At the end click \"Do Mosaic\".");
		});
		g_lidarDlgs[s] = this;
		map->setFocus();
	}
protected:
	// The X parks instead of destroying: the picker hides and leaves a handle in this window's Scene
	// Objects dock, so the cell selection and the chosen data directory survive. Only the parked row's
	// own "Delete" (which sets reallyClose) lets a close through.
	void closeEvent(QCloseEvent *e) override {
		if (reallyClose || !sceneAlive(scene)) { QDialog::closeEvent(e); return; }
		e->ignore();
		hide();
		parkTool(scene, this, "LIDAR2011 PT", IC_Rect,
		         "Closed LIDAR2011 PT picker — double-click to bring it back, click for Show / Delete",
		         [this]() { unpark(); }, parkedMenu());
		unfoldSceneObjects(scene);        // a handle nobody can see is no handle at all
	}
	// Escape reaches QDialog::reject(), which hides through done() WITHOUT a close event — that would
	// leave the picker invisible and unparked. Route it through close() so it parks like the X.
	void reject() override { close(); }
	// figure1_KeyPressFcn: '+'/'-' zoom, arrow keys scroll horizontally/vertically.
	void keyPressEvent(QKeyEvent *e) override {
		switch (e->key()) {
			case Qt::Key_Plus:  case Qt::Key_Equal: map->zoomBy(2.0);  return;
			case Qt::Key_Minus:                     map->zoomBy(0.5);  return;
			case Qt::Key_Right: map->panBy( 0.25,  0.0); return;
			case Qt::Key_Left:  map->panBy(-0.25,  0.0); return;
			case Qt::Key_Up:    map->panBy( 0.0,   0.25); return;
			case Qt::Key_Down:  map->panBy( 0.0,  -0.25); return;
		}
		QDialog::keyPressEvent(e);
	}
private:
	// Keep the pan sliders in step with the view; a slider is disabled while its whole axis is visible.
	void syncBars() {
		auto sync = [](QScrollBar *b, double v0, double v1, double f0, double f1, bool invert) {
			double span = v1 - v0, full = f1 - f0;
			b->blockSignals(true);
			bool on = span < full - 1e-6;
			b->setEnabled(on);
			b->setRange(0, on ? 1000 : 0);
			b->setPageStep(on ? int(1000.0 * span / full) : 1000);
			if (on) {
				double t = ((v0 + v1) / 2 - (f0 + span / 2)) / (full - span);
				if (invert) t = 1.0 - t;
				b->setValue(int(std::clamp(t * 1000.0, 0.0, 1000.0)));
			}
			b->blockSignals(false);
		};
		sync(hbar, map->vX0, map->vX1, map->fX0, map->fX1, false);
		sync(vbar, map->vY0, map->vY1, map->fY0, map->fY1, true);   // value 0 = north (top)
	}
	void onHBar(int v) {
		double span = map->vX1 - map->vX0, full = map->fX1 - map->fX0;
		if (span >= full) return;
		map->panTo(map->fX0 + span / 2 + v / 1000.0 * (full - span), (map->vY0 + map->vY1) / 2);
	}
	void onVBar(int v) {
		double span = map->vY1 - map->vY0, full = map->fY1 - map->fY0;
		if (span >= full) return;
		map->panTo((map->vX0 + map->vX1) / 2, map->fY1 - span / 2 - v / 1000.0 * (full - span));
	}
	// push_lidarMosaico_CB: the bounding box of every selected cell (inner unselected cells included),
	// the decimation resolution and the data directory go to Julia, which reads the tiles and builds
	// the mosaic grid. Row/col are Mirone's 1-based data-matrix addresses, so Julia can re-derive each
	// cell's name from the same table it pushed in.
	void doMosaic() {
		if (map->sel.empty()) {
			QMessageBox::warning(this, "LIDAR2011",
				QString::fromUtf8("Selecciona pelo menos um quadradinho (clica num quadrado do mosaico)."));
			return;
		}
		int rMin = std::numeric_limits<int>::max(), rMax = 0;
		int cMin = std::numeric_limits<int>::max(), cMax = 0;
		for (int ind : map->sel) {
			int col = (ind - 1) % map->nColsQ + 1, row = (ind - 1) / map->nColsQ + 1;
			rMin = std::min(rMin, row); rMax = std::max(rMax, row);
			cMin = std::min(cMin, col); cMax = std::max(cMax, col);
		}
		QString dir = cboDir->currentText().trimmed();
		if (dir.isEmpty()) {
			QMessageBox::warning(this, "LIDAR2011", QString::fromUtf8("Selecciona primeiro o directório dos dados."));
			return;
		}
		igmtSettings().setValue("lidar/dir", dir);
		QString params = QString("go;%1/%2/%3/%4;%5;%6").arg(rMin).arg(rMax).arg(cMin).arg(cMax)
		                        .arg(cboRes->currentText()).arg(dir);
		setStatus(QString::fromUtf8("A ler os fiches…  (a primeira vez também compila)"));
		QApplication::processEvents();                 // paint the status before the blocking call
		if (g_juliaLidar) g_juliaLidar(scene, this, params.toUtf8().constData());
		setStatus("");
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
		setFixedSize(433, 330);

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

		// ---- Row 4: NaN fill colour (grid holes) -------------------------------------------
		auto *lblNan = new QLabel("NaN fill color", this);
		lblNan->setGeometry(20, 200, 121, 16);
		btnNanColor = new QPushButton(this);
		btnNanColor->setGeometry(20, 220, 60, 24);
		btnNanColor->setToolTip("Solid colour used to paint grid NaN cells (holes). Click to choose.");
		QObject::connect(btnNanColor, &QPushButton::clicked, this, [this]() {
			QColor c = QColorDialog::getColor(QColor(nanColorHex), this, "NaN fill color");
			if (c.isValid()) { nanColorHex = c.name(); setNanSwatch(); }
		});

		// ---- Row 5: viewer background color --------------------------------------------------
		auto *lblBg = new QLabel("Background color", this);
		lblBg->setGeometry(160, 200, 141, 16);
		btnBgColor = new QPushButton(this);
		btnBgColor->setGeometry(160, 220, 60, 24);
		btnBgColor->setToolTip("Solid colour for the 3-D viewer background. Click to choose.");
		QObject::connect(btnBgColor, &QPushButton::clicked, this, [this]() {
			QColor start = bgColorHex.isEmpty() ? QColor("#292e38") : QColor(bgColorHex);
			QColor c = QColorDialog::getColor(start, this, "Background color");
			if (c.isValid()) { bgColorHex = c.name(); setBgSwatch(); }
		});
		// Tiny text-sized reset button, right against the swatch — restores the built-in gradient.
		auto *btnBgReset = new QToolButton(this);
		btnBgReset->setText("Reset");
		btnBgReset->setAutoRaise(true);
		btnBgReset->setToolTip("Restore the built-in dark-slate/steel-blue gradient background.");
		btnBgReset->adjustSize();
		btnBgReset->move(160 + 60 + 4, 220 + (24 - btnBgReset->height()) / 2);
		QObject::connect(btnBgReset, &QToolButton::clicked, this, [this]() { bgColorHex.clear(); setBgSwatch(); });

		// ---- OK -----------------------------------------------------------------------------
		auto *btnOK = new QPushButton("OK", this);
		btnOK->setGeometry(330, 290, 90, 28);
		btnOK->setDefault(true);
		QObject::connect(btnOK, &QPushButton::clicked, this, [this]() { save(); accept(); });

		load();
	}

private:
	QComboBox *cmbMeasureUnits, *cmbDistAzim, *cmbAzimDir, *cmbDefaultDir;
	QComboBox *cmbLineThickness, *cmbLineColor, *cmbCoastColor;
	QPushButton *btnNanColor, *btnBgColor;
	QString      nanColorHex;   // current NaN fill colour (#rrggbb), edited by the swatch button
	QString      bgColorHex;    // current background colour (#rrggbb); empty = built-in gradient default

	// Select a combo entry by text; for the editable directory combo just set the edit text.
	static void selectText(QComboBox *c, const QString &txt) {
		int i = c->findText(txt);
		if (i >= 0) c->setCurrentIndex(i);
		else if (c->isEditable() && !txt.isEmpty()) c->setEditText(txt);
	}

	// Paint the NaN swatch button with the current colour (readable text label = the hex).
	void setNanSwatch() {
		QColor c(nanColorHex);
		if (!c.isValid()) c = QColor(Qt::white);
		const QString fg = (c.lightnessF() > 0.5) ? "#000000" : "#ffffff";
		btnNanColor->setStyleSheet(QString("background-color:%1; color:%2;").arg(c.name(), fg));
		btnNanColor->setText(c.name());
	}

	// Paint the background swatch button with the current colour; empty bgColorHex (built-in
	// gradient, restored by the Reset button) shows a neutral swatch instead of a fake hex.
	void setBgSwatch() {
		if (bgColorHex.isEmpty()) {
			btnBgColor->setStyleSheet(QString());
			btnBgColor->setText("(default)");
			return;
		}
		QColor c(bgColorHex);
		if (!c.isValid()) c = QColor("#292e38");
		const QString fg = (c.lightnessF() > 0.5) ? "#000000" : "#ffffff";
		btnBgColor->setStyleSheet(QString("background-color:%1; color:%2;").arg(c.name(), fg));
		btnBgColor->setText(c.name());
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
		nanColorHex = prefNanColor(); setNanSwatch();
		bgColorHex = prefBackgroundColor(); setBgSwatch();
	}

	void save() {
		QSettings st = igmtSettings();
		st.setValue("prefs/measureUnits",  cmbMeasureUnits->currentText());
		st.setValue("prefs/distAzimType",  cmbDistAzim->currentText());
		st.setValue("prefs/azimDir",       cmbAzimDir->currentText());
		st.setValue("prefs/lineThickness", cmbLineThickness->currentText());
		st.setValue("prefs/lineColor",     cmbLineColor->currentText());
		st.setValue("prefs/coastColor",    cmbCoastColor->currentText());
		st.setValue("prefs/nanColor",      nanColorHex);
		st.setValue("prefs/backgroundColor", bgColorHex);
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

// Put a node's tile on screen. Two separate steps, because they cost wildly different amounts:
// BUILDING the geometry (makeGridTile: meshes up to ~513x513 quads out of s->gridZ, on the calling
// thread, inside the camera callback) versus merely ADDING an already-built actor to the assembly.
// A node that was coarsened away still owns its geometry, so coming back to that zoom level is the
// cheap step alone. Idempotent in both halves.
static void ensureNodeActor(Scene *s, QuadNode *n) {
	if (n->actor) {                       // already meshed -- re-adding is all that can be needed
		if (!n->inScene) {
			s->surfGroup->AddPart(n->actor); s->tiles.push_back(n->actor); n->inScene = true;
			// Re-style ONLY if the shading changed while this tile sat in the cache. applyShading
			// reaches `s->tiles`, i.e. tiles IN the scene, so a cached tile never hears about a mode
			// switch — that is how PBR tiles reappeared beside grid-illuminated ones after a zoom out
			// and back in. But re-baking unconditionally is just as wrong the other way: every re-add
			// re-ran the CPT mapping and the whole per-point shade for an already correctly coloured
			// mesh, which is why illumination felt heavier the more the view moved.
			if (n->styleGen != s->styleGen) { applySurfStyle(s, n->actor); n->styleGen = s->styleGen; }
		}
		return;
	}
	// Charge the meshing budget for THIS pass. Only a real build costs — a cached tile being re-added
	// above is free and must not consume the allowance that keeps the frame responsive.
	s->lodCellsLeft -= (long)(((n->i1 - n->i0) / n->step + 2) * (long)((n->j1 - n->j0) / n->step + 2));
	auto tpd = makeGridTile(s->gridZ.data(), s->gnx, s->gny,
							n->i0, n->i1, n->j0, n->j1, s->gx0, s->gdx, s->gy0, s->gdy, s->zmin, n->step);
	vtkNew<vtkPolyDataMapper> m; m->SetInputData(tpd);
	configureGridMapper(m, s->surfLut, s->zmin, s->zmax, s->surfCtfRange);
	auto a = vtkSmartPointer<vtkActor>::New(); a->SetMapper(m);
	a->GetProperty()->SetEdgeColor(0.12, 0.12, 0.12); a->GetProperty()->SetLineWidth(1.0);
	a->GetProperty()->SetEdgeVisibility(s->surfEdges);
	// The tile takes the window's CURRENT surface style, whatever it is -- applySurfStyle is the ONE
	// function that decides material + colouring (noShade / hillshade-unlit / matte / PBR) and it is
	// what every other surface actor goes through. This used to hard-set PBR here and only call
	// applySurfStyle when useHillshade was on, so a tile born mid-zoom in any other mode arrived
	// PBR-lit while its neighbours were not: pure grid illumination on the old tiles, PBR on the new
	// ones, in the same picture. A tile is not a special kind of surface.
	applySurfStyle(s, a);
	n->styleGen = s->styleGen;                   // freshly baked under the current shading state
	const vtkIdType npts = tpd->GetPoints()->GetNumberOfPoints();
	const vtkIdType ncel = tpd->GetPolys()->GetNumberOfCells();
	n->bytes = (size_t)npts * (12 + 4 + 12) + (size_t)ncel * 20;   // pts + z + normal + quad ids
	n->actor = a;
	n->inScene = true;
	s->surfGroup->AddPart(a);
	s->tiles.push_back(a);
	s->lodResidentBytes += n->bytes;
}

// Take a tile OFF SCREEN but KEEP its geometry. This is what coarsening does, and keeping the mesh
// is the whole point: zoom out one notch and back in and the finer level is re-added instantly
// instead of re-meshed. It used to free the actor outright, so the commonest interaction there is —
// zoom in, out, in — re-ran makeGridTile over the whole visible extent every single time, which is
// exactly the "some zoom levels are very slow / lots of under-hood regeneration" the user hit on a
// global 8640x4320 Ocean Color grid. The memory this keeps resident is NOT unbounded: it still
// counts towards lodResidentBytes, so evictLRU reclaims it against the same budget as before.
static void dropNodeActor(Scene *s, QuadNode *n) {
	if (!n->actor || !n->inScene) return;
	s->surfGroup->RemovePart(n->actor);
	for (size_t k = 0; k < s->tiles.size(); ++k)
		if (s->tiles[k] == n->actor) { s->tiles.erase(s->tiles.begin() + k); break; }
	n->inScene = false;
}

// Actually RELEASE a node's geometry. Only the LRU evictor calls this — refinement never does, or
// the cache above could not survive a zoom.
static void freeNodeActor(Scene *s, QuadNode *n) {
	if (!n->actor) return;
	dropNodeActor(s, n);
	s->lodResidentBytes = (s->lodResidentBytes >= n->bytes) ? s->lodResidentBytes - n->bytes : 0;
	n->actor = nullptr; n->bytes = 0;
}

static void dropSubtree(Scene *s, QuadNode *n) {
	if (!n) return;
	dropNodeActor(s, n);
	for (int k = 0; k < 4; ++k) dropSubtree(s, n->child[k]);
}

static void collectResident(QuadNode *n, std::vector<QuadNode*> &out) {
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
		freeNodeActor(s, n);                        // the ONE place geometry is really released
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
	}
	else if (s->lodCellsLeft <= 0) {
		// Out of meshing budget for this pass. Draw THIS (coarser) node instead of descending: the
		// area stays covered — usually for free, since a coarse ancestor is normally already built —
		// and the detail is owed to the catch-up pass. This is the whole anti-freeze mechanism: the
		// recursion stops at the budget instead of meshing a level's worth of tiles inside one
		// camera event.
		ensureNodeActor(s, n); n->lastUsed = s->lodFrame;
		for (int k = 0; k < 4; ++k) dropSubtree(s, n->child[k]);
		s->lodPending = true;
	}
	else {
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
	// ~600 k sampled cells per pass: enough that an ordinary zoom finishes in one go, small enough
	// that a level-crossing zoom cannot stall the frame. Raise for fewer catch-up passes, lower for a
	// snappier worst case.
	s->lodCellsLeft = 600000;
	s->lodPending   = false;
	refineNode(s, s->quadRoot, cam, camPos, vpH, tanHalf, cam->GetParallelScale(), parallel, /*tau=*/4.0);
	if (s->lodResidentBytes > s->lodBudgetBytes) evictLRU(s);
	// Detail still owed -> finish it after the frame is on screen. Single-shot and latched, so a
	// continuous drag (many camera events, each leaving work owed) can never queue a storm of passes.
	if (s->lodPending && !s->lodTimerArmed) {
		s->lodTimerArmed = true;
		QTimer::singleShot(30, s->win, [s]() {
			if (!sceneAlive(s)) return;
			s->lodTimerArmed = false;
			refineQuadtree(s);                     // one more budgeted pass; re-arms if still owed
			if (s->widget && s->widget->renderWindow()) s->widget->renderWindow()->Render();
		});
	}
}

static void onLodCamera(vtkObject*, unsigned long, void *cd, void*) {
	refineQuadtree(static_cast<Scene*>(cd));               // the BASE surface's tile pyramid
}

// Rebuild the window's BASE grid as a flat shaded IMAGE (asImage=true) or a real 3-D SURFACE
// (asImage=false), from the grid already resident in s->gridZ + its stored CPT — no host round-trip.
// Drives the Shading dock's "Shaded image (2-D)" geometry toggle, for BOTH a 3-D-cube layer and a
// plain single grid. The two callees re-shade + sync the dock internally.
extern "C" __declspec(dllexport) int gmtvtk_show_layer_image_h(void*, const float*, int, int, double, double,
                                                    double, double, int, const double*, const double*, int, const char*);
extern "C" __declspec(dllexport) int gmtvtk_replace_base_grid_h(void*, const float*, int, int, double, double,
                                                     double, double, int, const double*, const double*, int, const char*);
extern "C" __declspec(dllexport) int gmtvtk_remove_grid_h(void*, const char*);   // the ONE layer-removal path
// fwd (defined in 90_c_api.cpp; same TU) -- the SHARED flat-image/composite builder.
static int showLayerImageTail(Scene *s, const unsigned char *rgba, int txW, int txH,
                              const float *z, int nx, int ny,
                              double x0, double x1, double y0, double y1, int geographic,
                              const double *cz, const double *crgb, int ncolor, const char *name, bool isCustom);

static void rebuildBaseFromStored(Scene *s, bool asImage);   // the base's half, defined just below

// Flip ONE grid layer between the flat 2-D image and the 3-D surface. `ex == nullptr` = the base.
//
// NO new flat-map builder and NO new surface builder here. An extra's stored state is handed to the
// SAME `gmtvtk_show_layer_image_h` / `gmtvtk_replace_base_grid_h` the base flip already goes through
// (rebuildBaseFromStored, just below), after the SAME `gmtvtk_remove_grid_h` every layer removal
// already goes through. Those two builders make the layer they are handed the window's surface —
// which IS what "show this layer as the 2-D map / as the 3-D surface" means. ExtraObj::cz/crgb exist
// for exactly one reason: to be able to hand the layer's own CPT over at this point.
static void rebuildLayerFromStored(Scene *s, ExtraObj *ex, bool asImage) {
	if (!s) return;
	if (!ex) { rebuildBaseFromStored(s, asImage); return; }
	if (ex->gridZ.empty() || ex->gnx < 2 || ex->gny < 2 || ex->cz.size() < 2) return;
	const std::vector<float>  z    = ex->gridZ;       // COPIES: the removal below frees the originals
	const std::vector<double> cz   = ex->cz;
	const std::vector<double> crgb = ex->crgb;
	const int    nx = ex->gnx, ny = ex->gny, geog = ex->geog, nc = (int)cz.size();
	const double x0 = ex->gx0, x1 = ex->gx1, y0 = ex->gy0, y1 = ex->gy1;
	const std::string nm = ex->name;
	const bool showBar = ex->showBar;                 // THIS layer's own colour-bar intent, carried across
	gmtvtk_remove_grid_h(s, nm.c_str());              // the ONE removal path (actor, axes, rows, colorbar)
	if (asImage) gmtvtk_show_layer_image_h (s, z.data(), nx, ny, x0, x1, y0, y1, geog, cz.data(), crgb.data(), nc, nm.c_str());
	else         gmtvtk_replace_base_grid_h(s, z.data(), nx, ny, x0, x1, y0, y1, geog, cz.data(), crgb.data(), nc, nm.c_str());
	// The layer that owned the colour bar was just removed and rebuilt as the window's surface, so the
	// bar has to be re-derived for whatever is active NOW — through the same refreshGridColorbar every
	// other add / hide / restack ends in. The bar belongs to the LAYER, so the layer's own intent moves
	// with it; the flip changes how the layer is drawn, never whether it wants a bar.
	s->surfShowBar = showBar;
	refreshGridColorbar(s);
	rebuildSceneObjects(s);
}

static void rebuildBaseFromStored(Scene *s, bool asImage) {
	if (!s || s->gridZ.empty() || s->gnx < 2 || s->gny < 2 || s->baseCz.size() < 2) return;
	const std::vector<float>  z    = s->gridZ;         // COPY: the callees reassign s->gridZ from this pointer
	const std::vector<double> cz   = s->baseCz;
	const std::vector<double> crgb = s->baseCrgb;
	const int nc = (int)cz.size();
	const std::string nm = s->surfName;
	if (asImage) {
		// FLAT image. A tsunami is a host-composited land/water blend with NO single CPT to re-bake from,
		// so restore it through the SAME custom builder that made it (showLayerImageTail + stored composite
		// + per-side bakeAquaShade). A plain grid uses the normal flat-image bake.
		if ((int)s->aquaBaseRGBA.size() == s->gnx * s->gny * 4) {
			showLayerImageTail(s, s->aquaBaseRGBA.data(), s->gnx, s->gny, z.data(), s->gnx, s->gny,
			                   s->gx0, s->gx1, s->gy0, s->gy1, s->baseGeog, cz.data(), crgb.data(), nc, nm.c_str(), true);
			return;
		}
		gmtvtk_show_layer_image_h(s, z.data(), s->gnx, s->gny, s->gx0, s->gx1, s->gy0, s->gy1, s->baseGeog, cz.data(), crgb.data(), nc, nm.c_str());
	} else {
		// 3-D SURFACE. SACRED LAW: the same operation uses the same function -- a tsunami warps + hillshades
		// through the EXACT builder every grid uses (gmtvtk_replace_base_grid_h -> hillshadeMapper), no
		// special case. Drop the custom-texture flag so it shades as a plain grid; the stored composite
		// (aquaBaseRGBA) survives, so re-checking "Shaded image (2-D)" restores the flat blend above.
		s->customLayerTexture = false;
		gmtvtk_replace_base_grid_h(s, z.data(), s->gnx, s->gny, s->gx0, s->gx1, s->gy0, s->gy1, s->baseGeog, cz.data(), crgb.data(), nc, nm.c_str());
	}
}

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

	// ADOPT the boxes of a dialog whose .ui carries a COPY of deps/ui/grid_line_geometry.ui (same
	// object names — that file is the canonical block to copy from). Nothing is built: the very same
	// behaviour (the dim-fun cross-recompute, the Ref-grid picker, region()/inc()/fillGeometry) is
	// attached to the widgets Designer already laid out. That is what keeps a .ui-based dialog and a
	// code-built one on ONE implementation instead of the inline re-wiring igrf_calculator had to do.
	// Returns nullptr when the host has no such block.
	static GeoGridGeometry *adopt(QWidget *host) {
		if (!host) return nullptr;
		auto *g = new GeoGridGeometry(host, Adopt{});
		if (!g->xMin || !g->xMax || !g->xInc || !g->xN || !g->yMin || !g->yMax || !g->yInc || !g->yN) {
			delete g;
			return nullptr;
		}
		return g;
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

		wireBoxes();

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
			wireRefButton(refBtn);
			// HARD RULE: an edit box must NEVER execute (no editingFinished->module read). The ref grid
			// is loaded ONLY by the "..." picker button above. See only-action-button-executes-dialog.
			outer->addLayout(refRow);
		}
	}

private:
	struct Adopt {};                       // tag: take over an existing .ui block instead of building one

	// The adopting constructor. This object is a zero-size, never-shown QWidget whose only job is to
	// own the wiring — the visible widgets belong to the host .ui.
	GeoGridGeometry(QWidget *host, Adopt) : QWidget(host) {
		hide();
		xMin = host->findChild<QLineEdit *>("xMin");  xMax = host->findChild<QLineEdit *>("xMax");
		xInc = host->findChild<QLineEdit *>("xInc");  xN   = host->findChild<QLineEdit *>("xN");
		yMin = host->findChild<QLineEdit *>("yMin");  yMax = host->findChild<QLineEdit *>("yMax");
		yInc = host->findChild<QLineEdit *>("yInc");  yN   = host->findChild<QLineEdit *>("yN");
		refEdit = host->findChild<QLineEdit *>("refEdit");
		if (!xMin || !xMax || !xInc || !xN || !yMin || !yMax || !yInc || !yN) return;
		wireBoxes();
		if (auto *refBtn = host->findChild<QToolButton *>("refBtn")) wireRefButton(refBtn);
	}

	// Cross-field recompute (Mirone dim_funs.m, now in Julia). Each box recomputes the others on
	// focus-out / Enter.
	void wireBoxes() {
		QObject::connect(xMin, &QLineEdit::editingFinished, this, [this]{ runDimFun("xMin"); });
		QObject::connect(xMax, &QLineEdit::editingFinished, this, [this]{ runDimFun("xMax"); });
		QObject::connect(yMin, &QLineEdit::editingFinished, this, [this]{ runDimFun("yMin"); });
		QObject::connect(yMax, &QLineEdit::editingFinished, this, [this]{ runDimFun("yMax"); });
		QObject::connect(xInc, &QLineEdit::editingFinished, this, [this]{ runDimFun("xInc"); });
		QObject::connect(yInc, &QLineEdit::editingFinished, this, [this]{ runDimFun("yInc"); });
		QObject::connect(xN,   &QLineEdit::editingFinished, this, [this]{ runDimFun("nCols"); });
		QObject::connect(yN,   &QLineEdit::editingFinished, this, [this]{ runDimFun("nRows"); });
	}

	// "..." picker: gmtread the chosen grid's header and fill the eight boxes from it.
	void wireRefButton(QAbstractButton *refBtn) {
		QObject::connect(refBtn, &QAbstractButton::clicked, this, [this]() {
			QString f = QFileDialog::getOpenFileName(this, "Select reference grid", prefStartDir(),
			                                         "Grid/Image files (*.nc *.grd *.tif *.tiff);;All files (*)");
			if (f.isEmpty()) return;
			rememberStartDir(f);
			if (refEdit) refEdit->setText(f);
			if (!g_juliaGridMeta) return;
			const char *m = g_juliaGridMeta(f.toUtf8().constData());
			if (m) fillGeometry(QString::fromUtf8(m));
		});
		if (refEdit) fileBoxDoubleClick(refEdit, refBtn);   // double-click in the box opens the chooser
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

		// --- Buttons. "Compute" (not "Apply"): every module dialog in this app names its action
		//     button the same, so the same word always means "run the module".
		auto *btnRow = new QHBoxLayout();
		btnRow->addStretch();
		auto *btnApply = new QPushButton("Compute", this);
		auto *btnClose = new QPushButton("Close", this);
		// HARD RULE: NO edit box may ever execute grdsample. Qt auto-promotes the first QPushButton to
		// the dialog default, so Return in ANY QLineEdit would click Compute and run the module. Disable
		// auto-default on BOTH buttons => Enter in an edit box does nothing but finish that edit.
		btnApply->setAutoDefault(false); btnApply->setDefault(false);
		btnClose->setAutoDefault(false); btnClose->setDefault(false);
		btnRow->addWidget(btnApply);
		btnRow->addWidget(btnClose);
		v->addLayout(btnRow);
		addManualButton(this, btnRow, "grdsample");   // the green ? disk, lower-left as everywhere else

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
// Reduction to the Pole / Total field to Components (Geophysics > Magnetics > rtp3d.ui) — port of
// Mirone's parker_stuff.m ('redPole'/'component' cases) + utils/mboard.m (FFT edge-padding). Loaded
// at RUNTIME via QUiLoader (same technique as IgrfDialog/MagBarcodeDialog — rtp3d.ui references only
// plain Qt widget classes, so no custom-widget QUiLoader subclass is needed here). ONE dialog class
// serves BOTH menu entries, gated by `mode`:
//   mode 0 (RTP)        -> component is forced 0; the North/East/Vert radio group (gb_component)
//                          is GRAYED (disabled, never hidden — hiding it would collapse the layout
//                          and shrink the dialog vs Components mode, breaking the SAME-dialog/
//                          SAME-size guarantee for the SAME grid; see code-sharing-is-law).
//   mode 1 (Components) -> gb_component is enabled; component comes from whichever radio is checked.
// The "Bat" row (le_bat/btn_batBrowse) is unused by EITHER mode — rtp3d() takes only the field
// grid — so it's always disabled here, exactly like parker_stuff.m's 'redPole'/'component' branch
// (`set(handles.edit_BatGrid,'Enable','off', ...)`); it stays in the .ui for a future Parker
// direct/inverse mode (parker_stuff.m's other two `what_parker` cases), which DOES need it.
// Rows/Cols/Mirror/suggested-size lists reproduce parker_stuff.m's FFT-padding UI verbatim:
// browsing a Field grid reads its native size (reusing g_juliaGridMeta, same call IgrfDialog's "OR
// Ref grid" row uses) and prefills Rows/Cols to the next 5-smooth (fast-FFT) size >= 1.2x the
// native size (utils/mboard.m's own default), with the suggested-size listboxes offering the
// native size plus every larger 5-smooth size; Mirror greys the Rows/Cols controls out (mirror
// padding always doubles the grid, no target size to pick).
// If a grid is ALREADY loaded in this window, it's assumed to be the magnetic anomaly: Field is
// locked to "In memory grid" (no Browse needed) and Rows/Cols/suggested-lists are seeded straight
// from the loaded grid's own size (scene->gnx/gny) — same "selected" sentinel convention as the
// grdsample dialog's locked input row. On Compute, `_on_rtp3d` (rtp3d.jl) resolves "selected" via
// _FIGREG, exactly like grdsample.jl's own "selected" case.
// ============================================================================================
class Rtp3DDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	int mode = 0;                                    // 0 = Reduction to the Pole, 1 = Components
	int nativeNx = 0, nativeNy = 0;                   // last-browsed field grid's own size
	bool useSelected = false;                         // Field IS the window's already-loaded grid
	QLineEdit *fieldEdit, *fieldDipEdit, *fieldDecEdit, *magDipEdit, *magDecEdit;
	QLineEdit *rowsEdit, *colsEdit;
	QListWidget *rowsList, *colsList;
	QCheckBox *mirrorChk;
	QRadioButton *rbNorth, *rbEast, *rbVert;

	// 5-smooth (prime factors <= 5) sizes GMT's FFT is fast on — Mirone utils/mboard.m's `nlist`,
	// kept identical to rtp3d.jl's own `_FFT_GOOD_SIZES` copy (host UI only needs it for the
	// suggested-size listboxes; the actual padding math runs in Julia).
	static const std::vector<int> &goodSizes() {
		static const std::vector<int> v = {
			64,72,75,80,81,90,96,100,108,120,125,128,135,144,150,160,162,180,192,200,
			216,225,240,243,250,256,270,288,300,320,324,360,375,384,400,405,432,450,480,
			486,500,512,540,576,600,625,640,648,675,720,729,750,768,800,810,864,900,960,
			972,1000,1024,1080,1125,1152,1200,1215,1250,1280,1296,1350,1440,1458,1500,
			1536,1600,1620,1728,1800,1875,1920,1944,2000,2025,2048,2160,2187,2250,2304,
			2400,2430,2500,2560,2592,2700,2880,2916,3000,3072,3125,3200,3240,3375,3456,
			3600,3645,3750,3840,3888,4000,4096,4320,4374,4500,4608,4800,4860,5000 };
		return v;
	}
	// Next 5-smooth size >= round(n*1.2) (utils/mboard.m's "find the good number ~20% larger");
	// falls back to `n` itself if that would run off the end of the table.
	static int suggestedSize(int n) {
		int target = (int)std::lround(n * 1.2);
		for (int v : goodSizes()) if (v >= target) return v;
		return n;
	}
	// Fill a listbox with [n, every good size > n], selecting `sel` if present.
	static void fillSizeList(QListWidget *lw, int n, int sel) {
		if (!lw) return;
		lw->clear();
		lw->addItem(QString::number(n));
		int selRow = (sel == n) ? 0 : -1;
		for (int v : goodSizes()) {
			if (v <= n) continue;
			lw->addItem(QString::number(v));
			if (v == sel) selRow = lw->count() - 1;
		}
		if (selRow >= 0) lw->setCurrentRow(selRow);
	}

	explicit Rtp3DDialog(QWidget *parent, Scene *scene, int m) : scn(scene), mode(m) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/rtp3d.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("Rtp3DDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("Rtp3DDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		dlg->setWindowTitle(mode == 0 ? "Reduction to the Pole" : "Component from total anomaly");
		QDialog *d = dlg;

		fieldEdit    = d->findChild<QLineEdit *>("le_field");
		fieldDipEdit = d->findChild<QLineEdit *>("le_fieldDip");
		fieldDecEdit = d->findChild<QLineEdit *>("le_fieldDec");
		magDipEdit   = d->findChild<QLineEdit *>("le_magDip");
		magDecEdit   = d->findChild<QLineEdit *>("le_magDec");
		rowsEdit = d->findChild<QLineEdit *>("le_rows"); colsEdit = d->findChild<QLineEdit *>("le_cols");
		rowsList = d->findChild<QListWidget *>("list_rowsSuggested");
		colsList = d->findChild<QListWidget *>("list_colsSuggested");
		// Tighten row spacing so more suggested sizes fit in the SAME box (no stylesheet: applying
		// one switches Qt's style engine to a path that pads rows MORE, not less, and can grow the
		// dialog's sizeHint — see respect-ui-size-no-code-override). setSpacing(0) + a slightly
		// smaller font + no reserved icon column is enough on its own.
		for (QListWidget *lw : {rowsList, colsList}) {
			if (!lw) continue;
			lw->setSpacing(0);
			lw->setIconSize(QSize(0, 0));
			QFont fnt = lw->font();
			fnt.setPointSizeF(fnt.pointSizeF() * 0.85);
			lw->setFont(fnt);
		}
		mirrorChk = d->findChild<QCheckBox *>("chk_mirror");
		rbNorth = d->findChild<QRadioButton *>("rb_northComp");
		rbEast  = d->findChild<QRadioButton *>("rb_eastComp");
		rbVert  = d->findChild<QRadioButton *>("rb_vertComp");
		auto *fieldBrowse = d->findChild<QToolButton *>("btn_fieldBrowse");
		auto *computeBtn  = d->findChild<QPushButton *>("btn_compute");

		// "Bat" row: not used by rtp3d() in either mode — reserved for a future Parker
		// direct/inverse mode (parker_stuff.m), which DOES need a bathymetry grid.
		if (auto *w = d->findChild<QLineEdit *>("le_bat"))     w->setEnabled(false);
		if (auto *w = d->findChild<QToolButton *>("btn_batBrowse")) w->setEnabled(false);
		if (auto *w = d->findChild<QLabel *>("lbl_bat"))       w->setEnabled(false);

		// Component radio group only APPLIES in Components mode — but stays VISIBLE (just grayed)
		// in RTP mode, never hidden, so the dialog's layout/size is identical in both modes for the
		// same grid (setVisible(false) here previously collapsed the layout and shrank the RTP-mode
		// window — a same-dialog-different-size regression).
		if (auto *gbComp = d->findChild<QGroupBox *>("gb_component")) gbComp->setEnabled(mode != 0);

		// A grid is already loaded in THIS window -> assume it's the magnetic anomaly and use it
		// directly (same "selected" convention as the grdsample dialog above): Field is locked to
		// "In memory grid", and its native rows/cols (scene->gnx/gny, the full-res data layer) seed
		// the Rows/Cols boxes + suggested-size lists immediately, with no Browse round-trip needed.
		if (scene && scene->surf && !scene->emptyStart && !scene->imageOnly) {
			useSelected = true;
			fieldEdit->setText("In memory grid");
			fieldEdit->setReadOnly(true);
			fieldEdit->setEnabled(false);   // grayed
			if (fieldBrowse) fieldBrowse->setEnabled(false);
			if (scene->gnx > 1 && scene->gny > 1) {
				nativeNx = scene->gnx; nativeNy = scene->gny;
				int sugX = suggestedSize(nativeNx), sugY = suggestedSize(nativeNy);
				if (colsEdit) colsEdit->setText(QString::number(sugX));
				if (rowsEdit) rowsEdit->setText(QString::number(sugY));
				fillSizeList(colsList, nativeNx, sugX);
				fillSizeList(rowsList, nativeNy, sugY);
			}

			// Geographic grid -> check the box, and default BOTH the ambient-field and the
			// magnetization Dip/Dec boxes to the IGRF 2000 values at the grid's centre (induced
			// magnetization is the overwhelming default assumption — the user can still overtype
			// either pair). Same g_juliaIgrfPoint round-trip as IgrfDialog::recompute() above:
			// "lon/lat/elev_m/date_dec" -> "F/H/X/Y/Z/D/I", D/I are the last two fields.
			if (scene->hasCRS()) {
				if (auto *geoChk = d->findChild<QCheckBox *>("chk_geographicCoords")) geoChk->setChecked(true);
				double cx = 0, cy = 0; bool haveCenter = false;
				if (scene->gx1 > scene->gx0 && scene->gy1 > scene->gy0) {
					cx = (scene->gx0 + scene->gx1) / 2.0; cy = (scene->gy0 + scene->gy1) / 2.0; haveCenter = true;
				} else if (scene->x1 > scene->x0 && scene->y1 > scene->y0) {
					cx = (scene->x0 + scene->x1) / 2.0; cy = (scene->y0 + scene->y1) / 2.0; haveCenter = true;
				}
				if (haveCenter && g_juliaIgrfPoint) {
					QString state = QString("%1/%2/%3/%4").arg(cx, 0, 'g', 10).arg(cy, 0, 'g', 10)
					                                      .arg(0.0, 0, 'g', 10).arg(2000.0, 0, 'g', 10);
					const char *out = g_juliaIgrfPoint(state.toUtf8().constData());
					if (out) {
						const QStringList r = QString::fromUtf8(out).split('/');   // copy immediately (Julia-owned buffer)
						if (r.size() >= 7) {
							QString dec = QString::number(r[5].toDouble(), 'f', 2);
							QString inc = QString::number(r[6].toDouble(), 'f', 2);
							if (fieldDipEdit) fieldDipEdit->setText(inc);
							if (fieldDecEdit) fieldDecEdit->setText(dec);
							if (magDipEdit)   magDipEdit->setText(inc);
							if (magDecEdit)   magDecEdit->setText(dec);
						}
					}
				}
			}
		}

		auto setRowsColsEnabled = [this](bool on) {
			if (rowsEdit) rowsEdit->setEnabled(on);
			if (colsEdit) colsEdit->setEnabled(on);
			if (rowsList) rowsList->setEnabled(on);
			if (colsList) colsList->setEnabled(on);
		};
		if (mirrorChk) QObject::connect(mirrorChk, &QCheckBox::toggled, d, [setRowsColsEnabled](bool on) {
			setRowsColsEnabled(!on);   // mirror padding always doubles the grid — no target size to pick
		});

		if (rowsList) QObject::connect(rowsList, &QListWidget::currentTextChanged, d, [this](const QString &t) {
			if (rowsEdit && !t.isEmpty()) rowsEdit->setText(t);
		});
		if (colsList) QObject::connect(colsList, &QListWidget::currentTextChanged, d, [this](const QString &t) {
			if (colsEdit && !t.isEmpty()) colsEdit->setText(t);
		});

		if (fieldBrowse) QObject::connect(fieldBrowse, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getOpenFileName(d, "Select total-field anomaly grid", prefStartDir(),
			                                          "Grid files (*.nc *.grd *.tif *.tiff);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			fieldEdit->setText(fn);
			if (!g_juliaGridMeta) return;
			const char *m = g_juliaGridMeta(fn.toUtf8().constData());
			if (!m) return;
			const QStringList meta = QString::fromUtf8(m).split('/');    // copy immediately (Julia-owned buffer)
			if (meta.size() < 8) return;
			nativeNx = meta[6].toInt(); nativeNy = meta[7].toInt();
			if (nativeNx <= 0 || nativeNy <= 0) return;
			int sugX = suggestedSize(nativeNx), sugY = suggestedSize(nativeNy);
			if (colsEdit) colsEdit->setText(QString::number(sugX));
			if (rowsEdit) rowsEdit->setText(QString::number(sugY));
			fillSizeList(colsList, nativeNx, sugX);
			fillSizeList(rowsList, nativeNy, sugY);
		});

		if (computeBtn) QObject::connect(computeBtn, &QPushButton::clicked, d, [this, d]() {
			if (!g_juliaRtp3D) {
				QMessageBox::warning(d, "Error", "RTP: callback not registered (rebuild/restart needed?).");
				return;
			}
			QString field = useSelected ? QString("selected") : fieldEdit->text().trimmed();
			if (field.isEmpty()) {
				QMessageBox::warning(d, "Error", "You didn't give me a Field grid. What do you want me to do?");
				return;
			}
			bool okA, okB, okC, okE;
			double fDip = fieldDipEdit->text().toDouble(&okA), fDec = fieldDecEdit->text().toDouble(&okB);
			if (!okA || !okB) {
				QMessageBox::warning(d, "Error", "You need to give me valid magnetic field Inclination and Declination.");
				return;
			}
			double mDip = magDipEdit->text().toDouble(&okC), mDec = magDecEdit->text().toDouble(&okE);
			if (!okC || !okE) {
				QMessageBox::warning(d, "Error", "You need to give me valid magnetization Inclination and Declination.");
				return;
			}
			// Components mode: the 3 boxes are NOT mutually exclusive (autoExclusive=false in the .ui,
			// same as independent checkboxes) — ANY subset can be checked, and EVERY checked one must
			// get its OWN Compute run / its OWN grid. The old code picked a SINGLE component via a
			// ternary chain, so checking all 3 silently computed only North — that bug, not the
			// widgets, is what's fixed here: collect every checked component and run each in turn.
			std::vector<int> components;
			if (mode == 0) {
				components.push_back(0);
			} else {
				if (rbNorth->isChecked()) components.push_back(1);
				if (rbEast->isChecked())  components.push_back(2);
				if (rbVert->isChecked())  components.push_back(3);
				if (components.empty()) {
					QMessageBox::warning(d, "Error", "Check at least one component (North/East/Vert).");
					return;
				}
			}
			int newRows = rowsEdit ? rowsEdit->text().toInt() : 0;
			int newCols = colsEdit ? colsEdit->text().toInt() : 0;
			bool mirror = mirrorChk && mirrorChk->isChecked();
			static const char *compName[] = { "RTP", "North component", "East component", "Vertical component" };
			// The result (success/failure) matters HERE, on the dialog the user is actually looking
			// at — not just as a status-bar flash on the (possibly backgrounded/unnoticed) parent
			// viewer window, which is why a real Compute failure was previously invisible.
			showBusyDialog(mode == 0 ? "Computing RTP…" : "Computing components…");
			QStringList failed;
			for (int component : components) {
				QString params = QString("%1;%2;%3;%4;%5;%6;%7;%8;%9")
					.arg(field).arg(fDip).arg(fDec).arg(mDip).arg(mDec)
					.arg(component).arg(newRows).arg(newCols).arg(mirror ? "1" : "0");
				const int ok = g_juliaRtp3D(scn, params.toUtf8().constData());
				if (!ok) failed << compName[component];
			}
			closeBusyDialog();
			// Dialog stays OPEN — only the user closes it (never auto-close on success; the user may
			// want to run more components/variants on the same field without reopening).
			if (failed.isEmpty()) {
				QMessageBox::information(d, mode == 0 ? "RTP" : "Components",
					QString("Done — added to Scene Objects as %1. %2 start%3 UNCHECKED (hidden) so "
					        "it doesn't overlap what's already shown — tick its box to view it.")
						.arg(components.size() > 1 ? "new grids" : "a new grid")
						.arg(components.size() > 1 ? "They" : "It")
						.arg(components.size() > 1 ? "" : "s"));
			} else {
				QMessageBox::warning(d, "Error",
					QString("Failed: %1 — see this window's Errors console for details.").arg(failed.join(", ")));
			}
		});

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}
};

// ============================================================================================
// Gravity/Magnetic anomaly of a 3-D body (Geophysics > Magnetics > gmtgravmag3d) — front-end for
// GMT's gmtgravmag3d (Okabe 1979), driven through GMT.jl's `gravmag3d` (src/gravmag3d.jl).
// Loaded at RUNTIME via QUiLoader from deps/ui/gravmag3d_dialog.ui (plain Qt widget classes only,
// same technique as Rtp3DDialog/ClipGridDialog above).
//
// Three mutually-exclusive body sources, one per radio (GMT itself accepts exactly one):
//   rb_geomBody -> M|body: a LIST of geometric shapes (prism/sphere/…), each "shape,params"; the
//                  module takes any number of them, all sharing one density / one set of magnetic
//                  parameters (the module has no way to give each body its own — see the .qmd).
//   rb_file     -> T+r|raw_triang, T+v|index or T+s|stl, picked by cb_fileKind.
//   rb_memFV    -> the GMTfv solid already loaded in THIS window (resolved Julia-side from _FIGREG);
//                  gravmag3d only accepts triangles, so a quadrangle solid (cube) is rejected there.
// Gravity XOR magnetic is the second exclusive pair (C|density vs H|mag_params are mutually
// exclusive in GMT) — the unselected side's boxes are GRAYED, never hidden, so the dialog keeps one
// size in both modes (same rule as Rtp3DDialog's component group).
//
// Only Compute runs anything (only-action-button-executes-dialog); the dialog stays open across
// runs so several bodies/parameter variants can be tried without reopening. The result grid is
// added to the window by Julia (SACRED_LAW derived-variable display law, handled in _on_gravmag3d);
// a Track (F) run has no grid at all and comes back as a table window instead. Success is silent —
// the new grid appearing in the window IS the confirmation; only a failure gets a modal.
// ============================================================================================

// Every parameter the dialog holds, kept for the LIFETIME OF THE PROCESS: the dialog is deleted when
// closed (WA_DeleteOnClose), so reopening it from the menu must restore what the user had typed from
// here, not from the dead widget tree. Written on Compute and on close, read by the constructor.
struct GravMag3DState {
	bool valid = false;
	int shape = 0, fileKind = 0, bodyMode = 0;          // bodyMode: 0 = geometric, 1 = file, 2 = in-window FV
	bool grav = true, onebased = false, noswap = false, geog = false;
	QStringList bodies;
	QString params, bodyFile, density, fDec, fDip, mInt, mDec, mDip;
	QString xmin, xmax, ymin, ymax, xinc, yinc, zobs, level, thickness, radius, track, outfile;
};
static GravMag3DState g_gm3dState;

class GravMag3DDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QComboBox *shapeCb, *fileKindCb;
	QLineEdit *paramsEdit, *bodyFileEdit, *densityEdit;
	QLineEdit *fDecEdit, *fDipEdit, *mIntEdit, *mDecEdit, *mDipEdit;
	QLineEdit *xminEdit, *xmaxEdit, *yminEdit, *ymaxEdit, *xincEdit, *yincEdit;
	QLineEdit *zobsEdit, *levelEdit, *thickEdit, *radiusEdit, *trackEdit, *outfileEdit;
	QListWidget *bodyList;
	QRadioButton *rbGeom, *rbFile, *rbMemFV, *rbGrav, *rbMag;
	QCheckBox *oneBasedChk, *noSwapChk, *geogChk;

	// Slash-separated parameter template of each shape, verbatim from the gmtgravmag3d docs
	// (optional trailing fields in brackets). Shown as the params box's placeholder so the order
	// is never a guess; indices match cb_shape's item order in the .ui.
	static const char *paramHint(const QString &shape) {
		if (shape == "prism")     return "side_x/side_y/side_z/z0[/x0/y0]";
		if (shape == "sphere")    return "rad/z_center[/x0/y0/npts/n_slices]";
		if (shape == "ellipsoid") return "semi_x/semi_y/semi_z/z_center[/x0/y0/npts/n_slices]";
		if (shape == "cylinder")  return "rad/height/z0[/x0/y0/npts/n_slices]";
		if (shape == "cone")      return "semi_x/semi_y/height/z0[/x0/y0/npts]";
		if (shape == "pyramid")   return "side_x/side_y/height/z0[/x0/y0]";
		if (shape == "bell")      return "height/sx/sy/z0[/x0/y0/n_sig/npts/n_slices]";
		return "";
	}

	explicit GravMag3DDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/gravmag3d_dialog.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("GravMag3DDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("GravMag3DDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;

		shapeCb     = d->findChild<QComboBox *>("cb_shape");
		fileKindCb  = d->findChild<QComboBox *>("cb_fileKind");
		paramsEdit  = d->findChild<QLineEdit *>("le_bodyParams");
		bodyFileEdit= d->findChild<QLineEdit *>("le_bodyFile");
		densityEdit = d->findChild<QLineEdit *>("le_density");
		fDecEdit = d->findChild<QLineEdit *>("le_fDec");  fDipEdit = d->findChild<QLineEdit *>("le_fDip");
		mIntEdit = d->findChild<QLineEdit *>("le_mInt");  mDecEdit = d->findChild<QLineEdit *>("le_mDec");
		mDipEdit = d->findChild<QLineEdit *>("le_mDip");
		xminEdit = d->findChild<QLineEdit *>("le_xmin");  xmaxEdit = d->findChild<QLineEdit *>("le_xmax");
		yminEdit = d->findChild<QLineEdit *>("le_ymin");  ymaxEdit = d->findChild<QLineEdit *>("le_ymax");
		xincEdit = d->findChild<QLineEdit *>("le_xinc");  yincEdit = d->findChild<QLineEdit *>("le_yinc");
		zobsEdit = d->findChild<QLineEdit *>("le_zobs");  levelEdit = d->findChild<QLineEdit *>("le_level");
		thickEdit= d->findChild<QLineEdit *>("le_thickness"); radiusEdit = d->findChild<QLineEdit *>("le_radius");
		trackEdit= d->findChild<QLineEdit *>("le_track"); outfileEdit = d->findChild<QLineEdit *>("le_outfile");
		bodyList = d->findChild<QListWidget *>("list_bodies");
		rbGeom = d->findChild<QRadioButton *>("rb_geomBody");
		rbFile = d->findChild<QRadioButton *>("rb_file");
		rbMemFV= d->findChild<QRadioButton *>("rb_memFV");
		rbGrav = d->findChild<QRadioButton *>("rb_gravity");
		rbMag  = d->findChild<QRadioButton *>("rb_magnetic");
		oneBasedChk = d->findChild<QCheckBox *>("chk_onebased");
		noSwapChk   = d->findChild<QCheckBox *>("chk_noswap");
		geogChk     = d->findChild<QCheckBox *>("chk_geog");
		auto *addBtn   = d->findChild<QToolButton *>("btn_addBody");
		auto *delBtn   = d->findChild<QToolButton *>("btn_delBody");
		auto *fileBtn  = d->findChild<QToolButton *>("btn_bodyFileBrowse");
		auto *densBtn  = d->findChild<QToolButton *>("btn_densityBrowse");
		auto *trackBtn = d->findChild<QToolButton *>("btn_trackBrowse");
		auto *outBtn   = d->findChild<QToolButton *>("btn_outfileBrowse");
		auto *computeBtn = d->findChild<QPushButton *>("btn_compute");
		auto *closeBtn   = d->findChild<QPushButton *>("btn_close");

		if (bodyList) bodyList->setSpacing(0);
		if (shapeCb && paramsEdit) {
			paramsEdit->setPlaceholderText(paramHint(shapeCb->currentText()));
			QObject::connect(shapeCb, &QComboBox::currentTextChanged, d, [this](const QString &t) {
				paramsEdit->setPlaceholderText(paramHint(t));
			});
		}

		// A grid/surface is already loaded here -> seed the output Region from what the window shows
		// (the anomaly is nearly always wanted over the SAME area), and pre-tick Geographic when the
		// scene carries a CRS. Nothing is forced: every box stays editable. A remembered state (below)
		// wins over this seed — what the user typed themselves is never overwritten by the guess.
		if (scene && scene->surf && !scene->emptyStart) {
			double x0 = scene->gx0, x1 = scene->gx1, y0 = scene->gy0, y1 = scene->gy1;
			if (!(x1 > x0 && y1 > y0)) { x0 = scene->x0; x1 = scene->x1; y0 = scene->y0; y1 = scene->y1; }
			if (x1 > x0 && y1 > y0) {
				xminEdit->setText(QString::number(x0, 'g', 10)); xmaxEdit->setText(QString::number(x1, 'g', 10));
				yminEdit->setText(QString::number(y0, 'g', 10)); ymaxEdit->setText(QString::number(y1, 'g', 10));
				// ...and the increment from that same grid, so Region and Increment always describe one
				// consistent thing instead of a seeded region next to an arbitrary default step.
				const double dx = scene->gdx > 0 ? scene->gdx : (scene->gnx > 1 ? (x1 - x0) / (scene->gnx - 1) : 0);
				const double dy = scene->gdy > 0 ? scene->gdy : (scene->gny > 1 ? (y1 - y0) / (scene->gny - 1) : 0);
				if (dx > 0 && dy > 0) {
					xincEdit->setText(QString::number(dx, 'g', 10));
					yincEdit->setText(QString::number(dy, 'g', 10));
				}
			}
			if (scene->hasCRS() && geogChk) geogChk->setChecked(true);
		}

		// --- restore what the user last had (any previous incarnation of this dialog, any window).
		const GravMag3DState &st = g_gm3dState;
		if (st.valid) {
			if (shapeCb)    shapeCb->setCurrentIndex(st.shape);
			if (fileKindCb) fileKindCb->setCurrentIndex(st.fileKind);
			if (bodyList)   bodyList->addItems(st.bodies);
			paramsEdit->setText(st.params);      bodyFileEdit->setText(st.bodyFile);
			densityEdit->setText(st.density);
			fDecEdit->setText(st.fDec);  fDipEdit->setText(st.fDip);  mIntEdit->setText(st.mInt);
			mDecEdit->setText(st.mDec);  mDipEdit->setText(st.mDip);
			xminEdit->setText(st.xmin);  xmaxEdit->setText(st.xmax);
			yminEdit->setText(st.ymin);  ymaxEdit->setText(st.ymax);
			xincEdit->setText(st.xinc);  yincEdit->setText(st.yinc);
			zobsEdit->setText(st.zobs);  levelEdit->setText(st.level);
			thickEdit->setText(st.thickness); radiusEdit->setText(st.radius);
			trackEdit->setText(st.track);     outfileEdit->setText(st.outfile);
			oneBasedChk->setChecked(st.onebased); noSwapChk->setChecked(st.noswap);
			geogChk->setChecked(st.geog);
			QRadioButton *body = st.bodyMode == 1 ? rbFile : st.bodyMode == 2 ? rbMemFV : rbGeom;
			if (body) body->setChecked(true);
			if (st.grav) { if (rbGrav) rbGrav->setChecked(true); }
			else         { if (rbMag)  rbMag->setChecked(true);  }
		}

		// Snapshot everything back into g_gm3dState. Called on Compute AND on close, so both "run it,
		// tweak, run again later" and "closed it by mistake" come back with the same parameters.
		auto saveState = [this]() {
			GravMag3DState s;
			s.valid = true;
			s.shape = shapeCb ? shapeCb->currentIndex() : 0;
			s.fileKind = fileKindCb ? fileKindCb->currentIndex() : 0;
			s.bodyMode = (rbFile && rbFile->isChecked()) ? 1 : (rbMemFV && rbMemFV->isChecked()) ? 2 : 0;
			s.grav = !(rbMag && rbMag->isChecked());
			s.onebased = oneBasedChk->isChecked();  s.noswap = noSwapChk->isChecked();
			s.geog = geogChk->isChecked();
			for (int i = 0; bodyList && i < bodyList->count(); ++i) s.bodies << bodyList->item(i)->text();
			s.params = paramsEdit->text();   s.bodyFile = bodyFileEdit->text();
			s.density = densityEdit->text();
			s.fDec = fDecEdit->text();  s.fDip = fDipEdit->text();  s.mInt = mIntEdit->text();
			s.mDec = mDecEdit->text();  s.mDip = mDipEdit->text();
			s.xmin = xminEdit->text();  s.xmax = xmaxEdit->text();
			s.ymin = yminEdit->text();  s.ymax = ymaxEdit->text();
			s.xinc = xincEdit->text();  s.yinc = yincEdit->text();
			s.zobs = zobsEdit->text();  s.level = levelEdit->text();
			s.thickness = thickEdit->text();  s.radius = radiusEdit->text();
			s.track = trackEdit->text();      s.outfile = outfileEdit->text();
			g_gm3dState = s;
		};
		// Close (X button or the Close push button, both end up as a QEvent::Close) is where the
		// snapshot has to happen: `destroyed` fires from ~QObject, with the QDialog part of the object
		// already gone, so reading the widgets there would be reading through a half-dead object.
		struct GravMag3DSaveOnClose : QObject {
			std::function<void()> save;
			GravMag3DSaveOnClose(QObject *p, std::function<void()> fn) : QObject(p), save(fn) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close) save();
				return QObject::eventFilter(o, e);
			}
		};
		d->installEventFilter(new GravMag3DSaveOnClose(d, saveState));

		// --- body-source gating: each radio owns its own row(s), the other rows gray out.
		auto syncBodyMode = [this, addBtn, delBtn, fileBtn]() {
			const bool geom = rbGeom && rbGeom->isChecked();
			const bool file = rbFile && rbFile->isChecked();
			if (shapeCb)    shapeCb->setEnabled(geom);
			if (paramsEdit) paramsEdit->setEnabled(geom);
			if (bodyList)   bodyList->setEnabled(geom);
			if (addBtn)     addBtn->setEnabled(geom);
			if (delBtn)     delBtn->setEnabled(geom);
			if (fileKindCb)   fileKindCb->setEnabled(file);
			if (bodyFileEdit) bodyFileEdit->setEnabled(file);
			if (fileBtn)      fileBtn->setEnabled(file);
			// 1-based indices / clockwise facets only mean anything for the surface formats.
			if (oneBasedChk) oneBasedChk->setEnabled(file || (rbMemFV && rbMemFV->isChecked()));
			if (noSwapChk)   noSwapChk->setEnabled(file);
		};
		for (QRadioButton *rb : {rbGeom, rbFile, rbMemFV})
			if (rb) QObject::connect(rb, &QRadioButton::toggled, d, [syncBodyMode](bool) { syncBodyMode(); });
		syncBodyMode();

		// --- gravity XOR magnetic: the idle side is grayed, never hidden (constant dialog size).
		auto syncAnomMode = [this, densBtn]() {
			const bool grav = rbGrav && rbGrav->isChecked();
			if (densityEdit) densityEdit->setEnabled(grav);
			if (densBtn)     densBtn->setEnabled(grav);
			for (QLineEdit *e : {fDecEdit, fDipEdit, mIntEdit, mDecEdit, mDipEdit})
				if (e) e->setEnabled(!grav);
		};
		for (QRadioButton *rb : {rbGrav, rbMag})
			if (rb) QObject::connect(rb, &QRadioButton::toggled, d, [syncAnomMode](bool) { syncAnomMode(); });
		syncAnomMode();

		// --- body list: Add appends "shape,params", Del drops the selected row.
		if (addBtn) QObject::connect(addBtn, &QToolButton::clicked, d, [this, d]() {
			const QString params = paramsEdit->text().trimmed();
			if (params.isEmpty()) {
				QMessageBox::warning(d, "Error", QString("Give the %1 parameters first: %2")
					.arg(shapeCb->currentText()).arg(paramHint(shapeCb->currentText())));
				return;
			}
			bodyList->addItem(shapeCb->currentText() + "," + params);
			paramsEdit->clear();
		});
		if (delBtn) QObject::connect(delBtn, &QToolButton::clicked, d, [this]() {
			delete bodyList->takeItem(bodyList->currentRow());   // takeItem(-1) returns nullptr, delete is a no-op
		});

		if (fileBtn) QObject::connect(fileBtn, &QToolButton::clicked, d, [this, d]() {
			const QString filt = fileKindCb->currentIndex() == 2
				? "STL files (*.stl *.STL);;All files (*)"
				: "Triangle files (*.dat *.txt *.xyz);;All files (*)";
			QString fn = QFileDialog::getOpenFileName(d, "Select body file", prefStartDir(), filt);
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			bodyFileEdit->setText(fn);
		});
		if (densBtn) QObject::connect(densBtn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getOpenFileName(d, "Select variable-density grid", prefStartDir(),
			                                          "Grid files (*.nc *.grd *.tif *.tiff);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			densityEdit->setText(fn);
		});
		if (trackBtn) QObject::connect(trackBtn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getOpenFileName(d, "Select x,y locations file", prefStartDir(),
			                                          "Data files (*.dat *.txt *.xy);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			trackEdit->setText(fn);
		});
		if (outBtn) QObject::connect(outBtn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getSaveFileName(d, "Save result as", prefStartDir(),
			                                          "Grid files (*.nc *.grd);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			outfileEdit->setText(fn);
		});

		// STANDING RULE: an "OR Ref grid" row under every Region.
		addRefGridRow(d, d->findChild<QGridLayout *>("gridLayout_region"),
		              xminEdit, xmaxEdit, yminEdit, ymaxEdit, xincEdit, yincEdit);
		addManualButton(d, "gmtgravmag3d");

		// Standing rule: double-clicking any file box opens the same chooser its "..." button does.
		fileBoxDoubleClick(bodyFileEdit, fileBtn);
		fileBoxDoubleClick(densityEdit,  densBtn);
		fileBoxDoubleClick(trackEdit,    trackBtn);
		fileBoxDoubleClick(outfileEdit,  outBtn);

		if (closeBtn) QObject::connect(closeBtn, &QPushButton::clicked, d, [d]() { d->close(); });

		if (computeBtn) QObject::connect(computeBtn, &QPushButton::clicked, d, [this, d, saveState]() {
			if (!g_juliaGravMag3D) {
				QMessageBox::warning(d, "Error", "gravmag3d: callback not registered (rebuild/restart needed?).");
				return;
			}
			saveState();
			const bool toTrack = !trackEdit->text().trimmed().isEmpty();

			QString bodyKind, bodies, bodyFile, fileKind;
			if (rbFile && rbFile->isChecked()) {
				bodyKind = "file";
				bodyFile = bodyFileEdit->text().trimmed();
				if (bodyFile.isEmpty()) {
					QMessageBox::warning(d, "Error", "Pick the file that describes the body surface.");
					return;
				}
				fileKind = fileKindCb->currentIndex() == 1 ? "index" : fileKindCb->currentIndex() == 2 ? "stl" : "raw";
			}
			else if (rbMemFV && rbMemFV->isChecked()) {
				bodyKind = "memfv";
			}
			else {
				bodyKind = "geom";
				QStringList lst;
				for (int i = 0; i < bodyList->count(); ++i) lst << bodyList->item(i)->text();
				// Convenience only: a shape typed but never "Add"ed is still what the user meant when
				// the list is empty — no silent drop, and no forcing an extra click for one body.
				if (lst.isEmpty() && !paramsEdit->text().trimmed().isEmpty())
					lst << shapeCb->currentText() + "," + paramsEdit->text().trimmed();
				if (lst.isEmpty()) {
					QMessageBox::warning(d, "Error", "No body defined: pick a shape, type its parameters and press Add.");
					return;
				}
				bodies = lst.join('|');
			}

			const bool grav = rbGrav && rbGrav->isChecked();
			if (grav && densityEdit->text().trimmed().isEmpty()) {
				QMessageBox::warning(d, "Error", "A gravity anomaly needs a density (constant in SI, or a grid).");
				return;
			}
			QString magParams;
			if (!grav) {
				QStringList mp;
				for (QLineEdit *e : {fDecEdit, fDipEdit, mIntEdit, mDecEdit, mDipEdit}) {
					bool ok = false;
					e->text().trimmed().toDouble(&ok);
					if (!ok) {
						QMessageBox::warning(d, "Error", "All five magnetic parameters must be numbers.");
						return;
					}
					mp << e->text().trimmed();
				}
				magParams = mp.join('/');
			}

			// Region/increment are what the grid gets sampled on — meaningless (and not asked for)
			// when the anomaly is computed at the Track locations instead.
			QString region, inc;
			if (!toTrack) {
				const QString xm = xminEdit->text().trimmed(), xM = xmaxEdit->text().trimmed();
				const QString ym = yminEdit->text().trimmed(), yM = ymaxEdit->text().trimmed();
				if (xm.isEmpty() || xM.isEmpty() || ym.isEmpty() || yM.isEmpty()) {
					QMessageBox::warning(d, "Error", "Fill the output Region (xmin xmax ymin ymax) in the Output tab.");
					return;
				}
				region = xm + "/" + xM + "/" + ym + "/" + yM;
				const QString xi = xincEdit->text().trimmed();
				if (xi.isEmpty()) {
					QMessageBox::warning(d, "Error", "Give the output grid increment in the Output tab.");
					return;
				}
				inc = yincEdit->text().trimmed().isEmpty() ? xi : xi + "/" + yincEdit->text().trimmed();
			}

			// Newline-separated key=value block (same shape as the seismicity dialog's payload): the
			// option set here is far too wide for a positional list to stay readable on either side.
			QStringList kv;
			kv << "bodykind=" + bodyKind;
			if (!bodies.isEmpty())   kv << "bodies=" + bodies;
			if (!bodyFile.isEmpty()) { kv << "file=" + bodyFile; kv << "filekind=" + fileKind; }
			kv << QString("onebased=%1").arg(oneBasedChk->isChecked() && oneBasedChk->isEnabled() ? 1 : 0);
			kv << QString("noswap=%1").arg(noSwapChk->isChecked() && noSwapChk->isEnabled() ? 1 : 0);
			kv << QString("mode=") + (grav ? "grav" : "mag");
			if (grav) kv << "density=" + densityEdit->text().trimmed();
			else      kv << "magparams=" + magParams;
			if (!region.isEmpty()) kv << "region=" + region;
			if (!inc.isEmpty())    kv << "inc=" + inc;
			kv << QString("geog=%1").arg(geogChk->isChecked() ? 1 : 0);
			for (auto pair : { std::make_pair(QString("zobs"), zobsEdit), std::make_pair(QString("level"), levelEdit),
			                   std::make_pair(QString("thickness"), thickEdit), std::make_pair(QString("radius"), radiusEdit),
			                   std::make_pair(QString("track"), trackEdit), std::make_pair(QString("outfile"), outfileEdit) }) {
				const QString v = pair.second->text().trimmed();
				if (!v.isEmpty()) kv << pair.first + "=" + v;
			}

			showBusyDialog("Computing the anomaly…");
			const int ok = g_juliaGravMag3D(scn, kv.join('\n').toUtf8().constData());
			closeBusyDialog();
			// Success says nothing: the new grid landing in the window (or the table tab, for a track
			// run) is the confirmation. A FAILURE still gets a modal here — the alternative feedback
			// lives in the parent viewer's Errors console, which may well be behind this window.
			if (!ok)
				QMessageBox::warning(d, "Error", "gravmag3d failed — see this window's Errors console for details.");
		});

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}
};

// ============================================================================================
// Gravity/Magnetic anomaly of a body described by GRIDS (Geophysics > Magnetics > grdgravmag3d) —
// GMT's grdgravmag3d (same Okabe 1979 engine as gmtgravmag3d, different input), through GMT.jl's
// `grdgravmag3d` (src/grdgravmag3d.jl). Runtime .ui load, exactly like GravMag3DDialog above; the
// difference is entirely in WHAT the body is:
//   one grid  -> the anomaly of a constant-thickness layer under that surface (E|thickness), or of
//                the volume closed at its bottom/top (Z|level = b|t)
//   two grids -> the anomaly of the volume BETWEEN top and bottom (thickness/radius then apply as
//                the module documents)
// Gravity XOR magnetic again, but -H here is richer than gmtgravmag3d's: besides the five angles it
// takes a component letter, an intensity grid (+m) and an IGRF-variable ambient field (+i/+n), and
// GMT accepts SEVERAL -H at once — hence separate controls, assembled Julia-side into the one
// mag_params string (the module's own documented "z -H+n -H+mmag.grd" idiom).
// Success is silent, failures get a modal, and the parameters survive close/reopen — same three
// rules as GravMag3DDialog, for the same reasons.
// ============================================================================================
struct GrdGravMag3DState {
	bool valid = false;
	int zlevel = 0, component = 0, igrf = 0;      // combo indices
	bool topFromFile = false, useBottom = false, grav = true, geog = false;
	QString topFile, botFile, thickness, pad, density, magGrid;
	QString fDec, fDip, mInt, mDec, mDip;
	QString xmin, xmax, ymin, ymax, xinc, yinc, zobs, radius, threads, track, outfile;
};
static GrdGravMag3DState g_grdgm3dState;

class GrdGravMag3DDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QRadioButton *rbTopMem, *rbTopFile, *rbGrav, *rbMag;
	QCheckBox *useBottomChk, *geogChk;
	QComboBox *zlevelCb, *componentCb, *igrfCb;
	QLineEdit *topEdit, *botEdit, *thickEdit, *padEdit, *densityEdit, *magGridEdit;
	QLineEdit *fDecEdit, *fDipEdit, *mIntEdit, *mDecEdit, *mDipEdit;
	QLineEdit *xminEdit, *xmaxEdit, *yminEdit, *ymaxEdit, *xincEdit, *yincEdit;
	QLineEdit *zobsEdit, *radiusEdit, *threadsEdit, *trackEdit, *outfileEdit;

	explicit GrdGravMag3DDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/grdgravmag3d_dialog.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("GrdGravMag3DDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("GrdGravMag3DDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;

		rbTopMem  = d->findChild<QRadioButton *>("rb_topMem");
		rbTopFile = d->findChild<QRadioButton *>("rb_topFile");
		rbGrav    = d->findChild<QRadioButton *>("rb_gravity");
		rbMag     = d->findChild<QRadioButton *>("rb_magnetic");
		useBottomChk = d->findChild<QCheckBox *>("chk_useBottom");
		geogChk      = d->findChild<QCheckBox *>("chk_geog");
		zlevelCb    = d->findChild<QComboBox *>("cb_zlevel");
		componentCb = d->findChild<QComboBox *>("cb_component");
		igrfCb      = d->findChild<QComboBox *>("cb_igrf");
		topEdit    = d->findChild<QLineEdit *>("le_topFile");   botEdit = d->findChild<QLineEdit *>("le_botFile");
		thickEdit  = d->findChild<QLineEdit *>("le_thickness"); padEdit = d->findChild<QLineEdit *>("le_pad");
		densityEdit= d->findChild<QLineEdit *>("le_density");   magGridEdit = d->findChild<QLineEdit *>("le_magGrid");
		fDecEdit = d->findChild<QLineEdit *>("le_fDec");  fDipEdit = d->findChild<QLineEdit *>("le_fDip");
		mIntEdit = d->findChild<QLineEdit *>("le_mInt");  mDecEdit = d->findChild<QLineEdit *>("le_mDec");
		mDipEdit = d->findChild<QLineEdit *>("le_mDip");
		xminEdit = d->findChild<QLineEdit *>("le_xmin");  xmaxEdit = d->findChild<QLineEdit *>("le_xmax");
		yminEdit = d->findChild<QLineEdit *>("le_ymin");  ymaxEdit = d->findChild<QLineEdit *>("le_ymax");
		xincEdit = d->findChild<QLineEdit *>("le_xinc");  yincEdit = d->findChild<QLineEdit *>("le_yinc");
		zobsEdit = d->findChild<QLineEdit *>("le_zobs");  radiusEdit = d->findChild<QLineEdit *>("le_radius");
		threadsEdit = d->findChild<QLineEdit *>("le_threads");
		trackEdit   = d->findChild<QLineEdit *>("le_track");  outfileEdit = d->findChild<QLineEdit *>("le_outfile");
		auto *topBtn   = d->findChild<QToolButton *>("btn_topBrowse");
		auto *botBtn   = d->findChild<QToolButton *>("btn_botBrowse");
		auto *densBtn  = d->findChild<QToolButton *>("btn_densityBrowse");
		auto *magBtn   = d->findChild<QToolButton *>("btn_magGridBrowse");
		auto *trackBtn = d->findChild<QToolButton *>("btn_trackBrowse");
		auto *outBtn   = d->findChild<QToolButton *>("btn_outfileBrowse");
		auto *computeBtn = d->findChild<QPushButton *>("btn_compute");
		auto *closeBtn   = d->findChild<QPushButton *>("btn_close");

		// No grid in this window -> "the grid loaded in this window" is not an option; force the file
		// row (grayed radio, never hidden, so the dialog keeps one size).
		const bool haveMemGrid = (scene && scene->surf && !scene->emptyStart && !scene->imageOnly);
		if (!haveMemGrid) {
			rbTopMem->setEnabled(false);
			rbTopFile->setChecked(true);
		}
		else if (scene->hasCRS() && geogChk) {
			geogChk->setChecked(true);
		}

		const GrdGravMag3DState &st = g_grdgm3dState;
		if (st.valid) {
			if (st.topFromFile || !haveMemGrid) rbTopFile->setChecked(true);
			else                                rbTopMem->setChecked(true);
			useBottomChk->setChecked(st.useBottom);
			zlevelCb->setCurrentIndex(st.zlevel);
			componentCb->setCurrentIndex(st.component);
			igrfCb->setCurrentIndex(st.igrf);
			topEdit->setText(st.topFile);      botEdit->setText(st.botFile);
			thickEdit->setText(st.thickness);  padEdit->setText(st.pad);
			densityEdit->setText(st.density);  magGridEdit->setText(st.magGrid);
			fDecEdit->setText(st.fDec);  fDipEdit->setText(st.fDip);  mIntEdit->setText(st.mInt);
			mDecEdit->setText(st.mDec);  mDipEdit->setText(st.mDip);
			xminEdit->setText(st.xmin);  xmaxEdit->setText(st.xmax);
			yminEdit->setText(st.ymin);  ymaxEdit->setText(st.ymax);
			xincEdit->setText(st.xinc);  yincEdit->setText(st.yinc);
			zobsEdit->setText(st.zobs);  radiusEdit->setText(st.radius);
			threadsEdit->setText(st.threads);
			trackEdit->setText(st.track);      outfileEdit->setText(st.outfile);
			geogChk->setChecked(st.geog);
			if (st.grav) rbGrav->setChecked(true);
			else         rbMag->setChecked(true);
		}

		auto saveState = [this]() {
			GrdGravMag3DState s;
			s.valid = true;
			s.topFromFile = rbTopFile->isChecked();
			s.useBottom = useBottomChk->isChecked();
			s.grav = !rbMag->isChecked();
			s.geog = geogChk->isChecked();
			s.zlevel = zlevelCb->currentIndex();
			s.component = componentCb->currentIndex();
			s.igrf = igrfCb->currentIndex();
			s.topFile = topEdit->text();      s.botFile = botEdit->text();
			s.thickness = thickEdit->text();  s.pad = padEdit->text();
			s.density = densityEdit->text();  s.magGrid = magGridEdit->text();
			s.fDec = fDecEdit->text();  s.fDip = fDipEdit->text();  s.mInt = mIntEdit->text();
			s.mDec = mDecEdit->text();  s.mDip = mDipEdit->text();
			s.xmin = xminEdit->text();  s.xmax = xmaxEdit->text();
			s.ymin = yminEdit->text();  s.ymax = ymaxEdit->text();
			s.xinc = xincEdit->text();  s.yinc = yincEdit->text();
			s.zobs = zobsEdit->text();  s.radius = radiusEdit->text();
			s.threads = threadsEdit->text();
			s.track = trackEdit->text();      s.outfile = outfileEdit->text();
			g_grdgm3dState = s;
		};
		struct GrdGravMag3DSaveOnClose : QObject {
			std::function<void()> save;
			GrdGravMag3DSaveOnClose(QObject *p, std::function<void()> fn) : QObject(p), save(fn) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close) save();
				return QObject::eventFilter(o, e);
			}
		};
		d->installEventFilter(new GrdGravMag3DSaveOnClose(d, saveState));

		// Region (R) and Increment (I) DESCRIBE THE INPUT GRID — they are filled from it, never left
		// for the user to retype. One function, called at construction and again on every event that
		// changes which grid the input is (the top radio, a newly browsed top file).
		auto refreshRegionInc = [this, haveMemGrid]() {
			double w = 0, e = 0, s = 0, n = 0, dx = 0, dy = 0;
			bool got = false;
			if (rbTopMem->isChecked() && haveMemGrid && scn) {
				w = scn->gx0; e = scn->gx1; s = scn->gy0; n = scn->gy1;
				if (!(e > w && n > s)) { w = scn->x0; e = scn->x1; s = scn->y0; n = scn->y1; }
				dx = scn->gdx > 0 ? scn->gdx : (scn->gnx > 1 ? (e - w) / (scn->gnx - 1) : 0);
				dy = scn->gdy > 0 ? scn->gdy : (scn->gny > 1 ? (n - s) / (scn->gny - 1) : 0);
				got = (e > w && n > s && dx > 0 && dy > 0);
			}
			else if (g_juliaGridMeta && !topEdit->text().trimmed().isEmpty()) {
				// Same "w/e/s/n/dx/dy/nx/ny" round-trip the grdsample and RTP dialogs use.
				const char *m = g_juliaGridMeta(topEdit->text().trimmed().toUtf8().constData());
				if (m) {
					const QStringList meta = QString::fromUtf8(m).split('/');   // copy at once (Julia-owned buffer)
					if (meta.size() >= 6) {
						w = meta[0].toDouble(); e = meta[1].toDouble();
						s = meta[2].toDouble(); n = meta[3].toDouble();
						dx = meta[4].toDouble(); dy = meta[5].toDouble();
						got = (e > w && n > s && dx > 0 && dy > 0);
					}
				}
			}
			if (!got) return;
			xminEdit->setText(QString::number(w, 'g', 10));  xmaxEdit->setText(QString::number(e, 'g', 10));
			yminEdit->setText(QString::number(s, 'g', 10));  ymaxEdit->setText(QString::number(n, 'g', 10));
			xincEdit->setText(QString::number(dx, 'g', 10)); yincEdit->setText(QString::number(dy, 'g', 10));
		};

		auto syncTop = [this, topBtn, refreshRegionInc]() {
			const bool file = rbTopFile->isChecked();
			topEdit->setEnabled(file);
			if (topBtn) topBtn->setEnabled(file);
			refreshRegionInc();
		};
		QObject::connect(rbTopFile, &QRadioButton::toggled, d, [syncTop](bool) { syncTop(); });
		syncTop();

		auto syncBottom = [this, botBtn]() {
			const bool on = useBottomChk->isChecked();
			botEdit->setEnabled(on);
			if (botBtn) botBtn->setEnabled(on);
			// Two-grid mode defines the volume by the grids themselves: no layer thickness, and the
			// body is not "closed" at a bottom/top plane either.
			thickEdit->setEnabled(!on);
			zlevelCb->setEnabled(!on);
		};
		QObject::connect(useBottomChk, &QCheckBox::toggled, d, [syncBottom](bool) { syncBottom(); });
		syncBottom();

		auto syncAnom = [this, densBtn, magBtn]() {
			const bool grav = rbGrav->isChecked();
			densityEdit->setEnabled(grav);
			if (densBtn) densBtn->setEnabled(grav);
			for (QLineEdit *e : {fDecEdit, fDipEdit, mIntEdit, mDecEdit, mDipEdit}) e->setEnabled(!grav);
			componentCb->setEnabled(!grav);
			igrfCb->setEnabled(!grav);
			magGridEdit->setEnabled(!grav);
			if (magBtn) magBtn->setEnabled(!grav);
		};
		for (QRadioButton *rb : {rbGrav, rbMag})
			QObject::connect(rb, &QRadioButton::toggled, d, [syncAnom](bool) { syncAnom(); });
		syncAnom();

		auto browseInto = [d](QLineEdit *target, const QString &caption, const QString &filter) {
			QString fn = QFileDialog::getOpenFileName(d, caption, prefStartDir(), filter);
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			target->setText(fn);
		};
		const QString gridFilter = "Grid files (*.nc *.grd *.tif *.tiff);;All files (*)";
		if (topBtn)  QObject::connect(topBtn,  &QToolButton::clicked, d, [this, browseInto, gridFilter, refreshRegionInc]() {
			browseInto(topEdit, "Select the top surface grid", gridFilter);
			refreshRegionInc();			// a new input grid means new Region/Increment, always
		});
		if (botBtn)  QObject::connect(botBtn,  &QToolButton::clicked, d, [this, browseInto, gridFilter]() {
			browseInto(botEdit, "Select the bottom surface grid", gridFilter); });
		if (densBtn) QObject::connect(densBtn, &QToolButton::clicked, d, [this, browseInto, gridFilter]() {
			browseInto(densityEdit, "Select a variable-density grid", gridFilter); });
		if (magBtn)  QObject::connect(magBtn,  &QToolButton::clicked, d, [this, browseInto, gridFilter]() {
			browseInto(magGridEdit, "Select the magnetization intensity grid", gridFilter); });
		if (trackBtn) QObject::connect(trackBtn, &QToolButton::clicked, d, [this, browseInto]() {
			browseInto(trackEdit, "Select x,y locations file", "Data files (*.dat *.txt *.xy);;All files (*)"); });
		if (outBtn) QObject::connect(outBtn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getSaveFileName(d, "Save result as", prefStartDir(),
			                                          "Grid files (*.nc *.grd);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			outfileEdit->setText(fn);
		});

		// STANDING RULE: an "OR Ref grid" row under every Region.
		addRefGridRow(d, d->findChild<QGridLayout *>("gridLayout_region"),
		              xminEdit, xmaxEdit, yminEdit, ymaxEdit, xincEdit, yincEdit);
		addManualButton(d, "grdgravmag3d");

		// Standing rule: double-clicking any file box opens the same chooser its "..." button does.
		fileBoxDoubleClick(topEdit,     topBtn);
		fileBoxDoubleClick(botEdit,     botBtn);
		fileBoxDoubleClick(densityEdit, densBtn);
		fileBoxDoubleClick(magGridEdit, magBtn);
		fileBoxDoubleClick(trackEdit,   trackBtn);
		fileBoxDoubleClick(outfileEdit, outBtn);

		if (closeBtn) QObject::connect(closeBtn, &QPushButton::clicked, d, [d]() { d->close(); });

		if (computeBtn) QObject::connect(computeBtn, &QPushButton::clicked, d, [this, d, saveState]() {
			if (!g_juliaGrdGravMag3D) {
				QMessageBox::warning(d, "Error", "grdgravmag3d: callback not registered (rebuild/restart needed?).");
				return;
			}
			saveState();

			QString top;
			if (rbTopFile->isChecked()) {
				top = topEdit->text().trimmed();
				if (top.isEmpty()) {
					QMessageBox::warning(d, "Error", "Pick the top surface grid.");
					return;
				}
			}
			else {
				top = "selected";			// resolved Julia-side via _FIGREG, same sentinel as the other dialogs
			}
			QString bot;
			if (useBottomChk->isChecked()) {
				bot = botEdit->text().trimmed();
				if (bot.isEmpty()) {
					QMessageBox::warning(d, "Error", "Two-grid mode is on but no bottom grid was given.");
					return;
				}
			}

			const bool grav = rbGrav->isChecked();
			if (grav && densityEdit->text().trimmed().isEmpty()) {
				QMessageBox::warning(d, "Error", "A gravity anomaly needs a density (constant in SI, or a grid).");
				return;
			}
			QString magParams;
			if (!grav) {
				// The five angles are only meaningful with a CONSTANT ambient field; with IGRF (+i/+n)
				// the field's own dec/dip come from the model, and with an intensity grid (+m) the
				// constant Mag is replaced — so the angle set is only required when neither is in play.
				const bool useIgrf = igrfCb->currentIndex() != 0;
				const bool useGrid = !magGridEdit->text().trimmed().isEmpty();
				if (!useIgrf) {
					QStringList mp;
					for (QLineEdit *e : {fDecEdit, fDipEdit, mIntEdit, mDecEdit, mDipEdit}) {
						bool ok = false;
						e->text().trimmed().toDouble(&ok);
						if (!ok) {
							QMessageBox::warning(d, "Error", "All five magnetic parameters must be numbers "
							                                 "(or pick a variable IGRF ambient field).");
							return;
						}
						mp << e->text().trimmed();
					}
					magParams = mp.join('/');
				}
			}

			QStringList kv;
			kv << "top=" + top;
			if (!bot.isEmpty()) kv << "bottom=" + bot;
			kv << QString("mode=") + (grav ? "grav" : "mag");
			if (grav) kv << "density=" + densityEdit->text().trimmed();
			else {
				if (!magParams.isEmpty()) kv << "magparams=" + magParams;
				static const char *comp[] = { "", "t", "x", "y", "z", "h" };
				if (componentCb->currentIndex() > 0) kv << QString("component=") + comp[componentCb->currentIndex()];
				if (igrfCb->currentIndex() > 0)      kv << QString("igrf=") + (igrfCb->currentIndex() == 1 ? "+i" : "+n");
				if (!magGridEdit->text().trimmed().isEmpty()) kv << "maggrid=" + magGridEdit->text().trimmed();
			}
			if (zlevelCb->isEnabled() && zlevelCb->currentIndex() > 0)
				kv << QString("zlevel=") + (zlevelCb->currentIndex() == 1 ? "bottom" : "top");
			const QString xm = xminEdit->text().trimmed(), xM = xmaxEdit->text().trimmed();
			const QString ym = yminEdit->text().trimmed(), yM = ymaxEdit->text().trimmed();
			const int nReg = (!xm.isEmpty()) + (!xM.isEmpty()) + (!ym.isEmpty()) + (!yM.isEmpty());
			if (nReg == 4)      kv << "region=" + xm + "/" + xM + "/" + ym + "/" + yM;
			else if (nReg != 0) {
				QMessageBox::warning(d, "Error", "Give all four Region boxes, or leave all four empty "
				                                 "to use the input grid's own region.");
				return;
			}
			const QString xi = xincEdit->text().trimmed(), yi = yincEdit->text().trimmed();
			if (!xi.isEmpty()) kv << "inc=" + (yi.isEmpty() ? xi : xi + "/" + yi);
			kv << QString("geog=%1").arg(geogChk->isChecked() ? 1 : 0);
			for (auto pair : { std::make_pair(QString("thickness"), thickEdit), std::make_pair(QString("pad"), padEdit),
			                   std::make_pair(QString("zobs"), zobsEdit), std::make_pair(QString("radius"), radiusEdit),
			                   std::make_pair(QString("threads"), threadsEdit),
			                   std::make_pair(QString("track"), trackEdit), std::make_pair(QString("outfile"), outfileEdit) }) {
				if (!pair.second->isEnabled()) continue;   // a grayed control is not part of the run
				const QString v = pair.second->text().trimmed();
				if (!v.isEmpty()) kv << pair.first + "=" + v;
			}

			showBusyDialog("Computing the anomaly…");
			const int ok = g_juliaGrdGravMag3D(scn, kv.join('\n').toUtf8().constData());
			closeBusyDialog();
			if (!ok)
				QMessageBox::warning(d, "Error", "grdgravmag3d failed — see this window's Errors console for details.");
		});

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}
};

// ============================================================================================
// Continuous Reduction To the Pole, a.k.a. differential RTP (Geophysics > Magnetics > grdredpol) —
// GMT's grdredpol supplement (Luis & Miranda 2008, JGR 113 B10105). Runtime .ui load, same shape as
// the two gravmag dialogs above. Distinct from the plain "Reduction to the Pole" entry (Rtp3DDialog,
// Mirone's parker_stuff.m): that one assumes ONE field/magnetization direction for the whole grid,
// this one lets both vary — the grid is decomposed into moving windows (W) where each is taken as
// locally constant, with a per-point filter rebuilt by Taylor expansion (N turns that off).
// Direction source is an exclusive pair: IGRF at each point for a given year (T) or one constant
// dec/dip (C, the classical RTP). Independently, a magnetization inclination (Ei) and/or declination
// (Ed) grid may be supplied; whatever is NOT given as a grid still comes from IGRF, so those two
// boxes stay live in both modes.
// GMT.jl has no verbose wrapper for this supplement, so Julia drives it in MONOLITHIC mode — still
// one in-process library call.
// Success is silent, failures get a modal, parameters survive close/reopen, Region is filled from
// the input grid, and every file box opens its chooser on a double-click: the standing rules.
// ============================================================================================
struct GrdRedPolState {
	bool valid = false;
	bool inFromFile = false, igrf = true, noTaylor = false;
	int boundary = 0;
	QString inFile, year, dec, dip, incGrid, decGrid;
	QString filtRows, filtCols, winWidth, xmin, xmax, ymin, ymax, outfile, filterFile;
};
static GrdRedPolState g_grdredpolState;

class GrdRedPolDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QRadioButton *rbInMem, *rbInFile, *rbIgrf, *rbConst;
	QCheckBox *noTaylorChk;
	QComboBox *boundaryCb;
	QLineEdit *inEdit, *yearEdit, *decEdit, *dipEdit, *incGridEdit, *decGridEdit;
	QLineEdit *filtRowsEdit, *filtColsEdit, *winWidthEdit;
	QLineEdit *xminEdit, *xmaxEdit, *yminEdit, *ymaxEdit, *outfileEdit, *filterFileEdit;

	explicit GrdRedPolDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/grdredpol_dialog.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("GrdRedPolDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("GrdRedPolDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;

		rbInMem  = d->findChild<QRadioButton *>("rb_inMem");
		rbInFile = d->findChild<QRadioButton *>("rb_inFile");
		rbIgrf   = d->findChild<QRadioButton *>("rb_igrf");
		rbConst  = d->findChild<QRadioButton *>("rb_const");
		noTaylorChk = d->findChild<QCheckBox *>("chk_noTaylor");
		boundaryCb  = d->findChild<QComboBox *>("cb_boundary");
		inEdit   = d->findChild<QLineEdit *>("le_inFile");
		yearEdit = d->findChild<QLineEdit *>("le_year");
		decEdit  = d->findChild<QLineEdit *>("le_dec");   dipEdit = d->findChild<QLineEdit *>("le_dip");
		incGridEdit = d->findChild<QLineEdit *>("le_incGrid");
		decGridEdit = d->findChild<QLineEdit *>("le_decGrid");
		filtRowsEdit = d->findChild<QLineEdit *>("le_filtRows");
		filtColsEdit = d->findChild<QLineEdit *>("le_filtCols");
		winWidthEdit = d->findChild<QLineEdit *>("le_winWidth");
		xminEdit = d->findChild<QLineEdit *>("le_xmin");  xmaxEdit = d->findChild<QLineEdit *>("le_xmax");
		yminEdit = d->findChild<QLineEdit *>("le_ymin");  ymaxEdit = d->findChild<QLineEdit *>("le_ymax");
		outfileEdit    = d->findChild<QLineEdit *>("le_outfile");
		filterFileEdit = d->findChild<QLineEdit *>("le_filterFile");
		auto *inBtn      = d->findChild<QToolButton *>("btn_inBrowse");
		auto *incGridBtn = d->findChild<QToolButton *>("btn_incGridBrowse");
		auto *decGridBtn = d->findChild<QToolButton *>("btn_decGridBrowse");
		auto *outBtn     = d->findChild<QToolButton *>("btn_outfileBrowse");
		auto *filtBtn    = d->findChild<QToolButton *>("btn_filterFileBrowse");
		auto *computeBtn = d->findChild<QPushButton *>("btn_compute");
		auto *closeBtn   = d->findChild<QPushButton *>("btn_close");

		const bool haveMemGrid = (scene && scene->surf && !scene->emptyStart && !scene->imageOnly);
		if (!haveMemGrid) {
			rbInMem->setEnabled(false);          // grayed, never hidden: the dialog keeps one size
			rbInFile->setChecked(true);
		}

		const GrdRedPolState &st = g_grdredpolState;
		if (st.valid) {
			if (st.inFromFile || !haveMemGrid) rbInFile->setChecked(true);
			else                               rbInMem->setChecked(true);
			if (st.igrf) rbIgrf->setChecked(true);
			else         rbConst->setChecked(true);
			noTaylorChk->setChecked(st.noTaylor);
			boundaryCb->setCurrentIndex(st.boundary);
			inEdit->setText(st.inFile);      yearEdit->setText(st.year);
			decEdit->setText(st.dec);        dipEdit->setText(st.dip);
			incGridEdit->setText(st.incGrid); decGridEdit->setText(st.decGrid);
			filtRowsEdit->setText(st.filtRows); filtColsEdit->setText(st.filtCols);
			winWidthEdit->setText(st.winWidth);
			xminEdit->setText(st.xmin);  xmaxEdit->setText(st.xmax);
			yminEdit->setText(st.ymin);  ymaxEdit->setText(st.ymax);
			outfileEdit->setText(st.outfile);  filterFileEdit->setText(st.filterFile);
		}

		auto saveState = [this]() {
			GrdRedPolState s;
			s.valid = true;
			s.inFromFile = rbInFile->isChecked();
			s.igrf = rbIgrf->isChecked();
			s.noTaylor = noTaylorChk->isChecked();
			s.boundary = boundaryCb->currentIndex();
			s.inFile = inEdit->text();     s.year = yearEdit->text();
			s.dec = decEdit->text();       s.dip = dipEdit->text();
			s.incGrid = incGridEdit->text(); s.decGrid = decGridEdit->text();
			s.filtRows = filtRowsEdit->text(); s.filtCols = filtColsEdit->text();
			s.winWidth = winWidthEdit->text();
			s.xmin = xminEdit->text();  s.xmax = xmaxEdit->text();
			s.ymin = yminEdit->text();  s.ymax = ymaxEdit->text();
			s.outfile = outfileEdit->text();  s.filterFile = filterFileEdit->text();
			g_grdredpolState = s;
		};
		struct GrdRedPolSaveOnClose : QObject {
			std::function<void()> save;
			GrdRedPolSaveOnClose(QObject *p, std::function<void()> fn) : QObject(p), save(fn) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close) save();
				return QObject::eventFilter(o, e);
			}
		};
		d->installEventFilter(new GrdRedPolSaveOnClose(d, saveState));

		// Region DESCRIBES THE INPUT GRID — filled from it, refreshed whenever the input changes.
		// (grdredpol has no -I: the output keeps the input's own increment.)
		auto refreshRegion = [this, haveMemGrid]() {
			double w = 0, e = 0, s = 0, n = 0;
			bool got = false;
			if (rbInMem->isChecked() && haveMemGrid && scn) {
				w = scn->gx0; e = scn->gx1; s = scn->gy0; n = scn->gy1;
				if (!(e > w && n > s)) { w = scn->x0; e = scn->x1; s = scn->y0; n = scn->y1; }
				got = (e > w && n > s);
			}
			else if (g_juliaGridMeta && !inEdit->text().trimmed().isEmpty()) {
				const char *m = g_juliaGridMeta(inEdit->text().trimmed().toUtf8().constData());
				if (m) {
					const QStringList meta = QString::fromUtf8(m).split('/');   // copy at once (Julia-owned buffer)
					if (meta.size() >= 4) {
						w = meta[0].toDouble(); e = meta[1].toDouble();
						s = meta[2].toDouble(); n = meta[3].toDouble();
						got = (e > w && n > s);
					}
				}
			}
			if (!got) return;
			xminEdit->setText(QString::number(w, 'g', 10));  xmaxEdit->setText(QString::number(e, 'g', 10));
			yminEdit->setText(QString::number(s, 'g', 10));  ymaxEdit->setText(QString::number(n, 'g', 10));
		};

		auto syncInput = [this, inBtn, refreshRegion]() {
			const bool file = rbInFile->isChecked();
			inEdit->setEnabled(file);
			if (inBtn) inBtn->setEnabled(file);
			refreshRegion();
		};
		QObject::connect(rbInFile, &QRadioButton::toggled, d, [syncInput](bool) { syncInput(); });
		syncInput();

		// IGRF year vs constant dec/dip: the idle side grays out (never hidden).
		auto syncDirection = [this]() {
			const bool igrf = rbIgrf->isChecked();
			yearEdit->setEnabled(igrf);
			decEdit->setEnabled(!igrf);
			dipEdit->setEnabled(!igrf);
		};
		for (QRadioButton *rb : {rbIgrf, rbConst})
			QObject::connect(rb, &QRadioButton::toggled, d, [syncDirection](bool) { syncDirection(); });
		syncDirection();

		auto browseInto = [d](QLineEdit *target, const QString &caption) {
			QString fn = QFileDialog::getOpenFileName(d, caption, prefStartDir(),
			                                          "Grid files (*.nc *.grd *.tif *.tiff);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			target->setText(fn);
		};
		if (inBtn) QObject::connect(inBtn, &QToolButton::clicked, d, [this, browseInto, refreshRegion]() {
			browseInto(inEdit, "Select the magnetic anomaly grid");
			refreshRegion();			// a new input grid means a new Region, always
		});
		if (incGridBtn) QObject::connect(incGridBtn, &QToolButton::clicked, d, [this, browseInto]() {
			browseInto(incGridEdit, "Select the magnetization inclination grid"); });
		if (decGridBtn) QObject::connect(decGridBtn, &QToolButton::clicked, d, [this, browseInto]() {
			browseInto(decGridEdit, "Select the magnetization declination grid"); });
		if (outBtn) QObject::connect(outBtn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getSaveFileName(d, "Save the RTP grid as", prefStartDir(),
			                                          "Grid files (*.nc *.grd);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			outfileEdit->setText(fn);
		});
		if (filtBtn) QObject::connect(filtBtn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getSaveFileName(d, "Save the filter as", prefStartDir(),
			                                          "Grid files (*.nc *.grd);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			filterFileEdit->setText(fn);
		});

		// STANDING RULE: an "OR Ref grid" row under every Region. (grdredpol has no -I, so no
		// increment boxes to fill.)
		addRefGridRow(d, d->findChild<QGridLayout *>("gridLayout_region"),
		              xminEdit, xmaxEdit, yminEdit, ymaxEdit);
		addManualButton(d, "grdredpol");

		// Standing rule: double-clicking any file box opens the same chooser its "..." button does.
		fileBoxDoubleClick(inEdit,         inBtn);
		fileBoxDoubleClick(incGridEdit,    incGridBtn);
		fileBoxDoubleClick(decGridEdit,    decGridBtn);
		fileBoxDoubleClick(outfileEdit,    outBtn);
		fileBoxDoubleClick(filterFileEdit, filtBtn);

		if (closeBtn) QObject::connect(closeBtn, &QPushButton::clicked, d, [d]() { d->close(); });

		if (computeBtn) QObject::connect(computeBtn, &QPushButton::clicked, d, [this, d, saveState]() {
			if (!g_juliaGrdRedPol) {
				QMessageBox::warning(d, "Error", "grdredpol: callback not registered (rebuild/restart needed?).");
				return;
			}
			saveState();

			QString in = "selected";		// resolved Julia-side via _FIGREG, same sentinel as the other dialogs
			if (rbInFile->isChecked()) {
				in = inEdit->text().trimmed();
				if (in.isEmpty()) {
					QMessageBox::warning(d, "Error", "Pick the magnetic anomaly grid.");
					return;
				}
			}
			// The module itself rejects even or <5 filter sizes ("That was a ridiculous number of
			// filter coefficients"); catching it here says so on the box the user can actually fix.
			bool okR = false, okC = false;
			const int nRow = filtRowsEdit->text().trimmed().toInt(&okR);
			const int nCol = filtColsEdit->text().trimmed().toInt(&okC);
			if (!okR || !okC || nRow < 5 || nCol < 5 || nRow % 2 == 0 || nCol % 2 == 0) {
				QMessageBox::warning(d, "Error", "The filter coefficients (rows/cols) must both be ODD "
				                                 "numbers of at least 5.");
				return;
			}

			QStringList kv;
			kv << "input=" + in;
			if (rbIgrf->isChecked()) {
				kv << "year=" + yearEdit->text().trimmed();
			}
			else {
				bool okD = false, okI = false;
				decEdit->text().trimmed().toDouble(&okD);
				dipEdit->text().trimmed().toDouble(&okI);
				if (!okD || !okI) {
					QMessageBox::warning(d, "Error", "Constant mode needs a numeric declination and inclination.");
					return;
				}
				kv << "constdec=" + decEdit->text().trimmed();
				kv << "constdip=" + dipEdit->text().trimmed();
			}
			// Ei/Ed stay available in BOTH modes: the module falls back to IGRF for whichever of the
			// two is not given as a grid.
			if (!incGridEdit->text().trimmed().isEmpty()) kv << "incgrid=" + incGridEdit->text().trimmed();
			if (!decGridEdit->text().trimmed().isEmpty()) kv << "decgrid=" + decGridEdit->text().trimmed();
			kv << QString("filter=%1/%2").arg(nRow).arg(nCol);
			if (!winWidthEdit->text().trimmed().isEmpty()) kv << "window=" + winWidthEdit->text().trimmed();
			if (boundaryCb->currentIndex() > 0) kv << QString("boundary=") + (boundaryCb->currentIndex() == 1 ? "m" : "r");
			if (noTaylorChk->isChecked()) kv << "notaylor=1";
			const QString xm = xminEdit->text().trimmed(), xM = xmaxEdit->text().trimmed();
			const QString ym = yminEdit->text().trimmed(), yM = ymaxEdit->text().trimmed();
			const int nReg = (!xm.isEmpty()) + (!xM.isEmpty()) + (!ym.isEmpty()) + (!yM.isEmpty());
			if (nReg == 4)      kv << "region=" + xm + "/" + xM + "/" + ym + "/" + yM;
			else if (nReg != 0) {
				QMessageBox::warning(d, "Error", "Give all four Region boxes, or leave all four empty "
				                                 "to use the input grid's own region.");
				return;
			}
			if (!outfileEdit->text().trimmed().isEmpty())    kv << "outfile=" + outfileEdit->text().trimmed();
			if (!filterFileEdit->text().trimmed().isEmpty()) kv << "filterfile=" + filterFileEdit->text().trimmed();

			showBusyDialog("Computing the continuous RTP…");
			const int ok = g_juliaGrdRedPol(scn, kv.join('\n').toUtf8().constData());
			closeBusyDialog();
			if (!ok)
				QMessageBox::warning(d, "Error", "grdredpol failed — see this window's Errors console for details.");
		});

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}
};

// ============================================================================================
// grdgradient (GMT menu) — directional derivative / gradient of THIS window's grid. The layout is
// Mirone's own grdgradient window, control for control (Azim1/Azim2 lists in a "Horizontal Light
// Angles" box, the Slope/Aspect checkboxes with the direction-flavour combo beside them, then the
// Boundary condition and Normalization rows, OK at the bottom right), including its small blue "?"
// notes — those explain their own group, and are NOT the manual button (the green disk in the
// lower-left corner is, same as every other module dialog here).
//
// Azim1/Azim2 are LISTS of whole degrees, as in Mirone, not spin boxes: picking a light azimuth is a
// coarse choice and the list makes the common values one click away. Azim2 carries an empty first row
// meaning "not used" — supplying it asks the module for both gradients and keeps the larger one.
// Slope, Aspect and a plain directional derivative are three different outputs of the same module,
// so the result is named for whichever was asked (a recompute REPLACES that same name).
// ============================================================================================
struct GrdGradientState {
	bool valid = false;
	int azim1 = -1, azim2 = -1;             // < 0 = the blank row, i.e. not used
	bool slope = false, aspect = false, downSlope = true;   // aspect = down-slope by default (-Da)
	int dirFlavour = 0, boundary = 0, norm = 0, lambert = 0, saveStats = 0;
	QString amp, sigma, offset;
	QString lambAzim, lambElev, ambient, difuse, specular, shine;
	QString azimGrid, xmin, xmax, ymin, ymax, outfile;
};
static GrdGradientState g_grdgradState;

// ============================================================================================
// Illumination — Hillshade (View menu). Port of Mirone's src_figs/shading_params.m: pick a GMT
// illumination model on the little numbered toolbar, aim the light on the astrolabe dial (azimuth)
// and on the quarter-circle (elevation), press OK. The maths runs in Julia (src/hillshade.jl) and
// comes back as a per-node reflectance the shade engine modulates the colours with
// (gmtvtk_set_shade_intensity_h -> gmtIlluminate, the same modulator Mirone's mex_illuminate is).
//
// Mirone's 3 (Peucker) and 5 (Manip Raster) are dropped by request; what remains is renumbered
// 1..7 CONTINUOUSLY (Mirone's own number in brackets): 1 grdgradient classic, 2 grdgradient
// Lambertian, 3 Lambertian with lighting [4], 4 ESRI hillshade [6], 5 false colour [7],
// 6 dynamic range compression [8], 7 remove illumination [9].
// The false colour keeps BOTH of Mirone's algorithms — its two radio buttons — so the "Old
// algorithm" (shade_manip_raster) is here with its elevation and Amp factor, even though the
// stand-alone Manip Raster entry is gone.
// No Q_OBJECT/moc in the two custom widgets (paint/mouse overrides only, like BaseMapArea).
// ============================================================================================

// Mirone's axes1 — the astrolabe dial. One RED hand carries the azimuth; the false-colour model
// shows three (red/green/blue), one per colour channel. Dragging a hand sets its azimuth; clicking
// anywhere else in the disc grabs the nearest hand, which is shading_params.m's ButtonDown.
// Azimuth is 0 at NORTH, increasing CLOCKWISE — the GMT convention the light vector uses.
class AzimuthDial : public QWidget {
public:
	double az[3] = { 0.0, 120.0, 240.0 };            // red, green, blue hands
	bool   three = false;                            // false colour -> show/drag all three
	std::function<void(int, double)> onChange;       // (hand 0..2, azimuth in degrees)
	explicit AzimuthDial(QWidget *p) : QWidget(p) {
		setMinimumSize(112, 112);
		setCursor(Qt::SizeAllCursor);                // STANDING RULE: every drag uses SizeAll
	}
protected:
	int drag = -1;
	QPointF centre() const { return QPointF(width() / 2.0, height() / 2.0); }
	double  radius() const { return std::min(width(), height()) / 2.0 - 6.0; }
	// Widget point -> azimuth (deg from north, clockwise). Screen y grows DOWN, hence (dx, -dy).
	double azOf(const QPointF &p) const {
		const QPointF c = centre();
		double a = std::atan2(p.x() - c.x(), c.y() - p.y()) * 180.0 / vtkMath::Pi();
		while (a <   0.0) a += 360.0;
		while (a >= 360.0) a -= 360.0;
		return a;
	}
	QPointF handTip(double a) const {
		const QPointF c = centre();
		const double r = radius(), t = a * vtkMath::Pi() / 180.0;
		return QPointF(c.x() + r * std::sin(t), c.y() - r * std::cos(t));
	}
	int nearestHand(const QPointF &p) const {
		if (!three) return 0;
		int best = 0;  double bd = 1e30;
		for (int k = 0; k < 3; ++k) {
			const QPointF t = handTip(az[k]);
			const double d = std::hypot(p.x() - t.x(), p.y() - t.y());
			if (d < bd) { bd = d; best = k; }
		}
		return best;
	}
	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		g.setRenderHint(QPainter::Antialiasing, true);
		const QPointF c = centre();
		const double r = radius();
		g.setBrush(QColor(28, 28, 32));  g.setPen(QPen(QColor(150, 140, 110), 2));
		g.drawEllipse(c, r, r);
		// compass ring: a tick every 15 deg, longer every 90, with the cardinal letters
		g.setPen(QPen(QColor(150, 140, 110), 1));
		for (int a = 0; a < 360; a += 15) {
			const double t = a * vtkMath::Pi() / 180.0;
			const double f = (a % 90 == 0) ? 0.78 : 0.88;
			g.drawLine(QPointF(c.x() + f * r * std::sin(t), c.y() - f * r * std::cos(t)),
			           QPointF(c.x() +     r * std::sin(t), c.y() -     r * std::cos(t)));
		}
		g.setPen(QColor(200, 195, 175));
		QFont f = g.font();  f.setPointSizeF(f.pointSizeF() * 0.85);  g.setFont(f);
		const char *card[4] = { "N", "E", "S", "W" };
		for (int k = 0; k < 4; ++k) {
			const double t = k * 90.0 * vtkMath::Pi() / 180.0;
			const QPointF q(c.x() + 0.62 * r * std::sin(t), c.y() - 0.62 * r * std::cos(t));
			g.drawText(QRectF(q.x() - 8, q.y() - 8, 16, 16), Qt::AlignCenter, card[k]);
		}
		static const QColor hc[3] = { QColor(230, 40, 40), QColor(40, 200, 60), QColor(70, 110, 255) };
		const int n = three ? 3 : 1;
		for (int k = 0; k < n; ++k) {
			g.setPen(QPen(hc[k], 3, Qt::SolidLine, Qt::RoundCap));
			g.drawLine(c, handTip(az[k]));
		}
		g.setPen(Qt::NoPen);  g.setBrush(QColor(200, 195, 175));
		g.drawEllipse(c, 2.5, 2.5);
	}
	void mousePressEvent(QMouseEvent *e) override {
		drag = nearestHand(e->position());
		mouseMoveEvent(e);
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (drag < 0) return;
		az[drag] = azOf(e->position());
		update();
		if (onChange) onChange(drag, az[drag]);
	}
	void mouseReleaseEvent(QMouseEvent *) override { drag = -1; }
};

// Mirone's axes2 — the elevation quarter-circle. The hand swings between 0 (horizon, pointing east)
// and 90 (overhead); dragging anywhere inside the quadrant moves it, as shading_params.m does.
class ElevationDial : public QWidget {
public:
	double elev = 30.0;
	std::function<void(double)> onChange;
	explicit ElevationDial(QWidget *p) : QWidget(p) {
		setMinimumSize(66, 66);
		setCursor(Qt::SizeAllCursor);
	}
protected:
	bool drag = false;
	QPointF origin() const { return QPointF(4.0, height() - 4.0); }        // bottom-left corner
	double  radius() const { return std::min(width(), height()) - 10.0; }
	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		g.setRenderHint(QPainter::Antialiasing, true);
		const QPointF o = origin();
		const double r = radius();
		g.setPen(QPen(QColor(150, 145, 130), 1));
		g.drawArc(QRectF(o.x() - r, o.y() - r, 2 * r, 2 * r), 0, 90 * 16);   // first-quadrant arc
		g.drawLine(o, QPointF(o.x() + r, o.y()));
		g.drawLine(o, QPointF(o.x(), o.y() - r));
		const double t = elev * vtkMath::Pi() / 180.0;
		g.setPen(QPen(QColor(20, 20, 20), 3, Qt::SolidLine, Qt::RoundCap));
		g.drawLine(o, QPointF(o.x() + r * std::cos(t), o.y() - r * std::sin(t)));
	}
	void setFromPoint(const QPointF &p) {
		const QPointF o = origin();
		double a = std::atan2(o.y() - p.y(), p.x() - o.x()) * 180.0 / vtkMath::Pi();
		elev = std::clamp(a, 0.0, 90.0);
		update();
		if (onChange) onChange(elev);
	}
	void mousePressEvent(QMouseEvent *e) override { drag = true; setFromPoint(e->position()); }
	void mouseMoveEvent(QMouseEvent *e) override { if (drag) setFromPoint(e->position()); }
	void mouseReleaseEvent(QMouseEvent *) override { drag = false; }
};

// Remember the last illumination across openings of the dialog (Mirone rebuilds its window each
// time, but the light you just aimed is the light you want to nudge next).
struct HillshadeState {
	bool    valid = false;
	int     model = 1;
	double  azim = 0.0, elev = 30.0;
	double  azR = 0.0, azG = 120.0, azB = 240.0;
	bool    oldAlgo = true;                  // false colour: Mirone's radio_oldAlgo starts checked
	QString ambient = ".55", diffuse = ".6", specular = ".4", shine = "10";
	QString amp = "125", wavelength;
};
static HillshadeState g_hillshadeState;

// One Illumination dialog per window, alive while parked. Closing it with the X does NOT destroy it:
// it hides and PARKS as a handle at the bottom of that window's Scene Objects dock, exactly like a
// closed X,Y plot or Contours dialog — same Scene::parkedTools list, same row builder. Re-picking the
// menu entry brings the SAME dialog back, never a second one.
class HillshadeDialog;
static std::map<Scene *, HillshadeDialog *> g_hillshadeDlgs;

class HillshadeDialog {
public:
	QDialog *dlg = nullptr;
	Scene   *scn = nullptr;
	AzimuthDial   *dial = nullptr;
	ElevationDial *elevDial = nullptr;
	QLineEdit *eAzim, *eAzR, *eAzG, *eAzB, *eElev;
	QLineEdit *eAmbient, *eDiffuse, *eSpecular, *eShine, *eWave, *eAmp;
	QLabel *lAzim, *lElev;
	QWidget *elevWrap = nullptr;                                       // "Elevation" caption + its dial
	QWidget *reflBox = nullptr, *waveBox = nullptr, *fcBox = nullptr;   // per-model option panels
	QRadioButton *rbOldAlgo = nullptr, *rbGrdGrad = nullptr;
	QButtonGroup *models = nullptr, *algos = nullptr;
	int model = 1;
	bool reallyClose = false;   // set by the parked row's "Delete": let the next close through

	// Bring the dialog back from the dock (double-click, the row's checkbox, its "Show" item). ONE
	// function for every way back in, like xyUnpark / ContourDialog::unpark.
	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();
		dlg->raise();
		dlg->activateWindow();
	}

	// The parked row's menu — properties button and context menu are the same lambda, never two.
	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel  = m.addAction("Delete");
			QAction *pick  = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scn, dlg);
				dlg->deleteLater();          // destroyed -> the row and this object go with it
			}
		};
	}

	// Drop the registry entry by VALUE, not by `scn`: when the owning viewer window is torn down the
	// dialog dies with it and the Scene may already be gone, so the key cannot be trusted.
	~HillshadeDialog() {
		for (auto it = g_hillshadeDlgs.begin(); it != g_hillshadeDlgs.end(); )
			it = (it->second == this) ? g_hillshadeDlgs.erase(it) : std::next(it);
	}

	explicit HillshadeDialog(QWidget *parent, Scene *scene) : scn(scene) {
		dlg = new QDialog(parent);
		// WA_DeleteOnClose is deliberately NOT set: the dialog has to survive being closed so the
		// parked handle can bring it back with its settings intact. Only the row's "Delete" (which
		// sets reallyClose) lets a close through.
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;
		g_hillshadeDlgs[scene] = this;

		struct CloseParks : QObject {
			HillshadeDialog *hd;
			CloseParks(QObject *parent, HillshadeDialog *h) : QObject(parent), hd(h) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close && hd && !hd->reallyClose && sceneAlive(hd->scn)) {
					e->ignore();
					hd->dlg->hide();
					HillshadeDialog *h = hd;   // a lambda cannot capture a member of the enclosing class
					parkTool(h->scn, h->dlg, "Illumination", IC_Surface,
					         "Closed Illumination dialog — double-click to bring it back, click for Show / Delete",
					         [h]() { h->unpark(); }, h->parkedMenu());
					unfoldSceneObjects(hd->scn);   // a handle nobody can see is no handle at all
					return true;
				}
				return QObject::eventFilter(o, e);
			}
		};
		dlg->installEventFilter(new CloseParks(dlg, this));

		auto *outer = new QVBoxLayout(d);

		// --- the numbered model toolbar (Mirone's uitoggletools, same order, same tooltips) -------
		auto *bar = new QHBoxLayout();
		models = new QButtonGroup(d);
		models->setExclusive(true);
		struct Btn { int id; const char *label; const char *tip; };
		static const Btn btns[] = {
			{ 1, "1", "GMT grdgradient classic" },
			{ 2, "2", "GMT grdgradient Lambertian" },
			{ 3, "3", "Lambertian with lighting" },
			{ 4, "4", "Hillshade (ESRI)" },
			{ 5, "5", "False color" },
			{ 6, "6", "Dynamic Range Compression" },
		};
		for (const auto &b : btns) {
			auto *tb = new QToolButton(d);
			tb->setText(b.label);  tb->setToolTip(b.tip);
			tb->setCheckable(true);  tb->setFixedSize(28, 28);
			models->addButton(tb, b.id);
			bar->addWidget(tb);
		}
		bar->addSpacing(10);
		// "Remove illumination" is an ACTION, not a model: it strips the shade off the grid there and
		// then. It is NOT part of the exclusive model group and never touches what the dialog shows —
		// blanking the window would be the opposite of what it is for.
		auto *tbOff = new QToolButton(d);
		tbOff->setText("✕");
		tbOff->setToolTip("Remove the illumination from the grid (does not change this window)");
		tbOff->setFixedSize(28, 28);
		QObject::connect(tbOff, &QToolButton::clicked, d, [this]() { applyRemove(); });
		bar->addWidget(tbOff);
		bar->addStretch(1);
		outer->addLayout(bar);

		// --- the ONE band: azimuth compass | elevation | this model's options ----------------------
		auto *band = new QHBoxLayout();
		band->setSpacing(0);
		dial     = new AzimuthDial(d);
		elevDial = new ElevationDial(d);
		dial->setFixedSize(112, 112);
		elevDial->setFixedSize(72, 72);
		band->addWidget(dial, 0, Qt::AlignVCenter);
		band->addSpacing(14);
		// "Elevation" sits DIRECTLY on its dial (2 px apart), and the caption+dial pair is centred
		// against the compass beside it — not floated to the top of a tall grid row.
		lElev = new QLabel("Elevation", d);
		lElev->setAlignment(Qt::AlignHCenter);
		elevWrap = new QWidget(d);
		auto *elevCol = new QVBoxLayout(elevWrap);
		elevCol->setContentsMargins(0, 0, 0, 0);
		elevCol->setSpacing(2);
		elevCol->addWidget(lElev);
		elevCol->addWidget(elevDial, 0, Qt::AlignHCenter);
		band->addWidget(elevWrap, 0, Qt::AlignVCenter);
		band->addSpacing(24);

		auto mkEdit = [d](const QString &txt) {
			auto *e = new QLineEdit(txt, d);
			e->setValidator(new QDoubleValidator(e));
			e->setFixedWidth(52);
			return e;
		};
		// The per-model options sit BESIDE the two dials, never under them: each model's block is its
		// own panel and all three live in the SAME slot, so the window is one band tall whatever is
		// selected. Only one panel is ever visible, and a hidden widget takes no space in a layout,
		// so sharing the slot costs nothing. The slot takes the leftover width (stretch 1) instead of
		// leaving a dead strip down the right edge.
		auto *optSlot = new QWidget(d);
		auto *optLay  = new QGridLayout(optSlot);
		optLay->setContentsMargins(0, 0, 0, 0);
		auto mkPanel = [&](QWidget *&box, QFormLayout *&fl) {
			box = new QWidget(optSlot);
			fl  = new QFormLayout(box);
			fl->setContentsMargins(0, 0, 0, 0);
			fl->setLabelAlignment(Qt::AlignLeft);
			optLay->addWidget(box, 0, 0);
		};
		QFormLayout *flRefl = nullptr, *flWave = nullptr, *flFC = nullptr;
		mkPanel(reflBox, flRefl);  mkPanel(waveBox, flWave);  mkPanel(fcBox, flFC);

		eAmbient  = mkEdit(".55");   flRefl->addRow("Ambient light", eAmbient);
		eDiffuse  = mkEdit(".6");    flRefl->addRow("Diffuse reflection", eDiffuse);
		eSpecular = mkEdit(".4");    flRefl->addRow("Specular reflection", eSpecular);
		eShine    = mkEdit("10");    flRefl->addRow("Specular shine", eShine);
		eWave     = mkEdit("");      flWave->addRow("Wavelength (px)", eWave);
		eWave->setToolTip("Cut-in wavelength of the highpass filter, in pixels. Empty = half the "
		                  "longer grid side, the ppdrc default.");
		// False colour: Mirone's two exclusive radios + the Amp factor the old algorithm reads.
		// shading_params.m gives radio_oldAlgo Value 1, so the old algorithm is the default.
		rbOldAlgo = new QRadioButton("Old algorithm", d);
		rbGrdGrad = new QRadioButton("grdgradient", d);
		rbOldAlgo->setToolTip("Use the older algorithm (Manip Raster / shade_manip_raster)");
		rbGrdGrad->setToolTip("Compute illumination with grdgradient");
		algos = new QButtonGroup(d);  algos->setExclusive(true);
		algos->addButton(rbOldAlgo, 1);  algos->addButton(rbGrdGrad, 0);
		rbOldAlgo->setChecked(true);
		eAmp = mkEdit("125");
		eAmp->setToolTip("Amplitude factor the old algorithm divides its differences by.");
		flFC->addRow(rbOldAlgo);
		flFC->addRow("Amp factor", eAmp);
		flFC->addRow(rbGrdGrad);
		band->addWidget(optSlot, 1, Qt::AlignVCenter);
		outer->addLayout(band);

		// --- azimuth / elevation boxes + OK -------------------------------------------------------
		auto *row = new QHBoxLayout();
		lAzim = new QLabel("Azimuth", d);
		eAzim = mkEdit("0");
		eAzR  = mkEdit("0");    eAzR->setStyleSheet("background:#ff5555;");
		eAzG  = mkEdit("120");  eAzG->setStyleSheet("background:#55dd66;");
		eAzB  = mkEdit("240");  eAzB->setStyleSheet("background:#6688ff;");
		eAzR->setToolTip("Red component azimuth");
		eAzG->setToolTip("Green component azimuth");
		eAzB->setToolTip("Blue component azimuth");
		eElev = mkEdit("30");
		row->addWidget(lAzim);
		row->addWidget(eAzim);  row->addWidget(eAzR);  row->addWidget(eAzG);  row->addWidget(eAzB);
		row->addSpacing(12);
		row->addWidget(eElev);
		row->addStretch(1);
		auto *okBtn = new QPushButton("OK", d);
		okBtn->setDefault(true);
		row->addWidget(okBtn);
		outer->addLayout(row);

		// --- restore the last light, then wire everything ----------------------------------------
		const HillshadeState &st = g_hillshadeState;
		if (st.valid) {
			model = st.model;
			dial->az[0] = st.azR;  dial->az[1] = st.azG;  dial->az[2] = st.azB;
			if (st.model != 5) dial->az[0] = st.azim;
			elevDial->elev = st.elev;
			eAzim->setText(QString::number(st.azim, 'f', 0));
			eAzR->setText(QString::number(st.azR, 'f', 0));
			eAzG->setText(QString::number(st.azG, 'f', 0));
			eAzB->setText(QString::number(st.azB, 'f', 0));
			eElev->setText(QString::number(st.elev, 'f', 0));
			eAmbient->setText(st.ambient);  eDiffuse->setText(st.diffuse);
			eSpecular->setText(st.specular);  eShine->setText(st.shine);
			eWave->setText(st.wavelength);  eAmp->setText(st.amp);
			(st.oldAlgo ? rbOldAlgo : rbGrdGrad)->setChecked(true);
		}

		// Dial -> boxes. The single azimuth and the red false-colour azimuth are THE SAME hand
		// (Mirone's h_line(1), tagged 'red', feeds edit_azim or edit_azimR depending on the model).
		dial->onChange = [this](int hand, double a) {
			QLineEdit *e = (model == 5) ? (hand == 0 ? eAzR : (hand == 1 ? eAzG : eAzB)) : eAzim;
			e->setText(QString::number(a, 'f', 0));
		};
		elevDial->onChange = [this](double v) { eElev->setText(QString::number(v, 'f', 0)); };
		// Boxes -> dial (typing a number must move the hand, or the two would disagree).
		auto bindBox = [this](QLineEdit *e, int hand) {
			QObject::connect(e, &QLineEdit::editingFinished, dlg, [this, e, hand]() {
				bool ok = false;
				const double v = e->text().trimmed().toDouble(&ok);
				if (!ok) return;
				dial->az[hand] = std::fmod(std::fmod(v, 360.0) + 360.0, 360.0);
				dial->update();
			});
		};
		bindBox(eAzim, 0);  bindBox(eAzR, 0);  bindBox(eAzG, 1);  bindBox(eAzB, 2);
		QObject::connect(eElev, &QLineEdit::editingFinished, d, [this]() {
			bool ok = false;
			const double v = eElev->text().trimmed().toDouble(&ok);
			if (!ok) return;
			elevDial->elev = std::clamp(v, 0.0, 90.0);
			elevDial->update();
		});

		QObject::connect(models, &QButtonGroup::idClicked, d, [this](int id) { setModel(id); });
		// The false colour's algorithm radio decides whether elevation + Amp factor are read at all,
		// so it re-runs the same show/hide pass (Mirone's radio_oldAlgo_CB / radio_grdgrad_CB).
		QObject::connect(algos, &QButtonGroup::idClicked, d, [this](int) { setModel(model); });
		QObject::connect(okBtn, &QPushButton::clicked, d, [this]() { apply(); });

		if (auto *b = models->button(model)) b->setChecked(true);
		setModel(model);
		d->adjustSize();
		d->resize(d->minimumSizeHint());
	}

	// shading_params.m's show_needed + toggle_uis: only the controls the picked model actually reads
	// are shown, and the window title says which model that is.
	void setModel(int m) {
		model = m;
		const bool merc  = (m == 5);
		const bool takesAzim = (m == 1 || m == 2 || m == 3 || m == 4 || m == 6);
		// The false colour's OLD algorithm reads the elevation and the Amp factor; its grdgradient
		// flavour reads neither — Mirone's radio_grdgrad_CB disables edit_elev for exactly that reason.
		const bool oldAlgo   = merc && rbOldAlgo->isChecked();
		const bool takesElev = (m == 2 || m == 3 || m == 4) || oldAlgo;
		const bool takesRefl = (m == 3);
		const bool takesWave = (m == 6);
		lAzim->setVisible(takesAzim || merc);
		eAzim->setVisible(takesAzim);
		eAzR->setVisible(merc);  eAzG->setVisible(merc);  eAzB->setVisible(merc);
		elevWrap->setVisible(takesElev);  eElev->setVisible(takesElev);
		reflBox->setVisible(takesRefl);   // the three panels share one cell; exactly one shows
		waveBox->setVisible(takesWave);
		fcBox->setVisible(merc);
		eAmp->setVisible(oldAlgo);        // the Amp factor belongs to the old algorithm alone
		if (auto *lbl = fcBox->layout() ? qobject_cast<QFormLayout *>(fcBox->layout()) : nullptr)
			if (QWidget *w = lbl->labelForField(eAmp)) w->setVisible(oldAlgo);
		dial->setVisible(takesAzim || merc);
		dial->three = merc;
		if (merc) {                       // the red hand is shared: carry it over both ways
			bool ok = false;
			const double v = eAzR->text().trimmed().toDouble(&ok);
			if (ok) dial->az[0] = v;
		}
		else {
			bool ok = false;
			const double v = eAzim->text().trimmed().toDouble(&ok);
			if (ok) dial->az[0] = v;
		}
		dial->update();
		static const struct { int m; const char *title; } names[] = {
			{ 1, "GMT grdgradient" }, { 2, "GMT grdgradient - Lambertian" },
			{ 3, "Lambertian lighting" }, { 4, "Hillshade" }, { 5, "False color" },
			{ 6, "Dynamic Range Compression" },
		};
		for (const auto &n : names)
			if (n.m == m) dlg->setWindowTitle(n.title);
		dlg->adjustSize();
		dlg->resize(dlg->minimumSizeHint());   // shrink back when a bigger panel is swapped out
	}

	// The ✕ button: strip the illumination off the grid NOW. Model 7 is the host's "remove" code; the
	// dialog itself is left exactly as it was, so the light you had aimed is still there to re-apply.
	void applyRemove() {
		if (!g_juliaHillshade || !sceneAlive(scn)) return;
		const QByteArray p = QString("model=7\nazim=0\nelev=0\ngrid=%1\n")
		                     .arg(QString::fromStdString(activeGridName(scn))).toUtf8();
		g_juliaHillshade(scn, p.constData());
	}

	// STANDING RULE: only this action button runs anything — no edit box ever triggers a compute.
	void apply() {
		if (!g_juliaHillshade) {
			QMessageBox::warning(dlg, "Error", "Illumination: callback not registered "
			                                   "(rebuild/restart needed?).");
			return;
		}
		// The tool illuminates THE GRID THE WINDOW IS CURRENTLY SHOWING, so that — and only that — is
		// what decides whether it can run. Asked through the SAME resolveActiveGrid the colorbar, the Z
		// axis and the hover readout go through (activeGridName), never off scn->surf/imageOnly: those
		// describe how the window was FIRST built, so a window opened on an image with a grid dropped on
		// top of it (an Ocean Color browse image, then its L3 grid) is still `imageOnly` and was refused
		// even though a grid was plainly on screen.
		const std::string gname = sceneAlive(scn) ? activeGridName(scn) : std::string();
		if (gname.empty()) {
			QMessageBox::warning(dlg, "Error", "Illumination works on a grid, and this window is not "
			                                   "showing one.");
			return;
		}
		HillshadeState st;
		st.valid = true;  st.model = model;
		st.azim = eAzim->text().trimmed().toDouble();
		st.elev = eElev->text().trimmed().toDouble();
		st.azR = eAzR->text().trimmed().toDouble();
		st.azG = eAzG->text().trimmed().toDouble();
		st.azB = eAzB->text().trimmed().toDouble();
		st.ambient = eAmbient->text();  st.diffuse = eDiffuse->text();
		st.specular = eSpecular->text();  st.shine = eShine->text();
		st.wavelength = eWave->text().trimmed();
		st.amp = eAmp->text().trimmed();
		st.oldAlgo = rbOldAlgo->isChecked();
		g_hillshadeState = st;

		QStringList kv;
		kv << QString("model=%1").arg(model);
		kv << "grid=" + QString::fromStdString(gname);   // illuminate the DISPLAYED layer, not the base
		kv << QString("azim=%1").arg(st.azim);
		kv << QString("elev=%1").arg(st.elev);
		if (model == 3) {
			kv << "ambient="  + st.ambient.trimmed();
			kv << "diffuse="  + st.diffuse.trimmed();
			kv << "specular=" + st.specular.trimmed();
			kv << "shine="    + st.shine.trimmed();
		}
		if (model == 5) {
			kv << QString("azimR=%1").arg(st.azR);
			kv << QString("azimG=%1").arg(st.azG);
			kv << QString("azimB=%1").arg(st.azB);
			kv << QString("oldalgo=%1").arg(st.oldAlgo ? 1 : 0);
			if (st.oldAlgo && !st.amp.isEmpty()) kv << "amp=" + st.amp;
		}
		if (model == 6 && !st.wavelength.isEmpty()) kv << "wavelength=" + st.wavelength;

		const QByteArray p = kv.join('\n').toUtf8();
		QApplication::setOverrideCursor(Qt::WaitCursor);
		const int ok = g_juliaHillshade(scn, p.constData());
		QApplication::restoreOverrideCursor();
		if (!ok)
			QMessageBox::warning(dlg, "Illumination", "The illumination could not be computed. "
			                                          "See the Julia console for the reason.");
	}
};

class GrdGradientDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QListWidget *azim1List, *azim2List;
	QCheckBox *slopeChk, *aspectChk, *downSlopeChk;
	QComboBox *dirCb, *boundaryCb, *normCb, *lambertCb, *saveStatsCb;
	QLineEdit *ampEdit, *sigmaEdit, *offsetEdit;
	QLineEdit *lambAzimEdit, *lambElevEdit, *ambientEdit, *difuseEdit, *specularEdit, *shineEdit;
	QLineEdit *azimGridEdit, *xminEdit, *xmaxEdit, *yminEdit, *ymaxEdit, *outfileEdit;

	explicit GrdGradientDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/grdgradient_dialog.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("GrdGradientDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("GrdGradientDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;

		azim1List = d->findChild<QListWidget *>("list_azim1");
		azim2List = d->findChild<QListWidget *>("list_azim2");
		slopeChk  = d->findChild<QCheckBox *>("chk_slope");
		aspectChk = d->findChild<QCheckBox *>("chk_aspect");
		dirCb      = d->findChild<QComboBox *>("cb_dirFlavour");
		boundaryCb = d->findChild<QComboBox *>("cb_boundary");
		normCb     = d->findChild<QComboBox *>("cb_norm");
		ampEdit    = d->findChild<QLineEdit *>("le_amp");
		sigmaEdit  = d->findChild<QLineEdit *>("le_sigma");
		offsetEdit = d->findChild<QLineEdit *>("le_offset");
		downSlopeChk = d->findChild<QCheckBox *>("chk_downSlope");
		lambertCb    = d->findChild<QComboBox *>("cb_lambert");
		saveStatsCb  = d->findChild<QComboBox *>("cb_saveStats");
		lambAzimEdit = d->findChild<QLineEdit *>("le_lambAzim");
		lambElevEdit = d->findChild<QLineEdit *>("le_lambElev");
		ambientEdit  = d->findChild<QLineEdit *>("le_ambient");
		difuseEdit   = d->findChild<QLineEdit *>("le_difuse");
		specularEdit = d->findChild<QLineEdit *>("le_specular");
		shineEdit    = d->findChild<QLineEdit *>("le_shine");
		azimGridEdit = d->findChild<QLineEdit *>("le_azimGrid");
		xminEdit = d->findChild<QLineEdit *>("le_xmin");  xmaxEdit = d->findChild<QLineEdit *>("le_xmax");
		yminEdit = d->findChild<QLineEdit *>("le_ymin");  ymaxEdit = d->findChild<QLineEdit *>("le_ymax");
		outfileEdit = d->findChild<QLineEdit *>("le_outfile");
		auto *azimGridBtn = d->findChild<QToolButton *>("btn_azimGridBrowse");
		auto *outBtn      = d->findChild<QToolButton *>("btn_outfileBrowse");
		auto *okBtn = d->findChild<QPushButton *>("btn_ok");

		// Both lists are Mirone's: a BLANK first row (nothing selected) then 0..359, so row 0 means
		// "no azimuth". Rows are tightened to a single line of text (tightenListRows) — Mirone-tight,
		// not Qt's default padded height.
		for (QListWidget *lw : {azim1List, azim2List}) {
			lw->addItem(" ");
			for (int a = 0; a < 360; ++a) lw->addItem(QString::number(a));
			tightenListRows(lw);
		}

		const GrdGradientState &st = g_grdgradState;
		azim1List->setCurrentRow((st.valid && st.azim1 >= 0 && st.azim1 < 360) ? st.azim1 + 1 : 0);
		azim2List->setCurrentRow((st.valid && st.azim2 >= 0 && st.azim2 < 360) ? st.azim2 + 1 : 0);
		if (st.valid) {
			slopeChk->setChecked(st.slope);   aspectChk->setChecked(st.aspect);
			downSlopeChk->setChecked(st.downSlope);
			dirCb->setCurrentIndex(st.dirFlavour);
			boundaryCb->setCurrentIndex(st.boundary);
			normCb->setCurrentIndex(st.norm);
			ampEdit->setText(st.amp);  sigmaEdit->setText(st.sigma);  offsetEdit->setText(st.offset);
			lambertCb->setCurrentIndex(st.lambert);
			saveStatsCb->setCurrentIndex(st.saveStats);
			lambAzimEdit->setText(st.lambAzim);  lambElevEdit->setText(st.lambElev);
			ambientEdit->setText(st.ambient);    difuseEdit->setText(st.difuse);
			specularEdit->setText(st.specular);  shineEdit->setText(st.shine);
			azimGridEdit->setText(st.azimGrid);  outfileEdit->setText(st.outfile);
			xminEdit->setText(st.xmin);  xmaxEdit->setText(st.xmax);
			yminEdit->setText(st.ymin);  ymaxEdit->setText(st.ymax);
		}
		// Region DESCRIBES THE INPUT GRID (the window's own), so it is filled from it, not retyped.
		if (scene && scene->surf && !scene->emptyStart && st.xmin.isEmpty()) {
			double x0 = scene->gx0, x1 = scene->gx1, y0 = scene->gy0, y1 = scene->gy1;
			if (!(x1 > x0 && y1 > y0)) { x0 = scene->x0; x1 = scene->x1; y0 = scene->y0; y1 = scene->y1; }
			if (x1 > x0 && y1 > y0) {
				xminEdit->setText(QString::number(x0, 'g', 10)); xmaxEdit->setText(QString::number(x1, 'g', 10));
				yminEdit->setText(QString::number(y0, 'g', 10)); ymaxEdit->setText(QString::number(y1, 'g', 10));
			}
		}
		azim1List->scrollToItem(azim1List->currentItem(), QAbstractItemView::PositionAtCenter);

		auto saveState = [this]() {
			GrdGradientState s;
			s.valid = true;
			s.azim1 = azim1List->currentRow() - 1;            // row 0 is the blank "not used" entry
			s.azim2 = azim2List->currentRow() - 1;
			s.slope = slopeChk->isChecked();  s.aspect = aspectChk->isChecked();
			s.downSlope = downSlopeChk->isChecked();
			s.dirFlavour = dirCb->currentIndex();
			s.boundary = boundaryCb->currentIndex();
			s.norm = normCb->currentIndex();
			s.amp = ampEdit->text();  s.sigma = sigmaEdit->text();  s.offset = offsetEdit->text();
			s.lambert = lambertCb->currentIndex();
			s.saveStats = saveStatsCb->currentIndex();
			s.lambAzim = lambAzimEdit->text();  s.lambElev = lambElevEdit->text();
			s.ambient = ambientEdit->text();    s.difuse = difuseEdit->text();
			s.specular = specularEdit->text();  s.shine = shineEdit->text();
			s.azimGrid = azimGridEdit->text();  s.outfile = outfileEdit->text();
			s.xmin = xminEdit->text();  s.xmax = xmaxEdit->text();
			s.ymin = yminEdit->text();  s.ymax = ymaxEdit->text();
			g_grdgradState = s;
		};
		struct GrdGradientSaveOnClose : QObject {
			std::function<void()> save;
			GrdGradientSaveOnClose(QObject *p, std::function<void()> fn) : QObject(p), save(fn) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close) save();
				return QObject::eventFilter(o, e);
			}
		};
		d->installEventFilter(new GrdGradientSaveOnClose(d, saveState));

		// --- Mirone's interlocks, verbatim (grdgradient_mir.m). The azimuth lists are never disabled:
		// picking an azimuth CLEARS the two checkboxes, and ticking a checkbox blanks the azimuths.
		// The two checkboxes are mutually exclusive without being radio buttons, since both may be off.
		//   listbox_azim1_CB : row 0 -> also blank azim2; row > 0 -> uncheck slope + direction
		//   listbox_azim2_CB : azim2 cannot be set while azim1 is blank
		//   check_*_CB       : blank both azimuths, uncheck the other box; the flavour combo follows
		//                      the direction box only
		QObject::connect(azim1List, &QListWidget::currentRowChanged, d, [this](int row) {
			if (row <= 0) { azim2List->setCurrentRow(0); return; }
			QSignalBlocker b1(slopeChk), b2(aspectChk);
			slopeChk->setChecked(false);  aspectChk->setChecked(false);
			dirCb->setEnabled(false);
		});
		QObject::connect(azim2List, &QListWidget::currentRowChanged, d, [this](int row) {
			if (row > 0 && azim1List->currentRow() == 0) azim2List->setCurrentRow(0);
		});
		auto checkPicked = [this](QCheckBox *me, QCheckBox *other) {
			if (me->isChecked()) {
				QSignalBlocker b1(azim1List), b2(azim2List), b3(other);
				azim1List->setCurrentRow(0);  azim2List->setCurrentRow(0);
				other->setChecked(false);
			}
			// The flavour combo and the -Da switch belong to the direction box alone (Mirone enables
			// the combo there only; -Da is the module option Mirone's window never had).
			dirCb->setEnabled(aspectChk->isChecked());
			downSlopeChk->setEnabled(aspectChk->isChecked());
		};
		QObject::connect(slopeChk,  &QCheckBox::toggled, d, [this, checkPicked](bool) { checkPicked(slopeChk, aspectChk); });
		QObject::connect(aspectChk, &QCheckBox::toggled, d, [this, checkPicked](bool) { checkPicked(aspectChk, slopeChk); });
		dirCb->setEnabled(aspectChk->isChecked());
		downSlopeChk->setEnabled(aspectChk->isChecked());

		// --- Lambertian radiance (E). GMT states plainly that -E overrides -A, -D and -S, so picking a
		// radiance grays the whole Gradient tab rather than letting two conflicting requests go out.
		// Peucker and the ESRI-like "manip" hard-wire azimuth/elevation to 315/45 (the module IGNORES
		// whatever is typed), and the four reflectance parameters exist only for the full Lambertian —
		// so each of those follows the mode, grayed when it would be a lie.
		auto syncLambert = [this]() {
			const int m = lambertCb->currentIndex();          // 0 none, 1 full, 2 simple, 3 Peucker, 4 manip
			const bool on = m > 0;
			const bool takesView = (m == 1 || m == 2);
			const bool takesRefl = (m == 1);
			lambAzimEdit->setEnabled(takesView);
			lambElevEdit->setEnabled(takesView);
			for (QLineEdit *e : {ambientEdit, difuseEdit, specularEdit, shineEdit}) e->setEnabled(takesRefl);
			for (QWidget *w : {(QWidget *)azim1List, (QWidget *)azim2List, (QWidget *)slopeChk,
			                   (QWidget *)aspectChk, (QWidget *)dirCb, (QWidget *)downSlopeChk,
			                   (QWidget *)azimGridEdit})
				w->setEnabled(!on);
			if (!on) {                                        // hand the Gradient tab back its own rules
				dirCb->setEnabled(aspectChk->isChecked());
				downSlopeChk->setEnabled(aspectChk->isChecked());
			}
		};
		QObject::connect(lambertCb, &QComboBox::currentIndexChanged, d, [syncLambert](int) { syncLambert(); });
		syncLambert();

		// --- Normalization boxes, Mirone's edit callbacks: amp needs a normalization picked and a
		// positive number, sigma needs amp, offset needs sigma; a box that fails its test is cleared,
		// along with the ones that depend on it. All three stay enabled (Mirone never grays them).
		auto normEditCheck = [this]() {
			if (normCb->currentIndex() == 0) {                      // no normalization -> no arguments
				ampEdit->clear();  sigmaEdit->clear();  offsetEdit->clear();
				return;
			}
			bool ok = false;
			if (ampEdit->text().trimmed().toDouble(&ok) <= 0 || !ok) {
				if (!ampEdit->text().trimmed().isEmpty()) ampEdit->clear();
				sigmaEdit->clear();  offsetEdit->clear();
				return;
			}
			if (sigmaEdit->text().trimmed().toDouble(&ok) <= 0 || !ok) {
				if (!sigmaEdit->text().trimmed().isEmpty()) sigmaEdit->clear();
				offsetEdit->clear();
				return;
			}
			if (offsetEdit->text().trimmed().toDouble(&ok) <= 0 || !ok)
				if (!offsetEdit->text().trimmed().isEmpty()) offsetEdit->clear();
		};
		for (QLineEdit *e : {ampEdit, sigmaEdit, offsetEdit})
			QObject::connect(e, &QLineEdit::editingFinished, d, [normEditCheck]() { normEditCheck(); });
		QObject::connect(normCb, &QComboBox::currentIndexChanged, d, [this](int idx) {
			if (idx == 0) { ampEdit->clear();  sigmaEdit->clear();  offsetEdit->clear(); }
		});
		for (QLineEdit *e : {ampEdit, sigmaEdit, offsetEdit}) e->setEnabled(true);

		// Mirone's per-group "?" notes. Text is the module's own documentation, condensed.
		auto note = [d](QToolButton *b, const QString &title, const QString &text) {
			if (!b) return;
			QObject::connect(b, &QToolButton::clicked, d, [d, title, text]() {
				QMessageBox::information(d, title, text);
			});
		};
		note(d->findChild<QToolButton *>("btn_helpLight"), "Horizontal Light Angles",
			"Azimuth of the light source, in degrees clockwise from north (the +y direction) toward "
			"east (+x). The NEGATIVE of the directional derivative is computed, so values come out "
			"positive where the surface slopes downhill in that direction — the right sense for "
			"illuminating an image.\n\n"
			"Give a second azimuth to compute the gradient in both directions and keep whichever is "
			"larger in magnitude: useful for data with two directions of lineated structure "
			"(e.g. 0 and 270 lights from the north and from the west).");
		note(d->findChild<QToolButton *>("btn_helpDir"), "Slope and gradient direction",
			"Compute Slope returns the magnitude of the gradient vectors (as a percentage).\n\n"
			"Gradient direction returns the ASPECT, the down-slope direction, instead of the "
			"up-slope one.\n\n"
			"The combo picks how those angles are reported: azimuthal (clockwise from north) or "
			"cartesian (counter-clockwise from east); as orientations (0-180) instead of directions "
			"(0-360); or with 90 degrees added, which turns them into local strikes of the surface.");
		note(d->findChild<QToolButton *>("btn_helpBoundary"), "Boundary condition",
			"The first and last row/column of the result depend on what is assumed beyond the grid "
			"edges.\n\n"
			"Empty lets GMT decide. Geographic is right for a global lon/lat grid (the edges wrap and "
			"the poles are handled); periodic wraps in x and y; natural applies a natural-spline "
			"(zero second derivative) edge.");
		note(d->findChild<QToolButton *>("btn_helpNorm"), "Normalization",
			"Without normalization the raw gradients are returned. Otherwise they are offset and "
			"scaled to a maximum magnitude of amp [1]; offset defaults to the average gradient.\n\n"
			"linear:  gn = amp * (g - offset) / max(abs(g - offset))\n"
			"Laplace: a cumulative Laplace distribution, sigma estimated by the L1 norm if empty\n"
			"Cauchy:  a cumulative Cauchy distribution, sigma estimated by the L2 norm if empty\n\n"
			"For an intensity grid to illuminate an image, a good first try is Laplace with "
			"amp = 0.6.");

		note(d->findChild<QToolButton *>("btn_helpLambert"), "Lambertian radiance",
			"An ideal surface that reflects all the light striking it and looks equally bright from "
			"every viewing direction — the shading to feed grdimage/grdview. It OVERRIDES the Gradient "
			"tab.\n\n"
			"Full reflectance takes the light vector plus ambient [0.55], diffuse [0.6], specular "
			"[0.4] and shine [10]. The simple algorithm needs only the light vector. Peucker is a "
			"piecewise linear approximation — faster, and its azimuth/elevation are hardwired to "
			"315/45. The ESRI-like hillshade is likewise hardwired to 315/45.");

		if (azimGridBtn) QObject::connect(azimGridBtn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getOpenFileName(d, "Select the azimuth grid", prefStartDir(),
			                                          "Grid files (*.nc *.grd *.tif *.tiff);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			azimGridEdit->setText(fn);
		});
		if (outBtn) QObject::connect(outBtn, &QToolButton::clicked, d, [this, d]() {
			QString fn = QFileDialog::getSaveFileName(d, "Save result as", prefStartDir(),
			                                          "Grid files (*.nc *.grd);;All files (*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			outfileEdit->setText(fn);
		});
		// Standing rule: double-clicking any file box opens the same chooser its "..." button does.
		fileBoxDoubleClick(azimGridEdit, azimGridBtn);
		fileBoxDoubleClick(outfileEdit,  outBtn);

		// STANDING RULE: an "OR Ref grid" row under every Region. (grdgradient has no -I.)
		addRefGridRow(d, d->findChild<QGridLayout *>("gridLayout_region"),
		              xminEdit, xmaxEdit, yminEdit, ymaxEdit);
		addManualButton(d, "grdgradient");

		// Open at the smallest size the .ui's own layout allows, not at whatever Qt inflates the
		// window to. Done LAST, after every widget is populated and every maximumHeight in the .ui is
		// in effect, so the minimum is computed from the finished layout.
		d->adjustSize();
		d->resize(d->minimumSizeHint());

		if (okBtn) QObject::connect(okBtn, &QPushButton::clicked, d, [this, d, saveState]() {
			if (!g_juliaGrdGradient) {
				QMessageBox::warning(d, "Error", "grdgradient: callback not registered (rebuild/restart needed?).");
				return;
			}
			if (!(scn && scn->surf && !scn->emptyStart && !scn->imageOnly)) {
				QMessageBox::warning(d, "Error", "grdgradient works on the grid loaded in this window, "
				                                 "and this window has none.");
				return;
			}
			const int lamb = lambertCb->currentIndex();
			const int a1 = azim1List->currentRow() - 1, a2 = azim2List->currentRow() - 1;
			const QString azimGrid = azimGridEdit->text().trimmed();
			// Mirone's own consistency check, extended to the options its window never had: a radiance
			// or an azimuth grid is just as much "something to do" as an azimuth or a checkbox.
			if (lamb == 0 && azimGrid.isEmpty() && a1 < 0 && !slopeChk->isChecked() && !aspectChk->isChecked()) {
				QMessageBox::warning(d, "Chico Clever", "You haven't select anything usefull to do.");
				return;
			}
			saveState();

			QStringList kv;
			if (lamb > 0) {
				// -E overrides -A/-D/-S, so nothing from the Gradient tab travels with it.
				static const char *lm[] = { "", "full", "simple", "peucker", "manip" };
				kv << QString("lambert=") + lm[lamb];
				if (lambAzimEdit->isEnabled() && !lambAzimEdit->text().trimmed().isEmpty())
					kv << "lambazim=" + lambAzimEdit->text().trimmed();
				if (lambElevEdit->isEnabled() && !lambElevEdit->text().trimmed().isEmpty())
					kv << "lambelev=" + lambElevEdit->text().trimmed();
				for (auto pr : { std::make_pair(QString("ambient"), ambientEdit),
				                 std::make_pair(QString("difuse"), difuseEdit),
				                 std::make_pair(QString("specular"), specularEdit),
				                 std::make_pair(QString("shine"), shineEdit) }) {
					if (pr.second->isEnabled() && !pr.second->text().trimmed().isEmpty())
						kv << pr.first + "=" + pr.second->text().trimmed();
				}
			}
			else {
				// An azimuth GRID replaces the two lists: the module updates the azimuth per node.
				if (!azimGrid.isEmpty()) kv << "azimgrid=" + azimGrid;
				else if (a1 >= 0) {
					kv << QString("azim=%1").arg(a1);
					if (a2 >= 0) kv << QString("azim2=%1").arg(a2);
				}
				// Slope forces -D as well (Mirone: opt_S = '-Sp'; opt_D = '-D'), and the flavour letters
				// belong to the direction box: a = down-slope (aspect), c = trigonometric angles,
				// o = 0-180 orientations, n = +90.
				if (slopeChk->isChecked()) {
					kv << "finddir=";
					kv << "slope=1";
				}
				else if (aspectChk->isChecked()) {
					// ASPECT is the DOWN-slope azimuth, which is -Da. A bare -D is the UP-slope
					// direction, 180 degrees away — Mirone's window sends that despite labelling its
					// box "Aspect", and copying it verbatim made this checkbox report the wrong angle.
					// The "report down-slope" tick therefore only remains as a way to turn it OFF.
					static const char *flav[] = { "", "c", "o", "n" };
					QString flags = downSlopeChk->isChecked() ? "a" : "";
					flags += flav[dirCb->currentIndex()];
					kv << "finddir=" + flags;
				}
			}
			// Mirone's -Lx / -Ly / -Lxy / -Lg, which GMT6 spells as the -n option's +b modifier.
			static const char *bc[] = { "", "x", "y", "xy", "g" };
			if (boundaryCb->currentIndex() > 0) kv << QString("boundary=") + bc[boundaryCb->currentIndex()];
			// -N / -Ne / -Nt, with amp then /sigma then /offset appended, exactly as Mirone builds it.
			static const char *nm[] = { "", "simple", "laplace", "cauchy" };
			if (normCb->currentIndex() > 0) {
				kv << QString("norm=") + nm[normCb->currentIndex()];
				if (!ampEdit->text().trimmed().isEmpty())    kv << "amp=" + ampEdit->text().trimmed();
				if (!sigmaEdit->text().trimmed().isEmpty())  kv << "sigma=" + sigmaEdit->text().trimmed();
				if (!offsetEdit->text().trimmed().isEmpty()) kv << "offset=" + offsetEdit->text().trimmed();
			}
			// -Q: reuse one offset/sigma across grids or tiles (c = save, r = read, R = read + delete).
			static const char *qs[] = { "", "c", "r", "R" };
			if (saveStatsCb->currentIndex() > 0) kv << QString("savestats=") + qs[saveStatsCb->currentIndex()];
			// -R and -G. All four region boxes or none.
			const QString xm = xminEdit->text().trimmed(), xM = xmaxEdit->text().trimmed();
			const QString ym = yminEdit->text().trimmed(), yM = ymaxEdit->text().trimmed();
			const int nReg = (!xm.isEmpty()) + (!xM.isEmpty()) + (!ym.isEmpty()) + (!yM.isEmpty());
			if (nReg == 4)      kv << "region=" + xm + "/" + xM + "/" + ym + "/" + yM;
			else if (nReg != 0) {
				QMessageBox::warning(d, "Error", "Give all four Region boxes, or leave all four empty "
				                                 "to use the input grid's own region.");
				return;
			}
			if (!outfileEdit->text().trimmed().isEmpty()) kv << "outfile=" + outfileEdit->text().trimmed();

			showBusyDialog("Computing the gradient…");
			const int ok = g_juliaGrdGradient(scn, kv.join('\n').toUtf8().constData());
			closeBusyDialog();
			if (!ok)
				QMessageBox::warning(d, "Error", "grdgradient failed — see this window's Errors console for details.");
		});

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}
};

// ============================================================================================
// grdseamount (GMT menu) — synthetic seamounts (Gaussian, parabolic, polynomial, cone or disc;
// circular or elliptical) built from a TABLE of seamount parameters, not from an input grid. Runtime
// .ui load, same shape as the other module dialogs here.
//
// The table is either a file or, for the common one-seamount case, typed into the dialog: the
// Elliptical box decides which of the two column layouts is live (lon/lat/radius/height, or
// lon/lat/azimuth/semi-major/semi-minor/height), and the other set grays out rather than sitting
// there inviting values the module would never read.
//
// -S (the landslide simulation) is deliberately NOT here: it is a family of eight coupled modifiers
// that deserves its own dialog rather than a cramped corner of this one.
// ============================================================================================
struct GrdSeamountState {
	bool valid = false;
	bool fromFile = true, elliptical = false, flatCol = false, levelNaN = false, listStats = false;
	bool mask = false, time = false, density = false, logTime = false;
	int shape = 0, unit = 0, bmode = 0, fmode = 0;
	QString table, lon, lat, height, radius, azimuth, semiMajor, semiMinor, flattening;
	QString xmin, xmax, ymin, ymax, xinc, yinc, level, normalize, outgrid;
	QString maskOut, maskIn, maskScale, t0, t1, dt, list;
	QString H, rhoL, rhoH, densify, power, densityGrid, densityOut;
};
static GrdSeamountState g_grdseamountState;

class GrdSeamountDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QRadioButton *rbFile, *rbSingle;
	QCheckBox *ellipChk, *flatColChk, *levelNaNChk, *listStatsChk, *logTimeChk;
	QGroupBox *maskGb, *timeGb, *densityGb;
	QComboBox *shapeCb, *unitCb, *bmodeCb, *fmodeCb;
	QLineEdit *tableEdit, *lonEdit, *latEdit, *heightEdit, *radiusEdit, *azimuthEdit,
	          *semiMajorEdit, *semiMinorEdit, *flatteningEdit;
	QLineEdit *xminEdit, *xmaxEdit, *yminEdit, *ymaxEdit, *xincEdit, *yincEdit,
	          *levelEdit, *normalizeEdit, *outgridEdit;
	QLineEdit *maskOutEdit, *maskInEdit, *maskScaleEdit, *t0Edit, *t1Edit, *dtEdit, *listEdit;
	QLineEdit *hEdit, *rhoLEdit, *rhoHEdit, *densifyEdit, *powerEdit, *densityGridEdit, *densityOutEdit;

	explicit GrdSeamountDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/grdseamount_dialog.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("GrdSeamountDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("GrdSeamountDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;

		rbFile   = d->findChild<QRadioButton *>("rb_fromFile");
		rbSingle = d->findChild<QRadioButton *>("rb_single");
		ellipChk = d->findChild<QCheckBox *>("chk_elliptical");
		flatColChk  = d->findChild<QCheckBox *>("chk_flatCol");
		levelNaNChk = d->findChild<QCheckBox *>("chk_levelNaN");
		listStatsChk= d->findChild<QCheckBox *>("chk_listStats");
		logTimeChk  = d->findChild<QCheckBox *>("chk_logTime");
		maskGb    = d->findChild<QGroupBox *>("gb_mask");
		timeGb    = d->findChild<QGroupBox *>("gb_time");
		densityGb = d->findChild<QGroupBox *>("gb_density");
		shapeCb = d->findChild<QComboBox *>("cb_shape");  unitCb  = d->findChild<QComboBox *>("cb_unit");
		bmodeCb = d->findChild<QComboBox *>("cb_bmode");  fmodeCb = d->findChild<QComboBox *>("cb_fmode");
		tableEdit  = d->findChild<QLineEdit *>("le_table");
		lonEdit    = d->findChild<QLineEdit *>("le_lon");     latEdit    = d->findChild<QLineEdit *>("le_lat");
		heightEdit = d->findChild<QLineEdit *>("le_height");  radiusEdit = d->findChild<QLineEdit *>("le_radius");
		azimuthEdit= d->findChild<QLineEdit *>("le_azimuth");
		semiMajorEdit = d->findChild<QLineEdit *>("le_semiMajor");
		semiMinorEdit = d->findChild<QLineEdit *>("le_semiMinor");
		flatteningEdit= d->findChild<QLineEdit *>("le_flattening");
		xminEdit = d->findChild<QLineEdit *>("le_xmin");  xmaxEdit = d->findChild<QLineEdit *>("le_xmax");
		yminEdit = d->findChild<QLineEdit *>("le_ymin");  ymaxEdit = d->findChild<QLineEdit *>("le_ymax");
		xincEdit = d->findChild<QLineEdit *>("le_xinc");  yincEdit = d->findChild<QLineEdit *>("le_yinc");
		levelEdit = d->findChild<QLineEdit *>("le_level");
		normalizeEdit = d->findChild<QLineEdit *>("le_normalize");
		outgridEdit   = d->findChild<QLineEdit *>("le_outgrid");
		maskOutEdit = d->findChild<QLineEdit *>("le_maskOut");
		maskInEdit  = d->findChild<QLineEdit *>("le_maskIn");
		maskScaleEdit = d->findChild<QLineEdit *>("le_maskScale");
		t0Edit = d->findChild<QLineEdit *>("le_t0");  t1Edit = d->findChild<QLineEdit *>("le_t1");
		dtEdit = d->findChild<QLineEdit *>("le_dt");  listEdit = d->findChild<QLineEdit *>("le_list");
		hEdit = d->findChild<QLineEdit *>("le_H");
		rhoLEdit = d->findChild<QLineEdit *>("le_rhoL");  rhoHEdit = d->findChild<QLineEdit *>("le_rhoH");
		densifyEdit = d->findChild<QLineEdit *>("le_densify");  powerEdit = d->findChild<QLineEdit *>("le_power");
		densityGridEdit = d->findChild<QLineEdit *>("le_densityGrid");
		densityOutEdit  = d->findChild<QLineEdit *>("le_densityOut");
		auto *tableBtn = d->findChild<QToolButton *>("btn_tableBrowse");
		auto *demoBtn  = d->findChild<QToolButton *>("btn_demo");
		auto *outBtn   = d->findChild<QToolButton *>("btn_outgridBrowse");
		auto *listBtn  = d->findChild<QToolButton *>("btn_listBrowse");
		auto *dgBtn    = d->findChild<QToolButton *>("btn_densityGridBrowse");
		auto *doBtn    = d->findChild<QToolButton *>("btn_densityOutBrowse");
		auto *computeBtn = d->findChild<QPushButton *>("btn_compute");
		auto *closeBtn   = d->findChild<QPushButton *>("btn_close");

		const GrdSeamountState &st = g_grdseamountState;
		if (st.valid) {
			(st.fromFile ? rbFile : rbSingle)->setChecked(true);
			ellipChk->setChecked(st.elliptical);   flatColChk->setChecked(st.flatCol);
			levelNaNChk->setChecked(st.levelNaN);  listStatsChk->setChecked(st.listStats);
			logTimeChk->setChecked(st.logTime);
			maskGb->setChecked(st.mask);  timeGb->setChecked(st.time);  densityGb->setChecked(st.density);
			shapeCb->setCurrentIndex(st.shape);  unitCb->setCurrentIndex(st.unit);
			bmodeCb->setCurrentIndex(st.bmode);  fmodeCb->setCurrentIndex(st.fmode);
			tableEdit->setText(st.table);
			lonEdit->setText(st.lon);        latEdit->setText(st.lat);
			heightEdit->setText(st.height);  radiusEdit->setText(st.radius);
			azimuthEdit->setText(st.azimuth);
			semiMajorEdit->setText(st.semiMajor);  semiMinorEdit->setText(st.semiMinor);
			flatteningEdit->setText(st.flattening);
			xminEdit->setText(st.xmin);  xmaxEdit->setText(st.xmax);
			yminEdit->setText(st.ymin);  ymaxEdit->setText(st.ymax);
			xincEdit->setText(st.xinc);  yincEdit->setText(st.yinc);
			levelEdit->setText(st.level);  normalizeEdit->setText(st.normalize);
			outgridEdit->setText(st.outgrid);
			maskOutEdit->setText(st.maskOut);  maskInEdit->setText(st.maskIn);
			maskScaleEdit->setText(st.maskScale);
			t0Edit->setText(st.t0);  t1Edit->setText(st.t1);  dtEdit->setText(st.dt);
			listEdit->setText(st.list);
			hEdit->setText(st.H);  rhoLEdit->setText(st.rhoL);  rhoHEdit->setText(st.rhoH);
			densifyEdit->setText(st.densify);  powerEdit->setText(st.power);
			densityGridEdit->setText(st.densityGrid);  densityOutEdit->setText(st.densityOut);
		}

		auto saveState = [this]() {
			GrdSeamountState s;
			s.valid = true;
			s.fromFile = rbFile->isChecked();
			s.elliptical = ellipChk->isChecked();  s.flatCol = flatColChk->isChecked();
			s.levelNaN = levelNaNChk->isChecked(); s.listStats = listStatsChk->isChecked();
			s.logTime = logTimeChk->isChecked();
			s.mask = maskGb->isChecked();  s.time = timeGb->isChecked();  s.density = densityGb->isChecked();
			s.shape = shapeCb->currentIndex();  s.unit = unitCb->currentIndex();
			s.bmode = bmodeCb->currentIndex();  s.fmode = fmodeCb->currentIndex();
			s.table = tableEdit->text();
			s.lon = lonEdit->text();        s.lat = latEdit->text();
			s.height = heightEdit->text();  s.radius = radiusEdit->text();
			s.azimuth = azimuthEdit->text();
			s.semiMajor = semiMajorEdit->text();  s.semiMinor = semiMinorEdit->text();
			s.flattening = flatteningEdit->text();
			s.xmin = xminEdit->text();  s.xmax = xmaxEdit->text();
			s.ymin = yminEdit->text();  s.ymax = ymaxEdit->text();
			s.xinc = xincEdit->text();  s.yinc = yincEdit->text();
			s.level = levelEdit->text();  s.normalize = normalizeEdit->text();
			s.outgrid = outgridEdit->text();
			s.maskOut = maskOutEdit->text();  s.maskIn = maskInEdit->text();
			s.maskScale = maskScaleEdit->text();
			s.t0 = t0Edit->text();  s.t1 = t1Edit->text();  s.dt = dtEdit->text();
			s.list = listEdit->text();
			s.H = hEdit->text();  s.rhoL = rhoLEdit->text();  s.rhoH = rhoHEdit->text();
			s.densify = densifyEdit->text();  s.power = powerEdit->text();
			s.densityGrid = densityGridEdit->text();  s.densityOut = densityOutEdit->text();
			g_grdseamountState = s;
		};
		struct GrdSeamountSaveOnClose : QObject {
			std::function<void()> save;
			GrdSeamountSaveOnClose(QObject *p, std::function<void()> fn) : QObject(p), save(fn) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close) save();
				return QObject::eventFilter(o, e);
			}
		};
		d->installEventFilter(new GrdSeamountSaveOnClose(d, saveState));

		// File row vs typed row, and within the typed row the CIRCULAR columns vs the ELLIPTICAL ones:
		// only the set the module will actually read stays live.
		auto syncSource = [this, tableBtn]() {
			const bool file = rbFile->isChecked();
			const bool ell  = ellipChk->isChecked();
			tableEdit->setEnabled(file);
			if (tableBtn) tableBtn->setEnabled(file);
			lonEdit->setEnabled(!file);  latEdit->setEnabled(!file);  heightEdit->setEnabled(!file);
			radiusEdit->setEnabled(!file && !ell);
			azimuthEdit->setEnabled(!file && ell);
			semiMajorEdit->setEnabled(!file && ell);
			semiMinorEdit->setEnabled(!file && ell);
			// Flattening either comes from the last table column or is typed here — never both.
			flatteningEdit->setEnabled(!flatColChk->isChecked());
			flatColChk->setEnabled(file);            // a typed single seamount has no extra column
		};
		for (QRadioButton *rb : {rbFile, rbSingle})
			QObject::connect(rb, &QRadioButton::toggled, d, [syncSource](bool) { syncSource(); });
		QObject::connect(ellipChk,   &QCheckBox::toggled, d, [syncSource](bool) { syncSource(); });
		QObject::connect(flatColChk, &QCheckBox::toggled, d, [syncSource](bool) { syncSource(); });
		syncSource();

		auto browseOpen = [d](QLineEdit *target, const QString &caption, const QString &filter) {
			QString fn = QFileDialog::getOpenFileName(d, caption, prefStartDir(), filter);
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			target->setText(fn);
		};
		auto browseSave = [d](QLineEdit *target, const QString &caption, const QString &filter) {
			QString fn = QFileDialog::getSaveFileName(d, caption, prefStartDir(), filter);
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			target->setText(fn);
		};
		const QString gridFilter = "Grid files (*.nc *.grd);;All files (*)";
		if (tableBtn) QObject::connect(tableBtn, &QToolButton::clicked, d, [this, browseOpen]() {
			browseOpen(tableEdit, "Select the seamount table", "Data files (*.dat *.txt *.xy);;All files (*)"); });
		if (outBtn)  QObject::connect(outBtn,  &QToolButton::clicked, d, [this, browseSave, gridFilter]() {
			browseSave(outgridEdit, "Save the seamount grid as", gridFilter); });
		if (listBtn) QObject::connect(listBtn, &QToolButton::clicked, d, [this, browseSave]() {
			browseSave(listEdit, "Write the grid list to", "Text files (*.txt *.lis);;All files (*)"); });
		if (dgBtn)   QObject::connect(dgBtn,   &QToolButton::clicked, d, [this, browseSave, gridFilter]() {
			browseSave(densityGridEdit, "Save the density crossection as", gridFilter); });
		if (doBtn)   QObject::connect(doBtn,   &QToolButton::clicked, d, [this, browseSave, gridFilter]() {
			browseSave(densityOutEdit, "Save the mean-density grid as", gridFilter); });

		// "Demo seamount": the grdseamount example — a circular Gaussian seamount of 30 km basal radius
		// and 4500 m height on a 1 arc minute grid. It is a single record, so it lands in the typed row
		// rather than the file one.
		// The position is 1W 2S, the man page's own `echo 1W 2S 30 4500 | gmt grdseamount
		// -R1:30W/0:30W/2:30S/1:30S -I1m`. The .qmd page writes the same example as [1 2 30 4500] with
		// that same region, but 1E 2N lies OUTSIDE -1.5/-0.5/-2.5/-1.5, so it builds a grid of zeros —
		// which is what this button did until now.
		if (demoBtn) QObject::connect(demoBtn, &QToolButton::clicked, d, [this, syncSource]() {
			rbSingle->setChecked(true);
			ellipChk->setChecked(false);
			flatColChk->setChecked(false);
			shapeCb->setCurrentIndex(0);                  // gaussian
			unitCb->setCurrentIndex(0);                   // geographic: no -D
			lonEdit->setText("-1");    latEdit->setText("-2");
			radiusEdit->setText("30"); heightEdit->setText("4500");
			azimuthEdit->clear(); semiMajorEdit->clear(); semiMinorEdit->clear();
			flatteningEdit->clear();
			xminEdit->setText("-1.5"); xmaxEdit->setText("-0.5");
			yminEdit->setText("-2.5"); ymaxEdit->setText("-1.5");
			xincEdit->setText("1m");   yincEdit->clear();
			levelEdit->clear(); levelNaNChk->setChecked(false); normalizeEdit->clear();
			maskGb->setChecked(false); timeGb->setChecked(false); densityGb->setChecked(false);
			listStatsChk->setChecked(false);
			syncSource();
		});

		// Standing rules: a Ref grid row under the Region, the manual button, and a double-click in any
		// file box opening that box's own chooser.
		addRefGridRow(d, d->findChild<QGridLayout *>("gridLayout_region"),
		              xminEdit, xmaxEdit, yminEdit, ymaxEdit, xincEdit, yincEdit);
		addManualButton(d, "grdseamount");
		fileBoxDoubleClick(tableEdit,       tableBtn);
		fileBoxDoubleClick(outgridEdit,     outBtn);
		fileBoxDoubleClick(listEdit,        listBtn);
		fileBoxDoubleClick(densityGridEdit, dgBtn);
		fileBoxDoubleClick(densityOutEdit,  doBtn);

		if (closeBtn) QObject::connect(closeBtn, &QPushButton::clicked, d, [d]() { d->close(); });

		if (computeBtn) QObject::connect(computeBtn, &QPushButton::clicked, d, [this, d, saveState]() {
			if (!g_juliaGrdSeamount) {
				QMessageBox::warning(d, "Error", "grdseamount: callback not registered (rebuild/restart needed?).");
				return;
			}
			QStringList kv;
			const bool ell = ellipChk->isChecked();
			if (rbFile->isChecked()) {
				const QString t = tableEdit->text().trimmed();
				if (t.isEmpty()) {
					QMessageBox::warning(d, "Error", "Pick the table of seamount parameters.");
					return;
				}
				kv << "table=" + t;
			}
			else {
				// Build the one input record in the module's own column order.
				QStringList cols;
				cols << lonEdit->text().trimmed() << latEdit->text().trimmed();
				if (ell) cols << azimuthEdit->text().trimmed() << semiMajorEdit->text().trimmed()
				              << semiMinorEdit->text().trimmed();
				else     cols << radiusEdit->text().trimmed();
				cols << heightEdit->text().trimmed();
				for (const QString &c : cols) {
					bool ok = false;
					c.toDouble(&ok);
					if (!ok) {
						QMessageBox::warning(d, "Error", ell
							? "A typed elliptical seamount needs lon, lat, azimuth, semi-major, semi-minor and height."
							: "A typed seamount needs lon, lat, radius and height.");
						return;
					}
				}
				kv << "record=" + cols.join('/');
			}
			// R and I are REQUIRED by the module: there is no input grid to inherit them from.
			const QString xm = xminEdit->text().trimmed(), xM = xmaxEdit->text().trimmed();
			const QString ym = yminEdit->text().trimmed(), yM = ymaxEdit->text().trimmed();
			if (xm.isEmpty() || xM.isEmpty() || ym.isEmpty() || yM.isEmpty()) {
				QMessageBox::warning(d, "Error", "Fill the output Region — grdseamount has no input grid "
				                                 "to take it from.");
				return;
			}
			kv << "region=" + xm + "/" + xM + "/" + ym + "/" + yM;
			const QString xi = xincEdit->text().trimmed(), yi = yincEdit->text().trimmed();
			if (xi.isEmpty()) {
				QMessageBox::warning(d, "Error", "Give the output grid increment.");
				return;
			}
			kv << "inc=" + (yi.isEmpty() ? xi : xi + "/" + yi);

			static const char *shp[] = { "gaussian", "cone", "disc", "parabola", "polynomial" };
			kv << QString("shape=") + shp[shapeCb->currentIndex()];
			if (ell) kv << "elliptical=1";
			if (flatColChk->isChecked() && flatColChk->isEnabled()) kv << "flatcol=1";
			else if (!flatteningEdit->text().trimmed().isEmpty()) kv << "flattening=" + flatteningEdit->text().trimmed();
			static const char *un[] = { "", "k", "m", "n", "d" };
			if (unitCb->currentIndex() > 0) kv << QString("unit=") + un[unitCb->currentIndex()];
			if (!levelEdit->text().trimmed().isEmpty()) kv << "level=" + levelEdit->text().trimmed();
			if (levelNaNChk->isChecked())               kv << "levelnan=1";
			if (!normalizeEdit->text().trimmed().isEmpty()) kv << "normalize=" + normalizeEdit->text().trimmed();
			if (maskGb->isChecked()) {
				kv << "mask=1";
				if (!maskOutEdit->text().trimmed().isEmpty())   kv << "maskout=" + maskOutEdit->text().trimmed();
				if (!maskInEdit->text().trimmed().isEmpty())    kv << "maskin=" + maskInEdit->text().trimmed();
				if (!maskScaleEdit->text().trimmed().isEmpty()) kv << "maskscale=" + maskScaleEdit->text().trimmed();
			}
			if (listStatsChk->isChecked()) kv << "liststats=1";
			if (timeGb->isChecked()) {
				const QString t0 = t0Edit->text().trimmed(), t1 = t1Edit->text().trimmed(),
				              dt = dtEdit->text().trimmed();
				if (t0.isEmpty()) {
					QMessageBox::warning(d, "Error", "Temporal evolution needs at least a start time t0.");
					return;
				}
				QString T = t0;
				if (!t1.isEmpty() && !dt.isEmpty()) T += "/" + t1 + "/" + dt;
				if (logTimeChk->isChecked()) T += "+l";
				kv << "time=" + T;
				static const char *bm[] = { "c", "i" };
				static const char *fm[] = { "g", "c" };
				kv << QString("buildmode=") + bm[bmodeCb->currentIndex()] + "/" + fm[fmodeCb->currentIndex()];
				if (!listEdit->text().trimmed().isEmpty()) kv << "list=" + listEdit->text().trimmed();
				// With T set the module writes ONE grid PER TIME STEP, so it needs a filename template.
				if (outgridEdit->text().trimmed().isEmpty()) {
					QMessageBox::warning(d, "Error", "Temporal evolution writes one grid per time step, so "
					                                 "Save as must be a filename TEMPLATE, e.g. smt_%3.1f_%s.nc");
					return;
				}
			}
			if (densityGb->isChecked()) {
				const QString H = hEdit->text().trimmed(), rl = rhoLEdit->text().trimmed(),
				              rh = rhoHEdit->text().trimmed();
				if (H.isEmpty() || rl.isEmpty() || rh.isEmpty()) {
					QMessageBox::warning(d, "Error", "The density model needs H, rho low and rho high.");
					return;
				}
				kv << "densities=" + H + "/" + rl + "/" + rh;
				if (!densifyEdit->text().trimmed().isEmpty()) kv << "densify=" + densifyEdit->text().trimmed();
				if (!powerEdit->text().trimmed().isEmpty())   kv << "denspower=" + powerEdit->text().trimmed();
				if (!densityGridEdit->text().trimmed().isEmpty()) kv << "densitygrid=" + densityGridEdit->text().trimmed();
				if (!densityOutEdit->text().trimmed().isEmpty())  kv << "densityout=" + densityOutEdit->text().trimmed();
			}
			else if (!densityOutEdit->text().trimmed().isEmpty()) {
				QMessageBox::warning(d, "Error", "The mean-density grid (W) needs the density model (H) too.");
				return;
			}
			if (!outgridEdit->text().trimmed().isEmpty()) kv << "outgrid=" + outgridEdit->text().trimmed();
			saveState();

			showBusyDialog("Building the seamounts…");
			const int ok = g_juliaGrdSeamount(scn, kv.join('\n').toUtf8().constData());
			closeBusyDialog();
			if (!ok)
				QMessageBox::warning(d, "Error", "grdseamount failed — see this window's Errors console for details.");
		});

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}
};

// ============================================================================================
// Clip Grid (Grid Tools) — port of Mirone's src_figs/ml_clip.m. Replace grid nodes below/above the
// Below/Above thresholds with the given Value (or replace the whole [Below,Above] band by one Value
// in "Clip in between" mode), producing a NEW derived grid in the SAME window (SACRED_LAW
// derived-variable display law, handled Julia-side in _on_clipgrid). Loaded at RUNTIME via QUiLoader
// from deps/ui/clipp_grid.ui (only plain Qt widget classes, like Rtp3DDialog above).
//
// "Statistical Hammering" (bottom half): type a % End-members / n·STD / n·MAD value and hit UP to
// auto-derive the Below/Above/Value boxes from the data statistics — a g_juliaEval round-trip to
// InteractiveGMT._clip_stats_str, which prints "low/up" (the actual stats live in Julia, where the
// grid does). The three stat boxes are mutually clearing (editing one blanks the other two), exactly
// like ml_clip.m's edit_*_CB. Only UP and Apply run anything (only-action-button-executes-dialog);
// editing a box never computes.
//
// "New grid" vs "Stretch histogram" (mutually-exclusive radios): New grid clips as above; Stretch
// contrast-clamps both sides to [Below,Above] (Mirone's scaleto8 [below above] data range) — the
// same shared clip path in Julia, just with belowVal/aboveVal forced to Below/Above. "Compute
// Histogram" (Mirone's image_enhance) is not ported, so that button is disabled here.
//
// Clip needs a grid in the window: the menu opens the dialog only when one is loaded; Below/Above
// are prefilled from the scene's own z range (scene->zmin/zmax) on construction.
// ============================================================================================
class ClipGridDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QLineEdit *belowEdit, *belowValEdit, *aboveEdit, *aboveValEdit;
	QLineEdit *pctEdit, *nStdEdit, *nMadEdit;
	QCheckBox *inBetweenChk;
	QRadioButton *stretchRadio;

	explicit ClipGridDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/clipp_grid.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("ClipGridDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("ClipGridDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		dlg->setWindowTitle("Clip Grid");
		QDialog *d = dlg;

		belowEdit    = d->findChild<QLineEdit *>("belowLineEdit");
		belowValEdit = d->findChild<QLineEdit *>("belowValueLineEdit");
		aboveEdit    = d->findChild<QLineEdit *>("aboveLineEdit");
		aboveValEdit = d->findChild<QLineEdit *>("aboveValueLineEdit");
		pctEdit  = d->findChild<QLineEdit *>("percentEndMembersLineEdit");
		nStdEdit = d->findChild<QLineEdit *>("nStdLineEdit");
		nMadEdit = d->findChild<QLineEdit *>("nMadLineEdit");
		inBetweenChk = d->findChild<QCheckBox *>("clipInBetweenCheckBox");
		stretchRadio = d->findChild<QRadioButton *>("stretchHistogramRadioButton");
		auto *applyBtn   = d->findChild<QPushButton *>("applyButton");
		auto *upBtn      = d->findChild<QPushButton *>("upButton");
		auto *histoBtn   = d->findChild<QPushButton *>("computeHistogramButton");

		// Prefill Below/Above with the grid's own z range (ml_clip.m sets edit_below/above to
		// z_min/z_max with a %.4g format). scene->zmin/zmax carry the true (unscaled) range.
		if (scene && scene->zmax > scene->zmin) {
			if (belowEdit) belowEdit->setText(QString::number(scene->zmin, 'g', 4));
			if (aboveEdit) aboveEdit->setText(QString::number(scene->zmax, 'g', 4));
		}

		// "Clip in between": the band [Below,Above] is replaced by the Below Value (used as the
		// in-between value), so the Above Value box is meaningless — grey it out, like ml_clip.m's
		// check_inBetween_CB toggling edit_Ab_val's Enable.
		if (inBetweenChk && aboveValEdit) {
			QObject::connect(inBetweenChk, &QCheckBox::toggled, d, [this](bool on) {
				if (aboveValEdit) aboveValEdit->setEnabled(!on);
			});
		}

		// The three Statistical-Hammering boxes are mutually clearing: typing in one blanks the
		// other two (ml_clip.m edit_percent/nSigma/mad_CB). This is pure local UI — it never runs a
		// compute (that's UP's job) — so textEdited is fine here.
		auto clearOthers = [](QLineEdit *keep, QLineEdit *a, QLineEdit *b) {
			if (a && a != keep) a->clear();
			if (b && b != keep) b->clear();
		};
		if (pctEdit)  QObject::connect(pctEdit,  &QLineEdit::textEdited, d, [=](const QString &) { clearOthers(pctEdit,  nStdEdit, nMadEdit); });
		if (nStdEdit) QObject::connect(nStdEdit, &QLineEdit::textEdited, d, [=](const QString &) { clearOthers(nStdEdit, pctEdit,  nMadEdit); });
		if (nMadEdit) QObject::connect(nMadEdit, &QLineEdit::textEdited, d, [=](const QString &) { clearOthers(nMadEdit, pctEdit,  nStdEdit); });

		// "Compute Histogram" opens Mirone's image_enhance interactive histogram — not ported. Keep
		// the button visible (so the dialog matches the .ui) but disabled with an explaining tooltip.
		if (histoBtn) {
			histoBtn->setEnabled(false);
			histoBtn->setToolTip("Interactive histogram (Mirone image_enhance) is not ported yet.");
		}

		// UP: derive Below/Above (+ their Values) from whichever stat box is filled. The maths needs
		// the grid, which lives in Julia, so round-trip via g_juliaEval to _clip_stats_str; it prints
		// "low/up" (or nothing when no stat box is filled).
		if (upBtn) QObject::connect(upBtn, &QPushButton::clicked, d, [this, d]() {
			if (!g_juliaEval) { QMessageBox::warning(d, "Clip Grid", "This needs the Julia/GMT host."); return; }
			const QString sp = QString("%1;%2;%3")
				.arg(pctEdit  ? pctEdit->text().trimmed()  : QString())
				.arg(nStdEdit ? nStdEdit->text().trimmed() : QString())
				.arg(nMadEdit ? nMadEdit->text().trimmed() : QString());
			const QString cmd = QString("InteractiveGMT._clip_stats_str(Ptr{Cvoid}(UInt(%1)),\"%2\")")
				.arg((qulonglong)reinterpret_cast<uintptr_t>(scn)).arg(sp);
			static std::vector<char> buf(1 << 12);
			int n = g_juliaEval(scn, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
			if (n < 0) { QMessageBox::warning(d, "Clip Grid", QString::fromUtf8(buf.data(), -n)); return; }
			const QString out = QString::fromUtf8(buf.data(), n).trimmed();
			if (out.isEmpty()) return;                 // no stat box filled -> nothing to translate
			const QStringList lu = out.split('/');
			if (lu.size() != 2) return;
			if (belowEdit)    belowEdit->setText(lu[0]);
			if (belowValEdit) belowValEdit->setText(lu[0]);
			if (aboveEdit)    aboveEdit->setText(lu[1]);
			if (aboveValEdit) aboveValEdit->setText(lu[1]);
		});

		// Apply: the ONE action that clips. Validate Below/Above are numeric, then hand every field
		// to _on_clipgrid. The dialog stays open (run more variants without reopening).
		if (applyBtn) QObject::connect(applyBtn, &QPushButton::clicked, d, [this, d]() {
			if (!g_juliaClipGrid) { QMessageBox::warning(d, "Clip Grid", "Clip: callback not registered (rebuild/restart needed?)."); return; }
			bool okB, okA;
			const QString below = belowEdit ? belowEdit->text().trimmed() : QString();
			const QString above = aboveEdit ? aboveEdit->text().trimmed() : QString();
			below.toDouble(&okB); above.toDouble(&okA);
			if (!okB || !okA) { QMessageBox::warning(d, "Clip Grid", "Give me valid numeric Below and Above thresholds."); return; }
			const bool inBetween = inBetweenChk && inBetweenChk->isChecked();
			const bool stretch   = stretchRadio && stretchRadio->isChecked();
			const QString params = QString("%1;%2;%3;%4;%5;%6")
				.arg(below).arg(above)
				.arg(belowValEdit ? belowValEdit->text().trimmed() : QString())
				.arg(aboveValEdit ? aboveValEdit->text().trimmed() : QString())
				.arg(inBetween ? "1" : "0").arg(stretch ? "1" : "0");
			showBusyDialog(stretch ? "Stretching…" : "Clipping grid…");
			const int ok = g_juliaClipGrid(scn, params.toUtf8().constData());
			closeBusyDialog();
			if (ok)
				QMessageBox::information(d, "Clip Grid", stretch
					? "Done — display contrast stretched to [Below, Above]. The grid data is unchanged; "
					  "widen or narrow Below/Above and Apply again to adjust."
					: "Done — added to Scene Objects as a new grid, checked (visible); the source grid is "
					  "now unchecked.");
			else
				QMessageBox::warning(d, "Clip Grid",
					stretch ? "Stretch failed — see this window's Errors console for details."
					        : "Clip failed — see this window's Errors console for details.");
		});

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}
};

// ============================================================================================
// Histograms — port of Mirone's src_figs/image_histo.m ("Image -> Show Histogram").
//
// NO histogram maths here. The counts come from Julia's `GMT.histogray` — the SAME single function
// the Binarize dialog already uses (SACRED_LAW: one quantity, one function). C++ only does what
// only C++ can: hand over the pixels the window is actually SHOWING, and paint the result.
//
// `paintMironeHisto` is image_histo's plot_result + localStem: a coloured patch filling
// [0 x_max]x[0 y_max], black stem lines on top, XLim [0 255], YLim [0 max(counts)]. The Binarize
// dialog's histogram widget paints through the SAME function, so the two tools can never disagree
// about what the histogram of an image looks like.
//
// The pixels histogrammed are the ones the window actually DISPLAYS (Mirone histograms
// `get(hMirHand.hImg,'CData')`, the displayed image, never the underlying grid): for a grid that
// is its rendered colour image — the LUT/shaded colours VTK sends to the GPU — not its z values.
// ============================================================================================

// image_histo.m's plot_result: the axes colour, the patch over it, then the black stems. `plot` is
// the axes box in widget coords; y grows down, so a count maps to plot.bottom() - h.
//
// The patch spans grey levels [pLo,pHi]. image_histo leaves it over the whole range (it is just the
// axes tint); image_enhance shrinks it to the contrast WINDOW — same patch, same painter, one
// parameter apart (SACRED_LAW: the histogram is drawn by one function, never re-drawn per tool).
static void paintMironeHisto(QPainter &g, const QRectF &plot, const double *counts, const QColor &patch,
                             double pLo = 0.0, double pHi = 255.0,
                             const QColor &bg = QColor(230, 255, 255)) {   // axes 'Color',[0.9 1 1]
	double ymax = 0.0;
	for (int k = 0; k < 256; ++k) ymax = std::max(ymax, counts[k]);
	g.fillRect(plot, bg);
	const double xl = plot.left() + (std::clamp(pLo, 0.0, 255.0) / 255.0) * plot.width();
	const double xr = plot.left() + (std::clamp(pHi, 0.0, 255.0) / 255.0) * plot.width();
	g.fillRect(QRectF(xl, plot.top(), std::max(1.0, xr - xl), plot.height()), patch);
	if (ymax <= 0.0) return;
	// ANTIALIASED, and only here. 256 stems share a box ~450 px wide, so consecutive grey levels are
	// ~1.8 px apart: with the aliased rasteriser each stem snaps to whichever pixel COLUMN its
	// fractional x falls in, so some pairs land on neighbouring columns and read as one 2-px bar
	// while the next pair leaves a gap — the stems looked randomly thick and thin. Antialiasing keeps
	// every stem exactly one pen-width wide at its true sub-pixel position, at any window size (the
	// axes now stretch with the dialog). The patch and the axes box stay aliased = crisp.
	const bool wasAA = g.testRenderHint(QPainter::Antialiasing);
	g.setRenderHint(QPainter::Antialiasing, true);
	g.setPen(QPen(Qt::black, 1));                    // localStem: line(...,'color','k')
	for (int k = 0; k < 256; ++k) {
		if (counts[k] <= 0.0) continue;
		const double x = plot.left() + (k / 255.0) * plot.width();
		const double h = (counts[k] / ymax) * plot.height();
		g.drawLine(QPointF(x, plot.bottom()), QPointF(x, plot.bottom() - h));
	}
	g.setRenderHint(QPainter::Antialiasing, wasAA);
}

// One of image_histo's three MATLAB axes: the plot box plus the room its tick labels need. The box
// keeps image_histo_LayoutFcn's 481x120 geometry (left margin 50 for the Y labels, 24 below for the
// X labels when this is the bottom axes — R and G get no XTick there either, same reason).
class HistoAxesWidget : public QWidget {   // no Q_OBJECT (paint/mouse only, like BinHistoWidget)
public:
	double counts[256];
	QColor patch = QColor(120, 255, 114);   // image_histo's default (grey image); RGB uses the band tint
	bool   showXAxis = true;                // Mirone strips XTick on the R and G axes
	// image_enhance's contrast WINDOW: the patch shrinks to [winLo,winHi] and the three red lines
	// (left, dashed centre, right) become draggable. Off for image_histo, which only shows counts.
	bool   interactive = false;
	double winLo = 0.0, winHi = 255.0;
	std::function<void()> onMoved;          // dragging (live: edit boxes + the N statistic)
	std::function<void()> onReleased;       // drag finished
	std::function<void()> onPressed;        // this axes became the current band (wbd_strayClick)

	explicit HistoAxesWidget(QWidget *p) : QWidget(p) {
		for (double &c : counts) c = 0.0;
	}
	void setCounts(const double *c) {
		for (int k = 0; k < 256; ++k) counts[k] = c ? c[k] : 0.0;
		update();
	}
	void setWindow(double lo, double hi) {
		winLo = std::clamp(lo, 0.0, 255.0);
		winHi = std::clamp(hi, winLo, 255.0);
		update();
	}
	// Pixels inside the window, and their share of the whole band — image_enhance's text_stat.
	void windowStat(double &nIn, double &nTot) const {
		nIn = 0.0; nTot = 0.0;
		for (int k = 0; k < 256; ++k) {
			nTot += counts[k];
			if (k >= winLo && k <= winHi) nIn += counts[k];
		}
	}
	QRectF plotRect() const {
		const double bottomRoom = showXAxis ? 24.0 : 6.0;
		return QRectF(50.0, 5.0, std::max(10.0, width() - 55.0),
		              std::max(10.0, height() - 5.0 - bottomRoom));
	}
	double v2x(double v) const {
		const QRectF b = plotRect();
		return b.left() + (std::clamp(v, 0.0, 255.0) / 255.0) * b.width();
	}
	double x2v(double x) const {
		const QRectF b = plotRect();
		return std::clamp((x - b.left()) / std::max(1.0, b.width()) * 255.0, 0.0, 255.0);
	}

protected:
	int drag = 0;                           // 0 none, 1 left line, 2 the centre (slides both), 3 right

	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		g.fillRect(rect(), palette().window());
		const QRectF box = plotRect();
		paintMironeHisto(g, box, counts, patch, interactive ? winLo : 0.0, interactive ? winHi : 255.0);
		if (interactive) {                  // the three red lines of plot_result
			const double xl = v2x(winLo), xr = v2x(winHi), xc = v2x(0.5 * (winLo + winHi));
			g.setPen(QPen(QColor(255, 0, 0), 1));
			g.drawLine(QPointF(xl, box.top()), QPointF(xl, box.bottom()));
			g.drawLine(QPointF(xr, box.top()), QPointF(xr, box.bottom()));
			g.setPen(QPen(QColor(255, 0, 0), 2, Qt::DashLine));
			g.drawLine(QPointF(xc, box.top()), QPointF(xc, box.bottom()));
		}
		double ymax = 0.0;
		for (double c : counts) ymax = std::max(ymax, c);
		g.setPen(QPen(Qt::black, 1));
		g.drawRect(box);
		// Y ticks: MATLAB's automatic 0 .. y_max labelling, to the left of the box.
		QFont f = g.font(); f.setPointSizeF(7.5); g.setFont(f);
		for (int t = 0; t <= 2; ++t) {
			const double frac = t / 2.0, y = box.bottom() - frac * box.height();
			g.drawLine(QPointF(box.left() - 4.0, y), QPointF(box.left(), y));
			g.drawText(QRectF(0.0, y - 7.0, 46.0, 14.0), Qt::AlignRight | Qt::AlignVCenter,
			           QString::number(qRound(frac * ymax)));
		}
		if (!showXAxis) return;
		for (int t = 0; t <= 4; ++t) {                // grey levels 0 .. 255
			const int v = (t * 255) / 4;
			const double x = box.left() + (v / 255.0) * box.width();
			g.drawLine(QPointF(x, box.bottom()), QPointF(x, box.bottom() + 4.0));
			g.drawText(QRectF(x - 25.0, box.bottom() + 5.0, 50.0, 14.0), Qt::AlignCenter,
			           QString::number(v));
		}
	}

	// image_enhance's wbm_vertLine/wbd_vertLine/drag_vertLine/wbu_vertLine: grab whichever of the
	// three lines is within a few pixels, drag it, and let the dialog follow. Clicking anywhere in
	// the axes also makes this band the current one, exactly like wbd_strayClick.
	void mousePressEvent(QMouseEvent *e) override {
		if (onPressed) onPressed();
		if (!interactive) return;
		const double x = e->position().x();
		const double dl = std::abs(x - v2x(winLo)), dr = std::abs(x - v2x(winHi));
		const double dc = std::abs(x - v2x(0.5 * (winLo + winHi)));
		const double d  = std::min(dc, std::min(dl, dr));
		if (d > 8.0) return;
		drag = (d == dl) ? 1 : (d == dc) ? 2 : 3;
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (!drag) return;
		const double v = x2v(e->position().x());
		if (drag == 1) winLo = std::min(v, winHi - 1.0);
		else if (drag == 3) winHi = std::max(v, winLo + 1.0);
		else {                                        // the centre line slides the whole window
			const double half = 0.5 * (winHi - winLo);
			winLo = std::clamp(v - half, 0.0, 255.0 - 2.0 * half);
			winHi = winLo + 2.0 * half;
		}
		update();
		if (onMoved) onMoved();
	}
	void mouseReleaseEvent(QMouseEvent *) override {
		if (!drag) return;
		drag = 0;
		if (onReleased) onReleased();
	}
	void enterEvent(QEnterEvent *) override {
		if (interactive) setCursor(Qt::SizeAllCursor);   // every drag in iGMT uses SizeAll
	}
};

// The bytes the window actually PUTS ON SCREEN for `act`, and how many components each pixel has.
// Two display paths, both covered: a textured actor (a dropped image, a drape, the flat-2-D image)
// shows its texture verbatim; a coloured mesh (a grid surface — live CPT scalars OR the RGBA baked
// by hillshadeMapper) shows what its mapper maps, which is exactly what vtkMapper::MapScalars
// returns. No re-derivation of the colours here — that would be a second colour engine.
static bool actorDisplayedRGB(vtkActor *act, std::vector<unsigned char> &out, int &nb) {
	out.clear(); nb = 0;
	if (!act) return false;
	if (vtkTexture *tx = act->GetTexture()) {
		if (vtkImageData *id = tx->GetInput()) {
			if (auto *sc = vtkUnsignedCharArray::SafeDownCast(id->GetPointData()->GetScalars())) {
				nb = sc->GetNumberOfComponents();
				const unsigned char *p = sc->GetPointer(0);
				out.assign(p, p + static_cast<size_t>(sc->GetNumberOfTuples()) * nb);
				return !out.empty();
			}
		}
	}
	if (vtkMapper *m = act->GetMapper()) {
		vtkUnsignedCharArray *c = m->MapScalars(1.0);   // owned by the mapper — do NOT take/delete
		if (c && c->GetNumberOfTuples() > 0) {
			nb = c->GetNumberOfComponents();
			const unsigned char *p = c->GetPointer(0);
			out.assign(p, p + static_cast<size_t>(c->GetNumberOfTuples()) * nb);
			return true;
		}
	}
	return false;
}

// Resolve "the image on display" for this window (optionally the Scene Objects element `name`) and
// hand back its pixels. Order mirrors what the user sees: a named element first, then the primary
// bare image / flat-2-D drape, then the grid surface, then the first visible image extra.
static bool sceneDisplayedRGB(Scene *s, const char *name, std::vector<unsigned char> &out, int &nb,
                              QString &label) {
	if (!s) return false;
	if (name && *name) {
		for (auto &ex : s->extras) {
			if (ex.name != name) continue;
			label = QString::fromStdString(ex.name);
			if (actorDisplayedRGB(ex.drape, out, nb)) return true;
			return actorDisplayedRGB(ex.actor, out, nb);
		}
	}
	// Image EXTRAS come FIRST, newest last-added first. Every derived image in iGMT — a contrast or
	// decorrelation stretch, a crop, a mask — is added as an extra ON TOP of the source, so the most
	// recently added visible one is what the user is looking at. Reading s->drape (the window's
	// ORIGINAL image) before them meant a derived result was ignored whenever the original had not
	// been hidden, which is exactly how a ScaterPlot taken after a Decorrelation Stretch kept coming
	// back as the original.
	for (size_t k = s->extras.size(); k-- > 0; ) {
		auto &ex = s->extras[k];
		if (!ex.isImage) continue;
		vtkActor *a = ex.drape ? ex.drape.Get() : ex.actor.Get();
		if (!a || !a->GetVisibility()) continue;
		if (actorDisplayedRGB(a, out, nb)) { label = QString::fromStdString(ex.name); return true; }
	}
	if (s->drape && s->drape->GetVisibility() && actorDisplayedRGB(s->drape, out, nb)) {
		label = s->imageOnly ? (s->surfName.empty() ? QString("image") : QString::fromStdString(s->surfName))
		                     : QString("surface");
		return true;
	}
	if (s->surf && s->surf->GetVisibility() && actorDisplayedRGB(s->surf, out, nb)) {
		label = "surface";
		return true;
	}
	for (size_t k = s->extras.size(); k-- > 0; ) {     // anything else still visible (grid / mesh)
		auto &ex = s->extras[k];
		vtkActor *a = ex.drape ? ex.drape.Get() : ex.actor.Get();
		if (!a || !a->GetVisibility()) continue;
		if (actorDisplayedRGB(a, out, nb)) { label = QString::fromStdString(ex.name); return true; }
	}
	return false;
}

// The Scene Objects NAME of the image the window is DISPLAYING — the newest visible image extra,
// or "" for the window's own primary image. Every Image-menu tool opens on this instead of letting
// Julia fall back to "the first image of that kind", which is a guess: after a stretch there are
// several images in the window, and after deleting one there may be none where the guess points.
static QString displayedImageName(Scene *s) {
	if (!s) return QString();
	for (size_t k = s->extras.size(); k-- > 0; ) {
		auto &ex = s->extras[k];
		if (!ex.isImage) continue;
		vtkActor *a = ex.drape ? ex.drape.Get() : ex.actor.Get();
		if (a && a->GetVisibility()) return QString::fromStdString(ex.name);
	}
	return QString();
}

class HistoUiLoader : public QUiLoader {
public:
	QWidget *createWidget(const QString &className, QWidget *parent = nullptr,
	                       const QString &name = QString()) override {
		if (className == "HistoAxesWidget") {
			auto *w = new HistoAxesWidget(parent);
			w->setObjectName(name);
			return w;
		}
		return QUiLoader::createWidget(className, parent, name);
	}
};

// The live "Image histogram" window. Julia pushes one band's 256 counts at a time
// (gmtvtk_histo_set_counts), so the dialog never computes anything itself.
class ImageHistoDialog {
public:
	QDialog *dlg = nullptr;
	HistoAxesWidget *ax[3] = { nullptr, nullptr, nullptr };

	void setCounts(int band, const double *c, int n) {
		if (band < 0 || band > 2 || n < 256) return;
		if (ax[band]) ax[band]->setCounts(c);
	}
};

// "Image -> Show Histogram". RGB display -> three stacked axes (R, G, B, top to bottom, tinted like
// image_histo's plot_result). Single-band display -> image_histo's reshaped figure: axes2/axes3 gone
// and the dialog shrunk to the one remaining plot (new_height = 400 - 275 + 40 = 165).
static void showImageHistogram(QWidget *parent, Scene *s, const char *name) {
	if (!g_juliaImageHisto) {
		QMessageBox::warning(parent, "Image histogram",
		                     "Image histogram: callback not registered (rebuild/restart needed?).");
		return;
	}
	std::vector<unsigned char> px;
	int nb = 0;
	QString label;
	if (!sceneDisplayedRGB(s, name, px, nb, label) || nb < 1) {
		QMessageBox::warning(parent, "Image histogram", "This window has no displayed image to histogram.");
		return;
	}
	const size_t npix = px.size() / static_cast<size_t>(nb);
	const bool isRGB = (nb >= 3);

	HistoUiLoader loader;
	QFile f(gmtvtkUiDir() + "/image_histo.ui");
	if (!f.open(QFile::ReadOnly)) {
		qWarning("showImageHistogram: cannot open %s", qUtf8Printable(f.fileName()));
		return;
	}
	QDialog *d = qobject_cast<QDialog *>(loader.load(&f, parent));
	f.close();
	if (!d) return;
	d->setAttribute(Qt::WA_DeleteOnClose);
	d->setWindowTitle(label.isEmpty() ? QString("Image histogram")
	                                  : QString("Image histogram — %1").arg(label));

	auto *ih = new ImageHistoDialog;
	ih->dlg = d;
	// findChild<HistoAxesWidget*> is not possible (no Q_OBJECT — see BinHistoWidget): look the widget
	// up as a QWidget and cast, exactly like the Binarize dialog does for its histogram widget.
	ih->ax[0] = static_cast<HistoAxesWidget *>(d->findChild<QWidget *>("axes1"));
	ih->ax[1] = static_cast<HistoAxesWidget *>(d->findChild<QWidget *>("axes2"));
	ih->ax[2] = static_cast<HistoAxesWidget *>(d->findChild<QWidget *>("axes3"));
	if (!ih->ax[0] || !ih->ax[1] || !ih->ax[2]) { delete ih; delete d; return; }
	QObject::connect(d, &QObject::destroyed, d, [ih]() { delete ih; });

	if (isRGB) {
		const QColor tint[3] = { QColor(255, 124, 117), QColor(120, 255, 114), QColor(119, 119, 255) };
		for (int k = 0; k < 3; ++k) {
			ih->ax[k]->patch = tint[k];
			ih->ax[k]->showXAxis = (k == 2);  // only the bottom (blue) axes keeps its XTicks
		}
	}
	else {
		// The three axes sit in the dialog's QVBoxLayout, so hiding two makes the third take the whole
		// height on its own — no setGeometry (which the layout would overrule on the next resize).
		ih->ax[1]->hide(); ih->ax[2]->hide();
		ih->ax[0]->showXAxis = true;
		d->resize(d->width(), 165);
	}
	// Julia counts the bands (GMT.histogray) and pushes them straight back into ih->ax[].
	g_juliaImageHisto(s, ih, px.data(), static_cast<int>(npix), nb);
	d->show();
}

// ============================================================================================
// Image Enhance -> 1 - Indexed and RGB ("Adjust Contrast") — port of Mirone's
// src_figs/image_enhance.m. deps/ui/image_enhance.ui carries image_enhance_LayoutFcn's absolute
// geometry (536x540, MATLAB's bottom-up y converted to Qt's top-down).
//
// Per band: the histogram (the SAME HistoAxesWidget "Image -> Show Histogram" draws, here with its
// contrast WINDOW switched on) plus the Minimum/Maximum/Width/Center boxes, all four kept in step
// with the three draggable red lines. The window is display state and lives here; every pixel
// operation — the outlier limits, the contrast stretch, the decorrelation stretch — is Julia's
// (src/imageenhance.jl), so nothing is computed twice.
// ============================================================================================
class ImageEnhanceDialog;
static std::map<Scene *, ImageEnhanceDialog *> g_enhanceDlgs;

class ImageEnhanceDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	HistoAxesWidget *ax[3] = { nullptr, nullptr, nullptr };
	QLineEdit *eMinRange = nullptr, *eMaxRange = nullptr;
	QLineEdit *eMin = nullptr, *eMax = nullptr, *eWidth = nullptr, *eCenter = nullptr;
	QLineEdit *ePct[3] = { nullptr, nullptr, nullptr };
	QRadioButton *rDelOutliers = nullptr, *rBand[3] = { nullptr, nullptr, nullptr };
	QLabel *stat = nullptr;
	QString srcName;
	int  nbands = 1;                        // 1 (indexed/grey) or 3 (RGB)
	int  cur = 0;                           // handles.currAxes - 1
	double dmin[3] = { 0, 0, 0 }, dmax[3] = { 255, 255, 255 };   // handles.minCData / maxCData
	bool ready = false, filling = false;
	bool reallyClose = false;               // set by the parked row's "Delete": let the next close through

	// Closing PARKS this dialog as a row in the window's Scene Objects dock — the SAME
	// Scene::parkedTools list, parkTool/unparkTool pair and row builder a closed X,Y plot,
	// Contours or Illumination dialog uses (SACRED_LAW: one operation, one function). `unpark` is
	// the one way back in, whatever asked for it (double-click, the row's checkbox, its "Show").
	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();
		dlg->raise();
		dlg->activateWindow();
	}
	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel  = m.addAction("Delete");
			QAction *pick  = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scn, dlg);
				dlg->deleteLater();          // destroyed -> the row and this object go with it
			}
		};
	}
	// By VALUE, not by `scn`: when the viewer window is torn down the Scene may already be gone.
	~ImageEnhanceDialog() {
		for (auto it = g_enhanceDlgs.begin(); it != g_enhanceDlgs.end(); )
			it = (it->second == this) ? g_enhanceDlgs.erase(it) : std::next(it);
	}

	bool send(const QString &params) {
		if (!g_juliaImageEnhance) {
			QMessageBox::warning(dlg, "Image Enhance",
			                     "Image Enhance: callback not registered (rebuild/restart needed?).");
			return false;
		}
		return g_juliaImageEnhance(scn, this, params.toUtf8().constData()) != 0;
	}
	// Julia hands one band back at "init": its 256 counts and its own data range.
	void setBand(int band, const double *counts, int n, double lo, double hi, int nb) {
		if (band < 0 || band > 2 || n < 256) return;
		nbands = nb;
		dmin[band] = lo;  dmax[band] = hi;
		if (ax[band]) { ax[band]->setCounts(counts); ax[band]->setWindow(lo, hi); }
		if (band == cur) syncBoxes();
	}
	// Julia answers a "stretchlim" with the limits it found for that band.
	void setWindow(int band, double lo, double hi) {
		if (band < 0 || band > 2 || !ax[band]) return;
		ax[band]->setWindow(lo, hi);
		if (band == cur) syncBoxes();
	}
	// The four Window boxes + the N statistic, from whatever the current axes' window now is
	// (plot_result's edit-box fill and updateAll's text_stat, one place for both).
	void syncBoxes() {
		HistoAxesWidget *a = ax[cur];
		if (!a) return;
		filling = true;
		if (eMinRange) eMinRange->setText(QString::number(dmin[cur], 'g', 6));
		if (eMaxRange) eMaxRange->setText(QString::number(dmax[cur], 'g', 6));
		if (eMin)    eMin->setText(QString::number(a->winLo, 'f', 0));
		if (eMax)    eMax->setText(QString::number(a->winHi, 'f', 0));
		if (eWidth)  eWidth->setText(QString::number(a->winHi - a->winLo, 'f', 0));
		if (eCenter) eCenter->setText(QString::number(0.5 * (a->winLo + a->winHi), 'f', 0));
		filling = false;
		double nIn = 0.0, nTot = 0.0;
		a->windowStat(nIn, nTot);
		if (stat)
			stat->setText(QString("N = %1\t(%2%)").arg(qRound(nIn))
			              .arg(nTot > 0.0 ? nIn / nTot * 100.0 : 0.0, 0, 'f', 2));
	}
	// Window <- the boxes. `what` is which box was edited, so Width/Center keep their own meaning
	// (edit_widthWindow_CB / edit_centerWindow_CB grow the window about its centre, or move it).
	void windowFromBoxes(int what) {
		if (filling || !ax[cur]) return;
		HistoAxesWidget *a = ax[cur];
		double lo = a->winLo, hi = a->winHi;
		if (what == 0)      lo = eMin->text().toDouble();
		else if (what == 1) hi = eMax->text().toDouble();
		else if (what == 2) {                                     // width, about the current centre
			const double c = 0.5 * (lo + hi), w = eWidth->text().toDouble();
			lo = c - w / 2.0;  hi = c + w / 2.0;
		}
		else {                                                    // centre, keeping the width
			const double w = hi - lo, c = eCenter->text().toDouble();
			lo = c - w / 2.0;  hi = c + w / 2.0;
		}
		if (hi <= lo) return;
		a->setWindow(lo, hi);
		syncBoxes();
	}
	double pct(int band) const {
		const double v = ePct[band] ? ePct[band]->text().toDouble() : 2.0;
		return (v < 0.0 || v > 100.0) ? 2.0 : v;
	}

	explicit ImageEnhanceDialog(QWidget *parent, Scene *scene, const QString &imgName = QString())
	    : scn(scene), srcName(imgName) {
		HistoUiLoader loader;
		QFile f(gmtvtkUiDir() + "/image_enhance.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("ImageEnhanceDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) return;
		QDialog *d = dlg;

		ax[0] = static_cast<HistoAxesWidget *>(d->findChild<QWidget *>("axes1"));
		ax[1] = static_cast<HistoAxesWidget *>(d->findChild<QWidget *>("axes2"));
		ax[2] = static_cast<HistoAxesWidget *>(d->findChild<QWidget *>("axes3"));
		if (!ax[0] || !ax[1] || !ax[2]) { delete d; dlg = nullptr; return; }

		// The X button PARKS instead of closing. WA_DeleteOnClose is deliberately NOT set, and the
		// close event is swallowed outright — only the parked row's "Delete" (which sets reallyClose)
		// ever lets one through, so the dialog and its per-band windows survive being closed.
		struct CloseParks : QObject {
			ImageEnhanceDialog *ed;
			CloseParks(QObject *parent, ImageEnhanceDialog *e) : QObject(parent), ed(e) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close && ed && !ed->reallyClose && sceneAlive(ed->scn)) {
					e->ignore();
					ed->dlg->hide();
					ImageEnhanceDialog *h = ed;   // a lambda cannot capture a member of the enclosing class
					parkTool(h->scn, h->dlg, "Adjust Contrast", IC_Image,
					         "Closed Adjust Contrast dialog — double-click to bring it back, click for Show / Delete",
					         [h]() { h->unpark(); }, h->parkedMenu());
					unfoldSceneObjects(ed->scn);   // a handle nobody can see is no handle at all
					return true;
				}
				return QObject::eventFilter(o, e);
			}
		};
		d->installEventFilter(new CloseParks(d, this));

		eMinRange = d->findChild<QLineEdit *>("edit_minRange");
		eMaxRange = d->findChild<QLineEdit *>("edit_maxRange");
		eMin      = d->findChild<QLineEdit *>("edit_minWindow");
		eMax      = d->findChild<QLineEdit *>("edit_maxWindow");
		eWidth    = d->findChild<QLineEdit *>("edit_widthWindow");
		eCenter   = d->findChild<QLineEdit *>("edit_centerWindow");
		ePct[0]   = d->findChild<QLineEdit *>("edit_percentOutliersR");
		ePct[1]   = d->findChild<QLineEdit *>("edit_percentOutliersG");
		ePct[2]   = d->findChild<QLineEdit *>("edit_percentOutliersB");
		rDelOutliers = d->findChild<QRadioButton *>("radio_delOutliers");
		rBand[0]  = d->findChild<QRadioButton *>("radio_R");
		rBand[1]  = d->findChild<QRadioButton *>("radio_G");
		rBand[2]  = d->findChild<QRadioButton *>("radio_B");
		stat      = d->findChild<QLabel *>("text_stat");
		// The pressed button MUST stay marked so the user can see what was pushed. A focus ring is NOT
		// enough: the Windows style barely paints one, and any op that adds a scene element pulls the
		// activation away and takes the ring with it. So the four action buttons are CHECKABLE and
		// mutually exclusive — the last one pressed stays visibly down, whatever the focus does.
		// (Same device as the Binarize dialog's five method buttons.)
		auto *actionGroup = new QButtonGroup(d);
		actionGroup->setExclusive(true);
		for (QPushButton *b : d->findChildren<QPushButton *>()) {
			b->setAutoDefault(false);  b->setDefault(false);  b->setFocusPolicy(Qt::StrongFocus);
			b->setCheckable(true);
			actionGroup->addButton(b);
		}

		const QColor tint[3] = { QColor(255, 124, 117), QColor(120, 255, 114), QColor(119, 119, 255) };
		for (int k = 0; k < 3; ++k) {
			ax[k]->interactive = true;
			ax[k]->patch = tint[k];
			ax[k]->showXAxis = (k == 2);
			ax[k]->onPressed  = [this, k]() { setBandCurrent(k); };
			ax[k]->onMoved    = [this]()    { syncBoxes(); };
			ax[k]->onReleased = [this]()    { syncBoxes(); };
		}
		// R/G/B radios and a click in an axes are the same act: pick the current band (swap_radios).
		for (int k = 0; k < 3; ++k)
			if (rBand[k]) QObject::connect(rBand[k], &QRadioButton::clicked, d, [this, k]() { setBandCurrent(k); });

		QObject::connect(eMin, &QLineEdit::editingFinished, d, [this]() { windowFromBoxes(0); });
		QObject::connect(eMax, &QLineEdit::editingFinished, d, [this]() { windowFromBoxes(1); });
		QObject::connect(eWidth, &QLineEdit::editingFinished, d, [this]() { windowFromBoxes(2); });
		QObject::connect(eCenter, &QLineEdit::editingFinished, d, [this]() { windowFromBoxes(3); });

		// Apply (push_applyRange_CB): either the band's own data range, or the limits that clip the
		// requested percentage of outliers — the latter is Julia's localStretchlim, on the real pixels.
		if (auto *b = d->findChild<QPushButton *>("push_applyRange"))
			QObject::connect(b, &QPushButton::clicked, d, [this]() {
				if (rDelOutliers && rDelOutliers->isChecked())
					send(QString("stretchlim;%1;%2").arg(cur).arg(pct(cur)));
				else {
					ax[cur]->setWindow(dmin[cur], dmax[cur]);
					syncBoxes();
				}
			});
		// Contrast Stretch (push_contStrectch_CB): imadjust with each band's window, [0 1] out.
		if (auto *b = d->findChild<QPushButton *>("push_contStrectch"))
			QObject::connect(b, &QPushButton::clicked, d, [this]() {
				QString p = "contrast";
				for (int k = 0; k < 3; ++k) {
					const int j = (k < nbands) ? k : 0;
					p += QString(";%1;%2").arg(ax[j]->winLo).arg(ax[j]->winHi);
				}
				send(p);
			});
		if (auto *b = d->findChild<QPushButton *>("push_decorrStrectch"))
			QObject::connect(b, &QPushButton::clicked, d, [this]() {
				send(QString("decorr;%1").arg(pct(0)));   // the R box is the tolerance, as in Mirone
			});
		// ScaterPlot reads the pixels that are ON SCREEN RIGHT NOW (sceneDisplayedRGB — the same
		// reader Show Histogram uses), never anything this dialog remembered: after a Decorrelation
		// Stretch the displayed image IS the decorrelated one, so that is what gets plotted, with no
		// second copy of "which image is current" to fall out of step.
		if (auto *b = d->findChild<QPushButton *>("push_scaterPlot"))
			QObject::connect(b, &QPushButton::clicked, d, [this]() {
				if (!g_juliaRgbScatter) {
					QMessageBox::warning(dlg, "ScaterPlot",
					                     "ScaterPlot: callback not registered (rebuild/restart needed?).");
					return;
				}
				std::vector<unsigned char> px;
				int nb = 0;
				QString label;
				if (!sceneDisplayedRGB(scn, "", px, nb, label) || nb < 3) {
					QMessageBox::warning(dlg, "ScaterPlot", "ScaterPlot needs an RGB image on display.");
					return;
				}
				g_juliaRgbScatter(scn, px.data(), static_cast<int>(px.size() / nb), nb,
				                  label.toUtf8().constData());
			});

		// Connected LAST on purpose: Qt runs slots in connection order, so this fires AFTER the op
		// above has run (and after any scene rebuild it triggered stole the activation) and puts the
		// mark back on the button that was actually pressed.
		for (QPushButton *b : d->findChildren<QPushButton *>())
			QObject::connect(b, &QPushButton::clicked, d, [this, b]() {
				if (dlg) dlg->activateWindow();
				b->setFocus(Qt::TabFocusReason);
			});

		if (!send("init;" + srcName)) {
			QMessageBox::warning(parent, "Image Enhance",
			                     "Image Enhance failed — see this window's Errors console for details.");
			delete d; dlg = nullptr; return;
		}
		// A single-band display is image_enhance's reshaped figure: no G/B anything, and the window
		// shrinks to the one remaining plot (new_height = 540 - 291 + 40 = 289).
		if (nbands < 3) {
			for (const char *n : { "axes2", "axes3", "radio_R", "radio_G", "radio_B",
			                       "edit_percentOutliersG", "edit_percentOutliersB",
			                       "push_scaterPlot", "push_decorrStrectch" })
				if (auto *w = d->findChild<QWidget *>(n)) w->hide();
			ax[0]->showXAxis = true;
			ax[0]->setGeometry(0, 129, 486, 160);
			if (auto *w = d->findChild<QWidget *>("frame_axes")) w->setGeometry(5, 115, 526, 157);
			if (auto *w = d->findChild<QWidget *>("text_tip"))   w->setGeometry(6, 267, 250, 15);
			if (auto *w = d->findChild<QWidget *>("text_stat"))  w->setGeometry(380, 267, 150, 15);
			d->resize(d->width(), 289);
		}
		syncBoxes();
		ready = true;
		g_enhanceDlgs[scn] = this;
		// Only a REAL destruction gets here (the parked row's "Delete", or the viewer window dying) —
		// the X is swallowed by CloseParks above. The destructor drops the registry entry.
		QObject::connect(d, &QObject::destroyed, d, [this]() {
			if (g_juliaImageEnhance) g_juliaImageEnhance(scn, this, "close");   // drop Julia's state
			delete this;
		});
	}

	// swap_radios: the current band drives the boxes, the statistic and every Apply/stretch.
	void setBandCurrent(int k) {
		if (k < 0 || k >= nbands) return;
		cur = k;
		if (rBand[k]) rBand[k]->setChecked(true);
		syncBoxes();
	}
};

// ============================================================================================
// Binarize Image (Image menu) — port of Mirone's src_figs/thresholdit.m. Turn this window's image
// into a black-and-white mask by thresholding its grey levels, then clean the mask up (dust,
// holes, connected-component labels) and either add it as a new element or use it to mask the
// source image. Loaded at RUNTIME via QUiLoader from deps/ui/binarize.ui, whose geometry is the
// literal port of thresholdit_LayoutFcn's absolute positions (680x499, MATLAB's bottom-up y
// converted to Qt's top-down).
//
// ALL the maths lives in Julia (src/binarize.jl): the grey image, the histogram, every threshold
// method, the current mask and its one-level undo. This dialog only paints what Julia pushes back
// (gmtvtk_binarize_set_histogram / gmtvtk_binarize_set_preview) and sends "op;args" through
// g_juliaBinarize — so the preview the user judges and the mask "Good, I like it" commits are the
// SAME mask from the SAME function (SACRED_LAW), never a C++-side re-threshold for speed.
//
// The histogram widget below is Mirone's axes2: a stem plot of the 256 grey-level counts with
// either ONE draggable grey line (single-line mode) or a draggable/resizable red [lo,hi] box
// (window mode) — its three handles being the left edge, the centre (moves the whole box) and the
// right edge, exactly like thresholdit.m's hVertLines(1..3). Like there, a drag repaints locally
// and only the RELEASE re-binarizes (a per-pixel re-threshold on every mouse move would crawl on a
// big image).
// ============================================================================================
// One live Binarize dialog per window: closing it only hides it, so the Image menu must re-show the
// existing one (mask + undo intact) rather than build a second, empty one. Same bookkeeping as the
// LIDAR picker's g_lidarDlgs.
class BinarizeDialog;
static std::map<Scene *, BinarizeDialog *> g_binarizeDlgs;

class BinHistoWidget : public QWidget {   // no Q_OBJECT (only paint/mouse overrides, like BaseMapArea)
public:
	double counts[256];                   // grey-level histogram, as Julia's histogray gives it
	bool   haveHisto = false;
	bool   windowMode = false;            // false: single line (level);  true: [lo,hi] band
	double level = 128.0;                 // single-line threshold, grey level [0 255]
	double lo = 64.0, hi = 192.0;         // window-mode band, grey levels [0 255]
	std::function<void()> onReleased;     // a drag finished -> the dialog asks Julia to re-binarize

	explicit BinHistoWidget(QWidget *p) : QWidget(p) {
		for (double &c : counts) c = 0.0;
		setMinimumSize(200, 50);
		setCursor(Qt::SizeAllCursor);     // every drag in iGMT uses SizeAll, never a cross
	}
	void setHistogram(const double *c, int n) {
		for (int k = 0; k < 256; ++k) counts[k] = (c && k < n) ? c[k] : 0.0;
		haveHisto = true;
		update();
	}
	// Grey level <-> widget x. The plot area is the widget minus a 4 px side margin and the bottom
	// strip that carries the level text (Mirone's `hText` under the axes).
	double plotLeft()  const { return 4.0; }
	double plotRight() const { return width() - 4.0; }
	double plotBottom() const { return height() - 14.0; }
	double v2x(double v) const { return plotLeft() + (v / 255.0) * (plotRight() - plotLeft()); }
	double x2v(double x) const {
		const double t = (x - plotLeft()) / std::max(1.0, plotRight() - plotLeft());
		return std::clamp(t * 255.0, 0.0, 255.0);
	}

protected:
	int drag = 0;                         // 0 none, 1 the single line, 2 lo, 3 the whole box, 4 hi

	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		g.fillRect(rect(), palette().window());
		const double y0 = plotBottom();
		// The SAME histogram painter "Image -> Show Histogram" uses (image_histo's plot_result +
		// localStem: green patch, black stems, YLim [0 max]) — one look, one function.
		if (haveHisto)
			paintMironeHisto(g, QRectF(plotLeft(), 2.0, plotRight() - plotLeft(), y0 - 2.0),
			                 counts, QColor(120, 255, 114));
		g.setPen(QPen(Qt::black, 1));
		g.drawLine(QPointF(plotLeft(), y0), QPointF(plotRight(), y0));
		if (!haveHisto) return;
		if (windowMode) {                                  // the box + its three red handles
			const double xl = v2x(lo), xr = v2x(hi), xc = v2x(0.5 * (lo + hi));
			g.fillRect(QRectF(xl, 0.0, xr - xl, y0), QColor(255, 80, 80, 40));
			g.setPen(QPen(QColor(220, 40, 40), 2));
			g.drawLine(QPointF(xl, 0.0), QPointF(xl, y0));
			g.drawLine(QPointF(xr, 0.0), QPointF(xr, y0));
			g.setPen(QPen(QColor(220, 40, 40), 2, Qt::DashLine));
			g.drawLine(QPointF(xc, 0.0), QPointF(xc, y0));
			g.setPen(palette().windowText().color());
			g.drawText(QRectF(xl - 40.0, y0, 80.0, 14.0), Qt::AlignCenter, QString::number(qRound(lo)));
			g.drawText(QRectF(xr - 40.0, y0, 80.0, 14.0), Qt::AlignCenter, QString::number(qRound(hi)));
		}
		else {                                             // the single grey line + its level text
			const double x = v2x(level);
			g.setPen(QPen(QColor(128, 128, 128), 2));
			g.drawLine(QPointF(x, 0.0), QPointF(x, y0));
			QFont f = g.font(); f.setBold(true); g.setFont(f);
			g.setPen(QColor(102, 102, 102));
			g.drawText(QRectF(x - 40.0, y0, 80.0, 14.0), Qt::AlignCenter, QString::number(qRound(level)));
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (!haveHisto) return;
		const double x = e->position().x();
		if (windowMode) {
			const double dl = std::abs(x - v2x(lo)), dr = std::abs(x - v2x(hi));
			const double dc = std::abs(x - v2x(0.5 * (lo + hi)));
			const double d  = std::min(dc, std::min(dl, dr));
			if (d > 8.0) return;                          // clicked away from every handle
			drag = (d == dl) ? 2 : (d == dc) ? 3 : 4;
		}
		else {
			if (std::abs(x - v2x(level)) > 8.0) return;
			drag = 1;
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (!drag) return;
		const double v = x2v(e->position().x());
		if (drag == 1) level = v;
		else if (drag == 2) lo = std::min(v, hi - 1.0);
		else if (drag == 4) hi = std::max(v, lo + 1.0);
		else {                                            // the centre handle slides the whole box
			const double half = 0.5 * (hi - lo);
			lo = std::clamp(v - half, 0.0, 255.0 - 2.0 * half);
			hi = lo + 2.0 * half;
		}
		update();
	}
	void mouseReleaseEvent(QMouseEvent *) override {
		if (!drag) return;
		drag = 0;
		if (onReleased) onReleased();                     // NOW re-binarize (Mirone's VLUpFcn/BoxUpFcn)
	}
};

// QUiLoader that knows the one custom class binarize.ui references; everything else in that .ui is
// a plain Qt class QUiLoader already builds on its own (same technique as IgrfUiLoader).
class BinarizeUiLoader : public QUiLoader {
public:
	QWidget *createWidget(const QString &className, QWidget *parent = nullptr,
	                       const QString &name = QString()) override {
		if (className == "BinHistoWidget") {
			auto *w = new BinHistoWidget(parent);
			w->setObjectName(name);
			return w;
		}
		return QUiLoader::createWidget(className, parent, name);
	}
};

class BinarizeDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	BinHistoWidget *histo = nullptr;
	QLabel *preview = nullptr;
	QCheckBox *revertChk = nullptr, *applyOrigChk = nullptr, *useAlphaChk = nullptr;
	QLineEdit *dustEdit = nullptr, *labelEdit = nullptr;
	QRadioButton *windowRadio = nullptr;
	QImage mask;                                          // what Julia last pushed back (grey 0/255)
	QString srcName;                                      // image to threshold ("" = window primary)
	bool ready = false;                                   // init succeeded -> the caller may show it
	bool reallyClose = false;                             // the parked row's "Delete" lets a close through

	// The one way back from the parked row (double-click, its checkbox, its "Show"), and the row's
	// own menu — both the SAME pair every other parked tool uses.
	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();
		dlg->raise();
		dlg->activateWindow();
	}
	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel  = m.addAction("Delete");
			QAction *pick  = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scn, dlg);
				dlg->deleteLater();
			}
		};
	}

	// Julia pushes the grey-level histogram here once, when the dialog opens (op "init").
	void setHistogram(const double *c, int n) {
		if (histo) histo->setHistogram(c, n);
	}
	// Julia pushes the current mask here after EVERY op. `data` is w*h bytes, row-major, top row
	// first, 0 = black / 255 = white. `level` (or lo/hi) < 0 means "leave the handles where they
	// are" — a method button DOES move them, a dust/fill/undo does not.
	void setPreview(int w, int h, const unsigned char *data, double level, double lo, double hi) {
		if (w > 0 && h > 0 && data) {
			mask = QImage(data, w, h, w, QImage::Format_Grayscale8).copy();
			if (preview) preview->setPixmap(QPixmap::fromImage(mask).scaled(preview->size(),
			                                Qt::KeepAspectRatio, Qt::SmoothTransformation));
		}
		if (histo) {
			if (level >= 0.0) histo->level = level;
			if (lo >= 0.0 && hi >= 0.0) { histo->lo = lo; histo->hi = hi; }
			histo->update();
		}
	}
	// One door to Julia for every op (SACRED_LAW: one operation, one path). Reports a missing
	// callback rather than silently doing nothing.
	bool send(const QString &params) {
		if (!g_juliaBinarize) {
			QMessageBox::warning(dlg, "Binarize", "Binarize: callback not registered (rebuild/restart needed?).");
			return false;
		}
		return g_juliaBinarize(scn, this, params.toUtf8().constData()) != 0;
	}
	QString revertArg() const { return (revertChk && revertChk->isChecked()) ? "1" : "0"; }

	// `imgName` = Scene Objects name of the image to threshold; empty means the window's primary
	// image. It goes to Julia with the "init" op, so the dialog always works on the image the user
	// pointed at (a window can hold several).
	explicit BinarizeDialog(QWidget *parent, Scene *scene, const QString &imgName = QString())
	    : scn(scene), srcName(imgName) {
		BinarizeUiLoader loader;
		QFile f(gmtvtkUiDir() + "/binarize.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("BinarizeDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("BinarizeDialog: QUiLoader failed to load the .ui"); return; }
		// NO WA_DeleteOnClose, and the X does not even close: it PARKS the dialog as a row in the
		// window's Scene Objects dock — the SAME Scene::parkedTools list, parkTool/unparkTool pair and
		// row builder a closed X,Y plot / Contours / Illumination dialog uses (SACRED_LAW: one
		// operation, one function). The mask, its undo copy and the histogram handles survive; only
		// the parked row's "Delete" destroys the dialog, and THAT is when Julia's state is dropped.
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;

		struct CloseParks : QObject {
			BinarizeDialog *bd;
			CloseParks(QObject *parent, BinarizeDialog *b) : QObject(parent), bd(b) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close && bd && !bd->reallyClose && sceneAlive(bd->scn)) {
					e->ignore();
					bd->dlg->hide();
					BinarizeDialog *h = bd;
					parkTool(h->scn, h->dlg, "Binarize Image", IC_Image,
					         "Closed Binarize dialog — double-click to bring it back, click for Show / Delete",
					         [h]() { h->unpark(); }, h->parkedMenu());
					unfoldSceneObjects(bd->scn);
					return true;
				}
				return QObject::eventFilter(o, e);
			}
		};
		d->installEventFilter(new CloseParks(d, this));

		// BinHistoWidget has no Q_OBJECT -> static_cast (BinarizeUiLoader is the only thing that
		// ever builds one), same as IgrfDialog does with IgrfMapArea.
		histo   = static_cast<BinHistoWidget *>(d->findChild<QWidget *>("histoWidget"));
		preview = d->findChild<QLabel *>("previewLabel");
		revertChk    = d->findChild<QCheckBox *>("revertCheck");
		applyOrigChk = d->findChild<QCheckBox *>("applyOrigCheck");
		useAlphaChk  = d->findChild<QCheckBox *>("useAlphaCheck");
		dustEdit  = d->findChild<QLineEdit *>("dustSizeEdit");
		labelEdit = d->findChild<QLineEdit *>("labelEdit");
		windowRadio = d->findChild<QRadioButton *>("windowRadio");
		auto *singleRadio = d->findChild<QRadioButton *>("singleLineRadio");
		auto *undoBtn = d->findChild<QPushButton *>("undoButton");
		if (undoBtn) undoBtn->setIcon(d->style()->standardIcon(QStyle::SP_ArrowBack));

		// THE highlight must stay on the button the user last pressed. In a QDialog every QPushButton
		// is autoDefault by default, and Qt keeps handing the "default" frame back to the FIRST such
		// button (Otsu) after every click — which is what reads as "the focus jumped back to Otsu".
		// setFocus() alone cannot win that, the default-button machinery re-paints over it. So: no
		// button is autoDefault/default here, and each click parks a real, VISIBLE focus on itself.
		for (QPushButton *b : d->findChildren<QPushButton *>()) {
			b->setAutoDefault(false);
			b->setDefault(false);
			b->setFocusPolicy(Qt::StrongFocus);
		}

		// The five automatic threshold methods (thresholdit.m's push_*_CB): Julia computes the
		// level AND the mask, and pushes both back — the histogram line follows the method.
		auto method = [this](QPushButton *b, const char *name) {
			return [this, b, name]() { send(QString("method;%1;%2").arg(name).arg(revertArg())); };
		};
		// The five method buttons are CHECKABLE and mutually exclusive: the one last pressed stays
		// visibly down, so which method produced the mask on screen is readable at a glance and
		// cannot be undone by anything that happens to move the keyboard focus (a raised console
		// tab, the parent window taking activation, …). Focus alone was not enough.
		auto *methodGroup = new QButtonGroup(d);
		methodGroup->setExclusive(true);
		for (const char *n : { "otsuButton", "maxEntropyButton", "minCrossEntropyButton",
		                        "isodataButton", "triangleButton" })
			if (auto *b = d->findChild<QPushButton *>(n)) { b->setCheckable(true); methodGroup->addButton(b); }

		if (auto *b = d->findChild<QPushButton *>("otsuButton"))            QObject::connect(b, &QPushButton::clicked, d, method(b, "otsu"));
		if (auto *b = d->findChild<QPushButton *>("maxEntropyButton"))      QObject::connect(b, &QPushButton::clicked, d, method(b, "maxent"));
		if (auto *b = d->findChild<QPushButton *>("minCrossEntropyButton")) QObject::connect(b, &QPushButton::clicked, d, method(b, "mince"));
		if (auto *b = d->findChild<QPushButton *>("isodataButton"))         QObject::connect(b, &QPushButton::clicked, d, method(b, "isodata"));
		if (auto *b = d->findChild<QPushButton *>("triangleButton"))        QObject::connect(b, &QPushButton::clicked, d, method(b, "triangle"));

		// Single line <-> Window: only which handles the histogram shows/answers to (radio_*_CB).
		// Switching does NOT re-binarize — dragging (or a method button) does.
		if (singleRadio) QObject::connect(singleRadio, &QRadioButton::toggled, d, [this](bool on) {
			if (histo && on) { histo->windowMode = false; histo->update(); }
		});
		if (windowRadio) QObject::connect(windowRadio, &QRadioButton::toggled, d, [this](bool on) {
			if (histo && on) { histo->windowMode = true; histo->update(); }
		});

		// Drag release -> re-binarize with the handle(s) the user just moved.
		if (histo) histo->onReleased = [this]() {
			if (!histo) return;
			if (histo->windowMode)
				send(QString("window;%1;%2;%3").arg(histo->lo, 0, 'f', 2).arg(histo->hi, 0, 'f', 2).arg(revertArg()));
			else
				send(QString("level;%1;%2").arg(histo->level, 0, 'f', 2).arg(revertArg()));
		};

		// Revert: complement the CURRENT mask, whatever produced it (check_revert_CB) — not a
		// re-threshold, so a cleaned/filled mask keeps its cleaning.
		if (revertChk) QObject::connect(revertChk, &QCheckBox::toggled, d, [this](bool on) {
			send(QString("revert;%1").arg(on ? "1" : "0"));
		});

		// Mask clean-up: dust (bwareaopen + a 2 px hailing distance), holes, labels, undo.
		if (auto *b = d->findChild<QPushButton *>("cleanDustButton")) QObject::connect(b, &QPushButton::clicked, d, [this]() {
			send(QString("dust;%1").arg(dustEdit ? dustEdit->text().trimmed() : QString("15")));
		});
		if (auto *b = d->findChild<QPushButton *>("fillHolesButton")) QObject::connect(b, &QPushButton::clicked, d, [this]() {
			send("fill");
		});
		if (auto *b = d->findChild<QPushButton *>("labelMatrixButton")) QObject::connect(b, &QPushButton::clicked, d, [this]() {
			send(QString("label;%1").arg(labelEdit ? labelEdit->text().trimmed() : QString("0")));
		});
		if (undoBtn) QObject::connect(undoBtn, &QPushButton::clicked, d, [this]() { send("undo"); });

		// Idiot-proofing of the two numeric boxes, exactly like edit_dustSize_CB / edit_label_CB.
		if (dustEdit) QObject::connect(dustEdit, &QLineEdit::editingFinished, d, [this]() {
			bool ok; const double v = dustEdit->text().trimmed().toDouble(&ok);
			if (!ok || v < 0.0) dustEdit->setText("15");
		});
		if (labelEdit) QObject::connect(labelEdit, &QLineEdit::editingFinished, d, [this]() {
			bool ok; const double v = labelEdit->text().trimmed().toDouble(&ok);
			if (!ok || v < 0.0) labelEdit->setText("0");
		});

		// "Good, I like it" — the ONE button that commits (only-action-button-executes-dialog).
		if (auto *b = d->findChild<QPushButton *>("okButton")) QObject::connect(b, &QPushButton::clicked, d, [this]() {
			const bool applyOrig = applyOrigChk && applyOrigChk->isChecked();
			const bool useAlpha  = useAlphaChk  && useAlphaChk->isChecked();
			showBusyDialog("Binarizing…");
			const bool ok = send(QString("ok;%1;%2").arg(applyOrig ? "1" : "0").arg(useAlpha ? "1" : "0"));
			closeBusyDialog();
			if (ok)
				QMessageBox::information(dlg, "Binarize", applyOrig
					? "Done — the source image is now masked."
					: "Done — the mask was added to Scene Objects, checked (visible); the source image is now unchecked.");
			else
				QMessageBox::warning(dlg, "Binarize", "Binarize failed — see this window's Errors console for details.");
		});

		// Every button parks the focus on ITSELF after it ran — connected LAST, so it fires after the
		// button's own op handler (a QMessageBox in "Good, I like it" would otherwise leave the focus
		// wherever the box left it). TabFocusReason, not OtherFocusReason: the Windows style paints
		// the focus ring only for keyboard-style reasons, so "Other" would move focus invisibly and
		// still look like the highlight sat on Otsu.
		for (QPushButton *b : d->findChildren<QPushButton *>())
			QObject::connect(b, &QPushButton::clicked, d, [this, b]() {
				if (dlg) dlg->activateWindow();     // take activation back from whatever the op raised
				b->setFocus(Qt::TabFocusReason);
			});

		// Ask Julia for the image: it greys it, histograms it and pushes back the first (Otsu) mask —
		// thresholdit.m's own opening move. No busy dialog: the image is ALREADY in memory, this only
		// walks its pixels once.
		if (!send("init;" + srcName)) {
			QMessageBox::warning(parent, "Binarize", "No image to binarize in this window (or the read failed).");
			delete d;  dlg = nullptr;
			return;
		}
		ready = true;
		g_binarizeDlgs[scn] = this;      // the Image menu re-shows THIS one instead of building another

		QObject::connect(d, &QObject::destroyed, d, [this]() {
			for (auto it = g_binarizeDlgs.begin(); it != g_binarizeDlgs.end(); )
				it = (it->second == this) ? g_binarizeDlgs.erase(it) : std::next(it);
			if (g_juliaBinarize) g_juliaBinarize(scn, this, "close");   // drop Julia's mask/undo state
			delete this;
		});
	}
};

// Hooks imageObjectMenu (50_scene.cpp, compiled earlier in this TU) uses to offer "Binarize Image…"
// on an image's Scene Objects handle: that entry is where a closed (= hidden, never destroyed)
// Binarize dialog comes back from, mask and undo intact. Installed at load, before any menu can pop.
static bool binarizeHasDialog(Scene *scene) { return g_binarizeDlgs.count(scene) != 0; }
static void binarizeReopen(Scene *scene, const char *name) {
	auto it = g_binarizeDlgs.find(scene);
	if (it != g_binarizeDlgs.end() && it->second->dlg) {
		it->second->unpark();   // parked? the row goes away and the SAME dialog comes back
		return;
	}
	auto *w = new BinarizeDialog(scene ? scene->win : nullptr, scene, QString::fromUtf8(name ? name : ""));
	if (w->dlg && w->ready) w->dlg->show();
	else delete w;
}
// Same for "Adjust Contrast" — its X button hides it, and this is how it comes back.
static void enhanceReopen(Scene *scene, const char *name) {
	auto it = g_enhanceDlgs.find(scene);
	if (it != g_enhanceDlgs.end() && it->second->dlg) {
		it->second->unpark();   // parked? the row goes away and the SAME dialog comes back
		return;
	}
	auto *w = new ImageEnhanceDialog(scene ? scene->win : nullptr, scene, QString::fromUtf8(name ? name : ""));
	if (w->dlg && w->ready) w->dlg->show();
	else delete w;
}
// ============================================================================================
// Image > Image resize — port of Mirone's src_figs/imageresize.m. deps/ui/image_resize.ui carries
// imageresize_LayoutFcn's absolute geometry (328x261).
//
// Everything here is the Photoshop-style bookkeeping of the .m: pixel W/H (in pixels or percent),
// the "document" size that the same pixels make at a given resolution, and Constrain Proportions
// tying the two edits together. NOT ONE PIXEL is resampled here — OK hands (w, h, method) to Julia,
// which calls gdalwarp (src/imageresize.jl). Mirone resamples with cvlib_mex('resize') = OpenCV;
// the four popup entries map onto gdalwarp's -r: nearest/bilinear/bicubic as named, and "Area
// Relation" (OpenCV INTER_AREA, "pixel area relation") onto `average`, which is the same
// definition — each output pixel is the mean of the source pixels its footprint covers.
// ============================================================================================
class ImageResizeDialog;
static std::map<Scene *, ImageResizeDialog *> g_resizeDlgs;

class ImageResizeDialog {
public:
	QDialog *dlg = nullptr;
	Scene   *scn = nullptr;
	bool reallyClose = false;               // set by the parked row's "Delete": let the next close through
	QLineEdit *ePixW = nullptr, *ePixH = nullptr, *eDocW = nullptr, *eDocH = nullptr, *eRes = nullptr;
	QComboBox *cPixW = nullptr, *cPixH = nullptr, *cDocW = nullptr, *cDocH = nullptr, *cRes = nullptr;
	QComboBox *cMethod = nullptr;
	QCheckBox *kConstrain = nullptr;
	QString srcName;
	// handles.* of the .m, same names
	double imgW = 0, imgH = 0;                    // handles.imgSize(2), handles.imgSize(1)
	double pixWidth = 0, pixHeight = 0;
	double docWidth = 0, docHeight = 0;
	double resolution = 72.0, unitFact = 2.54;
	bool   isPercent = false, ready = false, filling = false;

	double resolutionFact() const { return unitFact / resolution; }   // handles.resolutionFact

	// Closing PARKS this dialog as a row in Scene Objects — the same parkTool/unparkTool pair the
	// Adjust Contrast, Binarize, X,Y plot, Contours and Illumination dialogs use. One mechanism.
	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();
		dlg->raise();
		dlg->activateWindow();
	}
	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel  = m.addAction("Delete");
			QAction *pick  = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scn, dlg);
				dlg->deleteLater();          // destroyed -> the row and this object go with it
			}
		};
	}
	~ImageResizeDialog() {
		for (auto it = g_resizeDlgs.begin(); it != g_resizeDlgs.end(); )
			it = (it->second == this) ? g_resizeDlgs.erase(it) : std::next(it);
	}

	// Julia answers "init" with the SOURCE image's size (the displayed texture is padded to the
	// window bbox by _drape_to_bbox, so its dimensions are not the image's).
	void setSize(int w, int h) {
		imgW = pixWidth  = w;
		imgH = pixHeight = h;
		docWidth  = pixWidth  * resolutionFact();
		docHeight = pixHeight * resolutionFact();
		ready = true;
		refresh();
	}

	// pix2size + size2pix of the .m, plus the percent display of popup_pixWidth_CB.
	void refresh() {
		filling = true;
		const double w = isPercent ? pixWidth  / std::max(1.0, imgW) * 100.0 : pixWidth;
		const double h = isPercent ? pixHeight / std::max(1.0, imgH) * 100.0 : pixHeight;
		ePixW->setText(QString::number(qRound(w)));
		ePixH->setText(QString::number(qRound(h)));
		eDocW->setText(QString::number(docWidth, 'g', 6));
		eDocH->setText(QString::number(docHeight, 'g', 6));
		eRes->setText(QString::number(cRes->currentIndex() == 0 ? resolution : resolution / 2.54, 'g', 6));
		filling = false;
	}

	// edit_pixWidth_CB / edit_pixHeight_CB. `which` = 0 width, 1 height.
	void pixEdited(int which) {
		if (filling || !ready) return;
		QLineEdit *e = which == 0 ? ePixW : ePixH;
		bool ok = false;
		double xx = qRound(e->text().trimmed().toDouble(&ok));
		if (!ok) { refresh(); return; }
		const double p = xx / 100.0;
		const double base = which == 0 ? imgW : imgH, other = which == 0 ? imgH : imgW;
		if (isPercent) xx = base * p;
		if (xx < 1.0) { refresh(); return; }
		double &me = which == 0 ? pixWidth : pixHeight;
		double &you = which == 0 ? pixHeight : pixWidth;
		const double meOld = me;
		me = xx;
		if (kConstrain->isChecked())                       // handles.constrainProp
			you = isPercent ? other * p : you * me / std::max(1.0, meOld);
		docWidth  = pixWidth  * resolutionFact();
		docHeight = pixHeight * resolutionFact();
		refresh();
	}

	// edit_docWidth_CB / edit_docHeight_CB: the document size drives the pixel count back.
	void docEdited(int which) {
		if (filling || !ready) return;
		QLineEdit *e = which == 0 ? eDocW : eDocH;
		bool ok = false;
		const double xx = e->text().trimmed().toDouble(&ok);
		if (!ok || xx <= 0.0) { refresh(); return; }
		double &me = which == 0 ? docWidth : docHeight;
		double &you = which == 0 ? docHeight : docWidth;
		const double meOld = me;
		me = xx;
		if (kConstrain->isChecked()) you = you * me / std::max(1e-12, meOld);
		pixWidth  = std::max(1.0, docWidth  / resolutionFact());
		pixHeight = std::max(1.0, docHeight / resolutionFact());
		refresh();
	}

	// edit_docResolution_CB: stored in DPI whatever the popup says, then the pixel counts follow.
	void resEdited() {
		if (filling || !ready) return;
		bool ok = false;
		const double xx = eRes->text().trimmed().toDouble(&ok);
		if (!ok || xx <= 0.0) { refresh(); return; }
		resolution = cRes->currentIndex() == 0 ? qRound(xx) : qRound(xx * 2.54);
		pixWidth  = std::max(1.0, docWidth  / resolutionFact());
		pixHeight = std::max(1.0, docHeight / resolutionFact());
		refresh();
	}
};

// The four popup_resampMethod entries, as gdalwarp -r names (see the class comment for "Area Relation").
static const char *resizeMethodName(int idx) {
	switch (idx) {
		case 1:  return "bilinear";
		case 2:  return "cubic";
		case 3:  return "average";     // Mirone/OpenCV "Area Relation" = INTER_AREA
		default: return "near";
	}
}

static void showImageResize(QWidget *parent, Scene *s, const char *name) {
	if (!g_juliaImageResize) {
		QMessageBox::warning(parent, "Resize Image",
		                     "Image resize: callback not registered (rebuild/restart needed?).");
		return;
	}
	auto open = g_resizeDlgs.find(s);            // already open (or parked)? that one comes back
	if (open != g_resizeDlgs.end() && open->second->dlg) { open->second->unpark(); return; }
	QUiLoader loader;
	QFile f(gmtvtkUiDir() + "/image_resize.ui");
	if (!f.open(QFile::ReadOnly)) {
		qWarning("showImageResize: cannot open %s", qUtf8Printable(f.fileName()));
		return;
	}
	QDialog *d = qobject_cast<QDialog *>(loader.load(&f, parent));
	f.close();
	if (!d) return;
	// NO WA_DeleteOnClose: closing parks the dialog (see CloseParks below), it does not destroy it.

	auto *rd = new ImageResizeDialog;
	rd->dlg = d;
	rd->scn = s;
	rd->srcName = QString::fromUtf8(name ? name : "");
	rd->ePixW = d->findChild<QLineEdit *>("edit_pixWidth");
	rd->ePixH = d->findChild<QLineEdit *>("edit_pixHeight");
	rd->eDocW = d->findChild<QLineEdit *>("edit_docWidth");
	rd->eDocH = d->findChild<QLineEdit *>("edit_docHeight");
	rd->eRes  = d->findChild<QLineEdit *>("edit_docResolution");
	rd->cPixW = d->findChild<QComboBox *>("popup_pixWidth");
	rd->cPixH = d->findChild<QComboBox *>("popup_pixHeight");
	rd->cDocW = d->findChild<QComboBox *>("popup_docWidth");
	rd->cDocH = d->findChild<QComboBox *>("popup_docHeight");
	rd->cRes  = d->findChild<QComboBox *>("popup_docResolution");
	rd->cMethod    = d->findChild<QComboBox *>("popup_resampMethod");
	rd->kConstrain = d->findChild<QCheckBox *>("check_constProportions");
	QPushButton *bOK = d->findChild<QPushButton *>("push_OK");
	QPushButton *bCancel = d->findChild<QPushButton *>("push_cancel");
	// push_cancel is OPTIONAL — the dialog's own X closes it just as well, and demanding it here made
	// the whole dialog silently refuse to open the moment the button was taken out of the .ui.
	if (!rd->ePixW || !rd->ePixH || !rd->eDocW || !rd->eDocH || !rd->eRes || !rd->cPixW ||
	    !rd->cPixH || !rd->cDocW || !rd->cDocH || !rd->cRes || !rd->cMethod || !rd->kConstrain ||
	    !bOK) {
		delete rd; delete d; return;
	}
	QObject::connect(d, &QObject::destroyed, d, [rd]() { delete rd; });
	g_resizeDlgs[s] = rd;

	// The X PARKS instead of closing — same filter the Adjust Contrast dialog installs. Only the
	// parked row's "Delete" (which sets reallyClose) ever lets a close event through.
	struct CloseParks : QObject {
		ImageResizeDialog *rd;
		CloseParks(QObject *parent, ImageResizeDialog *r) : QObject(parent), rd(r) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::Close && rd && !rd->reallyClose && sceneAlive(rd->scn)) {
				e->ignore();
				rd->dlg->hide();
				ImageResizeDialog *h = rd;
				parkTool(h->scn, h->dlg, "Resize Image", IC_Image,
				         "Closed Resize Image dialog — double-click to bring it back, click for Show / Delete",
				         [h]() { h->unpark(); }, h->parkedMenu());
				unfoldSceneObjects(rd->scn);
				return true;
			}
			return QObject::eventFilter(o, e);
		}
	};
	d->installEventFilter(new CloseParks(d, rd));

	QObject::connect(rd->ePixW, &QLineEdit::editingFinished, d, [rd]() { rd->pixEdited(0); });
	QObject::connect(rd->ePixH, &QLineEdit::editingFinished, d, [rd]() { rd->pixEdited(1); });
	QObject::connect(rd->eDocW, &QLineEdit::editingFinished, d, [rd]() { rd->docEdited(0); });
	QObject::connect(rd->eDocH, &QLineEdit::editingFinished, d, [rd]() { rd->docEdited(1); });
	QObject::connect(rd->eRes,  &QLineEdit::editingFinished, d, [rd]() { rd->resEdited(); });
	// popup_pixHeight_CB just forwards to popup_pixWidth_CB: the two popups are one control.
	auto pixUnits = [rd](int v) {
		if (rd->filling) return;
		rd->isPercent = (v == 1);
		rd->filling = true;
		rd->cPixW->setCurrentIndex(v);  rd->cPixH->setCurrentIndex(v);
		rd->filling = false;
		rd->refresh();
	};
	QObject::connect(rd->cPixW, QOverload<int>::of(&QComboBox::currentIndexChanged), d, pixUnits);
	QObject::connect(rd->cPixH, QOverload<int>::of(&QComboBox::currentIndexChanged), d, pixUnits);
	// popup_docWidth_CB: cm / mm / inch / points, likewise one control for both edits.
	auto docUnits = [rd](int v) {
		if (rd->filling) return;
		rd->unitFact = (v == 1) ? 254.0 : (v == 2) ? 1.0 : (v == 3) ? 1.0 / 72.0 : 2.54;
		rd->filling = true;
		rd->cDocW->setCurrentIndex(v);  rd->cDocH->setCurrentIndex(v);
		rd->filling = false;
		rd->docWidth  = rd->pixWidth  * rd->resolutionFact();
		rd->docHeight = rd->pixHeight * rd->resolutionFact();
		rd->refresh();
	};
	QObject::connect(rd->cDocW, QOverload<int>::of(&QComboBox::currentIndexChanged), d, docUnits);
	QObject::connect(rd->cDocH, QOverload<int>::of(&QComboBox::currentIndexChanged), d, docUnits);
	// popup_docResolution_CB: display only — the resolution itself never leaves DPI.
	QObject::connect(rd->cRes, QOverload<int>::of(&QComboBox::currentIndexChanged), d,
	                 [rd](int) { if (!rd->filling) rd->refresh(); });

	if (bCancel) QObject::connect(bCancel, &QPushButton::clicked, d, [d]() { d->close(); });
	QObject::connect(bOK, &QPushButton::clicked, d, [rd, d, s]() {
		if (!rd->ready || !g_juliaImageResize) return;
		const int w = std::max(1, qRound(rd->pixWidth)), h = std::max(1, qRound(rd->pixHeight));
		const QString req = QString("resize;%1;%2;%3;%4").arg(rd->srcName).arg(w).arg(h)
		                        .arg(QString::fromLatin1(resizeMethodName(rd->cMethod->currentIndex())));
		QApplication::setOverrideCursor(Qt::WaitCursor);
		g_juliaImageResize(s, rd, req.toUtf8().constData());
		QApplication::restoreOverrideCursor();
		d->close();
	});

	// Ask Julia for the source image's real size; it answers through gmtvtk_resize_set_size.
	const QString init = QString("init;%1").arg(rd->srcName);
	if (!g_juliaImageResize(s, rd, init.toUtf8().constData()) || !rd->ready) {
		QMessageBox::warning(parent, "Resize Image", "This window has no image to resize.");
		delete d;          // never opened -> destroy it outright; parking an empty dialog is no use
		return;
	}
	d->setWindowTitle(rd->srcName.isEmpty() ? QString("Resize Image")
	                                        : QString("Resize Image — %1").arg(rd->srcName));
	d->show();
}

// ============================================================================================
// Image > Shape detector — port of the "Digit / Segment" half of Mirone's src_figs/floodfill.m
// ("color segmentation or painting like the magick wand"). deps/ui/floodfill.ui carries
// floodfill_LayoutFcn's geometry for the controls that came across; the "Paint" half (pencil,
// paintbrush, colour palette, shapes) is not ported — it paints on the displayed image in place.
//
// This class holds only the settings and the seed picking. Every pixel — the fixed-range flood
// growth, the dilation, the Mahalanobis class, the polygons — is Julia's (src/floodfill.jl).
// ============================================================================================
class FloodFillDialog;
static std::map<Scene *, FloodFillDialog *> g_floodDlgs;

class FloodFillDialog {
public:
	QDialog *dlg = nullptr;
	Scene   *scn = nullptr;
	QSlider *slider = nullptr;
	QLabel  *tolLab = nullptr;
	QCheckBox *kDilate = nullptr, *kMahal = nullptr;
	QRadioButton *rConn4 = nullptr, *rSeg = nullptr, *rMask = nullptr, *rDigit = nullptr;
	QPushButton *bSingle = nullptr, *bMulti = nullptr;
	QLineEdit *eMinPts = nullptr;
	QLabel *lMinPts = nullptr;
	QString srcName;
	std::vector<std::pair<double, double>> seeds;    // world x,y collected while a pick is armed
	bool reallyClose = false;

	int  tol() const { return slider ? slider->value() : 20; }
	int  conn() const { return (rConn4 && rConn4->isChecked()) ? 4 : 8; }
	int  mode() const { return (rDigit && rDigit->isChecked()) ? 0 : (rMask && rMask->isChecked()) ? 2 : 1; }
	int  minPts() const { bool ok = false; int v = eMinPts ? eMinPts->text().toInt(&ok) : 50; return ok ? v : 50; }

	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();  dlg->raise();  dlg->activateWindow();
	}
	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel = m.addAction("Delete");
			QAction *pick = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) { reallyClose = true; unparkTool(scn, dlg); dlg->deleteLater(); }
		};
	}
	~FloodFillDialog() {
		disarm();
		for (auto it = g_floodDlgs.begin(); it != g_floodDlgs.end(); )
			it = (it->second == this) ? g_floodDlgs.erase(it) : std::next(it);
	}

	void disarm() {
		if (!sceneAlive(scn)) return;
		if (scn->vectorPickMode == 3) scn->vectorPickMode = 0;
		scn->seedPickCB = nullptr;
		scn->seedPickEndCB = nullptr;
		if (scn->widget) scn->widget->unsetCursor();
	}

	// "That's all of them": a right-click or a double-click in the view, or the button pressed again.
	// All three land HERE so the three can never behave differently — the button pops back up, the
	// pick disarms, and whatever was collected is sent off.
	void finishMulti() {
		disarm();
		setDown(bMulti, false);
		send("multi");
	}

	// Both buttons arm the SAME point pick; they differ only in how many seeds they collect before
	// the work is sent off — push_pickSingle fires on the first click, push_pickMultiple keeps
	// collecting until its button is pressed again (Mirone's `while (but == 1)` loop).
	void arm(bool multi, bool on) {
		if (!sceneAlive(scn)) return;
		if (!on) { disarm(); return; }
		seeds.clear();
		QPointer<QDialog> guard(dlg);
		scn->seedPickCB = [this, guard, multi](double x, double y) {
			if (!guard) return;
			seeds.emplace_back(x, y);
			if (!multi) { send("seed"); setDown(bSingle, false); }
			else if (scn && scn->win)
				scn->win->statusBar()->showMessage(
					QString("%1 shape(s) picked — right-click or double-click when done")
					    .arg(seeds.size()), 4000);
		};
		if (multi)
			scn->seedPickEndCB = [this, guard]() { if (guard) finishMulti(); };
		scn->vectorPickMode = 3;
		scn->vectorPickDbl = false;
		if (scn->widget) scn->widget->setCursor(Qt::CrossCursor);
		if (scn->win) scn->win->statusBar()->showMessage(
			multi ? "Click every shape whose colour you want — then RIGHT-CLICK or double-click to finish"
			      : "Click the shape you want", 6000);
	}

	// Keeps a toggle button and the scene's armed state from ever disagreeing.
	void setDown(QPushButton *b, bool on) {
		if (!b) return;
		QSignalBlocker blk(b);
		b->setChecked(on);
		if (!on) disarm();
	}

	void send(const char *op) {
		if (!g_juliaFloodFill || seeds.empty()) return;
		QString req = QString("%1;%2;%3;%4;%5;%6;%7;%8;%9")
		                  .arg(QString::fromLatin1(op)).arg(srcName).arg(tol()).arg(conn())
		                  .arg(kDilate && kDilate->isChecked() ? 1 : 0)
		                  .arg(kMahal && kMahal->isChecked() ? 1 : 0)
		                  .arg(mode()).arg(minPts()).arg(0);      // bg = floodfill.m's handles.bg_color
		for (const auto &s : seeds)
			req += QString(";%1,%2").arg(s.first, 0, 'g', 17).arg(s.second, 0, 'g', 17);
		QApplication::setOverrideCursor(Qt::WaitCursor);
		g_juliaFloodFill(scn, req.toUtf8().constData());
		QApplication::restoreOverrideCursor();
		seeds.clear();
	}
};

static void showFloodFill(QWidget *parent, Scene *s, const char *name) {
	if (!g_juliaFloodFill) {
		QMessageBox::warning(parent, "Shape detector",
		                     "Shape detector: callback not registered (rebuild/restart needed?).");
		return;
	}
	auto open = g_floodDlgs.find(s);
	if (open != g_floodDlgs.end() && open->second->dlg) { open->second->unpark(); return; }
	QUiLoader loader;
	QFile f(gmtvtkUiDir() + "/floodfill.ui");
	if (!f.open(QFile::ReadOnly)) {
		qWarning("showFloodFill: cannot open %s", qUtf8Printable(f.fileName()));
		return;
	}
	QDialog *d = qobject_cast<QDialog *>(loader.load(&f, parent));
	f.close();
	if (!d) return;

	auto *fd = new FloodFillDialog;
	fd->dlg = d;  fd->scn = s;
	fd->srcName = QString::fromUtf8(name ? name : "");
	fd->slider  = d->findChild<QSlider *>("slider_tolerance");
	fd->tolLab  = d->findChild<QLabel *>("text_tol");
	fd->kDilate = d->findChild<QCheckBox *>("checkbox_useDilation");
	fd->kMahal  = d->findChild<QCheckBox *>("check_mahal");
	fd->rConn4  = d->findChild<QRadioButton *>("radio_fourConn");
	fd->rSeg    = d->findChild<QRadioButton *>("radio_colorSegment");
	fd->rMask   = d->findChild<QRadioButton *>("radio_mask");
	fd->rDigit  = d->findChild<QRadioButton *>("radio_digitize");
	fd->bSingle = d->findChild<QPushButton *>("push_pickSingle");
	fd->bMulti  = d->findChild<QPushButton *>("push_pickMultiple");
	fd->eMinPts = d->findChild<QLineEdit *>("edit_minPts");
	fd->lMinPts = d->findChild<QLabel *>("text_minPts");
	if (!fd->slider || !fd->rConn4 || !fd->bSingle || !fd->bMulti) { delete fd; delete d; return; }
	QObject::connect(d, &QObject::destroyed, d, [fd]() { delete fd; });
	g_floodDlgs[s] = fd;

	struct CloseParks : QObject {
		FloodFillDialog *fd;
		CloseParks(QObject *parent, FloodFillDialog *f) : QObject(parent), fd(f) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::Close && fd && !fd->reallyClose && sceneAlive(fd->scn)) {
				e->ignore();
				fd->disarm();                    // a parked tool must not keep the view in pick mode
				fd->dlg->hide();
				FloodFillDialog *h = fd;
				parkTool(h->scn, h->dlg, "Shape detector", IC_Image,
				         "Closed Shape detector — double-click to bring it back, click for Show / Delete",
				         [h]() { h->unpark(); }, h->parkedMenu());
				unfoldSceneObjects(fd->scn);
				return true;
			}
			return QObject::eventFilter(o, e);
		}
	};
	d->installEventFilter(new CloseParks(d, fd));

	// slider_tolerance_CB: the label reads back the value, as in the .m
	QObject::connect(fd->slider, &QSlider::valueChanged, d, [fd](int v) {
		if (fd->tolLab) fd->tolLab->setText(QString("Tolerance = %1").arg(v));
	});
	// radio_digitize_CB / radio_colorSegment_CB / radio_mask_CB: Min pts shows only for Digitize.
	auto minPtsVis = [fd]() {
		const bool on = fd->rDigit && fd->rDigit->isChecked();
		if (fd->eMinPts) fd->eMinPts->setVisible(on);
		if (fd->lMinPts) fd->lMinPts->setVisible(on);
	};
	for (QRadioButton *r : { fd->rSeg, fd->rMask, fd->rDigit })
		if (r) QObject::connect(r, &QRadioButton::toggled, d, [minPtsVis](bool) { minPtsVis(); });
	minPtsVis();
	// check_mahal_CB: Mahalanobis is a single-class estimator, so Mirone greys "Pick multiple" out.
	if (fd->kMahal)
		QObject::connect(fd->kMahal, &QCheckBox::toggled, d, [fd](bool on) {
			if (fd->bMulti) fd->bMulti->setEnabled(!on);
		});

	QObject::connect(fd->bSingle, &QPushButton::toggled, d, [fd](bool on) {
		if (on) fd->setDown(fd->bMulti, false);
		fd->arm(false, on);
	});
	QObject::connect(fd->bMulti, &QPushButton::toggled, d, [fd](bool on) {
		if (on) { fd->setDown(fd->bSingle, false); fd->arm(true, true); }
		else fd->finishMulti();                       // pressed again = "done picking", same as a right-click
	});

	d->setWindowTitle(fd->srcName.isEmpty() ? QString("Shape detector")
	                                        : QString("Shape detector — %1").arg(fd->srcName));
	d->show();
}

// ============================================================================================
// Image > K-means classification — port of Mirone's src_figs/classificationfig.m. Two halves, as
// there: CLASSIFY the image (supervised, where the user clicks one seed per class, or unsupervised
// with k random centres) and then ISOLATE one or more of the classes it found, as colour or as mask.
//
// deps/ui/classification.ui carries classificationfig_LayoutFcn's geometry. Like every other tool
// here the dialog holds only settings and the picking; every pixel — the k-means itself (David
// Corney's dcKMeans, embedded in the .m) and the two isolations — is Julia's (src/classification.jl).
//
// The seeds come through the SAME point pick the Shape detector uses (Scene::vectorPickMode 3 +
// seedPickCB/seedPickEndCB, 85_polygon.cpp), so "click on the image to mark points" behaves
// identically in both tools — Mirone's getline_j, ended by a right-click or a double-click.
// ============================================================================================
class ClassificationDialog;
static std::map<Scene *, ClassificationDialog *> g_classifyDlgs;

class ClassificationDialog {
public:
	QDialog *dlg = nullptr;
	Scene   *scn = nullptr;
	QRadioButton *rSup = nullptr, *rUnsup = nullptr, *rColor = nullptr, *rMask = nullptr;
	QPushButton *bDefine = nullptr, *bCompute = nullptr, *bGetClass = nullptr, *bBgColor = nullptr;
	QLineEdit *eClasses = nullptr, *eNeighbors = nullptr;
	QListWidget *lstClasses = nullptr;
	QString srcName;
	std::vector<std::pair<double, double>> seeds;    // world x,y, one per class (click-define)
	QColor bg = Qt::white;                           // handles.bg_color
	bool reallyClose = false;

	bool supervised() const { return !rSup || rSup->isChecked(); }
	int  nClasses() const {
		bool ok = false;  int v = eClasses ? eClasses->text().toInt(&ok) : 3;
		return (ok && v >= 2) ? v : 3;               // edit_nClasses_CB: minimum allowed is 2
	}
	int  nNeighbors() const {
		bool ok = false;  int v = eNeighbors ? eNeighbors->text().toInt(&ok) : 3;
		return (ok && v >= 1) ? v : 3;
	}

	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();  dlg->raise();  dlg->activateWindow();
	}
	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel = m.addAction("Delete");
			QAction *pick = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) { reallyClose = true; unparkTool(scn, dlg); dlg->deleteLater(); }
		};
	}
	~ClassificationDialog() {
		disarm();
		for (auto it = g_classifyDlgs.begin(); it != g_classifyDlgs.end(); )
			it = (it->second == this) ? g_classifyDlgs.erase(it) : std::next(it);
	}

	void disarm() {
		if (!sceneAlive(scn)) return;
		if (scn->vectorPickMode == 3) scn->vectorPickMode = 0;
		scn->seedPickCB = nullptr;
		scn->seedPickEndCB = nullptr;
		if (scn->widget) scn->widget->unsetCursor();
	}

	// toggle_clickDefine_CB: collect one point per class until a right-click or a double-click ends
	// it (getline_j). Pressing the button again ends it too, so the three routes agree.
	void arm(bool on) {
		if (!sceneAlive(scn)) return;
		if (!on) { disarm(); report(); return; }
		seeds.clear();
		QPointer<QDialog> guard(dlg);
		scn->seedPickCB = [this, guard](double x, double y) {
			if (!guard) return;
			seeds.emplace_back(x, y);
			if (scn && scn->win)
				scn->win->statusBar()->showMessage(
				    QString("%1 class seed(s) — right-click or double-click when done")
				        .arg(seeds.size()), 4000);
		};
		scn->seedPickEndCB = [this, guard]() { if (guard) finish(); };
		scn->vectorPickMode = 3;
		scn->vectorPickDbl = false;
		if (scn->widget) scn->widget->setCursor(Qt::CrossCursor);
		if (scn->win) scn->win->statusBar()->showMessage(
			"Click ONE point on each colour/grey level you want as a class — then right-click "
			"or double-click to finish", 6000);
	}
	void finish() {
		disarm();
		if (bDefine) { QSignalBlocker blk(bDefine); bDefine->setChecked(false); }
		report();
	}
	void report() {
		if (bDefine)
			bDefine->setText(seeds.empty() ? QString("click-define")
			                               : QString("click-define (%1)").arg(seeds.size()));
	}

	// push_compute_CB. The seeds go over in WORLD coordinates; Julia turns them into pixels with the
	// same converter the Shape detector uses.
	void compute() {
		if (!g_juliaClassify) return;
		if (supervised() && seeds.size() < 2) {
			QMessageBox::warning(dlg, "K-means classification",
			                     "Supervised classification needs at least two seed points.\n"
			                     "Press \"click-define\" and click one point on each class.");
			return;
		}
		QString req = QString("compute;%1;%2;%3;%4")
		                  .arg(srcName).arg(supervised() ? 1 : 0).arg(nClasses()).arg(nNeighbors());
		for (const auto &s : seeds)
			req += QString(";%1,%2").arg(s.first, 0, 'g', 17).arg(s.second, 0, 'g', 17);
		QApplication::setOverrideCursor(Qt::WaitCursor);
		g_juliaClassify(scn, this, req.toUtf8().constData());
		QApplication::restoreOverrideCursor();
	}

	// The answer to "compute": fill the listbox with the classes that came out (0 … k-1, as the .m
	// labels them) and let "Isolate selected class" run.
	void setClasses(int k) {
		if (!lstClasses) return;
		lstClasses->clear();
		for (int i = 0; i < k; ++i) lstClasses->addItem(QString::number(i));
		tightenListRows(lstClasses);          // one text line per row, never Qt's default spacing
		if (k > 0) lstClasses->setCurrentRow(0);
		if (bGetClass) bGetClass->setEnabled(k > 0);
	}

	// push_getClass_CB
	void isolate() {
		if (!g_juliaClassify || !lstClasses) return;
		QList<QListWidgetItem *> sel = lstClasses->selectedItems();
		if (sel.isEmpty()) {
			QMessageBox::warning(dlg, "K-means classification", "Select one or more classes first.");
			return;
		}
		QStringList cls;
		for (QListWidgetItem *it : sel) cls << it->text();
		QString req = QString("isolate;%1;%2;%3,%4,%5;%6")
		                  .arg(srcName).arg(rMask && rMask->isChecked() ? 1 : 0)
		                  .arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(cls.join(','));
		QApplication::setOverrideCursor(Qt::WaitCursor);
		g_juliaClassify(scn, this, req.toUtf8().constData());
		QApplication::restoreOverrideCursor();
	}

	void paintBgButton() {
		if (!bBgColor) return;
		bBgColor->setStyleSheet(QString("background-color: %1; color: %2;")
		                            .arg(bg.name())
		                            .arg(bg.lightness() > 127 ? "black" : "white"));
	}
};

static void showClassification(QWidget *parent, Scene *s, const char *name) {
	if (!g_juliaClassify) {
		QMessageBox::warning(parent, "K-means classification",
		                     "K-means classification: callback not registered (rebuild/restart needed?).");
		return;
	}
	auto open = g_classifyDlgs.find(s);
	if (open != g_classifyDlgs.end() && open->second->dlg) { open->second->unpark(); return; }
	QUiLoader loader;
	QFile f(gmtvtkUiDir() + "/classification.ui");
	if (!f.open(QFile::ReadOnly)) {
		qWarning("showClassification: cannot open %s", qUtf8Printable(f.fileName()));
		return;
	}
	QDialog *d = qobject_cast<QDialog *>(loader.load(&f, parent));
	f.close();
	if (!d) return;

	auto *cd = new ClassificationDialog;
	cd->dlg = d;  cd->scn = s;
	cd->srcName    = QString::fromUtf8(name ? name : "");
	cd->rSup       = d->findChild<QRadioButton *>("radio_supervised");
	cd->rUnsup     = d->findChild<QRadioButton *>("radio_unsupervised");
	cd->rColor     = d->findChild<QRadioButton *>("radio_asColor");
	cd->rMask      = d->findChild<QRadioButton *>("radio_asMask");
	cd->bDefine    = d->findChild<QPushButton *>("toggle_clickDefine");
	cd->bCompute   = d->findChild<QPushButton *>("push_compute");
	cd->bGetClass  = d->findChild<QPushButton *>("push_getClass");
	cd->bBgColor   = d->findChild<QPushButton *>("push_bgColor");
	cd->eClasses   = d->findChild<QLineEdit *>("edit_nClasses");
	cd->eNeighbors = d->findChild<QLineEdit *>("edit_nNeighbors");
	cd->lstClasses = d->findChild<QListWidget *>("listbox_classes");
	if (!cd->rSup || !cd->bDefine || !cd->bCompute || !cd->lstClasses) { delete cd; delete d; return; }
	QObject::connect(d, &QObject::destroyed, d, [cd]() { delete cd; });
	g_classifyDlgs[s] = cd;

	struct CloseParks : QObject {
		ClassificationDialog *cd;
		CloseParks(QObject *parent, ClassificationDialog *c) : QObject(parent), cd(c) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::Close && cd && !cd->reallyClose && sceneAlive(cd->scn)) {
				e->ignore();
				cd->disarm();                    // a parked tool must not keep the view in pick mode
				cd->dlg->hide();
				ClassificationDialog *h = cd;
				parkTool(h->scn, h->dlg, "K-means classification", IC_Image,
				         "Closed K-means classification — double-click to bring it back, "
				         "click for Show / Delete",
				         [h]() { h->unpark(); }, h->parkedMenu());
				unfoldSceneObjects(cd->scn);
				return true;
			}
			return QObject::eventFilter(o, e);
		}
	};
	d->installEventFilter(new CloseParks(d, cd));

	// radio_supervised_CB / radio_unsupervised_CB: N classes is the unsupervised knob, click-define
	// the supervised one — each greys out with its mode, as in the .m.
	auto modeSync = [cd]() {
		const bool sup = cd->supervised();
		if (cd->eClasses) cd->eClasses->setEnabled(!sup);
		if (cd->bDefine) cd->bDefine->setEnabled(sup);
		if (!sup && cd->bDefine && cd->bDefine->isChecked()) cd->bDefine->setChecked(false);
	};
	for (QRadioButton *r : { cd->rSup, cd->rUnsup })
		if (r) QObject::connect(r, &QRadioButton::toggled, d, [modeSync](bool) { modeSync(); });
	modeSync();

	QObject::connect(cd->bDefine, &QPushButton::toggled, d, [cd](bool on) {
		if (on) cd->arm(true);
		else cd->finish();                        // pressed again = "done picking", as a right-click
	});
	QObject::connect(cd->bCompute, &QPushButton::clicked, d, [cd]() { cd->compute(); });
	if (cd->bGetClass)
		QObject::connect(cd->bGetClass, &QPushButton::clicked, d, [cd]() { cd->isolate(); });
	if (cd->bBgColor)
		QObject::connect(cd->bBgColor, &QPushButton::clicked, d, [cd, d]() {   // push_bgColor_CB
			QColor c = QColorDialog::getColor(cd->bg, d, "Background color");
			if (!c.isValid()) return;
			cd->bg = c;
			cd->paintBgButton();
		});
	cd->paintBgButton();

	d->setWindowTitle(cd->srcName.isEmpty() ? QString("kmeans classification")
	                                        : QString("kmeans classification — %1").arg(cd->srcName));
	d->show();
}

// ============================================================================================
// Image > Explore RGB — port of Mirone's mirone.m `Transfer_CB` 'RGBexp' plus the montage window
// utils/montage.m draws for it. THIRTEEN one-band components of the RGB image (Gray, R, G, B, H, S,
// V, Luminance, Red/Blue Chrominance, L*, a*, b*), as a grid of thumbnails, each labelled in the
// colour mirone.m gives it; clicking one takes THAT component at full resolution.
//
// Mirone's click opens a new Mirone figure. Here it lands as a NEW IMAGE IN THIS WINDOW's Scene
// Objects list, which is what iGMT does with every derived image (Julia's `_commit_derived_image!`).
//
// No .ui: this window is nothing but a montage of images, built from what Julia sends, so there is
// no fixed widget geometry to design. Every pixel is Julia's (src/rgbexplore.jl).
// ============================================================================================
static const char *const kRgbExpLabels[13] = {
	"Gray", "Red", "Green", "Blue", "Hue", "Saturation", "Value",
	"Luminance", "Red Chrominance", "Blue Chrominance",
	"L*a*b* => L*", "L*a*b* => a*", "L*a*b* => b*"
};
// mirone.m's `cores`: k, [0.7 0 0], [0 0.7 0], b, magenta*0.6 x3, cyan*0.6 x3, yellow*0.6 x3.
static QColor rgbExpLabelColor(int k) {
	switch (k) {
		case 0:  return QColor(0, 0, 0);
		case 1:  return QColor(178, 0, 0);
		case 2:  return QColor(0, 178, 0);
		case 3:  return QColor(0, 0, 255);
		case 4: case 5: case 6:  return QColor(153, 0, 153);
		case 7: case 8: case 9:  return QColor(0, 153, 153);
		default: return QColor(153, 153, 0);
	}
}

// One montage cell: the component thumbnail with its label burnt on top, and a click that asks for
// the full-resolution component. A plain QLabel override — no signals, so no moc involved.
struct RgbExpCell : QLabel {
	std::function<void()> onClick;
	explicit RgbExpCell(QWidget *parent = nullptr) : QLabel(parent) {}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && onClick) onClick();
		else QLabel::mousePressEvent(e);
	}
};

struct RgbExploreDialog {
	QDialog *dlg = nullptr;
	Scene   *scn = nullptr;
	QString  srcName;
	QGridLayout *grid = nullptr;
	bool     ready = false;

	// Julia's answer to "init": n thumbnails, w x h RGBA each, packed back to back, row 0 = SOUTH
	// (the viewer's own buffer convention — mirrored here for screen, exactly what montage.m's
	// 'flipud' does for a referenced Mirone image).
	void setThumbs(const unsigned char *rgba, int w, int h, int n) {
		if (!grid || !rgba || w < 1 || h < 1 || n < 1) return;
		if (n > 13) n = 13;
		// Cell shape follows the image's, so pick the layout whose overall shape is closest to a 4:3
		// window while wasting as few cells as possible — montage.m's `choose_layout` in spirit
		// (it reads the actual screen shape; the intent, a montage that is neither a strip nor a tower).
		int cols = 1;
		double best = 1e30;
		for (int c = 1; c <= n; ++c) {
			const int r = (n + c - 1) / c;
			const double aspect = (double)(c * w) / (double)(r * h);
			const double score = std::abs(std::log(aspect / (4.0 / 3.0))) + 0.05 * (r * c - n);
			if (score < best) { best = score; cols = c; }
		}
		for (int k = 0; k < n; ++k) {
			QImage im(w, h, QImage::Format_RGBA8888);
			memcpy(im.bits(), rgba + (size_t)k * w * h * 4, (size_t)w * h * 4);
			im = im.mirrored(false, true);                  // buffer row 0 is south; screen row 0 is north
			QPainter p(&im);                                // mirone.m: text(10, 50, label, 'FontSize',16, bold)
			QFont f = p.font(); f.setBold(true); f.setPointSize(11); p.setFont(f);
			p.setPen(rgbExpLabelColor(k));
			p.drawText(6, 6, w - 12, h - 12, Qt::AlignLeft | Qt::AlignTop, kRgbExpLabels[k]);
			p.end();
			auto *cell = new RgbExpCell(dlg);
			cell->setPixmap(QPixmap::fromImage(im));
			cell->setFixedSize(w, h);
			cell->setCursor(Qt::PointingHandCursor);
			cell->setToolTip(QString("%1 — click to add this component as a new image")
			                 .arg(kRgbExpLabels[k]));
			RgbExploreDialog *self = this;
			const int kk = k;
			cell->onClick = [self, kk]() { self->pick(kk); };
			grid->addWidget(cell, k / cols, k % cols);
		}
		ready = true;
	}

	// Click: the FULL-RESOLUTION component (never the thumbnail — that is only a preview of it) is
	// computed and added to this window's image list by Julia.
	void pick(int k) {
		if (!g_juliaRgbExplore || !sceneAlive(scn)) return;
		const QString req = QString("pick;%1;%2").arg(srcName).arg(k + 1);   // Julia counts 1..13
		QApplication::setOverrideCursor(Qt::WaitCursor);
		g_juliaRgbExplore(scn, this, req.toUtf8().constData());
		QApplication::restoreOverrideCursor();
	}
};

static void showRgbExplore(QWidget *parent, Scene *s, const char *name) {
	if (!g_juliaRgbExplore) {
		QMessageBox::warning(parent, "Explore RGB",
		                     "Explore RGB: callback not registered (rebuild/restart needed?).");
		return;
	}
	QDialog *d = new QDialog(parent);
	d->setAttribute(Qt::WA_DeleteOnClose);
	auto *lay = new QVBoxLayout(d);
	lay->setContentsMargins(4, 4, 4, 4);
	auto *grid = new QGridLayout;
	grid->setSpacing(2);
	lay->addLayout(grid);

	auto *rx = new RgbExploreDialog;
	rx->dlg = d;  rx->scn = s;  rx->srcName = QString::fromUtf8(name ? name : "");  rx->grid = grid;
	QObject::connect(d, &QObject::destroyed, d, [rx]() { delete rx; });

	// Julia builds the 13 component thumbnails and answers through gmtvtk_rgbexp_set_thumbs.
	QApplication::setOverrideCursor(Qt::WaitCursor);
	const int ok = g_juliaRgbExplore(s, rx, QString("init;%1").arg(rx->srcName).toUtf8().constData());
	QApplication::restoreOverrideCursor();
	if (!ok || !rx->ready) {
		QMessageBox::warning(parent, "Explore RGB",
		                     "Explore RGB needs an RGB image (see the Errors console for details).");
		delete d;
		return;
	}
	d->setWindowTitle(rx->srcName.isEmpty() ? QString("Explore RGB")
	                                        : QString("Explore RGB — %1").arg(rx->srcName));
	d->show();
}

static const struct BinarizeHookInstaller {
	BinarizeHookInstaller() {
		g_binarizeHasDialog = &binarizeHasDialog;
		g_binarizeReopen    = &binarizeReopen;
		g_enhanceReopen     = &enhanceReopen;
		g_showImageHisto    = [](Scene *s, const char *name) {
			showImageHistogram(s && s->widget ? s->widget->window() : nullptr, s, name);
		};
		g_showImageResize   = [](Scene *s, const char *name) {
			showImageResize(s && s->widget ? s->widget->window() : nullptr, s, name);
		};
		g_showFloodFill     = [](Scene *s, const char *name) {
			showFloodFill(s && s->widget ? s->widget->window() : nullptr, s, name);
		};
		g_showClassification = [](Scene *s, const char *name) {
			showClassification(s && s->widget ? s->widget->window() : nullptr, s, name);
		};
	}
} g_binarizeHookInstaller;

// ============================================================================================
// Empilhador (Tools) — port of Mirone's src_figs/empilhador.m. Stack a list of 2-D grids, or of
// MODIS/VIIRS/SeaWiFS L2 scenes, into ONE 3-D netCDF (or VTK, or a multi-band TIFF, or a VRT list).
// Loaded at RUNTIME via QUiLoader from deps/ui/empilhador.ui (plain Qt widget classes only, same
// technique as ClipGridDialog above).
//
// The list comes either from a list file (1, 2 or 3 columns, see GMT.jl's `empilhador` docstring)
// typed/browsed into the top box, or from data files picked one by one — the browse button accepts
// both and the list box always shows what will be stacked, in order.
//
// "L2 magic" reveals "Use config file"; checking THAT expands the dialog to the right with the four
// L2 knobs (SST quality, bitflags, cell size, N cells), pre-filled from ~/.gmt/L2config.txt — exactly
// Mirone's check_L2_CB / check_L2conf_CB pair, whose expansion was a Pos+[0 0 110 0] there and is a
// show/hide of l2ExtraWidget here (the .ui's own sizeHint decides the width, nothing is resized by
// hand). Checking "L2 magic" also ticks "Use sub-region?" for the user, as Mirone does: a scene in
// sensor coordinates has no region of its own to fall back on.
//
// Only Compute runs anything (only-action-button-executes-dialog). The dialog stays open afterwards
// so a second stack can be built without reopening it.
// ============================================================================================
class EmpilhadorDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QLineEdit *namesEdit, *northEdit, *southEdit, *westEdit, *eastEdit, *incEdit, *nCellsEdit;
	QListWidget *fileList;
	QCheckBox *l2Chk, *l2ConfChk, *regionChk, *bitflagsChk;
	QComboBox *qualityCombo;
	QRadioButton *netcdfRadio, *vtkRadio, *multiBandRadio, *vrtRadio;
	QWidget *l2Extra;
	int collapsedW = -1;			// width before "Use config file" ever expanded the dialog
	QStringList oneByOne;			// files picked one by one (Mirone's OneByOneNameList)

	explicit EmpilhadorDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/empilhador.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("EmpilhadorDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("EmpilhadorDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		dlg->setWindowTitle("Empilhador");
		QDialog *d = dlg;

		namesEdit    = d->findChild<QLineEdit *>("namesLineEdit");
		northEdit    = d->findChild<QLineEdit *>("northLineEdit");
		southEdit    = d->findChild<QLineEdit *>("southLineEdit");
		westEdit     = d->findChild<QLineEdit *>("westLineEdit");
		eastEdit     = d->findChild<QLineEdit *>("eastLineEdit");
		incEdit      = d->findChild<QLineEdit *>("incLineEdit");
		nCellsEdit   = d->findChild<QLineEdit *>("nCellsLineEdit");
		fileList     = d->findChild<QListWidget *>("fileListWidget");
		l2Chk        = d->findChild<QCheckBox *>("l2CheckBox");
		l2ConfChk    = d->findChild<QCheckBox *>("l2ConfigCheckBox");
		regionChk    = d->findChild<QCheckBox *>("regionCheckBox");
		bitflagsChk  = d->findChild<QCheckBox *>("bitflagsCheckBox");
		qualityCombo = d->findChild<QComboBox *>("qualityComboBox");
		netcdfRadio    = d->findChild<QRadioButton *>("netcdfRadioButton");
		vtkRadio       = d->findChild<QRadioButton *>("vtkRadioButton");
		multiBandRadio = d->findChild<QRadioButton *>("multiBandRadioButton");
		vrtRadio       = d->findChild<QRadioButton *>("vrtRadioButton");
		l2Extra        = d->findChild<QWidget *>("l2ExtraWidget");
		auto *browseBtn  = d->findChild<QToolButton *>("browseButton");
		auto *computeBtn = d->findChild<QPushButton *>("computeButton");

		// The four output radios are exclusive; the .ui does not group them explicitly, so do it
		// here (a QButtonGroup, not four hand-written toggle handlers).
		auto *grp = new QButtonGroup(d);
		grp->setExclusive(true);
		if (netcdfRadio)    grp->addButton(netcdfRadio);
		if (vtkRadio)       grp->addButton(vtkRadio);
		if (multiBandRadio) grp->addButton(multiBandRadio);
		if (vrtRadio)       grp->addButton(vrtRadio);

		if (l2ConfChk) l2ConfChk->setVisible(false);		// revealed by "L2 magic", as in Mirone
		if (l2Extra)   l2Extra->setVisible(false);
		setRegionEnabled(false);
		if (nCellsEdit && nCellsEdit->text().isEmpty()) nCellsEdit->setText("1");

		if (regionChk) QObject::connect(regionChk, &QCheckBox::toggled, d, [this](bool on) { setRegionEnabled(on); });

		// "L2 magic": reveal the config-file option and tick the sub-region for the user.
		if (l2Chk) QObject::connect(l2Chk, &QCheckBox::toggled, d, [this](bool on) {
			if (l2ConfChk) l2ConfChk->setVisible(on);
			if (on && regionChk) regionChk->setChecked(true);
			if (!on && l2ConfChk) l2ConfChk->setChecked(false);
		});

		// "Use config file": expand to the right and fill the four boxes from ~/.gmt/L2config.txt.
		// Unchecking must give the ORIGINAL size back. `adjustSize()` alone will not do it: it never
		// shrinks a window the user (or a previous expansion) has already sized, so the collapsed
		// width is remembered on the first expansion and restored verbatim here — the .ui's own
		// geometry, not a computed one.
		if (l2ConfChk) QObject::connect(l2ConfChk, &QCheckBox::toggled, d, [this, d](bool on) {
			if (on && collapsedW < 0) collapsedW = d->width();
			if (l2Extra) l2Extra->setVisible(on);
			if (on) loadConfig();
			if (on) {
				d->adjustSize();			// let the .ui's own sizeHint decide the expanded width
			} else if (collapsedW > 0) {
				d->layout()->activate();	// hidden widget is out of the layout: recompute, then shrink
				d->resize(collapsedW, d->height());
			}
		});

		// Bitflags and the SST quality level are alternatives, never both (Mirone check_bitflags_CB).
		if (bitflagsChk && qualityCombo)
			QObject::connect(bitflagsChk, &QCheckBox::toggled, d, [this](bool on) { qualityCombo->setEnabled(!on); });

		// Browse: a list file, or the data files themselves. Anything that is not a plain text list
		// is treated as data and appended to the one-by-one list (Mirone's `bin` branch).
		if (browseBtn) QObject::connect(browseBtn, &QToolButton::clicked, d, [this, d]() {
			const QStringList sel = QFileDialog::getOpenFileNames(d, "File with the grids list, or the grids themselves",
				prefStartDir(), "List files (*.txt *.dat);;Data files (*.nc *.grd *.hdf *.h5 *.tif *.tiff *.L2*);;All files (*.*)");
			if (sel.isEmpty()) return;
			rememberStartDir(sel.first());
			if (sel.size() == 1 && isListFile(sel.first())) {
				oneByOne.clear();
				if (namesEdit) namesEdit->setText(sel.first());
				showListFile(sel.first());
			} else {
				if (namesEdit) namesEdit->clear();
				for (const QString &s : sel) oneByOne << s;
				if (fileList) {
					fileList->clear();
					for (const QString &s : oneByOne) fileList->addItem(QFileInfo(s).fileName());
					tightenListRows(fileList);
				}
			}
		});

		// Typing a name (or a wildcard request) straight into the box and pressing Enter loads it.
		if (namesEdit) QObject::connect(namesEdit, &QLineEdit::editingFinished, d, [this]() {
			const QString t = namesEdit->text().trimmed();
			if (t.isEmpty() || !QFileInfo::exists(t)) return;	// a wildcard is resolved Julia-side
			oneByOne.clear();
			showListFile(t);
		});

		if (computeBtn) QObject::connect(computeBtn, &QPushButton::clicked, d, [this, d]() { compute(d); });

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}

	void setRegionEnabled(bool on) {
		if (northEdit) northEdit->setEnabled(on);
		if (southEdit) southEdit->setEnabled(on);
		if (westEdit)  westEdit->setEnabled(on);
		if (eastEdit)  eastEdit->setEnabled(on);
	}

	// A list file is a text file whose first non-comment line names something that exists.
	static bool isListFile(const QString &path) {
		QFile f(path);
		if (!f.open(QFile::ReadOnly | QFile::Text)) return false;
		QTextStream ts(&f);
		for (int k = 0; k < 20 && !ts.atEnd(); ++k) {
			const QString ln = ts.readLine().trimmed();
			if (ln.isEmpty() || ln.startsWith('#') || ln.startsWith('@') || ln.startsWith('>')) continue;
			const QString first = ln.split(QRegularExpression("\\s+")).value(0);
			const QFileInfo fi(QFileInfo(path).absolutePath() + "/" + first);
			return QFileInfo::exists(first) || fi.exists();
		}
		return false;
	}

	// Show the names a list file holds, so the user sees what Compute will chew on.
	void showListFile(const QString &path) {
		if (!fileList) return;
		fileList->clear();
		QFile f(path);
		if (!f.open(QFile::ReadOnly | QFile::Text)) return;
		QTextStream ts(&f);
		while (!ts.atEnd()) {
			const QString ln = ts.readLine().trimmed();
			if (ln.isEmpty() || ln.startsWith('#') || ln.startsWith('@') || ln.startsWith('>')) continue;
			fileList->addItem(QFileInfo(ln.split(QRegularExpression("\\s+")).value(0)).fileName());
		}
		tightenListRows(fileList);
	}

	// Fill the region and the two interpolation boxes from ~/.gmt/L2config.txt. The parsing lives in
	// Julia (GMT.jl's _emp_sniff_config is the ONE reader of that file); this only asks for the
	// values, as "w/e/s/n;inc;ncells;quality".
	void loadConfig() {
		if (!g_juliaEval) return;
		static std::vector<char> buf(1 << 12);
		const QString cmd = QString("InteractiveGMT._emp_config_str()");
		int n = g_juliaEval(scn, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
		if (n <= 0) return;
		const QStringList p = QString::fromUtf8(buf.data(), n).trimmed().split(';');
		if (p.size() >= 1 && p[0].count('/') == 3) {
			const QStringList r = p[0].split('/');
			if (westEdit)  westEdit->setText(r[0]);
			if (eastEdit)  eastEdit->setText(r[1]);
			if (southEdit) southEdit->setText(r[2]);
			if (northEdit) northEdit->setText(r[3]);
		}
		if (p.size() >= 2 && incEdit)    incEdit->setText(p[1]);
		if (p.size() >= 3 && nCellsEdit) nCellsEdit->setText(p[2]);
		if (p.size() >= 4 && qualityCombo) {
			const int i = qualityCombo->findText(p[3]);
			if (i >= 0) qualityCombo->setCurrentIndex(i);
		}
	}

	// Compute: the ONE action button. Ask for the output name, then hand everything to Julia.
	void compute(QDialog *d) {
		if (!g_juliaEmpilhador) { QMessageBox::warning(d, "Empilhador", "Empilhador: callback not registered (rebuild/restart needed?)."); return; }
		const QString list = namesEdit ? namesEdit->text().trimmed() : QString();
		if (list.isEmpty() && oneByOne.isEmpty()) {
			QMessageBox::warning(d, "Empilhador", "No files to work on. Give me a list file or pick the files.");
			return;
		}

		const bool isVrt  = vrtRadio && vrtRadio->isChecked();
		const bool isVtk  = vtkRadio && vtkRadio->isChecked();
		const bool isTiff = multiBandRadio && multiBandRadio->isChecked();
		const char *fmt   = isVrt ? "vrt" : isVtk ? "vtk" : isTiff ? "tiff" : "netcdf";
		const QString filter = isVrt  ? "VRT list (*.vrt)"
		                     : isVtk  ? "VTK format (*.vtk)"
		                     : isTiff ? "(Geo)Tiff format (*.tiff *.tif)"
		                              : "netCDF grid format (*.nc *.grd)";
		const QString out = QFileDialog::getSaveFileName(d, "Output file", prefStartDir(), filter + ";;All files (*.*)");
		if (out.isEmpty()) return;
		rememberStartDir(out);

		const bool l2   = l2Chk && l2Chk->isChecked();
		const bool conf = l2ConfChk && l2ConfChk->isChecked();
		if (l2 && isTiff) {
			QMessageBox::warning(d, "Empilhador", "A multi-band image cannot be made out of L2 MODIS scenes.");
			return;
		}

		QString p;
		p += "out=" + out + "\n";
		p += QString("fmt=%1\n").arg(fmt);
		if (!list.isEmpty()) p += "list=" + list + "\n";
		for (int k = 0; k < oneByOne.size(); ++k) p += QString("file%1=%2\n").arg(k).arg(oneByOne[k]);
		if (regionChk && regionChk->isChecked()) {
			bool ok[4];
			const double w = westEdit->text().toDouble(&ok[0]), e = eastEdit->text().toDouble(&ok[1]);
			const double s = southEdit->text().toDouble(&ok[2]), n = northEdit->text().toDouble(&ok[3]);
			if (!ok[0] || !ok[1] || !ok[2] || !ok[3]) {
				QMessageBox::warning(d, "Empilhador", "One or more of the region limits was not provided.");
				return;
			}
			if (w >= e || s >= n) {
				QMessageBox::warning(d, "Empilhador", "West must be < East and South < North.");
				return;
			}
			p += QString("region=%1/%2/%3/%4\n").arg(w, 0, 'g', 12).arg(e, 0, 'g', 12).arg(s, 0, 'g', 12).arg(n, 0, 'g', 12);
		}
		p += QString("l2=%1\n").arg(l2 ? 1 : 0);
		p += QString("config=%1\n").arg(conf ? 1 : 0);
		if (l2) {
			if (bitflagsChk && bitflagsChk->isChecked()) p += "bitflags=1\n";
			else if (qualityCombo) p += "quality=" + qualityCombo->currentText() + "\n";
			if (incEdit && !incEdit->text().trimmed().isEmpty())       p += "inc=" + incEdit->text().trimmed() + "\n";
			if (nCellsEdit && !nCellsEdit->text().trimmed().isEmpty()) p += "ncells=" + nCellsEdit->text().trimmed() + "\n";
		}

		showBusyDialog("Stacking…");
		const int ok = g_juliaEmpilhador(scn, p.toUtf8().constData());
		closeBusyDialog();
		if (!ok) {
			QMessageBox::warning(d, "Empilhador", "Stacking failed — see this window's Errors console for details.");
			return;
		}
		// Written. Offer to load it, through the SAME file-open path as a drop or File > Open — a
		// stacked cube is just a file, and there is only one way into this viewer.
		const auto r = QMessageBox::question(d, "Empilhador", "Done — wrote\n\n" + out + "\n\nOpen it now?",
		                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
		if (r == QMessageBox::Yes) juliaOpenFile(scn, out.toUtf8().constData());
	}
};

// ============================================================================================
// Ocean Color Data Browser (Geophysics menu). Loaded at RUNTIME via QUiLoader from
// deps/ui/oceancolor_browser.ui (plain Qt widget classes only, same technique as the dialogs above).
//
// Browse the NASA OB.DAAC L3 archive: pick Instrument / Product / Period and the two preview tiles
// fill with the newest images that ACTUALLY exist on the server — the end of an instrument's record
// is never assumed here, it is asked (src/oceancolor.jl knows the catalogue and does the asking).
// < and > walk the pair one composite at a time; the date box jumps to a given day.
//
// The dialog holds no catalogue of its own: every question is one g_juliaOceanColor round-trip and
// the answer comes back through gmtvtk_oc_set_tile / gmtvtk_oc_status while that call is still on
// the stack. So there is exactly ONE place that knows what a file is called, and it is not here.
//
// The tiles are clickable — the selected one (blue frame) is what "Extract / Download" acts on, and
// a DOUBLE-click on a tile extracts that one directly. Extract downloads the image and puts it into
// the window georeferenced: the L3 browse PNG is the bare global grid, plate carrée, not a decorated
// figure, so its limits are known ([-180 180 -90 90], pixel-registered) and it IS a map.
//
// The X does NOT destroy the dialog: it hides and PARKS as a handle in the bottom strip of this
// window's Scene Objects dock, exactly like a closed X,Y plot / Contours / Binarize dialog — same
// Scene::parkedTools list, same parkTool/unparkTool pair, same row builder (50_scene.cpp), so the
// tiles the user browsed to survive and a double-click brings them straight back. Only the parked
// row's own "Delete" really closes it. Re-picking the menu entry re-opens the SAME dialog
// (g_oceanColorDlgs), never a second one.
// ============================================================================================
class OceanColorDialog;
static std::map<Scene *, OceanColorDialog *> g_oceanColorDlgs;   // one browser per window, alive while parked

// The two preview tiles are plain QLabels, which have no clicked() signal. One filter per tile turns
// a press into a selection; a filter on the dialog itself rescales both pixmaps when it is resized.
struct OCEventFilter : QObject {
	OceanColorDialog *owner;
	int idx;					// 0/1 = that tile was clicked; -1 = the dialog was resized
	OCEventFilter(OceanColorDialog *o, int i, QObject *parent) : QObject(parent), owner(o), idx(i) {}
	bool eventFilter(QObject *ob, QEvent *e) override;
};

class OceanColorDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QComboBox *cbInst = nullptr, *cbProd = nullptr, *cbPeriod = nullptr;
	QDateTimeEdit *dtEdit = nullptr;
	QLabel *tile[2] = { nullptr, nullptr }, *cap[2] = { nullptr, nullptr }, *status = nullptr;
	QLabel *cacheLbl = nullptr;	// the cache directory + how many MB of browse images/grids sit in it
	QPushButton *dlBtn = nullptr;
	QTimer *dateTimer = nullptr;	// lets the calendar popup finish closing before a scan starts
	QDate askedDate;				// the day already asked about — re-asking it is a no-op, not a second scan
	QPixmap full[2];			// the browse images at full size; the labels show scaled copies
	QString url[2];				// the address behind each tile ("" = the slot is empty)
	QString startYmd[2];		// each composite's START day (yyyyMMdd) — what < and > step from
	int sel = 1;				// the tile Extract acts on; the NEWER one to begin with
	bool reallyClose = false;	// set only by the parked row's "Delete"; otherwise the X parks

	explicit OceanColorDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/oceancolor_browser.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("OceanColorDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("OceanColorDialog: QUiLoader failed to load the .ui"); return; }
		// WA_DeleteOnClose is deliberately NOT set: the dialog (and the images browsed into its tiles)
		// has to survive being closed, because closing PARKS it. Only the parked row's "Delete" sets
		// reallyClose and lets a close event through.
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;
		g_oceanColorDlgs[scn] = this;

		struct CloseParks : QObject {
			OceanColorDialog *oc;
			CloseParks(QObject *parent, OceanColorDialog *o) : QObject(parent), oc(o) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close && oc && !oc->reallyClose && sceneAlive(oc->scn)) {
					e->ignore();
					oc->dlg->hide();
					OceanColorDialog *h = oc;	// a lambda cannot capture a member of the enclosing class
					parkTool(h->scn, h->dlg, "Ocean Color", IC_Image,
					         "Closed Ocean Color browser — double-click to bring it back, click for Show / Delete",
					         [h]() { h->unpark(); }, h->parkedMenu());
					// A handle the user cannot see is the same as no handle at all: reveal + unfold.
					unfoldSceneObjects(oc->scn);
					return true;
				}
				return QObject::eventFilter(o, e);
			}
		};
		dlg->installEventFilter(new CloseParks(dlg, this));
		// This wrapper outlives the constructor and is owned by the QDialog: when the dialog really is
		// destroyed (the parked row's Delete, or the parent window going away), the wrapper goes too.
		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });

		cbInst   = d->findChild<QComboBox *>("comboInstrument");
		cbProd   = d->findChild<QComboBox *>("comboProduct");
		cbPeriod = d->findChild<QComboBox *>("comboPeriod");
		dtEdit   = d->findChild<QDateTimeEdit *>("editDateTime");
		tile[0]  = d->findChild<QLabel *>("labelTileFirst");
		tile[1]  = d->findChild<QLabel *>("labelTileLast");
		cap[0]   = d->findChild<QLabel *>("labelTileFirstCaption");
		cap[1]   = d->findChild<QLabel *>("labelTileLastCaption");
		status   = d->findChild<QLabel *>("labelStatus");
		cacheLbl = d->findChild<QLabel *>("labelCache");
		dlBtn    = d->findChild<QPushButton *>("btnDownload");
		auto *prevBtn  = d->findChild<QToolButton *>("btnPrev");
		auto *nextBtn  = d->findChild<QToolButton *>("btnNext");
		auto *clearBtn = d->findChild<QToolButton *>("btnClearDateTime");

		// The tiles carry the two NEWEST images, not the ends of the record: the .ui's placeholder
		// captions would lie about that, so they are replaced before anything is fetched.
		for (int i = 0; i < 2; ++i) {
			if (tile[i]) {
				tile[i]->setText("");
				tile[i]->installEventFilter(new OCEventFilter(this, i, d));
			}
			if (cap[i]) cap[i]->setText("—");
		}
		d->installEventFilter(new OCEventFilter(this, -1, d));
		if (dtEdit) dtEdit->setDateTime(QDateTime::currentDateTimeUtc());

		// Picking an instrument / product / period is a question about the archive, so it asks it —
		// that IS this dialog's job (nothing is computed and nothing is added to the scene until
		// Extract, which stays the only action button).
		auto refresh = [this]() { requestLatest(); };
		if (cbInst)   QObject::connect(cbInst,   QOverload<int>::of(&QComboBox::currentIndexChanged), d, refresh);
		if (cbProd)   QObject::connect(cbProd,   QOverload<int>::of(&QComboBox::currentIndexChanged), d, refresh);
		if (cbPeriod) QObject::connect(cbPeriod, QOverload<int>::of(&QComboBox::currentIndexChanged), d, refresh);

		// < and > step the PAIR one composite at a time, each from the end it moves: < from the older
		// tile, > from the newer one, so repeated clicks walk the archive without ever re-showing the
		// same image twice.
		if (prevBtn) QObject::connect(prevBtn, &QToolButton::clicked, d, [this]() { step(0, -1); });
		if (nextBtn) QObject::connect(nextBtn, &QToolButton::clicked, d, [this]() { step(1, +1); });

		// The date box is navigation, not compute: it re-centres the pair on the day it holds.
		//
		// It fires on a date the user COMMITTED, never on one merely passed over. NOT on dateChanged:
		// the calendar popup writes each date it lands on straight into the editor as you move through
		// it, so dateChanged fires while you are still walking the months — and each fire raised the
		// busy dialog, which took the focus away from the popup, which moved the date again. That is
		// the blinking loop. `clicked`/`activated` on the calendar itself is the commit; typing or
		// spinning is committed by Enter / leaving the field (editingFinished).
		//
		// The single-shot timer is what keeps the busy dialog OUT of the popup's teardown: it lets the
		// click finish closing the calendar before a scan (and its dialog) starts.
		dateTimer = new QTimer(d);
		dateTimer->setSingleShot(true);
		dateTimer->setInterval(60);
		QObject::connect(dateTimer, &QTimer::timeout, d, [this]() { requestAt(); });
		// NOT editingFinished either: opening the calendar popup moves the focus off the line edit, and
		// losing focus IS "editing finished" to Qt — so merely CLICKING the calendar button fired a
		// scan, and its busy dialog came up on top of the calendar the user had only just opened.
		// A typed or spun date is committed by Enter, and nothing else.
		if (dtEdit) {
			dtEdit->installEventFilter(new OCEventFilter(this, -2, d));
			if (QCalendarWidget *cal = dtEdit->calendarWidget()) {
				QObject::connect(cal, &QCalendarWidget::clicked,   d, [this](const QDate &) { dateTimer->start(); });
				QObject::connect(cal, &QCalendarWidget::activated, d, [this](const QDate &) { dateTimer->start(); });
			}
			askedDate = dtEdit->date();		// what is on screen now was NOT asked for by the user
		}
		if (clearBtn) QObject::connect(clearBtn, &QToolButton::clicked, d, [this]() {
			if (dtEdit) {
				QSignalBlocker b(dtEdit);
				dtEdit->setDateTime(QDateTime::currentDateTimeUtc());
			}
			requestLatest();
		});

		if (dlBtn) QObject::connect(dlBtn, &QPushButton::clicked, d, [this]() { extract(sel); });

		selectTile(1);
		d->show();
		requestLatest();			// fill the tiles with the newest of the .ui's default selection
	}

	// --- what the dialog asks -----------------------------------------------------------------
	QString base() const {
		return QString("inst=%1\nprod=%2\nperiod=%3\n")
		       .arg(cbInst   ? cbInst->currentIndex()   + 1 : 1)
		       .arg(cbProd   ? cbProd->currentIndex()   + 1 : 1)
		       .arg(cbPeriod ? cbPeriod->currentIndex() + 1 : 1);
	}

	// `clear` distinguishes the two kinds of request: a BROWSE one replaces both tiles, so they are
	// emptied first (a stale preview under a new selection would be a lie); Extract answers about a
	// tile that is already on screen and must leave the previews exactly where they are.
	void request(const QString &extra, const char *busy, bool clear = true) {
		if (!g_juliaOceanColor) {
			setStatus("Ocean Color: callback not registered (rebuild/restart needed?).");
			return;
		}
		if (clear) clearTiles();
		const QByteArray p = (base() + extra).toUtf8();
		showBusyDialog(busy);
		const int ok = g_juliaOceanColor(scn, this, p.constData());
		closeBusyDialog();
		// A BROWSE ends by naming what is now selected — the status line belongs to the selection, not to
		// the last thing that happened. An ACTION (clear == false) keeps whatever the host reported about
		// it, which is the only case where the line says something the file name cannot.
		if (clear && !url[sel].isEmpty()) setStatus(fileOf(sel));
		if (!ok && clear && url[0].isEmpty() && url[1].isEmpty())
			setStatus("Nothing found for this selection — see this window's Errors console.");
	}

	void requestLatest() {
		askedDate = QDate();			// the tiles no longer show what the date box says
		request("req=latest\n", "Asking the OB.DAAC archive…");
	}

	// Re-centre on the day the box holds. A day already asked about is NOT asked again: the commit
	// signals can arrive more than once for one user action (a calendar click that also moves the
	// focus emits editingFinished too), and each extra scan would raise the busy dialog again.
	// Enter in the date box: commit whatever is typed/spun in it. Routed through the same timer as the
	// calendar's click, so there is one path from "the user chose a day" to the scan.
	void commitDate() { if (dateTimer) dateTimer->start(); }

	void requestAt() {
		if (!dtEdit) return;
		// Never while the calendar is still on screen: the scan's busy dialog would take the focus off
		// it. Wait for it to close instead — the commit is not lost, only deferred.
		if (QCalendarWidget *cal = dtEdit->calendarWidget())
			if (cal->isVisible()) { dateTimer->start(); return; }
		const QDate d = dtEdit->date();
		if (!d.isValid() || d == askedDate) return;
		askedDate = d;
		request(QString("req=at\ndate=%1\n").arg(d.toString("yyyyMMdd")),
		        "Asking the OB.DAAC archive…");
	}

	// Step from the tile at `from` (0 = older, 1 = newer) by one composite in direction `dir`.
	// With that end empty there is nothing to step from, so fall back to "what is newest".
	void step(int from, int dir) {
		askedDate = QDate();			// walked away from the box's day: it can be re-picked
		if (startYmd[from].isEmpty()) { requestLatest(); return; }
		request(QString("req=step\nstart=%1\ndir=%2\n").arg(startYmd[from]).arg(dir),
		        dir < 0 ? "Stepping back…" : "Stepping forward…");
	}

	// Extract: fetch the DATA behind tile `i` — the L3 netCDF, not the browse picture — and open it in
	// this window. `req=grid` is the SAME request the "Download grid" button pinned under a placed image
	// sends, so there is one downloader and one opener; the host turns a browse-image address into its
	// netCDF one itself (the two differ only by the trailing ".png"). It falls back to placing the browse
	// image when the data file cannot be had (it lives behind Earthdata URS and needs credentials).
	// The button acts on the selected tile and a double-click on the tile it was aimed at — ONE path,
	// not two, so a double-click can never do something the button would not.
	void extract(int i) {
		if (i < 0 || i > 1) return;
		if (url[i].isEmpty()) { setStatus("Nothing to extract — that tile is empty."); return; }
		selectTile(i);
		request(QString("req=grid\nurl=%1\n").arg(url[i]), "Downloading the grid…", false);
	}

	// Place tile `i`'s BROWSE IMAGE — the free, already-cached picture — in the window as a
	// georeferenced map. This is the double-click action; the button next to it fetches the data.
	void placeImage(int i) {
		if (i < 0 || i > 1) return;
		if (url[i].isEmpty()) { setStatus("Nothing to place — that tile is empty."); return; }
		selectTile(i);
		request(QString("req=open\nurl=%1\n").arg(url[i]), "Placing the browse image…", false);
	}

	// --- what the host pushes back ------------------------------------------------------------
	void setStatus(const QString &m) { if (status) status->setText(m); }

	// The cache line: which directory the browse images and downloaded grids go to, and how much is in
	// it. Pushed by the host (it owns the path — GMTuserdir), never guessed here.
	void setCacheInfo(const QString &m) { if (cacheLbl) cacheLbl->setText(m); }

	// Which Product rows the CURRENT instrument actually flies (bit i set = row i+1 is real), and which
	// one is in force. The host owns that fact — it is the one holding the catalogue — so this only
	// obeys: greys the rest, and moves the combo WITHOUT emitting, because the host has already
	// answered for the product it is passing. Emitting would fire a second, identical request.
	void setProducts(int mask, int cur) {
		if (!cbProd) return;
		auto *model = qobject_cast<QStandardItemModel *>(cbProd->model());
		for (int i = 0; i < cbProd->count(); ++i) {
			const bool on = (mask & (1 << i)) != 0;
			if (model && model->item(i)) model->item(i)->setEnabled(on);
			cbProd->setItemData(i, on ? QVariant() : QVariant("Not carried by this instrument"),
			                    Qt::ToolTipRole);
		}
		if (cur >= 1 && cur <= cbProd->count() && cbProd->currentIndex() != cur - 1) {
			QSignalBlocker b(cbProd);
			cbProd->setCurrentIndex(cur - 1);
		}
	}

	void clearTiles() {
		for (int i = 0; i < 2; ++i) {
			full[i] = QPixmap();
			url[i].clear();
			startYmd[i].clear();
			if (tile[i]) { tile[i]->setPixmap(QPixmap()); tile[i]->setText(""); }
			if (cap[i])  cap[i]->setText("—");
		}
	}

	void setTile(int i, const QString &pngPath, const QString &caption, const QString &u, const QString &ymd) {
		if (i < 0 || i > 1) return;
		url[i]      = u;
		startYmd[i] = ymd;
		if (cap[i]) cap[i]->setText(caption.isEmpty() ? "—" : caption);
		full[i] = pngPath.isEmpty() ? QPixmap() : QPixmap(pngPath);
		if (tile[i] && full[i].isNull()) tile[i]->setText(u.isEmpty() ? "" : "(no preview)");
		rescale(i);
	}

	void rescale(int i) {
		if (!tile[i] || full[i].isNull()) return;
		const QSize sz = tile[i]->contentsRect().size() - QSize(4, 4);
		if (sz.width() < 8 || sz.height() < 8) return;
		tile[i]->setPixmap(full[i].scaled(sz, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}

	void selectTile(int i) {
		if (i < 0 || i > 1) return;
		sel = i;
		for (int k = 0; k < 2; ++k)
			if (tile[k])
				tile[k]->setStyleSheet(k == sel ? "border: 2px solid #2d7ff9;" : "border: 1px solid #888;");
		if (dlBtn) dlBtn->setToolTip(url[sel].isEmpty() ? "Nothing selected" : url[sel]);
		// The status line names WHAT IS SELECTED, so it follows the selection instead of freezing on
		// whatever the last action reported. The file is the netCDF the Extract button will fetch, which
		// is the browse-image name minus its ".png".
		setStatus(fileOf(sel));
	}

	// The data file behind tile `i`: the browse-image basename with the ".png" taken off (the L3 netCDF
	// carries exactly that name). Empty tile -> a plain word, never a stale name from the other tile.
	QString fileOf(int i) const {
		if (i < 0 || i > 1 || url[i].isEmpty()) return QStringLiteral("—");
		QString b = url[i].section('/', -1);
		if (b.endsWith(".png", Qt::CaseInsensitive)) b.chop(4);
		return b;
	}

	// --- parking (Scene Objects bottom strip) -------------------------------------------------
	// Bring a parked browser back exactly as it was — the tiles it was showing are still in it, so
	// nothing is re-fetched. Same shape as every other parkable dialog's unpark().
	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();
		dlg->raise();
		dlg->activateWindow();
	}

	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel  = m.addAction("Delete");
			QAction *pick  = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scn, dlg);
				dlg->deleteLater();			// destroyed -> the row and this object go with it
			}
		};
	}

	~OceanColorDialog() {
		for (auto it = g_oceanColorDlgs.begin(); it != g_oceanColorDlgs.end(); )
			it = (it->second == this) ? g_oceanColorDlgs.erase(it) : std::next(it);
	}
};

// --- the NASA Earthdata login ------------------------------------------------------------------
// OB.DAAC gives the browse IMAGES away but keeps the DATA files behind Earthdata URS, so a grid
// download needs an account. The host asks for it HERE (once), then writes it to ~/.netrc itself —
// the standard file curl, wget and GDAL already read — and reports back what it wrote. Nothing is
// kept on this side: the two strings are copied straight into the caller's buffers and the widgets
// die with the dialog.
static int ocAskLogin(Scene *s, const char *msg, char *userBuf, int uCap, char *passBuf, int pCap) {
	if (!userBuf || uCap < 2 || !passBuf || pCap < 2) return 0;
	QWidget *parent = (s && s->win) ? static_cast<QWidget *>(s->win) : nullptr;
	QDialog d(parent);
	d.setWindowTitle("NASA Earthdata login");
	auto *v = new QVBoxLayout(&d);
	auto *lbl = new QLabel(QString::fromUtf8(msg ? msg : ""), &d);
	lbl->setWordWrap(true);
	lbl->setTextInteractionFlags(Qt::TextBrowserInteraction);
	lbl->setOpenExternalLinks(true);
	lbl->setMinimumWidth(420);
	v->addWidget(lbl);
	auto *form = new QFormLayout;
	auto *eu = new QLineEdit(&d);
	auto *ep = new QLineEdit(&d);
	ep->setEchoMode(QLineEdit::Password);
	form->addRow("Username", eu);
	form->addRow("Password", ep);
	v->addLayout(form);
	auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
	v->addWidget(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
	eu->setFocus();
	if (d.exec() != QDialog::Accepted) return 0;
	const QByteArray u = eu->text().trimmed().toUtf8();
	const QByteArray p = ep->text().toUtf8();
	if (u.isEmpty() || p.isEmpty() || u.size() >= uCap || p.size() >= pCap) return 0;
	memcpy(userBuf, u.constData(), u.size()); userBuf[u.size()] = '\0';
	memcpy(passBuf, p.constData(), p.size()); passBuf[p.size()] = '\0';
	return 1;
}

// --- the "Download grid" button pinned under an Ocean Color image ------------------------------
// The browse image the browser places is the L4 product (a PNG). The real DATA is the L3 netCDF of
// the very same name, so the button needs no catalogue of its own — Julia hands it the address when
// it places the image. Placement is redone before every render (a vtkCommand::StartEvent observer,
// the same hook the Z axis billboards use) so the button follows the image through pan/zoom/rotate
// instead of floating at a fixed corner.
static void ocPlaceGridButton(Scene *s) {
	if (!s || !s->ocBtn) return;
	const ExtraObj *own = nullptr;
	for (const auto &ex : s->extras)
		if (ex.name == s->ocBtnOwner) { own = &ex; break; }
	// Image gone, hidden, or no renderer to project through: no button either.
	if (!own || !own->actor || own->actor->GetVisibility() == 0 || !s->ren || !s->widget) {
		s->ocBtn->hide();
		return;
	}
	double b[6];
	own->actor->GetBounds(b);			// already in SCALED world coords, so no VE maths here
	const double wx = 0.5 * (b[0] + b[1]), wy = b[2], wz = 0.5 * (b[4] + b[5]);
	s->ren->SetWorldPoint(wx, wy, wz, 1.0);
	s->ren->WorldToDisplay();
	double *dd = s->ren->GetDisplayPoint();
	// VTK display coords are bottom-up in RENDERER pixels; Qt widget coords are top-down in LOGICAL
	// pixels. Convert through the widget's device-pixel ratio or the button lands off-screen on a
	// HiDPI display.
	const double dpr = s->widget->devicePixelRatioF() > 0 ? s->widget->devicePixelRatioF() : 1.0;
	const int px = int(dd[0] / dpr);
	const int py = int(s->widget->height() - dd[1] / dpr);
	const QSize sz = s->ocBtn->sizeHint();
	// A little BELOW the image edge, centred on it, and clamped so it never leaves the view.
	int x = px - sz.width() / 2;
	int y = py + 6;
	x = std::max(0, std::min(x, s->widget->width()  - sz.width()));
	y = std::max(0, std::min(y, s->widget->height() - sz.height()));
	s->ocBtn->setGeometry(x, y, sz.width(), sz.height());
	s->ocBtn->show();
	s->ocBtn->raise();
}

static void OcButtonCB(vtkObject *, unsigned long, void *cd, void *) {
	ocPlaceGridButton(static_cast<Scene *>(cd));
}

// --- the grid-download progress dialog ---------------------------------------------------------
// An L3 file is tens of MB, so the indeterminate busy spinner every other tool uses is wrong here:
// it says "working" and nothing else, which on a slow link is indistinguishable from a hang. This
// one shows the real numbers — how much has arrived out of how much there is.
//
// The download runs on the UI thread (Julia is called synchronously from the button), so the bar
// can only repaint if we pump the loop ourselves: ocProgressSet does that on every update. Cancel
// is reported back through its return value, which Julia turns into an aborted transfer.
static QProgressDialog *g_ocProgress = nullptr;
static bool             g_ocCancelled = false;

static QString ocHumanBytes(double n) {
	if (n >= 1024.0 * 1024.0 * 1024.0) return QString::number(n / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
	if (n >= 1024.0 * 1024.0)          return QString::number(n / (1024.0 * 1024.0), 'f', 1) + " MB";
	if (n >= 1024.0)                   return QString::number(n / 1024.0, 'f', 0) + " kB";
	return QString::number(n, 'f', 0) + " B";
}

static void ocProgressBegin(QWidget *parent, const QString &what) {
	if (g_ocProgress) { g_ocProgress->close(); delete g_ocProgress; g_ocProgress = nullptr; }
	g_ocCancelled = false;
	g_ocProgress = new QProgressDialog(what, "Cancel", 0, 100, parent);
	g_ocProgress->setWindowTitle("Ocean Color — downloading");
	g_ocProgress->setWindowModality(Qt::ApplicationModal);
	g_ocProgress->setMinimumWidth(420);
	g_ocProgress->setAutoClose(false);
	g_ocProgress->setAutoReset(false);
	g_ocProgress->setMinimumDuration(0);		// show at once: the wait IS the reason it exists
	g_ocProgress->setValue(0);
	g_ocProgress->show();
	QApplication::processEvents();
}

// `total <= 0` means the server never said how big the file is: fall back to a busy (indeterminate)
// bar, but still show the byte count so the user can see it climbing.
static int ocProgressSet(const QString &what, double done, double total) {
	if (!g_ocProgress) return g_ocCancelled ? 1 : 0;
	if (total > 0) {
		g_ocProgress->setRange(0, 1000);
		g_ocProgress->setValue(int(1000.0 * (done / total)));
		g_ocProgress->setLabelText(QString("%1\n%2 of %3  (%4%)")
			.arg(what, ocHumanBytes(done), ocHumanBytes(total))
			.arg(100.0 * done / total, 0, 'f', 1));
	}
	else {
		g_ocProgress->setRange(0, 0);			// size unknown -> busy bar, real byte count below it
		g_ocProgress->setLabelText(QString("%1\n%2 so far (total size unknown)")
			.arg(what, ocHumanBytes(done)));
	}
	QApplication::processEvents();
	if (g_ocProgress->wasCanceled()) g_ocCancelled = true;
	return g_ocCancelled ? 1 : 0;
}

static void ocProgressEnd() {
	if (!g_ocProgress) return;
	g_ocProgress->close();
	delete g_ocProgress;
	g_ocProgress = nullptr;
	QApplication::processEvents();
}

// Attach (or re-point) the button to the image named `owner`, which downloads `url`. Called by Julia
// right after it places an Ocean Color browse image. One button per window: a second image re-points
// the existing one rather than stacking buttons on top of each other.
static void ocAttachGridButton(Scene *s, const QString &owner, const QString &url, const QString &tip) {
	if (!s || !s->widget) return;
	s->ocBtnOwner = owner.toStdString();
	s->ocBtnUrl   = url.toStdString();
	if (!s->ocBtn) {
		s->ocBtn = new QPushButton("⭳ Download grid", s->widget);
		s->ocBtn->setCursor(Qt::PointingHandCursor);
		s->ocBtn->setStyleSheet("QPushButton { background: rgba(20,20,20,190); color: white; "
		                        "border: 1px solid #6aa9ff; border-radius: 4px; padding: 3px 10px; } "
		                        "QPushButton:hover { background: rgba(45,127,249,220); }");
		QObject::connect(s->ocBtn, &QPushButton::clicked, s->ocBtn, [s]() {
			if (!g_juliaOceanColor) {
				if (s->win) s->win->statusBar()->showMessage("Ocean Color: callback not registered", 3000);
				return;
			}
			const QByteArray p = QString("req=grid\nurl=%1\n").arg(QString::fromStdString(s->ocBtnUrl)).toUtf8();
			// No busy spinner here: Julia opens the REAL progress dialog (gmtvtk_oc_progress_*) as soon
			// as the first bytes are counted, so the user sees size and rate instead of a spinner.
			const int ok = g_juliaOceanColor(s, nullptr, p.constData());
			ocProgressEnd();			// belt and braces: never leave the bar up if Julia threw
			if (!ok && s->win)
				s->win->statusBar()->showMessage("Grid download failed — see this window's Errors console.", 6000);
		});
		// Reposition before every render, so the button rides the image instead of the window.
		vtkNew<vtkCallbackCommand> cb;
		cb->SetCallback(OcButtonCB);
		cb->SetClientData(s);
		s->ren->AddObserver(vtkCommand::StartEvent, cb);
	}
	s->ocBtn->setToolTip(tip.isEmpty() ? url : tip);
	ocPlaceGridButton(s);
}

bool OCEventFilter::eventFilter(QObject *ob, QEvent *e) {
	if (idx >= 0 && e->type() == QEvent::MouseButtonPress) {
		owner->selectTile(idx);
		return false;			// selection only — the label keeps whatever else it does with the click
	}
	// Double-click a tile = put THAT PICTURE in the window. Deliberately not what the button does: the
	// button fetches the DATA behind the tile, which needs a NASA Earthdata login, while the browse
	// image is free and instant. Two products, two actions — a user with no Earthdata account can still
	// get the map on screen (and the "Download grid" button pinned under it asks for the login later,
	// if they want the numbers after all). Qt delivers Press before DblClick, so the tile is already
	// selected here; placeImage() selects it again anyway, for callers that are not this one.
	if (idx >= 0 && e->type() == QEvent::MouseButtonDblClick) {
		owner->placeImage(idx);
		return true;
	}
	// idx == -2: the date box. ONLY Enter commits a typed or spun date — never focus loss, which is what
	// Qt calls "editing finished" and which merely opening the calendar popup causes.
	if (idx == -2 && e->type() == QEvent::KeyPress) {
		const int k = static_cast<QKeyEvent *>(e)->key();
		if (k == Qt::Key_Return || k == Qt::Key_Enter) owner->commitDate();
	}
	if (idx == -1 && e->type() == QEvent::Resize) {
		owner->rescale(0);
		owner->rescale(1);
	}
	return QObject::eventFilter(ob, e);
}

// ============================================================================================
// Contours (Grid Tools) — port of Mirone's src_figs/contouring.m. Build a list of elevations (round
// charting intervals, a start+step series, or one-off values) and Compute traces the window's grid at
// every one of them, drawing each level as its own line overlay under a single "Contours" Scene
// Objects group. Loaded at RUNTIME via QUiLoader from deps/ui/contouring.ui (plain Qt widget classes
// only, like ClipGridDialog above).
//
// The tracing is GDAL's marching squares, NOT grdcontour (~8x faster on a 3001x4801 grid) — see
// src/contours.jl. Everything that touches the grid is one g_juliaEval round-trip: _contour_zrange
// for the read-only Min/Max prefill (so the range shown and the grid traced can never diverge) and
// _on_contours for the drawing. Only Compute and the two Delete buttons run anything; the edit boxes
// and the list never compute on their own (only-action-button-executes-dialog).
//
// The Delete buttons prune the list AND, once Compute has drawn something, redraw the pruned list —
// which is Mirone's "delete removes those contours from the figure" behaviour, reached through the
// SAME _on_contours that Compute uses (Compute always redraws the WHOLE list, wiping the previous set
// first), never a second removal path.
//
// The X does NOT destroy the dialog: it hides and PARKS as a handle in the bottom strip of this
// window's Scene Objects dock, exactly like a closed X,Y plot — same Scene::parkedTools list, same
// parkTool/unparkTool pair, same row builder (50_scene.cpp), so the elevation list the user built
// survives and a double-click brings it straight back. Only the parked row's own "Delete" really
// closes it. Re-picking the menu entry re-opens the SAME dialog (g_contourDlgs), never a second one.
// ============================================================================================
class ContourDialog;
static std::map<Scene *, ContourDialog *> g_contourDlgs;   // one Contours dialog per window, alive while parked

// --- the three pieces both Grid Tools > Contours entries share -------------------------------
// "Automatic" and the Contour Tool dialog are the SAME operation with and without a dialog in front
// of it, so they go through these and never grow their own copy of the range query, the level
// guesser or the draw call.

// The z range of the grid that will be contoured, asked of the host (InteractiveGMT._contour_zrange
// prints "min/max"). Never s->zmin/zmax: the range shown and the grid traced must be one thing.
static bool contourZRange(Scene *s, double &zmin, double &zmax) {
	if (!g_juliaEval || !s) return false;
	const QString cmd = QString("InteractiveGMT._contour_zrange(Ptr{Cvoid}(UInt(%1)))")
		.arg((qulonglong)reinterpret_cast<uintptr_t>(s));
	static std::vector<char> buf(1 << 12);
	int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
	if (n <= 0) return false;
	const QStringList mm = QString::fromUtf8(buf.data(), n).trimmed().split('/');
	if (mm.size() != 2) return false;
	bool o1 = false, o2 = false;
	const double a = mm[0].toDouble(&o1), b = mm[1].toDouble(&o2);
	if (!o1 || !o2) return false;
	zmin = a;  zmax = b;
	return true;
}

// Round charting levels across [lo,hi] — contouring.m's push_GuessIntervals_CB (which asked MATLAB's
// contourc to guess levels off a synthetic ramp): 1/2/5·10^k steps giving ~12 intervals, snapped to
// multiples of the step so the values read like chart depths. THE level guesser: "Automatic" and the
// dialog's "Add Common Charting Intervals" are the same button, one of them without the dialog.
static QVector<double> contourNiceLevels(double lo, double hi) {
	QVector<double> v;
	if (!(hi > lo)) return v;
	const double raw = (hi - lo) / 12.0;
	const double mag = std::pow(10.0, std::floor(std::log10(raw)));
	const double n   = raw / mag;
	double step = (n <= 1.5) ? 1.0 : (n <= 3.0) ? 2.0 : (n <= 7.0) ? 5.0 : 10.0;
	step *= mag;
	const long long k0 = (long long)std::ceil(lo / step);
	const long long k1 = (long long)std::floor(hi / step);
	for (long long k = k0; k <= k1 && v.size() < 500; ++k) v.push_back(k * step);
	return v;
}

// Trace and draw `levels`. An EMPTY list clears the window's contours. Returns false and reports on
// failure (Errors tab + a message box on `parent`).
static bool contourDrawLevels(Scene *s, QWidget *parent, const QString &minPts, bool labels,
                              const QVector<double> &levels) {
	if (!g_juliaEval) {
		QMessageBox::warning(parent, "Contours", "This computation needs the Julia/GMT host.");
		return false;
	}
	QStringList ls;
	for (double x : levels) ls << QString::number(x, 'g', 12);
	const QString params = QString("%1;%2;%3").arg(minPts).arg(labels ? "1" : "0").arg(ls.join(','));
	const QString cmd = QString("InteractiveGMT._on_contours(Ptr{Cvoid}(UInt(%1)),\"%2\")")
		.arg((qulonglong)reinterpret_cast<uintptr_t>(s)).arg(params);
	showBusyDialog("Tracing contours…");
	static std::vector<char> buf(1 << 14);
	int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
	closeBusyDialog();
	if (n < 0) {
		const QString msg = QString::fromUtf8(buf.data(), -n);
		sceneLogError(s, msg);
		QMessageBox::warning(parent, "Contours", msg);
		return false;
	}
	return true;
}

class ContourDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QListWidget *list = nullptr;
	QLineEdit *zminEdit, *zmaxEdit, *minPtsEdit, *singleEdit, *startEdit, *stepEdit;
	QCheckBox *labelsChk = nullptr;
	double zmin = 0.0, zmax = 0.0;
	bool haveRange = false;    // Julia answered with the grid's z range
	bool applied   = false;    // Compute has drawn at least once -> Delete must refresh the scene
	bool reallyClose = false;  // set by the parked row's "Delete": let the next close through

	// Bring the dialog back from the dock (double-click, the row's checkbox, its "Show" item). ONE
	// function for every way back in, like xyUnpark.
	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();
		dlg->raise();
		dlg->activateWindow();
	}

	// The parked row's menu — properties button and context menu are the same lambda, never two.
	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel  = m.addAction("Delete");
			QAction *pick  = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scn, dlg);
				dlg->deleteLater();          // destroyed -> the row and this object go with it
			}
		};
	}

	// Drop the registry entry by VALUE, not by `scn`: when the owning viewer window is torn down the
	// dialog dies with it and the Scene may already be gone, so the key cannot be trusted.
	~ContourDialog() {
		for (auto it = g_contourDlgs.begin(); it != g_contourDlgs.end(); )
			it = (it->second == this) ? g_contourDlgs.erase(it) : std::next(it);
	}

	QVector<double> levels() const {
		QVector<double> v;
		if (!list) return v;
		for (int i = 0; i < list->count(); ++i) {
			bool ok = false;
			const double x = list->item(i)->text().toDouble(&ok);
			if (ok) v.push_back(x);
		}
		return v;
	}

	// Replace the list with `v`, sorted ascending and de-duplicated (contouring.m sorts on every Add).
	void setLevels(QVector<double> v) {
		if (!list) return;
		std::sort(v.begin(), v.end());
		v.erase(std::unique(v.begin(), v.end()), v.end());
		list->clear();
		for (double x : v) list->addItem(QString::number(x, 'g', 10));
	}

	// The ONE call that draws. `params` = "minpts;labels;lev1,lev2,…"; an empty level list just
	// clears the window's contours.
	void redraw(bool announceEmpty) {
		const QVector<double> v = levels();
		if (v.isEmpty() && announceEmpty) { QMessageBox::warning(dlg, "Contours", "The elevation list is empty — nothing to contour."); return; }
		if (!contourDrawLevels(scn, dlg, minPtsEdit ? minPtsEdit->text().trimmed() : QString("0"),
		                       labelsChk && labelsChk->isChecked(), v))
			return;
		applied = !v.isEmpty();
	}

	explicit ContourDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/contouring.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("ContourDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("ContourDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		dlg->setWindowTitle("Contours");
		QDialog *d = dlg;
		g_contourDlgs[scn] = this;

		// The X PARKS the dialog instead of destroying it — same contract as a closed X,Y plot, and the
		// same Scene::parkedTools machinery. WA_DeleteOnClose is deliberately NOT set: the dialog (and
		// the elevation list in it) has to survive being closed. Only the parked row's "Delete" sets
		// reallyClose and lets a close through.
		struct CloseParks : QObject {
			ContourDialog *cd;
			CloseParks(QObject *parent, ContourDialog *c) : QObject(parent), cd(c) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close && cd && !cd->reallyClose && sceneAlive(cd->scn)) {
					e->ignore();
					cd->dlg->hide();
					ContourDialog *c = cd;       // a lambda cannot capture a member of the enclosing class
					parkTool(c->scn, c->dlg, "Contours", IC_Line,
					         "Closed Contours dialog — double-click to bring it back, click for Show / Delete",
					         [c]() { c->unpark(); }, c->parkedMenu());
					// A handle the user cannot see is the same as no handle at all: reveal + unfold.
					unfoldSceneObjects(cd->scn);
					return true;
				}
				return QObject::eventFilter(o, e);
			}
		};
		dlg->installEventFilter(new CloseParks(dlg, this));

		list       = d->findChild<QListWidget *>("listbox_ElevValues");
		zminEdit   = d->findChild<QLineEdit *>("edit_Zmin");
		zmaxEdit   = d->findChild<QLineEdit *>("edit_Zmax");
		minPtsEdit = d->findChild<QLineEdit *>("edit_minNpts");
		singleEdit = d->findChild<QLineEdit *>("edit_SingleElev");
		startEdit  = d->findChild<QLineEdit *>("edit_StartElev");
		stepEdit   = d->findChild<QLineEdit *>("edit_ElevStep");
		labelsChk  = d->findChild<QCheckBox *>("check_plotLabels");
		auto *guessBtn  = d->findChild<QPushButton *>("push_GuessIntervals");
		auto *addBtn    = d->findChild<QPushButton *>("push_Add");
		auto *genBtn    = d->findChild<QPushButton *>("push_GenerateIntervals");
		auto *delSelBtn = d->findChild<QPushButton *>("push_DeleteSelected");
		auto *delAllBtn = d->findChild<QPushButton *>("push_DeleteAll");
		auto *applyBtn  = d->findChild<QPushButton *>("push_Apply");

		// Min/Max come from the grid Julia will actually contour — one source, so the boxes cannot
		// describe a different layer than the trace does.
		haveRange = contourZRange(scn, zmin, zmax);
		if (haveRange) {
			if (zminEdit) zminEdit->setText(QString::number(zmin, 'g', 8));
			if (zmaxEdit) zmaxEdit->setText(QString::number(zmax, 'g', 8));
		}

		if (guessBtn) QObject::connect(guessBtn, &QPushButton::clicked, d, [this, d]() {
			if (!haveRange) { QMessageBox::warning(d, "Contours", "The grid's elevation range is unknown."); return; }
			setLevels(contourNiceLevels(zmin, zmax));      // the SAME guesser "Automatic" uses
		});

		// Add: one value onto the list (contouring.m push_Add_CB — sorts, then clears the box).
		if (addBtn) QObject::connect(addBtn, &QPushButton::clicked, d, [this, d]() {
			bool ok = false;
			const double x = singleEdit ? singleEdit->text().trimmed().toDouble(&ok) : 0.0;
			if (!ok) { QMessageBox::warning(d, "Contours", "Give me a numeric Single elevation."); return; }
			QVector<double> v = levels();
			v.push_back(x);
			setLevels(v);
			if (singleEdit) singleEdit->clear();
		});

		// Generate: start:step:max for a positive step, start:step:min for a negative one — exactly
		// contouring.m push_GenerateIntervals_CB, including its "Generate how? From the empty outer
		// space?" complaint when either box is blank.
		if (genBtn) QObject::connect(genBtn, &QPushButton::clicked, d, [this, d]() {
			bool ok1 = false, ok2 = false;
			const double s0 = startEdit ? startEdit->text().trimmed().toDouble(&ok1) : 0.0;
			const double ds = stepEdit  ? stepEdit->text().trimmed().toDouble(&ok2)  : 0.0;
			if (!ok1 || !ok2 || ds == 0.0) { QMessageBox::warning(d, "Contours", "Generate how? Give a Starting Elevation and a non-zero Elevation Step."); return; }
			if (!haveRange)                { QMessageBox::warning(d, "Contours", "The grid's elevation range is unknown."); return; }
			QVector<double> v;
			const double stop = (ds > 0) ? zmax : zmin;
			for (int k = 0; k < 5000; ++k) {
				const double x = s0 + k * ds;
				if (ds > 0 ? (x > stop) : (x < stop)) break;
				v.push_back(x);
			}
			setLevels(v);
		});

		if (delSelBtn) QObject::connect(delSelBtn, &QPushButton::clicked, d, [this]() {
			if (!list) return;
			const QList<QListWidgetItem *> sel = list->selectedItems();
			if (sel.isEmpty()) return;
			for (QListWidgetItem *it : sel) delete list->takeItem(list->row(it));
			if (applied) redraw(false);       // those contours leave the window too (contouring.m)
		});

		if (delAllBtn) QObject::connect(delAllBtn, &QPushButton::clicked, d, [this]() {
			if (!list) return;
			list->clear();
			if (applied) { redraw(false); applied = false; }
		});

		if (applyBtn) QObject::connect(applyBtn, &QPushButton::clicked, d, [this]() { redraw(true); });

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}
};

// ============================================================================================
// Terrain Modeling (Grid Tools) — port of Mirone's src_figs/multiscale.m, the chooser in front of
// the mirblock.c MEX (ported in src/multiscale.jl). multiscale.m is a MODAL one-shot that returns
// {method, size} and nothing else, so this is a plain function rather than a dialog class: load
// deps/ui/multiscale.ui, exec(), read the two widgets back.
//
// The combo's order IS mirblock's -A<n>, which is why the .m shouts that the two must agree; the
// authoritative list lives in Julia (MIRBLOCK_METHODS, multiscale.jl) and the .ui repeats it, so
// only the INDEX travels. The window size must be odd (the .m pops an errordlg and bumps it by one);
// a QSpinBox stepping by 2 from 3 makes that unreachable, and the round-up below covers a typed-in
// even number.
static bool multiscaleAsk(QWidget *parent, int &method, int &nWin) {
	QUiLoader loader;
	QFile f(gmtvtkUiDir() + "/multiscale.ui");
	if (!f.open(QFile::ReadOnly)) {
		qWarning("multiscaleAsk: cannot open %s", qUtf8Printable(f.fileName()));
		return false;
	}
	QDialog *dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
	f.close();
	if (!dlg) { qWarning("multiscaleAsk: QUiLoader failed to load the .ui"); return false; }
	dlg->setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
	auto *cb = dlg->findChild<QComboBox *>("cb_method");
	auto *sb = dlg->findChild<QSpinBox *>("sb_win");
	if (auto *bb = dlg->findChild<QDialogButtonBox *>("buttonBox")) {
		QObject::connect(bb, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
		QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
	}
	if (cb) cb->setCurrentIndex(method);
	if (sb) sb->setValue(nWin);
	const bool ok = (dlg->exec() == QDialog::Accepted);
	if (ok) {
		if (cb) method = cb->currentIndex();
		if (sb) nWin = sb->value();
		if (nWin < 3)  nWin = 3;
		if (nWin % 2 == 0) nWin++;                  // multiscale.m's "Fixing it"
	}
	delete dlg;
	return ok;
}

// ============================================================================================
// Grid calculator (Grid Tools) — port of Mirone's src_figs/grid_calculator.m. An expression box on
// top, the list of usable grids below it, a digit/operator keypad and a function keypad on the right.
// Loaded at RUNTIME via QUiLoader from deps/ui/grid_calculator.ui (plain Qt widget classes only,
// like ClipGridDialog above).
//
// WHICH grids the list shows: every grid of THIS window's Scene Objects whose limits AND increments
// match the window's base grid exactly — anything else cannot be combined node-by-node, so it is not
// offered. The list is built Julia-side (_gridcalc_names, gridcalc.jl), where the grids live.
// "Load Grid" appends a grid from disk; like Mirone it only stores the name (the file is read at
// Compute time, and only then checked against the same limits/increments).
//
// A double-click on a list row pushes that grid into the expression box by NAME. Unlike Mirone, no
// `&` prefix is needed: Julia substitutes the names textually before the expression is parsed, so a
// bare label works even with blanks in it (`&name` is still accepted there, and is required only for
// a label that collides with a function name — see gridToken below). Compute hands the expression +
// the loaded-file paths to Julia (_on_gridcalc), which evaluates it element-wise and adds the result
// as a NEW derived grid (SACRED_LAW derived-variable display law).
// ============================================================================================
class GridCalculatorDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QPlainTextEdit *cmdEdit = nullptr;
	QListWidget *listBox = nullptr;

	explicit GridCalculatorDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/grid_calculator.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("GridCalculatorDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("GridCalculatorDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		dlg->setWindowTitle("Grid calculator");
		QDialog *d = dlg;

		cmdEdit = d->findChild<QPlainTextEdit *>("edit_command");
		listBox = d->findChild<QListWidget *>("listbox_inArrays");

		// Append text at the end of the expression box and keep the caret there.
		auto append = [this](const QString &t) {
			if (!cmdEdit) return;
			cmdEdit->moveCursor(QTextCursor::End);
			cmdEdit->insertPlainText(t);
			cmdEdit->setFocus();
		};

		// One loop wires every keypad button: the objectName prefix says what kind of token it is, so
		// adding a button to the .ui needs no C++ change as long as it keeps the naming convention.
		// Digits/operators/parens are inserted verbatim (spaced, for operators); a function button
		// inserts "name(" built from its objectName suffix, NOT from its label, so a prettified label
		// ("log e") can never leak into the expression.
		for (QPushButton *b : d->findChildren<QPushButton *>()) {
			const QString nm = b->objectName();
			b->setAutoDefault(false);  b->setDefault(false);   // Enter in the expression box types a newline
			b->setFocusPolicy(Qt::NoFocus);                    // …and never clicks a keypad button
			if (nm.startsWith("push_digit_"))
				QObject::connect(b, &QPushButton::clicked, d, [append, b]() { append(b->text()); });
			else if (nm.startsWith("push_op_"))
				QObject::connect(b, &QPushButton::clicked, d, [append, b]() { append(" " + b->text() + " "); });
			else if (nm.startsWith("push_fun_")) {
				const QString fn = nm.mid(9);
				QObject::connect(b, &QPushButton::clicked, d, [append, fn]() { append(" " + fn + "("); });
			}
			else if (nm == "push_par_l") QObject::connect(b, &QPushButton::clicked, d, [append]() { append("("); });
			else if (nm == "push_par_r") QObject::connect(b, &QPushButton::clicked, d, [append]() { append(")"); });
			else if (nm == "push_clear") QObject::connect(b, &QPushButton::clicked, d, [this]() { if (cmdEdit) cmdEdit->clear(); });
		}

		// Double-click pushes the grid's name into the expression (Mirone listbox_inArrays_CB): an empty
		// box (or one holding a single bare name) is REPLACED, otherwise the token is appended.
		if (listBox) QObject::connect(listBox, &QListWidget::itemDoubleClicked, d, [this](QListWidgetItem *it) {
			if (!it || !cmdEdit) return;
			const QString tok = gridToken(it->text());
			const QString cur = cmdEdit->toPlainText().trimmed();
			// A box holding nothing but ONE grid name is REPLACED (picking again = changing your mind);
			// anything else gets the name appended.
			bool loneName = cur.isEmpty();
			for (int i = 0; !loneName && listBox && i < listBox->count(); ++i)
				loneName = (cur == gridToken(listBox->item(i)->text()));
			if (loneName)
				cmdEdit->setPlainText(tok);
			else {
				cmdEdit->moveCursor(QTextCursor::End);
				cmdEdit->insertPlainText(" " + tok);
			}
			cmdEdit->setFocus();
		});

		// "Load Grid" does NOT read the file (Mirone push_loadGrid_CB): it only remembers the name here
		// and the full path on the item, so Compute can read it then.
		if (auto *loadBtn = d->findChild<QPushButton *>("push_loadGrid"))
			QObject::connect(loadBtn, &QPushButton::clicked, d, [this, d]() {
				const QString fn = QFileDialog::getOpenFileName(d, "Select grid", prefStartDir(),
					"Grids (*.grd *.nc *.tif *.tiff *.img);;All files (*)");
				if (fn.isEmpty() || !listBox) return;
				rememberStartDir(fn);
				auto *it = new QListWidgetItem(QFileInfo(fn).fileName(), listBox);
				it->setData(Qt::UserRole, fn);
				it->setToolTip(fn);
				tightenListRows(listBox);
			});

		auto compute = [this, d]() { runCompute(d); };
		if (auto *b = d->findChild<QPushButton *>("push_compute")) QObject::connect(b, &QPushButton::clicked, d, compute);
		if (auto *b = d->findChild<QPushButton *>("push_equal"))   QObject::connect(b, &QPushButton::clicked, d, compute);
		if (auto *b = d->findChild<QPushButton *>("push_close"))   QObject::connect(b, &QPushButton::clicked, d, [d]() { d->close(); });

		addManualButton(d, "grdmath");     // the green ? disk, lower-left as in every other dialog
		fillList();

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}

	// The Scene Objects label of this window's base grid — the name Julia knows it by is "" (it is the
	// primary), so the dialog tells Julia which visible label stands for it.
	QString baseName() const {
		if (!scn) return QString();
		return scn->surfName.empty() ? QString("Surface") : QString::fromStdString(scn->surfName);
	}

	// What a double-click writes into the expression. Grid names are substituted TEXTUALLY on the Julia
	// side before the expression is parsed, so the BARE label is enough — blanks included. Mirone's
	// `&name` is still accepted there, and is what a label that collides with a function name (a grid
	// called "abs") must use, since the bare form would otherwise eat the function call.
	static QString gridToken(const QString &name) {
		static const QStringList funcs = {"sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh",
		                                  "tanh", "exp", "log", "log2", "log10", "sqrt", "abs", "sign",
		                                  "floor", "ceil", "round", "min", "max", "pi", "e"};
		return funcs.contains(name) ? "&" + name : name;
	}

	// Ask Julia which grids of this window are usable (same limits AND increments as the base grid);
	// it prints one name per line. Grids the user added with "Load Grid" are kept (they carry a path).
	void fillList() {
		if (!listBox) return;
		QList<QListWidgetItem *> loaded;
		for (int i = listBox->count() - 1; i >= 0; --i)
			if (!listBox->item(i)->data(Qt::UserRole).toString().isEmpty()) loaded.prepend(listBox->takeItem(i));
		listBox->clear();
		if (g_juliaEval && scn) {
			const QString cmd = QString("InteractiveGMT._gridcalc_names(Ptr{Cvoid}(UInt(%1)),raw\"%2\")")
				.arg((qulonglong)reinterpret_cast<uintptr_t>(scn)).arg(baseName());
			std::vector<char> buf(1 << 14);
			int n = g_juliaEval(scn, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
			if (n > 0) {
				const QString out = QString::fromUtf8(buf.data(), n);
				for (const QString &nm : out.split('\n', Qt::SkipEmptyParts))
					if (!nm.trimmed().isEmpty()) new QListWidgetItem(nm.trimmed(), listBox);
			}
		}
		for (QListWidgetItem *it : loaded) listBox->addItem(it);
		tightenListRows(listBox);
	}

	// Compute: the ONE action that evaluates. Hands the expression and the paths of the "Load Grid"
	// entries to Julia; the dialog stays open so the next expression needs no reopening.
	void runCompute(QDialog *d) {
		if (!cmdEdit) return;
		const QString expr = cmdEdit->toPlainText().simplified();
		if (expr.isEmpty()) { QMessageBox::warning(d, "Grid calculator", "Type an expression first."); return; }
		if (!g_juliaGridCalc) {
			QMessageBox::warning(d, "Grid calculator", "Grid calculator: callback not registered (rebuild/restart needed?).");
			return;
		}
		QStringList kv;
		kv << "expr=" + expr;
		kv << "base=" + baseName();
		if (listBox) {
			int k = 0;
			for (int i = 0; i < listBox->count(); ++i) {
				const QString p = listBox->item(i)->data(Qt::UserRole).toString();
				if (!p.isEmpty()) kv << QString("file%1=%2").arg(++k).arg(p);
			}
		}
		showBusyDialog("Computing…");
		const int ok = g_juliaGridCalc(scn, kv.join("\n").toUtf8().constData());
		closeBusyDialog();
		if (ok) fillList();      // the result is a new Scene Objects grid -> offer it to the next expression
		else    QMessageBox::warning(d, "Grid calculator",
		                             "Compute failed — see this window's Errors console for details.");
	}
};

// ============================================================================================
// grdtrend (GMT menu) — fit a low-order polynomial trend to the window's grid. Layout is Mirone's
// grdtrend window (src_figs/grdtrend_mir.m): the What-to-compute radios, the model-parameter combo,
// Robust Fit and Protect NaNs — plus the options GMT grew since Mirone's port: the 1-D +x/+y fits
// (model 1-4 there, 1-10 for a surface), -R, and the -W input weight grid with its +s modifier.
// Loaded at RUNTIME via QUiLoader from deps/ui/grdtrend_dialog.ui.
//
// "Weights" is only produced BY the robust fit (GMT writes the weights it used into the -W file), so
// that radio forces Robust Fit on. "Protect NaNs" only bites on the trend surface — a polynomial
// covers the input's holes too, and Mirone puts them back — so it greys out for the other two.
// ============================================================================================
class GrdTrendDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QComboBox *modelCb = nullptr, *axisCb = nullptr;
	QCheckBox *robustChk = nullptr, *nanChk = nullptr, *sigmaChk = nullptr;
	QRadioButton *rbTrend = nullptr, *rbResid = nullptr, *rbWeights = nullptr;
	QLineEdit *wEdit = nullptr, *outEdit = nullptr;
	QLineEdit *xmin = nullptr, *xmax = nullptr, *ymin = nullptr, *ymax = nullptr;

	explicit GrdTrendDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/grdtrend_dialog.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("GrdTrendDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("GrdTrendDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		dlg->setWindowTitle("grdtrend");
		QDialog *d = dlg;

		modelCb   = d->findChild<QComboBox *>("cb_model");
		axisCb    = d->findChild<QComboBox *>("cb_axis");
		robustChk = d->findChild<QCheckBox *>("chk_robust");
		nanChk    = d->findChild<QCheckBox *>("chk_protectNaNs");
		sigmaChk  = d->findChild<QCheckBox *>("chk_sigma");
		rbTrend   = d->findChild<QRadioButton *>("rb_trend");
		rbResid   = d->findChild<QRadioButton *>("rb_residuals");
		rbWeights = d->findChild<QRadioButton *>("rb_weights");
		wEdit     = d->findChild<QLineEdit *>("edit_wfile");
		outEdit   = d->findChild<QLineEdit *>("edit_outfile");
		xmin = d->findChild<QLineEdit *>("edit_xmin");  xmax = d->findChild<QLineEdit *>("edit_xmax");
		ymin = d->findChild<QLineEdit *>("edit_ymin");  ymax = d->findChild<QLineEdit *>("edit_ymax");

		if (axisCb) {
			axisCb->addItem("x and y (surface)", "");
			axisCb->addItem("x only", "x");
			axisCb->addItem("y only", "y");
		}
		fillModelCombo(3);
		// A 1-D fit only has 4 terms (m1 + m2*t + m3*t^2 + m4*t^3); a surface has 10. Refill on change,
		// keeping the current choice when it still fits.
		if (axisCb) QObject::connect(axisCb, QOverload<int>::of(&QComboBox::currentIndexChanged), d,
			[this]() { fillModelCombo(modelCb ? modelCb->currentText().toInt() : 3); });

		// Region + increment-less "OR Ref grid" row, the standing rule for every Region group.
		if (auto *rg = d->findChild<QGridLayout *>("gridLayout_region"))
			addRefGridRow(d, rg, xmin, xmax, ymin, ymax);
		// …prefilled from the window's own grid, also the standing rule.
		if (scene && scene->gnx > 1 && scene->gny > 1) {
			if (xmin) xmin->setText(QString::number(scene->gx0, 'g', 12));
			if (xmax) xmax->setText(QString::number(scene->gx1, 'g', 12));
			if (ymin) ymin->setText(QString::number(scene->gy0, 'g', 12));
			if (ymax) ymax->setText(QString::number(scene->gy1, 'g', 12));
		}

		auto *wBtn = d->findChild<QToolButton *>("btn_wfile");
		if (wBtn && wEdit) {
			QObject::connect(wBtn, &QToolButton::clicked, d, [this, d]() {
				QString p = QFileDialog::getOpenFileName(d, "Select weight grid", prefStartDir(),
					"Grids (*.grd *.nc);;All files (*)");
				if (!p.isEmpty()) { wEdit->setText(p); rememberStartDir(p); }
			});
			fileBoxDoubleClick(wEdit, wBtn);      // double-click in the box opens the chooser
		}
		auto *oBtn = d->findChild<QToolButton *>("btn_outfile");
		if (oBtn && outEdit) {
			QObject::connect(oBtn, &QToolButton::clicked, d, [this, d]() {
				QString p = QFileDialog::getSaveFileName(d, "Save result grid", prefStartDir(),
					"Grids (*.grd *.nc);;All files (*)");
				if (!p.isEmpty()) { outEdit->setText(p); rememberStartDir(p); }
			});
			fileBoxDoubleClick(outEdit, oBtn);
		}

		// The weights ARE the robust fit's by-product: picking them turns Robust Fit on and locks it.
		// Protect NaNs only applies to the trend surface.
		auto syncWhat = [this]() {
			const bool wantW = rbWeights && rbWeights->isChecked();
			if (robustChk) {
				if (wantW && !robustChk->isChecked()) robustChk->setChecked(true);
				robustChk->setEnabled(!wantW);
			}
			if (nanChk) nanChk->setEnabled(rbTrend && rbTrend->isChecked());
		};
		for (QRadioButton *rb : { rbTrend, rbResid, rbWeights })
			if (rb) QObject::connect(rb, &QRadioButton::toggled, d, [syncWhat](bool) { syncWhat(); });
		syncWhat();

		for (QPushButton *b : d->findChildren<QPushButton *>()) { b->setAutoDefault(false); b->setDefault(false); }
		if (auto *b = d->findChild<QPushButton *>("push_compute")) QObject::connect(b, &QPushButton::clicked, d, [this, d]() { runCompute(d); });
		if (auto *b = d->findChild<QPushButton *>("push_close"))   QObject::connect(b, &QPushButton::clicked, d, [d]() { d->close(); });
		addManualButton(d, "grdtrend");            // the green ? disk, lower-left as everywhere else

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}

	// 1..10 for a surface fit, 1..4 for a 1-D (+x / +y) one — the limits the module itself enforces.
	void fillModelCombo(int keep) {
		if (!modelCb) return;
		const bool oneD = axisCb && !axisCb->currentData().toString().isEmpty();
		const int nmax = oneD ? 4 : 10;
		QSignalBlocker block(modelCb);
		modelCb->clear();
		for (int k = 1; k <= nmax; ++k) modelCb->addItem(QString::number(k));
		modelCb->setCurrentIndex(qBound(0, (keep > 0 ? keep : 3) - 1, nmax - 1));
	}

	void runCompute(QDialog *d) {
		if (!g_juliaGrdTrend) {
			QMessageBox::warning(d, "grdtrend", "grdtrend: callback not registered (rebuild/restart needed?).");
			return;
		}
		const QString what = (rbResid && rbResid->isChecked())     ? "diff"
		                   : (rbWeights && rbWeights->isChecked()) ? "weights" : "trend";
		QStringList kv;
		kv << "what=" + what;
		kv << QString("model=%1").arg(modelCb ? modelCb->currentText() : "3");
		kv << QString("robust=%1").arg(robustChk && robustChk->isChecked() ? 1 : 0);
		kv << QString("protectnans=%1").arg(nanChk && nanChk->isChecked() ? 1 : 0);
		kv << "axis=" + (axisCb ? axisCb->currentData().toString() : QString());
		// -R only when all four boxes are filled, and only when they are not simply the grid's own
		// limits (an unnarrowed region is not worth passing).
		if (xmin && xmax && ymin && ymax && !xmin->text().trimmed().isEmpty() && !xmax->text().trimmed().isEmpty()
		    && !ymin->text().trimmed().isEmpty() && !ymax->text().trimmed().isEmpty())
			kv << QString("region=%1/%2/%3/%4").arg(xmin->text().trimmed()).arg(xmax->text().trimmed())
			                                   .arg(ymin->text().trimmed()).arg(ymax->text().trimmed());
		QString wf = wEdit ? wEdit->text().trimmed() : QString();
		if (!wf.isEmpty()) {
			if (sigmaChk && sigmaChk->isChecked()) wf += "+s";
			kv << "wfile=" + wf;
		}
		if (outEdit && !outEdit->text().trimmed().isEmpty()) kv << "outfile=" + outEdit->text().trimmed();
		kv << "grid=" + QString::fromStdString(activeGridName(scn));   // fit the DISPLAYED layer
		showBusyDialog("Fitting trend…");
		const int ok = g_juliaGrdTrend(scn, kv.join("\n").toUtf8().constData());
		closeBusyDialog();
		if (!ok) QMessageBox::warning(d, "grdtrend",
		                              "grdtrend failed — see this window's Errors console for details.");
	}
};

// ============================================================================================
// grdlandmask (GMT menu) — build a wet/dry mask grid from the shoreline database. Layout is Mirone's
// grdlandmask window: the shared "Griding Line Geometry" block, coastline resolution, Min area (-A),
// registration, and the five Node values with the Boundary flag. The .ui carries a verbatim copy of
// deps/ui/grid_line_geometry.ui, and GeoGridGeometry::adopt() takes those widgets over so the
// geometry block behaves EXACTLY as grdsample's (one implementation, not a re-wiring).
//
// Beyond Mirone's template, from grdlandmask.qmd: "auto" resolution, the -E border VALUES (line
// tracing: one value, or cborder/lborder/iborder/pborder), and the module's second method — hand it
// a grid and it masks THAT grid instead of producing a bare mask ("Apply the mask to this window's
// grid"). Mirone's "Force float" is not carried over: GMT.jl hands back a Float32 grid either way,
// so the checkbox would toggle nothing.
// ============================================================================================
class GrdLandmaskDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	GeoGridGeometry *geo = nullptr;        // the adopted .ui block (region + spacing + Ref grid)
	QComboBox *resCb = nullptr, *lvMinCb = nullptr, *lvMaxCb = nullptr;
	QLineEdit *areaEdit = nullptr, *borderEdit = nullptr, *outEdit = nullptr;
	QLineEdit *nodeEdit[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
	QCheckBox *gridRegChk = nullptr, *verboseChk = nullptr, *boundaryChk = nullptr, *clipChk = nullptr;

	explicit GrdLandmaskDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/grdlandmask_dialog.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("GrdLandmaskDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("GrdLandmaskDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		dlg->setWindowTitle("grdlandmask");
		QDialog *d = dlg;

		geo = GeoGridGeometry::adopt(d);       // the SAME block grdsample uses, wiring and all
		resCb   = d->findChild<QComboBox *>("cb_res");
		lvMinCb = d->findChild<QComboBox *>("cb_levelMin");
		lvMaxCb = d->findChild<QComboBox *>("cb_levelMax");
		areaEdit   = d->findChild<QLineEdit *>("edit_area");
		borderEdit = d->findChild<QLineEdit *>("edit_border");
		outEdit    = d->findChild<QLineEdit *>("edit_outfile");
		for (int k = 0; k < 5; ++k) nodeEdit[k] = d->findChild<QLineEdit *>(QString("edit_n%1").arg(k + 1));
		gridRegChk  = d->findChild<QCheckBox *>("chk_gridReg");
		verboseChk  = d->findChild<QCheckBox *>("chk_verbose");
		boundaryChk = d->findChild<QCheckBox *>("chk_boundary");
		clipChk     = d->findChild<QCheckBox *>("chk_clip");

		if (resCb) {
			resCb->addItem("crude", "c");         resCb->addItem("low", "l");
			resCb->addItem("intermediate", "i");  resCb->addItem("high", "h");
			resCb->addItem("full", "f");          resCb->addItem("auto", "a");   // -Da, newer than Mirone
			resCb->setCurrentIndex(2);            // intermediate, as in Mirone
		}
		for (QComboBox *cb : { lvMinCb, lvMaxCb })
			if (cb) for (int k = 0; k <= 4; ++k) cb->addItem(QString::number(k));
		if (lvMaxCb) lvMaxCb->setCurrentIndex(4);

		// Prefill the geometry from the window's own grid — the standing rule for every region spec.
		if (geo && scene) {
			if (scene->gnx > 1 && scene->gny > 1)
				geo->fillGeometry(QString("%1/%2/%3/%4/%5/%6/%7/%8")
					.arg(scene->gx0).arg(scene->gx1).arg(scene->gy0).arg(scene->gy1)
					.arg(scene->gdx).arg(scene->gdy).arg(scene->gnx).arg(scene->gny));
			else if (scene->x1 > scene->x0 && scene->y1 > scene->y0)
				geo->fillGeometry(QString("%1/%2/%3/%4////").arg(scene->x0).arg(scene->x1)
					.arg(scene->y0).arg(scene->y1));
		}
		if (gridRegChk && geo)
			QObject::connect(gridRegChk, &QCheckBox::toggled, d,
			                 [this](bool on) { if (geo) geo->setRegistration(!on); });

		// Border VALUES only mean something with line tracing on.
		if (boundaryChk && borderEdit) {
			borderEdit->setEnabled(boundaryChk->isChecked());
			QObject::connect(boundaryChk, &QCheckBox::toggled, d,
			                 [this](bool on) { if (borderEdit) borderEdit->setEnabled(on); });
		}
		// Masking the window's grid means the geometry IS that grid's — the module takes it from the
		// grid itself, so the boxes stop being an input.
		if (clipChk) QObject::connect(clipChk, &QCheckBox::toggled, d, [this, d](bool on) {
			if (auto *gg = d->findChild<QGroupBox *>("geoGroup")) gg->setEnabled(!on);
			if (auto *re = d->findChild<QLineEdit *>("refEdit")) re->setEnabled(!on);
			if (auto *rb = d->findChild<QToolButton *>("refBtn")) rb->setEnabled(!on);
		});

		if (auto *oBtn = d->findChild<QToolButton *>("btn_outfile")) {
			QObject::connect(oBtn, &QToolButton::clicked, d, [this, d]() {
				QString p = QFileDialog::getSaveFileName(d, "Save mask grid", prefStartDir(),
					"Grids (*.grd *.nc);;All files (*)");
				if (!p.isEmpty()) { outEdit->setText(p); rememberStartDir(p); }
			});
			if (outEdit) fileBoxDoubleClick(outEdit, oBtn);
		}

		for (QPushButton *b : d->findChildren<QPushButton *>()) { b->setAutoDefault(false); b->setDefault(false); }
		if (auto *b = d->findChild<QPushButton *>("push_compute")) QObject::connect(b, &QPushButton::clicked, d, [this, d]() { runCompute(d); });
		if (auto *b = d->findChild<QPushButton *>("push_close"))   QObject::connect(b, &QPushButton::clicked, d, [d]() { d->close(); });
		addManualButton(d, "grdlandmask");         // the green ? disk, lower-left as everywhere else

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}

	void runCompute(QDialog *d) {
		if (!g_juliaGrdLandmask) {
			QMessageBox::warning(d, "grdlandmask", "grdlandmask: callback not registered (rebuild/restart needed?).");
			return;
		}
		const bool clip = clipChk && clipChk->isChecked();
		QStringList kv;
		if (!clip && geo) {                        // the mask's own region/spacing (ignored when masking a grid)
			kv << "region=" + geo->region();
			kv << "inc=" + geo->inc();
		}
		kv << QString("clip=%1").arg(clip ? 1 : 0);
		kv << "res=" + (resCb ? resCb->currentData().toString() : QString("i"));
		// -A: area[/min_level/max_level]. Levels travel only when they are not the full 0-4 default.
		QString area = areaEdit ? areaEdit->text().trimmed() : QString();
		if (!area.isEmpty() && area != "0") {
			const QString lo = lvMinCb ? lvMinCb->currentText() : QString("0");
			const QString hi = lvMaxCb ? lvMaxCb->currentText() : QString("4");
			if (lo != "0" || hi != "4") area += "/" + lo + "/" + hi;
			kv << "area=" + area;
		}
		// -N: the five node values, trimmed to the two-value form when the last three are the default.
		QStringList nv;
		for (int k = 0; k < 5; ++k) nv << (nodeEdit[k] ? nodeEdit[k]->text().trimmed() : QString());
		if (!nv.contains(QString())) {
			if (nv[2] == "0" && nv[3] == "1" && nv[4] == "0") nv = QStringList{ nv[0], nv[1] };
			kv << "maskvalues=" + nv.join('/');
		}
		if (boundaryChk && boundaryChk->isChecked())
			kv << "border=" + (borderEdit ? borderEdit->text().trimmed() : QString());
		kv << QString("pixel=%1").arg(gridRegChk && gridRegChk->isChecked() ? 0 : 1);
		kv << QString("verbose=%1").arg(verboseChk && verboseChk->isChecked() ? 1 : 0);
		if (outEdit && !outEdit->text().trimmed().isEmpty()) kv << "outfile=" + outEdit->text().trimmed();
		kv << "grid=" + QString::fromStdString(activeGridName(scn));   // mask the DISPLAYED layer
		showBusyDialog("Building land mask…");
		const int ok = g_juliaGrdLandmask(scn, kv.join("\n").toUtf8().constData());
		closeBusyDialog();
		if (!ok) QMessageBox::warning(d, "grdlandmask",
		                              "grdlandmask failed — see this window's Errors console for details.");
	}
};

// Ends an armed vector pick (85_polygon.cpp — that is where the pick itself is served, so the
// disarm lives with it; declared here because this fragment is #included first).
static void vectorPickDisarm(Scene *s);

// ============================================================================================
// Euler rotations (Plates menu) — port of Mirone's src_figs/euler_stuff.m, re-based on GMT's spotter
// supplement (which post-dates the Mirone code): the rotations themselves are `backtracker`, the pole
// algebra is `rotconverter`, and the geodetic-latitude option is `mapproject -Ng` — no rotation maths
// is re-implemented here or on the Julia side (src/plates.jl). Mirone's three fake tab panels became
// one real QTabWidget, and its "Pick line from Figure" became the scene-element combo plus a one-shot
// "Pick in view" click (Scene::vectorPickArmed, resolved by the same hit tests the double-click edit
// path uses). Loaded at RUNTIME via QUiLoader from deps/ui/euler_stuff.ui.
// ============================================================================================

// One line of the poles catalogue (data/plates/lista_polos.dat, carried over from Mirone's
// continents/lista_polos.dat): "lon lat angle age !comment".
struct EulerPole {
	double lon = 0, lat = 0, ang = 0, age = 0;
	QString text;                                  // the whole line, as shown in the picker
};

static std::vector<EulerPole> readPolesCatalogue() {
	std::vector<EulerPole> out;
	QFile f(gmtvtkDataDir() + "/plates/lista_polos.dat");
	if (!f.open(QFile::ReadOnly | QFile::Text)) return out;
	QTextStream in(&f);
	while (!in.atEnd()) {
		const QString line = in.readLine();
		const QString t = line.trimmed();
		if (t.isEmpty() || t.startsWith('#')) continue;
		// The comment after '!' is the pole's identity (plate pair, chron, author) — keep it in the
		// display text, parse only the four numbers before it.
		const QString nums = t.section('!', 0, 0);
		const QStringList f4 = nums.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
		if (f4.size() < 4) continue;
		EulerPole p;
		bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
		p.lon = f4[0].toDouble(&ok1); p.lat = f4[1].toDouble(&ok2);
		p.ang = f4[2].toDouble(&ok3); p.age = f4[3].toDouble(&ok4);
		if (!(ok1 && ok2 && ok3 && ok4)) continue;
		p.text = t;
		out.push_back(p);
	}
	return out;
}

// What the picker was asked to do with the poles the user selected.
enum class PolesPick { Cancelled, Use, MakeStages };

// The stage-pole options Mirone's choosebox carries in its toolbar (finite2stages' HALF and SIDE).
struct StageOpts {
	bool half = false;        // half angles (HALF=2) — the flow-line case, GMT's -M0.5
	bool inverse = false;     // a_STAGE_b instead of b_STAGE_a (HALF < 0) — the conjugate plate
	int  side = 1;            // 1 = northern hemisphere (-N), -1 = southern (-S), 0 = positive angles
};

// Modal catalogue picker. `maxSel` = how many poles the caller can use (1 for the single-pole box, 2
// for Add poles, 0 = any number for Interpolate poles). `headerPoles` (may be empty) are the poles the
// ACTIVE line carries in its own header; they are listed FIRST, above the catalogue, exactly as
// Mirone's push_polesList_CB puts an isochron's own FIN/STG poles on top of lista_polos.dat.
// "Make stage poles" (only offered when several poles can be picked) is Mirone's choosebox stage
// path: it returns PolesPick::MakeStages with `opts` filled from the same three controls.
static PolesPick pickPolesFromCatalogue(QWidget *parent, int maxSel, std::vector<EulerPole> &out,
                                        const std::vector<EulerPole> &headerPoles = {},
                                        StageOpts *opts = nullptr) {
	std::vector<EulerPole> cat = headerPoles;
	const int nHeader = (int)cat.size();
	for (const EulerPole &p : readPolesCatalogue()) cat.push_back(p);
	if (cat.empty()) {
		QMessageBox::warning(parent, "Poles selector",
			"No poles catalogue found (data/plates/lista_polos.dat).");
		return PolesPick::Cancelled;
	}
	QDialog dlg(parent);
	dlg.setWindowTitle(maxSel == 1 ? "Poles selector — pick one"
	                 : maxSel == 2 ? "Poles selector — pick two (pole 1 first)"
	                               : "Poles selector — pick the model's poles");
	dlg.resize(620, 420);
	auto *v = new QVBoxLayout(&dlg);
	auto *lw = new QListWidget(&dlg);
	lw->setSelectionMode(maxSel == 1 ? QAbstractItemView::SingleSelection
	                                 : QAbstractItemView::ExtendedSelection);
	QFont mono("Courier New");
	mono.setStyleHint(QFont::Monospace);
	lw->setFont(mono);
	for (size_t i = 0; i < cat.size(); ++i) {
		auto *it = new QListWidgetItem(cat[i].text, lw);
		it->setData(Qt::UserRole, (int)i);
		if ((int)i < nHeader) {                       // the active line's OWN poles, marked as such
			QFont f = it->font(); f.setBold(true); it->setFont(f);
		}
	}
	tightenListRows(lw);
	v->addWidget(new QLabel(nHeader > 0 ? "lon   lat   angle   age      (bold = this line's own header poles)"
	                                    : "lon   lat   angle   age", &dlg));
	v->addWidget(lw, 1);

	// Mirone's choosebox stage-pole toolbar: half angles, inverse (conjugate) stages, hemisphere.
	QCheckBox *halfChk = nullptr, *invChk = nullptr;
	QComboBox *sideCb = nullptr;
	QPushButton *stagesBtn = nullptr;
	if (maxSel != 1) {
		auto *sr = new QHBoxLayout();
		halfChk = new QCheckBox("Half angles", &dlg);
		halfChk->setToolTip("Half opening angles — the flow line of a single plate (rotconverter -M0.5)");
		invChk = new QCheckBox("Inverse stages", &dlg);
		invChk->setToolTip("Stages of the conjugate plate (a_STAGE_b instead of b_STAGE_a)");
		sideCb = new QComboBox(&dlg);
		sideCb->addItem("North hemisphere poles", 1);
		sideCb->addItem("South hemisphere poles", -1);
		sideCb->addItem("Positive angles", 0);
		sr->addWidget(halfChk); sr->addWidget(invChk); sr->addWidget(sideCb); sr->addStretch(1);
		v->addLayout(sr);
	}

	auto *row = new QHBoxLayout();
	if (maxSel != 1) {
		stagesBtn = new QPushButton("Make stage poles", &dlg);
		stagesBtn->setAutoDefault(false);
		stagesBtn->setToolTip("Turn the selected finite poles into a stage-pole file and use it as the "
		                      "rotation model");
		row->addWidget(stagesBtn);
	}
	row->addStretch(1);
	auto *ok = new QPushButton("OK", &dlg);
	auto *cancel = new QPushButton("Cancel", &dlg);
	ok->setAutoDefault(false); cancel->setAutoDefault(false);
	row->addWidget(ok); row->addWidget(cancel);
	v->addLayout(row);
	PolesPick what = PolesPick::Cancelled;
	QObject::connect(ok, &QPushButton::clicked, &dlg, [&dlg, &what]() { what = PolesPick::Use; dlg.accept(); });
	QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
	QObject::connect(lw, &QListWidget::itemDoubleClicked, &dlg, [&dlg, &what]() { what = PolesPick::Use; dlg.accept(); });
	if (stagesBtn) QObject::connect(stagesBtn, &QPushButton::clicked, &dlg, [&dlg, &what]() {
		what = PolesPick::MakeStages; dlg.accept();
	});
	if (dlg.exec() != QDialog::Accepted || what == PolesPick::Cancelled) return PolesPick::Cancelled;
	if (opts) {
		opts->half    = halfChk && halfChk->isChecked();
		opts->inverse = invChk && invChk->isChecked();
		opts->side    = sideCb ? sideCb->currentData().toInt() : 1;
	}
	out.clear();
	// Selection order is not preserved by Qt, so rows are taken TOP-DOWN — the catalogue's own order,
	// which is also what "pole 1 first" means for Add poles and what Interpolate poles needs (it
	// sorts by age anyway).
	const int cap = (what == PolesPick::MakeStages) ? 0 : maxSel;   // stages always take every selected pole
	for (int i = 0; i < lw->count(); ++i) {
		if (!lw->item(i)->isSelected()) continue;
		out.push_back(cat[(size_t)lw->item(i)->data(Qt::UserRole).toInt()]);
		if (cap > 0 && (int)out.size() >= cap) break;
	}
	return out.empty() ? PolesPick::Cancelled : what;
}

class EulerDialog;
// One Euler rotations dialog per window, alive while parked — re-picking the menu entry brings THAT
// one back (unpark), never a second one. Same registry contract as g_contourDlgs above.
static std::map<Scene *, EulerDialog *> g_eulerDlgs;

class EulerDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	bool reallyClose = false;   // set by the parked row's "Delete": let the next close through
	QTabWidget *tabs = nullptr;
	// Do Rotations
	QListWidget *targetList = nullptr;
	QPushButton *pickBtn = nullptr, *rectBtn = nullptr;
	QLabel *activeLbl = nullptr;
	QLineEdit *polesEdit = nullptr, *agesEdit = nullptr;
	QLineEdit *poleLon = nullptr, *poleLat = nullptr, *poleAng = nullptr;
	QCheckBox *revertChk = nullptr, *geodeticChk = nullptr, *usePoleChk = nullptr;
	QListWidget *agesList = nullptr;
	// Add poles
	QLineEdit *p1Lon = nullptr, *p1Lat = nullptr, *p1Ang = nullptr;
	QLineEdit *p2Lon = nullptr, *p2Lat = nullptr, *p2Ang = nullptr;
	QLineEdit *p3Lon = nullptr, *p3Lat = nullptr, *p3Ang = nullptr;
	// Interpolate poles
	QLineEdit *finiteEdit = nullptr, *ages2Edit = nullptr, *interpOut = nullptr;
	QListWidget *ages2List = nullptr;
	// Poles taken from the catalogue instead of a file (Interpolate poles) — "lon lat ang age;…"
	QString inlinePoles;

	explicit EulerDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/euler_stuff.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("EulerDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("EulerDialog: QUiLoader failed to load the .ui"); return; }
		// WA_DeleteOnClose is deliberately NOT set: the X PARKS this dialog instead of destroying it,
		// exactly as a closed X,Y plot or Contours dialog does — everything set up in it (the picked
		// lines, the poles file, the ages list) has to survive being closed. See CloseParks below.
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;
		g_eulerDlgs[scn] = this;

		struct CloseParks : QObject {
			EulerDialog *ed;
			CloseParks(QObject *parent, EulerDialog *e) : QObject(parent), ed(e) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close && ed && !ed->reallyClose && sceneAlive(ed->scn)) {
					e->ignore();
					// An armed pick must not outlive the dialog going away from the screen.
					vectorPickDisarm(ed->scn);
					if (ed->pickBtn) { QSignalBlocker b(ed->pickBtn); ed->pickBtn->setChecked(false); }
					if (ed->rectBtn) { QSignalBlocker b(ed->rectBtn); ed->rectBtn->setChecked(false); }
					ed->dlg->hide();
					EulerDialog *p = ed;         // a lambda cannot capture a member of the enclosing class
					parkTool(p->scn, p->dlg, "Euler rotations", IC_Line,
					         "Closed Euler rotations dialog — double-click to bring it back, click for Show / Delete",
					         [p]() { p->unpark(); }, p->parkedMenu());
					// A handle the user cannot see is the same as no handle at all: reveal + unfold.
					unfoldSceneObjects(ed->scn);
					return true;
				}
				return QObject::eventFilter(o, e);
			}
		};
		dlg->installEventFilter(new CloseParks(dlg, this));

		tabs        = d->findChild<QTabWidget *>("tabs");
		targetList  = d->findChild<QListWidget *>("list_targets");
		pickBtn     = d->findChild<QPushButton *>("push_pick");
		rectBtn     = d->findChild<QPushButton *>("push_rect");
		activeLbl   = d->findChild<QLabel *>("lb_active");
		polesEdit   = d->findChild<QLineEdit *>("edit_polesFile");
		agesEdit    = d->findChild<QLineEdit *>("edit_agesFile");
		poleLon     = d->findChild<QLineEdit *>("edit_poleLon");
		poleLat     = d->findChild<QLineEdit *>("edit_poleLat");
		poleAng     = d->findChild<QLineEdit *>("edit_poleAng");
		revertChk   = d->findChild<QCheckBox *>("chk_revert");
		geodeticChk = d->findChild<QCheckBox *>("chk_geodetic");
		usePoleChk  = d->findChild<QCheckBox *>("chk_usePole");
		agesList    = d->findChild<QListWidget *>("list_ages");
		p1Lon = d->findChild<QLineEdit *>("edit_pole1Lon"); p1Lat = d->findChild<QLineEdit *>("edit_pole1Lat");
		p1Ang = d->findChild<QLineEdit *>("edit_pole1Ang");
		p2Lon = d->findChild<QLineEdit *>("edit_pole2Lon"); p2Lat = d->findChild<QLineEdit *>("edit_pole2Lat");
		p2Ang = d->findChild<QLineEdit *>("edit_pole2Ang");
		p3Lon = d->findChild<QLineEdit *>("edit_pole3Lon"); p3Lat = d->findChild<QLineEdit *>("edit_pole3Lat");
		p3Ang = d->findChild<QLineEdit *>("edit_pole3Ang");
		finiteEdit = d->findChild<QLineEdit *>("edit_finiteFile");
		ages2Edit  = d->findChild<QLineEdit *>("edit_agesFile2");
		interpOut  = d->findChild<QLineEdit *>("edit_interpOut");
		ages2List  = d->findChild<QListWidget *>("list_ages2");
		tightenListRows(agesList);
		tightenListRows(ages2List);
		tightenListRows(targetList);

		refillTargets();
		// The single-pole boxes only take input when the pole IS the rotation (Mirone's own gating).
		auto syncPole = [this]() {
			const bool on = usePoleChk && usePoleChk->isChecked();
			for (QLineEdit *e : { poleLon, poleLat, poleAng }) if (e) e->setEnabled(on);
			if (polesEdit) polesEdit->setEnabled(!on);
			if (agesEdit)  agesEdit->setEnabled(!on);
			if (agesList)  agesList->setEnabled(!on);
		};
		if (usePoleChk) QObject::connect(usePoleChk, &QCheckBox::toggled, d, [syncPole](bool) { syncPole(); });
		syncPole();

		wireFileBox(d, "edit_polesFile", "btn_polesFile", "Select rotation poles file",
		            "Rotation poles (*.stg *.rot *.dat *.txt);;All files (*)", false);
		wireFileBox(d, "edit_finiteFile", "btn_finiteFile", "Select finite rotation poles file",
		            "Rotation poles (*.rot *.dat *.txt);;All files (*)", false);
		wireFileBox(d, "edit_interpOut", "btn_interpOut", "Save interpolated poles",
		            "Poles (*.dat *.stg);;All files (*)", true);
		wireAgesBox(d, "edit_agesFile", "btn_agesFile", agesList);
		wireAgesBox(d, "edit_agesFile2", "btn_agesFile2", ages2List);

		// Pick in view / Rect select: arm the scene's vector pick and let clicks in the 3-D window ADD
		// lines to the selection (Mirone's "Pick line from Figure" and its rubber-band twin). Both
		// buttons are checkable and mutually exclusive — arming one disarms the other.
		if (pickBtn) QObject::connect(pickBtn, &QPushButton::toggled, d,
			[this, d](bool on) { armPick(d, on ? 1 : 0); });
		if (rectBtn) QObject::connect(rectBtn, &QPushButton::toggled, d,
			[this, d](bool on) { armPick(d, on ? 2 : 0); });
		if (targetList) QObject::connect(targetList, &QListWidget::itemSelectionChanged, d,
			[this]() { setActiveLabel(); });
		setActiveLabel();

		if (auto *b = d->findChild<QPushButton *>("push_polesList"))
			QObject::connect(b, &QPushButton::clicked, d, [this, d]() { pickSinglePole(d); });
		if (auto *b = d->findChild<QPushButton *>("push_polesListAdd"))
			QObject::connect(b, &QPushButton::clicked, d, [this, d]() { pickTwoPoles(d); });
		if (auto *b = d->findChild<QPushButton *>("push_polesListInterp"))
			QObject::connect(b, &QPushButton::clicked, d, [this, d]() { pickModelPoles(d); });
		// The bar code is the tool Mirone opens from here too — the SAME dialog the Magnetics menu
		// opens, not a second copy of it.
		if (auto *b = d->findChild<QPushButton *>("push_magbar"))
			QObject::connect(b, &QPushButton::clicked, d, [d]() {
				auto *w = new MagBarcodeDialog(d);
				if (w->dlg) w->dlg->show();
			});
		if (auto *b = d->findChild<QPushButton *>("push_showGMT"))
			QObject::connect(b, &QPushButton::clicked, d, [this, d]() { run(d, true); });

		for (QPushButton *b : d->findChildren<QPushButton *>()) { b->setAutoDefault(false); b->setDefault(false); }
		if (auto *b = d->findChild<QPushButton *>("push_compute"))
			QObject::connect(b, &QPushButton::clicked, d, [this, d]() { run(d, false); });
		if (auto *b = d->findChild<QPushButton *>("push_close")) QObject::connect(b, &QPushButton::clicked, d, [d]() { d->close(); });
		// The manual page follows the tab: the rotations run backtracker, the two pole tabs rotconverter.
		addManualButton(d, [this]() {
			return (tabs && tabs->currentIndex() == 0) ? QString("backtracker") : QString("rotconverter");
		});

		QObject::connect(d, &QObject::destroyed, d, [this]() {
			// Never leave the scene armed for a pick nobody is listening to.
			if (scn && sceneAlive(scn)) {
				vectorPickDisarm(scn);
				scn->vectorPickCB = nullptr;
			}
			delete this;
		});
	}

	// Drop the registry entry by VALUE, not by `scn`: when the owning viewer window is torn down the
	// dialog dies with it and the Scene may already be gone, so the key cannot be trusted.
	~EulerDialog() {
		for (auto it = g_eulerDlgs.begin(); it != g_eulerDlgs.end(); )
			it = (it->second == this) ? g_eulerDlgs.erase(it) : std::next(it);
	}

	// Bring the dialog back from the dock (double-click, the row's checkbox, its "Show" item). ONE
	// function for every way back in, like xyUnpark / ContourDialog::unpark.
	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();
		dlg->raise();
		dlg->activateWindow();
	}

	// The parked row's menu — properties button and context menu are the same lambda, never two.
	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel  = m.addAction("Delete");
			QAction *pick  = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scn, dlg);
				dlg->deleteLater();          // destroyed -> the row and this object go with it
			}
		};
	}

	// mode: 0 = off, 1 = click-to-add, 2 = rubber-band rectangle. The two buttons are exclusive, and
	// the answer (one or more Scene Objects labels, '\n'-separated) is ADDED to the current selection.
	void armPick(QDialog *d, int mode) {
		if (!scn || !sceneAlive(scn)) return;
		if (mode == 0) { vectorPickDisarm(scn); return; }
		if (pickBtn && mode != 1 && pickBtn->isChecked()) { QSignalBlocker b(pickBtn); pickBtn->setChecked(false); }
		if (rectBtn && mode != 2 && rectBtn->isChecked()) { QSignalBlocker b(rectBtn); rectBtn->setChecked(false); }
		QPointer<QDialog> guard(d);
		scn->vectorPickCB = [this, guard, mode](const std::string &names) {
			if (!guard) return;                            // dialog closed meanwhile: drop the answer
			if (mode == 2) {                               // one-shot: the scene already disarmed itself
				if (rectBtn) { QSignalBlocker b(rectBtn); rectBtn->setChecked(false); }
			}
			if (names.empty()) { setActiveLabel(); return; }
			refillTargets();
			if (targetList) {
				for (const QString &nm : QString::fromStdString(names).split('\n', Qt::SkipEmptyParts)) {
					for (QListWidgetItem *it : targetList->findItems(nm, Qt::MatchExactly))
						it->setSelected(true);
				}
			}
			setActiveLabel();
		};
		scn->vectorPickPrevShape = (int)scn->polyShape;
		scn->vectorPickMode = mode;
		scn->vectorPickDrawing = false;
		if (scn->widget) scn->widget->setCursor(Qt::CrossCursor);
		if (scn->win) scn->win->statusBar()->showMessage(
			mode == 1 ? "Click each line to rotate (the button stays down until you press it again)"
			          : "Click one corner of the selection rectangle, then the opposite one", 5000);
	}

	// Every LINE element of the window, whichever door it came in through: drawn polygons/polylines
	// and imported line overlays alike (SACRED_LAW — one operation, one list). The current selection
	// survives the refill (rows are re-selected by name).
	void refillTargets() {
		if (!targetList || !scn || !sceneAlive(scn)) return;
		QStringList keep;
		for (QListWidgetItem *it : targetList->selectedItems()) keep << it->text();
		QSignalBlocker block(targetList);
		targetList->clear();
		for (auto &pg : scn->polys) {
			if (pg.isFault || pg.isSlip || pg.nestKind != 0) continue;   // not lines to reconstruct
			targetList->addItem(QString::fromStdString(pg.name));
		}
		for (auto &ov : scn->overlays) {
			if (ov.mode != 1) continue;                                   // point layers are not lines
			targetList->addItem(QString::fromStdString(ov.name));
		}
		tightenListRows(targetList);
		for (const QString &nm : keep)
			for (QListWidgetItem *it : targetList->findItems(nm, Qt::MatchExactly))
				it->setSelected(true);
	}

	QStringList selectedTargets() const {
		QStringList v;
		if (targetList) for (QListWidgetItem *it : targetList->selectedItems()) v << it->text();
		return v;
	}

	void setActiveLabel() {
		if (!activeLbl) return;
		const QStringList t = selectedTargets();
		if (t.isEmpty()) {
			activeLbl->setText("NO ACTIVE LINE");
			activeLbl->setStyleSheet("color: rgb(200, 0, 0); font-weight: bold;");
		} else {
			activeLbl->setText(t.size() == 1 ? "GOT A LINE TO WORK WITH: " + t.first()
			                                 : QString("GOT %1 LINES TO WORK WITH").arg(t.size()));
			activeLbl->setStyleSheet("color: rgb(0, 140, 0); font-weight: bold;");
		}
	}

	void wireFileBox(QDialog *d, const char *editName, const char *btnName, const QString &title,
	                 const QString &filter, bool save) {
		auto *e = d->findChild<QLineEdit *>(editName);
		auto *b = d->findChild<QToolButton *>(btnName);
		if (!e || !b) return;
		QObject::connect(b, &QToolButton::clicked, d, [d, e, title, filter, save]() {
			const QString p = save ? QFileDialog::getSaveFileName(d, title, prefStartDir(), filter)
			                       : QFileDialog::getOpenFileName(d, title, prefStartDir(), filter);
			if (!p.isEmpty()) { e->setText(p); rememberStartDir(p); }
		});
		fileBoxDoubleClick(e, b);
	}

	// The ages box takes a FILE (one age per line, optionally "age chron"), a plain list ("10 20 30")
	// or a range ("0:5:50") — the same three spellings Mirone accepts. Whatever it resolves to lands in
	// the list widget, and the LIST is what Compute sends: one place holds the ages, never two.
	void wireAgesBox(QDialog *d, const char *editName, const char *btnName, QListWidget *lw) {
		auto *e = d->findChild<QLineEdit *>(editName);
		auto *b = d->findChild<QToolButton *>(btnName);
		if (!e || !b || !lw) return;
		QObject::connect(b, &QToolButton::clicked, d, [d, e, lw]() {
			const QString p = QFileDialog::getOpenFileName(d, "Select ages file", prefStartDir(),
			                                               "Ages (*.dat *.txt);;All files (*)");
			if (p.isEmpty()) return;
			e->setText(p); rememberStartDir(p);
			fillAges(lw, p);
		});
		fileBoxDoubleClick(e, b);
		QObject::connect(e, &QLineEdit::editingFinished, d, [e, lw]() { fillAges(lw, e->text().trimmed()); });
	}

	// age -> row; the numeric value rides in Qt::UserRole so a labelled chron shows its name but still
	// computes on its number.
	static void fillAges(QListWidget *lw, const QString &spec) {
		if (!lw) return;
		lw->clear();
		if (spec.isEmpty()) return;
		auto addAge = [lw](double a, const QString &label) {
			auto *it = new QListWidgetItem(label.isEmpty() ? QString::number(a, 'g', 10)
			                                               : label + "    " + QString::number(a, 'g', 10), lw);
			it->setData(Qt::UserRole, a);
		};
		if (QFileInfo(spec).isFile()) {
			QFile f(spec);
			if (f.open(QFile::ReadOnly | QFile::Text)) {
				QTextStream in(&f);
				while (!in.atEnd()) {
					const QString t = in.readLine().trimmed();
					if (t.isEmpty() || t.startsWith('#')) continue;
					const QStringList c = t.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
					bool ok = false;
					const double a = c[0].toDouble(&ok);
					if (!ok) continue;
					addAge(a, c.size() > 1 ? c[1] : QString());
				}
			}
		} else if (spec.contains(':')) {                    // first:step:last (Mirone's own spelling)
			const QStringList c = spec.split(':', Qt::SkipEmptyParts);
			if (c.size() == 3) {
				bool o1 = false, o2 = false, o3 = false;
				const double a0 = c[0].toDouble(&o1), st = c[1].toDouble(&o2), a1 = c[2].toDouble(&o3);
				if (o1 && o2 && o3 && st > 0)
					for (double a = a0; a <= a1 + 1e-9; a += st) addAge(a, QString());
			}
		} else {
			for (const QString &tok : spec.split(QRegularExpression("[\\s,;]+"), Qt::SkipEmptyParts)) {
				bool ok = false;
				const double a = tok.toDouble(&ok);
				if (ok) addAge(a, QString());
			}
		}
		tightenListRows(lw);
	}

	static QString agesCsv(QListWidget *lw) {
		QStringList v;
		if (lw) for (int i = 0; i < lw->count(); ++i)
			v << QString::number(lw->item(i)->data(Qt::UserRole).toDouble(), 'g', 12);
		return v.join(',');
	}
	static QString ageLabelsCsv(QListWidget *lw) {
		QStringList v;
		if (lw) for (int i = 0; i < lw->count(); ++i) {
			QString t = lw->item(i)->text();
			t.replace(',', ' ');
			v << t.trimmed();
		}
		return v.join(',');
	}

	// The poles the ACTIVE line carries in its OWN header (a Mirone isochron's FIN"…"/STG0"…"/…), asked
	// of Julia so the header parser stays the ONE that already exists (isocs.jl's _isoc_parse_header).
	// Answer: one "lon lat ang age !LABEL" line per pole, exactly the catalogue's own spelling.
	std::vector<EulerPole> headerPoles() {
		std::vector<EulerPole> out;
		const QStringList t = selectedTargets();
		if (t.isEmpty() || !g_juliaEuler) return out;
		g_eulerResult.clear();
		QStringList kv;
		kv << "op=headerpoles" << "target1=" + t.first() << "showcmd=0";
		if (!g_juliaEuler(scn, kv.join("\n").toUtf8().constData())) return out;
		for (const QString &ln : QString::fromStdString(g_eulerResult).split('\n', Qt::SkipEmptyParts)) {
			const QStringList f4 = ln.section('!', 0, 0).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
			if (f4.size() < 4) continue;
			EulerPole p;
			p.lon = f4[0].toDouble(); p.lat = f4[1].toDouble();
			p.ang = f4[2].toDouble(); p.age = f4[3].toDouble();
			p.text = ln.trimmed();
			out.push_back(p);
		}
		return out;
	}

	// Mirone's choosebox stage path (its own file-writing branch was commented out, so this revives
	// what euler_stuff's "Stage poles" answer was meant to do): turn the picked finite poles into a
	// stage-pole file and make it THE rotation model of the Do Rotations tab. rotconverter does the
	// conversion (op=stages, src/plates.jl).
	void makeStagePoles(QDialog *d, const std::vector<EulerPole> &p, const StageOpts &so) {
		if (!g_juliaEuler) {
			QMessageBox::warning(d, "Poles selector", "Euler rotations: callback not registered.");
			return;
		}
		if (p.size() < 2) {
			QMessageBox::warning(d, "Poles selector", "Pick at least two finite poles to build stages from.");
			return;
		}
		QStringList v;
		for (const EulerPole &e : p)
			v << QString("%1 %2 %3 %4").arg(e.lon, 0, 'g', 10).arg(e.lat, 0, 'g', 10)
			                           .arg(e.ang, 0, 'g', 10).arg(e.age, 0, 'g', 10);
		g_eulerResult.clear();
		QStringList kv;
		kv << "op=stages" << "poles=" + v.join(';')
		   << QString("half=%1").arg(so.half ? 1 : 0)
		   << QString("inverse=%1").arg(so.inverse ? 1 : 0)
		   << QString("side=%1").arg(so.side)
		   << QString("geodetic=%1").arg(geodeticChk && geodeticChk->isChecked() ? 1 : 0)
		   << "showcmd=0";
		const int ok = g_juliaEuler(scn, kv.join("\n").toUtf8().constData());
		const QString answer = QString::fromStdString(g_eulerResult);
		if (!ok) {
			QMessageBox::warning(d, "Stage poles", answer.isEmpty()
				? QString("Could not build the stage poles.") : answer);
			return;
		}
		// Julia writes the table to a file and reports "<path>\n<the table>".
		const int nl = answer.indexOf('\n');
		const QString path = nl < 0 ? answer : answer.left(nl);
		const QString table = nl < 0 ? QString() : answer.mid(nl + 1);
		if (polesEdit) polesEdit->setText(path);
		if (usePoleChk) usePoleChk->setChecked(false);      // the model is the rotation now, not one pole
		QDialog out(d);
		out.setWindowTitle("Stage Poles");
		out.resize(500, 320);
		auto *lay = new QVBoxLayout(&out);
		auto *te = new QPlainTextEdit(table, &out);
		te->setReadOnly(true);
		QFont mono("Courier New");
		mono.setStyleHint(QFont::Monospace);
		te->setFont(mono);
		lay->addWidget(new QLabel("Now the rotation model of Do Rotations:\n" + path, &out));
		lay->addWidget(te);
		auto *b = new QPushButton("Close", &out);
		b->setAutoDefault(false);
		QObject::connect(b, &QPushButton::clicked, &out, &QDialog::accept);
		lay->addWidget(b);
		out.exec();
	}

	void pickSinglePole(QDialog *d) {
		std::vector<EulerPole> p;
		StageOpts so;
		const PolesPick what = pickPolesFromCatalogue(d, 1, p, headerPoles(), &so);
		if (what == PolesPick::Cancelled) return;
		if (what == PolesPick::MakeStages) { makeStagePoles(d, p, so); return; }
		if (poleLon) poleLon->setText(QString::number(p[0].lon, 'g', 10));
		if (poleLat) poleLat->setText(QString::number(p[0].lat, 'g', 10));
		if (poleAng) poleAng->setText(QString::number(p[0].ang, 'g', 10));
		if (usePoleChk) usePoleChk->setChecked(true);
	}
	void pickTwoPoles(QDialog *d) {
		std::vector<EulerPole> p;
		StageOpts so;
		const PolesPick what = pickPolesFromCatalogue(d, 2, p, headerPoles(), &so);
		if (what == PolesPick::Cancelled) return;
		if (what == PolesPick::MakeStages) { makeStagePoles(d, p, so); return; }
		auto put = [](QLineEdit *lo, QLineEdit *la, QLineEdit *an, const EulerPole &e) {
			if (lo) lo->setText(QString::number(e.lon, 'g', 10));
			if (la) la->setText(QString::number(e.lat, 'g', 10));
			if (an) an->setText(QString::number(e.ang, 'g', 10));
		};
		put(p1Lon, p1Lat, p1Ang, p[0]);
		if (p.size() > 1) put(p2Lon, p2Lat, p2Ang, p[1]);
	}
	void pickModelPoles(QDialog *d) {
		std::vector<EulerPole> p;
		StageOpts so;
		const PolesPick what = pickPolesFromCatalogue(d, 0, p, headerPoles(), &so);
		if (what == PolesPick::Cancelled) return;
		if (what == PolesPick::MakeStages) { makeStagePoles(d, p, so); return; }
		QStringList v;
		for (const EulerPole &e : p)
			v << QString("%1 %2 %3 %4").arg(e.lon, 0, 'g', 10).arg(e.lat, 0, 'g', 10)
			                           .arg(e.ang, 0, 'g', 10).arg(e.age, 0, 'g', 10);
		inlinePoles = v.join(';');
		if (finiteEdit) finiteEdit->setText(QString("<%1 poles from the catalogue>").arg(p.size()));
	}

	// Compute (or, with showcmd, just ask what command WOULD run). One entry point for all three tabs
	// — the tab only decides which "op=" the same callback is handed.
	void run(QDialog *d, bool showcmd) {
		if (!g_juliaEuler) {
			QMessageBox::warning(d, "Euler rotations",
			                     "Euler rotations: callback not registered (rebuild/restart needed?).");
			return;
		}
		const int tab = tabs ? tabs->currentIndex() : 0;
		QStringList kv;
		if (tab == 0) {
			const QStringList targets = selectedTargets();
			if (targets.isEmpty()) {
				QMessageBox::warning(d, "Euler rotations", "No line selected — pick one to rotate.");
				return;
			}
			const bool usePole = usePoleChk && usePoleChk->isChecked();
			if (usePole) {
				if (!poleLon || !poleLat || !poleAng || poleLon->text().trimmed().isEmpty() ||
				    poleLat->text().trimmed().isEmpty() || poleAng->text().trimmed().isEmpty()) {
					QMessageBox::warning(d, "Euler rotations", "Fill the pole's Lon, Lat and Angle.");
					return;
				}
			} else {
				if (!polesEdit || polesEdit->text().trimmed().isEmpty()) {
					QMessageBox::warning(d, "Euler rotations", "No rotation poles file given.");
					return;
				}
				if (!agesList || agesList->count() == 0) {
					QMessageBox::warning(d, "Euler rotations",
					                     "No ages given — the rotations are computed at those ages.");
					return;
				}
			}
			kv << "op=rotate";
			for (int i = 0; i < targets.size(); ++i) kv << QString("target%1=").arg(i + 1) + targets[i];
			kv << "polesfile=" + (polesEdit ? polesEdit->text().trimmed() : QString());
			kv << "ages=" + agesCsv(agesList);
			kv << "agelabels=" + ageLabelsCsv(agesList);
			kv << QString("revert=%1").arg(revertChk && revertChk->isChecked() ? 1 : 0);
			kv << QString("geodetic=%1").arg(geodeticChk && geodeticChk->isChecked() ? 1 : 0);
			kv << QString("usepole=%1").arg(usePole ? 1 : 0);
			kv << "polelon=" + (poleLon ? poleLon->text().trimmed() : QString());
			kv << "polelat=" + (poleLat ? poleLat->text().trimmed() : QString());
			kv << "poleang=" + (poleAng ? poleAng->text().trimmed() : QString());
		} else if (tab == 1) {
			auto num = [](QLineEdit *e) { return e ? e->text().trimmed() : QString(); };
			if (num(p1Lon).isEmpty() || num(p1Lat).isEmpty() || num(p1Ang).isEmpty() ||
			    num(p2Lon).isEmpty() || num(p2Lat).isEmpty() || num(p2Ang).isEmpty()) {
				QMessageBox::warning(d, "Add poles", "Both poles need Lon, Lat and Angle.");
				return;
			}
			kv << "op=add";
			kv << "p1lon=" + num(p1Lon) << "p1lat=" + num(p1Lat) << "p1ang=" + num(p1Ang);
			kv << "p2lon=" + num(p2Lon) << "p2lat=" + num(p2Lat) << "p2ang=" + num(p2Ang);
		} else {
			const bool haveFile = finiteEdit && !finiteEdit->text().trimmed().isEmpty() && inlinePoles.isEmpty();
			if (!haveFile && inlinePoles.isEmpty()) {
				QMessageBox::warning(d, "Interpolate poles",
				                     "Give a finite poles file, or pick the poles from the catalogue.");
				return;
			}
			if (!ages2List || ages2List->count() == 0) {
				QMessageBox::warning(d, "Interpolate poles", "No ages to interpolate at.");
				return;
			}
			kv << "op=interp";
			kv << "polesfile=" + (haveFile ? finiteEdit->text().trimmed() : QString());
			kv << "poles=" + inlinePoles;
			kv << "ages=" + agesCsv(ages2List);
			kv << "outfile=" + (interpOut ? interpOut->text().trimmed() : QString());
		}
		kv << QString("showcmd=%1").arg(showcmd ? 1 : 0);

		g_eulerResult.clear();                           // clear the answer channel before asking
		if (!showcmd) showBusyDialog("Rotating…");
		const int ok = g_juliaEuler(scn, kv.join("\n").toUtf8().constData());
		if (!showcmd) closeBusyDialog();
		const QString answer = QString::fromStdString(g_eulerResult);
		if (!ok) {
			QMessageBox::warning(d, "Euler rotations", answer.isEmpty()
				? QString("Failed — see this window's Errors console for details.") : answer);
			return;
		}
		if (showcmd) {
			QMessageBox box(QMessageBox::Information, "GMT command", answer, QMessageBox::Ok, d);
			box.setTextInteractionFlags(Qt::TextSelectableByMouse);
			box.exec();
			return;
		}
		if (tab == 0) {
			refillTargets();                              // the rotated lines are elements too
		} else if (tab == 1) {
			// "lon lat angle" from rotconverter
			const QStringList c = answer.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
			if (c.size() >= 3) {
				if (p3Lon) p3Lon->setText(c[0]);
				if (p3Lat) p3Lat->setText(c[1]);
				if (p3Ang) p3Ang->setText(c[2]);
			}
		} else if (!answer.isEmpty()) {
			QDialog out(d);
			out.setWindowTitle("Interpolated poles");
			out.resize(460, 380);
			auto *v = new QVBoxLayout(&out);
			auto *te = new QPlainTextEdit(answer, &out);
			te->setReadOnly(true);
			QFont mono("Courier New");
			mono.setStyleHint(QFont::Monospace);
			te->setFont(mono);
			v->addWidget(te);
			auto *b = new QPushButton("Close", &out);
			b->setAutoDefault(false);
			QObject::connect(b, &QPushButton::clicked, &out, &QDialog::accept);
			v->addWidget(b);
			out.exec();
		}
	}
};

// ============================================================================================
// Plate calculator (Plates menu) — port of Mirone's src_figs/plate_calculator.m. Pick a plate motion
// model, a fixed and a moving plate, and it shows the relative Euler pole; give it a point and it
// shows the speed and azimuth of the moving plate there. Loaded at RUNTIME via QUiLoader from
// deps/ui/plate_calculator.ui.
//
// The maths is not here and not in Julia either: the relative pole is a gmtvector sum of angular
// velocity vectors and the velocity is gmtpmodeler (src/plates.jl, ops plates/platepole/platevel),
// reached through the SAME g_juliaEuler channel the Euler rotations dialog uses — one Plates menu,
// one door.
//
// The world map of coloured plate polygons is Mirone's, polygon for polygon: its own nuvel_polyg /
// PB_polyg / REVEL_polyg .mat files, decoded once into data/plates/<model>_polyg.dat, with the patch
// list, tags, colours and DRAW ORDER of the matching set_*plate_model. Clicking it does what
// bdn_plate does: the clicked plate becomes MOVING, its nearest neighbour (data/plates/
// plate_neighbours.dat == do_plate_comb) becomes FIXED, and the velocity is computed there.
//
// Geometry is Mirone's too — the .ui is a straight conversion of its LayoutFcn, fixed 709x317.
// ============================================================================================

// One filled plate on the map. `pts` are lon/lat; a plate split at the date line is several of
// these with the same tag, exactly as Mirone draws it as several patches.
struct PlatePolygon {
	QString   tag, name;
	QColor    col;
	QPolygonF pts;
};

// The clickable map; no Q_OBJECT (only paint/mouse overrides), like BaseMapArea / IgrfMapArea.
class PlateMapArea : public QWidget {
public:
	std::vector<PlatePolygon> polys;
	QMap<QString, QStringList> neigh;             // plate -> the plates it borders, this model
	QString model;
	double poleLon = 0, poleLat = 0;
	bool   havePole = false;
	// (lon, lat, moving plate tag, fixed plate tag). The tags are empty when the click missed
	// every plate — the point still travels, so the user can compute there by hand.
	std::function<void(double, double, const QString &, const QString &)> onPick;

	explicit PlateMapArea(QWidget *p) : QWidget(p) {
		setMinimumSize(200, 120);
		setCursor(Qt::CrossCursor);
	}

	// The drawing rectangle, leaving room for the tick labels (left for lat, bottom for lon).
	QRectF plotRect() const { return QRectF(rect()).adjusted(34, 6, -8, -20); }
	QPointF toPx(double lon, double lat, const QRectF &r) const {
		return QPointF(r.left() + (lon + 180.0) / 360.0 * r.width(),
		               r.top()  + (90.0 - lat)  / 180.0 * r.height());
	}
	double pxLon(double x, const QRectF &r) const { return -180.0 + (x - r.left()) / r.width()  * 360.0; }
	double pxLat(double y, const QRectF &r) const { return   90.0 - (y - r.top())  / r.height() * 180.0; }

	void setPole(double lon, double lat, bool ok) {
		poleLon = lon; poleLat = lat; havePole = ok;
		update();
	}

	// Load one model's polygons + its neighbour table. Both files are plain text shipped in
	// data/plates; a missing file leaves an empty (blank) map rather than killing the dialog.
	void setModel(const QString &key) {
		model = key;
		polys.clear();
		neigh.clear();
		QFile f(gmtvtkDataDir() + "/plates/" + key + "_polyg.dat");
		if (f.open(QFile::ReadOnly | QFile::Text)) {
			QTextStream in(&f);
			while (!in.atEnd()) {
				const QString ln = in.readLine();
				if (ln.isEmpty() || ln.startsWith('#')) continue;
				if (ln.startsWith('>')) {                       // "> AB Name r/g/b"
					const QStringList c = ln.mid(1).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
					if (c.size() < 3) continue;
					PlatePolygon p;
					p.tag = c[0];
					p.name = c[1];
					const QStringList rgb = c[2].split('/');
					if (rgb.size() == 3) p.col = QColor(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt());
					polys.push_back(p);
					continue;
				}
				if (polys.empty()) continue;
				const QStringList c = ln.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
				if (c.size() < 2) continue;
				polys.back().pts << QPointF(c[0].toDouble(), c[1].toDouble());
			}
		}
		QFile n(gmtvtkDataDir() + "/plates/plate_neighbours.dat");
		if (n.open(QFile::ReadOnly | QFile::Text)) {
			QTextStream in(&n);
			while (!in.atEnd()) {
				const QString ln = in.readLine();
				if (ln.isEmpty() || ln.startsWith('#')) continue;
				QStringList c = ln.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
				if (c.size() < 2 || c[0] != key) continue;
				const QString plate = c[1];
				c.removeFirst(); c.removeFirst();
				neigh[plate] = c;
			}
		}
		update();
	}

	// The plate under a point: the LAST polygon that contains it, because later polygons are painted
	// over earlier ones — the same "who is on top" answer MATLAB's patch stacking gave Mirone.
	QString plateAt(double lon, double lat) const {
		for (int i = (int)polys.size() - 1; i >= 0; --i)
			if (polys[i].pts.containsPoint(QPointF(lon, lat), Qt::OddEvenFill)) return polys[i].tag;
		return QString();
	}

	// bdn_plate's closest-neighbour search, verbatim: the angular distance from the clicked point to
	// every vertex of each neighbouring plate; the plate with the smallest one is taken as FIXED.
	QString nearestNeighbour(const QString &tag, double lon, double lat) const {
		const QStringList cand = neigh.value(tag);
		const double D2R = M_PI / 180.0;
		const double c_tet = std::cos((90.0 - lat) * D2R), s_tet = std::sin((90.0 - lat) * D2R);
		QString best;
		double bestD = std::numeric_limits<double>::max();
		for (const QString &ab : cand) {
			double dmin = std::numeric_limits<double>::max();
			for (const PlatePolygon &p : polys) {
				if (p.tag != ab) continue;
				for (const QPointF &v : p.pts) {
					double d = c_tet * std::cos((90.0 - v.y()) * D2R) +
					           s_tet * std::sin((90.0 - v.y()) * D2R) * std::cos((lon - v.x()) * D2R);
					d = std::acos(std::clamp(d, -1.0, 1.0));
					if (std::fabs(d) < dmin) dmin = std::fabs(d);
				}
			}
			if (dmin < bestD) { bestD = dmin; best = ab; }
		}
		return best;
	}

	// The whole click answer in one place, so a test can drive it without a synthetic mouse event.
	void pickAt(double lon, double lat) {
		QString mov = plateAt(lon, lat);
		// MORVEL has no Africa pole: its Nubia (NB) is what the Africa polygon stands for here —
		// Mirone's own remap in bdn_plate.
		if (model == "MORVEL" && mov == "AF") mov = "NB";
		const QString fix = mov.isEmpty() ? QString() : nearestNeighbour(mov, lon, lat);
		if (onPick) onPick(lon, lat, mov, fix);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter g(this);
		g.setRenderHint(QPainter::Antialiasing, true);
		const QRectF r = plotRect();
		g.fillRect(rect(), palette().window());
		g.fillRect(r, Qt::white);
		g.save();
		g.setClipRect(r);
		QPen edge(Qt::black);
		edge.setWidthF(0.6);
		g.setPen(edge);
		for (const PlatePolygon &p : polys) {
			QPolygonF px;
			px.reserve(p.pts.size());
			for (const QPointF &v : p.pts) px << toPx(v.x(), v.y(), r);
			g.setBrush(p.col);
			g.drawPolygon(px);
		}
		// The Euler pole, drawn as Mirone draws it: a filled dot where the axis leaves the Earth and
		// a cross inside a circle where it goes back in (the antipode).
		if (havePole) {
			double lo = poleLon;
			if (lo > 180.0) lo -= 360.0;
			const QPointF a = toPx(lo, poleLat, r);
			g.setPen(QPen(Qt::black, 1.5));
			g.setBrush(Qt::black);
			g.drawEllipse(a, 3.5, 3.5);
			double lo2 = lo + 180.0;
			if (lo2 > 180.0) lo2 -= 360.0;
			const QPointF b = toPx(lo2, -poleLat, r);
			g.setBrush(Qt::NoBrush);
			g.drawEllipse(b, 4.0, 4.0);
			g.drawLine(QPointF(b.x() - 5, b.y()), QPointF(b.x() + 5, b.y()));
			g.drawLine(QPointF(b.x(), b.y() - 5), QPointF(b.x(), b.y() + 5));
		}
		g.restore();
		// Frame + ticks, at the same intervals Mirone's axes shows.
		g.setPen(QPen(Qt::black, 1));
		g.setBrush(Qt::NoBrush);
		g.drawRect(r);
		QFont fnt = g.font();
		fnt.setPointSizeF(std::max(6.0, fnt.pointSizeF() - 1.5));
		g.setFont(fnt);
		for (int lon = -150; lon <= 150; lon += 50) {
			const double x = toPx(lon, 0, r).x();
			g.drawLine(QPointF(x, r.bottom()), QPointF(x, r.bottom() - 4));
			g.drawText(QRectF(x - 22, r.bottom() + 2, 44, 16), Qt::AlignHCenter | Qt::AlignTop,
			           QString::number(lon));
		}
		for (int lat = -80; lat <= 80; lat += 20) {
			const double y = toPx(0, lat, r).y();
			g.drawLine(QPointF(r.left(), y), QPointF(r.left() + 4, y));
			g.drawText(QRectF(r.left() - 34, y - 8, 30, 16), Qt::AlignRight | Qt::AlignVCenter,
			           QString::number(lat));
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		const QRectF r = plotRect();
		if (!r.contains(e->position())) return;
		pickAt(std::clamp(pxLon(e->position().x(), r), -180.0, 180.0),
		       std::clamp(pxLat(e->position().y(), r),  -90.0,  90.0));
	}
};

class PlateCalcDialog;
// One Plate calculator per window, alive while parked — same registry contract as g_eulerDlgs.
static std::map<Scene *, PlateCalcDialog *> g_plateDlgs;

class PlateCalcDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	bool reallyClose = false;
	QComboBox *cbFixed = nullptr, *cbMoving = nullptr;
	QLineEdit *edLon = nullptr, *edLat = nullptr;
	QLineEdit *poleLon = nullptr, *poleLat = nullptr, *poleRate = nullptr;
	QLabel *lbSpeed = nullptr, *lbAzim = nullptr;
	QCheckBox *abs2rel = nullptr;
	PlateMapArea *map = nullptr;
	std::vector<QRadioButton *> modelBtns;      // parallel to modelKeys below
	QStringList modelKeys;

	explicit PlateCalcDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/plate_calculator.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("PlateCalcDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("PlateCalcDialog: QUiLoader failed to load the .ui"); return; }
		// The X PARKS this dialog (see CloseParks) — the picked model, plates and pole survive it.
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;
		g_plateDlgs[scn] = this;

		struct CloseParks : QObject {
			PlateCalcDialog *pd;
			CloseParks(QObject *parent, PlateCalcDialog *p) : QObject(parent), pd(p) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close && pd && !pd->reallyClose && sceneAlive(pd->scn)) {
					e->ignore();
					pd->dlg->hide();
					PlateCalcDialog *p = pd;
					parkTool(p->scn, p->dlg, "Plate calculator", IC_Line,
					         "Closed Plate calculator — double-click to bring it back, click for Show / Delete",
					         [p]() { p->unpark(); }, p->parkedMenu());
					unfoldSceneObjects(pd->scn);
					return true;
				}
				return QObject::eventFilter(o, e);
			}
		};
		dlg->installEventFilter(new CloseParks(dlg, this));

		cbFixed  = d->findChild<QComboBox *>("combo_fixed");
		cbMoving = d->findChild<QComboBox *>("combo_moving");
		edLon    = d->findChild<QLineEdit *>("edit_lon");
		edLat    = d->findChild<QLineEdit *>("edit_lat");
		poleLon  = d->findChild<QLineEdit *>("edit_poleLon");
		poleLat  = d->findChild<QLineEdit *>("edit_poleLat");
		poleRate = d->findChild<QLineEdit *>("edit_poleRate");
		lbSpeed  = d->findChild<QLabel *>("lb_speed");
		lbAzim   = d->findChild<QLabel *>("lb_azim");
		abs2rel  = d->findChild<QCheckBox *>("chk_abs2rel");

		// The map goes exactly where the .ui's placeholder sits, so its rectangle stays editable in
		// Designer (QUiLoader builds no custom class of ours — the placeholder is a plain QWidget).
		if (auto *holder = d->findChild<QWidget *>("map_holder")) {
			map = new PlateMapArea(d);
			map->setGeometry(holder->geometry());
			holder->hide();
			map->show();
			// A click IS the tool: it sets the point, the moving plate and the fixed plate, then the
			// velocity — Mirone's bdn_plate, which ends by calling push_Calculate_CB.
			map->onPick = [this, d](double lon, double lat, const QString &mov, const QString &fix) {
				if (edLon) edLon->setText(QString::number(lon, 'f', 3));
				if (edLat) edLat->setText(QString::number(lat, 'f', 3));
				if (mov.isEmpty()) {
					if (scn && scn->win) scn->win->statusBar()->showMessage(
						"No pole known for that place — pick the plates by hand", 4000);
					return;
				}
				const bool absolute = isAbsolute() && !relativized();
				auto setCombo = [](QComboBox *cb, const QString &ab) {
					if (!cb || ab.isEmpty()) return;
					const int i = cb->findData(ab);
					if (i >= 0) { QSignalBlocker b(cb); cb->setCurrentIndex(i); }
				};
				setCombo(cbMoving, mov);
				if (!absolute) setCombo(cbFixed, fix);
				updatePole();
				calculate(d, true);            // quiet: a click must never raise a message box
			};
		}

		// The radio -> model key table. Same seven models Mirone offers, four relative + three absolute.
		const struct { const char *obj; const char *key; } models[] = {
			{ "rb_nuvel1a", "Nuvel1A" }, { "rb_morvel", "MORVEL" }, { "rb_pbird", "PB" },
			{ "rb_geodvel", "GEODVEL" }, { "rb_nnr", "NNR" }, { "rb_deos2k", "DEOS2K" },
			{ "rb_revel", "REVEL" },
		};
		for (const auto &m : models) {
			if (auto *b = d->findChild<QRadioButton *>(m.obj)) {
				modelBtns.push_back(b);
				modelKeys << m.key;
				QObject::connect(b, &QRadioButton::toggled, d, [this](bool on) { if (on) modelChanged(); });
			}
		}

		if (cbFixed) QObject::connect(cbFixed, QOverload<int>::of(&QComboBox::currentIndexChanged), d,
			[this](int) { updatePole(); });
		if (cbMoving) QObject::connect(cbMoving, QOverload<int>::of(&QComboBox::currentIndexChanged), d,
			[this](int) { updatePole(); });
		if (abs2rel) QObject::connect(abs2rel, &QCheckBox::toggled, d,
			[this](bool) { syncMode(); updatePole(); });

		for (QPushButton *b : d->findChildren<QPushButton *>()) { b->setAutoDefault(false); b->setDefault(false); }
		if (auto *b = d->findChild<QPushButton *>("push_calc"))
			QObject::connect(b, &QPushButton::clicked, d, [this, d]() { calculate(d, false); });
		if (auto *b = d->findChild<QPushButton *>("push_readme"))
			QObject::connect(b, &QPushButton::clicked, d, [d]() { readme(d); });

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });

		modelChanged();                      // fills the plate combos of the default model (Nuvel-1A)
	}

	~PlateCalcDialog() {
		for (auto it = g_plateDlgs.begin(); it != g_plateDlgs.end(); )
			it = (it->second == this) ? g_plateDlgs.erase(it) : std::next(it);
	}

	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();
		dlg->raise();
		dlg->activateWindow();
	}

	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel  = m.addAction("Delete");
			QAction *pick  = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scn, dlg);
				dlg->deleteLater();
			}
		};
	}

	QString modelKey() const {
		for (int i = 0; i < (int)modelBtns.size(); ++i)
			if (modelBtns[i]->isChecked()) return modelKeys[i];
		return QString("Nuvel1A");
	}
	// The three absolute-motion frames. For them the moving plate's own pole IS the answer, so the
	// fixed-plate combo means nothing — unless "Make it relative" is ticked (Mirone's abs2rel).
	bool isAbsolute() const {
		const QString k = modelKey();
		return (k == "NNR" || k == "DEOS2K" || k == "REVEL");
	}
	bool relativized() const { return abs2rel && abs2rel->isChecked(); }

	// One place asks Julia; `ok` says whether the call succeeded, the return value is its answer.
	QString ask(const QStringList &kv, bool *ok) {
		if (ok) *ok = false;
		if (!g_juliaEuler) return QString();
		g_eulerResult.clear();
		const int r = g_juliaEuler(scn, kv.join("\n").toUtf8().constData());
		if (ok) *ok = (r != 0);
		return QString::fromStdString(g_eulerResult);
	}

	void syncMode() {
		const bool absolute = isAbsolute();
		if (abs2rel) abs2rel->setVisible(absolute);
		if (cbFixed) cbFixed->setEnabled(!absolute || relativized());
	}

	void clearResults() {
		if (lbSpeed) lbSpeed->setText("Speed   = ");
		if (lbAzim)  lbAzim->setText("Azimuth = ");
	}

	// A new model brings a new plate list — both combos are refilled and reset to the first plate,
	// exactly as Mirone does, and the pole/velocity boxes start over.
	void modelChanged() {
		// No modal complaint here: this runs while the dialog is being built (and on every model
		// change), so a missing callback leaves the combos empty and Calculate does the complaining.
		if (!cbFixed || !cbMoving || !g_juliaEuler) return;
		bool ok = false;
		const QString answer = ask(QStringList() << "op=plates" << "model=" + modelKey(), &ok);
		if (!ok) {
			QMessageBox::warning(dlg, "Plate calculator", answer.isEmpty()
				? QString("Could not read the poles of this model.") : answer);
			return;
		}
		{
			QSignalBlocker b1(cbFixed), b2(cbMoving);
			cbFixed->clear();
			cbMoving->clear();
			for (const QString &ln : answer.split('\n', Qt::SkipEmptyParts)) {
				const QStringList c = ln.split('\t');
				if (c.size() < 2) continue;
				cbFixed->addItem(c[1], c[0]);
				cbMoving->addItem(c[1], c[0]);
			}
		}
		if (map) map->setModel(modelKey());          // the map follows the model, like Mirone's patches
		syncMode();
		clearResults();
		updatePole();
	}

	// The pole of the plate pair, straight into the three boxes. This is a display refresh, not the
	// tool's result: the velocity is computed only when Calculate is pressed.
	void updatePole() {
		if (!cbFixed || !cbMoving || cbMoving->count() == 0 || !g_juliaEuler) return;
		const bool absolute = isAbsolute() && !relativized();
		QStringList kv;
		kv << "op=platepole" << "model=" + modelKey()
		   << "fix=" + (absolute ? QString() : cbFixed->currentData().toString())
		   << "mov=" + cbMoving->currentData().toString();
		bool ok = false;
		const QString answer = ask(kv, &ok);
		if (!ok) {
			QMessageBox::warning(dlg, "Plate calculator", answer.isEmpty()
				? QString("Could not compute the Euler pole.") : answer);
			return;
		}
		const QStringList c = answer.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
		// Empty answer = no motion (the same plate on both sides): clear the boxes, like Mirone.
		const bool have = (c.size() >= 3);
		if (poleLon)  poleLon->setText(have ? c[0] : QString());
		if (poleLat)  poleLat->setText(have ? c[1] : QString());
		if (poleRate) poleRate->setText(have ? c[2] : QString());
		if (map) map->setPole(have ? c[0].toDouble() : 0.0, have ? c[1].toDouble() : 0.0, have);
		clearResults();
	}

	// `quiet` = the caller is a map click, where a missing value is normal and a message box would be
	// noise. The Calculate button passes false and complains, exactly as Mirone's errordlg does.
	void calculate(QDialog *d, bool quiet) {
		if (!g_juliaEuler) {
			if (!quiet) QMessageBox::warning(d, "Plate calculator",
			                                 "Plate calculator: callback not registered (rebuild/restart needed?).");
			return;
		}
		auto txt = [](QLineEdit *e) { return e ? e->text().trimmed() : QString(); };
		if (txt(poleLon).isEmpty() || txt(poleLat).isEmpty() || txt(poleRate).isEmpty()) {
			if (!quiet) QMessageBox::warning(d, "Plate calculator", "Euler pole parameters are wrong.");
			return;
		}
		if (txt(edLon).isEmpty() || txt(edLat).isEmpty()) {
			if (!quiet) QMessageBox::warning(d, "Plate calculator",
			                                 "Calculate the velocity where? Give a Lon and a Lat.");
			return;
		}
		QStringList kv;
		kv << "op=platevel" << "polelon=" + txt(poleLon) << "polelat=" + txt(poleLat)
		   << "polerate=" + txt(poleRate) << "lon=" + txt(edLon) << "lat=" + txt(edLat) << "showcmd=0";
		bool ok = false;
		const QString answer = ask(kv, &ok);
		if (!ok) {
			if (!quiet) QMessageBox::warning(d, "Plate calculator", answer.isEmpty()
				? QString("Failed — see this window's Errors console for details.") : answer);
			return;
		}
		const QStringList c = answer.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
		if (c.size() < 2) return;
		if (lbSpeed) lbSpeed->setText("Speed   = " + c[0] + " mm/yr");
		if (lbAzim)  lbAzim->setText("Azimuth = " + c[1] + " degree (cw from N)");
	}

	static void readme(QDialog *d) {
		const QString msg =
			"Click around the map and watch the results. The plate you click becomes the MOVING plate, "
			"its nearest neighbour becomes the FIXED one, and the speed and azimuth are computed at the "
			"point you clicked. You can also change the point, the plates, or the rotation pole itself by "
			"hand and press Calculate.\n\n"
			"The black dot on the map is where the current Euler pole leaves the Earth; the cross inside "
			"a circle is where it goes back in.\n\n"
			"The four models on the left are RELATIVE: the pair of plates you choose is what the pole "
			"describes. The three on the right are ABSOLUTE frames, so the moving plate's own pole is the "
			"answer and the fixed plate is ignored — unless you tick \"Relativize\", which turns the "
			"absolute model into a relative one for the chosen pair.\n\n"
			"Not every model has a pole for every plate polygon: the polygons come from the Nuvel-1A and "
			"P. Bird plate sets (Mirone's own, and the only ones available), so a click on some areas "
			"says nothing. Type the point coordinates, choose the plate in the combo and hit Calculate.";
		QMessageBox box(QMessageBox::Information, "Help on Plate Calculator", msg, QMessageBox::Ok, d);
		box.setTextInteractionFlags(Qt::TextSelectableByMouse);
		box.exec();
	}
};

// ============================================================================================
// Compute Euler pole (Plates menu) — port of Mirone's src_figs/compute_euler.m. Fits the pole that
// takes one isochron onto its conjugate, either by the brute-force search over a box of poles or by
// the Hellinger method. The maths lives in Julia (src/computeeuler.jl + GMT.jl's hellinger_auto);
// this is the dialog, loaded at RUNTIME via QUiLoader from deps/ui/compute_euler.ui.
//
// It goes through the SAME `gmtvtk_set_euler_callback` door as the other two Plates dialogs, with
// its own `op=ceuler` / `op=ceuler_stop` — one callback for the whole menu.
//
// The search can take minutes, so it does NOT block: Julia returns at once and pushes progress and
// the running best pole back through gmtvtk_compute_euler_progress (below), which drives the
// progress bar and the five result boxes exactly like the MATLAB dialog's live updates. STOP asks
// the search to stop; what it then reports is the best it had found, i.e. what is already on screen.
class ComputeEulerDialog;
static std::map<Scene *, ComputeEulerDialog *> g_ceulerDlgs;
static ComputeEulerDialog *g_ceulerRunning = nullptr;    // the dialog whose run is in flight (one at a time)

class ComputeEulerDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	bool reallyClose = false;
	QComboBox *line1 = nullptr, *line2 = nullptr;
	QLineEdit *pLon = nullptr, *pLat = nullptr, *pAng = nullptr;
	QLineEdit *fLon = nullptr, *fLat = nullptr, *fAng = nullptr;
	QLineEdit *stRes = nullptr, *bfRes = nullptr;
	QLineEdit *lonRange = nullptr, *latRange = nullptr, *angRange = nullptr;
	QLineEdit *nLon = nullptr, *nLat = nullptr, *nAng = nullptr;
	QLineEdit *errFile = nullptr;
	QLabel *nIntLbl = nullptr;
	QCheckBox *hellChk = nullptr, *plotResChk = nullptr, *loopChk = nullptr, *showCubeChk = nullptr;
	QCheckBox *statsChk = nullptr, *ellipseChk = nullptr, *forceChk = nullptr, *segChk = nullptr;
	QRadioButton *ncRadio = nullptr, *vtkRadio = nullptr;
	QProgressBar *bar = nullptr;
	QPushButton *stopBtn = nullptr, *computeBtn = nullptr, *recycleBtn = nullptr, *pickBtn = nullptr;
	// The N-points boxes double as the Hellinger DP tolerance / volume readout, as in the MATLAB
	// dialog; their normal contents are kept here while that mode is on.
	QString savedNLon, savedNLat, savedNAng;

	explicit ComputeEulerDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/compute_euler.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("ComputeEulerDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("ComputeEulerDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		QDialog *d = dlg;
		g_ceulerDlgs[scn] = this;

		// The X parks it (everything set up in it — the two lines, the starting pole, the ranges —
		// has to survive being closed), same contract as EulerDialog.
		struct CloseParks : QObject {
			ComputeEulerDialog *ed;
			CloseParks(QObject *parent, ComputeEulerDialog *e) : QObject(parent), ed(e) {}
			bool eventFilter(QObject *o, QEvent *e) override {
				if (e->type() == QEvent::Close && ed && !ed->reallyClose && sceneAlive(ed->scn)) {
					e->ignore();
					ed->setPickDown(false);       // a parked dialog never leaves the view armed
					ed->parkNow();
					return true;
				}
				return QObject::eventFilter(o, e);
			}
		};
		dlg->installEventFilter(new CloseParks(dlg, this));

		line1 = d->findChild<QComboBox *>("combo_line1");
		line2 = d->findChild<QComboBox *>("combo_line2");
		pLon  = d->findChild<QLineEdit *>("edit_pLon_ini");
		pLat  = d->findChild<QLineEdit *>("edit_pLat_ini");
		pAng  = d->findChild<QLineEdit *>("edit_pAng_ini");
		fLon  = d->findChild<QLineEdit *>("edit_pLon_fim");
		fLat  = d->findChild<QLineEdit *>("edit_pLat_fim");
		fAng  = d->findChild<QLineEdit *>("edit_pAng_fim");
		stRes = d->findChild<QLineEdit *>("edit_InitialResidue");
		bfRes = d->findChild<QLineEdit *>("edit_BFresidue");
		lonRange = d->findChild<QLineEdit *>("edit_LonRange");
		latRange = d->findChild<QLineEdit *>("edit_LatRange");
		angRange = d->findChild<QLineEdit *>("edit_AngRange");
		nLon = d->findChild<QLineEdit *>("edit_nLon");
		nLat = d->findChild<QLineEdit *>("edit_nLat");
		nAng = d->findChild<QLineEdit *>("edit_nAng");
		errFile = d->findChild<QLineEdit *>("edit_err_file");
		nIntLbl = d->findChild<QLabel *>("textNint");
		hellChk = d->findChild<QCheckBox *>("check_hellinger");
		plotResChk = d->findChild<QCheckBox *>("check_plotRes");
		loopChk = d->findChild<QCheckBox *>("check_loopUntil");
		showCubeChk = d->findChild<QCheckBox *>("check_showCube");
		statsChk = d->findChild<QCheckBox *>("check_show_stats");
		ellipseChk = d->findChild<QCheckBox *>("check_error_ellipse");
		forceChk = d->findChild<QCheckBox *>("check_in_equal_out");
		segChk = d->findChild<QCheckBox *>("check_color_segmentation");
		ncRadio = d->findChild<QRadioButton *>("radio_netcdf");
		vtkRadio = d->findChild<QRadioButton *>("radio_VTK");
		bar = d->findChild<QProgressBar *>("slider_wait");
		stopBtn = d->findChild<QPushButton *>("push_stop");
		computeBtn = d->findChild<QPushButton *>("push_compute");
		recycleBtn = d->findChild<QPushButton *>("push_reciclePole");

		refillLines();
		// "Pick lines from Figure" is a TOGGLE: it stays DOWN for as long as the pick is running, and
		// while it is down a LEFT DOUBLE-CLICK on a line in the 3-D view drops that line into the first
		// empty box (First Line, then Second Line). The .ui's flat Windows-11 paint made it read as a
		// label, so it gets a real raised edge and an unmistakable sunken/highlighted look when down.
		pickBtn = d->findChild<QPushButton *>("push_pickLines");
		if (pickBtn) {
			pickBtn->setCheckable(true);
			pickBtn->setStyleSheet(
				"QPushButton { border: 1px solid palette(mid); border-radius: 3px;"
				" background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
				" stop:0 palette(light), stop:1 palette(button)); }"
				"QPushButton:hover { border-color: palette(highlight); }"
				"QPushButton:pressed, QPushButton:checked { border: 2px solid palette(shadow);"
				" background: palette(highlight); color: palette(highlighted-text); }");
			QObject::connect(pickBtn, &QPushButton::toggled, d, [this](bool on) { armPick(on); });
		}
		wireFileBox(d, "combo_line1", "btn_line1", "Select the moving isochron",
		            "Line files (*.dat *.txt *.xy);;All files (*)");
		wireFileBox(d, "combo_line2", "btn_line2", "Select the fixed isochron",
		            "Line files (*.dat *.txt *.xy);;All files (*)");

		// The Poles selector is the catalogue picker the Euler rotations dialog uses — one picker.
		if (auto *b = d->findChild<QPushButton *>("push_polesList"))
			QObject::connect(b, &QPushButton::clicked, d, [this, d]() {
				std::vector<EulerPole> sel;
				if (pickPolesFromCatalogue(d, 1, sel) != PolesPick::Use || sel.empty()) return;
				if (pLon) pLon->setText(QString::number(sel[0].lon));
				if (pLat) pLat->setText(QString::number(sel[0].lat));
				if (pAng) pAng->setText(QString::number(sel[0].ang));
			});
		// "^": start over from the pole just computed (Mirone's push_reciclePole).
		if (recycleBtn) QObject::connect(recycleBtn, &QPushButton::clicked, d, [this]() { recyclePole(); });

		if (auto *b = d->findChild<QToolButton *>("push_err_file")) {
			QObject::connect(b, &QToolButton::clicked, d, [this, d]() {
				const QString fn = QFileDialog::getSaveFileName(d, "Save residues grid", QString(),
					"Residues grid (*.nc *.grd *.vtk);;All files (*)");
				if (fn.isEmpty()) return;
				if (errFile) errFile->setText(fn);
				const QString ext = QFileInfo(fn).suffix().toLower();
				if (ext == "vtk") { if (vtkRadio) vtkRadio->setChecked(true); }
				else if (ncRadio) ncRadio->setChecked(true);
			});
			fileBoxDoubleClick(errFile, b);      // standing rule: the box opens the chooser too
		}

		if (hellChk) QObject::connect(hellChk, &QCheckBox::toggled, d, [this](bool) { syncHellinger(); });
		syncHellinger();

		// The three N-points boxes take "N*Delta" as well as a plain count (Mirone's edit_nInt_CB),
		// and a changed range re-derives the spacing — so the tooltip always says what the step is.
		struct NRow { QLineEdit *n; QLineEdit *r; };
		const NRow rows[3] = { { nLon, lonRange }, { nLat, latRange }, { nAng, angRange } };
		for (const NRow &row : rows) {
			if (!row.n || !row.r) continue;
			QLineEdit *ne = row.n, *re = row.r;
			QObject::connect(ne, &QLineEdit::editingFinished, d, [this, ne, re]() { syncNInt(ne, re); });
			QObject::connect(re, &QLineEdit::editingFinished, d, [this, ne, re]() { syncNInt(ne, re); });
			syncNInt(ne, re);
		}

		for (QPushButton *b : d->findChildren<QPushButton *>()) { b->setAutoDefault(false); b->setDefault(false); }
		if (computeBtn) QObject::connect(computeBtn, &QPushButton::clicked, d, [this]() { run(); });
		if (stopBtn) QObject::connect(stopBtn, &QPushButton::clicked, d, [this]() { stop(); });
		addManualButton(dlg, []() { return QString("backtracker"); });

		QObject::connect(d, &QObject::destroyed, d, [this]() {
			if (g_ceulerRunning == this) g_ceulerRunning = nullptr;
			if (scn && sceneAlive(scn)) {          // never leave the scene armed for a pick nobody hears
				if (scn->vectorPickMode == 1) vectorPickDisarm(scn);
				scn->vectorPickCB = nullptr;
			}
			delete this;
		});
	}

	~ComputeEulerDialog() {
		if (g_ceulerRunning == this) g_ceulerRunning = nullptr;
		for (auto it = g_ceulerDlgs.begin(); it != g_ceulerDlgs.end(); )
			it = (it->second == this) ? g_ceulerDlgs.erase(it) : std::next(it);
	}

	void unpark() {
		if (!dlg) return;
		unparkTool(scn, dlg);
		dlg->setWindowState(dlg->windowState() & ~Qt::WindowMinimized);
		dlg->showNormal();
		dlg->raise();
		dlg->activateWindow();
	}

	// Get off the screen but stay alive, with the Scene Objects row that brings it back. ONE function
	// for every reason to do it — the X, and arming a pick (the dialog must not sit on top of the very
	// view the user is about to double-click in) — never two ways of parking the same dialog.
	void parkNow() {
		if (!dlg || !sceneAlive(scn)) return;
		dlg->hide();
		ComputeEulerDialog *p = this;
		parkTool(scn, dlg, "Compute Euler pole", IC_Line,
		         "Closed Compute Euler pole dialog — double-click to bring it back, "
		         "click for Show / Delete",
		         [p]() { p->unpark(); }, parkedMenu());
		unfoldSceneObjects(scn);
	}

	std::function<void(const QPoint &)> parkedMenu() {
		return [this](const QPoint &g) {
			QMenu m;
			QAction *aShow = m.addAction("Show");
			m.addSeparator();
			QAction *aDel = m.addAction("Delete");
			QAction *pick = m.exec(g);
			if (pick == aShow) unpark();
			else if (pick == aDel) {
				reallyClose = true;
				unparkTool(scn, dlg);
				dlg->deleteLater();
			}
		};
	}

	// Every LINE element of the window, in both combos — the same list (and the same rules about
	// what counts as a line) the Euler rotations dialog builds. What the user typed (or picked, or
	// browsed to) is kept; NOTHING is ever pre-selected — choosing the two lines is the user's job,
	// so both boxes stay empty until they say otherwise.
	void refillLines() {
		if (!scn || !sceneAlive(scn)) return;
		QStringList names;
		for (auto &pg : scn->polys) {
			if (pg.isFault || pg.isSlip || pg.nestKind != 0) continue;
			names << QString::fromStdString(pg.name);
		}
		for (auto &ov : scn->overlays) {
			if (ov.mode != 1) continue;
			names << QString::fromStdString(ov.name);
		}
		for (int k = 0; k < 2; ++k) {
			QComboBox *c = (k == 0) ? line1 : line2;
			if (!c) continue;
			const QString had = c->currentText().trimmed();
			QSignalBlocker b(c);
			c->clear();
			c->addItems(names);
			c->setCurrentIndex(-1);                  // no pre-selection, ever
			if (!had.isEmpty()) c->setCurrentText(had);   // a file path counts too, list or no list
		}
	}

	// Arm / disarm the "point at a line" pick. It is the SAME shared mechanism the Euler rotations
	// dialog's "Pick in view" uses (Scene::vectorPickMode == 1, resolved by vectorPickFire) — only the
	// mouse door differs: vectorPickDbl asks for a LEFT DOUBLE-CLICK, as this dialog's button says.
	void armPick(bool on) {
		if (!scn || !sceneAlive(scn)) return;
		if (!on) {
			if (scn->vectorPickMode == 1) vectorPickDisarm(scn);
			scn->vectorPickCB = nullptr;
			return;
		}
		refillLines();
		QPointer<QDialog> guard(dlg);
		scn->vectorPickCB = [this, guard](const std::string &names) {
			if (!guard) return;                            // dialog gone meanwhile: drop the answer
			const QString nm = QString::fromStdString(names).section('\n', 0, 0).trimmed();
			if (nm.isEmpty()) return;                      // clicked past every line: stay armed
			const QString l1 = line1 ? line1->currentText().trimmed() : QString();
			QComboBox *c = (line1 && l1.isEmpty()) ? line1 : line2;
			if (!c) return;
			if (c == line2 && nm == l1) return;            // the same line cannot be both
			c->setCurrentText(nm);
			const bool done = line1 && line2 && !line1->currentText().trimmed().isEmpty() &&
			                  !line2->currentText().trimmed().isEmpty();
			if (done) {
				setPickDown(false);                        // both picked: the button pops back up
				unpark();                                  // …and the dialog comes back on screen
			}
			else if (scn->win)
				scn->win->statusBar()->showMessage("Now double-click the SECOND line", 5000);
		};
		scn->vectorPickPrevShape = (int)scn->polyShape;
		scn->vectorPickMode = 1;
		scn->vectorPickDbl = true;
		scn->vectorPickDrawing = false;
		// Out of the way while the user works in the view — parked exactly as the X parks it, so the
		// Scene Objects row brings it back if they give up half way. It returns by itself on the
		// second pick.
		parkNow();
		if (scn->widget) scn->widget->setCursor(Qt::CrossCursor);
		if (scn->win) scn->win->statusBar()->showMessage(
			"Double-click each line in the figure — First Line, then Second Line. The button stays "
			"down until both are picked (press it again to give up)", 6000);
	}

	// One place that moves the button and the scene together, so they can never disagree about
	// whether a pick is running (the button being down IS the "pick in progress" indicator).
	void setPickDown(bool on) {
		if (pickBtn) {
			QSignalBlocker b(pickBtn);
			pickBtn->setChecked(on);
		}
		armPick(on);
	}

	// A "..." button that fills an editable combo with a file path. The combo's own line edit gets
	// the app-wide double-click-opens-the-chooser rule (fileBoxDoubleClick, 30_app.cpp), which works
	// by CLICKING that same button — so there is one file-picking path, never two that can drift.
	void wireFileBox(QDialog *d, const char *comboName, const char *btnName, const QString &title,
	                 const QString &filter) {
		auto *c = d->findChild<QComboBox *>(comboName);
		auto *b = d->findChild<QToolButton *>(btnName);
		if (!c || !b) return;
		QObject::connect(b, &QToolButton::clicked, d, [c, d, title, filter]() {
			const QString fn = QFileDialog::getOpenFileName(d, title, QString(), filter);
			if (!fn.isEmpty()) c->setCurrentText(fn);
		});
		fileBoxDoubleClick(c->lineEdit(), b);
	}

	// check_hellinger_CB: the two methods do not share their controls, so one switch swaps them.
	void syncHellinger() {
		const bool on = hellChk && hellChk->isChecked();
		for (QLineEdit *e : { lonRange, latRange, angRange }) if (e) e->setEnabled(!on);
		for (QWidget *w : { (QWidget *)errFile, (QWidget *)ncRadio, (QWidget *)vtkRadio,
		                    (QWidget *)showCubeChk })
			if (w) w->setVisible(!on);
		if (auto *lb = dlg->findChild<QLabel *>("text_resid_grid")) lb->setVisible(!on);
		if (auto *b = dlg->findChild<QToolButton *>("push_err_file")) b->setVisible(!on);
		for (QWidget *w : { (QWidget *)statsChk, (QWidget *)ellipseChk, (QWidget *)forceChk,
		                    (QWidget *)segChk })
			if (w) w->setVisible(on);
		if (on) {
			savedNLon = nLon ? nLon->text() : QString();
			savedNLat = nLat ? nLat->text() : QString();
			savedNAng = nAng ? nAng->text() : QString();
			if (nLon) { nLon->setText("8"); nLon->setToolTip(
				"Tolerance used to break the isochron into linear chunks (the Hellinger segments), in km"); }
			if (nLat) { nLat->setText(""); nLat->setReadOnly(true);
				nLat->setToolTip("Volume of the confidence region, in km3"); }
			if (nAng) nAng->setVisible(false);
			if (nIntLbl) nIntLbl->setText("DP tolerance");
		} else {
			if (nLon && !savedNLon.isEmpty()) nLon->setText(savedNLon);
			if (nLat) { nLat->setReadOnly(false); if (!savedNLat.isEmpty()) nLat->setText(savedNLat); }
			if (nAng) { nAng->setVisible(true); if (!savedNAng.isEmpty()) nAng->setText(savedNAng); }
			if (nIntLbl) nIntLbl->setText("N Points");
		}
	}

	// edit_nInt_CB: the box holds either a COUNT of points (forced odd, so the starting pole is one
	// of them) or the form "N*Delta" (e.g. 100*0.1), which sets the range as well — N is forced even
	// there, so the count N+1 is again odd. The tooltip reports the resulting spacing either way.
	// In Hellinger mode these boxes mean something else entirely, so the rule does not apply.
	void syncNInt(QLineEdit *ne, QLineEdit *re) {
		if (!ne || !re || (hellChk && hellChk->isChecked())) return;
		const QString s = ne->text().trimmed();
		if (s.isEmpty()) return;
		const int star = s.indexOf('*');
		double d = 0.0;
		int nPts = 0;
		if (star > 0) {
			bool okN = false, okD = false;
			const int nInt0 = s.left(star).toInt(&okN);
			d = s.mid(star + 1).toDouble(&okD);
			if (!okN || !okD || nInt0 <= 0 || d <= 0) { ne->setText("3"); return; }
			const int nInt = (nInt0 % 2) ? nInt0 + 1 : nInt0;      // an EVEN number of intervals
			nPts = nInt + 1;                                        // hence an ODD number of points
			re->setText(QString::number(nInt * d, 'g', 6));
		} else {
			nPts = qAbs(s.toInt());
			if (nPts <= 0) { ne->setText("3"); return; }
			if (!(nPts % 2)) nPts += 1;                             // an ODD number of points
			const double rang = re->text().trimmed().toDouble();
			d = (nPts > 1) ? rang / (nPts - 1) : 0.0;
		}
		QSignalBlocker b(ne);
		ne->setText(QString::number(nPts));
		ne->setToolTip(QString("The range interval is divided into this number of equally spaced points\n"
		                       "Alternatively use the form N*Delta (e.g. 100*0.1) to set up both range and "
		                       "resolution\nActual point spacing is = %1").arg(d, 0, 'g', 6));
	}

	void recyclePole() {
		if (pLon && fLon && !fLon->text().isEmpty()) pLon->setText(fLon->text());
		if (pLat && fLat && !fLat->text().isEmpty()) pLat->setText(fLat->text());
		if (pAng && fAng && !fAng->text().isEmpty()) pAng->setText(fAng->text());
	}

	// The search is running: Compute is out, STOP is in.
	void setRunning(bool on) {
		if (computeBtn) computeBtn->setEnabled(!on);
		if (stopBtn) stopBtn->setEnabled(on);
		if (!on && bar) bar->setValue(bar->maximum());
	}

	// gmtvtk_compute_euler_progress lands here: `cur` < 0 means the run is over.
	void progress(int cur, int nmax, const QString &txt) {
		const QStringList c = txt.split('\t');
		auto put = [&c](QLineEdit *e, int i) {
			if (e && i < c.size() && !c[i].isEmpty()) e->setText(c[i]);
		};
		put(fLon, 0); put(fLat, 1); put(fAng, 2); put(stRes, 3); put(bfRes, 4);
		if (c.size() > 5 && !c[5].isEmpty() && nLat) nLat->setText(c[5]);   // Hellinger's volume
		if (bar && nmax > 0) {
			bar->setMaximum(nmax);
			bar->setValue(cur < 0 ? nmax : cur);
		}
		if (cur < 0) {
			setRunning(false);
			g_ceulerRunning = nullptr;
			if (recycleBtn && fLon && !fLon->text().isEmpty()) recycleBtn->setVisible(true);
			refillLines();                       // the fitted line is an element of the window now
		}
	}

	void stop() {
		if (!g_juliaEuler) return;
		QStringList kv;
		kv << "op=ceuler_stop";
		g_juliaEuler(scn, kv.join("\n").toUtf8().constData());
		if (stopBtn) stopBtn->setEnabled(false);
	}

	void run() {
		if (!dlg) return;
		if (!g_juliaEuler) {
			QMessageBox::warning(dlg, "Compute Euler pole", "The Julia side is not connected.");
			return;
		}
		const QString l1 = line1 ? line1->currentText().trimmed() : QString();
		const QString l2 = line2 ? line2->currentText().trimmed() : QString();
		if (l1.isEmpty() || l2.isEmpty()) {
			QMessageBox::warning(dlg, "Compute Euler pole",
				"Compute an Euler pole with what? It would help if you provide me TWO lines.");
			return;
		}
		if (l1 == l2) {
			QMessageBox::warning(dlg, "Compute Euler pole", "The two lines must be different.");
			return;
		}
		auto num = [](QLineEdit *e) { return e ? e->text().trimmed() : QString(); };
		if (num(pLon).isEmpty() || num(pLat).isEmpty() || num(pAng).isEmpty()) {
			QMessageBox::warning(dlg, "Compute Euler pole",
				"I need a first guess of the Euler pole you are seeking for.\n"
				"Pay attention to the \"Starting Pole Section\".");
			return;
		}
		const bool hell = hellChk && hellChk->isChecked();
		const bool onlyRes = plotResChk && plotResChk->isChecked();

		// Clear the previous answer, exactly as push_compute_CB does before starting.
		for (QLineEdit *e : { fLon, fLat, fAng, stRes, bfRes }) if (e) e->clear();

		QStringList kv;
		kv << "op=ceuler";
		kv << "line1=" + l1 << "line2=" + l2;
		kv << "polelon=" + num(pLon) << "polelat=" + num(pLat) << "poleang=" + num(pAng);
		kv << "lonrange=" + num(lonRange) << "latrange=" + num(latRange) << "angrange=" + num(angRange);
		kv << "nlon=" + num(nLon) << "nlat=" + num(nLat) << "nang=" + num(nAng);
		kv << QString("hellinger=%1").arg(hell ? 1 : 0);
		kv << QString("plotres=%1").arg(onlyRes ? 1 : 0);
		kv << QString("loop=%1").arg(loopChk && loopChk->isChecked() ? 1 : 0);
		if (hell) {
			kv << "dptol=" + num(nLon);            // the N-points box IS the DP tolerance in this mode
			kv << QString("showstats=%1").arg(statsChk && statsChk->isChecked() ? 1 : 0);
			kv << QString("ellipse=%1").arg(ellipseChk && ellipseChk->isChecked() ? 1 : 0);
			kv << QString("forcepole=%1").arg(forceChk && forceChk->isChecked() ? 1 : 0);
			kv << QString("colorseg=%1").arg(segChk && segChk->isChecked() ? 1 : 0);
		} else {
			kv << "residfile=" + (errFile ? errFile->text().trimmed() : QString());
			kv << QString("residfmt=%1").arg(vtkRadio && vtkRadio->isChecked() ? "vtk" : "nc");
			kv << QString("showcube=%1").arg(showCubeChk && showCubeChk->isChecked() ? 1 : 0);
		}

		// Only the brute-force search runs in the background; the other two branches answer at once.
		const bool async = !hell && !onlyRes;
		if (async) {
			if (bar) { bar->setMaximum(qMax(1, num(nLon).toInt())); bar->setValue(0); }
			setRunning(true);
			g_ceulerRunning = this;
		}
		g_eulerResult.clear();
		const int ok = g_juliaEuler(scn, kv.join("\n").toUtf8().constData());
		const QString answer = QString::fromStdString(g_eulerResult);
		if (!ok) {
			if (async) { setRunning(false); g_ceulerRunning = nullptr; }
			QMessageBox::warning(dlg, "Compute Euler pole", answer.isEmpty()
				? QString("Failed — see this window's Errors console for details.") : answer);
			return;
		}
		if (!async) {
			refillLines();
			if (!answer.isEmpty()) {
				QMessageBox box(QMessageBox::Information, "Compute Euler pole", answer, QMessageBox::Ok, dlg);
				box.setTextInteractionFlags(Qt::TextSelectableByMouse);
				box.exec();
			}
			if (hell) {
				// The Hellinger answer came back through the same progress channel; nothing else to do.
				if (recycleBtn && fLon && !fLon->text().isEmpty()) recycleBtn->setVisible(true);
			}
		}
	}
};

// The one door Julia's worker pushes progress and results through (src/computeeuler.jl `_ce_push`).
// `txt` is tab-separated: pole lon, lat, angle, starting residue, best-fit residue [, volume].
void computeEulerProgress(int cur, int nmax, const QString &txt) {
	if (g_ceulerRunning && g_ceulerRunning->dlg) { g_ceulerRunning->progress(cur, nmax, txt); return; }
	// Not a background run (Hellinger, "plot residues only"): the dialog of any live window takes it.
	for (auto &kv : g_ceulerDlgs)
		if (kv.second && kv.second->dlg) { kv.second->progress(cur, nmax, txt); return; }
}

// ============================================================================================
// grdfilter (GMT menu) — filter a grid in the space domain. Layout is Mirone's Grdfilter window:
// the shared "Griding Line Geometry" block (adopted from the .ui's verbatim copy of
// grid_line_geometry.ui, so it behaves exactly as grdsample's), the Filter type + width, and the
// Distance flag. Loaded at RUNTIME via QUiLoader from deps/ui/grdfilter_dialog.ui.
//
// Beyond Mirone's three controls, from grdfilter.qmd: the full filter list (custom/operator weight
// grids, histogram mode, lower/upper and their signed variants), +h high-pass, +q quantile for the
// median, /binwidth and +c for the histogram mode, +l/+u for the mode filters, a second width for a
// RECTANGULAR filter, the -N NaN policy and -T registration toggle. The per-filter extras only light
// up for the filter that owns them, so the row never offers a knob the chosen filter ignores.
// ============================================================================================
class GrdFilterDialog {
public:
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	GeoGridGeometry *geo = nullptr;
	QComboBox *typeCb = nullptr, *distCb = nullptr, *nanCb = nullptr, *modeCb = nullptr;
	QLineEdit *widthEdit = nullptr, *width2Edit = nullptr, *quantEdit = nullptr, *binEdit = nullptr;
	QLineEdit *wgridEdit = nullptr, *outEdit = nullptr;
	QCheckBox *hpChk = nullptr, *centerChk = nullptr, *toggleChk = nullptr;

	explicit GrdFilterDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/grdfilter_dialog.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("GrdFilterDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("GrdFilterDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		dlg->setWindowTitle("grdfilter");
		QDialog *d = dlg;

		geo = GeoGridGeometry::adopt(d);       // the SAME block grdsample uses, wiring and all
		typeCb = d->findChild<QComboBox *>("cb_type");
		distCb = d->findChild<QComboBox *>("cb_dist");
		nanCb  = d->findChild<QComboBox *>("cb_nans");
		modeCb = d->findChild<QComboBox *>("cb_mode");
		widthEdit  = d->findChild<QLineEdit *>("edit_width");
		width2Edit = d->findChild<QLineEdit *>("edit_width2");
		quantEdit  = d->findChild<QLineEdit *>("edit_quantile");
		binEdit    = d->findChild<QLineEdit *>("edit_binwidth");
		wgridEdit  = d->findChild<QLineEdit *>("edit_wgrid");
		outEdit    = d->findChild<QLineEdit *>("edit_outfile");
		hpChk     = d->findChild<QCheckBox *>("chk_highpass");
		centerChk = d->findChild<QCheckBox *>("chk_centerbins");
		toggleChk = d->findChild<QCheckBox *>("chk_toggle");

		if (typeCb) {                                    // data = the GMT filter code letter
			typeCb->addItem("boxcar", "b");              typeCb->addItem("cosine arch", "c");
			typeCb->addItem("gaussian", "g");            typeCb->addItem("custom (weight grid)", "f");
			typeCb->addItem("operator (weight grid)", "o");
			typeCb->addItem("median", "m");              typeCb->addItem("mode (LMS)", "p");
			typeCb->addItem("histogram mode", "h");
			typeCb->addItem("lower", "l");               typeCb->addItem("lower, positives only", "L");
			typeCb->addItem("upper", "u");               typeCb->addItem("upper, negatives only", "U");
		}
		if (distCb) {
			distCb->addItem("p — pixels, width in odd # of pixels", "p");
			distCb->addItem("0 — grid units, Cartesian", "0");
			distCb->addItem("1 — degrees, width in km", "1");
			distCb->addItem("2 — degrees, km, dx scaled by cos(mid y)", "2");
			distCb->addItem("3 — degrees, km, dx scaled by cos(y)", "3");
			distCb->addItem("4 — degrees, km, spherical", "4");
			distCb->addItem("5 — Mercator img units, km, spherical", "5");
			// A projected/Cartesian grid must NOT get a "degrees" flag, so the default follows the grid:
			// geographic -> 1 (Mirone's own default), anything else -> 0. baseGeog is the flag the host
			// set when the grid was added (GMT.guessgeog), the same one the axes and xfac already use.
			distCb->setCurrentIndex((scene && scene->baseGeog) ? 2 : 1);
		}
		if (nanCb) {
			nanCb->addItem("ignore NaNs", "i");
			nanCb->addItem("NaN if any NaN in the circle", "p");
			nanCb->addItem("ignore, but keep input NaNs", "r");
		}
		if (modeCb) {
			modeCb->addItem("average", "");
			modeCb->addItem("lowermost", "l");
			modeCb->addItem("uppermost", "u");
		}

		// Prefill the geometry from the window's own grid — the standing rule for every region spec.
		if (geo && scene) {
			if (scene->gnx > 1 && scene->gny > 1)
				geo->fillGeometry(QString("%1/%2/%3/%4/%5/%6/%7/%8")
					.arg(scene->gx0).arg(scene->gx1).arg(scene->gy0).arg(scene->gy1)
					.arg(scene->gdx).arg(scene->gdy).arg(scene->gnx).arg(scene->gny));
			else if (scene->x1 > scene->x0 && scene->y1 > scene->y0)
				geo->fillGeometry(QString("%1/%2/%3/%4////").arg(scene->x0).arg(scene->x1)
					.arg(scene->y0).arg(scene->y1));
		}

		if (auto *wBtn = d->findChild<QToolButton *>("btn_wgrid")) {
			QObject::connect(wBtn, &QToolButton::clicked, d, [this, d]() {
				QString p = QFileDialog::getOpenFileName(d, "Select filter weight grid", prefStartDir(),
					"Grids (*.grd *.nc);;All files (*)");
				if (!p.isEmpty()) { wgridEdit->setText(p); rememberStartDir(p); }
			});
			if (wgridEdit) fileBoxDoubleClick(wgridEdit, wBtn);
		}
		if (auto *oBtn = d->findChild<QToolButton *>("btn_outfile")) {
			QObject::connect(oBtn, &QToolButton::clicked, d, [this, d]() {
				QString p = QFileDialog::getSaveFileName(d, "Save filtered grid", prefStartDir(),
					"Grids (*.grd *.nc);;All files (*)");
				if (!p.isEmpty()) { outEdit->setText(p); rememberStartDir(p); }
			});
			if (outEdit) fileBoxDoubleClick(outEdit, oBtn);
		}

		if (typeCb) QObject::connect(typeCb, QOverload<int>::of(&QComboBox::currentIndexChanged), d,
		                             [this]() { syncFilterExtras(); });
		if (distCb) QObject::connect(distCb, QOverload<int>::of(&QComboBox::currentIndexChanged), d,
		                             [this]() { syncFilterExtras(); });
		syncFilterExtras();

		for (QPushButton *b : d->findChildren<QPushButton *>()) { b->setAutoDefault(false); b->setDefault(false); }
		if (auto *b = d->findChild<QPushButton *>("push_compute")) QObject::connect(b, &QPushButton::clicked, d, [this, d]() { runCompute(d); });
		if (auto *b = d->findChild<QPushButton *>("push_close"))   QObject::connect(b, &QPushButton::clicked, d, [d]() { d->close(); });
		addManualButton(d, "grdfilter");           // the green ? disk, lower-left as everywhere else

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}

	QString code() const { return typeCb ? typeCb->currentData().toString() : QString("b"); }
	QString dist() const { return distCb ? distCb->currentData().toString() : QString("0"); }

	// Only the knobs the CHOSEN filter actually reads stay live: quantile is the median's, bin width
	// and Center bins the histogram mode's, the multiple-mode pick belongs to the two mode filters, and
	// the weight grid to custom/operator (which also have no width of their own). A rectangular filter
	// (second width) is only legal with distance flag p or 0.
	void syncFilterExtras() {
		const QString c = code();
		const bool isWeightGrid = (c == "f" || c == "o");
		const bool isMedian     = (c == "m");
		const bool isHistMode   = (c == "h");
		const bool isMode       = (c == "p" || c == "h");
		const bool rectOK       = (dist() == "p" || dist() == "0");
		if (quantEdit)  quantEdit->setEnabled(isMedian);
		if (binEdit)    binEdit->setEnabled(isHistMode);
		if (centerChk)  centerChk->setEnabled(isHistMode);
		if (modeCb)     modeCb->setEnabled(isMode);
		if (wgridEdit)  wgridEdit->setEnabled(isWeightGrid);
		if (auto *b = dlg->findChild<QToolButton *>("btn_wgrid")) b->setEnabled(isWeightGrid);
		if (widthEdit)  widthEdit->setEnabled(!isWeightGrid);
		if (width2Edit) width2Edit->setEnabled(!isWeightGrid && rectOK);
	}

	void runCompute(QDialog *d) {
		if (!g_juliaGrdFilter) {
			QMessageBox::warning(d, "grdfilter", "grdfilter: callback not registered (rebuild/restart needed?).");
			return;
		}
		const QString c = code();
		const bool isWeightGrid = (c == "f" || c == "o");
		// -F: <code><width>[/width2][+modifiers], or <code><weight grid> for custom/operator.
		QString F = c;
		if (isWeightGrid) {
			const QString wg = wgridEdit ? wgridEdit->text().trimmed() : QString();
			if (wg.isEmpty()) { QMessageBox::warning(d, "grdfilter", "This filter needs a weight grid."); return; }
			F += wg;
		} else {
			const QString w = widthEdit ? widthEdit->text().trimmed() : QString();
			if (w.isEmpty()) { QMessageBox::warning(d, "grdfilter", "Give the filter width."); return; }
			F += w;
			const QString w2 = (width2Edit && width2Edit->isEnabled()) ? width2Edit->text().trimmed() : QString();
			if (!w2.isEmpty()) F += "/" + w2;
			if (c == "h" && binEdit && !binEdit->text().trimmed().isEmpty()) F += "/" + binEdit->text().trimmed();
		}
		if (c == "m" && quantEdit && !quantEdit->text().trimmed().isEmpty()) F += "+q" + quantEdit->text().trimmed();
		if (c == "h" && centerChk && centerChk->isChecked()) F += "+c";
		if ((c == "p" || c == "h") && modeCb && !modeCb->currentData().toString().isEmpty())
			F += "+" + modeCb->currentData().toString();
		if (hpChk && hpChk->isChecked()) F += "+h";

		QStringList kv;
		kv << "filter=" + F;
		kv << "distance=" + dist();
		if (geo) {
			kv << "region=" + geo->region();
			kv << "inc=" + geo->inc();
		}
		if (nanCb && nanCb->currentData().toString() != "i") kv << "nans=" + nanCb->currentData().toString();
		if (toggleChk && toggleChk->isChecked()) kv << "toggle=1";
		if (outEdit && !outEdit->text().trimmed().isEmpty()) kv << "outfile=" + outEdit->text().trimmed();
		kv << "grid=" + QString::fromStdString(activeGridName(scn));   // filter the DISPLAYED layer
		showBusyDialog("Filtering…");
		const int ok = g_juliaGrdFilter(scn, kv.join("\n").toUtf8().constData());
		closeBusyDialog();
		if (!ok) QMessageBox::warning(d, "grdfilter",
		                              "grdfilter failed — see this window's Errors console for details.");
	}
};

// ============================================================================================
// Interpolation / griding (GMT menu) — grid an x,y,z table. Layout is Mirone's Surface window
// (src_figs/griding_mir.m): Input Data File (+ header count), the shared "Griding Line Geometry"
// block (adopted from the .ui's verbatim copy of grid_line_geometry.ui, so it behaves exactly as
// grdsample's), the "For Near Neighbor only" search radius + units, the Griding Method combo with
// its Options button, and Verbose / Plot pts / Compute. Loaded at RUNTIME via QUiLoader from
// deps/ui/interpolation_dialog.ui.
//
// Mirone's method list is followed except for "Minimum Curvature - mbgrid", which is not a GMT
// module; blockmode, greenspline and sphinterpolate are added because GMT grids with them too.
//
// The per-method Options window (Mirone's "Surface op..." dialog) is NOT a second dialog class: the
// rows differ per module, so ONE builder walks the option table `optionSpec(method)` and produces
// the window for whichever method is selected. Values survive switching method and re-opening the
// window (optVals), and travel to Julia as one "opt_<kwarg>=<value>" line each — the Julia side maps
// them to GMT.jl keywords, so the option NAMES here are GMT.jl keyword names, not option letters.
// ============================================================================================
class InterpolationDialog {
public:
	// One row of a method's Options window. `key` is the GMT.jl keyword it feeds; `flag` is the GMT
	// option letter shown to its right, exactly as Mirone's options window does.
	struct Opt {
		QString key, label, flag, kind, deflt, tip;
		QStringList items;      // combo only: "shown text|data" pairs
	};

	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	GeoGridGeometry *geo = nullptr;
	QComboBox *methodCb = nullptr, *unitsCb = nullptr, *coordsCb = nullptr;
	QLineEdit *inEdit = nullptr, *nhEdit = nullptr, *radiusEdit = nullptr, *outEdit = nullptr;
	QCheckBox *hdrChk = nullptr, *toggleChk = nullptr, *pixelChk = nullptr,
	          *verboseChk = nullptr, *plotChk = nullptr;
	QGroupBox *nnGroup = nullptr;
	QMap<QString, QMap<QString, QString>> optVals;    // method -> (option key -> value as typed)

	explicit InterpolationDialog(QWidget *parent, Scene *scene) : scn(scene) {
		QUiLoader loader;
		QFile f(gmtvtkUiDir() + "/interpolation_dialog.ui");
		if (!f.open(QFile::ReadOnly)) {
			qWarning("InterpolationDialog: cannot open %s", qUtf8Printable(f.fileName()));
			return;
		}
		dlg = qobject_cast<QDialog *>(loader.load(&f, parent));
		f.close();
		if (!dlg) { qWarning("InterpolationDialog: QUiLoader failed to load the .ui"); return; }
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setWindowModality(Qt::NonModal);
		dlg->setWindowTitle("Interpolate");
		QDialog *d = dlg;

		geo = GeoGridGeometry::adopt(d);       // the SAME block grdsample uses, wiring and all
		methodCb = d->findChild<QComboBox *>("cb_method");
		unitsCb  = d->findChild<QComboBox *>("cb_units");
		coordsCb = d->findChild<QComboBox *>("cb_coords");
		inEdit     = d->findChild<QLineEdit *>("edit_infile");
		nhEdit     = d->findChild<QLineEdit *>("edit_nheaders");
		radiusEdit = d->findChild<QLineEdit *>("edit_radius");
		outEdit    = d->findChild<QLineEdit *>("edit_outfile");
		hdrChk     = d->findChild<QCheckBox *>("chk_headers");
		toggleChk  = d->findChild<QCheckBox *>("chk_toggle");
		pixelChk   = d->findChild<QCheckBox *>("chk_pixel");
		verboseChk = d->findChild<QCheckBox *>("chk_verbose");
		plotChk    = d->findChild<QCheckBox *>("chk_plotpts");
		nnGroup    = d->findChild<QGroupBox *>("gb_nn");

		if (methodCb) {                                  // data = the GMT module that does the work
			methodCb->addItem("Minimum Curvature - surface", "surface");
			methodCb->addItem("Delaunay Triangulation", "triangulate");
			methodCb->addItem("Near Neighbor", "nearneighbor");
			methodCb->addItem("Median", "blockmedian");
			methodCb->addItem("Mean", "blockmean");
			methodCb->addItem("Mode", "blockmode");
			methodCb->addItem("Green's spline", "greenspline");
			methodCb->addItem("Spherical surface (sphinterpolate)", "sphinterpolate");
		}
		if (unitsCb) {
			unitsCb->addItem("data units", "");          // no unit letter: the radius is in x,y units
			unitsCb->addItem("degrees", "d");            unitsCb->addItem("arc minutes", "m");
			unitsCb->addItem("arc seconds", "s");        unitsCb->addItem("meters", "e");
			unitsCb->addItem("kilometers", "k");         unitsCb->addItem("miles", "M");
			unitsCb->addItem("nautical miles", "n");     unitsCb->addItem("feet", "f");
		}
		if (coordsCb) {
			coordsCb->addItem("auto", "auto");           // Julia asks GMT.guessgeog about the data
			coordsCb->addItem("geographic", "geog");
			coordsCb->addItem("cartesian", "cart");
		}

		// The window's own grid (when there is one) seeds the geometry, as every region spec does.
		// Picking a data file below overwrites it with THAT file's limits.
		if (geo && scene) {
			if (scene->gnx > 1 && scene->gny > 1)
				geo->fillGeometry(QString("%1/%2/%3/%4/%5/%6/%7/%8")
					.arg(scene->gx0).arg(scene->gx1).arg(scene->gy0).arg(scene->gy1)
					.arg(scene->gdx).arg(scene->gdy).arg(scene->gnx).arg(scene->gny));
		}

		if (auto *inBtn = d->findChild<QToolButton *>("btn_infile")) {
			QObject::connect(inBtn, &QToolButton::clicked, d, [this, d]() {
				QString p = QFileDialog::getOpenFileName(d, "Select x,y,z data file", prefStartDir(),
					"Data tables (*.dat *.txt *.xyz *.csv);;All files (*)");
				if (p.isEmpty()) return;
				inEdit->setText(p);
				rememberStartDir(p);
				fillFromData(p);
			});
			if (inEdit) fileBoxDoubleClick(inEdit, inBtn);
		}
		if (auto *oBtn = d->findChild<QToolButton *>("btn_outfile")) {
			QObject::connect(oBtn, &QToolButton::clicked, d, [this, d]() {
				QString p = QFileDialog::getSaveFileName(d, "Save gridded result", prefStartDir(),
					"Grids (*.grd *.nc);;All files (*)");
				if (!p.isEmpty()) { outEdit->setText(p); rememberStartDir(p); }
			});
			if (outEdit) fileBoxDoubleClick(outEdit, oBtn);
		}

		if (pixelChk) QObject::connect(pixelChk, &QCheckBox::toggled, d, [this](bool on) {
			if (geo) geo->setRegistration(on);           // the dim-fun must know which registration
		});
		if (methodCb) QObject::connect(methodCb, QOverload<int>::of(&QComboBox::currentIndexChanged), d,
		                               [this]() { syncMethod(); });
		syncMethod();

		for (QPushButton *b : d->findChildren<QPushButton *>()) { b->setAutoDefault(false); b->setDefault(false); }
		if (auto *b = d->findChild<QPushButton *>("push_options")) QObject::connect(b, &QPushButton::clicked, d, [this, d]() { showOptions(d); });
		if (auto *b = d->findChild<QPushButton *>("push_compute")) QObject::connect(b, &QPushButton::clicked, d, [this, d]() { runCompute(d); });
		if (auto *b = d->findChild<QPushButton *>("push_close"))   QObject::connect(b, &QPushButton::clicked, d, [d]() { d->close(); });
		// The green ? disk opens the page of the module CURRENTLY selected, not a fixed one.
		addManualButton(d, [this]() { return method(); });

		QObject::connect(d, &QObject::destroyed, d, [this]() { delete this; });
	}

	QString method() const { return methodCb ? methodCb->currentData().toString() : QString("surface"); }

	// Only Near Neighbor reads the search-radius group (it is that module's REQUIRED -S).
	void syncMethod() {
		if (nnGroup) nnGroup->setEnabled(method() == "nearneighbor");
	}

	// A data file's own limits (and a first guess at the spacing) fill the geometry boxes — the same
	// g_juliaGridMeta callback the "OR Ref grid" picker uses, which reads a table as happily as a grid.
	void fillFromData(const QString &path) {
		if (!geo || !g_juliaGridMeta || path.isEmpty()) return;
		const char *m = g_juliaGridMeta(path.toUtf8().constData());
		if (m && *m) geo->fillGeometry(QString::fromUtf8(m));
	}

	// The option rows of each griding module: GMT.jl keyword, label, the GMT flag Mirone shows next to
	// the box, the widget kind, and the initial value. Everything a module does NOT offer simply is not
	// in its list, so no window ever shows a knob its module ignores.
	static QVector<Opt> optionSpec(const QString &m) {
		QVector<Opt> v;
		if (m == "surface") {
			v.push_back({"mask", "Max radius", "-M", "text", "", "Blank the nodes farther than this from a data point", {}});
			v.push_back({"maskcells", "Clip cells", "-M", "text", "", "Same as Max radius but counted in CELLS around a constrained node (0 = only its own cell)", {}});
			v.push_back({"aspect_ratio", "Aspect ratio", "-A", "text", "1", "dy = dx / ar. 'm' uses the cosine of the mean latitude", {}});
			v.push_back({"convergence", "Convergence limit", "-C", "text", "", "Iteration stops below this max change; append % for a fraction of the data rms", {}});
			v.push_back({"lower", "Lower limit", "-Ll", "text", "", "A value, a grid file, or 'd' for the minimum data value", {}});
			v.push_back({"upper", "Upper limit", "-Lu", "text", "", "A value, a grid file, or 'd' for the maximum data value", {}});
			v.push_back({"iterations", "Max iterations", "-N", "text", "500", "Stop after this many iterations even if not converged", {}});
			v.push_back({"suggest", "Suggest grid dimensions", "-Q", "check", "0", "Report the sizes with a highly composite factor (nothing is gridded)", {}});
			v.push_back({"search_radius", "Search radius", "-S", "text", "0.0", "Only used to initialize the grid before the first iteration", {}});
			v.push_back({"tension_i", "Internal Tension", "-Ti", "text", "0", "0 (minimum curvature) to 1 (harmonic). ~0.25 for potential fields, ~0.35 for topography", {}});
			v.push_back({"tension_b", "Boundary Tension", "-Tb", "text", "0", "Tension in the boundary condition, 0 to 1", {}});
			v.push_back({"over_relaxation", "Relaxation Factor", "-Z", "text", "1.4", "1 to 2. Larger converges faster but may go unstable", {}});
			v.push_back({"breakline", "Breakline file", "-D", "file", "", "x,y,z line whose vertices constrain the nearest nodes directly", {}});
			v.push_back({"preproc", "Pre-process", "", "combo", "", "Decimate the data per cell first, as the manual strongly advises",
			             {"none|", "blockmean|blockmean", "blockmedian|blockmedian", "blockmode|blockmode"}});
		}
		else if (m == "nearneighbor") {
			v.push_back({"sectors", "Sectors  n[/n_min]", "-N", "text", "", "Split the search circle in n sectors; a node needs data in at least n_min of them [4/4]", {}});
			v.push_back({"empty", "Value at empty nodes", "-E", "text", "", "What a node with no data in range gets [NaN]", {}});
			v.push_back({"weights", "4th column holds weights", "-W", "check", "0", "Multiply the geometric weights by a per-point weight read from column 4", {}});
		}
		else if (m == "triangulate") {
			v.push_back({"empty", "Value at empty nodes", "-E", "text", "", "What a node outside the triangulation gets [NaN]", {}});
		}
		else if (m == "blockmean" || m == "blockmedian" || m == "blockmode") {
			const QString stat = (m == "blockmean") ? "mean" : (m == "blockmedian" ? "median" : "mode");
			const QString spread = (m == "blockmean") ? "std" : "scale";
			v.push_back({"field", "Field", "-A", "combo", stat, "Which per-block quantity becomes the grid",
			             {stat + "|" + stat, spread + "|" + spread, "highest|highest", "lowest|lowest", "weights|weights"}});
			v.push_back({"center", "Use block center as location", "-C", "check", "0", "Report the block's center instead of the mean position of its points", {}});
			v.push_back({"weights", "Weighted input (4th column)", "-W", "check", "0", "Column 4 holds the weight of each point", {}});
			if (m == "blockmedian")
				v.push_back({"quantile", "Quantile", "-T", "text", "", "Return this quantile (0-1) instead of the median (0.5)", {}});
			if (m == "blockmode")
				v.push_back({"histogram_binning", "Histogram binning width", "-D", "text", "", "Bin the data and return the modal bin instead of the LMS mode", {}});
		}
		else if (m == "greenspline") {
			v.push_back({"distmode", "Distance mode", "-Z", "combo", "1", "How distances between data points are measured",
			             {"1 - x,y user units, Cartesian|1", "2 - x,y degrees, Flat Earth|2",
			              "3 - x,y degrees, spherical km|3", "4 - x,y degrees, great-circle cosines|4"}});
			v.push_back({"splines", "Spline type", "-S", "combo", "t", "Which Green function is summed",
			             {"c - minimum curvature|c", "t - continuous curvature in tension|t",
			              "r - regularized in tension|r", "l - linear / bilinear|l",
			              "p - spherical minimum curvature (Parker)|p",
			              "q - spherical in tension (Wessel & Becker)|q"}});
			v.push_back({"tension", "Tension (for t, r, q)", "-S", "text", "", "Tension factor 0-1 appended to the spline directive", {}});
			v.push_back({"approx", "Approximate: eigenvalue cutoff", "-C", "text", "", "Solve by SVD and keep only the eigenvalues above this cutoff", {}});
			v.push_back({"leave_trend", "Leave the trend alone", "-L", "check", "0", "Do not remove/restore a trend before/after the interpolation", {}});
		}
		else if (m == "sphinterpolate") {
			v.push_back({"tension", "Tension mode", "-Q", "combo", "p", "How tension factors are computed",
			             {"p - piecewise linear|p", "l - local smooth|l", "g - global smooth|g", "s - smoothing|s"}});
			v.push_back({"var_tension", "Variable tension", "-T", "check", "0", "Use variable rather than constant tension", {}});
			v.push_back({"skipdup", "Skip duplicate points", "-D", "check", "0", "The spherical algorithm cannot take duplicates", {}});
			v.push_back({"scale", "Scale data by max range", "-Z", "check", "0", "Normalize the z range before interpolating", {}});
		}
		return v;
	}

	// The value a row currently holds (what the user typed, else the spec's default).
	QString optValue(const QString &m, const Opt &o) const {
		const QMap<QString, QString> vals = optVals.value(m);
		return vals.contains(o.key) ? vals.value(o.key) : o.deflt;
	}

	// ONE builder for every method's options window (Mirone's "Surface op..." dialog): a row per entry
	// of optionSpec(), the GMT flag to its right, and the assembled summary at the bottom. OK stores the
	// values against the method; Cancel leaves them as they were.
	void showOptions(QDialog *parent) {
		const QString m = method();
		const QVector<Opt> spec = optionSpec(m);
		if (spec.isEmpty()) return;

		QDialog od(parent);
		od.setWindowTitle(m + " options");
		auto *outer = new QVBoxLayout(&od);
		auto *grid = new QGridLayout();
		grid->setHorizontalSpacing(8);
		grid->setVerticalSpacing(4);
		QVector<QWidget *> widgets;
		QLineEdit *summary = new QLineEdit(&od);
		summary->setReadOnly(true);

		int row = 0;
		for (const Opt &o : spec) {
			const QString val = optValue(m, o);
			QWidget *w = nullptr;
			if (o.kind == "check") {
				auto *c = new QCheckBox(o.label, &od);
				c->setChecked(val == "1");
				grid->addWidget(c, row, 0, 1, 2);
				w = c;
			}
			else if (o.kind == "combo") {
				auto *c = new QComboBox(&od);
				for (const QString &it : o.items) {
					const int bar = it.lastIndexOf('|');
					c->addItem(it.left(bar), it.mid(bar + 1));
				}
				const int ix = c->findData(val);
				c->setCurrentIndex(ix >= 0 ? ix : 0);
				grid->addWidget(new QLabel(o.label, &od), row, 0);
				grid->addWidget(c, row, 1);
				w = c;
			}
			else {                                    // "text" and "file" share the box; file adds a "..."
				auto *e = new QLineEdit(val, &od);
				e->setMinimumWidth(o.kind == "file" ? 220 : 90);
				if (o.kind != "file") e->setMaximumWidth(120);
				grid->addWidget(new QLabel(o.label, &od), row, 0);
				grid->addWidget(e, row, 1);
				if (o.kind == "file") {
					auto *b = new QToolButton(&od);
					b->setText("...");
					QObject::connect(b, &QToolButton::clicked, &od, [e, &od]() {
						QString p = QFileDialog::getOpenFileName(&od, "Select file", prefStartDir(), "All files (*)");
						if (!p.isEmpty()) { e->setText(p); rememberStartDir(p); }
					});
					grid->addWidget(b, row, 3);
					fileBoxDoubleClick(e, b);
				}
				w = e;
			}
			if (!o.tip.isEmpty()) w->setToolTip(o.tip);
			auto *flagLab = new QLabel(o.flag, &od);
			flagLab->setStyleSheet("font-weight: bold;");
			grid->addWidget(flagLab, row, 2);
			widgets.push_back(w);
			++row;
		}
		outer->addLayout(grid);
		outer->addWidget(summary);

		auto *btnRow = new QHBoxLayout();
		auto *cancelBtn = new QPushButton("Cancel", &od);
		auto *okBtn = new QPushButton("OK", &od);
		cancelBtn->setAutoDefault(false);  cancelBtn->setDefault(false);
		okBtn->setAutoDefault(false);      okBtn->setDefault(false);
		btnRow->addStretch(1);
		btnRow->addWidget(cancelBtn);
		btnRow->addWidget(okBtn);
		outer->addLayout(btnRow);
		addManualButton(&od, btnRow, m);

		// Harvest the widgets into a (key -> value) map — used both for the live summary and for OK.
		auto harvest = [&spec, &widgets]() {
			QMap<QString, QString> out;
			for (int i = 0; i < spec.size() && i < widgets.size(); ++i) {
				const Opt &o = spec[i];
				if (o.kind == "check")
					out[o.key] = static_cast<QCheckBox *>(widgets[i])->isChecked() ? "1" : "0";
				else if (o.kind == "combo")
					out[o.key] = static_cast<QComboBox *>(widgets[i])->currentData().toString();
				else
					out[o.key] = static_cast<QLineEdit *>(widgets[i])->text().trimmed();
			}
			return out;
		};
		auto refresh = [this, m, &harvest, summary]() {
			const QMap<QString, QString> cur = harvest();
			QStringList parts;
			for (const Opt &o : optionSpec(m)) {
				const QString v = cur.value(o.key);
				if (v.isEmpty()) continue;
				if (o.kind == "check" && v != "1") continue;
				parts << (o.flag.isEmpty() ? o.key + "=" + v : (o.kind == "check" ? o.flag : o.flag + v));
			}
			summary->setText(parts.join(' '));
		};
		for (QWidget *w : widgets) {
			if (auto *e = qobject_cast<QLineEdit *>(w))      QObject::connect(e, &QLineEdit::textChanged, &od, [&refresh]() { refresh(); });
			else if (auto *c = qobject_cast<QCheckBox *>(w)) QObject::connect(c, &QCheckBox::toggled, &od, [&refresh]() { refresh(); });
			else if (auto *b = qobject_cast<QComboBox *>(w)) QObject::connect(b, QOverload<int>::of(&QComboBox::currentIndexChanged), &od, [&refresh]() { refresh(); });
		}
		refresh();

		QObject::connect(cancelBtn, &QPushButton::clicked, &od, &QDialog::reject);
		QObject::connect(okBtn, &QPushButton::clicked, &od, &QDialog::accept);
		if (od.exec() == QDialog::Accepted) optVals[m] = harvest();
	}

	void runCompute(QDialog *d) {
		if (!g_juliaInterpolate) {
			QMessageBox::warning(d, "Interpolate", "Interpolate: callback not registered (rebuild/restart needed?).");
			return;
		}
		const QString in = inEdit ? inEdit->text().trimmed() : QString();
		if (in.isEmpty()) { QMessageBox::warning(d, "Interpolate", "Give the x,y,z data file to grid."); return; }
		const QString m = method();
		if (m == "nearneighbor" && (!radiusEdit || radiusEdit->text().trimmed().isEmpty())) {
			QMessageBox::warning(d, "Interpolate", "Near Neighbor needs a search radius.");
			return;
		}
		QString R, I;
		if (geo) { R = geo->region();  I = geo->inc(); }
		if (I.isEmpty() || R.contains("//") || R.startsWith('/') || R.endsWith('/')) {
			QMessageBox::warning(d, "Interpolate", "Give the output region and spacing (Griding Line Geometry).");
			return;
		}

		QStringList kv;
		kv << "method=" + m;
		kv << "infile=" + in;
		kv << "region=" + R;
		kv << "inc=" + I;
		if (hdrChk && hdrChk->isChecked() && nhEdit && !nhEdit->text().trimmed().isEmpty())
			kv << "headers=" + nhEdit->text().trimmed();
		if (pixelChk && pixelChk->isChecked())   kv << "pixel=1";
		if (toggleChk && toggleChk->isChecked()) kv << "toggle=1";
		if (coordsCb) kv << "coords=" + coordsCb->currentData().toString();
		if (m == "nearneighbor" && radiusEdit)
			kv << "radius=" + radiusEdit->text().trimmed() + (unitsCb ? unitsCb->currentData().toString() : QString());
		if (verboseChk && verboseChk->isChecked()) kv << "verbose=1";
		if (plotChk && plotChk->isChecked())       kv << "plotpts=1";
		if (outEdit && !outEdit->text().trimmed().isEmpty()) kv << "outfile=" + outEdit->text().trimmed();
		// One line per option row that carries a value — a blank box means "let the module default".
		// A ticked box travels as the word "true", never as "1": several options take a NUMBER whose
		// legitimate value is 1 (greenspline's distance mode, nearneighbor's sectors), and the Julia
		// side cannot tell a flag from a value once they look alike.
		for (const Opt &o : optionSpec(m)) {
			const QString v = optValue(m, o);
			if (v.isEmpty()) continue;
			if (o.kind == "check") {
				if (v == "1") kv << "opt_" + o.key + "=true";
				continue;
			}
			kv << "opt_" + o.key + "=" + v;
		}

		showBusyDialog("Griding…");
		const int ok = g_juliaInterpolate(scn, kv.join("\n").toUtf8().constData());
		closeBusyDialog();
		if (!ok) QMessageBox::warning(d, "Interpolate",
		                              "Griding failed — see this window's Errors console for details.");
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
static void geoLineLenAz(double lon1, double lat1, double lon2, double lat2, double &km, double &az) {
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
static bool faultLineGeom(Scene *s, double &len, double &az, bool &geog) {
	int pi = -1;
	for (size_t i = 0; i < s->polys.size(); ++i) if (s->polys[i].isFault) { pi = (int)i; break; }
	if (pi < 0 || s->polys[pi].v.size() < 2) return false;
	const auto &v = s->polys[pi].v;
	geog = s->crsProj4.find("longlat") != std::string::npos || s->crsProj4.find("latlong") != std::string::npos;
	if (s->crsProj4.empty()) {                                  // unknown CRS -> crude range test
		double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
		for (auto &p : v) { x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]); y0 = std::min(y0, p[1]); y1 = std::max(y1, p[1]); }
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
			for (auto &p : v) ts << p[0] << ' ' << p[1] << '\n';
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
	const auto &A = v.front(); const auto &B = v.back();
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
	Polygon &pg = s->polys[pi];
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
static void faultBuildPlanePD(vtkPolyData *pd, const std::vector<std::array<double,3>> &corners) {
	vtkNew<vtkPoints> pts;
	for (auto &c : corners) pts->InsertNextPoint(c[0], c[1], c[2]);
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
								   const std::array<double,3> &t0, const std::array<double,3> &t1,
								   const std::array<double,3> &b0, const std::array<double,3> &b1) {
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
	Polygon &pg = s->polys[pi];
	const double D2R = 3.14159265358979323846 / 180.0;
	const double off  = width * std::cos(dip * D2R);               // down-dip horizontal projection (W·cos dip)
	const double vert = width * std::sin(dip * D2R);               // down-dip vertical drop      (W·sin dip)
	const auto &A = pg.v.front();  const auto &B = pg.v.back();     // trace endpoints (the long / top edge)

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
		auto toR = [&](const std::array<double,3> &p, double o[3]){ o[0]=p[0]*sx; o[1]=p[1]; o[2]=p[2]*sz; };
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
		if (scn) for (auto &pg : scn->polys) if (pg.isSlip && !pg.v.empty()) {
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
		const double wid = fWid->text().toDouble(), dip = fDip->text().toDouble();
		const double strk = fStrike->text().toDouble(), rake = dRake->text().toDouble();
		// PERSIST the geometry onto the fault polygon (first isFault — the same one faultUpdatePlane
		// targets) so Save Session writes real width/dip/strike/rake/depth-to-top, not NaN. Without this
		// a drawn fault round-trips as a bare trace: the plane only reappears after re-opening this dialog.
		for (auto &pg : scn->polys) if (pg.isFault) {
			pg.faultWidth = wid; pg.faultDip = dip; pg.faultStrike = strk; pg.faultRake = rake;
			pg.faultDepthTop = fDepTop->text().toDouble();
			break;
		}
		faultUpdatePlane(scn, wid, dip, strk, rake,
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
			for (auto &pg : scene->polys) if (pg.isFault) {
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
			} else if (scn) for (auto &pg : scn->polys) if (pg.isFault && !pg.v.empty()) {
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
	dlg->onAction = [s](const QString &params) {
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

// Intercepts the X on the cube-layer dock: closing it would make the cube's layers unreachable, so
// instead of hiding/destroying, the X RE-DOCKS the panel into the window. The X stays visible (the
// dock is Closable); this filter just swallows the close and docks it.
class CubeDockCloseFilter : public QObject {
public:
	QDockWidget *dock;
	explicit CubeDockCloseFilter(QDockWidget *d) : QObject(d), dock(d) {}
	bool eventFilter(QObject *obj, QEvent *ev) override {
		if (obj == dock && ev->type() == QEvent::Close) {
			ev->ignore();
			dock->setFloating(false);   // re-dock into its home (bottom) area
			dock->show();
			return true;                // swallow the close -> never destroyed
		}
		return QObject::eventFilter(obj, ev);
	}
};

// ── Cube Layer Selector Dialog ───────────────────────────────────────────────────────────────
// Non-modal dialog for scrubbing through layers of a 3-D NetCDF cube: a native QScrollBar (arrow
// buttons at each end, like the original Mirone tool) plus an editable QSpinBox, kept in sync both
// ways, plus a "Scale color to global min/max" checkbox. Every change fires the Julia callback
// LIVE -- cheap, since each call reads exactly ONE 2-D layer off disk, never the whole cube.
// Loaded from cube_layer_selector.ui via QUiLoader (same as IgrfDialog).
//
// Lifetime is PER-SCENE (Scene::cubeDlg), using the exact safe idiom already proven by
// elasticDlg/focalStudioDlg: WA_DeleteOnClose frees the QDialog; a `destroyed -> null the slot`
// connection (context = s->win) clears the Scene pointer when the dialog OR its parent window
// dies. NO global singleton, NO wrapper object, NO `delete this` -- all state lives in the widgets
// and capture-by-value lambdas. The previous design (a global g_cubeLayerDlg wrapper that
// close()d + delete'd a prior instance, and fired the first layer synchronously from its own
// constructor deep inside the Julia drop callback) crashed with a use-after-free / reentrancy
// access violation (QWidget handleClose / notifyInternal2); this is the rewrite.
static void showCubeLayerDialog(Scene *s, const QString &cubeName, int nLayers) {
	if (!sceneAlive(s) || !s->win || nLayers <= 0) return;

	// Re-drop into a window that already has the cube dock open: just re-point its range at the new
	// cube and raise it.
	if (s->cubeDlg) {
		auto *sb  = s->cubeDlg->findChild<QScrollBar *>("layerScrollBar");
		auto *spn = s->cubeDlg->findChild<QSpinBox  *>("layerSpin");
		auto *chk = s->cubeDlg->findChild<QCheckBox  *>("globalScaleCheck");
		auto *lbl = s->cubeDlg->findChild<QLabel     *>("infoLabel");   // optional (absent in current .ui)
		if (sb)  { sb->setRange(1, nLayers);  sb->setValue(1); }
		if (spn) { spn->setRange(1, nLayers); spn->setValue(1); }
		if (lbl) lbl->setText(QString("3D Cube: %1 (%2 layers)").arg(cubeName).arg(nLayers));
		// New cube in this dock: it is NOT in RAM yet (Julia cleared _CUBE_RAM on the fresh drop),
		// so re-enable the "Load all in RAM" button.
		if (auto *rb = s->cubeDlg->findChild<QPushButton *>("loadRamBtn")) {
			rb->setText("Load all in RAM");
			rb->setEnabled(true);
		}
		s->cubeDlg->show();
		s->cubeDlg->raise();
		if (sb && chk && g_juliaCubeLayer) g_juliaCubeLayer(s, sb->value() - 1, chk->isChecked() ? 1 : 0);   // show layer 1 of the new cube
		return;
	}

	QMainWindow *mw = s->win;   // the viewer window is a QMainWindow (hosts objDock etc.)

	if (g_juliaCubeLayer) g_juliaCubeLayer(s, 0, 0);   // display layer 1 BEFORE building the panel

	QUiLoader loader;
	QFile f(gmtvtkUiDir() + "/cube_layer_selector.ui");
	if (!f.open(QFile::ReadOnly)) {
		qWarning("CubeLayerDialog: cannot open %s", qUtf8Printable(f.fileName()));
		return;
	}
	// The cube layer selector lives INSIDE the viewer window as a bottom QDockWidget (same idiom as
	// the Scene Objects / shading docks) -- NOT a separate floating window that covered or sat beside
	// the image. Load the .ui content and host it in the dock (flag it Qt::Widget so the loaded
	// QDialog embeds as a plain panel instead of trying to be its own window).
	QWidget *content = qobject_cast<QWidget *>(loader.load(&f, mw));
	f.close();
	if (!content) { qWarning("CubeLayerDialog: QUiLoader failed to load the .ui"); return; }
	content->setWindowFlags(Qt::Widget);

	auto *sb  = content->findChild<QScrollBar *>("layerScrollBar");
	auto *spn = content->findChild<QSpinBox  *>("layerSpin");
	auto *chk = content->findChild<QCheckBox  *>("globalScaleCheck");
	auto *lbl = content->findChild<QLabel     *>("infoLabel");   // OPTIONAL title label -- the current
	// cube_layer_selector.ui has no such widget (only "layerNoLabel"), so it must NOT be required:
	// gating the whole thing on it (as the old code did) silently aborted creation -> nothing showed.
	// Only the three CONTROLS are mandatory.
	if (!sb || !spn || !chk) {
		qWarning("CubeLayerDialog: failed to find controls (scrollbar/spin/check)");
		content->deleteLater();
		return;
	}

	// Windows 11's native style draws scrollbar arrow buttons hairline-thin (only clearly visible on
	// hover) -- force classic Win32 chrome on JUST this scrollbar so the end arrows are always
	// visible, like the original Mirone slider. QStyle parented to the scrollbar -> auto-freed.
	if (QStyle *classicStyle = QStyleFactory::create("windowsvista")) {
		classicStyle->setParent(sb);
		sb->setStyle(classicStyle);
	}

	if (lbl) lbl->setText(QString("3D Cube: %1 (%2 layers)").arg(cubeName).arg(nLayers));
	sb->setRange(1, nLayers);  sb->setValue(1);
	spn->setRange(1, nLayers); spn->setValue(1);

	QDockWidget *dock = new QDockWidget("Cube layers", mw);
	dock->setObjectName("cubeLayerDock");
	dock->setAllowedAreas(Qt::AllDockWidgetAreas);
	// Keep the X visible (Closable); a filter turns it into "re-dock" instead of destroy so the
	// layers can never become unreachable. Bottom is its home dock area.
	dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
	dock->setWidget(content);
	mw->addDockWidget(Qt::BottomDockWidgetArea, dock);
	dock->installEventFilter(new CubeDockCloseFilter(dock));

	// Start FLOATING, centered inside the viewer window. Use the .ui's OWN size verbatim — no
	// override. A floating dock is a top-level widget, so position in global coords from the centre.
	dock->setFloating(true);
	dock->resize(content->sizeHint());
	dock->move(mw->mapToGlobal(mw->rect().center()) - QPoint(dock->width() / 2, dock->height() / 2));

	s->cubeDlg = dock;
	// Clear the Scene slot when the dock is destroyed (its X, or when the window dies and takes it
	// along). Context = mw so a dead window auto-drops the connection.
	QObject::connect(dock, &QObject::destroyed, mw, [s]{ s->cubeDlg = nullptr; });

	// Fires the Julia callback for the current widget state, and ALWAYS tells the user -- in the
	// window's status bar -- exactly what the action does: which layer, and whether the colour scale
	// is this layer's own range or the whole cube's global min/max. Toggling the checkbox looks like
	// it does "nothing" only because the widget flips instantly; this message makes the actual work
	// (recolour + colorbar retarget) visible. sb/chk captured by value (raw pointers valid for the
	// dock's life; the connections that call this live on the widgets themselves).
	auto fire = [s, sb, chk]() {
		if (s->win)
			s->win->statusBar()->showMessage(
				QString("Cube layer %1  —  colour scale: %2")
					.arg(sb->value())
					.arg(chk->isChecked() ? "GLOBAL cube min/max" : "this layer's range"), 5000);
		if (g_juliaCubeLayer) g_juliaCubeLayer(s, sb->value() - 1, chk->isChecked() ? 1 : 0);
	};

	// Scrollbar <-> spinbox two-way sync; a shared heap `guard` bool (owned by the dock, freed with
	// it) stops the mirrored setValue from re-firing.
	bool *guard = new bool(false);
	QObject::connect(dock, &QObject::destroyed, dock, [guard]{ delete guard; });
	QObject::connect(sb, &QScrollBar::valueChanged, dock, [spn, guard, fire](int v) {
		if (*guard) return;
		*guard = true; spn->setValue(v); *guard = false;
		fire();
	});
	QObject::connect(spn, QOverload<int>::of(&QSpinBox::valueChanged), dock, [sb, guard, fire](int v) {
		if (*guard) return;
		*guard = true; sb->setValue(v); *guard = false;
		fire();
	});
	QObject::connect(chk, &QCheckBox::toggled, dock, [fire](bool) { fire(); });

	// "Load all in RAM": pull the whole cube into memory (Julia checks free RAM first and refuses if
	// it won't fit). While the (blocking, up to tens of seconds) load runs, show the busy cursor.
	// Afterwards every layer switch is a memory slice instead of a per-layer disk read.
	if (auto *ramBtn = content->findChild<QPushButton *>("loadRamBtn")) {
		QObject::connect(ramBtn, &QPushButton::clicked, dock, [s, ramBtn]() {
			if (!sceneAlive(s) || !g_juliaCubeLoadAll) return;
			QApplication::setOverrideCursor(Qt::WaitCursor);
			QApplication::processEvents();               // let the cursor paint before we block
			int rc = g_juliaCubeLoadAll(s);
			QApplication::restoreOverrideCursor();
			if (rc == 0) {
				ramBtn->setText("In RAM ✓");
				ramBtn->setEnabled(false);
				if (s->win)
					s->win->statusBar()->showMessage("Cube loaded into RAM — layer switching is now instant", 5000);
			}
			else if (rc == 1) {
				QMessageBox::warning(s->win, "Load all in RAM",
					"Not enough free RAM to hold the whole cube in memory.\n"
					"Keeping the per-layer disk reads.");
			}
			else {
				QMessageBox::warning(s->win, "Load all in RAM",
					"Failed to load the cube into memory.");
			}
		});
	}

	dock->show();    // data (layer 1) is already displayed above, so the panel appears after it
	dock->raise();
}

// ── Multi-variable netCDF picker ────────────────────────────────────────────
// A dropped netCDF file may hold several named variables (2-D grids and 3-D
// cubes). `rows` is a "\t"-separated, "\n"-terminated table -- one line per
// variable, "name\tsize\ttype". Shows a MODAL dialog with a checkbox left of each
// variable name (load any subset), a "compute per-layer min/max" option, and a
// Load button. Writes the 0-based indices of the checked variables into `sel`
// (capacity `maxSel`), sets `*prescan` to whether the min/max option is on, and
// returns the number of selected variables (0 = cancelled / nothing checked).
// Modal (exec) on purpose -- the drop handler blocks here and reads the result,
// then loads the chosen variable(s). No Julia callback fires while the dialog is
// open, so none of showCubeLayerDialog's reentrancy hazards apply (loading runs
// after exec() returns, back in the Julia drop handler).
static int showNetcdfVarDialog(Scene *s, const QString &title, const QString &rows,
                               int *sel, int maxSel, int *prescan) {
	if (prescan) *prescan = 1;
	QWidget *parent = (s && s->win) ? static_cast<QWidget *>(s->win) : nullptr;
	QStringList lines = rows.split('\n', Qt::SkipEmptyParts);
	if (lines.isEmpty() || !sel || maxSel <= 0) return 0;

	QDialog dlg(parent);
	dlg.setWindowTitle(title.isEmpty() ? QStringLiteral("Select netCDF variable(s)") : title);
	dlg.setModal(true);

	auto *lay  = new QVBoxLayout(&dlg);
	auto *info = new QLabel(QStringLiteral(
		"This file holds several variables. Tick the ones to load:"), &dlg);
	info->setWordWrap(true);
	lay->addWidget(info);

	auto *table = new QTableWidget(lines.size(), 3, &dlg);
	table->setHorizontalHeaderLabels({QStringLiteral("Variable"), QStringLiteral("Size"), QStringLiteral("Type")});
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->verticalHeader()->setVisible(false);
	for (int r = 0; r < lines.size(); ++r) {
		QStringList c = lines[r].split('\t');
		// The variable-name item carries the checkbox (drawn to the LEFT of the text); UNticked by
		// default -- the user opts each variable in (nothing loads unless something is ticked).
		auto *nameItem = new QTableWidgetItem(c.size() > 0 ? c[0] : QString());
		nameItem->setFlags((nameItem->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
		nameItem->setCheckState(Qt::Unchecked);
		table->setItem(r, 0, nameItem);
		table->setItem(r, 1, new QTableWidgetItem(c.size() > 1 ? c[1] : QString()));
		table->setItem(r, 2, new QTableWidgetItem(c.size() > 2 ? c[2] : QString()));
	}
	table->resizeColumnsToContents();
	table->horizontalHeader()->setStretchLastSection(true);
	lay->addWidget(table);

	// Select-all / none helpers + the per-layer min/max prescan option.
	auto *selAll  = new QPushButton(QStringLiteral("Select all"), &dlg);
	auto *selNone = new QPushButton(QStringLiteral("Select none"), &dlg);
	auto setAll = [table](Qt::CheckState st) {
		for (int r = 0; r < table->rowCount(); ++r)
			if (auto *it = table->item(r, 0)) it->setCheckState(st);
	};
	QObject::connect(selAll,  &QPushButton::clicked, &dlg, [setAll]{ setAll(Qt::Checked); });
	QObject::connect(selNone, &QPushButton::clicked, &dlg, [setAll]{ setAll(Qt::Unchecked); });
	auto *selRow = new QHBoxLayout;
	selRow->addWidget(selAll);
	selRow->addWidget(selNone);
	selRow->addStretch(1);
	lay->addLayout(selRow);

	auto *preChk = new QCheckBox(QStringLiteral(
		"Compute per-layer min/max (fixes the colour scale && vertical axis for 3-D cubes)"), &dlg);
	preChk->setChecked(true);
	lay->addWidget(preChk);

	auto *btnBox = new QDialogButtonBox(&dlg);
	auto *loadBtn = btnBox->addButton(QStringLiteral("Load"), QDialogButtonBox::AcceptRole);
	btnBox->addButton(QDialogButtonBox::Cancel);
	lay->addWidget(btnBox);

	QObject::connect(loadBtn, &QPushButton::clicked, &dlg, [&]{ dlg.accept(); });
	QObject::connect(btnBox, &QDialogButtonBox::rejected, &dlg, [&]{ dlg.reject(); });
	// Double-click a row = load just that one variable.
	QObject::connect(table, &QTableWidget::cellDoubleClicked, &dlg, [&](int r, int){
		setAll(Qt::Unchecked);
		if (auto *it = table->item(r, 0)) it->setCheckState(Qt::Checked);
		dlg.accept();
	});

	dlg.resize(500, qMin(210 + lines.size() * 26, 520));
	if (dlg.exec() != QDialog::Accepted) return 0;

	if (prescan) *prescan = preChk->isChecked() ? 1 : 0;
	int n = 0;
	for (int r = 0; r < lines.size() && n < maxSel; ++r) {
		auto *it = table->item(r, 0);
		if (it && it->checkState() == Qt::Checked) sel[n++] = r;
	}
	return n;
}

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
			// Pass the window's own Scene *as a raw Julia pointer literal (not through the `fig`
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
static QIcon makeSwipeIcon();               // split-tile glyph for the Swipe toggle (85_polygon.cpp)
static QIcon makeLinkIcon();                // two-windows-and-a-chain glyph for the Link toggle (85_polygon.cpp)
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
	for (const RecentItem &r : g_recent) { paths << r.path; cats << r.cat; }
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
		for (const RecentItem &r : g_recent) {
			if (r.cat != c) continue;
			if (!header) { QAction *h = menu->addAction(kCatName[c]); h->setEnabled(false); header = true; }
			const QString full = r.path;
			QAction *act = menu->addAction(QFileInfo(full).fileName());
			act->setToolTip(full); act->setStatusTip(full);
			QObject::connect(act, &QAction::triggered, [s, full]() {
				if (!g_juliaDrop) return;
				// Route through the drop path so the file opens INTO this window
				// (or promotes an empty launcher) instead of spawning a new window.
				juliaOpenFile(s, full.toStdString().c_str());
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
	axesDestroy(s, s->baseAxes);       // the BASE raster's own axes go with the base raster (only its
	                                    // own — every extra keeps the set IT owns, untouched)
	// profLine is a pile vector -> it may live in the depth-cleared overlay renderer; clear both layers.
	if (s->profLine) { s->ren->RemoveActor(s->profLine); if (s->axesRen) s->axesRen->RemoveActor(s->profLine); }
	if (s->bar)      s->ren->RemoveActor2D(s->bar);
	if (s->barTicks) s->ren->RemoveActor2D(s->barTicks);
	for (auto &ta : s->barLabels) if (ta) s->ren->RemoveActor2D(ta);
	s->barLabels.clear(); s->barValues.clear();
	s->surfGroup = nullptr; s->drape = nullptr; s->bar = nullptr; s->barTicks = nullptr;
	s->layerImgMode = false;   // any real surface build exits the fast cube-layer image mode
	if (s->layerCamCmd && s->ren->GetActiveCamera()) s->ren->GetActiveCamera()->RemoveObserver(s->layerCamCmd);
	s->layerCamCmd = nullptr;
	if (s->layerDetail) s->ren->RemoveActor(s->layerDetail);
	s->layerDetail = nullptr; s->layerDetailImg = nullptr;
	if (s->layerDetailTimer) s->layerDetailTimer->stop();
	s->layerDetailReg[0] = s->layerDetailReg[1] = 0.0;

	// Colour map. A GMT CPT arrives as control nodes (cz[i] -> crgb[i]); a
	// vtkColorTransferFunction maps z to colour at those exact (possibly non-uniform,
	// data-equalized) positions. No CPT -> a plain blue->red ramp (demo).
	vtkSmartPointer<vtkScalarsToColors> lut;
	bool ctfRange = false;
	if (cz && crgb && ncolor > 0) {
		lut = makeGridCTF(s, cz, crgb, ncolor);		// NaN fill colour applied by construction
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
	// s->nanColor was seeded from Preferences when the Scene was created, so it is already right here
	// however this window was reached. The fallback ramp above is built by hand (HSV, no CPT nodes),
	// so it is the one LUT makeGridCTF cannot produce -- it gets the colour applied explicitly.
	applyNanColorToLut(s->surfLut, s->nanColor);
	// Remember the base CPT + geographic flag so the Shading dock can rebuild this grid as a flat image
	// or a surface on demand (rebuildBaseFromStored) without the host re-sending the data.
	if (cz && crgb && ncolor > 0) { s->baseCz.assign(cz, cz + ncolor); s->baseCrgb.assign(crgb, crgb + 3 * ncolor); }
	s->baseGeog = geographic;

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
		sceneSetGridLayer(s, gz, gnx, gny, x0, x1, y0, y1);
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
	configureGridMapper(map, lut, s->zmin, s->zmax, ctfRange);

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

	// --- this raster's OWN cube axes ----------------------------------------
	// SACRED_LAW.md Raster-own-axes law. The BASE surface is a raster like any other, so it gets a
	// set built by the SAME axesBuild every dropped/derived grid, image and mesh goes through --
	// there is no separate "the window's axes" construction any more. Its frame is ITS OWN extent,
	// ITS OWN z range, in ITS OWN units, with ITS OWN geographic flag; nothing else in the window
	// can reach it and it reaches nothing else.
	axesSetFrame(s->baseAxes, s->x0, s->x1, s->y0, s->y1, s->zmin, s->zmax, geographic);
	// Empty launcher (blankStart): the cube axes + tick/label billboards are NEVER added to the
	// renderer and the initial label build is skipped, so the blank window can't flash an axis box
	// with numbers for a frame. A dropped file PROMOTES this same window via buildSceneContent.
	axesBuild(s, s->baseAxes, !blankStart);
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
	// vtkCellPicker's ray-cell intersection never hits a zero-area Verts cell (a point cloud's own
	// geometry, e.g. view_points/"Point cloud view") -- vtkPointPicker does nearest-point-in-tolerance
	// picking instead, tried as a fallback in onMouseMove when the cell picker misses.
	s->pointPicker = vtkSmartPointer<vtkPointPicker>::New();
	s->pointPicker->SetTolerance(0.01);

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

	// Single source of truth for "run the request Julia currently has settings for" -- called by the
	// OK button AND (when in Time series mode) automatically right after a map pick, so picking a
	// point IS the trigger for a time series, same as SACRED_LAW.md's "only the action button
	// executes" principle but here the map click doubles as that action button by explicit request.
	auto runCompute = std::make_shared<std::function<void()>>();
	*runCompute = [s, eStart, eEnd, eLon, eLat, eInc, rGrid, cV, cE2, cN2, cW, cE, cS, cN]() {
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
	};
	// "Click point on map": get the dialog out of the way, turn the map cursor into a crosshair (the
	// visible cue that a pick is armed), arm a one-shot pick on the map widget; on click refill
	// Lon/Lat, restore the cursor + dialog, and (Time series mode only -- Grid(s) is always global,
	// a point is meaningless there) immediately run the request so the click itself produces the
	// time series instead of silently doing nothing.
	// showMinimized(), NOT lower(): dlg is OWNED by s->win (Qt sets the Windows "owned window"
	// relation), and Windows always keeps an owned popup above its owner in z-order -- asking to
	// lower the owned dialog below other apps' windows dragged the OWNER (the whole iGMT main
	// window) down with it, so the map itself vanished behind e.g. the editor instead of the dialog
	// simply stepping aside. Minimizing only the dialog never touches the owner's stacking.
	// pickFilter (shared, tracked outside the lambda) lets the dialog's destroyed handler below clean
	// up an still-armed filter + crosshair cursor if the user closes the dialog without ever clicking.
	auto pickFilter = std::make_shared<QPointer<MapPickFilter>>();
	QObject::connect(bPick, &QPushButton::clicked, dlg, [s, dlg, eLon, eLat, rGrid, runCompute, pickFilter]() {
		if (!s->widget) return;
		dlg->showMinimized();
		s->widget->setCursor(Qt::CrossCursor);
		if (s->win) s->win->statusBar()->showMessage("Earth Tides: click a point on the map…", 5000);
		MapPickFilter *f = new MapPickFilter(s, s->widget,
		                                      [s, dlg, eLon, eLat, rGrid, runCompute](double lon, double lat) {
			eLon->setValue(lon); eLat->setValue(lat);
			if (s->widget) s->widget->unsetCursor();
			dlg->showNormal(); dlg->raise(); dlg->activateWindow();
			if (!rGrid->isChecked()) (*runCompute)();          // Time series: the pick IS the trigger
		});
		*pickFilter = f;
		s->widget->installEventFilter(f);
	});
	QObject::connect(dlg, &QObject::destroyed, dlg, [s, pickFilter]() {
		if (*pickFilter) {
			if (s->widget) { s->widget->removeEventFilter(*pickFilter); s->widget->unsetCursor(); }
			pickFilter->data()->deleteLater();
		}
	});
	QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
	QObject::connect(bb, &QDialogButtonBox::accepted, dlg, [runCompute]() { (*runCompute)(); });
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
	for (auto &pg : s->polys) if (pg.isFault && pg.faultPlane3D) {
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

// Load a .igmtz session INTO `s`/`win`. The ONE path for this: the File > Load Session menu action
// and a dropped/opened .igmtz file (drop.jl's _on_drop, via gmtvtk_load_session_h below) both call
// THIS function -- never two independent re-derivations (SACRED_LAW.md). The priming progress
// dialog (parented to `win`, shown + pumped before the real load starts) is not cosmetic: a
// brand-new window's Scene Objects dock can still be mid-layout when the heavy session replay
// starts, and skipping this warm-up leaves stale/overlapping tree rows behind.
static void loadSessionIntoWindow(Scene *s, QMainWindow *win, const QString &path) {
	if (!g_juliaLoadSession) {
		if (s->win) s->win->statusBar()->showMessage("Load Session: callback not registered", 3000);
		return;
	}
	if (g_progress) delete g_progress;
	g_progress = new QProgressDialog(win);
	g_progress->setWindowTitle("Loading Session...");
	g_progress->setRange(0, 0);
	g_progress->setCancelButton(nullptr);
	g_progress->setWindowModality(Qt::ApplicationModal);
	g_progress->show();
	g_progress->raise();
	g_progress->activateWindow();
	for (int i = 0; i < 10; i++) QApplication::processEvents();
	const QByteArray utf8 = path.toUtf8();
	g_juliaLoadSession(s, utf8.constData());
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
	// Preferences "NaN fill colour", read HERE — at scene birth, before ANY grid can be built in this
	// window. It used to be read inside the primary-surface builder, so a window that never took that
	// path (an empty launcher, or one whose first content is an image) left s->nanColor at the struct
	// default and every grid added to it afterwards ignored the preference. SACRED_LAW.md: one source,
	// loaded once, for every element type — not a value that only exists if you entered by one door.
	prefNanColorRGB(s->nanColor[0], s->nanColor[1], s->nanColor[2]);
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
	applyBackgroundPref(s->ren);    // Preferences > Background color (default: dark-slate/steel-blue gradient)
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
		linkUnlinkWindows(s);                          // drop a Link partner's pointer to this scene
		g_scenes.erase(s);                             // invalidate any host-held handle to s
		delete s->giz; delete s;
		for (Scene *o : g_scenes) swipeRefreshAvailability(o);   // one fewer window to link with
	});

	auto actReset = [s]() {
		s->ren->ResetCamera();
		s->widget->renderWindow()->Render();
	};
	auto actToggleAxes = [s]() {
		sceneAxesSetShown(s, !sceneAxesShown(s));   // window-wide view command: EVERY raster's own set
		rebuildAxisLabels(s);
		rebuildSceneObjects(s);                     // the per-raster Axes rows re-read their own intent
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
		const bool axesVis = sceneAxesShown(s);
		const bool barVis  = colorbarVisible(s);
		sceneAxesSetShown(s, false);             // decoration only — never part of the georeferenced pixels
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

		sceneAxesSetShown(s, axesVis);
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
			"compass ring: azimuth.   'x' hides/shows the gizmo.\n\n"
			"gmtvtk.dll built: " __DATE__ " " __TIME__);
	};

	QMenu *mFile = win->menuBar()->addMenu("&File");
	// Preferences: settings dialog (deps/ui/preferences.ui). Values persist via QSettings.
	mFile->addAction("&Preferences…", [win]() {
		const QString nanBefore = prefNanColor();
		const QString bgBefore  = prefBackgroundColor();
		PreferencesDialog(win).exec();
		// NaN fill colour changed -> repaint every open window's grid NaN cells live. The hole
		// GEOMETRY is already filled (colour-independent); only the CTF NaN colour + baked textures
		// need refreshing, so update each scene's colour and re-run applyShading (re-bakes hillshade /
		// flat image and re-renders).
		if (prefNanColor() != nanBefore) {
			for (Scene *sc : g_scenes) {
				if (!sceneAlive(sc)) continue;
				prefNanColorRGB(sc->nanColor[0], sc->nanColor[1], sc->nanColor[2]);
				// EVERY LUT this window owns, not just the two that were remembered here before:
				// the Aquamoto land CTF is a grid LUT like any other and was silently skipped.
				applyNanColorToLut(sc->surfLut, sc->nanColor);
				for (auto &ex : sc->extras) applyNanColorToLut(ex.lut, sc->nanColor);
				applyNanColorToLut(sc->aquaLandLut, sc->nanColor);
				applyShading(sc);
			}
		}
		// Background colour changed -> re-run the SAME applyBackgroundPref every open window's
		// renderer went through at creation time (SACRED_LAW: one function, no inline SetBackground).
		if (prefBackgroundColor() != bgBefore) {
			for (Scene *sc : g_scenes) {
				if (!sceneAlive(sc)) continue;
				applyBackgroundPref(sc->ren);
				sc->widget->renderWindow()->Render();
			}
		}
	});
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
			for (const QString &f : files) {
				const QByteArray utf8 = f.toUtf8();        // keep buffer alive across call
				juliaOpenFile(s, utf8.constData());        // busy dialog up before Julia is entered
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
			loadSessionIntoWindow(s, win, f);
		});
		mFile->addSeparator();
		// Ctrl+V: paste the clipboard INTO this window — an image becomes a new image object, a
		// numeric table a line/polygon overlay (or X,Y series), a copied FILE opens as if dropped.
		// Shared action (makePasteAction, 30_app.cpp); adding it here is what arms the shortcut.
		mFile->addAction(makePasteAction(win, [s]() {
			if (!scenePasteClipboard(s) && s->win)
				s->win->statusBar()->showMessage("Clipboard holds nothing that can be pasted", 3000);
		}));
		mFile->addSeparator();
		mFile->addAction("&Close", [win](){ win->close(); }, QKeySequence::Close);

	// --- Image menu (mirrors Mirone's Image menu, mirone_uis.m) --------------------------------
	// Operations on the window's IMAGE (not its grid). First entry: Binarize (thresholdit.m).
	QMenu *mImage = win->menuBar()->addMenu("&Image");
	QAction *aBinarize = mImage->addAction("&Binarize Image…", [win, s]() {
		auto it = g_binarizeDlgs.find(s);                    // hidden earlier? bring back the SAME one
		if (it != g_binarizeDlgs.end() && it->second->dlg) {
			it->second->unpark();   // parked? the row goes away and the SAME dialog comes back
			return;
		}
		auto *w = new BinarizeDialog(win, s, displayedImageName(s));   // self-deletes with its QDialog
		if (w->dlg && w->ready) w->dlg->show();
		else delete w;
	});
	// Flip (mirone_uis.m: a "Flip" submenu with "Flip Up-Down" / "Flip Left-Right", both landing on
	// mirone.m's `Transfer_CB` 'flipUD'/'flipLR'). Re-orders the pixels of the displayed image inside
	// its own ground extent, so a REFERENCED image keeps its coordinates: the georeference is not
	// touched at all, only the pixel order. Applying the same flip twice restores the image.
	QMenu *mFlip = mImage->addMenu("&Flip");
	QAction *aFlipUD = mFlip->addAction("Flip &Up-Down", [s]() {
		flipImageObject(s, displayedImageName(s), true);
	});
	QAction *aFlipLR = mFlip->addAction("Flip &Left-Right", [s]() {
		flipImageObject(s, displayedImageName(s), false);
	});
	// Show Histogram (image_histo.m): the histogram of what the window DISPLAYS — for a grid that is
	// its rendered colour image, never its z values.
	QAction *aHisto = mImage->addAction("Show &Histogram", [win, s]() { showImageHistogram(win, s, ""); });
	// Image resize (imageresize.m), where mirone_uis.m puts it: in the Image menu, after a separator.
	QAction *aResize = mImage->addAction("Image &resize…", [win, s]() {
		showImageResize(win, s, displayedImageName(s).toUtf8().constData());
	});
	// Shape detector (floodfill.m) — the label mirone_uis.m gives it, in the Image menu as there.
	QAction *aShape = mImage->addAction("&Shape detector…", [win, s]() {
		showFloodFill(win, s, displayedImageName(s).toUtf8().constData());
	});
	// K-means classification (classificationfig.m), the entry mirone_uis.m puts right after it.
	QAction *aClassify = mImage->addAction("&K-means classification…", [win, s]() {
		showClassification(win, s, displayedImageName(s).toUtf8().constData());
	});
	// Image Enhance submenu, kept exactly as mirone_uis.m builds it (the numbering included). Only
	// entry 1 (image_enhance.m) is ported so far; 2 (image_adjust.m) and 3 (ice_m.m) are placeholders.
	mImage->addSeparator();
	QMenu *mEnhance = mImage->addMenu("Image Enhance");
	QAction *aEnh1 = mEnhance->addAction("1 - Indexed and RGB", [win, s]() {
		auto it = g_enhanceDlgs.find(s);                     // already open? just bring it forward
		if (it != g_enhanceDlgs.end() && it->second->dlg) {
			it->second->unpark();   // parked? the row goes away and the SAME dialog comes back
			return;
		}
		auto *w = new ImageEnhanceDialog(win, s, displayedImageName(s));
		if (w->dlg && w->ready) w->dlg->show();
		else delete w;
	});
	QAction *aEnh2 = mEnhance->addAction("2 - Indexed only");
	QAction *aEnh3 = mEnhance->addAction("Image Color Editor (Indexed and RGB)");
	aEnh2->setEnabled(false);  aEnh3->setEnabled(false);     // not ported yet
	// Explore RGB (mirone.m 'RGBexp'), where mirone_uis.m puts it: right after Image Enhance.
	QAction *aRgbExp = mImage->addAction("&Explore RGB", [win, s]() {
		showRgbExplore(win, s, displayedImageName(s).toUtf8().constData());
	});
	// Binarize needs an image to threshold — greyed out (refreshed on every open) when the window
	// holds none. Show Histogram works off the DISPLAY, so a grid window qualifies too.
	QObject::connect(mImage, &QMenu::aboutToShow, [s, aBinarize, aHisto, aResize, aShape, aClassify,
	                                              aEnh1, aFlipUD, aFlipLR, aRgbExp]() {
		// Explore RGB splits an image into colour components, so it needs a genuine RGB one — Mirone
		// hides the entry otherwise (`if (ndims(img) < 3), return, end`). Julia flags which images
		// qualify (s->imgRGB), because the viewer sees every image as an RGBA texture.
		aRgbExp->setEnabled(s->imgRGB.count(displayedImageName(s).toStdString()) != 0);
		aBinarize->setEnabled(sceneHasImage(s));
		aHisto->setEnabled(sceneHasImage(s) || sceneHasGrid(s));
		aFlipUD->setEnabled(sceneHasImage(s));  // flips an image's own pixels, so it needs a real one
		aFlipLR->setEnabled(sceneHasImage(s));
		aResize->setEnabled(sceneHasImage(s));  // resamples the image itself, so it needs a real one
		aShape->setEnabled(sceneHasImage(s));   // grows a region over the image's own pixels
		aClassify->setEnabled(sceneHasImage(s));// clusters the image's own pixel colours
		aEnh1->setEnabled(sceneHasImage(s));   // it rewrites an image's pixels, so it needs a real one
	});

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
	// screen at the current zoom. sceneVisibleRegion (10_geometry.cpp) is the ONE implementation —
	// also read by the Link tool's cross-window camera sync (57_swipe.cpp); this local wrapper just
	// keeps every existing capture-list/call-site below (`[…, visibleRegion, …]`) unchanged.
	auto visibleRegion = [s](double &W, double &E, double &S, double &N) -> bool {
		return sceneVisibleRegion(s, W, E, S, N);
	};
	// SACRED LAW, single source of truth: EVERY Geography/Seismology leaf that plots onto the
	// window needs something to land on. On an EMPTY launcher, load the whole-world Base Map IN
	// PLACE (the SAME "global" request the Base Maps picker sends -> _on_basemap promotes the
	// launcher + adds the etopo4 image) instead of refusing / silently no-op'ing. Runs
	// synchronously, so on return s->x0..y1 already frame the world. false only if the basemap
	// callback is missing. ONE function — every current leaf that PLOTS something onto the window
	// (coastline/borders/rivers, volcanoes/meteorites/hydrothermal/tides, plate boundaries,
	// seismicity) and every FUTURE Geography leaf that does the same (hotspots, isochrons, cities,
	// DSDP/ODP/IODP, …) must call this FIRST, never re-derive/duplicate the empty-launcher check or
	// skip it for one kind. Exception: leaves that only COMPUTE off W/E/S/N and plot nothing (Earth
	// Tides) don't need a base — gate on that distinction, not on which leaf it happens to be.
	auto ensureGeoBase = [s]() -> bool {
		if (!s->emptyStart) return true;
		if (!g_juliaBaseMap) {
			if (s->win) s->win->statusBar()->showMessage("Geography: Base Map callback not registered", 3000);
			return false;
		}
		if (s->win) s->win->statusBar()->showMessage("Geography: loading base map…");
		showBusyDialog("Base Map");              // indeterminate busy bar (first-run GMT compile)
		g_juliaBaseMap(s, "-180/180/-90/90/0/global");
		closeBusyDialog();
		return true;
	};
	// Hand a seismicity request to Julia: make sure there is a base map, append the visible map
	// region (the in-map event crop + the USGS query bbox, like Mirone's in_map_region), send.
	auto sendSeismicity = [s, visibleRegion, ensureGeoBase](const QString &params) {
		if (!ensureGeoBase()) return;
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
	// A leaf that fetches a GSHHG/point feature for the current view: ensure a base to land on
	// (ensureGeoBase, empty-launcher -> world Base Map), compute the visible region, hand
	// "<kind>/<res>/W/E/S/N" to Julia, which calls GMT.coast(R=…, D=res, M=true) and adds the lines
	// (or the matching point-dataset reader for volcano/meteorite/hydro/tides/plateboundaries).
	// EVERY Geography leaf that plots goes through this ONE closure — never a per-kind fork.
	auto geoPlot = [s, visibleRegion, ensureGeoBase](const QString &kind, const char *res) {
		return [s, kind, res, visibleRegion, ensureGeoBase]() {
			if (!g_juliaGeo) {
				if (s->win) s->win->statusBar()->showMessage("Geography: callback not registered", 3000);
				return;
			}
			if (!ensureGeoBase()) return;
			double W = -180, E = 180, S = -90, N = 90;          // whole-earth fallback if no view region
			visibleRegion(W, E, S, N);
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
	s->geoMenu = mGeo;                              // gmtvtk_set_crs keeps it enabled once real data has a CRS
	mGeo->menuAction()->setEnabled(true);          // unblocked from window-open: leaf actions fall back to
	                                                 // the whole-earth region (visibleRegion) on an empty launcher
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
	QMenu *mIsoc = mGeo->addMenu("Magnetic isochrons");
	mIsoc->addAction("GPlates", geoPlot("isochrons_gplates", ""));
	mIsoc->addAction("custom",  geoTODO("Magnetic isochrons (custom)"));
	mGeo->addAction("Volcanoes",                     geoPlot("volcano", ""));
	mGeo->addAction("Meteorite impacts",             geoPlot("meteorite", ""));
	mGeo->addAction("Hydrothermal sites",            geoPlot("hydro", ""));
	mGeo->addAction("Tide Stations",                 geoPlot("tidestations", ""));
	mGeo->addAction("Tides (download)",              geoPlot("tides", ""));
	mGeo->addAction("Earth Tides", [s, visibleRegion]() {
		// No basemap needed: Earth Tides computes off W/E/S/N alone, nothing plotted onto a surface.
		double W = -180, E = 180, S = -90, N = 90;          // whole-earth fallback if no view region
		visibleRegion(W, E, S, N);
		showEarthTidesDialog(s, W, E, S, N);
	});
	mGeo->addAction("Fracture Zones",                geoTODO("Fracture Zones"));
	mGeo->addAction("Plate boundaries",              geoPlot("plateboundaries", ""));

	QMenu *mCit = mGeo->addMenu("Cities");
	mCit->addAction("Major cities", geoPlot("city_major", ""));
	mCit->addAction("Other cities", geoPlot("city", ""));

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

	auto *fGroup  = new std::function<void()>();   // show the discipline chooser
	auto *fTsu    = new std::function<void()>();    // show Tsunamis
	auto *fSeis   = new std::function<void()>();    // show Seismology
	auto *fMag    = new std::function<void()>();    // show Magnetics
	auto *fPlates = new std::function<void()>();    // show Plates

	// Re-open the menu at its menubar slot after a rotate (deferred so it runs once the triggering
	// click has finished closing the menu).
	auto reopen = [win, mGphy]() {
		QTimer::singleShot(0, mGphy, [win, mGphy]() {
			QRect r = win->menuBar()->actionGeometry(mGphy->menuAction());
			mGphy->popup(win->menuBar()->mapToGlobal(r.bottomLeft()));
		});
	};

	*fGroup = [mGphy, win, s, fTsu, fSeis, fMag, fPlates]() {
		mGphy->clear();
		mGphy->setTitle("Geophysics ▾");
		mGphy->addAction("Tsunamis",   [fTsu]()    { (*fTsu)(); });
		mGphy->addAction("Seismology", [fSeis]()   { (*fSeis)(); });
		mGphy->addAction("Magnetics",  [fMag]()    { (*fMag)(); });
		mGphy->addAction("Plates",     [fPlates]() { (*fPlates)(); });
		mGphy->addSeparator();
		// Ocean Color: a single tool, not a discipline — it opens its dialog instead of rotating the
		// menu, so it sits below the separator rather than in the discipline list above it.
		mGphy->addAction("Oceancolor…", [win, s]() {
			// Already open or parked in this window? Bring THAT one back — never a second browser.
			auto it = g_oceanColorDlgs.find(s);
			if (it != g_oceanColorDlgs.end() && it->second && it->second->dlg) { it->second->unpark(); return; }
			warmupTool("oceancolor");
			new OceanColorDialog(win, s);
		});
	};
	// Clicking the "Geophysics ›" row ITSELF (not one of the disciplines in its flyout) goes back to
	// the neutral chooser — menubar title "Geophysics ▾", all disciplines listed. Qt never emits
	// triggered() for a submenu's OWN parent row (a click there only opens the flyout), so the click
	// is intercepted here on the parent menu, before Qt swallows it.
	struct GphyHomeOnParentClick : QObject {
		QMenu *menu;
		std::function<void()> *group;
		std::function<void()> reopen;
		GphyHomeOnParentClick(QMenu *m, std::function<void()> *g, std::function<void()> ro)
			: QObject(m), menu(m), group(g), reopen(ro) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::MouseButtonRelease) {
				QAction *a = menu->actionAt(static_cast<QMouseEvent *>(e)->position().toPoint());
				if (a && a->menu() && a->text().startsWith("Geophysics")) {
					menu->close();
					(*group)();
					reopen();
					return true;			// consumed: do NOT let Qt treat it as "just open the flyout"
				}
			}
			return QObject::eventFilter(o, e);
		}
	};
	mGphy->installEventFilter(new GphyHomeOnParentClick(mGphy, fGroup, reopen));

	auto backItem = [mGphy, fTsu, fSeis, fMag, fPlates](const QString &current) {
		// Single entry — itself a submenu, direct access to any OTHER discipline (skips the
		// chooser page entirely). Each fXxx already reopens the menu itself at its end.
		QMenu *mBack = mGphy->addMenu("Geophysics ›");
		if (current != "Tsunamis")   mBack->addAction("Tsunamis",   [fTsu]()    { (*fTsu)();    });
		if (current != "Seismology") mBack->addAction("Seismology", [fSeis]()   { (*fSeis)();   });
		if (current != "Magnetics")  mBack->addAction("Magnetics",  [fMag]()    { (*fMag)();    });
		if (current != "Plates")     mBack->addAction("Plates",     [fPlates]() { (*fPlates)(); });
		mGphy->addSeparator();
	};

	// Tsunamis discipline — currently just NSWING (port of Mirone's swan_options.m).
	*fTsu = [mGphy, win, s, backItem, reopen, actNestedGridsTsu]() {
		mGphy->clear();
		mGphy->setTitle("Tsunamis ▾");
		backItem("Tsunamis");
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
		mGphy->addAction("Aquamoto viewer…", [win, s]() {
			// Non-modal, own top-level window (aquamoto.ui roots at QMainWindow, loaded verbatim --
			// no resize/rescale of its own geometry). Closing only HIDES it (AquamotoHideOnClose); this
			// reuses the SAME window for the scene, with its file/slice/state intact, rather than
			// spawning a duplicate. Re-accessible afterwards from the layer's Scene Objects handle too.
			AquamotoWindow::openFor(win, s);
		});
		mGphy->addAction(actNestedGridsTsu);
		reopen();
	};

	// Magnetics discipline — IGRF Calculator (port of Mirone's igrf_options.m, GMT.jl magref).
	*fMag = [mGphy, win, s, backItem, reopen]() {
		mGphy->clear();
		mGphy->setTitle("Magnetics ▾");
		backItem("Magnetics");
		mGphy->addAction("IGRF", [win, s]() {
			auto *w = new IgrfDialog(win, s);   // self-deletes when its QDialog closes (WA_DeleteOnClose)
			if (w->dlg) w->dlg->show();
		});
		mGphy->addAction("Geomagnetic Bar Code", [win]() {
			auto *w = new MagBarcodeDialog(win);
			if (w->dlg) w->dlg->show();
		});
		// Reduction to the Pole / Total field to Components — port of Mirone's parker_stuff.m
		// ('redPole'/'component' cases). Same dialog class (Rtp3DDialog), gated by mode: 0=RTP
		// (component forced 0, radio group hidden), 1=Components (radio group picks North/East/Vert).
		mGphy->addAction("Reduction to the Pole", [win, s]() {
			auto *w = new Rtp3DDialog(win, s, 0);
			if (w->dlg) w->dlg->show();
		});
		mGphy->addAction("Total field to Components", [win, s]() {
			auto *w = new Rtp3DDialog(win, s, 1);
			if (w->dlg) w->dlg->show();
		});
		// Anomaly (gravity OR magnetic) of a 3-D body — GMT's gmtgravmag3d (Okabe 1979).
		mGphy->addAction("gmtgravmag3d", [win, s]() {
			auto *w = new GravMag3DDialog(win, s);   // self-deletes when its QDialog closes (WA_DeleteOnClose)
			if (w->dlg) w->dlg->show();
		});
		// Same anomaly, body described by one or two GRIDS instead of triangles.
		mGphy->addAction("grdgravmag3d", [win, s]() {
			auto *w = new GrdGravMag3DDialog(win, s);
			if (w->dlg) w->dlg->show();
		});
		// Continuous RTP (differential): field AND magnetization allowed to vary over the grid.
		mGphy->addAction("grdredpol", [win, s]() {
			auto *w = new GrdRedPolDialog(win, s);
			if (w->dlg) w->dlg->show();
		});
		// Import *.gmt/*.nc cruise track file(s) — port of Mirone's GeophysicsImportGmtFile_CB
		// (mirone.m). Plots the navigation of MGD77+ netCDF cruise files (Julia's g_juliaImportGmt,
		// via GMT.jl's own mgd77list — see src/mgd77tracks.jl). The legacy pre-MGD77 *.gmt binary
		// format is NOT supported by GMT >= 5's mgd77 suite; Julia reports that per-file in the
		// window's Errors console rather than guessing at its byte layout.
		QMenu *mImportGmt = mGphy->addMenu("Import *.gmt/*.nc file(s)");
		mImportGmt->addAction("Single *.gmt/*.nc file", [win, s]() {
			if (!g_juliaImportGmt) {
				if (s->win) s->win->statusBar()->showMessage("Import *.gmt/*.nc file: callback not registered", 3000);
				return;
			}
			QString fn = QFileDialog::getOpenFileName(win, "Select gmt/nc File", prefStartDir(),
			                                          "gmt/nc files (*.gmt *.GMT *.nc *.NC);;All Files (*.*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			g_juliaImportGmt(s, fn.toUtf8().constData(), 0);
		});
		mImportGmt->addAction("List of files", [win, s]() {
			if (!g_juliaImportGmt) {
				if (s->win) s->win->statusBar()->showMessage("Import *.gmt/*.nc file: callback not registered", 3000);
				return;
			}
			QString fn = QFileDialog::getOpenFileName(win, "Select list file", prefStartDir(),
			                                          "Data files (*.dat *.DAT *.txt *.TXT);;All Files (*.*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			g_juliaImportGmt(s, fn.toUtf8().constData(), 1);
		});
		// gmtedit — the MGD77 track editor (port of Mirone's src_figs/gmtedit.m), the sibling of
		// the Import item in mirone_uis.m. Opens its OWN window (67_gmtedit.cpp), not tied to this
		// Scene, so it takes no grid/surface and needs none.
		mGphy->addAction("gmtedit (MGD77 track editor)", [win, s]() {
			GmtEdit *e = buildGmtEdit("gmtedit", 200.0);
			if (!e || !g_juliaGmtEdit)
				return;
			// This window becomes the editor's parent (gmtedit.m's hMirAxes), so its link tool can
			// send a clicked record here as a marker.
			geSetParent(e, s);
			// Offer the file straight away — gmtedit(FILE) is how Mirone's users reach it.
			QString fn = QFileDialog::getOpenFileName(win, "Select gmt File", prefStartDir(),
				"Cruise files (*.nc *.NC *.gmt *.GMT);;MGD77+ netCDF (*.nc *.NC);;"
				"Legacy *.gmt binary (*.gmt *.GMT);;All Files (*.*)");
			if (fn.isEmpty()) return;
			rememberStartDir(fn);
			g_juliaGmtEdit(e, "open", fn.toUtf8().constData());
		});
		reopen();
	};

	// Seismology discipline — Seismicity (earthquakes.m port) + TODO stubs (geoTODO) + the
	// Elastic deformation submenu.
	*fSeis = [mGphy, mElastic, geoTODO, backItem, reopen, s, win, openSeismicity, sendSeismicity, visibleRegion]() {
		mGphy->clear();
		mGphy->setTitle("Seismology ▾");
		backItem("Seismology");
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

	// Plates discipline — plate kinematics, migrated from Mirone: Euler rotations (euler_stuff.m)
	// re-based on GMT's spotter supplement, and the Plate calculator (plate_calculator.m).
	*fPlates = [mGphy, win, s, backItem, reopen]() {
		mGphy->clear();
		mGphy->setTitle("Plates ▾");
		backItem("Plates");
		mGphy->addAction("Euler rotations…", [win, s]() {
			// Its X PARKS it (Scene Objects row), so re-picking this entry brings THAT dialog back with
			// everything still set up in it — never a second, empty one.
			auto it = g_eulerDlgs.find(s);
			if (it != g_eulerDlgs.end() && it->second && it->second->dlg) { it->second->unpark(); return; }
			auto *w = new EulerDialog(win, s);   // deletes itself when its QDialog is really destroyed
			if (w->dlg) w->dlg->show();
		});
		mGphy->addAction("Plate calculator…", [win, s]() {
			auto it = g_plateDlgs.find(s);
			if (it != g_plateDlgs.end() && it->second && it->second->dlg) { it->second->unpark(); return; }
			auto *w = new PlateCalcDialog(win, s);
			if (w->dlg) w->dlg->show();
		});
		// The FIRST pick of this entry showed nothing (only a second one did, through the unpark()
		// branch): this page of the menu is not the menubar's own popup — `reopen()` re-opened it with
		// mGphy->popup(), whose grab is still being torn down while the action's handler runs, so a
		// window created and shown from inside it never reached the screen. Deferred with the SAME
		// singleShot(0) idiom reopen() itself uses: the click finishes closing the menu first, then the
		// dialog opens, is raised and activated. A .ui that fails to load SAYS so instead of leaving
		// the entry looking dead.
		mGphy->addAction("Compute Euler pole…", [win, s]() {
			// Create-or-restore, then MAKE SURE it is on screen. Everything here exists because the
			// first pick of this entry used to open nothing at all:
			//   * this page of the menu is not the menubar's own popup — reopen() re-opened it with
			//     mGphy->popup(), and a window shown while that popup still holds its grab does not
			//     come up. So the open WAITS for QApplication::activePopupWidget() to be gone.
			//   * a dialog that ended up off every screen is moved to the middle of its viewer.
			//   * show/raise/activateWindow always run, restore path included.
			auto open = [win, s]() {
				if (!sceneAlive(s)) return;
				ComputeEulerDialog *ed = nullptr;
				auto it = g_ceulerDlgs.find(s);
				if (it != g_ceulerDlgs.end() && it->second && it->second->dlg) ed = it->second;
				if (ed)
					ed->unpark();
				else {
					auto *w = new ComputeEulerDialog(win, s);
					if (!w->dlg) {
						QMessageBox::warning(win, "Compute Euler pole",
							QString("Could not load %1/compute_euler.ui").arg(gmtvtkUiDir()));
						return;
					}
					ed = w;
				}
				QDialog *d = ed->dlg;
				bool onScreen = false;
				for (QScreen *sc : QGuiApplication::screens())
					if (sc->availableGeometry().intersects(d->frameGeometry())) { onScreen = true; break; }
				if (!onScreen)
					d->move(win->frameGeometry().center() - QPoint(d->width() / 2, d->height() / 2));
				d->show();
				d->raise();
				d->activateWindow();
			};
			// Self-rescheduling until no popup is holding the input grab (~1 s of tries at most, then
			// it opens anyway — never silently give up on the user's click).
			auto tries = std::make_shared<int>(0);
			auto step = std::make_shared<std::function<void()>>();
			*step = [win, open, tries, step]() {
				if (QApplication::activePopupWidget() && ++*tries < 30) {
					QTimer::singleShot(30, win, *step);
					return;
				}
				open();
			};
			(*step)();
		});
		reopen();
	};

	(*fGroup)();   // initial population: the discipline chooser

	// --- Tools menu: open the standalone X,Y plot tool (blank; ready for File>Open or Julia) ----
	QMenu *mTools = win->menuBar()->addMenu("&Tools");
	// Opened FROM this viewer, so this viewer's Scene Objects dock is where it parks when closed.
	mTools->addAction("X,Y plot", [s] { if (XYPlot *p = xyOpenBlankFromHost()) p->owner = s; });
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
	// LIDAR2011 PT (port of Mirone's cartas_militares.m 'nikles' mode): a picker over the LIDAR2011 PT
	// survey's 1600x1000 m tile matrix; select cells, "Faz Mosaico" -> Julia reads the tiles and opens
	// the mosaic grid. The tile table is fetched from Julia (op "init") right after construction, so the
	// mesh is painted from data/lidarPT.dat. Non-modal; WA_DeleteOnClose frees it when closed.
	mTools->addAction("LIDAR2011 PT", [win, s]() {
		auto it = g_lidarDlgs.find(s);
		if (it != g_lidarDlgs.end()) { it->second->unpark(); return; }   // the SAME picker, selection intact
		warmupTool("lidarpt");
		QPixmap img;
		if (!g_lidarImg.isEmpty()) img.load(g_lidarImg);
		LidarPicker *dlg = new LidarPicker(win, s, img);
		// WA_DeleteOnClose is deliberately NOT set: closing parks the picker in Scene Objects, and the
		// parked handle has to be able to bring this same dialog back.
		if (g_juliaLidar) g_juliaLidar(s, dlg, "init");    // Julia gmtreads data/lidarPT.dat -> setTiles
		dlg->show();
	});
	// Empilhador (port of Mirone's empilhador.m): stack a list of grids, or of MODIS/VIIRS/SeaWiFS L2
	// scenes, into one 3-D netCDF/VTK/multi-band TIFF/VRT. Non-modal, stays open for a second stack.
	mTools->addAction("Empilhador", [win, s]() {
		warmupTool("empilhador");
		EmpilhadorDialog *e = new EmpilhadorDialog(win, s);
		if (e->dlg) e->dlg->show();
	});

	// --- GMT menu: helper windows to drive GMT modules (TODO: populate with module tools) ----
	QMenu *mGMT = win->menuBar()->addMenu("&GMT");
	mGMT->addAction("grdsample", [win, s]() {
		GrdsampleDialog dlg(win, s);
		dlg.exec();   // Julia is invoked inside the dialog on Apply
	});
	mGMT->addAction("grdgradient", [win, s]() {
		auto *w = new GrdGradientDialog(win, s);   // self-deletes when its QDialog closes (WA_DeleteOnClose)
		if (w->dlg) w->dlg->show();
	});
	mGMT->addAction("grdseamount", [win, s]() {
		auto *w = new GrdSeamountDialog(win, s);
		if (w->dlg) w->dlg->show();
	});
	mGMT->addAction("grdfilter", [win, s]() {
		if (!s->surf || s->emptyStart || s->imageOnly) {
			QMessageBox::warning(win, "grdfilter", "Load a grid into this window first.");
			return;
		}
		auto *w = new GrdFilterDialog(win, s);
		if (w->dlg) w->dlg->show();
	});
	// grdlandmask needs no grid at all (it builds a mask from a region), so it is offered always.
	mGMT->addAction("grdlandmask", [win, s]() {
		auto *w = new GrdLandmaskDialog(win, s);
		if (w->dlg) w->dlg->show();
	});
	// Interpolation needs no grid either — it MAKES one from an x,y,z table.
	mGMT->addAction("Interpolate", [win, s]() {
		auto *w = new InterpolationDialog(win, s);
		if (w->dlg) w->dlg->show();
	});
	mGMT->addAction("grdtrend", [win, s]() {
		if (!s->surf || s->emptyStart || s->imageOnly) {
			QMessageBox::warning(win, "grdtrend", "Load a grid into this window first.");
			return;
		}
		auto *w = new GrdTrendDialog(win, s);
		if (w->dlg) w->dlg->show();
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

	// "Clip Grid" (port of Mirone src_figs/ml_clip.m): threshold/statistical clipping of the window's
	// grid into a NEW derived grid. Opens the clipp_grid.ui dialog (ClipGridDialog). Only meaningful
	// with a grid loaded — offer it only then, like the rest of the grid-modifying tools.
	mGridTools->addSeparator();
	mGridTools->addAction("Clip Grid…", [win, s]() {
		if (!s->surf || s->emptyStart || s->imageOnly) {
			QMessageBox::warning(win, "Clip Grid", "Load a grid into this window first.");
			return;
		}
		auto *w = new ClipGridDialog(win, s);
		if (w->dlg) w->dlg->show();
	});

	// "Contours" (port of Mirone src_figs/contouring.m): trace the window's grid (GDAL marching
	// squares, see src/contours.jl) and draw the levels as one grouped overlay set. Two ways in, the
	// SAME operation behind both — "Automatic" is the tool with its dialog skipped: it asks for the
	// grid's range and hands contourNiceLevels' guess straight to contourDrawLevels, which is exactly
	// what the Contour Tool's "Add Common Charting Intervals" + Compute do by hand.
	QMenu *mContours = mGridTools->addMenu("Contours");
	auto needGrid = [win, s]() {
		if (!s->surf || s->emptyStart || s->imageOnly) {
			QMessageBox::warning(win, "Contours", "Load a grid into this window first.");
			return false;
		}
		return true;
	};
	mContours->addAction("Automatic", [win, s, needGrid]() {
		if (!needGrid()) return;
		double zmn = 0.0, zmx = 0.0;
		if (!contourZRange(s, zmn, zmx)) {
			QMessageBox::warning(win, "Contours", "Could not read this grid's elevation range.");
			return;
		}
		const QVector<double> lv = contourNiceLevels(zmn, zmx);
		if (lv.isEmpty()) {
			QMessageBox::warning(win, "Contours", "This grid has no usable elevation range to contour.");
			return;
		}
		contourDrawLevels(s, win, "0", /*labels=*/true, lv);
	});
	// One dialog per window: if it is only parked (closed with its X, sitting as a handle at the
	// bottom of Scene Objects), this brings THAT one back with its elevation list intact.
	mContours->addAction("Contour Tool…", [win, s, needGrid]() {
		if (!needGrid()) return;
		auto it = g_contourDlgs.find(s);
		if (it != g_contourDlgs.end() && it->second && it->second->dlg) { it->second->unpark(); return; }
		auto *w = new ContourDialog(win, s);
		if (w->dlg) w->dlg->show();
	});

	// "Grid calculator" (port of Mirone src_figs/grid_calculator.m): combine the window's grids with
	// an arithmetic expression. Needs at least the base grid to have something to list.
	mGridTools->addAction("Grid calculator…", [win, s]() {
		if (!s->surf || s->emptyStart || s->imageOnly) {
			QMessageBox::warning(win, "Grid calculator", "Load a grid into this window first.");
			return;
		}
		auto *w = new GridCalculatorDialog(win, s);
		if (w->dlg) w->dlg->show();
	});

	// "Terrain Modeling" (port of Mirone src_figs/multiscale.m + the mirblock.c MEX behind it): pick
	// a moving-window method and a window size, and the chosen quantity is computed for every node.
	// The chooser is deps/ui/multiscale.ui, loaded at RUNTIME (multiscaleAsk below) — a modal
	// one-shot like the .m's, whose only job is to hand (method, window) to Julia's _on_multiscale.
	mGridTools->addAction("Terrain Modeling…", [win, s]() {
		if (!s->surf || s->emptyStart || s->imageOnly) {
			QMessageBox::warning(win, "Terrain Modeling", "Load a grid into this window first.");
			return;
		}
		if (!g_juliaEval) {
			QMessageBox::warning(win, "Terrain Modeling", "This computation needs the Julia/GMT host.");
			return;
		}
		int method = 0, nWin = 3;
		if (!multiscaleAsk(win, method, nWin)) return;
		showBusyDialog("Computing…");
		const QString cmd = QString("InteractiveGMT._on_multiscale(Ptr{Cvoid}(UInt(%1)),%2,%3)")
								.arg((qulonglong)reinterpret_cast<uintptr_t>(s)).arg(method).arg(nWin);
		static std::vector<char> buf(1 << 12);
		int n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
		closeBusyDialog();
		if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));   // Julia threw -> Errors tab
	});

	// The two csaps-driven entries: "Spline Smooth" (port of Mirone mirone.m GridToolsSmooth_CB) and
	// "SDG" (GridToolsSDG_CB) — the same 2-D cubic smoothing spline, one keeping the fitted SURFACE
	// and the other its analytic derivatives (the Second Derivative in the direction of the Gradient,
	// the article-76 FOS detector). Mirone puts a separator above the pair; so do we.
	//
	// Both ask for the SAME thing with the SAME default, so they ask through ONE function: the .m's
	// own inputdlg ("Enter smoothing p parameter", 12 decimals), pre-filled with csaps's own estimate
	// fetched from Julia by the same round-trip the Contour dialog uses to fill its Min/Max. `jlFmt`
	// carries %1 = the scene pointer and %2 = the chosen p.
	mGridTools->addSeparator();
	auto runCsapsTool = [win, s](const char *title, const char *busy, const char *jlFmt) {
		if (!s->surf || s->emptyStart || s->imageOnly) {
			QMessageBox::warning(win, title, "Load a grid into this window first.");
			return;
		}
		if (!g_juliaEval) {
			QMessageBox::warning(win, title, "This computation needs the Julia/GMT host.");
			return;
		}
		static std::vector<char> buf(1 << 12);
		const qulonglong sp = (qulonglong)reinterpret_cast<uintptr_t>(s);
		double p0 = 0.0;
		const QString q = QString("InteractiveGMT._csaps_default_p(Ptr{Cvoid}(UInt(%1)))").arg(sp);
		int n = g_juliaEval(s, q.toStdString().c_str(), buf.data(), (int)buf.size());
		if (n > 0) p0 = QString::fromUtf8(buf.data(), n).trimmed().toDouble();
		bool ok = false;
		const double p = QInputDialog::getDouble(win, "Smoothing parameter input",
			"Enter smoothing p parameter:", p0, 0.0, 1.0, 12, &ok);
		if (!ok) return;
		showBusyDialog(busy);
		const QString cmd = QString(jlFmt).arg(sp).arg(p, 0, 'g', 15);
		n = g_juliaEval(s, cmd.toStdString().c_str(), buf.data(), (int)buf.size());
		closeBusyDialog();
		if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));   // Julia threw -> Errors tab
	};
	mGridTools->addAction("Spline Smooth…", [runCsapsTool]() {
		runCsapsTool("Spline Smooth", "Smoothing the grid…",
		             "InteractiveGMT._on_spline_smooth(Ptr{Cvoid}(UInt(%1)),%2)");
	});
	QMenu *mSDG = mGridTools->addMenu("SDG");
	mSDG->addAction("Positive", [runCsapsTool]() {
		runCsapsTool("SDG", "Computing SDG…", "InteractiveGMT._on_sdg(Ptr{Cvoid}(UInt(%1)),\"positive\",%2)");
	});
	mSDG->addAction("Negative", [runCsapsTool]() {
		runCsapsTool("SDG", "Computing SDG…", "InteractiveGMT._on_sdg(Ptr{Cvoid}(UInt(%1)),\"negative\",%2)");
	});
	mSDG->addAction("Both", [runCsapsTool]() {
		runCsapsTool("SDG", "Computing SDG…", "InteractiveGMT._on_sdg(Ptr{Cvoid}(UInt(%1)),\"both\",%2)");
	});

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

	QMenu *mHelp = win->menuBar()->addMenu("&Help");
	mHelp->addAction("&About", actAbout);
	// Check for Updates: `] dev`-installed checkout only (InteractiveGMT.update!(), selfupdate.jl) --
	// git fetch+fast-forward-merge, then Pkg.build to relink the DLL. Blocking (network + a rebuild),
	// so wrap it in the same indeterminate busy dialog every other slow g_juliaEval call uses; the
	// eval bridge captures update!()'s own println()'d progress lines as its return text (_console_eval,
	// console.jl), which we just show back verbatim in a message box.
	mHelp->addAction("Check for &Updates…", [win, s]() {
		if (!g_juliaEval) {
			QMessageBox::warning(win, "Check for Updates", "This needs the Julia/GMT host.");
			return;
		}
		showBusyDialog("Checking for updates…");
		static std::vector<char> buf(1 << 16);
		int n = g_juliaEval(s, "InteractiveGMT.update!()", buf.data(), (int)buf.size());
		closeBusyDialog();
		QString txt = QString::fromUtf8(buf.data(), n < 0 ? -n : n).trimmed();
		if (txt.isEmpty()) txt = "No output.";
		if (n < 0) QMessageBox::warning(win, "Check for Updates", txt);
		else       QMessageBox::information(win, "Check for Updates", txt);
	});

	// --- toolbar row (below the menu bar): quick-access buttons (ParaView-style) ------------
	// Open file -> route through the SAME drop path as File > Open (g_juliaDrop / _on_drop): the file
	// opens INTO this window (or promotes an empty launcher), NOT iview() which would spawn a new
	// window and auto-send a 2-col table to the X,Y tool. 2D/3D -> the shared act2D toggle.
	QToolBar *tb = win->addToolBar("Main");
	tb->setMovable(false);
	tb->setToolButtonStyle(Qt::ToolButtonIconOnly);   // icon-only toolbar — no text labels on any button
	QAction *actOpen = tb->addAction(win->style()->standardIcon(QStyle::SP_DirOpenIcon), "");  // icon only, no text
	actOpen->setToolTip("Open a grid / image / table file in this window");
	QObject::connect(actOpen, &QAction::triggered, [s, win]() {
		if (!g_juliaDrop) {
			if (s->win) s->win->statusBar()->showMessage("Open: callback not registered", 3000);
			return;
		}
		const QString fn = QFileDialog::getOpenFileName(win, "Open file", prefStartDir());
		if (fn.isEmpty()) return;
		rememberStartDir(fn);
		const QByteArray utf8 = fn.toUtf8();          // keep the buffer alive across the call
		juliaOpenFile(s, utf8.constData());           // into THIS window; a table -> line overlay, never X,Y
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
		{ makeCircleIcon(),   "Circle",    "Draw a circle: click the centre, then a point on the edge.",           Scene::SH_Circle   },
	};
	QToolButton *flyout = new QToolButton(tb);           // the shared shape slot
	flyout->setPopupMode(QToolButton::MenuButtonPopup);  // click icon = use tool; click arrow = flyout
	flyout->setToolButtonStyle(Qt::ToolButtonIconOnly);
	QMenu *shapeMenu = new QMenu(flyout);                // the dropdown flyout list
	QAction *defaultShape = nullptr;                     // the tool the slot starts on (Polyline)
	for (const ToolDef &td : flyoutTools) {
		QAction *act = shapeMenu->addAction(td.icon, td.name);   // icon + label (the slot itself stays icon-only)
		act->setCheckable(true);
		act->setToolTip(td.tip);
		const Scene::ShapeKind kind = td.kind;
		QObject::connect(act, &QAction::toggled, [s, act, kind](bool on){ polygonToolToggled(s, act, kind, on); });
		s->shapeActs.push_back(act);
		if (kind == Scene::SH_Polyline) defaultShape = act;
	}
	flyout->setMenu(shapeMenu);
	// Start on Polyline (icon + tooltip mirror it); fall back to the first entry if Polyline ever goes away.
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
	for (const SymToolDef &td : symTools) {
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
	for (const BodyDef &bd : bodyTools) {
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

	// Swipe / Link: ONE toolbar slot, two ways to compare two rasters (57_swipe.cpp), sitting
	// immediately before the Info flyout. Shaped like the 2D/3D flyout (icon-only slot + a native
	// dropdown arrow). Picking either entry from the dropdown SELECTS that mode AND TURNS IT ON
	// (swipeSelectMode) — picking a tool out of a menu is the act of starting it; there is no second,
	// invisible arming click. The slot's own CLICK is then the plain on/off toggle for the selected
	// mode, via the ONE shared checkable action `actSwipe`, whose `toggled` signal dispatches to
	// swipeToggled or linkToggled depending on `s->swipeToolMode`. `actSwipe` is not itself added to
	// the toolbar — only `tbSwipe` is; the two stay in sync explicitly (see the two connections below)
	// since the button owns the popup menu and must show its own pressed/checked look independently of
	// any QAction default-action wiring (which would fight the per-mode icon swap — same reasoning
	// tb2D's own comment gives for avoiding setDefaultAction).
	//   Swipe — needs >=2 grids/images IN THIS WINDOW; the pair is implicit with exactly two, else a
	//           dialog asks which layer to pair the displayed one with.
	//   Link  — >=2 rasters here pairs two of THEM (right-click flips which shows); with only one
	//           raster here it pairs with another OPEN WINDOW over the same region (right-click
	//           raises that window, framed on what this one is showing).
	QToolButton *tbSwipe = new QToolButton(tb);
	tbSwipe->setPopupMode(QToolButton::MenuButtonPopup);
	tbSwipe->setToolButtonStyle(Qt::ToolButtonIconOnly);
	tbSwipe->setCheckable(true);
	tbSwipe->setEnabled(false);                           // no data yet; availability refreshed on rebuild
	s->swipeToolBtn = tbSwipe;                            // swipeRefreshAvailability's actual on-screen target
	QMenu *swipeModeMenu = new QMenu(tbSwipe);
	QAction *actModeSwipe = swipeModeMenu->addAction("Swipe");
	QAction *actModeLink  = swipeModeMenu->addAction("Link");
	actModeSwipe->setCheckable(true); actModeLink->setCheckable(true);
	actModeSwipe->setToolTip("Swipe: split two layers of this window across a draggable divider");
	actModeLink->setToolTip("Link: right-click switches which of the two paired layers shows");
	auto *swipeModeGroup = new QActionGroup(tbSwipe);     // exclusive: exactly one mode selected
	swipeModeGroup->addAction(actModeSwipe); swipeModeGroup->addAction(actModeLink);
	actModeSwipe->setChecked(true);                       // default mode = Swipe (unchanged prior behaviour)

	QAction *actSwipe = new QAction(tbSwipe);             // the shared ON/OFF toggle (not itself in the menu)
	actSwipe->setCheckable(true);
	s->swipeAct = actSwipe;
	QObject::connect(actSwipe, &QAction::toggled, [s, actSwipe](bool on) {
		s->swipeToolMode == Scene::ToolMode::Link ? linkToggled(s, actSwipe, on) : swipeToggled(s, actSwipe, on);
	});
	// swipeSelectMode (57_swipe.cpp) does the actual work (reset off, icon/tooltip, availability) --
	// the SAME function gmtvtk_swipe_select_mode_h wraps for host/test code, so there is only ONE
	// place this bookkeeping happens.
	QObject::connect(actModeSwipe, &QAction::triggered, [s](bool) { swipeSelectMode(s, false); });
	QObject::connect(actModeLink,  &QAction::triggered, [s](bool) { swipeSelectMode(s, true);  });
	// tbSwipe's OWN checked state drives its pressed/highlighted look; actSwipe is the "real" model
	// (what swipeToggled/linkToggled/swipeRefreshAvailability read and set). A click flips tbSwipe's
	// native checked state FIRST (Qt's own checkable-button handling, before `clicked` fires), so the
	// handler below just copies that INTO actSwipe; the reverse connection then keeps tbSwipe in sync
	// whenever actSwipe changes some OTHER way (a cancelled partner pick, the auto-off timer in
	// swipeRefreshAvailability) — guarded by the equality check so it never fights the click handler.
	QObject::connect(tbSwipe, &QToolButton::clicked, tbSwipe, [actSwipe, tbSwipe]() {
		actSwipe->setChecked(tbSwipe->isChecked());
	});
	QObject::connect(actSwipe, &QAction::toggled, tbSwipe, [tbSwipe](bool on) {
		if (tbSwipe->isChecked() != on) tbSwipe->setChecked(on);
	});
	tbSwipe->setMenu(swipeModeMenu);
	tbSwipe->setIcon(makeSwipeIcon());                    // starting glyph (default mode = Swipe)
	tb->addWidget(tbSwipe);
	// This window's own availability, AND every other window's: Link's cross-window pairing means a
	// new window can be the second half every ALREADY-OPEN window was missing (the mirror of the
	// window-destroyed handler's own re-check).
	for (Scene *o : g_scenes) swipeRefreshAvailability(o);

	// Info flyout sits LAST on the toolbar row (built earlier, added here so it's the rightmost item).
	tb->addSeparator();
	tb->addWidget(tbInfo);

	// --- native right-click context menu over the 3-D view ------------------
	widget->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(widget, &QWidget::customContextMenuRequested,
		[=](const QPoint &pos) {
			// Link owns the right button outright while active: the peek itself is driven by the raw
			// press/release (LinkPeekFilter, 57_swipe.cpp) because a context-menu request cannot
			// express "held" then "released". All that is left here is to SWALLOW the request the
			// platform still raises on release (WM_CONTEXTMENU does not come from the Qt mouse event
			// the filter consumed), so no per-element menu pops in the middle of a peek. Same guards
			// as the two below it, so a rubber-band selection or a polygon drawing in progress keeps
			// its own right-click meaning even with Link on.
			if (s->linkOn && !(s->rbEnabled && (s->rbConsume ||
				(QApplication::keyboardModifiers() & Qt::ControlModifier))) &&
				!(s->polyMode && s->polyDrawing))
				return;
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
				// polyHitText matches every label (all are billboards now, 2026-07-24 — all
				// independently draggable). Each kind gets its OWN properties menu, right on the
				// label itself, never nested inside the symbol/ball's own menu: standalone (Text tool,
				// no groupName) -> textLabelMenu (full editor: string/font/colour + Remove);
				// batch-owned (groupName set) -> batchTextLabelsDialog (font/size/colour/bold/italic/
				// visibility, offering "this label only" vs "all labels in this group" — `tHit` names
				// the CLICKED label, so the dialog knows which one "this" means).
				if (tHit >= 0) {
					if (s->texts[tHit].groupName.empty())
						textLabelMenu(s, s->texts[tHit].actor.Get(), widget->mapToGlobal(pos));
					else
						batchTextLabelsDialog(s, s->texts[tHit].groupName, tHit, widget->mapToGlobal(pos));
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
	QObject::connect(slAz, &QSlider::valueChanged, [s](int v){ s->lightAz = v; dropExternShade(s); applyShading(s); });
	form->addRow("Sun azimuth", slAz);
	wireTip(slAz, "Sun azimuth", 0, 360, "deg", 0);

	QSlider *slEl = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "Sun elevation" slider — key-light elevation above horizon
	slEl->setRange(0, 90); slEl->setValue(int(s->lightEl));
	QObject::connect(slEl, &QSlider::valueChanged, [s](int v){ s->lightEl = v; dropExternShade(s); applyShading(s); });
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

	// Four ALTERNATIVE relief looks — PBR (lit), Cast shadows (lit self-shadowing), Hillshade/Lambert
	// and Hillshade/grdimage — are MUTUALLY EXCLUSIVE but all four may be off. Each toggled handler:
	// when turned ON, uncheck the other three (QSignalBlocker stops their handlers re-firing), then
	// re-derive ALL Scene flags from the live checkbox states (so an off-handler never wrongly clears
	// a flag the just-checked box set). hillGrd selects Lambert vs grdimage; litBake selects the PBR
	// bake (flat image only). PBR is the DEFAULT lit look: on a 3-D surface it IS the GPU shading; on
	// a flat image it bakes a CPU approximation so "Shaded image" alone matches the loaded grid. With
	// every look off on a FLAT image the picture is plain CPT, no shade.
	// "Shaded image (2-D)" is an INDEPENDENT GEOMETRY toggle: ON = fast flat image, OFF = 3-D surface.
	QCheckBox *cbFlat   = new QCheckBox(panel);                                                        // GRAPHICAL ELEMENT: "Shaded image (2-D)" geometry toggle
	QCheckBox *cbPBR    = new QCheckBox(panel); cbPBR->setChecked(!s->useHillshade && !s->useShadows); // GRAPHICAL ELEMENT: "Shade (PBR)" lit-look checkbox
	QCheckBox *cbShadow = new QCheckBox(panel); cbShadow->setChecked(s->useShadows);                  // GRAPHICAL ELEMENT: "Cast shadows" checkbox
	QCheckBox *cbHillL  = new QCheckBox(panel); cbHillL->setChecked(s->useHillshade && !s->hillGrd);  // GRAPHICAL ELEMENT: "Hillshade (Lambert)" checkbox
	QCheckBox *cbHillG  = new QCheckBox(panel); cbHillG->setChecked(s->useHillshade &&  s->hillGrd);  // GRAPHICAL ELEMENT: "Hillshade (grdimage)" checkbox
	s->cbFlat = cbFlat; s->cbShadow = cbShadow; s->cbHillL = cbHillL; s->cbHillG = cbHillG; s->cbPBR = cbPBR;

	// The box describes THE LAYER THE WINDOW IS SHOWING — checked state and enabled state both — asked
	// through the same activeGridLayer/resolveActiveGrid every other per-layer question goes through.
	// It used to be set once, at window build, off the BASE's own gridZ: a window whose displayed grid
	// was an extra (a grid dropped on an image, an Ocean Color subregion) got a permanently dead box.
	s->syncFlatBox = [s, cbFlat]() {
		QSignalBlocker b(cbFlat);
		cbFlat->setChecked(s->layerImgMode);       // the window's own flat/3-D state, whichever layer set it
		cbFlat->setEnabled(resolveActiveGrid(s).valid);
	};
	s->syncFlatBox();

	// Geometry toggle: rebuild THAT layer as flat image / surface (one function, base or extra).
	QObject::connect(cbFlat, &QCheckBox::toggled, [s](bool b){
		ExtraObj *lay = activeGridLayer(s);
		if (!lay) s->cubeFlatImg = b;                        // cube layer switches follow the base's flag
		rebuildLayerFromStored(s, lay, b);
		if (s->syncFlatEnable) s->syncFlatEnable();          // grey/un-grey the flat-dead controls
	});
	form->addRow("Shaded image (2-D)", cbFlat);

	// The four relief looks: mutually exclusive, illumination only (applyShading, in place). syncShade
	// re-derives the Scene flags from the live checkbox states; each ON handler unchecks the other three.
	auto syncShade = [s, cbShadow, cbHillL, cbHillG, cbPBR]() {
		dropExternShade(s);   // picking a look here replaces whatever the Hillshade tool had loaded
		s->useShadows   = cbShadow->isChecked();
		s->useHillshade = cbHillL->isChecked() || cbHillG->isChecked();
		s->hillGrd      = cbHillG->isChecked();
		s->litBake      = cbPBR->isChecked();      // flat PBR bake; a 3-D surface is PBR-lit regardless
		applyShading(s);
		if (s->syncFlatEnable) s->syncFlatEnable();  // which sliders are live depends on the chosen look
	};
	QObject::connect(cbPBR, &QCheckBox::toggled, [=](bool b){
		if (b) { QSignalBlocker bs(cbShadow), bl(cbHillL), bg(cbHillG); cbShadow->setChecked(false); cbHillL->setChecked(false); cbHillG->setChecked(false); }
		syncShade();
	});
	form->addRow("Shade (PBR)", cbPBR);

	QObject::connect(cbShadow, &QCheckBox::toggled, [=](bool b){
		if (b) { QSignalBlocker bp(cbPBR), bl(cbHillL), bg(cbHillG); cbPBR->setChecked(false); cbHillL->setChecked(false); cbHillG->setChecked(false); }
		syncShade();
	});
	form->addRow("Cast shadows", cbShadow);

	QObject::connect(cbHillL, &QCheckBox::toggled, [=](bool b){
		if (b) { QSignalBlocker bp(cbPBR), bs(cbShadow), bg(cbHillG); cbPBR->setChecked(false); cbShadow->setChecked(false); cbHillG->setChecked(false); }
		syncShade();
	});
	form->addRow("Hillshade (Lambert)", cbHillL);

	QObject::connect(cbHillG, &QCheckBox::toggled, [=](bool b){
		if (b) { QSignalBlocker bp(cbPBR), bs(cbShadow), bl(cbHillL); cbPBR->setChecked(false); cbShadow->setChecked(false); cbHillL->setChecked(false); }
		syncShade();
	});
	form->addRow("Hillshade (grdimage)", cbHillG);

	// Grey out the controls a given look can't use, so the dock never offers a control that does
	// nothing. A flat image is a baked texture: IBL / occlusion / cast-shadows need real 3-D geometry;
	// the PBR material + key/fill lights feed the bake ONLY in the flat PBR look (else dead); the sun
	// Az/El feed any flat shade (PBR or hillshade) but not a plain image. Tone / FXAA are screen passes,
	// useless under a baked hillshade (unlit verbatim colours), so blocked there.
	s->syncFlatEnable = [=]() {
		const bool flat    = s->layerImgMode;
		const bool hill    = s->useHillshade;
		const bool pbrBake = flat && s->litBake && !hill;
		const bool matLive = !flat || pbrBake;                 // PBR material + key/fill lights
		for (QWidget *w : { (QWidget*)slRough, (QWidget*)slMetal, (QWidget*)slLight, (QWidget*)slFill })
			w->setEnabled(matLive);
		const bool sunLive = !flat || hill || pbrBake;         // sun az/el: any lit look, not a plain image
		slAz->setEnabled(sunLive); slEl->setEnabled(sunLive);
		for (QWidget *w : { (QWidget*)cbIBL, (QWidget*)slEnv, (QWidget*)cbSSAO, (QWidget*)slSSAO, (QWidget*)cbShadow })
			w->setEnabled(!flat);                              // 3-D geometry only
		cbTone->setEnabled(!hill); cbFXAA->setEnabled(!hill);  // screen passes: dead under a hillshade
	};
	s->syncFlatEnable();

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
	// Mirone's Image > Illuminate, next to the dock it shares its job with (port of shading_params.m).
	mView->addAction("&Illumination (Hillshade)…", [win, s]() {
		// Start compiling the illumination models NOW, while the user is still choosing one — see
		// warmupTool (30_app.cpp). Fires on the re-open path too: the warm-up itself only ever runs
		// once per session, so the second call costs nothing and we never have to reason about which
		// of the two paths the user took.
		warmupTool("illumination");
		auto it = g_hillshadeDlgs.find(s);       // parked or already open -> the SAME dialog, never a 2nd
		if (it != g_hillshadeDlgs.end() && it->second && it->second->dlg) { it->second->unpark(); return; }
		auto *w = new HillshadeDialog(win, s);
		if (w->dlg) w->dlg->show();
	});
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
	auto makeFoldable = [win](QDockWidget *d, QWidget *body, const QString &titleText) -> FoldTitleBar* {
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

	// --- Bottom tabbed panel: Profile / Julia Console / Errors ---------------
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

	// (No "Data Viewer" tab: a table of numbers is shown by THE ONE shared table dialog —
	// buildDataTableDialog, popped by gmtvtk_set_table / show_table — not by a second spreadsheet
	// living here. Two implementations of one operation is exactly what SACRED_LAW forbids.)

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
	// No "Data Viewer Panel" entry: there is no such tab any more. A table of numbers pops up in THE
	// shared table dialog when a result produces one (gmtvtk_set_table / show_table).

	// Empty launcher / blank start: hide the surface, cube axes and gizmo BEFORE the first paint so
	// the window opens as a clean dark canvas instead of flashing an empty blue cube-axes box for one
	// frame (the caller's post-show hides would otherwise only bite on the NEXT render).
	if (blankStart) {
		if (s->surf) s->surf->SetVisibility(0);
		axesHideAll(s->baseAxes);                // empty launcher: nothing to annotate yet
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
	// The open width (1.5x the dock's own minimum) is NOT set here: an empty launcher has nothing in
	// its panel yet and only gets content later, when a file is dropped, so sizing at creation missed
	// exactly that window. rebuildSceneObjects applies it the first time the panel has rows.

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

