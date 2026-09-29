#include "RageState.h"

#include <algorithm>
#include <cmath>

namespace kr {

void RageState::Configure(const RageSettings& settings)
{
    settings_ = settings;
    settings_.maximum = std::clamp(settings_.maximum, 1, 10000);
    settings_.maxGainPerSecond = std::clamp(settings_.maxGainPerSecond, 1, 10000);
    settings_.normalHit = std::clamp(settings_.normalHit, 0, 10000);
    settings_.powerHit = std::clamp(settings_.powerHit, 0, 10000);
    settings_.specialHit = std::clamp(settings_.specialHit, 0, 10000);
    settings_.receivedHit = std::clamp(settings_.receivedHit, 0, 10000);
    settings_.kill = std::clamp(settings_.kill, 0, 10000);
    if (loadClampSource_) {
        // 读档发生在这份权威配置之前：按存档里的原始怒气值重新钳制，不丢值。
        current_ = std::clamp(*loadClampSource_, 0, settings_.maximum);
        loadClampSource_.reset();
    } else {
        current_ = std::clamp(current_, 0, settings_.maximum);
    }
}

void RageState::SetEligible(bool eligible) { eligible_ = eligible; }

void RageState::SetRaging(bool active)
{
    if (active != raging_) {
        ClearTransient();
        // 变身状态发生切换（含由外部路径触发）：本地启动请求当场结算。这里按 abort 语义处理——
        // 不扣怒气、保留满怒可重试——但绝不能悬挂到变身结束：悬空的 pending 会在变身期间被
        // Save() 写成 pending=true，读档时 ReconcileAfterLoad 见到 pending+实际变身就会误扣怒气。
        pending_.reset();
    }
    raging_ = active;
}

int RageState::GainFor(GainKind kind) const
{
    switch (kind) {
    case GainKind::Normal: return settings_.normalHit;
    case GainKind::Power: return settings_.powerHit;
    case GainKind::Special: return settings_.specialHit;
    case GainKind::Received: return settings_.receivedHit;
    case GainKind::Kill: return settings_.kill;
    }
    return 0;
}

void RageState::PruneGainWindow(double nowSeconds)
{
    while (!gains_.empty() && nowSeconds - gains_.front().time >= 1.0) {
        gains_.pop_front();
    }
    while (!hits_.empty() && nowSeconds - hits_.front().time >= 0.1) {
        hits_.pop_front();
    }
}

bool RageState::IsDuplicateHit(const GainEvent& event, double nowSeconds)
{
    return std::any_of(hits_.begin(), hits_.end(), [&](const HitStamp& stamp) {
        const bool sameChannel = (event.kind == GainKind::Kill) == (stamp.event.kind == GainKind::Kill)
            && (event.kind == GainKind::Received) == (stamp.event.kind == GainKind::Received);
        return sameChannel && event.actorID == stamp.event.actorID
            && event.sourceID == stamp.event.sourceID && nowSeconds - stamp.time < 0.1;
    });
}

AwardResult RageState::TryAward(const GainEvent& event, double nowSeconds)
{
    if (!std::isfinite(nowSeconds) || !eligible_ || raging_ || pending_ || restoredPending_) {
        return {};
    }
    if (lastTime_ && nowSeconds < *lastTime_) {
        nowSeconds = *lastTime_;
    }
    lastTime_ = nowSeconds;
    PruneGainWindow(nowSeconds);
    if (IsDuplicateHit(event, nowSeconds)) {
        return {};
    }
    hits_.push_back({event, nowSeconds});
    int inWindow = 0;
    for (const auto& stamp : gains_) {
        inWindow += stamp.amount;
    }
    const int award = std::max(0, std::min({GainFor(event.kind), settings_.maximum - current_,
        settings_.maxGainPerSecond - inWindow}));
    const bool wasFull = current_ >= settings_.maximum;
    if (award > 0) {
        current_ += award;
        gains_.push_back({nowSeconds, award});
        loadClampSource_.reset();
    }
    return {award, !wasFull && current_ >= settings_.maximum};
}

AwardResult RageState::TryAwardPlayerDamage(double nowSeconds)
{
    if (!std::isfinite(nowSeconds)) {
        return {};
    }
    if (lastDamageTime_ && nowSeconds < *lastDamageTime_) {
        nowSeconds = *lastDamageTime_;
    }
    lastDamageTime_ = nowSeconds;
    while (!damageGains_.empty() && nowSeconds - damageGains_.front().time >= 1.0) {
        damageGains_.pop_front();
    }
    int inWindow = 0;
    for (const auto& stamp : damageGains_) {
        inWindow += stamp.amount;
    }
    const bool wasFull = current_ >= settings_.maximum;
    const int award = std::max(0, std::min({2, settings_.maximum - current_, 10 - inWindow}));
    if (award > 0) {
        current_ += award;
        damageGains_.push_back({nowSeconds, award});
        loadClampSource_.reset();
    }
    return {award, !wasFull && current_ >= settings_.maximum};
}

AwardResult RageState::TryAwardCombatHit(bool inCombat, bool suspended)
{
    if (!inCombat) {
        EndCombat();
        return {};
    }
    if (suspended) {
        return {};
    }
    const bool wasFull = current_ >= settings_.maximum;
    const int award = std::max(0, std::min(2, settings_.maximum - current_));
    if (award > 0) {
        current_ += award;
        loadClampSource_.reset();
    }
    return {award, !wasFull && current_ >= settings_.maximum};
}

AwardResult RageState::UpdateCombat(double nowSeconds, bool inCombat, bool suspended)
{
    if (!inCombat) {
        EndCombat();
        return {};
    }
    if (suspended || !std::isfinite(nowSeconds)) {
        SuspendCombatTimer();
        return {};
    }
    if (!lastCombatTime_) {
        lastCombatTime_ = nowSeconds;
        return {};
    }
    const double elapsed = nowSeconds - *lastCombatTime_;
    lastCombatTime_ = nowSeconds;
    // A stalled heartbeat cannot establish how much of the gap was active gameplay.
    if (elapsed < 0.0 || elapsed > 1.5) {
        combatSeconds_ = 0.0;
        return {};
    }
    if (current_ >= settings_.maximum) {
        combatSeconds_ = 0.0;
        return {};
    }
    combatSeconds_ += elapsed;
    const int seconds = static_cast<int>(std::floor(combatSeconds_ + 0.000001));
    if (seconds <= 0) {
        return {};
    }
    combatSeconds_ = std::max(0.0, combatSeconds_ - seconds);
    const int award = std::min(seconds * 2, settings_.maximum - current_);
    current_ += award;
    loadClampSource_.reset();
    return {award, current_ >= settings_.maximum};
}

void RageState::SuspendCombatTimer()
{
    lastCombatTime_.reset();
}

void RageState::EndCombat()
{
    current_ = 0;
    pending_.reset();
    restoredPending_ = false;
    loadClampSource_.reset();
    ClearTransient();
}

std::optional<std::uint64_t> RageState::TryBeginActivation()
{
    if (!GetSnapshot().ready) {
        return std::nullopt;
    }
    pending_ = nextToken_++;
    return pending_;
}

bool RageState::CommitActivation(std::uint64_t token)
{
    if (!pending_ || *pending_ != token) {
        return false;
    }
    current_ = 0;
    pending_.reset();
    loadClampSource_.reset();
    raging_ = true;
    ClearTransient();
    return true;
}

bool RageState::AbortActivation(std::uint64_t token)
{
    if (!pending_ || *pending_ != token) {
        return false;
    }
    pending_.reset();
    return true;
}

Snapshot RageState::GetSnapshot() const
{
    const bool pending = pending_.has_value() || restoredPending_;
    return {current_, settings_.maximum, eligible_, raging_, pending,
        eligible_ && !raging_ && !pending && current_ >= settings_.maximum};
}

SavedState RageState::Save() const
{
    // 权威 Configure 尚未到达时 current_ 可能仍是被临时上限钳制过的值；持久化必须写回原始怒气，
    // 否则「读档后立刻存档」会把丢掉的怒气固化进存档。延迟值只在真实状态推进时被清除。
    const int persisted = loadClampSource_ ? *loadClampSource_ : current_;
    return {1, persisted, pending_.has_value() || restoredPending_, raging_};
}

void RageState::Restore(const SavedState& state)
{
    ClearTransient();
    damageGains_.clear();
    lastDamageTime_.reset();
    pending_.reset();
    loadClampSource_.reset();
    eligible_ = false;
    if (state.version != 1) {
        current_ = 0;
        restoredPending_ = false;
        raging_ = false;
        return;
    }
    current_ = std::clamp(state.current, 0, settings_.maximum);
    if (state.current > settings_.maximum) {
        // 存档怒气被当前上限截断了。此刻的 settings_ 可能还是默认值（Papyrus 的
        // ConfigureRage 在 OnPlayerLoadGame 里晚于反序列化，真实顺序是 Restore -> Reconcile ->
        // Configure），先记住原值，等权威 Configure 到达再定；普通 Reconcile 不得清除它。
        loadClampSource_ = state.current;
    }
    restoredPending_ = state.pending;
    raging_ = state.raging;
}

void RageState::ReconcileAfterLoad(bool actualRaging)
{
    if (restoredPending_ && actualRaging) {
        // 原效果确实生效：怒气被真实消耗，这是不可保留的状态推进，延迟值随之作废。
        current_ = 0;
        loadClampSource_.reset();
    }
    restoredPending_ = false;
    pending_.reset();
    raging_ = actualRaging;
    ClearTransient();
}

void RageState::ClearTransient()
{
    lastCombatTime_.reset();
    combatSeconds_ = 0.0;
    gains_.clear();
    hits_.clear();
    lastTime_.reset();
}

void RageState::Reset()
{
    damageGains_.clear();
    lastDamageTime_.reset();
    current_ = 0;
    eligible_ = false;
    raging_ = false;
    restoredPending_ = false;
    pending_.reset();
    loadClampSource_.reset();
    ClearTransient();
}

}
