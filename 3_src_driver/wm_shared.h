/* wm_shared.h - Shared protocol between WmShared driver and RobRunPort client.
 * Pure C compatible. Regions are non-overlapping; sizeof(WmOpData)=272.
 */
#ifndef WM_SHARED_H
#define WM_SHARED_H

#include <stdint.h>
#include <stddef.h>

#define WM_MAGIC        0x999u
#define WM_BUFFER_SIZE  0x10000u
#define WM_BATCH_MAX    64
#define WM_BATCH_DATA   0xF000u

/* Buffer layout (verified non-overlapping):
 *   OP     0x0000 .. 0x0110   WmOpData      (272 B)
 *   BHDR   0x0200 .. 0x0610   WmBatchHeader (1040 B)
 *   BDATA  0x0700 .. 0xF700   batch payload (0xF000 B)
 *   BSTAT  0xF700 .. 0xF740   per-item flags (64 B)
 *   CODE   0xF800             uint32 request code
 *   STATUS 0xF804             uint16 status
 */
#define WM_OFF_OP       0x0000u
#define WM_OFF_BHDR     0x0200u
#define WM_OFF_BDATA    0x0700u
#define WM_OFF_BSTAT    0xF700u
#define WM_OFF_CODE     0xF800u
#define WM_OFF_STATUS   0xF804u

enum {
    WMSC_COMPLETE = 0,
    WMSC_BASE     = 1,
    WMSC_PEB      = 2,
    WMSC_COPY     = 3,
    WMSC_ALLOC    = 4,
    WMSC_QUERY    = 5,
    WMSC_MODULE   = 6,
    WMSC_BATCH    = 7,
    WMSC_CONTEXT  = 8,
    WMSC_PROTECT  = 9,
    WMSC_FREE     = 10,
    WMSC_PID      = 11
};

enum {
    WMSS_INACTIVE = 0,
    WMSS_ACTIVE   = 1,
    WMSS_WAITING  = 2,
    WMSS_EXIT     = 3
};

#pragma pack(push, 1)
typedef struct {
    uint32_t ProcessId;
    uint64_t Buffer;
    uint64_t pCode;
    uint64_t pStatus;
    uint32_t Magic;
    uint32_t Pad0;
    uint64_t WakeHandle;   /* client-side handle, informational */
} WmCommData;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    uint32_t TargetPid;
    uint32_t Pad0;
    uint64_t Address;
    uint64_t BufferPtr;
    uint64_t Size;
    uint64_t Result;
    uint32_t Protect;
    uint32_t AllocType;
    uint32_t FreeType;
    uint32_t ReadOp;
    uint32_t ModuleNameLen;
    uint32_t Pad1;
    uint16_t ModuleName[64];
    uint64_t RegionBase;
    uint64_t RegionSize;
    uint32_t State;
    uint32_t ProtectOut;
    uint32_t Type;
    uint32_t Pad2;
    uint32_t ThreadId;
    uint32_t CtxOk;
    uint64_t Rip, Rsp, Rbp, Rflags;
    int32_t  NtStatus;
    uint32_t Pad3;
} WmOpData;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    uint32_t Offset;
    uint32_t Size;
    uint64_t Address;
} WmBatchItem;

typedef struct {
    uint32_t Count;
    uint32_t TotalBytes;
    uint32_t TargetPid;
    uint32_t Pad;
    WmBatchItem Items[WM_BATCH_MAX];
} WmBatchHeader;
#pragma pack(pop)

/* A future field addition must fail the build, not silently overlap. */
typedef char wm_op_fits[(sizeof(WmOpData) <= WM_OFF_BHDR) ? 1 : -1];
typedef char wm_hdr_fits[(WM_OFF_BHDR + sizeof(WmBatchHeader) <= WM_OFF_BDATA) ? 1 : -1];
typedef char wm_data_fits[(WM_OFF_BDATA + WM_BATCH_DATA <= WM_OFF_BSTAT) ? 1 : -1];

#endif /* WM_SHARED_H */