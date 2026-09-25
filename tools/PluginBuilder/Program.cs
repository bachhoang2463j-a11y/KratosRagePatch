using System.Text.Encodings.Web;
using System.Text.Json;
using Mutagen.Bethesda.Plugins;

namespace PluginBuilder;

/// <summary>
/// Command line entry point.
///
///   PluginBuilder analyze --report &lt;json&gt; [--gow &lt;esp&gt;] [--data &lt;dir&gt;] [--refs "Plugin.esp=ID,..."] [--dust "Plugin.esp=ID,..."]
///   PluginBuilder build   --stage &lt;dir&gt;  [--data &lt;dir&gt;]
///   PluginBuilder all     --stage &lt;dir&gt;  --report &lt;json&gt; ...
///
/// --refs / --dust default to the forms named by the interface contract (see Defaults.ContractRefs).
/// Reads the original mod and the game install read-only. Writes only to --report and --stage.
/// </summary>
public static class Program
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public static int Main(string[] rawArgs)
    {
        try
        {
            var args = Arguments.Parse(rawArgs);
            var command = args.Command;
            var gow = args.Get("gow", Defaults.GodOfWar);
            var data = args.Get("data", Defaults.GameData);
            var report = args.Get("report", Defaults.Report);
            var stage = args.Get("stage", Defaults.Stage);

            if (command is "analyze" or "all")
            {
                var refs = ParseForms(args.Get("refs", Defaults.ContractRefs));
                var dust = ParseForms(args.Get("dust", Defaults.ContractDust));
                var result = Analyzer.Run(new Analyzer.Inputs(gow, data, refs, dust));
                WriteJson(report, result);
                Console.WriteLine($"analyze: wrote {Path.GetFullPath(report)} ({result.Count} top level sections)");
            }

            if (command is "build" or "all")
            {
                var result = PatchBuilder.Run(new PatchBuilder.Inputs(gow, data, stage));
                WriteJson(Path.Combine(stage, "..", "PluginBuilder", "build-report.json"), result);
                Console.WriteLine($"build: wrote {Path.GetFullPath(PatchBuilder.PluginPath(stage))}");
            }

            return 0;
        }
        catch (ToolException ex)
        {
            Console.Error.WriteLine($"error: {ex.Message}");
            return 2;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"unexpected failure: {ex}");
            return 1;
        }
    }

    private static void WriteJson(string path, object value)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        File.WriteAllText(path, JsonSerializer.Serialize(value, JsonOptions) + Environment.NewLine);
    }

    private static List<(ModKey Plugin, uint LocalId)> ParseForms(string spec)
    {
        var list = new List<(ModKey, uint)>();
        foreach (var item in spec.Split(new[] { ',', ';' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var parts = item.Split('=', 2);
            if (parts.Length != 2) throw new ToolException($"bad --refs entry '{item}', expected Plugin.esp=0xID");
            var idText = parts[1].Trim();
            var id = Convert.ToUInt32(idText.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? idText[2..] : idText, 16);
            list.Add((ModKey.FromNameAndExtension(parts[0].Trim()), id));
        }
        return list;
    }

    private sealed class Arguments
    {
        public string Command { get; private set; } = "all";
        private readonly Dictionary<string, string> _values = new(StringComparer.OrdinalIgnoreCase);

        public static Arguments Parse(string[] raw)
        {
            var result = new Arguments();
            var i = 0;
            if (raw.Length > 0 && !raw[0].StartsWith('-'))
            {
                result.Command = raw[0].ToLowerInvariant();
                i = 1;
            }
            for (; i < raw.Length; i++)
            {
                var token = raw[i];
                if (!token.StartsWith("--")) throw new ToolException($"unexpected argument '{token}'");
                var key = token[2..];
                if (i + 1 >= raw.Length) throw new ToolException($"--{key} needs a value");
                result._values[key] = raw[++i];
            }
            if (result.Command is not ("analyze" or "build" or "all"))
                throw new ToolException($"unknown command '{result.Command}' (expected analyze|build|all)");
            return result;
        }

        public string Get(string key, string fallback) => _values.TryGetValue(key, out var v) ? v : fallback;
    }
}
