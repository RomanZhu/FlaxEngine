// Copyright (c) Wojciech Figat. All rights reserved.

using Flax.Build;
using Flax.Build.NativeCpp;

/// <summary>
/// https://www.sqlite.org/
/// </summary>
public class sqlite : ThirdPartyModule
{
    /// <inheritdoc />
    public override void Init()
    {
        base.Init();

        LicenseType = LicenseTypes.Custom;
        LicenseFilePath = "SQLite public domain.txt";

        // Merge third-party modules into engine binary.
        BinaryModuleName = "FlaxEngine";
    }

    /// <inheritdoc />
    public override void Setup(BuildOptions options)
    {
        base.Setup(options);

        options.CompileEnv.PreprocessorDefinitions.Add("SQLITE_DQS=0");
        options.CompileEnv.PreprocessorDefinitions.Add("SQLITE_OMIT_LOAD_EXTENSION=1");
        options.CompileEnv.PreprocessorDefinitions.Add("SQLITE_THREADSAFE=1");
        options.CompileEnv.PreprocessorDefinitions.Add("SQLITE_USE_URI=0");
    }
}
