// ---------------------------------------------------------------------------
// RageState 战斗命中定额规则离线单元测试（0.3.0 契约）。
//
// 被测：kr::RageState::TryAwardCombatHit(bool inCombat, bool suspended)
//  * !inCombat：调用 EndCombat 清零（含取消 pending），返回 {0,false}。
//  * suspended：不加不清，返回 {0,false}。
//  * 否则：每次固定 +2，只受 maximum 封顶；不看 normal/power/special/received/kill/
//    maxGainPerSecond；不做 0.1s 去重；不因 eligible/raging/pending 拒绝。
//  * 增量 > 0 时清除读档延期上限源 loadClampSource_；满怒边沿（becameFull）仅首次。
//
// 仅依赖 src/RageState.h/.cpp，不涉及 Skyrim / SKSE / 网络。
// ---------------------------------------------------------------------------

#include "../src/RageState.h"

#include "TestSupport.h"

namespace {

kr::RageSettings Settings(int maximum, int legacyNormal = 7, int legacyBudget = 1)
{
    kr::RageSettings settings;
    settings.maximum = maximum;
    settings.normalHit = legacyNormal;
    settings.powerHit = legacyNormal + 2;
    settings.specialHit = legacyNormal + 4;
    settings.receivedHit = legacyNormal + 6;
    settings.kill = legacyNormal + 8;
    settings.maxGainPerSecond = legacyBudget;
    return settings;
}

}  // namespace

// 每次命中固定 +2，与配置的 hit 值/每秒预算无关，也不做任何时间去重。
KR_TEST(combat_hit_awards_fixed_two_per_hit)
{
    kr::RageState state;
    state.Configure(Settings(100));
    state.SetEligible(true);

    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 2);
    // 连续调用（无时间参数、无同源/同目标参数）仍逐次授予，不得合并。
    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 6);
    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 8);
}

// maxGainPerSecond=1 / normalHit 异常都不影响：10 次命中应为 20。
KR_TEST(combat_hit_ignores_legacy_settings_and_budget)
{
    kr::RageState state;
    state.Configure(Settings(100, /*normal*/ 9, /*budget*/ 1));
    state.SetEligible(true);

    for (int i = 0; i < 10; ++i) {
        KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    }
    KRT_EQI(state.GetSnapshot().current, 20);
    KRT_EQB(state.GetSnapshot().ready, false);
}

// 奇数上限只补差额，满怒边沿仅首次。
KR_TEST(combat_hit_caps_at_maximum_and_full_edge_once)
{
    kr::RageState state;
    state.Configure(Settings(5));
    state.SetEligible(true);

    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);

    const kr::AwardResult full = state.TryAwardCombatHit(true, false);
    KRT_EQI(full.awarded, 1);         // 只补到上限
    KRT_EQB(full.becameFull, true);   // 满怒边沿首次
    KRT_EQI(state.GetSnapshot().current, 5);

    const kr::AwardResult after = state.TryAwardCombatHit(true, false);
    KRT_EQI(after.awarded, 0);
    KRT_EQB(after.becameFull, false); // 已满，不再报边沿
    KRT_EQI(state.GetSnapshot().current, 5);
    KRT_EQB(state.GetSnapshot().ready, true);
}

// eligible 为假 / raging / restored pending 都不阻止积攒；不改动这些状态位。
KR_TEST(combat_hit_not_blocked_by_ineligible_raging_or_pending)
{
    {
        kr::RageState state;
        state.Configure(Settings(100));
        state.SetEligible(false);
        KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
        KRT_EQI(state.GetSnapshot().current, 2);
        KRT_EQB(state.GetSnapshot().ready, false);
    }
    {
        kr::RageState state;
        state.Configure(Settings(100));
        state.SetEligible(true);
        state.SetRaging(true);
        KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
        KRT_EQI(state.GetSnapshot().current, 2);
        KRT_EQB(state.GetSnapshot().raging, true);
    }
    {
        kr::RageState state;  // 默认上限 100，留下积攒空间
        kr::SavedState saved;
        saved.current = 1;
        saved.pending = true;  // 读档带来的未结算挂起
        state.Restore(saved);
        KRT_EQB(state.GetSnapshot().pending, true);

        KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
        KRT_EQI(state.GetSnapshot().current, 3);
        KRT_EQB(state.GetSnapshot().pending, true);  // 挂起状态不因命中改变
    }
}

// 未 Configure 时用默认上限，命中照常积攒；eligible/ready 仍为假。
KR_TEST(combat_hit_works_without_configure)
{
    kr::RageState state;
    state.SetEligible(false);

    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 2);
    KRT_EQI(state.GetSnapshot().maximum, 100);
    KRT_EQB(state.GetSnapshot().ready, false);
}

