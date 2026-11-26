// crashreport.cpp -- what a crash on a USER'S machine leaves behind (Windows). Own translation unit:
// it needs <windows.h>, which cannot meet the viewer's single TU (gmtvtk.cpp).
//
// Three things, all written into ONE session folder the host names (src/crashreport.jl):
//
//   trail.txt  the last TRAIL_SLOTS user actions (clicks, menus, dialogs, Qt errors), a ring in a
//              MEMORY-MAPPED file. Every write is a memcpy into the page cache, and the system writes
//              those pages to disk even when the process dies a death no handler sees (heap
//              corruption, __fastfail): the trail survives every kind of crash.
//   stack.txt  the faulting thread's native stack, module+offset per frame (user machines have no
//              PDBs; offsets are matched to the release DLL's PDB here), plus the module list.
//   crash.dmp  a minidump of the process at the fault.
//
// The catcher is a FIRST vectored handler: it runs before Julia's own (which prints a banner and calls
// jl_exit(1) on a Julia thread, so a filter behind it never runs) and on threads Julia does not know
// (Qt, VTK, TBB, GDAL workers). A first-chance exception may still be handled further on, so this
// writes once per process and DECIDES NOTHING: the host keeps the folder only if the process then exits
// with a non-zero code. Faults in Julia's own code and JIT frames are Julia's business (GC safepoints
// raise access violations ON PURPOSE) and are skipped. Cost while nothing crashes: none.

#include <cstdio>
#include <cstring>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <winevt.h>

static wchar_t g_dir[MAX_PATH] = L"";

// --------------------------------------------------------------------------------------- trail
static const int  TRAIL_SLOTS = 200;
static const int  TRAIL_SLOT  = 256;           // bytes per line, '\n' included
static char         *g_trail = nullptr;
static volatile LONG g_trailSeq = 0;

static void wpath(wchar_t *out, size_t cap, const wchar_t *name) {
	_snwprintf(out, cap, L"%ls\\%ls", g_dir, name);
	out[cap - 1] = 0;
}

static bool trailOpen() {
	wchar_t p[MAX_PATH];
	wpath(p, MAX_PATH, L"trail.txt");
	HANDLE f = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	const DWORD size = TRAIL_SLOTS * TRAIL_SLOT;
	HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, size, nullptr);
	CloseHandle(f);                            // the mapping keeps the file
	if (!m) return false;
	g_trail = static_cast<char *>(MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, size));
	CloseHandle(m);                            // the view keeps the mapping
	if (!g_trail) return false;
	for (int i = 0; i < TRAIL_SLOTS; ++i) {    // blank lines until written: the file reads as text
		std::memset(g_trail + i * TRAIL_SLOT, ' ', TRAIL_SLOT - 1);
		g_trail[i * TRAIL_SLOT + TRAIL_SLOT - 1] = '\n';
	}
	return true;
}

// One line: "<seq> <hh:mm:ss.mmm> <text>". The host sorts by seq. Any thread may call it.
extern "C" void igmt_trail_add(const char *text) {
	if (!g_trail || !text) return;
	const LONG seq = InterlockedIncrement(&g_trailSeq);
	SYSTEMTIME t;
	GetLocalTime(&t);
	char line[TRAIL_SLOT];
	int n = std::snprintf(line, sizeof(line), "%ld %02d:%02d:%02d.%03d %s", (long)seq, t.wHour, t.wMinute, t.wSecond,
	                      t.wMilliseconds, text);
	if (n < 0) return;
	if (n > TRAIL_SLOT - 1) n = TRAIL_SLOT - 1;
	for (int i = 0; i < n; ++i)
		if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
	char *slot = g_trail + ((seq - 1) % TRAIL_SLOTS) * TRAIL_SLOT;
	std::memset(slot, ' ', TRAIL_SLOT - 1);
	std::memcpy(slot, line, n);
}

// ---------------------------------------------------------------------------------- the catcher
using MiniDumpWriteDumpFn     = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                               PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
using SymSetOptionsFn         = DWORD(WINAPI *)(DWORD);
using SymInitializeFn         = BOOL(WINAPI *)(HANDLE, PCSTR, BOOL);
using StackWalk64Fn           = BOOL(WINAPI *)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                                               PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64,
                                               PTRANSLATE_ADDRESS_ROUTINE64);
