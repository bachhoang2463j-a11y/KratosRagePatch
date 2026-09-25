#include "GameBridge.h"

namespace kr {
namespace {
constexpr auto kPatch = "KratosRagePatch.esp";
constexpr auto kOriginal = "God of War.esp";

template <class T>
T* Lookup(std::uint32_t id, std::string_view plugin)
{
    auto* data = RE::TESDataHandler::GetSingleton();
    return data ? data->LookupForm<T>(id, plugin) : nullptr;
}
}

GameBridge& GameBridge::Get()
{
    static GameBridge instance;
    return instance;
}

double GameBridge::Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void GameBridge::Queue(std::function<void()> task)
{
    std::lock_guard lock(mutex_);
    const auto generation = generation_;
    if (auto* tasks = SKSE::GetTaskInterface()) {
        tasks->AddTask([this, generation, task = std::move(task)]() {
            std::lock_guard lock(mutex_);
            if (generation == generation_) {
                task();
            }
        });
    }
}

bool GameBridge::ResolveForms()
{
    rageEffect_ = Lookup<RE::EffectSetting>(0x813, kOriginal);
    rageSpell_ = Lookup<RE::SpellItem>(0x814, kOriginal);
    readyAbility_ = Lookup<RE::SpellItem>(0x802, kPatch);
    readyEffect_ = Lookup<RE::EffectSetting>(0x803, kPatch);
    requestEffect_ = Lookup<RE::EffectSetting>(0x805, kPatch);
    axeKeyword_ = Lookup<RE::BGSKeyword>(0x805, kOriginal);
    for (std::uint32_t i = 0; i < movement_.size(); ++i) {
        movement_[i] = Lookup<RE::EffectSetting>(0x801 + i, "Keytrace.esp");
    }
    singleWeapons_.clear();
    pairedWeapons_.clear();
    auto addWeapon = [](auto& list, std::uint32_t id, std::string_view plugin) {
        if (auto* form = Lookup<RE::TESForm>(id, plugin)) {
            list.push_back(form->GetFormID());
        }
    };
    addWeapon(singleWeapons_, 0x808, "[full_inu] Mjolnir.esp");
    addWeapon(singleWeapons_, 0xF5EB, "PPM PKiS.esp");
    for (auto id : {0x804u, 0x807u}) {
        addWeapon(pairedWeapons_, id, "[full_inu] Mjolnir.esp");
    }
    for (auto id : {0xF5EDu, 0xF5EFu, 0xF5F0u, 0xF5F1u, 0xF5F2u}) {
        addWeapon(pairedWeapons_, id, "PPM PKiS.esp");
    }
    successCasts_.clear();
    for (const auto& [id, plugin] : std::initializer_list<std::pair<std::uint32_t, std::string_view>>{
        {0x2FFD2, "Skyrim.esm"}, {0x841, kOriginal}, {0x7A82B, "Skyrim.esm"}}) {
        if (auto* spell = Lookup<RE::SpellItem>(id, plugin)) {
            successCasts_.push_back({spell, 1.0f, 1.0f, false});
        } else {
            spdlog::error("Required original spell {:X} in {} was not resolved", id, plugin);
            return false;
        }
    }
    return rageEffect_ && rageSpell_ && readyAbility_ && readyEffect_ && requestEffect_ && axeKeyword_;
}

void GameBridge::Initialize()
{
    std::lock_guard lock(mutex_);
    available_ = ResolveForms();
    if (!available_) {
        spdlog::error("KratosRage disabled: required plugin records were not resolved");
        return;
    }
    if (!registered_) {
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESMagicEffectApplyEvent>(this);
            events->AddEventSink<RE::TESCombatEvent>(this);
            registered_ = true;
        }
    }
    // UI sink 用独立标记：events holder 注册成功而 UI 未就绪时，下次 Initialize 仍会补注册。
    if (!menuSinkRegistered_) {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(this);
            menuSinkRegistered_ = true;
        }
    }
    spdlog::info("KratosRage records and event sinks initialized");
}

bool GameBridge::IsAvailable() const
{
    std::lock_guard lock(mutex_);
    return available_;
}

Snapshot GameBridge::GetSnapshot() const
{
    std::lock_guard lock(mutex_);
    return state_.GetSnapshot();
}

bool GameBridge::HasActiveEffect(RE::Actor* actor, RE::EffectSetting* effect) const
{
    if (!actor || !effect) {
        return false;
    }
    auto* magicTarget = actor->AsMagicTarget();
    auto* effects = magicTarget ? magicTarget->GetActiveEffectList() : nullptr;
    if (!effects) {
        return false;
    }
    for (auto* active : *effects) {
        if (active && active->GetBaseObject() == effect
            && !active->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
            return true;
        }
    }
    return false;
}

