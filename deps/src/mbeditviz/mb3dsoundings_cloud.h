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
// CUBE flagging, on the pane: the good soundings (mb3dsdgCloudGood's order) where bad[i] != 0 are flagged
// as FILTERED, as edits of the pane (shown at once, written to the .esf by its Save). How many, -1 = no pane.
int mb3dsdgCloudFlag(void *scene, const unsigned char *bad, int n);
// CUBE flagging, straight into the swath files' .esf (no pane): n soundings by ping index and beam, a
// ping's time ptime[ping] in files[pfile[ping]], saved as FILTER flags. n, or -1 on failure.
int mb3dsdgEsfFlag(const MbEditHost &host, const QStringList &files, const double *ptime, const int *pfile,
                   int nping, const int *ping, const int *beam, int n);
// A line area's "Show point-cloud": the soundings of `parentScene`'s swath cloud inside the polygon `ring`
// (nring x,y pairs, true coords) in a window of their own (`makeWindow`, given the good ones as x,y,z and a
// title; it returns the new window), with their own 3D Soundings pane. They keep their ping and beam, so
// the area pane's edits are the parent's soundings': Accept hands them to the parent, Discard drops them
// (closing the area window, by `closeWin`, is a Discard), and the parent -- whose pane steps aside
// meanwhile, its soundings and edits kept -- gets its pane back. False when nothing opened.
bool mb3dsdgOpenAreaCloud(void *parentScene, const double *ring, int nring,
                          const std::function<void *(const double *xyz, int n, const QString &title)> &makeWindow,
                          const std::function<void(void *)> &closeWin);
// The open 3D Soundings view (mb3dsoundings_window.h): points it draws, good / flagged counts; and View >
// Show flagged through its menu entry
bool mb3dsdgCounts(int *drawn, int *good, int *flagged);
bool mb3dsdgSetShowFlagged(bool on);
// An area pane is the open one (its parent waiting); its Accept (true) or Discard (false), as its buttons
bool mb3dsdgAreaOpen();
bool mb3dsdgAreaFinish(bool accept);
// The name of `scene`'s swath-cloud pane cloud ("" when it has none)
QString mb3dsdgCloudName(void *scene);

// The pane's Navigation toggle (mb3dsoundings_window.h): calls `show`; its box starts at `on`.
void mb3dsdgSetNavToggle(std::function<void(bool)> show, bool on);

#endif // MB3DSOUNDINGS_CLOUD_H_
