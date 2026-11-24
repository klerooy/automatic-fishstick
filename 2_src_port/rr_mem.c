/* rr_mem.c - No-hook shared-buffer transport for WmShared driver.
 *
 * Replaces IOCTL/device transport with registry handshake + shared memory.
 * Driver runs a system thread that reads session info from registry,
 * attaches to this process, and polls the shared buffer for requests.
 * No kernel function pointers are modified. No device objects created.
 *
 * All functions above the transport layer (rr_module, rr_scan, rr_attach,
 * etc.) are unchanged - they call rr_read/rr_peb which now go through
 * the shared buffer instead of NtDeviceIoControlFile.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "rr_mem.h"
#include "wm_shared.h"

/* winternl.h carries STATUS_TIMEOUT but not the device status below; the
 * value is from ntddk/ntstatus.h (0xC000009F, "device not connected"). */
#ifndef STATUS_DEVICE_NOT_CONNECTED
#define STATUS_DEVICE_NOT_CONNECTED ((NTSTATUS)0xC000009FL)
#endif
#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT              ((NTSTATUS)0x00000102L)
#endif

/* === TRANSPORT STATE === */
static uint8_t* g_buffer = NULL;
static volatile uint32_t* g_pCode = NULL;
static volatile uint16_t* g_pStatus = NULL;
static int g_connected = 0;
static uint64_t g_peb_cache = 0;
static HANDLE g_wake = NULL;

/* Per-thread small-read status (unchanged semantics). */
static __declspec(thread) int g_ok = 0;

uint32_t g_rr_pid = 0;
long g_batch_status = 0, g_batch_info = 0, g_batch_expected = 0;

static volatile long g_calls = 0;
static volatile long g_bytes = 0;

/* One op slot serves the whole process: every request must be serialized.
 * Without this, concurrent scan workers overwrite WM_OFF_OP mid-flight and
 * the driver answers into a dead thread's stack buffer (heap/stack smash). */
/* Transport serialization + self-diagnosis. One op slot serves every thread;
 * tx_diag.log next to the exe proves this build is the locked one. */
static CRITICAL_SECTION g_tx_cs;
static volatile LONG g_tx_ready = 0;
static volatile LONG g_tx_wait_max_ms = 0;
static volatile LONG g_tx_timeouts = 0;
static volatile LONG g_tx_ops = 0;
static void tx_init(void) {
    if (InterlockedCompareExchange(&g_tx_ready, 1, 0) == 0)
        InitializeCriticalSection(&g_tx_cs);
}
static void tx_lock(void)   { tx_init(); EnterCriticalSection(&g_tx_cs); }
static void tx_unlock(void) { LeaveCriticalSection(&g_tx_cs); }

