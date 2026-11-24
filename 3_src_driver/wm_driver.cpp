/* wm_driver.cpp - WmShared: no-hook kernel memory server for RobRunPort.
 * Transport: registry handshake + shared-buffer polling via system thread.
 * No function pointer hooks. No device objects. No syscall interception.
 *
 * Buffer access is done with MmCopyVirtualMemory against the client EPROCESS
 * that the handshake pins. The session thread never calls KeStackAttachProcess:
 * a client that exits (or is killed) must not take the kernel down with it, so
 * every user-buffer touch is a bounded cross-process copy that fails closed.
 */
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
    NTSYSCALLAPI NTSTATUS NTAPI ZwAllocateVirtualMemory(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
    NTSYSCALLAPI NTSTATUS NTAPI ZwFreeVirtualMemory(HANDLE, PVOID*, PSIZE_T, ULONG);
    NTSYSCALLAPI NTSTATUS NTAPI ZwQueryVirtualMemory(HANDLE, PVOID, MEMORY_INFORMATION_CLASS, PVOID, SIZE_T, PSIZE_T);
    NTSYSCALLAPI NTSTATUS NTAPI ZwOpenEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    NTSYSCALLAPI NTSTATUS NTAPI ZwOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
    NTSYSCALLAPI NTSTATUS NTAPI ZwQueryValueKey(HANDLE, PUNICODE_STRING, KEY_VALUE_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    NTSYSCALLAPI NTSTATUS NTAPI ZwDeleteValueKey(HANDLE, PUNICODE_STRING);
    NTSYSCALLAPI NTSTATUS NTAPI ZwClose(HANDLE);
    extern POBJECT_TYPE* ExEventObjectType;
}

#include "wm_shared.h"

#define ETHREAD_WIN32START_OFFSET 0x6B8

#define WM_REG_PATH  L"\\Registry\\Machine\\SYSTEM\\WmPort"
#define WM_REG_VALUE L"Session"
#define WM_WAKE_PATH L"\\BaseNamedObjects\\WmSharedWake"

#ifndef PROCESS_VM_OPERATION
#define PROCESS_VM_OPERATION 0x0008
#endif
#ifndef PROCESS_QUERY_INFORMATION
#define PROCESS_QUERY_INFORMATION 0x0400
#endif

typedef struct _WM_SYSTEM_PROCESS_INFORMATION {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
} WM_SPI;

#pragma pack(push, 1)
typedef struct _WM_SESSION { uint32_t Pid; uint64_t BufferVa; } WM_SESSION;
#pragma pack(pop)

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

static WmCommData g_Comm{};
static PEPROCESS g_ClientProc = nullptr;
static PKEVENT   g_WakeEvent = nullptr;
static PKEVENT   g_LiveEvent = nullptr;
static volatile bool g_Connected = false;
static volatile bool g_Shutdown = false;

static void ksleep(ULONG ms) {
    LARGE_INTEGER li; li.QuadPart = -10000LL * (LONGLONG)ms;
    KeDelayExecutionThread(KernelMode, FALSE, &li);
}

static bool client_copy(PVOID local, uint64_t va, SIZE_T n, bool to_client) {
    if (!g_ClientProc || !local || !n) return false;
    if (va < 0x10000ULL || va >= 0x7FFFFFFFFFFFULL) return false;
    SIZE_T done = 0;
    NTSTATUS st = to_client
        ? MmCopyVirtualMemory(PsGetCurrentProcess(), local, g_ClientProc, (PVOID)va, n, KernelMode, &done)
        : MmCopyVirtualMemory(g_ClientProc, (PVOID)va, PsGetCurrentProcess(), local, n, KernelMode, &done);
    return NT_SUCCESS(st) && done == n;
}

static void set_status(uint16_t s) { client_copy(&s, g_Comm.pStatus, sizeof(s), true); }
static uint32_t get_code() { uint32_t c = 0; client_copy(&c, g_Comm.pCode, sizeof(c), false); return c; }
static void set_code(uint32_t c) { client_copy(&c, g_Comm.pCode, sizeof(c), true); }

static PEPROCESS lookup_process(ULONG pid) {
    PEPROCESS p = nullptr;
    if (!pid) return nullptr;
    if (!NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &p))) return nullptr;
    return p;
}

