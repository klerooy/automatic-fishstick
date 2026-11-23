/* rr_mem.c - Shared-memory transport for WmShared driver. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "rr_mem.h"
#include "../3_src_driver/wm_shared.h"

static uint8_t*  g_buf = NULL;
static HANDLE    g_worker = NULL;
static volatile LONG g_connected = 0;
static volatile LONG g_exit_flag = 0;
static CRITICAL_SECTION g_cs;
static int g_cs_init = 0;
static uint64_t g_peb_cache = 0;
static __declspec(thread) int g_ok = 0;

uint32_t g_rr_pid = 0;
static volatile long g_calls = 0;
static volatile long g_bytes = 0;
static uint32_t g_forced_pid = 0;

long rr_stat_calls(void) { return g_calls; }
long rr_stat_bytes(void) { return g_bytes; }
void rr_stat_reset(void) { g_calls = 0; g_bytes = 0; }

static volatile uint32_t* p_code(void)   { return (volatile uint32_t*)(g_buf + WM_OFF_CODE); }
static volatile uint16_t* p_status(void) { return (volatile uint16_t*)(g_buf + WM_OFF_STATUS); }
static WmOpData*          p_op(void)     { return (WmOpData*)(g_buf + WM_OFF_OP); }
static WmBatchHeader*     p_bhdr(void)   { return (WmBatchHeader*)(g_buf + WM_OFF_BHDR); }
static uint8_t*           p_bdata(void)  { return g_buf + WM_OFF_BDATA; }
static uint8_t*           p_bstat(void)  { return g_buf + WM_OFF_BSTAT; }

static int send_request(uint32_t code) {
    *p_code() = code;
    *p_status() = WMSS_WAITING;
    DWORD t0 = GetTickCount();
    while (*p_code() != WMSC_COMPLETE || *p_status() != WMSS_ACTIVE) {
        if (GetTickCount() - t0 > 5000) return 0;
        if (InterlockedCompareExchange(&g_exit_flag, 0, 0)) return 0;
        Sleep(0);
    }
    g_calls++;
    return 1;
}

typedef NTSTATUS (NTAPI* fNtConvert)(PVOID, PVOID, PVOID, PVOID);

static DWORD WINAPI kernel_worker(LPVOID param) {
    WmCommData* comm = (WmCommData*)param;
    HMODULE nd = GetModuleHandleW(L"ntdll.dll");
    fNtConvert fn = (fNtConvert)GetProcAddress(nd,
        "NtConvertBetweenAuxiliaryCounterAndPerformanceCounter");
    if (!fn) { InterlockedExchange(&g_connected, 0); return 1; }
    PVOID pData = comm;              /* hook derefs A1 once -> &comm */
    INT64 out = 0; ULONG err = 0;
    fn((PVOID)1, &pData, &out, &err);
    InterlockedExchange(&g_connected, 0);
    return 0;
}

int rr_drv_connect(void) {
    if (InterlockedCompareExchange(&g_connected, 0, 0)) return 1;
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = 1; }
    g_buf = (uint8_t*)VirtualAlloc(NULL, WM_BUFFER_SIZE,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_buf) return 0;
    memset(g_buf, 0, WM_BUFFER_SIZE);
    /* Defined idle state: a stale ACTIVE from an earlier mapping must not make
     * connect() succeed against a driver that is no longer listening. */
    *p_code()   = WMSC_COMPLETE;
    *p_status() = WMSS_INACTIVE;

    static WmCommData comm;
    comm.ProcessId = GetCurrentProcessId();
    comm.Buffer    = (uint64_t)g_buf;
    comm.pCode     = (uint64_t)(g_buf + WM_OFF_CODE);
    comm.pStatus   = (uint64_t)(g_buf + WM_OFF_STATUS);
    comm.Magic     = WM_MAGIC;

    InterlockedExchange(&g_exit_flag, 0);
    g_worker = CreateThread(NULL, 0, kernel_worker, &comm, 0, NULL);
    if (!g_worker) { VirtualFree(g_buf, 0, MEM_RELEASE); g_buf = NULL; return 0; }

    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 8000) {
        if (*p_status() == WMSS_ACTIVE && *p_code() == WMSC_COMPLETE) {
            InterlockedExchange(&g_connected, 1);
            return 1;
        }
        Sleep(10);
    }
    InterlockedExchange(&g_exit_flag, 1);
    *p_status() = WMSS_EXIT;
    WaitForSingleObject(g_worker, 2000);
    CloseHandle(g_worker); g_worker = NULL;
    VirtualFree(g_buf, 0, MEM_RELEASE); g_buf = NULL;
    return 0;
}

