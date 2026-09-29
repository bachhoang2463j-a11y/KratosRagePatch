// ===========================================================================
// KratosRagePatch —— src/GameBridge.cpp 离线集成回归（mock harness 驱动）
//
// 被测对象是 tools/test_bridge.py 原样复制到 build/bridge-tests/source 的
// src/GameBridge.cpp + src/RageState.cpp；伪游戏 API 见 tests/bridge/fake/Fake.h。
//
// 设计原则：
//  * 断言只读真实 GameBridge 产生的可观测副作用（怒气快照、施法日志、spell marker、
//    mod callback 事件、SKSE 任务队列），fake 不自证、不复写桥接规则。
//  * 0.3.0 起怒气的唯一增长通道是原生 TESHitEvent：玩家发起的、通过筛选的每一次命中
//    固定 +2（只受 maximum 封顶）；同一目标同源同一帧多段、跨目标命中分别累计，
//    不做 0.1s 去重、不套每秒预算。战斗时钟只负责状态：脱战/死亡清零、取消 pending、
//    变身状态与激活事务对账，不再授予怒气。旧 SubmitWeaponHit/MagicHit/ProjectileHit/
//    IncomingHit/Kill 与 SubmitPlayerDamage 全部无操作，死亡事件不计分。
//  * 过滤在「事件到达」与「任务处理」两个时点都要成立：到达时拒绝(available/未对账/
//    暂停/Loading/非玩家来源/格挡/自身/非 Actor/null/目标当场已死/队友/被指挥/非敌对)，
//    处理时重新确认(可用/活着/仍战斗/已对账/UI 未暂停/句柄可解析/目标仍非友军且敌对)。
//  * 用例按「能抓实现缺陷」写：捕获句柄而非裸指针（删映射即失效）、事件字段在入队前
//    复制（局部事件销毁后 drain 仍计分）、暂停/loading/未对账在到达或处理时拒绝且不补发、
//    Restore/Revert 使旧代任务失效。
// ===========================================================================

#include "Fake.h"
#include "GameBridge.h"

#include <chrono>
#include <cstddef>
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

// TESHitEvent::Flag 的位值（与 vendor CommonLib 一致）。
constexpr int kFlagPower = 1 << 0;
constexpr int kFlagSneak = 1 << 1;
constexpr int kFlagBash = 1 << 2;
constexpr int kFlagBlocked = 1 << 3;

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
    RE::TESObjectREFR* nonActorRefr{};    // 非 Actor 目标（过滤用）
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

    // 非 Actor 目标：TESObjectREFR 基类实例，As<Actor>() 必为 null
    s.nonActorRefr = krfake::AddRecord<RE::TESObjectREFR>(skyrim, 0x55);

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
    settings.maxGainPerSecond = 1;   // 命中通道不得读它
    return settings;
}

