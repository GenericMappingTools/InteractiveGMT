// ============================================================================
//  wcd_window.cpp -- the water column viewer's window: a port of kmwcd_viewer.py's MainWindow and
//  _PickDialog (Christian dos Santos Ferreira, MARUM / MB-System). The data side -- KMALL reading,
//  geometry, navigation, the pick -- is Julia's (src/wcdviewer.jl), reached through WcdHost.
//
//  What is kmwcd_viewer.py's, kept: the controls and what each does (depth window, amplitude range,
//  fixed across-track width, replay rate, Bottom, Pick, Save Image at twice the resolution with the
//  metadata line written on it), the ping slider stepping one ping per wheel notch, Left / Right keys,
//  the plot (raw amplitude, viridis, across-track drawn as -sin(angle)*range, depth down), the bottom
//  detection colours and symbols, and the picked-positions table with its Save as TXT / Remove
//  selected / Close. What is new is only the drawing: matplotlib's pcolormesh (shading 'nearest') is
//  painted here by looking up, for each pixel, the beam nearest in angle and the sample nearest in
//  range -- the same nearest-cell picture, without a plotting library.
// ============================================================================

#include "wcd_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QUiLoader>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace {

struct Wcd;
Wcd *g_wcd = nullptr;

class WcdCanvas : public QWidget {
public:
	explicit WcdCanvas(Wcd *m, QWidget *parent) : QWidget(parent), m_(m) {
		setMinimumSize(400, 220);
		setFocusPolicy(Qt::ClickFocus);
	}

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *e) override;

private:
	Wcd *m_;
};

struct Wcd {
	WcdHost host;
	MbParking *parking = nullptr;
	QMainWindow *win = nullptr;
	WcdCanvas *canvas = nullptr;
	QLabel *labelFile = nullptr, *labelMeta = nullptr, *labelPing = nullptr;
	QDoubleSpinBox *dmin = nullptr, *dmax = nullptr, *amin = nullptr, *amax = nullptr, *xmax = nullptr, *rate = nullptr;
	QCheckBox *chkBottom = nullptr, *chkPick = nullptr;
	QPushButton *btnReplay = nullptr;
	QSlider *slider = nullptr;
	QTimer *replay = nullptr;
	QTableWidget *table = nullptr;          // the picked positions, under the plot
	QWidget *picksPanel = nullptr;          // the table + its buttons: hidden until the first pick

	QString fileName;
	int npings = 0, cur = 0;
	std::vector<double> cz, crgb;            // the colour scale (viridis) over 0..1

	// the ping on display
	int nb = 0, ns = 0;
	std::vector<float> amp;                  // beam-major
	std::vector<double> ang;
	std::vector<int> order;                  // beams sorted by angle
	double dr = 0.0, xlo = 0.0, xhi = 0.0;
	QString meta;
	std::vector<double> bottom;              // 4 per detection: x, depth, colour code, hollow
	bool bottomLoaded = false;
	struct PickMark {
		int ping;
		double x, d;
	};
	std::vector<PickMark> marks;             // the picks, marked on the plot of the ping they were taken on

	// the plot's frame, in widget pixels, and the data range it shows
	QRectF plot;
	double x0 = -1, x1 = 1, d0 = 0, d1 = 1;
};

QWidget *wcdLoadUi(Wcd *m, const char *file, QWidget *parent) {
	QFile f(QDir(m->host.base.uiDir).filePath(QString::fromLatin1(file)));
	if (!f.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(parent, "Water column viewer", QString("Cannot open %1").arg(f.fileName()));
		return nullptr;
	}
	QUiLoader loader;
	QWidget *w = loader.load(&f, parent);
	if (!w)
		QMessageBox::warning(parent, "Water column viewer", QString("Cannot load %1").arg(f.fileName()));
	return w;
}

template <typename T>
T *wcdChild(QWidget *root, const char *name, QStringList &missing) {
	T *w = root->findChild<T *>(QString::fromLatin1(name));
	if (!w)
		missing << QString::fromLatin1(name);
	return w;
}

// the colour of an amplitude: the scale clamps at both ends, as matplotlib's vmin / vmax do
QRgb wcdColour(Wcd *m, double v) {
	const double lo = m->amin->value(), hi = m->amax->value();
	double t = (hi > lo) ? (v - lo) / (hi - lo) : 0.5;
	t = std::clamp(t, 0.0, 1.0);
	const size_t n = m->cz.size();
	if (n < 2)
		return qRgb(int(255 * t), int(255 * t), int(255 * t));
	size_t k = std::upper_bound(m->cz.begin(), m->cz.end(), t) - m->cz.begin();
	k = std::clamp<size_t>(k, 1, n - 1);
	const double z0 = m->cz[k - 1], z1 = m->cz[k];
	const double f = (z1 > z0) ? (t - z0) / (z1 - z0) : 0.0;
	int c[3];
	for (int j = 0; j < 3; j++)
		c[j] = int(std::lround(255 * (m->crgb[3 * (k - 1) + j] + f * (m->crgb[3 * k + j] - m->crgb[3 * (k - 1) + j]))));
	return qRgb(c[0], c[1], c[2]);
}

