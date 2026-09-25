// ---------------------------------------------------------------------------
// CombatTimerTests: UpdateCombat / EndCombat / SuspendCombatTimer 的真实规则。
// 只通过公开接口驱动，不用 TryAward / TryAwardPlayerDamage 伪造进度。
// 时间点全部使用 0.5 / 1.0 等可精确表示的值，避免浮点边界误差。
// ---------------------------------------------------------------------------

#include "RageState.h"

#include "TestSupport.h"

#include <optional>

namespace {

using kr::AwardResult;
using kr::RageSettings;
using kr::RageState;
using kr::SavedState;
using kr::Snapshot;

// 累计到满怒（默认上限 100，战斗每整秒 +2，首个 tick 只是基线）。
void TickToFull(RageState& state)
{
    state.UpdateCombat(0.0, true, false);
    for (int t = 1; t <= 50; ++t) {
        state.UpdateCombat(static_cast<double>(t), true, false);
    }
}

KR_TEST(Combat_FirstTickIsBaselineOnly)
{
    RageState state;
    const AwardResult r = state.UpdateCombat(0.0, true, false);
    KRT_EQI(r.awarded, 0);
    KRT_EQB(r.becameFull, false);
    KRT_EQI(state.GetSnapshot().current, 0);
}

KR_TEST(Combat_HalfSecondNoGainThenFullSecondAwards2)
{
    RageState state;
    state.UpdateCombat(0.0, true, false);
    const AwardResult half = state.UpdateCombat(0.5, true, false);
    KRT_EQI(half.awarded, 0);
    const AwardResult full = state.UpdateCombat(1.0, true, false);
    KRT_EQI(full.awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 2);
}

KR_TEST(Combat_Accumulates2PerSecondUpTo100Over50Seconds)
{
    RageState state;
    state.SetEligible(true);
    state.UpdateCombat(0.0, true, false);
    int fullCount = 0;
    for (int t = 1; t <= 50; ++t) {
        const AwardResult r = state.UpdateCombat(static_cast<double>(t), true, false);
        KRT_EQI(r.awarded, 2);
        if (r.becameFull) ++fullCount;
    }
    KRT_EQI(fullCount, 1);
    KRT_EQI(state.GetSnapshot().current, 100);
    KRT_EQB(state.GetSnapshot().ready, true);
}

KR_TEST(Combat_OutOfCombatClearsCurrentToZero)
{
    RageState state;
    state.SetEligible(true);
    TickToFull(state);
    KRT_EQI(state.GetSnapshot().current, 100);
    const AwardResult r = state.UpdateCombat(60.0, false, false);
    KRT_EQI(r.awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 0);
    KRT_EQB(state.GetSnapshot().ready, false);
}

KR_TEST(Combat_GainsWithoutEligibleButReadyStaysFalse)
{
    RageState state;  // 从不 SetEligible。
    state.UpdateCombat(0.0, true, false);
    for (int t = 1; t <= 50; ++t) {
        state.UpdateCombat(static_cast<double>(t), true, false);
    }
    const Snapshot snap = state.GetSnapshot();
    KRT_EQI(snap.current, 100);
    KRT_EQB(snap.eligible, false);
    KRT_EQB(snap.ready, false);
}

KR_TEST(Combat_GainsWhileRagingButReadyStaysFalse)
{
    RageState state;
    state.SetEligible(true);
    state.SetRaging(true);  // 变身中：ClearTransient 会重置计时，重新起表。
    state.UpdateCombat(0.0, true, false);
    for (int t = 1; t <= 50; ++t) {
        state.UpdateCombat(static_cast<double>(t), true, false);
    }
    const Snapshot snap = state.GetSnapshot();
    KRT_EQI(snap.current, 100);
    KRT_EQB(snap.raging, true);
    KRT_EQB(snap.ready, false);
}

KR_TEST(Combat_SameSecondRepeatTickDoesNotDoubleCount)
{
    RageState state;
    state.UpdateCombat(0.0, true, false);
    KRT_EQI(state.UpdateCombat(1.0, true, false).awarded, 2);
    KRT_EQI(state.UpdateCombat(1.0, true, false).awarded, 0);
    KRT_EQI(state.UpdateCombat(1.0, true, false).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 2);
}

KR_TEST(Combat_StallGapAndClockRewindDoNotBackfill)
{
    RageState state;
    state.UpdateCombat(0.0, true, false);
    state.UpdateCombat(0.5, true, false);  // 小数部分 0.5。
    // 断档 1.8s（> 1.5s）：不补算，且小数部分作废。
    KRT_EQI(state.UpdateCombat(2.3, true, false).awarded, 0);
    KRT_EQI(state.UpdateCombat(2.8, true, false).awarded, 0);  // 重新积累 0.5。
    KRT_EQI(state.UpdateCombat(3.3, true, false).awarded, 2);  // 0.5 + 0.5 = 1 整秒。
    KRT_EQI(state.GetSnapshot().current, 2);
    // 回拨：elapsed < 0，不补算，小数部分作废。
    KRT_EQI(state.UpdateCombat(3.0, true, false).awarded, 0);
    KRT_EQI(state.UpdateCombat(3.5, true, false).awarded, 0);
    KRT_EQI(state.UpdateCombat(4.0, true, false).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 4);
}

KR_TEST(Combat_SuspendKeepsFractionAndSkipsPausedTime)
{
    RageState state;
    state.UpdateCombat(0.0, true, false);
    state.UpdateCombat(0.5, true, false);   // 保留小数 0.5。
    state.UpdateCombat(0.7, true, true);    // 暂停：不计 0.7-0.5。
    KRT_EQI(state.GetSnapshot().current, 0);
    state.UpdateCombat(1.0, true, false);   // 恢复后首个 tick 只是校准。
    KRT_EQI(state.GetSnapshot().current, 0);
    KRT_EQI(state.UpdateCombat(1.5, true, false).awarded, 2);  // 0.5 + 0.5。
    KRT_EQI(state.GetSnapshot().current, 2);
}

KR_TEST(Combat_SuspendedTrueWithNotInCombatClearsFirst)
{
    RageState state;
    state.UpdateCombat(0.0, true, false);
    state.UpdateCombat(0.5, true, false);
    state.UpdateCombat(1.0, true, false);
    KRT_EQI(state.GetSnapshot().current, 2);
    // inCombat 分支先于 suspended 分支：必须按脱离战斗清零。
    state.UpdateCombat(2.0, false, true);
    KRT_EQI(state.GetSnapshot().current, 0);
}

KR_TEST(Combat_BecameFullFiresOnceThenNoExtraGain)
{
    RageState state;
    state.SetEligible(true);
    state.UpdateCombat(0.0, true, false);
    bool sawFull = false;
    for (int t = 1; t <= 49; ++t) {
        KRT_EQB(state.UpdateCombat(static_cast<double>(t), true, false).becameFull, false);
    }
    sawFull = state.UpdateCombat(50.0, true, false).becameFull;
    KRT_EQB(sawFull, true);
    // 已满后再 tick：不再增加，也不再报告 becameFull。
    for (int t = 51; t <= 55; ++t) {
        const AwardResult r = state.UpdateCombat(static_cast<double>(t), true, false);
        KRT_EQI(r.awarded, 0);
        KRT_EQB(r.becameFull, false);
    }
    KRT_EQI(state.GetSnapshot().current, 100);
}

KR_TEST(Combat_OddMaximumClampsFinalAward)
{
    RageState state;
    RageSettings settings;
    settings.maximum = 99;  // 奇数上限：最后一步只补到上限。
    state.Configure(settings);
    state.SetEligible(true);
    state.UpdateCombat(0.0, true, false);
    for (int t = 1; t <= 49; ++t) {
        KRT_EQI(state.UpdateCombat(static_cast<double>(t), true, false).awarded, 2);
    }
    KRT_EQI(state.GetSnapshot().current, 98);
    const AwardResult last = state.UpdateCombat(50.0, true, false);
    KRT_EQI(last.awarded, 1);
    KRT_EQB(last.becameFull, true);
    KRT_EQI(state.GetSnapshot().current, 99);
}

KR_TEST(Combat_ConfigureDoesNotBypassOrResetTimer)
{
    RageState state;
    state.UpdateCombat(0.0, true, false);
    state.UpdateCombat(0.5, true, false);  // 小数 0.5。
    RageSettings settings;
    settings.maximum = 200;
    state.Configure(settings);  // 不得清掉计时进度或当前值基线。
    KRT_EQI(state.UpdateCombat(1.0, true, false).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 2);
}

KR_TEST(Combat_EndCombatCancelsPendingButKeepsRagingFlag)
{
    RageState state;
    state.SetEligible(true);
    TickToFull(state);
    const std::optional<std::uint64_t> token = state.TryBeginActivation();
    KRT_EXPECT_HAS(token);
    KRT_EQB(state.CommitActivation(*token), true);
    // CommitActivation 后 raging=true 且怒气已消耗；EndCombat 不得熄灭 raging。
    state.UpdateCombat(60.0, false, false);
    const Snapshot snap = state.GetSnapshot();
    KRT_EQB(snap.raging, true);
    KRT_EQI(snap.current, 0);
    KRT_EQB(snap.pending, false);
    // pending 途径：Abort 一个挂起请求后 EndCombat 同样清掉 pending。
    RageState other;
    other.SetEligible(true);
    TickToFull(other);
    const std::optional<std::uint64_t> tok2 = other.TryBeginActivation();
    KRT_EXPECT_HAS(tok2);
    other.EndCombat();
    KRT_EQB(other.GetSnapshot().pending, false);
}

KR_TEST(Combat_EndCombatClearsLoadClampSourceNoRevival)
{
    RageState state;
    RageSettings settings;
    settings.maximum = 50;
    state.Configure(settings);
    // 存档怒气 80 超过当前上限：Restore 记为延迟值，Save 必须写回原值。
    state.Restore(SavedState{1, 80, false, false});
    KRT_EQI(state.Save().current, 80);
    state.UpdateCombat(0.0, false, false);  // 脱离战斗。
    KRT_EQI(state.GetSnapshot().current, 0);
    KRT_EQI(state.Save().current, 0);  // 延迟值已被 EndCombat 清除，不得复活。
}

KR_TEST(Combat_LifecycleClearsTimerWithoutBackfillAndSaveKeepsCurrent)
{
    // Restore：清计时，读档值保留；下一个 tick 只是校准。
    RageState a;
    a.UpdateCombat(0.0, true, false);
    a.UpdateCombat(0.5, true, false);
    a.Restore(SavedState{1, 42, false, false});
    KRT_EQI(a.GetSnapshot().current, 42);
    KRT_EQI(a.UpdateCombat(100.0, true, false).awarded, 0);
    KRT_EQI(a.UpdateCombat(101.0, true, false).awarded, 2);
    KRT_EQI(a.GetSnapshot().current, 44);
    // 战斗中 Save 保留 current。
    KRT_EQI(a.Save().current, 44);

    // Reconcile：清计时；restoredPending + 实际变身则按真实消耗清零。
    RageState b;
    b.UpdateCombat(0.0, true, false);
    b.Restore(SavedState{1, 30, true, false});
    b.ReconcileAfterLoad(true);
    KRT_EQI(b.GetSnapshot().current, 0);
    KRT_EQI(b.UpdateCombat(100.0, true, false).awarded, 0);  // 校准，不追补。
    KRT_EQI(b.UpdateCombat(101.0, true, false).awarded, 2);

    // Reset：全部归零并重置计时。
    RageState c;
    c.SetEligible(true);
    TickToFull(c);
    c.Reset();
    KRT_EQI(c.GetSnapshot().current, 0);
    KRT_EQI(c.UpdateCombat(100.0, true, false).awarded, 0);

    // CommitActivation：怒气消耗、计时清零，恢复后不追补。
    RageState d;
    d.SetEligible(true);
    TickToFull(d);
    const std::optional<std::uint64_t> token = d.TryBeginActivation();
    KRT_EXPECT_HAS(token);
    KRT_EQB(d.CommitActivation(*token), true);
    KRT_EQI(d.GetSnapshot().current, 0);
    KRT_EQI(d.UpdateCombat(100.0, true, false).awarded, 0);  // 校准。
    KRT_EQI(d.UpdateCombat(101.0, true, false).awarded, 2);
}

}  // namespace
