using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

namespace PluginBuilder;

/// <summary>
/// Writes KratosRagePatch.esp (ESL-flagged light plugin, form version 44) from the record contract,
/// plus the matching Seq file. Every record is built through Mutagen's typed API - no hand written
/// plugin binary anywhere.
/// </summary>
public static class PatchBuilder
{
    public sealed record Inputs(string GodOfWarPath, string DataDir, string StageDir);

    public const uint FullGlowSource = 0x021733;   // Dragonborn.esm local id of the copied EFSH
    public const uint PlayerRef = 0x000014;        // Skyrim.esm PlayerRef

    public static string PluginPath(string stage) => Path.Combine(stage, Defaults.PatchPluginName);
    public static string SeqPath(string stage) => Path.Combine(stage, "Seq", Path.GetFileNameWithoutExtension(Defaults.PatchPluginName) + ".seq");

    public static Dictionary<string, object?> Run(Inputs input)
    {
        var playerRefKey = new FormKey(Defaults.SkyrimKey, PlayerRef);
        var dbPath = Path.Combine(input.DataDir, Defaults.DragonbornKey.FileName.String);
        var skyPath = Path.Combine(input.DataDir, Defaults.SkyrimKey.FileName.String);
        if (!File.Exists(dbPath)) throw new ToolException($"missing master: {dbPath}");
        if (!File.Exists(skyPath)) throw new ToolException($"missing master: {skyPath}");

        var mod = new SkyrimMod(Defaults.PatchKey, SkyrimRelease.SkyrimSE);
        mod.ModHeader.Flags |= SkyrimModHeader.HeaderFlag.Small;             // ESL / light plugin
        mod.ModHeader.FormVersion = 44;                                     // SSE form version
        mod.ModHeader.Author = "KratosRagePatch";
        mod.ModHeader.Description = "Kratos Rage runtime records (raggiungere)";
        mod.ModHeader.Stats.NextFormID = Id.NextFormId;

        // ---------------------------------------------------------------- 0x803 / 0x805 markers
        var readyEffect = new MagicEffect(new FormKey(Defaults.PatchKey, Id.RageReadyEffect), SkyrimRelease.SkyrimSE)
        {
            EditorID = "KR_RageReadyEffect",
            Name = "Kratos Rage Ready",
            Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.Script) { ActorValue = ActorValue.None },
            CastType = CastType.ConstantEffect,
            TargetType = TargetType.Self,
            BaseCost = 0,
            MagicSkill = ActorValue.None,
            ResistValue = ActorValue.None,
            CastingSoundLevel = SoundLevel.Silent,
            MinimumSkillLevel = 0,
            Flags = MagicEffect.Flag.HideInUI | MagicEffect.Flag.NoMagnitude | MagicEffect.Flag.NoArea
                  | MagicEffect.Flag.NoDuration | MagicEffect.Flag.Painless | MagicEffect.Flag.NoHitEffect,
        };
        mod.MagicEffects.Add(readyEffect);

