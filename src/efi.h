/* このOSで使う分だけの UEFI 定義 (UEFI Specification に準拠) */
#pragma once
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef short i16;
typedef int i32;
typedef long long i64;
typedef u16 c16;
typedef u64 Status;
typedef void *Handle;
typedef void *Event;

typedef struct { u32 a; u16 b, c; u8 d[8]; } Guid;
typedef struct { u64 sig; u32 rev, size, crc, res; } TableHeader;
typedef struct { u16 year; u8 month, day, hour, minute, second, pad1; u32 ns; i16 tz; u8 dst, pad2; } Time;

typedef struct { u16 scan; c16 ch; } Key;
typedef struct TextIn {
    Status (*Reset)(struct TextIn *, u8);
    Status (*ReadKeyStroke)(struct TextIn *, Key *);
    Event WaitForKey;
} TextIn;

typedef struct { i32 x, y, z; u8 left, right; } PointerState;
typedef struct { u64 resx, resy, resz; u8 left, right; } PointerMode;
typedef struct Pointer {
    Status (*Reset)(struct Pointer *, u8);
    Status (*GetState)(struct Pointer *, PointerState *);
    Event WaitForInput;
    PointerMode *Mode;
} Pointer;

typedef struct { u64 minx, miny, minz, maxx, maxy, maxz; u32 attr; } AbsMode;
typedef struct { u64 x, y, z; u32 buttons; } AbsState;
typedef struct AbsPointer {
    Status (*Reset)(struct AbsPointer *, u8);
    Status (*GetState)(struct AbsPointer *, AbsState *);
    Event WaitForInput;
    AbsMode *Mode;
} AbsPointer;

typedef struct { u32 version, width, height, format, mask[4], stride; } GopInfo;
typedef struct { u32 max, mode; GopInfo *info; u64 infoSize, fbBase, fbSize; } GopMode;
typedef struct Gop {
    Status (*QueryMode)(struct Gop *, u32, u64 *, GopInfo **);
    Status (*SetMode)(struct Gop *, u32);
    Status (*Blt)(struct Gop *, u32 *, u32, u64, u64, u64, u64, u64, u64, u64);
    GopMode *Mode;
} Gop;

typedef struct File {
    u64 rev;
    Status (*Open)(struct File *, struct File **, c16 *, u64, u64);
    Status (*Close)(struct File *);
    Status (*Delete)(struct File *);
    Status (*Read)(struct File *, u64 *, void *);
    Status (*Write)(struct File *, u64 *, void *);
    Status (*GetPosition)(struct File *, u64 *);
    Status (*SetPosition)(struct File *, u64);
    Status (*GetInfo)(struct File *, Guid *, u64 *, void *);
    Status (*SetInfo)(struct File *, Guid *, u64, void *);
    Status (*Flush)(struct File *);
} File;
typedef struct { u64 size, fileSize, physicalSize; Time created, accessed, modified; u64 attr; c16 name[256]; } FileInfo;
typedef struct FileSystem {
    u64 rev;
    Status (*OpenVolume)(struct FileSystem *, File **);
} FileSystem;
typedef struct { u32 rev; Handle parent; void *system; Handle device; } LoadedImage;

#define FILE_READ 1ULL
#define FILE_WRITE 2ULL
#define FILE_CREATE 0x8000000000000000ULL
#define FILE_DIRECTORY 0x10ULL

typedef struct { u32 type, pad; u64 phys, virt, pages, attr; } MemoryDesc;

typedef struct {
    TableHeader hdr;
    void *RaiseTPL, *RestoreTPL, *AllocatePages, *FreePages;
    Status (*GetMemoryMap)(u64 *, void *, u64 *, u64 *, u32 *);
    Status (*AllocatePool)(u32, u64, void **);
    Status (*FreePool)(void *);
    Status (*CreateEvent)(u32, u64, void *, void *, Event *);
    Status (*SetTimer)(Event, u32, u64);
    Status (*WaitForEvent)(u64, Event *, u64 *);
    void *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    Status (*HandleProtocol)(Handle, Guid *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage, *ExitBootServices;
    void *GetNextMonotonicCount;
    Status (*Stall)(u64);
    Status (*SetWatchdogTimer)(u64, u64, u64, c16 *);
    void *ConnectController, *DisconnectController;
    Status (*OpenProtocol)(Handle, Guid *, void **, Handle, Handle, u32);
    void *CloseProtocol, *OpenProtocolInformation, *ProtocolsPerHandle;
    Status (*LocateHandleBuffer)(u32, Guid *, void *, u64 *, Handle **);
    Status (*LocateProtocol)(Guid *, void *, void **);
} BootServices;

typedef struct {
    TableHeader hdr;
    Status (*GetTime)(Time *, void *);
    void *SetTime, *GetWakeupTime, *SetWakeupTime, *SetVirtualAddressMap, *ConvertPointer;
    void *GetVariable, *GetNextVariableName, *SetVariable, *GetNextHighMonotonicCount;
    void (*ResetSystem)(u32, Status, u64, void *);
} RuntimeServices;

typedef struct {
    TableHeader hdr;
    c16 *vendor;
    u32 revision;
    Handle conInHandle;
    TextIn *conIn;
    Handle conOutHandle;
    void *conOut;
    Handle errHandle;
    void *err;
    RuntimeServices *rt;
    BootServices *bs;
} SystemTable;