void rr_drv_disconnect(void) {
    if (!InterlockedCompareExchange(&g_connected, 0, 0)) return;
    InterlockedExchange(&g_exit_flag, 1);
    if (g_buf) *p_status() = WMSS_EXIT;
    if (g_worker) {
        WaitForSingleObject(g_worker, 3000);
        CloseHandle(g_worker); g_worker = NULL;
    }
    if (g_buf) { VirtualFree(g_buf, 0, MEM_RELEASE); g_buf = NULL; }
    g_peb_cache = 0;
}

int rr_drv_open(void) {
    if (InterlockedCompareExchange(&g_connected, 0, 0)) return 1;
    return rr_drv_connect();
}
void rr_drv_close(void) { rr_drv_disconnect(); }
int rr_drv_live(void)  { return g_ok; }
int rr_drv_ready(void) { return InterlockedCompareExchange(&g_connected, 0, 0); }
int rr_drv_hide_process(uint32_t pid, int hide) { (void)pid; (void)hide; return 0; }

int rr_read(uint64_t addr, void* out, size_t n) {
    if (!out || n == 0 || n > 0x10000 || !g_buf) return 0;
    if (!InterlockedCompareExchange(&g_connected, 0, 0)) return 0;
    EnterCriticalSection(&g_cs);
    WmOpData* op = p_op();
    memset(op, 0, sizeof(*op));
    op->TargetPid = g_rr_pid;
    op->Address   = addr;
    op->BufferPtr = (uint64_t)out;
    op->Size      = n;
    op->ReadOp    = 1;
    int ok = send_request(WMSC_COPY);
    if (ok && op->NtStatus >= 0 && op->Size == n) { g_ok = 1; g_bytes += (long)n; }
    else ok = 0;
    LeaveCriticalSection(&g_cs);
    return ok;
}

int rr_write(uint64_t addr, const void* in, size_t n) {
    if (!in || n == 0 || n > 0x10000 || !g_buf) return 0;
    if (!InterlockedCompareExchange(&g_connected, 0, 0)) return 0;
    EnterCriticalSection(&g_cs);
    WmOpData* op = p_op();
    memset(op, 0, sizeof(*op));
    op->TargetPid = g_rr_pid;
    op->Address   = addr;
    op->BufferPtr = (uint64_t)in;
    op->Size      = n;
    op->ReadOp    = 0;
    int ok = send_request(WMSC_COPY);
    if (ok && op->NtStatus >= 0) { g_bytes += (long)n; } else ok = 0;
    LeaveCriticalSection(&g_cs);
    return ok;
}

long g_batch_status = 0, g_batch_info = 0, g_batch_expected = 0;

int rr_read_batch(RR_RD_REQ* items, int n) {
    if (!items || n <= 0 || n > WM_BATCH_MAX || !g_buf) return 0;
    if (!InterlockedCompareExchange(&g_connected, 0, 0)) return 0;
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
    EnterCriticalSection(&g_cs);
    WmBatchHeader* hdr = p_bhdr();
    memset(hdr, 0, sizeof(*hdr));
    hdr->Count = (uint32_t)n;
    hdr->TotalBytes = (uint32_t)total;
    hdr->TargetPid = g_rr_pid;
    uint32_t off = 0;
    for (int i = 0; i < n; i++) {
        hdr->Items[i].Address = items[i].addr;
        hdr->Items[i].Size = items[i].size;
        hdr->Items[i].Offset = off;
        off += items[i].size;
    }
    int ok = send_request(WMSC_BATCH);
    int result = -1;
    if (ok) {
        uint8_t* flags = p_bstat();
        uint8_t* data = p_bdata();
        int good = 0; uint32_t doff = 0;
        for (int i = 0; i < n; i++) {
            if (flags[i]) { memcpy(items[i].dst, data + doff, items[i].size); good++; }
            doff += items[i].size;
        }
        g_bytes += (long)off;
        result = good > 0 ? good : -1;
    }
    LeaveCriticalSection(&g_cs);
    return result;
}

