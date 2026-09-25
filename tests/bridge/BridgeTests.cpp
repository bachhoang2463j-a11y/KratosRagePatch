// ===========================================================================
// KratosRagePatch —— src/GameBridge.cpp 离线集成回归（mock harness 驱动）
//
// 被测对象是 tools/test_bridge.py 原样复制到 build/bridge-tests/source 的
// src/GameBridge.cpp + src/RageState.cpp；伪游戏 API 见 tests/bridge/fake/Fake.h。
//
// 设计原则：
//  * 断言只读真实 GameBridge 产生的可观测副作用（怒气快照、施法日志、spell marker、
//    mod callback 事件、SKSE 任务队列），fake 不自证、不复写桥接规则。
//  * 0.2.0 起怒气的唯一增长通道是战斗时钟：Papyrus 每 0.5s 单发 TickCombat()，
//    战斗中每满 1 秒 +2（首 tick 只校准），菜单/暂停不涨也不补时间；脱战或死亡
//    立即清零并撤 ready/取消 pending。旧 SubmitWeaponHit/MagicHit/ProjectileHit/
//    IncomingHit/Kill 与 SubmitPlayerDamage 全部无操作。eligible 只由配置 + 原装备
//    决定，仅限制原释放，不影响积攒。
//  * 计时基于真实 steady_clock：测试用 ~1.05s 的真实间隔推进心跳（<1.5s 断档阈值，
//    累计满 1 秒 +2）；>1.5s 的断档必须不补算。
//  * 用例按「能抓旧时序缺陷」写：同帧排空后仍必须 pending 且不得回滚；陈旧请求在
//    Restore 后不得执行。tools/test_bridge.py --mutant 可实测这一区分能力。
// ===========================================================================

#include "Fake.h"
#include "GameBridge.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

void Fail(const char* file, int line, const std::string& message)
{
    ++g_failures;
    std::cout << "    [FAIL] " << g_case << " @ " << file << ":" << line << " -> " << message << "\n";
}

void CheckTrue(bool ok, const char* expr, const char* file, int line)
{
    ++g_checks;
    if (!ok) {
        Fail(file, line, std::string("expected true: ") + expr);
    }
}

void CheckEq(long long actual, long long expected, const char* expr, const char* file, int line)
{
    ++g_checks;
    if (actual != expected) {
        Fail(file, line, std::string(expr) + ": expected " + std::to_string(expected)
                + ", actual " + std::to_string(actual));
    }
}

