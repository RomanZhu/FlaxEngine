// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetRecord.h"
#include "SubAsset.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Tracked processor selection and explicit settings.</summary>
struct FLAXENGINE_API AssetProcessorMeta
{
    String ID;
    int32 SettingsVersion = 1;
    StringAnsi SettingsJson = "{}\n";
    Dictionary<StringAnsi, StringAnsi> UnknownFields;
};

/// <summary>Failure injection points used to verify atomic sidecar publication.</summary>
enum class AssetMetaWriteFailurePoint : byte
{
    None,
    BeforeWrite,
    AfterWrite,
    AfterValidate,
    BeforeReplace,
};

/// <summary>Universal tracked asset sidecar.</summary>
struct FLAXENGINE_API AssetMeta
{
    static constexpr int32 CurrentMetaVersion = 1;

    int32 MetaVersion = CurrentMetaVersion;
    Guid ID;
    String AssetType;
    AssetSourceKind SourceKind = AssetSourceKind::ImportedSource;
    AssetProcessorMeta Processor;
    Dictionary<String, SubAssetMeta> SubAssets;
    Array<String> Labels;
    StringAnsi UserDataJson;
    Dictionary<StringAnsi, StringAnsi> UnknownFields;
    bool MetaUpgradeRequired = false;

    /// <summary>Parses and validates sidecar bytes without writing.</summary>
    /// <returns>True on failure.</returns>
    static bool Parse(const StringAnsiView& json, const StringView& path, AssetMeta& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Reads and parses a sidecar without modifying it.</summary>
    /// <returns>True on failure.</returns>
    static bool Load(const StringView& path, AssetMeta& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Writes canonical bytes through sibling staging, validation, and atomic replace.</summary>
    /// <returns>True on failure.</returns>
    static bool SaveAtomic(const StringView& path, const AssetMeta& value, AssetPipelineDiagnostic& diagnostic, uint32* selfWriteHash = nullptr, AssetMetaWriteFailurePoint failurePoint = AssetMetaWriteFailurePoint::None);

    /// <summary>Serializes canonical UTF-8 JSON without writing.</summary>
    /// <returns>True on failure.</returns>
    bool ToJson(StringAnsi& output, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Clones tracked identity, assigning a new root and all-new subasset/tombstone GUIDs.</summary>
    AssetMeta CloneWithNewIdentities() const;
};
