// ===========================================================================
// KratosRagePatch —— src/GameBridge.cpp 离线集成回归（mock harness 驱动）
//
// 被测对象是 tools/test_bridge.py 原样复制到 build/bridge-tests/source 的
// src/GameBridge.cpp + src/RageState.cpp；伪游戏 API 见 tests/bridge/fake/Fake.h。
//
// 设计原则：
//  * 断言只读真实 GameBridge 产生的可观测副作用（怒气快照、施法日志、spell marker、
//    mod callback 事件、SKSE 任务队列），fake 不自证、不复写桥接规则。
//  * 计分的唯一入口是「玩家造成的实际生命伤害」：SubmitPlayerDamage(targetID, baseID,
//    healthLost) 每事件固定 +2、全体目标共享 1 秒 +10 预算并受 maximum 封顶；与配置的
//    normalHit/powerHit/...、maxGainPerSecond、敌意、装备、死亡、pending/raging/configured
//    均无关（只拒绝非有限/非正 healthLost、targetID=0、targetID=玩家）。旧
//    SubmitWeaponHit/MagicHit/ProjectileHit/IncomingHit/Kill 与死亡事件均已无操作；eligible
//    只由配置 + 原装备决定，仅限制原释放，不影响积攒。
//  * 用例按「能抓旧时序缺陷」写：同帧排空后仍必须 pending 且不得回滚；
//    陈旧请求在 Restore 后不得执行。这两种断言在
//    「同帧三次重试后立刻回滚」和「Queue 无 generation epoch」的旧实现上会 FAIL。
//    tools/test_bridge.py --mutant 可实测这一区分能力。
// ===========================================================================

#include "Fake.h"
#include "GameBridge.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
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
#define CHECK_NEAR(actual, expected, tol) \
    ::CheckTrue(std::llabs((actual) - (expected)) <= (tol), #actual " ~= " #expected, __FILE__, __LINE__)

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
    RE::SpellItem* magicWhitelist{};      // God of War.esp 0x841
    RE::SpellItem* projectileWhitelist{}; // God of War.esp 0x82B
    RE::SpellItem* offWhitelist{};        // God of War.esp 0x818（不在审计白名单）
    RE::SpellItem* incomingSpell{};       // 命中条件的来袭法术（kFireAndForget + kAimed + duration 0）
    RE::SpellItem* incomingSelfSpell{};   // kSelf → 自施光环，不算直接来袭
    RE::SpellItem* incomingBenignSpell{}; // 非 hostile/detrimental → 不算直接来袭
    RE::SpellItem* incomingAuraSpell{};   // kConcentration → 连续施法，不算直接来袭
    RE::SpellItem* incomingDotSpell{};         // duration=30 且无 NoDuration → 不算直接来袭
    RE::SpellItem* incomingNoDurationSpell{};  // duration=30 但 MGEF 有 NoDuration → 仍算
    std::vector<RE::Actor*> enemies;
    RE::Actor* neutral{};                 // 既非敌意也非友军：伤害通道不做敌意判定，应照常计分
    RE::Actor* teammate{};
    RE::Actor* summon{};
    RE::Actor* corpse{};
};

Scenario& S()
{
    static Scenario scenario;
    return scenario;
}

RE::EffectSetting* MakeEffect(int plugin, std::uint32_t localID, bool hostile, bool detrimental)
{
    auto* effect = krfake::AddEffect(plugin, localID);
    using Flag = RE::EffectSetting::EffectSettingData::Flag;
    if (hostile) {
        effect->data.flags.set(Flag::kHostile);
    }
    if (detrimental) {
        effect->data.flags.set(Flag::kDetrimental);
    }
    effect->data.primaryAV = RE::ActorValue::kHealth;
    return effect;
}

