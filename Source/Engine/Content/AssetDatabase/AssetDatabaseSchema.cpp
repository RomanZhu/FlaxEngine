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
    for (const SourceAssetObjectRow& object : Objects)
    {
        const String objectKey = ObjectKey(object.AssetGuid, object.LocalFileId);
        const String stableKey = StableObjectKey(object.AssetGuid, object.StableIdentifier);
        if (!sourceIds.ContainsKey(object.AssetGuid) || object.LocalFileId <= 0 || object.StableIdentifier.IsEmpty() ||
            object.FirstSeenRevision > object.LastSeenRevision || object.LastSeenRevision > Database.CurrentRevision || object.LastModifiedRevision > Database.CurrentRevision ||
            objectIds.ContainsKey(objectKey) || stableObjectIds.ContainsKey(stableKey))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate object row."));
        objectIds.Add(objectKey, 0);
        stableObjectIds.Add(stableKey, 0);
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

    Dictionary<String, byte> targetIds;
    for (const SourceAssetImportTargetRow& row : ImportTargets)
    {
        if (row.TargetId.IsEmpty() || targetIds.ContainsKey(row.TargetId))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate import target row."));
        targetIds.Add(row.TargetId, 0);
    }

    Dictionary<String, byte> artifactObjectIds;
    for (const SourceArtifactObjectRow& row : ArtifactObjects)
    {
        const String key = String(row.Artifact.ToString()) + TEXT(":") + ObjectKey(row.AssetGuid, row.LocalFileId);
        if (row.Artifact.IsZero() || !objectIds.ContainsKey(ObjectKey(row.AssetGuid, row.LocalFileId)) ||
            row.TypeName.IsEmpty() || row.ObjectBlobId.IsZero() || artifactObjectIds.ContainsKey(key))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate artifact object row."));
        artifactObjectIds.Add(key, 0);
    }

    Dictionary<String, byte> labelIds;
    for (const SourceAssetLabelRow& row : Labels)
    {
        const String key = row.AssetGuid.ToString(Guid::FormatType::N) + TEXT(":") + row.Label;
        if (!sourceIds.ContainsKey(row.AssetGuid) || row.Label.IsEmpty() || labelIds.ContainsKey(key))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate label row."));
        labelIds.Add(key, 0);
    }

    uint64 previousSequence = 0;
    for (const SourceFileJournalRow& row : FileJournal)
    {
        if (row.Sequence == 0 || row.Sequence <= previousSequence || row.EventKind.IsEmpty())
            return Fail(diagnostic, TEXT("Source asset database contains an invalid file journal row."));
        previousSequence = row.Sequence;
    }

    Dictionary<Guid, byte> refreshIds;
    for (const SourceRefreshSessionRow& row : RefreshSessions)
    {
        if (!row.RefreshId.IsValid() || row.Status.IsEmpty() || row.StartingRevision > Database.CurrentRevision ||
            row.EndingRevision > Database.CurrentRevision || refreshIds.ContainsKey(row.RefreshId))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate refresh session row."));
        refreshIds.Add(row.RefreshId, 0);
    }

    Dictionary<Guid, byte> attemptIds;
    for (const SourceImportAttemptRow& row : ImportAttempts)
    {
        if (!row.AttemptId.IsValid() || !sourceIds.ContainsKey(row.AssetGuid) || row.TargetId.IsEmpty() ||
            row.Status.IsEmpty() || row.RequestedRevision > Database.CurrentRevision || attemptIds.ContainsKey(row.AttemptId))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate import attempt row."));
        attemptIds.Add(row.AttemptId, 0);
    }

    Dictionary<String, byte> customDependencyIds;
    for (const SourceCustomDependencyRow& row : CustomDependencies)
    {
        if (row.DependencyName.IsEmpty() || row.CurrentHash.IsZero() || row.ModifiedRevision > Database.CurrentRevision ||
            customDependencyIds.ContainsKey(row.DependencyName))
            return Fail(diagnostic, TEXT("Source asset database contains an invalid or duplicate custom dependency row."));
        customDependencyIds.Add(row.DependencyName, 0);
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
        writer.WriteString(value.OriginImporter);
        writer.WriteString(value.OriginDescription);
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

    writer.Write((uint32)ImportTargets.Count());
    for (const SourceAssetImportTargetRow& value : ImportTargets)
    {
        writer.WriteString(value.TargetId);
        writer.WriteString(value.Platform);
        writer.WriteString(value.Architecture);
        writer.WriteString(value.GraphicsApi);
        writer.WriteString(value.QualityLevel);
        writer.WriteString(value.FeatureSet);
        writer.WriteString(value.BuildConfiguration);
        writer.Write(value.CanonicalHash);
    }

    writer.Write((uint32)ArtifactObjects.Count());
    for (const SourceArtifactObjectRow& value : ArtifactObjects)
    {
        writer.Write(value.Artifact);
        writer.Write(value.AssetGuid);
        writer.Write(value.LocalFileId);
        writer.WriteString(value.TypeName);
        writer.Write(value.ObjectBlobId);
        writer.Write(value.MetadataBlobId);
        writer.WriteString(value.CompatibilityTag);
    }

    writer.Write((uint32)Labels.Count());
    for (const SourceAssetLabelRow& value : Labels)
    {
        writer.Write(value.AssetGuid);
        writer.WriteString(value.Label);
    }

    writer.Write((uint32)FileJournal.Count());
    for (const SourceFileJournalRow& value : FileJournal)
    {
        writer.Write(value.Sequence);
        writer.WriteString(value.EventKind);
        writer.WriteString(value.OldPath);
        writer.WriteString(value.NewPath);
        writer.WriteString(value.FileIdentityHint);
        writer.Write(value.ObservedSize);
        writer.Write(value.ObservedMtime);
        writer.Write(value.ObservedUtcTicks);
        writer.Write(value.ProcessedRefreshId);
    }

    writer.Write((uint32)RefreshSessions.Count());
    for (const SourceRefreshSessionRow& value : RefreshSessions)
    {
        writer.Write(value.RefreshId);
        writer.Write(value.StartingRevision);
        writer.Write(value.EndingRevision);
        writer.WriteString(value.Reason);
        writer.Write(value.IterationCount);
        writer.WriteString(value.Status);
        writer.Write(value.StartedUtcTicks);
        writer.Write(value.CompletedUtcTicks);
    }

    writer.Write((uint32)ImportAttempts.Count());
    for (const SourceImportAttemptRow& value : ImportAttempts)
    {
        writer.Write(value.AttemptId);
        writer.Write(value.RefreshId);
        writer.Write(value.AssetGuid);
        writer.WriteString(value.TargetId);
        writer.Write(value.RequestedRevision);
        writer.Write(value.InputFingerprint);
        writer.WriteString(value.WorkerId);
        writer.WriteString(value.Status);
        writer.Write(value.StartedUtcTicks);
        writer.Write(value.CompletedUtcTicks);
        writer.WriteString(value.FailureCode);
    }

    writer.Write((uint32)CustomDependencies.Count());
    for (const SourceCustomDependencyRow& value : CustomDependencies)
    {
        writer.WriteString(value.DependencyName);
        writer.Write(value.CurrentHash);
        writer.WriteString(value.Provider);
        writer.Write(value.ModifiedRevision);
    }
    writer.Finish(output);
}

bool SourceAssetDatabaseState::Deserialize(const byte* data, uint32 length, SourceAssetDatabaseState& output,
    AssetPipelineDiagnostic& diagnostic, bool validate)
{
    Reader reader(data, length);
    SourceAssetDatabaseState value;
    byte flag;
    uint32 count;
    if (reader.Read(value.Database.SchemaVersion) || reader.Read(value.Database.ProjectId) ||
        reader.Read(value.Database.CurrentRevision) || reader.Read(value.Database.LastCompleteScanId) ||
        reader.Read(value.Database.ImporterRegistryGeneration) || reader.Read(flag) || flag > 1 || reader.ReadCount(count))
        return Fail(diagnostic, TEXT("Source asset database header is truncated or malformed."));
    const uint32 serializedSchemaVersion = value.Database.SchemaVersion;
    if (serializedSchemaVersion != AssetDatabaseSchema::Version)
        return Fail(diagnostic, TEXT("Source asset database schema version is not supported."));
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
        if (reader.Read(item.AssetGuid) || reader.Read(item.LocalFileId) || reader.ReadString(item.StableIdentifier) ||
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
            reader.ReadString(item.OriginImporter) || reader.ReadString(item.OriginDescription) ||
            reader.ReadString(item.OriginPath) || reader.Read(item.OriginLine) || reader.Read(item.OriginColumn) ||
            kind > (byte)AssetDependencyKind::LogicalPath)
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

    {
        if (reader.ReadCount(count))
            return Fail(diagnostic, TEXT("Source asset database import target table is malformed."));
        value.ImportTargets.Resize(count, false);
        for (SourceAssetImportTargetRow& item : value.ImportTargets)
        {
            if (reader.ReadString(item.TargetId) || reader.ReadString(item.Platform) || reader.ReadString(item.Architecture) ||
                reader.ReadString(item.GraphicsApi) || reader.ReadString(item.QualityLevel) || reader.ReadString(item.FeatureSet) ||
                reader.ReadString(item.BuildConfiguration) || reader.Read(item.CanonicalHash))
                return Fail(diagnostic, TEXT("Source asset database import target table is malformed."));
        }

        if (reader.ReadCount(count))
            return Fail(diagnostic, TEXT("Source asset database artifact object table is malformed."));
        value.ArtifactObjects.Resize(count, false);
        for (SourceArtifactObjectRow& item : value.ArtifactObjects)
        {
            if (reader.Read(item.Artifact) || reader.Read(item.AssetGuid) || reader.Read(item.LocalFileId) ||
                reader.ReadString(item.TypeName) || reader.Read(item.ObjectBlobId) || reader.Read(item.MetadataBlobId) ||
                reader.ReadString(item.CompatibilityTag))
                return Fail(diagnostic, TEXT("Source asset database artifact object table is malformed."));
        }

        if (reader.ReadCount(count))
            return Fail(diagnostic, TEXT("Source asset database label table is malformed."));
        value.Labels.Resize(count, false);
        for (SourceAssetLabelRow& item : value.Labels)
        {
            if (reader.Read(item.AssetGuid) || reader.ReadString(item.Label))
                return Fail(diagnostic, TEXT("Source asset database label table is malformed."));
        }

        if (reader.ReadCount(count))
            return Fail(diagnostic, TEXT("Source asset database file journal table is malformed."));
        value.FileJournal.Resize(count, false);
        for (SourceFileJournalRow& item : value.FileJournal)
        {
            if (reader.Read(item.Sequence) || reader.ReadString(item.EventKind) || reader.ReadString(item.OldPath) ||
                reader.ReadString(item.NewPath) || reader.ReadString(item.FileIdentityHint) || reader.Read(item.ObservedSize) ||
                reader.Read(item.ObservedMtime) || reader.Read(item.ObservedUtcTicks) || reader.Read(item.ProcessedRefreshId))
                return Fail(diagnostic, TEXT("Source asset database file journal table is malformed."));
        }

        if (reader.ReadCount(count))
            return Fail(diagnostic, TEXT("Source asset database refresh session table is malformed."));
        value.RefreshSessions.Resize(count, false);
        for (SourceRefreshSessionRow& item : value.RefreshSessions)
        {
            if (reader.Read(item.RefreshId) || reader.Read(item.StartingRevision) || reader.Read(item.EndingRevision) ||
                reader.ReadString(item.Reason) || reader.Read(item.IterationCount) || reader.ReadString(item.Status) ||
                reader.Read(item.StartedUtcTicks) || reader.Read(item.CompletedUtcTicks))
                return Fail(diagnostic, TEXT("Source asset database refresh session table is malformed."));
        }

        if (reader.ReadCount(count))
            return Fail(diagnostic, TEXT("Source asset database import attempt table is malformed."));
        value.ImportAttempts.Resize(count, false);
        for (SourceImportAttemptRow& item : value.ImportAttempts)
        {
            if (reader.Read(item.AttemptId) || reader.Read(item.RefreshId) || reader.Read(item.AssetGuid) ||
                reader.ReadString(item.TargetId) || reader.Read(item.RequestedRevision) || reader.Read(item.InputFingerprint) ||
                reader.ReadString(item.WorkerId) || reader.ReadString(item.Status) || reader.Read(item.StartedUtcTicks) ||
                reader.Read(item.CompletedUtcTicks) || reader.ReadString(item.FailureCode))
                return Fail(diagnostic, TEXT("Source asset database import attempt table is malformed."));
        }

        if (reader.ReadCount(count))
            return Fail(diagnostic, TEXT("Source asset database custom dependency table is malformed."));
        value.CustomDependencies.Resize(count, false);
        for (SourceCustomDependencyRow& item : value.CustomDependencies)
        {
            if (reader.ReadString(item.DependencyName) || reader.Read(item.CurrentHash) || reader.ReadString(item.Provider) ||
                reader.Read(item.ModifiedRevision))
                return Fail(diagnostic, TEXT("Source asset database custom dependency table is malformed."));
        }
    }
    if (!reader.AtEnd() || (validate && value.Validate(diagnostic)))
        return true;
    output = MoveTemp(value);
    return false;
}
