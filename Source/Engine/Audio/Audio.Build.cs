// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using System.IO;
using Flax.Build;
using Flax.Build.NativeCpp;

/// <summary>
/// Audio module.
/// </summary>
public class Audio : EngineModule
{
    /// <inheritdoc />
    public override void Setup(BuildOptions options)
    {
        base.Setup(options);

        options.SourcePaths.Clear();
        options.SourceFiles.AddRange(Directory.GetFiles(FolderPath, "*.*", SearchOption.TopDirectoryOnly));
        options.SourcePaths.Add(Path.Combine(FolderPath, "Events"));
        options.SourcePaths.Add(Path.Combine(FolderPath, "Diagnostics"));

        var depsRoot = options.DepsFolder;

        bool useNone = true;
        bool useOpenAL = false;
        bool useXAudio2 = false;

        // Audio Event Subsystem
        options.CompileEnv.PreprocessorDefinitions.Add("COMPILE_WITH_AUDIO_EVENTS");
        options.CompileEnv.PreprocessorDefinitions.Add("AUDIO_EVENT_API_NONE");

        // Resolve FMOD Studio SDK
        string fmodRoot = System.Environment.GetEnvironmentVariable("FMOD_SDK_DIR");
        if (string.IsNullOrEmpty(fmodRoot) || !Directory.Exists(fmodRoot))
            fmodRoot = System.Environment.GetEnvironmentVariable("FMOD_DIR");
        if (string.IsNullOrEmpty(fmodRoot) || !Directory.Exists(fmodRoot))
            fmodRoot = Path.Combine(Globals.EngineRoot, "Source", "ThirdParty", "FMOD");
        if (string.IsNullOrEmpty(fmodRoot) || !Directory.Exists(fmodRoot))
        {
            var defaultWindowsFmod = @"C:\Program Files (x86)\FMOD SoundSystem\FMOD Studio API Windows";
            if (Directory.Exists(defaultWindowsFmod))
                fmodRoot = defaultWindowsFmod;
        }

        bool disableFmod = string.Equals(System.Environment.GetEnvironmentVariable("FLAX_AUDIO_DISABLE_FMOD"), "1", System.StringComparison.OrdinalIgnoreCase) ||
                           string.Equals(System.Environment.GetEnvironmentVariable("FLAX_AUDIO_DISABLE_FMOD"), "true", System.StringComparison.OrdinalIgnoreCase);
        // This integration currently deploys the FMOD Studio Windows SDK layout.
        // Keep other targets on the backend-neutral Null path until their native
        // SDK libraries and runtime deployment rules are explicitly supplied.
        bool hasFmod = !disableFmod && options.Platform.Target == TargetPlatform.Windows &&
                       !string.IsNullOrEmpty(fmodRoot) && Directory.Exists(fmodRoot);
        if (hasFmod)
        {
            options.SourcePaths.Add(Path.Combine(FolderPath, "FMOD"));
            options.CompileEnv.PreprocessorDefinitions.Add("AUDIO_EVENT_API_FMOD");

            options.PrivateIncludePaths.Add(Path.Combine(fmodRoot, "api", "core", "inc"));
            options.PrivateIncludePaths.Add(Path.Combine(fmodRoot, "api", "studio", "inc"));

            switch (options.Platform.Target)
            {
            case TargetPlatform.Windows:
            {
                var coreLibDir = Path.Combine(fmodRoot, "api", "core", "lib", "x64");
                var studioLibDir = Path.Combine(fmodRoot, "api", "studio", "lib", "x64");
                bool useLogging = options.Configuration == TargetConfiguration.Debug || options.Configuration == TargetConfiguration.Development;

                if (useLogging)
                {
                    options.OutputFiles.Add(Path.Combine(coreLibDir, "fmodL_vc.lib"));
                    options.OutputFiles.Add(Path.Combine(studioLibDir, "fmodstudioL_vc.lib"));
                    options.DependencyFiles.Add(Path.Combine(coreLibDir, "fmodL.dll"));
                    options.DependencyFiles.Add(Path.Combine(studioLibDir, "fmodstudioL.dll"));
                }
                else
                {
                    options.OutputFiles.Add(Path.Combine(coreLibDir, "fmod_vc.lib"));
                    options.OutputFiles.Add(Path.Combine(studioLibDir, "fmodstudio_vc.lib"));
                    options.DependencyFiles.Add(Path.Combine(coreLibDir, "fmod.dll"));
                    options.DependencyFiles.Add(Path.Combine(studioLibDir, "fmodstudio.dll"));
                }
                break;
            }
            default:
                break;
            }
        }

        switch (options.Platform.Target)
        {
        case TargetPlatform.Windows:
            useNone = true;
            useOpenAL = true;
            //useXAudio2 = true;
            break;
        case TargetPlatform.XboxOne:
        case TargetPlatform.UWP:
        case TargetPlatform.XboxScarlett:
            useXAudio2 = true;
            break;
        case TargetPlatform.Linux:
            useOpenAL = true;
            break;
        case TargetPlatform.PS4:
            options.SourcePaths.Add(Path.Combine(Globals.EngineRoot, "Source", "Platforms", "PS4", "Engine", "Audio"));
            options.CompileEnv.PreprocessorDefinitions.Add("AUDIO_API_PS4");
            break;
        case TargetPlatform.Android:
            useOpenAL = true;
            break;
        case TargetPlatform.Switch:
            options.SourcePaths.Add(Path.Combine(Globals.EngineRoot, "Source", "Platforms", "Switch", "Engine", "Audio"));
            options.CompileEnv.PreprocessorDefinitions.Add("AUDIO_API_SWITCH");
            break;
        case TargetPlatform.PS5:
            options.SourcePaths.Add(Path.Combine(Globals.EngineRoot, "Source", "Platforms", "PS5", "Engine", "Audio"));
            options.CompileEnv.PreprocessorDefinitions.Add("AUDIO_API_PS5");
            break;
        case TargetPlatform.Mac:
            useOpenAL = true;
            break;
        case TargetPlatform.iOS:
            useOpenAL = true;
            break;
        case TargetPlatform.Web:
            useOpenAL = true;
            break;
        default: throw new InvalidPlatformException(options.Platform.Target);
        }

        if (useNone)
        {
            options.SourcePaths.Add(Path.Combine(FolderPath, "None"));
            options.CompileEnv.PreprocessorDefinitions.Add("AUDIO_API_NONE");
        }

        if (useOpenAL)
        {
            options.SourcePaths.Add(Path.Combine(FolderPath, "OpenAL"));
            options.CompileEnv.PreprocessorDefinitions.Add("AUDIO_API_OPENAL");

            switch (options.Platform.Target)
            {
            case TargetPlatform.Windows:
            case TargetPlatform.UWP:
                options.OutputFiles.Add(Path.Combine(depsRoot, "OpenAL32.lib"));
                options.DependencyFiles.Add(Path.Combine(depsRoot, "OpenAL32.dll"));
                break;
            case TargetPlatform.Linux:
                options.OutputFiles.Add(Path.Combine(depsRoot, "libopenal.a"));
                break;
            case TargetPlatform.Android:
                options.OutputFiles.Add(Path.Combine(depsRoot, "libopenal.a"));
                options.Libraries.Add("OpenSLES");
                break;
            case TargetPlatform.Mac:
                options.OutputFiles.Add(Path.Combine(depsRoot, "libopenal.a"));
                options.Libraries.Add("CoreAudio.framework");
                options.Libraries.Add("AudioUnit.framework");
                options.Libraries.Add("AudioToolbox.framework");
                break;
            case TargetPlatform.iOS:
                options.OutputFiles.Add(Path.Combine(depsRoot, "libopenal.a"));
                options.Libraries.Add("CoreAudio.framework");
                options.Libraries.Add("AudioToolbox.framework");
                break;
            case TargetPlatform.Web:
                options.LinkEnv.CustomArgs.Add("-lopenal");
                break;
            default: throw new InvalidPlatformException(options.Platform.Target);
            }
        }

        if (useXAudio2)
        {
            options.SourcePaths.Add(Path.Combine(FolderPath, "XAudio2"));
            options.CompileEnv.PreprocessorDefinitions.Add("AUDIO_API_XAUDIO2");

            switch (options.Platform.Target)
            {
            case TargetPlatform.Windows:
            case TargetPlatform.XboxOne:
            case TargetPlatform.UWP:
            case TargetPlatform.XboxScarlett:
                options.OutputFiles.Add("xaudio2.lib");
                break;
            default: throw new InvalidPlatformException(options.Platform.Target);
            }
        }

        options.PrivateDependencies.Add("AudioTool");
    }

    /// <inheritdoc />
    public override void GetFilesToDeploy(List<string> files)
    {
        files.AddRange(Directory.GetFiles(FolderPath, "*.h", SearchOption.AllDirectories));
    }
}
