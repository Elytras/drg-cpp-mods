#pragma once
#include <string>
#include <exception>
#include <string_view>
using uint64 = unsigned long long;
using int64 = signed long long;
using int32 = signed int;
using uint32 = unsigned int;
using uint16 = unsigned short;
using int16 = signed short;
using uint8 = unsigned char;
using int8 = signed char;
using wchar = wchar_t;
using GCreateMallocFn = void(*)();

namespace SDK {
    namespace InSDKUtils
    {
        extern uint64 GetImageBase();
    }
};

using namespace SDK::InSDKUtils;
extern int Utf8ToWide(const char* src, wchar* dst, int capacity);
extern void* FindPattern(const wchar_t* moduleName, std::string_view pattern);
// Allocation-free pattern scan — MUST be used on the allocator-resolution path so the
// global operator new (UE_GLOBAL_ALLOC) can't re-enter itself and deadlock. See Lib_Utils.cpp.
extern void* FindPatternNoAlloc(const wchar_t* moduleName, std::string_view pattern);
class UnrealAllocator
{
    inline static uint64 RVA = 0;
    inline static UnrealAllocator* Cached = nullptr;
    inline static bool Failed = false;

    UnrealAllocator() = default;

    static GCreateMallocFn GetCreateMalloc()
    {   
        // GCreateMalloc unique prologue.
        //   UE 4.27 (DRG): sub rsp, 0xA8 ; mov rax, gs:[0x58] ; mov ecx, [rel tls_index]
        //                  The tls_index load follows the gs read. Exact rsp size + trailing
        //                  8B 0D needed — 16 bytes alone has a second match in this binary.
        //   UE 5.6  (RC) : sub rsp, 0xB8 ; mov ecx, [rel tls_index] ; mov rax, gs:[0x58]
        //                  (UE5 moved the tls_index load to before the gs read.)
        static GCreateMallocFn fn = []() -> GCreateMallocFn
        {
#if defined(RogueCore) && RogueCore
            uint8_t* match = static_cast<uint8_t*>(FindPatternNoAlloc(nullptr,
                "48 81 EC ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00"));
#else
            uint8_t* match = static_cast<uint8_t*>(FindPatternNoAlloc(nullptr,
                "48 81 EC A8 00 00 00 65 48 8B 04 25 58 00 00 00 8B 0D ?? ?? ?? ??"));
#endif
            return match ? reinterpret_cast<GCreateMallocFn>(match) : nullptr;
        }();
        return fn;
    }

public:
    static uint64 GetRVA() { return RVA; }

    static UnrealAllocator* Resolve()
    {
        if (Cached) return Cached;
        if (Failed) return nullptr;

        // Find GCreateMalloc by its unique prologue
        uint8_t* createBase = reinterpret_cast<uint8_t*>(GetCreateMalloc());
        if (!createBase)
        {
            Failed = true;
            return nullptr;
        }

        // Within GCreateMalloc, find 'mov [rip+disp32], rdi ; jmp 7'
        // 48 89 3D ?? ?? ?? ?? EB 07 — first store to GMalloc
        uint8_t* gmallocWrite = nullptr;
        for (int i = 0; i < 0x200 - 8; i++)
        {
            if (createBase[i]   == 0x48 &&
                createBase[i+1] == 0x89 &&
                createBase[i+2] == 0x3D &&
                createBase[i+7] == 0xEB &&
                createBase[i+8] == 0x07)
            {
                gmallocWrite = createBase + i;
                break;
            }
        }

        if (!gmallocWrite)
        {
            Failed = true;
            return nullptr;
        }

        // RIP-relative: disp32 at +3, next instruction at +7
        int32_t gmRel = *reinterpret_cast<int32_t*>(gmallocWrite + 3);
        uint8_t* gmAddr = gmallocWrite + 7 + gmRel;

        uint64 base = GetImageBase();
        RVA = static_cast<uint64>(gmAddr - reinterpret_cast<uint8_t*>(base));

        auto tryGet = [&]() -> UnrealAllocator*
        {
            return *reinterpret_cast<UnrealAllocator**>(gmAddr);
        };

        UnrealAllocator* alloc = tryGet();
        if (!alloc)
        {
            static bool attemptedCreate = false;
            if (!attemptedCreate)
            {
                attemptedCreate = true;
                GetCreateMalloc()();
                alloc = tryGet();
            }
        }

        if (!alloc) return nullptr;

        Cached = alloc;
        return Cached;
    }

