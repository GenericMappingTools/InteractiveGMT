/* iGMT desktop launcher — ONE program, three operating systems.
 *
 * Replaces the Windows-only script trio (iview_app.vbs + installer/make_desktop_shortcut.vbs +
 * iview_splash.hta): those could never exist on Linux/macOS, so those two platforms had no
 * desktop icon at all. Everything they did lives here, in portable C with three thin
 * platform blocks.
 *
 * Two modes:
 *
 *   igmt [--julia=PATH] [--root=PATH] [files...]
 *       Launch. Resolves the InteractiveGMT package root and julia's executable, spawns
 *       `julia -t auto [--project=root] <root>/iview_app.jl <files...>` DETACHED, and (Windows)
 *       shows the splash until the viewer window signals it is up.
 *
 *   igmt --install-shortcut [--julia=PATH] [--root=PATH]
 *       Create/refresh the desktop entry: a .lnk on Windows, a .desktop on Linux, an .app
 *       bundle on macOS. Run by _ensure_desktop_shortcut() (src/InteractiveGMT.jl) at
 *       precompile time, which knows both paths for certain and passes them as hints.
 *
 * WHERE THE BINARY LIVES: the PACKAGE ROOT, beside iview_app.jl and igmt.ico — that is what the
 * desktop entry points at. A release zip unpacks it into <depot>/gmtvtk_runtime/deps/build, so
 * --install-shortcut copies it up into the package root from there. <home>/.gmt is for SETTINGS
 * (iGMT.ini) and receives nothing but igmt_launcher.ini: never a binary, never an icon copy.
 * The ini records root + julia so a launch needs no searching; it is rewritten by every
 * --install-shortcut run (i.e. every precompile), and if it ever goes stale the resolver falls
 * back to a live search — the same search the old .vbs did.
 *
 * Pure C, no Qt, no VTK, no dependency on gmtvtk.dll: this must run BEFORE Julia exists in the
 * process, and must still put a readable error on screen on a machine where nothing works.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>   /* CommandLineToArgvW */
#include <objidl.h>
#include <olectl.h>
#include <ocidl.h>
#define PATHSEP '\\'
#define PATHSEPS "\\"
#define ENVSEP ';'
#define EXESUF ".exe"
#else
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif
#define PATHSEP '/'
#define PATHSEPS "/"
#define ENVSEP ':'
#define EXESUF ""
#endif

#define MAXP 4096

/* ------------------------------------------------------------------ small path/file helpers */

static void joinp(char *out, size_t n, const char *a, const char *b)
{
	size_t la = strlen(a);
	if (la && (a[la - 1] == '/' || a[la - 1] == '\\'))
		snprintf(out, n, "%s%s", a, b);
	else
		snprintf(out, n, "%s%c%s", a, PATHSEP, b);
}

static void parent_of(char *p)
{
	char *s = p + strlen(p);
	while (s > p && *s != '/' && *s != '\\') s--;
	*s = 0;
}

#ifdef _WIN32
static wchar_t *wide(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}

static char *utf8(const wchar_t *w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = (char *)malloc((size_t)n);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	return s;
}
#endif

static int file_exists(const char *p)
{
#ifdef _WIN32
	wchar_t *w = wide(p);
	DWORD a = GetFileAttributesW(w);
	free(w);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
#endif
}

static int dir_exists(const char *p)
{
#ifdef _WIN32
	wchar_t *w = wide(p);
	DWORD a = GetFileAttributesW(w);
	free(w);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void make_dir(const char *p)
{
#ifdef _WIN32
	wchar_t *w = wide(p);
	CreateDirectoryW(w, NULL);
	free(w);
#else
	mkdir(p, 0755);
#endif
}

/* mkdir -p, so an .app's Contents/MacOS lands in one call. */
static void make_dirs(const char *p)
{
	char buf[MAXP];
	size_t i;
	snprintf(buf, sizeof(buf), "%s", p);
	for (i = 1; buf[i]; i++) {
		if (buf[i] == '/' || buf[i] == '\\') {
			char c = buf[i];
			buf[i] = 0;
			make_dir(buf);
			buf[i] = c;
		}
	}
	make_dir(buf);
}

static int copy_file(const char *src, const char *dst, int executable)
{
#ifdef _WIN32
	wchar_t *ws = wide(src), *wd = wide(dst);
	BOOL ok = CopyFileW(ws, wd, FALSE);
	free(ws); free(wd);
	(void)executable;
	return ok ? 0 : -1;
#else
	int in, out, rc = 0;
	char buf[65536];
	ssize_t k;
	if ((in = open(src, O_RDONLY)) < 0) return -1;
	if ((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, executable ? 0755 : 0644)) < 0) {
		close(in);
		return -1;
	}
	while ((k = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)k) != k) { rc = -1; break; }
	if (k < 0) rc = -1;
	close(in);
	close(out);
	if (rc == 0 && executable) chmod(dst, 0755);
	return rc;
#endif
}

static const char *home_dir(void)
{
	static char h[MAXP];
	if (h[0]) return h;
#ifdef _WIN32
	{
		const char *up = getenv("USERPROFILE");
		if (up && *up) snprintf(h, sizeof(h), "%s", up);
		else snprintf(h, sizeof(h), "C:\\Users\\Default");
	}
#else
	{
		const char *up = getenv("HOME");
		snprintf(h, sizeof(h), "%s", (up && *up) ? up : "/tmp");
	}
#endif
	return h;
}

/* <home>/.gmt — the same directory iGMT already keeps its settings ini in. */
static const char *gmt_dir(void)
{
	static char d[MAXP];
	if (!d[0]) {
		joinp(d, sizeof(d), home_dir(), ".gmt");
		make_dirs(d);
	}
	return d;
}

static void self_path(char *out, size_t n)
{
#if defined(_WIN32)
	wchar_t w[MAXP];
	char *s;
	GetModuleFileNameW(NULL, w, MAXP);
	s = utf8(w);
	snprintf(out, n, "%s", s);
	free(s);
#elif defined(__APPLE__)
	uint32_t sz = (uint32_t)n;
	if (_NSGetExecutablePath(out, &sz) != 0) snprintf(out, n, "igmt");
#else
	ssize_t k = readlink("/proc/self/exe", out, n - 1);
	if (k < 0) k = 0;
	out[k] = 0;
#endif
}

static void message_box(const char *title, const char *text)
{
#ifdef _WIN32
	wchar_t *wt = wide(title), *wx = wide(text);
	MessageBoxW(NULL, wx, wt, MB_ICONEXCLAMATION | MB_OK);
	free(wt); free(wx);
#else
	fprintf(stderr, "%s: %s\n", title, text);
#endif
}

/* ------------------------------------------------------------------------------ the ini file */

static void ini_path(char *out, size_t n) { joinp(out, n, gmt_dir(), "igmt_launcher.ini"); }

static int ini_get(const char *key, char *out, size_t n)
{
	char path[MAXP], line[MAXP];
	FILE *f;
	size_t kl = strlen(key);
	int got = 0;
	ini_path(path, sizeof(path));
	if (!(f = fopen(path, "r"))) return 0;
	while (fgets(line, sizeof(line), f)) {
		size_t l = strlen(line);
		while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
		if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
			snprintf(out, n, "%s", line + kl + 1);
			got = out[0] != 0;
		}
	}
	fclose(f);
	return got;
}

