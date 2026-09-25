// ---------------------------------------------------------------------------
// 满怒启动（token / 提交 / 中止）与 fixture 驱动的启动流程用例。
// 仅依赖 src/RageState.h/.cpp，不涉及 Skyrim / SKSE / 网络。
// ---------------------------------------------------------------------------

#include "../src/RageState.h"

#include "TestSupport.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
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

double NumberField(const krtest::Json& object, const char* key, double fallback)
{
    const krtest::Json* value = object.Find(key);
    return value ? value->AsNumber(fallback) : fallback;
}

kr::GainEvent EventFromJson(const krtest::Json& op)
{
    kr::GainEvent event;
    bool ok = true;
    if (const krtest::Json* kindJson = op.Find("kind")) {
        event.kind = KindFromName(kindJson->AsString(), ok);
    }
    if (const krtest::Json* v = op.Find("actor")) event.actorID = v->AsU32(0);
    if (const krtest::Json* v = op.Find("source")) event.sourceID = v->AsU32(0);
    return event;
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

struct ActivationContext {
    kr::RageState state;
    kr::RageSettings settings;
    std::vector<std::pair<std::string, std::uint64_t>> tokens;
    std::vector<std::pair<std::string, kr::SavedState>> saves;

    bool FindToken(const std::string& key, std::uint64_t& out) const
    {
        for (const auto& entry : tokens) {
            if (entry.first == key) {
                out = entry.second;
                return true;
            }
        }
        return false;
    }

    bool FindSave(const std::string& key, kr::SavedState& out) const
    {
        for (const auto& entry : saves) {
            if (entry.first == key) {
                out = entry.second;
                return true;
            }
        }
        return false;
    }
};

bool ResolveToken(const krtest::Json& op, const ActivationContext& ctx, std::uint64_t& out)
{
    if (const krtest::Json* ref = op.Find("tokenRef")) return ctx.FindToken(ref->AsString(), out);
    if (const krtest::Json* literal = op.Find("token")) {
        out = static_cast<std::uint64_t>(literal->AsInt(0));
        return true;
    }
    return false;
}

void ApplyOp(ActivationContext& ctx, const krtest::Json& op)
{
    const krtest::Json* opName = op.Find("op");
    const std::string kind = opName ? opName->AsString() : std::string();
    const krtest::Json* expect = op.Find("expect");
    const krtest::Json* value = op.Find("value");

    if (kind == "configure") {
        if (const krtest::Json* cfg = op.Find("settings")) ApplySettingsJson(*cfg, ctx.settings);
        ctx.state.Configure(ctx.settings);
    } else if (kind == "eligible") {
        ctx.state.SetEligible(value && value->AsBool());
    } else if (kind == "raging") {
        ctx.state.SetRaging(value && value->AsBool());
    } else if (kind == "gain") {
        const kr::AwardResult result = ctx.state.TryAward(EventFromJson(op), NumberField(op, "t", 0.0));
        if (expect) {
            if (const krtest::Json* v = expect->Find("awarded")) krtest::ExpectEqI(result.awarded, v->AsInt(), "gain.awarded", __FILE__, __LINE__);
            if (const krtest::Json* v = expect->Find("becameFull")) krtest::ExpectEqB(result.becameFull, v->AsBool(), "gain.becameFull", __FILE__, __LINE__);
        }
    } else if (kind == "begin") {
        const std::optional<std::uint64_t> token = ctx.state.TryBeginActivation();
        if (const krtest::Json* keep = op.Find("keep")) {
            if (token.has_value()) ctx.tokens.push_back(std::make_pair(keep->AsString(), *token));
        }
        if (expect) {
            if (const krtest::Json* v = expect->Find("hasToken")) {
                krtest::ExpectEqB(token.has_value(), v->AsBool(), "begin.hasToken", __FILE__, __LINE__);
            }
        }
    } else if (kind == "commit" || kind == "abort") {
        std::uint64_t token = 0;
        if (!ResolveToken(op, ctx, token)) {
            krtest::RecordFailure(__FILE__, __LINE__, "unresolved token for op: " + kind);
            return;
        }
        const bool ok = (kind == "commit") ? ctx.state.CommitActivation(token) : ctx.state.AbortActivation(token);
        if (expect) {
            if (const krtest::Json* v = expect->Find("ok")) krtest::ExpectEqB(ok, v->AsBool(), kind + ".ok", __FILE__, __LINE__);
        }
    } else if (kind == "save") {
        const kr::SavedState saved = ctx.state.Save();
        if (const krtest::Json* keep = op.Find("keep")) ctx.saves.push_back(std::make_pair(keep->AsString(), saved));
        if (expect) {
            if (const krtest::Json* v = expect->Find("version")) krtest::ExpectEqI(saved.version, v->AsInt(), "saved.version", __FILE__, __LINE__);
            if (const krtest::Json* v = expect->Find("current")) krtest::ExpectEqI(saved.current, v->AsInt(), "saved.current", __FILE__, __LINE__);
            if (const krtest::Json* v = expect->Find("pending")) krtest::ExpectEqB(saved.pending, v->AsBool(), "saved.pending", __FILE__, __LINE__);
            if (const krtest::Json* v = expect->Find("raging")) krtest::ExpectEqB(saved.raging, v->AsBool(), "saved.raging", __FILE__, __LINE__);
        }
    } else if (kind == "restore") {
        kr::SavedState saved;
        if (const krtest::Json* use = op.Find("use")) {
            if (!ctx.FindSave(use->AsString(), saved)) {
                krtest::RecordFailure(__FILE__, __LINE__, "unknown save key: " + use->AsString());
                return;
            }
        } else if (const krtest::Json* fields = op.Find("value")) {
            if (const krtest::Json* v = fields->Find("version")) saved.version = v->AsU32(saved.version);
            if (const krtest::Json* v = fields->Find("current")) saved.current = v->AsInt(saved.current);
            if (const krtest::Json* v = fields->Find("pending")) saved.pending = v->AsBool(saved.pending);
            if (const krtest::Json* v = fields->Find("raging")) saved.raging = v->AsBool(saved.raging);
        }
        ctx.state.Restore(saved);
    } else if (kind == "reconcile") {
        ctx.state.ReconcileAfterLoad(value && value->AsBool());
    } else if (kind == "reset") {
        ctx.state.Reset();
    } else if (kind == "snapshot") {
        // 只做检查
    } else {
        krtest::RecordFailure(__FILE__, __LINE__, "unknown op: " + kind);
        return;
    }

    if (expect) CheckSnapshotFields(ctx.state, *expect, __FILE__, __LINE__);
}

void RunActivationCase(const krtest::Json& fixtureCase)
{
    ActivationContext ctx;
    if (const krtest::Json* cfg = fixtureCase.Find("initialSettings")) ApplySettingsJson(*cfg, ctx.settings);
    ctx.state.Configure(ctx.settings);

    if (const krtest::Json* ops = fixtureCase.Find("ops")) {
        if (!ops->IsArray()) {
            krtest::RecordFailure(__FILE__, __LINE__, "ops must be an array");
            return;
        }
        for (const krtest::Json& op : ops->items) ApplyOp(ctx, op);
    }
}

}  // namespace