void AddSpellEffect(RE::SpellItem* spell, RE::EffectSetting* effect, std::uint32_t duration = 0)
{
    // 一次性世界搭建里的小对象，进程内不回收（测试结束即退出）。
    auto* item = new RE::Effect();
    item->baseEffect = effect;
    item->effectItem.duration = duration;
    spell->effects.push_back(item);
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

    // 审计白名单：magic 通道 + projectile 通道（0x841 同时是结算 payload，沿用同一实例）
    for (std::uint32_t id : {0x81Fu, 0x845u, 0x84Eu, 0x851u, 0x853u, 0x859u, 0x85Au,
             0x85Bu, 0x85Cu, 0x85Du, 0x85Fu, 0x862u, 0x867u, 0x86Du}) {
        krfake::AddSpell(gow, id);
    }
    s.magicWhitelist = s.successCasts[1];
    for (std::uint32_t id : {0x840u, 0x842u, 0x86Eu}) {
        krfake::AddSpell(gow, id);
    }
    s.projectileWhitelist = krfake::AddSpell(gow, 0x82B);

    // native 不得重复施放的原版法术 + 未列入白名单的法术
    s.nativeReapply[0] = krfake::AddSpell(gow, 0x817);
    s.nativeReapply[1] = krfake::AddSpell(gow, 0x844);
    s.offWhitelist = krfake::AddSpell(gow, 0x818);

    // 来袭法术（IsDirectIncomingSpell 判定）
    s.incomingSpell = krfake::AddSpell(gow, 0x8A0);
    s.incomingSpell->castingType = RE::MagicSystem::CastingType::kFireAndForget;
    s.incomingSpell->delivery = RE::MagicSystem::Delivery::kAimed;
    AddSpellEffect(s.incomingSpell, MakeEffect(gow, 0x8A1, true, true));

    s.incomingSelfSpell = krfake::AddSpell(gow, 0x8A2);
    s.incomingSelfSpell->castingType = RE::MagicSystem::CastingType::kFireAndForget;
    s.incomingSelfSpell->delivery = RE::MagicSystem::Delivery::kSelf;
    AddSpellEffect(s.incomingSelfSpell, MakeEffect(gow, 0x8A3, true, true));

    s.incomingBenignSpell = krfake::AddSpell(gow, 0x8A4);
    s.incomingBenignSpell->castingType = RE::MagicSystem::CastingType::kFireAndForget;
    s.incomingBenignSpell->delivery = RE::MagicSystem::Delivery::kAimed;
    AddSpellEffect(s.incomingBenignSpell, MakeEffect(gow, 0x8A5, false, false));

    // 持续施法（kConcentration，如火焰/治疗类通道法术）不属于当前 kFireAndForget 直接命中边界
    s.incomingAuraSpell = krfake::AddSpell(gow, 0x8A6);
    s.incomingAuraSpell->castingType = RE::MagicSystem::CastingType::kConcentration;
    s.incomingAuraSpell->delivery = RE::MagicSystem::Delivery::kAimed;
    AddSpellEffect(s.incomingAuraSpell, MakeEffect(gow, 0x8A7, true, true));

    // 持续性 DoT 首次命中：duration > 0 且 MGEF 无 NoDuration → GetDuration() != 0 → 不算直接来袭
    s.incomingDotSpell = krfake::AddSpell(gow, 0x8A8);
    s.incomingDotSpell->castingType = RE::MagicSystem::CastingType::kFireAndForget;
    s.incomingDotSpell->delivery = RE::MagicSystem::Delivery::kAimed;
    AddSpellEffect(s.incomingDotSpell, MakeEffect(gow, 0x8A9, true, true), 30);

    // duration > 0 但 MGEF 带 NoDuration → Effect::GetDuration() 归零 → 仍算即时直接命中
    s.incomingNoDurationSpell = krfake::AddSpell(gow, 0x8AA);
    s.incomingNoDurationSpell->castingType = RE::MagicSystem::CastingType::kFireAndForget;
    s.incomingNoDurationSpell->delivery = RE::MagicSystem::Delivery::kAimed;
    auto* noDurationEffect = MakeEffect(gow, 0x8AB, true, true);
    noDurationEffect->data.flags.set(RE::EffectSetting::EffectSettingData::Flag::kNoDuration);
    AddSpellEffect(s.incomingNoDurationSpell, noDurationEffect, 30);

    // 玩家 / 敌群 / 友军 / 召唤 / 死体
    s.player = krfake::AddPlayer(0x00000014);
    for (int i = 0; i < 8; ++i) {
        s.enemies.push_back(krfake::AddActor(static_cast<RE::FormID>(0x00010000 + i)));
    }
    s.neutral = krfake::AddActor(0x00020003);
    s.teammate = krfake::AddActor(0x00020000);
    s.summon = krfake::AddActor(0x00020001);
    s.corpse = krfake::AddActor(0x00020002);

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
    settings.normalHit = 2;
    settings.powerHit = 4;
    settings.specialHit = 3;
    settings.receivedHit = 1;
    settings.kill = 5;
    settings.maxGainPerSecond = 1000;
    return settings;
}

