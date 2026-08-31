// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "PreparedAsset.h"
#include "AssetProcessor.h"
#include "Engine/Content/AssetDatabase/SourceHashCache.h"
#include "Engine/Core/NonCopyable.h"

/// <summary>Controlled, recording source access used by processor Prepare callbacks.</summary>
struct FLAXENGINE_API PrepareAssetContext : public NonCopyable
{
private:
    String _projectRoot;
    String _contentRoot;
    String _libraryRoot;
    AssetRecord _record;
    AssetProcessorDescriptor _descriptor;
    StringAnsi _settings;
    SourceHashCache* _hashCache;
    AssetCancellationToken _cancellation;
    uint64 _maximumSourceBytes;
    int32 _maximumInputs;
    uint64 _sourceBytesRead = 0;
    Array<AssetDependency> _declaredDependencies;
    Array<DeclaredArtifactOutput> _declaredOutputs;
    Array<AssetPipelineDiagnostic> _diagnostics;

public:
    PrepareAssetContext(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
        const AssetRecord& record, const AssetProcessorDescriptor& descriptor, const StringAnsiView& normalizedSettings,
        SourceHashCache& hashCache, const AssetCancellationToken& cancellation, uint64 maximumSourceBytes = 1024ull * 1024ull * 1024ull, int32 maximumInputs = 4096);

    const AssetRecord& GetRecord() const
    {
        return _record;
    }
    const StringAnsi& GetSettings() const
    {
        return _settings;
    }
    const AssetCancellationToken& GetCancellation() const
    {
        return _cancellation;
    }
    const Array<AssetPipelineDiagnostic>& GetDiagnostics() const
    {
        return _diagnostics;
    }

    /// <summary>Reads and records one canonical Content file. Returns true on failure.</summary>
    bool ReadSourceFile(const StringView& path, Array<byte>& data, ContentHash& hash, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic);

    bool DeclareBuildInput(const StringView& stableIdentity, const AssetObjectId& id, const ArtifactKey& exactArtifact, const AssetSemanticInterface& semanticInterface, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic);
    bool DeclareRuntimeReference(const StringView& stableIdentity, const AssetObjectId& id, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic);
    bool DeclareToolchain(const StringView& stableIdentity, const ContentHash& semanticIdentity, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic);
    bool DeclareOutput(const StringAnsiView& kind, const Guid& effectiveAssetId, AssetPipelineDiagnostic& diagnostic);
    void ReportDiagnostic(const AssetPipelineDiagnostic& diagnostic);

    /// <summary>Validates and seals the deterministic result against the current database revision.</summary>
    bool Finalize(uint64 currentDatabaseRevision, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic);

private:
    bool CheckCancellation(AssetPipelineDiagnostic& diagnostic) const;
    bool AddDependency(AssetDependency dependency, AssetPipelineDiagnostic& diagnostic);
};
