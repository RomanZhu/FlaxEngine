// Copyright (c) Wojciech Figat. All rights reserved.

#include "CookedContentGeneration.h"
#include "RuntimeAssetCatalog.h"
#include "Engine/Content/AssetDatabase/DurableAssetFileSystem.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include <algorithm>

namespace
{
    const Char* GenerationsDirectory = TEXT("Generations");
    const Char* CurrentGenerationFile = TEXT("CurrentGeneration");
    const Char* CompletionFile = TEXT(".complete");
    const Char* RuntimeCatalogFile = TEXT("RuntimeAssetCatalog.bin");

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Cook;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    bool IsCancelled(const Function<bool()>& callback)
    {
        return callback.IsBinded() && callback();
    }

    void HashUInt64(ContentHasher& hasher, uint64 value)
    {
        byte bytes[8];
        for (int32 i = 0; i < ARRAY_COUNT(bytes); i++)
            bytes[i] = static_cast<byte>(value >> (i * 8));
        hasher.Update(bytes, ARRAY_COUNT(bytes));
    }

    bool HashStagedData(const String& stagingDataRoot, ContentHash& result, AssetPipelineDiagnostic& diagnostic,
        const Function<bool()>& isCancellationRequested)
    {
        Array<String> files;
        if (FileSystem::DirectoryGetFiles(files, stagingDataRoot, TEXT("*"), DirectorySearchOption::AllDirectories))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, stagingDataRoot, TEXT("Cannot enumerate the staged cooked-content generation."));
        if (files.Count() > 1)
            std::sort(files.Get(), files.Get() + files.Count());

