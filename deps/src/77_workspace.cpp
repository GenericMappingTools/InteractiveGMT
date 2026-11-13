// 77_workspace.cpp — the WORKSPACE dock (the status corner's table button): MATLAB's workspace browser for the
// Julia session this viewer runs in. One row per variable of `Main` (name, value, size, type, bytes). A
// double-click shows the variable in THE shared table dialog (show_table -> gmtvtk_set_table), grids
// and images included; the right-click menu offers the picture instead, in this window or a new one,
// through the same open path a dropped file takes (src/workspace.jl).
//
// The list and every action are asked of Julia through ONE callback, g_juliaWorkspace: the answer has
// its own buffer and never travels as the console's captured stdout.
//
// Built lazily, the first time the panel is asked for: a window that never opens it pays nothing.
// While it is visible it re-asks the list once a second; Julia answers from a cheap signature of the
// bindings, and the tree is rebuilt only when the text that comes back has changed.

// fn(scene, request, out, cap, err, errcap). Two-phase: out == NULL runs the request and returns the
// byte length of its answer; a second call with a buffer of that size copies it. -1 = failure, the
// reason written to `err`. Requests are TAB-separated: "list", "list!" (rebuild, ignore the cache), or
// "<op>\t<name>[\t<arg>]" with op = table (arg = image band) / here / new / summary.
typedef int (*JuliaWorkspaceFn)(void *scene, const char *req, char *out, int cap, char *err, int errcap);
static JuliaWorkspaceFn g_juliaWorkspace = nullptr;

static const char *kWsDockName = "igWorkspaceDock";
static bool g_wsBusy = false;          // an action is running: the 1 s refresh stays out of its way

// A row that sorts its Bytes column by the byte count, not by the "1.2 KiB" text.
struct WsItem : public QTreeWidgetItem {
	using QTreeWidgetItem::QTreeWidgetItem;
	bool operator<(const QTreeWidgetItem &o) const override {
		// the two groups ("This window", "Julia session") keep their order whichever way the rows sort
		const QVariant ra = data(0, Qt::UserRole + 4), rb = o.data(0, Qt::UserRole + 4);
		if (ra.isValid() && rb.isValid()) {
			const bool desc = treeWidget() && treeWidget()->header()->sortIndicatorOrder() == Qt::DescendingOrder;
			return desc ? ra.toInt() > rb.toInt() : ra.toInt() < rb.toInt();
		}
		const int c = treeWidget() ? treeWidget()->sortColumn() : 0;
		if (c == 4) return data(4, Qt::UserRole).toDouble() < o.data(4, Qt::UserRole).toDouble();
		return QTreeWidgetItem::operator<(o);
	}
};

// One request -> its answer. False, with `err` set, on failure.
static bool wsAsk(Scene *s, const QString &req, QByteArray &out, QString &err) {
	if (!g_juliaWorkspace) {
		err = "The Workspace panel is not connected to Julia.";
		return false;
	}
	const QByteArray rq = req.toUtf8();
	const int errcap = 4096;
	QByteArray eb(errcap, '\0');
	const int n = g_juliaWorkspace(s, rq.constData(), nullptr, 0, eb.data(), errcap);
	if (n < 0) {
		err = QString::fromUtf8(eb.constData());
		return false;
	}
	out.resize(n);
	if (n > 0 && g_juliaWorkspace(s, rq.constData(), out.data(), n, eb.data(), errcap) != n) {
		err = QString::fromUtf8(eb.constData());
		return false;
	}
	return true;
}

