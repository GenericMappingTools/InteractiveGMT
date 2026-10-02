// ============================================================================
//  mbedit_window.h -- the ONE entry point of the swath bathymetry editor (MB-System's mbedit,
//  ported) for the rest of the viewer.
//
//  The editor lives in its own folder, deps/src/mbedit/, apart from the viewer's code:
//    mbedit.c / mbedit.h / mbedit_mbio.h   the engine (mbedit_prog.c), plain C, own TU
//    mbedit_window.cpp                      the Qt window (mbedit_callbacks.c), own TU
//    deps/ui/mbedit*.ui                     its window and dialogs, loaded at run time
//  The viewer reaches it only through mbeditOpenWindow(), from the Tools menu. What the
//  window needs from the viewer it is HANDED in MbEditHost (the viewer's helpers are
//  file-static in gmtvtk.cpp, so a separate translation unit cannot call them by name).
// ============================================================================

#ifndef MBEDIT_WINDOW_H_
#define MBEDIT_WINDOW_H_

#include <QIcon>
#include <QString>

class QWidget;

struct MbEditHost {
	QString uiDir;                                   // deps/ui, where the mbedit*.ui files are
	QIcon icon;                                      // the app's window icon
	void (*busyText)(const char *text);              // raise (or re-label) the app's busy notice
	void (*busyOff)();                               // close it
	void (*windowOpened)();                          // a top-level window the pump must keep alive
	void (*windowClosed)();
	QString (*startDir)();                           // the file dialogs' starting folder
	void (*rememberDir)(const QString &path);
	QString (*setting)(const char *key);             // iGMT.ini
	void (*setSetting)(const char *key, const QString &value);
};

// Open the editor, or raise it if it is already open (the engine is a single instance: its
// state is file-static, as in mbedit). With a `file` (a swath file or a datalist) it is loaded
// at once, as `mbedit -I file -F format` does; without one, File > Open is offered.
// `useEsf` decides about an existing edit save file: -1 ask (mbedit's dialog), 0 ignore it,
// 1 apply it (mbedit -S). Returns false when the editor could not be opened.
bool mbeditOpenWindow(QWidget *parent, const MbEditHost &host, const QString &file = QString(), int format = 0,
                      int useEsf = -1);

// Load MB-System's MBIO library the way the editor does (INTERACTIVEGMT_MBIO, iGMT.ini, the one GMT
// loaded, then asking), without opening the editor. The ONE MBIO loader: the sound velocity tool
// (deps/src/mbvelocitytool/) reads its swath files through the same library and table.
bool mbeditLoadMbio(QWidget *parent, const MbEditHost &host);

// The MBIO library GMT loaded with its MB-System supplement (GMT_CUSTOM_LIBS), found by the host
// through GMT. Tried right after INTERACTIVEGMT_MBIO and the saved path. Empty = none.
void mbeditSetMbioHint(const QString &path);

// Drive / read the open editor (the host's C API and tests). All are no-ops without an editor.
//   state: [open, numfiles, currentfile, nbuffer, ngood, icurrent, nplot, nflagged, nunflagged]
int mbeditState(int *out, int n);
bool mbeditKey(int ch);                          // a key on the canvas, as typed
bool mbeditClick(int x, int y);                  // one press + release of the edit button
bool mbeditSavePng(const QString &path);         // the canvas as drawn
bool mbeditClose();                              // Quit: save the file being edited, close

#endif // MBEDIT_WINDOW_H_