// !inCombat：清零（含取消挂起的激活令牌）。
KR_TEST(combat_hit_out_of_combat_clears_and_cancels_pending)
{
    kr::RageState state;
    state.Configure(Settings(100));
    state.SetEligible(true);
    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 4);

    const kr::AwardResult cleared = state.TryAwardCombatHit(false, false);
    KRT_EQI(cleared.awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 0);

    // 满额 → 挂起激活 → 脱战清零必须同时取消 pending。
    kr::RageState pending;
    pending.Configure(Settings(4));
    pending.SetEligible(true);
    KRT_EQI(pending.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(pending.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EXPECT_HAS(pending.TryBeginActivation());
    KRT_EQB(pending.GetSnapshot().pending, true);

    KRT_EQI(pending.TryAwardCombatHit(false, false).awarded, 0);
    KRT_EQB(pending.GetSnapshot().pending, false);
    KRT_EQI(pending.GetSnapshot().current, 0);
}

// suspended：不加不清；挂起激活令牌也保持。
KR_TEST(combat_hit_suspended_preserves_ledger_and_pending)
{
    kr::RageState state;
    state.Configure(Settings(100));
    state.SetEligible(true);
    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 4);

    const kr::AwardResult suspended = state.TryAwardCombatHit(true, true);
    KRT_EQI(suspended.awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 4);  // 不清

    kr::RageState pending;
    pending.Configure(Settings(4));
    pending.SetEligible(true);
    KRT_EQI(pending.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EQI(pending.TryAwardCombatHit(true, false).awarded, 2);
    KRT_EXPECT_HAS(pending.TryBeginActivation());
    KRT_EQI(pending.TryAwardCombatHit(true, true).awarded, 0);
    KRT_EQB(pending.GetSnapshot().pending, true);
    KRT_EQI(pending.GetSnapshot().current, 4);
}

// Reset 清空运行态（保留配置）。
KR_TEST(combat_hit_reset_clears_run_state)
{
    kr::RageState state;
    state.Configure(Settings(10));
    state.SetEligible(true);
    KRT_EQI(state.TryAwardCombatHit(true, false).awarded, 2);

    state.Reset();
    KRT_EQI(state.GetSnapshot().current, 0);
    KRT_EQI(state.GetSnapshot().maximum, 10);
    KRT_EQB(state.GetSnapshot().eligible, false);
    KRT_EQI(state.Save().current, 0);
}

// ---------------------------------------------------------------------------
// loadClampSource 延期上限路径
// 读档值 > 当前（临时）上限时 current 被钳到上限，原值挂在 loadClampSource_：
//   * 满额时命中无增量 → 必须保留延期源（Save 仍写回原值，不得固化截断）。
//   * 权威 Configure 到达后按恢复出的上限重钳，延期源被消费；其后的真实增量继续写回、
//     不会复活旧值。
// 说明：延期源只会在 current==maximum（满额）时产生，因此"增量清除延期源"的可达路径是
//       Restore -> 权威 Configure -> 命中；下面两段共同覆盖保留与消费/推进。
// ---------------------------------------------------------------------------

// 满额无增量：命中 awarded==0，延期源保留，Save 写回原始高值。
KR_TEST(combat_hit_full_without_increment_keeps_deferred_clamp_source)
{
    kr::RageState state;  // 尚未 Configure，默认上限 100
    kr::SavedState saved;
    saved.current = 250;  // 存档来自上限 300 的配置
    state.Restore(saved);

    KRT_EQI(state.GetSnapshot().current, 100);    // 临时按默认上限钳制
    KRT_EQI(state.GetSnapshot().maximum, 100);
    KRT_EQI(state.Save().current, 250);           // 延期源仍被持久化

    const kr::AwardResult full = state.TryAwardCombatHit(true, false);
    KRT_EQI(full.awarded, 0);                      // 已满，无增量
    KRT_EQB(full.becameFull, false);               // 边沿已在满额时耗过，不再上报
    KRT_EQI(state.GetSnapshot().current, 100);
    KRT_EQI(state.Save().current, 250);            // 无增量 → 延期源必须保留
}

// 权威 Configure 消费延期源后按真实上限恢复；其后的命中增量继续推进并持久化。
KR_TEST(combat_hit_after_authoritative_configure_advances_and_clears_source)
{
    kr::RageState state;  // 默认上限 100
    kr::SavedState saved;
    saved.current = 250;
    state.Restore(saved);
    KRT_EQI(state.GetSnapshot().current, 100);
    KRT_EQI(state.Save().current, 250);

    kr::RageSettings authoritative;
    authoritative.maximum = 300;
    state.Configure(authoritative);                // 按恢复出的真实上限重钳
    KRT_EQI(state.GetSnapshot().maximum, 300);
    KRT_EQI(state.GetSnapshot().current, 250);     // 怒气完整恢复，延期源被消费
    KRT_EQI(state.Save().current, 250);            // 不再是截断值

    const kr::AwardResult hit = state.TryAwardCombatHit(true, false);
    KRT_EQI(hit.awarded, 2);                       // 有空间 → 真实增量
    KRT_EQI(state.GetSnapshot().current, 252);
    KRT_EQI(state.Save().current, 252);            // 增量已写入真实账本
}