static void tx_diag_dump(void) {
    FILE* f = fopen("tx_diag.log", "a");
    if (!f) return;
    fprintf(f, "%lu tid=%lu ops=%ld wait_max=%ldms timeouts=%ld\n",
            (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId(),
            (long)g_tx_ops, (long)g_tx_wait_max_ms, (long)g_tx_timeouts);
    fclose(f);
}

long rr_stat_calls(void) { return g_calls; }
long rr_stat_bytes(void) { return g_bytes; }
void rr_stat_reset(void) { g_calls = 0; g_bytes = 0; }

/* Session data written to registry for driver handshake. */
#pragma pack(push, 1)
typedef struct { uint32_t Pid; uint64_t BufferVa; } WM_SESSION_REG;
#pragma pack(pop)

/* Send a request via shared buffer and wait for driver completion. */
static NTSTATUS send_request(uint32_t code, WmOpData* op) {
    if (!g_connected || !g_buffer) return STATUS_DEVICE_NOT_CONNECTED;
    DWORD t_enter = GetTickCount();
    tx_lock();
    LONG waited = (LONG)(GetTickCount() - t_enter);
    for (;;) { LONG cur = g_tx_wait_max_ms;
               if (waited <= cur || InterlockedCompareExchange(&g_tx_wait_max_ms, waited, cur) == cur) break; }
    if (!g_connected || !g_buffer) { tx_unlock(); return STATUS_DEVICE_NOT_CONNECTED; }
    memcpy(g_buffer + WM_OFF_OP, op, sizeof(WmOpData));
    *g_pCode = code;
    *g_pStatus = WMSS_WAITING;
    if (g_wake) SetEvent(g_wake);
    DWORD start = GetTickCount();
    while (*g_pCode != WMSC_COMPLETE) {
        if (GetTickCount() - start > 5000) {
            /* Driver may still write into op->BufferPtr (caller stack).
             * Hold the slot until it finishes; caller stays blocked here. */
            while (*g_pCode != WMSC_COMPLETE && g_connected) Sleep(1);
            if (g_pStatus) *g_pStatus = WMSS_ACTIVE;
            InterlockedIncrement(&g_tx_timeouts);
            tx_diag_dump();
            tx_unlock();
            return STATUS_TIMEOUT;
        }
        Sleep(0);
    }
    memcpy(op, g_buffer + WM_OFF_OP, sizeof(WmOpData));
    *g_pStatus = WMSS_ACTIVE;
    g_calls++;
    if ((InterlockedIncrement(&g_tx_ops) % 2000) == 0) tx_diag_dump();
    tx_unlock();
    return (NTSTATUS)op->NtStatus;
}

/* === LIFECYCLE === */

int rr_drv_open(void) {
    if (g_connected) return 1;

    HANDLE live = OpenEventW(SYNCHRONIZE, FALSE, L"Global\\WmDriverLive");
    if (!live) {
        fprintf(stderr, "[drv] OpenEventW(WmDriverLive) failed err=%lu\n", GetLastError());
        return 0;
    }
    CloseHandle(live);

    g_buffer = (uint8_t*)VirtualAlloc(NULL, WM_BUFFER_SIZE,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_buffer) { fprintf(stderr, "[drv] VirtualAlloc failed err=%lu\n", GetLastError()); return 0; }
    memset(g_buffer, 0, WM_BUFFER_SIZE);

    WmCommData* c = (WmCommData*)g_buffer;
    c->Magic = WM_MAGIC;
    c->ProcessId = GetCurrentProcessId();
    c->Buffer = (uint64_t)(uintptr_t)g_buffer;
    c->pCode = c->Buffer + WM_OFF_CODE;
    c->pStatus = c->Buffer + WM_OFF_STATUS;
    if (!g_wake) g_wake = CreateEventA(NULL, FALSE, FALSE, "Global\\WmSharedWake");
    c->WakeHandle = (uint64_t)(uintptr_t)g_wake;

    g_pCode = (volatile uint32_t*)(g_buffer + WM_OFF_CODE);
    g_pStatus = (volatile uint16_t*)(g_buffer + WM_OFF_STATUS);
    *g_pCode = WMSC_COMPLETE;
    *g_pStatus = WMSS_INACTIVE;

    WM_SESSION_REG sess;
    sess.Pid = GetCurrentProcessId();
    sess.BufferVa = (uint64_t)(uintptr_t)g_buffer;

    HKEY hk = NULL;
    LONG rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\WmPort", 0, NULL,
                               REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hk, NULL);
    if (rc != ERROR_SUCCESS) {
        fprintf(stderr, "[drv] RegCreateKeyEx failed rc=%ld\n", rc);
        VirtualFree(g_buffer, 0, MEM_RELEASE); g_buffer = NULL;
        return 0;
    }
    rc = RegSetValueExA(hk, "Session", 0, REG_BINARY, (const BYTE*)&sess, sizeof(sess));
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS) {
        fprintf(stderr, "[drv] RegSetValueEx failed rc=%ld\n", rc);
        VirtualFree(g_buffer, 0, MEM_RELEASE); g_buffer = NULL;
        return 0;
    }
    fprintf(stderr, "[drv] Session written pid=%lu va=%llX, waiting ACTIVE...\n",
            sess.Pid, (unsigned long long)sess.BufferVa);

    DWORD t0 = GetTickCount();
    while (*g_pStatus != WMSS_ACTIVE) {
        if (GetTickCount() - t0 > 3000) {
            fprintf(stderr, "[drv] TIMEOUT waiting ACTIVE (status=%u code=%lu) after %lu ms\n",
                    (unsigned)*g_pStatus, (unsigned long)*g_pCode, (unsigned long)(GetTickCount()-t0));
            VirtualFree(g_buffer, 0, MEM_RELEASE); g_buffer = NULL;
            g_pCode = NULL; g_pStatus = NULL;
            HKEY hk2 = NULL;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\WmPort", 0, KEY_SET_VALUE, &hk2) == ERROR_SUCCESS) {
                RegDeleteValueA(hk2, "Session");
                RegCloseKey(hk2);
            }
            return 0;
        }
        Sleep(10);
    }

    fprintf(stderr, "[drv] CONNECTED status=ACTIVE\n");
    g_connected = 1;
    return 1;
}