        ContentHasher hasher;
        static const char Domain[] = "flax-cooked-content-generation-v1";
        hasher.Update(Domain, ARRAY_COUNT(Domain) - 1);
        const int32 relativeStart = stagingDataRoot.Length() + 1;
        Array<byte> bytes;
        bytes.Resize(1024 * 1024);
        for (String& file : files)
        {
            if (IsCancelled(isCancellationRequested))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, stagingDataRoot, TEXT("Cooked-content generation publication was cancelled while hashing output."));
            if (file.Length() <= relativeStart)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, file, TEXT("A staged output path is outside the cooked-content generation root."));
            String relative = file.Substring(relativeStart);
            relative.Replace('\\', '/');
            const StringAnsi portable(relative);
            HashUInt64(hasher, portable.Length());
            hasher.Update(portable.Get(), portable.Length());
            const uint64 fileSize = FileSystem::GetFileSize(file);
            if (fileSize > MAX_uint32)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, file, TEXT("A staged cooked-content output exceeds the supported single-file size."));
            HashUInt64(hasher, fileSize);
            File* stream = File::Open(file, FileMode::OpenExisting, FileAccess::Read, FileShare::All);
            if (stream == nullptr)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, file, TEXT("Cannot open a staged cooked-content output."));
            SCOPE_EXIT { Delete(stream); };
            uint32 remaining = static_cast<uint32>(fileSize);
            while (remaining != 0)
            {
                if (IsCancelled(isCancellationRequested))
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, file, TEXT("Cooked-content generation publication was cancelled while hashing output bytes."));
                const uint32 bufferSize = static_cast<uint32>(bytes.Count());
                const uint32 requested = remaining < bufferSize ? remaining : bufferSize;
                uint32 read = 0;
                if (stream->Read(bytes.Get(), requested, &read) || read != requested)
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, file, TEXT("Cannot read a staged cooked-content output."));
                hasher.Update(bytes.Get(), read);
                remaining -= read;
            }
        }
        result = hasher.Finalize();
        return result.IsZero()
            ? Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, stagingDataRoot, TEXT("Cooked-content generation hash is invalid."))
            : false;
    }

    bool ValidateGeneration(const String& dataRoot, const ContentHash& expectedGeneration, AssetPipelineDiagnostic& diagnostic)
    {
        // Full byte hashing happens before publication. Startup intentionally performs bounded structural checks;
        // catalog and package readers retain their normal format validation when bytes are consumed.
        const String markerPath = dataRoot / CompletionFile;
        StringAnsi marker;
        ContentHash markerGeneration;
        if (File::ReadAllText(markerPath, marker) || ContentHash::Parse(StringAnsiView(marker), markerGeneration) || markerGeneration != expectedGeneration)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, markerPath, TEXT("Cooked-content generation completion marker is missing or invalid."));

        const String contentPath = dataRoot / TEXT("Content");
        const String headPath = contentPath / TEXT("head");
        if (!FileSystem::FileExists(headPath) || FileSystem::GetFileSize(headPath) == 0)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, headPath, TEXT("Cooked-content generation has no product header."));
        const String catalogPath = contentPath / RuntimeCatalogFile;
        RuntimeAssetCatalog catalog;
        if (RuntimeAssetCatalog::Load(catalogPath, catalog, diagnostic))
        {
            diagnostic.Stage = AssetPipelineDiagnosticStage::Cook;
            return true;
        }
        for (const RuntimeAssetCatalogEntry& entry : catalog.GetEntries())
        {
            const String packagePath = contentPath / String(entry.PackageName);
            if (!FileSystem::FileExists(packagePath) || FileSystem::GetFileSize(packagePath) == 0)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, packagePath, TEXT("Cooked runtime catalog references a missing package."));
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool FlushGeneration(const String& dataRoot, AssetPipelineDiagnostic& diagnostic, const Function<bool()>& isCancellationRequested)
    {
        Array<String> files;
        if (FileSystem::DirectoryGetFiles(files, dataRoot, TEXT("*"), DirectorySearchOption::AllDirectories))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, dataRoot, TEXT("Cannot enumerate the staged cooked-content generation for durability."));
        for (const String& file : files)
        {
            if (IsCancelled(isCancellationRequested))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, dataRoot, TEXT("Cooked-content generation publication was cancelled before durable commit."));
            if (DurableAssetFileSystem::FlushFile(file))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, file, TEXT("Cannot flush a staged cooked-content output."));
        }
        Array<String> pending;
        Array<String> directories;
        pending.Add(dataRoot);
        while (pending.HasItems())
        {
            const String directory = pending.Last();
            pending.RemoveLast();
            directories.Add(directory);
            Array<String> children;
            if (FileSystem::GetChildDirectories(children, directory))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, directory, TEXT("Cannot enumerate staged cooked-content directories for durability."));
            pending.Add(children);
        }
        for (int32 i = directories.Count() - 1; i >= 0; i--)
        {
            if (DurableAssetFileSystem::FlushDirectory(directories[i]))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, directories[i], TEXT("Cannot flush a staged cooked-content directory."));
        }
        return false;
    }

    bool ReplaceGenerationPointer(const StringView& contentRoot, const ContentHash& generation, AssetPipelineDiagnostic& diagnostic)
    {
        const String pointerPath = CookedContentGeneration::GetCurrentGenerationPath(contentRoot);
        const String pointerStaging = pointerPath + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
        bool deletePointerStaging = true;
        SCOPE_EXIT
        {
            if (deletePointerStaging && FileSystem::FileExists(pointerStaging))
                DurableAssetFileSystem::DeleteFile(pointerStaging);
        };
        const StringAnsi generationText = generation.ToString();
        if (DurableAssetFileSystem::WriteFile(pointerStaging, generationText.Get(), generationText.Length()) ||
            DurableAssetFileSystem::Replace(pointerPath, pointerStaging))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, pointerPath, TEXT("Cannot atomically activate the cooked-content generation."));
        deletePointerStaging = false;
        return false;
    }

    bool RemoveRollbackRoot(CookedContentDeploymentState& state, AssetPipelineDiagnostic& diagnostic)
    {
        if (state.RollbackRoot.HasChars() && FileSystem::DirectoryExists(state.RollbackRoot) &&
            DurableAssetFileSystem::DeleteDirectory(state.RollbackRoot, true))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, state.RollbackRoot, TEXT("Cannot remove cooked-content rollback data."));
        state.RollbackRoot.Clear();
        state.PreviousGenerationMoved = false;
        return false;
    }
}