// Re-list the variables into `tree`. Unless `force`, an answer identical to the last one leaves the
// tree alone (selection and scroll included). A failure becomes one inert row, never a pop-up: this
// runs once a second.
static void wsRefresh(Scene *s, QTreeWidget *tree, bool force) {
	if (!sceneAlive(s) || g_wsBusy) return;
	QByteArray out;
	QString err;
	QString txt;
	if (!wsAsk(s, force ? "list!" : "list", out, err))
		txt = "\x01" + err;            // can never equal a real listing
	else
		txt = QString::fromUtf8(out);
	if (!force && txt == tree->property("igWsLast").toString()) return;
	tree->setProperty("igWsLast", txt);

	const QString cur = tree->currentItem() ? tree->currentItem()->text(0) : QString();
	const int scroll = tree->verticalScrollBar()->value();
	// the two groups keep the open/closed state the user left them in
	for (int g = 0; g < tree->topLevelItemCount(); ++g) {
		QTreeWidgetItem *gi = tree->topLevelItem(g);
		const QString key = gi->data(0, Qt::UserRole + 3).toString();
		if (!key.isEmpty()) tree->setProperty(("igWsFold_" + key).toUtf8().constData(), !gi->isExpanded());
	}
	tree->setUpdatesEnabled(false);
	tree->setSortingEnabled(false);
	tree->clear();
	if (txt.startsWith(QChar(1))) {
		auto *it = new QTreeWidgetItem(tree, {txt.mid(1)});
		it->setFlags(Qt::NoItemFlags);
	}
	else {
		// Two groups: the grids and images of THIS window's Scene Objects (bound to Julia variables by
		// workspace.jl, so the console computes with them by name), then the rest of the session.
		auto group = [tree](const QString &key, const QString &title, int rank, const QString &tip) {
			auto *g = new WsItem(tree, {title});
			g->setData(0, Qt::UserRole + 3, key);
			g->setData(0, Qt::UserRole + 4, rank);
			g->setFlags(Qt::ItemIsEnabled);
			g->setFirstColumnSpanned(true);
			QFont f = g->font(0);
			f.setBold(true);
			g->setFont(0, f);
			g->setToolTip(0, tip);
			return g;
		};
		QTreeWidgetItem *gScene = nullptr, *gJulia = nullptr;
		// name \t value \t size \t type \t bytes \t kind \t bands \t nbytes \t origin
		for (const QString &line : txt.split('\n', Qt::SkipEmptyParts)) {
			const QStringList f = line.split('\t');
			if (f.size() < 8) continue;
			const QString origin = f.value(8);
			QTreeWidgetItem *parent;
			if (!origin.isEmpty()) {
				if (!gScene) gScene = group("scene", "This window", 0,
					"The grids and images of this window's Scene Objects. Each one is a Julia variable "
					"of that name: use it in the console (mask = name .> 100).");
				parent = gScene;
			}
			else {
				if (!gJulia) gJulia = group("julia", "Julia session", 1, "The variables of the Julia session (Main).");
				parent = gJulia;
			}
			auto *it = new WsItem(parent, {f[0], f[1], f[2], f[3], f[4]});
			it->setData(0, Qt::UserRole, f[5]);
			it->setData(0, Qt::UserRole + 1, f[6].toInt());
			it->setData(0, Qt::UserRole + 2, origin);
			it->setData(4, Qt::UserRole, f[7].toDouble());
			it->setToolTip(0, origin.isEmpty() ? f[0] : QString("%1 = Scene Objects \"%2\"").arg(f[0], origin));
			it->setToolTip(1, f[1]);
			it->setToolTip(3, f[3]);
			it->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
			if (f[0] == cur) tree->setCurrentItem(it);
		}
		for (QTreeWidgetItem *g : {gScene, gJulia})
			if (g) g->setExpanded(!tree->property(("igWsFold_" + g->data(0, Qt::UserRole + 3).toString())
			                                          .toUtf8().constData()).toBool());
	}
	tree->setSortingEnabled(true);
	for (int c = 0; c < tree->columnCount(); ++c) tree->resizeColumnToContents(c);
	tree->verticalScrollBar()->setValue(scroll);
	tree->setUpdatesEnabled(true);
}

// Run one action on a variable. The busy notice goes up around the call (a big grid's table takes a
// moment to build); a failure pops up.
static void wsAct(Scene *s, QTreeWidget *tree, const QString &op, const QString &name,
                  const QString &arg = QString()) {
	if (!sceneAlive(s) || g_wsBusy) return;
	QString req = op + '\t' + name;
	if (!arg.isEmpty()) req += '\t' + arg;
	const QString what = (op == "table")   ? QString("Building the table of %1...").arg(name) :
	                     (op == "summary") ? QString("Describing %1...").arg(name) :
	                     (op == "plot")    ? QString("Plotting %1...").arg(name) :
	                                         QString("Showing %1...").arg(name);
	QByteArray out;
	QString err;
	g_wsBusy = true;
	showBusyDialog(what.toUtf8().constData());
	const bool ok = wsAsk(s, req, out, err);
	closeBusyDialog();
	g_wsBusy = false;
	if (!ok) {
		QMessageBox::warning(tree, "Workspace", err);
		return;
	}
	if (op == "summary")
		showInfoText(tree->window(), "Workspace — " + name, QString::fromUtf8(out));
	if (sceneAlive(s)) wsRefresh(s, tree, false);
}