void rr_drv_close(void) {
    tx_lock();
    if (g_buffer && g_pStatus) {
        *g_pStatus = WMSS_EXIT;
        if (g_wake) SetEvent(g_wake);
        Sleep(50);
    }
    if (g_buffer) { VirtualFree(g_buffer, 0, MEM_RELEASE); g_buffer = NULL; }
    g_pCode = NULL; g_pStatus = NULL;
    if (g_wake) { CloseHandle(g_wake); g_wake = NULL; }
    g_connected = 0; g_peb_cache = 0; g_ok = 0;
    tx_unlock();

    HKEY hk = NULL;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\WmPort", 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        RegDeleteValueA(hk, "Session");
        RegCloseKey(hk);
    }
}

int rr_drv_live(void) { return g_ok; }
int rr_drv_ready(void) { return g_connected ? 1 : 0; }

/* === READ / WRITE === */

int rr_read(uint64_t addr, void* out, size_t n) {
    if (!out || n == 0 || n > 0x10000u) return 0;
    WmOpData op;
    memset(&op, 0, sizeof(op));
    op.TargetPid = g_rr_pid;
    op.Address = addr;
    op.BufferPtr = (uint64_t)(uintptr_t)out;
    op.Size = n;
    op.ReadOp = 1;
    NTSTATUS st = send_request(WMSC_COPY, &op);
    g_bytes += (long)n;
    if (st >= 0 && op.Size == n) { g_ok = 1; return 1; }
    return 0;
}

int rr_write(uint64_t addr, const void* in, size_t n) {
    if (!in || n == 0 || n > 0x10000u) return 0;
    WmOpData op;
    memset(&op, 0, sizeof(op));
    op.TargetPid = g_rr_pid;
    op.Address = addr;
    op.BufferPtr = (uint64_t)(uintptr_t)in;
    op.Size = n;
    op.ReadOp = 0;
    NTSTATUS st = send_request(WMSC_COPY, &op);
    g_bytes += (long)n;
    return st >= 0 ? 1 : 0;
}

int rr_ok(void) { return g_ok; }

uint64_t rr_u64(uint64_t a) { uint64_t v = 0; g_ok = 0; rr_read(a, &v, 8); return v; }
uint32_t rr_u32(uint64_t a) { uint32_t v = 0; g_ok = 0; rr_read(a, &v, 4); return v; }
int32_t  rr_i32(uint64_t a) { int32_t  v = 0; g_ok = 0; rr_read(a, &v, 4); return v; }
float    rr_f32(uint64_t a) { float    v = 0; g_ok = 0; rr_read(a, &v, 4); return v; }

/* === BATCH READ === */

