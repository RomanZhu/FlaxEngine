// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Globalization;
using System.Threading;
using System.Threading.Tasks;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>Controls whether recently deleted identities participate in a path lookup.</summary>
    [Flags]
    public enum AssetPathToGUIDOptions
    {
        /// <summary>Queries live records only.</summary>
        OnlyExistingAssets = 0,
        /// <summary>Also queries identities retained by the current database session.</summary>
        IncludeRecentlyDeletedAssets = 1,
    }

    /// <summary>Controls canonical source reserialization.</summary>
    [Flags]
    public enum ForceReserializeAssetsOptions
    {
        /// <summary>Rewrites authored source documents.</summary>
        ReserializeAssets = 1,
        /// <summary>Also canonicalizes adjacent metadata.</summary>
        ReserializeMetadata = 2,
    }

    /// <summary>A deterministic 128-bit content hash used by managed asset APIs.</summary>
    [Serializable]
    public struct Hash128 : IEquatable<Hash128>
    {
        /// <summary>The upper 64 bits.</summary>
        public ulong High;
        /// <summary>The lower 64 bits.</summary>
        public ulong Low;

        /// <summary>Creates a hash from its two words.</summary>
        public Hash128(ulong high, ulong low)
        {
            High = high;
            Low = low;
        }

        /// <summary>Returns true when all bits are zero.</summary>
        public bool IsZero => High == 0 && Low == 0;

        /// <summary>Parses 32 hexadecimal digits.</summary>
        public static bool TryParse(string value, out Hash128 result)
        {
            result = default;
            if (value == null || value.Length != 32 ||
                !ulong.TryParse(value.Substring(0, 16), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var high) ||
                !ulong.TryParse(value.Substring(16, 16), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var low))
                return false;
            result = new Hash128(high, low);
            return true;
        }

        /// <inheritdoc />
        public bool Equals(Hash128 other) => High == other.High && Low == other.Low;

        /// <inheritdoc />
        public override bool Equals(object obj) => obj is Hash128 other && Equals(other);

        /// <inheritdoc />
        public override int GetHashCode() => unchecked((High.GetHashCode() * 397) ^ Low.GetHashCode());

        /// <inheritdoc />
        public override string ToString() => High.ToString("x16", CultureInfo.InvariantCulture) + Low.ToString("x16", CultureInfo.InvariantCulture);

        /// <summary>Compares two hashes.</summary>
        public static bool operator ==(Hash128 left, Hash128 right) => left.Equals(right);
        /// <summary>Compares two hashes.</summary>
        public static bool operator !=(Hash128 left, Hash128 right) => !left.Equals(right);
    }

    /// <summary>Identifies one immutable artifact generation.</summary>
    [Serializable]
    public struct ArtifactKey : IEquatable<ArtifactKey>
    {
        /// <summary>The exact artifact digest.</summary>
        public Hash128 Hash;

        /// <summary>Creates an artifact key.</summary>
        public ArtifactKey(Hash128 hash)
        {
            Hash = hash;
        }

        /// <summary>Returns true when the key is usable.</summary>
        public bool IsValid => !Hash.IsZero;

        /// <inheritdoc />
        public bool Equals(ArtifactKey other) => Hash == other.Hash;

        /// <inheritdoc />
        public override bool Equals(object obj) => obj is ArtifactKey other && Equals(other);

        /// <inheritdoc />
        public override int GetHashCode() => Hash.GetHashCode();

        /// <inheritdoc />
        public override string ToString() => Hash.ToString();
    }

    /// <summary>Immutable public information about a published artifact.</summary>
    public sealed class ArtifactInfo
    {
        /// <summary>Persistent source identity.</summary>
        public string Guid { get; internal set; }
        /// <summary>Exact artifact key.</summary>
        public ArtifactKey Key { get; internal set; }
        /// <summary>Target profile used by the artifact.</summary>
        public Content.Settings.BuildTarget Target { get; internal set; }
        /// <summary>Whether this generation is current for the desired inputs.</summary>
        public bool IsCurrent { get; internal set; }
        /// <summary>Monotonic database revision observed by this result.</summary>
        public ulong DatabaseRevision { get; internal set; }
    }

    /// <summary>Managed request for an exact asset-object load.</summary>
    public sealed class AssetLoadRequest
    {
        private readonly Task<FlaxEngine.Object> _task;

        internal AssetLoadRequest(AssetObjectId id, Type expectedType, CancellationToken cancellationToken)
        {
            ObjectId = id;
            _task = System.Threading.Tasks.Task.Run(() =>
            {
                cancellationToken.ThrowIfCancellationRequested();
                var obj = AssetDatabase.LoadAsset(id, expectedType);
                if (obj is Asset asset && asset.WaitForLoaded() == false)
                    throw new InvalidOperationException("The asset failed to load.");
                cancellationToken.ThrowIfCancellationRequested();
                return (FlaxEngine.Object)obj;
            }, cancellationToken);
        }

        /// <summary>The persistent object requested by the caller.</summary>
        public AssetObjectId ObjectId { get; }
        /// <summary>The asynchronous load operation.</summary>
        public Task<FlaxEngine.Object> Task => _task;
        /// <summary>Returns true when the operation completed.</summary>
        public bool IsCompleted => _task.IsCompleted;
    }
}
