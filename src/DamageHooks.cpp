#include "PCH.h"
#include "DamageHooks.h"
#include "GameBridge.h"

#include <cmath>
#include <cstddef>
#include <limits>

namespace kr {
namespace {

// HandleHealthDamage vfunc slot inside the Actor / Character vtable. The same slot on the
// same two tables is what the reference implementation (TrueHUD) hooks; the value comes
// from the class layout, never from a hand written address.
constexpr std::size_t kHealthDamageVfunc = 0x104;

// Original entries we chain into. Exactly one call per dispatch, even when the report is
// skipped.
REL::Relocation<void(RE::Actor*, RE::Actor*, float)> g_originalActor;
REL::Relocation<void(RE::Character*, RE::Actor*, float)> g_originalCharacter;

struct Observation
{
    RE::Actor* target{};
    RE::FormID targetID{};
    RE::FormID baseID{};
    float healthBefore{};
    bool candidate{};
};

// A damage application can re-enter this hook (the report itself may provoke further damage
// handling). Frames are merged per target, not globally: a nested frame on a target that is
// already being observed folds into that observation (the outermost before/after delta covers
// the nested loss too, so one hit still yields at most one report for it), while a nested hit
// on a different target - or a player hit nested inside a non-player dispatch - starts its own
// observation and reports on its own. The stack is a fixed thread_local array, so every hit
// stays allocation-free; frames beyond the cap still chain to their original but are not
// reported (unreachable at any sane nesting depth).
constexpr std::size_t kMaxObservations = 16;
thread_local Observation g_observationStack[kMaxObservations];
thread_local std::size_t g_observationDepth = 0;

[[nodiscard]] bool IsObserved(const RE::Actor* a_target)
{
    for (std::size_t i = 0; i < g_observationDepth; ++i) {
        if (g_observationStack[i].target == a_target) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] float ReadHealth(RE::Actor* a_actor)
{
    auto* owner = a_actor ? a_actor->AsActorValueOwner() : nullptr;
    return owner ? owner->GetActorValue(RE::ActorValue::kHealth)
                 : std::numeric_limits<float>::quiet_NaN();
}

// Evaluated before the original call, so the death state and the health reading describe
// the actor as it was when the damage arrived.
[[nodiscard]] bool IsCandidateDamage(RE::Actor* a_target, RE::Actor* a_attacker, float a_damage)
{
    if (!a_target || !a_attacker || !std::isfinite(a_damage) || a_damage >= 0.0f) {
        return false;  // health loss arrives as a negative, finite damage value
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || a_attacker != player || a_target == player) {
        return false;  // strictly the player as attacker, never the player as target
    }
    return !a_target->IsDead();
}

void ReportHealthLoss(const Observation& a_observation)
{
    if (!a_observation.candidate) {
        return;  // not a player-caused hit, or a nested frame of one already being handled
    }

    const float healthAfter = ReadHealth(a_observation.target);
    const float lost = a_observation.healthBefore - healthAfter;
    if (!std::isfinite(healthAfter) || !std::isfinite(lost) || !(lost > 0.0f)) {
        return;  // no actual health was lost (immune, absorbed, unreadable)
    }
    GameBridge::Get().SubmitPlayerDamage(a_observation.targetID, a_observation.baseID, lost);
}

class ReportScope
{
public:
    ReportScope(RE::Actor* a_target, RE::Actor* a_attacker, float a_damage)
    {
        if (!IsCandidateDamage(a_target, a_attacker, a_damage) || IsObserved(a_target) ||
            g_observationDepth >= kMaxObservations) {
            return;  // not a player-caused hit, a merged same-target frame, or past the cap
        }
        auto& observation = g_observationStack[g_observationDepth++];
        observation.target = a_target;
        observation.targetID = a_target->GetFormID();
        observation.baseID = 0;
        if (auto* base = a_target->GetBaseObject()) {
            observation.baseID = base->GetFormID();
        }
        // Evaluated before the original call, so the death state and the health reading
        // describe the actor as it was when the damage arrived.
        observation.healthBefore = ReadHealth(a_target);
        observation.candidate = true;
        entry_ = &observation;
    }

    ~ReportScope()
    {
        if (!entry_) {
            return;
        }
        const Observation observation = *entry_;
        --g_observationDepth;  // this entry is always the top: nesting is strictly LIFO
        ReportHealthLoss(observation);
    }

    ReportScope(const ReportScope&) = delete;
    ReportScope& operator=(const ReportScope&) = delete;

private:
    Observation* entry_{};
};

void HandleHealthDamageActor(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
{
    const ReportScope scope(a_this, a_attacker, a_damage);
    g_originalActor(a_this, a_attacker, a_damage);
}

void HandleHealthDamageCharacter(RE::Character* a_this, RE::Actor* a_attacker, float a_damage)
{
    const ReportScope scope(a_this, a_attacker, a_damage);
    g_originalCharacter(a_this, a_attacker, a_damage);
}

// Reads the slot before writing it, so a missing entry disables this hook instead of
// replacing it with a call that would have nothing to chain into.
[[nodiscard]] bool HasEntry(std::uintptr_t a_vtable, std::size_t a_index)
{
    return a_vtable != 0 && reinterpret_cast<const std::uintptr_t*>(a_vtable)[a_index] != 0;
}

template <class Fn, class Original>
bool InstallVfunc(const REL::VariantID& a_vtable, const char* a_label, Fn a_hook, Original& a_original)
{
    REL::Relocation<std::uintptr_t> vtable{ a_vtable };
    if (!HasEntry(vtable.address(), kHealthDamageVfunc)) {
        spdlog::error("KratosRage damage hook disabled: {} vtable has no entry at vfunc 0x104", a_label);
        return false;
    }
    a_original = vtable.write_vfunc(kHealthDamageVfunc, a_hook);
    return true;
}

}

void InstallDamageHooks()
{
    static bool attempted = false;
    if (attempted) {
        spdlog::info("KratosRage damage hooks already attempted; repeat install ignored");
        return;
    }
    attempted = true;

    const bool actor = InstallVfunc(RE::VTABLE_Actor[0], "Actor", &HandleHealthDamageActor, g_originalActor);
    const bool character = InstallVfunc(RE::VTABLE_Character[0], "Character", &HandleHealthDamageCharacter, g_originalCharacter);

    if (actor || character) {
        spdlog::info("KratosRage damage hooks installed (Actor={}, Character={}) at vfunc 0x104", actor, character);
    } else {
        spdlog::error("KratosRage damage hooks not installed; player damage is not reported");
    }
}

}