String CookedContentGeneration::GetGenerationsPath(const StringView& contentRoot)
{
    return String(contentRoot) / GenerationsDirectory;
}

String CookedContentGeneration::GetCurrentGenerationPath(const StringView& contentRoot)
{
    return String(contentRoot) / CurrentGenerationFile;
}

bool CookedContentGeneration::CreateStaging(const StringView& contentRoot, const Guid& jobID, String& stagingDataRoot,
    AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    const String generationsPath = GetGenerationsPath(contentRoot);
    stagingDataRoot = generationsPath / (TEXT(".staging-") + jobID.ToString(Guid::FormatType::N));
    if (FileSystem::DirectoryExists(stagingDataRoot) || DurableAssetFileSystem::EnsureDirectory(generationsPath) ||
        DurableAssetFileSystem::EnsureDirectory(stagingDataRoot) ||
        DurableAssetFileSystem::EnsureDirectory(stagingDataRoot / TEXT("Content")))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, stagingDataRoot, TEXT("Cannot create an isolated cooked-content staging directory."));
    return false;
}

bool CookedContentGeneration::ShouldCopyStreamingFile(bool destinationExists, const DateTime& sourceModified, const DateTime& destinationModified)
{
    return !destinationExists || destinationModified < sourceModified;
}

bool CookedContentGeneration::Publish(const StringView& contentRoot, const StringView& stagingDataRoot, ContentHash& generation,
    AssetPipelineDiagnostic& diagnostic, const Function<bool()>& isCancellationRequested,
    CookedContentPublicationFailurePoint failurePoint)
{
    generation = ContentHash();
    diagnostic = AssetPipelineDiagnostic();
    const String staging(stagingDataRoot);
    const String generationsPath = GetGenerationsPath(contentRoot);
    if (!FileSystem::DirectoryExists(staging) ||
        !FileSystem::AreFilePathsEquivalent(StringUtils::GetDirectoryName(staging), generationsPath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, staging, TEXT("Cooked-content staging must be a direct child of the generation store."));
    bool stagingExists = true;
    SCOPE_EXIT
    {
        if (stagingExists && FileSystem::DirectoryExists(staging))
            DurableAssetFileSystem::DeleteDirectory(staging, true);
    };

    if (IsCancelled(isCancellationRequested))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, staging, TEXT("Cooked-content generation publication was cancelled."));
    if (HashStagedData(staging, generation, diagnostic, isCancellationRequested))
        return true;

    const StringAnsi generationText = generation.ToString();
    const String markerPath = staging / CompletionFile;
    if (DurableAssetFileSystem::WriteFile(markerPath, generationText.Get(), generationText.Length()) ||
        ValidateGeneration(staging, generation, diagnostic) || FlushGeneration(staging, diagnostic, isCancellationRequested))
        return true;
    if (failurePoint == CookedContentPublicationFailurePoint::BeforeGenerationMove)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, staging, TEXT("Injected failure before cooked-content generation commit."));
    if (IsCancelled(isCancellationRequested))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, staging, TEXT("Cooked-content generation publication was cancelled before generation commit."));

    const String finalDataRoot = generationsPath / String(generationText);
    if (FileSystem::DirectoryExists(finalDataRoot))
    {
        if (ValidateGeneration(finalDataRoot, generation, diagnostic))
            return true;
    }
    else
    {
        if (DurableAssetFileSystem::Move(finalDataRoot, staging, false))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, finalDataRoot, TEXT("Cannot commit the immutable cooked-content generation."));
        stagingExists = false;
        if (ValidateGeneration(finalDataRoot, generation, diagnostic))
            return true;
    }
    if (failurePoint == CookedContentPublicationFailurePoint::AfterGenerationMove)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, finalDataRoot, TEXT("Injected failure after cooked-content generation commit."));
    if (IsCancelled(isCancellationRequested))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, finalDataRoot, TEXT("Cooked-content generation publication was cancelled before activation."));
    if (failurePoint == CookedContentPublicationFailurePoint::BeforePointerReplace)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, finalDataRoot, TEXT("Injected failure before cooked-content generation activation."));

    if (ReplaceGenerationPointer(contentRoot, generation, diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool CookedContentGeneration::BeginDeployment(const StringView& contentRoot, const StringView& stagingDataRoot,
    const StringView& rollbackRoot, CookedContentDeploymentState& state, AssetPipelineDiagnostic& diagnostic,
    const Function<bool()>& isCancellationRequested)
{
    state = CookedContentDeploymentState();
    diagnostic = AssetPipelineDiagnostic();
    String content(contentRoot);
    String rollback(rollbackRoot);
    FileSystem::NormalizePath(content);
    FileSystem::NormalizePath(rollback);
    if (rollback.IsEmpty() || AssetPathPolicy::IsSameOrChild(rollback, content) || AssetPathPolicy::IsSameOrChild(content, rollback))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, rollback, TEXT("Cooked-content rollback data must be outside the packaged content tree."));
    if (FileSystem::DirectoryExists(rollback) || FileSystem::FileExists(rollback))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, rollback, TEXT("Cooked-content rollback path is already in use."));

    const String pointerPath = GetCurrentGenerationPath(content);
    if (FileSystem::FileExists(pointerPath))
    {
        String previousContent;
        if (Resolve(content, previousContent, state.PreviousGeneration, diagnostic))
            return true;
        state.HadPreviousGeneration = true;
    }

    if (Publish(content, stagingDataRoot, state.NewGeneration, diagnostic, isCancellationRequested))
        return true;
    state.RollbackRoot = rollback;
    state.ActivationChanged = !state.HadPreviousGeneration || state.PreviousGeneration != state.NewGeneration;

    const auto rollbackFailure = [&]()
    {
        const AssetPipelineDiagnostic original = diagnostic;
        AssetPipelineDiagnostic rollbackDiagnostic;
        if (RollbackDeployment(content, state, rollbackDiagnostic))
        {
            diagnostic = original;
            diagnostic.Message = String::Format(TEXT("{0} Rollback also failed: {1}"), original.Message, rollbackDiagnostic.Message);
        }
        else
        {
            diagnostic = original;
        }
        return true;
    };

    if (IsCancelled(isCancellationRequested))
    {
        Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, content, TEXT("Cooked-content deployment was cancelled before platform packaging."));
        return rollbackFailure();
    }

    const String generationsPath = GetGenerationsPath(content);
    const String activeDataRoot = generationsPath / String(state.NewGeneration.ToString());
    if (state.ActivationChanged && state.HadPreviousGeneration)
    {
        if (DurableAssetFileSystem::EnsureDirectory(rollback))
        {
            Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, rollback, TEXT("Cannot create cooked-content rollback storage."));
            return rollbackFailure();
        }
        const String previousDataRoot = generationsPath / String(state.PreviousGeneration.ToString());
        const String rollbackDataRoot = rollback / String(state.PreviousGeneration.ToString());
        const bool moveFailed = DurableAssetFileSystem::Move(rollbackDataRoot, previousDataRoot, false);
        state.PreviousGenerationMoved = !FileSystem::DirectoryExists(previousDataRoot) && FileSystem::DirectoryExists(rollbackDataRoot);
        if (!state.PreviousGenerationMoved)
        {
            Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, previousDataRoot, moveFailed
                ? TEXT("Cannot move the previous cooked-content generation outside the package view.")
                : TEXT("The previous cooked-content generation move did not reach its expected endpoint."));
            return rollbackFailure();
        }
    }

    Array<String> children;
    if (FileSystem::GetChildDirectories(children, generationsPath))
    {
        Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, generationsPath, TEXT("Cannot enumerate cooked-content generations before platform packaging."));
        return rollbackFailure();
    }
    for (const String& child : children)
    {
        if (FileSystem::AreFilePathsEquivalent(child, activeDataRoot))
            continue;
        if (IsCancelled(isCancellationRequested))
        {
            Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, generationsPath, TEXT("Cooked-content deployment was cancelled while preparing the package view."));
            return rollbackFailure();
        }
        if (DurableAssetFileSystem::DeleteDirectory(child, true))
        {
            Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, child, TEXT("Cannot remove an inactive cooked-content generation from the package view."));
            return rollbackFailure();
        }
    }

    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool CookedContentGeneration::CommitDeployment(CookedContentDeploymentState& state, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    if (RemoveRollbackRoot(state, diagnostic))
        return true;
    state.ActivationChanged = false;
    return false;
}