kr::RageSettings FullSettings()
{
    kr::RageSettings settings;
    settings.maximum = 4;            // 两次命中即可打满，缩短用例
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

RE::Actor* Enemy(int index)
{
    return S().enemies[static_cast<std::size_t>(index)];
}

// 命中通道只接受敌对目标；ResetScenario 会把 hostile 清回 false，用例起点统一重设。
void MarkEnemiesHostile()
{
    for (RE::Actor* enemy : S().enemies) {
        enemy->hostile = true;
    }
}

// 每个用例的共同起点：清副作用日志 + 重置游戏态与桥接态，再按给定配置就绪。
// 与生产时序对齐：Revert/Restore 后必然有 ReconcileAfterLoad（OnPlayerLoadGame）收口。
void Start(const kr::RageSettings& settings)
{
    krfake::ResetScenario();
    MarkEnemiesHostile();
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

// Papyrus 0.5s 单发心跳的测试替身：入队 + 排空。0.3.0 起它只处理状态，不再授予怒气。
void Tick()
{
    kr::GameBridge::Get().TickCombat();
    krfake::DrainAll();
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

// 原生 TESHitEvent：只入队（不同步执行），由 drain/RunFrame 显式推进。
void FireHit(RE::TESObjectREFR* target, RE::FormID source = 0, int flags = 0, RE::Actor* cause = nullptr)
{
    RE::TESHitEvent event;
    event.target = RE::NiPointer<RE::TESObjectREFR>(target);
    event.cause = RE::NiPointer<RE::TESObjectREFR>(cause ? cause : S().player);
    event.source = source;
    event.projectile = 0;
    event.flags = static_cast<RE::TESHitEvent::Flag>(flags);
    krfake::FireHit(event);
}

// 事件 + 排空（等价一次完整主线程处理）。
void Hit(RE::TESObjectREFR* target, RE::FormID source = 0, int flags = 0, RE::Actor* cause = nullptr)
{
    FireHit(target, source, flags, cause);
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

// 进战斗后用真实命中把怒气打满到当前上限（每次 +2，不触发施法）。
void FillRage()
{
    SetCombat(true);
    const int maximum = Snap().maximum;
    for (int i = 0; i < 500 && Current() < maximum; ++i) {
        Hit(Enemy(0));
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

// 菜单开关与战斗事件：只投递事件（不同步执行任务），由 drain 显式推进。
void FireMenu(const char* name, bool opening)
{
    RE::MenuOpenCloseEvent event;
    event.menuName = RE::BSFixedString(name);
    event.opening = opening;
    krfake::FireMenuOpenClose(event);
}

void FireCombat(RE::Actor* actor)
{
    RE::TESCombatEvent event;
    event.actor = RE::NiPointer<RE::TESObjectREFR>(actor);
    krfake::FireCombat(event);
}

// 日志观测：TraceCombatHit 的格式串含 "KratosRage hit"，args[0] 即 result 类别。
bool IsHitLog(const krfake::LogRecord& record)
{
    return record.format.find("KratosRage hit") != std::string::npos && !record.args.empty();
}

int HitLogCount(const std::string& result)
{
    int count = 0;
    for (const krfake::LogRecord& record : krfake::Logs()) {
        if (IsHitLog(record) && record.args[0] == result) {
            ++count;
        }
    }
    return count;
}

const krfake::LogRecord* FindHitLog(const std::string& result)
{
    for (const krfake::LogRecord& record : krfake::Logs()) {
        if (IsHitLog(record) && record.args[0] == result) {
            return &record;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

// 1. 未 Configure 也按命中 +2；未配置时 eligible/ready 仍为假，不挂 ready marker。
void CaseHitsWithoutConfigure()
{
    krfake::ResetScenario();
    MarkEnemiesHostile();
    auto& bridge = kr::GameBridge::Get();
    bridge.Revert();
    krfake::DrainAll();
    EquipAxe();
    bridge.ReconcileAfterLoad();   // 与生产一致：Revert 后 PostLoad/NewGame 必然 reconcile
    krfake::DrainAll();

    SetCombat(true);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 2);
    Hit(Enemy(1));
    CHECK_EQ(Current(), 4);

    CHECK(!Snap().eligible);
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());
}

// 2. 命中固定 +2：与 normalHit/powerHit/... 及 maxGainPerSecond 无关，也不做 0.1s 去重。
void CaseHitGainFixedIgnoresLegacySettings()
{
    Start(ScoreSettings());       // maxGainPerSecond = 1、hit 值全部刻意异常
    CHECK_EQ(Snap().maximum, 100);

    SetCombat(true);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 2);
    Hit(Enemy(0));                // 同源同目标紧接命中不被合并
    CHECK_EQ(Current(), 4);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 6);       // 与 maxGainPerSecond=1 / normalHit=7 无关

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

    Hit(Enemy(0));                // 满怒后继续命中：封顶且不再发事件
    Hit(Enemy(0));
    CHECK_EQ(Current(), 4);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 1);
}

// 4. 奇数上限：最后一次只补差额；满怒边沿只报一次。
void CaseOddMaximumPartialAndFullEdge()
{
    kr::RageSettings settings = FullSettings();
    settings.maximum = 5;
    Start(settings);
    SetCombat(true);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 0);

    Hit(Enemy(0));
    CHECK_EQ(Current(), 2);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 4);
    Hit(Enemy(0));                // 只补 1 点
    CHECK_EQ(Current(), 5);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 1);

    Hit(Enemy(0));
    CHECK_EQ(Current(), 5);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 1);
    CHECK(Snap().ready);
}

// 5. 非战斗命中零增长；脱战立即清零并发 KR_RageChanged，此后仍不涨。
void CaseNoGainOutOfCombatAndClearOnLeave()
{
    Start(ScoreSettings());
    const int changesAtStart = krfake::EventCount("KR_RageChanged");

    Hit(Enemy(0));                // 未进战斗：拒绝
    CHECK_EQ(Current(), 0);

    SetCombat(true);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 2);

    SetCombat(false);
    Tick();                       // 脱战 tick：清零
    CHECK_EQ(Current(), 0);
    CHECK(krfake::EventCount("KR_RageChanged") > changesAtStart);

    Hit(Enemy(0));
    CHECK_EQ(Current(), 0);       // 脱战后命中不涨
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());
}

// 6. 战斗时钟本身不再授予：零命中时经历 >1s 的心跳也不涨（1-2 处真实等待之一）。
void CaseCombatTimeAloneNeverAwards()
{
    Start(FullSettings());
    SetCombat(true);
    Tick();                       // 首 tick 仅状态

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    Tick();
    CHECK_EQ(Current(), 0);       // 仅心跳、无命中 → 不涨
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    Tick();
    CHECK_EQ(Current(), 0);
    CHECK_EQ(krfake::Casts().size(), 0);
    CHECK(!Snap().ready);
}

// 7. 脱战清零的同时取消 pending、撤 ready；脱战后满怒请求不得穿过（不得施法）。
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

// 8. 旧 PEX 五个入口、死亡事件与旧伤害通道都不得计分；只有命中事件生效。
void CaseLegacyEntriesDoNotScore()
{
    Start(ScoreSettings());
    SetCombat(true);
    CHECK_EQ(Current(), 0);

    WeaponHit(Enemy(0), kFlagPower);
    MagicHit(Enemy(1), S().successCasts[1]);
    ProjectileHit(Enemy(2), S().successCasts[1]);
    IncomingHit(Enemy(3), S().successCasts[1], false);
    Kill(Enemy(4));
    PlayerDamage(Enemy(5));
    PlayerDamage(nullptr);
    PlayerDamage(Enemy(6), 0.0f);
    PlayerDamage(Enemy(6), -1.0f);
    CHECK_EQ(Current(), 0);

    RE::TESDeathEvent death;
    death.actorDying = RE::NiPointer<RE::TESObjectREFR>(Enemy(5));
    death.actorKiller = RE::NiPointer<RE::TESObjectREFR>(S().player);
    death.dead = true;
    krfake::FireDeath(death);
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    // 对照：命中通道仍活着
    Hit(Enemy(0));
    CHECK_EQ(Current(), 2);
    CHECK_EQ(krfake::Casts().size(), 0);
}

