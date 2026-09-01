// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDocument.h"
#include "AssetSaveService.h"
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Core/Types/Span.h"
#include "Engine/Core/Types/Variant.h"
#include "Engine/Scripting/ScriptingType.h"
#include "Engine/Visject/VisjectMeta.h"

/// <summary>One authored graph parameter keyed by a stable GUID rather than display name.</summary>
struct FLAXENGINE_API GraphDocumentParameter
{
    Guid ID;
    String Name;
    VariantType Type;
    Variant Default;
    bool IsPublic = true;
    VisjectMeta Meta;
};

/// <summary>One visject box projected as a versioned pin identifier of the form box:{id}.</summary>
struct FLAXENGINE_API GraphDocumentPin
{
    byte BoxID = 0;
    VariantType Type;
};

/// <summary>Directed connection using stable node and pin identifiers.</summary>
struct FLAXENGINE_API GraphDocumentConnection
{
    uint32 FromNode = 0;
    byte FromPin = 0;
    uint32 ToNode = 0;
    byte ToPin = 0;
};

/// <summary>One graph node with durable identity independent of array order.</summary>
struct FLAXENGINE_API GraphDocumentNode
{
    uint32 LegacyID = 0;
    uint16 GroupID = 0;
    uint16 TypeID = 0;
    int32 TypeVersion = 1;
    float PositionX = 0.0f;
    float PositionY = 0.0f;
    Array<Variant> Values;
    Array<GraphDocumentPin> Pins;
    VisjectMeta Meta;
    bool Unknown = false;
    StringAnsi CustomJson = "{}\n";

    StringAnsi GetStableID() const;
    StringAnsi GetStableType() const;
};

/// <summary>Decoded authored graph used by codecs, validators, and compilers.</summary>
struct FLAXENGINE_API GraphDocument
{
    String TypeName;
    int32 DocumentVersion = 1;
    int32 GraphVersion = 1;
    Array<GraphDocumentParameter> Parameters;
    Array<GraphDocumentNode> Nodes;
    Array<GraphDocumentConnection> Connections;
    VisjectMeta GraphMeta;
    StringAnsi PropertiesJson = "{}\n";
    StringAnsi EditorJson = "{}\n";
};

/// <summary>Immutable canonical graph document plus extracted compiler inputs.</summary>
struct FLAXENGINE_API GraphDocumentSnapshot : AssetDocumentSnapshot
{
    int32 GraphVersion = 0;
    GraphDocument Document;
    Array<byte> CompatibilitySurface;
    ContentHash FunctionInterfaceHash;
};

/// <summary>Canonical JSON codec for graph-authored assets.</summary>
class FLAXENGINE_API GraphDocumentCodec : public IAssetDocumentCodec
{
public:
    static constexpr int32 CurrentDocumentVersion = 1;
    static constexpr int32 CurrentGraphVersion = 1;
    static constexpr int32 CompatibilityGraphVersion = 7000;
    static constexpr int32 MaximumSurfaceBytes = 64 * 1024 * 1024;

