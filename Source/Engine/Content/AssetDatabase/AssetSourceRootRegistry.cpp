// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetSourceRootRegistry.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"

namespace
{
    const Guid ProjectContentRootId(0x50524f4au, 0x434f4e54u, 0x454e5400u, 0x00000001u);
    const Guid SceneFragmentsRootId(0x5343454eu, 0x45465241u, 0x474d454eu, 0x54530001u);
    const Guid EngineContentRootId(0x454e4749u, 0x4e45434fu, 0x4e54454eu, 0x54000001u);

    AssetSourceRootPermission Permissions(bool write, bool scan, bool genericMutation)
    {
        uint32 value = static_cast<uint32>(AssetSourceRootPermission::Read);
        if (write)
            value |= static_cast<uint32>(AssetSourceRootPermission::Write);
        if (scan)
            value |= static_cast<uint32>(AssetSourceRootPermission::Scan);
        if (genericMutation)
            value |= static_cast<uint32>(AssetSourceRootPermission::GenericMutation);
        return static_cast<AssetSourceRootPermission>(value);
    }

    String NormalizeAbsolute(const StringView& value, const StringView& owner = StringView::Empty)
    {
        String result = FileSystem::IsRelative(value) && !owner.IsEmpty() ? String(owner) / value : String(value);
        StringUtils::PathRemoveRelativeParts(result);
        result.Replace((Char)92, '/');
        while (result.Length() > 1 && result.EndsWith('/'))
            result.Resize(result.Length() - 1);
        return result;
    }

    String NormalizeLogical(const StringView& value)
    {
        String result(value);
        result.Replace((Char)92, '/');
        StringUtils::PathRemoveRelativeParts(result);
        while (result.StartsWith('/'))
            result = result.Substring(1);
        while (result.EndsWith('/'))
            result.Resize(result.Length() - 1);
        return result;
    }

    String RelativePrefix(const StringView& owner, const StringView& physical)
    {
        String result = FileSystem::ConvertAbsolutePathToRelative(owner, physical);
        result.Replace((Char)92, '/');
        if (result == TEXT("."))
            result.Clear();
        return NormalizeLogical(result);
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path,
        const StringView& message, AssetPipelineDiagnosticStage stage = AssetPipelineDiagnosticStage::Configuration)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }
}

AssetSourceRootRegistry::AssetSourceRootRegistry(const StringView& projectRoot, const StringView& libraryRoot)
    : _projectRoot(NormalizeAbsolute(projectRoot))
    , _libraryRoot(NormalizeAbsolute(libraryRoot, projectRoot))
{
}

const Array<AssetSourceRoot>& AssetSourceRootRegistry::GetRoots() const
{
    return _roots;
}

bool AssetSourceRootRegistry::RegisterProjectRoots(const StringView& contentRoot, AssetPipelineDiagnostic& diagnostic)
{
    AssetSourceRoot content;
    content.RootId = ProjectContentRootId;
    content.Name = TEXT("project-content");
    content.OwnerPath = _projectRoot;
    content.PhysicalPath = NormalizeAbsolute(contentRoot, _projectRoot);
    content.LogicalPrefix = RelativePrefix(content.OwnerPath, content.PhysicalPath);
    content.Kind = AssetSourceRootKind::ProjectContent;
    content.Visibility = AssetSourceRootVisibility::Public;
    content.Permissions = Permissions(true, true, true);
    content.PublicAssetNamespace = true;
    content.BrowserVisible = true;
    content.RequiresAdjacentMeta = true;
    if (Register(MoveTemp(content), diagnostic))
        return true;

    AssetSourceRoot fragments;
    fragments.RootId = SceneFragmentsRootId;
    fragments.Name = TEXT("project-external-actors");
    fragments.OwnerPath = _projectRoot;
    fragments.PhysicalPath = _projectRoot / TEXT("ExternalActors");
    fragments.Kind = AssetSourceRootKind::SceneFragments;
    fragments.Visibility = AssetSourceRootVisibility::Private;
    fragments.Permissions = Permissions(true, false, false);
    return Register(MoveTemp(fragments), diagnostic);
}

bool AssetSourceRootRegistry::RegisterEngineRoot(const StringView& ownerPath, const StringView& physicalPath,
    AssetPipelineDiagnostic& diagnostic)
{
    AssetSourceRoot root;
    root.RootId = EngineContentRootId;
    root.Name = TEXT("engine-content");
    root.OwnerPath = NormalizeAbsolute(ownerPath);
    root.PhysicalPath = NormalizeAbsolute(physicalPath, root.OwnerPath);
    root.LogicalPrefix = RelativePrefix(root.OwnerPath, root.PhysicalPath);
    root.Kind = AssetSourceRootKind::EngineContent;
    root.Visibility = AssetSourceRootVisibility::Public;
    root.Permissions = Permissions(false, true, false);
    root.PublicAssetNamespace = true;
    root.BrowserVisible = true;
    return Register(MoveTemp(root), diagnostic);
}