using SymFromAddrFn           = BOOL(WINAPI *)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);

static const char *moduleOf(DWORD64 addr, char *buf, DWORD cap, DWORD64 *base) {
	HMODULE hm = nullptr;
	buf[0] = '\0';
	if (base) *base = 0;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCSTR>(addr), &hm))
		return nullptr;
	GetModuleFileNameA(hm, buf, cap);
	if (base) *base = reinterpret_cast<DWORD64>(hm);
	const char *b = std::strrchr(buf, '\\');
	return b ? b + 1 : buf;
}

// The PE header's link time stamp: names the exact build of a module, which is what matches a PDB.
static DWORD moduleStamp(DWORD64 base) {
	if (!base) return 0;
	auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
	auto *nt  = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	return nt->FileHeader.TimeDateStamp;
}

struct CrashJob {
	EXCEPTION_POINTERS *ep;
	DWORD               tid;
	HANDLE              thread;
};

static const char *codeName(DWORD c) {
	switch (c) {
		case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
		case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
		case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
		case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
		case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
		default:                              return "?";
	}
}

static BOOL CALLBACK listModule(PCSTR name, DWORD64 base, ULONG size, PVOID ctx) {
	std::fprintf(static_cast<FILE *>(ctx), "  %016llx %8lx %08lx %s\n", (unsigned long long)base, (unsigned long)size,
	             (unsigned long)moduleStamp(base), name);
	return TRUE;
}
using EnumLoadedModules64Fn = BOOL(WINAPI *)(HANDLE, PENUMLOADED_MODULES_CALLBACK64, PVOID);

// Runs on a FRESH thread: the faulting one may have no stack left (stack overflow), and dbghelp is
// not something to start on a thread that just faulted.
static DWORD WINAPI crashWrite(LPVOID arg) {
	CrashJob *job = static_cast<CrashJob *>(arg);
	EXCEPTION_RECORD *er = job->ep->ExceptionRecord;
	HMODULE dh = LoadLibraryW(L"dbghelp.dll");
	auto dump    = dh ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dh, "MiniDumpWriteDump")) : nullptr;
	auto symOpt  = dh ? reinterpret_cast<SymSetOptionsFn>(GetProcAddress(dh, "SymSetOptions")) : nullptr;
	auto symInit = dh ? reinterpret_cast<SymInitializeFn>(GetProcAddress(dh, "SymInitialize")) : nullptr;
	auto walk    = dh ? reinterpret_cast<StackWalk64Fn>(GetProcAddress(dh, "StackWalk64")) : nullptr;
	auto symAddr = dh ? reinterpret_cast<SymFromAddrFn>(GetProcAddress(dh, "SymFromAddr")) : nullptr;
	auto fnTable = dh ? reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE64>(GetProcAddress(dh, "SymFunctionTableAccess64")) : nullptr;
	auto modBase = dh ? reinterpret_cast<PGET_MODULE_BASE_ROUTINE64>(GetProcAddress(dh, "SymGetModuleBase64")) : nullptr;
	auto enumMod = dh ? reinterpret_cast<EnumLoadedModules64Fn>(GetProcAddress(dh, "EnumerateLoadedModules64")) : nullptr;
	HANDLE proc = GetCurrentProcess();

	wchar_t p[MAX_PATH];
	wpath(p, MAX_PATH, L"stack.txt");
	FILE *f = _wfopen(p, L"w");
	if (f) {
		char mod[MAX_PATH];
		DWORD64 base = 0;
		const char *fm = moduleOf(reinterpret_cast<DWORD64>(er->ExceptionAddress), mod, MAX_PATH, &base);
		std::fprintf(f, "exception %08lX %s in %s+0x%llx (thread %lu)\n", (unsigned long)er->ExceptionCode,
		             codeName(er->ExceptionCode), fm ? fm : "?",
		             (unsigned long long)(reinterpret_cast<DWORD64>(er->ExceptionAddress) - base), (unsigned long)job->tid);
		if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
			std::fprintf(f, "%s address 0x%llx\n", er->ExceptionInformation[0] == 0 ? "reading" :
			             er->ExceptionInformation[0] == 1 ? "writing" : "executing",
			             (unsigned long long)er->ExceptionInformation[1]);
		std::fprintf(f, "\nstack (module+offset, nearest exported symbol):\n");
		if (symOpt && symInit && walk && fnTable && modBase) {
			symOpt(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
			symInit(proc, nullptr, TRUE);
			CONTEXT ctx = *job->ep->ContextRecord;
			STACKFRAME64 sf = {};
			sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
			sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
			sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
			alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 256];
			for (int i = 0; i < 64; ++i) {
				if (!walk(IMAGE_FILE_MACHINE_AMD64, proc, job->thread, &sf, &ctx, nullptr, fnTable, modBase, nullptr) ||
				    !sf.AddrPC.Offset)
					break;
				const DWORD64 pc = sf.AddrPC.Offset;
				DWORD64 b = 0;
				const char *mb = moduleOf(pc, mod, MAX_PATH, &b);
				auto *si = reinterpret_cast<SYMBOL_INFO *>(symbuf);
				si->SizeOfStruct = sizeof(SYMBOL_INFO);
				si->MaxNameLen = 255;
				DWORD64 disp = 0;
				const bool sym = symAddr && symAddr(proc, pc, &disp, si);
				if (mb) std::fprintf(f, "  #%02d %s+0x%llx", i, mb, (unsigned long long)(pc - b));
				else    std::fprintf(f, "  #%02d (JIT code) 0x%llx", i, (unsigned long long)pc);
				if (sym) std::fprintf(f, "  %s+0x%llx", si->Name, (unsigned long long)disp);
				std::fprintf(f, "\n");
			}
		}
		else std::fprintf(f, "  (dbghelp.dll not available)\n");
		if (enumMod) {
			std::fprintf(f, "\nmodules (base, size, link stamp, path):\n");
			enumMod(proc, listModule, f);
		}
		std::fclose(f);
	}
	if (dump) {
		wpath(p, MAX_PATH, L"crash.dmp");
		HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			MINIDUMP_EXCEPTION_INFORMATION mei;
			mei.ThreadId = job->tid;
			mei.ExceptionPointers = job->ep;
			mei.ClientPointers = FALSE;
			dump(proc, GetCurrentProcessId(), h,
			     static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
			     &mei, nullptr, nullptr);
			CloseHandle(h);
		}
	}
	return 0;
}

