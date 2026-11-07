// Rotating Quicksaves ASI (companion of the Rotating Quicksaves script mod).
// One engine change: quicksaves never replace each other. The profile manager's PrepareSave (vtable slot 0x80)
// turns a quicksave request with slot -1 (the game's F5 request) into slot 0, which skips the engine's
// "delete the newest quicksave" rule. The script mod deletes old quicksaves itself after each quicksave.
// The change is armed only once the script mod has asked for the status (it does so when a game loads), so
// without the script mod F5 stays the game's normal single quicksave and saves never pile up.
// F5 itself stays the game's key: nothing in input.settings is needed.
// Status for the script: GetNumSaveSlots(SGT_ForcedCheckPoint = engine value 4) (vtable slot 0x108, a stub returning -1 on PC)
// answers 31000 + version when active, 30000 + reason when inactive; -1 means this ASI is not loaded.
// Targets are found at run time (RTTI for the vtables, a string reference for the save function), so no fixed addresses.
// The vtable slots are chained: whatever pointer is in a slot when we hook (game code or another ASI) is called.
// Log: RotatingQuicksaves.log next to this file. File RotatingQuicksaves_disabled next to it: change nothing.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define RETADDR() _ReturnAddress()
#else
#define RETADDR() __builtin_return_address(0)
#endif

#define RQS_VERSION 100   // 1.0.0 as major*100 + minor*10 + patch; the script shows it as x.y.z
#define RQS_VERSION_STR "1.0.0"
enum { R_OK = 0, R_DISABLED = 1, R_NO_VTABLE = 2, R_BAD_PREPARESAVE = 3, R_NO_F5 = 4, R_PATCH_FAILED = 5 };
static const char *REASON[] = { "active", "disabled by file", "save manager class not found",
	"save function check failed", "(unused since 1.0: was the F5 key check)", "memory patch failed" };

typedef int (*PrepareSave_t)(void *mgr, uint8_t *info);
typedef int (*NumSlots_t)(void *mgr, int type);
typedef int (*DeleteSave_t)(void *mgr, uint8_t *info);
static PrepareSave_t origPrepare[2];
static NumSlots_t origNumSlots[2];
static DeleteSave_t origDelete[2];
static uintptr_t *vtables[2];
static int status = R_NO_VTABLE;
static BOOL active = FALSE;

static uint8_t *modBase; static size_t modSize;
static FILE *logf; static CRITICAL_SECTION logLock;

static void logline(const char *fmt, ...)
{
	if (!logf) return;
	EnterCriticalSection(&logLock);
	SYSTEMTIME t; GetLocalTime(&t);
	fprintf(logf, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list a; va_start(a, fmt); vfprintf(logf, fmt, a); va_end(a);
	fputc('\n', logf); fflush(logf);
	LeaveCriticalSection(&logLock);
}

// ---- PE helpers ----
typedef struct { uint8_t *p; size_t n; } Range;
static Range section(const char *name)
{
	IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(modBase + ((IMAGE_DOS_HEADER *)modBase)->e_lfanew);
	IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
		if (!strncmp((char *)s[i].Name, name, 8)) { Range r = { modBase + s[i].VirtualAddress, s[i].Misc.VirtualSize }; return r; }
	Range z = { 0, 0 }; return z;
}
static BOOL inRange(Range r, const void *p, size_t n) { return (uint8_t *)p >= r.p && (uint8_t *)p + n <= r.p + r.n; }
static uint8_t *memfind(Range r, const void *pat, size_t n, uint8_t *from)
{
	uint8_t *p = from ? from : r.p, *end = r.p + r.n;
	for (; p + n <= end; p++) if (*p == *(uint8_t *)pat && !memcmp(p, pat, n)) return p;
	return NULL;
}

// RTTI: ".?AV<cls>@@" type descriptor -> complete object locator (sig 1, offset 0, self RVA) -> vtable after it.
static uintptr_t *findVtable(const char *cls)
{
	char pat[128]; int n = snprintf(pat, sizeof pat, ".?AV%s@@", cls) + 1; // include the terminating 0
	Range data = section(".data"), rdata = section(".rdata"), all = { modBase, modSize };
	uint8_t *s = memfind(data, pat, n, NULL);
	if (!s) s = memfind(all, pat, n, NULL);
	if (!s) return NULL;
	uint32_t tdRva = (uint32_t)(s - 16 - modBase);
	uintptr_t *found = NULL; int count = 0;
	for (uint8_t *c = memfind(rdata, &tdRva, 4, NULL); c; c = memfind(rdata, &tdRva, 4, c + 1)) {
		uint8_t *col = c - 12; uint32_t *f = (uint32_t *)col;
		if (!inRange(rdata, col, 24) || f[0] != 1 || f[1] != 0 || f[5] != (uint32_t)(col - modBase)) continue;
		uintptr_t colVa = (uintptr_t)col;
		for (uint8_t *v = memfind(rdata, &colVa, 8, NULL); v; v = memfind(rdata, &colVa, 8, v + 1)) {
			found = (uintptr_t *)(v + 8); count++;
		}
	}
	return count == 1 ? found : NULL;
}

// PrepareSave check: the function (or, if another ASI already hooked the slot, we trust it and log) must load
// the save-name format "%s_%lx_%lx_%lx" with a RIP-relative lea within its first 0x900 bytes.
static BOOL isPrepareSave(uintptr_t fn, uint8_t *fmt)
{
	Range text = section(".text");
	if (!inRange(text, (void *)fn, 0x900)) return FALSE;
	for (uint8_t *p = (uint8_t *)fn; p < (uint8_t *)fn + 0x900; p++)
		if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05)
			if (p + 7 + *(int32_t *)(p + 3) == fmt) return TRUE;
	return FALSE;
}

