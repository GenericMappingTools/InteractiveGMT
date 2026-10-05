// ============================================================================
//  mb3dsoundings_cloud.cpp -- the 3D Soundings pane of a swath point cloud (mb3dsoundings_cloud.h).
//  A translation unit of its own. No reading here: the soundings are what MB-System's mbgetdata
//  handed the host.
// ============================================================================

#include "mb3dsoundings_cloud.h"
#include "mb3dsoundings_window.h"
#include "mbeditviz.h"

#include <QFileInfo>
#include <QMessageBox>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

// mbgetdata -A-1000000: a flagged beam's depth comes shifted by this
const double kFlagShift = -1000000.0;

struct CloudEdit {
	int ping;
	int beam;
	char flag;
};
struct Cloud {
	std::vector<mb3dsoundings_sounding_struct> s;
	mb3dsoundings_struct data;
	QString name;
	void *scene = nullptr;                    // the window it is the pane of
	MbEditHost host;
	std::vector<double> pingTime;              // [ping] -> its time (what the .esf keys on)
	std::vector<int> pingFile;                 // [ping] -> index into files
	QStringList files;
	std::vector<CloudEdit> edits;
	int nedits = 0;
	double maxDepth = 0.0;
	// an AREA cloud (a line area's "Show point-cloud"): the soundings of `parent` inside the area, same
	// pings and beams, in a window of its own. Its edits never reach a file: Accept hands them to the
	// parent, Discard drops them; either way the parent gets its pane back. closeWin closes its window.
	struct Cloud *parent = nullptr;
	std::function<void(void *)> closeWin;
	double cellsize() const {                 // mbeditviz's grid cell size rule (2% of the depth)
		return maxDepth > 0.0 ? 0.02 * maxDepth : 1.0;
	}
};
Cloud *g_cloud = nullptr;

void cloudEdit(int ifile, int iping, int ibeam, char beamflag, int flush) {
	(void)ifile;
	if (!g_cloud || flush == MB3DSDG_EDIT_FLUSHPREVIOUS)
		return;
	g_cloud->edits.push_back({iping, ibeam, beamflag});
	g_cloud->nedits++;
}

void cloudInfo(int ifile, int iping, int ibeam, char *infostring) {
	(void)ifile;
	if (!g_cloud)
		return;
	for (const auto &s : g_cloud->s)
		if (s.iping == iping && s.ibeam == ibeam) {
			snprintf(infostring, 1024, "%s | ping %d | beam %d | lon %.6f | lat %.6f | depth %.2f m | %s",
			         g_cloud->name.toUtf8().constData(), iping, ibeam, s.x, s.y, -s.z,
			         mb_beam_ok(s.beamflag) ? "good" : "flagged");
			return;
		}
}

// mbeditviz's "filter by sparse voxels" (mbeditviz_mb3dsoundings_flagsparsevoxels), on these soundings:
// voxels of sizemultiplier x the cell size; a good sounding is flagged when its voxel and the 26 around it
// hold fewer than `threshold` good soundings in all. Horizontal metres from lon/lat about the centre.
void cloudSparse(int sizemultiplier, int threshold) {
	if (!g_cloud || g_cloud->s.empty())
		return;
	auto &S = g_cloud->s;
	double lon0 = 0.0, lat0 = 0.0;
	for (const auto &s : S) {
		lon0 += s.x;
		lat0 += s.y;
	}
	lon0 /= double(S.size());
	lat0 /= double(S.size());
	const double my = 111319.49, mx = my * std::cos(lat0 * M_PI / 180.0);
	const double d = sizemultiplier * g_cloud->cellsize();
	auto key = [](long long i, long long j, long long k) {
		return ((i + (1LL << 20)) << 42) | ((j + (1LL << 20)) << 21) | (k + (1LL << 20));
	};
	struct Vox {
		int own = 0, around = 0;
		std::vector<int> ids;
	};
	std::unordered_map<long long, Vox> vox;
	for (size_t n = 0; n < S.size(); n++) {
		if (!mb_beam_ok(S[n].beamflag))
			continue;
		const long long i = (long long)std::floor((S[n].x - lon0) * mx / d);
		const long long j = (long long)std::floor((S[n].y - lat0) * my / d);
		const long long k = (long long)std::floor(S[n].z / d);
		for (long long a = -1; a <= 1; a++)
			for (long long b = -1; b <= 1; b++)
				for (long long e = -1; e <= 1; e++) {
					Vox &v = vox[key(i + a, j + b, k + e)];
					if (a == 0 && b == 0 && e == 0) {
						v.own++;
						v.ids.push_back(int(n));
					}
					else
						v.around++;
				}
	}
	for (auto &kv : vox) {
		const Vox &v = kv.second;
		if (v.own > 0 && v.own + v.around < threshold)
			for (int n : v.ids) {
				S[n].beamflag = MB_FLAG_FLAG + MB_FLAG_MANUAL;
				g_cloud->data.num_soundings_unflagged--;
				g_cloud->data.num_soundings_flagged++;
				cloudEdit(0, S[n].iping, S[n].ibeam, S[n].beamflag, MB3DSDG_EDIT_NOFLUSH);
			}
	}
}

