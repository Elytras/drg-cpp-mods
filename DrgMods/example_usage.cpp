#include "EasyHook.hpp"
#include <Windows.h>
#include <iostream>

// Example 1: Hooking MessageBoxA
typedef int (WINAPI* MessageBoxA_t)(HWND, LPCSTR, LPCSTR, UINT);
MessageBoxA_t OriginalMessageBoxA = nullptr;

int WINAPI HookedMessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
    std::cout << "[HOOK] MessageBoxA called!" << std::endl;
    std::cout << "Text: " << lpText << std::endl;
    std::cout << "Caption: " << lpCaption << std::endl;
    
    // Call original function with modified parameters
    return OriginalMessageBoxA(hWnd, "Hooked Message!", "Hooked Title", uType);
}

// Example 2: Hooking a custom function
int Add(int a, int b) {
    return a + b;
}

typedef int (*Add_t)(int, int);
Add_t OriginalAdd = nullptr;

int HookedAdd(int a, int b) {
    std::cout << "[HOOK] Add called with: " << a << " + " << b << std::endl;
    int result = OriginalAdd(a, b);
    std::cout << "[HOOK] Original result: " << result << std::endl;
    return result * 2; // Double the result!
}

// Example 3: Using RAII Hook class
void Example_RAIIHook() {
    std::cout << "\n=== RAII Hook Example ===" << std::endl;
    
    {
        EasyHook::Hook<Add_t> addHook(reinterpret_cast<void*>(&Add), &HookedAdd);
        addHook.Enable();
        
        std::cout << "Result (hooked): " << Add(5, 3) << std::endl;
        
        addHook.Disable();
        std::cout << "Result (unhooked): " << Add(5, 3) << std::endl;
        
    } // Hook automatically removed when out of scope
    
    std::cout << "Result (after scope): " << Add(5, 3) << std::endl;
}

// Example 4: Manual hook management
void Example_ManualHook() {
    std::cout << "\n=== Manual Hook Example ===" << std::endl;
    
    // Initialize the hook manager
    if (!EasyHook::Init()) {
        std::cerr << "Failed to initialize EasyHook" << std::endl;
        return;
    }
    
    // Create and enable hook for MessageBoxA
    void* messageBoxAddr = reinterpret_cast<void*>(&MessageBoxA);
    
    if (EasyHook::CreateAndEnableHook(messageBoxAddr, &HookedMessageBoxA, &OriginalMessageBoxA)) {
        std::cout << "MessageBoxA hook created and enabled!" << std::endl;
        
        // Test the hook
        MessageBoxA(NULL, "Test", "Test", MB_OK);
        
        // Disable the hook
        EasyHook::HookManager::Get().DisableHook(messageBoxAddr);
        std::cout << "Hook disabled" << std::endl;
        
        // Remove the hook
        EasyHook::HookManager::Get().RemoveHook(messageBoxAddr);
    }
    
    // Shutdown
    EasyHook::Shutdown();
}

// Example 5: Using the HookManager directly
void Example_HookManager() {
    std::cout << "\n=== HookManager Direct Usage ===" << std::endl;
    
    auto& manager = EasyHook::HookManager::Get();
    
    if (!manager.Initialize()) {
        std::cerr << "Failed to initialize" << std::endl;
        return;
    }
    
    // Create hook
    void* addAddr = reinterpret_cast<void*>(&Add);
    if (manager.CreateHook(addAddr, &HookedAdd, &OriginalAdd)) {
        std::cout << "Hook created successfully" << std::endl;
        std::cout << "Active hooks: " << manager.GetActiveHookCount() << std::endl;
        
        // Enable hook
        manager.EnableHook(addAddr);
        std::cout << "Result: " << Add(10, 20) << std::endl;
        
        // Disable all hooks
        manager.DisableAllHooks();
        std::cout << "Result (disabled): " << Add(10, 20) << std::endl;
        
        // Re-enable
        manager.EnableHook(addAddr);
        std::cout << "Result (re-enabled): " << Add(10, 20) << std::endl;
    }
    
    manager.Uninitialize();
}

int main() {
    std::cout << "EasyHook Library Examples\n" << std::endl;
    
    try {
        Example_RAIIHook();
        Example_ManualHook();
        Example_HookManager();
    }
    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
    }
    
    std::cout << "\nPress Enter to exit...";
    std::cin.get();
    
    return 0;
}
