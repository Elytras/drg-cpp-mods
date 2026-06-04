#pragma once
// Lib_EasyHook.h — MinHook wrapper.

#include <stdexcept>
#include <unordered_map>
#include <MinHook.h>

namespace EasyHook
{
    class HookManager
    {
    public:
        static HookManager& Get();
        HookManager(const HookManager&) = delete;
        HookManager& operator=(const HookManager&) = delete;

        bool Initialize();
        bool Uninitialize();

        template<typename FuncType>
        bool CreateHook(void* target, FuncType detour, FuncType* original)
        {
            if (!initialized_) throw std::runtime_error("HookManager not initialized.");
            if (MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)) == MH_OK)
            {
                activeHooks_[target] = reinterpret_cast<void*>(detour);
                return true;
            }
            return false;
        }

        bool EnableHook(void* target) const;
        bool DisableHook(void* target) const;
        bool EnableAllHooks() const;
        bool DisableAllHooks() const;
        bool RemoveHook(void* target);

        bool        IsInitialized() const;
        size_t      GetActiveHookCount() const;
        const char* GetStatusString(MH_STATUS s);
        ~HookManager();

    private:
        HookManager() = default;
        bool initialized_ = false;
        std::unordered_map<void*, void*> activeHooks_;
    };

    template<typename FuncType>
    class Hook
    {
    public:
        Hook(void* targetFunc, FuncType detourFunc)
            : target_(targetFunc), detour_(detourFunc)
        {
            if (!HookManager::Get().IsInitialized())
                HookManager::Get().Initialize();
            if (!HookManager::Get().CreateHook(target_, detour_, &original_))
                throw std::runtime_error("Failed to create hook");
        }

        bool Enable()
        {
            if (!enabled_) enabled_ = HookManager::Get().EnableHook(target_);
            return enabled_;
        }

        bool Disable()
        {
            if (enabled_) enabled_ = !HookManager::Get().DisableHook(target_);
            return !enabled_;
        }

        FuncType GetOriginal() const { return original_; }
        bool     IsEnabled()   const { return enabled_; }

        ~Hook()
        {
            if (enabled_) Disable();
            HookManager::Get().RemoveHook(target_);
        }

    private:
        void*    target_   = nullptr;
        FuncType detour_   = nullptr;
        FuncType original_ = nullptr;
        bool     enabled_  = false;
    };

    bool Init();
    bool Shutdown();

    template<typename FuncType>
    inline bool CreateAndEnableHook(void* target, FuncType detour, FuncType* original)
    {
        if (!HookManager::Get().CreateHook(target, detour, original)) return false;
        return HookManager::Get().EnableHook(target);
    }
} // namespace EasyHook
