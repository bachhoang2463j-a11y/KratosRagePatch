using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

namespace PluginBuilder;

/// <summary>
/// Read-only inspection of the original mod's plugin and of the two vanilla masters the interface
/// contract touches. Produces the raw facts that <c>data/integration.json</c> is assembled from.
/// Nothing here writes to the mod, the game install or the MO2 profile.
/// </summary>
public static class Analyzer
{
    public sealed record Inputs(
        string GodOfWarPath,
        string DataDir,
        IReadOnlyList<(ModKey Plugin, uint LocalId)> Refs,
        IReadOnlyList<(ModKey Plugin, uint LocalId)> ExcludedDust);

    public static Dictionary<string, object?> Run(Inputs input)
    {
        if (!File.Exists(input.GodOfWarPath))
            throw new ToolException($"original plugin not found: {input.GodOfWarPath}");

        var gowPath = ModPath.FromPath(input.GodOfWarPath);
        var gowKey = ModKey.FromNameAndExtension(Path.GetFileName(input.GodOfWarPath));
        var gow = SkyrimMod.CreateFromBinary(gowPath, SkyrimRelease.SkyrimSE);

        // Load every master of the original plugin (plus the vanilla plugins the contract names) as a
        // lazy overlay so the link cache can resolve cross-plugin links without parsing 250MB eagerly.
        var masters = new List<ISkyrimModDisposableGetter>();
        var seen = new HashSet<ModKey> { gowKey };
        foreach (var master in gow.ModHeader.MasterReferences.Select(m => m.Master)
                     .Concat(new[] { Defaults.SkyrimKey, Defaults.DragonbornKey }))
        {
            if (!seen.Add(master)) continue;
            var path = Path.Combine(input.DataDir, master.FileName.String);
            if (File.Exists(path)) masters.Add(SkyrimMod.CreateFromBinaryOverlay(ModPath.FromPath(path), SkyrimRelease.SkyrimSE));
        }

        using var _ = new DisposableBag(masters);
        var allMods = new List<ISkyrimModGetter> { gow };
        allMods.AddRange(masters);
        var cache = allMods.ToImmutableLinkCache();
        var modsByKey = allMods.ToDictionary(m => m.ModKey, m => m);

        IMagicEffectGetter? Mgef(IFormLinkGetter<IMagicEffectGetter>? link)
            => link is null || link.IsNull ? null : cache.TryResolve<IMagicEffectGetter>(link.FormKey, out var m) ? m : null;

        IProjectileGetter? Proj(IFormLinkGetter<IProjectileGetter>? link)
            => link is null || link.IsNull ? null : cache.TryResolve<IProjectileGetter>(link.FormKey, out var p) ? p : null;

        var spells = gow.Spells.ToDictionary(s => s.FormKey, s => s);
        var mgefs = gow.MagicEffects.ToDictionary(m => m.FormKey, m => m);

        var report = new Dictionary<string, object?>
        {
            ["generatedBy"] = "tools/PluginBuilder (Mutagen.Bethesda.Skyrim 0.53.1, net8.0)",
            ["godOfWarPlugin"] = gowKey.FileName.String,
            ["godOfWarMasters"] = gow.ModHeader.MasterReferences.Select(m => m.Master.FileName.String).ToList(),
            ["godOfWarCounts"] = new Dictionary<string, object?>
            {
                ["spells"] = gow.Spells.Count,
                ["magicEffects"] = gow.MagicEffects.Count,
                ["effectShaders"] = gow.EffectShaders.Count,
                ["quests"] = gow.Quests.Count,
            },
        };

        // ---- 1. every form the original interface references, with its *actual* record type ----
        var referenced = new List<object?>();
        foreach (var (plugin, localId) in input.Refs)
            referenced.Add(DescribeReferenced(plugin, localId, cache, Mgef, Proj));
        report["referencedForms"] = referenced;

        // ---- 2. the original mod's own spell/magic-effect tables ----
        report["godOfWarSpells"] = gow.Spells
            .OrderBy(s => s.FormKey.ID)
            .Select(s => DescribeSpell(s, Mgef, Proj))
            .ToList();
        report["godOfWarMagicEffects"] = gow.MagicEffects
            .OrderBy(m => m.FormKey.ID)
            .Select(m => DescribeMagicEffect(m, Proj))
            .ToList();

        // ---- 3. direct-attack candidates of the original mod ----
        var dustKeys = input.ExcludedDust.Select(d => new FormKey(d.Plugin, d.LocalId)).ToHashSet();
        report["attackCandidates"] = gow.Spells
            .OrderBy(s => s.FormKey.ID)
            .Select(s => DescribeCandidate(s, Mgef, Proj, dustKeys))
            .ToList();

        // ---- 4. the vanilla effect shader that 0x806 copies ----
        report["effectShaderSource"] = DescribeEffectShaderSource(cache);

        // ---- 5. shapes to mirror when writing the new quests (taken from a vanilla quest) ----
        report["questTemplate"] = DescribeQuestTemplate(modsByKey, out var aliasTemplate);
        report["aliasScriptTemplate"] = aliasTemplate;

        report["playerRef"] = new Dictionary<string, object?>
        {
            ["formKey"] = new FormKey(Defaults.SkyrimKey, 0x000014).ToString(),
            ["resolved"] = cache.TryResolve<IPlacedGetter>(new FormKey(Defaults.SkyrimKey, 0x000014), out var player) ? player.EditorID ?? "(placed object)" : null,
        };

        return report;
    }