int rr_read_batch(RR_RD_REQ* items, int n) {
    if (!g_connected || !items || n <= 0 || n > WM_BATCH_MAX) return 0;

    size_t total = 0;
    for (int i = 0; i < n; i++) total += items[i].size;
    if (total == 0) return 0;

    if (total > WM_BATCH_DATA) {
        int head = 0; size_t acc = 0;
        while (head < n && acc + items[head].size <= WM_BATCH_DATA)
            acc += items[head].size, head++;
        if (head == 0 || head == n) return 0;
        int a = rr_read_batch(items, head);
        int b = rr_read_batch(items + head, n - head);
        if (a < 0 && b < 0) return -1;
        return (a > 0 ? a : 0) + (b > 0 ? b : 0);
    }

    tx_lock();
    if (!g_connected || !g_buffer) { tx_unlock(); return 0; }
    WmBatchHeader* hdr = (WmBatchHeader*)(g_buffer + WM_OFF_BHDR);
    memset(hdr, 0, sizeof(WmBatchHeader));
    hdr->Count = (uint32_t)n;
    hdr->TargetPid = g_rr_pid;

    uint32_t off = 0;
    for (int i = 0; i < n; i++) {
        if (items[i].size == 0 || items[i].size > WM_BATCH_DATA) continue;
        if (off + items[i].size > WM_BATCH_DATA) break;
        hdr->Items[i].Address = items[i].addr;
        hdr->Items[i].Size = items[i].size;
        hdr->Items[i].Offset = off;
        off += items[i].size;
    }
    hdr->TotalBytes = off;
    if (hdr->Count == 0 || off == 0) { tx_unlock(); return 0; }

    *g_pCode = WMSC_BATCH;
    *g_pStatus = WMSS_WAITING;
    if (g_wake) SetEvent(g_wake);
    DWORD start = GetTickCount();
    while (*g_pCode != WMSC_COMPLETE) {
        if (GetTickCount() - start > 5000) {
            while (*g_pCode != WMSC_COMPLETE && g_connected) Sleep(1);
            if (g_pStatus) *g_pStatus = WMSS_ACTIVE;
            InterlockedIncrement(&g_tx_timeouts);
            tx_diag_dump();
            tx_unlock();
            return -1;
        }
        Sleep(0);
    }
    g_calls++;

    uint8_t* flags = g_buffer + WM_OFF_BSTAT;
    uint8_t* data = g_buffer + WM_OFF_BDATA;
    int ok = 0;
    uint32_t doff = 0;
    for (int i = 0; i < n; i++) {
        if (flags[i]) {
            memcpy(items[i].dst, data + doff, items[i].size);
            ok++;
        }
        doff += items[i].size;
    }
    g_bytes += (long)off;
    *g_pStatus = WMSS_ACTIVE;
    if ((InterlockedIncrement(&g_tx_ops) % 2000) == 0) tx_diag_dump();
    tx_unlock();
    return ok > 0 ? ok : -1;
}

/* === REGION / ALLOC / CONTEXT / HIDE === */

int rr_query_region(uint64_t addr, RR_REGION* out) {
    if (!out) return 0;
    WmOpData op;
    memset(&op, 0, sizeof(op));
    op.TargetPid = g_rr_pid;
    op.Address = addr;
    NTSTATUS st = send_request(WMSC_QUERY, &op);
    if (st < 0) return 0;
    out->base = op.RegionBase;
    out->size = op.RegionSize;
    out->state = op.State;
    out->protect = op.ProtectOut;
    out->type = op.Type;
    return 1;
}

int rr_drv_alloc_remote(uint32_t pid, size_t size, uint32_t protect, uint64_t* out) {
    if (out) *out = 0;
    if (!pid || size == 0 || size > 0x2000000u || !out) return 0;
    WmOpData op;
    memset(&op, 0, sizeof(op));
    op.TargetPid = pid;
    op.Size = size;
    op.Protect = protect;
    op.AllocType = MEM_COMMIT | MEM_RESERVE;
    NTSTATUS st = send_request(WMSC_ALLOC, &op);
    if (st < 0 || !op.Result) return 0;
    *out = op.Result;
    return 1;
}