bool AssetSourceRootRegistry::RegisterPluginRoot(const Guid& rootId, const StringView& name,
    const StringView& physicalPath, const StringView& logicalPrefix, AssetPipelineDiagnostic& diagnostic)
{
    AssetSourceRoot root;
    root.RootId = rootId;
    root.Name = TEXT("plugin:") + String(name);
    root.OwnerPath = NormalizeAbsolute(physicalPath);
    root.PhysicalPath = root.OwnerPath;
    root.LogicalPrefix = NormalizeLogical(logicalPrefix);
    root.Kind = AssetSourceRootKind::PluginContent;
    root.Visibility = AssetSourceRootVisibility::Public;
    root.Permissions = Permissions(false, true, false);
    root.PublicAssetNamespace = true;
    root.BrowserVisible = true;
    return Register(MoveTemp(root), diagnostic);
}

bool AssetSourceRootRegistry::RegisterExternalReadOnlyRoot(const Guid& rootId, const StringView& name,
    const StringView& physicalPath, const StringView& logicalPrefix, AssetPipelineDiagnostic& diagnostic)
{
    AssetSourceRoot root;
    root.RootId = rootId;
    root.Name = TEXT("external:") + String(name);
    root.OwnerPath = NormalizeAbsolute(physicalPath);
    root.PhysicalPath = root.OwnerPath;
    root.LogicalPrefix = NormalizeLogical(logicalPrefix);
    root.Kind = AssetSourceRootKind::ExternalReadOnlyContent;
    root.Visibility = AssetSourceRootVisibility::Public;
    root.Permissions = Permissions(false, true, false);
    root.PublicAssetNamespace = true;
    root.BrowserVisible = true;
    return Register(MoveTemp(root), diagnostic);
}

bool AssetSourceRootRegistry::Register(AssetSourceRoot root, AssetPipelineDiagnostic& diagnostic)
{
    root.OwnerPath = NormalizeAbsolute(root.OwnerPath);
    root.PhysicalPath = NormalizeAbsolute(root.PhysicalPath, root.OwnerPath);
    root.LogicalPrefix = NormalizeLogical(root.LogicalPrefix);
    if (!root.RootId.IsValid() || root.Name.IsEmpty() || root.OwnerPath.IsEmpty() || root.PhysicalPath.IsEmpty() ||
        FileSystem::IsRelative(root.OwnerPath) || FileSystem::IsRelative(root.PhysicalPath) ||
        (root.PublicAssetNamespace && !AssetPathPolicy::IsPackageEntryPathValid(PackageEntryPath(root.LogicalPrefix))) ||
        (!root.PublicAssetNamespace && root.LogicalPrefix.HasChars()) ||
        !root.HasPermission(AssetSourceRootPermission::Read))
    {
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, root.PhysicalPath,
            TEXT("Asset source root identity, paths, logical prefix, or read permission is invalid."));
    }
    if ((root.Visibility == AssetSourceRootVisibility::Private && (root.PublicAssetNamespace || root.BrowserVisible)) ||
        (root.HasPermission(AssetSourceRootPermission::GenericMutation) &&
            (!root.HasPermission(AssetSourceRootPermission::Write) || !root.PublicAssetNamespace ||
                root.Visibility != AssetSourceRootVisibility::Public)))
    {
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, root.PhysicalPath,
            TEXT("Generic asset mutation requires a public writable source root."));
    }
    for (const AssetSourceRoot& existing : _roots)
    {
        if (existing.RootId == root.RootId || existing.Name == root.Name ||
            (root.LogicalPrefix.HasChars() && existing.LogicalPrefix.Compare(root.LogicalPrefix, StringSearchCase::IgnoreCase) == 0) ||
            existing.PhysicalPath.Compare(root.PhysicalPath, StringSearchCase::IgnoreCase) == 0)
        {
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, root.PhysicalPath,
                TEXT("Asset source root identity, logical prefix, or physical path is already registered."));
        }
    }
    _roots.Add(MoveTemp(root));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetSourceRootRegistry::Resolve(const StringView& input, ResolvedAssetSourcePath& result,
    AssetPipelineDiagnostic& diagnostic) const
{
    return ResolveWithPermission(input, AssetSourceRootPermission::Read, result, diagnostic);
}