kr::RageSettings FullSettings()
{
    kr::RageSettings settings;
    settings.maximum = 10;
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
void Start(const kr::RageSettings& settings)
{
    krfake::ResetScenario();
    auto& bridge = kr::GameBridge::Get();
    bridge.Revert();
    krfake::DrainAll();
    EquipAxe();
    for (RE::Actor* enemy : S().enemies) {
        enemy->hostile = true;
    }
    S().teammate->teammate = true;
    S().summon->commander = S().player;
    S().corpse->hostile = true;
    bridge.Configure(settings, kDeniedInterval);
    krfake::DrainAll();
}

kr::Snapshot Snap() { return kr::GameBridge::Get().GetSnapshot(); }
int Current() { return Snap().current; }
bool MarkerOn() { return S().player->HasSpell(S().readyAbility); }

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

// 实际生命伤害入口（0.1.1 起唯一计分通道）。baseID 只是 hook 带来的旁证，接口不据此做任何判定。
void PlayerDamage(RE::Actor* target, float healthLost = 5.0f)
{
    kr::GameBridge::Get().SubmitPlayerDamage(target ? target->GetFormID() : 0, 0, healthLost);
    krfake::DrainAll();
}
void PlayerDamageRaw(RE::FormID targetID, RE::FormID baseID, float healthLost)
{
    kr::GameBridge::Get().SubmitPlayerDamage(targetID, baseID, healthLost);
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

// 让怒气打满到 maximum=10（不触发施法）：伤害通道每事件固定 +2，5 次刚好用满 10/秒预算。
void FillRage()
{
    for (int i = 0; i < 5; ++i) {
        PlayerDamage(S().enemies[static_cast<std::size_t>(i)]);
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

// 1. 伤害通道与配置、装备无关：未 Configure、未装备武器也按实际扣血固定 +2；
//    未配置时 eligible/ready 仍为假，也不得挂 ready marker。
void CaseDamageScoresWithoutConfigure()
{
    krfake::ResetScenario();
    kr::GameBridge::Get().Revert();
    krfake::DrainAll();

    PlayerDamage(S().enemies[0]);
    CHECK_EQ(Current(), 2);
    CHECK(!Snap().eligible);
    CHECK(!Snap().ready);
    CHECK(!MarkerOn());
}

// 2. 伤害通道固定 +2/事件、+10/秒，与 normalHit/powerHit/... 及 maxGainPerSecond 无关；
//    配置上限（GetRageMaximum）仍然生效并对外可见。
void CaseDamageGainIsFixedAndMaximumApplies()
{
    kr::RageSettings settings = ScoreSettings();
    settings.maximum = 100;
    settings.normalHit = 7;
    settings.powerHit = 9;
    settings.specialHit = 11;
    settings.receivedHit = 13;
    settings.kill = 15;
    settings.maxGainPerSecond = 1;   // 若伤害通道仍读它，5 次累计只会有 1 分
    Start(settings);

    CHECK_EQ(Snap().maximum, 100);
    CHECK(Snap().eligible);

    PlayerDamage(S().enemies[0]);
    CHECK_EQ(Current(), 2);
    PlayerDamage(S().enemies[1]);
    CHECK_EQ(Current(), 4);
    PlayerDamage(S().enemies[2]);
    CHECK_EQ(Current(), 6);
    PlayerDamage(S().enemies[3]);
    CHECK_EQ(Current(), 8);
    PlayerDamage(S().enemies[4]);
    CHECK_EQ(Current(), 10);          // 每事件 +2，与 normalHit=7 无关

    PlayerDamage(S().enemies[5]);
    CHECK_EQ(Current(), 10);          // 10/秒 固定预算截断（不是 maxGainPerSecond=1）
    CHECK_EQ(Snap().maximum, 100);    // GetRageMaximum 仍来自配置
    CHECK_EQ(krfake::Casts().size(), 0);
}

// 3. 目标筛选只拒玩家与 0；中立/友军/召唤/死者一律允许。非法 healthLost（0/负/NaN/Inf）
//    必须拒绝且不消耗预算。
void CaseDamageTargetAndValueValidation()
{
    Start(ScoreSettings());
    CHECK_EQ(Current(), 0);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    PlayerDamage(S().neutral);         // 中立（非敌意）允许
    CHECK_EQ(Current(), 2);
    PlayerDamage(S().teammate);        // 友军允许
    CHECK_EQ(Current(), 4);
    PlayerDamage(S().summon);          // 召唤物允许
    CHECK_EQ(Current(), 6);
    S().corpse->dead = true;
    PlayerDamageRaw(S().corpse->GetFormID(), 0xBEEF, 5.0f);  // 已死/任意 baseID 都不拒绝
    CHECK_EQ(Current(), 8);

    // 非法输入：全部拒绝，且不得占用 1 秒预算
    PlayerDamage(S().player);              // targetID = 玩家自身
    CHECK_EQ(Current(), 8);
    PlayerDamageRaw(0, 0, 5.0f);           // targetID = 0
    CHECK_EQ(Current(), 8);
    PlayerDamage(S().enemies[0], 0.0f);    // 0 伤害
    CHECK_EQ(Current(), 8);
    PlayerDamage(S().enemies[0], -1.0f);   // 负伤害
    CHECK_EQ(Current(), 8);
    PlayerDamage(S().enemies[0], nan);     // NaN
    CHECK_EQ(Current(), 8);
    PlayerDamage(S().enemies[0], inf);     // +Inf
    CHECK_EQ(Current(), 8);
    PlayerDamage(S().enemies[0], -inf);    // -Inf
    CHECK_EQ(Current(), 8);

    PlayerDamage(S().enemies[0]);          // 预算未被动用 → 第 5 次仍 +2
    CHECK_EQ(Current(), 10);
    PlayerDamage(S().enemies[1]);          // 第 6 次：1 秒预算已满
    CHECK_EQ(Current(), 10);
    CHECK_EQ(krfake::Casts().size(), 0);
}

// 4. 旧 PEX 五个入口与死亡事件都不得计分（无操作 / sink 已移除），只有实际伤害通道生效。
void CaseLegacyEntriesDoNotScore()
{
    Start(ScoreSettings());
    CHECK_EQ(Current(), 0);

    WeaponHit(S().enemies[0]);
    MagicHit(S().enemies[1], S().magicWhitelist);
    ProjectileHit(S().enemies[2], S().projectileWhitelist);
    IncomingHit(S().enemies[3], S().incomingSpell, false);
    S().enemies[4]->dead = true;
    Kill(S().enemies[4]);
    CHECK_EQ(Current(), 0);

    // 旧 TESDeathEvent sink 已移除：killer = 玩家的死亡事件同样不得计分
    RE::TESDeathEvent death;
    death.actorDying = RE::NiPointer<RE::TESObjectREFR>(S().enemies[5]);
    death.actorKiller = RE::NiPointer<RE::TESObjectREFR>(S().player);
    death.dead = true;
    krfake::FireDeath(death);
    krfake::DrainAll();
    CHECK_EQ(Current(), 0);
    CHECK_EQ(krfake::Casts().size(), 0);

    // 对照：唯一计分入口仍活着
    PlayerDamage(S().enemies[0]);
    CHECK_EQ(Current(), 2);
}

// 5. 伤害预算：同一目标立刻 5 次损伤即满 10（无 0.1s 去重），第 6 次为 0；
//    预算跨目标共享；越过 1 秒窗口后恢复。
void CaseDamageBudgetSharedPerSecond()
{
    Start(ScoreSettings());   // maximum = 100，排除上限干扰
    CHECK_EQ(Current(), 0);

    for (int i = 0; i < 5; ++i) {
        PlayerDamage(S().enemies[0]);   // 同一目标连续 5 次，全部计分
    }
    CHECK_EQ(Current(), 10);

    PlayerDamage(S().enemies[0]);
    CHECK_EQ(Current(), 10);            // 第 6 次：1 秒预算已用满 → 0

    PlayerDamage(S().enemies[3]);       // 换目标不换预算 → 仍为 0
    CHECK_EQ(Current(), 10);

    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    PlayerDamage(S().enemies[0]);
    CHECK_EQ(Current(), 12);            // 窗口滚出后恢复 +2

    CHECK_EQ(krfake::Casts().size(), 0);
}

// 6. maximum 封顶且 KR_RageBecameFull 只发一次；封顶来自配置上限而不是 10/秒预算。
void CaseMaximumCapAndBecameFullOnce()
{
    Start(FullSettings());   // maximum = 10
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 0);

    for (int i = 0; i < 5; ++i) {
        PlayerDamage(S().enemies[static_cast<std::size_t>(i)]);
    }
    CHECK_EQ(Current(), 10);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 1);

    // 越过 1 秒预算窗口后再打，仍被 maximum 封顶：证明封顶不是预算造成的
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    PlayerDamage(S().enemies[0]);
    CHECK_EQ(Current(), 10);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 1);
    CHECK(Snap().ready);
}

// 7. pending / raging 期间伤害照常计分：挂起（通常已满）与变身中都要能继续攒。
//    pending 用提升上限制造「挂起但未满」的可观测空间；变身走正常确认提交。
void CaseDamageScoresDuringPendingAndRaging()
{
    Start(FullSettings());   // maximum = 10
    FillRage();
    CHECK_EQ(Current(), 10);
    CHECK(Snap().ready);

    std::this_thread::sleep_for(std::chrono::milliseconds(1050));  // 让伤害预算窗口滚出

    Request805();
    krfake::DrainAll();
    CHECK(Snap().pending);

    // 提高上限：Configure 不解除挂起，此刻 pending 且未满，可观测到挂起期间的加怒
    kr::RageSettings raised = FullSettings();
    raised.maximum = 100;
    kr::GameBridge::Get().Configure(raised, kDeniedInterval);
    krfake::DrainAll();
    CHECK(Snap().pending);
    CHECK_EQ(Current(), 10);

    PlayerDamage(S().enemies[0]);
    CHECK_EQ(Current(), 12);           // pending 期间仍计分

    // 813 落地 → commit：怒气清零、进入变身；10 预算不因 commit 重置
    krfake::ApplyEffect(S().player, S().rageEffect);
    Refresh();
    CHECK(Snap().raging);
    CHECK_EQ(Current(), 0);

    PlayerDamage(S().enemies[1]);
    CHECK_EQ(Current(), 2);            // 变身期间仍计分
}

// 8. 打满 + 同步 813（施法即时落效果）→ 只施放一次 814，确认后清零并结算
void CaseFullOneCastSyncConfirm()
{
    Start(FullSettings());
    FillRage();
    CHECK_EQ(Current(), 10);
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
    CHECK_EQ(Current(), 10);

    Request805();
    krfake::DrainAll();
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);
    CHECK_EQ(Snap().pending, true);    // 未确认前必须挂起
    CHECK_EQ(Current(), 10);
    CHECK_EQ(SuccessCastTotal(), 0);

    krfake::ApplyEffect(S().player, S().rageEffect);  // 效果延迟落下
    FireEffectEvent(S().player, S().rageEffect);      // 引擎 813 事件（未来实现的即时确认路径）
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
    CHECK_EQ(Current(), 10);

    Request805();
    const int ran = krfake::DrainAll();
    CHECK(!krfake::SpinFlag());
    CHECK(ran <= 8);
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);     // 成功施法只有 814
    CHECK_EQ(Snap().pending, true);                    // 同帧排空后必须仍 pending
    CHECK_EQ(Current(), 10);                           // 怒气不得被扣
    CHECK_EQ(Snap().raging, false);
    CHECK_EQ(SuccessCastTotal(), 0);
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 0);  // 不得在同帧内回滚并报拒绝
    CHECK(!Snap().ready);                              // pending 期间不得 ready
    CHECK(!MarkerOn());                                // ready marker 必须撤下，防止重复触发

    Refresh();                                          // deadline 未到 → 仍不得回滚
    CHECK_EQ(Snap().pending, true);
    CHECK_EQ(Current(), 10);
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    Refresh();                                          // 越过 0.5s deadline 才回滚

    CHECK_EQ(Snap().pending, false);
    CHECK_EQ(Current(), 10);                            // 失败保留怒气
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