// 9. 目标过滤：自身/队友/被玩家或队友指挥/非敌对/非 Actor/null/事件时已死 均不计。
void CaseHitTargetFilters()
{
    Start(ScoreSettings());
    SetCombat(true);

    Hit(S().player);                          // 自身
    CHECK_EQ(Current(), 0);

    Enemy(1)->teammate = true;                // 队友
    Hit(Enemy(1));
    CHECK_EQ(Current(), 0);
    Enemy(1)->teammate = false;

    RE::Actor* mate = Enemy(2);               // 被队友直接指挥
    mate->teammate = true;
    Enemy(3)->commander = mate;
    Hit(Enemy(3));
    CHECK_EQ(Current(), 0);
    Enemy(3)->commander = nullptr;
    mate->teammate = false;

    Enemy(4)->commander = S().player;         // 被玩家直接指挥
    Hit(Enemy(4));
    CHECK_EQ(Current(), 0);
    Enemy(4)->commander = nullptr;

    Enemy(5)->hostile = false;                // 对玩家非敌对
    Hit(Enemy(5));
    CHECK_EQ(Current(), 0);
    Enemy(5)->hostile = true;

    Hit(S().nonActorRefr);                    // 非 Actor 目标
    CHECK_EQ(Current(), 0);

    FireHit(nullptr);                         // null 目标
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    Enemy(6)->dead = true;                    // 事件到达时已死
    Hit(Enemy(6));
    CHECK_EQ(Current(), 0);
    Enemy(6)->dead = false;

    // 控制组：一个合规目标
    Hit(Enemy(0));
    CHECK_EQ(Current(), 2);
}

// 10. 格挡不计；power/sneak/bash 固定同值；source=0 允许（不查来源白名单）。
void CaseBlockedAndAttackFlags()
{
    Start(ScoreSettings());
    SetCombat(true);

    Hit(Enemy(0), 0, kFlagBlocked);
    CHECK_EQ(Current(), 0);
    Hit(Enemy(0), 0, kFlagPower | kFlagBlocked);   // 格挡优先于攻击类别
    CHECK_EQ(Current(), 0);

    Hit(Enemy(0), 0, kFlagPower);
    CHECK_EQ(Current(), 2);
    Hit(Enemy(0), 0, kFlagSneak);
    CHECK_EQ(Current(), 4);
    Hit(Enemy(0), 0, kFlagBash);
    CHECK_EQ(Current(), 6);
    Hit(Enemy(0), 0, kFlagPower | kFlagSneak | kFlagBash);
    CHECK_EQ(Current(), 8);

    Hit(Enemy(0), 0, 0);                      // source 0 也计
    CHECK_EQ(Current(), 10);
    Hit(Enemy(0), S().axe->GetFormID(), 0);   // 任意来源都计
    CHECK_EQ(Current(), 12);

    CHECK_EQ(krfake::Casts().size(), 0);
}

// 11. 玩家来源：cause 必须等于玩家（NPC 或空 cause 都不计）。
void CaseNpcCauseRejected()
{
    Start(ScoreSettings());
    SetCombat(true);

    FireHit(Enemy(0), 0, 0, Enemy(4));         // 另一 NPC 是攻击者 → 拒绝
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    {
        RE::TESHitEvent event;                 // cause 为空 → 拒绝
        event.target = RE::NiPointer<RE::TESObjectREFR>(Enemy(0));
        event.flags = RE::TESHitEvent::Flag::kNone;
        krfake::FireHit(event);
    }
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    Hit(Enemy(0));                             // 控制组
    CHECK_EQ(Current(), 2);
}

// 12. 同帧多目标/同源多段无合并；>5 次/秒仍逐次累计。
void CaseSameFrameMultipleTargetsAndSegments()
{
    Start(ScoreSettings());
    SetCombat(true);

    FireHit(Enemy(0));
    FireHit(Enemy(0));
    FireHit(Enemy(1));
    FireHit(Enemy(0));                        // 同一帧 4 次
    CHECK_EQ(Current(), 0);                   // 未 drain 前不计分
    krfake::DrainAll();
    CHECK_EQ(Current(), 8);                   // 4 次各 +2，不合并

    for (int i = 0; i < 6; ++i) {             // 同一帧同源同目标 6 段（>5 次/秒）
        FireHit(Enemy(0));
    }
    krfake::DrainAll();
    CHECK_EQ(Current(), 20);

    CHECK_EQ(krfake::Casts().size(), 0);
}

