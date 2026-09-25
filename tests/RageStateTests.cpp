// ---------------------------------------------------------------------------
// RageState 离线单元测试：怒气获取、去重、滚动速率上限、上限封顶、配置钳制。
// 仅依赖 src/RageState.h/.cpp，不涉及 Skyrim / SKSE / 网络。
// ---------------------------------------------------------------------------

#include "../src/RageState.h"

#include "TestSupport.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

kr::GainEvent Ev(kr::GainKind kind, std::uint32_t actor, std::uint32_t source)
{
    kr::GainEvent event;
    event.kind = kind;
    event.actorID = actor;
    event.sourceID = source;
    return event;
}

void ApplySettingsJson(const krtest::Json& cfg, kr::RageSettings& settings)
{
    if (const krtest::Json* v = cfg.Find("maximum")) settings.maximum = v->AsInt(settings.maximum);
    if (const krtest::Json* v = cfg.Find("normalHit")) settings.normalHit = v->AsInt(settings.normalHit);
    if (const krtest::Json* v = cfg.Find("powerHit")) settings.powerHit = v->AsInt(settings.powerHit);
    if (const krtest::Json* v = cfg.Find("specialHit")) settings.specialHit = v->AsInt(settings.specialHit);
    if (const krtest::Json* v = cfg.Find("receivedHit")) settings.receivedHit = v->AsInt(settings.receivedHit);
    if (const krtest::Json* v = cfg.Find("kill")) settings.kill = v->AsInt(settings.kill);
    if (const krtest::Json* v = cfg.Find("maxGainPerSecond")) settings.maxGainPerSecond = v->AsInt(settings.maxGainPerSecond);
}

kr::GainKind KindFromName(const std::string& name, bool& ok)
{
    ok = true;
    if (name == "Normal") return kr::GainKind::Normal;
    if (name == "Power") return kr::GainKind::Power;
    if (name == "Special") return kr::GainKind::Special;
    if (name == "Received") return kr::GainKind::Received;
    if (name == "Kill") return kr::GainKind::Kill;
    ok = false;
    return kr::GainKind::Normal;
}

void CheckSnapshotFields(const kr::RageState& state, const krtest::Json& expect, const char* file, int line)
{
    const kr::Snapshot snap = state.GetSnapshot();
    if (const krtest::Json* v = expect.Find("current")) krtest::ExpectEqI(snap.current, v->AsInt(), "snapshot.current", file, line);
    if (const krtest::Json* v = expect.Find("maximum")) krtest::ExpectEqI(snap.maximum, v->AsInt(), "snapshot.maximum", file, line);
    if (const krtest::Json* v = expect.Find("eligible")) krtest::ExpectEqB(snap.eligible, v->AsBool(), "snapshot.eligible", file, line);
    if (const krtest::Json* v = expect.Find("raging")) krtest::ExpectEqB(snap.raging, v->AsBool(), "snapshot.raging", file, line);
    if (const krtest::Json* v = expect.Find("pending")) krtest::ExpectEqB(snap.pending, v->AsBool(), "snapshot.pending", file, line);
    if (const krtest::Json* v = expect.Find("ready")) krtest::ExpectEqB(snap.ready, v->AsBool(), "snapshot.ready", file, line);
}

