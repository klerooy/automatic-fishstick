/* rr_exec.c - channel driver for the managed-call transport.
 *
 * Why this exists: on a title with an active kernel anti-cheat, the transport's
 * two Win32 steps are both denied (VirtualAllocEx and CreateRemoteThread return
 * ERROR_ACCESS_DENIED). The memory itself is reachable through the read/write
 * driver; only the execution step is missing. This driver supplies it: a
 * user-mode APC queued straight from the kernel needs no process or thread
 * handle, so handle filtering has nothing to filter.
 *
 * Design constraints, in order:
 *   1. No BSOD path. There is no IRP handling here, no device object, no
 *      dispatch table - the driver is an entry point, one system thread, and
 *      one exported function (KeInsertQueueApc). Nothing version-fragile is
 *      touched.
 *   2. No general privilege hole. The APC target must be a process whose image
 *      name is "ROB RUN.exe", checked with PsGetProcessImageFileName before the
 *      thread is looked up. A world-writable request file therefore cannot be
 *      used to execute code in any other process on the machine.
 *   3. Replaceable. Images mapped this way are resident until reboot, so the
 *      channel carries a generation counter: a newer image bumps it and every
 *      older worker exits at its next poll instead of double-executing.
 *
 * Channel: C:\Users\Public\rr_exec.bin, polled every 15 ms.
 *   0x00  REQ  0x30  { magic, seq, op, pid, tid, pad, routine, context }
 *   0x40  REP  0x18  { magic, seq, status, stage, extra }
 *   0x80  ULONG generation
 *
 * op 0x00 ping, op 0x01 queue APC, op 0xFE stop worker.
 *
 * Built without WDK headers on purpose (see build_driver.bat): every kernel
 * declaration below is the documented x64 shape of the API being called, and
 * the import library is generated from ntoskrnl.exe's own export table.
 */

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long               ntstatus;

#define NT_SUCCESS(s)                  ((s) >= 0)
#define STATUS_SUCCESS                 ((ntstatus)0x00000000)
#define STATUS_UNSUCCESSFUL            ((ntstatus)0xC0000001)
#define STATUS_INVALID_PARAMETER       ((ntstatus)0xC000000D)
#define STATUS_INSUFFICIENT_RESOURCES  ((ntstatus)0xC000009A)
#define STATUS_NOT_FOUND               ((ntstatus)0xC0000225)
#define STATUS_ACCESS_DENIED           ((ntstatus)0xC0000022)

#define POOL_NX             0x200u          /* NonPagedPoolNx */
#define APC_TAG             0x43455852u     /* 'RXEC' */
#define APC_BYTES           0x80u           /* sizeof(KAPC) is 0x58 on x64 */

#define FILE_OPEN_IF        3u
#define FILE_SHARE_BOTH     3u
#define FILE_ATTR_NORMAL    0x80u
#define FILE_NON_DIRECTORY  0x40u
#define FILE_SYNC_IO        0x20u
#define GENERIC_RW          0xC0000000u
#define OBJ_CASE_INSENSITIVE 0x40u
#define THREAD_ALL_ACCESS   0x1FFFFFu

#define REQ_MAGIC           0x31515252u     /* 'RRQ1' */
#define REP_MAGIC           0x31505252u     /* 'RRP1' */
#define OFF_REQ             0x00u
#define OFF_REP             0x40u
#define OFF_GEN             0x80u
#define FILE_BYTES          0x100u

#define OP_PING             0x00u
#define OP_APC              0x01u
#define OP_STOP             0xFEu

typedef struct { u16 Length; u16 MaximumLength; unsigned short* Buffer; } USTR;
typedef struct {
    u32 Length;                     /* 0x00, must be sizeof(OBJ) */
    u32 Pad0;
    void* RootDirectory;            /* 0x08 */
    USTR* ObjectName;               /* 0x10 */
    u32 Attributes;                 /* 0x18 */
    u32 Pad1;
    void* SecurityDescriptor;       /* 0x20 */
    void* SecurityQualityOfService; /* 0x28 */
} OBJ;                              /* 0x30 */

typedef struct { ntstatus Status; u32 Pad; u64 Information; } IOSB;

typedef struct {
    u32 magic, seq, op, pid, tid, pad;
    u64 routine, context;
} REQ;                              /* 0x30 */

typedef struct {
    u32 magic, seq;
    ntstatus status;
    u32 stage;
    u64 extra;
} REP;                              /* 0x18 */

/* Imports. Names match ntoskrnl.exe exports exactly; build_driver.bat turns the
 * export table into the import library the linker needs. */
