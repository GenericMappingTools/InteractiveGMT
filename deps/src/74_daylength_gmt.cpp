// 74_daylength_gmt.cpp -- T & L > "Day length & axial tilt".
//
// The first T & L demo: how the tilt of Earth's rotation axis sets the length of the day through a
// year -- globe with its day/night line (side or polar view), hours of daylight per latitude, and the
// whole-year latitude x day-of-year map. The native, animated orbit demo beside it is 74_daylength.cpp.
//
// TWO LAYERS (src/daylength.jl's banner): the BASE picture -- everything that is the same on every day
// -- is drawn by GMT on the host once per tilt and projection and handed over as PNG bytes; the day's
// OVERLAY -- night, terminator, overhead Sun, bars, cursor, title -- comes as a small display list in
// page centimetres and is painted here with QPainter. A day therefore costs a list of a few hundred
// numbers and a paint, not a GMT run plus Ghostscript.
//
// Both host callbacks are two-phase like every host payload: out == nullptr computes and returns the
// size, the second call copies it. 0 = failure, the reason in `err`.

typedef int (*JuliaDayLengthBaseFn)(double tilt, int view, unsigned char *out, int cap, char *err, int errcap);
typedef int (*JuliaDayLengthListFn)(int doy, double tilt, int view, double *out, int cap, char *err, int errcap);
static JuliaDayLengthBaseFn g_juliaDayLengthBase = nullptr;
static JuliaDayLengthListFn g_juliaDayLengthList = nullptr;
static bool dayLengthPicWired() { return g_juliaDayLengthBase && g_juliaDayLengthList; }

struct DayLengthPicDialog {
	QDialog *dlg = nullptr;
	Scene *scn = nullptr;
	QLabel *view = nullptr, *dateLbl = nullptr;
	QScrollBar *slider = nullptr;
	QPushButton *play = nullptr;
	QDoubleSpinBox *tilt = nullptr;
	QComboBox *proj = nullptr;
	QSpinBox *step = nullptr;
	QTimer *timer = nullptr;
	QPixmap base;                            // the GMT-drawn picture for the current tilt + projection
	QPixmap shown;                           // base + the day's overlay
	bool busy = false;                       // a host call is running: no second one on top of it
	// The year map's yellow disc (display-list record 6), page cm: dragging it sets the day.
	double hx = 0, hy = 0, hr = 0, hx0 = 0, hx1 = 1, pageW = 0, pageH = 0;
	int hnd = 0;
	bool dragging = false;
	bool parked = false;
	bool reallyClose = false;

