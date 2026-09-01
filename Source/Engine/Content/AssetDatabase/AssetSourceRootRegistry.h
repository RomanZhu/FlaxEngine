// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetPath.h"
#include "AssetSourceRoot.h"

/// <summary>Resolved source path together with its owning root policy.</summary>
struct FLAXENGINE_API ResolvedAssetSourcePath
{
    AssetSourceRoot Root;
    AssetPathPolicy::ProjectPath Path;
};

/// <summary>Registry and canonical path boundary for authoritative asset source roots.</summary>
class FLAXENGINE_API AssetSourceRootRegistry
{
    String _projectRoot;
    String _libraryRoot;
    Array<AssetSourceRoot> _roots;

public:
    AssetSourceRootRegistry(const StringView& projectRoot, const StringView& libraryRoot);

    const Array<AssetSourceRoot>& GetRoots() const;

    /// <summary>Registers public Content and private ExternalActors roots for a project.</summary>
    /// <returns>True on failure.</returns>
    bool RegisterProjectRoots(const StringView& contentRoot, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Registers engine-owned, public read-only content.</summary>
    /// <returns>True on failure.</returns>
    bool RegisterEngineRoot(const StringView& ownerPath, const StringView& physicalPath, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Registers one public read-only plugin content mount.</summary>
    /// <returns>True on failure.</returns>
    bool RegisterPluginRoot(const Guid& rootId, const StringView& name, const StringView& physicalPath,
        const StringView& logicalPrefix, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Registers one explicit public read-only external content mount.</summary>
    /// <returns>True on failure.</returns>
    bool RegisterExternalReadOnlyRoot(const Guid& rootId, const StringView& name, const StringView& physicalPath,
        const StringView& logicalPrefix, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Registers a fully specified trusted root.</summary>
    /// <returns>True on failure.</returns>
    bool Register(AssetSourceRoot root, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Resolves a readable logical or physical source path.</summary>
    /// <returns>True on failure.</returns>
    bool Resolve(const StringView& input, ResolvedAssetSourcePath& result, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Resolves a path that may be consumed by the ordinary database scanner.</summary>
    /// <returns>True on failure.</returns>
    bool ResolveForScan(const StringView& input, ResolvedAssetSourcePath& result, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Resolves a writable public path accepted by generic asset operations.</summary>
    /// <returns>True on failure.</returns>
    bool ResolveForGenericMutation(const StringView& input, AssetPathPolicy::ProjectPath& result,
        AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Converts a registered physical path to its stable logical path.</summary>
    /// <returns>True on failure.</returns>
    bool PhysicalToLogical(const StringView& physicalPath, String& logicalPath, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Converts a registered logical path to its canonical physical path.</summary>
    /// <returns>True on failure.</returns>
    bool LogicalToPhysical(const StringView& logicalPath, String& physicalPath, AssetPipelineDiagnostic& diagnostic) const;

private:
    bool ResolveWithPermission(const StringView& input, AssetSourceRootPermission permission,
        ResolvedAssetSourcePath& result, AssetPipelineDiagnostic& diagnostic) const;
};