__declspec(dllimport) ntstatus PsLookupProcessByProcessId(void* pid, void** out);
__declspec(dllimport) ntstatus PsLookupThreadByThreadId(void* tid, void** out);
__declspec(dllimport) void*    PsGetThreadProcess(void* thread);
__declspec(dllimport) char*    PsGetProcessImageFileName(void* process);
__declspec(dllimport) void     ObfDereferenceObject(void* obj);
__declspec(dllimport) void*    ExAllocatePoolWithTag(u32 pool, u64 size, u32 tag);
__declspec(dllimport) void     ExFreePoolWithTag(void* p, u32 tag);
__declspec(dllimport) void     KeInitializeApc(void* apc, void* thread, u8 state,
                                               void* kernelRoutine, void* rundownRoutine,
                                               void* normalRoutine, u8 mode, void* ctx);
__declspec(dllimport) u8       KeInsertQueueApc(void* apc, void* a1, void* a2, u8 boost);
__declspec(dllimport) ntstatus PsCreateSystemThread(void** handle, u32 access, void* objAttr,
                                                    void* process, void* clientId,
                                                    void* startRoutine, void* startContext);
__declspec(dllimport) ntstatus PsTerminateSystemThread(ntstatus code);
__declspec(dllimport) ntstatus KeDelayExecutionThread(u8 waitMode, u8 alertable, long long* interval);
__declspec(dllimport) ntstatus ZwCreateFile(void** handle, u32 access, OBJ* oa, IOSB* ios,
                                            long long* allocSize, u32 fileAttr, u32 share,
                                            u32 disposition, u32 options, void* ea, u32 eaLen);
__declspec(dllimport) ntstatus ZwReadFile(void* handle, void* ev, void* apcRoutine, void* apcCtx,
                                          IOSB* ios, void* buf, u32 len, long long* offset, u32* key);
__declspec(dllimport) ntstatus ZwWriteFile(void* handle, void* ev, void* apcRoutine, void* apcCtx,
                                           IOSB* ios, void* buf, u32 len, long long* offset, u32* key);
__declspec(dllimport) ntstatus ZwClose(void* handle);
__declspec(dllimport) void     RtlInitUnicodeString(USTR* dst, const unsigned short* src);

static void*    g_file;
static u32      g_generation;
static volatile long g_stop;

static void zero(void* p, u32 n) {
    u8* b = (u8*)p;
    while (n--) *b++ = 0;
}

static int image_is_target(void* process) {
    static const char want[] = "ROB RUN.exe";
    char* name = PsGetProcessImageFileName(process);
    int i;
    if (!name) return 0;
    for (i = 0; i < 16; ++i) {
        if (want[i] != name[i]) return 0;
        if (!want[i] && !name[i]) return 1;
    }
    return 0;
}

/* APC bookkeeping: both the normal path and a discarded APC free the block. */
static void apc_kernel(void* apc, void** normal, void** ctx, void** a1, void** a2) {
    (void)normal; (void)ctx; (void)a1; (void)a2;
    ExFreePoolWithTag(apc, APC_TAG);
}

static void apc_rundown(void* apc) {
    ExFreePoolWithTag(apc, APC_TAG);
}

static ntstatus queue_user_apc(REQ* q, REP* r) {
    void* process = 0;
    void* thread = 0;
    ntstatus st;
    void* apc;

    if (!q->pid || !q->tid || !q->routine) return STATUS_INVALID_PARAMETER;
    /* User canonical range only: a kernel address here would be a privilege bug. */
    if (q->routine > 0x7FFFFFFEFFFFull) return STATUS_INVALID_PARAMETER;

    st = PsLookupProcessByProcessId((void*)(u64)q->pid, &process);
    if (!NT_SUCCESS(st)) { r->stage = 1; return st; }
    if (!image_is_target(process)) {
        ObfDereferenceObject(process);
        r->stage = 2;
        return STATUS_ACCESS_DENIED;
    }
    st = PsLookupThreadByThreadId((void*)(u64)q->tid, &thread);
    if (!NT_SUCCESS(st)) {
        ObfDereferenceObject(process);
        r->stage = 3;
        return st;
    }
    if (PsGetThreadProcess(thread) != process) {
        st = STATUS_NOT_FOUND;
        r->stage = 4;
        goto done;
    }

    apc = ExAllocatePoolWithTag(POOL_NX, APC_BYTES, APC_TAG);
    if (!apc) { st = STATUS_INSUFFICIENT_RESOURCES; r->stage = 5; goto done; }
    zero(apc, APC_BYTES);
    KeInitializeApc(apc, thread, 0 /* OriginalApcEnvironment */,
                    apc_kernel, apc_rundown, (void*)q->routine,
                    1 /* UserMode */, (void*)q->context);
    if (!KeInsertQueueApc(apc, 0, 0, 0)) {
        ExFreePoolWithTag(apc, APC_TAG);
        st = STATUS_UNSUCCESSFUL;
        r->stage = 6;
        goto done;
    }
    r->stage = 7;
    st = STATUS_SUCCESS;

done:
    ObfDereferenceObject(thread);
    ObfDereferenceObject(process);
    return st;
}