// 13. 事件字段在入队前复制：局部事件销毁后再 drain 仍应计分（不得保留事件指针）。
void CaseEventFieldsCopiedBeforeLocalDestroy()
{
    Start(ScoreSettings());
    SetCombat(true);

    {
        RE::TESHitEvent event;
        event.target = RE::NiPointer<RE::TESObjectREFR>(Enemy(0));
        event.cause = RE::NiPointer<RE::TESObjectREFR>(S().player);
        event.source = S().axe->GetFormID();
        event.projectile = 0;
        event.flags = RE::TESHitEvent::Flag::kPowerAttack;
        krfake::FireHit(event);
    }                                         // 局部事件在此销毁
    krfake::DrainAll();

    CHECK_EQ(Current(), 2);
}

// 14. sink 注册幂等：Initialize 多次仍只有一个 TESHitEvent sink。
void CaseHitSinkRegistrationIdempotent()
{
    CHECK_EQ(krfake::SinkList<RE::TESHitEvent>().size(), 1);
    kr::GameBridge::Get().Initialize();
    krfake::DrainAll();
    CHECK_EQ(krfake::SinkList<RE::TESHitEvent>().size(), 1);

    // 幂等后命中通道照常工作
    Start(ScoreSettings());
    SetCombat(true);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 2);
}

// 15. 句柄失效：事件时捕获的句柄在处理时无法解析 → 拒绝（不得用永久裸指针）。
void CaseHandleInvalidationRejectsQueuedHit()
{
    Start(ScoreSettings());
    SetCombat(true);

    RE::Actor* target = Enemy(0);
    FireHit(target);                          // 入队时捕获句柄
    krfake::DropMapping(target->GetFormID()); // 模拟对象失效
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    krfake::DrainAll();
    CHECK_EQ(Current(), 0);
}

// 16. 事件到达时目标活着、入队后死亡但敌对仍有效 → 处理时不得再按 IsDead 拒绝。
void CaseTargetDeathAfterEnqueueStillScores()
{
    Start(ScoreSettings());
    SetCombat(true);

    FireHit(Enemy(0));
    Enemy(0)->dead = true;
    krfake::DrainAll();
    CHECK_EQ(Current(), 2);

    Enemy(0)->dead = false;
}

// 17. 入队后玩家脱战或死亡：处理时重新确认玩家状态，拒绝且不授予。
void CasePlayerStateChangeRejectsQueuedHit()
{
    Start(ScoreSettings());
    SetCombat(true);

    FireHit(Enemy(0));
    SetCombat(false);                         // 入队后脱战
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    SetCombat(true);
    FireHit(Enemy(1));
    S().player->dead = true;                  // 入队后死亡
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    S().player->dead = false;
}

// 18. 暂停/loading：到达前与处理前都要拒绝，恢复后不补发。
void CaseSuspendedArrivalAndProcessingRejected()
{
    auto* ui = RE::UI::GetSingleton();
    Start(ScoreSettings());
    SetCombat(true);

    ui->paused = true;                        // 到达前暂停
    FireHit(Enemy(0));
    ui->paused = false;
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    FireHit(Enemy(1));                        // 处理前暂停
    ui->paused = true;
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);
    ui->paused = false;

    ui->loadingMenu = true;                   // 到达前 Loading
    FireHit(Enemy(2));
    ui->loadingMenu = false;
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    Hit(Enemy(3));                            // 控制组
    CHECK_EQ(Current(), 2);
}

// 19. 尚未对账时到达的命中直接拒绝；之后 Reconcile + drain 也不补发。
void CaseUnreconciledArrivalNotReplayed()
{
    Start(ScoreSettings());
    SetCombat(true);
    const kr::SavedState saved = kr::GameBridge::Get().Save();

    kr::GameBridge::Get().Restore(saved);     // needsReconcile_ = true
    FireHit(Enemy(0));                        // 到达即拒绝
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);
    CHECK(!Snap().pending);
}

// 20. Restore/Revert 使旧代已入队命中失效（generation 隔离）。
void CaseStaleHitDroppedByRestore()
{
    Start(ScoreSettings());
    SetCombat(true);
    const kr::SavedState saved = kr::GameBridge::Get().Save();

    FireHit(Enemy(0));                        // 旧代任务，尚未执行
    kr::GameBridge::Get().Restore(saved);     // ++generation_
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);
}

void CaseStaleHitDroppedByRevert()
{
    Start(ScoreSettings());
    SetCombat(true);

    FireHit(Enemy(0));                        // 旧代任务，尚未执行
    kr::GameBridge::Get().Revert();           // ++generation_ 并清场
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);
    CHECK(!Snap().eligible);
}

// 21. 打满 + 同步 813（施法即时落效果）→ 只施放一次 814，确认后清零并结算
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

// 22. 打满 + 延迟 813（先施法后落效果/事件）→ 仍然一次施放并且只在确认后清零
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

// 23. 同帧排空后仍未确认 → 必须仍 pending、怒气仍满、不得报拒绝；0.5s 后刷新才回滚
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

// 24. SKSE 任务必须可显式 drain：RunFrame 只跑当帧已入队任务，DrainAll 有界排空
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

// 25. native 不得代为重复施放 817 / 844；成功施法严格等于 814 + 三个结算 payload
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

// 26. 变身期间命中照常积攒；变身结束后继续积攒并因满怒 ready
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

    Hit(Enemy(0));                      // 变身期间也计分
    CHECK_EQ(Current(), 2);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 4);
    CHECK(!Snap().ready);               // 变身中不得 ready

    krfake::ClearEffects(S().player);   // 813 结束
    Refresh();
    CHECK(!Snap().raging);
    CHECK(Snap().eligible);
    CHECK(Snap().ready);                // 变身结束后怒气仍满 → ready（无施法）
}