	void unpark() {
		if (!dlg) return;
		parked = false;
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

	bool parkNow() {
		stopPlay();
		if (reallyClose || !dlg || !scn || !sceneAlive(scn)) return false;
		if (parked) {
			dlg->hide();
			return true;
		}
		parked = true;
		dlg->hide();
		parkTool(scn, dlg, dlg->windowTitle(), IC_Image,
		         "Closed " + dlg->windowTitle() + " — double-click to bring it back, click for Show / Delete",
		         [this]() { unpark(); }, parkedMenu());
		return true;
	}

	static QString dateOf(int doy) {
		return QDate(2026, 1, 1).addDays(doy - 1).toString("d MMMM");
	}

	void fail(const QString &what, const char *err) {
		stopPlay();
		QMessageBox::warning(dlg, dlg->windowTitle(), QString("%1:\n\n%2").arg(what, QString::fromUtf8(err)));
	}

	// The base picture for the current tilt and projection (the one slow step: a GMT run). The wait
	// cursor and the date line say it is working; no pop-up (user, 2026-10-23).
	bool drawBase() {
		if (!dayLengthPicWired()) {
			QMessageBox::warning(dlg, dlg->windowTitle(), "The day-length demo's Julia side is not wired (src/daylength.jl).");
			return false;
		}
		busy = true;
		dateLbl->setText("(drawing…)");
		QApplication::setOverrideCursor(Qt::WaitCursor);
		QApplication::processEvents();
		char err[2048] = {0};
		const double t = tilt->value();
		const int v = proj->currentIndex();
		const int n = g_juliaDayLengthBase(t, v, nullptr, 0, err, int(sizeof(err)));
		std::vector<unsigned char> buf(n > 0 ? size_t(n) : 0);
		QPixmap pm;
		const bool ok = n > 0 && g_juliaDayLengthBase(t, v, buf.data(), n, err, int(sizeof(err))) == n &&
		                pm.loadFromData(buf.data(), uint(n), "PNG");
		QApplication::restoreOverrideCursor();
		busy = false;
		if (!ok) {
			fail("Could not draw the picture", err);
			return false;
		}
		base = pm;
		return true;
	}

	// Paint the day's display list (src/daylength.jl, daylength_overlay) over the base. Page cm ->
	// pixels by one scale: the base picture IS the page (W x H cm), cropped to it.
	void paintOverlay(const std::vector<double> &L, QPixmap &pm, int doy) {
		const double W = L[0], H = L[1];
		const double k = pm.width() / W;                      // pixels per cm
		const double kpt = k * 2.54 / 72.0;                   // pixels per point
		auto P = [&](double x, double y) { return QPointF(x * k, (H - y) * k); };
		auto col = [&](size_t i) { return QColor(int(L[i]), int(L[i + 1]), int(L[i + 2])); };
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing);
		size_t i = 2;
		while (i < L.size()) {
			const int kind = int(L[i]);
			if (kind == 1 || kind == 2) {
				const int n = int(L[i + 1]);
				const size_t o = i + (kind == 1 ? 6 : 7);
				QPolygonF poly;
				for (int j = 0; j < n; ++j) poly << P(L[o + 2 * j], L[o + 2 * j + 1]);
				if (kind == 1) {
					QColor c = col(i + 2);
					c.setAlphaF(float(L[i + 5]));
					p.setPen(Qt::NoPen);
					p.setBrush(c);
					p.drawPolygon(poly);
				}
				else {
					QPen pen(col(i + 2), std::max(1.0, L[i + 5] * kpt));
					if (L[i + 6] != 0) pen.setStyle(Qt::DashLine);
					p.setPen(pen);
					p.setBrush(Qt::NoBrush);
					p.drawPolyline(poly);
				}
				i = o + 2 * size_t(n);
			}
			else if (kind == 3) {
				p.setPen(QPen(Qt::black, std::max(1.0, L[i + 7] * kpt)));
				p.setBrush(col(i + 4));
				p.drawEllipse(P(L[i + 1], L[i + 2]), L[i + 3] * k, L[i + 3] * k);
				i += 8;
			}
			else if (kind == 4) {
				QFont f("Helvetica");
				f.setPixelSize(std::max(7, int(std::lround(L[i + 5] * kpt))));
				p.setFont(f);
				p.setPen(Qt::black);
				const QString txt = QString::number(L[i + 3], 'f', 1);
				const QPointF a = P(L[i + 1], L[i + 2]);
				const double w = 200.0, h = f.pixelSize() * 1.4;
				const QRectF r = L[i + 4] > 0 ? QRectF(a.x() - w, a.y() - h / 2, w, h) : QRectF(a.x(), a.y() - h / 2, w, h);
				p.drawText(r, (L[i + 4] > 0 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, txt);
				i += 6;
			}
			else if (kind == 5) {
				QFont f("Helvetica");
				f.setBold(true);
				f.setPixelSize(int(std::lround(L[i + 3] * kpt)));
				p.setFont(f);
				p.setPen(Qt::black);
				const QString txt = QString("%1  ·  Sun's declination %2°  ·  axial tilt %3°")
				    .arg(dateOf(doy)).arg(QString::asprintf("%+5.1f", L[i + 5]).trimmed())
				    .arg(L[i + 6], 0, 'f', 2);
				const QPointF a = P(L[i + 1], L[i + 2]);
				p.drawText(QRectF(a.x() - 2000, a.y() - f.pixelSize(), 4000, 2.0 * f.pixelSize()), Qt::AlignCenter, txt);
				i += 7;
			}
			else if (kind == 6) {                         // the year-map disc's drag handle, not drawn
				hx = L[i + 1], hy = L[i + 2], hr = L[i + 3], hx0 = L[i + 4], hx1 = L[i + 5];
				hnd = int(L[i + 6]);
				pageW = W, pageH = H;
				i += 7;
			}
			else
				break;                                    // unknown record: stop rather than misread
		}
	}

	// Widget pixel -> page cm, through the picture as fit() lays it (aspect kept, centred).
	QPointF toPage(const QPointF &w) const {
		if (shown.isNull() || pageW <= 0) return QPointF(-1, -1);
		const QSize sz = shown.size().scaled(view->size(), Qt::KeepAspectRatio);
		const double ox = (view->width() - sz.width()) / 2.0, oy = (view->height() - sz.height()) / 2.0;
		return QPointF((w.x() - ox) / sz.width() * pageW, pageH - (w.y() - oy) / sz.height() * pageH);
	}
	bool overDisc(const QPointF &w) const {
		if (hnd < 2) return false;
		const QPointF p = toPage(w);
		return std::hypot(p.x() - hx, p.y() - hy) <= hr * 1.8;
	}
	int dayAt(const QPointF &w) const {
		const double x = toPage(w).x();
		return std::clamp(int(std::lround(1 + (x - hx0) * (hnd - 1) / (hx1 - hx0))), 1, hnd);
	}

	void fit() {
		if (shown.isNull()) return;
		view->setPixmap(shown.scaled(view->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}

	// Show day `doy`: the base (drawn first if there is none) with the day's overlay painted on it.
	void showDay(int doy) {
		if (busy) return;
		if (base.isNull() && !drawBase()) return;
		char err[2048] = {0};
		const double t = tilt->value();
		const int v = proj->currentIndex();
		const int n = g_juliaDayLengthList(doy, t, v, nullptr, 0, err, int(sizeof(err)));
		std::vector<double> L(n > 0 ? size_t(n) : 0);
		if (n < 2 || g_juliaDayLengthList(doy, t, v, L.data(), n, err, int(sizeof(err))) != n) {
			fail(QString("Could not draw %1").arg(dateOf(doy)), err);
			return;
		}
		shown = base.copy();
		paintOverlay(L, shown, doy);
		dateLbl->setText(dateOf(doy));
		fit();
	}

	// A new tilt or projection: a new base, then the day on it.
	void rebase() {
		base = QPixmap();
		showDay(slider->value());
	}

	void stopPlay() {
		if (timer && timer->isActive()) timer->stop();
		if (play) play->setText("Play");
	}

	void tick() {
		if (busy) return;
		int d = slider->value() + step->value();
		if (d > slider->maximum()) d = slider->minimum() + (d - slider->maximum() - 1);
		slider->setValue(d);                 // valueChanged -> showDay
	}
};

static DayLengthPicDialog *g_dayLengthPic = nullptr;

// Open (or bring back) the dialog.
static bool dayLengthPicOpen(QWidget *parent, Scene *scene) {
	if (g_dayLengthPic) {
		g_dayLengthPic->unpark();
		g_dayLengthPic->scn = scene;
		return true;
	}
	auto *m = new DayLengthPicDialog;
	m->scn = scene;
	QDialog *dlg = new QDialog(parent);
	m->dlg = dlg;
	dlg->setWindowTitle("Day length & axial tilt");
	dlg->setWindowIcon(appIcon());
	dlg->setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint |
	                    Qt::WindowMaximizeButtonHint);

	m->view = new QLabel(dlg);
	m->view->setAlignment(Qt::AlignCenter);
	m->view->setMinimumSize(480, 390);
	m->view->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);   // the picture follows the window
	m->view->setStyleSheet("background: white;");

	m->play = new QPushButton("Play", dlg);
	m->play->setToolTip("Run the year.");
	m->slider = new QScrollBar(Qt::Horizontal, dlg);
	m->slider->setRange(1, 365);
	m->slider->setSingleStep(1);
	m->slider->setPageStep(7);
	m->slider->setValue(172);
	m->slider->setToolTip("Day of the year (2026).");
	// ARROWS AT BOTH TIPS, as Aquamoto's slice slider (75_aquamoto.cpp): no native style draws them
	// reliably in this Qt build, so the bar is a plain groove + handle and the arrows are real buttons.
	m->slider->setStyleSheet(
		"QScrollBar:horizontal { border: none; background: #e0e0e0; height: 8px; margin: 0px; border-radius: 4px; }"
		"QScrollBar::handle:horizontal { background: #808080; border-radius: 4px; min-width: 20px; }"
		"QScrollBar::handle:horizontal:hover { background: #606060; }"
		"QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
		"QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }");
	auto *dayLeft  = new QToolButton(dlg);
	auto *dayRight = new QToolButton(dlg);
	dayLeft->setArrowType(Qt::LeftArrow);
	dayRight->setArrowType(Qt::RightArrow);
	dayLeft->setAutoRepeat(true);
	dayRight->setAutoRepeat(true);
	dayLeft->setToolTip("One day back");
	dayRight->setToolTip("One day forward");
	QObject::connect(dayLeft, &QToolButton::clicked, dlg, [m]() {
		m->slider->setValue(m->slider->value() > 1 ? m->slider->value() - 1 : m->slider->maximum());
	});
	QObject::connect(dayRight, &QToolButton::clicked, dlg, [m]() {
		m->slider->setValue(m->slider->value() < m->slider->maximum() ? m->slider->value() + 1 : 1);
	});
	m->dateLbl = new QLabel(dlg);
	m->dateLbl->setMinimumWidth(110);
	m->step = new QSpinBox(dlg);
	m->step->setRange(1, 30);
	m->step->setValue(1);
	m->step->setPrefix("step ");
	m->step->setSuffix(" d");
	m->step->setToolTip("Days advanced at each Play frame.");
	m->proj = new QComboBox(dlg);
	m->proj->addItems({"Orthographic", "Polar (North)", "Polar (South)"});
	m->proj->setToolTip("How the globe is seen: from the side (90° away from the Sun), or looking down on "
	                    "the North or the South pole.");
	m->tilt = new QDoubleSpinBox(dlg);
	m->tilt->setRange(0.0, 90.0);
	m->tilt->setDecimals(2);
	m->tilt->setSingleStep(1.0);
	m->tilt->setValue(23.44);
	m->tilt->setPrefix("tilt ");
	m->tilt->setSuffix("°");
	m->tilt->setToolTip("Axial tilt (obliquity). Earth's is 23.44°. 0° gives 12 h everywhere all year; "
	                    "more than 23.44° pushes the polar circles towards the equator.");
	QPushButton *earth = new QPushButton("Earth", dlg);
	earth->setToolTip("Back to Earth's tilt, 23.44°.");
	for (QPushButton *b : {m->play, earth}) {
		b->setAutoDefault(false);
		b->setDefault(false);
	}

	auto *row = new QHBoxLayout;
	row->addWidget(m->play);
	row->addWidget(dayLeft);
	row->addWidget(m->slider, 1);
	row->addWidget(dayRight);
	row->addWidget(m->dateLbl);
	row->addWidget(m->step);
	row->addWidget(m->proj);
	row->addWidget(m->tilt);
	row->addWidget(earth);
	auto *lay = new QVBoxLayout(dlg);
	lay->setContentsMargins(6, 6, 6, 6);
	lay->addWidget(m->view, 1);
	lay->addLayout(row);
	// Fit the screen it opens on (never taller than 85 % of its available height, title bar on screen),
	// the width following the picture's shape (page 26.5 x 20.6 cm) plus the control row.
	{
		QScreen *scr = parent && parent->screen() ? parent->screen() : QGuiApplication::primaryScreen();
		const QRect av = scr->availableGeometry();
		const int h = std::min(900, int(av.height() * 0.85));
		const int w = std::min(std::max(int((h - 50) * 26.5 / 20.6) + 12, 900), int(av.width() * 0.9));
		dlg->resize(w, h);
		dlg->move(av.x() + (av.width() - w) / 2, av.y() + std::max(0, (av.height() - h - 30) / 2));
	}

	m->timer = new QTimer(dlg);
	m->timer->setInterval(40);
	QObject::connect(m->timer, &QTimer::timeout, dlg, [m]() { m->tick(); });
	QObject::connect(m->play, &QPushButton::clicked, dlg, [m]() {
		if (m->timer->isActive()) {
			m->stopPlay();
			return;
		}
		m->play->setText("Pause");
		m->timer->start();
	});
	QObject::connect(m->slider, &QScrollBar::valueChanged, dlg, [m](int d) { m->showDay(d); });
	// The year map's yellow disc drags along the year: it sets the slider, so it IS the slider.
	struct DiscDrag : QObject {
		DayLengthPicDialog *d;
		DiscDrag(QObject *p, DayLengthPicDialog *dd) : QObject(p), d(dd) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::MouseButtonPress) {
				auto *me = static_cast<QMouseEvent *>(e);
				if (me->button() == Qt::LeftButton && d->overDisc(me->position())) {
					d->dragging = true;
					return true;
				}
			}
			else if (e->type() == QEvent::MouseMove) {
				auto *me = static_cast<QMouseEvent *>(e);
				if (d->dragging) {
					d->slider->setValue(d->dayAt(me->position()));
					return true;
				}
				d->view->setCursor(d->overDisc(me->position()) ? Qt::SizeAllCursor : Qt::ArrowCursor);
			}
			else if (e->type() == QEvent::MouseButtonRelease && d->dragging) {
				d->dragging = false;
				return true;
			}
			return QObject::eventFilter(o, e);
		}
	};
	m->view->setMouseTracking(true);
	m->view->installEventFilter(new DiscDrag(m->view, m));
	QObject::connect(m->proj, &QComboBox::currentIndexChanged, dlg, [m](int) { m->rebase(); });
	QObject::connect(m->tilt, &QDoubleSpinBox::editingFinished, dlg, [m]() { m->rebase(); });
	QObject::connect(earth, &QPushButton::clicked, dlg, [m]() {
		const QSignalBlocker b(m->tilt);
		m->tilt->setValue(23.44);
		m->rebase();
	});

	// The picture follows the window's size.
	struct Refit : QObject {
		DayLengthPicDialog *d;
		Refit(QObject *p, DayLengthPicDialog *dd) : QObject(p), d(dd) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::Resize) d->fit();
			return QObject::eventFilter(o, e);
		}
	};
	m->view->installEventFilter(new Refit(m->view, m));

	// PARKED LIKE EVERY TOOL WINDOW (parkTool / parkOnMinimise, 50_scene.cpp).
	struct CloseParks : QObject {
		DayLengthPicDialog *d;
		CloseParks(QObject *p, DayLengthPicDialog *dd) : QObject(p), d(dd) {}
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::Close) {
				if (d->parkNow()) {
					e->ignore();
					return true;
				}
				d->dlg->deleteLater();
			}
			return QObject::eventFilter(o, e);
		}
	};
	dlg->installEventFilter(new CloseParks(dlg, m));
	parkOnMinimise(dlg, [m]() { m->parkNow(); });
	QObject::connect(dlg, &QDialog::rejected, dlg, [m]() {   // Esc
		if (!m->parkNow()) m->dlg->deleteLater();
	});
	QObject::connect(dlg, &QObject::destroyed, [m]() {
		unparkTool(m->scn, m->dlg);
		if (g_dayLengthPic == m) g_dayLengthPic = nullptr;
		delete m;
	});
	if (parent) QObject::connect(parent, &QObject::destroyed, dlg, [m]() {
		m->reallyClose = true;
		m->dlg->deleteLater();
	});

	wrapTooltips(dlg);
	g_dayLengthPic = m;
	dlg->show();
	QApplication::processEvents();
	m->showDay(m->slider->value());
	return true;
}
