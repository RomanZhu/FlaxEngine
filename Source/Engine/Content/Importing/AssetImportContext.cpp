// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImportContext.h"
#include "Engine/Content/AssetDatabase/SubAsset.h"
#include <algorithm>

namespace
{
    bool ContextFailure(AssetPipelineDiagnostic& diagnostic, const AssetGuid& asset, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = asset.Value;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    bool DependencyLess(const AssetImportDependency& a, const AssetImportDependency& b)
    {
        if (a.Kind != b.Kind)
            return static_cast<byte>(a.Kind) < static_cast<byte>(b.Kind);
        if (a.Identity != b.Identity)
            return a.Identity < b.Identity;
        if (a.Object != b.Object)
            return a.Object.ToString() < b.Object.ToString();
        return a.ExactArtifact.ToString() < b.ExactArtifact.ToString();
    }
}

AssetImportContext::AssetImportContext(const AssetGuid& asset, const StringView& sourcePath, const ArtifactTarget& target,
                                       const StringAnsiView& settings, AssetImportReadCallback read)
    : _asset(asset)
    , _sourcePath(sourcePath)
    , _target(target)
    , _settings(settings)
    , _read(MoveTemp(read))
{
}

bool AssetImportContext::ReadSource(Array<byte>& data, ContentHash& hash, AssetPipelineDiagnostic& diagnostic)
{
    return ReadDependencyFile(_sourcePath, data, hash, diagnostic);
}

bool AssetImportContext::ReadDependencyFile(const StringView& path, Array<byte>& data, ContentHash& hash, AssetPipelineDiagnostic& diagnostic)
{
    if (_completed || !_read.IsBinded())
        return ContextFailure(diagnostic, _asset, _sourcePath, TEXT("Importer input reader is unavailable."));
    if (_read(path, data, hash, diagnostic))
        return true;
    AssetImportDependency dependency;
    dependency.Kind = AssetImportDependencyKind::SourceFile;
    dependency.Identity = path;
    dependency.ExpectedHash = hash;
    dependency.Origin = _sourcePath;
    _result.Dependencies.Add(MoveTemp(dependency));
    return false;
}

void AssetImportContext::DependsOnSourceAsset(const AssetObjectId& object, const StringView& origin)
{
    if (_completed)
        return;
    AssetImportDependency dependency;
    dependency.Kind = AssetImportDependencyKind::SourceAsset;
    dependency.Object = object;
    dependency.Identity = object.ToString();
    dependency.Origin = origin;
    _result.Dependencies.Add(MoveTemp(dependency));
}

void AssetImportContext::DependsOnSourceAsset(const AssetGuid& asset, const StringView& origin)
{
    DependsOnSourceAsset(AssetObjectId::Main(asset), origin);
}

void AssetImportContext::DependsOnArtifact(const AssetObjectId& object, const StringView& origin)
{
    if (_completed)
        return;
    AssetImportDependency dependency;
    dependency.Kind = AssetImportDependencyKind::ImportedObject;
    dependency.Object = object;
    dependency.Identity = object.ToString();
    dependency.Origin = origin;
    _result.Dependencies.Add(MoveTemp(dependency));
}

void AssetImportContext::DependsOnArtifact(const ArtifactKey& artifact, const StringView& origin)
{
    if (_completed)
        return;
    AssetImportDependency dependency;
    dependency.Kind = AssetImportDependencyKind::ImportedArtifact;
    dependency.ExactArtifact = artifact;
    dependency.Identity = String(artifact.ToString());
    dependency.Origin = origin;
    _result.Dependencies.Add(MoveTemp(dependency));
}

void AssetImportContext::AddNamedDependency(AssetImportDependencyKind kind, const StringView& identity, const ContentHash& value, const StringView& origin)
{
    if (_completed)
        return;
    AssetImportDependency dependency;
    dependency.Kind = kind;
    dependency.Identity = identity;
    dependency.ExpectedHash = value;
    dependency.Origin = origin;
    _result.Dependencies.Add(MoveTemp(dependency));
}

void AssetImportContext::DependsOnCustomDependency(const StringView& name, const ContentHash& value, const StringView& origin)
{
    AddNamedDependency(AssetImportDependencyKind::CustomDependency, name, value, origin);
}

void AssetImportContext::DependsOnFolderContents(const StringView& path, const ContentHash& value, const StringView& origin)
{
    AddNamedDependency(AssetImportDependencyKind::FolderContents, path, value, origin);
}

void AssetImportContext::DependsOnSearchQuery(const StringView& query, const ContentHash& value, const StringView& origin)
{
    AddNamedDependency(AssetImportDependencyKind::SearchQuery, query, value, origin);
}

void AssetImportContext::DependsOnToolchain(const StringView& name, const ContentHash& value, const StringView& origin)
{
    AddNamedDependency(AssetImportDependencyKind::Toolchain, name, value, origin);
}

void AssetImportContext::DependsOnProjectSetting(const StringView& name, const ContentHash& value, const StringView& origin)
{
    AddNamedDependency(AssetImportDependencyKind::ProjectSetting, name, value, origin);
}

int32 AssetImportContext::AddObjectToAsset(const StringView& stableIdentifier, const StringView& typeName, const StringView& displayName)
{
    if (_completed || !SubAssetPolicy::IsKeyValid(stableIdentifier) || typeName.IsEmpty())
        return -1;
    for (const AssetImportedObjectDeclaration& object : _result.Objects)
    {
        if (object.StableIdentifier == stableIdentifier)
            return -1;
    }
    AssetImportedObjectDeclaration declaration;
    declaration.StableIdentifier = stableIdentifier;
    declaration.TypeName = typeName;
    declaration.DisplayName = displayName;
    _result.Objects.Add(MoveTemp(declaration));
    return _result.Objects.Count() - 1;
}

int32 AssetImportContext::CreateOutput(const StringView& name, const StringAnsiView& kind, const StringAnsiView& extension,
                                       ArtifactTargetDimension targetDimensions)
{
    if (_completed || name.IsEmpty() || kind.IsEmpty() || extension.IsEmpty() ||
        (static_cast<uint32>(targetDimensions) & ~static_cast<uint32>(ArtifactTargetDimension::All)) != 0)
        return -1;
    for (const AssetImportOutputDeclaration& output : _result.Outputs)
    {
        if (output.Name == name)
            return -1;
    }
    AssetImportOutputDeclaration output;
    output.Name = name;
    output.Kind = kind;
    output.Extension = extension;
    output.TargetDimensions = targetDimensions;
    _result.Outputs.Add(MoveTemp(output));
    return _result.Outputs.Count() - 1;
}

bool AssetImportContext::WriteOutput(int32 outputIndex, const Span<byte>& data, AssetPipelineDiagnostic& diagnostic)
{
    if (_completed || outputIndex < 0 || outputIndex >= _result.Outputs.Count())
        return ContextFailure(diagnostic, _asset, _sourcePath, TEXT("Importer wrote to an undeclared output."));
    AssetImportOutputDeclaration& output = _result.Outputs[outputIndex];
    if (output.Data.HasItems())
        return ContextFailure(diagnostic, _asset, _sourcePath, TEXT("Importer output was completed more than once."));
    output.Data.Add(data.Get(), data.Length());
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetImportContext::SetMainObject(int32 objectIndex, AssetPipelineDiagnostic& diagnostic)
{
    if (_completed || objectIndex < 0 || objectIndex >= _result.Objects.Count())
        return ContextFailure(diagnostic, _asset, _sourcePath, TEXT("Importer selected an undeclared main object."));
    if (_result.MainObject >= 0)
        _result.Objects[_result.MainObject].IsMain = false;
    _result.MainObject = objectIndex;
    _result.Objects[objectIndex].IsMain = true;
    return false;
}

void AssetImportContext::AddDiagnostic(const AssetPipelineDiagnostic& diagnostic)
{
    _result.Diagnostics.Add(diagnostic);
}

bool AssetImportContext::Complete(bool requireMainObject, AssetImportContextResult& result, AssetPipelineDiagnostic& diagnostic)
{
    if (_completed)
        return ContextFailure(diagnostic, _asset, _sourcePath, TEXT("Importer context was completed more than once."));
    if (requireMainObject && _result.MainObject < 0)
        return ContextFailure(diagnostic, _asset, _sourcePath, TEXT("Importer did not declare its main object."));
    std::sort(_result.Dependencies.Get(), _result.Dependencies.Get() + _result.Dependencies.Count(), DependencyLess);
    for (int32 i = _result.Dependencies.Count() - 1; i > 0; i--)
    {
        const AssetImportDependency& a = _result.Dependencies[i - 1];
        const AssetImportDependency& b = _result.Dependencies[i];
        if (!DependencyLess(a, b) && !DependencyLess(b, a))
        {
            if (a.ExpectedHash != b.ExpectedHash)
            {
                diagnostic = AssetPipelineDiagnostic();
                diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
                diagnostic.AssetGuid = _asset.Value;
                diagnostic.SourcePath = _sourcePath;
                diagnostic.Message = TEXT("A controlled importer input changed while it was being read.");
                return true;
            }
            _result.Dependencies.RemoveAt(i);
        }
    }
    _completed = true;
    result = MoveTemp(_result);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