    // ------------------------------------------------------------------ referenced forms

    private static object? DescribeReferenced(
        ModKey plugin, uint localId, ILinkCache cache, Func<IFormLinkGetter<IMagicEffectGetter>?, IMagicEffectGetter?> mgef, Func<IFormLinkGetter<IProjectileGetter>?, IProjectileGetter?> proj)
    {
        var key = new FormKey(plugin, localId);
        var entry = new Dictionary<string, object?>
        {
            ["requested"] = $"{plugin.FileName.String}:0x{localId:X}",
            ["formKey"] = key.ToString(),
        };

        if (!cache.TryResolve<IMajorRecordGetter>(key, out var record) || record is null)
        {
            entry["found"] = false;
            entry["recordType"] = null;
            entry["note"] = "form id not present in the loaded plugins";
            return entry;
        }

        entry["found"] = true;
        entry["recordType"] = RecordTypeOf(record);
        entry["editorID"] = record.EditorID;
        entry["name"] = (record as ISpellGetter)?.Name?.String ?? (record as IMagicEffectGetter)?.Name?.String;
        entry["majorFlags"] = record.MajorRecordFlagsRaw;

        switch (record)
        {
            case ISpellGetter spell:
                entry["spell"] = DescribeSpell(spell, mgef, proj);
                break;
            case IMagicEffectGetter mf:
                entry["magicEffect"] = DescribeMagicEffect(mf, proj);
                break;
            case IEffectShaderGetter efsh:
                entry["effectShader"] = DescribeEffectShader(efsh);
                break;
        }
        return entry;
    }

    private static string RecordTypeOf(IMajorRecordGetter record)
    {
        // Mutagen's runtime type mapping - the exact record signature, never a guess.
        if (record is ISpellGetter) return "SPEL";
        if (record is IMagicEffectGetter) return "MGEF";
        if (record is IEffectShaderGetter) return "EFSH";
        if (record is IQuestGetter) return "QUST";
        if (record is IProjectileGetter) return "PROJ";
        if (record is IExplosionGetter) return "EXPL";
        if (record is IPlacedGetter) return "REFR";
        if (record is INpcGetter) return "NPC_";
        if (record is IKeywordGetter) return "KYWD";
        if (record is IWeaponGetter) return "WEAP";
        if (record is IArmorGetter) return "ARMO";
        if (record is IActivatorGetter) return "ACTI";
        var name = record.GetType().Name;
        return name.EndsWith("BinaryOverlay", StringComparison.Ordinal) ? name[..^"BinaryOverlay".Length] : name;
    }

    // ------------------------------------------------------------------ record shapes

    internal static Dictionary<string, object?> DescribeEffect(
        IEffectGetter effect, Func<IFormLinkGetter<IMagicEffectGetter>?, IMagicEffectGetter?> mgef, Func<IFormLinkGetter<IProjectileGetter>?, IProjectileGetter?> proj)
    {
        var mf = effect.BaseEffect is null ? null : mgef(effect.BaseEffect);
        return new Dictionary<string, object?>
        {
            ["baseEffect"] = effect.BaseEffect?.FormKey.ToString(),
            ["baseEffectEditorID"] = mf?.EditorID,
            ["baseEffectName"] = mf?.Name?.String,
            ["archetype"] = mf?.Archetype?.Type.ToString(),
            ["actorValue"] = mf?.Archetype is AMagicEffectArchetype arch ? arch.ActorValue.ToString() : null,
            ["projectile"] = mf?.Projectile is { IsNull: false } pl ? pl.FormKey.ToString() : null,
            ["projectileEditorID"] = proj(mf?.Projectile)?.EditorID,
            ["magnitude"] = effect.Data?.Magnitude,
            ["area"] = effect.Data?.Area,
            ["duration"] = effect.Data?.Duration,
            ["conditionCount"] = effect.Conditions?.Count,
        };
    }

