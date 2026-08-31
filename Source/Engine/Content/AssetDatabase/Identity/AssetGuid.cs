// Copyright (c) Wojciech Figat. All rights reserved.

using System;

namespace FlaxEngine
{
    [Serializable]
    partial struct AssetGuid : IEquatable<AssetGuid>
    {
        /// <summary>Initializes a persistent source identifier.</summary>
        public AssetGuid(Guid value)
        {
            Value = value;
        }

        /// <summary>Gets whether this identifier names a source asset.</summary>
        public bool IsValid => Value != Guid.Empty;

        /// <summary>Parses the canonical 32-character source identifier.</summary>
        public static bool TryParse(string value, out AssetGuid result)
        {
            result = default;
            if (value == null || value.Length != 32)
                return false;
            for (int i = 0; i < value.Length; i++)
            {
                if (!Uri.IsHexDigit(value[i]))
                    return false;
            }
            var guid = Json.JsonSerializer.ParseID(value);
            result = new AssetGuid(guid);
            return result.IsValid;
        }

        /// <inheritdoc />
        public bool Equals(AssetGuid other)
        {
            return Value == other.Value;
        }

        /// <inheritdoc />
        public override bool Equals(object obj)
        {
            return obj is AssetGuid other && Equals(other);
        }

        /// <inheritdoc />
        public override int GetHashCode()
        {
            return Value.GetHashCode();
        }

        public static bool operator ==(AssetGuid left, AssetGuid right) => left.Equals(right);
        public static bool operator !=(AssetGuid left, AssetGuid right) => !left.Equals(right);

        /// <inheritdoc />
        public override string ToString()
        {
            return Json.JsonSerializer.GetStringID(Value);
        }
    }
}