long rr_drv_get_context(uint32_t thread_id, RR_THREAD_CTX* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!thread_id || !out) return -1;
    WmOpData op;
    memset(&op, 0, sizeof(op));
    op.ThreadId = thread_id;
    NTSTATUS st = send_request(WMSC_CONTEXT, &op);
    out->tid = thread_id;
    out->ok = op.CtxOk;
    out->rip = op.Rip;
    out->rsp = op.Rsp;
    out->rbp = op.Rbp;
    out->rflags = op.Rflags;
    return (long)st;
}

int rr_drv_hide_process(uint32_t pid, int hide) {
    (void)pid; (void)hide;
    return 0;
}

/* === IMAGE BASE / PEB === */

uint64_t rr_image_base(void) {
    WmOpData op;
    memset(&op, 0, sizeof(op));
    op.TargetPid = g_rr_pid;
    send_request(WMSC_BASE, &op);
    return op.Result;
}

uint64_t rr_peb(void) {
    if (g_peb_cache) return g_peb_cache;
    WmOpData op;
    memset(&op, 0, sizeof(op));
    op.TargetPid = g_rr_pid;
    send_request(WMSC_PEB, &op);
    if (op.Result) g_peb_cache = op.Result;
    return g_peb_cache;
}

/* === PROCESS ENUMERATION (unchanged) === */

typedef struct _RR_PE32 {
    DWORD dwSize, cntUsage, th32ProcessID;
    ULONG_PTR th32DefaultHeapID;
    DWORD th32ModuleID, cntThreads, th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    CHAR szExeFile[MAX_PATH];
} RR_PE32;

