#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace kr {

enum class GainKind : std::uint8_t { Normal, Power, Special, Received, Kill };

struct RageSettings {
    int maximum{100};
    int normalHit{2};
    int powerHit{4};
    int specialHit{3};
    int receivedHit{1};
    int kill{5};
    int maxGainPerSecond{8};
};

struct GainEvent {
    GainKind kind{GainKind::Normal};
    std::uint32_t actorID{};
    std::uint32_t sourceID{};
};

struct AwardResult {
    int awarded{};
    bool becameFull{};
};

struct Snapshot {
    int current{};
    int maximum{100};
    bool eligible{};
    bool raging{};
    bool pending{};
    bool ready{};
};

struct SavedState {
    std::uint32_t version{1};
    int current{};
    bool pending{};
    bool raging{};
};

class RageState {
public:
    void Configure(const RageSettings& settings);
    void SetEligible(bool eligible);
    void SetRaging(bool active);
    AwardResult TryAward(const GainEvent& event, double nowSeconds);
    AwardResult TryAwardPlayerDamage(double nowSeconds);
    std::optional<std::uint64_t> TryBeginActivation();
    bool CommitActivation(std::uint64_t token);
    bool AbortActivation(std::uint64_t token);
    Snapshot GetSnapshot() const;
    SavedState Save() const;
    void Restore(const SavedState& state);
    void ReconcileAfterLoad(bool actualRaging);
    void Reset();

private:
    struct GainStamp { double time; int amount; };
    struct HitStamp { GainEvent event; double time; };
    bool IsDuplicateHit(const GainEvent& event, double nowSeconds);
    void PruneGainWindow(double nowSeconds);
    int GainFor(GainKind kind) const;
    void ClearTransient();

    RageSettings settings_{};
    int current_{};
    bool eligible_{};
    bool raging_{};
    bool restoredPending_{};
    std::uint64_t nextToken_{1};
    std::optional<std::uint64_t> pending_{};
    // 读档时被当前（可能还不是权威的）上限截断掉的原始怒气值；等到真正的 Configure 到达后
    // 再按恢复出的真实上限重新钳制，避免自定义上限（如 > 默认 100）在加载阶段丢怒气。
    std::optional<int> loadClampSource_{};
    std::deque<GainStamp> gains_;
    std::deque<HitStamp> hits_;
    std::optional<double> lastTime_{};
    // 实际伤害预算跨变身保留，避免消费怒气后在同一秒重新领取额度。
    std::deque<GainStamp> damageGains_;
    std::optional<double> lastDamageTime_{};
};

}