// "nice" tick values over [a, b]
std::vector<double> wcdTicks(double a, double b, int want = 7) {
	std::vector<double> t;
	const double lo = std::min(a, b), hi = std::max(a, b), span = hi - lo;
	if (!(span > 0))
		return t;
	const double raw = span / want, mag = std::pow(10.0, std::floor(std::log10(raw))), r = raw / mag;
	const double step = mag * (r < 1.5 ? 1 : r < 3 ? 2 : r < 7 ? 5 : 10);
	for (double v = std::ceil(lo / step) * step; v <= hi + 1e-9 * span; v += step)
		t.push_back(std::abs(v) < 1e-12 * span ? 0.0 : v);
	return t;
}

// ---- the ping on display ------------------------------------------------------------------------
void wcdLoadPing(Wcd *m) {
	m->nb = m->ns = 0;
	m->amp.clear();
	m->ang.clear();
	m->bottom.clear();
	m->bottomLoaded = false;
	m->meta.clear();
	if (!m->host.ping || m->npings <= 0)
		return;
	int dims[2] = {0, 0};
	if (!m->host.ping(m->cur, dims, nullptr, 0, nullptr, 0, nullptr, nullptr, 0))
		return;
	m->nb = dims[0];
	m->ns = dims[1];
	m->amp.assign(size_t(m->nb) * size_t(m->ns), NAN);
	m->ang.assign(size_t(m->nb), 0.0);
	double xtra[3] = {0, 0, 0};
	char meta[1024] = "";
	if (!m->host.ping(m->cur, dims, m->amp.data(), int(m->amp.size()), m->ang.data(), int(m->ang.size()), xtra, meta,
	                  int(sizeof(meta)))) {
		m->nb = m->ns = 0;
		return;
	}
	m->dr = xtra[0];
	m->xlo = xtra[1];
	m->xhi = xtra[2];
	m->meta = QString::fromUtf8(meta);
	m->order.resize(size_t(m->nb));
	std::iota(m->order.begin(), m->order.end(), 0);
	std::sort(m->order.begin(), m->order.end(), [m](int a, int b) { return m->ang[a] < m->ang[b]; });
}

void wcdLoadBottom(Wcd *m) {
	m->bottomLoaded = true;
	m->bottom.clear();
	if (!m->host.bottom)
		return;
	const int n = m->host.bottom(m->cur, nullptr, 0);
	if (n <= 0)
		return;
	m->bottom.assign(size_t(4 * n), 0.0);
	m->host.bottom(m->cur, m->bottom.data(), int(m->bottom.size()));
}

// redraw: the header line, the ping label and the plot
void wcdRedraw(Wcd *m) {
	// the header line, one item per line in the narrow panel ("Ping i:", DateTime, SOG, Pos)
	m->labelMeta->setText(QString(m->meta).replace(": ", ":\n").replace(", ", "\n"));
	m->labelPing->setText(QString::number(m->cur));
	m->canvas->update();
}

void wcdSetPing(Wcd *m, int i) {
	if (m->npings <= 0)
		return;
	m->cur = std::clamp(i, 0, m->npings - 1);
	{
		QSignalBlocker b(m->slider);
		m->slider->setValue(m->cur);
	}
	wcdLoadPing(m);
	wcdRedraw(m);
}