    static UnrealAllocator* Get(bool forceRetry = false)
    {
        if (Cached && !forceRetry)
            return Cached;

        if (forceRetry)
        {
            Cached = nullptr;
            Failed = false;
        }

        return Resolve();
    }

    static bool IsReady()
    {
        return Get() != nullptr;
    }

    // Non-resolving peek at the cached allocator (never scans, never allocates). The global
    // operator new uses this as its fast path so a cached allocation can't trigger resolution.
    static UnrealAllocator* Peek() { return Cached; }

    UnrealAllocator(const UnrealAllocator&) = delete;
    UnrealAllocator& operator=(const UnrealAllocator&) = delete;

    // FMalloc vtable layout. Diverges between UE 4.27 and UE 5.6 because
    // UE5 expanded FExec (added Exec_Dev / Exec_Runtime), pushing all of
    // FMalloc's own slots down. Confirmed for UE5 via FMemory::Malloc:
    //   jmp qword [rax+0x28]      → Malloc at slot 5
    //   call qword [rax+0xB8]     → ValidateHeap at slot 23
    // Slots that aren't directly confirmed (pads, GetDescriptiveName etc.)
    // are placed by best guess from UE5 public source; nothing currently
    // calls them so a wrong slot would still compile cleanly.
#if defined(RogueCore) && RogueCore
    // ── UE 5.6 ────────────────────────────────────────────────────────────
    virtual void           Pad00_Destructor()                                         = 0;  // slot 0
    virtual bool           Exec               (void*, const wchar_t*, void*)          = 0;  // slot 1
    virtual bool           _Pad10_ExecDev     ()                                      = 0;  // slot 2 — UE5: FExec::Exec_Dev
    virtual bool           _Pad18_ExecRuntime ()                                      = 0;  // slot 3 — UE5: FExec::Exec_Runtime
    virtual void           _Pad20             ()                                      = 0;  // slot 4 — unknown UE5 virtual

    virtual void*          Malloc             (uint64 Count, uint32 Alignment = 1)    = 0;  // slot 5  [+0x28] ✓
    virtual void*          TryMalloc          (uint64 Count, uint32 Alignment = 1)    = 0;  // slot 6
    virtual void*          Realloc            (void* Original, uint64 Count, uint32 Alignment = 1) = 0; // slot 7
    virtual void*          TryRealloc         (void* Original, uint64 Count, uint32 Alignment = 1) = 0; // slot 8
    virtual void           Free               (void* Original)                        = 0;  // slot 9

    virtual uint64         QuantizeSize       (uint64 Count, uint32 Alignment = 1)    = 0;  // slot 10
    virtual bool           GetAllocationSize  (void* Original, uint64& SizeOut)       = 0;  // slot 11

    virtual void           Trim                                  (bool bTrimThreadCaches) = 0; // slot 12
    virtual void           SetupTLSCachesOnCurrentThread         ()                       = 0; // slot 13
    virtual void           _Pad70_MarkTLSUsed                    ()                       = 0; // slot 14 — UE5 new
    virtual void           _Pad78_MarkTLSUnused                  ()                       = 0; // slot 15 — UE5 new
    virtual void           ClearAndDisableTLSCachesOnCurrentThread()                      = 0; // slot 16
    virtual void           InitializeStatsMetadata               ()                       = 0; // slot 17

    virtual void           UpdateStats                           ()                       = 0; // slot 18
    virtual void           GetAllocatorStats                     (void* out_Stats)        = 0; // slot 19
    virtual const wchar_t* GetDescriptiveName                    ()                       = 0; // slot 20
    virtual void           DumpAllocatorStats                    (void* Ar)               = 0; // slot 21

    virtual bool           IsInternallyThreadSafe                ()                       = 0; // slot 22
    virtual bool           ValidateHeap                          ()                       = 0; // slot 23 [+0xB8] ✓
#else
    // ── UE 4.27 ───────────────────────────────────────────────────────────
    virtual void           Pad00_Destructor()                                         = 0;  // slot 0
    virtual bool           Exec               (void*, const wchar_t*, void*)          = 0;  // slot 1

    virtual void*          Malloc             (uint64 Count, uint32 Alignment = 1)    = 0;  // slot 2  [+0x10]
    virtual void*          TryMalloc          (uint64 Count, uint32 Alignment = 1)    = 0;  // slot 3
    virtual void*          Realloc            (void* Original, uint64 Count, uint32 Alignment = 1) = 0; // slot 4
    virtual void*          TryRealloc         (void* Original, uint64 Count, uint32 Alignment = 1) = 0; // slot 5
    virtual void           Free               (void* Original)                        = 0;  // slot 6