bool GameBridge::IsAllowedEquipment(RE::Actor* actor) const
{
    if (!actor || actor != RE::PlayerCharacter::GetSingleton()) {
        return false;
    }
    auto* right = actor->GetEquippedObject(false);
    auto* left = actor->GetEquippedObject(true);
    auto* unarmedWeapon = right ? right->As<RE::TESObjectWEAP>() : nullptr;
    const bool unarmed = !right
        || (unarmedWeapon && unarmedWeapon->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee);
    if (unarmed && left && left->As<RE::TESObjectARMO>()) {
        return true;
    }
    if (right) {
        if (auto* weapon = right->As<RE::TESObjectWEAP>(); weapon && weapon->HasKeyword(axeKeyword_)) {
            return true;
        }
        if (std::ranges::find(singleWeapons_, right->GetFormID()) != singleWeapons_.end()) {
            return true;
        }
    }
    return right && left
        && std::ranges::find(pairedWeapons_, right->GetFormID()) != pairedWeapons_.end()
        && std::ranges::find(pairedWeapons_, left->GetFormID()) != pairedWeapons_.end();
}

void GameBridge::Configure(RageSettings settings, float deniedInterval)
{
    Queue([this, settings, deniedInterval] {
        state_.Configure(settings);
        configured_ = true;
        deniedInterval_ = std::clamp(deniedInterval, 0.1f, 5.0f);
        UpdateStateFromGame();
        Notify("KR_RageChanged");
    });
}

void GameBridge::UpdateStateFromGame()
{
    if (!available_) {
        return;
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        state_.SuspendCombatTimer();
        return;
    }
    // Loading screens can temporarily report an out-of-combat player before the save is ready.
    if (needsReconcile_ || IsLoadingTransition()) {
        state_.SuspendCombatTimer();
        return;
    }
    const auto before = state_.GetSnapshot();
    const bool actualRaging = HasActiveEffect(player, rageEffect_);
    // Do not dispel the original transformation when clearing the combat resource.
    if (!player->IsInCombat() || player->IsDead()) {
        activation_.reset();   // EndCombat 会清 pending，激活令牌必须同步作废，防止旧 token 被再认领
        state_.EndCombat();
    }
    if (activation_) {
        ConfirmActivation(*activation_);
    }
    if (!activation_) {
        state_.SetRaging(actualRaging);
    }
    state_.SetEligible(configured_ && !player->IsDead() && IsAllowedEquipment(player));
    SyncReadyMarker();
    const auto after = state_.GetSnapshot();
    if (before.eligible != after.eligible || before.raging != after.raging || before.ready != after.ready
        || before.current != after.current) {
        Notify("KR_RageChanged");
    }
}

void GameBridge::SyncReadyMarker()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !readyAbility_ || markerChanging_) {
        return;
    }
    const bool shouldHave = state_.GetSnapshot().ready;
    const bool has = player->HasSpell(readyAbility_);
    if (shouldHave != has) {
        markerChanging_ = true;
        if (shouldHave) {
            player->AddSpell(readyAbility_);
        } else {
            player->RemoveSpell(readyAbility_);
        }
        markerChanging_ = false;
    }
}

void GameBridge::RefreshEquipmentState()
{
    Queue([this] { UpdateStateFromGame(); });
}

// Papyrus 0.5s 单发心跳：唯一怒气增长通道。战斗中每满 1 秒 +2；菜单/暂停/非战斗
// 不增长也不补时间；脱战或死亡由 UpdateStateFromGame/UpdateCombat 清零。
void GameBridge::TickCombat()
{
    Queue([this] {
        if (!available_) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            state_.SuspendCombatTimer();
            return;
        }
        const bool inCombat = player->IsInCombat() && !player->IsDead();
        if (inCombat != observedCombat_) {
            observedCombat_ = inCombat;
            spdlog::info(inCombat ? "KratosRage combat heartbeat: player entered combat"
                                  : "KratosRage combat heartbeat: player left combat, rage cleared");
        } else if (!combatHeartbeatLogged_) {
            combatHeartbeatLogged_ = true;
            spdlog::info("KratosRage combat heartbeat received");
        }
        const auto before = state_.GetSnapshot();
        UpdateStateFromGame();
        AwardResult result{};
        if (needsReconcile_ || IsLoadingTransition()) {
            // 读档/Loading 过渡：权威 combat 尚未拉取，心跳只停表，绝不在此阶段清掉存档怒气。
            state_.SuspendCombatTimer();
        } else {
            result = state_.UpdateCombat(Now(), inCombat, IsUISuspended());
            if (!inCombat && activation_) {
                // 双保险：EndCombat 清 pending 时激活令牌必须同步作废。
                activation_.reset();
            }
        }
        const auto after = state_.GetSnapshot();
        if (result.awarded > 0 || before.current != after.current) {
            SyncReadyMarker();
            Notify("KR_RageChanged");
        }
        if (result.becameFull) {
            spdlog::info("KratosRage rage reached maximum via combat timer");
            Notify("KR_RageBecameFull");
        }
    });
}