// ---- the drawing ----------------------------------------------------------------------------
void wcdDraw(Wcd *m, QPainter &p, const QSize &sz, double dpr) {
	p.fillRect(QRect(QPoint(0, 0), sz), Qt::white);
	const QFontMetricsF fm(p.font());
	// the data range: depth window top-down; across-track fixed by the user, else tight on the ping
	m->d0 = m->dmin->value();
	m->d1 = m->dmax->value();
	if (std::abs(m->d1 - m->d0) <= 1e-6)
		m->d1 = m->d0 + 1e-3;
	const double xm = m->xmax->value();
	m->x0 = xm > 0 ? -xm : m->xlo;
	m->x1 = xm > 0 ? xm : m->xhi;
	if (!(m->x1 > m->x0)) {
		m->x0 -= 1;
		m->x1 += 1;
	}
	// margins exactly as wide as what is written in them: the depth ticks + title on the left, the
	// across-track ticks + title below, the colour bar + its ticks + title on the right
	double wy = 0, wc = 0;
	for (double v : wcdTicks(m->d0, m->d1, 6))
		wy = std::max(wy, fm.horizontalAdvance(QString::number(v, 'g', 6)));
	for (double v : wcdTicks(m->amin->value(), m->amax->value(), 6))
		wc = std::max(wc, fm.horizontalAdvance(QString::number(v, 'g', 6)));
	const double L = fm.height() + 4 + wy + 8, T = fm.height() / 2 + 2, B = 2 * fm.height() + 10;
	const double R = 10 + 12 + 6 + wc + 4 + fm.height() + 4;
	m->plot = QRectF(L, T, std::max(10.0, sz.width() - L - R), std::max(10.0, sz.height() - T - B));
	const QRectF &pr = m->plot;
	if (m->nb <= 0 || m->ns <= 0) {
		p.setPen(Qt::black);
		p.drawRect(pr);
		return;
	}
	// the water column: per pixel, the beam nearest in angle and the sample nearest in range
	const int W = int(std::lround(pr.width() * dpr)), H = int(std::lround(pr.height() * dpr));
	QImage img(W, H, QImage::Format_ARGB32);
	img.fill(Qt::transparent);
	const double dlo = std::min(m->dmin->value(), m->dmax->value()), dhi = std::max(m->dmin->value(), m->dmax->value());
	const double amin = m->ang[m->order.front()], amax = m->ang[m->order.back()];
	const double edge = m->nb > 1 ? 0.5 * (amax - amin) / (m->nb - 1) : 0.5;
	for (int r = 0; r < H; r++) {
		const double d = m->d0 + (r + 0.5) / H * (m->d1 - m->d0);
		if (d < dlo || d > dhi)
			continue;                                        // the depth window masks the data
		QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(r));
		for (int c = 0; c < W; c++) {
			const double x = m->x0 + (c + 0.5) / W * (m->x1 - m->x0);
			const double rng = std::hypot(x, d);
			const double a = std::atan2(-x, d) * 57.29577951308232;   // x is drawn as -sin(angle)*range
			if (a < amin - edge || a > amax + edge)
				continue;
			auto it = std::lower_bound(m->order.begin(), m->order.end(), a,
			                           [m](int b, double v) { return m->ang[b] < v; });
			int bi;
			if (it == m->order.end())
				bi = m->order.back();
			else if (it == m->order.begin())
				bi = *it;
			else
				bi = (a - m->ang[*(it - 1)] < m->ang[*it] - a) ? *(it - 1) : *it;
			const long s = std::lround(rng / m->dr);
			if (s < 0 || s >= m->ns)
				continue;
			const float v = m->amp[size_t(bi) * size_t(m->ns) + size_t(s)];
			if (std::isfinite(v))
				row[c] = wcdColour(m, v);
		}
	}
	img.setDevicePixelRatio(dpr);
	p.drawImage(pr.topLeft(), img);

	auto px = [m, &pr](double x) { return pr.left() + (x - m->x0) / (m->x1 - m->x0) * pr.width(); };
	auto py = [m, &pr](double d) { return pr.top() + (d - m->d0) / (m->d1 - m->d0) * pr.height(); };

	// the bottom detections (#MRZ): red amplitude, dark blue phase, grey none, green rejected;
	// filled = normal detection, hollow = extra detection
	if (m->chkBottom->isChecked()) {
		if (!m->bottomLoaded)
			wcdLoadBottom(m);
		p.save();
		p.setClipRect(pr);
		p.setRenderHint(QPainter::Antialiasing);
		const QColor cols[4] = {QColor("gray"), QColor("red"), QColor("darkblue"), QColor("green")};
		const double rad = 2.4;
		for (size_t k = 0; k + 3 < m->bottom.size(); k += 4) {
			const QColor c = cols[std::clamp(int(m->bottom[k + 2]), 0, 3)];
			const QPointF q(px(m->bottom[k]), py(m->bottom[k + 1]));
			if (m->bottom[k + 3] != 0.0) {
				p.setPen(QPen(c, 0.8));
				p.setBrush(Qt::NoBrush);
			}
			else {
				p.setPen(Qt::NoPen);
				p.setBrush(c);
			}
			p.drawEllipse(q, rad, rad);
		}
		p.restore();
		m->win->statusBar()->showMessage(QString("Bottom detections: %1").arg(m->bottom.size() / 4));
	}

	// the picks of this ping: a cross, white under black, readable on any colour
	{
		p.save();
		p.setClipRect(pr);
		p.setRenderHint(QPainter::Antialiasing);
		for (const auto &k : m->marks) {
			if (k.ping != m->cur)
				continue;
			const QPointF q(px(k.x), py(k.d));
			for (const auto &pen : {QPen(Qt::white, 3.5), QPen(Qt::black, 1.5)}) {
				p.setPen(pen);
				p.drawLine(q + QPointF(-6, -6), q + QPointF(6, 6));
				p.drawLine(q + QPointF(-6, 6), q + QPointF(6, -6));
			}
		}
		p.restore();
	}

	// the axes
	p.setPen(Qt::black);
	p.setBrush(Qt::NoBrush);
	p.drawRect(pr);
	for (double v : wcdTicks(m->x0, m->x1)) {
		const double X = px(v);
		p.drawLine(QPointF(X, pr.bottom()), QPointF(X, pr.bottom() + 4));
		const QString s = QString::number(v, 'g', 6);
		p.drawText(QPointF(X - fm.horizontalAdvance(s) / 2, pr.bottom() + 6 + fm.ascent()), s);
	}
	for (double v : wcdTicks(m->d0, m->d1, 6)) {
		const double Y = py(v);
		p.drawLine(QPointF(pr.left() - 4, Y), QPointF(pr.left(), Y));
		const QString s = QString::number(v, 'g', 6);
		p.drawText(QPointF(pr.left() - 7 - fm.horizontalAdvance(s), Y + fm.ascent() / 2 - 1), s);
	}
	const QString xl = "Across-track (m)";
	p.drawText(QPointF(pr.center().x() - fm.horizontalAdvance(xl) / 2, pr.bottom() + 8 + fm.height() + fm.ascent()), xl);
	p.save();
	p.translate(2 + fm.ascent(), pr.center().y());
	p.rotate(-90);
	const QString yl = "Depth (m)";
	p.drawText(QPointF(-fm.horizontalAdvance(yl) / 2, fm.ascent() / 2), yl);
	p.restore();

	// the colour bar, fixed beside the plot
	const QRectF cb(pr.right() + 10, pr.top() + 0.1 * pr.height(), 12, 0.8 * pr.height());
	const double lo = m->amin->value(), hi = m->amax->value();
	for (int k = 0; k < int(cb.height()); k++) {
		const double v = hi - (k + 0.5) / cb.height() * (hi - lo);
		p.setPen(QColor(wcdColour(m, v)));
		p.drawLine(QPointF(cb.left(), cb.top() + k), QPointF(cb.right(), cb.top() + k));
	}
	p.setPen(Qt::black);
	p.drawRect(cb);
	if (hi > lo)
		for (double v : wcdTicks(lo, hi, 6)) {
			const double Y = cb.bottom() - (v - lo) / (hi - lo) * cb.height();
			p.drawLine(QPointF(cb.right(), Y), QPointF(cb.right() + 4, Y));
			p.drawText(QPointF(cb.right() + 7, Y + fm.ascent() / 2 - 1), QString::number(v, 'g', 6));
		}
	p.save();
	p.translate(cb.right() + 6 + wc + 4 + fm.descent(), cb.center().y());
	p.rotate(90);
	p.drawText(QPointF(-fm.horizontalAdvance("Amplitude") / 2, 0), "Amplitude");
	p.restore();
}