#define CHECK(cond) ::CheckTrue(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
    ::CheckEq(static_cast<long long>(actual), static_cast<long long>(expected), #actual, __FILE__, __LINE__)

constexpr float kDeniedInterval = 0.5f;

// ---------------------------------------------------------------------------
// 场景（表单对象整进程只建一次；GameBridge 单例长期持有这些指针）
// ---------------------------------------------------------------------------
struct Scenario {
    RE::PlayerCharacter* player{};
    RE::TESObjectWEAP* axe{};
    RE::BGSKeyword* axeKeyword{};
    RE::EffectSetting* rageEffect{};      // God of War.esp 0x813
    RE::SpellItem* rageSpell{};           // God of War.esp 0x814
    RE::SpellItem* readyAbility{};        // KratosRagePatch.esp 0x802
    RE::EffectSetting* requestEffect{};   // KratosRagePatch.esp 0x805
    RE::EffectSetting* movement[4]{};     // Keytrace.esp 0x801..0x804
    RE::SpellItem* successCasts[3]{};     // 0x2FFD2 / 0x841 / 0x7A82B
    RE::SpellItem* nativeReapply[2]{};    // 0x817 / 0x844（native 不得重复施放）
    std::vector<RE::Actor*> enemies;
};

Scenario& S()
{
    static Scenario scenario;
    return scenario;
}

bool BuildWorldOnce()
{
    static bool built = false;
    static bool ok = false;
    if (built) {
        return ok;
    }
    built = true;

    auto& s = S();
    const int skyrim = krfake::AddPlugin("Skyrim.esm");
    const int gow = krfake::AddPlugin("God of War.esp");
    const int patch = krfake::AddPlugin("KratosRagePatch.esp");
    const int keytrace = krfake::AddPlugin("Keytrace.esp");
    const int mjolnir = krfake::AddPlugin("[full_inu] Mjolnir.esp");
    const int pkis = krfake::AddPlugin("PPM PKiS.esp");

    // ResolveForms 的成功条件
    s.rageEffect = krfake::AddEffect(gow, 0x813);
    s.rageSpell = krfake::AddSpell(gow, 0x814);
    s.readyAbility = krfake::AddSpell(patch, 0x802);
    krfake::AddEffect(patch, 0x803);
    s.requestEffect = krfake::AddEffect(patch, 0x805);
    s.axeKeyword = krfake::AddKeyword(gow, 0x805);
    for (std::uint32_t i = 0; i < 4; ++i) {
        s.movement[i] = krfake::AddEffect(keytrace, 0x801 + i);
    }
    s.axe = krfake::AddWeapon(gow, 0x900);
    s.axe->weaponType = RE::WEAPON_TYPE::kOneHandAxe;
    s.axe->AddKeywordForTest(s.axeKeyword);

    // 白名单武器
    krfake::AddWeapon(mjolnir, 0x808);
    krfake::AddWeapon(pkis, 0xF5EB);
    for (std::uint32_t id : {0x804u, 0x807u}) {
        krfake::AddWeapon(mjolnir, id);
    }
    for (std::uint32_t id : {0xF5EDu, 0xF5EFu, 0xF5F0u, 0xF5F1u, 0xF5F2u}) {
        krfake::AddWeapon(pkis, id);
    }

    // 成功施法清单（缺任一 ResolveForms 就失败）
    s.successCasts[0] = krfake::AddSpell(skyrim, 0x2FFD2);
    s.successCasts[1] = krfake::AddSpell(gow, 0x841);
    s.successCasts[2] = krfake::AddSpell(skyrim, 0x7A82B);

    // native 不得重复施放的原版法术
    s.nativeReapply[0] = krfake::AddSpell(gow, 0x817);
    s.nativeReapply[1] = krfake::AddSpell(gow, 0x844);

    // 玩家 / 敌群
    s.player = krfake::AddPlayer(0x00000014);
    for (int i = 0; i < 8; ++i) {
        s.enemies.push_back(krfake::AddActor(static_cast<RE::FormID>(0x00010000 + i)));
    }

    kr::GameBridge::Get().Initialize();
    ok = kr::GameBridge::Get().IsAvailable();
    if (!ok) {
        std::cout << "!! GameBridge::Initialize() 未就绪：fake 记录表不完整，后续用例无意义\n";
    }
    return ok;
}

kr::RageSettings ScoreSettings()
{
    kr::RageSettings settings;
    settings.maximum = 100;
    settings.normalHit = 7;
    settings.powerHit = 9;
    settings.specialHit = 11;
    settings.receivedHit = 13;
    settings.kill = 15;
    settings.maxGainPerSecond = 1;   // 战斗时钟不得读它
    return settings;
}

kr::RageSettings FullSettings()
{
    kr::RageSettings settings;
    settings.maximum = 4;            // 2 秒战斗即可打满，缩短用例墙钟时间
    settings.normalHit = 5;
    settings.powerHit = 6;
    settings.specialHit = 7;
    settings.receivedHit = 1;
    settings.kill = 9;
    settings.maxGainPerSecond = 1000;
    return settings;
}

void EquipAxe()
{
    S().player->equippedRight = S().axe;
    S().player->equippedLeft = nullptr;
}

// 每个用例的共同起点：清副作用日志 + 重置游戏态与桥接态，再按给定配置就绪。
// 与生产时序对齐：Revert/Restore 后必然有 ReconcileAfterLoad（OnPlayerLoadGame）收口。
void Start(const kr::RageSettings& settings)
{
    krfake::ResetScenario();
    auto& bridge = kr::GameBridge::Get();
    bridge.Revert();
    krfake::DrainAll();
    EquipAxe();
    bridge.Configure(settings, kDeniedInterval);
    krfake::DrainAll();
    bridge.ReconcileAfterLoad();
    krfake::DrainAll();
}

kr::Snapshot Snap() { return kr::GameBridge::Get().GetSnapshot(); }
int Current() { return Snap().current; }
bool MarkerOn() { return S().player->HasSpell(S().readyAbility); }

void SetCombat(bool on) { S().player->inCombat = on; }

// Papyrus 0.5s 单发心跳的测试替身：入队 + 排空。
void Tick()
{
    kr::GameBridge::Get().TickCombat();
    krfake::DrainAll();
}

// 真实等待 ms 后打一次心跳。~1050ms 保证单次间隔落在 (0, 1.5s] 内且累计满 1 秒。
void Advance(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    Tick();
}

// 旧 PEX 兼容入口：均为无操作（保留签名，供旧脚本调用不报错）。
void WeaponHit(RE::Actor* target, int flags = 0)
{
    kr::GameBridge::Get().SubmitWeaponHit(target, S().axe, nullptr, flags);
    krfake::DrainAll();
}
void MagicHit(RE::Actor* target, RE::TESForm* source)
{
    kr::GameBridge::Get().SubmitMagicHit(target, source, nullptr);
    krfake::DrainAll();
}
void ProjectileHit(RE::Actor* target, RE::TESForm* source)
{
    kr::GameBridge::Get().SubmitProjectileHit(target, source, nullptr);
    krfake::DrainAll();
}
void IncomingHit(RE::Actor* attacker, RE::TESForm* source, bool blocked)
{
    kr::GameBridge::Get().SubmitIncomingHit(attacker, source, nullptr, false, false, false, blocked);
    krfake::DrainAll();
}
void Kill(RE::Actor* victim)
{
    kr::GameBridge::Get().SubmitKill(victim, S().player);
    krfake::DrainAll();
}
void PlayerDamage(RE::Actor* target, float healthLost = 5.0f)
{
    kr::GameBridge::Get().SubmitPlayerDamage(target ? target->GetFormID() : 0, 0, healthLost);
    krfake::DrainAll();
}

void FireEffectEvent(RE::TESObjectREFR* target, RE::EffectSetting* effect)
{
    RE::TESMagicEffectApplyEvent event;
    event.target = RE::NiPointer<RE::TESObjectREFR>(target);
    event.caster = RE::NiPointer<RE::TESObjectREFR>(S().player);
    event.magicEffect = effect->GetFormID();
    krfake::FireMagicEffect(event);
}
void Request805() { FireEffectEvent(S().player, S().requestEffect); }

void Refresh()
{
    kr::GameBridge::Get().RefreshEquipmentState();
    krfake::DrainAll();
}

void FireMenu(bool opening)
{
    RE::MenuOpenCloseEvent event;
    event.menuName = RE::BSFixedString("TestMenu");
    event.opening = opening;
    krfake::FireMenuOpenClose(event);
}

// 进战斗后用 ~1.05s 间隔的心跳把怒气打满到当前上限（不触发施法）。
void FillRage()
{
    SetCombat(true);
    Tick();   // 首 tick 只校准
    for (int i = 0; i < 20 && Current() < Snap().maximum; ++i) {
        Advance(1050);
    }
}

int SuccessCastTotal()
{
    int total = 0;
    for (RE::SpellItem* spell : S().successCasts) {
        total += krfake::CastCount(spell);
    }
    return total;
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

// 1. 战斗时钟与配置无关：未 Configure 也按战斗时长 +2/秒；未配置时 eligible/ready
//    仍为假，也不得挂 ready marker。
void CaseCombatScoresWithoutConfigure()
{
    krfake::ResetScenario();
    kr::GameBridge::Get().Revert();
    krfake::DrainAll();
    EquipAxe();
    kr::GameBridge::Get().ReconcileAfterLoad();   // 与生产一致：Revert 后 PostLoad/NewGame 必然 reconcile
    krfake::DrainAll();

    SetCombat(true);
    Tick();                       // 首 tick 校准
    CHECK_EQ(Current(), 0);
    Advance(1050);
    CHECK_EQ(Current(), 2);

    CHECK(!Snap().eligible);
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());
}

// 2. 固定 +2/秒，与 normalHit/powerHit/... 及 maxGainPerSecond 无关；配置上限可见。
void CaseCombatGainIsFixedAndMaximumApplies()
{
    Start(ScoreSettings());       // maxGainPerSecond = 1、hit 值全部刻意异常
    CHECK_EQ(Snap().maximum, 100);

    SetCombat(true);
    Tick();
    Advance(1050);
    CHECK_EQ(Current(), 2);
    Advance(1050);
    CHECK_EQ(Current(), 4);
    Advance(1050);
    CHECK_EQ(Current(), 6);       // 仍然每秒 +2，与 maxGainPerSecond=1 / normalHit=7 无关

    CHECK_EQ(krfake::Casts().size(), 0);
}

// 3. maximum 封顶且 KR_RageBecameFull 只发一次（边沿）。
void CaseMaximumCapAndBecameFullOnce()
{
    Start(FullSettings());        // maximum = 4
    SetCombat(true);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 0);

    FillRage();
    CHECK_EQ(Current(), 4);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 1);
    CHECK(Snap().ready);
    CHECK(MarkerOn());

    Advance(1050);                // 满怒后继续心跳：封顶且不再发事件
    Advance(1050);
    CHECK_EQ(Current(), 4);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 1);
}

