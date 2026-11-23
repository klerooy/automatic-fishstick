/* wm_resolver.cpp - locate the kernel function-pointer slot that
 * NtConvertBetweenAuxiliaryCounterAndPerformanceCounter calls indirectly.
 *
 * Build-independent: anchored on the export name, decodes every RIP-relative
 * load form, does not require the target section to be writable, and follows
 * export thunks. Reports a stage code so a failure is diagnosable.
 */
#include <ntifs.h>
#include <ntddk.h>
#include <ntimage.h>
#include <stdint.h>

extern "C" {
NTSYSCALLAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG, PVOID, ULONG, PULONG);
}

/* Stage codes returned through wm_last_stage(). */
enum {
    WMST_OK = 0,
    WMST_NO_LIST_LEN = 1,
    WMST_POOL_FAIL = 2,
    WMST_NO_NTOSKRNL = 3,
    WMST_BAD_PE = 4,
    WMST_NO_SECTIONS = 5,
    WMST_NO_EXPORT_DIR = 6,
    WMST_NO_EXPORT_NAME = 7,
    WMST_SPAN_OOB = 8,
    WMST_NO_RIP_LOAD = 9,
    WMST_NO_CODE_SLOT = 10
};

static ULONG g_wm_stage = WMST_OK;
static ULONG g_wm_loads = 0;      /* RIP-relative loads seen in the body */
static ULONG g_wm_fnrva = 0;      /* resolved function RVA (after thunks) */

extern "C" ULONG wm_last_stage(void) { return g_wm_stage; }
extern "C" ULONG wm_load_count(void) { return g_wm_loads; }
extern "C" ULONG wm_func_rva(void)   { return g_wm_fnrva; }

typedef struct _WM_MODINFO {
    HANDLE Section; PVOID MappedBase; PVOID ImageBase;
    ULONG ImageSize; ULONG Flags; USHORT LoadOrderIndex;
    USHORT InitOrderIndex; USHORT LoadCount; USHORT OffsetToFileName;
    UCHAR FullPathName[256];
} WM_MODINFO;
typedef struct _WM_MODS { ULONG Count; WM_MODINFO Modules[1]; } WM_MODS;

typedef struct { ULONG Va, Vsize, Chars; } WM_SEC;
typedef struct { PVOID Base; ULONG Size; WM_SEC Sec[64]; ULONG SecCount; } WM_IMG;

static int wm_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        ++a; ++b;
    }
    return *a == *b;
}
static int wm_ends(const char* s, const char* suf) {
    size_t ls = 0, lf = 0;
    while (s[ls]) ++ls;
    while (suf[lf]) ++lf;
    return lf <= ls && wm_ieq(s + (ls - lf), suf);
}

static int wm_get_image(WM_IMG* out) {
    ULONG need = 0;
    ZwQuerySystemInformation(11, nullptr, 0, &need);
    if (!need) need = 0x40000;          /* some builds do not report a length */
    need += 0x4000;

    WM_MODS* m = (WM_MODS*)ExAllocatePool2(POOL_FLAG_NON_PAGED, need, 'rW');
    if (!m) { g_wm_stage = WMST_POOL_FAIL; return 0; }

    int ok = 0;
    ULONG got = 0;
    if (NT_SUCCESS(ZwQuerySystemInformation(11, m, need, &got))) {
        for (ULONG i = 0; i < m->Count; ++i) {
            char path[256]; ULONG k = 0;
            for (; k < 255 && m->Modules[i].FullPathName[k]; ++k)
                path[k] = (char)m->Modules[i].FullPathName[k];
            path[k] = 0;
            if (!wm_ends(path, "ntoskrnl.exe")) continue;
            out->Base = m->Modules[i].ImageBase;
            out->Size = m->Modules[i].ImageSize;
            ok = 1; break;
        }
    }
    ExFreePoolWithTag(m, 'rW');
    if (!ok || !out->Base || !out->Size) { g_wm_stage = WMST_NO_NTOSKRNL; return 0; }

    auto* dos = (IMAGE_DOS_HEADER*)out->Base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { g_wm_stage = WMST_BAD_PE; return 0; }
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)out->Base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { g_wm_stage = WMST_BAD_PE; return 0; }

    out->SecCount = 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (USHORT i = 0; i < nt->FileHeader.NumberOfSections && i < 64; ++i) {
        ULONG va = sec[i].VirtualAddress, vs = sec[i].Misc.VirtualSize;
        if (!vs || va >= out->Size) continue;
        out->Sec[out->SecCount].Va = va;
        out->Sec[out->SecCount].Vsize = vs;
        out->Sec[out->SecCount].Chars = sec[i].Characteristics;
        ++out->SecCount;
    }
    if (!out->SecCount) { g_wm_stage = WMST_NO_SECTIONS; return 0; }
    return 1;
}

