// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Importing/AssetImportContext.h"
#include "Engine/Content/Importing/AssetImportService.h"
#include "Engine/Content/Importing/AssetImportWorkerProtocol.h"
#include "Engine/Content/Importing/CallbackImporterPipelineService.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Scripting/Internal/InternalCalls.h"
#include "Engine/Scripting/ManagedCLR/MCore.h"
#include "Engine/Scripting/ManagedCLR/MUtils.h"
#include <algorithm>

#if USE_CSHARP

namespace
{
    using ScriptedImporterInvoke = int32 (*)(const Char*, int32);

    CriticalSection BridgeLocker;
    Array<AssetImporterDescriptor> PendingDescriptors;
    ScriptedImporterInvoke PendingInvoke = nullptr;
    bool RegistrationOpen = false;
    String LastError;
    thread_local AssetImportContext* CurrentContext = nullptr;
    thread_local const AssetImportJobRequest* CurrentWorkerRequest = nullptr;

    void SetError(const StringView& message)
    {
        ScopeLock lock(BridgeLocker);
        LastError = message;
    }

    void SplitExtensions(const StringView& text, Array<String>& result)
    {
        result.Clear();
        int32 start = 0;
        for (int32 i = 0; i <= text.Length(); i++)
        {
            if (i != text.Length() && text[i] != TEXT(';'))
                continue;
            if (i > start)
            {
                String extension(text.Get() + start, i - start);
                if (extension.IsEmpty() || extension[0] != TEXT('.'))
                    extension = TEXT(".") + extension;
                result.Add(MoveTemp(extension));
            }
            start = i + 1;
        }
    }

    bool RequireContext(AssetPipelineDiagnostic* diagnostic = nullptr)
    {
        if (CurrentContext)
            return true;
        if (diagnostic)
        {
            diagnostic->Code = AssetPipelineDiagnosticCode::BuildFailed;
            diagnostic->Stage = AssetPipelineDiagnosticStage::Build;
            diagnostic->Message = TEXT("Scripted importer context is only valid during OnImportAsset.");
        }
        SetError(TEXT("Scripted importer context is only valid during OnImportAsset."));
        return false;
    }

    ContentHash ParseOptionalHash(MString* valueObject)
    {
        String value;
        MUtils::ToString(valueObject, value);
        ContentHash result;
        if (value.HasChars())
            ContentHash::Parse(value, result);
        return result;
    }

    bool WorkerFailure(AssetPipelineDiagnostic& diagnostic, const AssetImportJobRequest& request,
                       AssetPipelineDiagnosticCode code, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = request.Asset.Value;
        diagnostic.SourcePath = request.SourcePath;
        diagnostic.ProcessorId = request.Importer.ID;
        diagnostic.Target = String(request.Target.BuildKey(ArtifactTargetDimension::All).ToString());
        diagnostic.Message = message;
        return true;
    }