// mbeditviz's "colour unflagged soundings"
void cloudColor(int color) {
	if (!g_cloud)
		return;
	for (auto &s : g_cloud->s)
		if (mb_beam_ok(s.beamflag))
			s.beamcolor = color;
}

// Save (the pane's Save button, and the pane closing): every edit not yet saved appended to its file's
// .esf with MB-System's own edit-save routines (the way MBedit saves them): mb_esf_load in APPEND mode,
// mb_esf_save per beam by ping time, mb_esf_close, and editing switched on in the file's .par
// (mb_pr_update_edit) so mbprocess applies them. The saved edits are then cleared.
// THE .esf writer: `edits` (by ping index and beam) appended to each file's edit save file, by ping time
// (pingTime[ping]) in its file (files[pingFile[ping]]). The names of the files that failed.
QStringList saveEsfEdits(const QStringList &files, const std::vector<double> &pingTime,
                         const std::vector<int> &pingFile, const std::vector<CloudEdit> &edits) {
	QStringList failed;
	for (int f = 0; f < files.size(); f++) {
		bool any = false;
		for (const auto &e : edits)
			if (e.ping >= 0 && e.ping < int(pingFile.size()) && pingFile[e.ping] == f) {
				any = true;
				break;
			}
		if (!any)
			continue;
		char file[MB_PATH_MAXLINE], esffile[MB_PATH_MAXLINE];
		snprintf(file, sizeof(file), "%s", files[f].toUtf8().constData());
		struct mb_esf_struct esf;
		memset(&esf, 0, sizeof(esf));
		int error = MB_ERROR_NO_ERROR;
		if (mb_esf_load(0, "iGMT", file, false, MBP_ESF_APPEND, esffile, &esf, &error) != MB_SUCCESS) {
			failed << QFileInfo(files[f]).fileName();
			continue;
		}
		for (const auto &e : edits) {
			if (e.ping < 0 || e.ping >= int(pingFile.size()) || pingFile[e.ping] != f)
				continue;
			int action = MBP_EDIT_FLAG;
			if (mb_beam_ok(e.flag))
				action = MBP_EDIT_UNFLAG;
			else if (mb_beam_check_flag_filter(e.flag) || mb_beam_check_flag_filter2(e.flag))
				action = MBP_EDIT_FILTER;
			mb_esf_save(0, &esf, pingTime[e.ping], e.beam, action, &error);
		}
		mb_esf_close(0, &esf, &error);
		mb_pr_update_edit(0, file, MBP_EDIT_ON, esffile, &error);
	}
	return failed;
}

void cloudSaveEdits(Cloud *c) {
	if (!c || c->edits.empty())
		return;
	const QStringList failed = saveEsfEdits(c->files, c->pingTime, c->pingFile, c->edits);
	if (failed.isEmpty())
		c->edits.clear();
	else
		QMessageBox::warning(nullptr, "3D Soundings", "The edits could not be saved for: " + failed.join(", "));
}

void areaFinish(bool accept, bool closeWindow);

// Save: an area cloud's Save is its Accept (its edits go to the parent, never to a file)
void cloudSave() {
	if (g_cloud && g_cloud->parent)
		areaFinish(true, true);
	else
		cloudSaveEdits(g_cloud);
}

// the area pane's Discard
void cloudDiscard() {
	if (g_cloud && g_cloud->parent)
		areaFinish(false, true);
}