// 4. 非战斗零增长；脱战立即清零并发 KR_RageChanged，此后仍不涨。
void CaseNoGainAndClearWhenOutOfCombat()
{
    Start(ScoreSettings());
    const int changesAtStart = krfake::EventCount("KR_RageChanged");

    Tick();                       // 未进战斗：只校准
    Advance(1050);
    CHECK_EQ(Current(), 0);

    SetCombat(true);
    Tick();
    Advance(1050);
    CHECK_EQ(Current(), 2);

    SetCombat(false);
    Tick();                       // 脱战 tick：清零
    CHECK_EQ(Current(), 0);
    CHECK(krfake::EventCount("KR_RageChanged") > changesAtStart);

    Advance(1050);
    CHECK_EQ(Current(), 0);       // 脱战后不涨
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());
}

// 5. 脱战清零的同时取消 pending、撤 ready；脱战后满怒请求不得穿过（不得施法）。
void CaseLeaveCombatCancelsPendingAndBlocksActivation()
{
    Start(FullSettings());
    FillRage();
    CHECK(Snap().ready);

    Request805();
    krfake::RunFrame();
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK(Snap().pending);

    SetCombat(false);
    Tick();
    CHECK_EQ(Current(), 0);           // 脱战清零
    CHECK(!Snap().pending);           // 挂起被取消
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // 越过 0.25s 施法节流
    Request805();                     // 非战斗满怒不得穿过
    krfake::DrainAll();
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK_EQ(SuccessCastTotal(), 0);
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 1);
}

