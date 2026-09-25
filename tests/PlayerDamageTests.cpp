#include "../src/RageState.h"
#include "TestSupport.h"

#include <limits>

namespace {
void FillBudget(kr::RageState& state, double time)
{
    for (int i = 0; i < 5; ++i) {
        KRT_EQI(state.TryAwardPlayerDamage(time).awarded, 2);
    }
}
}

KR_TEST(player_damage_same_timestamp_has_five_awards)
{
    kr::RageState state;
    FillBudget(state, 10.0);
    KRT_EQI(state.TryAwardPlayerDamage(10.0).awarded, 0);
    KRT_EQI(state.TryAwardPlayerDamage(10.5).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 10);
}

KR_TEST(player_damage_budget_recovers_exactly_after_one_second)
{
    kr::RageState state;
    FillBudget(state, 10.0);
    KRT_EQI(state.TryAwardPlayerDamage(10.999999).awarded, 0);
    FillBudget(state, 11.0);
    KRT_EQI(state.TryAwardPlayerDamage(11.0).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 20);
}

KR_TEST(player_damage_uses_rolling_not_calendar_seconds)
{
    kr::RageState state;
    for (double time : {0.0, 0.125, 0.25, 0.375, 0.5}) {
        KRT_EQI(state.TryAwardPlayerDamage(time).awarded, 2);
    }
    KRT_EQI(state.TryAwardPlayerDamage(0.75).awarded, 0);
    KRT_EQI(state.TryAwardPlayerDamage(1.0).awarded, 2);
    KRT_EQI(state.TryAwardPlayerDamage(1.124).awarded, 0);
    KRT_EQI(state.TryAwardPlayerDamage(1.125).awarded, 2);
    KRT_EQI(state.TryAwardPlayerDamage(1.25).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 16);
}

KR_TEST(player_damage_ignores_legacy_gain_settings)
{
    kr::RageState state;
    kr::RageSettings legacy;
    legacy.normalHit = 0;
    legacy.powerHit = 999;
    legacy.specialHit = 999;
    legacy.receivedHit = 999;
    legacy.kill = 999;
    legacy.maxGainPerSecond = 1;
    state.Configure(legacy);
    FillBudget(state, 0.0);
    KRT_EQI(state.TryAwardPlayerDamage(0.0).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 10);
}

KR_TEST(player_damage_can_fill_while_ineligible_or_raging)
{
    for (bool raging : {false, true}) {
        kr::RageState state;
        kr::RageSettings settings;
        settings.maximum = 6;
        state.Configure(settings);
        state.SetEligible(raging);
        state.SetRaging(raging);
        KRT_EQI(state.TryAwardPlayerDamage(0.0).awarded, 2);
        KRT_EQI(state.TryAwardPlayerDamage(0.0).awarded, 2);
        const auto full = state.TryAwardPlayerDamage(0.0);
        KRT_EQI(full.awarded, 2);
        KRT_EQB(full.becameFull, true);
        KRT_EQI(state.GetSnapshot().current, 6);
        KRT_EQB(state.GetSnapshot().ready, false);
        KRT_EXPECT_NONE(state.TryBeginActivation());
    }
}

KR_TEST(player_damage_caps_current_and_full_edge)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 5;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAwardPlayerDamage(0.0).awarded, 2);
    const auto second = state.TryAwardPlayerDamage(0.0);
    KRT_EQI(second.awarded, 2);
    KRT_EQB(second.becameFull, false);
    const auto full = state.TryAwardPlayerDamage(0.0);
    KRT_EQI(full.awarded, 1);
    KRT_EQB(full.becameFull, true);
    const auto extra = state.TryAwardPlayerDamage(0.0);
    KRT_EQI(extra.awarded, 0);
    KRT_EQB(extra.becameFull, false);
    KRT_EQI(state.GetSnapshot().current, 5);
    KRT_EQB(state.GetSnapshot().ready, true);
}