// What a double-click does: the table for anything that has numbers in it; a solid opens in a new
// window; anything else shows its summary.
static void wsOpenDefault(Scene *s, QTreeWidget *tree, QTreeWidgetItem *it) {
	const QString kind = it->data(0, Qt::UserRole).toString();
	const QString name = it->text(0);
	if (kind.isEmpty()) return;                     // the inert error row
	if (kind == "fv")
		wsAct(s, tree, "new", name);
	else if (kind == "other")
		wsAct(s, tree, "summary", name);
	else
		wsAct(s, tree, "table", name, kind == "image" ? "1" : QString());
}

static void wsMenu(Scene *s, QTreeWidget *tree, const QPoint &p) {
	QMenu m(tree);
	QTreeWidgetItem *it = tree->itemAt(p);
	const QString kind = it ? it->data(0, Qt::UserRole).toString() : QString();
	if (it && !kind.isEmpty()) {
		const QString name = it->text(0);
		const int nb = it->data(0, Qt::UserRole + 1).toInt();
		const bool inWindow = !it->data(0, Qt::UserRole + 2).toString().isEmpty();   // already shown here
		QAction *def = nullptr;
		// The first, plain-words option: "Plot it" for a series (X,Y tool), "See it" for anything that
		// is a picture or a solid (a new window, through the same door as "...in a new window").
		if (kind == "vector" || kind == "dataset")
			m.addAction("Plot it", [s, tree, name]() { wsAct(s, tree, "plot", name); });
		else if (kind == "grid" || kind == "image" || kind == "matrix" || kind == "fv") {
			QAction *see = m.addAction("See it", [s, tree, name]() { wsAct(s, tree, "new", name); });
			if (kind == "fv") def = see;                    // a solid's double-click does the same
		}
		if (m.actions().size()) m.addSeparator();
		if (kind == "image" && nb > 1) {
			QMenu *bm = m.addMenu("Show as table");
			for (int b = 1; b <= nb; ++b)
				bm->addAction(QString("Band %1").arg(b), [s, tree, name, b]() {
					wsAct(s, tree, "table", name, QString::number(b));
				});
		}
		else if (kind != "fv" && kind != "other")
			def = m.addAction("Show as table", [s, tree, name, kind]() {
				wsAct(s, tree, "table", name, kind == "image" ? "1" : QString());
			});
		if (kind == "grid" || kind == "image" || kind == "matrix") {
			if (!inWindow)
				m.addAction("Show as image in this window", [s, tree, name]() { wsAct(s, tree, "here", name); });
			m.addAction("Show as image in a new window", [s, tree, name]() { wsAct(s, tree, "new", name); });
		}
		else if (kind == "dataset") {
			m.addAction("Plot in this window",  [s, tree, name]() { wsAct(s, tree, "here", name); });
			m.addAction("Plot in a new window", [s, tree, name]() { wsAct(s, tree, "new", name); });
		}
		m.addSeparator();
		QAction *sum = m.addAction("Show summary", [s, tree, name]() { wsAct(s, tree, "summary", name); });
		if (kind == "other") def = sum;
		if (def) {
			QFont f = def->font();
			f.setBold(true);
			def->setFont(f);
		}
		m.addAction("Copy name", [name]() { QApplication::clipboard()->setText(name); });
		m.addSeparator();
	}
	m.addAction("Refresh", [s, tree]() { wsRefresh(s, tree, true); });
	m.exec(tree->viewport()->mapToGlobal(p));
}