    internal static Dictionary<string, object?> DescribeSpell(
        ISpellGetter spell, Func<IFormLinkGetter<IMagicEffectGetter>?, IMagicEffectGetter?> mgef, Func<IFormLinkGetter<IProjectileGetter>?, IProjectileGetter?> proj)
    {
        return new Dictionary<string, object?>
        {
            ["editorID"] = spell.EditorID,
            ["name"] = spell.Name?.String,
            ["type"] = spell.Type.ToString(),
            ["castType"] = spell.CastType.ToString(),
            ["targetType"] = spell.TargetType.ToString(),
            ["baseCost"] = spell.BaseCost,
            ["range"] = spell.Range,
            ["castDuration"] = spell.CastDuration,
            ["chargeTime"] = spell.ChargeTime,
            ["flags"] = spell.Flags.ToString(),
            ["keywords"] = (spell.Keywords ?? []).Select(k => k.FormKey.ToString()).ToList(),
            ["menuDisplayObject"] = spell.MenuDisplayObject is { IsNull: false } mdo ? mdo.FormKey.ToString() : null,
            ["effectCount"] = (spell.Effects ?? []).Count,
            ["effects"] = (spell.Effects ?? []).Select(e => DescribeEffect(e, mgef, proj)).ToList(),
        };
    }

    internal static Dictionary<string, object?> DescribeMagicEffect(IMagicEffectGetter mf, Func<IFormLinkGetter<IProjectileGetter>?, IProjectileGetter?> proj)
    {
        return new Dictionary<string, object?>
        {
            ["editorID"] = mf.EditorID,
            ["name"] = mf.Name?.String,
            ["archetype"] = mf.Archetype?.Type.ToString(),
            ["actorValue"] = mf.Archetype is AMagicEffectArchetype arch ? arch.ActorValue.ToString() : null,
            ["secondActorValue"] = mf.SecondActorValue.ToString(),
            ["castType"] = mf.CastType.ToString(),
            ["targetType"] = mf.TargetType.ToString(),
            ["baseCost"] = mf.BaseCost,
            ["flags"] = mf.Flags.ToString(),
            ["projectile"] = mf.Projectile is { IsNull: false } p ? p.FormKey.ToString() : null,
            ["projectileEditorID"] = proj(mf.Projectile)?.EditorID,
            ["explosion"] = mf.Explosion is { IsNull: false } e ? e.FormKey.ToString() : null,
            ["hitShader"] = mf.HitShader is { IsNull: false } hs ? hs.FormKey.ToString() : null,
            ["enchantShader"] = mf.EnchantShader is { IsNull: false } es ? es.FormKey.ToString() : null,
            ["hitEffectArt"] = mf.HitEffectArt is { IsNull: false } ha ? ha.FormKey.ToString() : null,
            ["castingArt"] = mf.CastingArt is { IsNull: false } ca ? ca.FormKey.ToString() : null,
            ["impactData"] = mf.ImpactData is { IsNull: false } ids ? ids.FormKey.ToString() : null,
            ["menuDisplayObject"] = mf.MenuDisplayObject is { IsNull: false } mdo ? mdo.FormKey.ToString() : null,
            ["minimumSkillLevel"] = mf.MinimumSkillLevel,
            ["conditionCount"] = (mf.Conditions ?? []).Count,
            ["keywordCount"] = (mf.Keywords ?? []).Count,
        };
    }

    internal static Dictionary<string, object?> DescribeEffectShader(IEffectShaderGetter efsh)
    {
        return new Dictionary<string, object?>
        {
            ["editorID"] = efsh.EditorID,
            ["flags"] = efsh.Flags.ToString(),
            ["fillTexture"] = efsh.FillTexture is null ? null : AssetPath(efsh.FillTexture),
            ["particleShaderTexture"] = efsh.ParticleShaderTexture is null ? null : AssetPath(efsh.ParticleShaderTexture),
            ["particlePaletteTexture"] = efsh.ParticlePaletteTexture is null ? null : AssetPath(efsh.ParticlePaletteTexture),
            ["membranePaletteTexture"] = efsh.MembranePaletteTexture is null ? null : AssetPath(efsh.MembranePaletteTexture),
            ["holesTexture"] = efsh.HolesTexture is null ? null : AssetPath(efsh.HolesTexture),
            ["addonModels"] = efsh.AddonModels is { IsNull: false } am ? am.FormKey.ToString() : null,
            ["ambientSound"] = efsh.AmbientSound is { IsNull: false } snd ? snd.FormKey.ToString() : null,
        };
    }