    virtual uint64         QuantizeSize       (uint64 Count, uint32 Alignment = 1)    = 0;  // slot 7
    virtual bool           GetAllocationSize  (void* Original, uint64& SizeOut)       = 0;  // slot 8

    virtual void           Trim                                  (bool bTrimThreadCaches) = 0; // slot 9
    virtual void           SetupTLSCachesOnCurrentThread         ()                       = 0; // slot 10
    virtual void           ClearAndDisableTLSCachesOnCurrentThread()                      = 0; // slot 11
    virtual void           InitializeStatsMetadata               ()                       = 0; // slot 12

    virtual void           UpdateStats                           ()                       = 0; // slot 13
    virtual void           GetAllocatorStats                     (void* out_Stats)        = 0; // slot 14
    virtual void           DumpAllocatorStats                    (void* Ar)               = 0; // slot 15

    virtual bool           IsInternallyThreadSafe                ()                       = 0; // slot 16
    virtual bool           ValidateHeap                          ()                       = 0; // slot 17
    virtual const wchar_t* GetDescriptiveName                    ()                       = 0; // slot 18
#endif
};

template<typename T>
class UnrealArray
{
    T*    Data        = nullptr;
    int32 NumElements = 0;
    int32 MaxElements = 0;

public:
    UnrealArray() = default;

    ~UnrealArray() { FreeData(); }

    UnrealArray(const UnrealArray& Other)
    {
        if (Other.NumElements > 0)
        {
            Grow(Other.NumElements);
            for (int32 i = 0; i < Other.NumElements; ++i)
                new (Data + i) T(Other.Data[i]);
            NumElements = Other.NumElements;
        }
    }

    UnrealArray& operator=(const UnrealArray& Other)
    {
        if (this != &Other)
        {
            Clear();
            FreeData();
            if (Other.NumElements > 0)
            {
                Grow(Other.NumElements);
                for (int32 i = 0; i < Other.NumElements; ++i)
                    new (Data + i) T(Other.Data[i]);
                NumElements = Other.NumElements;
            }
        }
        return *this;
    }

    UnrealArray(UnrealArray&& Other) noexcept { Steal(Other); }

    UnrealArray& operator=(UnrealArray&& Other) noexcept
    {
        if (this != &Other)
        {
            FreeData();
            Steal(Other);
        }
        return *this;
    }

    int32 Num()     const { return NumElements; }
    int32 Max()     const { return MaxElements; }
    bool  IsEmpty() const { return NumElements == 0; }

    T*       GetData()       { return Data; }
    const T* GetData() const { return Data; }

    T&       operator[](int32 i)       { return Data[i]; }
    const T& operator[](int32 i) const { return Data[i]; }

    T*       begin()       { return Data; }
    T*       end()         { return Data + NumElements; }
    const T* begin() const { return Data; }
    const T* end()   const { return Data + NumElements; }

    void Reserve(int32 Cap)
    {
        if (Cap <= MaxElements) return;
        Grow(Cap);
    }

    void Add(const T& Item)
    {
        EnsureSpace(NumElements + 1);
        new (Data + NumElements) T(Item);
        ++NumElements;
    }

    void Add(T&& Item)
    {
        EnsureSpace(NumElements + 1);
        new (Data + NumElements) T(static_cast<T&&>(Item));
        ++NumElements;
    }

    void RemoveAt(int32 i)
    {
        Data[i].~T();
        for (int32 j = i; j < NumElements - 1; ++j)
        {
            new (Data + j) T(static_cast<T&&>(Data[j + 1]));
            Data[j + 1].~T();
        }
        --NumElements;
    }

    void Clear()
    {
        for (int32 i = 0; i < NumElements; ++i)
            Data[i].~T();
        NumElements = 0;
    }

private:
    void Grow(int32 Cap)
    {
        UnrealAllocator* Alloc = UnrealAllocator::Get();
        if (!Alloc) throw std::exception("Allocator not ready");

        T* NewData = static_cast<T*>(Alloc->Realloc(Data, Cap * sizeof(T), alignof(T)));
        Data        = NewData;
        MaxElements = Cap;
    }

    void EnsureSpace(int32 Needed)
    {
        if (Needed <= MaxElements) return;
        int32 NewCap = MaxElements ? MaxElements * 2 : 4;
        if (NewCap < Needed) NewCap = Needed;
        Grow(NewCap);
    }