void WcdCanvas::paintEvent(QPaintEvent *) {
	QPainter p(this);
	wcdDraw(m_, p, size(), devicePixelRatioF());
}

// ---- picks (_on_plot_click, _PickDialog's table, now under the plot) ------------------------------
// The table is as wide as the plot and only as tall as its rows (up to 8, then it scrolls); with no rows
// it is not there at all. The WINDOW grows and shrinks by exactly what the table takes, so the plot keeps
// its size: the viewer is small while nothing is picked and grows only with use.
void wcdFitPicks(Wcd *m) {
	auto total = [m]() { return m->picksPanel->isVisible() ? m->picksPanel->height() + 2 : 0; };
	const int before = total();
	const int rows = m->table->rowCount();
	if (rows == 0)
		m->picksPanel->hide();
	else {
		const int shown = std::min(rows, 8);
		int h = m->table->horizontalHeader()->height() + 2 * m->table->frameWidth();
		for (int r = 0; r < shown; r++)
			h += m->table->rowHeight(r);
		m->table->setFixedHeight(h);         // the columns fit the width: no horizontal bar to make room for
		m->picksPanel->show();
		m->picksPanel->adjustSize();
		m->picksPanel->setFixedHeight(m->picksPanel->sizeHint().height());
	}
	const int after = rows == 0 ? 0 : m->picksPanel->sizeHint().height() + 2;
	if (after != before)
		m->win->resize(m->win->width(), m->win->height() + after - before);
}

