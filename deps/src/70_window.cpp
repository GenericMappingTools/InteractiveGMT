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
		QSettings st("InteractiveGMT", "i'GMT");
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
			QSettings st("InteractiveGMT", "i'GMT");
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

static QuadNode* buildQuadNode(int i0, int i1, int j0, int j1, int level,
							   double x0, double dx, double y0, double dy) {
	QuadNode* n = new QuadNode();
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

static void ensureNodeActor(Scene* s, QuadNode* n) {
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

static void dropNodeActor(Scene* s, QuadNode* n) {
	if (!n->actor) return;
	s->surfGroup->RemovePart(n->actor);
	for (size_t k = 0; k < s->tiles.size(); ++k)
		if (s->tiles[k] == n->actor) { s->tiles.erase(s->tiles.begin() + k); break; }
	s->lodResidentBytes = (s->lodResidentBytes >= n->bytes) ? s->lodResidentBytes - n->bytes : 0;
	n->actor = nullptr; n->bytes = 0;
}

static void dropSubtree(Scene* s, QuadNode* n) {
	if (!n) return;
	dropNodeActor(s, n);
	for (int k = 0; k < 4; ++k) dropSubtree(s, n->child[k]);
}

static void collectResident(QuadNode* n, std::vector<QuadNode*>& out) {
	if (!n) return;
	if (n->actor) out.push_back(n);
	for (int k = 0; k < 4; ++k) collectResident(n->child[k], out);
}

static void evictLRU(Scene* s) {
	std::vector<QuadNode*> res; collectResident(s->quadRoot, res);
	std::sort(res.begin(), res.end(), [](QuadNode* a, QuadNode* b){ return a->lastUsed < b->lastUsed; });
	for (QuadNode* n : res) {
		if (s->lodResidentBytes <= s->lodBudgetBytes) break;
		if (n->lastUsed == s->lodFrame) continue;   // never evict a tile drawn this frame
		dropNodeActor(s, n);
	}
}

static void refineNode(Scene* s, QuadNode* n, vtkCamera* cam, const double camPos[3],
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

static void refineQuadtree(Scene* s) {
	if (!s->quadRoot || !s->ren) return;
	vtkCamera* cam = s->ren->GetActiveCamera(); if (!cam) return;
	double camPos[3]; cam->GetPosition(camPos);
	int* sz = s->ren->GetSize(); const double vpH = (sz && sz[1] > 0) ? sz[1] : 600.0;
	const bool parallel = cam->GetParallelProjection() != 0;
	const double tanHalf = std::tan(vtkMath::RadiansFromDegrees(cam->GetViewAngle() * 0.5));
	s->lodFrame++;
	refineNode(s, s->quadRoot, cam, camPos, vpH, tanHalf, cam->GetParallelScale(), parallel, /*tau=*/2.0);
	if (s->lodResidentBytes > s->lodBudgetBytes) evictLRU(s);
}

static void onLodCamera(vtkObject*, unsigned long, void* cd, void*) {
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
	explicit FoldTitleBar(const QString& t, QWidget* parent = nullptr)
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

// Fold / un-fold the Shading dock programmatically (Surface row click in the Scene Objects panel).
// Lives here because FoldTitleBar is complete only in this TU fragment; 50_scene.cpp forward-decls it.
static void toggleShadingFold(Scene *s) {
	if (s && s->shadeFoldBar && s->shadeFoldBar->onClick)
		s->shadeFoldBar->onClick();
}

// Polygon draw/edit tool (defined in 85_polygon.cpp, #included after this file). The toolbar
// button toggles draw mode via polygonSetMode; the mouse gestures are driven from GLView.
static void polygonSetMode(Scene* s, bool on);
static void polygonToolToggled(Scene* s, QAction* act, Scene::ShapeKind shape, bool on);
static QIcon makePolygonIcon();
static QIcon makePolylineIcon();
static QIcon makeLineIcon();
static QIcon makeRectIcon();
static QIcon makeCircleIcon();
static QIcon makeTextIcon();
static QIcon makeViewModeIcon(bool twoD);   // "2D"/"3D" glyph for the icon-only view-toggle button
static int  polyHitText(Scene* s, int x, int y, double tol);   // text label under the cursor (85_polygon.cpp)


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
	QSettings st("InteractiveGMT", "i'GMT");
	const QStringList paths = st.value("recent/paths").toStringList();
	const QVariantList cats  = st.value("recent/cats").toList();
	for (int i = 0; i < paths.size(); ++i)
		g_recent.push_back({ paths[i], (i < cats.size()) ? cats[i].toInt() : 2 });
}

static void saveRecent() {
	QStringList paths; QVariantList cats;
	for (const RecentItem& r : g_recent) { paths << r.path; cats << r.cat; }
	QSettings st("InteractiveGMT", "i'GMT");
	st.setValue("recent/paths", paths);
	st.setValue("recent/cats", cats);
}

// Promote a freshly-opened file to the front of the MRU (de-dup, cap, persist).
static void addRecentFile(const char* cpath, int cat) {
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
static void populateRecentMenu(QMenu *menu, Scene* s) {
	loadRecent();
	menu->clear();
	static const char* kCatName[3] = { "Grids", "Images", "Datasets" };
	bool any = false;
	for (int c = 0; c < 3; ++c) {
		bool header = false;
		for (const RecentItem& r : g_recent) {
			if (r.cat != c) continue;
			if (!header) { QAction *h = menu->addAction(kCatName[c]); h->setEnabled(false); header = true; }
			const QString full = r.path;
			QAction* act = menu->addAction(QFileInfo(full).fileName());
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
	if (!any) { QAction* none = menu->addAction("(no recent files)"); none->setEnabled(false); }
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
static void buildSceneContent(Scene* s, vtkSmartPointer<vtkPolyData> pd,
                              double x0, double x1, double y0, double y1,
                              const double* cz, const double* crgb, int ncolor,
                              const unsigned char* img, int iw, int ih, int ibands,
                              int edges, bool pointCloud, int geographic,
                              const float* gz, int gnx, int gny, bool blankStart) {
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
		vtkTextProperty* tp = t->GetTextProperty();
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
		vtkTextProperty* tp = s->axes->GetTitleTextProperty(i);
		tp->SetColor(1.0, 1.0, 1.0); tp->SetFontFamilyToArial(); tp->BoldOff(); tp->ItalicOff(); tp->ShadowOff();
		vtkTextProperty* lp = s->axes->GetLabelTextProperty(i);
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
	buildColorbar(s, lut);

	// Default view: world +Z up; azimuth 0 (look north, +Y) and elevation 35deg above
	// horizontal. Camera sits due south of the focal point, raised 35deg. Then zoom in so
	// the relief fills most of the display (ResetCamera alone leaves a wide margin).
	s->ren->ResetCamera();
	{
		vtkCamera* cam = s->ren->GetActiveCamera();
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
	Scene* s = nullptr;
	std::function<void(double, double)> cb;
	bool down = false, moved = false; double px = 0, py = 0;
	MapPickFilter(Scene* sc, QObject* parent, std::function<void(double, double)> f)
		: QObject(parent), s(sc), cb(std::move(f)) {}
protected:
	bool eventFilter(QObject* obj, QEvent* ev) override {
		const QEvent::Type t = ev->type();
		if (t == QEvent::MouseButtonPress) {
			QMouseEvent* me = static_cast<QMouseEvent*>(ev);
			if (me->button() == Qt::LeftButton) {
				down = true; moved = false; px = me->position().x(); py = me->position().y();
				return true;                                  // swallow so VTK doesn't start a rotate
			}
		} else if (t == QEvent::MouseMove && down) {
			QMouseEvent* me = static_cast<QMouseEvent*>(ev);
			if (std::abs(me->position().x() - px) > 3 || std::abs(me->position().y() - py) > 3) moved = true;
			return true;
		} else if (t == QEvent::MouseButtonRelease && down) {
			QMouseEvent* me = static_cast<QMouseEvent*>(ev);
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
static void showEarthTidesDialog(Scene* s, double cW, double cE, double cS, double cN) {
	if (!g_juliaEarthTide) {
		if (s->win) s->win->statusBar()->showMessage("Earth Tides: callback not registered", 3000);
		return;
	}
	const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
	QDialog* dlg = new QDialog(s->win);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle("Earth Tides");

	QDateTimeEdit* eStart = new QDateTimeEdit(nowUtc, dlg);
	QDateTimeEdit* eEnd   = new QDateTimeEdit(nowUtc.addDays(10), dlg);
	for (QDateTimeEdit* e : { eStart, eEnd }) {
		e->setDisplayFormat("dd-MMM-yyyy HH:mm:ss"); e->setCalendarPopup(true); e->setTimeSpec(Qt::UTC);
	}
	QDoubleSpinBox* eLon = new QDoubleSpinBox(dlg);
	eLon->setRange(-360.0, 360.0); eLon->setDecimals(4); eLon->setValue(0.5 * (cW + cE));
	QDoubleSpinBox* eLat = new QDoubleSpinBox(dlg);
	eLat->setRange(-90.0, 90.0); eLat->setDecimals(4); eLat->setValue(0.5 * (cS + cN));

	QRadioButton* rSeries = new QRadioButton("Time series", dlg); rSeries->setChecked(true);
	QRadioButton* rGrid   = new QRadioButton("Grid(s)", dlg);
	QButtonGroup* mode = new QButtonGroup(dlg);
	mode->addButton(rSeries); mode->addButton(rGrid);
	// Grid(s) uses a single instant (Start date) over a global region, so freeze End date then; a
	// time series spans Start->End, so unfreeze it for "Time series".
	QObject::connect(rGrid, &QRadioButton::toggled, dlg, [eEnd](bool on) { eEnd->setEnabled(!on); });
	eEnd->setEnabled(!rGrid->isChecked());                 // initial state (series default -> enabled)

	QCheckBox* cV = new QCheckBox("Vertical", dlg); cV->setChecked(true);
	QCheckBox* cE2 = new QCheckBox("East", dlg);
	QCheckBox* cN2 = new QCheckBox("North", dlg);

	QPushButton* bPick = new QPushButton("Click point on map", dlg);

	// Grid spacing (degrees) for Grid(s) mode; relevant only when gridding -> enabled with rGrid.
	QDoubleSpinBox* eInc = new QDoubleSpinBox(dlg);
	eInc->setRange(0.05, 10.0); eInc->setDecimals(2); eInc->setSingleStep(0.25); eInc->setValue(0.5);
	QObject::connect(rGrid, &QRadioButton::toggled, dlg, [eInc](bool on) { eInc->setEnabled(on); });
	eInc->setEnabled(rGrid->isChecked());                  // disabled in the default Time-series mode

	// Layout: left column = dates + mode + components; right column = lon/lat + grid inc + pick + OK.
	QFormLayout* left = new QFormLayout;
	left->addRow("Start date:", eStart);
	left->addRow("End date:",   eEnd);
	left->addRow(rSeries);
	left->addRow(rGrid);
	left->addRow(cV);
	left->addRow(cE2);
	left->addRow(cN2);

	QFormLayout* right = new QFormLayout;
	right->addRow("Lon:", eLon);
	right->addRow("Lat:", eLat);
	right->addRow("Grid inc (°):", eInc);
	right->addRow(bPick);
	QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
	right->addRow(bb);

	QHBoxLayout* cols = new QHBoxLayout(dlg);
	cols->addLayout(left); cols->addSpacing(16); cols->addLayout(right);

	// "Click point on map": hide the dialog, arm a one-shot pick on the map widget; on click refill
	// Lon/Lat and reshow. (Grid region still comes from the visible extent captured at menu time.)
	QObject::connect(bPick, &QPushButton::clicked, dlg, [s, dlg, eLon, eLat]() {
		if (!s->widget) return;
		dlg->lower();                                       // keep visible (don't hide), just out of the way
		if (s->win) s->win->statusBar()->showMessage("Earth Tides: click a point on the map…", 5000);
		MapPickFilter* f = new MapPickFilter(s, s->widget, [dlg, eLon, eLat](double lon, double lat) {
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
		const char* m = rGrid->isChecked() ? "grid" : "series";
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

static Scene* buildAndShow(vtkSmartPointer<vtkPolyData> pd,
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
						 bool blankStart = false) {         // empty launcher: open as a clean dark canvas (no axes flash)
	ensureApp();

	Scene *s = new Scene();
	g_scenes.insert(s);                     // register as a live figure handle
	s->imageOnly = imageOnly;               // set BEFORE the Scene Objects panel is built (rebuildSceneObjects)
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
	QObject::connect(win, &QObject::destroyed, [s]() {
		--g_openWindows;
		if (g_lastScene == s) g_lastScene = nullptr;   // don't let add_overlay touch a freed scene
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
		QString fn = QFileDialog::getSaveFileName(s->win, "Save screenshot", "gmtvtk.png", "PNG (*.png)");
		if (fn.isEmpty()) return;
		vtkNew<vtkWindowToImageFilter> w2i;
		w2i->SetInput(s->widget->renderWindow());
		w2i->SetScale(2); w2i->Update();
		vtkNew<vtkPNGWriter> wr;
		wr->SetFileName(fn.toLocal8Bit().constData());
		wr->SetInputConnection(w2i->GetOutputPort());
		wr->Write();
	};
	auto actToggleGizmo = [s]() {
		if (s->giz) { setGizmoVisible(*s->giz, !s->giz->visible); s->widget->renderWindow()->Render(); }
	};
	// State-driven flat-2D <-> 3D switch. 2D = collapse VE to a plane, top-down orthographic,
	// rotation/tilt locked (gated in DragCB via s->flat2d), gizmo hidden. The Z tick billboards
	// self-hide when the drawn Z extent is zero (placeTickBillboards d0==d1 guard). Idempotent:
	// calling with the current state is a no-op. The shared act2D checkmark is kept in sync.
	auto setFlat2D = [s](bool on) {
		if (on == s->flat2d) { if (s->act2D) s->act2D->setChecked(on); return; }
		vtkCamera* cam = s->ren->GetActiveCamera();
		s->flat2d = on;
		if (s->flat2d) {
			cam->GetPosition(s->sav_pos);          // save the 3D view to restore later
			cam->GetFocalPoint(s->sav_foc);
			cam->GetViewUp(s->sav_vup);
			s->sav_parallel = cam->GetParallelProjection();

			// 2D = TOP-DOWN ORTHO ONLY. Keep the relief and its PBR lighting exactly as in 3D
			// (illumination must NOT change) — viewed straight down in parallel projection it reads
			// as a shaded-relief map. We do NOT flatten (ve) or touch lighting: flattening kills the
			// hillshade, and LightingOff on PBR renders near-black.
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
		if (s->act2D) s->act2D->setChecked(s->flat2d);
		s->widget->renderWindow()->Render();
	};
	auto actToggle2D = [s, setFlat2D]() { setFlat2D(!s->flat2d); };
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
	mFile->addAction("Save &Screenshot…", actShot);
	// Save Grid / Save Image: each opens the format-picker dialog (saveObjectDialog) for the window's
	// primary object (empty name). Greyed out when the window holds no grid / no image — refreshed on
	// every menu open so it tracks drops, basemap tiles, etc.
	QAction* aSaveGrid  = mFile->addAction("Save &Grid…",  [s]() { saveObjectDialog(s, "grid",  QString()); });
	QAction* aSaveImage = mFile->addAction("Save &Image…", [s]() { saveObjectDialog(s, "image", QString()); });
	// Background region: open a blank white 2-D map framed to W/E/S/N (default the whole geographic
	// earth). The dialog hands "W/E/S/N/geographic" to Julia (g_juliaBgRegion), which opens a fresh
	// window — ready to drop coastlines / overlays onto. Reports if the callback is not wired.
	QAction* aBgRegion = mFile->addAction("&Background region…", [win, s]() {
		BgRegionDialog dlg(win);
		if (dlg.exec() != QDialog::Accepted || dlg.region.isEmpty()) return;
		if (g_juliaBgRegion) g_juliaBgRegion(s, dlg.region.toUtf8().constData());
		else if (s->win) s->win->statusBar()->showMessage("Background region: callback not registered", 3000);
	});
	// Per-open gating: Save entries reflect what's loaded; Background region is hidden for good once the
	// window holds ANY content (a grid, an image, or any dropped extra) — it only makes sense on a bare
	// launcher whose limits are still up for grabs.
	QObject::connect(mFile, &QMenu::aboutToShow, [s, aSaveGrid, aSaveImage, aBgRegion]() {
		aSaveGrid->setEnabled(sceneHasGrid(s));
		aSaveImage->setEnabled(sceneHasImage(s));
		const bool hasContent = (s->surf && !s->emptyStart) || !s->extras.empty();
		aBgRegion->setVisible(!hasContent);
	});
	mFile->addSeparator();
	// Recent Files: persistent MRU, grouped Grids/Images/Datasets, rebuilt each time it opens so a
	// file opened in any window shows up here too. Re-opens a pick in a NEW window via iview().
	QMenu* mRecent = mFile->addMenu("Recent &Files");
	mRecent->setToolTipsVisible(true);                       // show the full path on hover
	QObject::connect(mRecent, &QMenu::aboutToShow, [mRecent, s]() { populateRecentMenu(mRecent, s); });
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
			const QString req = QString("%1/%2/%3/%4/%5/%6").arg(kind).arg(res)
				.arg(W, 0, 'f', 6).arg(E, 0, 'f', 6).arg(S, 0, 'f', 6).arg(N, 0, 'f', 6);
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
	s->geoMenu = mGeo;                              // gmtvtk_set_crs reveals it once the data has a CRS
	mGeo->menuAction()->setVisible(false);         // hidden until a referencing system is known
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
	mGeo->addAction("Global seismicity (1990-2009)", geoTODO("Global seismicity"));
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

	mGeo->addSeparator();
	mGeo->addAction("Atlas", geoTODO("Atlas"));

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

	win->menuBar()->addMenu("&Help")->addAction("&About", actAbout);

	// --- toolbar row (below the menu bar): quick-access buttons (ParaView-style) ------------
	// Open file -> hand the path back to Julia (iview auto-dispatches grid/image/dataset into a
	// NEW window). 2D/3D -> the shared act2D toggle. More buttons can be appended here later.
	QToolBar *tb = win->addToolBar("Main");
	tb->setMovable(false);
	tb->setToolButtonStyle(Qt::ToolButtonIconOnly);   // icon-only toolbar — no text labels on any button
	QAction* actOpen = tb->addAction(win->style()->standardIcon(QStyle::SP_DirOpenIcon), "");  // icon only, no text
	actOpen->setToolTip("Open a grid / image / table file in a new window");
	QObject::connect(actOpen, &QAction::triggered, [s, win]() {
		const QString fn = QFileDialog::getOpenFileName(win, "Open file");
		if (fn.isEmpty() || !g_juliaEval) return;
		// Build iview("…") with the path safely quoted (raw string => backslashes are literal).
		std::string cmd = "InteractiveGMT.iview(raw\"" + fn.toStdString() + "\")";
		static std::vector<char> buf(1 << 12);
		int n = g_juliaEval(s, cmd.c_str(), buf.data(), (int)buf.size());
		if (n < 0) sceneLogError(s, QString::fromUtf8(buf.data(), -n));   // open failed -> Errors tab
	});
	// 2D/3D toggle: an icon-only button whose glyph shows the CURRENT view ("3D" in 3D, "2D" in flat
	// 2D). Its own QToolButton (not act2D, which keeps the descriptive "Flat 2D (map)" menu text).
	// The icon tracks act2D's checked state, so any toggle source (menu, context menu, bare-image
	// init in 90_c_api) keeps it in sync.
	QToolButton *tb2D = new QToolButton(tb);
	tb2D->setToolButtonStyle(Qt::ToolButtonIconOnly);
	tb2D->setIcon(makeViewModeIcon(false));
	tb2D->setToolTip("Toggle flat 2D map / 3D perspective view");
	QObject::connect(tb2D, &QToolButton::clicked, actToggle2D);
	QObject::connect(s->act2D, &QAction::toggled, tb2D, [tb2D](bool on){ tb2D->setIcon(makeViewModeIcon(on)); });
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
	struct ToolDef { QIcon icon; const char* name; const char* tip; Scene::ShapeKind kind; };
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
	QToolButton* flyout = new QToolButton(tb);           // the shared shape slot
	flyout->setPopupMode(QToolButton::MenuButtonPopup);  // click icon = use tool; click arrow = flyout
	flyout->setToolButtonStyle(Qt::ToolButtonIconOnly);
	QMenu* shapeMenu = new QMenu(flyout);                // the dropdown flyout list
	for (const ToolDef& td : flyoutTools) {
		QAction* act = shapeMenu->addAction(td.icon, td.name);   // icon + label (the slot itself stays icon-only)
		act->setCheckable(true);
		act->setToolTip(td.tip);
		const Scene::ShapeKind kind = td.kind;
		QObject::connect(act, &QAction::toggled, [s, act, kind](bool on){ polygonToolToggled(s, act, kind, on); });
		s->shapeActs.push_back(act);
	}
	flyout->setMenu(shapeMenu);
	flyout->setDefaultAction(shapeMenu->actions().first());      // start on Polygon (icon + tooltip mirror it)
	// Picking a sibling from the flyout makes it the slot's current tool (Illustrator behaviour): the
	// chosen action toggles on (its connection enters draw mode) and becomes the button's default.
	QObject::connect(shapeMenu, &QMenu::triggered, flyout, [flyout](QAction* a){ flyout->setDefaultAction(a); });
	tb->addWidget(flyout);

	// Text — its own icon-only toggle (not a "drawn shape" family member, but shares the exclusive
	// s->shapeActs group so selecting it untoggles the active shape tool and vice-versa).
	QAction* actText = tb->addAction(makeTextIcon(), "");
	actText->setCheckable(true);
	actText->setToolTip("Place a text label: click a point on the scene, then type the text.");
	QObject::connect(actText, &QAction::toggled, [s, actText](bool on){ polygonToolToggled(s, actText, Scene::SH_Text, on); });
	s->shapeActs.push_back(actText);

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
					textLabelMenu(s, s->texts[tHit].actor, widget->mapToGlobal(pos));
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
			QAction* ca = m.addAction("Cube Axes", actToggleAxes);
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
	auto wireTip = [](QSlider* sl, QString name, double rmin, double rmax, QString unit, int dec) {
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
	QSlider* slSSAO = new QSlider(Qt::Horizontal, panel);   // GRAPHICAL ELEMENT: "SSAO radius" slider — ambient-occlusion sampling radius
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
	auto makeFoldable = [win](QDockWidget* d, QWidget* body, const QString& titleText) -> FoldTitleBar* {
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
	QTabWidget*  tabs = new QTabWidget(bottomDock);
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
	QPlainTextEdit* conOut = new QPlainTextEdit(conPanel);
	conOut->setReadOnly(true);
	conOut->setFont(QFont("Consolas", 10));
	conOut->setPlaceholderText("Julia output appears here. `fig` is this window. e.g.  add!(fig, [x y z]; mode=:points)");
	QLineEdit* conIn = new QLineEdit(conPanel);
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
	QPlainTextEdit* errOut = new QPlainTextEdit(tabs);
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
	QWidget*     titleBar = new QWidget(bottomDock);
	QHBoxLayout* titleLay = new QHBoxLayout(titleBar);
	titleLay->setContentsMargins(6, 2, 6, 2);
	titleLay->setSpacing(4);
	QToolButton* hideBtn = new QToolButton(titleBar);
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
	QToolButton* floatBtn = new QToolButton(titleBar);
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
	auto showTab = [s](QWidget* page) {
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
	// Polygon draw/edit tool: gestures are handled in the GLView widget (mouse*Event overrides,
	// 60_profile.cpp), gated on the tool state, so navigation is untouched when the tool is idle.
	// non-blocking: return now; the host pumps gmtvtk_process_events().
	return s;
}