// ============================ 手工用例（规则逐条） ==========================

// 只有"已装备 + 满怒"才可启动。
KR_TEST(activation_requires_full_and_eligible)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;
    state.Configure(settings);

    KRT_EXPECT_NONE(state.TryBeginActivation());

    state.SetEligible(true);
    KRT_EXPECT_NONE(state.TryBeginActivation());

    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);
    KRT_EQB(state.GetSnapshot().ready, true);
    KRT_EXPECT_HAS(state.TryBeginActivation());
}

// 同一时刻只能存在一个启动请求。
KR_TEST(single_pending_activation)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);

    const std::optional<std::uint64_t> first = state.TryBeginActivation();
    KRT_EXPECT_HAS(first);
    KRT_EQB(state.GetSnapshot().pending, true);
    KRT_EQB(state.GetSnapshot().ready, false);
    KRT_EXPECT_NONE(state.TryBeginActivation());
}

// token 在多次尝试之间不复用。
KR_TEST(activation_token_is_not_reused_across_attempts)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);

    const std::optional<std::uint64_t> first = state.TryBeginActivation();
    KRT_EXPECT_HAS(first);
    if (!first.has_value()) return;
    KRT_EXPECT(state.AbortActivation(*first));

    const std::optional<std::uint64_t> second = state.TryBeginActivation();
    KRT_EXPECT_HAS(second);
    if (!second.has_value()) return;
    KRT_EXPECT_FALSE(*first == *second);
}

