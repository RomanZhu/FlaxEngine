// Copyright (c) Wojciech Figat. All rights reserved.

namespace Flax.CLI.Core;

internal readonly record struct SemanticVersion(int Major, int Minor, int Patch, int Build = 0) : IComparable<SemanticVersion>
{
    public int CompareTo(SemanticVersion other)
    {
        var result = Major.CompareTo(other.Major);
        if (result != 0) return result;
        result = Minor.CompareTo(other.Minor);
        if (result != 0) return result;
        result = Patch.CompareTo(other.Patch);
        return result != 0 ? result : Build.CompareTo(other.Build);
    }

    public override string ToString() => Build == 0 ? $"{Major}.{Minor}.{Patch}" : $"{Major}.{Minor}.{Patch}.{Build}";

    public static bool TryParse(string? value, out SemanticVersion version)
    {
        version = default;
        if (string.IsNullOrWhiteSpace(value))
            return false;
        var components = value.Split('.', '-', '+');
        if (components.Length < 2 || !int.TryParse(components[0], out var major) || !int.TryParse(components[1], out var minor))
            return false;
        var patch = components.Length > 2 && int.TryParse(components[2], out var parsedPatch) ? parsedPatch : 0;
        var build = components.Length > 3 && int.TryParse(components[3], out var parsedBuild) ? parsedBuild : 0;
        version = new SemanticVersion(major, minor, patch, build);
        return true;
    }

    public static bool operator >(SemanticVersion left, SemanticVersion right) => left.CompareTo(right) > 0;
    public static bool operator <(SemanticVersion left, SemanticVersion right) => left.CompareTo(right) < 0;
    public static bool operator >=(SemanticVersion left, SemanticVersion right) => left.CompareTo(right) >= 0;
    public static bool operator <=(SemanticVersion left, SemanticVersion right) => left.CompareTo(right) <= 0;
}