    bool IsSafeOutputName(const StringView& name, const StringAnsiView& extension)
    {
        if (!SubAssetPolicy::IsKeyValid(name) || extension.IsEmpty() || extension.Length() > 32 || extension[0] != '.')
            return false;
        for (int32 i = 1; i < extension.Length(); i++)
        {
            const char c = extension[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return false;
        }
        return true;
    }

    bool RunManagedWorker(const AssetImportJobRequest& request, AssetImportJobResult& result, AssetPipelineDiagnostic& diagnostic)
    {
        if (request.Importer.ProviderKind != AssetProcessorProviderKind::Managed)
            return WorkerFailure(diagnostic, request, AssetPipelineDiagnosticCode::ProcessorMissing,
                TEXT("Editor worker received a non-managed importer request."));
        AssetImporterRegistry* registry = AssetImportService::GetImporterRegistry();
        if (!registry)
            return WorkerFailure(diagnostic, request, AssetPipelineDiagnosticCode::ProcessorMissing,
                TEXT("Managed importer registry is unavailable in the worker."));
        AssetImporterLease lease;
        if (registry->TryAcquire(request.Importer.ID, lease, diagnostic))
            return true;
        const AssetImporterDescriptor& descriptor = lease.Get();
        if (descriptor.ProviderKind != AssetProcessorProviderKind::Managed ||
            descriptor.ImporterVersion != request.Importer.ImporterVersion ||
            descriptor.SettingsSchemaVersion != request.Importer.SettingsSchemaVersion ||
            descriptor.ImplementationHash != request.Importer.ImplementationHash)
            return WorkerFailure(diagnostic, request, AssetPipelineDiagnosticCode::PrepareInvalidated,
                TEXT("Managed importer implementation changed after the parent prepared this request."));

        AssetMeta meta;
        const StringAnsi metaJson(reinterpret_cast<const char*>(request.MetaSnapshot.Get()), request.MetaSnapshot.Count());
        if (AssetMeta::Parse(metaJson, request.SourcePath + TEXT(".meta"), meta, diagnostic))
            return true;
        AssetImportReadCallback read = [&request](const StringView& requestedPath, Array<byte>& data,
                                                  ContentHash& hash, AssetPipelineDiagnostic& readDiagnostic)
        {
            String path(requestedPath);
            if (path.IsEmpty())
                path = request.SourcePath;
            if (FileSystem::IsRelative(path))
                path = Globals::ProjectFolder / path;
            FileSystem::NormalizePath(path);
            String sourcePath(request.SourcePath);
            FileSystem::NormalizePath(sourcePath);
            if (path.Compare(sourcePath, StringSearchCase::IgnoreCase) == 0)
            {
                data = request.SourceSnapshot;
                hash = request.SourceHash;
                readDiagnostic = AssetPipelineDiagnostic();
                return false;
            }
            for (const AssetImportWorkerInput& input : request.AuthorizedInputs)
            {
                if (path.Compare(input.CanonicalPath, StringSearchCase::IgnoreCase) == 0)
                {
                    data = input.Snapshot;
                    hash = input.Hash;
                    readDiagnostic = AssetPipelineDiagnostic();
                    return false;
                }
            }
            return WorkerFailure(readDiagnostic, request, AssetPipelineDiagnosticCode::UndeclaredInput,
                TEXT("Scripted importer attempted to read a file outside its authorized input snapshots."));
        };

        AssetImportContext context(request.Asset, request.SourcePath, request.Target, meta.Processor.SettingsJson, MoveTemp(read));
        CurrentWorkerRequest = &request;
        const bool importFailed = descriptor.Import(context, diagnostic);
        CurrentWorkerRequest = nullptr;
        if (importFailed)
            return true;
        AssetImportContextResult contextResult;
        if (context.Complete(descriptor.ProducesMainObject, contextResult, diagnostic))
            return true;
        for (const AssetPipelineDiagnostic& item : contextResult.Diagnostics)
        {
            result.Diagnostics.Add(item);
            if (item.Severity == AssetPipelineDiagnosticSeverity::Error)
            {
                diagnostic = item;
                return true;
            }
        }
        result.Objects = MoveTemp(contextResult.Objects);
        result.Dependencies = MoveTemp(contextResult.Dependencies);
        for (const AssetImportOutputDeclaration& output : contextResult.Outputs)
        {
            if (!IsSafeOutputName(output.Name, output.Extension) || output.Data.IsEmpty())
                return WorkerFailure(diagnostic, request, AssetPipelineDiagnosticCode::ArtifactInvalid,
                    TEXT("Scripted importer produced an invalid or empty output declaration."));
            AssetImportWorkerOutput workerOutput;
            workerOutput.Name = output.Name;
            workerOutput.Kind = output.Kind;
            workerOutput.RelativePath = output.Name + String(output.Extension);
            workerOutput.Size = output.Data.Count();
            workerOutput.Hash = ContentHash::Compute(output.Data.Get(), output.Data.Count());
            workerOutput.TargetDimensions = output.TargetDimensions;
            const String outputPath = request.OutputStagingPath / workerOutput.RelativePath;
            const String outputDirectory = StringUtils::GetDirectoryName(outputPath);
            if ((!FileSystem::DirectoryExists(outputDirectory) && FileSystem::CreateDirectory(outputDirectory)) ||
                File::WriteAllBytes(outputPath, output.Data.Get(), output.Data.Count()))
                return WorkerFailure(diagnostic, request, AssetPipelineDiagnosticCode::BuildFailed,
                    TEXT("Scripted importer worker could not write a declared output."));
            result.Outputs.Add(MoveTemp(workerOutput));
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
}

DEFINE_INTERNAL_CALL(bool) ScriptedImporterInternal_BeginRegistration(void* invoke)
{
    ScopeLock lock(BridgeLocker);
    PendingDescriptors.Clear();
    PendingInvoke = reinterpret_cast<ScriptedImporterInvoke>(invoke);
    RegistrationOpen = PendingInvoke != nullptr;
    LastError = RegistrationOpen ? String::Empty : String(TEXT("Scripted importer callback is missing."));
    return !RegistrationOpen;
}

DEFINE_INTERNAL_CALL(bool) ScriptedImporterInternal_AddRegistration(MString* idObject, int32 importerVersion,
    int32 settingsVersion, MString* implementationHashObject, MString* extensionsObject, int32 priority, int32 flags)
{
    String id, implementationHashText, extensions;
    MUtils::ToString(idObject, id);
    MUtils::ToString(implementationHashObject, implementationHashText);
    MUtils::ToString(extensionsObject, extensions);
    ContentHash implementationHash;
    if (!RegistrationOpen || ContentHash::Parse(implementationHashText, implementationHash))
    {
        SetError(TEXT("Scripted importer registration identity or implementation hash is invalid."));
        return true;
    }

    AssetImporterDescriptor descriptor;
    descriptor.ID = id;
    descriptor.ProviderID = TEXT("managed-scripted-importers");
    descriptor.ImporterVersion = static_cast<uint32>(importerVersion);
    descriptor.SettingsSchemaVersion = static_cast<uint32>(settingsVersion);
    descriptor.ImplementationHash = implementationHash;
    descriptor.ProviderKind = AssetProcessorProviderKind::Managed;
    SplitExtensions(extensions, descriptor.Extensions);
    descriptor.Priority = priority;
    descriptor.SupportsOverride = (flags & 1) != 0;
    descriptor.ProducesMainObject = (flags & 2) != 0;
    descriptor.ProducesSubObjects = (flags & 4) != 0;
    descriptor.SupportsParallelImport = (flags & 8) != 0;
    descriptor.RequiresMainThread = (flags & 16) != 0;
    descriptor.PathSensitive = (flags & 32) != 0;
    descriptor.ProcessSafe = true;
    const ScriptedImporterInvoke callback = PendingInvoke;
    descriptor.Import = [id, callback](AssetImportContext& context, AssetPipelineDiagnostic& diagnostic)
    {
        if (!callback || !CurrentWorkerRequest)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
            diagnostic.ProcessorId = id;
            diagnostic.Message = callback
                ? TEXT("Scripted importers may execute only in an isolated worker process.")
                : TEXT("Scripted importer callback was unloaded.");
            return true;
        }
        AssetImportContext* previous = CurrentContext;
        CurrentContext = &context;
        const int32 failed = callback(id.Get(), id.Length());
        CurrentContext = previous;
        if (failed != 0)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
            diagnostic.AssetGuid = context.GetAsset().Value;
            diagnostic.SourcePath = context.GetSourcePath();
            diagnostic.ProcessorId = id;
            diagnostic.Message = TEXT("Scripted importer OnImportAsset failed. See importer diagnostics for details.");
            return true;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    };
    descriptor.RequestBuild = [](const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic)
    {
        return CallbackImporterPipelineService::RequestBuild(assetID, force, diagnostic);
    };
    descriptor.GetBuildStatus = [](const Guid& assetID, AssetPipelineDiagnostic& diagnostic)
    {
        return CallbackImporterPipelineService::GetStatus(assetID, diagnostic);
    };
    ScopeLock lock(BridgeLocker);
    if (!RegistrationOpen)
    {
        LastError = TEXT("Scripted importer registration is not open.");
        return true;
    }
    PendingDescriptors.Add(MoveTemp(descriptor));
    return false;
}

DEFINE_INTERNAL_CALL(bool) ScriptedImporterInternal_CommitRegistration()
{
    Array<AssetImporterDescriptor> descriptors;
    {
        ScopeLock lock(BridgeLocker);
        if (!RegistrationOpen)
        {
            LastError = TEXT("Scripted importer registration is not open.");
            return true;
        }
        descriptors = MoveTemp(PendingDescriptors);
        RegistrationOpen = false;
        PendingInvoke = nullptr;
    }
    AssetPipelineDiagnostic diagnostic;
    if (AssetImportService::EnsureInitialized(diagnostic))
    {
        SetError(diagnostic.Message);
        return true;
    }
    Array<String> changed;
    if (AssetImportService::GetImporterRegistry()->ReplaceProviderSet(TEXT("managed-scripted-importers"), MoveTemp(descriptors), changed, diagnostic))
    {
        SetError(diagnostic.Message);
        return true;
    }
    if (changed.HasItems())
    {
        if (AssetRefreshCoordinator* refresh = AssetImportService::GetRefreshCoordinator())
            refresh->RequestRefresh(AssetRefreshReason::ImporterRegistry);
    }
    SetError(StringView::Empty);
    return false;
}

DEFINE_INTERNAL_CALL(void) ScriptedImporterInternal_AbortRegistration()
{
    ScopeLock lock(BridgeLocker);
    PendingDescriptors.Clear();
    PendingInvoke = nullptr;
    RegistrationOpen = false;
}

DEFINE_INTERNAL_CALL(MString*) ScriptedImporterInternal_GetLastError()
{
    ScopeLock lock(BridgeLocker);
    return MUtils::ToString(LastError);
}

DEFINE_INTERNAL_CALL(int32) ScriptedImporterInternal_RunWorker()
{
    String requestPath, resultPath, capability;
    if (Platform::GetEnvironmentVariable(TEXT("FLAX_ASSET_IMPORT_REQUEST"), requestPath) || requestPath.IsEmpty() ||
        Platform::GetEnvironmentVariable(TEXT("FLAX_ASSET_IMPORT_RESULT"), resultPath) || resultPath.IsEmpty() ||
        Platform::GetEnvironmentVariable(TEXT("FLAX_ASSET_IMPORT_CAPABILITY"), capability) || capability.IsEmpty())
    {
        SetError(TEXT("Isolated importer worker environment is incomplete."));
        return 6;
    }
    return AssetImportWorkerHost::Run(requestPath, resultPath, capability, RunManagedWorker);
}

DEFINE_INTERNAL_CALL(void) ScriptedImporterContextInternal_GetAsset(Guid* asset)
{
    if (asset)
        *asset = CurrentContext ? CurrentContext->GetAsset().Value : Guid::Empty;
}

DEFINE_INTERNAL_CALL(MString*) ScriptedImporterContextInternal_GetSourcePath()
{
    return MUtils::ToString(CurrentContext ? CurrentContext->GetSourcePath() : String::Empty);
}

DEFINE_INTERNAL_CALL(MString*) ScriptedImporterContextInternal_GetTargetDimension(int32 dimension)
{
    if (!CurrentContext)
        return MUtils::ToString(String::Empty);
    const ArtifactTarget& target = CurrentContext->GetTarget();
    switch (dimension)
    {
    case 0: return MUtils::ToString(String(target.Platform));
    case 1: return MUtils::ToString(String(target.Architecture));
    case 2: return MUtils::ToString(String(target.Graphics));
    case 3: return MUtils::ToString(String(target.Configuration));
    case 4: return MUtils::ToString(String(target.Quality));
    case 5: return MUtils::ToString(String(target.TextureCompression));
    case 6: return MUtils::ToString(String(target.AudioCodec));
    case 7: return MUtils::ToString(String(target.ShaderCompiler));
    case 8: return MUtils::ToString(String(target.Role));
    default: return MUtils::ToString(String::Empty);
    }
}

DEFINE_INTERNAL_CALL(int32) ScriptedImporterContextInternal_GetTargetFeatureFlagCount()
{
    return CurrentContext ? CurrentContext->GetTarget().FeatureFlags.Count() : 0;
}

DEFINE_INTERNAL_CALL(MString*) ScriptedImporterContextInternal_GetTargetFeatureFlag(int32 index)
{
    if (!CurrentContext || index < 0 || index >= CurrentContext->GetTarget().FeatureFlags.Count())
        return MUtils::ToString(String::Empty);
    return MUtils::ToString(String(CurrentContext->GetTarget().FeatureFlags[index]));
}

DEFINE_INTERNAL_CALL(MString*) ScriptedImporterContextInternal_GetSettings()
{
    return MUtils::ToString(CurrentContext ? String(CurrentContext->GetSettings()) : String::Empty);
}

DEFINE_INTERNAL_CALL(MArray*) ScriptedImporterContextInternal_Read(MString* pathObject, int32* count, bool* failed)
{
    if (count)
        *count = 0;
    if (failed)
        *failed = true;
    AssetPipelineDiagnostic diagnostic;
    if (!RequireContext(&diagnostic))
    {
        const Array<byte> empty;
        return MUtils::ToArray(empty, MCore::TypeCache::Byte);
    }
    String path;
    MUtils::ToString(pathObject, path);
    Array<byte> bytes;
    ContentHash hash;
    const bool readFailed = path.IsEmpty()
        ? CurrentContext->ReadSource(bytes, hash, diagnostic)
        : CurrentContext->ReadDependencyFile(path, bytes, hash, diagnostic);
    if (readFailed)
    {
        CurrentContext->AddDiagnostic(diagnostic);
        const Array<byte> empty;
        return MUtils::ToArray(empty, MCore::TypeCache::Byte);
    }
    if (count)
        *count = bytes.Count();
    if (failed)
        *failed = false;
    return MUtils::ToArray(bytes, MCore::TypeCache::Byte);
}

DEFINE_INTERNAL_CALL(void) ScriptedImporterContextInternal_DependsOnObject(Guid* asset, int64 localId, int32 kind)
{
    if (!RequireContext() || !asset)
        return;
    const AssetObjectId object(AssetGuid(*asset), localId);
    if (kind == 0)
        CurrentContext->DependsOnSourceAsset(object);
    else
        CurrentContext->DependsOnArtifact(object);
}

DEFINE_INTERNAL_CALL(bool) ScriptedImporterContextInternal_DependsOnExactArtifact(MString* artifactObject)
{
    AssetPipelineDiagnostic diagnostic;
    if (!RequireContext(&diagnostic))
        return true;
    String artifactText;
    MUtils::ToString(artifactObject, artifactText);
    ArtifactKey artifact;
    if (ArtifactKey::Parse(artifactText, artifact) || artifact.IsZero())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = CurrentContext->GetAsset().Value;
        diagnostic.SourcePath = CurrentContext->GetSourcePath();
        diagnostic.Message = TEXT("Scripted importer declared an invalid exact artifact dependency key.");
        CurrentContext->AddDiagnostic(diagnostic);
        SetError(diagnostic.Message);
        return true;
    }
    CurrentContext->DependsOnArtifact(artifact);
    SetError(StringView::Empty);
    return false;
}

DEFINE_INTERNAL_CALL(void) ScriptedImporterContextInternal_DependsOnNamed(int32 kind, MString* identityObject, MString* hashObject)
{
    if (!RequireContext())
        return;
    String identity;
    MUtils::ToString(identityObject, identity);
    ContentHash hash = ParseOptionalHash(hashObject);
    if (kind == 0)
    {
        if (hash.IsZero())
        {
            if (CustomDependencyRegistry* registry = AssetImportService::GetCustomDependencyRegistry())
                registry->TryGet(identity, hash);
        }
        if (hash.IsZero())
        {
            AssetPipelineDiagnostic diagnostic;
            diagnostic.Code = AssetPipelineDiagnosticCode::UndeclaredInput;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = CurrentContext->GetAsset().Value;
            diagnostic.SourcePath = CurrentContext->GetSourcePath();
            diagnostic.Message = TEXT("Scripted importer referenced an unregistered custom dependency.");
            CurrentContext->AddDiagnostic(diagnostic);
        }
        CurrentContext->DependsOnCustomDependency(identity, hash);
    }
    else if (kind == 1)
        CurrentContext->DependsOnFolderContents(identity, hash);
    else if (kind == 2)
        CurrentContext->DependsOnSearchQuery(identity, hash);
    else if (kind == 3)
        CurrentContext->DependsOnToolchain(identity, hash);
    else
        CurrentContext->DependsOnProjectSetting(identity, hash);
}

DEFINE_INTERNAL_CALL(bool) ScriptedImporterContextInternal_DependsOnFolder(MString* pathObject)
{
    AssetPipelineDiagnostic diagnostic;
    if (!RequireContext(&diagnostic))
        return true;
    String path;
    MUtils::ToString(pathObject, path);
    AssetPathPolicy::ProjectPath folder;
    if (AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder,
        Globals::ProjectLibraryFolder, path, folder, diagnostic))
    {
        CurrentContext->AddDiagnostic(diagnostic);
        SetError(diagnostic.Message);
        return true;
    }
    if (CurrentWorkerRequest)
    {
        ContentHasher hasher;
        int32 matched = 0;
        for (const AssetImportWorkerInput& input : CurrentWorkerRequest->AuthorizedInputs)
        {
            if (!AssetPathPolicy::IsSameOrChild(input.CanonicalPath, folder.AbsolutePath))
                continue;
            const StringAnsi identity(input.Identity);
            hasher.Update(identity.Get(), identity.Length());
            hasher.Update(input.Hash.Bytes, sizeof(input.Hash.Bytes));
            matched++;
        }
        if (matched == 0)
            hasher.Update("empty-folder", 12);
        CurrentContext->DependsOnFolderContents(folder.ProjectRelativePath, hasher.Finalize());
        SetError(StringView::Empty);
        return false;
    }
    if (!FileSystem::DirectoryExists(folder.AbsolutePath))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.Message = TEXT("Scripted importer folder dependency does not exist.");
        CurrentContext->AddDiagnostic(diagnostic);
        SetError(diagnostic.Message);
        return true;
    }
    Array<String> files;
    if (FileSystem::DirectoryGetFiles(files, folder.AbsolutePath, TEXT("*"), DirectorySearchOption::AllDirectories))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.Message = TEXT("Scripted importer folder dependency could not be enumerated.");
        CurrentContext->AddDiagnostic(diagnostic);
        SetError(diagnostic.Message);
        return true;
    }
    std::sort(files.Get(), files.Get() + files.Count());
    ContentHasher hasher;
    for (const String& file : files)
    {
        Array<byte> bytes;
        ContentHash hash;
        if (CurrentContext->ReadDependencyFile(file, bytes, hash, diagnostic))
        {
            CurrentContext->AddDiagnostic(diagnostic);
            SetError(diagnostic.Message);
            return true;
        }
        AssetPathPolicy::ProjectPath normalized;
        if (AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder,
            Globals::ProjectLibraryFolder, file, normalized, diagnostic))
        {
            CurrentContext->AddDiagnostic(diagnostic);
            SetError(diagnostic.Message);
            return true;
        }
        const StringAnsi relative(normalized.ProjectRelativePath);
        hasher.Update(relative.Get(), relative.Length());
        hasher.Update(hash.Bytes, sizeof(hash.Bytes));
    }
    CurrentContext->DependsOnFolderContents(folder.ProjectRelativePath, hasher.Finalize());
    SetError(StringView::Empty);
    return false;
}