// 提交成功：清零一次、进入变身、期间不积攒。
KR_TEST(commit_success_clears_once_and_enters_raging)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);

    const std::optional<std::uint64_t> token = state.TryBeginActivation();
    KRT_EXPECT_HAS(token);
    if (!token.has_value()) return;

    KRT_EXPECT(state.CommitActivation(*token));
    const kr::Snapshot snap = state.GetSnapshot();
    KRT_EQI(snap.current, 0);
    KRT_EQB(snap.raging, true);
    KRT_EQB(snap.pending, false);
    KRT_EQB(snap.ready, false);

    KRT_EXPECT_FALSE(state.CommitActivation(*token));
    KRT_EQI(state.GetSnapshot().current, 0);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 2, 72), 0.5).awarded, 0);
}

// 错误 token 提交/中止都失败，满怒保留。
KR_TEST(wrong_token_failure_preserves_rage)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);

    const std::optional<std::uint64_t> token = state.TryBeginActivation();
    KRT_EXPECT_HAS(token);
    if (!token.has_value()) return;

    KRT_EXPECT_FALSE(state.CommitActivation(*token + 1));
    KRT_EQI(state.GetSnapshot().current, 4);
    KRT_EQB(state.GetSnapshot().pending, true);
    KRT_EQB(state.GetSnapshot().ready, false);

    KRT_EXPECT_FALSE(state.AbortActivation(*token + 1));
    KRT_EQB(state.GetSnapshot().pending, true);
    KRT_EQI(state.GetSnapshot().current, 4);
}

// 中止：满怒保留、可重试；重试后旧 token 失效。
KR_TEST(abort_preserves_rage_and_allows_retry)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);

    const std::optional<std::uint64_t> first = state.TryBeginActivation();
    KRT_EXPECT_HAS(first);
    if (!first.has_value()) return;
    KRT_EXPECT(state.AbortActivation(*first));

    const kr::Snapshot afterAbort = state.GetSnapshot();
    KRT_EQI(afterAbort.current, 4);
    KRT_EQB(afterAbort.pending, false);
    KRT_EQB(afterAbort.ready, true);

    const std::optional<std::uint64_t> second = state.TryBeginActivation();
    KRT_EXPECT_HAS(second);
    if (!second.has_value()) return;

    KRT_EXPECT_FALSE(state.CommitActivation(*first));
    KRT_EQI(state.GetSnapshot().current, 4);
    KRT_EXPECT(state.CommitActivation(*second));
    KRT_EQI(state.GetSnapshot().current, 0);
    KRT_EQB(state.GetSnapshot().raging, true);
}

// 没有启动请求时提交/中止一律失败且不改状态。
KR_TEST(commit_or_abort_without_pending_fails)
{
    kr::RageState state;
    state.Configure(kr::RageSettings{});
    state.SetEligible(true);

    KRT_EXPECT_FALSE(state.CommitActivation(1));
    KRT_EXPECT_FALSE(state.AbortActivation(1));
    KRT_EQI(state.GetSnapshot().current, 0);
    KRT_EQB(state.GetSnapshot().raging, false);
}

// 读档遗留的 pending 未结算前不得再次启动。
KR_TEST(activation_blocked_while_restored_pending)
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

    KRT_EXPECT_NONE(state.TryBeginActivation());
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 0);
    KRT_EQI(state.GetSnapshot().current, 4);
}