        var requestEffect = new MagicEffect(new FormKey(Defaults.PatchKey, Id.ActivationRequestEffect), SkyrimRelease.SkyrimSE)
        {
            EditorID = "KR_ActivationRequestEffect",
            Name = "Kratos Rage Activation Request",
            Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.Script) { ActorValue = ActorValue.None },
            CastType = CastType.FireAndForget,
            TargetType = TargetType.Self,
            BaseCost = 0,
            MagicSkill = ActorValue.None,
            ResistValue = ActorValue.None,
            CastingSoundLevel = SoundLevel.Silent,
            MinimumSkillLevel = 0,
            Flags = MagicEffect.Flag.HideInUI | MagicEffect.Flag.NoMagnitude | MagicEffect.Flag.NoArea
                  | MagicEffect.Flag.Painless | MagicEffect.Flag.NoHitEffect,
        };
        mod.MagicEffects.Add(requestEffect);

        // ---------------------------------------------------------------- 0x802 constant ability
        var readyAbility = new Spell(new FormKey(Defaults.PatchKey, Id.RageReadyAbility), SkyrimRelease.SkyrimSE)
        {
            EditorID = "KR_RageReadyAbility",
            Name = "Kratos Rage Ready Ability",
            Type = SpellType.Ability,
            CastType = CastType.ConstantEffect,
            TargetType = TargetType.Self,
            BaseCost = 0,
            Effects = { MakeEffect(readyEffect.FormKey, magnitude: 0, area: 0, duration: 0) },
        };
        mod.Spells.Add(readyAbility);

        // ---------------------------------------------------------------- 0x804 free self request spell
        var requestSpell = new Spell(new FormKey(Defaults.PatchKey, Id.ActivationRequestSpell), SkyrimRelease.SkyrimSE)
        {
            EditorID = "KR_ActivationRequestSpell",
            Name = "Kratos Rage Activation Request",
            Type = SpellType.Spell,
            CastType = CastType.FireAndForget,
            TargetType = TargetType.Self,
            BaseCost = 0,
            ChargeTime = 0,
            CastDuration = 0,
            Range = 0,
            Flags = SpellDataFlag.ManualCostCalc,
            Effects = { MakeEffect(requestEffect.FormKey, magnitude: 0, area: 0, duration: 1) },
        };
        mod.Spells.Add(requestSpell);

        // ---------------------------------------------------------------- 0x806 glow (copy of Dragonborn 0x021733)
        using var dragonborn = SkyrimMod.CreateFromBinaryOverlay(ModPath.FromPath(dbPath), SkyrimRelease.SkyrimSE);
        if (!dragonborn.EffectShaders.TryGetValue(new FormKey(Defaults.DragonbornKey, FullGlowSource), out var glowSource) || glowSource is null)
            throw new ToolException($"Dragonborn.esm 0x{FullGlowSource:X} is missing or is not an EFSH record");
        var glow = glowSource.Duplicate(new FormKey(Defaults.PatchKey, Id.FullRageGlow));
        glow.EditorID = "KR_FullRageGlow";
        mod.EffectShaders.Add(glow);

        // ---------------------------------------------------------------- quests + VMAD
        using var skyrim = SkyrimMod.CreateFromBinaryOverlay(ModPath.FromPath(skyPath), SkyrimRelease.SkyrimSE);
        var template = QuestTemplate.FromSkyrim(skyrim);
        var widgetKey = new FormKey(Defaults.PatchKey, Id.WidgetQuest);
        var mainKey = new FormKey(Defaults.PatchKey, Id.MainQuest);

        var widgetQuest = new Quest(widgetKey, SkyrimRelease.SkyrimSE)
        {
            EditorID = "KR_WidgetQuest",
            Name = "Kratos Rage Widget",
            Flags = Quest.Flag.StartGameEnabled,
            Type = Quest.TypeEnum.None,
            Priority = template.Priority,
            QuestFormVersion = template.QuestFormVersion,
            Unknown = template.Unknown,
            VirtualMachineAdapter = new QuestAdapter
            {
                Version = template.AdapterVersion,
                ObjectFormat = template.AdapterObjectFormat,
                Scripts = { new ScriptEntry { Name = "KR_Widget", Flags = template.ScriptFlags } },
            },
        };
        mod.Quests.Add(widgetQuest);

        var mainQuest = new Quest(mainKey, SkyrimRelease.SkyrimSE)
        {
            EditorID = "KR_MainQuest",
            Name = "Kratos Rage",
            Flags = Quest.Flag.StartGameEnabled,
            Type = Quest.TypeEnum.None,
            Priority = template.Priority,
            QuestFormVersion = template.QuestFormVersion,
            Unknown = template.Unknown,
            NextAliasID = 1,
            Aliases =
            {
                new QuestAlias
                {
                    ID = 0,
                    Name = "KR_PlayerAlias",
                    Type = QuestAlias.TypeEnum.Reference,
                    ForcedReference = new FormLinkNullable<IPlacedGetter>(playerRefKey),
                },
            },
            VirtualMachineAdapter = new QuestAdapter
            {
                Version = template.AdapterVersion,
                ObjectFormat = template.AdapterObjectFormat,
                Scripts =
                {
                    new ScriptEntry
                    {
                        Name = "KR_Config",
                        Flags = template.ScriptFlags,
                        Properties =
                        {
                            new ScriptObjectProperty
                            {
                                Name = "Widget",
                                Flags = template.PropertyFlags,
                                Object = new FormLink<ISkyrimMajorRecordGetter>(widgetKey),
                                Alias = -1,
                            },
                        },
                    },
                },
                Aliases =
                {
                    new QuestFragmentAlias
                    {
                        Version = template.AliasEntryVersion,
                        ObjectFormat = template.AliasEntryObjectFormat,
                        Property = new ScriptObjectProperty
                        {
                            Name = template.AliasPropertyName,
                            Flags = template.AliasPropertyFlags,
                            // The alias entry's object union is [static][alias][owning quest form id]: vanilla
                            // Skyrim.esm writes the owning quest's own FormID here in every one of its 2075
                            // alias entries, so mirror that instead of leaving it null.
                            Object = new FormLink<ISkyrimMajorRecordGetter>(mainKey),
                            Alias = 0,
                        },
                        Scripts =
                        {
                            new ScriptEntry
                            {
                                Name = "KR_PlayerAlias",
                                Flags = template.ScriptFlags,
                                Properties =
                                {
                                    FormObjectProperty("Config", template.PropertyFlags, mainKey),
                                    FormObjectProperty("Widget", template.PropertyFlags, widgetKey),
                                    FormObjectProperty("FullGlow", template.PropertyFlags, new FormKey(Defaults.PatchKey, Id.FullRageGlow)),
                                },
                            },
                        },
                    },
                },
            },
        };
        mod.Quests.Add(mainQuest);

        // ---------------------------------------------------------------- masters
        mod.ModHeader.MasterReferences.Add(new MasterReference { Master = Defaults.SkyrimKey });
        var needsDragonborn = (glow.AddonModels is { IsNull: false }) || (glow.AmbientSound is { IsNull: false });
        if (needsDragonborn)
            mod.ModHeader.MasterReferences.Add(new MasterReference { Master = Defaults.DragonbornKey });

        // ---------------------------------------------------------------- write
        var pluginPath = PluginPath(input.StageDir);
        Directory.CreateDirectory(input.StageDir);
        mod.WriteToBinary(pluginPath);

        var seqPath = SeqPath(input.StageDir);
        WriteSeq(seqPath, new[] { Id.MainQuest, Id.WidgetQuest }, mod.ModHeader.MasterReferences.Count);

        var verify = Verifier.Run(pluginPath, seqPath, mod.ModHeader.MasterReferences.Select(m => m.Master.FileName.String).ToList(), input.DataDir);

        return new Dictionary<string, object?>
        {
            ["plugin"] = Path.GetFullPath(pluginPath),
            ["seq"] = Path.GetFullPath(seqPath),
            ["eslFlagged"] = mod.ModHeader.Flags.HasFlag(SkyrimModHeader.HeaderFlag.Small),
            ["formVersion"] = mod.ModHeader.FormVersion,
            ["masters"] = mod.ModHeader.MasterReferences.Select(m => m.Master.FileName.String).ToList(),
            ["glowSourceLinksToDragonborn"] = needsDragonborn,
            // The VMAD object union at object format 2 and the two encodings it forced. Recorded here
            // so the reasoning and the shipped values stay auditable next to the plugin itself; the
            // raw bytes are re-decoded independently in tests/test_package.py.
            ["vmadEncoding"] = new Dictionary<string, object?>
            {
                ["objectFormat"] = 2,
                ["objectUnion"] = "[static: int16][alias: int16][form id: uint32]",
                ["formValueAlias"] = -1,
                ["evidence"] = new[]
                {
                    "vanilla Skyrim.esm 0x0010D9F4 (JailQuest) VMAD: its 12 Alias_* object properties decode to 12 distinct, mutually consistent alias indices only under this order (Alias_Player -> 15, the alias the JailQuestPlayerScript entry is attached to); the reverse order collapses all of them to alias 0",
                    "vanilla Skyrim.esm: all 5910 form-valued object properties use alias -1 (01 <flags> 00 00 ff ff <form id>)",
                    "vanilla Skyrim.esm: all 2075 alias entries of 974 quests name their own quest in the object union, never 0",
                    "independent raw-byte re-read of this plugin: tests/test_package.py TestVmadBindingsFromRawBytes",
                },
                ["bindings"] = new Dictionary<string, object?>
                {
                    ["KR_Config.Widget"] = "alias -1, form 0x801",
                    ["KR_PlayerAlias.Config"] = "alias -1, form 0x800",
                    ["KR_PlayerAlias.Widget"] = "alias -1, form 0x801",
                    ["KR_PlayerAlias.FullGlow"] = "alias -1, form 0x806",
                    ["aliasEntry"] = "alias 0, own quest 0x800",
                    ["KR_Widget"] = "no VMAD property (SKI_WidgetBase defaults + OnWidgetInit)",
                },
            },
            ["records"] = new Dictionary<string, object?>
            {
                ["0x800"] = "QUST KR_MainQuest (StartGameEnabled, alias 0 KR_PlayerAlias, VMAD KR_Config + KR_PlayerAlias)",
                ["0x801"] = "QUST KR_WidgetQuest (StartGameEnabled, VMAD KR_Widget)",
                ["0x802"] = "SPEL KR_RageReadyAbility (Ability/Constant/Self -> 0x803)",
                ["0x803"] = "MGEF KR_RageReadyEffect (Script marker)",
                ["0x804"] = "SPEL KR_ActivationRequestSpell (Spell/FireAndForget/Self -> 0x805, 1s)",
                ["0x805"] = "MGEF KR_ActivationRequestEffect (Script marker)",
                ["0x806"] = $"EFSH KR_FullRageGlow (copy of Dragonborn.esm 0x{FullGlowSource:X})",
            },
            ["verify"] = verify,
        };
    }

    /// <summary>
    /// A script property that holds a form. The VMAD object union at object format 2 is encoded as
    /// [static: int16][alias: int16][form id: uint32], so a form value must carry alias -1 and the
    /// form's own id - the same encoding Mutagen uses for the KR_Config.Widget property. Writing
    /// alias 0 instead would make the engine read it as "alias 0 of the form in the id field"
    /// (verified against Skyrim.esm: all 5910 form-valued vanilla object properties use alias -1).
    /// </summary>
    private static ScriptObjectProperty FormObjectProperty(string name, ScriptProperty.Flag flags, FormKey target)
        => new()
        {
            Name = name,
            Flags = flags,
            Object = new FormLink<ISkyrimMajorRecordGetter>(target),
            Alias = -1,
        };

    private static Effect MakeEffect(FormKey magicEffect, float magnitude, int area, int duration)
        => new()
        {
            BaseEffect = new FormLinkNullable<IMagicEffectGetter>(magicEffect),
            Data = new EffectData { Magnitude = magnitude, Area = area, Duration = duration },
        };

    /// <summary>
    /// Skyrim's Seq file is a bare array of little-endian uint32 quest form ids. Every Seq file this
    /// machine ships encodes the plugin's own index in the high byte; the value that stays valid
    /// regardless of where the user's load order puts the plugin is the index it has when only its
    /// masters precede it, i.e. the master count (verified against the Seq files present in Data and
    /// in the MO2 mod list - see docs notes in the build report).
    /// </summary>
    private static void WriteSeq(string path, IReadOnlyList<uint> questIds, int masterCount)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var bytes = new byte[questIds.Count * 4];
        for (var i = 0; i < questIds.Count; i++)
        {
            var value = ((uint)masterCount << 24) | questIds[i];
            BitConverter.TryWriteBytes(bytes.AsSpan(i * 4, 4), value);
        }
        File.WriteAllBytes(path, bytes);
    }

    /// <summary>Header fields mirrored from a vanilla SSE quest so the new quests match the engine's expectations.</summary>
    private sealed record QuestTemplate(
        byte Priority, byte QuestFormVersion, int Unknown,
        short AdapterVersion, ushort AdapterObjectFormat, ScriptEntry.Flag ScriptFlags,
        short AliasEntryVersion, ushort AliasEntryObjectFormat, string AliasPropertyName, ScriptProperty.Flag PropertyFlags,
        ScriptProperty.Flag AliasPropertyFlags)
    {
        public static QuestTemplate FromSkyrim(ISkyrimModGetter skyrim)
        {
            ISkyrimModGetter mod = skyrim;
            foreach (var quest in mod.Quests)
            {
                if (quest.VirtualMachineAdapter is null) continue;
                var adapter = quest.VirtualMachineAdapter;
                var aliasEntry = (adapter.Aliases ?? []).FirstOrDefault();
                return new QuestTemplate(
                    Priority: quest.Priority,
                    QuestFormVersion: quest.QuestFormVersion,
                    Unknown: quest.Unknown,
                    AdapterVersion: adapter.Version,
                    AdapterObjectFormat: adapter.ObjectFormat,
                    ScriptFlags: adapter.Scripts is { Count: > 0 } ? adapter.Scripts[0].Flags : (ScriptEntry.Flag)0,
                    AliasEntryVersion: aliasEntry?.Version ?? adapter.Version,
                    AliasEntryObjectFormat: aliasEntry?.ObjectFormat ?? adapter.ObjectFormat,
                    AliasPropertyName: aliasEntry?.Property?.Name ?? "Alias",
                    PropertyFlags: adapter.Scripts is { Count: > 0 } && adapter.Scripts[0].Properties is { Count: > 0 }
                        ? adapter.Scripts[0].Properties[0].Flags
                        : (ScriptProperty.Flag)1,
                    AliasPropertyFlags: aliasEntry?.Property?.Flags ?? (ScriptProperty.Flag)1);
            }
            throw new ToolException("no vanilla quest with a VMAD found in Skyrim.esm - cannot mirror quest header fields");
        }
    }
}
