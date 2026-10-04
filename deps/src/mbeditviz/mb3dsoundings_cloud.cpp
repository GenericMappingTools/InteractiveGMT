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

#include <cmath>
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
void cloudSaveEdits(Cloud *c) {
	if (!c || c->edits.empty())
		return;
	QStringList failed;
	for (int f = 0; f < c->files.size(); f++) {
		bool any = false;
		for (const auto &e : c->edits)
			if (e.ping >= 0 && e.ping < int(c->pingFile.size()) && c->pingFile[e.ping] == f) {
				any = true;
				break;
			}
		if (!any)
			continue;
		char file[MB_PATH_MAXLINE], esffile[MB_PATH_MAXLINE];
		snprintf(file, sizeof(file), "%s", c->files[f].toUtf8().constData());
		struct mb_esf_struct esf;
		memset(&esf, 0, sizeof(esf));
		int error = MB_ERROR_NO_ERROR;
		if (mb_esf_load(0, "iGMT", file, false, MBP_ESF_APPEND, esffile, &esf, &error) != MB_SUCCESS) {
			failed << QFileInfo(c->files[f]).fileName();
			continue;
		}
		for (const auto &e : c->edits) {
			if (e.ping < 0 || e.ping >= int(c->pingFile.size()) || c->pingFile[e.ping] != f)
				continue;
			int action = MBP_EDIT_FLAG;
			if (mb_beam_ok(e.flag))
				action = MBP_EDIT_UNFLAG;
			else if (mb_beam_check_flag_filter(e.flag) || mb_beam_check_flag_filter2(e.flag))
				action = MBP_EDIT_FILTER;
			mb_esf_save(0, &esf, c->pingTime[e.ping], e.beam, action, &error);
		}
		mb_esf_close(0, &esf, &error);
		mb_pr_update_edit(0, file, MBP_EDIT_ON, esffile, &error);
	}
	if (failed.isEmpty())
		c->edits.clear();
	else
		QMessageBox::warning(nullptr, "3D Soundings", "The edits could not be saved for: " + failed.join(", "));
}

void cloudSave() {
	cloudSaveEdits(g_cloud);
}

// CUBE gridding: the host's CUBE dialog, its input these soundings (mb3dsdgCloudGood)
void cloudCube() {
	if (g_cloud && g_cloud->host.openCubeOnCloud)
		g_cloud->host.openCubeOnCloud(g_cloud->scene, g_cloud->name.toUtf8().constData());
}

// the pane is gone: what is not saved yet is saved, the soundings released
void cloudDismiss() {
	Cloud *c = g_cloud;
	g_cloud = nullptr;
	cloudSaveEdits(c);
	delete c;
}

} // namespace

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
			s.ifile = 0;
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
	Mb3dsdgNotify n;
	n.dismiss = &cloudDismiss;
	n.edit = &cloudEdit;
	n.info = &cloudInfo;
	n.flagsparsevoxels = &cloudSparse;
	n.colorsoundings = &cloudColor;
	n.save = &cloudSave;
	n.cube = &cloudCube;
	if (!mb3dsdgOpenPane(scene, host, &c->data, n)) {
		g_cloud = nullptr;
		delete c;
		return false;
	}
	return true;
}
