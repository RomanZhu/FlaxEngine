// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseSchema.h"
#include "AssetDatabaseBinary.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Platform/StringUtils.h"

using namespace SourceAssetDatabaseBinary;

constexpr uint32 AssetDatabaseSchema::Version;

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.Message = message;
        return true;
    }

    void WriteDiagnostic(Writer& writer, const AssetPipelineDiagnostic& value)
    {
        writer.Write(value.SchemaVersion);
        writer.Write((int32)value.Code);
        writer.Write((int32)value.Severity);
        writer.Write((int32)value.Stage);
        writer.Write(value.AssetGuid);
        writer.WriteString(value.SourcePath);
        writer.WriteString(value.ProcessorId);
        writer.WriteString(value.Target);
        writer.WriteString(value.OutputKind);
        writer.WriteString(value.Location.File);
        writer.Write(value.Location.Line);
        writer.Write(value.Location.Column);
        writer.WriteString(value.Location.GraphNode);
        writer.WriteString(value.Location.GraphPin);
        writer.WriteString(value.Message);
        writer.WriteString(value.Remediation);
        writer.Write((uint32)value.Related.Count());
        for (const String& related : value.Related)
            writer.WriteString(related);
    }

    bool ReadDiagnostic(Reader& reader, AssetPipelineDiagnostic& value)
    {
        int32 code, severity, stage;
        uint32 relatedCount;
        if (reader.Read(value.SchemaVersion) || reader.Read(code) || reader.Read(severity) || reader.Read(stage) ||
            reader.Read(value.AssetGuid) || reader.ReadString(value.SourcePath) || reader.ReadString(value.ProcessorId) ||
            reader.ReadString(value.Target) || reader.ReadString(value.OutputKind) || reader.ReadString(value.Location.File) ||
            reader.Read(value.Location.Line) || reader.Read(value.Location.Column) || reader.ReadString(value.Location.GraphNode) ||
            reader.ReadString(value.Location.GraphPin) || reader.ReadString(value.Message) || reader.ReadString(value.Remediation) ||
            reader.ReadCount(relatedCount) || code < 0 || code > (int32)AssetPipelineDiagnosticCode::ArtifactIncompatible ||
            severity < 0 || severity > (int32)AssetPipelineDiagnosticSeverity::Error ||
            stage < 0 || stage > (int32)AssetPipelineDiagnosticStage::Migration)
            return true;
        value.Code = (AssetPipelineDiagnosticCode)code;
        value.Severity = (AssetPipelineDiagnosticSeverity)severity;
        value.Stage = (AssetPipelineDiagnosticStage)stage;
        value.Related.Resize(relatedCount, false);
        for (String& related : value.Related)
            if (reader.ReadString(related))
                return true;
        return false;
    }

    String ObjectKey(const Guid& guid, int64 localFileId)
    {
        return guid.ToString(Guid::FormatType::N) + TEXT(":") + StringUtils::ToString(localFileId);
    }

    String StableObjectKey(const Guid& guid, const StringView& stableIdentifier)
    {
        return guid.ToString(Guid::FormatType::N) + TEXT(":") + stableIdentifier;
    }

    String PublicationKey(const Guid& guid, int64 localFileId, const StringView& targetId)
    {
        return ObjectKey(guid, localFileId) + TEXT(":") + targetId;
    }
}

