// Copyright (c) Wojciech Figat. All rights reserved.

#include "ProjectMigrationOrchestrator.h"
#include "ProjectMigrationSteps.h"
#include "AssetMeta.h"
#include "AssetProjectValidator.h"
#include "AssetDatabaseFacade.h"
#include "SourceHashCache.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include <algorithm>
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#elif PLATFORM_LINUX || PLATFORM_MAC
#include <sys/statvfs.h>
#endif

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAlloc;

    struct JournalOperation
    {
        ProjectMigrationPhase Phase = ProjectMigrationPhase::None;
        String Name;
        String Path;
        String TemporaryPath;
        String PreimageHash;
        String PostimageHash;
        bool Committed = false;
    };

    struct MigrationJournalState
    {
        static constexpr int32 FormatVersion = 1;

        Guid TransactionId = Guid::Empty;
        ProjectMigrationPhase Phase = ProjectMigrationPhase::None;
        String ProjectDescriptorPath;
        String ProjectRoot;
        String ContentRoot;
        String BackupRoot;
        String DescriptorBackupPath;
        String ContentBackupPath;
        String JournalPath;
        String LockPath;
        String CandidateMarkerPath;
        String CandidateMarkerFingerprint;
        String InitialSourceFingerprint;
        String CurrentSourceFingerprint;
        String VerificationFingerprint;
        String CompletionReportFingerprint;
        Array<String> MountRoots;
        Array<String> MountFingerprints;
        String EvidenceHashes[10];
        Array<JournalOperation> Operations;
    };

    const Char* PhaseName(ProjectMigrationPhase phase)
    {
        switch (phase)
        {
        case ProjectMigrationPhase::None: return TEXT("None");
        case ProjectMigrationPhase::M0PreflightAndBackup: return TEXT("M0PreflightAndBackup");
        case ProjectMigrationPhase::M1LegacyGraphFrozen: return TEXT("M1LegacyGraphFrozen");
        case ProjectMigrationPhase::M2CanonicalRootsAndSettings: return TEXT("M2CanonicalRootsAndSettings");
        case ProjectMigrationPhase::M3AssetsClassified: return TEXT("M3AssetsClassified");
        case ProjectMigrationPhase::M4MetadataAndIdentityConverted: return TEXT("M4MetadataAndIdentityConverted");
        case ProjectMigrationPhase::M5ReferencesRewritten: return TEXT("M5ReferencesRewritten");
        case ProjectMigrationPhase::M6AuthoredSourcesWritten: return TEXT("M6AuthoredSourcesWritten");
        case ProjectMigrationPhase::M7CleanDatabaseImported: return TEXT("M7CleanDatabaseImported");
        case ProjectMigrationPhase::M8SemanticallyVerified: return TEXT("M8SemanticallyVerified");
        case ProjectMigrationPhase::M9Committed: return TEXT("M9Committed");
        case ProjectMigrationPhase::RolledBack: return TEXT("RolledBack");
        case ProjectMigrationPhase::Failed: return TEXT("Failed");
        default: return TEXT("Invalid");
        }
    }

    bool ParsePhase(const StringView& name, ProjectMigrationPhase& phase)
    {
        for (int32 value = static_cast<int32>(ProjectMigrationPhase::None); value <= static_cast<int32>(ProjectMigrationPhase::Failed); value++)
        {
            const ProjectMigrationPhase candidate = static_cast<ProjectMigrationPhase>(value);
            if (name == PhaseName(candidate))
            {
                phase = candidate;
                return false;
            }
        }
        return true;
    }

    AssetPipelineDiagnostic MakeDiagnostic(const StringView& path, const StringView& message, const StringView& remediation = StringView::Empty)
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        diagnostic.Remediation = remediation;
        return diagnostic;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message, const StringView& remediation = StringView::Empty)
    {
        diagnostic = MakeDiagnostic(path, message, remediation);
        return true;
    }

    String NormalizeAbsolute(const StringView& path)
    {
        String result = FileSystem::ConvertRelativePathToAbsolute(String(path));
        StringUtils::PathRemoveRelativeParts(result);
        result.Replace('\\', '/');
        while (result.Length() > 1 && result.EndsWith(TEXT("/")))
            result.Remove(result.Length() - 1, 1);
        return result;
    }

    bool IsSameOrChild(const StringView& parent, const StringView& path)
    {
        const String normalizedParent = NormalizeAbsolute(parent);
        const String normalizedPath = NormalizeAbsolute(path);
        return FileSystem::AreFilePathsEquivalent(normalizedParent, normalizedPath) ||
            normalizedPath.StartsWith(normalizedParent + TEXT("/"), StringSearchCase::IgnoreCase);
    }

    bool EnsureParent(const StringView& path)
    {
        const String parent(StringUtils::GetDirectoryName(path));
        return parent.IsEmpty() || (!FileSystem::DirectoryExists(parent) && FileSystem::CreateDirectory(parent));
    }

    bool QueryFreeBytes(const StringView& path, uint64& bytes)
    {
#if PLATFORM_WINDOWS
        const String value(path);
        ULARGE_INTEGER available;
        return GetDiskFreeSpaceExW(*value, &available, nullptr, nullptr) == 0 ? true : (bytes = available.QuadPart, false);
#elif PLATFORM_LINUX || PLATFORM_MAC
        const StringAnsi value(path);
        struct statvfs info;
        if (statvfs(value.Get(), &info) != 0)
            return true;
        bytes = static_cast<uint64>(info.f_bavail) * static_cast<uint64>(info.f_frsize);
        return false;
#else
        return true;
#endif
    }

    bool HashFile(const StringView& path, String& hash, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> bytes;
        if (File::ReadAllBytes(path, bytes))
            return Fail(diagnostic, path, TEXT("Migration cannot read and hash a required file."));
        hash = String(ContentHash::Compute(bytes.Get(), bytes.Count()).ToString());
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    void HashString(ContentHasher& hasher, const StringView& value)
    {
        const StringAnsi text(value);
        const uint32 length = text.Length();
        hasher.Update(&length, sizeof(length));
        hasher.Update(text.Get(), text.Length());
    }

    bool CollectDirectories(const StringView& root, Array<String>& directories)
    {
        Array<String> pending;
        pending.Add(String(root));
        for (int32 index = 0; index < pending.Count(); index++)
        {
            Array<String> children;
            if (FileSystem::GetChildDirectories(children, pending[index]))
                return true;
            for (String& child : children)
            {
                directories.Add(child);
                pending.Add(MoveTemp(child));
            }
        }
        return false;
    }

    bool HashTree(const StringView& root, String& hash, AssetPipelineDiagnostic& diagnostic)
    {
        if (!FileSystem::DirectoryExists(root))
            return Fail(diagnostic, root, TEXT("Migration source tree is missing."));
        Array<String> files;
        Array<String> directories;
        if (FileSystem::DirectoryGetFiles(files, String(root), TEXT("*"), DirectorySearchOption::AllDirectories) ||
            CollectDirectories(root, directories))
            return Fail(diagnostic, root, TEXT("Migration cannot enumerate the complete source tree."));
        if (files.Count() > 1)
            std::sort(files.Get(), files.Get() + files.Count());
        if (directories.Count() > 1)
            std::sort(directories.Get(), directories.Get() + directories.Count());
        ContentHasher hasher;
        const StringAnsi domain("flax-project-migration-tree-v1");
        hasher.Update(domain.Get(), domain.Length());
        for (const String& directory : directories)
        {
            String relative = FileSystem::ConvertAbsolutePathToRelative(root, directory);
            relative.Replace('\\', '/');
            HashString(hasher, relative + TEXT("/"));
        }
        SourceHashCache cache;
        for (const String& file : files)
        {
            String relative = FileSystem::ConvertAbsolutePathToRelative(root, file);
            relative.Replace('\\', '/');
            HashString(hasher, relative);
            ContentHash content;
            SourceHashFileState state;
            if (cache.HashFile(file, content, state, diagnostic))
                return true;
            hasher.Update(content.Bytes, sizeof(content.Bytes));
        }
        hash = String(hasher.Finalize().ToString());
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool WriteAtomic(const StringView& path, const StringAnsiView& bytes, AssetPipelineDiagnostic& diagnostic)
    {
        const String destination(path);
        const String staging = destination + TEXT(".tmp");
        if (EnsureParent(destination) || File::WriteAllBytes(staging, bytes.Get(), bytes.Length()))
        {
            FileSystem::DeleteFile(staging);
            return Fail(diagnostic, destination, TEXT("Migration atomic staging write failed."));
        }
        Array<byte> verify;
        if (File::ReadAllBytes(staging, verify) || verify.Count() != bytes.Length() ||
            (verify.Count() != 0 && Platform::MemoryCompare(verify.Get(), bytes.Get(), verify.Count()) != 0) ||
            FileSystem::MoveFile(destination, staging, true))
        {
            FileSystem::DeleteFile(staging);
            return Fail(diagnostic, destination, TEXT("Migration atomic replacement failed verification."));
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    void AddAnsi(JsonValue& object, const char* name, const StringAnsiView& value, JsonAlloc& allocator)
    {
        object.AddMember(JsonValue(name, allocator), JsonValue(value.Get(), value.Length(), allocator), allocator);
    }

    void AddString(JsonValue& object, const char* name, const StringView& value, JsonAlloc& allocator)
    {
        AddAnsi(object, name, StringAnsi(value), allocator);
    }

    bool ReadString(const JsonValue& object, const char* name, String& value)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || !member->value.IsString())
            return true;
        value = String(StringAnsiView(member->value.GetString(), member->value.GetStringLength()));
        return false;
    }

    bool SerializeJournal(const MigrationJournalState& journal, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
    {
        JsonDocument document;
        document.SetObject();
        JsonAlloc& allocator = document.GetAllocator();
        document.AddMember("formatVersion", MigrationJournalState::FormatVersion, allocator);
        AddString(document, "transactionId", journal.TransactionId.ToString(Guid::FormatType::N), allocator);
        AddString(document, "phase", PhaseName(journal.Phase), allocator);
        AddString(document, "projectDescriptorPath", journal.ProjectDescriptorPath, allocator);
        AddString(document, "projectRoot", journal.ProjectRoot, allocator);
        AddString(document, "contentRoot", journal.ContentRoot, allocator);
        AddString(document, "backupRoot", journal.BackupRoot, allocator);
        AddString(document, "descriptorBackupPath", journal.DescriptorBackupPath, allocator);
        AddString(document, "contentBackupPath", journal.ContentBackupPath, allocator);
        AddString(document, "journalPath", journal.JournalPath, allocator);
        AddString(document, "lockPath", journal.LockPath, allocator);
        AddString(document, "candidateMarkerPath", journal.CandidateMarkerPath, allocator);
        AddString(document, "candidateMarkerFingerprint", journal.CandidateMarkerFingerprint, allocator);
        AddString(document, "initialSourceFingerprint", journal.InitialSourceFingerprint, allocator);
        AddString(document, "currentSourceFingerprint", journal.CurrentSourceFingerprint, allocator);
        AddString(document, "verificationFingerprint", journal.VerificationFingerprint, allocator);
        AddString(document, "completionReportFingerprint", journal.CompletionReportFingerprint, allocator);

        JsonValue mounts(rapidjson::kArrayType);
        for (int32 i = 0; i < journal.MountRoots.Count(); i++)
        {
            JsonValue value(rapidjson::kObjectType);
            AddString(value, "root", journal.MountRoots[i], allocator);
            AddString(value, "fingerprint", journal.MountFingerprints[i], allocator);
            mounts.PushBack(value, allocator);
        }
        document.AddMember("readOnlyMounts", mounts, allocator);

        JsonValue evidence(rapidjson::kObjectType);
        for (int32 i = 0; i < ARRAY_COUNT(journal.EvidenceHashes); i++)
            AddString(evidence, StringAnsi::Format("m{0}", i).Get(), journal.EvidenceHashes[i], allocator);
        document.AddMember("evidence", evidence, allocator);

        JsonValue operations(rapidjson::kArrayType);
        for (const JournalOperation& operation : journal.Operations)
        {
            JsonValue value(rapidjson::kObjectType);
            AddString(value, "phase", PhaseName(operation.Phase), allocator);
            AddString(value, "name", operation.Name, allocator);
            AddString(value, "path", operation.Path, allocator);
            AddString(value, "temporaryPath", operation.TemporaryPath, allocator);
            AddString(value, "preimageHash", operation.PreimageHash, allocator);
            AddString(value, "postimageHash", operation.PostimageHash, allocator);
            value.AddMember("committed", operation.Committed, allocator);
            operations.PushBack(value, allocator);
        }
        document.AddMember("operations", operations, allocator);

        Array<StringAnsi> order;
        order.Add("formatVersion");
        order.Add("transactionId");
        order.Add("phase");
        order.Add("projectDescriptorPath");
        order.Add("projectRoot");
        order.Add("contentRoot");
        order.Add("backupRoot");
        order.Add("descriptorBackupPath");
        order.Add("contentBackupPath");
        order.Add("journalPath");
        order.Add("lockPath");
        order.Add("candidateMarkerPath");
        order.Add("candidateMarkerFingerprint");
        order.Add("initialSourceFingerprint");
        order.Add("currentSourceFingerprint");
        order.Add("verificationFingerprint");
        order.Add("completionReportFingerprint");
        order.Add("readOnlyMounts");
        order.Add("evidence");
        order.Add("operations");
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Write(document, output, error, &order))
            return Fail(diagnostic, journal.JournalPath, TEXT("Migration journal canonical serialization failed."));
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool ParseJournal(const StringAnsiView& source, const StringView& path, MigrationJournalState& journal, AssetPipelineDiagnostic& diagnostic)
    {
        StringAnsi canonical;
        CanonicalJsonError canonicalError;
        if (CanonicalJsonWriter::Canonicalize(source, canonical, canonicalError))
            return Fail(diagnostic, path, TEXT("Migration journal is malformed."));
        JsonDocument document;
        document.Parse(canonical.Get(), canonical.Length());
        if (document.HasParseError() || !document.IsObject())
            return Fail(diagnostic, path, TEXT("Migration journal root is invalid."));
        const auto version = document.FindMember("formatVersion");
        if (version == document.MemberEnd() || !version->value.IsInt() || version->value.GetInt() != MigrationJournalState::FormatVersion)
            return Fail(diagnostic, path, TEXT("Migration journal format version is unsupported."));
        String transaction;
        String phase;
        if (ReadString(document, "transactionId", transaction) || Guid::Parse(transaction, journal.TransactionId) || !journal.TransactionId.IsValid() ||
            ReadString(document, "phase", phase) || ParsePhase(phase, journal.Phase) ||
            ReadString(document, "projectDescriptorPath", journal.ProjectDescriptorPath) ||
            ReadString(document, "projectRoot", journal.ProjectRoot) || ReadString(document, "contentRoot", journal.ContentRoot) ||
            ReadString(document, "backupRoot", journal.BackupRoot) || ReadString(document, "descriptorBackupPath", journal.DescriptorBackupPath) ||
            ReadString(document, "contentBackupPath", journal.ContentBackupPath) || ReadString(document, "journalPath", journal.JournalPath) ||
            ReadString(document, "lockPath", journal.LockPath) || ReadString(document, "candidateMarkerPath", journal.CandidateMarkerPath) ||
            ReadString(document, "candidateMarkerFingerprint", journal.CandidateMarkerFingerprint) ||
            ReadString(document, "initialSourceFingerprint", journal.InitialSourceFingerprint) ||
            ReadString(document, "currentSourceFingerprint", journal.CurrentSourceFingerprint) ||
            ReadString(document, "verificationFingerprint", journal.VerificationFingerprint) ||
            ReadString(document, "completionReportFingerprint", journal.CompletionReportFingerprint))
            return Fail(diagnostic, path, TEXT("Migration journal is missing required state."));
        if (!FileSystem::AreFilePathsEquivalent(journal.JournalPath, path))
            return Fail(diagnostic, path, TEXT("Migration journal path identity does not match its contents."));

        const auto mounts = document.FindMember("readOnlyMounts");
        if (mounts == document.MemberEnd() || !mounts->value.IsArray())
            return Fail(diagnostic, path, TEXT("Migration journal has no mount inventory."));
        for (const JsonValue& value : mounts->value.GetArray())
        {
            String root;
            String fingerprint;
            if (!value.IsObject() || ReadString(value, "root", root) || ReadString(value, "fingerprint", fingerprint))
                return Fail(diagnostic, path, TEXT("Migration journal mount inventory is invalid."));
            journal.MountRoots.Add(MoveTemp(root));
            journal.MountFingerprints.Add(MoveTemp(fingerprint));
        }

        const auto evidence = document.FindMember("evidence");
        if (evidence == document.MemberEnd() || !evidence->value.IsObject())
            return Fail(diagnostic, path, TEXT("Migration journal evidence is invalid."));
        for (int32 i = 0; i < ARRAY_COUNT(journal.EvidenceHashes); i++)
        {
            const StringAnsi key = StringAnsi::Format("m{0}", i);
            if (ReadString(evidence->value, key.Get(), journal.EvidenceHashes[i]))
                return Fail(diagnostic, path, TEXT("Migration journal evidence set is incomplete."));
        }

        const auto operations = document.FindMember("operations");
        if (operations == document.MemberEnd() || !operations->value.IsArray())
            return Fail(diagnostic, path, TEXT("Migration journal operation set is invalid."));
        for (const JsonValue& value : operations->value.GetArray())
        {
            if (!value.IsObject())
                return Fail(diagnostic, path, TEXT("Migration journal operation is invalid."));
            JournalOperation operation;
            String operationPhase;
            const auto committed = value.FindMember("committed");
            if (ReadString(value, "phase", operationPhase) || ParsePhase(operationPhase, operation.Phase) ||
                ReadString(value, "name", operation.Name) || ReadString(value, "path", operation.Path) ||
                ReadString(value, "temporaryPath", operation.TemporaryPath) || ReadString(value, "preimageHash", operation.PreimageHash) ||
                ReadString(value, "postimageHash", operation.PostimageHash) || committed == value.MemberEnd() || !committed->value.IsBool())
                return Fail(diagnostic, path, TEXT("Migration journal operation is invalid."));
            operation.Committed = committed->value.GetBool();
            journal.Operations.Add(MoveTemp(operation));
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool SaveJournal(const MigrationJournalState& journal, AssetPipelineDiagnostic& diagnostic)
    {
        StringAnsi source;
        return SerializeJournal(journal, source, diagnostic) || WriteAtomic(journal.JournalPath, source, diagnostic);
    }

    bool LoadJournal(const StringView& path, MigrationJournalState& journal, AssetPipelineDiagnostic& diagnostic)
    {
        if (String(path).EndsWith(TEXT(".tmp"), StringSearchCase::IgnoreCase))
            return Fail(diagnostic, path, TEXT("A migration journal staging file is not committed state."));
        StringAnsi source;
        if (File::ReadAllText(path, source))
            return Fail(diagnostic, path, TEXT("Migration journal cannot be read."));
        return ParseJournal(source, path, journal, diagnostic);
    }

    bool CreateLock(const MigrationJournalState& journal, AssetPipelineDiagnostic& diagnostic)
    {
        if (FileSystem::FileExists(journal.LockPath))
            return Fail(diagnostic, journal.LockPath, TEXT("Another migration transaction already owns the project lock."));
        JsonDocument document;
        document.SetObject();
        JsonAlloc& allocator = document.GetAllocator();
        AddString(document, "transactionId", journal.TransactionId.ToString(Guid::FormatType::N), allocator);
        AddString(document, "journalPath", journal.JournalPath, allocator);
        Array<StringAnsi> order;
        order.Add("transactionId");
        order.Add("journalPath");
        StringAnsi source;
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Write(document, source, error, &order) || EnsureParent(journal.LockPath))
            return Fail(diagnostic, journal.LockPath, TEXT("Migration project lock could not be prepared."));
        File* file = File::Open(journal.LockPath, FileMode::CreateNew, FileAccess::Write, FileShare::None);
        if (!file)
            return Fail(diagnostic, journal.LockPath, TEXT("Migration project lock could not be acquired."));
        uint32 written = 0;
        const bool failed = file->Write(source.Get(), source.Length(), &written) || written != static_cast<uint32>(source.Length());
        Delete(file);
        if (failed)
        {
            FileSystem::DeleteFile(journal.LockPath);
            return Fail(diagnostic, journal.LockPath, TEXT("Migration project lock record could not be written."));
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    File* AcquireLock(const MigrationJournalState& journal, AssetPipelineDiagnostic& diagnostic)
    {
        StringAnsi source;
        if (File::ReadAllText(journal.LockPath, source))
        {
            Fail(diagnostic, journal.LockPath, TEXT("Migration project lock is missing."));
            return nullptr;
        }
        JsonDocument document;
        document.Parse(source.Get(), source.Length());
        String transaction;
        String journalPath;
        if (document.HasParseError() || !document.IsObject() || ReadString(document, "transactionId", transaction) ||
            ReadString(document, "journalPath", journalPath) || transaction != journal.TransactionId.ToString(Guid::FormatType::N) ||
            !FileSystem::AreFilePathsEquivalent(journalPath, journal.JournalPath))
        {
            Fail(diagnostic, journal.LockPath, TEXT("Migration project lock belongs to different state."));
            return nullptr;
        }
        File* file = File::Open(journal.LockPath, FileMode::OpenExisting, FileAccess::Read, FileShare::None);
        if (!file)
        {
            Fail(diagnostic, journal.LockPath, TEXT("Migration project lock is currently held by another process."));
            return nullptr;
        }
        diagnostic = AssetPipelineDiagnostic();
        return file;
    }
}

namespace
{
    bool HasFatalPreflight(const AssetProjectValidationResult& validation)
    {
        for (const AssetPipelineDiagnostic& diagnostic : validation.Diagnostics)
        {
            if (diagnostic.Severity != AssetPipelineDiagnosticSeverity::Error)
                continue;
            switch (diagnostic.Code)
            {
            case AssetPipelineDiagnosticCode::DuplicateGuid:
            case AssetPipelineDiagnosticCode::PathCollision:
            case AssetPipelineDiagnosticCode::SourceMissing:
            case AssetPipelineDiagnosticCode::SourceBusy:
            case AssetPipelineDiagnosticCode::MetaParseError:
            case AssetPipelineDiagnosticCode::InvalidMeta:
            case AssetPipelineDiagnosticCode::FutureMetaVersion:
                return true;
            case AssetPipelineDiagnosticCode::InvalidSettingsCombination:
                if (diagnostic.Message.Contains(TEXT("duplicate"), StringSearchCase::IgnoreCase) ||
                    diagnostic.Message.Contains(TEXT("newer"), StringSearchCase::IgnoreCase))
                    return true;
                break;
            default:
                break;
            }
        }
        return false;
    }

    bool VerifyReadOnlyMounts(const MigrationJournalState& journal, AssetPipelineDiagnostic& diagnostic)
    {
        if (journal.MountRoots.Count() != journal.MountFingerprints.Count())
            return Fail(diagnostic, journal.JournalPath, TEXT("Migration mount inventory is contradictory."));
        for (int32 i = 0; i < journal.MountRoots.Count(); i++)
        {
            String current;
            if (HashTree(journal.MountRoots[i], current, diagnostic))
                return true;
            if (current != journal.MountFingerprints[i])
                return Fail(diagnostic, journal.MountRoots[i],
                    TEXT("A read-only content mount changed after migration preflight."),
                    TEXT("Restore the exact mounted content version used at M0 before resuming."));
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool VerifySourceFingerprint(const MigrationJournalState& journal, AssetPipelineDiagnostic& diagnostic)
    {
        String current;
        if (HashTree(journal.ContentRoot, current, diagnostic))
            return true;
        if (current != journal.CurrentSourceFingerprint)
            return Fail(diagnostic, journal.ContentRoot,
                TEXT("The project source tree changed outside the current migration phase."),
                TEXT("Restore the recorded phase output or rollback before resuming."));
        return false;
    }

    bool StoreCanonicalReport(const MigrationJournalState& journal, ProjectMigrationPhase phase, const StringView& report,
        const StringView& name, String& hash, AssetPipelineDiagnostic& diagnostic)
    {
        if (report.IsEmpty())
            return Fail(diagnostic, journal.JournalPath, TEXT("This migration phase requires non-empty verification evidence."));
        const StringAnsi input(report);
        StringAnsi canonical;
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Canonicalize(input, canonical, error))
            return Fail(diagnostic, journal.JournalPath, TEXT("Migration evidence must be valid deterministic JSON."));
        const String outputPath = journal.BackupRoot / String::Format(TEXT("{0}-{1}.json"), PhaseName(phase), name);
        if (WriteAtomic(outputPath, canonical, diagnostic))
            return true;
        hash = String(ContentHash::Compute(canonical.Get(), canonical.Length()).ToString());
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool WriteIdentityMap(const MigrationJournalState& journal, String& hash, AssetPipelineDiagnostic& diagnostic)
    {
        Array<String> metas;
        if (FileSystem::DirectoryGetFiles(metas, journal.ContentRoot, TEXT("*.meta"), DirectorySearchOption::AllDirectories))
            return Fail(diagnostic, journal.ContentRoot, TEXT("Migration cannot enumerate metadata for the identity map."));
        if (metas.Count() > 1)
            std::sort(metas.Get(), metas.Get() + metas.Count());
        JsonDocument document;
        document.SetObject();
        JsonAlloc& allocator = document.GetAllocator();
        document.AddMember("schemaVersion", 1, allocator);
        AddString(document, "transactionId", journal.TransactionId.ToString(Guid::FormatType::N), allocator);
        JsonValue entries(rapidjson::kArrayType);
        for (const String& metaPath : metas)
        {
            AssetMeta meta;
            if (AssetMeta::Load(metaPath, meta, diagnostic))
                return true;
            JsonValue entry(rapidjson::kObjectType);
            String sourcePath = metaPath.Substring(0, metaPath.Length() - 5);
            String relative = FileSystem::ConvertAbsolutePathToRelative(journal.ContentRoot, sourcePath);
            relative.Replace('\\', '/');
            AddString(entry, "sourcePath", relative, allocator);
            AddString(entry, "fileGuid", meta.ID.ToString(Guid::FormatType::N), allocator);
            AddString(entry, "typeName", meta.AssetType, allocator);
            entry.AddMember("sourceKind", static_cast<int32>(meta.SourceKind), allocator);
            entry.AddMember("folder", meta.FolderAsset, allocator);
            entries.PushBack(entry, allocator);
        }
        document.AddMember("entries", entries, allocator);
        Array<StringAnsi> order;
        order.Add("schemaVersion");
        order.Add("transactionId");
        order.Add("entries");
        StringAnsi source;
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Write(document, source, error, &order))
            return Fail(diagnostic, journal.ContentRoot, TEXT("Migration identity map serialization failed."));
        const String outputPath = journal.BackupRoot / TEXT("M4MetadataAndIdentityConverted-identity-map.json");
        if (WriteAtomic(outputPath, source, diagnostic))
            return true;
        hash = String(ContentHash::Compute(source.Get(), source.Length()).ToString());
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool NextPhaseNeedsEvidence(ProjectMigrationPhase phase)
    {
        return phase == ProjectMigrationPhase::M0PreflightAndBackup ||
            phase == ProjectMigrationPhase::M7CleanDatabaseImported;
    }

    ProjectMigrationResult MakeResult(const MigrationJournalState& journal, bool succeeded, const StringView& message)
    {
        ProjectMigrationResult result;
        result.Succeeded = succeeded;
        result.Completed = journal.Phase == ProjectMigrationPhase::M9Committed;
        result.AwaitingExternalWork = succeeded && NextPhaseNeedsEvidence(journal.Phase);
        result.TransactionId = journal.TransactionId;
        result.Phase = journal.Phase;
        result.JournalPath = journal.JournalPath;
        result.BackupRoot = journal.BackupRoot;
        result.SourceTreeFingerprint = journal.CurrentSourceFingerprint;
        result.VerificationFingerprint = journal.VerificationFingerprint;
        result.Message = message;
        return result;
    }

    void AddFailure(ProjectMigrationResult& result, AssetPipelineDiagnostic&& diagnostic)
    {
        result.Succeeded = false;
        result.Message = diagnostic.Message;
        result.Diagnostics.Add(MoveTemp(diagnostic));
    }

    JournalOperation& BeginOperation(MigrationJournalState& journal, ProjectMigrationPhase phase, const StringView& name,
        const StringView& path, const StringView& temporaryPath, const StringView& preimageHash)
    {
        JournalOperation operation;
        operation.Phase = phase;
        operation.Name = name;
        operation.Path = path;
        operation.TemporaryPath = temporaryPath;
        operation.PreimageHash = preimageHash;
        journal.Operations.Add(MoveTemp(operation));
        return journal.Operations.Last();
    }

    bool PreparePhaseSnapshot(MigrationJournalState& journal, ProjectMigrationPhase phase, String& snapshot,
        AssetPipelineDiagnostic& diagnostic)
    {
        snapshot = journal.BackupRoot / (String(PhaseName(phase)) + TEXT("-preimage"));
        if (!FileSystem::DirectoryExists(snapshot) && FileSystem::CopyDirectory(snapshot, journal.ContentRoot, true))
            return Fail(diagnostic, snapshot, TEXT("Migration could not create the durable phase preimage."));
        String fingerprint;
        if (HashTree(snapshot, fingerprint, diagnostic))
            return true;
        if (fingerprint != journal.CurrentSourceFingerprint)
            return Fail(diagnostic, snapshot, TEXT("Migration phase preimage does not match the journal source fingerprint."));
        return false;
    }

    bool RecordFileMutations(MigrationJournalState& journal, ProjectMigrationPhase phase, const StringView& snapshot,
        AssetPipelineDiagnostic& diagnostic)
    {
        Array<String> before;
        Array<String> after;
        if (FileSystem::DirectoryGetFiles(before, String(snapshot), TEXT("*"), DirectorySearchOption::AllDirectories) ||
            FileSystem::DirectoryGetFiles(after, journal.ContentRoot, TEXT("*"), DirectorySearchOption::AllDirectories))
            return Fail(diagnostic, journal.ContentRoot, TEXT("Migration cannot enumerate per-file conversion results."));
        Array<String> relatives;
        for (const String& path : before)
        {
            String relative = FileSystem::ConvertAbsolutePathToRelative(snapshot, path);
            relative.Replace('\\', '/');
            relatives.AddUnique(relative);
        }
        for (const String& path : after)
        {
            String relative = FileSystem::ConvertAbsolutePathToRelative(journal.ContentRoot, path);
            relative.Replace('\\', '/');
            relatives.AddUnique(relative);
        }
        if (relatives.Count() > 1)
            std::sort(relatives.Get(), relatives.Get() + relatives.Count());
        for (const String& relative : relatives)
        {
            const String preimage = String(snapshot) / relative;
            const String postimage = journal.ContentRoot / relative;
            String beforeHash;
            String afterHash;
            if (FileSystem::FileExists(preimage) && HashFile(preimage, beforeHash, diagnostic))
                return true;
            if (FileSystem::FileExists(postimage) && HashFile(postimage, afterHash, diagnostic))
                return true;
            if (beforeHash == afterHash)
                continue;
            JournalOperation& file = BeginOperation(journal, phase,
                beforeHash.IsEmpty() ? TEXT("CreateFile") : (afterHash.IsEmpty() ? TEXT("DeleteFile") : TEXT("ConvertFile")),
                postimage, preimage, beforeHash);
            file.PostimageHash = afterHash;
            file.Committed = true;
        }
        return false;
    }

    bool RecoverInterruptedSourcePhase(MigrationJournalState& journal, AssetPipelineDiagnostic& diagnostic)
    {
        if (journal.Operations.IsEmpty())
            return false;
        const JournalOperation& operation = journal.Operations.Last();
        if (operation.Committed || operation.Phase < ProjectMigrationPhase::M2CanonicalRootsAndSettings ||
            operation.Phase > ProjectMigrationPhase::M6AuthoredSourcesWritten ||
            !FileSystem::DirectoryExists(operation.TemporaryPath))
            return false;
        String snapshotHash;
        if (HashTree(operation.TemporaryPath, snapshotHash, diagnostic) || snapshotHash != operation.PreimageHash)
            return Fail(diagnostic, operation.TemporaryPath, TEXT("Interrupted migration phase preimage is missing or corrupt."));
        String currentHash;
        if (HashTree(journal.ContentRoot, currentHash, diagnostic))
            return true;
        if (currentHash != snapshotHash)
        {
            if (FileSystem::DeleteDirectory(journal.ContentRoot, true) ||
                FileSystem::CopyDirectory(journal.ContentRoot, operation.TemporaryPath, true) ||
                HashTree(journal.ContentRoot, currentHash, diagnostic) || currentHash != snapshotHash)
                return Fail(diagnostic, journal.ContentRoot, TEXT("Interrupted migration phase could not restore its durable preimage."));
        }
        journal.Operations.RemoveLast();
        return SaveJournal(journal, diagnostic);
    }

    bool CompleteM0(MigrationJournalState& journal, const AssetProjectValidationResult& validation, AssetPipelineDiagnostic& diagnostic)
    {
        String descriptorHash;
        if (HashFile(journal.ProjectDescriptorPath, descriptorHash, diagnostic))
            return true;
        JournalOperation& descriptorOperation = BeginOperation(journal, ProjectMigrationPhase::M0PreflightAndBackup,
            TEXT("BackupProjectDescriptor"), journal.ProjectDescriptorPath, journal.DescriptorBackupPath, descriptorHash);
        if (SaveJournal(journal, diagnostic))
            return true;
        if (FileSystem::FileExists(journal.DescriptorBackupPath))
        {
            String existing;
            if (HashFile(journal.DescriptorBackupPath, existing, diagnostic) || existing != descriptorHash)
                return Fail(diagnostic, journal.DescriptorBackupPath, TEXT("Existing project descriptor backup does not match its preimage."));
        }
        else if (EnsureParent(journal.DescriptorBackupPath) || FileSystem::CopyFile(journal.DescriptorBackupPath, journal.ProjectDescriptorPath))
        {
            return Fail(diagnostic, journal.DescriptorBackupPath, TEXT("Project descriptor backup failed."));
        }
        descriptorOperation.PostimageHash = descriptorHash;
        descriptorOperation.Committed = true;

        String sourceHash;
        if (HashTree(journal.ContentRoot, sourceHash, diagnostic))
            return true;
        JournalOperation& contentOperation = BeginOperation(journal, ProjectMigrationPhase::M0PreflightAndBackup,
            TEXT("BackupContentTree"), journal.ContentRoot, journal.ContentBackupPath, sourceHash);
        if (SaveJournal(journal, diagnostic))
            return true;
        if (FileSystem::DirectoryExists(journal.ContentBackupPath))
        {
            String existing;
            if (HashTree(journal.ContentBackupPath, existing, diagnostic) || existing != sourceHash)
                return Fail(diagnostic, journal.ContentBackupPath, TEXT("Existing Content backup does not match its preimage."));
        }
        else if (FileSystem::CopyDirectory(journal.ContentBackupPath, journal.ContentRoot, true))
        {
            return Fail(diagnostic, journal.ContentBackupPath, TEXT("Complete Content source-tree backup failed."));
        }
        String copiedHash;
        if (HashTree(journal.ContentBackupPath, copiedHash, diagnostic) || copiedHash != sourceHash)
            return Fail(diagnostic, journal.ContentBackupPath, TEXT("Content backup verification failed."));
        contentOperation.PostimageHash = copiedHash;
        contentOperation.Committed = true;

        if (StoreCanonicalReport(journal, ProjectMigrationPhase::M0PreflightAndBackup, validation.ReportJson,
            TEXT("dry-run"), journal.EvidenceHashes[0], diagnostic))
            return true;
        journal.InitialSourceFingerprint = sourceHash;
        journal.CurrentSourceFingerprint = sourceHash;
        journal.Phase = ProjectMigrationPhase::M0PreflightAndBackup;
        return SaveJournal(journal, diagnostic);
    }
}

ProjectMigrationResult ProjectMigrationOrchestrator::Begin(const StringView& projectDescriptorPath, const StringView& contentRoot,
    const StringView& backupParent, const StringView& journalPath)
{
    MigrationJournalState journal;
    journal.TransactionId = Guid::New();
    journal.ProjectDescriptorPath = NormalizeAbsolute(projectDescriptorPath);
    journal.ProjectRoot = NormalizeAbsolute(StringUtils::GetDirectoryName(journal.ProjectDescriptorPath));
    journal.ContentRoot = NormalizeAbsolute(contentRoot);
    journal.JournalPath = NormalizeAbsolute(journalPath);
    journal.LockPath = journal.ProjectRoot / TEXT(".asset-migration.lock");
    const String normalizedBackupParent = NormalizeAbsolute(backupParent);
    journal.BackupRoot = normalizedBackupParent / (TEXT("AssetMigration-") + journal.TransactionId.ToString(Guid::FormatType::N));
    journal.DescriptorBackupPath = journal.BackupRoot / String(StringUtils::GetFileName(journal.ProjectDescriptorPath));
    journal.ContentBackupPath = journal.BackupRoot / TEXT("Content");
    journal.CandidateMarkerPath = journal.ProjectRoot / (TEXT(".asset-system-v3-") + journal.TransactionId.ToString(Guid::FormatType::N) + TEXT(".flaxproj"));
    ProjectMigrationResult result = MakeResult(journal, false, TEXT("Migration preflight failed."));
    AssetPipelineDiagnostic diagnostic;

    const String expectedContent = NormalizeAbsolute(journal.ProjectRoot / TEXT("Content"));
    if (!FileSystem::FileExists(journal.ProjectDescriptorPath) || !FileSystem::DirectoryExists(journal.ContentRoot) ||
        !FileSystem::AreFilePathsEquivalent(expectedContent, journal.ContentRoot))
    {
        AddFailure(result, MakeDiagnostic(journal.ProjectDescriptorPath,
            TEXT("Migration requires an existing project descriptor and its exact canonical Content root.")));
        return result;
    }
    if (IsSameOrChild(journal.ProjectRoot, journal.JournalPath) || IsSameOrChild(journal.ProjectRoot, journal.BackupRoot))
    {
        AddFailure(result, MakeDiagnostic(journal.JournalPath,
            TEXT("Migration journal and durable backup parent must be outside the project tree so a clean Library rebuild cannot erase recovery state.")));
        return result;
    }
    if (FileSystem::FileExists(journal.JournalPath) || FileSystem::FileExists(journal.LockPath))
    {
        AddFailure(result, MakeDiagnostic(journal.LockPath, TEXT("A migration journal or exclusive project lock already exists.")));
        return result;
    }

    File* descriptorProbe = File::Open(journal.ProjectDescriptorPath, FileMode::OpenExisting, FileAccess::Read, FileShare::Read);
    if (!descriptorProbe)
    {
        AddFailure(result, MakeDiagnostic(journal.ProjectDescriptorPath,
            TEXT("Migration preflight cannot acquire a stable project-descriptor read handle; another process may be writing the project.")));
        return result;
    }
    Delete(descriptorProbe);

    if (!FileSystem::DirectoryExists(normalizedBackupParent) && FileSystem::CreateDirectory(normalizedBackupParent))
    {
        AddFailure(result, MakeDiagnostic(normalizedBackupParent, TEXT("Migration cannot create the external backup parent.")));
        return result;
    }
    uint64 freeBytes = 0;
    const uint64 sourceBytes = FileSystem::GetDirectorySize(journal.ContentRoot) + FileSystem::GetFileSize(journal.ProjectDescriptorPath);
    if (QueryFreeBytes(normalizedBackupParent, freeBytes) || freeBytes < sourceBytes * 2)
    {
        AddFailure(result, MakeDiagnostic(normalizedBackupParent,
            TEXT("Migration preflight requires free space for a complete backup and one verified recovery staging copy.")));
        return result;
    }

    AssetProjectValidationResult validation = AssetProjectValidator::Validate(journal.ProjectDescriptorPath, journal.ContentRoot);
    result.Diagnostics = validation.Diagnostics;
    if (validation.Bootstrap.ReadOnly || validation.Bootstrap.AssetSystemVersion >= AssetPipelineBootstrap::CurrentAssetSystemVersion ||
        HasFatalPreflight(validation) || validation.ReportJson.IsEmpty())
    {
        result.Message = TEXT("Migration dry-run found fatal preflight blockers or the project is already at/newer than asset-system v3.");
        return result;
    }
    for (const AssetMount& mount : validation.Bootstrap.Mounts)
    {
        if (mount.Kind == AssetMountKind::ProjectContent)
            continue;
        String fingerprint;
        if (HashTree(mount.PhysicalRoot, fingerprint, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            return result;
        }
        journal.MountRoots.Add(mount.PhysicalRoot);
        journal.MountFingerprints.Add(MoveTemp(fingerprint));
    }

    if (FileSystem::CreateDirectory(journal.BackupRoot) || CreateLock(journal, diagnostic) || SaveJournal(journal, diagnostic) ||
        CompleteM0(journal, validation, diagnostic))
    {
        result = MakeResult(journal, false, diagnostic.Message);
        result.Diagnostics = validation.Diagnostics;
        result.Diagnostics.Add(MoveTemp(diagnostic));
        return result;
    }
    result = MakeResult(journal, true, TEXT("M0 completed: dry-run and verified external backup are durable."));
    result.Diagnostics = validation.Diagnostics;
    return result;
}

namespace
{
    bool HasErrors(const Array<AssetPipelineDiagnostic>& diagnostics)
    {
        for (const AssetPipelineDiagnostic& diagnostic : diagnostics)
        {
            if (diagnostic.Severity == AssetPipelineDiagnosticSeverity::Error)
                return true;
        }
        return false;
    }

    void FailFromStep(ProjectMigrationResult& result, const MigrationJournalState& journal, Array<AssetPipelineDiagnostic>& diagnostics,
        const StringView& fallback)
    {
        result = MakeResult(journal, false, fallback);
        result.Diagnostics = MoveTemp(diagnostics);
        if (result.Diagnostics.IsEmpty())
            result.Diagnostics.Add(MakeDiagnostic(journal.ContentRoot, fallback));
        result.Message = result.Diagnostics[0].Message;
    }

    bool CommitPhase(MigrationJournalState& journal, JournalOperation& operation, ProjectMigrationPhase phase,
        const StringView& postimageHash, AssetPipelineDiagnostic& diagnostic)
    {
        const String snapshot = operation.TemporaryPath;
        operation.PostimageHash = postimageHash;
        operation.Committed = true;
        if (phase >= ProjectMigrationPhase::M2CanonicalRootsAndSettings &&
            phase <= ProjectMigrationPhase::M6AuthoredSourcesWritten &&
            RecordFileMutations(journal, phase, snapshot, diagnostic))
            return true;
        journal.CurrentSourceFingerprint = postimageHash;
        journal.Phase = phase;
        return SaveJournal(journal, diagnostic);
    }

    bool WriteCompletionReport(const MigrationJournalState& journal, String& hash, AssetPipelineDiagnostic& diagnostic)
    {
        JsonDocument document;
        document.SetObject();
        JsonAlloc& allocator = document.GetAllocator();
        document.AddMember("schemaVersion", 1, allocator);
        AddString(document, "transactionId", journal.TransactionId.ToString(Guid::FormatType::N), allocator);
        AddString(document, "sourceTreeFingerprint", journal.CurrentSourceFingerprint, allocator);
        AddString(document, "candidateMarkerFingerprint", journal.CandidateMarkerFingerprint, allocator);
        AddString(document, "verificationFingerprint", journal.VerificationFingerprint, allocator);
        JsonValue evidence(rapidjson::kObjectType);
        for (int32 i = 0; i <= 8; i++)
            AddString(evidence, StringAnsi::Format("m{0}", i).Get(), journal.EvidenceHashes[i], allocator);
        document.AddMember("evidence", evidence, allocator);
        Array<StringAnsi> order;
        order.Add("schemaVersion");
        order.Add("transactionId");
        order.Add("sourceTreeFingerprint");
        order.Add("candidateMarkerFingerprint");
        order.Add("verificationFingerprint");
        order.Add("evidence");
        StringAnsi source;
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Write(document, source, error, &order))
            return Fail(diagnostic, journal.BackupRoot, TEXT("Migration completion report serialization failed."));
        const String path = journal.BackupRoot / TEXT("migration-completion.json");
        if (WriteAtomic(path, source, diagnostic))
            return true;
        hash = String(ContentHash::Compute(source.Get(), source.Length()).ToString());
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    void ComputeVerificationFingerprint(MigrationJournalState& journal)
    {
        ContentHasher hasher;
        const StringAnsi domain("flax-project-migration-verification-v1");
        hasher.Update(domain.Get(), domain.Length());
        HashString(hasher, journal.TransactionId.ToString(Guid::FormatType::N));
        HashString(hasher, journal.CurrentSourceFingerprint);
        HashString(hasher, journal.CandidateMarkerFingerprint);
        for (int32 i = 0; i <= 8; i++)
            HashString(hasher, journal.EvidenceHashes[i]);
        for (const String& mountFingerprint : journal.MountFingerprints)
            HashString(hasher, mountFingerprint);
        journal.VerificationFingerprint = String(hasher.Finalize().ToString());
    }
}

ProjectMigrationResult ProjectMigrationOrchestrator::Resume(const StringView& journalPath, const ProjectMigrationEvidence& evidence)
{
    MigrationJournalState journal;
    AssetPipelineDiagnostic diagnostic;
    if (LoadJournal(NormalizeAbsolute(journalPath), journal, diagnostic))
    {
        ProjectMigrationResult result;
        result.JournalPath = NormalizeAbsolute(journalPath);
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }
    ProjectMigrationResult result = MakeResult(journal, false, TEXT("Migration phase failed."));
    if (journal.Phase == ProjectMigrationPhase::M9Committed)
        return MakeResult(journal, true, TEXT("Migration is already committed."));
    if (journal.Phase == ProjectMigrationPhase::RolledBack || journal.Phase == ProjectMigrationPhase::Failed)
    {
        AddFailure(result, MakeDiagnostic(journal.JournalPath, TEXT("This migration transaction cannot be resumed from its terminal state.")));
        return result;
    }
    File* lock = AcquireLock(journal, diagnostic);
    if (!lock)
    {
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }
    if (RecoverInterruptedSourcePhase(journal, diagnostic))
    {
        Delete(lock);
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }
    if (VerifyReadOnlyMounts(journal, diagnostic))
    {
        Delete(lock);
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }

    switch (journal.Phase)
    {
    case ProjectMigrationPhase::None:
    {
        const AssetProjectValidationResult validation = AssetProjectValidator::Validate(journal.ProjectDescriptorPath, journal.ContentRoot);
        if (HasFatalPreflight(validation) || CompleteM0(journal, validation, diagnostic))
        {
            result = MakeResult(journal, false, diagnostic.Message);
            result.Diagnostics = validation.Diagnostics;
            if (diagnostic.Code != AssetPipelineDiagnosticCode::None)
                result.Diagnostics.Add(MoveTemp(diagnostic));
        }
        else
        {
            result = MakeResult(journal, true, TEXT("M0 completed after recovery: backup and dry-run state are verified."));
            result.Diagnostics = validation.Diagnostics;
        }
        break;
    }
    case ProjectMigrationPhase::M0PreflightAndBackup:
    {
        if (VerifySourceFingerprint(journal, diagnostic) || !evidence.LegacyGraphFrozen)
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                diagnostic = MakeDiagnostic(journal.JournalPath, TEXT("M1 requires a frozen final legacy graph report."));
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        JournalOperation& operation = BeginOperation(journal, ProjectMigrationPhase::M1LegacyGraphFrozen,
            TEXT("FreezeLegacyGraph"), journal.BackupRoot / TEXT("M1LegacyGraphFrozen-legacy-graph.json"), StringView::Empty,
            journal.CurrentSourceFingerprint);
        if (SaveJournal(journal, diagnostic) || StoreCanonicalReport(journal, ProjectMigrationPhase::M1LegacyGraphFrozen,
            evidence.ReportJson, TEXT("legacy-graph"), journal.EvidenceHashes[1], diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        operation.PostimageHash = journal.EvidenceHashes[1];
        operation.Committed = true;
        journal.Phase = ProjectMigrationPhase::M1LegacyGraphFrozen;
        if (SaveJournal(journal, diagnostic))
            AddFailure(result, MoveTemp(diagnostic));
        else
            result = MakeResult(journal, true, TEXT("M1 completed: the final legacy graph evidence is frozen and hashed."));
        break;
    }
    case ProjectMigrationPhase::M1LegacyGraphFrozen:
    {
        if (VerifySourceFingerprint(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        String snapshot;
        if (PreparePhaseSnapshot(journal, ProjectMigrationPhase::M2CanonicalRootsAndSettings, snapshot, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        JournalOperation& operation = BeginOperation(journal, ProjectMigrationPhase::M2CanonicalRootsAndSettings,
            TEXT("EstablishCanonicalSettingsAndMounts"), journal.ContentRoot, snapshot, journal.CurrentSourceFingerprint);
        if (SaveJournal(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        Array<AssetPipelineDiagnostic> diagnostics;
        if (ProjectMigrationSteps::EstablishCanonicalSettings(journal.ProjectDescriptorPath, journal.ContentRoot, diagnostics) || HasErrors(diagnostics))
        {
            FailFromStep(result, journal, diagnostics, TEXT("M2 canonical settings or explicit mount conversion failed."));
            break;
        }
        String current;
        if (HashTree(journal.ContentRoot, current, diagnostic) || CommitPhase(journal, operation,
            ProjectMigrationPhase::M2CanonicalRootsAndSettings, current, diagnostic))
            AddFailure(result, MoveTemp(diagnostic));
        else
            result = MakeResult(journal, true, TEXT("M2 completed: canonical settings roles and explicit mounts are established."));
        break;
    }
    case ProjectMigrationPhase::M2CanonicalRootsAndSettings:
    {
        if (VerifySourceFingerprint(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        const String quarantine = journal.BackupRoot / TEXT("Quarantine");
        String snapshot;
        if (PreparePhaseSnapshot(journal, ProjectMigrationPhase::M3AssetsClassified, snapshot, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        JournalOperation& operation = BeginOperation(journal, ProjectMigrationPhase::M3AssetsClassified,
            TEXT("ClassifyAndConvertLegacyAssets"), journal.ContentRoot, snapshot, journal.CurrentSourceFingerprint);
        if (SaveJournal(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        Array<AssetPipelineDiagnostic> diagnostics;
        if (ProjectMigrationSteps::ClassifyAndConvertLegacyAssets(journal.ContentRoot, quarantine, diagnostics) || HasErrors(diagnostics))
        {
            FailFromStep(result, journal, diagnostics, TEXT("M3 found unsupported, lossy, or unreadable legacy assets."));
            break;
        }
        String current;
        if (HashTree(journal.ContentRoot, current, diagnostic) || CommitPhase(journal, operation,
            ProjectMigrationPhase::M3AssetsClassified, current, diagnostic))
            AddFailure(result, MoveTemp(diagnostic));
        else
            result = MakeResult(journal, true, TEXT("M3 completed: legacy assets are converted or quarantined; unresolved types block here."));
        break;
    }
    case ProjectMigrationPhase::M3AssetsClassified:
    {
        if (VerifySourceFingerprint(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        String snapshot;
        if (PreparePhaseSnapshot(journal, ProjectMigrationPhase::M4MetadataAndIdentityConverted, snapshot, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        JournalOperation& operation = BeginOperation(journal, ProjectMigrationPhase::M4MetadataAndIdentityConverted,
            TEXT("CompleteMetadataAndIdentityMap"), journal.ContentRoot, snapshot, journal.CurrentSourceFingerprint);
        if (SaveJournal(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        Array<AssetPipelineDiagnostic> diagnostics;
        if (ProjectMigrationSteps::CompleteAndUpgradeMetadata(journal.ProjectRoot, journal.ContentRoot, diagnostics) || HasErrors(diagnostics))
        {
            FailFromStep(result, journal, diagnostics, TEXT("M4 metadata completion or identity conversion failed."));
            break;
        }
        if (WriteIdentityMap(journal, journal.EvidenceHashes[4], diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        String current;
        if (HashTree(journal.ContentRoot, current, diagnostic) || CommitPhase(journal, operation,
            ProjectMigrationPhase::M4MetadataAndIdentityConverted, current, diagnostic))
            AddFailure(result, MoveTemp(diagnostic));
        else
            result = MakeResult(journal, true, TEXT("M4 completed: every supported source/folder has current metadata and a hashed identity map."));
        break;
    }
    case ProjectMigrationPhase::M4MetadataAndIdentityConverted:
    {
        if (VerifySourceFingerprint(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        String snapshot;
        if (PreparePhaseSnapshot(journal, ProjectMigrationPhase::M5ReferencesRewritten, snapshot, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        JournalOperation& operation = BeginOperation(journal, ProjectMigrationPhase::M5ReferencesRewritten,
            TEXT("RewriteAndVerifySerializedReferences"), journal.ContentRoot, snapshot,
            journal.CurrentSourceFingerprint);
        if (SaveJournal(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        Array<AssetPipelineDiagnostic> diagnostics;
        StringAnsi report;
        const String legacyPreimage = journal.BackupRoot / TEXT("M3AssetsClassified-preimage");
        if (ProjectMigrationSteps::RewriteAndVerifySerializedReferences(journal.ContentRoot, legacyPreimage, report, diagnostics) || HasErrors(diagnostics))
        {
            FailFromStep(result, journal, diagnostics, TEXT("M5 serialized-reference rewrite failed."));
            break;
        }
        String current;
        if (StoreCanonicalReport(journal, ProjectMigrationPhase::M5ReferencesRewritten, String(report),
            TEXT("reference-rewrite"), journal.EvidenceHashes[5], diagnostic) || HashTree(journal.ContentRoot, current, diagnostic) ||
            CommitPhase(journal, operation, ProjectMigrationPhase::M5ReferencesRewritten, current, diagnostic))
            AddFailure(result, MoveTemp(diagnostic));
        else
            result = MakeResult(journal, true, TEXT("M5 completed: serialized references were rewritten, verified, journaled, and hashed."));
        break;
    }
    case ProjectMigrationPhase::M5ReferencesRewritten:
    {
        if (VerifySourceFingerprint(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        String snapshot;
        if (PreparePhaseSnapshot(journal, ProjectMigrationPhase::M6AuthoredSourcesWritten, snapshot, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        JournalOperation& operation = BeginOperation(journal, ProjectMigrationPhase::M6AuthoredSourcesWritten,
            TEXT("CanonicalizeAndVerifyAuthoredSources"), journal.ContentRoot, snapshot,
            journal.CurrentSourceFingerprint);
        if (SaveJournal(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        Array<AssetPipelineDiagnostic> diagnostics;
        StringAnsi report;
        if (ProjectMigrationSteps::CanonicalizeAndVerifyAuthoredSources(journal.ContentRoot, true, report, diagnostics) || HasErrors(diagnostics))
        {
            FailFromStep(result, journal, diagnostics, TEXT("M6 authored-source canonicalization failed."));
            break;
        }
        String current;
        if (StoreCanonicalReport(journal, ProjectMigrationPhase::M6AuthoredSourcesWritten, String(report),
            TEXT("authored-sources"), journal.EvidenceHashes[6], diagnostic) || HashTree(journal.ContentRoot, current, diagnostic) ||
            CommitPhase(journal, operation, ProjectMigrationPhase::M6AuthoredSourcesWritten, current, diagnostic))
            AddFailure(result, MoveTemp(diagnostic));
        else
            result = MakeResult(journal, true, TEXT("M6 completed: authored sources were canonicalized, round-trip verified, journaled, and hashed."));
        break;
    }
    case ProjectMigrationPhase::M6AuthoredSourcesWritten:
    {
        if (VerifySourceFingerprint(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        if (AssetDatabaseFacade::CleanLibrary() || AssetDatabaseFacade::Refresh(
            ImportAssetOptions::ForceUpdate | ImportAssetOptions::ForceSynchronousImport))
        {
            result = MakeResult(journal, false, TEXT("M7 actual clean Library rebuild and synchronous import failed."));
            result.Diagnostics = AssetDatabaseFacade::GetDiagnostics();
            if (result.Diagnostics.IsEmpty())
                result.Diagnostics.Add(MakeDiagnostic(journal.ProjectRoot / TEXT("Library"), result.Message));
            break;
        }
        JournalOperation& operation = BeginOperation(journal, ProjectMigrationPhase::M7CleanDatabaseImported,
            TEXT("CleanLibraryAndSynchronousImport"), journal.ProjectRoot / TEXT("Library"),
            journal.BackupRoot / TEXT("M7CleanDatabaseImported-clean-import.json"), journal.CurrentSourceFingerprint);
        const String report = evidence.ReportJson.HasChars() ? evidence.ReportJson :
            TEXT("{\"cleanLibrary\":true,\"forceSynchronousImport\":true,\"schemaVersion\":1}");
        if (SaveJournal(journal, diagnostic) || StoreCanonicalReport(journal, ProjectMigrationPhase::M7CleanDatabaseImported,
            report, TEXT("clean-import"), journal.EvidenceHashes[7], diagnostic))
            AddFailure(result, MoveTemp(diagnostic));
        else if (CommitPhase(journal, operation, ProjectMigrationPhase::M7CleanDatabaseImported,
            journal.CurrentSourceFingerprint, diagnostic))
            AddFailure(result, MoveTemp(diagnostic));
        else
            result = MakeResult(journal, true, TEXT("M7 completed: clean refresh/import evidence is durable and hashed."));
        break;
    }
    case ProjectMigrationPhase::M7CleanDatabaseImported:
    {
        if (VerifySourceFingerprint(journal, diagnostic) || !evidence.HostCookSucceeded)
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                diagnostic = MakeDiagnostic(journal.JournalPath, TEXT("M8 requires a successful host cook."));
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        Array<AssetPipelineDiagnostic> diagnostics;
        StringAnsi report;
        if (ProjectMigrationSteps::VerifyImportedDatabase(journal.ContentRoot, report, diagnostics) || HasErrors(diagnostics))
        {
            FailFromStep(result, journal, diagnostics, TEXT("M8 imported database and persistent-reference verification failed."));
            break;
        }
        JournalOperation& operation = BeginOperation(journal, ProjectMigrationPhase::M8SemanticallyVerified,
            TEXT("SemanticVerificationAndCandidateMarker"), journal.ProjectDescriptorPath, journal.CandidateMarkerPath,
            journal.CurrentSourceFingerprint);
        if (SaveJournal(journal, diagnostic) || StoreCanonicalReport(journal, ProjectMigrationPhase::M8SemanticallyVerified,
            String(report), TEXT("semantic-verification"), journal.EvidenceHashes[8], diagnostic) ||
            ProjectMigrationSteps::WriteCandidateProjectMarker(journal.ProjectDescriptorPath, journal.CandidateMarkerPath,
                journal.CandidateMarkerFingerprint, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        AssetProjectValidationResult validation = AssetProjectValidator::Validate(journal.CandidateMarkerPath, journal.ContentRoot);
        if (!validation.Valid)
        {
            result = MakeResult(journal, false, TEXT("M8 candidate marker failed full asset-system v3 validation."));
            result.Diagnostics = MoveTemp(validation.Diagnostics);
            break;
        }
        const String candidateBackup = journal.BackupRoot / TEXT("asset-system-v3-candidate.flaxproj");
        if (!FileSystem::FileExists(candidateBackup) && FileSystem::CopyFile(candidateBackup, journal.CandidateMarkerPath))
        {
            AddFailure(result, MakeDiagnostic(candidateBackup, TEXT("Verified candidate marker backup failed.")));
            break;
        }
        String candidateBackupHash;
        if (HashFile(candidateBackup, candidateBackupHash, diagnostic) || candidateBackupHash != journal.CandidateMarkerFingerprint)
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                diagnostic = MakeDiagnostic(candidateBackup, TEXT("Verified candidate marker backup hash does not match."));
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        ComputeVerificationFingerprint(journal);
        operation.PostimageHash = journal.CandidateMarkerFingerprint;
        operation.Committed = true;
        journal.Phase = ProjectMigrationPhase::M8SemanticallyVerified;
        if (SaveJournal(journal, diagnostic))
            AddFailure(result, MoveTemp(diagnostic));
        else
            result = MakeResult(journal, true, TEXT("M8 completed: semantic evidence, host cook, and fully validated candidate marker are hashed."));
        break;
    }
    case ProjectMigrationPhase::M8SemanticallyVerified:
    {
        if (VerifySourceFingerprint(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        String currentDescriptorHash;
        if (HashFile(journal.ProjectDescriptorPath, currentDescriptorHash, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        const bool markerAlreadyCommitted = currentDescriptorHash == journal.CandidateMarkerFingerprint;
        if (!markerAlreadyCommitted)
        {
            String candidateHash;
            if (HashFile(journal.CandidateMarkerPath, candidateHash, diagnostic) || candidateHash != journal.CandidateMarkerFingerprint)
            {
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                    diagnostic = MakeDiagnostic(journal.CandidateMarkerPath, TEXT("M9 candidate marker changed after verification."));
                AddFailure(result, MoveTemp(diagnostic));
                break;
            }
        }
        JournalOperation& operation = BeginOperation(journal, ProjectMigrationPhase::M9Committed,
            TEXT("CommitProjectMarkerLast"), journal.ProjectDescriptorPath, journal.CandidateMarkerPath,
            currentDescriptorHash);
        if (WriteCompletionReport(journal, journal.CompletionReportFingerprint, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        journal.EvidenceHashes[9] = journal.CompletionReportFingerprint;
        if (SaveJournal(journal, diagnostic))
        {
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        if (!markerAlreadyCommitted && FileSystem::MoveFile(journal.ProjectDescriptorPath, journal.CandidateMarkerPath, true))
        {
            AddFailure(result, MakeDiagnostic(journal.ProjectDescriptorPath, TEXT("M9 atomic project marker replacement failed.")));
            break;
        }
        String committedHash;
        if (HashFile(journal.ProjectDescriptorPath, committedHash, diagnostic) || committedHash != journal.CandidateMarkerFingerprint)
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                diagnostic = MakeDiagnostic(journal.ProjectDescriptorPath, TEXT("M9 project marker replacement failed hash verification."));
            AddFailure(result, MoveTemp(diagnostic));
            break;
        }
        operation.PostimageHash = committedHash;
        operation.Committed = true;
        journal.Phase = ProjectMigrationPhase::M9Committed;
        if (SaveJournal(journal, diagnostic))
            AddFailure(result, MoveTemp(diagnostic));
        else
            result = MakeResult(journal, true, TEXT("M9 completed: completion report was hashed and the v3 project marker was committed last."));
        break;
    }
    default:
        AddFailure(result, MakeDiagnostic(journal.JournalPath, TEXT("Migration journal phase is not resumable.")));
        break;
    }

    Delete(lock);
    if (result.Succeeded && journal.Phase == ProjectMigrationPhase::M9Committed && FileSystem::DeleteFile(journal.LockPath))
    {
        result.Succeeded = false;
        result.Message = TEXT("Migration committed, but the exclusive migration lock could not be removed.");
        result.Diagnostics.Add(MakeDiagnostic(journal.LockPath, result.Message));
    }
    return result;
}

ProjectMigrationResult ProjectMigrationOrchestrator::Inspect(const StringView& journalPath)
{
    MigrationJournalState journal;
    AssetPipelineDiagnostic diagnostic;
    const String normalized = NormalizeAbsolute(journalPath);
    if (LoadJournal(normalized, journal, diagnostic))
    {
        ProjectMigrationResult result;
        result.JournalPath = normalized;
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }
    return MakeResult(journal, true, journal.Phase == ProjectMigrationPhase::M9Committed
        ? TEXT("Migration is committed.")
        : TEXT("Migration journal loaded; resume advances exactly one phase."));
}

ProjectMigrationResult ProjectMigrationOrchestrator::Rollback(const StringView& journalPath)
{
    MigrationJournalState journal;
    AssetPipelineDiagnostic diagnostic;
    const String normalized = NormalizeAbsolute(journalPath);
    if (LoadJournal(normalized, journal, diagnostic))
    {
        ProjectMigrationResult result;
        result.JournalPath = normalized;
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }
    ProjectMigrationResult result = MakeResult(journal, false, TEXT("Migration rollback failed."));
    if (journal.Phase == ProjectMigrationPhase::M9Committed)
    {
        AddFailure(result, MakeDiagnostic(journal.ProjectDescriptorPath,
            TEXT("Rollback is forbidden after the v3 project marker has committed."),
            TEXT("Restore through source control or the durable migration backup.")));
        return result;
    }
    if (journal.Phase == ProjectMigrationPhase::RolledBack)
        return MakeResult(journal, true, TEXT("Migration was already rolled back."));
    File* lock = AcquireLock(journal, diagnostic);
    if (!lock)
    {
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }

    String backupFingerprint;
    if (HashTree(journal.ContentBackupPath, backupFingerprint, diagnostic) || backupFingerprint != journal.InitialSourceFingerprint)
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            diagnostic = MakeDiagnostic(journal.ContentBackupPath, TEXT("Rollback backup Content fingerprint is invalid."));
        Delete(lock);
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }
    if (FileSystem::DirectoryExists(journal.ContentRoot))
    {
        String current;
        if (HashTree(journal.ContentRoot, current, diagnostic) ||
            (current != journal.CurrentSourceFingerprint && current != journal.InitialSourceFingerprint))
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                diagnostic = MakeDiagnostic(journal.ContentRoot,
                    TEXT("Rollback refuses to overwrite source edits made after the last durable migration phase."));
            Delete(lock);
            AddFailure(result, MoveTemp(diagnostic));
            return result;
        }
    }

    const String staging = journal.ProjectRoot / (TEXT("Content.migration-restore-") + journal.TransactionId.ToString(Guid::FormatType::N));
    if (FileSystem::DirectoryExists(staging))
    {
        String stagingFingerprint;
        if (HashTree(staging, stagingFingerprint, diagnostic) || stagingFingerprint != journal.InitialSourceFingerprint)
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                diagnostic = MakeDiagnostic(staging, TEXT("Rollback staging tree does not match the durable backup."));
            Delete(lock);
            AddFailure(result, MoveTemp(diagnostic));
            return result;
        }
    }
    else if (FileSystem::CopyDirectory(staging, journal.ContentBackupPath, true))
    {
        Delete(lock);
        AddFailure(result, MakeDiagnostic(staging, TEXT("Rollback could not stage the verified Content backup.")));
        return result;
    }

    String descriptorPreimage;
    if (HashFile(journal.ProjectDescriptorPath, descriptorPreimage, diagnostic))
    {
        Delete(lock);
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }
    JournalOperation& operation = BeginOperation(journal, ProjectMigrationPhase::RolledBack,
        TEXT("RestoreVerifiedM0Backup"), journal.ContentRoot, staging, descriptorPreimage);
    if (SaveJournal(journal, diagnostic))
    {
        Delete(lock);
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }

    if (FileSystem::DirectoryExists(journal.ContentRoot) && FileSystem::DeleteDirectory(journal.ContentRoot, true))
    {
        Delete(lock);
        AddFailure(result, MakeDiagnostic(journal.ContentRoot, TEXT("Rollback could not remove the uncommitted migrated Content tree.")));
        return result;
    }
    if (FileSystem::CopyDirectory(journal.ContentRoot, staging, true))
    {
        Delete(lock);
        AddFailure(result, MakeDiagnostic(journal.ContentRoot, TEXT("Rollback could not restore Content from verified staging.")));
        return result;
    }
    String restoredFingerprint;
    if (HashTree(journal.ContentRoot, restoredFingerprint, diagnostic) || restoredFingerprint != journal.InitialSourceFingerprint)
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            diagnostic = MakeDiagnostic(journal.ContentRoot, TEXT("Rollback restored Content failed fingerprint verification."));
        Delete(lock);
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }

    StringAnsi descriptorBackup;
    if (File::ReadAllText(journal.DescriptorBackupPath, descriptorBackup) ||
        WriteAtomic(journal.ProjectDescriptorPath, descriptorBackup, diagnostic))
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            diagnostic = MakeDiagnostic(journal.DescriptorBackupPath, TEXT("Rollback could not restore the project descriptor backup."));
        Delete(lock);
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }
    String restoredDescriptorHash;
    String backupDescriptorHash;
    if (HashFile(journal.ProjectDescriptorPath, restoredDescriptorHash, diagnostic) ||
        HashFile(journal.DescriptorBackupPath, backupDescriptorHash, diagnostic) || restoredDescriptorHash != backupDescriptorHash)
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            diagnostic = MakeDiagnostic(journal.ProjectDescriptorPath, TEXT("Rollback project descriptor failed hash verification."));
        Delete(lock);
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }

    operation.PostimageHash = restoredFingerprint;
    operation.Committed = true;
    journal.CurrentSourceFingerprint = restoredFingerprint;
    journal.Phase = ProjectMigrationPhase::RolledBack;
    if (SaveJournal(journal, diagnostic))
    {
        Delete(lock);
        AddFailure(result, MoveTemp(diagnostic));
        return result;
    }
    if (FileSystem::FileExists(journal.CandidateMarkerPath))
        FileSystem::DeleteFile(journal.CandidateMarkerPath);
    FileSystem::DeleteDirectory(staging, true);
    Delete(lock);
    if (FileSystem::DeleteFile(journal.LockPath))
    {
        AddFailure(result, MakeDiagnostic(journal.LockPath, TEXT("Rollback completed, but the migration lock could not be removed.")));
        return result;
    }
    result = MakeResult(journal, true, TEXT("Migration rolled back to the verified M0 descriptor and Content backup."));
    result.Completed = false;
    return result;
}