void RunHitCase(const krtest::Json& fixtureCase)
{
    kr::RageSettings settings;
    if (const krtest::Json* cfg = fixtureCase.Find("settings")) ApplySettingsJson(*cfg, settings);

    kr::RageState state;
    state.Configure(settings);

    bool eligible = true;
    bool raging = false;
    if (const krtest::Json* pre = fixtureCase.Find("pre")) {
        if (const krtest::Json* v = pre->Find("eligible")) eligible = v->AsBool(eligible);
        if (const krtest::Json* v = pre->Find("raging")) raging = v->AsBool(raging);
    }
    state.SetEligible(eligible);
    state.SetRaging(raging);

    std::vector<kr::AwardResult> results;
    if (const krtest::Json* events = fixtureCase.Find("events")) {
        if (events->IsArray()) {
            for (const krtest::Json& event : events->items) {
                const krtest::Json* kindJson = event.Find("kind");
                bool ok = true;
                const kr::GainKind kind = KindFromName(kindJson ? kindJson->AsString() : std::string(), ok);
                if (!ok) {
                    krtest::RecordFailure(__FILE__, __LINE__, "unknown gain kind in fixture");
                    continue;
                }
                kr::GainEvent gain;
                gain.kind = kind;
                if (const krtest::Json* v = event.Find("actor")) gain.actorID = v->AsU32(0);
                if (const krtest::Json* v = event.Find("source")) gain.sourceID = v->AsU32(0);
                const krtest::Json* t = event.Find("t");
                results.push_back(state.TryAward(gain, t ? t->AsNumber(0.0) : 0.0));
            }
        }
    }

    const krtest::Json* expect = fixtureCase.Find("expect");
    if (!expect) return;

    if (const krtest::Json* awards = expect->Find("awarded")) {
        if (!awards->IsArray()) {
            krtest::RecordFailure(__FILE__, __LINE__, "expect.awarded must be an array");
        } else {
            krtest::ExpectEqI(static_cast<long long>(results.size()),
                              static_cast<long long>(awards->items.size()),
                              "event count", __FILE__, __LINE__);
            const std::size_t n = std::min(results.size(), awards->items.size());
            for (std::size_t i = 0; i < n; ++i) {
                krtest::ExpectEqI(results[i].awarded, awards->items[i].AsInt(),
                                  "awarded[" + std::to_string(i) + "]", __FILE__, __LINE__);
            }
        }
    }

    if (const krtest::Json* full = expect->Find("becameFull")) {
        if (!full->IsArray()) {
            krtest::RecordFailure(__FILE__, __LINE__, "expect.becameFull must be an array");
        } else {
            const std::size_t n = std::min(results.size(), full->items.size());
            for (std::size_t i = 0; i < n; ++i) {
                krtest::ExpectEqB(results[i].becameFull, full->items[i].AsBool(),
                                  "becameFull[" + std::to_string(i) + "]", __FILE__, __LINE__);
            }
        }
    }

    CheckSnapshotFields(state, *expect, __FILE__, __LINE__);
}

}  // namespace

// ============================ 手工用例（规则逐条） ==========================

// 默认配比：普通2 / 重击4 / 特殊3 / 受击1 / 击杀5；每个事件间隔 1 秒互不占用同一窗口。
KR_TEST(default_gain_values_per_kind)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});
    state.SetEligible(true);

    const kr::AwardResult normal = state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0);
    const kr::AwardResult power = state.TryAward(Ev(kr::GainKind::Power, 2, 72), 1.0);
    const kr::AwardResult special = state.TryAward(Ev(kr::GainKind::Special, 3, 73), 2.0);
    const kr::AwardResult received = state.TryAward(Ev(kr::GainKind::Received, 4, 74), 3.0);
    const kr::AwardResult kill = state.TryAward(Ev(kr::GainKind::Kill, 5, 75), 4.0);

    KRT_EQI(normal.awarded, 2);
    KRT_EQI(power.awarded, 4);
    KRT_EQI(special.awarded, 3);
    KRT_EQI(received.awarded, 1);
    KRT_EQI(kill.awarded, 5);

    const kr::Snapshot snap = state.GetSnapshot();
    KRT_EQI(snap.current, 15);
    KRT_EQI(snap.maximum, 100);
    KRT_EQB(snap.ready, false);
}

// 上限 100 封顶；满怒信号只报一次（再命中不重复上报）。
KR_TEST(maximum_cap_100_and_became_full_reported_once)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 100;
    settings.normalHit = 1000;
    settings.maxGainPerSecond = 1000;
    state.Configure(settings);
    state.SetEligible(true);

    const kr::AwardResult first = state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0);
    KRT_EQI(first.awarded, 100);
    KRT_EQB(first.becameFull, true);
    KRT_EQI(state.GetSnapshot().current, 100);

    const kr::AwardResult second = state.TryAward(Ev(kr::GainKind::Normal, 2, 72), 0.3);
    KRT_EQI(second.awarded, 0);
    KRT_EQB(second.becameFull, false);
    KRT_EQI(state.GetSnapshot().current, 100);
    KRT_EQB(state.GetSnapshot().ready, true);
}

