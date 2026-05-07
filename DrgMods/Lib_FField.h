#pragma once
// Lib_FField.h — FLifetimeProperty, replication helpers.

#include <cassert>
#include "Lib_Forward.h"

// =========================================================================
// Replicated property defines
// =========================================================================
enum ELifetimeRepNotifyCondition
{
    REPNOTIFY_OnChanged = 0             ,// Only call the property's RepNotify function if it changes from the local value
    REPNOTIFY_Always = 1                ,// Always Call the property's RepNotify function when it is received from the server
};
class FLifetimeProperty
{
public:

    uint16 RepIndex;
    ELifetimeCondition Condition;
    ELifetimeRepNotifyCondition RepNotifyCondition;
    bool bIsPushBased;

    FLifetimeProperty()
        : RepIndex(0)
        , Condition(ELifetimeCondition::COND_None)
        , RepNotifyCondition(ELifetimeRepNotifyCondition::REPNOTIFY_OnChanged)
        , bIsPushBased(false)
    {
    }

    FLifetimeProperty(int32 InRepIndex)
        : RepIndex(InRepIndex)
        , Condition(ELifetimeCondition::COND_None)
        , RepNotifyCondition(ELifetimeRepNotifyCondition::REPNOTIFY_OnChanged)
        , bIsPushBased(false)
    {
        assert(InRepIndex <= 65535);
    }

    FLifetimeProperty(
        int32 InRepIndex,
        ELifetimeCondition InCondition,
        ELifetimeRepNotifyCondition InRepNotifyCondition = ELifetimeRepNotifyCondition::REPNOTIFY_OnChanged,
        bool bInIsPushBased = false
    )
        : RepIndex(InRepIndex)
        , Condition(InCondition)
        , RepNotifyCondition(InRepNotifyCondition)
        , bIsPushBased(bInIsPushBased)
    {
        assert(InRepIndex <= 65535);
    }

    inline bool operator==(const FLifetimeProperty& Other) const
    {
        if (RepIndex == Other.RepIndex)
        {
            assert(Condition == Other.Condition);
            assert(RepNotifyCondition == Other.RepNotifyCondition);
            assert(bIsPushBased == Other.bIsPushBased);
            return true;
        }

        return false;
    }
};