// CUBE gridding: the host's CUBE dialog, its input these soundings (mb3dsdgCloudGood)
// Gridding: the host's gridding dialog (Interpolate, on mbgrid), its input these soundings
void cloudGrid() {
	if (g_cloud && g_cloud->host.openGridOnCloud)
		g_cloud->host.openGridOnCloud(g_cloud->scene, g_cloud->name.toUtf8().constData());
}

void cloudCube(bool filter) {
	if (g_cloud && g_cloud->host.openCubeOnCloud)
		g_cloud->host.openCubeOnCloud(g_cloud->scene, g_cloud->name.toUtf8().constData(), filter);
}

// the pane is gone: what is not saved yet is saved, the soundings released. An area cloud's pane gone by
// itself (its window closed) is a Discard.
void cloudDismiss() {
	if (g_cloud && g_cloud->parent) {
		areaFinish(false, false);
		return;
	}
	Cloud *c = g_cloud;
	g_cloud = nullptr;
	cloudSaveEdits(c);
	delete c;
}

void cloudDiscard();

// The notify functions every cloud pane is opened with
Mb3dsdgNotify cloudNotify() {
	Mb3dsdgNotify n;
	n.dismiss = &cloudDismiss;
	n.edit = &cloudEdit;
	n.info = &cloudInfo;
	n.flagsparsevoxels = &cloudSparse;
	n.colorsoundings = &cloudColor;
	n.save = &cloudSave;
	n.cube = &cloudCube;
	n.grid = &cloudGrid;
	n.discard = &cloudDiscard;
	return n;
}

// End the area cloud in g_cloud: with `accept` its edits become the parent's (each sounding found by its
// ping and beam, its flag set, the edit recorded for the parent's Save); then the area pane goes (its edits
// written nowhere), its window too unless it is already closing, and the parent -- whose soundings and edits
// waited in memory -- gets its pane back in its own window. If that window is gone, the parent's edits are
// saved and it is released, as any closed pane's are.
void areaFinish(bool accept, bool closeWindow) {
	Cloud *a = g_cloud;
	if (!a || !a->parent)
		return;
	Cloud *p = a->parent;
	if (accept && !a->edits.empty()) {
		int nb = 0;
		for (const auto &s : p->s)
			nb = std::max(nb, s.ibeam + 1);
		std::unordered_map<long long, size_t> at;
		for (size_t i = 0; i < p->s.size(); i++)
			at[(long long)p->s[i].iping * nb + p->s[i].ibeam] = i;
		for (const auto &e : a->edits) {
			auto it = at.find((long long)e.ping * nb + e.beam);
			if (it == at.end())
				continue;
			auto &s = p->s[it->second];
			const bool was = mb_beam_ok(s.beamflag), now = mb_beam_ok(e.flag);
			s.beamflag = e.flag;
			if (was && !now) {
				p->data.num_soundings_unflagged--;
				p->data.num_soundings_flagged++;
			}
			else if (!was && now) {
				p->data.num_soundings_unflagged++;
				p->data.num_soundings_flagged--;
			}
			p->edits.push_back(e);
			p->nedits++;
		}
	}
	void *areaScene = a->scene;
	auto closeWin = a->closeWin;
	g_cloud = nullptr;                           // the area pane's teardown then saves nothing
	if (mb3dsdgIsOpen())
		mb3dsdgEnd();
	delete a;
	// the rest after this event: the pane being torn down may be the one whose button got us here
	QTimer::singleShot(0, [p, areaScene, closeWin, closeWindow]() {
		if (closeWindow && closeWin)
			closeWin(areaScene);
		if (p->host.sceneWindow && p->host.sceneWindow(p->scene)) {
			g_cloud = p;
			if (!mb3dsdgOpenPane(p->scene, p->host, &p->data, cloudNotify())) {
				g_cloud = nullptr;
				cloudSaveEdits(p);
				delete p;
			}
		}
		else {
			cloudSaveEdits(p);
			delete p;
		}
	});
}

} // namespace

