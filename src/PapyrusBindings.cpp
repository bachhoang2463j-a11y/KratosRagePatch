#include "GameBridge.h"

namespace kr {
namespace {
void ConfigureRage(RE::StaticFunctionTag*, int maximum, int normal, int power, int special,
    int received, int kill, int perSecond, float deniedInterval)
{
    GameBridge::Get().Configure({maximum, normal, power, special, received, kill, perSecond}, deniedInterval);
}

void SubmitWeaponHit(RE::StaticFunctionTag*, RE::TESObjectREFR* target, RE::TESForm* source,
    RE::BGSProjectile* projectile, int flags)
{
    GameBridge::Get().SubmitWeaponHit(target, source, projectile, flags);
}

void SubmitMagicHit(RE::StaticFunctionTag*, RE::TESObjectREFR* target, RE::TESForm* source, RE::BGSProjectile* projectile)
{
    GameBridge::Get().SubmitMagicHit(target, source, projectile);
}

void SubmitProjectileHit(RE::StaticFunctionTag*, RE::TESObjectREFR* target, RE::TESForm* source, RE::BGSProjectile* projectile)
{
    GameBridge::Get().SubmitProjectileHit(target, source, projectile);
}

void SubmitIncomingHit(RE::StaticFunctionTag*, RE::TESObjectREFR* aggressor, RE::TESForm* source,
    RE::BGSProjectile* projectile, bool power, bool sneak, bool bash, bool blocked)
{
    GameBridge::Get().SubmitIncomingHit(aggressor, source, projectile, power, sneak, bash, blocked);
}

void SubmitKill(RE::StaticFunctionTag*, RE::Actor* victim, RE::Actor* killer)
{
    GameBridge::Get().SubmitKill(victim, killer);
}

void RefreshEquipmentState(RE::StaticFunctionTag*) { GameBridge::Get().RefreshEquipmentState(); }
void ReconcileAfterLoad(RE::StaticFunctionTag*) { GameBridge::Get().ReconcileAfterLoad(); }
void NotifyActivationAttempt(RE::StaticFunctionTag*, std::string control, bool attack,
    bool forward, bool back, bool left, bool right)
{
    GameBridge::Get().NotifyActivationAttempt(std::move(control), attack, forward, back, left, right);
}

int GetRage(RE::StaticFunctionTag*) { return GameBridge::Get().GetSnapshot().current; }
int GetRageMaximum(RE::StaticFunctionTag*) { return GameBridge::Get().GetSnapshot().maximum; }
bool IsRaging(RE::StaticFunctionTag*) { return GameBridge::Get().GetSnapshot().raging; }
bool IsReady(RE::StaticFunctionTag*) { return GameBridge::Get().GetSnapshot().ready; }
bool IsEligible(RE::StaticFunctionTag*) { return GameBridge::Get().GetSnapshot().eligible; }
bool IsPending(RE::StaticFunctionTag*) { return GameBridge::Get().GetSnapshot().pending; }
bool IsAvailable(RE::StaticFunctionTag*) { return GameBridge::Get().IsAvailable(); }
}

bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* vm)
{
    if (!vm) {
        return false;
    }
    constexpr auto script = "KR_Native";
    vm->RegisterFunction("ConfigureRage", script, ConfigureRage);
    vm->RegisterFunction("SubmitWeaponHit", script, SubmitWeaponHit);
    vm->RegisterFunction("SubmitMagicHit", script, SubmitMagicHit);
    vm->RegisterFunction("SubmitProjectileHit", script, SubmitProjectileHit);
    vm->RegisterFunction("SubmitIncomingHit", script, SubmitIncomingHit);
    vm->RegisterFunction("SubmitKill", script, SubmitKill);
    vm->RegisterFunction("RefreshEquipmentState", script, RefreshEquipmentState);
    vm->RegisterFunction("NotifyActivationAttempt", script, NotifyActivationAttempt);
    vm->RegisterFunction("ReconcileAfterLoad", script, ReconcileAfterLoad);
    vm->RegisterFunction("GetRage", script, GetRage);
    vm->RegisterFunction("GetRageMaximum", script, GetRageMaximum);
    vm->RegisterFunction("IsRaging", script, IsRaging);
    vm->RegisterFunction("IsReady", script, IsReady);
    vm->RegisterFunction("IsEligible", script, IsEligible);
    vm->RegisterFunction("IsPending", script, IsPending);
    vm->RegisterFunction("IsAvailable", script, IsAvailable);
    return true;
}

}
