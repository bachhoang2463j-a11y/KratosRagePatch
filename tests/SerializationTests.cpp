// ---------------------------------------------------------------------------
// 存档 / 读档 / 读档结算（ReconcileAfterLoad）离线用例。
// 关键规则：读档不自动产生"满怒"信号；只有 pending 且实际变身成功才清零。
// ---------------------------------------------------------------------------

#include "../src/RageState.h"

#include "TestSupport.h"

#include <cstdint>

namespace {

kr::GainEvent Ev(kr::GainKind kind, std::uint32_t actor, std::uint32_t source)
{
    kr::GainEvent event;
    event.kind = kind;
    event.actorID = actor;
    event.sourceID = source;
    return event;
}

}  // namespace

// 存档字段与恢复后的怒气值。
KR_TEST(save_restore_roundtrip_preserves_progress)
{
    kr::RageSettings settings;
    kr::RageState source;
    source.Configure(settings);
    source.SetEligible(true);
    KRT_EQI(source.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 2);
    KRT_EQI(source.TryAward(Ev(kr::GainKind::Normal, 2, 72), 1.0).awarded, 2);
    KRT_EQI(source.TryAward(Ev(kr::GainKind::Power, 3, 73), 2.0).awarded, 4);

    const kr::SavedState saved = source.Save();
    KRT_EQI(saved.version, 1);
    KRT_EQI(saved.current, 8);
    KRT_EQB(saved.pending, false);
    KRT_EQB(saved.raging, false);

    kr::RageState restored;
    restored.Configure(settings);
    restored.Restore(saved);

    const kr::Snapshot snap = restored.GetSnapshot();
    KRT_EQI(snap.current, 8);
    KRT_EQI(snap.maximum, 100);
    KRT_EQB(snap.pending, false);
    KRT_EQB(snap.raging, false);
    KRT_EQB(snap.eligible, false);  // 读档后必须由装备刷新（SetEligible）重新置位
    KRT_EQB(snap.ready, false);
}

// 读档本身不产生满怒通知，也不重复上报；就绪需要显式的装备刷新。
KR_TEST(restore_does_not_auto_notify_full_and_needs_eligibility_refresh)
{
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;

    kr::RageState source;
    source.Configure(settings);
    source.SetEligible(true);
    KRT_EQI(source.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);
    const kr::SavedState saved = source.Save();
    KRT_EQI(saved.current, 4);

    kr::RageState restored;
    restored.Configure(settings);
    restored.Restore(saved);
    KRT_EQI(restored.GetSnapshot().current, 4);
    KRT_EQB(restored.GetSnapshot().ready, false);

    restored.SetEligible(true);
    KRT_EQB(restored.GetSnapshot().ready, true);

    const kr::AwardResult further = restored.TryAward(Ev(kr::GainKind::Normal, 2, 72), 1.0);
    KRT_EQI(further.awarded, 0);
    KRT_EQB(further.becameFull, false);
    KRT_EQI(restored.GetSnapshot().current, 4);
}

// 版本不符：整体重置。
KR_TEST(restore_rejects_unknown_version)
{
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;

    kr::RageState state;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);

    kr::SavedState bad;
    bad.version = 2;
    bad.current = 4;
    bad.pending = true;
    bad.raging = true;
    state.Restore(bad);

    const kr::Snapshot snap = state.GetSnapshot();
    KRT_EQI(snap.current, 0);
    KRT_EQB(snap.pending, false);
    KRT_EQB(snap.raging, false);
    KRT_EQB(snap.eligible, false);
    KRT_EQB(snap.ready, false);
}

// 越界怒气值钳制到 [0, maximum]。
KR_TEST(restore_clamps_current_into_range)
{
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;

    kr::RageState state;
    state.Configure(settings);

    kr::SavedState high;
    high.current = 999;
    state.Restore(high);
    KRT_EQI(state.GetSnapshot().current, 4);

    kr::SavedState low;
    low.current = -5;
    state.Restore(low);
    KRT_EQI(state.GetSnapshot().current, 0);
}

// 存档中的 pending 标志：恢复后阻塞积攒与再次启动。
KR_TEST(saved_pending_flag_blocks_gain_after_restore)
{
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;

    kr::RageState source;
    source.Configure(settings);
    source.SetEligible(true);
    KRT_EQI(source.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);
    KRT_EXPECT_HAS(source.TryBeginActivation());

    const kr::SavedState saved = source.Save();
    KRT_EQB(saved.pending, true);
    KRT_EQI(saved.current, 4);
    KRT_EQB(saved.raging, false);

    kr::RageState restored;
    restored.Configure(settings);
    restored.Restore(saved);
    restored.SetEligible(true);

    KRT_EQB(restored.GetSnapshot().pending, true);
    KRT_EQB(restored.GetSnapshot().ready, false);
    KRT_EQI(restored.TryAward(Ev(kr::GainKind::Normal, 2, 72), 0.5).awarded, 0);
    KRT_EQI(restored.GetSnapshot().current, 4);
    KRT_EXPECT_NONE(restored.TryBeginActivation());
}