// 6. 旧 PEX 五个入口、死亡事件与旧伤害通道都不得计分（全部无操作），只有战斗时钟生效。
void CaseLegacyEntriesDoNotScore()
{
    Start(ScoreSettings());
    SetCombat(true);
    CHECK_EQ(Current(), 0);

    WeaponHit(S().enemies[0]);
    MagicHit(S().enemies[1], S().successCasts[1]);
    ProjectileHit(S().enemies[2], S().successCasts[1]);
    IncomingHit(S().enemies[3], S().successCasts[1], false);
    Kill(S().enemies[4]);
    PlayerDamage(S().enemies[5]);
    PlayerDamage(nullptr);
    PlayerDamage(S().enemies[6], 0.0f);
    PlayerDamage(S().enemies[6], -1.0f);
    CHECK_EQ(Current(), 0);

    RE::TESDeathEvent death;
    death.actorDying = RE::NiPointer<RE::TESObjectREFR>(S().enemies[5]);
    death.actorKiller = RE::NiPointer<RE::TESObjectREFR>(S().player);
    death.dead = true;
    krfake::FireDeath(death);
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    // 对照：战斗时钟仍活着
    Tick();
    Advance(1050);
    CHECK_EQ(Current(), 2);
    CHECK_EQ(krfake::Casts().size(), 0);
}

// 7. 菜单/暂停停表且不补时间：短暂停、长断档、UI 暂停与 Main freezeTime 都不产生加分。
void CaseMenuSuspendDoesNotCatchUp()
{
    auto* ui = RE::UI::GetSingleton();
    auto* main = RE::Main::GetSingleton();
    Start(ScoreSettings());
    SetCombat(true);
    Tick();
    Advance(1050);
    CHECK_EQ(Current(), 2);

    // (a) 短暂停（open/close 事件紧挨着）也不补：close 后首个 tick 只校准
    FireMenu(true);
    FireMenu(false);
    Tick();
    CHECK_EQ(Current(), 2);
    Advance(1050);
    CHECK_EQ(Current(), 4);

    // (b) 长断档（菜单打开挂起 > 1.5s）不得追补
    FireMenu(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    Tick();
    CHECK_EQ(Current(), 4);       // 挂起期间的心跳不加分
    FireMenu(false);
    Tick();                       // close 后首个 tick 校准
    CHECK_EQ(Current(), 4);
    Advance(1050);
    CHECK_EQ(Current(), 6);

    // (c) UI GameIsPaused 停表
    ui->paused = true;
    Advance(1050);
    CHECK_EQ(Current(), 6);
    ui->paused = false;
    Tick();
    Advance(1050);
    CHECK_EQ(Current(), 8);

    // (d) Main freezeTime 停表
    main->freezeTime = true;
    Advance(1050);
    CHECK_EQ(Current(), 8);
    main->freezeTime = false;
    Tick();
    Advance(1050);
    CHECK_EQ(Current(), 10);

    CHECK_EQ(krfake::Casts().size(), 0);
}

// 8. 打满 + 同步 813（施法即时落效果）→ 只施放一次 814，确认后清零并结算
void CaseFullOneCastSyncConfirm()
{
    Start(FullSettings());
    FillRage();
    CHECK_EQ(Current(), 4);
    CHECK(MarkerOn());

    krfake::OnCastApply(S().rageSpell, S().rageEffect);  // 施放 814 时引擎挂上 813
    Request805();
    krfake::DrainAll();
    Refresh();

    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK_EQ(Snap().pending, false);
    CHECK_EQ(Snap().raging, true);
    CHECK_EQ(Current(), 0);
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 0);
    CHECK_EQ(SuccessCastTotal(), 3);
    CHECK(!MarkerOn());
    CHECK(!Snap().ready);

    Request805();                      // 满怒只允许一次施放
    krfake::DrainAll();
    Refresh();
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK_EQ(SuccessCastTotal(), 3);
}