int rr_ok(void) { return g_ok; }
uint64_t rr_u64(uint64_t a) { uint64_t v=0; g_ok=0; rr_read(a,&v,8); return v; }
uint32_t rr_u32(uint64_t a) { uint32_t v=0; g_ok=0; rr_read(a,&v,4); return v; }
int32_t  rr_i32(uint64_t a) { int32_t  v=0; g_ok=0; rr_read(a,&v,4); return v; }
float    rr_f32(uint64_t a) { float    v=0; g_ok=0; rr_read(a,&v,4); return v; }

static int ioctl_ptr(uint32_t code, uint64_t* out) {
    if (!g_buf || !InterlockedCompareExchange(&g_connected, 0, 0)) return 0;
    EnterCriticalSection(&g_cs);
    WmOpData* op = p_op();
    memset(op, 0, sizeof(*op));
    op->TargetPid = g_rr_pid;
    int ok = send_request(code);
    *out = op->Result;
    LeaveCriticalSection(&g_cs);
    return ok && op->NtStatus >= 0 && *out != 0;
}

uint64_t rr_image_base(void) { uint64_t b=0; return ioctl_ptr(WMSC_BASE,&b)?b:0; }
uint64_t rr_peb(void) {
    if (g_peb_cache) return g_peb_cache;
    uint64_t b=0; if (ioctl_ptr(WMSC_PEB,&b)) g_peb_cache=b; return g_peb_cache;
}

int rr_query_region(uint64_t addr, RR_REGION* out) {
    if (!out || !g_buf) return 0;
    if (!InterlockedCompareExchange(&g_connected, 0, 0)) return 0;
    EnterCriticalSection(&g_cs);
    WmOpData* op = p_op();
    memset(op, 0, sizeof(*op));
    op->TargetPid = g_rr_pid; op->Address = addr;
    int ok = send_request(WMSC_QUERY);
    if (ok && op->NtStatus >= 0) {
        out->base=op->RegionBase; out->size=op->RegionSize;
        out->state=op->State; out->protect=op->ProtectOut; out->type=op->Type;
    } else ok = 0;
    LeaveCriticalSection(&g_cs);
    return ok;
}

int rr_drv_alloc_remote(uint32_t pid, size_t size, uint32_t protect, uint64_t* out) {
    if (out) *out=0;
    if (!out||!pid||size==0||size>0x2000000||!g_buf) return 0;
    if (!InterlockedCompareExchange(&g_connected,0,0)) return 0;
    EnterCriticalSection(&g_cs);
    WmOpData* op = p_op();
    memset(op,0,sizeof(*op));
    op->TargetPid=pid; op->Size=size; op->Protect=protect; op->AllocType=0x3000;
    int ok = send_request(WMSC_ALLOC);
    if (ok && op->NtStatus>=0 && op->Result) *out=op->Result; else ok=0;
    LeaveCriticalSection(&g_cs);
    return ok;
}

long rr_drv_get_context(uint32_t thread_id, RR_THREAD_CTX* out) {
    if (out) memset(out,0,sizeof(*out));
    if (!thread_id||!out||!g_buf) return -1;
    if (!InterlockedCompareExchange(&g_connected,0,0)) return -1;
    EnterCriticalSection(&g_cs);
    WmOpData* op = p_op();
    memset(op,0,sizeof(*op));
    op->ThreadId=thread_id;
    int ok = send_request(WMSC_CONTEXT);
    long st=-1;
    if (ok) {
        out->rip=op->Rip; out->rsp=op->Rsp; out->rbp=op->Rbp; out->rflags=op->Rflags;
        out->tid=thread_id; out->ok=op->CtxOk;
        st=op->CtxOk?0:(long)op->NtStatus;
    }
    LeaveCriticalSection(&g_cs);
    return st;
}

typedef struct _RR_PE32 { DWORD dwSize,cntUsage,th32ProcessID; ULONG_PTR th32DefaultHeapID;
    DWORD th32ModuleID,cntThreads,th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags;
    CHAR szExeFile[MAX_PATH]; } RR_PE32;

int rr_process_visible(uint32_t pid) {
    if (!pid) return 0;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snap==INVALID_HANDLE_VALUE) return -1;
    RR_PE32 e; e.dwSize=sizeof(e);
    typedef BOOL(WINAPI*F)(HANDLE,RR_PE32*);
    F first=(F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"Process32First");
    F next=(F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"Process32Next");
    int found=0;
    if (first&&first(snap,&e)){do{if(e.th32ProcessID==pid){found=1;break;}}while(next&&next(snap,&e));}
    CloseHandle(snap); return found;
}