static BOOL ownedByGame(uintptr_t fn) { return fn >= (uintptr_t)modBase && fn < (uintptr_t)modBase + modSize; }
static void moduleOf(uintptr_t addr, char *out, size_t n)
{
	HMODULE m = NULL; out[0] = 0;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m))
		GetModuleFileNameA(m, out, (DWORD)n);
	if (!out[0]) snprintf(out, n, "unknown");
}

// ---- hooks ----
// SSavegameInfo: +4 type, +8 slotIndex, +0x18 name (char*), +0x20 length
// the name is an 8-bit string (pointer + length), seen in the 0.1.1 log
static void infoName(uint8_t *info, char *out, size_t n)
{
	const char *c = *(const char **)(info + 0x18); out[0] = 0;
	if (!c || IsBadReadPtr(c, 1)) return;
	size_t i = 0;
	for (; i + 1 < n && c[i] && !IsBadReadPtr(c + i, 1); i++) out[i] = c[i];
	out[i] = 0;
}
static volatile LONG armed;   // set when the script mod first asks for the status
static int which(void *mgr) { return (*(uintptr_t **)mgr == vtables[1]) ? 1 : 0; }

static int hkPrepare(void *mgr, uint8_t *info)
{
	int type = *(int *)(info + 4), slot = *(int *)(info + 8);
	void *ret = RETADDR();
	BOOL rewrite = active && armed && type == 2 && slot < 0;
	if (rewrite) *(int *)(info + 8) = 0;
	int r = origPrepare[which(mgr)](mgr, info);
	char name[160]; infoName(info, name, sizeof name);
	logline("PrepareSave type=%d slot=%d%s -> %d name=%s caller=witcher3+0x%llx", type, slot,
		rewrite ? " (now 0: no replace)" : (active && !armed && type == 2 && slot < 0) ? " (script mod not seen: vanilla replace)" : "", r, name,
		(unsigned long long)((uint8_t *)ret - modBase));
	return r;
}
static int hkNumSlots(void *mgr, int type)
{
	static int logged = 0;
	if (type == 4) {   // SGT_ForcedCheckPoint is 4 in the engine's enum (SGT_CheckPoint is 5)
		int r = active ? 31000 + RQS_VERSION : 30000 + status;
		if (active && !InterlockedExchange(&armed, 1)) logline("script mod seen: quicksaves now add a new file");
		if (logged < 3) { logged++; logline("status asked by the script: %d", r); }
		return r;
	}
	return origNumSlots[which(mgr)](mgr, type);
}
static int hkDelete(void *mgr, uint8_t *info)
{
	char name[160]; infoName(info, name, sizeof name);
	logline("DeleteSaveGame (script) type=%d name=%s", *(int *)(info + 4), name);
	return origDelete[which(mgr)](mgr, info);
}

// every .sav delete/rename in the game process, with the caller, to settle which code removes quicksaves
typedef BOOL (WINAPI *DeleteFileW_t)(LPCWSTR);
typedef BOOL (WINAPI *MoveFileExW_t)(LPCWSTR, LPCWSTR, DWORD);
static DeleteFileW_t origDeleteFileW; static MoveFileExW_t origMoveFileExW;
static BOOL isSave(LPCWSTR p) { return p && (wcsstr(p, L"gamesaves") || wcsstr(p, L".sav")); }
static BOOL WINAPI hkDeleteFileW(LPCWSTR p)
{
	if (isSave(p)) logline("DeleteFileW %ls caller=witcher3+0x%llx", p, (unsigned long long)((uint8_t *)RETADDR() - modBase));
	return origDeleteFileW(p);
}
static BOOL WINAPI hkMoveFileExW(LPCWSTR a, LPCWSTR b, DWORD f)
{
	if (isSave(a) || isSave(b)) logline("MoveFileExW %ls -> %ls", a, b ? b : L"(null)");
	return origMoveFileExW(a, b, f);
}
static void **findIat(const char *dll, const char *fn)
{
	IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(modBase + ((IMAGE_DOS_HEADER *)modBase)->e_lfanew);
	IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (IMAGE_IMPORT_DESCRIPTOR *i = (IMAGE_IMPORT_DESCRIPTOR *)(modBase + d.VirtualAddress); i->Name; i++) {
		if (_stricmp((char *)(modBase + i->Name), dll)) continue;
		IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(modBase + i->OriginalFirstThunk);
		void **iat = (void **)(modBase + i->FirstThunk);
		for (int k = 0; names[k].u1.AddressOfData; k++)
			if (!(names[k].u1.Ordinal & IMAGE_ORDINAL_FLAG64) &&
				!strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(modBase + names[k].u1.AddressOfData))->Name, fn)) return &iat[k];
	}
	return NULL;
}
static BOOL writeMem(void *dst, const void *src, size_t n)
{
	DWORD old;
	if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
	memcpy(dst, src, n);
	VirtualProtect(dst, n, old, &old);
	FlushInstructionCache(GetCurrentProcess(), dst, n);
	return TRUE;
}