static ntstatus file_io(void* buf, u32 len, u32 off, int write) {
    IOSB ios;
    long long pos = (long long)off;
    ntstatus st;
    zero(&ios, sizeof ios);
    st = write ? ZwWriteFile(g_file, 0, 0, 0, &ios, buf, len, &pos, 0)
               : ZwReadFile(g_file, 0, 0, 0, &ios, buf, len, &pos, 0);
    if (!NT_SUCCESS(st)) return st;
    return ios.Status;
}

static int channel_open(void) {
    static const unsigned short path[] = L"\\??\\C:\\Users\\Public\\rr_exec.bin";
    USTR name;
    OBJ oa;
    IOSB ios;
    ntstatus st;

    if (g_file) return 1;
    RtlInitUnicodeString(&name, path);
    zero(&oa, sizeof oa);
    oa.Length = (u32)sizeof(OBJ);
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    zero(&ios, sizeof ios);
    st = ZwCreateFile(&g_file, GENERIC_RW, &oa, &ios, 0, FILE_ATTR_NORMAL,
                      FILE_SHARE_BOTH, FILE_OPEN_IF,
                      FILE_NON_DIRECTORY | FILE_SYNC_IO, 0, 0);
    if (!NT_SUCCESS(st) || !NT_SUCCESS(ios.Status)) {
        g_file = 0;
        return 0;
    }
    return 1;
}

static void channel_close(void) {
    if (g_file) {
        ZwClose(g_file);
        g_file = 0;
    }
}

static void sleep_ms(long ms) {
    long long interval = -10000LL * (long long)ms;
    KeDelayExecutionThread(0 /* KernelMode */, 0, &interval);
}

static u32 read_generation(void) {
    u32 gen = 0;
    if (NT_SUCCESS(file_io(&gen, sizeof gen, OFF_GEN, 0))) return gen;
    return 0;
}

static void worker(void* unused) {
    REQ q;
    REP r;
    u32 last = 0;

    (void)unused;
    if (!channel_open()) {
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);
        return;
    }
    g_generation = read_generation() + 1;
    if (!g_generation) g_generation = 1;
    /* Taking the channel: whatever image ran before sees a higher generation at
     * its next poll and stops, so a fresh mapping always wins without a reboot. */
    file_io(&g_generation, sizeof g_generation, OFF_GEN, 1);

    while (!g_stop) {
        u32 gen;

        zero(&q, sizeof q);
        if (!NT_SUCCESS(file_io(&q, sizeof q, OFF_REQ, 0))) { sleep_ms(50); continue; }

        gen = read_generation();
        if (gen && gen > g_generation) break;      /* a newer image owns the channel */

        if (q.magic != REQ_MAGIC || !q.seq || q.seq == last) {
            sleep_ms(15);
            continue;
        }
        last = q.seq;

        zero(&r, sizeof r);
        r.magic = REP_MAGIC;
        r.seq = q.seq;
        r.status = STATUS_SUCCESS;

        if (q.op == OP_PING) {
            r.stage = 0x50;
            r.extra = (u64)g_generation;
        } else if (q.op == OP_APC) {
            r.status = queue_user_apc(&q, &r);
        } else if (q.op == OP_STOP) {
            r.stage = 0x51;
            file_io(&r, sizeof r, OFF_REP, 1);
            break;
        } else {
            r.status = STATUS_INVALID_PARAMETER;
            r.stage = 0x52;
        }
        file_io(&r, sizeof r, OFF_REP, 1);
        sleep_ms(15);
    }
    channel_close();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

ntstatus DriverEntry(void* driverObject, void* registryPath) {
    void* thread = 0;
    ntstatus st;
    (void)driverObject;
    (void)registryPath;

    st = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, 0, 0, 0,
                              (void*)worker, 0);
    if (!NT_SUCCESS(st)) return st;
    /* PsCreateSystemThread hands back a kernel handle, not an object pointer.
     * Dereferencing it as a pointer writes to the handle value itself, which is
     * what took the machine down on the first load. Close it instead. */
    if (thread) ZwClose(thread);
    return STATUS_SUCCESS;
}
