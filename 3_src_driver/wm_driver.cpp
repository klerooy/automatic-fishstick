/* wm_driver.cpp - WmShared: stealthy kernel memory server for RobRunPort. */
#include <ntifs.h>
#include <ntddk.h>
#include <ntimage.h>
#include <intrin.h>

extern "C" {
    NTKERNELAPI PVOID PsGetProcessSectionBaseAddress(PEPROCESS);
    NTKERNELAPI PPEB  NTAPI PsGetProcessPeb(PEPROCESS);
    NTKERNELAPI NTSTATUS MmCopyVirtualMemory(PEPROCESS, PVOID, PEPROCESS, PVOID,
                                             SIZE_T, KPROCESSOR_MODE, PSIZE_T);
    NTSYSCALLAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG, PVOID, ULONG, PULONG);
    NTSYSCALLAPI NTSTATUS NTAPI ZwProtectVirtualMemory(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);
}

#include "wm_shared.h"

extern PVOID wm_find_converter_slot(PVOID* out_original);
extern "C" ULONG wm_last_stage(void);
extern "C" ULONG wm_load_count(void);
extern "C" ULONG wm_func_rva(void);

static INT64 (NTAPI *g_OrigConverter)(ULONG64, PULONG64, PULONG64) = nullptr;

/* Last resolver outcome, kept in .data so a kernel debugger can read it even
 * after DriverEntry returned. Slot is the absolute .data word to be swapped. */
static volatile ULONG     g_DiagStage = 0xFFFFFFFF;
static volatile ULONG     g_DiagLoads = 0;
static volatile ULONG     g_DiagFnRva = 0;
static volatile ULONG_PTR g_DiagSlot  = 0;

typedef struct _RTL_PROCESS_MODULE_INFORMATION {
    HANDLE Section; PVOID MappedBase; PVOID ImageBase;
    ULONG ImageSize; ULONG Flags; USHORT LoadOrderIndex;
    USHORT InitOrderIndex; USHORT LoadCount; USHORT OffsetToFileName;
    UCHAR FullPathName[256];
} RTL_PROCESS_MODULE_INFORMATION;

typedef struct _RTL_PROCESS_MODULES {
    ULONG NumberOfModules;
    RTL_PROCESS_MODULE_INFORMATION Modules[1];
} RTL_PROCESS_MODULES;

typedef struct _PEB_LDR_DATA_X {
    ULONG Length; UCHAR Initialized; PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
} PEB_LDR_DATA_X;

typedef struct _LDR_ENTRY_X {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase; PVOID EntryPoint; ULONG SizeOfImage;
    UNICODE_STRING FullDllName; UNICODE_STRING BaseDllName;
} LDR_ENTRY_X;

typedef struct _PEB_X {
    UCHAR Reserved1[2]; UCHAR BeingDebugged; UCHAR Reserved2[1];
    PVOID Reserved3[2];
    PEB_LDR_DATA_X* Ldr;
} PEB_X;

static DWORD64 g_HookAddr = 0;
static WmCommData g_Comm{};
static PEPROCESS g_ClientProc = nullptr;
static bool g_Connected = false;

static void ksleep(ULONG ms) {
    LARGE_INTEGER li; li.QuadPart = -10000LL * (LONGLONG)ms;
    KeDelayExecutionThread(KernelMode, FALSE, &li);
}

/* The hook and the poll loop both run on the client's syscall thread, so the
 * client's address space is the current one. Probe the user range and copy
 * directly; MmCopyVirtualMemory with UserMode would reject the kernel-side
 * bounce buffers used here. */