// 配置范围钳制：上限下限 1、每秒预算下限 1、负增益归零；上限下调时 current 被钳制。
KR_TEST(configure_clamps_ranges_and_reconciles_current)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 0;
    settings.maxGainPerSecond = 0;
    settings.normalHit = -7;
    settings.kill = 100000;
    state.Configure(settings);
    state.SetEligible(true);

    KRT_EQI(state.GetSnapshot().maximum, 1);
    const kr::AwardResult clamped = state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0);
    KRT_EQI(clamped.awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 0);

    kr::RageState other;
    kr::RageSettings high;
    high.maximum = 100;
    high.normalHit = 8;
    high.maxGainPerSecond = 100;
    other.Configure(high);
    other.SetEligible(true);
    KRT_EQI(other.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 8);

    kr::RageSettings lowered = high;
    lowered.maximum = 5;
    other.Configure(lowered);
    KRT_EQI(other.GetSnapshot().current, 5);
}

// 同源同目标 0.1 秒内重复命中只计一次。
KR_TEST(dedupe_same_source_and_target_within_0_1s)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});
    state.SetEligible(true);

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.05).awarded, 0);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.099).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 2);
}

// 0.1 秒整边界：窗口按 [now-0.1, now) 判定，恰好过期后允许再次计分。
KR_TEST(dedupe_window_boundary_at_exactly_0_1s_allows_again)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});
    state.SetEligible(true);

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.1).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 4);
}

// 攻击类别不绕过去重：同源同目标的普通/重击/特殊在 0.1 秒内视为同一次命中。
KR_TEST(dedupe_attack_category_does_not_bypass)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});
    state.SetEligible(true);

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Power, 1, 71), 0.02).awarded, 0);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Special, 1, 71), 0.03).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 2);
}

// 不同目标或不同来源允许分别计分（0.1 秒内）。
KR_TEST(dedupe_different_target_or_source_allowed)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});
    state.SetEligible(true);

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 2, 71), 0.01).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 72), 0.02).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 2, 72), 0.03).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 8);
}

// 同一时间戳（无回拨）的重复事件仍然去重。
KR_TEST(same_timestamp_duplicate_is_still_deduped)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});
    state.SetEligible(true);

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 5.0).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 5.0).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 2);
}

// 滚动 1 秒预算：部分裁剪（不足额按剩余额度发）、超额丢弃。
KR_TEST(rolling_window_partial_clip_and_drop)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.normalHit = 3;
    settings.maxGainPerSecond = 8;
    state.Configure(settings);
    state.SetEligible(true);

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 3);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 2, 72), 0.1).awarded, 3);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 3, 73), 0.2).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 4, 74), 0.3).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 8);

    // 1 秒窗口滚动后旧额度释放。
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 5, 75), 1.0).awarded, 3);
    KRT_EQI(state.GetSnapshot().current, 11);
}

// 每秒上限由所有来源共享，不区分攻击类别。
KR_TEST(rolling_window_counts_all_sources_together)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});
    state.SetEligible(true);

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Kill, 1, 71), 0.0).awarded, 5);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 2, 72), 0.1).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Special, 3, 73), 0.2).awarded, 1);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 4, 74), 0.3).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 8);
}