int mb3dsdgCloudFlag(void *scene, const unsigned char *bad, int n) {
	if (!g_cloud || g_cloud->scene != scene || !bad)
		return -1;
	int i = 0, nflag = 0;
	for (auto &s : g_cloud->s) {
		if (!mb_beam_ok(s.beamflag))
			continue;                                // only the good soundings are in `bad`, in order
		if (i >= n)
			break;
		if (bad[i++]) {
			s.beamflag = char(MB_FLAG_FLAG + MB_FLAG_FILTER);
			g_cloud->data.num_soundings_unflagged--;
			g_cloud->data.num_soundings_flagged++;
			cloudEdit(0, s.iping, s.ibeam, s.beamflag, MB3DSDG_EDIT_NOFLUSH);
			nflag++;
		}
	}
	if (nflag)
		mb3dsdgPlot();
	return nflag;
}

int mb3dsdgEsfFlag(const MbEditHost &host, const QStringList &files, const double *ptime, const int *pfile,
                   int nping, const int *ping, const int *beam, int n) {
	if (!ptime || !pfile || !ping || !beam || nping <= 0 || n < 0 || files.isEmpty())
		return -1;
	if (!mbeditLoadMbio(nullptr, host))
		return -1;
	std::vector<CloudEdit> edits;
	edits.reserve(size_t(n));
	for (int i = 0; i < n; i++)
		edits.push_back({ping[i], beam[i], char(MB_FLAG_FLAG + MB_FLAG_FILTER)});
	const QStringList failed = saveEsfEdits(files, std::vector<double>(ptime, ptime + nping),
	                                        std::vector<int>(pfile, pfile + nping), edits);
	return failed.isEmpty() ? n : -1;
}

bool mb3dsdgOpenAreaCloud(void *parentScene, const double *ring, int nring,
                          const std::function<void *(const double *xyz, int n, const QString &title)> &makeWindow,
                          const std::function<void(void *)> &closeWin) {
	Cloud *p = g_cloud;
	if (!p || p->scene != parentScene || !ring || nring < 3 || !makeWindow)
		return false;
	auto inside = [ring, nring](double x, double y) {   // even-odd rule over the ring (x,y pairs)
		bool in = false;
		for (int i = 0, j = nring - 1; i < nring; j = i++) {
			const double xi = ring[2 * i], yi = ring[2 * i + 1], xj = ring[2 * j], yj = ring[2 * j + 1];
			if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
				in = !in;
		}
		return in;
	};
	// the parent's soundings inside the area, on the parent's own ping x beam frame (so every sounding
	// keeps its identity), flagged ones carried as mbgetdata carries them (shifted by the flag offset)
	const int nping = int(p->pingTime.size());
	int nbeam = 0;
	for (const auto &s : p->s)
		nbeam = std::max(nbeam, s.ibeam + 1);
	if (nping <= 0 || nbeam <= 0)
		return false;
	const size_t nn = size_t(nping) * size_t(nbeam);
	std::vector<double> lon(nn, NAN), lat(nn, NAN), z(nn, NAN), good;
	for (const auto &s : p->s) {
		if (!inside(s.x, s.y))
			continue;
		const size_t k = size_t(s.ibeam) * size_t(nping) + size_t(s.iping);
		lon[k] = s.x;
		lat[k] = s.y;
		const bool ok = mb_beam_ok(s.beamflag);
		z[k] = ok ? s.z : s.z - kFlagShift;
		if (ok) {
			good.push_back(s.x);
			good.push_back(s.y);
			good.push_back(s.z);
		}
	}
	if (good.empty()) {
		QMessageBox::information(nullptr, "Show point-cloud", "No good sounding of " + p->name + " lies inside this area.");
		return false;
	}
	const QString title = p->name + " (area)";
	// the parent waits: its soundings and edits stay, only its pane goes (one 3-D sounding editor at a time)
	g_cloud = nullptr;
	if (mb3dsdgIsOpen())
		mb3dsdgEnd();
	void *areaScene = makeWindow(good.data(), int(good.size() / 3), title);
	bool ok = areaScene && mb3dsdgOpenCloud(areaScene, p->host, lon.data(), lat.data(), z.data(), nping, nbeam, title,
	                                        p->pingTime.data(), p->pingFile.data(), p->files);
	if (ok && g_cloud && g_cloud->scene == areaScene) {
		g_cloud->parent = p;
		g_cloud->closeWin = closeWin;
		mb3dsdgSetAreaMode();
		return true;
	}
	// it did not open: the parent gets its pane back
	if (areaScene && closeWin)
		closeWin(areaScene);
	g_cloud = p;
	if (!mb3dsdgOpenPane(p->scene, p->host, &p->data, cloudNotify())) {
		g_cloud = nullptr;
		cloudSaveEdits(p);
		delete p;
	}
	return false;
}