static LONG CALLBACK crashCatch(EXCEPTION_POINTERS *ep) {
	if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
	switch (ep->ExceptionRecord->ExceptionCode) {
		case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
		case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			break;
		default:
			return EXCEPTION_CONTINUE_SEARCH;
	}
	char mod[MAX_PATH];
	const char *fm = moduleOf(reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress), mod, MAX_PATH, nullptr);
	// JIT code (no module) and Julia's runtime raise these on purpose: never a crash of ours to report
	if (!fm || _strnicmp(fm, "libjulia", 8) == 0 || _strnicmp(fm, "sys.", 4) == 0) return EXCEPTION_CONTINUE_SEARCH;
	static volatile LONG once = 0;
	if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
	char line[200];
	std::snprintf(line, sizeof(line), "*** %s in %s", codeName(ep->ExceptionRecord->ExceptionCode), fm);
	igmt_trail_add(line);
	CrashJob job;
	job.ep  = ep;
	job.tid = GetCurrentThreadId();
	job.thread = nullptr;
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &job.thread, 0, FALSE,
	                DUPLICATE_SAME_ACCESS);
	HANDLE t = CreateThread(nullptr, 1 << 20, crashWrite, &job, 0, nullptr);
	if (t) {
		WaitForSingleObject(t, 60000);
		CloseHandle(t);
	}
	if (job.thread) CloseHandle(job.thread);
	return EXCEPTION_CONTINUE_SEARCH;            // Julia / Windows carry on exactly as before
}