static ULONG wm_export_rva(const WM_IMG* img, const char* name) {
    auto* dos = (IMAGE_DOS_HEADER*)img->Base;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)img->Base + dos->e_lfanew);
    ULONG dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!dir || dir >= img->Size) { g_wm_stage = WMST_NO_EXPORT_DIR; return 0; }

    auto* exp = (IMAGE_EXPORT_DIRECTORY*)((uint8_t*)img->Base + dir);
    ULONG nn = exp->NumberOfNames;
    if (!nn || nn > 0x20000) { g_wm_stage = WMST_NO_EXPORT_DIR; return 0; }

    auto* names = (ULONG*)((uint8_t*)img->Base + exp->AddressOfNames);
    auto* ords  = (USHORT*)((uint8_t*)img->Base + exp->AddressOfNameOrdinals);
    auto* funcs = (ULONG*)((uint8_t*)img->Base + exp->AddressOfFunctions);

    for (ULONG i = 0; i < nn; ++i) {
        ULONG nr = names[i];
        if (!nr || nr >= img->Size) continue;
        const char* s = (const char*)((uint8_t*)img->Base + nr);
        ULONG limit = img->Size - nr, k = 0;
        while (k < limit && s[k] && name[k]) { if (s[k] != name[k]) break; ++k; }
        if (name[k] == 0 && (k >= limit || s[k] == 0)) {
            USHORT o = ords[i];
            if (o >= exp->NumberOfFunctions) continue;
            ULONG fr = funcs[o];
            if (fr && fr < img->Size) return fr;
        }
    }
    g_wm_stage = WMST_NO_EXPORT_NAME;
    return 0;
}

/* A slot must hold a pointer into an EXECUTABLE section of ntoskrnl.
 * The section holding the slot itself may be read-only: HAL dispatch tables
 * are not guaranteed to carry IMAGE_SCN_MEM_WRITE. */
static int wm_in_exec(const WM_IMG* img, ULONG rva) {
    for (ULONG i = 0; i < img->SecCount; ++i)
        if ((img->Sec[i].Chars & IMAGE_SCN_MEM_EXECUTE) &&
            rva >= img->Sec[i].Va && rva < img->Sec[i].Va + img->Sec[i].Vsize)
            return 1;
    return 0;
}
static int wm_code_ptr(const WM_IMG* img, PVOID fn) {
    ULONG_PTR b = (ULONG_PTR)img->Base, p = (ULONG_PTR)fn;
    if (p < b || p >= b + img->Size) return 0;
    return wm_in_exec(img, (ULONG)(p - b));
}

/* Follow up to four near jumps / indirect jumps so an export thunk does not
 * hide the real body from the scan window. */
static ULONG wm_follow_thunks(const WM_IMG* img, ULONG rva) {
    uint8_t* base = (uint8_t*)img->Base;
    for (int hop = 0; hop < 4; ++hop) {
        if (rva + 8 > img->Size) break;
        uint8_t op = base[rva];
        if (op == 0xE9 || op == 0xE8) {
            int32_t d = *(int32_t*)(base + rva + 1);
            ULONG nxt = (ULONG)((LONG64)rva + 5 + d);
            if (nxt && nxt + 8 < img->Size) { rva = nxt; continue; }
        }
        if (op == 0xFF && base[rva + 1] == 0x25) {
            int32_t d = *(int32_t*)(base + rva + 2);
            ULONG pr = (ULONG)((LONG64)rva + 6 + d);
            if (pr + 8 < img->Size) {
                ULONG_PTR tgt = *(ULONG_PTR*)(base + pr);
                if (tgt >= (ULONG_PTR)img->Base &&
                    tgt < (ULONG_PTR)img->Base + img->Size) {
                    rva = (ULONG)(tgt - (ULONG_PTR)img->Base);
                    continue;
                }
            }
        }
        break;
    }
    return rva;
}

/* Scan one exported function body for a RIP-relative load whose target word
 * holds a code pointer, i.e. the indirection slot the function dispatches
 * through. Returns the slot address (absolute) or nullptr. */
