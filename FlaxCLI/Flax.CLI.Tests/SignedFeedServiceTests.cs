// Copyright (c) Wojciech Figat. All rights reserved.

using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Flax.CLI.Core;
using Flax.CLI.Services;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class SignedFeedServiceTests
{
    private string _root = null!;

    [SetUp]
    public void SetUp()
    {
        _root = Path.Combine(Path.GetTempPath(), "flax-cli-feed-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_root);
    }

    [TearDown]
    public void TearDown()
    {
        if (Directory.Exists(_root))
            Directory.Delete(_root, recursive: true);
    }

    [Test]
    public void VerifiesCanonicalManifestAndInstallsHashedArchive()
    {
        var service = new SignedFeedService();
        var archive = Path.Combine(_root, "engine.zip");
        using (var zip = ZipFile.Open(archive, ZipArchiveMode.Create))
        {
            var entry = zip.CreateEntry("bin/tool.txt");
            using var writer = new StreamWriter(entry.Open(), Encoding.UTF8);
            writer.Write("signed feed payload");
        }

        var hash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(archive))).ToLowerInvariant();
        var manifest = new JsonObject
        {
            ["schema"] = 1,
            ["entries"] = new JsonArray(new JsonObject { ["id"] = "engine", ["archive"] = archive, ["sha256"] = hash }),
        };
        using var rsa = RSA.Create(2048);
        var signature = rsa.SignData(Encoding.UTF8.GetBytes(SignedFeedService.Canonicalize(manifest)), HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        var signaturePath = Path.Combine(_root, "feed.sig");
        var keyPath = Path.Combine(_root, "feed.pem");
        File.WriteAllText(signaturePath, Convert.ToBase64String(signature));
        File.WriteAllText(keyPath, rsa.ExportSubjectPublicKeyInfoPem());

        var verification = service.Verify(manifest, signaturePath, keyPath);
        Assert.That(verification.Valid, Is.True);

        var destination = Path.Combine(_root, "install");
        service.Install(manifest, "engine", destination, confirm: true);
        Assert.That(File.ReadAllText(Path.Combine(destination, "bin", "tool.txt")), Is.EqualTo("signed feed payload"));
    }

    [Test]
    public void RejectsInstallWithoutExplicitConfirmation()
    {
        var service = new SignedFeedService();
        var manifest = new JsonObject
        {
            ["entries"] = new JsonArray(new JsonObject { ["id"] = "engine", ["archive"] = "missing.zip" }),
        };

        var error = Assert.Throws<CliException>(() => service.Install(manifest, "engine", Path.Combine(_root, "install"), confirm: false));

        Assert.That(error!.Code, Is.EqualTo("FLX-FEED-CONFIRM-0004"));
    }
}