DEFINE_INTERNAL_CALL(int32) ScriptedImporterContextInternal_AddObject(MString* stableObject, MString* typeObject, MString* displayObject)
{
    if (!RequireContext())
        return -1;
    String stable, type, display;
    MUtils::ToString(stableObject, stable);
    MUtils::ToString(typeObject, type);
    MUtils::ToString(displayObject, display);
    return CurrentContext->AddObjectToAsset(stable, type, display);
}

DEFINE_INTERNAL_CALL(bool) ScriptedImporterContextInternal_SetMainObject(int32 objectIndex)
{
    AssetPipelineDiagnostic diagnostic;
    if (!RequireContext(&diagnostic) || CurrentContext->SetMainObject(objectIndex, diagnostic))
    {
        if (CurrentContext)
            CurrentContext->AddDiagnostic(diagnostic);
        return true;
    }
    return false;
}

DEFINE_INTERNAL_CALL(int32) ScriptedImporterContextInternal_CreateOutput(MString* nameObject, MString* kindObject,
    MString* extensionObject, uint32 targetDimensions)
{
    if (!RequireContext())
        return -1;
    String name, kind, extension;
    MUtils::ToString(nameObject, name);
    MUtils::ToString(kindObject, kind);
    MUtils::ToString(extensionObject, extension);
    return CurrentContext->CreateOutput(name, StringAnsi(kind), StringAnsi(extension),
        static_cast<ArtifactTargetDimension>(targetDimensions));
}