// 9. 打满 + 延迟 813（先施法后落效果/事件）→ 仍然一次施放并且只在确认后清零
void CaseFullOneCastDelayedConfirm()
{
    Start(FullSettings());
    FillRage();
    CHECK_EQ(Current(), 4);

    Request805();
    krfake::DrainAll();
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK_EQ(Snap().pending, true);    // 未确认前必须挂起
    CHECK_EQ(Current(), 4);
    CHECK_EQ(SuccessCastTotal(), 0);

    krfake::ApplyEffect(S().player, S().rageEffect);  // 效果延迟落下
    FireEffectEvent(S().player, S().rageEffect);      // 引擎 813 事件
    Refresh();

    CHECK_EQ(Snap().pending, false);
    CHECK_EQ(Snap().raging, true);
    CHECK_EQ(Current(), 0);
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK_EQ(SuccessCastTotal(), 3);

    Refresh();                          // 重复刷新不得重复结算
    CHECK_EQ(SuccessCastTotal(), 3);
}

// 10. 同帧排空后仍未确认 → 必须仍 pending、怒气仍满、不得报拒绝；0.5s 后刷新才回滚
//     （「同帧三次重试后立刻 abort」的旧实现会在此 FAIL）
void CasePendingSurvivesSameFrameDrain()
{
    Start(FullSettings());
    FillRage();
    CHECK_EQ(Current(), 4);

    Request805();
    const int ran = krfake::DrainAll();
    CHECK(!krfake::SpinFlag());
    CHECK(ran <= 8);
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);     // 成功施法只有 814
    CHECK_EQ(Snap().pending, true);                    // 同帧排空后必须仍 pending
    CHECK_EQ(Current(), 4);                            // 怒气不得被扣
    CHECK_EQ(Snap().raging, false);
    CHECK_EQ(SuccessCastTotal(), 0);
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 0);  // 不得在同帧内回滚并报拒绝
    CHECK(!Snap().ready);                              // pending 期间不得 ready
    CHECK(!MarkerOn());                                // ready marker 必须撤下，防止重复触发

    Refresh();                                          // deadline 未到 → 仍不得回滚
    CHECK_EQ(Snap().pending, true);
    CHECK_EQ(Current(), 4);
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    Refresh();                                          // 越过 0.5s deadline 才回滚

    CHECK_EQ(Snap().pending, false);
    CHECK_EQ(Current(), 4);                             // 失败保留怒气
    CHECK_EQ(Snap().raging, false);
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);       // 回滚不重施法
    CHECK_EQ(SuccessCastTotal(), 0);
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 1);
    CHECK(MarkerOn());
    CHECK(Snap().ready);

    // 回滚后可再次尝试，且这次 813 生效 → 正常结算
    krfake::OnCastApply(S().rageSpell, S().rageEffect);
    Request805();
    krfake::DrainAll();
    Refresh();
    CHECK_EQ(Current(), 0);
    CHECK_EQ(Snap().raging, true);
    CHECK_EQ(SuccessCastTotal(), 3);
}

// 11. SKSE 任务必须可显式 drain：RunFrame 只跑当帧已入队任务，DrainAll 有界排空
void CaseExplicitTaskDrain()
{
    Start(FullSettings());
    FillRage();

    Request805();
    CHECK(!krfake::Tasks().empty());    // 事件回调只是入队，不同步执行
    krfake::RunFrame();                 // 显式跑一帧
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK_EQ(Snap().pending, true);

    kr::GameBridge::Get().RefreshEquipmentState();   // 只入队
    CHECK(!krfake::Tasks().empty());
    const int ran = krfake::DrainAll(200);
    CHECK(!krfake::SpinFlag());
    CHECK(ran >= 1);
    CHECK(ran < 50);
}

// 12. native 不得代为重复施放 817 / 844；成功施法严格等于 814 + 三个结算 payload
void CaseNativeDoesNotReapplyOriginals()
{
    Start(FullSettings());
    FillRage();
    krfake::OnCastApply(S().rageSpell, S().rageEffect);
    Request805();
    krfake::DrainAll();
    Refresh();

    CHECK_EQ(krfake::Casts().size(), 4);
    CHECK_EQ(krfake::CastCount(S().nativeReapply[0]), 0);
    CHECK_EQ(krfake::CastCount(S().nativeReapply[1]), 0);
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK_EQ(krfake::CastCount(S().successCasts[0]), 1);
    CHECK_EQ(krfake::CastCount(S().successCasts[1]), 1);
    CHECK_EQ(krfake::CastCount(S().successCasts[2]), 1);

    const std::vector<krfake::CastRecord>& casts = krfake::Casts();
    CHECK(casts[1].spell == S().successCasts[0]);
    CHECK(casts[2].spell == S().successCasts[1]);
    CHECK(casts[3].spell == S().successCasts[2]);
    for (const krfake::CastRecord& cast : casts) {
        CHECK(cast.spell != S().nativeReapply[0]);
        CHECK(cast.spell != S().nativeReapply[1]);
    }
}