static void ini_write(const char *root, const char *julia)
{
	char path[MAXP];
	FILE *f;
	ini_path(path, sizeof(path));
	if (!(f = fopen(path, "w"))) return;
	fprintf(f, "# Written by `igmt --install-shortcut`. Rewritten on every precompile of\n");
	fprintf(f, "# InteractiveGMT; the launcher searches for itself if these ever go stale.\n");
	fprintf(f, "root=%s\n", root ? root : "");
	fprintf(f, "julia=%s\n", julia ? julia : "");
	fclose(f);
}

/* ------------------------------------------------------------- locating the package + julia */

static int is_pkg_root(const char *d)
{
	char p[MAXP];
	joinp(p, sizeof(p), d, "iview_app.jl");
	return file_exists(p);
}

/* First entry of JULIA_DEPOT_PATH, else <home>/.julia — exactly what Julia itself resolves. */
static void depot_root(char *out, size_t n)
{
	const char *e = getenv("JULIA_DEPOT_PATH");
	if (e && *e) {
		const char *sep = strchr(e, ENVSEP);
		size_t len = sep ? (size_t)(sep - e) : strlen(e);
		if (len >= n) len = n - 1;
		memcpy(out, e, len);
		out[len] = 0;
		if (out[0]) return;
	}
	joinp(out, n, home_dir(), ".julia");
}

/* Newest-modified <depot>/packages/InteractiveGMT/<hash> that actually holds the package —
 * a `] add` install re-hashes that folder name on every update, so "newest" is the live one. */