// 变身中 / 未装备 / 已提交启动请求期间都不积攒。
KR_TEST(no_gain_when_ineligible_raging_or_pending)
{
    kr::RageState ineligible;
    ineligible.Configure(kr::RageSettings{});
    ineligible.SetEligible(false);
    KRT_EQI(ineligible.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 0);
    KRT_EQI(ineligible.GetSnapshot().current, 0);
    KRT_EQB(ineligible.GetSnapshot().ready, false);

    kr::RageState raging;
    raging.Configure(kr::RageSettings{});
    raging.SetEligible(true);
    raging.SetRaging(true);
    KRT_EQI(raging.TryAward(Ev(kr::GainKind::Kill, 1, 71), 0.0).awarded, 0);
    KRT_EQI(raging.GetSnapshot().current, 0);
    KRT_EQB(raging.GetSnapshot().ready, false);

    kr::RageState pending;
    kr::RageSettings small;
    small.maximum = 4;
    small.normalHit = 4;
    pending.Configure(small);
    pending.SetEligible(true);
    KRT_EQI(pending.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);
    KRT_EXPECT_HAS(pending.TryBeginActivation());
    KRT_EQI(pending.TryAward(Ev(kr::GainKind::Kill, 2, 72), 0.3).awarded, 0);
    KRT_EQI(pending.GetSnapshot().current, 4);
    KRT_EQB(pending.GetSnapshot().pending, true);
    KRT_EQB(pending.GetSnapshot().ready, false);
}

// 读档带来的 pending 未结算前不积攒。
KR_TEST(no_gain_while_restored_pending_is_unresolved)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;
    state.Configure(settings);

    kr::SavedState saved;
    saved.current = 4;
    saved.pending = true;
    state.Restore(saved);
    state.SetEligible(true);

    KRT_EQB(state.GetSnapshot().pending, true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 4);
}

// 非有限时间戳（NaN/Inf）被拒绝，不积攒也不崩溃。
KR_TEST(non_finite_time_is_ignored)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});
    state.SetEligible(true);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), nan).awarded, 0);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 2, 72), inf).awarded, 0);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 3, 73), -inf).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 0);
}

// ---------------------------------------------------------------------------
// SPEC_GAP：按契约编写、当前实现下预期失败的用例（证据，不是测试写错）。
// ---------------------------------------------------------------------------

// 契约：0.1 秒去重窗口内的同一次命中不得因为时钟回拨而再次计分。
// 现状：TryAward 在 nowSeconds < lastTime_ 时调用 ClearTransient()，连同去重表一起清空，
//       因此 -1 毫秒的回拨就能让同一 (actor, source) 命中重复计分。
KR_TEST(SPEC_GAP_backward_clock_must_not_reset_dedupe_window)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});
    state.SetEligible(true);

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 10.0).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 9.999).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 2);
}

// 契约：1 秒内的 8 点预算不得因为时钟回拨而重置。
// 现状：回拨触发 ClearTransient() 清空 gains_，窗口内已用额度归零，可再刷满 8 点。
KR_TEST(SPEC_GAP_backward_clock_must_not_reset_rolling_budget)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.normalHit = 2;
    settings.maxGainPerSecond = 8;
    state.Configure(settings);
    state.SetEligible(true);

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 10.0).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 2, 72), 10.01).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 3, 73), 10.02).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 4, 74), 10.03).awarded, 2);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 5, 75), 9.5).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 8);
}

// ============================ fixture 驱动用例 ==============================

namespace krtest {

int RunHitFixtureCases(const std::string& fixtureDir)
{
    const std::string path = fixtureDir + "/hit_cases.json";
    Json root;
    std::string error;
    SetCurrentCase("fixture:hit_cases.json");
    if (!LoadJsonFile(path, root, error)) {
        RecordFailure(path.c_str(), 0, "cannot load fixture: " + error);
        return 1;
    }
    if (!root.IsArray()) {
        RecordFailure(path.c_str(), 0, "fixture root must be an array");
        return 1;
    }

    int failed = 0;
    for (std::size_t i = 0; i < root.items.size(); ++i) {
        const Json& fixtureCase = root.items[i];
        const Json* nameJson = fixtureCase.Find("name");
        const std::string name = "hit[" + std::to_string(i) + "] " +
                                 (nameJson ? nameJson->AsString() : std::string("<unnamed>"));
        const bool isSpecGap = name.find("spec_gap") != std::string::npos;
        const std::size_t before = Failures().size();
        SetCurrentCase(name);
        RunHitCase(fixtureCase);
        const bool ok = Failures().size() == before;
        if (!ok) ++failed;
        std::cout << (ok ? "[PASS] " : (isSpecGap ? "[SPEC-GAP-EXPECTED-FAIL] " : "[FAIL] ")) << name << "\n";
    }
    return failed;
}

}  // namespace krtest