// 27. 旧会话入队的任务（读档后的 Configure + 遗留的 805 请求）在 Restore 之后必须整体失效
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
    CHECK(!Snap().eligible);
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 1);

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

// 28. 未确认的挂起请求不得跨存档消耗怒气
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

    // 读档后重新配置再触发，应该正常结算（先越过 0.25s 施法节流）
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

// 29. 三源移动键（方向键或 Keytrace 效果）与拒绝提示的可观测面
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

// 30. Revert 同样必须使旧代已入队任务失效（与 27 的 Restore 版对称）
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

// 31. 同一次读档里第二次 Reconcile 不得吞掉进行中的激活（needsReconcile_ 守卫）
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

    // 第二次 reconcile（同一次读档里的重复调用）必须整体跳过
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

// 32. 死亡立即结束战斗：清零、撤 ready、此后不涨，命中也被拒绝。
void CaseDeathStopsAndClears()
{
    Start(ScoreSettings());
    SetCombat(true);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 2);

    S().player->dead = true;
    Tick();
    CHECK_EQ(Current(), 0);           // 死亡视为脱战 → 清零
    CHECK(!Snap().eligible);

    Hit(Enemy(0));
    CHECK_EQ(Current(), 0);           // 死亡期间命中不涨
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());
}

// 33. 战斗中存档保留怒气、脱战后存档为 0；读档后命中继续累计。
void CaseSaveRestoresCombatRageOnly()
{
    Start(ScoreSettings());
    SetCombat(true);
    Hit(Enemy(0));
    Hit(Enemy(0));
    CHECK_EQ(Current(), 4);

    const kr::SavedState inCombatSave = kr::GameBridge::Get().Save();
    CHECK_EQ(inCombatSave.current, 4);

    SetCombat(false);
    Tick();
    CHECK_EQ(Current(), 0);
    const kr::SavedState outOfCombatSave = kr::GameBridge::Get().Save();
    CHECK_EQ(outOfCombatSave.current, 0);

    // 恢复战斗存档（此时在战斗中）：怒气保留并继续按命中推进
    SetCombat(true);
    kr::GameBridge::Get().Restore(inCombatSave);
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();
    CHECK_EQ(Current(), 4);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 6);

    // 恢复脱战存档：清零
    SetCombat(false);
    kr::GameBridge::Get().Restore(outOfCombatSave);
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();
    Tick();
    CHECK_EQ(Current(), 0);
}

// 34. Loading/缺玩家期间保留怒气；console 暂停阻止激活；缺玩家不授予。
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

// 35. Loading / 全部 UI 暂停路径 / 主循环 freezeTime、gameActive：到达与处理两个时点都必须
//     拒绝，恢复后不得补发；UI / Main 单例缺失时保守拒绝。
void CaseAllPausePathsRejectAtBothPoints()
{
    Start(ScoreSettings());
    SetCombat(true);
    RE::UI* ui = RE::UI::GetSingleton();
    RE::Main* main = RE::Main::GetSingleton();

    using UIFlag = bool RE::UI::*;
    const UIFlag uiFlags[] = {&RE::UI::paused, &RE::UI::itemMenu, &RE::UI::appMenu, &RE::UI::modalMenu,
        &RE::UI::loadingMenu, &RE::UI::console, &RE::UI::dialogueMenu};
    for (UIFlag flag : uiFlags) {
        ui->*flag = true;                 // 到达前暂停
        FireHit(Enemy(0));
        ui->*flag = false;
        krfake::DrainAll();
        CHECK_EQ(Current(), 0);

        FireHit(Enemy(0));                // 处理前暂停
        ui->*flag = true;
        krfake::DrainAll();
        ui->*flag = false;
        CHECK_EQ(Current(), 0);
    }

    main->freezeTime = true;              // freezeTime 到达前
    FireHit(Enemy(0));
    main->freezeTime = false;
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    FireHit(Enemy(0));                    // freezeTime 处理前
    main->freezeTime = true;
    krfake::DrainAll();
    main->freezeTime = false;
    CHECK_EQ(Current(), 0);

    main->gameActive = false;             // gameActive 到达前
    FireHit(Enemy(0));
    main->gameActive = true;
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    FireHit(Enemy(0));                    // gameActive 处理前
    main->gameActive = false;
    krfake::DrainAll();
    main->gameActive = true;
    CHECK_EQ(Current(), 0);

    krfake::UIUnavailable() = true;       // UI 单例缺失：到达前
    FireHit(Enemy(0));
    krfake::UIUnavailable() = false;
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    FireHit(Enemy(0));                    // UI 单例缺失：处理前
    krfake::UIUnavailable() = true;
    krfake::DrainAll();
    krfake::UIUnavailable() = false;
    CHECK_EQ(Current(), 0);

    krfake::MainUnavailable() = true;     // Main 单例缺失：到达前
    FireHit(Enemy(0));
    krfake::MainUnavailable() = false;
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    FireHit(Enemy(0));                    // Main 单例缺失：处理前
    krfake::MainUnavailable() = true;
    krfake::DrainAll();
    krfake::MainUnavailable() = false;
    CHECK_EQ(Current(), 0);

    Hit(Enemy(0));                        // 控制组
    CHECK_EQ(Current(), 2);
}