    void FreeData()
    {
        if (Data)
        {
            UnrealAllocator::Get()->Free(Data);
            Data = nullptr;
        }
        NumElements = MaxElements = 0;
    }

    void Steal(UnrealArray& Other)
    {
        Data        = Other.Data;
        NumElements = Other.NumElements;
        MaxElements = Other.MaxElements;

        Other.Data        = nullptr;
        Other.NumElements = 0;
        Other.MaxElements = 0;
    }
};

template<>
class UnrealArray<wchar>
{
    wchar* Data        = nullptr;
    int32  NumElements = 0;
    int32  MaxElements = 0;

public:
    UnrealArray() = default;

    ~UnrealArray() { Free(); }

    UnrealArray(const UnrealArray& Other) { Assign(Other.Data, Other.NumElements); }

    UnrealArray& operator=(const UnrealArray& Other)
    {
        if (this != &Other)
        {
            Free();
            Assign(Other.Data, Other.NumElements);
        }
        return *this;
    }

    UnrealArray(UnrealArray&& Other) noexcept { Steal(Other); }

    UnrealArray& operator=(UnrealArray&& Other) noexcept
    {
        if (this != &Other)
        {
            Free();
            Steal(Other);
        }
        return *this;
    }

    UnrealArray(const char* Narrow)
    {
        if (!Narrow) return;
        int len = (int)strlen(Narrow);
        Allocate(len + 1);
        int written = Utf8ToWide(Narrow, Data, MaxElements);
        if (written < 0) written = 0;
        Data[written] = 0;
        NumElements = written + 1;
    }

    UnrealArray(std::string_view Narrow)
    {
        if (Narrow.empty()) return;
        Allocate((int32)Narrow.size() + 1);
        int written = Utf8ToWide(Narrow.data(), Data, MaxElements);
        if (written < 0) written = 0;
        Data[written] = 0;
        NumElements = written + 1;
    }

    UnrealArray(const wchar* Wide)
    {
        if (!Wide) return;
        int len = (int)wcslen(Wide);
        Allocate(len + 1);
        __assume(Data != 0);
        memcpy(Data, Wide, (len + 1) * sizeof(wchar));
        NumElements = len + 1;
    }

    UnrealArray(std::wstring_view Wide)
    {
        if (Wide.empty()) return;
        Allocate((int32)Wide.size() + 1);
        __assume(Data != 0);
        memcpy(Data, Wide.data(), Wide.size() * sizeof(wchar));
        Data[Wide.size()] = 0;
        NumElements = (int32)Wide.size() + 1;
    }

    UnrealArray(const std::string& Str)  : UnrealArray(std::string_view(Str))  {}
    UnrealArray(const std::wstring& Str) : UnrealArray(std::wstring_view(Str)) {}

    const wchar* CStr()    const { return Data ? Data : L""; }
    int32        Len()     const { return NumElements ? NumElements - 1 : 0; }
    bool         IsEmpty() const { return Len() == 0; }

    int32  Num() const { return NumElements; }
    int32  Max() const { return MaxElements; }
    wchar* GetData()       { return Data; }
    const wchar* GetData() const { return Data; }

    wchar*       begin()       { return Data; }
    wchar*       end()         { return Data + NumElements; }
    const wchar* begin() const { return Data; }
    const wchar* end()   const { return Data + NumElements; }

private:
    void Allocate(int32 Cap)
    {
        UnrealAllocator* Alloc = UnrealAllocator::Get();
        if (!Alloc) throw std::exception("Allocator not ready");
        Data = (wchar*)Alloc->Malloc(Cap * sizeof(wchar), alignof(wchar));
        MaxElements = Cap;
    }

    void Free()
    {
        if (Data)
        {
            UnrealAllocator::Get()->Free(Data);
            Data = nullptr;
        }
        NumElements = 0;
        MaxElements = 0;
    }

    void Assign(const wchar* Src, int32 LenWithNull)
    {
        if (!Src || LenWithNull <= 0) return;
        Allocate(LenWithNull);
        memcpy(Data, Src, LenWithNull * sizeof(wchar));
        NumElements = LenWithNull;
    }

    void Steal(UnrealArray& Other)
    {
        Data        = Other.Data;
        NumElements = Other.NumElements;
        MaxElements = Other.MaxElements;

        Other.Data        = nullptr;
        Other.NumElements = 0;
        Other.MaxElements = 0;
    }
};

using UnrealString = UnrealArray<wchar>;