static bool copy_from_user(PVOID dst, PVOID src, SIZE_T n) {
    if (!src || !dst || !n) return false;
    __try {
        ProbeForRead(src, n, 1);
        RtlCopyMemory(dst, src, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
static bool copy_to_user(PVOID dst, PVOID src, SIZE_T n) {
    if (!src || !dst || !n) return false;
    __try {
        ProbeForWrite(dst, n, 1);
        RtlCopyMemory(dst, src, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static void set_status(uint16_t s) {
    copy_to_user((PVOID)g_Comm.pStatus, &s, sizeof(s));
}
static uint32_t get_code() {
    uint32_t c = 0;
    copy_from_user(&c, (PVOID)g_Comm.pCode, sizeof(c));
    return c;
}
static void set_code(uint32_t c) {
    copy_to_user((PVOID)g_Comm.pCode, &c, sizeof(c));
}

static PEPROCESS lookup_process(ULONG pid) {
    PEPROCESS p = nullptr;
    PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &p);
    return p;
}

static void op_base(WmOpData* op) {
    PEPROCESS p = lookup_process(op->TargetPid);
    if (!p) { op->NtStatus = STATUS_NOT_FOUND; return; }
    op->Result = (uint64_t)PsGetProcessSectionBaseAddress(p);
    ObfDereferenceObject(p);
    op->NtStatus = op->Result ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static void op_peb(WmOpData* op) {
    PEPROCESS p = lookup_process(op->TargetPid);
    if (!p) { op->NtStatus = STATUS_NOT_FOUND; return; }
    op->Result = (uint64_t)PsGetProcessPeb(p);
    ObfDereferenceObject(p);
    op->NtStatus = op->Result ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static void op_copy(WmOpData* op) {
    if (op->Size == 0 || op->Size > 0x10000) { op->NtStatus = STATUS_INVALID_PARAMETER; return; }
    PEPROCESS p = lookup_process(op->TargetPid);
    if (!p) { op->NtStatus = STATUS_NOT_FOUND; return; }
    SIZE_T ret = 0;
    if (op->ReadOp) {
        op->NtStatus = MmCopyVirtualMemory(p, (PVOID)op->Address,
            g_ClientProc, (PVOID)op->BufferPtr, (SIZE_T)op->Size, UserMode, &ret);
    } else {
        op->NtStatus = MmCopyVirtualMemory(g_ClientProc, (PVOID)op->BufferPtr,
            p, (PVOID)op->Address, (SIZE_T)op->Size, UserMode, &ret);
    }
    op->Size = ret;
    ObfDereferenceObject(p);
}

static void op_alloc(WmOpData* op) {
    PEPROCESS p = lookup_process(op->TargetPid);
    if (!p) { op->NtStatus = STATUS_NOT_FOUND; return; }
    KAPC_STATE apc{};
    KeStackAttachProcess(p, &apc);
    PVOID base = (PVOID)op->Address;
    SIZE_T sz = (SIZE_T)op->Size;
    op->NtStatus = ZwAllocateVirtualMemory(ZwCurrentProcess(), &base, 0, &sz,
                                           op->AllocType, op->Protect);
    op->Result = (uint64_t)base;
    op->Size = sz;
    KeUnstackDetachProcess(&apc);
    ObfDereferenceObject(p);
}

static void op_free(WmOpData* op) {
    PEPROCESS p = lookup_process(op->TargetPid);
    if (!p) { op->NtStatus = STATUS_NOT_FOUND; return; }
    KAPC_STATE apc{};
    KeStackAttachProcess(p, &apc);
    PVOID base = (PVOID)op->Address;
    SIZE_T sz = (SIZE_T)op->Size;
    op->NtStatus = ZwFreeVirtualMemory(ZwCurrentProcess(), &base, &sz, op->FreeType);
    KeUnstackDetachProcess(&apc);
    ObfDereferenceObject(p);
}

static void op_protect(WmOpData* op) {
    PEPROCESS p = lookup_process(op->TargetPid);
    if (!p) { op->NtStatus = STATUS_NOT_FOUND; return; }
    KAPC_STATE apc{};
    KeStackAttachProcess(p, &apc);
    PVOID base = (PVOID)op->Address;
    SIZE_T sz = (SIZE_T)op->Size;
    ULONG old = 0;
    op->NtStatus = ZwProtectVirtualMemory(ZwCurrentProcess(), &base, &sz, op->Protect, &old);
    op->ProtectOut = old;
    KeUnstackDetachProcess(&apc);
    ObfDereferenceObject(p);
}

static void op_query(WmOpData* op) {
    PEPROCESS p = lookup_process(op->TargetPid);
    if (!p) { op->NtStatus = STATUS_NOT_FOUND; return; }
    KAPC_STATE apc{};
    KeStackAttachProcess(p, &apc);
    MEMORY_BASIC_INFORMATION mbi{};
    SIZE_T ret = 0;
    op->NtStatus = ZwQueryVirtualMemory(ZwCurrentProcess(), (PVOID)op->Address,
                                        MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
    if (NT_SUCCESS(op->NtStatus)) {
        op->RegionBase = (uint64_t)mbi.BaseAddress;
        op->RegionSize = (uint64_t)mbi.RegionSize;
        op->State = mbi.State;
        op->ProtectOut = mbi.Protect;
        op->Type = mbi.Type;
    }
    KeUnstackDetachProcess(&apc);
    ObfDereferenceObject(p);
}

static void op_module(WmOpData* op) {
    PEPROCESS p = lookup_process(op->TargetPid);
    if (!p) { op->NtStatus = STATUS_NOT_FOUND; return; }
    KAPC_STATE apc{};
    KeStackAttachProcess(p, &apc);
    __try {
        PEB_X* peb = (PEB_X*)PsGetProcessPeb(p);
        if (!peb || !peb->Ldr) { op->NtStatus = STATUS_NOT_FOUND; }
        else {
            LIST_ENTRY* head = &peb->Ldr->InLoadOrderModuleList;
            LIST_ENTRY* cur = head->Flink;
            op->NtStatus = STATUS_NOT_FOUND;
            for (int i = 0; i < 512 && cur != head; ++i) {
                LDR_ENTRY_X* e = CONTAINING_RECORD(cur, LDR_ENTRY_X, InLoadOrderLinks);
                if (e->BaseDllName.Length > 0 && e->BaseDllName.Buffer &&
                    e->BaseDllName.Length == (USHORT)op->ModuleNameLen &&
                    _wcsnicmp(e->BaseDllName.Buffer, (const wchar_t*)op->ModuleName,
                              op->ModuleNameLen / 2) == 0) {
                    op->Result = (uint64_t)e->DllBase;
                    op->Size = e->SizeOfImage;
                    op->NtStatus = STATUS_SUCCESS;
                    break;
                }
                cur = cur->Flink;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        op->NtStatus = GetExceptionCode();
    }
    KeUnstackDetachProcess(&apc);
    ObfDereferenceObject(p);
}

static void op_batch(WmOpData* /*unused*/) {
    WmBatchHeader hdr{};
    PVOID hdrAddr = (PVOID)(g_Comm.Buffer + WM_OFF_BHDR);
    if (!copy_from_user(&hdr, hdrAddr, sizeof(hdr))) return;
    if (hdr.Count == 0 || hdr.Count > WM_BATCH_MAX ||
        hdr.TotalBytes == 0 || hdr.TotalBytes > WM_BATCH_DATA) return;

    PEPROCESS p = lookup_process(hdr.TargetPid);
    if (!p) return;

    /* One page-sized bounce buffer reused for every item keeps the stack small. */
    static uint8_t tmp[0x1000];

    uint64_t dataAddr = g_Comm.Buffer + WM_OFF_BDATA;
    PVOID statAddr = (PVOID)(g_Comm.Buffer + WM_OFF_BSTAT);
    uint8_t flags[WM_BATCH_MAX]{};
    uint32_t off = 0;

    for (uint32_t i = 0; i < hdr.Count; ++i) {
        uint32_t size = hdr.Items[i].Size;
        if (size == 0 || size > WM_BATCH_DATA ||
            off + size > WM_BATCH_DATA ||
            hdr.Items[i].Offset != off) {
            off += size;
            continue;
        }
        uint32_t done = 0;
        int ok = 1;
        while (done < size) {
            uint32_t chunk = size - done;
            if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
            SIZE_T ret = 0;
            NTSTATUS st = MmCopyVirtualMemory(
                p, (PVOID)(hdr.Items[i].Address + done),
                IoGetCurrentProcess(), tmp, chunk, UserMode, &ret);
            if (!NT_SUCCESS(st) || ret != chunk) { ok = 0; break; }
            if (!copy_to_user((PVOID)(dataAddr + off + done), tmp, chunk)) { ok = 0; break; }
            done += chunk;
        }
        if (ok) flags[i] = 1;
        off += size;
    }
    copy_to_user(statAddr, flags, hdr.Count);
    ObfDereferenceObject(p);
}

static void op_context(WmOpData* op) {
    PETHREAD thread = nullptr;
    NTSTATUS st = PsLookupThreadByThreadId((HANDLE)(ULONG_PTR)op->ThreadId, &thread);
    if (!NT_SUCCESS(st) || !thread) { op->NtStatus = STATUS_NOT_FOUND; op->CtxOk = 0; return; }
    op->CtxOk = 0;
    op->NtStatus = STATUS_NOT_IMPLEMENTED;
    ObfDereferenceObject(thread);
}

static void respond() {
    WmOpData op{};
    PVOID opAddr = (PVOID)(g_Comm.Buffer + WM_OFF_OP);
    if (!copy_from_user(&op, opAddr, sizeof(op))) return;

    uint32_t code = get_code();
    switch (code) {
        case WMSC_BASE:    op_base(&op);    break;
        case WMSC_PEB:     op_peb(&op);     break;
        case WMSC_COPY:    op_copy(&op);    break;
        case WMSC_ALLOC:   op_alloc(&op);   break;
        case WMSC_FREE:    op_free(&op);    break;
        case WMSC_PROTECT: op_protect(&op); break;
        case WMSC_QUERY:   op_query(&op);   break;
        case WMSC_MODULE:  op_module(&op);  break;
        case WMSC_BATCH:   op_batch(&op);   break;
        case WMSC_CONTEXT: op_context(&op); break;
        default: op.NtStatus = STATUS_INVALID_DEVICE_REQUEST; break;
    }

    if (code != WMSC_BATCH)
        copy_to_user(opAddr, &op, sizeof(op));
    set_code(WMSC_COMPLETE);
    set_status(WMSS_ACTIVE);
}

static void shared_loop() {
    g_ClientProc = lookup_process(g_Comm.ProcessId);
    if (!g_ClientProc) return;
    g_Connected = true;

    /* Publish readiness BEFORE polling. The client's connect loop waits for
     * exactly this pair; the zeroed buffer stays INACTIVE without it. */
    set_code(WMSC_COMPLETE);
    set_status(WMSS_ACTIVE);

    for (;;) {
        uint16_t st = 0;

        /* A failed read means the client unmapped its buffer or exited. The
         * status word is the only lifeline; treat loss of it as a disconnect
         * instead of polling a dead address forever. */
        if (!copy_from_user(&st, (PVOID)g_Comm.pStatus, sizeof(st))) break;

        if (st == WMSS_EXIT) {
            set_status(WMSS_INACTIVE);
            break;
        }
        if (st == WMSS_WAITING) {
            respond();
            continue;
        }
        ksleep(st == WMSS_ACTIVE ? 1 : 50);
    }

    ObfDereferenceObject(g_ClientProc);
    g_ClientProc = nullptr;
    g_Connected = false;
}

/* NtConvertBetweenAuxiliaryCounterAndPerformanceCounter(flags, &value, &perf,
 * &err) reads the qword at &value and dispatches the converter with that
 * qword in RCX. The client stores the WmCommData address in `value`, so the
 * hook receives the comm block pointer directly in A1. */
static bool read_comm(PVOID p, WmCommData* out) {
    if (!p) return false;
    if (!copy_from_user(out, p, sizeof(*out))) return false;

    if (out->Magic != WM_MAGIC) return false;
    if (out->Buffer < 0x10000ULL || out->Buffer >= 0x00007FFFFFFFFFFFULL) return false;
    if (out->pCode   != out->Buffer + WM_OFF_CODE ||
        out->pStatus != out->Buffer + WM_OFF_STATUS)
        return false;
    return true;
}

static INT64 NTAPI HookConverter(ULONG64 Value, PULONG64 Perf, PULONG64 Err) {
    if (ExGetPreviousMode() != UserMode || g_Connected)
        return g_OrigConverter(Value, Perf, Err);

    WmCommData comm{};
    if (!read_comm((PVOID)(ULONG_PTR)Value, &comm))
        return g_OrigConverter(Value, Perf, Err);

    g_Comm = comm;

    /* The slot stays pointing at HookConverter for the lifetime of the
     * mapping. The driver is never unloaded, so restoring the old pointer on
     * connect made the first disconnect permanently deafen the transport. */
    shared_loop();

    /* The session consumed the call; report success without invoking the
     * original converter (the "value" was a pointer, not a counter). */
    if (Perf) *Perf = 0;
    if (Err)  *Err = 0;
    return 0;
}

static NTSTATUS initialize() {
    PVOID original = nullptr;
    PVOID slot = wm_find_converter_slot(&original);

    g_DiagStage = wm_last_stage();
    g_DiagLoads = wm_load_count();
    g_DiagFnRva = wm_func_rva();
    g_DiagSlot  = (ULONG_PTR)slot;

    /* Customer-bit status carries the failing stage in the low nibble, so the
     * mapper's existing "DriverEntry returned 0x..." line names the reason.
     * 0xE0000001..0xE000000A map to the WMST_* enum in wm_resolver.cpp. */
    if (!slot || !original) {
        ULONG st = g_DiagStage ? g_DiagStage : 10;
        return (NTSTATUS)(0xE0000000u | (st & 0xFu));
    }

    g_HookAddr = (DWORD64)slot;
    g_OrigConverter = (decltype(g_OrigConverter))
        InterlockedExchangePointer((PVOID*)slot, (PVOID)HookConverter);
    if (!g_OrigConverter) {
        InterlockedExchangePointer((PVOID*)slot, original);
        g_HookAddr = 0;
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING) {
    return initialize();
}