// 36. 菜单打开又关闭后才 drain：到达时捕获的旧命中（hitContext_ 已变）不得补发；此后新命中放行。
void CaseMenuRoundTripStalesQueuedHit()
{
    Start(ScoreSettings());
    SetCombat(true);

    FireHit(Enemy(0));                    // 旧上下文命中，尚未执行
    FireMenu("InventoryMenu", true);      // 打开递增 hitContext_
    FireMenu("InventoryMenu", false);     // 关闭再递增
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);               // 旧命中不得在恢复后补发

    FireHit(Enemy(0));                    // 新上下文命中
    krfake::DrainAll();
    CHECK_EQ(Current(), 2);
}

// 37. 入队后目标变为队友 / 被玩家或队友指挥 / 非敌对：处理时重新判定并拒绝。
void CaseEnqueuedTargetAllianceChangeRejected()
{
    Start(ScoreSettings());
    SetCombat(true);

    RE::Actor* teammate = Enemy(0);
    FireHit(teammate);
    teammate->teammate = true;            // 入队后变队友
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);
    teammate->teammate = false;

    RE::Actor* commanded = Enemy(1);
    FireHit(commanded);
    commanded->commander = S().player;    // 入队后被玩家指挥
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);
    commanded->commander = nullptr;

    RE::Actor* mate = Enemy(3);
    mate->teammate = true;
    RE::Actor* commandedByMate = Enemy(2);
    FireHit(commandedByMate);
    commandedByMate->commander = mate;    // 入队后被队友指挥
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);
    commandedByMate->commander = nullptr;
    mate->teammate = false;

    RE::Actor* neutral = Enemy(4);
    FireHit(neutral);
    neutral->hostile = false;             // 入队后变为非敌对
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);
    neutral->hostile = true;

    Hit(Enemy(0));                        // 控制组
    CHECK_EQ(Current(), 2);
}

// 38. 事件入队后改 target/cause/flags/source/projectile，并在事件销毁后再 drain：计分与
//     日志字段都必须来自接收时的拷贝，不依赖已销毁局部内存。
void CaseMutatingEventAfterEnqueueUsesReceiptCopy()
{
    Start(ScoreSettings());
    SetCombat(true);
    krfake::Logs().clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // 越过日志限流窗口

    {
        RE::TESHitEvent event;
        event.target = RE::NiPointer<RE::TESObjectREFR>(Enemy(0));
        event.cause = RE::NiPointer<RE::TESObjectREFR>(S().player);
        event.source = 0x1234;
        event.projectile = 0x5678;
        event.flags = RE::TESHitEvent::Flag::kPowerAttack;
        krfake::FireHit(event);

        event.target = RE::NiPointer<RE::TESObjectREFR>(S().player);   // 若晚读 → self 拒绝
        event.cause = RE::NiPointer<RE::TESObjectREFR>(Enemy(4));      // 若晚读 → not_player
        event.source = 0xABCD;
        event.projectile = 0xEF01;
        event.flags = RE::TESHitEvent::Flag::kHitBlocked;              // 若晚读 → blocked
    }                                                                  // 事件在此销毁
    krfake::DrainAll();

    CHECK_EQ(Current(), 2);               // 基于接收时拷贝
    const krfake::LogRecord* awarded = FindHitLog("awarded");
    CHECK(awarded != nullptr);
    if (awarded) {
        CHECK(awarded->args[6] == std::to_string(0x1234));             // source 为接收值
        CHECK(awarded->args[7] == std::to_string(0x5678));             // projectile 为接收值
        CHECK(awarded->args[8] == std::to_string(static_cast<int>(RE::TESHitEvent::Flag::kPowerAttack)));
    }
}

// 39. 日志类别独立且按 reason 每秒限流：同一秒 >5 次命中全部计分，received/awarded/became_full
//     各自独立输出，限流不影响累计。
void CaseLogCategoriesRateLimitedScoringUnaffected()
{
    Start(ScoreSettings());
    SetCombat(true);
    krfake::Logs().clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    for (int i = 0; i < 6; ++i) {         // 同一秒 6 次命中（>5）
        FireHit(Enemy(0));
    }
    krfake::DrainAll();
    CHECK_EQ(Current(), 12);              // 限流只作用于日志，6 次全部 +2
    CHECK_EQ(HitLogCount("received"), 1); // received 自成一类，每秒一条
    CHECK_EQ(HitLogCount("awarded"), 1);  // awarded 独立输出，不受 received 影响

    Start(FullSettings());                // 满怒边沿自成一类
    SetCombat(true);
    krfake::Logs().clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Hit(Enemy(0));
    Hit(Enemy(0));                        // 第二次命中打满
    CHECK_EQ(Current(), 4);
    CHECK_EQ(HitLogCount("became_full"), 1);
    CHECK(HitLogCount("awarded") >= 1);
}

