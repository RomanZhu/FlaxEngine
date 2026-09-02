// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using System.IO;
using Flax.Build;
using Flax.Build.NativeCpp;

/// <summary>
/// Content module.
/// </summary>
public class Content : EngineModule
{
    /// <inheritdoc />
    public override void Setup(BuildOptions options)
    {
        base.Setup(options);

        options.PrivateDependencies.Add("lz4");
        options.PrivateDependencies.Add("AudioTool");
        options.PrivateDependencies.Add("TextureTool");
        options.PrivateDependencies.Add("ModelTool");
        options.PrivateDependencies.Add("Particles");

        if (options.Target.IsEditor)
        {
            options.PrivateDependencies.Add("ShadersCompilation");
            options.PrivateDependencies.Add("MaterialGenerator");
            options.PrivateDependencies.Add("ContentImporters");
            options.PrivateDependencies.Add("ContentExporters");
            options.PrivateDependencies.Add("Graphics");
        }
        else
        {
            // Player targets consume only cooked packages and the runtime GUID catalog. Keep
            // source databases, importers, authoring documents, and build processors out of
            // both native compilation and managed bindings.
            options.SourcePaths.Clear();
            AddSources(options, FolderPath, SearchOption.TopDirectoryOnly);
            AddSources(options, Path.Combine(FolderPath, "Assets"));
            AddSources(options, Path.Combine(FolderPath, "Builtin"));
            AddSources(options, Path.Combine(FolderPath, "Cache"));
            AddSources(options, Path.Combine(FolderPath, "Factories"));
            AddSources(options, Path.Combine(FolderPath, "Loading"));
            AddSources(options, Path.Combine(FolderPath, "Storage"));
            AddSources(options, Path.Combine(FolderPath, "Artifacts"), SearchOption.TopDirectoryOnly, "ArtifactCompatibility.h", "ArtifactKey.cpp", "ArtifactKey.h", "ArtifactLease.cpp", "ArtifactLease.h", "ArtifactTarget.cpp", "ArtifactTarget.h", "ResolvedArtifact.cpp", "ResolvedArtifact.h");
            AddSources(options, Path.Combine(FolderPath, "AssetDatabase"), SearchOption.TopDirectoryOnly, "AssetPath.cpp", "AssetPath.h", "DurableAssetFileSystem.cpp", "DurableAssetFileSystem.h");
            AddSources(options, Path.Combine(FolderPath, "AssetDatabase", "Identity"), SearchOption.TopDirectoryOnly, "AssetGuid.cs", "AssetGuid.h", "AssetIdentitySerialization.cpp", "AssetIdentitySerialization.h", "AssetObjectId.h", "GlobalAssetObjectId.cs", "GlobalAssetObjectId.h");
            AddSources(options, Path.Combine(FolderPath, "AssetPipeline"), SearchOption.TopDirectoryOnly, "AssetPipelineDiagnostics.cpp", "AssetPipelineDiagnostics.h", "AssetPipelineSettings.cpp", "AssetPipelineSettings.h");
            AddSources(options, Path.Combine(FolderPath, "Build"), SearchOption.TopDirectoryOnly, "CookedContentGeneration.cpp", "CookedContentGeneration.h", "RuntimeAssetCatalog.cpp", "RuntimeAssetCatalog.h");
        }
    }

    private static void AddSources(BuildOptions options, string path, SearchOption searchOption = SearchOption.AllDirectories, params string[] files)
    {
        if (files.Length == 0)
        {
            options.SourceFiles.AddRange(Directory.GetFiles(path, "*", searchOption));
            return;
        }

        foreach (var file in files)
            options.SourceFiles.Add(Path.Combine(path, file));
    }

    /// <inheritdoc />
    public override void GetFilesToDeploy(List<string> files)
    {
        files.AddRange(Directory.GetFiles(FolderPath, "*.h", SearchOption.TopDirectoryOnly));
        files.AddRange(Directory.GetFiles(Path.Combine(FolderPath, "Assets"), "*.h", SearchOption.TopDirectoryOnly));
        files.AddRange(Directory.GetFiles(Path.Combine(FolderPath, "Cache"), "*.h", SearchOption.TopDirectoryOnly));
        files.AddRange(Directory.GetFiles(Path.Combine(FolderPath, "Factories"), "*.h", SearchOption.TopDirectoryOnly));
        files.AddRange(Directory.GetFiles(Path.Combine(FolderPath, "Storage"), "*.h", SearchOption.TopDirectoryOnly));
    }
}