static int newest_pkg_dir(const char *parent, char *out, size_t n)
{
	int found = 0;
#ifdef _WIN32
	char pat[MAXP];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	wchar_t *wp;
	ULONGLONG best = 0;
	snprintf(pat, sizeof(pat), "%s\\*", parent);
	wp = wide(pat);
	h = FindFirstFileW(wp, &fd);
	free(wp);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do {
		char *nm, cand[MAXP];
		ULONGLONG t;
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (fd.cFileName[0] == L'.') continue;
		nm = utf8(fd.cFileName);
		joinp(cand, sizeof(cand), parent, nm);
		free(nm);
		if (!is_pkg_root(cand)) continue;
		t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
		if (t >= best) { best = t; snprintf(out, n, "%s", cand); found = 1; }
	} while (FindNextFileW(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(parent);
	struct dirent *e;
	time_t best = 0;
	if (!d) return 0;
	while ((e = readdir(d))) {
		char cand[MAXP];
		struct stat st;
		if (e->d_name[0] == '.') continue;
		joinp(cand, sizeof(cand), parent, e->d_name);
		if (stat(cand, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		if (!is_pkg_root(cand)) continue;
		if (st.st_mtime >= best) { best = st.st_mtime; snprintf(out, n, "%s", cand); found = 1; }
	}
	closedir(d);
#endif
	return found;
}

/* Resolution order, cheapest first. Every step is validated (is_pkg_root), so a stale hint
 * silently falls through to the live search instead of failing the launch. */
static int find_root(char *out, size_t n, const char *hint)
{
	char buf[MAXP], depot[MAXP];

	if (hint && *hint && is_pkg_root(hint)) { snprintf(out, n, "%s", hint); return 1; }
	if (ini_get("root", buf, sizeof(buf)) && is_pkg_root(buf)) { snprintf(out, n, "%s", buf); return 1; }

	/* Running in place from <root>/deps/build/igmt — two levels up is the package. */
	self_path(buf, sizeof(buf));
	parent_of(buf);                       /* .../deps/build   */
	{
		char here[MAXP];
		snprintf(here, sizeof(here), "%s", buf);
		if (is_pkg_root(here)) { snprintf(out, n, "%s", here); return 1; }
	}
	parent_of(buf);                       /* .../deps         */
	parent_of(buf);                       /* .../<root>       */
	if (is_pkg_root(buf)) { snprintf(out, n, "%s", buf); return 1; }

	depot_root(depot, sizeof(depot));
	joinp(buf, sizeof(buf), depot, "dev" PATHSEPS "InteractiveGMT");
	if (is_pkg_root(buf)) { snprintf(out, n, "%s", buf); return 1; }

	joinp(buf, sizeof(buf), depot, "packages" PATHSEPS "InteractiveGMT");
	if (dir_exists(buf) && newest_pkg_dir(buf, out, n)) return 1;

	return 0;
}

static int julia_ok(const char *p) { return p && *p && file_exists(p); }

#ifdef _WIN32
/* "1.10.4" -> 1010004 so 1.10 outranks 1.9 (a string compare gets that backwards). */
static long version_key(const char *v)
{
	long a = 0, b = 0, c = 0;
	sscanf(v, "%ld.%ld.%ld", &a, &b, &c);
	return a * 1000000 + b * 1000 + c;
}

/* Scan the usual install roots. A julia that ALREADY has InteractiveGMT precompiled in this
 * depot beats a newer one that does not: picking the newest blindly can hand the launcher a
 * julia that must precompile the whole dependency tree before the window appears. */
static int scan_julia_roots(char *out, size_t n)
{
	const char *roots[4];
	char lap[MAXP], depot[MAXP];
	int i, found = 0, best_warm = -1;
	long best_ver = -1;
	const char *la = getenv("LOCALAPPDATA");

	snprintf(lap, sizeof(lap), "%s\\Programs", la ? la : "");
	depot_root(depot, sizeof(depot));
	roots[0] = "C:\\programs";
	roots[1] = "C:\\";
	roots[2] = la ? lap : NULL;
	roots[3] = NULL;

	for (i = 0; i < 3; i++) {
		char pat[MAXP];
		WIN32_FIND_DATAW fd;
		HANDLE h;
		wchar_t *wp;
		if (!roots[i]) continue;
		snprintf(pat, sizeof(pat), "%s\\julia-*", roots[i]);
		wp = wide(pat);
		h = FindFirstFileW(wp, &fd);
		free(wp);
		if (h == INVALID_HANDLE_VALUE) continue;
		do {
			char *nm, cand[MAXP], warmdir[MAXP];
			long ver;
			int warm;
			long maj = 0, min = 0;
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
			nm = utf8(fd.cFileName);
			snprintf(cand, sizeof(cand), "%s\\%s\\bin\\julia.exe", roots[i], nm);
			if (!file_exists(cand)) { free(nm); continue; }
			ver = version_key(nm + 6);
			sscanf(nm + 6, "%ld.%ld", &maj, &min);
			free(nm);
			snprintf(warmdir, sizeof(warmdir), "%s\\compiled\\v%ld.%ld\\InteractiveGMT", depot, maj, min);
			warm = dir_exists(warmdir) ? 1 : 0;
			if (warm > best_warm || (warm == best_warm && ver > best_ver)) {
				best_warm = warm;
				best_ver = ver;
				snprintf(out, n, "%s", cand);
				found = 1;
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	return found;
}
#endif

static int find_julia(char *out, size_t n, const char *hint)
{
	char buf[MAXP];
	const char *env, *path;

	if (julia_ok(hint)) { snprintf(out, n, "%s", hint); return 1; }
	if (ini_get("julia", buf, sizeof(buf)) && julia_ok(buf)) { snprintf(out, n, "%s", buf); return 1; }

	env = getenv("JULIA_EXE");
	if (julia_ok(env)) { snprintf(out, n, "%s", env); return 1; }

	/* Whatever a terminal `julia` would run. */
	path = getenv("PATH");
	if (path && *path) {
		const char *p = path;
		while (*p) {
			const char *sep = strchr(p, ENVSEP);
			size_t len = sep ? (size_t)(sep - p) : strlen(p);
			if (len && len < sizeof(buf) - 32) {
				char dir[MAXP], cand[MAXP];
				memcpy(dir, p, len);
				dir[len] = 0;
				joinp(cand, sizeof(cand), dir, "julia" EXESUF);
				if (file_exists(cand)) { snprintf(out, n, "%s", cand); return 1; }
			}
			if (!sep) break;
			p = sep + 1;
		}
	}

	/* juliaup's shim. */
#ifdef _WIN32
	{
		char depot[MAXP];
		depot_root(depot, sizeof(depot));
		joinp(buf, sizeof(buf), depot, "juliaup" PATHSEPS "bin" PATHSEPS "julia.exe");
		if (file_exists(buf)) { snprintf(out, n, "%s", buf); return 1; }
	}
	if (scan_julia_roots(out, n)) return 1;
#else
	{
		static const char *cands[] = {
			"/.juliaup/bin/julia", "/.local/bin/julia", NULL
		};
		static const char *abs_cands[] = {
			"/usr/local/bin/julia", "/opt/homebrew/bin/julia", "/usr/bin/julia",
			"/opt/julia/bin/julia", NULL
		};
		int i;
		for (i = 0; cands[i]; i++) {
			snprintf(buf, sizeof(buf), "%s%s", home_dir(), cands[i]);
			if (file_exists(buf)) { snprintf(out, n, "%s", buf); return 1; }
		}
		for (i = 0; abs_cands[i]; i++)
			if (file_exists(abs_cands[i])) { snprintf(out, n, "%s", abs_cands[i]); return 1; }
	}
#endif
	return 0;
}

/* ----------------------------------------------------------------------------------- splash
 *
 * THE SAME SPLASH ON ALL THREE SYSTEMS: the same picture, the same size relative to the screen,
 * the same caption, closing on the same event. Only the drawing API differs — Win32/GDI,
 * X11, Cocoa — because there is no portable one and linking Qt here is impossible (this runs
 * before Julia, before gmtvtk.dll, before Qt is on any search path).
 *
 * Shown AFTER julia is already spawned, so the click gets instant feedback while Julia + package
 * load — the variable, unpredictable part — happens in the background. It closes on the real
 * event, not a timer: iview_app.jl writes a ready-flag once the viewer window is up and the
 * splash polls for it, with a long timeout as the only backstop.
 *
 * Image source differs by platform for one reason: Cocoa (NSImage) and Win32 (OleLoadPicture)
 * decode JPEG themselves, X11 decodes nothing at all. Rather than drag libjpeg/libpng into a
 * launcher that must work on a machine where nothing else does, the same picture also ships as
 * an uncompressed .bmp, which is ~40 lines of parsing and no dependency.
 */

/* Must match iview_app.jl's joinpath(tempdir(), "igmt_ready.flag") — Julia's tempdir() is
 * %TEMP% on Windows and $TMPDIR (per-user, /var/folders/... on macOS) else /tmp on Unix. */
static void ready_flag_path(char *out, size_t n)
{
#ifdef _WIN32
	const char *t = getenv("TEMP");
	joinp(out, n, (t && *t) ? t : ".", "igmt_ready.flag");
#else
	const char *t = getenv("TMPDIR");
	char dir[MAXP];
	size_t l;
	snprintf(dir, sizeof(dir), "%s", (t && *t) ? t : "/tmp");
	l = strlen(dir);
	while (l > 1 && dir[l - 1] == '/') dir[--l] = 0;
	joinp(out, n, dir, "igmt_ready.flag");
#endif
}

static char g_flag[MAXP];

/* The picture, wherever this copy of the package keeps it: beside iview_app.jl in an installed
 * tree, under deps/assets in a git checkout. */
static int splash_image_path(const char *root, const char *leaf, char *out, size_t n)
{
	joinp(out, n, root, leaf);
	if (file_exists(out)) return 1;
	snprintf(out, n, "%s%cdeps%cassets%c%s", root, PATHSEP, PATHSEP, PATHSEP, leaf);
	return file_exists(out);
}

/* Same geometry rule everywhere: 30% of screen width, 520 px floor, 0.68 aspect, centered. */
static void splash_geometry(int screen_w, int screen_h, int *x, int *y, int *w, int *h)
{
	*w = (int)(screen_w * 0.30);
	if (*w < 520) *w = 520;
	*h = (int)(*w * 0.68);
	*x = (screen_w - *w) / 2;
	*y = (screen_h - *h) / 2;
}

/* LAYOUT, ported element for element from iview_splash.hta so the splash keeps the look it has
 * always had. Three things over the nebula background:
 *   - the iGMT icon, 120 px, in the UPPER-LEFT corner (the .hta's `img { position:absolute;
 *     top:0; left:0; width:120px }`) — it is the app's identity and is never dropped;
 *   - the caption, centred, with a dark halo because the nebula's middle is bright;
 *   - an indeterminate progress bar: a dark track with a lighter chunk sweeping across it.
 * Same three elements, same proportions, on Windows, Linux and macOS. */
#define SPLASH_ICON_PX  75        /* the .hta asked for 120, which reads far too heavy at this size */
#define SPLASH_CYCLE_MS 1100      /* one sweep of the chunk — the .hta's `slide 1.1s linear` */

/* Track geometry in window coordinates, y measured from the TOP on every platform (Cocoa's
 * bottom-left origin is flipped once, at the call site). #bar was 60% of the width, #chunk 18%. */
static void splash_bar_rect(int w, int h, int *bx, int *by, int *bw, int *bh, int *cw)
{
	*bw = (int)(w * 0.60);
	*bh = (int)(h * 0.030);
	if (*bh < 5) *bh = 5;
	*bx = (w - *bw) / 2;
	*by = h - h / 5;
	*cw = (int)(w * 0.18);
}

/* Left edge of the chunk at time t: from fully off the track's left to fully off its right,
 * which is what the .hta's `from left:-18vw to left:78vw` describes. */
static int splash_chunk_x(int bx, int bw, int cw, unsigned long ms)
{
	double t = (double)(ms % SPLASH_CYCLE_MS) / (double)SPLASH_CYCLE_MS;
	return bx - cw + (int)((bw + cw) * t);
}

static unsigned long now_ms(void)
{
#ifdef _WIN32
	return (unsigned long)GetTickCount();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
#endif
}

#ifdef _WIN32
static IPicture *g_pic;
static HICON g_icon;

static void splash_paint(HWND hw)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(hw, &ps);
	RECT rc;
	GetClientRect(hw, &rc);
	if (g_pic) {
		OLE_XSIZE_HIMETRIC iw = 0;
		OLE_YSIZE_HIMETRIC ih = 0;
		g_pic->lpVtbl->get_Width(g_pic, &iw);
		g_pic->lpVtbl->get_Height(g_pic, &ih);
		if (iw > 0 && ih > 0) {
			/* background-size: cover — crop the source to the window's aspect, then stretch. */
			double wa = (double)(rc.right) / (double)(rc.bottom);
			double ia = (double)iw / (double)ih;
			long sx = 0, sy = 0, sw = iw, sh = ih;
			if (ia > wa) { sw = (long)(ih * wa); sx = (iw - sw) / 2; }
			else         { sh = (long)(iw / wa); sy = (ih - sh) / 2; }
			g_pic->lpVtbl->Render(g_pic, dc, 0, 0, rc.right, rc.bottom,
			                      sx, ih - sy, sw, -sh, NULL);
		}
	}
	else {
		HBRUSH b = CreateSolidBrush(RGB(10, 13, 24));
		FillRect(dc, &rc, b);
		DeleteObject(b);
	}
	/* The icon, upper-left corner — the .hta's `img { top:0; left:0; width:120px }`. */
	if (g_icon)
		DrawIconEx(dc, 0, 0, g_icon, SPLASH_ICON_PX, SPLASH_ICON_PX, 0, NULL, DI_NORMAL);

	{
		/* \x2026 is the ellipsis, written as a code point on purpose: MSVC reads this file with
		 * the system codepage unless told otherwise, so a literal UTF-8 "…" in a wide string
		 * came out as mojibake ("startingâ€¦"). Escapes cannot be misread. */
		static const wchar_t *cap = L"Starting iGMT\x2026";
		/* ANTIALIASED_QUALITY, not CLEARTYPE_QUALITY: ClearType needs an opaque background to
		 * blend its subpixels against, and GDI silently drops to ALIASED glyphs when it is asked
		 * to draw with a TRANSPARENT background over an image — which is what made the caption
		 * look low-resolution. Greyscale antialiasing has no such requirement. */
		HFONT f = CreateFontW(-(rc.bottom / 16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
		                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
		                      DEFAULT_PITCH, L"Segoe UI");
		HFONT old = (HFONT)SelectObject(dc, f);
		RECT tr = rc, sh2 = rc;
		SetBkMode(dc, TRANSPARENT);
		/* One soft drop shadow, not a ring of eight: the ring thickened every glyph and read as
		 * a blur. This is only there so the caption survives the nebula's bright middle. */
		OffsetRect(&sh2, 1, 2);
		SetTextColor(dc, RGB(0, 0, 0));
		DrawTextW(dc, cap, -1, &sh2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SetTextColor(dc, RGB(240, 243, 250));
		DrawTextW(dc, cap, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, old);
		DeleteObject(f);
	}

	{	/* Indeterminate progress bar — dark track, lighter chunk sweeping across it. */
		int bx, by, bw, bh, cw, cx;
		RECT r;
		HBRUSH track = CreateSolidBrush(RGB(51, 51, 51));
		HBRUSH chunk = CreateSolidBrush(RGB(91, 155, 213));
		splash_bar_rect(rc.right, rc.bottom, &bx, &by, &bw, &bh, &cw);
		cx = splash_chunk_x(bx, bw, cw, now_ms());
		SetRect(&r, bx, by, bx + bw, by + bh);
		FillRect(dc, &r, track);
		IntersectClipRect(dc, bx, by, bx + bw, by + bh);   /* the chunk is clipped by the track */
		SetRect(&r, cx, by, cx + cw, by + bh);
		FillRect(dc, &r, chunk);
		SelectClipRgn(dc, NULL);
		DeleteObject(track);
		DeleteObject(chunk);
	}

	{	/* The .hta's 1px #444 frame. */
		HBRUSH b = CreateSolidBrush(RGB(68, 68, 68));
		FrameRect(dc, &rc, b);
		DeleteObject(b);
	}
	EndPaint(hw, &ps);
}

static LRESULT CALLBACK splash_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT:
		splash_paint(hw);
		return 0;
	case WM_TIMER:
		/* iview_app.jl writes the flag once its window is up — the same handshake
		 * iview_splash.hta used, so the splash closes on the real event, not a fixed timer.
		 * wp == 2 is the safety timeout: never leave a splash on screen forever. */
		if (wp == 2 || file_exists(g_flag)) {
			DestroyWindow(hw);
			return 0;
		}
		{	/* Animate the progress chunk: only the track needs repainting. */
			RECT rc, r;
			int bx, by, bw, bh, cw;
			GetClientRect(hw, &rc);
			splash_bar_rect(rc.right, rc.bottom, &bx, &by, &bw, &bh, &cw);
			SetRect(&r, bx, by, bx + bw, by + bh);
			InvalidateRect(hw, &r, FALSE);
		}
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hw, msg, wp, lp);
}

static void splash_load(const char *root)
{
	char img[MAXP];
	wchar_t *w;
	IStream *st = NULL;
	if (!splash_image_path(root, "crystalball.jpg", img, sizeof(img))) return;
	w = wide(img);
	if (SUCCEEDED(SHCreateStreamOnFileEx(w, STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, NULL, &st))) {
		OleLoadPicture(st, 0, FALSE, &IID_IPicture, (void **)&g_pic);
		st->lpVtbl->Release(st);
	}
	free(w);

	/* The iGMT icon for the corner. LoadImage picks the best frame in the .ico and scales it. */
	if (splash_image_path(root, "igmt.ico", img, sizeof(img))) {
		w = wide(img);
		g_icon = (HICON)LoadImageW(NULL, w, IMAGE_ICON, SPLASH_ICON_PX, SPLASH_ICON_PX,
		                           LR_LOADFROMFILE);
		free(w);
	}
}

static void splash_run(const char *root)
{
	WNDCLASSEXW wc;
	HWND hw;
	MSG msg;
	int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
	int x, y, w, h;
	splash_geometry(sw, sh, &x, &y, &w, &h);

	OleInitialize(NULL);
	splash_load(root);

	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = splash_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = L"iGMTSplash";
	RegisterClassExW(&wc);

	hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"iGMTSplash", L"iGMT", WS_POPUP,
	                     x, y, w, h, NULL, NULL, wc.hInstance, NULL);
	if (!hw) return;
	ShowWindow(hw, SW_SHOWNOACTIVATE);
	UpdateWindow(hw);
	SetTimer(hw, 1, 40, NULL);          /* 25 Hz: animates the bar AND polls the ready-flag */
	SetTimer(hw, 2, 180000, NULL);
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	if (g_icon) DestroyIcon(g_icon);
	if (g_pic) g_pic->lpVtbl->Release(g_pic);
	OleUninitialize();
}

#elif defined(__APPLE__)

/* Cocoa. A borderless NSWindow at status level with the picture as the content layer's
 * contents; kCAGravityResizeAspectFill is Core Animation's name for the same "cover" crop the
 * Win32 and X11 paths compute by hand. The event loop is pumped by hand instead of [NSApp run]
 * so the exit condition stays the shared one — the ready-flag — and not a delegate callback. */
static void splash_run(const char *root)
{
	char img[MAXP];
	int have = splash_image_path(root, "crystalball.jpg", img, sizeof(img));

	@autoreleasepool {
		NSApplication *app = [NSApplication sharedApplication];
		NSRect scr;
		int x, y, w, h;
		NSWindow *win;
		NSView *v, *track, *chunk;
		NSTextField *lab;
		int barw = 0, chunkw = 0;
		time_t t0;

		[app setActivationPolicy:NSApplicationActivationPolicyAccessory];
		scr = [[NSScreen mainScreen] frame];
		splash_geometry((int)scr.size.width, (int)scr.size.height, &x, &y, &w, &h);

		/* Cocoa's origin is bottom-left; splash_geometry hands out a top-left y like every
		 * other platform here, so flip it once rather than special-casing the shared rule. */
		win = [[NSWindow alloc]
		        initWithContentRect:NSMakeRect(scr.origin.x + x,
		                                       scr.origin.y + scr.size.height - y - h, w, h)
		                  styleMask:NSWindowStyleMaskBorderless
		                    backing:NSBackingStoreBuffered
		                      defer:NO];
		[win setLevel:NSStatusWindowLevel];
		[win setOpaque:YES];
		[win setBackgroundColor:[NSColor colorWithCalibratedRed:0.039 green:0.051 blue:0.094 alpha:1.0]];

		v = [win contentView];
		[v setWantsLayer:YES];
		if (have) {
			NSImage *im = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:img]];
			if (im) {
				v.layer.contents = im;
				v.layer.contentsGravity = kCAGravityResizeAspectFill;
				v.layer.masksToBounds = YES;
			}
		}

		/* The iGMT icon, upper-left corner. Cocoa's origin is bottom-left, so "top" is h - size. */
		{
			char ip[MAXP];
			NSImage *icon = nil;
			if (splash_image_path(root, "igmt.icns", ip, sizeof(ip)) ||
			    splash_image_path(root, "app_icon.png", ip, sizeof(ip)))
				icon = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:ip]];
			if (icon) {
				NSImageView *iv = [[NSImageView alloc]
				    initWithFrame:NSMakeRect(0, h - SPLASH_ICON_PX, SPLASH_ICON_PX, SPLASH_ICON_PX)];
				[iv setImage:icon];
				[iv setImageScaling:NSImageScaleProportionallyUpOrDown];
				[v addSubview:iv];
			}
		}

		lab = [[NSTextField alloc] initWithFrame:NSMakeRect(0, h / 2.0 - h / 18.0, w, h / 9.0)];
		/* Written as UTF-8 bytes, not a literal "…", so the string cannot depend on how the
		 * compiler decodes this source file — the mistake that produced "startingâ€¦" on Windows. */
		[lab setStringValue:[NSString stringWithUTF8String:"Starting iGMT\xe2\x80\xa6"]];
		[lab setBezeled:NO];
		[lab setDrawsBackground:NO];
		[lab setEditable:NO];
		[lab setSelectable:NO];
		[lab setAlignment:NSTextAlignmentCenter];
		[lab setTextColor:[NSColor colorWithCalibratedRed:0.91 green:0.925 blue:0.96 alpha:1.0]];
		[lab setFont:[NSFont systemFontOfSize:h / 14.0 weight:NSFontWeightSemibold]];
		{	/* The dark halo the .hta got from text-shadow — the nebula's middle is bright. */
			NSShadow *sh2 = [[NSShadow alloc] init];
			[sh2 setShadowColor:[NSColor blackColor]];
			[sh2 setShadowBlurRadius:3.0];
			[sh2 setShadowOffset:NSMakeSize(1, -2)];
			[lab setWantsLayer:YES];
			[lab setShadow:sh2];
		}
		[v addSubview:lab];

		/* Indeterminate progress bar, built from two plain layer-backed views so it animates
		 * exactly like the Win32 and X11 ones instead of looking like an Aqua spinner. */
		{
			int bx, by, bw, bh, cw;
			splash_bar_rect(w, h, &bx, &by, &bw, &bh, &cw);
			track = [[NSView alloc] initWithFrame:NSMakeRect(bx, h - by - bh, bw, bh)];
			[track setWantsLayer:YES];
			track.layer.backgroundColor = [[NSColor colorWithCalibratedWhite:0.2 alpha:1.0] CGColor];
			track.layer.masksToBounds = YES;      /* clips the chunk at the track's ends */
			chunk = [[NSView alloc] initWithFrame:NSMakeRect(-cw, 0, cw, bh)];
			[chunk setWantsLayer:YES];
			chunk.layer.backgroundColor =
			    [[NSColor colorWithCalibratedRed:0.357 green:0.608 blue:0.835 alpha:1.0] CGColor];
			[track addSubview:chunk];
			[v addSubview:track];
			barw = bw;
			chunkw = cw;
		}

		[win orderFrontRegardless];

		t0 = time(NULL);
		while (!file_exists(g_flag) && time(NULL) - t0 < 180) {
			NSEvent *e;
			/* Chunk position from the shared clock, in the track's own coordinates. */
			[chunk setFrameOrigin:NSMakePoint(splash_chunk_x(0, barw, chunkw, now_ms()), 0)];
			while ((e = [app nextEventMatchingMask:NSEventMaskAny
			                             untilDate:[NSDate dateWithTimeIntervalSinceNow:0.04]
			                                inMode:NSDefaultRunLoopMode
			                               dequeue:YES]))
				[app sendEvent:e];
		}
		[win close];
	}
}

#else

/* X11. Xlib decodes no image format at all, so the splash picture also ships as an
 * uncompressed 24-bit .bmp — parsed here in a few dozen lines, which is a far better trade than
 * making the launcher depend on libjpeg/libpng being present and loadable. */
static unsigned char *load_bmp(const char *path, int *ow, int *oh)
{
	FILE *f = fopen(path, "rb");
	unsigned char hdr[54], *row, *out;
	unsigned int offbits, bpp, comp;
	int w, h, flip, stride, yy, xx;

	if (!f) return NULL;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || hdr[0] != 'B' || hdr[1] != 'M') {
		fclose(f);
		return NULL;
	}
	offbits = (unsigned)hdr[10] | ((unsigned)hdr[11] << 8) | ((unsigned)hdr[12] << 16) | ((unsigned)hdr[13] << 24);
	w = (int)((unsigned)hdr[18] | ((unsigned)hdr[19] << 8) | ((unsigned)hdr[20] << 16) | ((unsigned)hdr[21] << 24));
	h = (int)((unsigned)hdr[22] | ((unsigned)hdr[23] << 8) | ((unsigned)hdr[24] << 16) | ((unsigned)hdr[25] << 24));
	bpp = (unsigned)hdr[28] | ((unsigned)hdr[29] << 8);
	comp = (unsigned)hdr[30] | ((unsigned)hdr[31] << 8);
	flip = 1;                       /* BMP rows run bottom-up unless height is negative */
	if (h < 0) { h = -h; flip = 0; }
	if (comp != 0 || (bpp != 24 && bpp != 32) || w <= 0 || h <= 0 || w > 20000 || h > 20000) {
		fclose(f);
		return NULL;
	}

	stride = ((w * (int)bpp / 8) + 3) & ~3;
	row = (unsigned char *)malloc((size_t)stride);
	out = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
	if (!row || !out) { free(row); free(out); fclose(f); return NULL; }
	fseek(f, (long)offbits, SEEK_SET);
	for (yy = 0; yy < h; yy++) {
		int dy = flip ? (h - 1 - yy) : yy;
		unsigned char *d = out + (size_t)dy * (size_t)w * 4;
		if (fread(row, 1, (size_t)stride, f) != (size_t)stride) break;
		for (xx = 0; xx < w; xx++) {
			const unsigned char *s = row + (size_t)xx * (bpp / 8);
			d[xx * 4 + 0] = s[2];   /* BMP stores BGR(A) */
			d[xx * 4 + 1] = s[1];
			d[xx * 4 + 2] = s[0];
			d[xx * 4 + 3] = (bpp == 32) ? s[3] : 255;   /* the icon needs its alpha kept */
		}
	}
	free(row);
	fclose(f);
	*ow = w;
	*oh = h;
	return out;
}

