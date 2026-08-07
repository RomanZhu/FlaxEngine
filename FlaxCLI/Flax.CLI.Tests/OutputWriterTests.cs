// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json;
using Flax.CLI.Core;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class OutputWriterTests
{
    [Test]
    public void JsonUsesTheStableResultEnvelope()
    {
        using var stdout = new StringWriter();
        using var stderr = new StringWriter();
        var options = new GlobalOptions { Format = "json" };

        new OutputWriter(stdout, stderr).Write("engines", CliResult.Ok(new { count = 2 }), options, TimeSpan.FromMilliseconds(15), "request");

        using var document = JsonDocument.Parse(stdout.ToString());
        Assert.That(document.RootElement.GetProperty("schemaVersion").GetString(), Is.EqualTo("1.0"));
        Assert.That(document.RootElement.GetProperty("success").GetBoolean(), Is.True);
        Assert.That(document.RootElement.GetProperty("command").GetString(), Is.EqualTo("engines"));
        Assert.That(document.RootElement.GetProperty("data").GetProperty("count").GetInt32(), Is.EqualTo(2));
        Assert.That(stderr.ToString(), Is.Empty);
    }

    [Test]
    public void TsvStripsControlCharacters()
    {
        using var stdout = new StringWriter();
        var options = new GlobalOptions { Format = "tsv" };

        new OutputWriter(stdout, TextWriter.Null).Write("projects", CliResult.Ok(new[] { new { name = "unsafe\u001b[31m" } }), options, TimeSpan.Zero, "request");

        var output = stdout.ToString();
        Assert.That(output.IndexOf('\u001b'), Is.EqualTo(-1), string.Join(',', output.Select(x => (int)x)));
    }

    [Test]
    public void NdjsonWritesBufferedEventsBeforeTheResult()
    {
        using var stdout = new StringWriter();
        var result = CliResult.Ok(new { done = true });
        result.Events.Add(new { type = "progress", value = 0.5 });

        new OutputWriter(stdout, TextWriter.Null).Write("command", result, new GlobalOptions { Format = "ndjson" }, TimeSpan.Zero, "request");

        var lines = stdout.ToString().Split(Environment.NewLine, StringSplitOptions.RemoveEmptyEntries);
        Assert.That(lines, Has.Length.EqualTo(2));
        using var progress = JsonDocument.Parse(lines[0]);
        using var final = JsonDocument.Parse(lines[1]);
        Assert.That(progress.RootElement.GetProperty("type").GetString(), Is.EqualTo("progress"));
        Assert.That(final.RootElement.GetProperty("type").GetString(), Is.EqualTo("result"));
    }
}
