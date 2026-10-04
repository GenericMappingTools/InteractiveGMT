// ============================================================================
//  mb3dsoundings_cloud.h -- the 3D Soundings pane of a swath point cloud.
//
//  The soundings come from MB-System's own mbgetdata (the GMT module of its supplement, called by the
//  host: `mbgetdata -I<file> -A-1000000`): three nping x nbeam arrays -- longitude, latitude, depth
//  (up) -- column-major (ping fastest), NaN where a ping has no such beam, every FLAGGED beam (the
//  file's own flags and its .esf edits) shifted by -1000000 in depth. The 3-D sounding editor then
//  opens as a narrow pane of the iGMT window `scene` (mb3dsdgOpenPane).
// ============================================================================

#ifndef MB3DSOUNDINGS_CLOUD_H_
#define MB3DSOUNDINGS_CLOUD_H_

#include <QString>
#include <QStringList>

#include <functional>

#include "../mbedit/mbedit_window.h"

// `ptime` / `pfile` (nping each): every ping's time and its file's index into `files` -- MB-System's
// mblist -OM.F, same ping order as mbgetdata -- what the edits are saved by (each file's .esf, by ping
// time and beam, MB-System's mb_esf_save) when the pane closes. 1 = the pane opened
bool mb3dsdgOpenCloud(void *scene, const MbEditHost &host, const double *lon, const double *lat, const double *z,
                      int nping, int nbeam, const QString &name, const double *ptime, const int *pfile,
                      const QStringList &files);

// The pane's GOOD soundings as they stand (its edits included), lon/lat/z triples into xyz (up to cap of
// them; xyz may be null to count). The count, or -1 when `scene` has no swath-cloud pane.
int mb3dsdgCloudGood(void *scene, double *xyz, int cap);

// The pane's Navigation toggle (mb3dsoundings_window.h): calls `show`; its box starts at `on`.
void mb3dsdgSetNavToggle(std::function<void(bool)> show, bool on);

#endif // MB3DSOUNDINGS_CLOUD_H_