static HANDLE open_target(uint32_t pid) {
    PEPROCESS p = lookup_process(pid);
    if (!p) return nullptr;
    HANDLE h = nullptr;
    NTSTATUS st = ObOpenObjectByPointer(p, OBJ_KERNEL_HANDLE, nullptr,
                                        PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
                                        *PsProcessType, KernelMode, &h);
    ObfDereferenceObject(p);
    return NT_SUCCESS(st) ? h : nullptr;
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
    if (!g_ClientProc) { op->NtStatus = STATUS_DEVICE_NOT_CONNECTED; return; }
    PEPROCESS p = lookup_process(op->TargetPid);
    if (!p) { op->NtStatus = STATUS_NOT_FOUND; return; }
    SIZE_T ret = 0;
    if (op->ReadOp)
        op->NtStatus = MmCopyVirtualMemory(p, (PVOID)op->Address, g_ClientProc, (PVOID)op->BufferPtr, (SIZE_T)op->Size, UserMode, &ret);
    else
        op->NtStatus = MmCopyVirtualMemory(g_ClientProc, (PVOID)op->BufferPtr, p, (PVOID)op->Address, (SIZE_T)op->Size, UserMode, &ret);
    op->Size = ret;
    ObfDereferenceObject(p);
}

static void op_alloc(WmOpData* op) {
    HANDLE h = open_target(op->TargetPid);
    if (!h) { op->NtStatus = STATUS_NOT_FOUND; return; }
    PVOID base = (PVOID)op->Address; SIZE_T sz = (SIZE_T)op->Size;
    op->NtStatus = ZwAllocateVirtualMemory(h, &base, 0, &sz, op->AllocType, op->Protect);
    op->Result = (uint64_t)base; op->Size = sz;
    ZwClose(h);
}

static void op_free(WmOpData* op) {
    HANDLE h = open_target(op->TargetPid);
    if (!h) { op->NtStatus = STATUS_NOT_FOUND; return; }
    PVOID base = (PVOID)op->Address; SIZE_T sz = (SIZE_T)op->Size;
    op->NtStatus = ZwFreeVirtualMemory(h, &base, &sz, op->FreeType);
    ZwClose(h);
}

static void op_protect(WmOpData* op) {
    HANDLE h = open_target(op->TargetPid);
    if (!h) { op->NtStatus = STATUS_NOT_FOUND; return; }
    PVOID base = (PVOID)op->Address; SIZE_T sz = (SIZE_T)op->Size; ULONG old = 0;
    op->NtStatus = ZwProtectVirtualMemory(h, &base, &sz, op->Protect, &old);
    op->ProtectOut = old;
    ZwClose(h);
}

