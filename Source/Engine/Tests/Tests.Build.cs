// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using System.IO;
using Flax.Build;
using Flax.Build.NativeCpp;

/// <summary>
/// Engine tests module.
/// </summary>
public class Tests : EngineModule
{
    /// <inheritdoc />
    public Tests()
    {
        Deploy = false;
    }

    /// <inheritdoc />
    public override void Setup(BuildOptions options)
    {
        base.Setup(options);

        options.CompileEnv.PreprocessorDefinitions.Add("COMPILE_WITH_TESTS=1");
        options.CompileEnv.PreprocessorDefinitions.Add("COMPILE_WITH_ASSETS_IMPORTER=1");
        options.PrivateDependencies.Add("CSG");
        options.PrivateDependencies.Add("ModelTool");
        options.PrivateDependencies.Add("TextureTool");
        options.PrivateDependencies.Add("Audio");
        options.PrivateDependencies.Add("ShadersCompilation");

        // FMOD-specific tests are compiled only when the SDK is available. The
        // generic/no-FMOD test target continues to build without this branch.
        string fmodRoot = System.Environment.GetEnvironmentVariable("FMOD_SDK_DIR");
        if (string.IsNullOrEmpty(fmodRoot) || !Directory.Exists(fmodRoot))
            fmodRoot = System.Environment.GetEnvironmentVariable("FMOD_DIR");
        if (string.IsNullOrEmpty(fmodRoot) || !Directory.Exists(fmodRoot))
            fmodRoot = Path.Combine(Globals.EngineRoot, "Source", "ThirdParty", "FMOD");
        if ((!Directory.Exists(fmodRoot)) && options.Platform.Target == TargetPlatform.Windows)
            fmodRoot = @"C:\Program Files (x86)\FMOD SoundSystem\FMOD Studio API Windows";
        bool disableFmod = string.Equals(System.Environment.GetEnvironmentVariable("FLAX_AUDIO_DISABLE_FMOD"), "1", System.StringComparison.OrdinalIgnoreCase) ||
                           string.Equals(System.Environment.GetEnvironmentVariable("FLAX_AUDIO_DISABLE_FMOD"), "true", System.StringComparison.OrdinalIgnoreCase);
        if (!disableFmod && Directory.Exists(fmodRoot))
        {
            options.CompileEnv.PreprocessorDefinitions.Add("AUDIO_EVENT_API_FMOD");
            options.PrivateIncludePaths.Add(Path.Combine(fmodRoot, "api", "core", "inc"));
            options.PrivateIncludePaths.Add(Path.Combine(fmodRoot, "api", "studio", "inc"));
        }
    }

    /// <inheritdoc />
    public override void GetFilesToDeploy(List<string> files)
    {
    }
}