/* "cover": crop the source to the window's aspect ratio, then nearest-neighbour scale it to
 * fill — the same framing the Win32 Render call and Cocoa's ResizeAspectFill produce. */
static unsigned int *cover_scale(const unsigned char *src, int sw, int sh, int dw, int dh)
{
	unsigned int *dst = (unsigned int *)malloc((size_t)dw * (size_t)dh * 4);
	double wa = (double)dw / (double)dh, ia = (double)sw / (double)sh;
	int cx = 0, cy = 0, cw = sw, ch = sh, y, x;
	if (!dst) return NULL;
	if (ia > wa) { cw = (int)(sh * wa); cx = (sw - cw) / 2; }
	else         { ch = (int)(sw / wa); cy = (sh - ch) / 2; }
	for (y = 0; y < dh; y++) {
		int sy = cy + (int)((double)y * ch / dh);
		for (x = 0; x < dw; x++) {
			int sx = cx + (int)((double)x * cw / dw);
			const unsigned char *p = src + ((size_t)sy * sw + sx) * 4;
			dst[(size_t)y * dw + x] = ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
		}
	}
	return dst;
}

/* The iGMT icon, alpha-composited into the already-scaled background at the upper-left corner.
 * It ships as a 32-bit BGRA .bmp at exactly SPLASH_ICON_PX so there is no resampling to do here
 * — and no way for the icon to be silently dropped, which is what this whole path exists for. */
