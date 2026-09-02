// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactTarget.h"
#include "Engine/Content/AssetDatabase/Identity/AssetObjectId.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Core/NonCopyable.h"
#include "Engine/Core/Types/Span.h"

class FileBase;

enum class AssetImportDependencyKind : byte
{
    SourceAsset,
    SourceFile,
    ImportedObject,
    ImportedArtifact,
    CustomDependency,
    FolderContents,
    SearchQuery,
    Toolchain,
    ImporterAssembly,
    BuildTarget,
    ProjectSetting,
    LogicalPath,
};

struct FLAXENGINE_API AssetImportDependency
{
    AssetImportDependencyKind Kind = AssetImportDependencyKind::SourceFile;
    AssetObjectId Object;
    String Identity;
    ContentHash ExpectedHash;
    ArtifactKey ExactArtifact;
    String Origin;
};

struct FLAXENGINE_API AssetImportedObjectDeclaration
{
    String StableIdentifier;
    String TypeName;
    String DisplayName;
    bool IsMain = false;
};

struct FLAXENGINE_API AssetImportOutputDeclaration
{
    String Name;
    StringAnsi Kind;
    StringAnsi Extension;
    ArtifactTargetDimension TargetDimensions = ArtifactTargetDimension::All;
    String RelativePath;
    String StagingPath;
    uint64 Size = 0;
    ContentHash Hash;
    bool Completed = false;
};

struct FLAXENGINE_API AssetImportContextResult
{
    Array<AssetImportedObjectDeclaration> Objects;
    Array<AssetImportOutputDeclaration> Outputs;
    Array<AssetImportDependency> Dependencies;
    Array<AssetPipelineDiagnostic> Diagnostics;
    int32 MainObject = -1;
};

using AssetImportReadCallback = Function<bool(const StringView&, Array<byte>&, ContentHash&, AssetPipelineDiagnostic&)>;

/// <summary>Importer-facing declaration and controlled input-read surface.</summary>
class FLAXENGINE_API AssetImportContext : public NonCopyable
{
    AssetGuid _asset;
    String _sourcePath;
    ArtifactTarget _target;
    StringAnsi _settings;
    AssetImportReadCallback _read;
    AssetImportContextResult _result;
    struct OutputStreamState
    {
        FileBase* Writer = nullptr;
        ContentHasher Hasher;
    };
    String _outputStagingPath;
    Array<OutputStreamState> _outputStreams;
    uint64 _maximumOutputBytes;
    int32 _maximumOutputFiles;
    uint64 _outputBytesWritten = 0;
    bool _completed = false;

public:
    AssetImportContext(const AssetGuid& asset, const StringView& sourcePath, const ArtifactTarget& target,
                       const StringAnsiView& settings, AssetImportReadCallback read, const StringView& outputStagingPath,
                       uint64 maximumOutputBytes, int32 maximumOutputFiles);
    ~AssetImportContext();

    const AssetGuid& GetAsset() const { return _asset; }
    const String& GetSourcePath() const { return _sourcePath; }
    const ArtifactTarget& GetTarget() const { return _target; }
    const StringAnsi& GetSettings() const { return _settings; }
    const Array<AssetPipelineDiagnostic>& GetDiagnostics() const { return _result.Diagnostics; }

    bool ReadSource(Array<byte>& data, ContentHash& hash, AssetPipelineDiagnostic& diagnostic);
    bool ReadDependencyFile(const StringView& path, Array<byte>& data, ContentHash& hash, AssetPipelineDiagnostic& diagnostic);
    void DependsOnSourceAsset(const AssetGuid& asset, const StringView& origin = StringView::Empty);
    void DependsOnSourceAsset(const AssetObjectId& object, const StringView& origin = StringView::Empty);
    void DependsOnArtifact(const AssetObjectId& object, const StringView& origin = StringView::Empty);
    void DependsOnArtifact(const ArtifactKey& artifact, const StringView& origin = StringView::Empty);
    void DependsOnCustomDependency(const StringView& name, const ContentHash& value, const StringView& origin = StringView::Empty);
    void DependsOnFolderContents(const StringView& path, const ContentHash& value, const StringView& origin = StringView::Empty);
    void DependsOnSearchQuery(const StringView& query, const ContentHash& value, const StringView& origin = StringView::Empty);
    void DependsOnToolchain(const StringView& name, const ContentHash& value, const StringView& origin = StringView::Empty);
    void DependsOnProjectSetting(const StringView& name, const ContentHash& value, const StringView& origin = StringView::Empty);
    void DependsOnLogicalPath(const StringView& origin = StringView::Empty);

    int32 AddObjectToAsset(const StringView& stableIdentifier, const StringView& typeName, const StringView& displayName = StringView::Empty);
    int32 CreateOutput(const StringView& name, const StringAnsiView& kind, const StringAnsiView& extension,
                       ArtifactTargetDimension targetDimensions = ArtifactTargetDimension::All);
    bool WriteOutput(int32 outputIndex, const Span<byte>& data, AssetPipelineDiagnostic& diagnostic);
    bool CompleteOutput(int32 outputIndex, AssetPipelineDiagnostic& diagnostic);
    bool SetMainObject(int32 objectIndex, AssetPipelineDiagnostic& diagnostic);
    void AddDiagnostic(const AssetPipelineDiagnostic& diagnostic);
    bool Complete(bool requireMainObject, AssetImportContextResult& result, AssetPipelineDiagnostic& diagnostic);

private:
    void AddNamedDependency(AssetImportDependencyKind kind, const StringView& identity, const ContentHash& value, const StringView& origin);
};