static void op_query(WmOpData* op) {
    HANDLE h = open_target(op->TargetPid);
    if (!h) { op->NtStatus = STATUS_NOT_FOUND; return; }
    MEMORY_BASIC_INFORMATION mbi{}; SIZE_T ret = 0;
    op->NtStatus = ZwQueryVirtualMemory(h, (PVOID)op->Address, MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
    if (NT_SUCCESS(op->NtStatus)) {
        op->RegionBase = (uint64_t)mbi.BaseAddress; op->RegionSize = (uint64_t)mbi.RegionSize;
        op->State = mbi.State; op->ProtectOut = mbi.Protect; op->Type = mbi.Type;
    }
    ZwClose(h);
}

static void op_module(WmOpData* op) {
    PEPROCESS p = lookup_process(op->TargetPid);
    if (!p) { op->NtStatus = STATUS_NOT_FOUND; return; }
    __try {
        PVOID pebAddr = (PVOID)PsGetProcessPeb(p);
        if (!pebAddr) { op->NtStatus = STATUS_NOT_FOUND; __leave; }
        SIZE_T ret = 0; NTSTATUS st;
        uint64_t ldrPtr = 0;
        st = MmCopyVirtualMemory(p, (PVOID)((ULONG_PTR)pebAddr + 0x18), IoGetCurrentProcess(), &ldrPtr, sizeof(ldrPtr), KernelMode, &ret);
        if (!NT_SUCCESS(st) || !ldrPtr) { op->NtStatus = STATUS_NOT_FOUND; __leave; }
        LIST_ENTRY head{};
        st = MmCopyVirtualMemory(p, (PVOID)(ldrPtr + 0x10), IoGetCurrentProcess(), &head, sizeof(head), KernelMode, &ret);
        if (!NT_SUCCESS(st)) { op->NtStatus = STATUS_NOT_FOUND; __leave; }
        ULONG_PTR headAddr = ldrPtr + 0x10, cur = (ULONG_PTR)head.Flink;
        op->NtStatus = STATUS_NOT_FOUND;
        for (int i = 0; i < 512 && cur && cur != headAddr; ++i) {
            LIST_ENTRY entry{};
            st = MmCopyVirtualMemory(p, (PVOID)cur, IoGetCurrentProcess(), &entry, sizeof(entry), KernelMode, &ret);
            if (!NT_SUCCESS(st)) break;
            USHORT nameLen = 0;
            st = MmCopyVirtualMemory(p, (PVOID)(cur + 0x58), IoGetCurrentProcess(), &nameLen, sizeof(nameLen), KernelMode, &ret);
            if (!NT_SUCCESS(st) || !nameLen || nameLen > 128) { cur = (ULONG_PTR)entry.Flink; continue; }
            uint64_t bufPtr = 0;
            st = MmCopyVirtualMemory(p, (PVOID)(cur + 0x60), IoGetCurrentProcess(), &bufPtr, sizeof(bufPtr), KernelMode, &ret);
            if (!NT_SUCCESS(st) || !bufPtr) { cur = (ULONG_PTR)entry.Flink; continue; }
            if (nameLen != (USHORT)op->ModuleNameLen) { cur = (ULONG_PTR)entry.Flink; continue; }
            wchar_t nameBuf[64]{};
            st = MmCopyVirtualMemory(p, (PVOID)bufPtr, IoGetCurrentProcess(), nameBuf, nameLen, KernelMode, &ret);
            if (!NT_SUCCESS(st)) { cur = (ULONG_PTR)entry.Flink; continue; }
            UNICODE_STRING got, want;
            RtlInitUnicodeString(&got, nameBuf); got.Length = got.MaximumLength = nameLen;
            RtlInitUnicodeString(&want, (const wchar_t*)op->ModuleName); want.Length = want.MaximumLength = (USHORT)op->ModuleNameLen;
            if (!RtlEqualUnicodeString(&got, &want, TRUE)) { cur = (ULONG_PTR)entry.Flink; continue; }
            uint64_t dllBase = 0; ULONG imgSize = 0;
            MmCopyVirtualMemory(p, (PVOID)(cur + 0x30), IoGetCurrentProcess(), &dllBase, sizeof(dllBase), KernelMode, &ret);
            MmCopyVirtualMemory(p, (PVOID)(cur + 0x40), IoGetCurrentProcess(), &imgSize, sizeof(imgSize), KernelMode, &ret);
            op->Result = dllBase; op->Size = imgSize; op->NtStatus = STATUS_SUCCESS; break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { op->NtStatus = GetExceptionCode(); }
    ObfDereferenceObject(p);
}

static void op_batch(WmOpData*) {
    WmBatchHeader hdr{};
    if (!client_copy(&hdr, g_Comm.Buffer + WM_OFF_BHDR, sizeof(hdr), false)) return;
    if (!hdr.Count || hdr.Count > WM_BATCH_MAX || !hdr.TotalBytes || hdr.TotalBytes > WM_BATCH_DATA) return;
    PEPROCESS p = lookup_process(hdr.TargetPid);
    if (!p) return;
    static uint8_t tmp[0x1000];
    uint64_t dataAddr = g_Comm.Buffer + WM_OFF_BDATA;
    uint8_t flags[WM_BATCH_MAX]{}; uint32_t off = 0;
    for (uint32_t i = 0; i < hdr.Count; ++i) {
        uint32_t size = hdr.Items[i].Size;
        if (!size || size > WM_BATCH_DATA || off + size > WM_BATCH_DATA || hdr.Items[i].Offset != off) { off += size; continue; }
        uint32_t done = 0; int ok = 1;
        while (done < size) {
            uint32_t chunk = size - done; if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
            SIZE_T ret = 0;
            NTSTATUS st = MmCopyVirtualMemory(p, (PVOID)(hdr.Items[i].Address + done), IoGetCurrentProcess(), tmp, chunk, UserMode, &ret);
            if (!NT_SUCCESS(st) || ret != chunk) { ok = 0; break; }
            if (!client_copy(tmp, dataAddr + off + done, chunk, true)) { ok = 0; break; }
            done += chunk;
        }
        if (ok) flags[i] = 1; off += size;
    }
    client_copy(flags, g_Comm.Buffer + WM_OFF_BSTAT, hdr.Count, true);
    ObfDereferenceObject(p);
}

static void op_context(WmOpData* op) {
    PETHREAD t = nullptr;
    NTSTATUS st = PsLookupThreadByThreadId((HANDLE)(ULONG_PTR)op->ThreadId, &t);
    if (!NT_SUCCESS(st) || !t) { op->NtStatus = STATUS_NOT_FOUND; op->CtxOk = 0; return; }
    op->CtxOk = 0; op->NtStatus = STATUS_NOT_IMPLEMENTED;
    ObfDereferenceObject(t);
}

static void op_pid(WmOpData* op) {
    op->Result = 0; op->NtStatus = STATUS_NOT_FOUND;
    uint32_t nameLen = op->ModuleNameLen;
    if (!nameLen || nameLen > sizeof(op->ModuleName)) return;
    wchar_t want[65]{};
    if (nameLen > sizeof(want) - sizeof(want[0])) return;
    RtlCopyMemory(want, op->ModuleName, nameLen);
    UNICODE_STRING wantUs; RtlInitUnicodeString(&wantUs, want);

    ULONG size = 0x10000, ret = 0;
    PVOID buf = nullptr;
    for (int tries = 0; tries < 4; ++tries) {
        buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, 'pW');
        if (!buf) return;
        NTSTATUS st = ZwQuerySystemInformation(5, buf, size, &ret);
        if (NT_SUCCESS(st)) break;
        ExFreePoolWithTag(buf, 'pW'); buf = nullptr;
        if (st != STATUS_INFO_LENGTH_MISMATCH) return;
        size = ret + 0x10000;
        if (size > 0x800000) return;
    }
    if (!buf) return;
    auto* sp = (WM_SPI*)buf;
    for (;;) {
        if (sp->ImageName.Buffer && sp->ImageName.Length) {
            if (RtlEqualUnicodeString(&sp->ImageName, &wantUs, TRUE)) {
                op->Result = (uint64_t)(ULONG_PTR)sp->UniqueProcessId;
                op->NtStatus = STATUS_SUCCESS;
                break;
            }
        }
        if (!sp->NextEntryOffset) break;
        sp = (WM_SPI*)((uint8_t*)sp + sp->NextEntryOffset);
    }
    ExFreePoolWithTag(buf, 'pW');
}

static void respond() {
    WmOpData op{};
    if (!client_copy(&op, g_Comm.Buffer + WM_OFF_OP, sizeof(op), false)) return;
    uint32_t code = get_code();
    switch (code) {
        case WMSC_BASE: op_base(&op); break; case WMSC_PEB: op_peb(&op); break;
        case WMSC_COPY: op_copy(&op); break; case WMSC_ALLOC: op_alloc(&op); break;
        case WMSC_FREE: op_free(&op); break; case WMSC_PROTECT: op_protect(&op); break;
        case WMSC_QUERY: op_query(&op); break; case WMSC_MODULE: op_module(&op); break;
        case WMSC_BATCH: op_batch(&op); break; case WMSC_CONTEXT: op_context(&op); break;
        case WMSC_PID: op_pid(&op); break;
        default: op.NtStatus = STATUS_INVALID_DEVICE_REQUEST; break;
    }
    if (code != WMSC_BATCH) client_copy(&op, g_Comm.Buffer + WM_OFF_OP, sizeof(op), true);
    set_code(WMSC_COMPLETE);
}

static void wait_ack(void) {
    for (int i = 0; i < 400; ++i) {
        uint16_t st = 0;
        if (!client_copy(&st, g_Comm.pStatus, sizeof(st), false)) return;
        if (st != WMSS_WAITING) return;
        KeStallExecutionProcessor(25);
    }
}

static bool read_session_raw(WM_SESSION* out) {
    UNICODE_STRING kp, vn;
    RtlInitUnicodeString(&kp, WM_REG_PATH);
    RtlInitUnicodeString(&vn, WM_REG_VALUE);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &kp, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    HANDLE hKey = nullptr;
    if (!NT_SUCCESS(ZwOpenKey(&hKey, KEY_READ, &oa))) return false;
    uint8_t buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(WM_SESSION)]{};
    ULONG rl = 0;
    NTSTATUS st = ZwQueryValueKey(hKey, &vn, KeyValuePartialInformation, buf, sizeof(buf), &rl);
    ZwClose(hKey);
    if (!NT_SUCCESS(st)) return false;
    auto* kvi = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
    if (kvi->DataLength < sizeof(WM_SESSION)) return false;
    RtlCopyMemory(out, kvi->Data, sizeof(WM_SESSION));
    return true;
}

static bool read_session(WM_SESSION* out) {
    if (!read_session_raw(out)) return false;
    return out->Pid && out->BufferVa >= 0x10000ULL && out->BufferVa < 0x7FFFFFFFFFFFULL;
}

static void clear_session(uint32_t pid) {
    WM_SESSION cur{};
    if (!read_session_raw(&cur) || cur.Pid != pid) return;
    UNICODE_STRING kp, vn;
    RtlInitUnicodeString(&kp, WM_REG_PATH);
    RtlInitUnicodeString(&vn, WM_REG_VALUE);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &kp, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    HANDLE hKey = nullptr;
    if (NT_SUCCESS(ZwOpenKey(&hKey, KEY_SET_VALUE, &oa))) {
        ZwDeleteValueKey(hKey, &vn);
        ZwClose(hKey);
    }
}

static bool validate_comm(uint64_t va, uint32_t pid) {
    WmCommData c{};
    if (!client_copy(&c, va, sizeof(c), false)) return false;
    if (c.Magic != WM_MAGIC || c.ProcessId != pid) return false;
    if (c.Buffer < 0x10000ULL || c.Buffer >= 0x7FFFFFFFFFFFULL) return false;
    if (c.pCode != c.Buffer + WM_OFF_CODE || c.pStatus != c.Buffer + WM_OFF_STATUS) return false;
    g_Comm = c; return true;
}

static void open_wake_event(void) {
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, WM_WAKE_PATH);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    HANDLE h = nullptr;
    if (!NT_SUCCESS(ZwOpenEvent(&h, EVENT_MODIFY_STATE | SYNCHRONIZE, &oa))) return;
    PKEVENT ev = nullptr;
    if (NT_SUCCESS(ObReferenceObjectByHandle(h, EVENT_MODIFY_STATE | SYNCHRONIZE,
                                             *ExEventObjectType, KernelMode,
                                             (PVOID*)&ev, nullptr)) && ev)
        g_WakeEvent = ev;
    ZwClose(h);
}

