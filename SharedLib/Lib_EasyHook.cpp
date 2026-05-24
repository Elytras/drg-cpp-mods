#include "Lib_EasyHook.h"

namespace EasyHook
{
    HookManager& HookManager::Get()
    {
        static HookManager i;
        return i;
    }

    bool HookManager::Initialize()
    {
        if (initialized_) return true;
        if (MH_Initialize() == MH_OK)
        {
            initialized_ = true;
            return true;
        }
        return false;
    }

    bool HookManager::Uninitialize()
    {
        if (!initialized_) return true;
        if (MH_Uninitialize() == MH_OK)
        {
            initialized_ = false;
            activeHooks_.clear();
            return true;
        }
        return false;
    }

    bool HookManager::EnableHook(void* target) const
    {
        if (!initialized_) throw std::runtime_error("HookManager not initialized.");
        return MH_EnableHook(target) == MH_OK;
    }

    bool HookManager::DisableHook(void* target) const
    {
        if (!initialized_) throw std::runtime_error("HookManager not initialized.");
        return MH_DisableHook(target) == MH_OK;
    }

    bool HookManager::EnableAllHooks() const
    {
        if (!initialized_) throw std::runtime_error("HookManager not initialized.");
        return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    }

    bool HookManager::DisableAllHooks() const
    {
        if (!initialized_) throw std::runtime_error("HookManager not initialized.");
        return MH_DisableHook(MH_ALL_HOOKS) == MH_OK;
    }

    bool HookManager::RemoveHook(void* target)
    {
        if (!initialized_) throw std::runtime_error("HookManager not initialized.");
        if (MH_RemoveHook(target) == MH_OK)
        {
            activeHooks_.erase(target);
            return true;
        }
        return false;
    }

    bool HookManager::IsInitialized() const { return initialized_; }

    size_t HookManager::GetActiveHookCount() const { return activeHooks_.size(); }

    const char* HookManager::GetStatusString(MH_STATUS s) { return MH_StatusToString(s); }

    HookManager::~HookManager()
    {
        if (initialized_) Uninitialize();
    }

    bool Init() { return HookManager::Get().Initialize(); }

    bool Shutdown() { return HookManager::Get().Uninitialize(); }

} // namespace EasyHook
