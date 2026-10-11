/*======================================================================================
 * mbna_mbio.c -- the MBIO entry points the mbnavadjust port needs beyond the swath editor's,
 * picked out of the library the swath editor ALREADY loaded (mbedit_mbio_open, deps/src/mbedit/),
 * as the navigation editor does (deps/src/mbnavedit/mbnavedit.c, mbnavedit_mbio_open).
 *====================================================================================*/

/* Dl_info / dladdr are GNU extensions: _GNU_SOURCE must precede the FIRST system header */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#	define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <windows.h>
#else
#	include <dlfcn.h>
#endif

#include "mbnavadjust_mbio.h"

struct mbna_mbio_table mbna_mbio;
static bool mbna_mbio_ok = false;

/* The module that holds the swath editor's MBIO table: the library mbedit_mbio_open loaded. */
#ifdef _WIN32
static void *mbna_mbio_module(char *path, int pathlen) {
	HMODULE h = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)(void *)mbedit_mbio.get_all, &h))
		return NULL;
	wchar_t wpath[MB_PATH_MAXLINE];
	const DWORD n = GetModuleFileNameW(h, wpath, MB_PATH_MAXLINE);
	if (n == 0 || n >= MB_PATH_MAXLINE || WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, pathlen, NULL, NULL) == 0)
		path[0] = '\0';
	return (void *)h;
}
static void *mbna_dlsym(void *h, const char *name) {
	return (void *)GetProcAddress((HMODULE)h, name);
}
#else
static void *mbna_mbio_module(char *path, int pathlen) {
	Dl_info info;
	if (dladdr((void *)mbedit_mbio.get_all, &info) == 0 || info.dli_fname == NULL)
		return NULL;
	snprintf(path, pathlen, "%s", info.dli_fname);
	/* the library is already loaded: this only hands back its handle */
	return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
}
static void *mbna_dlsym(void *h, const char *name) {
	return dlsym(h, name);
}
#endif

int mbna_mbio_open(char *msg, int msglen) {
	if (msg && msglen > 0)
		msg[0] = '\0';
	if (mbna_mbio_ok)
		return 1;
	if (!mbedit_mbio_loaded()) {
		snprintf(msg, msglen, "The MB-System MBIO library is not loaded");
		return 0;
	}
	char libpath[MB_PATH_MAXLINE] = "";
	void *h = mbna_mbio_module(libpath, (int)sizeof(libpath));
	if (h == NULL) {
		snprintf(msg, msglen, "Cannot find the module of the loaded MBIO library");
		return 0;
	}

	struct mbna_mbio_table t;
	memset(&t, 0, sizeof(t));
	struct {
		void **slot;
		const char *name;
	} syms[] = {
		{(void **)&t.user_host_date, "mb_user_host_date"},
		{(void **)&t.coor_scale, "mb_coor_scale"},
		{(void **)&t.get_date, "mb_get_date"},
		{(void **)&t.make_info, "mb_make_info"},
		{(void **)&t.write_init, "mb_write_init"},
		{(void **)&t.put_all, "mb_put_all"},
		{(void **)&t.get_store, "mb_get_store"},
		{(void **)&t.beamwidths, "mb_beamwidths"},
		{(void **)&t.sonartype, "mb_sonartype"},
		{(void **)&t.pr_update_navadj, "mb_pr_update_navadj"},
		{(void **)&t.pr_get_rollbias, "mb_pr_get_rollbias"},
		{(void **)&t.pr_get_heading, "mb_pr_get_heading"},
		{(void **)&t.pr_update_rollbias, "mb_pr_update_rollbias"},
		{(void **)&t.pr_update_heading, "mb_pr_update_heading"},
		{(void **)&t.pr_get_output, "mb_pr_get_output"},
		{(void **)&t.pr_get_ofile, "mb_pr_get_ofile"},
		{(void **)&t.mergesort, "mb_mergesort"},
		{(void **)&t.get_relative_path, "mb_get_relative_path"},
		{(void **)&t.copyfile, "mb_copyfile"},
		{(void **)&t.catfiles, "mb_catfiles"},
		{(void **)&t.datalist_read, "mb_datalist_read"},
		{(void **)&t.mallocd, "mb_mallocd"},
		{(void **)&t.reallocd, "mb_reallocd"},
		{(void **)&t.freed, "mb_freed"},
	};
	for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
		*syms[i].slot = mbna_dlsym(h, syms[i].name);
		if (*syms[i].slot == NULL) {
			snprintf(msg, msglen, "%s does not export %s, which mbnavadjust needs", libpath, syms[i].name);
			return 0;
		}
	}
	mbna_mbio = t;
	mbna_mbio_ok = true;
	return 1;
}
