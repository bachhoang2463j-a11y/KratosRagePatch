#pragma once

#include "PCH.h"
#include "RageState.h"

namespace kr {

class GameBridge final : public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>,
                         public RE::BSTEventSink<RE::TESCombatEvent>,
                         public RE::BSTEventSink<RE::TESHitEvent>,
                         public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    static GameBridge& Get();
    void Initialize();
    bool IsAvailable() const;
    Snapshot GetSnapshot() const;
    void Configure(RageSettings settings, float deniedInterval);
    void RefreshEquipmentState();
    void TickCombat();
    void SubmitPlayerDamage(RE::FormID targetID, RE::FormID baseID, float healthLost);
    void SubmitWeaponHit(RE::TESObjectREFR* target, RE::TESForm* source, RE::BGSProjectile* projectile, int flags);
    void SubmitMagicHit(RE::TESObjectREFR* target, RE::TESForm* source, RE::BGSProjectile* projectile);
    void SubmitProjectileHit(RE::TESObjectREFR* target, RE::TESForm* source, RE::BGSProjectile* projectile);
    void SubmitIncomingHit(RE::TESObjectREFR* aggressor, RE::TESForm* source, RE::BGSProjectile* projectile,
        bool powerAttack, bool sneakAttack, bool bashAttack, bool blocked);
    void SubmitKill(RE::Actor* victim, RE::Actor* killer);
    void NotifyActivationAttempt(std::string control, bool rightAttack, bool forward, bool back, bool left, bool right);
    void ReconcileAfterLoad();
    SavedState Save() const;
    void Restore(const SavedState& state);
    void Revert();

    RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent*, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override;
    RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent*, RE::BSTEventSource<RE::TESCombatEvent>*) override;
    RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent*, RE::BSTEventSource<RE::TESHitEvent>*) override;
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent*, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

private:
    struct CastSpec { RE::SpellItem* spell; float effectiveness; float magnitude; bool selfTarget; };
    struct CombatHit {
        RE::ActorHandle target;
        RE::FormID causeID{};
        RE::FormID targetID{};
        RE::FormID sourceID{};
        RE::FormID projectileID{};
        std::uint64_t sequence{};
        std::uint64_t context{};
        std::uint8_t flags{};
    };

    struct HitTrace {
        double last{-1000.0};
        std::uint64_t count{};
        std::uint64_t suppressed{};
    };

    void Queue(std::function<void()> task);
    void ProcessCombatHit(const CombatHit& hit);
    void TraceCombatHit(const CombatHit& hit, const char* result, int awarded = 0);
    bool ResolveForms();
    bool IsAllowedEquipment(RE::Actor*) const;
    bool HasActiveEffect(RE::Actor*, RE::EffectSetting*) const;
    bool IsUISuspended() const;
    bool IsLoadingTransition() const;
    void UpdateStateFromGame();
    void SyncReadyMarker();
    void RunActivationTransaction();
    bool CastLikePayload(const CastSpec& spec);
    void ConfirmActivation(std::uint64_t token);
    void CancelActivation(std::uint64_t token);
    void CompleteActivation(std::uint64_t token);
    void Notify(const char* name);
    void NotifyDenied();
    static double Now();

    mutable std::recursive_mutex mutex_;
    RageState state_;
    bool available_{};
    bool configured_{};
    bool needsReconcile_{true};
    bool registered_{};
    bool menuSinkRegistered_{};
    bool combatHeartbeatLogged_{};
    bool observedCombat_{};
    bool markerChanging_{};
    float deniedInterval_{0.5f};
    double lastDenied_{-1000.0};
    double lastActivation_{-1000.0};
    double activationDeadline_{};
    std::uint64_t generation_{};
    std::uint64_t hitContext_{};
    std::uint64_t hitSequence_{};
    std::unordered_map<std::string, HitTrace> hitTraces_;
    std::optional<std::uint64_t> activation_;
    RE::EffectSetting* rageEffect_{};
    RE::SpellItem* rageSpell_{};
    RE::SpellItem* readyAbility_{};
    RE::EffectSetting* readyEffect_{};
    RE::EffectSetting* requestEffect_{};
    RE::BGSKeyword* axeKeyword_{};
    std::array<RE::EffectSetting*, 4> movement_{};
    std::vector<RE::FormID> singleWeapons_;
    std::vector<RE::FormID> pairedWeapons_;
    std::vector<CastSpec> successCasts_;
};

void RegisterSerializationCallbacks();
bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* vm);

}
