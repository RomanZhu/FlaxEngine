// Copyright (c) Wojciech Figat. All rights reserved.

namespace Flax.CLI.Core;

internal static class CommandLine
{
    public static GlobalOptions Parse(string[] args)
    {
        var result = new GlobalOptions();
        result.OriginalArgs.AddRange(args);
        for (var index = 0; index < args.Length; index++)
        {
            var token = args[index];
            if (token == "--")
            {
                result.PassThrough.AddRange(args[(index + 1)..]);
                break;
            }

            switch (token)
            {
            case "--help" or "-h":
                result.Help = true;
                break;
            case "--version" or "-V":
                result.Version = true;
                break;
            case "--json":
                result.Format = "json";
                result.FormatSpecified = true;
                break;
            case "--quiet":
                result.Quiet = true;
                break;
            case "--verbose":
                result.Verbose = true;
                break;
            case "--no-color":
                result.NoColor = true;
                break;
            case "--non-interactive":
                result.NonInteractive = true;
                break;
            case "--trace":
                result.Trace = true;
                break;
            case "--format":
                result.Format = RequireValue(args, ref index, token);
                result.FormatSpecified = true;
                break;
            case "--project":
                result.Project = RequireValue(args, ref index, token);
                break;
            case "--engine":
                result.Engine = RequireValue(args, ref index, token);
                break;
            case "--timeout":
                var timeout = RequireValue(args, ref index, token);
                if (!double.TryParse(timeout, out var seconds) || seconds <= 0)
                    throw Usage($"Invalid timeout '{timeout}'. Expected a positive number of seconds.");
                result.Timeout = TimeSpan.FromSeconds(seconds);
                break;
            default:
                result.CommandTokens.Add(token);
                break;
            }
        }

        if (!new[] { "human", "tsv", "json", "ndjson" }.Contains(result.Format, StringComparer.OrdinalIgnoreCase))
            throw Usage($"Unknown output format '{result.Format}'.");
        result.Format = result.Format.ToLowerInvariant();
        return result;
    }

    private static string RequireValue(string[] args, ref int index, string option)
    {
        if (++index >= args.Length)
            throw Usage($"Option {option} requires a value.");
        return args[index];
    }

    public static CliException Usage(string message) => new(ExitCode.Usage, "FLX-CLI-0002", message);
}

internal sealed class CommandArguments(IEnumerable<string> tokens)
{
    private readonly List<string> _tokens = [.. tokens];

    public bool Flag(string name)
    {
        var index = _tokens.FindIndex(x => string.Equals(x, name, StringComparison.OrdinalIgnoreCase));
        if (index < 0)
            return false;
        _tokens.RemoveAt(index);
        return true;
    }

    public string? Option(string name)
    {
        for (var index = 0; index < _tokens.Count; index++)
        {
            if (_tokens[index].StartsWith(name + "=", StringComparison.OrdinalIgnoreCase))
            {
                var value = _tokens[index][(name.Length + 1)..];
                _tokens.RemoveAt(index);
                return value;
            }
            if (!string.Equals(_tokens[index], name, StringComparison.OrdinalIgnoreCase))
                continue;
            if (index + 1 >= _tokens.Count || _tokens[index + 1].StartsWith("--", StringComparison.Ordinal))
                throw CommandLine.Usage($"Option {name} requires a value.");
            var result = _tokens[index + 1];
            _tokens.RemoveRange(index, 2);
            return result;
        }
        return null;
    }

    public List<string> Options(string name)
    {
        var result = new List<string>();
        string? value;
        while ((value = Option(name)) != null)
            result.Add(value);
        return result;
    }

    public string? Positional()
    {
        var index = _tokens.FindIndex(x => !x.StartsWith("-", StringComparison.Ordinal));
        if (index < 0)
            return null;
        var result = _tokens[index];
        _tokens.RemoveAt(index);
        return result;
    }

    public List<string> Positionals()
    {
        var result = new List<string>();
        string? value;
        while ((value = Positional()) != null)
            result.Add(value);
        return result;
    }

    public List<string> TakeRemaining()
    {
        var result = _tokens.ToList();
        _tokens.Clear();
        return result;
    }

    public void Complete()
    {
        if (_tokens.Count != 0)
            throw CommandLine.Usage($"Unknown argument '{_tokens[0]}'.");
    }
}