static PVOID wm_scan_slot(const WM_IMG* img, ULONG rva, PVOID* out_original) {
    const ULONG span = 0x800;
    if (rva + span > img->Size) { g_wm_stage = WMST_SPAN_OOB; return nullptr; }

    uint8_t* fn = (uint8_t*)img->Base + rva;
    ULONG_PTR base = (ULONG_PTR)img->Base;

    PVOID best = nullptr; int best_score = -1;
    ULONG saw_load = 0;

    for (ULONG i = 0; i + 7 <= span; ++i) {
        uint8_t rex = fn[i], op = fn[i + 1], modrm = fn[i + 2];
        /* REX.W mov/lea with a RIP-relative operand: mod=00, rm=101.
         * (modrm & 0xC7) == 0x05 accepts every destination register. */
        if (rex != 0x48 && rex != 0x4C) continue;
        if (op != 0x8B && op != 0x8D) continue;
        if ((modrm & 0xC7) != 0x05) continue;

        ++saw_load;
        int32_t disp = *(int32_t*)(fn + i + 3);
        uint8_t* target = fn + i + 7 + disp;
        if ((ULONG_PTR)target < base || (ULONG_PTR)target + 8 > base + img->Size) continue;

        ULONG slot_rva = (ULONG)((ULONG_PTR)target - base);

        /* Optional index load right after: mov reg,[base_reg + disp].
         * Any base register except the SIB/rip encoded forms. */
        if (i + 14 <= span) {
            uint8_t* q = fn + i + 7;
            if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8B) {
                uint8_t mm = q[2], mod = mm & 0xC0, rm = mm & 7;
                if (rm != 4 && rm != 5) {
                    if (mod == 0x40)      slot_rva += (ULONG)*(int8_t*)(q + 3);
                    else if (mod == 0x80) slot_rva += (ULONG)*(int32_t*)(q + 3);
                }
            }
        }
        if (slot_rva & 7) continue;
        if (slot_rva + 8 > img->Size) continue;

        PVOID cur = *(PVOID*)(base + slot_rva);
        if (!wm_code_ptr(img, cur)) continue;

        int score = 0;
        for (ULONG k = 7; k < 40 && i + k + 2 <= span; ++k) {
            if (fn[i + k] == 0xE8) { score = 2; break; }
            if (fn[i + k] == 0xFF &&
                (fn[i + k + 1] == 0xD0 || fn[i + k + 1] == 0xD1 ||
                 fn[i + k + 1] == 0xD2 || fn[i + k + 1] == 0x15)) { score = 2; break; }
        }
        if (score > best_score) {
            best_score = score;
            best = (PVOID)(base + slot_rva);
        }
    }

    g_wm_loads = saw_load;
    if (!saw_load) { g_wm_stage = WMST_NO_RIP_LOAD; return nullptr; }
    if (!best)     { g_wm_stage = WMST_NO_CODE_SLOT; return nullptr; }

    g_wm_stage = WMST_OK;
    if (out_original) *out_original = *(PVOID*)best;
    return best;
}

/* ntoskrnl does not export the Nt* syscall entry points (they are reached
 * through the service table), so the build-stable anchor is the exported Ke*
 * implementation the syscall wrapper lands in. The Nt* name is kept first
 * for builds that do export a thunk under that name. */
static const char* const k_wm_anchors[] = {
    "KeConvertAuxiliaryCounterToPerformanceCounter",
    "NtConvertBetweenAuxiliaryCounterAndPerformanceCounter"
};

PVOID wm_find_converter_slot(PVOID* out_original) {
    if (out_original) *out_original = nullptr;
    g_wm_stage = WMST_OK; g_wm_loads = 0; g_wm_fnrva = 0;

    WM_IMG img{};
    if (!wm_get_image(&img)) return nullptr;

    ULONG fail_stage = 0;

    for (ULONG a = 0; a < (ULONG)(sizeof(k_wm_anchors) / sizeof(k_wm_anchors[0])); ++a) {
        ULONG rva = wm_export_rva(&img, k_wm_anchors[a]);
        if (!rva) {
            if (!fail_stage) fail_stage = g_wm_stage;
            continue;
        }
        rva = wm_follow_thunks(&img, rva);
        g_wm_fnrva = rva;

        PVOID slot = wm_scan_slot(&img, rva, out_original);
        if (slot) return slot;
        if (!fail_stage) fail_stage = g_wm_stage;
    }

    g_wm_stage = fail_stage ? fail_stage : WMST_NO_EXPORT_NAME;
    return nullptr;
}