// 13. 变身期间战斗时钟照常积攒；变身结束后继续积攒
void CaseScoringResumesAfterRageEnds()
{
    Start(FullSettings());
    FillRage();
    krfake::OnCastApply(S().rageSpell, S().rageEffect);
    Request805();
    krfake::DrainAll();
    Refresh();
    CHECK(Snap().raging);
    CHECK_EQ(Current(), 0);

    Tick();                             // commit 清瞬态计时后的首个 tick 只校准
    Advance(1050);
    CHECK_EQ(Current(), 2);             // 变身期间也计分

    krfake::ClearEffects(S().player);   // 813 结束
    Refresh();
    CHECK(!Snap().raging);
    CHECK(Snap().eligible);

    Tick();                             // SetRaging 切换清瞬态计时后的校准 tick
    Advance(1050);
    CHECK_EQ(Current(), 4);             // 变身结束后继续计分
    CHECK(Snap().ready);
}

// 14. 旧会话入队的任务（读档后的 Configure + 遗留的 805 请求）在 Restore 之后必须整体失效
//     （Queue 无 generation epoch 的旧实现：Configure 先跑使状态重新 ready，旧请求紧接着施放 814
//      → 本用例在 krfake::CastCount(rageSpell) == 0 上 FAIL）
void CaseStaleRequestDroppedByRestore()
{
    Start(FullSettings());
    FillRage();
    CHECK_EQ(Current(), 4);
    CHECK(Snap().ready);

    const kr::SavedState saved = kr::GameBridge::Get().Save();
    CHECK_EQ(saved.current, 4);
    CHECK_EQ(saved.pending, false);

    // 读档完成前的同一帧里已经压入两件事：读档后的 Configure，以及一个旧会话遗留的 805 请求。
    kr::GameBridge::Get().Configure(FullSettings(), kDeniedInterval);
    Request805();
    CHECK(!krfake::Tasks().empty());

    kr::GameBridge::Get().Restore(saved);    // 读档：此前入队的一切都必须失效
    krfake::DrainAll();
    kr::GameBridge::Get().ReconcileAfterLoad();   // 生产时序：读档后必然 reconcile 收口
    krfake::DrainAll();

    CHECK_EQ(krfake::CastCount(S().rageSpell), 0);     // 陈旧请求不得启动施法
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 0);  // 也不得执行到"拒绝"分支
    CHECK_EQ(SuccessCastTotal(), 0);
    CHECK_EQ(Current(), 4);                            // 战斗存档保留怒气
    CHECK_EQ(Snap().pending, false);

    // 守卫不能过宽：Restore 之后重新配置的请求必须照常施法与结算
    kr::GameBridge::Get().Configure(FullSettings(), kDeniedInterval);
    krfake::DrainAll();
    CHECK(Snap().ready);
    krfake::OnCastApply(S().rageSpell, S().rageEffect);
    Request805();
    krfake::DrainAll();
    Refresh();
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK_EQ(Current(), 0);
    CHECK_EQ(Snap().raging, true);
    CHECK_EQ(SuccessCastTotal(), 3);
}

// 15. 未确认的挂起请求不得跨存档消耗怒气
void CasePendingDoesNotCrossSave()
{
    Start(FullSettings());
    FillRage();

    Request805();
    krfake::RunFrame();                 // 执行请求：施放 814、进入 pending、未确认
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK_EQ(Snap().pending, true);

    const kr::SavedState saved = kr::GameBridge::Get().Save();
    CHECK_EQ(saved.pending, true);      // 挂起状态必须写进存档
    CHECK_EQ(saved.current, 4);

    kr::GameBridge::Get().Restore(saved);
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();

    CHECK_EQ(Current(), 4);             // 旧请求不得跨档扣怒气
    CHECK_EQ(Snap().pending, false);
    CHECK_EQ(Snap().raging, false);
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);  // 不得重复施放
    CHECK_EQ(SuccessCastTotal(), 0);

    // 读档后重新配置再触发，应该正常结算（先越过 0.25s 施法节流：它是防连点重复施放的设计行为）
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    kr::GameBridge::Get().Configure(FullSettings(), kDeniedInterval);
    krfake::DrainAll();
    CHECK(Snap().ready);
    krfake::OnCastApply(S().rageSpell, S().rageEffect);
    Request805();
    krfake::DrainAll();
    Refresh();
    CHECK_EQ(Current(), 0);
    CHECK_EQ(SuccessCastTotal(), 3);
}

