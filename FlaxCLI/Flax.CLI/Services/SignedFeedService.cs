// Copyright (c) Wojciech Figat. All rights reserved.

using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Flax.CLI.Core;

namespace Flax.CLI.Services;

internal sealed class SignedFeedService
{
    public JsonObject ReadManifest(string path)
    {
        if (!File.Exists(path))
            throw new CliException(ExitCode.ContextRequired, "FLX-FEED-0004", $"Feed manifest '{path}' does not exist.");
        try
        {
            var root = JsonNode.Parse(File.ReadAllText(path)) as JsonObject;
            return root ?? throw new InvalidDataException("The feed manifest must be a JSON object.");
        }
        catch (JsonException ex)
        {
            throw new CliException(ExitCode.Usage, "FLX-FEED-0002", "The feed manifest is not valid JSON.", new { exception = ex.Message });
        }
    }

    public FeedVerification Verify(JsonObject manifest, string signaturePath, string publicKeyPath)
    {
        if (!File.Exists(signaturePath) || !File.Exists(publicKeyPath))
            throw new CliException(ExitCode.ContextRequired, "FLX-FEED-0004", "The detached signature and public-key files are required.");
        var signature = ReadSignature(File.ReadAllText(signaturePath));
        var keyText = File.ReadAllText(publicKeyPath);
        using var rsa = RSA.Create();
        try { rsa.ImportFromPem(keyText); }
        catch (Exception ex) { throw new CliException(ExitCode.Usage, "FLX-FEED-KEY-0002", "The public key is not a valid PEM RSA key.", new { exception = ex.Message }); }
        var canonical = Canonicalize(manifest);
        var valid = rsa.VerifyData(Encoding.UTF8.GetBytes(canonical), signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        var fingerprint = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(keyText))).ToLowerInvariant();
        return new FeedVerification(valid, fingerprint, canonical.Length, DateTime.UtcNow);
    }

    public static string Canonicalize(JsonNode node)
    {
        var options = new JsonSerializerOptions { WriteIndented = false };
        return CanonicalNode(node).ToJsonString(options);
    }

    public void Install(JsonObject manifest, string entryId, string destination, bool confirm)
    {
        if (!confirm)
            throw new CliException(ExitCode.Authorization, "FLX-FEED-CONFIRM-0004", "Installing a feed entry requires --yes.");
        var entries = manifest["entries"] as JsonArray ?? throw new CliException(ExitCode.Usage, "FLX-FEED-0002", "The feed manifest has no entries array.");
        var entry = entries.OfType<JsonObject>().FirstOrDefault(x => string.Equals(x["id"]?.GetValue<string>(), entryId, StringComparison.OrdinalIgnoreCase));
        if (entry == null)
            throw new CliException(ExitCode.ContextRequired, "FLX-FEED-ENTRY-0004", $"Feed entry '{entryId}' was not found.");
        var archive = entry["archive"]?.GetValue<string>() ?? throw new CliException(ExitCode.Usage, "FLX-FEED-0002", "Feed entry has no archive path.");
        archive = Path.GetFullPath(archive, Path.GetDirectoryName(destination) ?? Environment.CurrentDirectory);
        if (!File.Exists(archive))
            throw new CliException(ExitCode.ContextRequired, "FLX-FEED-0004", $"Feed archive '{archive}' does not exist.");
        var expectedHash = entry["sha256"]?.GetValue<string>();
        if (!string.IsNullOrWhiteSpace(expectedHash))
        {
            var actual = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(archive))).ToLowerInvariant();
            if (!actual.Equals(expectedHash.Trim().ToLowerInvariant(), StringComparison.Ordinal))
                throw new CliException(ExitCode.Authorization, "FLX-FEED-HASH-0003", "The feed archive SHA-256 does not match the signed manifest.", new { expected = expectedHash, actual });
        }
        var target = Path.GetFullPath(destination);
        Directory.CreateDirectory(target);
        var staging = target + ".staging-" + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(staging);
        try
        {
            using var zip = ZipFile.OpenRead(archive);
            foreach (var item in zip.Entries)
            {
                var path = Path.GetFullPath(Path.Combine(staging, item.FullName));
                if (!path.StartsWith(staging + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                    throw new CliException(ExitCode.Authorization, "FLX-FEED-ZIP-0003", "The feed archive contains an unsafe path.");
                if (string.IsNullOrEmpty(item.Name)) { Directory.CreateDirectory(path); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                item.ExtractToFile(path, true);
            }
            foreach (var source in Directory.EnumerateFileSystemEntries(staging))
            {
                var destinationPath = Path.Combine(target, Path.GetFileName(source));
                if (Directory.Exists(source))
                {
                    Directory.CreateDirectory(destinationPath);
                    CopyDirectory(source, destinationPath);
                }
                else File.Copy(source, destinationPath, true);
            }
        }
        finally { try { Directory.Delete(staging, true); } catch { } }
    }

    private static JsonNode CanonicalNode(JsonNode node) => node switch
    {
        JsonObject obj => new JsonObject(obj.OrderBy(x => x.Key, StringComparer.Ordinal).ToDictionary(x => x.Key, x => x.Value == null ? null : CanonicalNode(x.Value), StringComparer.Ordinal)),
        JsonArray array => new JsonArray(array.Select(x => x == null ? null : CanonicalNode(x)).ToArray()),
        _ => node.DeepClone(),
    };

    private static byte[] ReadSignature(string text)
    {
        text = text.Trim();
        try
        {
            if (text.StartsWith("{", StringComparison.Ordinal))
                text = JsonNode.Parse(text)?["signature"]?.GetValue<string>() ?? text;
        }
        catch { }
        try { return Convert.FromBase64String(text); }
        catch { }
        try { return Convert.FromHexString(text); }
        catch (Exception ex) { throw new CliException(ExitCode.Usage, "FLX-FEED-SIGNATURE-0002", "The detached signature must be base64, hex, or a JSON signature field.", new { exception = ex.Message }); }
    }

    private static void CopyDirectory(string source, string destination)
    {
        foreach (var file in Directory.EnumerateFiles(source)) File.Copy(file, Path.Combine(destination, Path.GetFileName(file)), true);
        foreach (var dir in Directory.EnumerateDirectories(source)) { var child = Path.Combine(destination, Path.GetFileName(dir)); Directory.CreateDirectory(child); CopyDirectory(dir, child); }
    }
}

internal sealed record FeedVerification(bool Valid, string PublicKeyFingerprint, int CanonicalBytes, DateTime VerifiedUtc);