    private static string? AssetPath(object assetLink)
    {
        var t = assetLink.GetType();
        var prop = t.GetProperty("GivenPath") ?? t.GetProperty("Path");
        return prop?.GetValue(assetLink)?.ToString();
    }

    // ------------------------------------------------------------------ attack candidates

    private static Dictionary<string, object?> DescribeCandidate(
        ISpellGetter spell, Func<IFormLinkGetter<IMagicEffectGetter>?, IMagicEffectGetter?> mgef, Func<IFormLinkGetter<IProjectileGetter>?, IProjectileGetter?> proj, HashSet<FormKey> dust)
    {
        var effects = (spell.Effects ?? []).Select(e => DescribeEffect(e, mgef, proj)).ToList();
        var resolved = (spell.Effects ?? []).Select(e => e.BaseEffect is null ? null : mgef(e.BaseEffect)).Where(m => m is not null).Select(m => m!).ToList();

        var archetypes = resolved.Select(m => m.Archetype?.Type.ToString()).Where(a => a is not null).Distinct().ToList();
        var projectileKeys = resolved.Where(m => m.Projectile is { IsNull: false }).Select(m => m.Projectile!.FormKey).Distinct().ToList();
        var appliedToOthers = spell.TargetType is TargetType.Aimed or TargetType.TargetActor or TargetType.TargetLocation or TargetType.Touch;
        var isCloak = archetypes.Contains("Cloak");
        var isConcentration = spell.CastType == CastType.Concentration || resolved.Any(m => m.CastType == CastType.Concentration);
        var isDust = dust.Contains(spell.FormKey);
        var isDualCastOrPlayerOnly = false;
        _ = isDualCastOrPlayerOnly;

        string? exclusion =
            isCloak ? "cloak" :
            isConcentration ? "concentration" :
            isDust ? "dust" :
            null;

        string? channel = exclusion is not null ? null
            : projectileKeys.Count > 0 ? "SubmitProjectileHit"
            : appliedToOthers ? "SubmitMagicHit"
            : "SubmitWeaponHit";

        return new Dictionary<string, object?>
        {
            ["localID"] = $"0x{spell.FormKey.ID:X}",
            ["editorID"] = spell.EditorID,
            ["name"] = spell.Name?.String,
            ["recordType"] = "SPEL",
            ["spellType"] = spell.Type.ToString(),
            ["castType"] = spell.CastType.ToString(),
            ["targetType"] = spell.TargetType.ToString(),
            ["baseCost"] = spell.BaseCost,
            ["archetypes"] = archetypes,
            ["projectiles"] = projectileKeys.Select(k => k.ToString()).ToList(),
            ["projectileEditorIDs"] = resolved.Where(m => m.Projectile is { IsNull: false }).Select(m => proj(m.Projectile)?.EditorID).Where(x => x is not null).Distinct().ToList(),
            ["maxDuration"] = (spell.Effects ?? []).Count == 0 ? 0 : (spell.Effects ?? []).Max(e => e.Data?.Duration ?? 0),
            ["appliesToOthers"] = appliedToOthers,
            ["excluded"] = exclusion is not null,
            ["exclusionReason"] = exclusion,
            ["channel"] = channel,
            ["signals"] = new Dictionary<string, object?>
            {
                ["cloak"] = isCloak,
                ["concentration"] = isConcentration,
                ["dust"] = isDust,
                ["selfTargeted"] = spell.TargetType == TargetType.Self,
                ["hasProjectile"] = projectileKeys.Count > 0,
                ["isAbility"] = spell.Type == SpellType.Ability,
                ["isPower"] = spell.Type is SpellType.Power or SpellType.LesserPower,
            },
            ["effects"] = effects,
        };
    }

    // ------------------------------------------------------------------ copies

