using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

namespace PluginBuilder;

/// <summary>
/// Reads the plugin and the Seq file back from disk and checks them against the interface contract.
/// This is a structural round trip, not a runtime check: it proves the binary contains what the
/// contract asks for, not that the game behaves as intended.
/// </summary>
internal static class Verifier
{
    public static Dictionary<string, object?> Run(string pluginPath, string seqPath, IReadOnlyList<string> expectedMasters, string dataDir)
    {
        var checks = new List<object?>();
        void Check(string name, bool passed, object? detail = null)
            => checks.Add(new Dictionary<string, object?> { ["check"] = name, ["passed"] = passed, ["detail"] = detail });

        SkyrimMod read;
        try
        {
            read = SkyrimMod.CreateFromBinary(ModPath.FromPath(pluginPath), SkyrimRelease.SkyrimSE);
        }
        catch (Exception ex)
        {
            Check("plugin re-reads with Mutagen", false, ex.Message);
            return Finish(checks);
        }

        Check("plugin re-reads with Mutagen", true, read.ModKey.FileName.String);
        Check("ESL (Small) flag set", read.ModHeader.Flags.HasFlag(SkyrimModHeader.HeaderFlag.Small), read.ModHeader.Flags.ToString());
        Check("IsSmallMaster", read.IsSmallMaster, read.IsSmallMaster);
        Check("form version 44", read.ModHeader.FormVersion == 44, read.ModHeader.FormVersion);
        var masters = read.ModHeader.MasterReferences.Select(m => m.Master.FileName.String).ToList();
        // Mutagen writes the master list sorted by file name (Dragonborn.esm before Skyrim.esm here), so the
        // order on disk is not the order the records were declared in. The engine resolves each master by
        // name and the plugin's own FormIDs use the index of *this* list, so only the set has to match.
        Check("masters as declared (set)", masters.Count == expectedMasters.Count
            && masters.ToHashSet(StringComparer.OrdinalIgnoreCase).SetEquals(expectedMasters),
            $"declared=[{string.Join(", ", expectedMasters)}] written=[{string.Join(", ", masters)}]");

        IMajorRecordGetter? Rec(uint id)
        {
            var key = new FormKey(Defaults.PatchKey, id);
            if (read.MagicEffects.TryGetValue(key, out var m)) return m;
            if (read.Spells.TryGetValue(key, out var s)) return s;
            if (read.Quests.TryGetValue(key, out var q)) return q;
            if (read.EffectShaders.TryGetValue(key, out var e)) return e;
            return null;
        }

        foreach (var (id, kind) in new (uint, string)[]
                 {
                     (Id.MainQuest, "QUST"), (Id.WidgetQuest, "QUST"), (Id.RageReadyAbility, "SPEL"), (Id.RageReadyEffect, "MGEF"),
                     (Id.ActivationRequestSpell, "SPEL"), (Id.ActivationRequestEffect, "MGEF"), (Id.FullRageGlow, "EFSH"),
                 })
        {
            var rec = Rec(id);
            var actual = rec switch
            {
                IQuestGetter => "QUST",
                ISpellGetter => "SPEL",
                IMagicEffectGetter => "MGEF",
                IEffectShaderGetter => "EFSH",
                null => null,
                _ => rec.GetType().Name,
            };
            Check($"0x{id:X} present as {kind}", actual == kind, actual ?? "missing");
        }

        // ---- magic effects
        if (read.MagicEffects.TryGetValue(new FormKey(Defaults.PatchKey, Id.RageReadyEffect), out var ready))
        {
            Check("0x803 archetype Script", (ready.Archetype as MagicEffectArchetype)?.Type == MagicEffectArchetype.TypeEnum.Script, (ready.Archetype as MagicEffectArchetype)?.Type.ToString());
            Check("0x803 constant + self", ready.CastType == CastType.ConstantEffect && ready.TargetType == TargetType.Self, $"{ready.CastType}/{ready.TargetType}");
            Check("0x803 hidden, not hostile/detrimental",
                ready.Flags.HasFlag(MagicEffect.Flag.HideInUI) && !ready.Flags.HasFlag(MagicEffect.Flag.Hostile) && !ready.Flags.HasFlag(MagicEffect.Flag.Detrimental),
                ready.Flags.ToString());
            Check("0x803 no visuals", (ready.HitShader is null || ready.HitShader.IsNull) && (ready.EnchantShader is null || ready.EnchantShader.IsNull)
                                      && (ready.HitEffectArt is null || ready.HitEffectArt.IsNull) && (ready.CastingArt is null || ready.CastingArt.IsNull),
                "hit/enchant/cast art all null");
            Check("0x803 has no script", ready.VirtualMachineAdapter is null, ready.VirtualMachineAdapter is null);
        }
        if (read.MagicEffects.TryGetValue(new FormKey(Defaults.PatchKey, Id.ActivationRequestEffect), out var request))
        {
            Check("0x805 archetype Script", (request.Archetype as MagicEffectArchetype)?.Type == MagicEffectArchetype.TypeEnum.Script, (request.Archetype as MagicEffectArchetype)?.Type.ToString());
            Check("0x805 fire and forget + self", request.CastType == CastType.FireAndForget && request.TargetType == TargetType.Self, $"{request.CastType}/{request.TargetType}");
            Check("0x805 hidden, no magnitude/area/duration",
                request.Flags.HasFlag(MagicEffect.Flag.HideInUI) && request.Flags.HasFlag(MagicEffect.Flag.NoMagnitude)
                && request.Flags.HasFlag(MagicEffect.Flag.NoArea),
                request.Flags.ToString());
        }

        // ---- spells
        if (read.Spells.TryGetValue(new FormKey(Defaults.PatchKey, Id.RageReadyAbility), out var ability))
        {
            Check("0x802 ability/constant/self", ability.Type == SpellType.Ability && ability.CastType == CastType.ConstantEffect && ability.TargetType == TargetType.Self,
                $"{ability.Type}/{ability.CastType}/{ability.TargetType}");
            var links = (ability.Effects ?? []).Select(e => e.BaseEffect?.FormKey).ToList();
            Check("0x802 uses only 0x803", links.Count == 1 && links[0] == new FormKey(Defaults.PatchKey, Id.RageReadyEffect),
                string.Join(",", links.Select(l => l?.ToString())));
        }
        if (read.Spells.TryGetValue(new FormKey(Defaults.PatchKey, Id.ActivationRequestSpell), out var spell))
        {
            Check("0x804 spell/fire-and-forget/self", spell.Type == SpellType.Spell && spell.CastType == CastType.FireAndForget && spell.TargetType == TargetType.Self,
                $"{spell.Type}/{spell.CastType}/{spell.TargetType}");
            Check("0x804 free (cost 0)", spell.BaseCost == 0, spell.BaseCost);
            var effect = (spell.Effects ?? []).FirstOrDefault();
            Check("0x804 uses 0x805 for 1 second",
                (spell.Effects ?? []).Count == 1 && effect?.BaseEffect?.FormKey == new FormKey(Defaults.PatchKey, Id.ActivationRequestEffect) && effect?.Data?.Duration == 1,
                $"effects={(spell.Effects ?? []).Count} duration={effect?.Data?.Duration}");
            Check("0x804 not flagged PCStartSpell", !spell.Flags.HasFlag(SpellDataFlag.PCStartSpell), spell.Flags.ToString());
        }

        // ---- effect shader copy
        var sourceShader = ReadSourceShader(dataDir);
        if (read.EffectShaders.TryGetValue(new FormKey(Defaults.PatchKey, Id.FullRageGlow), out var glow))
        {
            Check("0x806 editor id", glow.EditorID == "KR_FullRageGlow", glow.EditorID);
            if (sourceShader is not null)
            {
                var same = AssetPath(glow.FillTexture) == AssetPath(sourceShader.FillTexture)
                           && AssetPath(glow.ParticleShaderTexture) == AssetPath(sourceShader.ParticleShaderTexture)
                           && AssetPath(glow.ParticlePaletteTexture) == AssetPath(sourceShader.ParticlePaletteTexture)
                           && AssetPath(glow.MembranePaletteTexture) == AssetPath(sourceShader.MembranePaletteTexture)
                           && AssetPath(glow.HolesTexture) == AssetPath(sourceShader.HolesTexture)
                           && AssetPath(glow.FillColorKey1) == AssetPath(sourceShader.FillColorKey1);
                Check("0x806 keeps the source textures/colors (no new assets)", same, AssetPath(glow.FillTexture));
                Check("0x806 is a standalone record (no spell/armor references it)",
                    read.Spells.All(s => (s.Effects ?? []).All(e => e.BaseEffect?.FormKey != glow.FormKey))
                    && read.MagicEffects.All(m => m.HitShader?.FormKey != glow.FormKey && m.EnchantShader?.FormKey != glow.FormKey),
                    "no reference from any record in this plugin");
            }
            else
            {
                Check("Dragonborn source EFSH available for comparison", false, "Dragonborn.esm 0x021733 not readable");
            }
        }

        // ---- quests
        if (read.Quests.TryGetValue(new FormKey(Defaults.PatchKey, Id.MainQuest), out var main))
        {
            Check("0x800 start-game enabled", main.Flags.HasFlag(Quest.Flag.StartGameEnabled), main.Flags.ToString());
            Check("0x800 editor id + name", main.EditorID == "KR_MainQuest", main.EditorID);
            var alias = (main.Aliases ?? []).FirstOrDefault();
            Check("0x800 alias 0 KR_PlayerAlias forced to PlayerRef",
                alias is not null && alias.ID == 0 && alias.Name == "KR_PlayerAlias"
                && alias.ForcedReference?.FormKey == new FormKey(Defaults.SkyrimKey, PatchBuilder.PlayerRef),
                $"id={alias?.ID} name={alias?.Name} forced={alias?.ForcedReference?.FormKey}");
            var adapter = main.VirtualMachineAdapter;
            Check("0x800 VMAD script KR_Config", adapter?.Scripts is { Count: 1 } && adapter.Scripts[0].Name == "KR_Config",
                string.Join(",", adapter?.Scripts.Select(s => s.Name) ?? Array.Empty<string>()));
            var widgetProp = (adapter?.Scripts ?? []).FirstOrDefault()?.Properties?.OfType<IScriptObjectPropertyGetter>().FirstOrDefault(p => p.Name == "Widget");
            Check("0x800 KR_Config.Widget -> 0x801",
                widgetProp?.Object?.FormKey == new FormKey(Defaults.PatchKey, Id.WidgetQuest), widgetProp?.Object?.FormKey.ToString());
            var aliasEntry = (adapter?.Aliases ?? []).FirstOrDefault();
            Check("0x800 alias VMAD entry points at alias 0", aliasEntry?.Property?.Alias == 0, aliasEntry?.Property?.Alias);
            // Vanilla writes the owning quest's own FormID into the alias entry's object union (all 2075
            // alias entries in Skyrim.esm do), so the entry must not be left with a null form id.
            Check("0x800 alias VMAD entry names its own quest (vanilla self-reference)",
                aliasEntry?.Property?.Object?.FormKey == new FormKey(Defaults.PatchKey, Id.MainQuest),
                aliasEntry?.Property?.Object?.FormKey.ToString());
            var aliasScript = (aliasEntry?.Scripts ?? []).FirstOrDefault();
            Check("0x800 alias script KR_PlayerAlias", aliasScript?.Name == "KR_PlayerAlias", aliasScript?.Name);
            var expected = new Dictionary<string, uint>
            {
                ["Config"] = Id.MainQuest,
                ["Widget"] = Id.WidgetQuest,
                ["FullGlow"] = Id.FullRageGlow,
            };
            foreach (var (propertyName, target) in expected)
            {
                var prop = (aliasScript?.Properties ?? []).OfType<IScriptObjectPropertyGetter>().FirstOrDefault(p => p.Name == propertyName);
                // A form-valued object property carries alias -1 plus the form's own id at object format 2;
                // alias 0 would make the engine resolve "alias 0 of the id in that field" instead.
                Check($"0x800 KR_PlayerAlias.{propertyName} -> 0x{target:X} (form value, alias -1)",
                    prop is not null && prop.Alias == -1 && prop.Object?.FormKey == new FormKey(Defaults.PatchKey, target),
                    $"alias={prop?.Alias} object={prop?.Object?.FormKey}");
            }
        }
        if (read.Quests.TryGetValue(new FormKey(Defaults.PatchKey, Id.WidgetQuest), out var widget))
        {
            Check("0x801 start-game enabled", widget.Flags.HasFlag(Quest.Flag.StartGameEnabled), widget.Flags.ToString());
            Check("0x801 VMAD script KR_Widget",
                widget.VirtualMachineAdapter?.Scripts is { Count: 1 } && widget.VirtualMachineAdapter.Scripts[0].Name == "KR_Widget",
                string.Join(",", widget.VirtualMachineAdapter?.Scripts.Select(s => s.Name) ?? Array.Empty<string>()));
            // SKI_WidgetBase (vendor/skyui-sdk) declares WidgetName with a default and assigns it in
            // OnWidgetInit; Modes/HAnchor/VAnchor/X/Y/Alpha are script-backed properties that cannot be
            // written from a VMAD. So the widget script must carry no property at all.
            Check("0x801 KR_Widget needs no VMAD properties (SkyUI SDK defaults + OnWidgetInit)",
                (widget.VirtualMachineAdapter?.Scripts ?? []).All(s => (s.Properties ?? []).Count == 0),
                $"properties={(widget.VirtualMachineAdapter?.Scripts ?? []).Sum(s => (s.Properties ?? []).Count)}");
        }

        // ---- every script the VMAD points at must actually ship next to the plugin
        var scriptsDir = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(pluginPath))!, "Scripts");
        var vmadScriptNames = read.Quests
            .SelectMany(q => (q.VirtualMachineAdapter?.Scripts ?? [])
                .Select(s => s.Name)
                .Concat((q.VirtualMachineAdapter?.Aliases ?? [])
                    .SelectMany(a => (a.Scripts ?? []).Select(s => s.Name))))
            .Distinct()
            .OrderBy(n => n, StringComparer.Ordinal)
            .ToList();
        Check("VMAD scripts found in the plugin", vmadScriptNames.Count == 3,
            string.Join(", ", vmadScriptNames));
        foreach (var name in vmadScriptNames)
        {
            Check($"VMAD script {name} has a compiled script in Scripts/", File.Exists(Path.Combine(scriptsDir, name + ".pex")),
                name + ".pex");
        }

        // ---- seq
        try
        {
            var seq = File.ReadAllBytes(seqPath);
            var masterCount = masters.Count;
            var expectedSeq = new[] { ((uint)masterCount << 24) | Id.MainQuest, ((uint)masterCount << 24) | Id.WidgetQuest };
            var values = new List<uint>();
            for (var i = 0; i + 4 <= seq.Length; i += 4) values.Add(BitConverter.ToUInt32(seq, i));
            Check("seq is a bare uint32 array with one entry per start-game quest",
                seq.Length == 8 && seq.Length % 4 == 0, $"{seq.Length} bytes");
            Check("seq lists 0x800 and 0x801 in the contract's own plugin index",
                values.Count == 2 && values[0] == expectedSeq[0] && values[1] == expectedSeq[1],
                string.Join(", ", values.Select(v => "0x" + v.ToString("X8"))));
        }
        catch (Exception ex)
        {
            Check("seq readable", false, ex.Message);
        }

        return Finish(checks);
    }

    private static Dictionary<string, object?> Finish(List<object?> checks)
    {
        var failed = checks.Cast<Dictionary<string, object?>>().Count(c => c["passed"] is false);
        return new Dictionary<string, object?>
        {
            ["passed"] = failed == 0,
            ["failed"] = failed,
            ["total"] = checks.Count,
            ["checks"] = checks,
        };
    }

    private static IEffectShaderGetter? ReadSourceShader(string dataDir)
    {
        var dbPath = Path.Combine(dataDir, Defaults.DragonbornKey.FileName.String);
        if (!File.Exists(dbPath)) return null;
        using var db = SkyrimMod.CreateFromBinaryOverlay(ModPath.FromPath(dbPath), SkyrimRelease.SkyrimSE);
        return db.EffectShaders.TryGetValue(new FormKey(Defaults.DragonbornKey, PatchBuilder.FullGlowSource), out var shader) ? shader : null;
    }

    private static string? AssetPath(object? assetLink)
    {
        if (assetLink is null) return null;
        var type = assetLink.GetType();
        var prop = type.GetProperty("GivenPath") ?? type.GetProperty("Path");
        return prop?.GetValue(assetLink)?.ToString();
    }
}
