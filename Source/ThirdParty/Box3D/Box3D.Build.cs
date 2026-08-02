// Copyright (c) Wojciech Figat. All rights reserved.

using System.IO;
using Flax.Build;
using Flax.Build.NativeCpp;

/// <summary>
/// https://github.com/erincatto/box3d
/// </summary>
public class Box3D : ThirdPartyModule
{
    /// <inheritdoc />
    public override void Init()
    {
        base.Init();

        LicenseType = LicenseTypes.MIT;
        LicenseFilePath = "LICENSE";

        // Merge third-party modules into engine binary
        BinaryModuleName = "FlaxEngine";
    }

    /// <inheritdoc />
    public override void Setup(BuildOptions options)
    {
        base.Setup(options);

        options.SourcePaths.Clear();
        options.SourceFiles.Clear();
        foreach (var file in Directory.GetFiles(Path.Combine(FolderPath, "src"), "*.c", SearchOption.TopDirectoryOnly))
            options.SourceFiles.Add(file);

        options.PublicIncludePaths.Add(Path.Combine(FolderPath, "include"));
        options.PrivateIncludePaths.Add(Path.Combine(FolderPath, "src"));
        options.PublicDefinitions.Add("COMPILE_WITH_BOX3D");

        if (EngineConfiguration.WithLargeWorlds(options))
            options.PublicDefinitions.Add("BOX3D_DOUBLE_PRECISION");
    }
}