static void close_wake_event(void) {
    if (g_WakeEvent) { ObDereferenceObject(g_WakeEvent); g_WakeEvent = nullptr; }
}

static void wait_wake(void) {
    if (!g_WakeEvent) { ksleep(2); return; }
    LARGE_INTEGER timeout;
    timeout.QuadPart = -200000LL;
    KeWaitForSingleObject(g_WakeEvent, Executive, KernelMode, FALSE, &timeout);
}

static void session_thread(PVOID) {
    while (!g_Shutdown) {
        WM_SESSION sess{};
        if (!read_session(&sess)) { ksleep(250); continue; }
        PEPROCESS proc = lookup_process(sess.Pid);
        if (!proc) { ksleep(250); continue; }

        g_ClientProc = proc;
        if (!validate_comm(sess.BufferVa, sess.Pid)) {
            g_ClientProc = nullptr;
            ObfDereferenceObject(proc);
            ksleep(250);
            continue;
        }
        g_Connected = true;
        open_wake_event();
        set_code(WMSC_COMPLETE);
        set_status(WMSS_ACTIVE);

        while (!g_Shutdown) {
            uint16_t st = 0;
            if (!client_copy(&st, g_Comm.pStatus, sizeof(st), false)) break;
            if (st == WMSS_EXIT) { set_status(WMSS_INACTIVE); break; }
            if (st == WMSS_WAITING) { respond(); wait_ack(); continue; }
            wait_wake();
        }

        g_Connected = false;
        close_wake_event();
        g_ClientProc = nullptr;
        ObfDereferenceObject(proc);
        clear_session(sess.Pid);
        ksleep(200);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void spoof_thread_start(HANDLE hThread) {
    PETHREAD t = nullptr;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID*)&t, nullptr)) || !t) return;
    ULONG need = 0; ZwQuerySystemInformation(11, nullptr, 0, &need);
    if (need) {
        need += 0x4000;
        auto* m = (RTL_PROCESS_MODULES*)ExAllocatePool2(POOL_FLAG_NON_PAGED, need, 'pW');
        if (m) {
            if (NT_SUCCESS(ZwQuerySystemInformation(11, m, need, &need))) {
                for (ULONG i = 0; i < m->NumberOfModules; ++i) {
                    const char* n = (const char*)(m->Modules[i].FullPathName + m->Modules[i].OffsetToFileName);
                    const char* a = n; const char* b = "ntoskrnl.exe"; bool match = true;
                    while (*a && *b) { char ca=*a, cb=*b; if(ca>='A'&&ca<='Z')ca+=32; if(cb>='A'&&cb<='Z')cb+=32; if(ca!=cb){match=false;break;} ++a;++b; }
                    if (match && *a == *b) {
                        ULONG_PTR* slot = (ULONG_PTR*)((uint8_t*)t + ETHREAD_WIN32START_OFFSET);
                        ULONG_PTR current = *slot;
                        if (current >= 0xFFFF800000000000ULL)
                            *slot = (ULONG_PTR)m->Modules[i].ImageBase + 0x1000;
                        break;
                    }
                }
            }
            ExFreePoolWithTag(m, 'pW');
        }
    }
    ObfDereferenceObject(t);
}

static void create_live_marker(void) {
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\BaseNamedObjects\\WmDriverLive");
    HANDLE hEvent = nullptr;
    g_LiveEvent = IoCreateNotificationEvent(&name, &hEvent);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING) {
    create_live_marker();
    HANDLE hThread = nullptr;
    NTSTATUS st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, nullptr, nullptr, nullptr, session_thread, nullptr);
    if (!NT_SUCCESS(st)) return st;
    spoof_thread_start(hThread);
    ZwClose(hThread);
    return STATUS_SUCCESS;
}