bool mb3dsdgAreaOpen() {
	return g_cloud && g_cloud->parent;
}

bool mb3dsdgAreaFinish(bool accept) {
	if (!mb3dsdgAreaOpen())
		return false;
	areaFinish(accept, true);
	return true;
}

QString mb3dsdgCloudName(void *scene) {
	return (g_cloud && g_cloud->scene == scene) ? g_cloud->name : QString();
}

int mb3dsdgCloudGood(void *scene, double *xyz, int cap) {
	if (!g_cloud || g_cloud->scene != scene)
		return -1;
	int n = 0;
	for (const auto &s : g_cloud->s) {
		if (!mb_beam_ok(s.beamflag))
			continue;
		if (xyz && n < cap) {
			xyz[3 * n] = s.x;
			xyz[3 * n + 1] = s.y;
			xyz[3 * n + 2] = s.z;
		}
		n++;
	}
	return n;
}

bool mb3dsdgOpenCloud(void *scene, const MbEditHost &host, const double *lon, const double *lat, const double *z,
                      int nping, int nbeam, const QString &name, const double *ptime, const int *pfile,
                      const QStringList &files) {
	if (!scene || !lon || !lat || !z || !ptime || !pfile || nping <= 0 || nbeam <= 0 || files.isEmpty())
		return false;
	// the edits are written with MB-System's .esf routines, through the MBIO library the editors load
	if (!mbeditLoadMbio(nullptr, host))
		return false;
	if (mb3dsdgIsOpen())
		mb3dsdgEnd();                        // one 3-D sounding editor at a time (its edits are saved)
	if (g_cloud)
		cloudDismiss();
	auto *c = new Cloud;
	c->name = name;
	c->scene = scene;
	c->host = host;
	c->pingTime.assign(ptime, ptime + nping);
	c->pingFile.assign(pfile, pfile + nping);
	c->files = files;
	c->s.reserve(size_t(nping) * size_t(nbeam));
	for (int b = 0; b < nbeam; b++)
		for (int p = 0; p < nping; p++) {
			const size_t k = size_t(b) * size_t(nping) + size_t(p);   // column-major, ping fastest
			double zz = z[k];
			if (std::isnan(zz) || std::isnan(lon[k]) || std::isnan(lat[k]))
				continue;
			// -A-1000000 moves a flagged beam a million metres away -- UP, as this mbgetdata applies it
			// (a negative -A value also asks for the .esf); whichever way, it is put back
			const bool flagged = std::fabs(zz) > 0.5 * std::fabs(kFlagShift);
			if (flagged)
				zz -= (zz > 0.0 ? 1.0 : -1.0) * std::fabs(kFlagShift);
			mb3dsoundings_sounding_struct s{};
			s.ifile = pfile[p];                  // its file (Color by File, and the file an edit belongs to)
			s.iping = p;
			s.ibeam = b;
			s.beamflag = flagged ? char(MB_FLAG_FLAG + MB_FLAG_MANUAL) : char(MB_FLAG_NONE);
			s.beamflagorg = s.beamflag;
			s.x = lon[k];                        // the pane draws in TRUE coords: x, y = lon, lat
			s.y = lat[k];
			s.z = zz;
			c->s.push_back(s);
			if (-zz > c->maxDepth)
				c->maxDepth = -zz;
		}
	if (c->s.empty()) {
		QMessageBox::warning(nullptr, "3D Soundings", "mbgetdata gave no soundings for " + name);
		delete c;
		return false;
	}
	memset(&c->data, 0, sizeof(c->data));
	c->data.num_soundings = int(c->s.size());
	c->data.num_soundings_alloc = int(c->s.size());
	c->data.soundings = c->s.data();
	for (const auto &s : c->s) {
		if (mb_beam_ok(s.beamflag))
			c->data.num_soundings_unflagged++;
		else
			c->data.num_soundings_flagged++;
	}
	c->data.scale = 1.0;
	c->data.zscale = 1.0;
	c->data.displayed = true;
	g_cloud = c;
	if (!mb3dsdgOpenPane(scene, host, &c->data, cloudNotify())) {
		g_cloud = nullptr;
		delete c;
		return false;
	}
	return true;
}