void rr_force_pid(uint32_t pid){g_forced_pid=pid;}

int rr_attach(const char* exe_name){
    if(g_forced_pid){g_rr_pid=g_forced_pid;return 1;}
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE)return 0;
    RR_PE32 e;e.dwSize=sizeof(e);
    typedef BOOL(WINAPI*F)(HANDLE,RR_PE32*);
    F first=(F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"Process32First");
    F next=(F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"Process32Next");
    int found=0;
    if(first&&first(snap,&e)){do{if(!_stricmp(e.szExeFile,exe_name)){g_rr_pid=e.th32ProcessID;found=1;break;}}while(next&&next(snap,&e));}
    CloseHandle(snap);return found;
}

#define PL(v) ((v)>=0x10000ULL&&(v)<0x00007FFFFFFFFFFFULL)

int rr_module(const char* name, uint64_t* base, uint32_t* size){
    *base=0;if(size)*size=0;
    if(!g_buf||!InterlockedCompareExchange(&g_connected,0,0))return 0;
    EnterCriticalSection(&g_cs);
    WmOpData* op=p_op();
    memset(op,0,sizeof(*op));
    op->TargetPid=g_rr_pid;
    wchar_t wname[64];
    int chars=MultiByteToWideChar(CP_ACP,0,name,-1,wname,64);
    if(chars<=0){LeaveCriticalSection(&g_cs);return 0;}
    op->ModuleNameLen=(uint32_t)((chars-1)*2);
    memcpy(op->ModuleName,wname,op->ModuleNameLen);
    int ok=send_request(WMSC_MODULE);
    if(ok&&op->NtStatus>=0&&op->Result){*base=op->Result;if(size)*size=(uint32_t)op->Size;}else ok=0;
    LeaveCriticalSection(&g_cs);
    return ok&&PL(*base);
}

int rr_modules(char* names,uint64_t*bases,uint32_t*sizes,int max){
    if(!names||!bases||max<=0)return 0;
    uint64_t peb=rr_peb();if(!peb)return 0;
    uint64_t ldr=rr_u64(peb+0x18);if(!PL(ldr))return 0;
    uint64_t head=ldr+0x10;uint64_t cur=rr_u64(head);if(!PL(cur))return 0;
    int n=0;
    for(int i=0;i<512&&n<max;i++){
        if(cur==head||!PL(cur))break;
        USHORT blen=(USHORT)rr_u32(cur+0x58);uint64_t bbuf=rr_u64(cur+0x60);
        char*slot=names+(size_t)n*64;slot[0]=0;
        if(blen>0&&blen<512&&PL(bbuf)){
            wchar_t got[256];size_t c2=blen/2;if(c2>255)c2=255;
            if(rr_read(bbuf,got,c2*2)){got[c2]=0;WideCharToMultiByte(CP_ACP,0,got,-1,slot,64,NULL,NULL);}
        }
        bases[n]=rr_u64(cur+0x30);if(sizes)sizes[n]=rr_u32(cur+0x40);
        if(slot[0])n++;cur=rr_u64(cur);
    }
    return n;
}

int rr_scan(uint64_t start,uint64_t size,const uint8_t*pat,const char*mask,uint64_t*hits,int max_hits){
    if(!size||!pat||!mask)return 0;
    size_t plen=strlen(mask);if(!plen||plen>128||size<plen)return 0;
    enum{CHUNK=0x10000};static uint8_t buf[CHUNK+128];int found=0;
    for(uint64_t off=0;off+plen<=size;off+=CHUNK){
        size_t want=(size_t)((size-off)<CHUNK?(size-off):CHUNK);
        size_t read_n=want+plen-1;if(read_n>sizeof(buf))read_n=sizeof(buf);
        if(!rr_read(start+off,buf,read_n))continue;
        for(size_t i=0;i+plen<=read_n;i++){
            size_t k=0;for(;k<plen;k++)if(mask[k]=='x'&&buf[i+k]!=pat[k])break;
            if(k==plen){if(found<max_hits)hits[found]=start+off+i;found++;if(found>=max_hits)return found;}
        }
    }
    return found;
}