// Isolated harness for the real src/DamageHooks.cpp.
//
// The production file is copied byte for byte into the build tree and compiled against the
// fake RE/REL/SKSE/GameBridge headers in fake/, so what runs here is the shipped hook code,
// not a copy of its logic. Nothing in this harness touches the game: the "engine" and the
// vtable are ordinary test objects.

#include "PCH.h"
#include "GameBridge.h"
#include "DamageHooks.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using ActorHealthDamage = void (*)(RE::Actor*, RE::Actor*, float);
using CharacterHealthDamage = void (*)(RE::Character*, RE::Actor*, float);

struct DamageReport
{
    RE::FormID targetID{};
    RE::FormID baseID{};
    float healthLost{};
};

// What the fake original (standing in for the engine's own HandleHealthDamage) does for the
// next dispatch.
struct DispatchBehaviour
{
    float healthAfter{ 100.0f };
    float nestedHealthAfter{ 100.0f };
    RE::Actor* nestedTarget{};
    bool nested{ false };
    RE::Actor* nestedAttacker{};  // nullptr: reuse the outer attacker
    bool kill{ false };           // the original call also kills the outer target
};

std::vector<DamageReport> g_reports;
std::vector<std::string> g_sequence;
int g_originalActorCalls = 0;
int g_originalCharacterCalls = 0;
float g_lastOriginalDamage = 0.0f;
RE::Actor* g_lastOriginalTarget = nullptr;
RE::Actor* g_lastOriginalAttacker = nullptr;
bool g_inNestedOriginal = false;
DispatchBehaviour g_actorBehaviour;
DispatchBehaviour g_characterBehaviour;
int g_checks = 0;
int g_failures = 0;

void Check(bool a_condition, const std::string& a_what)
{
    ++g_checks;
    if (a_condition) {
        return;
    }
    ++g_failures;
    std::printf("FAIL: %s\n", a_what.c_str());
}

std::string Joined(const std::vector<std::string>& a_parts)
{
    std::string result;
    for (const auto& part : a_parts) {
        if (!result.empty()) {
            result += ",";
        }
        result += part;
    }
    return result;
}

std::string ReportText()
{
    std::string result;
    for (const auto& report : g_reports) {
        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer), "[%X/%X/%.3f]", report.targetID, report.baseID,
            static_cast<double>(report.healthLost));
        result += buffer;
    }
    return result.empty() ? std::string{ "-" } : result;
}

void ResetEventLog()
{
    g_reports.clear();
    g_sequence.clear();
    g_originalActorCalls = 0;
    g_originalCharacterCalls = 0;
    g_lastOriginalDamage = 0.0f;
    g_lastOriginalTarget = nullptr;
    g_lastOriginalAttacker = nullptr;
    g_inNestedOriginal = false;
}

bool NearlyEqual(float a_left, float a_right)
{
    return std::fabs(a_left - a_right) < 1e-4f;
}

void ExpectReport(const char* a_case, std::size_t a_count, RE::FormID a_targetID, RE::FormID a_baseID, float a_lost)
{
    Check(g_reports.size() == a_count,
        std::string{ a_case } + ": expected " + std::to_string(a_count) + " report(s), got " + std::to_string(g_reports.size()) + " " + ReportText());
    if (g_reports.size() != 1 || a_count != 1) {
        return;
    }
    const auto& report = g_reports.front();
    Check(report.targetID == a_targetID && report.baseID == a_baseID,
        std::string{ a_case } + ": target/base ids forwarded unchanged, got " + ReportText());
    Check(NearlyEqual(report.healthLost, a_lost),
        std::string{ a_case } + ": reported health loss is " + std::to_string(a_lost) + ", got " + ReportText());
}

// ---- the "engine" side the hook chains into ------------------------------------------------

void EngineOriginalActor(RE::Actor* a_target, RE::Actor* a_attacker, float a_damage)
{
    ++g_originalActorCalls;
    g_sequence.emplace_back("original");
    g_lastOriginalDamage = a_damage;
    g_lastOriginalTarget = a_target;
    g_lastOriginalAttacker = a_attacker;

    if (g_actorBehaviour.nested && g_actorBehaviour.nestedTarget) {
        g_actorBehaviour.nested = false;  // exactly one nesting level
        g_inNestedOriginal = true;
        RE::Actor* nestedAttacker = g_actorBehaviour.nestedAttacker ? g_actorBehaviour.nestedAttacker : a_attacker;
        reinterpret_cast<ActorHealthDamage>(RE::g_actorVtableEntries[0x104])(g_actorBehaviour.nestedTarget, nestedAttacker, -3.0f);
        g_inNestedOriginal = false;
    }
    if (a_target) {
        a_target->actorValueOwner.health = g_inNestedOriginal ? g_actorBehaviour.nestedHealthAfter : g_actorBehaviour.healthAfter;
        if (g_actorBehaviour.kill) {
            g_actorBehaviour.kill = false;
            a_target->dead = true;
        }
    }
}