void wcdWirePicks(Wcd *m, QPushButton *bSave, QPushButton *bRemove, QPushButton *bClear) {
	{
		QWidget *d = m->win;
		// each column as wide as what it holds (the UTC time is the long one); the last takes the rest
		m->table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
		m->table->horizontalHeader()->setStretchLastSection(true);
		QObject::connect(bSave, &QPushButton::clicked, d, [m]() {
			const QString fn = QFileDialog::getSaveFileName(m->win, "Save Picks",
			                                                QDir(m->host.base.startDir()).filePath("picks.txt"),
			                                                "Text Files (*.txt);;All Files (*)");
			if (fn.isEmpty())
				return;
			m->host.base.rememberDir(fn);
			QFile f(fn);
			if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
				return;
			QTextStream out(&f);
			QStringList head;
			for (int c = 0; c < m->table->columnCount(); c++)
				head << m->table->horizontalHeaderItem(c)->text();
			out << head.join('\t') << "\n";
			for (int r = 0; r < m->table->rowCount(); r++) {
				QStringList vals;
				for (int c = 0; c < m->table->columnCount(); c++)
					vals << (m->table->item(r, c) ? m->table->item(r, c)->text() : QString());
				out << vals.join('\t') << "\n";
			}
		});
		QObject::connect(bRemove, &QPushButton::clicked, d, [m]() {
			std::vector<int> rows;
			for (const QModelIndex &ix : m->table->selectionModel()->selectedRows())
				rows.push_back(ix.row());
			std::sort(rows.rbegin(), rows.rend());
			for (int r : rows) {
				m->table->removeRow(r);
				if (r < int(m->marks.size()))
					m->marks.erase(m->marks.begin() + r);   // a row and its mark go together
			}
			wcdFitPicks(m);
			m->canvas->update();
		});
		// Clear empties the table, takes the marks off the plot, and folds the table away
		QObject::connect(bClear, &QPushButton::clicked, d, [m]() {
			m->table->setRowCount(0);
			m->marks.clear();
			wcdFitPicks(m);
			m->canvas->update();
		});
	}
}

void wcdPick(Wcd *m, double x, double d) {
	if (!m->host.pick || m->npings <= 0)
		return;
	char row[2048] = "";
	if (!m->host.pick(m->cur, x, d, row, int(sizeof(row)))) {
		m->win->statusBar()->showMessage("Nothing to pick (no grid).", 3000);
		return;
	}
	const QStringList vals = QString::fromUtf8(row).split('\t');
	const int r = m->table->rowCount();
	m->table->insertRow(r);
	for (int c = 0; c < vals.size() && c < m->table->columnCount(); c++)
		m->table->setItem(r, c, new QTableWidgetItem(vals[c]));
	wcdFitPicks(m);                          // the table unfolds / grows by this row
	m->table->scrollToBottom();
	// the picked sample, marked where it is (the row's own across-track and depth), and said so; one
	// mark per row, in the table's order
	m->marks.push_back({m->cur, vals.size() >= 4 ? vals[2].toDouble() : x, vals.size() >= 4 ? vals[3].toDouble() : d});
	m->win->statusBar()->showMessage(
	    vals.size() >= 8 ? QString("Picked ping %1: across %2 m, depth %3 m -> %4, %5").arg(vals[0], vals[2], vals[3], vals[6], vals[7])
	                     : QString("Picked"),
	    8000);
	m->canvas->update();
}

void WcdCanvas::mousePressEvent(QMouseEvent *e) {
	Wcd *m = m_;
	// a click on the frame line itself is a click on the plot's edge (contains() can exclude the edge)
	if (e->button() != Qt::LeftButton || !m->chkPick->isChecked() || m->nb <= 0 ||
	    !m->plot.adjusted(-1, -1, 1, 1).contains(e->position()))
		return;
	const QPointF q = e->position();
	const double x = m->x0 + (q.x() - m->plot.left()) / m->plot.width() * (m->x1 - m->x0);
	const double d = m->d0 + (q.y() - m->plot.top()) / m->plot.height() * (m->d1 - m->d0);
	wcdPick(m, x, d);
}