// Start reporting into `dir` (UTF-8; the folder exists). Returns 1 when the catcher is in place.
extern "C" __declspec(dllexport) int gmtvtk_crash_install(const char *dir) {
	static PVOID h = nullptr;
	if (h || !dir || !dir[0]) return h ? 1 : 0;
	if (!MultiByteToWideChar(CP_UTF8, 0, dir, -1, g_dir, MAX_PATH)) return 0;
	trailOpen();
	ULONG guarantee = 64 * 1024;              // stack kept back so the catcher can run after an overflow
	SetThreadStackGuarantee(&guarantee);
	h = AddVectoredExceptionHandler(1, crashCatch);
	return h ? 1 : 0;
}

extern "C" __declspec(dllexport) void gmtvtk_crash_trail(const char *text) {
	igmt_trail_add(text);
}

// ------------------------------------------------------------------------------- the event log
// The Windows Application log's own crash records ("Application Error" 1000, "Application Hang" 1002)
// for julia.exe over the last 30 days, newest first, as XML, one event per line. Windows writes one for
// EVERY kind of death, those no in-process handler can see included (heap corruption, __fastfail).
// Writes at most cap-1 bytes into out; returns the length.
using EvtQueryFn  = EVT_HANDLE(WINAPI *)(EVT_HANDLE, LPCWSTR, LPCWSTR, DWORD);
using EvtNextFn   = BOOL(WINAPI *)(EVT_HANDLE, DWORD, EVT_HANDLE *, DWORD, DWORD, PDWORD);
using EvtRenderFn = BOOL(WINAPI *)(EVT_HANDLE, EVT_HANDLE, DWORD, DWORD, PVOID, PDWORD, PDWORD);
using EvtCloseFn  = BOOL(WINAPI *)(EVT_HANDLE);

extern "C" __declspec(dllexport) int gmtvtk_crash_eventlog(char *out, int cap) {
	if (!out || cap <= 0) return 0;
	out[0] = 0;
	HMODULE w = LoadLibraryW(L"wevtapi.dll");
	if (!w) return 0;
	auto q   = reinterpret_cast<EvtQueryFn>(GetProcAddress(w, "EvtQuery"));
	auto nx  = reinterpret_cast<EvtNextFn>(GetProcAddress(w, "EvtNext"));
	auto rn  = reinterpret_cast<EvtRenderFn>(GetProcAddress(w, "EvtRender"));
	auto cl  = reinterpret_cast<EvtCloseFn>(GetProcAddress(w, "EvtClose"));
	int len = 0;
	if (q && nx && rn && cl) {
		EVT_HANDLE r = q(nullptr, L"Application",
		                 L"*[System[(EventID=1000 or EventID=1002) and TimeCreated[timediff(@SystemTime) <= 2592000000]]]",
		                 EvtQueryChannelPath | EvtQueryReverseDirection);
		if (r) {
			EVT_HANDLE ev[16];
			DWORD got = 0;
			int seen = 0;
			static wchar_t xml[32768], low[32768];
			static char u8[65536];
			while (seen < 200 && nx(r, 16, ev, 1000, 0, &got) && got) {
				for (DWORD i = 0; i < got; ++i, ++seen) {
					DWORD used = 0, props = 0;
					if (rn(nullptr, ev[i], EvtRenderEventXml, sizeof(xml), xml, &used, &props)) {
						wcsncpy(low, xml, 32767);
						low[32767] = 0;
						_wcslwr(low);
						if (wcsstr(low, L"julia.exe")) {
							int n = WideCharToMultiByte(CP_UTF8, 0, xml, -1, u8, sizeof(u8), nullptr, nullptr);
							if (n > 1 && len + n < cap) {
								std::memcpy(out + len, u8, n - 1);
								len += n - 1;
								out[len++] = '\n';
								out[len] = 0;
							}
						}
					}
					cl(ev[i]);
				}
			}
			cl(r);
		}
	}
	FreeLibrary(w);
	return len;
}

#else   // ------------------------------------------------------------------- not Windows: nothing yet

#define CRASH_API extern "C" __attribute__((visibility("default")))
extern "C" void igmt_trail_add(const char *) {}
CRASH_API int gmtvtk_crash_install(const char *) { return 0; }
CRASH_API void gmtvtk_crash_trail(const char *) {}
CRASH_API int gmtvtk_crash_eventlog(char *out, int cap) {
	if (out && cap > 0) out[0] = 0;
	return 0;
}

#endif