static void blit_icon(unsigned int *dst, int dw, int dh, const unsigned char *icon, int iw, int ih)
{
	int y, x;
	for (y = 0; y < ih && y < dh; y++) {
		for (x = 0; x < iw && x < dw; x++) {
			const unsigned char *s = icon + ((size_t)y * iw + x) * 4;
			unsigned int b = dst[(size_t)y * dw + x];
			unsigned a = s[3];
			unsigned br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
			unsigned r = (s[0] * a + br * (255 - a)) / 255;
			unsigned g = (s[1] * a + bg * (255 - a)) / 255;
			unsigned bl = (s[2] * a + bb * (255 - a)) / 255;
			dst[(size_t)y * dw + x] = (r << 16) | (g << 8) | bl;
		}
	}
}

static void splash_run(const char *root)
{
	char img[MAXP];
	Display *dpy;
	Window win;
	GC gc;
	XImage *xi = NULL;
	XSetWindowAttributes swa;
	XFontStruct *font = NULL;
	unsigned int *px = NULL;
	unsigned char *bmp = NULL;
	int scr, x, y, w, h, bw = 0, bh = 0, depth;
	int bx = 0, by = 0, bw2 = 0, bh2 = 0, cw = 0;
	time_t t0;
	static const char *fonts[] = {
		"-*-helvetica-bold-r-*--24-*", "-*-dejavu sans-bold-r-*--24-*", "9x15bold", "fixed", NULL
	};
	static const char *caption = "Starting iGMT...";   /* X11 core fonts are Latin-1: no U+2026 */
	int i, tw = 0;

	if (!(dpy = XOpenDisplay(NULL))) return;      /* no display (ssh, headless): no splash, no error */
	scr = DefaultScreen(dpy);
	depth = DefaultDepth(dpy, scr);
	splash_geometry(DisplayWidth(dpy, scr), DisplayHeight(dpy, scr), &x, &y, &w, &h);

	if (splash_image_path(root, "crystalball.bmp", img, sizeof(img)))
		bmp = load_bmp(img, &bw, &bh);
	if (bmp && depth >= 24) {
		px = cover_scale(bmp, bw, bh, w, h);
		/* The iGMT icon goes into the SAME buffer, so it is part of every redraw and cannot be
		 * lost to a missed expose. */
		if (px && splash_image_path(root, "igmt_splash.bmp", img, sizeof(img))) {
			int iw = 0, ih = 0;
			unsigned char *ic = load_bmp(img, &iw, &ih);
			if (ic) blit_icon(px, w, h, ic, iw, ih);
			free(ic);
		}
		if (px)
			xi = XCreateImage(dpy, DefaultVisual(dpy, scr), (unsigned)depth, ZPixmap, 0,
			                  (char *)px, (unsigned)w, (unsigned)h, 32, 0);
	}
	free(bmp);

	/* override_redirect: no title bar, no window-manager decoration or placement — a splash,
	 * not a window the user has to deal with. */
	memset(&swa, 0, sizeof(swa));
	swa.override_redirect = True;
	swa.background_pixel = 0x0a0d18;
	swa.event_mask = ExposureMask;
	win = XCreateWindow(dpy, RootWindow(dpy, scr), x, y, (unsigned)w, (unsigned)h, 0,
	                    CopyFromParent, InputOutput, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
	XStoreName(dpy, win, "iGMT");
	XMapRaised(dpy, win);

	gc = XCreateGC(dpy, win, 0, NULL);
	for (i = 0; fonts[i] && !font; i++) font = XLoadQueryFont(dpy, fonts[i]);
	if (font) {
		XSetFont(dpy, gc, font->fid);
		tw = XTextWidth(font, caption, (int)strlen(caption));
	}

	splash_bar_rect(w, h, &bx, &by, &bw2, &bh2, &cw);

	t0 = time(NULL);
	for (;;) {
		int redraw = 0;
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (ev.type == Expose) redraw = 1;
		}
		if (redraw) {
			/* Background + icon in one blit (they share the buffer), then the caption with the
			 * halo the .hta drew with text-shadow, then the 1px frame. */
			if (xi) XPutImage(dpy, win, gc, xi, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
			if (font) {
				/* One soft drop shadow, same as the Win32 path — a ring of eight only thickened
				 * the glyphs and read as a blur. */
				int tx = (w - tw) / 2, ty = h / 2 + font->ascent / 2;
				XSetForeground(dpy, gc, 0x000000);
				XDrawString(dpy, win, gc, tx + 1, ty + 2, caption, (int)strlen(caption));
				XSetForeground(dpy, gc, 0xf0f3fa);
				XDrawString(dpy, win, gc, tx, ty, caption, (int)strlen(caption));
			}
			XSetForeground(dpy, gc, 0x444444);
			XDrawRectangle(dpy, win, gc, 0, 0, (unsigned)(w - 1), (unsigned)(h - 1));
		}

		/* Progress bar, repainted every frame: dark track, lighter chunk sweeping across it,
		 * clipped to the track exactly as the .hta's `overflow:hidden` did. */
		{
			int cx = splash_chunk_x(bx, bw2, cw, now_ms());
			int cl = cx < bx ? bx : cx;
			int cr = (cx + cw) > (bx + bw2) ? (bx + bw2) : (cx + cw);
			XSetForeground(dpy, gc, 0x333333);
			XFillRectangle(dpy, win, gc, bx, by, (unsigned)bw2, (unsigned)bh2);
			if (cr > cl) {
				XSetForeground(dpy, gc, 0x5b9bd5);
				XFillRectangle(dpy, win, gc, cl, by, (unsigned)(cr - cl), (unsigned)bh2);
			}
		}
		XFlush(dpy);

		if (file_exists(g_flag) || time(NULL) - t0 >= 180) break;
		usleep(40000);
	}

	if (xi) XDestroyImage(xi);            /* frees px too */
	else free(px);
	if (font) XFreeFont(dpy, font);
	XFreeGC(dpy, gc);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
}
#endif

/* --------------------------------------------------------------------------------- spawning */

/* --project=<root> ONLY when <root> has its OWN Manifest.toml (a `] dev`-instantiated checkout).
 * A `] add` copy in the depot has no Manifest of its own — its resolved dependency graph lives
 * in the DEFAULT environment where `] add` put it. Pointing --project at a Manifest-less package
 * folder forces Julia to re-resolve and precompile the whole tree from scratch, which fails. */
static int wants_project(const char *root)
{
	char p[MAXP];
	joinp(p, sizeof(p), root, "Manifest.toml");
	return file_exists(p);
}

#ifdef _WIN32
static void cmd_add(char *cmd, size_t n, const char *arg)
{
	size_t l = strlen(cmd);
	snprintf(cmd + l, n - l, "%s\"%s\"", l ? " " : "", arg);
}

static int spawn_julia(const char *julia, const char *root, int nfiles, char **files)
{
	char cmd[32768], script[MAXP], proj[MAXP];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	wchar_t *wcmd, *wdir;
	int i;
	BOOL ok;

	cmd[0] = 0;
	cmd_add(cmd, sizeof(cmd), julia);
	strncat(cmd, " -t auto", sizeof(cmd) - strlen(cmd) - 1);
	if (wants_project(root)) {
		snprintf(proj, sizeof(proj), "--project=%s", root);
		cmd_add(cmd, sizeof(cmd), proj);
	}
	joinp(script, sizeof(script), root, "iview_app.jl");
	cmd_add(cmd, sizeof(cmd), script);
	for (i = 0; i < nfiles; i++) cmd_add(cmd, sizeof(cmd), files[i]);

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	/* MINIMIZED, never hidden: SW_HIDE in STARTUPINFO is inherited by the Qt window, which then
	 * never appears. iview_app.jl hides its own console on startup, so only the viewer remains. */
	si.wShowWindow = SW_SHOWMINNOACTIVE;

	wcmd = wide(cmd);
	wdir = wide(root);
	ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, wdir, &si, &pi);
	free(wcmd);
	free(wdir);
	if (!ok) return -1;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0;
}
#else
static int spawn_julia(const char *julia, const char *root, int nfiles, char **files)
{
	char script[MAXP], proj[MAXP];
	char **argv;
	int n = 0, i;
	pid_t pid;

	joinp(script, sizeof(script), root, "iview_app.jl");
	argv = (char **)calloc((size_t)nfiles + 8, sizeof(char *));
	argv[n++] = (char *)julia;
	argv[n++] = (char *)"-t";
	argv[n++] = (char *)"auto";
	if (wants_project(root)) {
		snprintf(proj, sizeof(proj), "--project=%s", root);
		argv[n++] = proj;
	}
	argv[n++] = script;
	for (i = 0; i < nfiles; i++) argv[n++] = files[i];
	argv[n] = NULL;

	pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		/* Detach fully: a new session, no controlling terminal, stdio to /dev/null, so the
		 * viewer outlives the launcher (and the Files/Finder process that started it). */
		int devnull;
		setsid();
		if (fork() != 0) _exit(0);
		devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, 0);
			dup2(devnull, 1);
			dup2(devnull, 2);
			if (devnull > 2) close(devnull);
		}
		if (chdir(root) != 0) { /* keep going: cwd is a convenience, not a requirement */ }
		execv(julia, argv);
		_exit(127);
	}
	{
		int status;
		waitpid(pid, &status, 0);   /* reap the short-lived intermediate, not the viewer */
	}
	free(argv);
	return 0;
}
#endif

