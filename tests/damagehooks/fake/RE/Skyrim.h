#pragma once

// Test stand-in for the RE surface the damage hook touches. Only the members the hook and
// the harness actually call are present, with the same names and call shapes as
// CommonLibSSE-NG (Actor::AsActorValueOwner, ActorValueOwner::GetActorValue,
// Actor::IsDead, TESObjectREFR::GetBaseObject, TESForm::GetFormID,
// PlayerCharacter::GetSingleton, VTABLE_Actor / VTABLE_Character).
//
// The two "vtable" arrays live here so a dispatch can go through the slot the hook wrote
// into, exactly like the engine would call it.

#include <array>
#include <cstdint>

#include "REL/Relocation.h"

namespace RE {

using FormID = std::uint32_t;

enum class ActorValue : std::int32_t
{
    kHealth = 24
};

class TESForm
{
public:
    [[nodiscard]] FormID GetFormID() const { return formID; }

    FormID formID{};
};

class TESBoundObject : public TESForm
{
};

class ActorValueOwner
{
public:
    // Stands in for the real virtual ActorValueOwner::GetActorValue(ActorValue).
    [[nodiscard]] float GetActorValue(ActorValue a_value) const
    {
        return a_value == ActorValue::kHealth ? health : 0.0f;
    }

    float health{ 100.0f };
};

class TESObjectREFR : public TESForm
{
public:
    [[nodiscard]] TESBoundObject* GetBaseObject() { return base; }
    [[nodiscard]] bool IsDead(bool = true) const { return dead; }

    TESBoundObject* base{};
    bool dead{ false };
};

class Actor : public TESObjectREFR
{
public:
    [[nodiscard]] ActorValueOwner* AsActorValueOwner() { return &actorValueOwner; }

    ActorValueOwner actorValueOwner;
};

class Character : public Actor
{
};

class PlayerCharacter : public Character
{
public:
    static PlayerCharacter* GetSingleton() { return singleton; }

    inline static PlayerCharacter* singleton{ nullptr };
};

// Fake VTABLE_Actor[0] / VTABLE_Character[0] storage. Slot 0x104 is the HandleHealthDamage
// entry the hook installs into; the harness pre-fills it with its own "engine" function.
inline std::array<std::uintptr_t, 0x108> g_actorVtableEntries{};
inline std::array<std::uintptr_t, 0x108> g_characterVtableEntries{};

inline const std::array<REL::VariantID, 1> VTABLE_Actor{
    REL::VariantID{ reinterpret_cast<std::uintptr_t>(g_actorVtableEntries.data()), 0, 0 }
};

inline const std::array<REL::VariantID, 1> VTABLE_Character{
    REL::VariantID{ reinterpret_cast<std::uintptr_t>(g_characterVtableEntries.data()), 0, 0 }
};

}