// 13. false -> true 满怒事件恰好一次
void CaseBecameFullEventOnce()
{
    Start(FullSettings());
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 0);

    for (int i = 0; i < 4; ++i) {
        PlayerDamage(S().enemies[static_cast<std::size_t>(i)]);   // 每次 +2 → 8，尚未满
    }
    CHECK_EQ(Current(), 8);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 0);

    PlayerDamage(S().enemies[4]);              // 第 5 次 +2 → 10，首次满怒
    CHECK_EQ(Current(), 10);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 1);

    PlayerDamage(S().enemies[5]);              // 满怒后继续受伤：封顶且不再发事件
    PlayerDamage(S().enemies[6]);
    CHECK_EQ(Current(), 10);
    CHECK_EQ(krfake::EventCount("KR_RageBecameFull"), 1);
}

// 14. 变身期间伤害照常计分；变身结束后继续计分
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

    // FillRage 已在本秒用满 10 预算，先越过窗口再验证「变身期间仍计分」
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    PlayerDamage(S().enemies[2]);
    CHECK_EQ(Current(), 2);             // 变身期间也计分

    krfake::ClearEffects(S().player);   // 813 结束
    Refresh();
    CHECK(!Snap().raging);
    CHECK(Snap().eligible);

    PlayerDamage(S().enemies[3]);
    CHECK_EQ(Current(), 4);             // 恢复后继续计分
}