// ROOM FOR THE PANEL: the window is rarely as wide as the screen, so before the panel takes width from
// the display area the window itself grows into the free screen space -- to the right first, then to
// the left -- by as much of `need` as there is room for. What it grew (and how far it moved left) is
// kept on the dock, and given back when the panel folds or hides, never more: the user's own window
// size is never shrunk. A maximized / full-screen window, or a floating panel, takes nothing.
// One room-making step (grow + show, fold + give back, ...) is several geometry changes; painted one by
// one, the display area widens, then narrows again -- a visible blink. The window does not paint while
// the step runs, and its LAYOUT is switched off: the window's new width and the dock's appearance are
// laid out together, ONCE, at the end, so the display area (the central widget) never passes through
// an in-between size -- it is resized at most once, and only when the screen had no room to give.
struct WsFreeze {
	QMainWindow *win;
	explicit WsFreeze(QMainWindow *w) : win(w) {
		win->setUpdatesEnabled(false);
		if (win->layout()) win->layout()->setEnabled(false);
	}
	~WsFreeze() {
		QApplication::sendPostedEvents(win, QEvent::Resize);
		if (QLayout *l = win->layout()) {
			l->setEnabled(true);
			l->activate();
		}
		QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
		win->setUpdatesEnabled(true);
	}
};

static bool wsGrowable(QMainWindow *win, QDockWidget *dock) {
	return !dock->isFloating() && !win->isMaximized() && !win->isFullScreen() && win->screen();
}

static void wsGrow(QMainWindow *win, QDockWidget *dock, int need) {
	if (need <= 0 || !wsGrowable(win, dock)) return;
	const QRect av = win->screen()->availableGeometry();
	const QRect fr = win->frameGeometry();
	const int right = std::max(0, av.right() - fr.right());
	const int left  = std::max(0, fr.left() - av.left());
	const int g = std::min(need, right + left);
	if (g <= 0) return;
	const int shift = std::max(0, g - right);          // what the right edge could not take
	const QRect geo = win->geometry();
	win->setGeometry(geo.x() - shift, geo.y(), geo.width() + g, geo.height());
	dock->setProperty("igWsGrown", dock->property("igWsGrown").toInt() + g);
	dock->setProperty("igWsShift", dock->property("igWsShift").toInt() + shift);
}

static void wsGiveBack(QMainWindow *win, QDockWidget *dock, int amount) {
	const int grown = dock->property("igWsGrown").toInt(), shift = dock->property("igWsShift").toInt();
	if (win->isMaximized() || win->isFullScreen()) {     // the user took the window over: forget it
		dock->setProperty("igWsGrown", 0);
		dock->setProperty("igWsShift", 0);
		return;
	}
	const int g = std::min(amount, grown);
	if (g <= 0) return;
	const int sb = std::min(shift, g);
	const QRect geo = win->geometry();
	win->setGeometry(geo.x() + sb, geo.y(), geo.width() - g, geo.height());
	dock->setProperty("igWsGrown", grown - g);
	dock->setProperty("igWsShift", shift - sb);
}

// The panel's open width, plus the splitter Qt puts between it and the display area.
static int wsOpenNeed(QMainWindow *win, QDockWidget *dock) {
	auto *bar = dynamic_cast<FoldTitleBar*>(dock->titleBarWidget());
	const int w = (bar && bar->openWidth > 0) ? bar->openWidth : 340;
	return w + win->style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, win);
}

static void wsHide(QMainWindow *win, QDockWidget *dock) {
	WsFreeze fz(win);
	dock->hide();
	wsGiveBack(win, dock, INT_MAX);
}

static bool g_wsNoGrow = false;        // the un-fold inside workspaceShow: the room was already made