// 40. 无装备：仍按命中积攒，但 eligible 为假 → 永远不 ready，也不挂 ready marker。
void CaseUnequippedAccumulatesButNeverReady()
{
    Start(FullSettings());                // maximum = 4
    S().player->equippedRight = nullptr;
    S().player->equippedLeft = nullptr;

    SetCombat(true);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 2);
    Hit(Enemy(0));
    CHECK_EQ(Current(), 4);               // 无装备无积攒门槛

    CHECK(!Snap().eligible);
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());

    Hit(Enemy(0));                        // 封顶且仍不 ready
    CHECK_EQ(Current(), 4);
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());
}

// 41. pending 期间增大 maximum 留出空间：命中仍 +2，pending 不被 Configure 取消，最终仍能结算。
void CasePendingSurvivesMaximumIncrease()
{
    Start(FullSettings());                // maximum = 4
    FillRage();
    CHECK(Snap().ready);

    Request805();                         // 施放 814 → pending（效果未落）
    krfake::DrainAll();
    CHECK(Snap().pending);
    CHECK_EQ(Current(), 4);

    kr::RageSettings bigger = FullSettings();
    bigger.maximum = 100;
    kr::GameBridge::Get().Configure(bigger, kDeniedInterval);
    krfake::DrainAll();
    CHECK(Snap().pending);                // Configure 不得取消 pending
    CHECK_EQ(Snap().maximum, 100);

    Hit(Enemy(0));                        // 有空间 → 仍 +2
    CHECK_EQ(Current(), 6);
    CHECK(Snap().pending);

    krfake::ApplyEffect(S().player, S().rageEffect);
    FireEffectEvent(S().player, S().rageEffect);
    krfake::DrainAll();
    kr::GameBridge::Get().RefreshEquipmentState();
    krfake::DrainAll();
    CHECK(!Snap().pending);
    CHECK(Snap().raging);
    CHECK_EQ(Current(), 0);
    CHECK_EQ(SuccessCastTotal(), 3);
}

// 42. 受击与到达时状态：指向玩家的受击无分；到达时非战斗随后进战斗不补发；到达时目标已死
//     随后复活也不补发。
void CaseIncomingAndArrivalStateNoReplay()
{
    Start(ScoreSettings());
    SetCombat(true);

    FireHit(S().player, 0, 0, Enemy(4));  // 目标为玩家、攻击者为 NPC → 拒绝
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    FireHit(S().player);                  // 目标与 cause 同为玩家 → self 拒绝
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);

    SetCombat(false);
    FireHit(Enemy(0));                    // 到达时非战斗 → 拒绝且不入队
    SetCombat(true);
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);               // 进战斗后不得补发

    Enemy(1)->dead = true;
    FireHit(Enemy(1));                    // 到达时目标已死 → 拒绝
    Enemy(1)->dead = false;
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);               // 复活后不得补发

    Hit(Enemy(0));                        // 控制组
    CHECK_EQ(Current(), 2);
}

// 43. 旧命中在 Restore + 已完成 reconcile（needsReconcile_ 已清）后执行仍被 generation 挡，
//     避免单靠 needsReconcile_ 误过测试。
void CaseStaleHitAfterRestoreReconcileBlockedByGeneration()
{
    Start(ScoreSettings());
    SetCombat(true);
    const kr::SavedState saved = kr::GameBridge::Get().Save();

    FireHit(Enemy(0));                    // 旧代命中入队
    auto oldHit = std::move(krfake::Tasks().front());
    krfake::Tasks().clear();
    kr::GameBridge::Get().Restore(saved);
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();
    oldHit();                            // 对账完成后执行旧任务，单靠 needsReconcile_ 已挡不住
    CHECK_EQ(Current(), 0);

    Hit(Enemy(0));                        // 新代命中照常
    CHECK_EQ(Current(), 2);
}

// 44. Revert 版的对称用例：reconcile 收口后旧命中仍必须被 generation 整体丢弃。
void CaseStaleHitAfterRevertReconcileBlockedByGeneration()
{
    Start(ScoreSettings());
    SetCombat(true);

    FireHit(Enemy(0));                    // 旧代命中入队
    auto oldHit = std::move(krfake::Tasks().front());
    krfake::Tasks().clear();
    kr::GameBridge::Get().Revert();
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();
    oldHit();
    CHECK_EQ(Current(), 0);

    Hit(Enemy(0));
    CHECK_EQ(Current(), 2);
}

// 45. 菜单开关与战斗事件离开再进入：命中 context 隔离的正反例。
void CaseMenuAndCombatContextIsolation()
{
    Start(ScoreSettings());
    SetCombat(true);

    FireHit(Enemy(0));                    // 旧上下文命中
    FireMenu("InventoryMenu", true);
    FireMenu("InventoryMenu", false);
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);               // 菜单往返后旧命中不得通过

    Hit(Enemy(0));                        // 新上下文命中放行
    CHECK_EQ(Current(), 2);

    FireHit(Enemy(0));                    // 入队旧上下文命中
    SetCombat(false);
    FireCombat(S().player);               // 离开战斗递增 hitContext_
    SetCombat(true);
    FireCombat(S().player);               // 再进入不递增
    krfake::DrainAll();
    CHECK_EQ(Current(), 2);               // 若 context 守卫缺失，旧命中会在重新战斗中被计分

    Hit(Enemy(0));                        // 新上下文命中放行
    CHECK_EQ(Current(), 4);
}