    private static Dictionary<string, object?> DescribeEffectShaderSource(ILinkCache cache)
    {
        var key = new FormKey(Defaults.DragonbornKey, 0x021733);
        var result = new Dictionary<string, object?> { ["requested"] = key.ToString() };
        if (!cache.TryResolve<IMajorRecordGetter>(key, out var record) || record is null)
        {
            result["found"] = false;
            return result;
        }
        result["found"] = true;
        result["recordType"] = RecordTypeOf(record);
        result["isEffectShader"] = record is IEffectShaderGetter;
        if (record is IEffectShaderGetter efsh)
        {
            result["effectShader"] = DescribeEffectShader(efsh);
            result["externalLinks"] = new List<string?>();
        }
        return result;
    }

    private static Dictionary<string, object?> DescribeQuestTemplate(IReadOnlyDictionary<ModKey, ISkyrimModGetter> modsByKey, out object? aliasTemplate)
    {
        aliasTemplate = null;
        if (!modsByKey.TryGetValue(Defaults.SkyrimKey, out var skyrim))
            return new Dictionary<string, object?> { ["found"] = false, ["note"] = "Skyrim.esm not loaded" };

        // vanilla quests whose VMAD carries alias scripts - the shape the new quests mirror
        foreach (var q in skyrim.Quests)
        {
            var adapter = q.VirtualMachineAdapter;
            if (adapter is null) continue;
            if ((adapter.Aliases ?? []).Count > 0)
            {
                aliasTemplate = DescribeQuestAdapter(adapter);
                return DescribeQuest(q);
            }
        }
        return new Dictionary<string, object?> { ["found"] = false, ["note"] = "no vanilla quest with alias scripts found" };
    }

    private static Dictionary<string, object?> DescribeQuest(IQuestGetter q) => new()
    {
        ["editorID"] = q.EditorID,
        ["formKey"] = q.FormKey.ToString(),
        ["flags"] = q.Flags.ToString(),
        ["priority"] = q.Priority,
        ["questFormVersion"] = q.QuestFormVersion,
        ["unknown"] = q.Unknown,
        ["type"] = q.Type.ToString(),
        ["nextAliasID"] = q.NextAliasID,
        ["aliasCount"] = (q.Aliases ?? []).Count,
        ["virtualMachineAdapter"] = q.VirtualMachineAdapter is null ? null : DescribeQuestAdapter(q.VirtualMachineAdapter),
    };

    private static Dictionary<string, object?> DescribeQuestAdapter(IQuestAdapterGetter adapter) => new()
    {
        ["version"] = adapter.Version,
        ["objectFormat"] = adapter.ObjectFormat,
        ["scripts"] = (adapter.Scripts ?? []).Select(DescribeScriptEntry).ToList(),
        ["aliasEntries"] = (adapter.Aliases ?? []).Select(a => new Dictionary<string, object?>
        {
            ["propertyName"] = a.Property?.Name,
            ["propertyAlias"] = a.Property?.Alias,
            ["propertyObjectFormKey"] = a.Property?.Object?.FormKey.ToString(),
            ["propertyFlags"] = a.Property?.Flags.ToString(),
            ["version"] = a.Version,
            ["objectFormat"] = a.ObjectFormat,
            ["scripts"] = (a.Scripts ?? []).Select(DescribeScriptEntry).ToList(),
        }).ToList(),
    };

    private static Dictionary<string, object?> DescribeScriptEntry(IScriptEntryGetter s) => new()
    {
        ["name"] = s.Name,
        ["flags"] = s.Flags.ToString(),
        ["properties"] = (s.Properties ?? []).Select(p => new Dictionary<string, object?>
        {
            ["name"] = p.Name,
            ["flags"] = p.Flags.ToString(),
            ["type"] = p switch
            {
                IScriptObjectPropertyGetter op => op.Object is null || op.Object.IsNull
                    ? (op.Alias != 0 ? $"Object(alias {op.Alias})" : "Object(null)")
                    : $"Object({op.Object.FormKey})",
                IScriptStringPropertyGetter sp => $"String(\"{sp.Data}\")",
                IScriptIntPropertyGetter ip => $"Int({ip.Data})",
                IScriptFloatPropertyGetter fp => $"Float({fp.Data})",
                _ => p.GetType().Name,
            },
        }).ToList(),
    };

    private sealed class DisposableBag : IDisposable
    {
        private readonly IEnumerable<IDisposable> _items;
        public DisposableBag(IEnumerable<IDisposable> items) => _items = items.ToList();
        public void Dispose()
        {
            foreach (var item in _items) item.Dispose();
        }
    }
}