// ---- open_file, save_image, replay ---------------------------------------------------------
bool wcdOpenFile(Wcd *m, const QString &fn) {
	if (!m->host.open)
		return false;
	m->win->statusBar()->showMessage("Indexing…");
	std::vector<double> info(4096, 0.0);
	char err[1024] = "";
	const QByteArray p = QDir::toNativeSeparators(fn).toUtf8();
	if (!m->host.open(p.constData(), info.data(), int(info.size()), err, int(sizeof(err)))) {
		const QString msg = QString::fromUtf8(err);
		m->win->statusBar()->showMessage(msg.contains("No #MWC") ? "No #MWC data found." : msg);
		if (!msg.contains("No #MWC"))
			QMessageBox::warning(m->win, "Water column viewer", msg);
		return false;
	}
	m->fileName = QFileInfo(fn).fileName();
	// a long line name does not wrap (no spaces): shortened in the middle, whole in the tooltip
	m->labelFile->setText(m->labelFile->fontMetrics().elidedText(m->fileName, Qt::ElideMiddle,
	                                                           std::max(60, m->labelFile->width())));
	m->labelFile->setToolTip(QDir::toNativeSeparators(fn));
	m->npings = int(info[0]);
	const double ampmin = info[1], ampmax = info[2], depthmax = info[3];
	const int nc = int(info[4]);
	m->cz.assign(info.begin() + 5, info.begin() + 5 + nc);
	m->crgb.assign(info.begin() + 5 + nc, info.begin() + 5 + 4 * nc);
	{
		QSignalBlocker b1(m->amin), b2(m->amax), b3(m->dmin), b4(m->dmax), b5(m->xmax), b6(m->slider);
		if (ampmin < ampmax) {
			m->amin->setRange(ampmin, ampmax);
			m->amax->setRange(ampmin, ampmax);
			m->amin->setValue(ampmin);
			m->amax->setValue(ampmax);
		}
		m->dmin->setRange(0, depthmax);
		m->dmax->setRange(0, depthmax);
		m->dmin->setValue(0);
		m->dmax->setValue(depthmax);
		m->xmax->setRange(0, 1e5);
		m->xmax->setValue(0);
		m->slider->setRange(0, std::max(0, m->npings - 1));
		m->slider->setSingleStep(1);
		m->slider->setValue(0);
	}
	m->win->statusBar()->showMessage("Load complete");
	wcdSetPing(m, 0);
	return true;
}

