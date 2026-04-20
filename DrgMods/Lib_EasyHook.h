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
        static HookManager& Get() { static HookManager i; return i; }
        HookManager(const HookManager&) = delete;
        HookManager& operator=(const HookManager&) = delete;

        bool Initialize()
        {
            if (initialized_) return true;
            if (MH_Initialize() == MH_OK) { initialized_ = true; return true; }
            return false;
        }
        bool Uninitialize()
        {
            if (!initialized_) return true;
            if (MH_Uninitialize() == MH_OK) { initialized_ = false; activeHooks_.clear(); return true; }
            return false;
        }
        template<typename FuncType>
        bool CreateHook(void* target, FuncType detour, FuncType* original)
        {
            if (!initialized_) throw std::runtime_error("HookManager not initialized.");
            if (MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)) == MH_OK)
            {
                activeHooks_[target] = reinterpret_cast<void*>(detour); return true;
            }
            return false;
        }
        bool EnableHook(void* target)  const { if (!initialized_) throw std::runtime_error("HookManager not initialized."); return MH_EnableHook(target) == MH_OK; }
        bool DisableHook(void* target) const { if (!initialized_) throw std::runtime_error("HookManager not initialized."); return MH_DisableHook(target) == MH_OK; }
        bool EnableAllHooks()          const { if (!initialized_) throw std::runtime_error("HookManager not initialized."); return MH_EnableHook(MH_ALL_HOOKS) == MH_OK; }
        bool DisableAllHooks()         const { if (!initialized_) throw std::runtime_error("HookManager not initialized."); return MH_DisableHook(MH_ALL_HOOKS) == MH_OK; }
        bool RemoveHook(void* target)
        {
            if (!initialized_) throw std::runtime_error("HookManager not initialized."); if (MH_RemoveHook(target) == MH_OK) { activeHooks_.erase(target); return true; } return false;
        }

        bool        IsInitialized()      const { return initialized_; }
        size_t      GetActiveHookCount() const { return activeHooks_.size(); }
        const char* GetStatusString(MH_STATUS s) { return MH_StatusToString(s); }
        ~HookManager() { if (initialized_) Uninitialize(); }
    private:
        HookManager() = default;
        bool initialized_ = false;
        std::unordered_map<void*, void*> activeHooks_;
    };

    template<typename FuncType>
    class Hook
    {
    public:
        Hook(void* targetFunc, FuncType detourFunc) : target_(targetFunc), detour_(detourFunc)
        {
            if (!HookManager::Get().IsInitialized()) HookManager::Get().Initialize();
            if (!HookManager::Get().CreateHook(target_, detour_, &original_))
                throw std::runtime_error("Failed to create hook");
        }
        bool Enable() { if (!enabled_) enabled_ = HookManager::Get().EnableHook(target_);  return enabled_; }
        bool Disable() { if (enabled_)  enabled_ = !HookManager::Get().DisableHook(target_); return !enabled_; }
        FuncType GetOriginal() const { return original_; }
        bool     IsEnabled()   const { return enabled_; }
        ~Hook() { if (enabled_) Disable(); HookManager::Get().RemoveHook(target_); }
    private:
        void* target_ = nullptr;
        FuncType detour_ = nullptr;
        FuncType original_ = nullptr;
        bool     enabled_ = false;
    };

    inline bool Init() { return HookManager::Get().Initialize(); }
    inline bool Shutdown() { return HookManager::Get().Uninitialize(); }

    template<typename FuncType>
    inline bool CreateAndEnableHook(void* target, FuncType detour, FuncType* original)
    {
        if (!HookManager::Get().CreateHook(target, detour, original)) return false;
        return HookManager::Get().EnableHook(target);
    }
} // namespace EasyHook