bool CookedContentGeneration::RollbackDeployment(const StringView& contentRoot, CookedContentDeploymentState& state,
    AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    if (!state.ActivationChanged)
        return RemoveRollbackRoot(state, diagnostic);

    const String content(contentRoot);
    const String generationsPath = GetGenerationsPath(content);
    const String newDataRoot = generationsPath / String(state.NewGeneration.ToString());
    if (state.HadPreviousGeneration)
    {
        const String previousDataRoot = generationsPath / String(state.PreviousGeneration.ToString());
        if (state.PreviousGenerationMoved)
        {
            const String rollbackDataRoot = state.RollbackRoot / String(state.PreviousGeneration.ToString());
            const bool moveFailed = DurableAssetFileSystem::Move(previousDataRoot, rollbackDataRoot, false);
            const bool movedBack = FileSystem::DirectoryExists(previousDataRoot) && !FileSystem::DirectoryExists(rollbackDataRoot);
            state.PreviousGenerationMoved = !movedBack;
            if (!movedBack)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, previousDataRoot, moveFailed
                    ? TEXT("Cannot restore the previous cooked-content generation.")
                    : TEXT("The previous cooked-content generation restore did not reach its expected endpoint."));
        }
        if (ValidateGeneration(previousDataRoot, state.PreviousGeneration, diagnostic))
            return true;
        if (ReplaceGenerationPointer(content, state.PreviousGeneration, diagnostic))
            return true;
    }
    else
    {
        const String pointerPath = GetCurrentGenerationPath(content);
        if (FileSystem::FileExists(pointerPath) && DurableAssetFileSystem::DeleteFile(pointerPath))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, pointerPath, TEXT("Cannot remove the first cooked-content generation activation."));
    }

    state.ActivationChanged = false;
    if (FileSystem::DirectoryExists(newDataRoot) && DurableAssetFileSystem::DeleteDirectory(newDataRoot, true))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, newDataRoot, TEXT("Cannot remove the failed cooked-content generation."));
    if (RemoveRollbackRoot(state, diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool CookedContentGeneration::Resolve(const StringView& contentRoot, String& activeContentPath, ContentHash& generation,
    AssetPipelineDiagnostic& diagnostic)
{
    activeContentPath.Clear();
    generation = ContentHash();
    const String pointerPath = GetCurrentGenerationPath(contentRoot);
    StringAnsi pointer;
    if (File::ReadAllText(pointerPath, pointer) || ContentHash::Parse(StringAnsiView(pointer), generation))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, pointerPath, TEXT("Cooked-content generation pointer is missing or invalid."));
    const String dataRoot = GetGenerationsPath(contentRoot) / String(pointer);
    if (ValidateGeneration(dataRoot, generation, diagnostic))
        return true;
    activeContentPath = dataRoot / TEXT("Content");
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