// 46. null 事件：四个进程事件入口都必须返回 kContinue 且无副作用（不递增 context、不入队、不计分、不写命中日志）。
void CaseNullEventsContinueWithoutSideEffects()
{
    Start(ScoreSettings());
    SetCombat(true);
    kr::GameBridge& bridge = kr::GameBridge::Get();
    krfake::Logs().clear();

    CHECK(bridge.ProcessEvent(static_cast<const RE::TESMagicEffectApplyEvent*>(nullptr), nullptr)
        == RE::BSEventNotifyControl::kContinue);
    CHECK(bridge.ProcessEvent(static_cast<const RE::TESCombatEvent*>(nullptr), nullptr)
        == RE::BSEventNotifyControl::kContinue);
    CHECK(bridge.ProcessEvent(static_cast<const RE::TESHitEvent*>(nullptr), nullptr)
        == RE::BSEventNotifyControl::kContinue);
    CHECK(bridge.ProcessEvent(static_cast<const RE::MenuOpenCloseEvent*>(nullptr), nullptr)
        == RE::BSEventNotifyControl::kContinue);

    CHECK(krfake::Tasks().empty());           // 未入队
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);                   // 不计分
    CHECK_EQ(HitLogCount("received"), 0);     // 未产生命中日志

    // context 未被 null 事件改变：之后入队的命中照常处理
    FireHit(Enemy(0));
    bridge.ProcessEvent(static_cast<const RE::MenuOpenCloseEvent*>(nullptr), nullptr);
    bridge.ProcessEvent(static_cast<const RE::TESHitEvent*>(nullptr), nullptr);
    krfake::DrainAll();
    CHECK_EQ(Current(), 2);
}

struct TestCase {
    const char* name;
    void (*fn)();
};

const TestCase kCases[] = {
    {"hits_without_configure_accumulate", &CaseHitsWithoutConfigure},
    {"hit_gain_fixed_ignores_legacy_settings", &CaseHitGainFixedIgnoresLegacySettings},
    {"maximum_cap_and_became_full_once", &CaseMaximumCapAndBecameFullOnce},
    {"odd_maximum_partial_and_full_edge", &CaseOddMaximumPartialAndFullEdge},
    {"no_gain_out_of_combat_and_clear_on_leave", &CaseNoGainOutOfCombatAndClearOnLeave},
    {"combat_time_alone_never_awards", &CaseCombatTimeAloneNeverAwards},
    {"leave_combat_cancels_pending_and_blocks_activation", &CaseLeaveCombatCancelsPendingAndBlocksActivation},
    {"legacy_entries_do_not_score", &CaseLegacyEntriesDoNotScore},
    {"hit_target_filters", &CaseHitTargetFilters},
    {"blocked_and_attack_flags", &CaseBlockedAndAttackFlags},
    {"npc_cause_rejected", &CaseNpcCauseRejected},
    {"same_frame_multiple_targets_and_segments", &CaseSameFrameMultipleTargetsAndSegments},
    {"event_fields_copied_before_local_destroy", &CaseEventFieldsCopiedBeforeLocalDestroy},
    {"hit_sink_registration_idempotent", &CaseHitSinkRegistrationIdempotent},
    {"handle_invalidation_rejects_queued_hit", &CaseHandleInvalidationRejectsQueuedHit},
    {"target_death_after_enqueue_still_scores", &CaseTargetDeathAfterEnqueueStillScores},
    {"player_state_change_rejects_queued_hit", &CasePlayerStateChangeRejectsQueuedHit},
    {"suspended_arrival_and_processing_rejected", &CaseSuspendedArrivalAndProcessingRejected},
    {"unreconciled_arrival_not_replayed_after_reconcile", &CaseUnreconciledArrivalNotReplayed},
    {"stale_hit_dropped_by_restore", &CaseStaleHitDroppedByRestore},
    {"stale_hit_dropped_by_revert", &CaseStaleHitDroppedByRevert},
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
    {"all_pause_paths_reject_at_both_points", &CaseAllPausePathsRejectAtBothPoints},
    {"menu_round_trip_stales_queued_hit", &CaseMenuRoundTripStalesQueuedHit},
    {"enqueued_target_alliance_change_rejected", &CaseEnqueuedTargetAllianceChangeRejected},
    {"mutating_event_after_enqueue_uses_receipt_copy", &CaseMutatingEventAfterEnqueueUsesReceiptCopy},
    {"log_categories_rate_limited_scoring_unaffected", &CaseLogCategoriesRateLimitedScoringUnaffected},
    {"unequipped_accumulates_but_never_ready", &CaseUnequippedAccumulatesButNeverReady},
    {"pending_survives_maximum_increase", &CasePendingSurvivesMaximumIncrease},
    {"incoming_and_arrival_state_no_replay", &CaseIncomingAndArrivalStateNoReplay},
    {"stale_hit_after_restore_reconcile_blocked_by_generation", &CaseStaleHitAfterRestoreReconcileBlockedByGeneration},
    {"stale_hit_after_revert_reconcile_blocked_by_generation", &CaseStaleHitAfterRevertReconcileBlockedByGeneration},
    {"menu_and_combat_context_isolation", &CaseMenuAndCombatContextIsolation},
    {"null_events_continue_without_side_effects", &CaseNullEventsContinueWithoutSideEffects},
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