void wcdSaveImage(Wcd *m, const QString &path) {
	const QSize sz = m->canvas->size();
	QImage img(sz * 2, QImage::Format_ARGB32);
	img.setDevicePixelRatio(2.0);
	{
		QPainter p(&img);
		wcdDraw(m, p, sz, 2.0);
		// the file name and the ping's metadata, bottom left, white on half-transparent black
		QFont f = p.font();
		f.setPointSizeF(8);
		p.setFont(f);
		const QString s = m->fileName + " | " + m->meta;
		const QFontMetricsF fm(f);
		const QRectF box(4, sz.height() - fm.height() - 6, fm.horizontalAdvance(s) + 4, fm.height() + 2);
		p.fillRect(box, QColor(0, 0, 0, 128));
		p.setPen(Qt::white);
		p.drawText(box.adjusted(2, 1, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, s);
	}
	img.save(path, path.toLower().endsWith(".png") ? "PNG" : "JPG");
}

void wcdToggleReplay(Wcd *m, bool on) {
	if (on && m->npings > 0) {
		m->replay->setInterval(std::max(1, int(1000.0 / std::max(0.1, m->rate->value()))));
		m->replay->start();
		m->btnReplay->setText(QString("Stop (%1 p/s)").arg(m->rate->value(), 0, 'f', 1));
	}
	else {
		m->replay->stop();
		m->btnReplay->setText("Replay");
		if (m->btnReplay->isChecked()) {
			QSignalBlocker b(m->btnReplay);
			m->btnReplay->setChecked(false);
		}
	}
}

// the slider moves exactly one ping per wheel notch; Left / Right step the ping anywhere in the window
struct WcdKeys : QObject {
	Wcd *m;
	WcdKeys(QObject *parent, Wcd *mm) : QObject(parent), m(mm) {}
	bool eventFilter(QObject *o, QEvent *e) override {
		if (o == m->slider && e->type() == QEvent::Wheel) {
			const int dy = static_cast<QWheelEvent *>(e)->angleDelta().y();
			if (dy > 0 && m->slider->value() < m->slider->maximum())
				m->slider->setValue(m->slider->value() + 1);
			else if (dy < 0 && m->slider->value() > m->slider->minimum())
				m->slider->setValue(m->slider->value() - 1);
			return true;
		}
		if (o == m->win && e->type() == QEvent::KeyPress) {
			const int k = static_cast<QKeyEvent *>(e)->key();
			if (k == Qt::Key_Left && m->cur > 0) {
				wcdSetPing(m, m->cur - 1);
				return true;
			}
			if (k == Qt::Key_Right && m->cur < m->npings - 1) {
				wcdSetPing(m, m->cur + 1);
				return true;
			}
		}
		return QObject::eventFilter(o, e);
	}
};

Wcd *wcdBuild(QWidget *parent, const WcdHost &host) {
	auto *m = new Wcd;
	m->host = host;
	QWidget *root = wcdLoadUi(m, "wcdviewer.ui", nullptr);
	auto *win = qobject_cast<QMainWindow *>(root);
	if (!win) {
		delete root;
		delete m;
		return nullptr;
	}
	QStringList missing;
	auto *btnOpen = wcdChild<QPushButton>(win, "btnOpen", missing);
	auto *btnSave = wcdChild<QPushButton>(win, "btnSave", missing);
	auto *btnHelp = wcdChild<QPushButton>(win, "btnHelp", missing);
	m->labelFile = wcdChild<QLabel>(win, "labelFile", missing);
	m->labelMeta = wcdChild<QLabel>(win, "labelMeta", missing);
	m->labelPing = wcdChild<QLabel>(win, "labelPing", missing);
	m->chkBottom = wcdChild<QCheckBox>(win, "chkBottom", missing);
	m->chkPick = wcdChild<QCheckBox>(win, "chkPick", missing);
	m->dmin = wcdChild<QDoubleSpinBox>(win, "dmin", missing);
	m->dmax = wcdChild<QDoubleSpinBox>(win, "dmax", missing);
	m->amin = wcdChild<QDoubleSpinBox>(win, "amin", missing);
	m->amax = wcdChild<QDoubleSpinBox>(win, "amax", missing);
	m->xmax = wcdChild<QDoubleSpinBox>(win, "xmax", missing);
	m->rate = wcdChild<QDoubleSpinBox>(win, "replayRate", missing);
	m->btnReplay = wcdChild<QPushButton>(win, "btnReplay", missing);
	m->slider = wcdChild<QSlider>(win, "slider", missing);
	auto *host_w = wcdChild<QWidget>(win, "canvasHost", missing);
	m->table = wcdChild<QTableWidget>(win, "table", missing);
	m->picksPanel = wcdChild<QWidget>(win, "picksPanel", missing);
	auto *btnSaveTxt = wcdChild<QPushButton>(win, "btnSaveTxt", missing);
	auto *btnRemove = wcdChild<QPushButton>(win, "btnRemove", missing);
	auto *btnClear = wcdChild<QPushButton>(win, "btnClear", missing);
	if (!missing.isEmpty()) {
		QMessageBox::warning(parent, "Water column viewer", "wcdviewer.ui lacks: " + missing.join(", "));
		delete win;
		delete m;
		return nullptr;
	}
	m->win = win;
	win->setAttribute(Qt::WA_DeleteOnClose);
	if (!host.base.icon.isNull())
		win->setWindowIcon(host.base.icon);
	auto *lay = new QVBoxLayout(host_w);
	lay->setContentsMargins(0, 0, 0, 0);
	m->canvas = new WcdCanvas(m, host_w);
	lay->addWidget(m->canvas);
	m->replay = new QTimer(win);

	QObject::connect(btnOpen, &QPushButton::clicked, win, [m]() {
		const QString fn = QFileDialog::getOpenFileName(m->win, "Open .kmwcd/.kmall", m->host.base.startDir(),
		                                                "*.kmwcd *.kmall");
		if (fn.isEmpty())
			return;
		m->host.base.rememberDir(fn);
		wcdOpenFile(m, fn);
	});
	QObject::connect(btnSave, &QPushButton::clicked, win, [m]() {
		const QString fn = QFileDialog::getSaveFileName(m->win, "Save Image",
		                                                QDir(m->host.base.startDir()).filePath(m->fileName),
		                                                "PNG (*.png);;JPEG (*.jpg)");
		if (fn.isEmpty())
			return;
		m->host.base.rememberDir(fn);
		wcdSaveImage(m, fn);
	});
	// Help: use, example data, credits -- deps/ui/wcdviewer_help.html, read at run time
	QObject::connect(btnHelp, &QPushButton::clicked, win, [m]() {
		QFile f(QDir(m->host.base.uiDir).filePath("wcdviewer_help.html"));
		const QString html = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
		                                                 : QString("Cannot open %1").arg(f.fileName());
		// a modest window that scrolls (it must fit any screen), links opening in the browser
		auto *dlg = new QDialog(m->win, Qt::Window | Qt::WindowCloseButtonHint);
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowTitle("Water column viewer — Help");
		auto *lay = new QVBoxLayout(dlg);
		lay->setContentsMargins(4, 4, 4, 4);
		auto *tb = new QTextBrowser(dlg);
		tb->setOpenExternalLinks(true);
		tb->setHtml(html);
		lay->addWidget(tb);
		dlg->resize(520, 420);
		dlg->show();
	});
	for (QDoubleSpinBox *s : {m->dmin, m->dmax, m->amin, m->amax, m->xmax})
		QObject::connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged), win, [m](double) { wcdRedraw(m); });
	QObject::connect(m->chkBottom, &QCheckBox::toggled, win, [m](bool) { wcdRedraw(m); });
	// Pick: a cross cursor while on; the picks go to the table under the plot
	QObject::connect(m->chkPick, &QCheckBox::toggled, win, [m](bool on) {
		m->canvas->setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
	});
	wcdWirePicks(m, btnSaveTxt, btnRemove, btnClear);
	QObject::connect(m->slider, &QSlider::valueChanged, win, [m](int v) {
		if (v != m->cur)
			wcdSetPing(m, v);
	});
	QObject::connect(m->btnReplay, &QPushButton::toggled, win, [m](bool on) { wcdToggleReplay(m, on); });
	QObject::connect(m->rate, QOverload<double>::of(&QDoubleSpinBox::valueChanged), win, [m](double) {
		if (m->replay->isActive())
			wcdToggleReplay(m, true);
	});
	QObject::connect(m->replay, &QTimer::timeout, win, [m]() {
		if (m->npings <= 0) {
			wcdToggleReplay(m, false);
			return;
		}
		wcdSetPing(m, (m->cur + 1) % m->npings);
	});
	auto *keys = new WcdKeys(win, m);
	m->slider->installEventFilter(keys);
	win->installEventFilter(keys);
	win->statusBar()->showMessage("Ready");
	return m;
}

} // namespace