/* -------------------------------------------------------------------- desktop entry creation */

/* Where the desktop entry points. The launcher LIVES IN THE PACKAGE ROOT, beside iview_app.jl and
 * igmt.ico — so this copies itself there from wherever it was built or unpacked (a release zip
 * extracts into <depot>/gmtvtk_runtime/deps/build) and returns that path.
 *
 * NOTHING is written to <home>/.gmt except the ini: that directory is for settings (iGMT.ini), not
 * for binaries or icon copies.
 *
 * If the copy cannot be made — an installed package tree may be read-only — the entry points at the
 * binary where it already is. Both locations are stable across a Pkg.update: gmtvtk_runtime is a
 * fixed path, and a dev checkout does not move. */
static const char *install_stable_copy(const char *root)
{
	static char dst[MAXP];
	char self[MAXP];
	self_path(self, sizeof(self));
	joinp(dst, sizeof(dst), root, "igmt" EXESUF);
	if (strcmp(self, dst) == 0) return dst;
#ifdef _WIN32
	{
		/* A previous copy may be running (the user's own splash), which would fail the overwrite
		 * with a share violation. Move it aside first; Windows drops it on the next boot. */
		char old[MAXP];
		joinp(old, sizeof(old), root, "igmt.old.exe");
		if (file_exists(dst)) {
			wchar_t *wd = wide(dst), *wo = wide(old);
			DeleteFileW(wo);
			if (MoveFileExW(wd, wo, MOVEFILE_REPLACE_EXISTING))
				MoveFileExW(wo, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
			free(wd); free(wo);
		}
	}
#endif
	if (copy_file(self, dst, 1) != 0) {
		snprintf(dst, sizeof(dst), "%s", self);   /* read-only tree: run it where it lies */
	}
	return dst;
}

/* The icon, used STRAIGHT FROM THE PACKAGE — never copied anywhere. igmt.ico already sits in the
 * package root, and deps/assets carries the .icns and the PNG. */
static int install_icon(const char *root, const char *leaf, char *out, size_t n)
{
	joinp(out, n, root, leaf);
	if (file_exists(out)) return 1;
	snprintf(out, n, "%s%cdeps%cassets%c%s", root, PATHSEP, PATHSEP, PATHSEP, leaf);
	if (file_exists(out)) return 1;
	snprintf(out, n, "%s%cdeps%cassets%capp_icon.png", root, PATHSEP, PATHSEP, PATHSEP);
	return file_exists(out);
}

#ifdef _WIN32
/* The ACTUAL desktop as the shell resolves it for this user. A hardcoded %USERPROFILE%\Desktop
 * is wrong: with Desktop redirected (OneDrive does exactly this), that folder exists but is not
 * the one displayed, so the icon lands where nobody can see it. */
static int desktop_dir(char *out, size_t n)
{
	wchar_t *p = NULL;
	if (FAILED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, NULL, &p))) return 0;
	{
		char *s = utf8(p);
		snprintf(out, n, "%s", s);
		free(s);
	}
	CoTaskMemFree(p);
	return 1;
}

