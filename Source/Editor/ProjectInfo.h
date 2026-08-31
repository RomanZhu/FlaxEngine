// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/Version.h"
#include "Engine/Core/Math/Ray.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Collections/HashSet.h"

/// <summary>
/// Contains information about Flax project.
/// </summary>
class FLAXENGINE_API ProjectInfo
{
public:

    static constexpr int32 CurrentAssetSystemVersion = 3;
    static constexpr int32 CurrentArtifactLayoutVersion = 2;
    static constexpr int32 CurrentSourceDocumentVersion = 1;

    /// <summary>
    /// The loaded projects cache.
    /// </summary>
    static Array<ProjectInfo*> ProjectsCache;

public:

    /// <summary>
    /// The project reference.
    /// </summary>
    struct Reference
    {
        /// <summary>
        /// The referenced project name.
        /// </summary>
        String Name;

        /// <summary>
        /// The referenced project.
        /// </summary>
        ProjectInfo* Project;
    };

public:

    /// <summary>
    /// The project name.
    /// </summary>
    String Name;

    /// <summary>
    /// The absolute path to the project file.
    /// </summary>
    String ProjectPath;

    /// <summary>
    /// The project root folder path.
    /// </summary>
    String ProjectFolderPath;

    /// <summary>
    /// Committed one-way asset-system format marker. Zero means the project predates the marker.
    /// </summary>
    int32 AssetSystemVersion;

    /// <summary>Stable bootstrap identity of an asset-system v3 project.</summary>
    Guid ProjectId;

    /// <summary>The single writable project source-root declaration.</summary>
    String SourceRoot;

    /// <summary>The persistent source-object identity model.</summary>
    String IdentityModel;

    /// <summary>The immutable artifact store layout version.</summary>
    int32 ArtifactLayoutVersion;

    /// <summary>The authored source-document format version.</summary>
    int32 SourceDocumentVersion;

    /// <summary>True when this editor cannot safely write the project's asset-system format.</summary>
    bool AssetSystemReadOnly;

    /// <summary>
    /// The project version.
    /// </summary>
    ::Version Version;

    /// <summary>
    /// The project publisher company.
    /// </summary>
    String Company;

    /// <summary>
    /// The project copyright note.
    /// </summary>
    String Copyright;

    /// <summary>
    /// The name of the build target to use for the game building (final, cooked game code).
    /// </summary>
    String GameTarget;

    /// <summary>
    /// The name of the build target to use for the game in editor building (editor game code).
    /// </summary>
    String EditorTarget;

    /// <summary>
    /// The project references.
    /// </summary>
    Array<Reference> References;

    /// <summary>
    /// The default scene asset identifier to open on project startup.
    /// </summary>
    Guid DefaultScene;

    /// <summary>
    /// The default scene spawn point (position and view direction).
    /// </summary>
    Ray DefaultSceneSpawn;

    /// <summary>
    /// The minimum version supported by this project.
    /// </summary>
    ::Version MinEngineVersion;

    /// <summary>
    /// The user-friendly nickname of the engine installation to use when opening the project. Can be used to open game project with a custom engine distributed for team members. This value must be the same in engine and game projects to be paired.
    /// </summary>
    String EngineNickname;

public:

    ProjectInfo()
    {
        Version = ::Version(1, 0);
        AssetSystemVersion = CurrentAssetSystemVersion;
        ProjectId = Guid::New();
        SourceRoot = TEXT("Content");
        IdentityModel = TEXT("guid-local-id");
        ArtifactLayoutVersion = CurrentArtifactLayoutVersion;
        SourceDocumentVersion = CurrentSourceDocumentVersion;
        AssetSystemReadOnly = false;
        DefaultSceneSpawn = Ray(Vector3::Zero, Vector3::Forward);
        DefaultScene = Guid::Empty;
    }

    /// <summary>Validates the committed asset-system marker without changing project state.</summary>
    bool ValidateAssetSystemMarker(String& error) const;

    /// <summary>
    /// Saves the project file (*.flaxproj).
    /// </summary>
    /// <returns>True if cannot save it, otherwise false.</returns>
    bool SaveProject();

    /// <summary>
    /// Loads the project file (*.flaxproj).
    /// </summary>
    /// <param name="projectPath">The absolute path to the file with a project.</param>
    /// <returns>True if cannot load it, otherwise false.</returns>
    bool LoadProject(const String& projectPath);

    /// <summary>
    /// Loads the old project file (Project.xml).
    /// [Deprecated: 16.04.2020, expires 16.04.2021]
    /// </summary>
    /// <param name="projectPath">The absolute path to the file with a project.</param>
    /// <returns>True if cannot load it, otherwise false.</returns>
    bool LoadOldProject(const String& projectPath);

    /// <summary>
    /// Gets all projects including this project, it's references and their references (any deep level of references).
    /// </summary>
    /// <param name="result">The result list of projects (this and all references).</param>
    void GetAllProjects(HashSet<ProjectInfo*>& result)
    {
        result.Add(this);
        for (auto& reference : References)
            if (reference.Project)
                reference.Project->GetAllProjects(result);
    }

    /// <summary>
    /// Loads the project from the specified file.
    /// </summary>
    /// <param name="path">The path.</param>
    /// <returns>The loaded project.</returns>
    static ProjectInfo* Load(const String& path);
};
