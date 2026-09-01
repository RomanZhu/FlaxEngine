// Copyright (c) Wojciech Figat. All rights reserved.

#include "SceneFragmentReconciler.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Platform/FileSystem.h"

namespace
{
    void AddDiagnostic(Array<SceneFragmentDiagnostic>& diagnostics, SceneFragmentDiagnosticCode code,
        const Guid& sceneGuid, int64 localId, const StringView& path, const StringView& message)
    {
        SceneFragmentDiagnostic diagnostic;
        diagnostic.Code = code;
        diagnostic.Fragment.OwnerSceneGuid = sceneGuid;
        diagnostic.Fragment.RootActorLocalId = localId;
        diagnostic.Path = path;
        diagnostic.Message = message;
        diagnostics.Add(MoveTemp(diagnostic));
    }
}

void SceneFragmentReconciler::Reconcile(const Guid& sceneGuid, Array<SceneFragmentDiagnostic>& diagnostics)
{
    diagnostics.Clear();
    const String scenePath = SceneFragmentStore::GetScenePath(sceneGuid);
    SceneFragmentIndex index;
    String error;
    if (SceneFragmentStore::ReadIndex(sceneGuid, index, error))
    {
        const SceneFragmentDiagnosticCode code = !FileSystem::FileExists(SceneFragmentStore::GetIndexPath(sceneGuid))
                                                     ? SceneFragmentDiagnosticCode::IndexMissing
                                                     : error.Contains(TEXT("future"), StringSearchCase::IgnoreCase)
                                                           ? SceneFragmentDiagnosticCode::FutureVersion
                                                           : error.Contains(TEXT("ownership"), StringSearchCase::IgnoreCase)
                                                                 ? SceneFragmentDiagnosticCode::OwnerMismatch
                                                                 : error.Contains(TEXT("duplicate"), StringSearchCase::IgnoreCase)
                                                                       ? SceneFragmentDiagnosticCode::DuplicateLocalId
                                                                       : error.Contains(TEXT("misplaced"), StringSearchCase::IgnoreCase)
                                                                             ? SceneFragmentDiagnosticCode::MisplacedFragment
                                                                             : SceneFragmentDiagnosticCode::Malformed;
        AddDiagnostic(diagnostics, code, sceneGuid, 0, SceneFragmentStore::GetIndexPath(sceneGuid), error);
        return;
    }

    HashSet<String> indexedPaths;
    for (const SceneFragmentIndexEntry& entry : index.Fragments)
        indexedPaths.Add(scenePath / entry.RelativePhysicalPath);
    Array<Array<byte>> fragments;
    if (SceneFragmentStore::Load(sceneGuid, index, fragments, error))
    {
        const SceneFragmentDiagnosticCode code = error.Contains(TEXT("missing"), StringSearchCase::IgnoreCase)
                                                     ? SceneFragmentDiagnosticCode::MissingFragment
                                                     : error.Contains(TEXT("hash"), StringSearchCase::IgnoreCase)
                                                           ? SceneFragmentDiagnosticCode::ContentMismatch
                                                           : error.Contains(TEXT("owner"), StringSearchCase::IgnoreCase)
                                                                 ? SceneFragmentDiagnosticCode::OwnerMismatch
                                                                 : error.Contains(TEXT("duplicate"), StringSearchCase::IgnoreCase)
                                                                       ? SceneFragmentDiagnosticCode::DuplicateLocalId
                                                                       : error.Contains(TEXT("misplaced"), StringSearchCase::IgnoreCase)
                                                                             ? SceneFragmentDiagnosticCode::MisplacedFragment
                                                                             : SceneFragmentDiagnosticCode::Malformed;
        AddDiagnostic(diagnostics, code, sceneGuid, 0, scenePath, error);
    }

    Array<String> physicalFiles;
    if (FileSystem::DirectoryExists(scenePath) &&
        !FileSystem::DirectoryGetFiles(physicalFiles, scenePath, TEXT("*.sceneactor"), DirectorySearchOption::AllDirectories))
    {
        for (const String& path : physicalFiles)
        {
            bool found = indexedPaths.Contains(path);
            if (!found)
            {
                for (const auto& indexed : indexedPaths)
                {
                    if (FileSystem::AreFilePathsEquivalent(path, indexed.Item))
                    {
                        found = true;
                        break;
                    }
                }
            }
            if (!found)
            {
                AddDiagnostic(diagnostics, SceneFragmentDiagnosticCode::OrphanFragment, sceneGuid, 0, path,
                    TEXT("Private scene fragment is not referenced by its owner index."));
            }
        }
    }
}