static void delete_at(const char *dir, const char *leaf)
{
	char p[MAXP];
	wchar_t *w;
	joinp(p, sizeof(p), dir, leaf);
	w = wide(p);
	DeleteFileW(w);
	free(w);
}

static int install_shortcut(const char *root, const char *julia)
{
	char desk[MAXP], lnk[MAXP], icon[MAXP];
	const char *target = install_stable_copy(root);
	IShellLinkW *sl = NULL;
	IPersistFile *pf = NULL;
	HRESULT hr;
	wchar_t *w;

	ini_write(root, julia);
	if (!install_icon(root, "igmt.ico", icon, sizeof(icon))) icon[0] = 0;
	if (!desktop_dir(desk, sizeof(desk))) return -1;

	/* Housekeeping from the retired .vbs era: stray copies it once dropped here, and the old
	 * pre-rename icon name, so a refresh never leaves two icons or loose files behind. */
	delete_at(desk, "iview_app.vbs");
	delete_at(desk, "igmt.ico");
	delete_at(desk, "iGMT.lnk");

	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl);
	if (FAILED(hr)) { CoUninitialize(); return -1; }

	w = wide(target);   sl->lpVtbl->SetPath(sl, w);              free(w);
	w = wide(root);     sl->lpVtbl->SetWorkingDirectory(sl, w);  free(w);
	sl->lpVtbl->SetArguments(sl, L"");   /* reset: Explorer writes a dropped path in here */
	sl->lpVtbl->SetDescription(sl, L"iGMT — interactive GMT viewer");
	sl->lpVtbl->SetShowCmd(sl, SW_SHOWMINNOACTIVE);
	if (icon[0]) { w = wide(icon); sl->lpVtbl->SetIconLocation(sl, w, 0); free(w); }

	hr = sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf);
	if (SUCCEEDED(hr)) {
		joinp(lnk, sizeof(lnk), desk, "i'GMT.lnk");
		w = wide(lnk);
		hr = pf->lpVtbl->Save(pf, w, TRUE);
		free(w);
		pf->lpVtbl->Release(pf);
	}
	sl->lpVtbl->Release(sl);
	CoUninitialize();
	return SUCCEEDED(hr) ? 0 : -1;
}

#elif defined(__APPLE__)
/* A .app bundle is just a directory with a plist and an executable inside — no Xcode, no
 * signing needed for a locally-created one. Lives in ~/Applications (per-user, no sudo) with a
 * symlink on the Desktop so it shows as an icon there too. */
