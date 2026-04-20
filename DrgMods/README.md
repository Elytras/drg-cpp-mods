# EasyHook - MinHook Wrapper Library

A simple, easy-to-use C++ wrapper around MinHook for function hooking.

## Features

- **Singleton Pattern**: Centralized hook management through `HookManager`
- **RAII Support**: Automatic hook cleanup with the `Hook` template class
- **Type-Safe**: Template-based API for type safety
- **Simple API**: Convenience functions for quick hooking
- **Exception Handling**: Clear error reporting
- **Multiple Hook Management**: Enable/disable individual or all hooks

## Requirements

- MinHook library
- C++11 or later
- Windows (MinHook is Windows-only)

## Quick Start

### Method 1: RAII Style (Recommended)

```cpp
#include "EasyHook.hpp"

typedef int (*MyFunc_t)(int, int);
MyFunc_t OriginalFunc = nullptr;

int HookedFunc(int a, int b) {
    // Your hook code
    return OriginalFunc(a, b);
}

int main() {
    EasyHook::Hook<MyFunc_t> hook(&TargetFunction, &HookedFunc);
    hook.Enable();
    
    // Hook is automatically removed when 'hook' goes out of scope
    return 0;
}
```

### Method 2: Simple Convenience Function

```cpp
#include "EasyHook.hpp"

int main() {
    EasyHook::Init();
    
    EasyHook::CreateAndEnableHook(
        &TargetFunction,
        &HookedFunction,
        &OriginalFunction
    );
    
    // Use the hook...
    
    EasyHook::Shutdown();
    return 0;
}
```

### Method 3: Manual Management

```cpp
#include "EasyHook.hpp"

int main() {
    auto& manager = EasyHook::HookManager::Get();
    manager.Initialize();
    
    manager.CreateHook(&Target, &Detour, &Original);
    manager.EnableHook(&Target);
    
    // Use the hook...
    
    manager.DisableHook(&Target);
    manager.RemoveHook(&Target);
    manager.Uninitialize();
    
    return 0;
}
```

## API Reference

### HookManager (Singleton)

#### Initialization
- `bool Initialize()` - Initialize MinHook
- `bool Uninitialize()` - Uninitialize MinHook and clean up
- `bool IsInitialized()` - Check if initialized

#### Hook Operations
- `bool CreateHook(void* target, FuncType detour, FuncType* original)` - Create a hook
- `bool EnableHook(void* target)` - Enable a specific hook
- `bool DisableHook(void* target)` - Disable a specific hook
- `bool EnableAllHooks()` - Enable all hooks
- `bool DisableAllHooks()` - Disable all hooks
- `bool RemoveHook(void* target)` - Remove a hook

#### Utilities
- `const char* GetStatusString(MH_STATUS status)` - Get status message
- `size_t GetActiveHookCount()` - Get number of active hooks

### Hook<FuncType> (RAII Class)

#### Constructor
```cpp
Hook(void* targetFunc, FuncType detourFunc)
```

#### Methods
- `bool Enable()` - Enable the hook
- `bool Disable()` - Disable the hook
- `FuncType GetOriginal()` - Get pointer to original function
- `bool IsEnabled()` - Check if hook is enabled

### Convenience Functions

```cpp
namespace EasyHook {
    bool Init();
    bool Shutdown();
    
    template<typename FuncType>
    bool CreateAndEnableHook(void* target, FuncType detour, FuncType* original);
}
```

## Examples

### Hooking Windows API

```cpp
typedef int (WINAPI* MessageBoxA_t)(HWND, LPCSTR, LPCSTR, UINT);
MessageBoxA_t OriginalMessageBoxA = nullptr;

int WINAPI HookedMessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
    // Modify behavior
    return OriginalMessageBoxA(hWnd, "Hooked!", "Hooked!", uType);
}

int main() {
    EasyHook::Init();
    
    void* messageBoxAddr = reinterpret_cast<void*>(&MessageBoxA);
    EasyHook::CreateAndEnableHook(messageBoxAddr, &HookedMessageBoxA, &OriginalMessageBoxA);
    
    MessageBoxA(NULL, "Test", "Test", MB_OK); // Will show "Hooked!"
    
    EasyHook::Shutdown();
    return 0;
}
```

### Hooking Custom Function

```cpp
int MyFunction(int x) {
    return x * 2;
}

typedef int (*MyFunction_t)(int);
MyFunction_t OriginalMyFunction = nullptr;

int HookedMyFunction(int x) {
    std::cout << "Called with: " << x << std::endl;
    return OriginalMyFunction(x) + 10; // Add 10 to result
}

int main() {
    EasyHook::Hook<MyFunction_t> hook(&MyFunction, &HookedMyFunction);
    hook.Enable();
    
    int result = MyFunction(5); // Prints "Called with: 5", returns 20 (5*2+10)
    
    return 0;
}
```

### Temporary Hook

```cpp
void TemporaryHook() {
    EasyHook::Hook<MyFunc_t> tempHook(&Target, &Detour);
    tempHook.Enable();
    
    // Do something with hook active
    
} // Hook automatically disabled and removed here
```

## Error Handling

The library uses exceptions for critical errors:

```cpp
try {
    EasyHook::Hook<MyFunc_t> hook(&Target, &Detour);
    hook.Enable();
}
catch (const std::runtime_error& ex) {
    std::cerr << "Hook error: " << ex.what() << std::endl;
}
```

## Building

Make sure to link against MinHook:

```cmake
# CMakeLists.txt example
target_include_directories(YourProject PRIVATE path/to/minhook/include)
target_link_libraries(YourProject PRIVATE MinHook)
```

Or in Visual Studio, add MinHook lib to your linker dependencies.

## License

This wrapper is provided as-is. MinHook has its own license (see MinHook repository).

## Notes

- Always call `Initialize()` before creating hooks (or use `Init()`)
- The RAII `Hook` class automatically initializes if needed
- Hooks are not thread-safe during creation/removal
- Remember to keep original function pointers in scope
- Use `void*` for target addresses, cast as needed