bool SourceAssetDatabaseState::Validate(AssetPipelineDiagnostic& diagnostic) const
{
    if (Database.SchemaVersion != AssetDatabaseSchema::Version || !Database.ProjectId.IsValid())
        return Fail(diagnostic, TEXT("Source asset database has an invalid schema or project identity."));

    Dictionary<Guid, byte> sourceIds;
    Dictionary<String, byte> sourcePaths;
    Dictionary<String, byte> metaPaths;
    for (const SourceAssetRow& source : Sources)
    {
        if (!source.AssetGuid.IsValid() || source.CanonicalPath.IsEmpty() ||
            source.FirstSeenRevision > source.LastSeenRevision || source.LastSeenRevision > Database.CurrentRevision ||
            source.LastModifiedRevision > Database.CurrentRevision || sourceIds.ContainsKey(source.AssetGuid) ||
            sourcePaths.ContainsKey(source.CanonicalPath) || (!source.CanonicalMetaPath.IsEmpty() && metaPaths.ContainsKey(source.CanonicalMetaPath)))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate source row."));
        sourceIds.Add(source.AssetGuid, 0);
        sourcePaths.Add(source.CanonicalPath, 0);
        if (!source.CanonicalMetaPath.IsEmpty())
            metaPaths.Add(source.CanonicalMetaPath, 0);
    }

    Dictionary<String, byte> objectIds;
    Dictionary<String, byte> stableObjectIds;
    Dictionary<Guid, byte> objectGuids;
    for (const SourceAssetObjectRow& object : Objects)
    {
        const String objectKey = ObjectKey(object.AssetGuid, object.LocalFileId);
        const String stableKey = StableObjectKey(object.AssetGuid, object.StableIdentifier);
        if (!sourceIds.ContainsKey(object.AssetGuid) || !object.ObjectGuid.IsValid() || object.LocalFileId <= 0 || object.StableIdentifier.IsEmpty() ||
            object.FirstSeenRevision > object.LastSeenRevision || object.LastSeenRevision > Database.CurrentRevision || object.LastModifiedRevision > Database.CurrentRevision ||
            objectIds.ContainsKey(objectKey) || stableObjectIds.ContainsKey(stableKey) || objectGuids.ContainsKey(object.ObjectGuid))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate object row."));
        objectIds.Add(objectKey, 0);
        stableObjectIds.Add(stableKey, 0);
        objectGuids.Add(object.ObjectGuid, 0);
    }

    for (const SourceAssetDependencyRow& dependency : Dependencies)
    {
        if (!sourceIds.ContainsKey(dependency.OwnerAssetGuid) || dependency.OwnerLocalFileId <= 0 || dependency.TargetId.IsEmpty() ||
            !objectIds.ContainsKey(ObjectKey(dependency.OwnerAssetGuid, dependency.OwnerLocalFileId)))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid dependency edge."));
        if (dependency.TargetLocalFileId != 0 && !objectIds.ContainsKey(ObjectKey(dependency.TargetAssetGuid, dependency.TargetLocalFileId)))
            return Fail(diagnostic, TEXT("Source asset database dependency points to an unknown object."));
    }

    Dictionary<String, byte> publicationIds;
    for (const SourceAssetPublicationRow& publication : Publications)
    {
        const String key = PublicationKey(publication.AssetGuid, publication.LocalFileId, publication.TargetId);
        if (!sourceIds.ContainsKey(publication.AssetGuid) || publication.LocalFileId <= 0 ||
            !objectIds.ContainsKey(ObjectKey(publication.AssetGuid, publication.LocalFileId)) || publication.TargetId.IsEmpty() ||
            publication.SourceRevision > Database.CurrentRevision || publicationIds.ContainsKey(key))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate publication row."));
        publicationIds.Add(key, 0);
    }

    Dictionary<uint64, byte> diagnosticIds;
    for (const SourceAssetDiagnosticRow& row : Diagnostics)
    {
        if (row.DiagnosticId == 0 || row.CreatedRevision > Database.CurrentRevision || diagnosticIds.ContainsKey(row.DiagnosticId) ||
            (row.AssetGuid.IsValid() && !sourceIds.ContainsKey(row.AssetGuid)))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate diagnostic row."));
        diagnosticIds.Add(row.DiagnosticId, 0);
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void SourceAssetDatabaseState::Serialize(Array<byte>& output) const
{
    Writer writer;
    writer.Write(Database.SchemaVersion);
    writer.Write(Database.ProjectId);
    writer.Write(Database.CurrentRevision);
    writer.Write(Database.LastCompleteScanId);
    writer.Write(Database.ImporterRegistryGeneration);
    writer.Write((byte)(Database.CleanShutdown ? 1 : 0));

    writer.Write((uint32)Sources.Count());
    for (const SourceAssetRow& value : Sources)
    {
        writer.Write(value.AssetGuid);
        writer.WriteString(value.Path);
        writer.WriteString(value.CanonicalPath);
        writer.WriteString(value.MetaPath);
        writer.WriteString(value.CanonicalMetaPath);
        writer.Write((byte)(value.IsFolder ? 1 : 0));
        writer.Write(value.SourceHash);
        writer.Write(value.MetaHash);
        writer.Write(value.MetaSemanticHash);
        writer.Write(value.SourceSize);
        writer.Write(value.SourceMtimeHint);
        writer.WriteString(value.ImporterId);
        writer.WriteString(value.PortabilityKey);
        writer.Write((byte)value.SourceKind);
        writer.Write(value.ImporterSettingsVersion);
        writer.Write(value.ImporterSettingsHash);
        writer.Write(value.ImporterCodeHash);
        writer.Write((byte)value.Status);
        writer.Write(value.FirstSeenRevision);
        writer.Write(value.LastSeenRevision);
        writer.Write(value.LastModifiedRevision);
    }

    writer.Write((uint32)Objects.Count());
    for (const SourceAssetObjectRow& value : Objects)
    {
        writer.Write(value.AssetGuid);
        writer.Write(value.ObjectGuid);
        writer.Write(value.LocalFileId);
        writer.WriteString(value.StableIdentifier);
        writer.WriteString(value.SubAssetKey);
        writer.WriteString(value.TypeName);
        writer.WriteString(value.DisplayName);
        writer.Write((byte)(value.IsMain ? 1 : 0));
        writer.Write((byte)(value.IsRemoved ? 1 : 0));
        writer.Write((byte)value.Status);
        writer.WriteString(value.ObjectMetadata);
        writer.Write(value.FirstSeenRevision);
        writer.Write(value.LastSeenRevision);
        writer.Write(value.LastModifiedRevision);
    }

    writer.Write((uint32)Dependencies.Count());
    for (const SourceAssetDependencyRow& value : Dependencies)
    {
        writer.Write(value.OwnerAssetGuid);
        writer.Write(value.OwnerLocalFileId);
        writer.WriteString(value.TargetId);
        writer.Write((byte)value.Kind);
        writer.Write(value.TargetAssetGuid);
        writer.Write(value.TargetLocalFileId);
        writer.WriteString(value.SourcePath);
        writer.Write(value.ExactArtifact);
        writer.WriteString(value.CustomDependency);
        writer.Write(value.Content);
        writer.Write(value.Flags);
        writer.WriteString(value.OriginPath);
        writer.Write(value.OriginLine);
        writer.Write(value.OriginColumn);
    }

    writer.Write((uint32)Publications.Count());
    for (const SourceAssetPublicationRow& value : Publications)
    {
        writer.Write(value.AssetGuid);
        writer.Write(value.LocalFileId);
        writer.WriteString(value.TargetId);
        writer.Write(value.Artifact);
        writer.Write(value.ManifestHash);
        writer.Write(value.InputFingerprint);
        writer.Write(value.SourceRevision);
        writer.Write(value.ImporterRegistryGeneration);
        writer.Write(value.PublishedUtcTicks);
        writer.Write((byte)(value.IsLastKnownGood ? 1 : 0));
    }

    writer.Write((uint32)Diagnostics.Count());
    for (const SourceAssetDiagnosticRow& value : Diagnostics)
    {
        writer.Write(value.DiagnosticId);
        writer.Write(value.AssetGuid);
        writer.Write(value.LocalFileId);
        WriteDiagnostic(writer, value.Diagnostic);
        writer.Write(value.AttemptId);
        writer.Write(value.CreatedRevision);
        writer.Write((byte)(value.IsActive ? 1 : 0));
    }
    writer.Finish(output);
}

bool SourceAssetDatabaseState::Deserialize(const byte* data, uint32 length, SourceAssetDatabaseState& output, AssetPipelineDiagnostic& diagnostic)
{
    Reader reader(data, length);
    SourceAssetDatabaseState value;
    byte flag;
    uint32 count;
    if (reader.Read(value.Database.SchemaVersion) || reader.Read(value.Database.ProjectId) ||
        reader.Read(value.Database.CurrentRevision) || reader.Read(value.Database.LastCompleteScanId) ||
        reader.Read(value.Database.ImporterRegistryGeneration) || reader.Read(flag) || flag > 1 || reader.ReadCount(count))
        return Fail(diagnostic, TEXT("Source asset database header is truncated or malformed."));
    value.Database.CleanShutdown = flag != 0;

    value.Sources.Resize(count, false);
    for (SourceAssetRow& item : value.Sources)
    {
        byte isFolder, sourceKind, status;
        if (reader.Read(item.AssetGuid) || reader.ReadString(item.Path) || reader.ReadString(item.CanonicalPath) ||
            reader.ReadString(item.MetaPath) || reader.ReadString(item.CanonicalMetaPath) || reader.Read(isFolder) ||
            reader.Read(item.SourceHash) || reader.Read(item.MetaHash) || reader.Read(item.MetaSemanticHash) || reader.Read(item.SourceSize) ||
            reader.Read(item.SourceMtimeHint) || reader.ReadString(item.ImporterId) || reader.ReadString(item.PortabilityKey) || reader.Read(sourceKind) || reader.Read(item.ImporterSettingsVersion) ||
            reader.Read(item.ImporterSettingsHash) || reader.Read(item.ImporterCodeHash) || reader.Read(status) ||
            reader.Read(item.FirstSeenRevision) || reader.Read(item.LastSeenRevision) || reader.Read(item.LastModifiedRevision) ||
            isFolder > 1 || sourceKind > (byte)AssetSourceKind::Folder || status > (byte)AssetRecordStatus::PathCollision)
            return Fail(diagnostic, TEXT("Source asset database source table is malformed."));
        item.IsFolder = isFolder != 0;
        item.SourceKind = (AssetSourceKind)sourceKind;
        item.Status = (AssetRecordStatus)status;
    }

    if (reader.ReadCount(count))
        return Fail(diagnostic, TEXT("Source asset database object table is malformed."));
    value.Objects.Resize(count, false);
    for (SourceAssetObjectRow& item : value.Objects)
    {
        byte isMain, isRemoved, status;
        if (reader.Read(item.AssetGuid) || reader.Read(item.ObjectGuid) || reader.Read(item.LocalFileId) || reader.ReadString(item.StableIdentifier) ||
            reader.ReadString(item.SubAssetKey) || reader.ReadString(item.TypeName) || reader.ReadString(item.DisplayName) || reader.Read(isMain) || reader.Read(isRemoved) || reader.Read(status) ||
            reader.ReadString(item.ObjectMetadata) || reader.Read(item.FirstSeenRevision) || reader.Read(item.LastSeenRevision) || reader.Read(item.LastModifiedRevision) ||
            isMain > 1 || isRemoved > 1 || status > (byte)AssetRecordStatus::PathCollision)
            return Fail(diagnostic, TEXT("Source asset database object table is malformed."));
        item.IsMain = isMain != 0;
        item.IsRemoved = isRemoved != 0;
        item.Status = (AssetRecordStatus)status;
    }

    if (reader.ReadCount(count))
        return Fail(diagnostic, TEXT("Source asset database dependency table is malformed."));
    value.Dependencies.Resize(count, false);
    for (SourceAssetDependencyRow& item : value.Dependencies)
    {
        byte kind;
        if (reader.Read(item.OwnerAssetGuid) || reader.Read(item.OwnerLocalFileId) || reader.ReadString(item.TargetId) || reader.Read(kind) ||
            reader.Read(item.TargetAssetGuid) || reader.Read(item.TargetLocalFileId) || reader.ReadString(item.SourcePath) ||
            reader.Read(item.ExactArtifact) || reader.ReadString(item.CustomDependency) || reader.Read(item.Content) || reader.Read(item.Flags) ||
            reader.ReadString(item.OriginPath) || reader.Read(item.OriginLine) || reader.Read(item.OriginColumn) ||
            kind > (byte)AssetDependencyKind::Toolchain)
            return Fail(diagnostic, TEXT("Source asset database dependency table is malformed."));
        item.Kind = (AssetDependencyKind)kind;
    }

    if (reader.ReadCount(count))
        return Fail(diagnostic, TEXT("Source asset database publication table is malformed."));
    value.Publications.Resize(count, false);
    for (SourceAssetPublicationRow& item : value.Publications)
    {
        byte isLastKnownGood;
        if (reader.Read(item.AssetGuid) || reader.Read(item.LocalFileId) || reader.ReadString(item.TargetId) || reader.Read(item.Artifact) ||
            reader.Read(item.ManifestHash) || reader.Read(item.InputFingerprint) || reader.Read(item.SourceRevision) ||
            reader.Read(item.ImporterRegistryGeneration) || reader.Read(item.PublishedUtcTicks) || reader.Read(isLastKnownGood) ||
            isLastKnownGood > 1)
            return Fail(diagnostic, TEXT("Source asset database publication table is malformed."));
        item.IsLastKnownGood = isLastKnownGood != 0;
    }

    if (reader.ReadCount(count))
        return Fail(diagnostic, TEXT("Source asset database diagnostic table is malformed."));
    value.Diagnostics.Resize(count, false);
    for (SourceAssetDiagnosticRow& item : value.Diagnostics)
    {
        byte isActive;
        if (reader.Read(item.DiagnosticId) || reader.Read(item.AssetGuid) || reader.Read(item.LocalFileId) ||
            ReadDiagnostic(reader, item.Diagnostic) || reader.Read(item.AttemptId) || reader.Read(item.CreatedRevision) ||
            reader.Read(isActive) || isActive > 1)
            return Fail(diagnostic, TEXT("Source asset database diagnostic table is malformed."));
        item.IsActive = isActive != 0;
    }

    if (!reader.AtEnd() || value.Validate(diagnostic))
        return true;
    output = MoveTemp(value);
    return false;
}