void EngineOriginalCharacter(RE::Character* a_target, RE::Actor* a_attacker, float a_damage)
{
    ++g_originalCharacterCalls;
    g_sequence.emplace_back("original");
    if (a_target) {
        a_target->actorValueOwner.health = g_characterBehaviour.healthAfter;
    }
    (void)a_attacker;
    (void)a_damage;
}

// ---- dispatch through the vtable slot, the way the engine would ----------------------------

void Hit(RE::Actor& a_target, RE::Actor& a_attacker, float a_damage)
{
    reinterpret_cast<ActorHealthDamage>(RE::g_actorVtableEntries[0x104])(&a_target, &a_attacker, a_damage);
}

void HitCharacter(RE::Character& a_target, RE::Actor& a_attacker, float a_damage)
{
    reinterpret_cast<CharacterHealthDamage>(RE::g_characterVtableEntries[0x104])(&a_target, &a_attacker, a_damage);
}

struct World
{
    World()
    {
        npcBase.formID = 0x0012345;
        otherBase.formID = 0x0067890;
        player.formID = 0x00000014;
        npc.formID = 0x00ABCDEF;
        npc.base = &npcBase;
        secondNpc.formID = 0x00777888;
        secondNpc.base = &otherBase;
        creature.formID = 0x00FEDCBA;
        creature.base = &npcBase;
        RE::PlayerCharacter::singleton = &player;
    }

    ~World() { RE::PlayerCharacter::singleton = nullptr; }

    RE::TESBoundObject npcBase;
    RE::TESBoundObject otherBase;
    RE::PlayerCharacter player;
    RE::Actor npc;
    RE::Actor secondNpc;
    RE::Character creature;
};

void InstallAndVerify()
{
    RE::g_actorVtableEntries[0x104] = reinterpret_cast<std::uintptr_t>(&EngineOriginalActor);
    RE::g_characterVtableEntries[0x104] = reinterpret_cast<std::uintptr_t>(&EngineOriginalCharacter);
    const auto actorBefore = RE::g_actorVtableEntries[0x104];
    const auto characterBefore = RE::g_characterVtableEntries[0x104];

    spdlog::test::clear();
    kr::InstallDamageHooks();

    const auto actorAfter = RE::g_actorVtableEntries[0x104];
    const auto characterAfter = RE::g_characterVtableEntries[0x104];
    Check(actorAfter != 0 && actorAfter != actorBefore, "install: Actor slot 0x104 points at the hook");
    Check(characterAfter != 0 && characterAfter != characterBefore, "install: Character slot 0x104 points at the hook");
    Check(spdlog::test::contains("damage hooks installed (Actor="), "install: success was logged once");

    spdlog::test::clear();
    kr::InstallDamageHooks();
    Check(RE::g_actorVtableEntries[0x104] == actorAfter && RE::g_characterVtableEntries[0x104] == characterAfter,
        "install: a repeat install leaves both slots untouched");
    Check(spdlog::test::contains("already attempted"), "install: a repeat install was logged as ignored");
}