static void install(const char *dir)
{
	char path[MAX_PATH];
	snprintf(path, sizeof path, "%s\\RotatingQuicksaves_disabled", dir);
	BOOL disabled = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;

	IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(modBase + ((IMAGE_DOS_HEADER *)modBase)->e_lfanew);
	modSize = nt->OptionalHeader.SizeOfImage;
	logline("Rotating Quicksaves ASI %s; game build 0x%08lX, image size 0x%zx", RQS_VERSION_STR,
		(unsigned long)nt->FileHeader.TimeDateStamp, modSize);

	vtables[0] = findVtable("CUserProfileManagerWindowsSaving");
	vtables[1] = findVtable("CUserProfileManagerWindows");
	logline("vtables: WindowsSaving %p, Windows %p", vtables[0], vtables[1]);
	if (!vtables[0] || !vtables[1]) { status = R_NO_VTABLE; logline("INACTIVE: %s; nothing changed", REASON[status]); return; }

	// checks before any change
	int st = disabled ? R_DISABLED : R_OK;
	uint8_t *fmt = memfind(section(".rdata"), "%s_%lx_%lx_%lx", 15, NULL);
	for (int i = 0; i < 2 && st == R_OK; i++) {
		uintptr_t fn = vtables[i][0x80 / 8];
		if (ownedByGame(fn)) { if (!fmt || !isPrepareSave(fn, fmt)) st = R_BAD_PREPARESAVE; }
		else { char m[MAX_PATH]; moduleOf(fn, m, sizeof m); logline("slot 0x80 already hooked by %s: chaining", m); }
	}

	// status answer is installed either way (only touches the -1 stub's slot)
	for (int i = 0; i < 2; i++) {
		origNumSlots[i] = (NumSlots_t)vtables[i][0x108 / 8];
		void *h = (void *)hkNumSlots; writeMem(&vtables[i][0x108 / 8], &h, 8);
	}
	if (st != R_OK) { status = st; logline("INACTIVE: %s; saving unchanged", REASON[st]); return; }

	// all checks passed: hook both managers
	BOOL ok = TRUE;
	for (int i = 0; i < 2; i++) {
		origPrepare[i] = (PrepareSave_t)vtables[i][0x80 / 8];
		origDelete[i] = (DeleteSave_t)vtables[i][0xF8 / 8];
		void *h1 = (void *)hkPrepare, *h2 = (void *)hkDelete;
		ok &= writeMem(&vtables[i][0x80 / 8], &h1, 8);
		ok &= writeMem(&vtables[i][0xF8 / 8], &h2, 8);
	}
	void **d = findIat("KERNEL32.dll", "DeleteFileW"), **m = findIat("KERNEL32.dll", "MoveFileExW");
	if (d) { origDeleteFileW = (DeleteFileW_t)*d; void *h = (void *)hkDeleteFileW; writeMem(d, &h, 8); }
	if (m) { origMoveFileExW = (MoveFileExW_t)*m; void *h = (void *)hkMoveFileExW; writeMem(m, &h, 8); }
	logline("file logging: DeleteFileW %s, MoveFileExW %s", d ? "on" : "not found", m ? "on" : "not found");
	if (!ok) { status = R_PATCH_FAILED; logline("ERROR: %s (state may be partial)", REASON[status]); return; }
	status = R_OK; active = TRUE;
	logline("ACTIVE: quicksaves will add a new file once the script mod checks in (F5 is the game's own)");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
	(void)res;
	if (reason != DLL_PROCESS_ATTACH) return TRUE;
	DisableThreadLibraryCalls(inst);
	char dir[MAX_PATH], path[MAX_PATH];
	GetModuleFileNameA(inst, dir, MAX_PATH);
	char *slash = strrchr(dir, '\\'); if (slash) *slash = 0;
	InitializeCriticalSection(&logLock);
	snprintf(path, sizeof path, "%s\\RotatingQuicksaves.log", dir);
	logf = fopen(path, "w");
	modBase = (uint8_t *)GetModuleHandleA(NULL);
	install(dir);
	return TRUE;
}