// 16. 三源移动键（方向键或 Keytrace 效果）与拒绝提示的可观测面
void CaseDeniedNotificationObservable()
{
    Start(FullSettings());

    kr::GameBridge::Get().NotifyActivationAttempt("Right Attack/Block", true, false, false, false, false);
    krfake::DrainAll();
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 0);   // 三源键未满足 → 不提示

    krfake::ApplyEffect(S().player, S().movement[0]);   // Keytrace 0x801
    krfake::ApplyEffect(S().player, S().movement[1]);   // Keytrace 0x802
    krfake::ApplyEffect(S().player, S().movement[3]);   // Keytrace 0x804
    kr::GameBridge::Get().NotifyActivationAttempt("Right Attack/Block", true, false, false, false, false);
    krfake::DrainAll();
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 1);   // 三个 movement 效果齐备 → 提示

    kr::GameBridge::Get().NotifyActivationAttempt("Right Attack/Block", true, false, false, false, false);
    krfake::DrainAll();
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 1);   // 0.5s 抑制窗口内不重复

    FillRage();
    CHECK(Snap().ready);
    kr::GameBridge::Get().NotifyActivationAttempt("Right Attack/Block", true, false, false, false, false);
    krfake::DrainAll();
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 1);   // 满怒待触发时不再报拒绝
}

// 17. Revert 同样必须使旧代已入队任务失效（与 14 的 Restore 版对称）
void CaseRevertDropsStaleRequest()
{
    Start(FullSettings());
    FillRage();
    CHECK_EQ(Current(), 4);

    kr::GameBridge::Get().Configure(FullSettings(), kDeniedInterval);  // 旧代任务
    Request805();                                                      // 旧代请求，尚未执行
    CHECK(!krfake::Tasks().empty());

    kr::GameBridge::Get().Revert();          // 应当使旧代任务整体失效
    krfake::DrainAll();

    CHECK_EQ(krfake::CastCount(S().rageSpell), 0);
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 0);  // 无 epoch 时陈旧请求会跑到拒绝分支
    CHECK_EQ(Current(), 0);                            // Revert 已清空怒气
    CHECK(!Snap().eligible);
}

// 18. 同一次读档里第二次 Reconcile 不得吞掉进行中的激活（needsReconcile_ 守卫）
void CaseDuplicateReconcileKeepsPending()
{
    Start(FullSettings());
    FillRage();

    Request805();
    krfake::RunFrame();                 // 第一次激活：pending
    CHECK_EQ(Snap().pending, true);
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);

    // 读档流程：Restore 置位 needsReconcile_，第一次 reconcile 按设计结算掉这次挂起
    const kr::SavedState saved = kr::GameBridge::Get().Save();
    kr::GameBridge::Get().Restore(saved);
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();
    kr::GameBridge::Get().Configure(FullSettings(), kDeniedInterval);   // Restore 清了 configured_
    krfake::DrainAll();
    CHECK_EQ(Snap().pending, false);
    CHECK_EQ(Current(), 4);
    CHECK(Snap().ready);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // 越过 0.25s 施法节流
    Request805();
    krfake::RunFrame();                 // 第二次激活：pending
    CHECK_EQ(Snap().pending, true);
    CHECK_EQ(krfake::CastCount(S().rageSpell), 2);

    // 第二次 reconcile（同一次读档里的重复调用）必须整体跳过：
    // 不得重置 activation_、不得发拒绝、不得吞掉刚推进的状态
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();
    CHECK_EQ(Snap().pending, true);
    CHECK_EQ(krfake::CastCount(S().rageSpell), 2);
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 0);
    CHECK_EQ(Current(), 4);

    // 若生效则最终仍能正常确认结算
    krfake::ApplyEffect(S().player, S().rageEffect);
    Refresh();
    CHECK_EQ(Snap().pending, false);
    CHECK_EQ(Current(), 0);
    CHECK_EQ(Snap().raging, true);
    CHECK_EQ(SuccessCastTotal(), 3);
}

// 19. 死亡立即结束战斗计时：清零、撤 ready、此后不涨。
void CaseDeathStopsAndClears()
{
    Start(ScoreSettings());
    SetCombat(true);
    Tick();
    Advance(1050);
    CHECK_EQ(Current(), 2);

    S().player->dead = true;
    Tick();
    CHECK_EQ(Current(), 0);           // 死亡视为脱战 → 清零
    CHECK(!Snap().eligible);

    Advance(1050);
    CHECK_EQ(Current(), 0);           // 死亡期间不涨
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());
}

// 20. 战斗中存档保留怒气、脱战后存档为 0；读档后按实际战斗状态重拉。
void CaseSaveRestoresCombatRageOnly()
{
    Start(ScoreSettings());
    SetCombat(true);
    Tick();
    Advance(1050);
    Advance(1050);
    CHECK_EQ(Current(), 4);

    const kr::SavedState inCombatSave = kr::GameBridge::Get().Save();
    CHECK_EQ(inCombatSave.current, 4);

    SetCombat(false);
    Tick();
    CHECK_EQ(Current(), 0);
    const kr::SavedState outOfCombatSave = kr::GameBridge::Get().Save();
    CHECK_EQ(outOfCombatSave.current, 0);

    // 恢复战斗存档（此时在战斗中）：怒气保留并继续按战斗时钟推进
    SetCombat(true);
    kr::GameBridge::Get().Restore(inCombatSave);
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();
    CHECK_EQ(Current(), 4);
    Advance(1050);
    CHECK_EQ(Current(), 4);           // 读档后首个 tick 只校准，不按存档前的时间补算
    Advance(1050);
    CHECK_EQ(Current(), 6);           // 之后按真实战斗时长推进

    // 恢复脱战存档：清零
    SetCombat(false);
    kr::GameBridge::Get().Restore(outOfCombatSave);
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();
    Tick();
    CHECK_EQ(Current(), 0);
}