    bool Decode(const StringAnsiView& source, AssetDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const override;
    bool DecodeGraph(const StringAnsiView& source, GraphDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Projects exact Visject surface bytes into a canonical graph document.</summary>
    static bool Encode(const StringView& typeName, const Span<byte>& surface, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Projects a loaded surface into the in-memory graph document model.</summary>
    static bool FromSurface(const StringView& typeName, const Span<byte>& surface, GraphDocument& document, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Serializes one in-memory document to canonical JSON.</summary>
    static bool ToCanonicalJson(const GraphDocument& document, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Writes canonical JSON through sibling staging, reparse, and atomic replace.</summary>
    static bool SaveAtomic(const StringView& path, const StringAnsiView& canonicalText,
        AssetPipelineDiagnostic& diagnostic, ContentHash* previousHash = nullptr,
        SourceSaveConflictPolicy conflictPolicy = SourceSaveConflictPolicy::Strict,
        AssetSaveRefreshMode refreshMode = AssetSaveRefreshMode::TrackedSource);

    /// <summary>Writes a non-graph canonical JSON document through sibling staging and atomic replace.</summary>
    static bool SaveJsonAtomic(const StringView& path, const StringAnsiView& canonicalText,
        AssetPipelineDiagnostic& diagnostic, ContentHash* previousHash = nullptr,
        SourceSaveConflictPolicy conflictPolicy = SourceSaveConflictPolicy::Strict,
        AssetSaveRefreshMode refreshMode = AssetSaveRefreshMode::TrackedSource);

    /// <summary>Writes editor-local canonical JSON without consulting project asset registration.</summary>
    static bool SaveLocalJsonAtomic(const StringView& path, const StringAnsiView& canonicalText,
        AssetPipelineDiagnostic& diagnostic, ContentHash* previousHash = nullptr);

    /// <summary>Creates a deterministic starter document for a supported runtime type.</summary>
    static bool CreateStarter(const StringView& typeName, GraphDocument& document, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Writes a compatibility .flax asset with Visject surface bytes and type-specific extra chunks.</summary>
    /// <param name="artifactStagingMode">When true, writes the flax in place and materials also generate runtime shader chunks.</param>
    static bool WriteCompatibilityAsset(const StringView& path, const Guid& id, const StringView& typeName, const Array<byte>& surface, const StringAnsiView& propertiesJson, AssetPipelineDiagnostic& diagnostic, bool artifactStagingMode = true);

    static bool IsSupportedType(const StringView& typeName);
    static const Char* ExtensionForType(const StringView& typeName);
    static bool TypeForExtension(const StringView& extension, String& typeName);

    /// <summary>Serializes one Variant as explicit canonical JSON.</summary>
    static bool EncodeVariantJson(const Variant& value, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Deserializes one Variant from explicit canonical JSON.</summary>
    static bool DecodeVariantJson(const StringAnsiView& source, Variant& value, AssetPipelineDiagnostic& diagnostic);
};

/// <summary>Structural graph validator used before compilation and publication.</summary>
class FLAXENGINE_API GraphDocumentValidator : public IAssetDocumentValidator
{
public:
    bool Validate(const AssetDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const override;
    static bool ValidateDocument(const GraphDocument& document, AssetPipelineDiagnostic& diagnostic);
};

/// <summary>Compatibility compiler that emits the current Visject surface payload.</summary>
class FLAXENGINE_API GraphDocumentCompiler : public IAssetDocumentCompiler
{
public:
    bool Compile(const AssetDocumentSnapshot& snapshot, Array<byte>& output, AssetPipelineDiagnostic& diagnostic) const override;
    static bool CompileDocument(const GraphDocument& document, Array<byte>& output, AssetPipelineDiagnostic& diagnostic);
};

/// <summary>Finds compiler inputs and runtime references without loading a runtime asset.</summary>
class FLAXENGINE_API GraphDependencyExtractor
{
public:
    static bool Extract(const GraphDocument& document, Array<AssetDependency>& dependencies, ContentHash& functionInterfaceHash, AssetPipelineDiagnostic& diagnostic);
};

/// <summary>Independent source, refresh, and import outcomes for an editor graph save.</summary>
API_STRUCT() struct FLAXENGINE_API AssetDocumentSaveResult
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetDocumentSaveResult);

    API_FIELD() bool SourceCommitted = false;
    API_FIELD() bool SourceUnchanged = false;
    API_FIELD() bool SourceDurabilityUncertain = false;
    API_FIELD() bool Conflict = false;
    API_FIELD() bool RefreshFailed = false;
    API_FIELD() bool ImportRequested = false;
    API_FIELD() bool ImportSucceeded = false;
    API_FIELD() bool ImportFailed = false;
    API_FIELD() bool ImportBlocked = false;
    API_FIELD() String SourceHash;
    API_FIELD() String Diagnostic;
};

/// <summary>Direct source-document operations used by editor document sessions.</summary>
API_CLASS(Static) class FLAXENGINE_API AssetDocumentService
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AssetDocumentService);

public:
    /// <summary>Parses and compiles an authored graph source into its editable surface representation.</summary>
    API_FUNCTION() static BytesContainer LoadGraphSource(const StringView& path);

    /// <summary>Atomically writes an edited graph surface to its source document.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool SaveGraphSource(const StringView& path, const BytesContainer& surface,
        const StringView& expectedSourceHash, const StringView& propertiesJson = StringView::Empty);

    /// <summary>Atomically writes an edited graph and reports source, refresh, and import separately.</summary>
    API_FUNCTION() static AssetDocumentSaveResult SaveGraphSourceDetailed(const StringView& path,
        const BytesContainer& surface, const StringView& expectedSourceHash,
        const StringView& propertiesJson = StringView::Empty, bool importAfterSave = true,
        bool forceImport = true);

    /// <summary>Creates a starter graph source and metadata without creating a runtime binary asset.</summary>
    /// <returns>The source GUID, or an invalid GUID on failure.</returns>
    API_FUNCTION() static Guid CreateGraphSource(const StringView& path, const StringView& typeName,
        const StringView& propertiesJson = StringView::Empty);
};

/// <summary>Editor document session: dirty/save/external-change detection without compiling on open.</summary>
class FLAXENGINE_API GraphDocumentSession
{
public:
    String Path;
    String TypeName;
    ContentHash LoadedHash;
    GraphDocument Document;
    bool Dirty = false;
    uint64 EditRevision = 0;

    /// <returns>True on failure.</returns>
    bool Open(const StringView& path, AssetPipelineDiagnostic& diagnostic);
    void MarkDirty(int64 localID = 0, const StringView& reason = TEXT("Graph document edited"));
    bool Save(bool allowOverwriteConflict, AssetPipelineDiagnostic& diagnostic);
    bool HasExternalChange(AssetPipelineDiagnostic& diagnostic) const;
};

/// <summary>Ephemeral unsaved preview artifacts under Library/Temp/Preview. Never published as current.</summary>
class FLAXENGINE_API GraphDocumentPreview
{
public:
#if USE_EDITOR
    /// <returns>True on failure.</returns>
    static bool Publish(const Guid& assetID, const StringView& typeName, const Span<byte>& surface,
        String& storagePath, ArtifactLease& lease, AssetPipelineDiagnostic& diagnostic);
    static void Release(const Guid& assetID);
#endif
    static bool IsPreviewPath(const StringView& path);
};