// UI/暂停判定：任一命中即视为本 tick 不在活跃游戏时间，时钟只停不补。
bool GameBridge::IsUISuspended() const
{
    if (auto* ui = RE::UI::GetSingleton()) {
        if (ui->GameIsPaused() || ui->IsItemMenuOpen() || ui->IsApplicationMenuOpen() || ui->IsModalMenuOpen()
            || ui->IsMenuOpen("Loading Menu") || ui->IsMenuOpen("Console") || ui->IsMenuOpen("Dialogue Menu")) {
            return true;
        }
    } else {
        return true;  // UI 单例缺失时保守停表
    }
    if (auto* main = RE::Main::GetSingleton()) {
        if (main->freezeTime || !main->gameActive) {
            return true;
        }
    } else {
        return true;
    }
    return false;
}

// 读档过渡期判定：Loading Menu 打开期间不推进任何战斗状态。
bool GameBridge::IsLoadingTransition() const
{
    auto* ui = RE::UI::GetSingleton();
    return ui && ui->IsMenuOpen("Loading Menu");
}

// Legacy entry retained for source and test compatibility; damage hooks are no longer installed.
void GameBridge::SubmitPlayerDamage(RE::FormID, RE::FormID, float) {}

// 0.2.0 起计分只走 TickCombat 战斗心跳；以下旧 PEX 入口保留签名供旧脚本调用，全部无操作。
void GameBridge::SubmitWeaponHit(RE::TESObjectREFR*, RE::TESForm*, RE::BGSProjectile*, int) {}

void GameBridge::SubmitMagicHit(RE::TESObjectREFR*, RE::TESForm*, RE::BGSProjectile*) {}

void GameBridge::SubmitProjectileHit(RE::TESObjectREFR*, RE::TESForm*, RE::BGSProjectile*) {}

void GameBridge::SubmitIncomingHit(RE::TESObjectREFR*, RE::TESForm*, RE::BGSProjectile*, bool, bool, bool, bool) {}

void GameBridge::SubmitKill(RE::Actor*, RE::Actor*) {}

void GameBridge::NotifyActivationAttempt(std::string control, bool rightAttack,
    bool forward, bool back, bool left, bool right)
{
    Queue([this, control = std::move(control), rightAttack, forward, back, left, right] {
        auto* player = RE::PlayerCharacter::GetSingleton();
        UpdateStateFromGame();
        if (!player || control != "Right Attack/Block" || !rightAttack) {
            return;
        }
        const bool keys = (forward || HasActiveEffect(player, movement_[0]))
            && (left || HasActiveEffect(player, movement_[1]))
            && (right || HasActiveEffect(player, movement_[3]));
        if (keys && state_.GetSnapshot().eligible && !state_.GetSnapshot().raging
            && !state_.GetSnapshot().pending && !state_.GetSnapshot().ready) {
            NotifyDenied();
        }
        (void)back;
    });
}

bool GameBridge::CastLikePayload(const CastSpec& spec)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !spec.spell) {
        return false;
    }
    auto* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
    if (!caster) {
        return false;
    }
    caster->CastSpellImmediate(spec.spell, false, spec.selfTarget ? player : nullptr,
        spec.effectiveness, false, spec.magnitude, player);
    return true;
}

void GameBridge::RunActivationTransaction()
{
    // 读档窗口 / Loading / 暂停菜单期间不得用旧 eligible 或旧 marker 启动激活。
    if (!available_ || activation_ || needsReconcile_ || IsLoadingTransition() || IsUISuspended()
        || Now() - lastActivation_ < 0.25) {
        return;
    }
    UpdateStateFromGame();
    const auto token = state_.TryBeginActivation();
    if (!token) {
        if (!state_.GetSnapshot().raging) {
            NotifyDenied();
        }
        return;
    }
    activation_ = *token;
    lastActivation_ = Now();
    activationDeadline_ = lastActivation_ + 0.5;
    SyncReadyMarker();
    Notify("KR_RageChanged");
    if (!CastLikePayload({rageSpell_, 1.0f, 1.0f, false})) {
        CancelActivation(*token);
        return;
    }
    ConfirmActivation(*token);
}