int RunEmptyEntryCase()
{
    spdlog::test::clear();
    kr::InstallDamageHooks();

    Check(RE::g_actorVtableEntries[0x104] == 0, "empty entry: the Actor slot was left untouched");
    Check(RE::g_characterVtableEntries[0x104] == 0, "empty entry: the Character slot was left untouched");
    Check(spdlog::test::contains("error: KratosRage damage hook disabled: Actor vtable has no entry at vfunc 0x104"),
        "empty entry: the Actor hook was disabled with an error");
    Check(spdlog::test::contains("error: KratosRage damage hook disabled: Character vtable has no entry at vfunc 0x104"),
        "empty entry: the Character hook was disabled with an error");
    Check(spdlog::test::contains("error: KratosRage damage hooks not installed"), "empty entry: the failed install was reported");
    Check(!spdlog::test::contains("damage hooks installed (Actor="), "empty entry: no success was logged");

    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

int RunBehaviourMatrix()
{
    InstallAndVerify();

    World world;

    // 1) the player damages a plain Actor through the Actor vtable
    {
        ResetEventLog();
        g_actorBehaviour = DispatchBehaviour{ 90.0f };
        Hit(world.npc, world.player, -10.0f);
        ExpectReport("actor hit", 1, world.npc.formID, world.npcBase.formID, 10.0f);
        Check(g_originalActorCalls == 1, "actor hit: the original entry was called exactly once");
        Check(Joined(g_sequence) == "original,report", "actor hit: the original runs before the report, got " + Joined(g_sequence));
        Check(g_lastOriginalTarget == &world.npc && g_lastOriginalAttacker == &world.player,
            "actor hit: the original received the same target and attacker");
        Check(NearlyEqual(g_lastOriginalDamage, -10.0f), "actor hit: the damage value passed through unchanged");
    }

    // 2) the player damages a Character (the vtable that carries all non-player actors)
    {
        ResetEventLog();
        world.creature.actorValueOwner.health = 50.0f;
        g_characterBehaviour = DispatchBehaviour{ 44.0f };
        HitCharacter(world.creature, world.player, -6.0f);
        ExpectReport("character hit", 1, world.creature.formID, world.npcBase.formID, 6.0f);
        Check(g_originalCharacterCalls == 1, "character hit: the original entry was called exactly once");
        Check(g_originalActorCalls == 0, "character hit: the Actor entry was not involved");
        Check(Joined(g_sequence) == "original,report", "character hit: the original runs before the report, got " + Joined(g_sequence));
    }

    // 3) self damage: attacker and target are both the player
    {
        ResetEventLog();
        g_actorBehaviour = DispatchBehaviour{ 90.0f };
        Hit(world.player, world.player, -10.0f);
        ExpectReport("self damage", 0, 0, 0, 0.0f);
        Check(g_originalActorCalls == 1, "self damage: the original entry was still called once");
    }

    // 4) NPC damages NPC: the attacker is not the player
    {
        ResetEventLog();
        g_actorBehaviour = DispatchBehaviour{ 90.0f };
        Hit(world.npc, world.secondNpc, -10.0f);
        ExpectReport("npc on npc", 0, 0, 0, 0.0f);
        Check(g_originalActorCalls == 1, "npc on npc: the original entry was still called once");
    }

    // 5) zero raw damage
    {
        ResetEventLog();
        g_actorBehaviour = DispatchBehaviour{ 90.0f };
        Hit(world.npc, world.player, 0.0f);
        ExpectReport("zero damage", 0, 0, 0, 0.0f);
        Check(g_originalActorCalls == 1, "zero damage: the original entry was still called once");
    }

    // 6) positive raw damage: a heal, not health loss
    {
        ResetEventLog();
        g_actorBehaviour = DispatchBehaviour{ 110.0f };
        Hit(world.npc, world.player, 5.0f);
        ExpectReport("positive damage", 0, 0, 0, 0.0f);
    }

    // 7) non-finite raw damage
    {
        ResetEventLog();
        g_actorBehaviour = DispatchBehaviour{ 90.0f };
        Hit(world.npc, world.player, std::numeric_limits<float>::quiet_NaN());
        Hit(world.npc, world.player, -std::numeric_limits<float>::infinity());
        ExpectReport("non-finite damage", 0, 0, 0, 0.0f);
        Check(g_originalActorCalls == 2, "non-finite damage: the original entry was still called for both dispatches");
    }

    // 8) negative damage that cost no health (immune, absorbed)
    {
        ResetEventLog();
        world.npc.actorValueOwner.health = 100.0f;
        g_actorBehaviour = DispatchBehaviour{ 100.0f };
        Hit(world.npc, world.player, -5.0f);
        ExpectReport("no health lost", 0, 0, 0, 0.0f);
        Check(g_originalActorCalls == 1, "no health lost: the original entry was still called once");
    }

    // 9) the health reading is unusable after the original call
    {
        ResetEventLog();
        world.npc.actorValueOwner.health = 100.0f;
        g_actorBehaviour = DispatchBehaviour{ std::numeric_limits<float>::quiet_NaN() };
        Hit(world.npc, world.player, -5.0f);
        ExpectReport("unreadable health", 0, 0, 0, 0.0f);
    }

    // 10) the target was already dead before the call
    {
        ResetEventLog();
        world.npc.actorValueOwner.health = 100.0f;
        world.npc.dead = true;
        g_actorBehaviour = DispatchBehaviour{ 90.0f };
        Hit(world.npc, world.player, -10.0f);
        ExpectReport("dead before the call", 0, 0, 0, 0.0f);
        Check(g_originalActorCalls == 1, "dead before the call: the original entry was still called once");
        world.npc.dead = false;
    }

    // 11) a nested damage event against a different target is reported for each target
    {
        ResetEventLog();
        world.npc.actorValueOwner.health = 100.0f;
        world.secondNpc.actorValueOwner.health = 100.0f;
        g_actorBehaviour = DispatchBehaviour{ 90.0f, 97.0f, &world.secondNpc, true };
        Hit(world.npc, world.player, -10.0f);
        Check(g_reports.size() == 2,
            std::string{ "nested different target: expected 2 reports, got " } + ReportText());
        if (g_reports.size() == 2) {
            Check(g_reports[0].targetID == world.secondNpc.formID && g_reports[0].baseID == world.otherBase.formID,
                "nested different target: the inner report carries the nested target's ids");
            Check(NearlyEqual(g_reports[0].healthLost, 3.0f),
                "nested different target: the inner report carries the nested loss");
            Check(g_reports[1].targetID == world.npc.formID && g_reports[1].baseID == world.npcBase.formID,
                "nested different target: the outer report carries the outer target's ids");
            Check(NearlyEqual(g_reports[1].healthLost, 10.0f),
                "nested different target: the outer report carries the outer loss");
        }
        Check(g_originalActorCalls == 2, "nested different target: both frames chained to the original exactly once");
        Check(NearlyEqual(world.secondNpc.actorValueOwner.health, 97.0f), "nested different target: the nested hit still applied its own health loss");
        Check(Joined(g_sequence) == "original,original,report,report", "nested different target: each frame reported after its own original, got " + Joined(g_sequence));
    }

    // 12) a nested damage event on the same target folds into one report
    {
        ResetEventLog();
        world.npc.actorValueOwner.health = 100.0f;
        g_actorBehaviour = DispatchBehaviour{ 90.0f, 97.0f, &world.npc, true };
        Hit(world.npc, world.player, -10.0f);
        ExpectReport("nested same target", 1, world.npc.formID, world.npcBase.formID, 10.0f);
        Check(g_originalActorCalls == 2, "nested same target: both frames chained to the original exactly once");
        Check(Joined(g_sequence) == "original,original,report", "nested same target: only the outer frame reported, got " + Joined(g_sequence));
    }

    // 13) a player hit nested inside a non-player dispatch is still reported
    {
        ResetEventLog();
        world.npc.actorValueOwner.health = 100.0f;
        world.secondNpc.actorValueOwner.health = 100.0f;
        g_actorBehaviour = DispatchBehaviour{ 90.0f, 97.0f, &world.secondNpc, true, &world.player };
        Hit(world.npc, world.secondNpc, -5.0f);  // npc on npc outside, player on npc inside
        ExpectReport("player hit in non-player frame", 1, world.secondNpc.formID, world.otherBase.formID, 3.0f);
        Check(g_originalActorCalls == 2, "player hit in non-player frame: both frames chained to the original exactly once");
    }

    // 14) a killing blow is still reported
    {
        ResetEventLog();
        world.npc.actorValueOwner.health = 100.0f;
        g_actorBehaviour = DispatchBehaviour{ 0.0f, 0.0f, nullptr, false, nullptr, true };
        Hit(world.npc, world.player, -10.0f);
        ExpectReport("killing blow", 1, world.npc.formID, world.npcBase.formID, 100.0f);
        Check(world.npc.dead, "killing blow: the original still applied the death");
        world.npc.dead = false;
    }

    // 15) an unrelated event after a nested one is still reported
    {
        ResetEventLog();
        world.npc.actorValueOwner.health = 100.0f;
        g_actorBehaviour = DispatchBehaviour{ 80.0f };
        Hit(world.npc, world.player, -20.0f);
        ExpectReport("event after nesting", 1, world.npc.formID, world.npcBase.formID, 20.0f);
    }

    // 16) ids and the amount are forwarded unchanged
    {
        ResetEventLog();
        world.npc.formID = 0x000D0D01;
        world.npcBase.formID = 0x0000BEEF;
        world.npc.actorValueOwner.health = 100.0f;
        g_actorBehaviour = DispatchBehaviour{ 92.5f };
        Hit(world.npc, world.player, -7.5f);
        ExpectReport("id forwarding", 1, 0x000D0D01, 0x0000BEEF, 7.5f);

        ResetEventLog();
        world.npc.base = nullptr;
        world.npc.actorValueOwner.health = 100.0f;
        g_actorBehaviour = DispatchBehaviour{ 90.0f };
        Hit(world.npc, world.player, -10.0f);
        ExpectReport("missing base form", 1, 0x000D0D01, 0, 10.0f);
        world.npc.base = &world.npcBase;
    }

    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}

namespace kr {

GameBridge& GameBridge::Get()
{
    static GameBridge instance;
    return instance;
}

void GameBridge::SubmitPlayerDamage(RE::FormID a_targetID, RE::FormID a_baseID, float a_healthLost)
{
    g_sequence.emplace_back("report");
    g_reports.push_back({ a_targetID, a_baseID, a_healthLost });
}

}

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "empty") {
        return RunEmptyEntryCase();
    }
    return RunBehaviourMatrix();
}