// Reset 清空 pending，旧 token 随之失效。
KR_TEST(reset_clears_pending_activation)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);

    const std::optional<std::uint64_t> token = state.TryBeginActivation();
    KRT_EXPECT_HAS(token);
    if (!token.has_value()) return;

    state.Reset();
    const kr::Snapshot snap = state.GetSnapshot();
    KRT_EQI(snap.current, 0);
    KRT_EQB(snap.pending, false);
    KRT_EQB(snap.raging, false);
    KRT_EQB(snap.ready, false);
    KRT_EXPECT_FALSE(state.CommitActivation(*token));
}

// 外部变身（SetRaging(true)）遇到本地 pending 必须当场结算：按 abort 语义不扣怒气、保留可重试，
// 但不得悬挂到变身结束——悬空的 pending 会被变身期间的 Save() 写成 pending=true，
// 下次读档 ReconcileAfterLoad 见到 pending+实际变身就会误扣一次怒气（in-session 与读档结果不一致）。
KR_TEST(external_raging_resolves_pending_immediately_without_charge)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);

    KRT_EXPECT_HAS(state.TryBeginActivation());

    state.SetRaging(true);
    KRT_EQB(state.GetSnapshot().raging, true);
    KRT_EQB(state.GetSnapshot().pending, false);  // 进入变身即结算，不再悬挂
    KRT_EQI(state.GetSnapshot().current, 4);      // abort 语义：未扣怒气

    const kr::SavedState midRage = state.Save();
    KRT_EQB(midRage.pending, false);  // 变身期间存档不得带 pending
    KRT_EQB(midRage.raging, true);
    KRT_EQI(midRage.current, 4);

    state.SetRaging(false);
    KRT_EQB(state.GetSnapshot().raging, false);
    KRT_EQB(state.GetSnapshot().pending, false);
    KRT_EQI(state.GetSnapshot().current, 4);      // 满怒保留，可重试
    KRT_EQB(state.GetSnapshot().ready, true);
    KRT_EXPECT_HAS(state.TryBeginActivation());
}

// ---------------------------------------------------------------------------
// SPEC_GAP：按契约编写、当前实现下预期失败的用例（证据，不是测试写错）。
// ---------------------------------------------------------------------------

// 契约：一次完整的变身周期结束后，启动请求（pending）不得残留。
// 现状：SetRaging() 不触碰 pending_，若变身由外部路径触发（未经 CommitActivation/AbortActivation），
//       pending 会永久悬挂——ready 恒为 false，既不能积攒也不能再次触发，直到读档（Restore）才自愈。
KR_TEST(SPEC_GAP_external_raging_cycle_must_not_leak_pending)
{
    kr::RageState state;
    kr::RageSettings settings;
    settings.maximum = 4;
    settings.normalHit = 4;
    state.Configure(settings);
    state.SetEligible(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 1, 71), 0.0).awarded, 4);

    KRT_EXPECT_HAS(state.TryBeginActivation());

    state.SetRaging(true);
    KRT_EQI(state.TryAward(Ev(kr::GainKind::Normal, 2, 72), 0.3).awarded, 0);
    KRT_EQB(state.GetSnapshot().raging, true);
    state.SetRaging(false);

    KRT_EQB(state.GetSnapshot().raging, false);
    KRT_EQB(state.GetSnapshot().pending, false);
}

// ============================ fixture 驱动用例 ==============================

namespace krtest {

int RunActivationFixtureCases(const std::string& fixtureDir)
{
    const std::string path = fixtureDir + "/activation_cases.json";
    Json root;
    std::string error;
    SetCurrentCase("fixture:activation_cases.json");
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
        const std::string name = "activation[" + std::to_string(i) + "] " +
                                 (nameJson ? nameJson->AsString() : std::string("<unnamed>"));
        const bool isSpecGap = name.find("spec_gap") != std::string::npos;
        const std::size_t before = Failures().size();
        SetCurrentCase(name);
        RunActivationCase(fixtureCase);
        const bool ok = Failures().size() == before;
        if (!ok) ++failed;
        std::cout << (ok ? "[PASS] " : (isSpecGap ? "[SPEC-GAP-EXPECTED-FAIL] " : "[FAIL] ")) << name << "\n";
    }
    return failed;
}

}  // namespace krtest