void GameBridge::ConfirmActivation(std::uint64_t token)
{
    if (!activation_ || *activation_ != token) {
        return;
    }
    if (HasActiveEffect(RE::PlayerCharacter::GetSingleton(), rageEffect_)) {
        CompleteActivation(token);
    } else if (Now() >= activationDeadline_) {
        CancelActivation(token);
        spdlog::warn("Rage effect was not confirmed; no rage consumed");
    }
}

void GameBridge::CancelActivation(std::uint64_t token)
{
    if (!activation_ || *activation_ != token) {
        return;
    }
    state_.AbortActivation(token);
    activation_.reset();
    SyncReadyMarker();
    Notify("KR_RageChanged");
    NotifyDenied();
}

void GameBridge::CompleteActivation(std::uint64_t token)
{
    if (!state_.CommitActivation(token)) {
        return;
    }
    activation_.reset();
    SyncReadyMarker();
    for (const auto& cast : successCasts_) {
        CastLikePayload(cast);
    }
    Notify("KR_RageChanged");
    spdlog::info("Rage activation confirmed and resource consumed");
}

void GameBridge::Notify(const char* name)
{
    if (auto* events = SKSE::GetModCallbackEventSource()) {
        SKSE::ModCallbackEvent event{RE::BSFixedString(name), RE::BSFixedString(""),
            static_cast<float>(state_.GetSnapshot().current), RE::PlayerCharacter::GetSingleton()};
        events->SendEvent(&event);
    }
}

void GameBridge::NotifyDenied()
{
    const auto now = Now();
    if (now - lastDenied_ >= deniedInterval_) {
        lastDenied_ = now;
        Notify("KR_RageDenied");
    }
}

void GameBridge::ReconcileAfterLoad()
{
    Queue([this] {
        if (!needsReconcile_) {
            return;
        }
        needsReconcile_ = false;
        activation_.reset();
        state_.ReconcileAfterLoad(HasActiveEffect(RE::PlayerCharacter::GetSingleton(), rageEffect_));
        UpdateStateFromGame();
        Notify("KR_RageChanged");
    });
}

SavedState GameBridge::Save() const
{
    std::lock_guard lock(mutex_);
    return state_.Save();
}

void GameBridge::Restore(const SavedState& state)
{
    std::lock_guard lock(mutex_);
    activation_.reset();
    configured_ = false;
    needsReconcile_ = true;
    ++generation_;
    combatHeartbeatLogged_ = false;
    observedCombat_ = false;
    state_.Restore(state);
}

void GameBridge::Revert()
{
    std::lock_guard lock(mutex_);
    state_.Reset();
    configured_ = false;
    needsReconcile_ = true;
    ++generation_;
    activation_.reset();
    lastDenied_ = -1000.0;
    lastActivation_ = -1000.0;
    combatHeartbeatLogged_ = false;
    observedCombat_ = false;
}

RE::BSEventNotifyControl GameBridge::ProcessEvent(const RE::TESMagicEffectApplyEvent* event,
    RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*)
{
    if (event && event->target.get() == RE::PlayerCharacter::GetSingleton()
        && event->caster.get() == RE::PlayerCharacter::GetSingleton()) {
        if (requestEffect_ && event->magicEffect == requestEffect_->GetFormID()) {
            Queue([this] { RunActivationTransaction(); });
        } else if (rageEffect_ && event->magicEffect == rageEffect_->GetFormID()) {
            Queue([this] {
                if (activation_) {
                    ConfirmActivation(*activation_);
                } else {
                    UpdateStateFromGame();
                }
            });
        }
    }
    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl GameBridge::ProcessEvent(const RE::TESCombatEvent* event,
    RE::BSTEventSource<RE::TESCombatEvent>*)
{
    if (event && event->actor.get() == RE::PlayerCharacter::GetSingleton()) {
        Queue([this] { UpdateStateFromGame(); });
    }
    return RE::BSEventNotifyControl::kContinue;
}

// 任何菜单的打开或关闭都把战斗时钟的起点抹掉：短暂暂停也不产生可补算的间隔。
// 只在锁内停表，不捕获事件指针入队。
RE::BSEventNotifyControl GameBridge::ProcessEvent(const RE::MenuOpenCloseEvent* event,
    RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
    if (event) {
        std::lock_guard lock(mutex_);
        state_.SuspendCombatTimer();
    }
    return RE::BSEventNotifyControl::kContinue;
}

}