static int install_shortcut(const char *root, const char *julia)
{
	char app[MAXP], macos[MAXP], res[MAXP], plist[MAXP], exe[MAXP], icon[MAXP], desk[MAXP];
	const char *stable = install_stable_copy(root);
	FILE *f;
	int have_icns;

	ini_write(root, julia);
	snprintf(app, sizeof(app), "%s/Applications/iGMT.app", home_dir());
	snprintf(macos, sizeof(macos), "%s/Contents/MacOS", app);
	snprintf(res, sizeof(res), "%s/Contents/Resources", app);
	make_dirs(macos);
	make_dirs(res);

	snprintf(exe, sizeof(exe), "%s/igmt", macos);
	if (copy_file(stable, exe, 1) != 0) return -1;

	have_icns = install_icon(root, "igmt.icns", icon, sizeof(icon));
	if (have_icns && strstr(icon, ".icns")) {
		char dst[MAXP];
		snprintf(dst, sizeof(dst), "%s/igmt.icns", res);
		copy_file(icon, dst, 0);
	}
	else {
		have_icns = 0;
	}

	snprintf(plist, sizeof(plist), "%s/Contents/Info.plist", app);
	if (!(f = fopen(plist, "w"))) return -1;
	fprintf(f,
	        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	        "<plist version=\"1.0\"><dict>\n"
	        "  <key>CFBundleName</key><string>iGMT</string>\n"
	        "  <key>CFBundleDisplayName</key><string>iGMT</string>\n"
	        "  <key>CFBundleIdentifier</key><string>org.generic-mapping-tools.igmt</string>\n"
	        "  <key>CFBundleVersion</key><string>0.1.0</string>\n"
	        "  <key>CFBundleShortVersionString</key><string>0.1.0</string>\n"
	        "  <key>CFBundlePackageType</key><string>APPL</string>\n"
	        "  <key>CFBundleExecutable</key><string>igmt</string>\n"
	        "%s"
	        "  <key>NSHighResolutionCapable</key><true/>\n"
	        "</dict></plist>\n",
	        have_icns ? "  <key>CFBundleIconFile</key><string>igmt.icns</string>\n" : "");
	fclose(f);

	/* Freshly written bundles are not re-read until LaunchServices notices; the symlink is what
	 * puts it on the Desktop. Both are best-effort — the app in ~/Applications is the real thing. */
	snprintf(desk, sizeof(desk), "%s/Desktop/iGMT.app", home_dir());
	unlink(desk);
	if (symlink(app, desk) != 0) { /* no Desktop folder — not an error */ }
	return 0;
}

#else
/* Linux: a .desktop file in the per-user applications dir (so it appears in the menu/launcher)
 * plus a copy on the Desktop itself, marked executable — GNOME/KDE both require that bit before
 * they will run a desktop file dropped there. */
static int write_desktop_file(const char *path, const char *exe, const char *icon)
{
	FILE *f = fopen(path, "w");
	if (!f) return -1;
	fprintf(f,
	        "[Desktop Entry]\n"
	        "Type=Application\n"
	        "Version=1.0\n"
	        "Name=iGMT\n"
	        "GenericName=Interactive GMT viewer\n"
	        "Comment=3-D viewer for GMT grids, images, tables and meshes\n"
	        "Exec=\"%s\" %%F\n"
	        "Icon=%s\n"
	        "Terminal=false\n"
	        "StartupNotify=true\n"
	        "Categories=Science;Geoscience;Education;\n"
	        "MimeType=application/x-netcdf;image/tiff;text/plain;\n",
	        exe, icon);
	fclose(f);
	chmod(path, 0755);
	return 0;
}

static int install_shortcut(const char *root, const char *julia)
{
	char apps[MAXP], entry[MAXP], icon[MAXP], desk[MAXP];
	const char *stable = install_stable_copy(root);

	ini_write(root, julia);
	if (!install_icon(root, "igmt.png", icon, sizeof(icon))) snprintf(icon, sizeof(icon), "applications-science");

	snprintf(apps, sizeof(apps), "%s/.local/share/applications", home_dir());
	make_dirs(apps);
	snprintf(entry, sizeof(entry), "%s/iGMT.desktop", apps);
	if (write_desktop_file(entry, stable, icon) != 0) return -1;

	snprintf(desk, sizeof(desk), "%s/Desktop", home_dir());
	if (dir_exists(desk)) {
		char d2[MAXP];
		snprintf(d2, sizeof(d2), "%s/iGMT.desktop", desk);
		write_desktop_file(d2, stable, icon);
	}
	return 0;
}
#endif

/* ------------------------------------------------------------------------------------- main */

static int run(int argc, char **argv)
{
	char root[MAXP], julia[MAXP];
	const char *hint_root = NULL, *hint_julia = NULL;
	char **files;
	int nfiles = 0, install = 0, i;

	files = (char **)calloc((size_t)argc + 1, sizeof(char *));
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--install-shortcut") == 0)        install = 1;
		else if (strncmp(argv[i], "--root=", 7) == 0)          hint_root = argv[i] + 7;
		else if (strncmp(argv[i], "--julia=", 8) == 0)         hint_julia = argv[i] + 8;
		else                                                   files[nfiles++] = argv[i];
	}

	if (!find_root(root, sizeof(root), hint_root)) {
		message_box("iGMT",
		            "Could not locate InteractiveGMT.\n\n"
		            "Checked the hint passed on the command line, ~/.gmt/igmt_launcher.ini, this "
		            "program's own folder, <depot>/dev/InteractiveGMT and "
		            "<depot>/packages/InteractiveGMT/*.\n\n"
		            "Is it added ('] add https://github.com/GenericMappingTools/InteractiveGMT') "
		            "or dev'd in Julia's default environment?");
		return 1;
	}

	if (install) {
		int rc;
		if (!find_julia(julia, sizeof(julia), hint_julia)) julia[0] = 0;
		rc = install_shortcut(root, julia);
		if (rc != 0) message_box("iGMT", "Could not create the desktop shortcut.");
		return rc == 0 ? 0 : 1;
	}

	if (!find_julia(julia, sizeof(julia), hint_julia)) {
		message_box("iGMT",
		            "Could not find julia.\n\n"
		            "Checked ~/.gmt/igmt_launcher.ini, $JULIA_EXE, every directory on PATH, the "
		            "juliaup shim and the usual install roots.\n\n"
		            "Install Julia, put its bin folder on PATH, or set JULIA_EXE to the full path "
		            "of the julia executable.");
		return 1;
	}

	/* Clear a stale ready-flag BEFORE launching, or the new splash sees the previous run's flag
	 * and closes instantly. */
	ready_flag_path(g_flag, sizeof(g_flag));
#ifdef _WIN32
	{
		wchar_t *w = wide(g_flag);
		DeleteFileW(w);
		free(w);
	}
#else
	unlink(g_flag);
#endif

	if (spawn_julia(julia, root, nfiles, files) != 0) {
		message_box("iGMT", "Could not start julia.");
		return 1;
	}

	splash_run(root);   /* returns when iview_app.jl signals its window is up */
	free(files);
	return 0;
}

#ifdef _WIN32
/* WIN32 subsystem: no console window flashes on a double-click. argv comes from the wide
 * command line, converted to UTF-8 so the rest of this file stays platform-neutral. */
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
	int argc = 0, i, rc;
	LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
	char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
	(void)inst; (void)prev; (void)cmdline; (void)show;
	for (i = 0; i < argc; i++) argv[i] = utf8(wargv[i]);
	LocalFree(wargv);
	rc = run(argc, argv);
	return rc;
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