// The status corner's Workspace button: build the dock on first use, then show it and list the variables.
static void workspaceShow(Scene *s) {
	if (!sceneAlive(s) || !s->win) return;
	QMainWindow *win = s->win;
	QDockWidget *dock = win->findChild<QDockWidget*>(kWsDockName);
	const bool wasHidden = !dock || dock->isHidden();
	if (!dock) {
		dock = new QDockWidget("Workspace", win);
		dock->setObjectName(kWsDockName);
		dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
		QTreeWidget *tree = new QTreeWidget(dock);
		tree->setColumnCount(5);
		tree->setHeaderLabels({"Name", "Value", "Size", "Type", "Bytes"});
		tree->setRootIsDecorated(true);             // the "This window" / "Julia session" groups
		tree->setUniformRowHeights(true);
		tree->setAlternatingRowColors(true);
		tree->setSortingEnabled(true);
		tree->sortByColumn(0, Qt::AscendingOrder);
		tree->setContextMenuPolicy(Qt::CustomContextMenu);
		dock->setWidget(tree);
		dock->hide();                                   // shown below, once the window has made room
		win->addDockWidget(Qt::RightDockWidgetArea, dock);
		dock->hide();
		installDockGeometryMemory(win, dock, "workspaceDock");
		QObject::connect(tree, &QTreeWidget::itemDoubleClicked, tree, [s, tree](QTreeWidgetItem *it, int) {
			if (sceneAlive(s) && it) wsOpenDefault(s, tree, it);
		});
		QObject::connect(tree, &QWidget::customContextMenuRequested, tree, [s, tree](const QPoint &p) {
			if (sceneAlive(s)) wsMenu(s, tree, p);
		});
		QTimer *tick = new QTimer(dock);
		tick->setInterval(1000);
		QObject::connect(tick, &QTimer::timeout, tree, [s, tree, dock]() {
			if (sceneAlive(s) && tree->isVisible()) wsRefresh(s, tree, false);   // not while folded
		});
		QObject::connect(dock, &QDockWidget::visibilityChanged, tick, [s, tree, tick](bool vis) {
			if (!vis) {
				tick->stop();
				return;
			}
			if (sceneAlive(s)) wsRefresh(s, tree, false);
			tick->start();
		});
		// Folds as the Scene Objects dock does (THE side-dock fold, 10_geometry.cpp), with the float
		// and close glyphs its own title bar would have had.
		FoldTitleBar *bar = makeDockFoldable(win, dock, tree, "Workspace");
		bar->openWidth = 340;
		bar->onFloat = [win, dock]() { dockToggleFloat(win, dock); };
		bar->onClose = [win, dock]() { wsHide(win, dock); };
		// Fold / un-fold through THE fold, with the window giving back / making the width it changes.
		bar->onClick = [win, dock, bar, fold = bar->onClick]() {
			std::optional<WsFreeze> fz;
			if (win->updatesEnabled()) fz.emplace(win);                   // (not already inside one)
			if (bar->folded) {                                            // un-folding
				// the window first (the display area is pinned), then the dock takes the new room
				if (!g_wsNoGrow)                                          // (the splitter is already there)
					wsGrow(win, dock, wsOpenNeed(win, dock) - dock->width() -
						win->style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, win));
				fold();
			}
			else {
				const int before = dock->width();
				fold();
				wsGiveBack(win, dock, before - bar->sizeHint().width());
			}
		};
		bar->updateGeometry();
		// the status corner's button shows pressed while the panel is on screen (folded or open)
		if (auto *btn = win->findChild<QToolButton*>("igWorkspaceBtn"))
			QObject::connect(dock, &QDockWidget::visibilityChanged, btn, [btn, dock](bool) {
				btn->setChecked(!dock->isHidden());
			});
	}
	auto *bar = dynamic_cast<FoldTitleBar*>(dock->titleBarWidget());
	// The list FIRST, before anything on screen moves: the first one is Julia compiling it (seconds),
	// and asked for inside the step below it left the window grown but unpainted all that time.
	if (auto *tree = qobject_cast<QTreeWidget*>(dock->widget())) {
		QApplication::setOverrideCursor(Qt::WaitCursor);
		wsRefresh(s, tree, true);
		QApplication::restoreOverrideCursor();
	}
	WsFreeze fz(win);
	// the window first (the display area is pinned), then the dock is shown into the new room
	if (wasHidden) wsGrow(win, dock, wsOpenNeed(win, dock));
	dock->show();
	dock->raise();
	// asked for while folded: open it (the room is made below, for the whole open width)
	if (bar && bar->folded && bar->onClick) {
		g_wsNoGrow = wasHidden;
		bar->onClick();
		g_wsNoGrow = false;
	}
	if (wasHidden && bar && !dock->isFloating())
		win->resizeDocks({dock}, {wsOpenNeed(win, dock) -
			win->style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, win)}, Qt::Horizontal);
}

// The status corner's Workspace button: on screen (open or folded) -> hidden, otherwise shown.
static void workspaceToggle(Scene *s) {
	if (!sceneAlive(s) || !s->win) return;
	QDockWidget *dock = s->win->findChild<QDockWidget*>(kWsDockName);
	if (dock && !dock->isHidden()) wsHide(s->win, dock);
	else                           workspaceShow(s);
	dock = s->win->findChild<QDockWidget*>(kWsDockName);
	if (auto *btn = s->win->findChild<QToolButton*>("igWorkspaceBtn"))
		btn->setChecked(dock && !dock->isHidden());
}