DEFINE_INTERNAL_CALL(bool) ScriptedImporterContextInternal_WriteOutput(int32 outputIndex, MArray* dataObject)
{
    AssetPipelineDiagnostic diagnostic;
    if (!RequireContext(&diagnostic))
        return true;
    const Array<byte> data = MUtils::ToArray<byte>(dataObject);
    if (CurrentContext->WriteOutput(outputIndex, Span<byte>(data.Get(), data.Count()), diagnostic))
    {
        CurrentContext->AddDiagnostic(diagnostic);
        return true;
    }
    return false;
}

DEFINE_INTERNAL_CALL(void) ScriptedImporterContextInternal_LogDiagnostic(int32 severity, MString* messageObject,
    MString* fileObject, int32 line, int32 column)
{
    if (!RequireContext())
        return;
    AssetPipelineDiagnostic diagnostic;
    diagnostic.Code = severity == 0 ? AssetPipelineDiagnosticCode::None : AssetPipelineDiagnosticCode::BuildFailed;
    if (severity < 0)
        severity = 0;
    else if (severity > 2)
        severity = 2;
    diagnostic.Severity = static_cast<AssetPipelineDiagnosticSeverity>(severity);
    diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
    diagnostic.AssetGuid = CurrentContext->GetAsset().Value;
    diagnostic.SourcePath = CurrentContext->GetSourcePath();
    MUtils::ToString(messageObject, diagnostic.Message);
    MUtils::ToString(fileObject, diagnostic.Location.File);
    diagnostic.Location.Line = line;
    diagnostic.Location.Column = column;
    CurrentContext->AddDiagnostic(diagnostic);
}

#endif