// 读档结算：只有 pending 且实际变身成功才清零；未变身则保留满怒可重试。
KR_TEST(reconcile_zeroes_only_when_transformation_actually_happened)
{
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;

    {
        kr::RageState state;
        state.Configure(settings);
        kr::SavedState saved;
        saved.current = 4;
        saved.pending = true;
        state.Restore(saved);
        state.ReconcileAfterLoad(true);

        const kr::Snapshot snap = state.GetSnapshot();
        KRT_EQI(snap.current, 0);
        KRT_EQB(snap.raging, true);
        KRT_EQB(snap.pending, false);
    }

    {
        kr::RageState state;
        state.Configure(settings);
        kr::SavedState saved;
        saved.current = 4;
        saved.pending = true;
        state.Restore(saved);
        state.ReconcileAfterLoad(false);

        const kr::Snapshot snap = state.GetSnapshot();
        KRT_EQI(snap.current, 4);
        KRT_EQB(snap.raging, false);
        KRT_EQB(snap.pending, false);

        state.SetEligible(true);
        KRT_EQB(state.GetSnapshot().ready, true);
        KRT_EXPECT_HAS(state.TryBeginActivation());
    }
}

// 没有 pending 时，即使实际在变身也不清零。
KR_TEST(reconcile_without_pending_keeps_current)
{
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;

    kr::RageState state;
    state.Configure(settings);
    kr::SavedState saved;
    saved.current = 4;
    saved.pending = false;
    state.Restore(saved);
    state.ReconcileAfterLoad(true);

    KRT_EQI(state.GetSnapshot().current, 4);
    KRT_EQB(state.GetSnapshot().raging, true);
    KRT_EQB(state.GetSnapshot().pending, false);
}

// 存档的 raging 标志恢复后禁止积攒，变身结束后恢复。
KR_TEST(saved_raging_flag_blocks_gain_until_rage_ends)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});

    kr::SavedState saved;
    saved.current = 6;
    saved.raging = true;
    state.Restore(saved);
    state.SetEligible(true);

    KRT_EQI(state.GetSnapshot().current, 6);
    KRT_EQB(state.GetSnapshot().raging, true);
    KRT_EQB(state.GetSnapshot().ready, false);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 0);

    state.SetRaging(false);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 2, 72), 1.0).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 8);
}

// 真实加载顺序覆盖：新进程里 SKSE 反序列化（Restore）先于 Papyrus 的 ReconcileAfterLoad 与
// ConfigureRage。默认上限 100 下读入 250 不得被永久截断；普通 Reconcile 不得清除延迟值，
// Configure 到达前 Save 也不得把临时的 100 写回替代 250。存档格式保持 v1。
KR_TEST(restore_reconcile_configure_real_order_preserves_value_above_default_maximum)
{
    kr::RageState state;  // 尚未 Configure，settings_ 仍是默认上限 100
    kr::SavedState saved;
    saved.current = 250;  // 存档来自上限 300 的配置
    state.Restore(saved);
    KRT_EQI(state.GetSnapshot().current, 100);  // 权威上限未到，先按当前上限钳制
    KRT_EQI(state.GetSnapshot().maximum, 100);

    // 真实顺序的第二步：ReconcileAfterLoad(false) 是普通结算，不得清除延迟值。
    state.ReconcileAfterLoad(false);
    KRT_EQI(state.GetSnapshot().current, 100);
    KRT_EQB(state.GetSnapshot().pending, false);
    KRT_EQB(state.GetSnapshot().raging, false);

    // Configure 到达前存档：必须写回原始 250，不能把临时的 100 固化。
    const kr::SavedState beforeConfigure = state.Save();
    KRT_EQI(beforeConfigure.current, 250);
    KRT_EQB(beforeConfigure.pending, false);
    KRT_EQB(beforeConfigure.raging, false);

    // 真实顺序的第三步：权威 Configure(300) 到达，怒气值完整恢复。
    kr::RageSettings settings;
    settings.maximum = 300;
    state.Configure(settings);
    KRT_EQI(state.GetSnapshot().maximum, 300);
    KRT_EQI(state.GetSnapshot().current, 250);
    KRT_EQB(state.GetSnapshot().ready, false);  // eligible 仍需装备刷新
    KRT_EQI(state.Save().current, 250);         // 之后存档写的是真实值
}

// pending 读档 + 实际已变身：怒气被真实消耗（不可保留的状态推进），延迟值当场作废；
// 其后即使到达高上限的 Configure 也不得复活已消耗的怒气。
KR_TEST(reconcile_charge_after_restore_is_irreversible)
{
    kr::RageState state;  // 默认上限 100
    kr::SavedState saved;
    saved.current = 250;
    saved.pending = true;
    state.Restore(saved);
    KRT_EQI(state.GetSnapshot().current, 100);
    KRT_EQB(state.GetSnapshot().pending, true);

    state.ReconcileAfterLoad(true);  // 实测原 813 效果已生效
    KRT_EQI(state.GetSnapshot().current, 0);
    KRT_EQB(state.GetSnapshot().pending, false);
    KRT_EQI(state.Save().current, 0);

    kr::RageSettings settings;
    settings.maximum = 300;
    state.Configure(settings);
    KRT_EQI(state.GetSnapshot().current, 0);  // 已消耗的怒气不因延迟值而复活
    KRT_EQI(state.Save().current, 0);
}

// Reset 清空全部运行态（保留配置）。
KR_TEST(reset_clears_everything_but_keeps_settings)
{
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;

    kr::RageState state;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);

    state.Reset();
    const kr::Snapshot snap = state.GetSnapshot();
    KRT_EQI(snap.current, 0);
    KRT_EQI(snap.maximum, 4);
    KRT_EQB(snap.eligible, false);
    KRT_EQB(snap.raging, false);
    KRT_EQB(snap.pending, false);
    KRT_EQB(snap.ready, false);
    KRT_EQI(state.Save().current, 0);
}