void CaseLoadingAndMissingPlayerPreserveRage()
{
    Start(FullSettings());
    FillRage();
    auto& bridge = kr::GameBridge::Get();
    const auto saved = bridge.Save();
    auto* ui = RE::UI::GetSingleton();
    ui->loadingMenu = true;
    SetCombat(false);
    bridge.Restore(saved);
    Tick();
    CHECK_EQ(Current(), 4);
    Request805();
    krfake::DrainAll();
    CHECK_EQ(krfake::CastCount(S().rageSpell), 0);
    bridge.ReconcileAfterLoad();
    krfake::DrainAll();
    CHECK_EQ(Current(), 4);
    ui->loadingMenu = false;
    SetCombat(true);
    bridge.Configure(FullSettings(), kDeniedInterval);
    krfake::DrainAll();
    CHECK(Snap().ready);
    ui->console = true;
    Request805();
    krfake::DrainAll();
    CHECK_EQ(krfake::CastCount(S().rageSpell), 0);
    ui->console = false;
    krfake::PlayerSlot() = nullptr;
    Tick();
    CHECK_EQ(Current(), 4);
    krfake::PlayerSlot() = S().player;
    SetCombat(false);
    Tick();
    CHECK_EQ(Current(), 0);
    CHECK(!MarkerOn());
}

struct TestCase {
    const char* name;
    void (*fn)();
};

const TestCase kCases[] = {
    {"combat_scores_without_configure", &CaseCombatScoresWithoutConfigure},
    {"combat_gain_fixed_maximum_applies", &CaseCombatGainIsFixedAndMaximumApplies},
    {"maximum_cap_and_became_full_once", &CaseMaximumCapAndBecameFullOnce},
    {"no_gain_and_clear_when_out_of_combat", &CaseNoGainAndClearWhenOutOfCombat},
    {"leave_combat_cancels_pending_and_blocks_activation", &CaseLeaveCombatCancelsPendingAndBlocksActivation},
    {"legacy_entries_do_not_score", &CaseLegacyEntriesDoNotScore},
    {"menu_suspend_does_not_catch_up", &CaseMenuSuspendDoesNotCatchUp},
    {"full_one_cast_sync_confirm", &CaseFullOneCastSyncConfirm},
    {"full_one_cast_delayed_confirm", &CaseFullOneCastDelayedConfirm},
    {"pending_survives_same_frame_drain", &CasePendingSurvivesSameFrameDrain},
    {"explicit_task_drain", &CaseExplicitTaskDrain},
    {"native_does_not_reapply_originals", &CaseNativeDoesNotReapplyOriginals},
    {"scoring_resumes_after_rage_ends", &CaseScoringResumesAfterRageEnds},
    {"stale_request_dropped_by_restore", &CaseStaleRequestDroppedByRestore},
    {"pending_does_not_cross_save", &CasePendingDoesNotCrossSave},
    {"denied_notification_observable", &CaseDeniedNotificationObservable},
    {"revert_drops_stale_request", &CaseRevertDropsStaleRequest},
    {"duplicate_reconcile_keeps_pending", &CaseDuplicateReconcileKeepsPending},
    {"death_stops_and_clears", &CaseDeathStopsAndClears},
    {"save_restores_combat_rage_only", &CaseSaveRestoresCombatRageOnly},
    {"loading_and_missing_player_preserve_rage", &CaseLoadingAndMissingPlayerPreserveRage},
};

}  // namespace

int main()
{
    std::cout << std::unitbuf;
    std::cout << "== KratosRagePatch GameBridge mock integration tests ==\n";
    if (!BuildWorldOnce()) {
        std::cout << "world setup failed\n";
        return 2;
    }
    std::cout << "world ready, cases: " << (sizeof(kCases) / sizeof(kCases[0])) << "\n\n";

    int failed = 0;
    for (const TestCase& test : kCases) {
        g_case = test.name;
        const int before = g_failures;
        test.fn();
        const bool pass = (g_failures == before);
        std::cout << (pass ? "[PASS] " : "[FAIL] ") << test.name;
        if (!pass) {
            std::cout << " (" << (g_failures - before) << " failed checks)";
            ++failed;
        }
        std::cout << "\n";
    }

    std::cout << "\nsummary: cases " << (sizeof(kCases) / sizeof(kCases[0]))
              << ", failed " << failed << ", checks " << g_checks
              << ", assertion failures " << g_failures << "\n";
    return failed == 0 ? 0 : 1;
}