KR_TEST(player_damage_configure_preserves_budget)
{
    kr::RageState state;
    FillBudget(state, 0.0);
    kr::RageSettings settings;
    settings.maximum = 200;
    settings.maxGainPerSecond = 9999;
    state.Configure(settings);
    KRT_EQI(state.TryAwardPlayerDamage(0.5).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 10);
}

KR_TEST(player_damage_activation_and_rage_transitions_preserve_budget)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAwardPlayerDamage(0.0).awarded, 2);
    KRT_EQI(state.TryAwardPlayerDamage(0.0).awarded, 2);
    const auto token = state.TryBeginActivation();
    KRT_EXPECT_HAS(token);
    if (!token) return;

    settings.maximum = 20;
    state.Configure(settings);
    KRT_EQI(state.TryAwardPlayerDamage(0.0).awarded, 2);
    KRT_EQB(state.GetSnapshot().pending, true);
    KRT_EQB(state.CommitActivation(*token), true);
    KRT_EQI(state.GetSnapshot().current, 0);
    KRT_EQI(state.TryAwardPlayerDamage(0.125).awarded, 2);
    KRT_EQI(state.TryAwardPlayerDamage(0.25).awarded, 2);
    KRT_EQI(state.TryAwardPlayerDamage(0.375).awarded, 0);
    state.SetRaging(false);
    KRT_EQI(state.TryAwardPlayerDamage(0.5).awarded, 0);
    state.ReconcileAfterLoad(false);
    KRT_EQI(state.TryAwardPlayerDamage(0.75).awarded, 0);
    KRT_EQI(state.TryAwardPlayerDamage(2.0).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 6);
}

KR_TEST(player_damage_restored_pending_does_not_block_accumulation)
{
    kr::RageState state;
    state.Restore({1, 1, true, false});
    KRT_EQI(state.TryAwardPlayerDamage(0.0).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 3);
    KRT_EQB(state.GetSnapshot().pending, true);
    KRT_EQB(state.GetSnapshot().ready, false);
    KRT_EQI(state.Save().current, 3);
}

KR_TEST(player_damage_restore_and_reset_begin_fresh_budget)
{
    kr::RageState state;
    FillBudget(state, 0.0);
    state.Restore(state.Save());
    FillBudget(state, 0.5);
    KRT_EQI(state.TryAwardPlayerDamage(0.75).awarded, 0);
    KRT_EQI(state.Save().current, 20);
    kr::RageState reloaded;
    reloaded.Restore(state.Save());
    KRT_EQI(reloaded.GetSnapshot().current, 20);
    state.Reset();
    KRT_EQI(state.GetSnapshot().current, 0);
    KRT_EQI(state.TryAwardPlayerDamage(0.75).awarded, 2);
}

KR_TEST(player_damage_rejects_invalid_time_without_resetting_budget)
{
    kr::RageState state;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    KRT_EQI(state.TryAwardPlayerDamage(nan).awarded, 0);
    KRT_EQI(state.TryAwardPlayerDamage(inf).awarded, 0);
    KRT_EQI(state.TryAwardPlayerDamage(-inf).awarded, 0);
    FillBudget(state, 10.0);
    KRT_EQI(state.TryAwardPlayerDamage(9.5).awarded, 0);
    KRT_EQI(state.TryAwardPlayerDamage(10.999).awarded, 0);
    KRT_EQI(state.TryAwardPlayerDamage(11.0).awarded, 2);
    KRT_EQI(state.GetSnapshot().current, 12);
}

KR_TEST(player_damage_respects_authoritative_maximum_after_restore)
{
    kr::RageState state;
    state.Restore({1, 150, false, false});
    KRT_EQI(state.GetSnapshot().current, 100);
    KRT_EQI(state.TryAwardPlayerDamage(0.0).awarded, 0);
    KRT_EQI(state.Save().current, 150);
    kr::RageSettings settings;
    settings.maximum = 200;
    state.Configure(settings);
    KRT_EQI(state.TryAwardPlayerDamage(0.0).awarded, 2);
    KRT_EQI(state.Save().current, 152);
}