bool AssetSourceRootRegistry::ResolveForScan(const StringView& input, ResolvedAssetSourcePath& result,
    AssetPipelineDiagnostic& diagnostic) const
{
    return ResolveWithPermission(input, AssetSourceRootPermission::Scan, result, diagnostic);
}

bool AssetSourceRootRegistry::ResolveForGenericMutation(const StringView& input, AssetPathPolicy::ProjectPath& result,
    AssetPipelineDiagnostic& diagnostic) const
{
    ResolvedAssetSourcePath resolved;
    if (ResolveWithPermission(input, AssetSourceRootPermission::GenericMutation, resolved, diagnostic) ||
        !resolved.Root.HasPermission(AssetSourceRootPermission::Write))
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, input,
                TEXT("Generic asset operations require a writable public source root."), AssetPipelineDiagnosticStage::Prepare);
        return true;
    }
    result = MoveTemp(resolved.Path);
    return false;
}

bool AssetSourceRootRegistry::PhysicalToLogical(const StringView& physicalPath, String& logicalPath,
    AssetPipelineDiagnostic& diagnostic) const
{
    ResolvedAssetSourcePath resolved;
    if (Resolve(physicalPath, resolved, diagnostic))
        return true;
    if (!resolved.Root.PublicAssetNamespace)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, physicalPath,
            TEXT("Private source roots have no public logical path."), AssetPipelineDiagnosticStage::Prepare);
    logicalPath = resolved.Path.ProjectRelativePath;
    return false;
}

bool AssetSourceRootRegistry::LogicalToPhysical(const StringView& logicalPath, String& physicalPath,
    AssetPipelineDiagnostic& diagnostic) const
{
    if (!FileSystem::IsRelative(logicalPath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, logicalPath,
            TEXT("A logical asset source path must be relative."), AssetPipelineDiagnosticStage::Prepare);
    ResolvedAssetSourcePath resolved;
    if (Resolve(logicalPath, resolved, diagnostic))
        return true;
    physicalPath = resolved.Path.AbsolutePath;
    return false;
}

bool AssetSourceRootRegistry::ResolveWithPermission(const StringView& input, AssetSourceRootPermission permission,
    ResolvedAssetSourcePath& result, AssetPipelineDiagnostic& diagnostic) const
{
    result = ResolvedAssetSourcePath();
    if (input.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, input,
            TEXT("Asset source path is empty."), AssetPipelineDiagnosticStage::Prepare);

    const bool logicalInput = FileSystem::IsRelative(input);
    String candidate = logicalInput ? NormalizeLogical(input) : NormalizeAbsolute(input);
    const AssetSourceRoot* selected = nullptr;
    int32 selectedLength = -1;
    for (const AssetSourceRoot& root : _roots)
    {
        if (logicalInput && !root.PublicAssetNamespace)
            continue;
        const StringView boundary = logicalInput ? StringView(root.LogicalPrefix) : StringView(root.PhysicalPath);
        if (AssetPathPolicy::IsSameOrChild(candidate, boundary) && boundary.Length() > selectedLength)
        {
            selected = &root;
            selectedLength = boundary.Length();
        }
    }
    if (!selected)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, input,
            TEXT("Asset source path is outside every registered root."), AssetPipelineDiagnosticStage::Prepare);
    if (!selected->HasPermission(permission))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, input,
            TEXT("Asset source root does not grant the requested operation."), AssetPipelineDiagnosticStage::Prepare);

    String absolute = candidate;
    if (logicalInput)
    {
        String relative = candidate.Substring(selected->LogicalPrefix.Length());
        while (relative.StartsWith('/'))
            relative = relative.Substring(1);
        absolute = relative.IsEmpty() ? selected->PhysicalPath : selected->PhysicalPath / relative;
    }
    AssetPathPolicy::ProjectPath normalized;
    if (AssetPathPolicy::TryNormalizeProjectPath(selected->OwnerPath, selected->PhysicalPath, _libraryRoot,
        absolute, normalized, diagnostic))
        return true;

    String relative = FileSystem::ConvertAbsolutePathToRelative(selected->PhysicalPath, normalized.AbsolutePath);
    relative.Replace((Char)92, '/');
    if (relative == TEXT("."))
        relative.Clear();
    normalized.DisplayPath = input;
    normalized.ProjectRelativePath = selected->PublicAssetNamespace
        ? selected->LogicalPrefix
        : RelativePrefix(selected->OwnerPath, selected->PhysicalPath);
    if (relative.HasChars())
        normalized.ProjectRelativePath = normalized.ProjectRelativePath / relative;
    normalized.PortabilityKey = normalized.ProjectRelativePath.ToLower();
    result.Root = *selected;
    result.Path = MoveTemp(normalized);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