// ---- entry points ----------------------------------------------------------------------------
bool wcdOpenWindow(QWidget *parent, const WcdHost &host, const QString &file) {
	if (!g_wcd) {
		Wcd *m = wcdBuild(parent, host);
		if (!m)
			return false;
		g_wcd = m;
		m->parking = mbParkable(m->win, host.base, "Water column viewer");
		if (host.base.windowOpened)
			host.base.windowOpened();
		QObject::connect(m->win, &QObject::destroyed, [m]() {
			if (m->host.base.windowClosed)
				m->host.base.windowClosed();
			if (g_wcd == m)
				g_wcd = nullptr;
			delete m;
		});
		m->win->show();
		mbPlaceRight(m->win, parent);
	}
	Wcd *m = g_wcd;
	mbParkRebind(m->parking, host.base.parkScene);
	mbParkShow(m->parking);
	if (!file.isEmpty())
		return wcdOpenFile(m, file);
	return true;
}

int wcdState(int *out, int n) {
	Wcd *m = g_wcd;
	if (m && m->chkBottom->isChecked() && !m->bottomLoaded)
		wcdLoadBottom(m);
	const int v[5] = {m ? 1 : 0, m ? m->npings : 0, m ? m->cur : 0, (m && m->table) ? m->table->rowCount() : 0,
	                  m ? int(m->bottom.size() / 4) : 0};
	const int k = n < 5 ? n : 5;
	for (int i = 0; i < k; i++)
		out[i] = v[i];
	return k;
}

bool wcdSetPing(int i) {
	if (!g_wcd || i < 0 || i >= g_wcd->npings)
		return false;
	wcdSetPing(g_wcd, i);
	return true;
}

// as a user does it: tick Pick (if it is not), then a real left press on the plot at (x, depth)
bool wcdPickAt(double x, double depth) {
	Wcd *m = g_wcd;
	if (!m || m->npings <= 0)
		return false;
	// the plot's frame and ranges are laid out by the drawing; a window that has not been painted yet
	// (one never exposed, e.g. kept off screen) has none, so lay it out now, exactly as a paint would
	{
		QImage scratch(m->canvas->size().expandedTo(QSize(1, 1)), QImage::Format_ARGB32);
		QPainter p(&scratch);
		wcdDraw(m, p, m->canvas->size(), 1.0);
	}
	if (!m->plot.isValid())
		return false;
	if (!m->chkPick->isChecked())
		m->chkPick->click();
	const QPointF q(m->plot.left() + (x - m->x0) / (m->x1 - m->x0) * m->plot.width(),
	                m->plot.top() + (depth - m->d0) / (m->d1 - m->d0) * m->plot.height());
	QMouseEvent press(QEvent::MouseButtonPress, q, m->canvas->mapToGlobal(q), Qt::LeftButton, Qt::LeftButton,
	                  Qt::NoModifier);
	QApplication::sendEvent(m->canvas, &press);
	QMouseEvent release(QEvent::MouseButtonRelease, q, m->canvas->mapToGlobal(q), Qt::LeftButton, Qt::NoButton,
	                    Qt::NoModifier);
	QApplication::sendEvent(m->canvas, &release);
	return true;
}

bool wcdSavePng(const QString &path) {
	if (!g_wcd)
		return false;
	wcdSaveImage(g_wcd, path);
	return QFile::exists(path);
}

bool wcdGrabWindow(const QString &path) {
	if (!g_wcd)
		return false;
	return g_wcd->win->grab().save(path);
}

bool wcdClose() {
	if (!g_wcd)
		return false;
	mbParkQuit(g_wcd->parking);
	return true;
}