int rr_process_visible(uint32_t pid) {
    if (!pid) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    RR_PE32 e;
    e.dwSize = sizeof(e);
    typedef BOOL (WINAPI* F)(HANDLE, RR_PE32*);
    F first = (F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Process32First");
    F next  = (F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Process32Next");
    int found = 0;
    if (first && first(snap, &e)) {
        do {
            if (e.th32ProcessID == pid) { found = 1; break; }
        } while (next && next(snap, &e));
    }
    CloseHandle(snap);
    return found;
}

static uint32_t g_forced_pid = 0;

void rr_force_pid(uint32_t pid) { g_forced_pid = pid; }

int rr_attach(const char* exe_name) {
    if (g_forced_pid) { g_rr_pid = g_forced_pid; return 1; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    RR_PE32 e;
    e.dwSize = sizeof(e);
    typedef BOOL (WINAPI* F)(HANDLE, RR_PE32*);
    F first = (F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Process32First");
    F next  = (F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Process32Next");
    int found = 0;
    if (first && first(snap, &e)) {
        do {
            if (!_stricmp(e.szExeFile, exe_name)) {
                g_rr_pid = e.th32ProcessID;
                found = 1;
                break;
            }
        } while (next && next(snap, &e));
    }
    CloseHandle(snap);
    if (!found && g_connected) {
        /* The anti-cheat can hide the process from the Win32 snapshot. Ask the
         * driver to resolve the image name from the kernel process list. */
        WmOpData op;
        memset(&op, 0, sizeof(op));
        int n = MultiByteToWideChar(CP_ACP, 0, exe_name, -1, op.ModuleName, 64);
        if (n > 1) {
            op.ModuleNameLen = (uint32_t)(n - 1) * 2;
            if (send_request(WMSC_PID, &op) >= 0 && op.Result) {
                g_rr_pid = (uint32_t)op.Result;
                found = 1;
            }
        }
    }
    return found;
}

/* Liveness by process object, not by the Win32 snapshot: a hidden process is
 * still alive, and a killed one is still gone. */
int rr_drv_alive(uint32_t pid) {
    if (!pid || !g_connected) return 0;
    WmOpData op;
    memset(&op, 0, sizeof(op));
    op.TargetPid = pid;
    if (send_request(WMSC_BASE, &op) < 0) return 0;
    return op.Result != 0;
}

/* === MODULE LOOKUP (unchanged - uses rr_read/rr_peb via new transport) === */

#define PL(v) ((v) >= 0x10000ULL && (v) < 0x00007FFFFFFFFFFFULL)

int rr_module(const char* name, uint64_t* base, uint32_t* size) {
    *base = 0;
    if (size) *size = 0;
    uint64_t peb = rr_peb();
    if (!peb) return 0;

    uint64_t ldr = rr_u64(peb + 0x18);
    if (!PL(ldr)) return 0;
    uint64_t head = ldr + 0x10;
    uint64_t cur  = rr_u64(head);
    if (!PL(cur)) return 0;

    wchar_t want[64];
    MultiByteToWideChar(CP_ACP, 0, name, -1, want, 64);
    size_t want_len = wcslen(want);

    for (int i = 0; i < 512; i++) {
        if (cur == head || !PL(cur)) break;

        USHORT blen = (USHORT)rr_u32(cur + 0x58);
        uint64_t bbuf = rr_u64(cur + 0x60);
        if (blen > 0 && blen < 512 && PL(bbuf)) {
            wchar_t got[256];
            size_t chars = blen / 2;
            if (chars > 255) chars = 255;
            if (rr_read(bbuf, got, chars * 2)) {
                got[chars] = 0;
                if (_wcsicmp(got, want) == 0) {
                    *base = rr_u64(cur + 0x30);
                    if (size) *size = rr_u32(cur + 0x40);
                    return PL(*base) ? 1 : 0;
                }
            }
        }
        cur = rr_u64(cur);
    }
    return 0;
}

int rr_modules(char* names, uint64_t* bases, uint32_t* sizes, int max) {
    if (!names || !bases || max <= 0) return 0;
    uint64_t peb = rr_peb();
    if (!peb) return 0;
    uint64_t ldr = rr_u64(peb + 0x18);
    if (!PL(ldr)) return 0;
    uint64_t head = ldr + 0x10;
    uint64_t cur = rr_u64(head);
    if (!PL(cur)) return 0;

    int n = 0;
    for (int i = 0; i < 512 && n < max; i++) {
        if (cur == head || !PL(cur)) break;
        USHORT blen = (USHORT)rr_u32(cur + 0x58);
        uint64_t bbuf = rr_u64(cur + 0x60);
        char* slot = names + (size_t)n * 64;
        slot[0] = 0;
        if (blen > 0 && blen < 512 && PL(bbuf)) {
            wchar_t got[256];
            size_t chars = blen / 2;
            if (chars > 255) chars = 255;
            if (rr_read(bbuf, got, chars * 2)) {
                got[chars] = 0;
                WideCharToMultiByte(CP_ACP, 0, got, -1, slot, 64, NULL, NULL);
            }
        }
        bases[n] = rr_u64(cur + 0x30);
        if (sizes) sizes[n] = rr_u32(cur + 0x40);
        if (slot[0]) n++;
        cur = rr_u64(cur);
    }
    return n;
}

/* === PATTERN SCAN (unchanged) === */

int rr_scan(uint64_t start, uint64_t size, const uint8_t* pat, const char* mask,
            uint64_t* hits, int max_hits) {
    if (!size || !pat || !mask) return 0;
    size_t plen = strlen(mask);
    if (!plen || plen > 128 || size < plen) return 0;

    enum { CHUNK = 0x10000 };
    static uint8_t buf[CHUNK + 128];
    int found = 0;

    for (uint64_t off = 0; off + plen <= size; off += CHUNK) {
        size_t want = (size_t)((size - off) < CHUNK ? (size - off) : CHUNK);
        size_t read_n = want + plen - 1;
        if (read_n > sizeof(buf)) read_n = sizeof(buf);
        if (!rr_read(start + off, buf, read_n)) continue;

        for (size_t i = 0; i + plen <= read_n; i++) {
            size_t k = 0;
            for (; k < plen; k++) {
                if (mask[k] == 'x' && buf[i + k] != pat[k]) break;
            }
            if (k == plen) {
                if (found < max_hits) hits[found] = start + off + i;
                found++;
                if (found >= max_hits) return found;
            }
        }
    }
    return found;
}