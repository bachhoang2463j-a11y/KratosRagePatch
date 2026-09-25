using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Skyrim;

namespace PluginBuilder;

/// <summary>
/// Thrown for any condition the tool can detect and explain instead of crashing on.
/// </summary>
public sealed class ToolException : Exception
{
    public ToolException(string message) : base(message) { }
}

/// <summary>
/// Default input paths of this machine (overridable on the command line). The original mod and the
/// game install are opened read-only; nothing is ever written outside --stage / --report.
/// </summary>
public static class Defaults
{
    public const string GodOfWar = @"F:\Skyrim SE\ModOrganizer\mods\斧头God of War Kratos Combat MCO\God of War.esp";
    public const string GameData = @"F:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data";
    public const string RepoRoot = @"D:\Project\KratosRagePatch";
    public const string PatchPluginName = "KratosRagePatch.esp";

    public static string Stage => Path.Combine(RepoRoot, "build", "stage");
    public static string Report => Path.Combine(RepoRoot, "build", "PluginBuilder", "record-report.json");
    public static string SkyrimEsm => Path.Combine(GameData, "Skyrim.esm");
    public static string DragonbornEsm => Path.Combine(GameData, "Dragonborn.esm");

    public static readonly ModKey SkyrimKey = ModKey.FromNameAndExtension("Skyrim.esm");
    public static readonly ModKey DragonbornKey = ModKey.FromNameAndExtension("Dragonborn.esm");
    public static readonly ModKey PatchKey = ModKey.FromNameAndExtension(PatchPluginName);

    /// <summary>
    /// Forms the interface contract names, so <c>analyze</c> produces the full report without any
    /// flag: the original PayloadInterpreter $SIBADA payload (God of War4.ini lines 33-41) and the
    /// two forms the OAR conditions gate on. Both --refs and --dust still override these.
    /// </summary>
    public const string ContractRefs =
        "Skyrim.esm=0x2FFD2," +        // crGiantStomp
        "God of War.esp=0x817," +      // EmptyhandedabilityDUPLICATE001
        "God of War.esp=0x844," +      // PerkAvoidDeathAbilitywar
        "God of War.esp=0x814," +      // SpartanFuryDUPLICATE001 (the rage spell)
        "God of War.esp=0x841," +      // warcrGiantStomp
        "Dragonborn.esm=0x1DF97," +    // written 0X201DF97 in the payload
        "Skyrim.esm=0x7A82B," +        // FireStorm
        "God of War.esp=0x813," +      // MGEF SpartanFury (OAR gate)
        "God of War.esp=0x805";        // KYWD LeviathansAxe (OAR gate)

    /// <summary>HitFog spells of the original mod: visuals only, never counted as hits.</summary>
    public const string ContractDust = "God of War.esp=0x872,God of War.esp=0x874";
}

/// <summary>Record ids reserved by the interface contract (docs/implementation-contracts.md).</summary>
public static class Id
{
    public const uint MainQuest = 0x800;
    public const uint WidgetQuest = 0x801;
    public const uint RageReadyAbility = 0x802;
    public const uint RageReadyEffect = 0x803;
    public const uint ActivationRequestSpell = 0x804;
    public const uint ActivationRequestEffect = 0x805;
    public const uint FullRageGlow = 0x806;
    public const uint NextFormId = 0x807;

    /// <summary>Original mod's own rage forms, quoted by the interface contract.</summary>
    public const uint OriginalRageEffect = 0x813;   // God of War.esp MGEF (原斯巴达之怒特效)
    public const uint OriginalRageSpell = 0x814;    // God of War.esp SPEL (原斯巴达之怒法术)

    /// <summary>ESL light-plugin ceiling: every new record must live inside 0x800-0xFFF.</summary>
    public const uint LightUpperBound = 0xFFF;
}