// 15. 旧会话入队的任务（读档后的 Configure + 遗留的 805 请求）在 Restore 之后必须整体失效
//     （Queue 无 generation epoch 的旧实现：Configure 先跑使状态重新 ready，旧请求紧接着施放 814
//      → 本用例在 krfake::CastCount(rageSpell) == 0 上 FAIL）
void CaseStaleRequestDroppedByRestore()
{
    Start(FullSettings());
    FillRage();
    CHECK_EQ(Current(), 10);
    CHECK(Snap().ready);

    const kr::SavedState saved = kr::GameBridge::Get().Save();
    CHECK_EQ(saved.current, 10);
    CHECK_EQ(saved.pending, false);

    // 读档完成前的同一帧里已经压入两件事：读档后的 Configure，以及一个旧会话遗留的 805 请求。
    kr::GameBridge::Get().Configure(FullSettings(), kDeniedInterval);
    Request805();
    CHECK(!krfake::Tasks().empty());

    kr::GameBridge::Get().Restore(saved);    // 读档：此前入队的一切都必须失效
    krfake::DrainAll();

    CHECK_EQ(krfake::CastCount(S().rageSpell), 0);     // 陈旧请求不得启动施法
    CHECK_EQ(krfake::EventCount("KR_RageDenied"), 0);  // 也不得执行到"拒绝"分支
    CHECK_EQ(SuccessCastTotal(), 0);
    CHECK_EQ(Current(), 10);
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

// 16. 未确认的挂起请求不得跨存档消耗怒气
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
    CHECK_EQ(saved.current, 10);

    kr::GameBridge::Get().Restore(saved);
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();

    CHECK_EQ(Current(), 10);            // 旧请求不得跨档扣怒气
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

// 17. 三源移动键（方向键或 Keytrace 效果）与拒绝提示的可观测面
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

// 18. Revert 同样必须使旧代已入队任务失效（与 15 的 Restore 版对称）
void CaseRevertDropsStaleRequest()
{
    Start(FullSettings());
    FillRage();
    CHECK_EQ(Current(), 10);

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

// 19. 同一次读档里第二次 Reconcile 不得吞掉进行中的激活（needsReconcile_ 守卫）
void CaseDuplicateReconcileKeepsPending()
{
    Start(FullSettings());
    FillRage();

    Request805();
    krfake::RunFrame();                 // 第一次激活：pending
    CHECK_EQ(Snap().pending, true);
    CHECK_EQ(krfake::CastCount(S().rageSpell), 1);

    // 读档流程里的第一次 reconcile：按设计结算掉这次挂起
    kr::GameBridge::Get().ReconcileAfterLoad();
    krfake::DrainAll();
    CHECK_EQ(Snap().pending, false);
    CHECK_EQ(Current(), 10);
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
    CHECK_EQ(Current(), 10);

    // 若生效则最终仍能正常确认结算
    krfake::ApplyEffect(S().player, S().rageEffect);
    Refresh();
    CHECK_EQ(Snap().pending, false);
    CHECK_EQ(Current(), 0);
    CHECK_EQ(Snap().raging, true);
    CHECK_EQ(SuccessCastTotal(), 3);
}

struct TestCase {
    const char* name;
    void (*fn)();
};

const TestCase kCases[] = {
    {"damage_scores_without_configure", &CaseDamageScoresWithoutConfigure},
    {"damage_gain_fixed_maximum_applies", &CaseDamageGainIsFixedAndMaximumApplies},
    {"damage_target_and_value_validation", &CaseDamageTargetAndValueValidation},
    {"legacy_entries_do_not_score", &CaseLegacyEntriesDoNotScore},
    {"damage_budget_shared_per_second", &CaseDamageBudgetSharedPerSecond},
    {"maximum_cap_and_became_full_once", &CaseMaximumCapAndBecameFullOnce},
    {"damage_scores_during_pending_and_raging", &CaseDamageScoresDuringPendingAndRaging},
    {"full_one_cast_sync_confirm", &CaseFullOneCastSyncConfirm},
    {"full_one_cast_delayed_confirm", &CaseFullOneCastDelayedConfirm},
    {"pending_survives_same_frame_drain", &CasePendingSurvivesSameFrameDrain},
    {"explicit_task_drain", &CaseExplicitTaskDrain},
    {"native_does_not_reapply_originals", &CaseNativeDoesNotReapplyOriginals},
    {"became_full_event_once", &CaseBecameFullEventOnce},
    {"scoring_resumes_after_rage_ends", &CaseScoringResumesAfterRageEnds},
    {"stale_request_dropped_by_restore", &CaseStaleRequestDroppedByRestore},
    {"pending_does_not_cross_save", &CasePendingDoesNotCrossSave},
    {"denied_notification_observable", &CaseDeniedNotificationObservable},
    {"revert_drops_stale_request", &CaseRevertDropsStaleRequest},
    {"duplicate_reconcile_keeps_pending", &CaseDuplicateReconcileKeepsPending},
};

}  // namespace

int main()
{
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
