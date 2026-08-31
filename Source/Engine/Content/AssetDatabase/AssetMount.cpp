// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetMount.h"
#include "AssetPath.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"

namespace
{
    AssetMountTable Registry;

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    bool IsLogicalPathValid(const StringView& value)
    {
        if (value.IsEmpty() || value.StartsWith('/') || value.EndsWith('/') || value.Contains(TEXT("\\")))
            return false;
        Array<String> segments;
        String(value).Split('/', segments);
        for (const String& segment : segments)
        {
            if (segment.IsEmpty() || segment == TEXT(".") || segment == TEXT(".."))
                return false;
        }
        return true;
    }

    bool IsPrefixOrChild(const StringView& path, const StringView& prefix)
    {
        if (path.Compare(prefix, StringSearchCase::IgnoreCase) == 0)
            return true;
        return path.Length() > prefix.Length() && path.StartsWith(prefix, StringSearchCase::IgnoreCase) && path[prefix.Length()] == '/';
    }

    bool ValidatePrefix(const AssetMount& mount)
    {
        if (!IsLogicalPathValid(mount.LogicalPrefix) || mount.LogicalPrefix == TEXT("Packages") ||
            mount.LogicalPrefix.StartsWith(TEXT("Packages/"), StringSearchCase::IgnoreCase) ||
            mount.LogicalPrefix == TEXT("Assets") || mount.LogicalPrefix.StartsWith(TEXT("Assets/"), StringSearchCase::IgnoreCase))
            return false;
        switch (mount.Kind)
        {
        case AssetMountKind::ProjectContent:
            return mount.LogicalPrefix == TEXT("Content") && mount.Writable;
        case AssetMountKind::EngineContent:
            return mount.LogicalPrefix == TEXT("EngineContent") && !mount.Writable;
        case AssetMountKind::PluginContent:
        {
            if (!mount.LogicalPrefix.StartsWith(TEXT("PluginContent/")) || mount.Writable)
                return false;
            const StringView id(mount.LogicalPrefix.Get() + 14, mount.LogicalPrefix.Length() - 14);
            return !id.IsEmpty() && !id.Contains(TEXT("/"));
        }
        case AssetMountKind::ExternalReadOnlyContent:
        {
            if (!mount.LogicalPrefix.StartsWith(TEXT("ExternalContent/")) || mount.Writable)
                return false;
            const StringView id(mount.LogicalPrefix.Get() + 16, mount.LogicalPrefix.Length() - 16);
            return !id.IsEmpty() && !id.Contains(TEXT("/"));
        }
        default:
            return false;
        }
    }

    String NormalizeAbsolute(const StringView& value)
    {
        String result(value);
        StringUtils::PathRemoveRelativeParts(result);
        result.Replace('\\', '/');
        while (result.Length() > 1 && result.EndsWith('/'))
            result.Remove(result.Length() - 1, 1);
        return result;
    }

    bool ResolveUnderMount(const AssetMount& mount, const StringView& input, String& resolved, AssetPipelineDiagnostic& diagnostic)
    {
        if (AssetPathPolicy::TryResolvePhysicalPath(input, resolved, diagnostic))
            return true;
        if (!AssetPathPolicy::IsSameOrChild(resolved, mount.PhysicalRoot))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, input,
                TEXT("Asset path escapes its registered mount through a filesystem link."));
        return false;
    }
}

bool AssetMountTable::InitializeProject(const StringView& projectRoot, const StringView& contentRoot, AssetPipelineDiagnostic& diagnostic)
{
    Clear();
    const String expected = NormalizeAbsolute(String(projectRoot) / TEXT("Content"));
    const String requested = NormalizeAbsolute(contentRoot);
    if (!FileSystem::AreFilePathsEquivalent(expected, requested))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, contentRoot,
            TEXT("The writable project mount must be the fixed Content directory."));
    AssetMount mount;
    mount.MountId = Guid(0x6f2434a1, 0xf554466f, 0x9496130a, 0xd2c82451);
    mount.LogicalPrefix = TEXT("Content");
    mount.PhysicalRoot = requested;
    mount.Kind = AssetMountKind::ProjectContent;
    mount.Writable = true;
    return Register(MoveTemp(mount), diagnostic);
}

bool AssetMountTable::Register(AssetMount mount, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    if (!mount.MountId.IsValid() || !ValidatePrefix(mount) || mount.PhysicalRoot.IsEmpty() || FileSystem::IsRelative(mount.PhysicalRoot))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, mount.PhysicalRoot,
            TEXT("Asset mount identity, logical prefix, permissions, kind, or physical root is invalid."));
    mount.PhysicalRoot = NormalizeAbsolute(mount.PhysicalRoot);
    if (!FileSystem::DirectoryExists(mount.PhysicalRoot))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, mount.PhysicalRoot, TEXT("Asset mount physical root does not exist."));
    String resolvedRoot;
    if (AssetPathPolicy::TryResolvePhysicalPath(mount.PhysicalRoot, resolvedRoot, diagnostic))
        return true;
    if (!mount.AllowLinkedRoot && !FileSystem::AreFilePathsEquivalent(mount.PhysicalRoot, resolvedRoot))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, mount.PhysicalRoot,
            TEXT("Asset mount root resolves through a link that was not explicitly allowed."));
    mount.PhysicalRoot = resolvedRoot;

    ScopeLock lock(_locker);
    for (const AssetMount& existing : _mounts)
    {
        if (existing.MountId == mount.MountId)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::DuplicateGuid, mount.PhysicalRoot, TEXT("Asset mount ID is already registered."));
        if (IsPrefixOrChild(existing.LogicalPrefix, mount.LogicalPrefix) || IsPrefixOrChild(mount.LogicalPrefix, existing.LogicalPrefix))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, mount.LogicalPrefix, TEXT("Asset mount logical prefixes overlap."));
        if (AssetPathPolicy::IsSameOrChild(existing.PhysicalRoot, mount.PhysicalRoot) || AssetPathPolicy::IsSameOrChild(mount.PhysicalRoot, existing.PhysicalRoot))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, mount.PhysicalRoot, TEXT("Asset mount physical roots overlap."));
        if (existing.Kind == AssetMountKind::ProjectContent && mount.Kind == AssetMountKind::ProjectContent)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, mount.PhysicalRoot, TEXT("Exactly one writable project Content mount is allowed."));
    }
    _mounts.Add(MoveTemp(mount));
    return false;
}

bool AssetMountTable::ReplaceAll(const Array<AssetMount>& mounts, AssetPipelineDiagnostic& diagnostic)
{
    AssetMountTable candidate;
    for (const AssetMount& mount : mounts)
    {
        if (candidate.Register(mount, diagnostic))
            return true;
    }
    Array<AssetMount> validated = candidate.GetMounts();
    {
        ScopeLock lock(_locker);
        _mounts = MoveTemp(validated);
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void AssetMountTable::Clear()
{
    ScopeLock lock(_locker);
    _mounts.Clear();
}

Array<AssetMount> AssetMountTable::GetMounts() const
{
    ScopeLock lock(_locker);
    return _mounts;
}

bool AssetMountTable::ResolveLogical(const StringView& logicalPath, AssetMountResolution& result, AssetPipelineDiagnostic& diagnostic) const
{
    result = AssetMountResolution();
    if (!IsLogicalPathValid(logicalPath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, logicalPath, TEXT("Logical asset path is not canonical."));
    AssetMount mount;
    bool found = false;
    {
        ScopeLock lock(_locker);
        for (const AssetMount& candidate : _mounts)
        {
            if (IsPrefixOrChild(logicalPath, candidate.LogicalPrefix))
            {
                mount = candidate;
                found = true;
                break;
            }
        }
    }
    if (!found)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, logicalPath, TEXT("Logical asset path does not belong to a registered mount."));
    const String relative = logicalPath.Length() == mount.LogicalPrefix.Length()
        ? String::Empty
        : String(logicalPath.Get() + mount.LogicalPrefix.Length() + 1, logicalPath.Length() - mount.LogicalPrefix.Length() - 1);
    const String physical = relative.IsEmpty() ? mount.PhysicalRoot : mount.PhysicalRoot / relative;
    String resolved;
    if (ResolveUnderMount(mount, physical, resolved, diagnostic))
        return true;
    result.Found = true;
    result.Mount = mount;
    result.LogicalPath = logicalPath;
    result.RelativePath = relative;
    result.PhysicalPath = NormalizeAbsolute(physical);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetMountTable::ResolvePhysical(const StringView& physicalPath, AssetMountResolution& result, AssetPipelineDiagnostic& diagnostic) const
{
    result = AssetMountResolution();
    if (physicalPath.IsEmpty() || FileSystem::IsRelative(physicalPath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, physicalPath, TEXT("Physical asset path must be absolute."));
    const String normalized = NormalizeAbsolute(physicalPath);
    String resolved;
    if (AssetPathPolicy::TryResolvePhysicalPath(normalized, resolved, diagnostic))
        return true;
    AssetMount mount;
    bool found = false;
    {
        ScopeLock lock(_locker);
        for (const AssetMount& candidate : _mounts)
        {
            if (AssetPathPolicy::IsSameOrChild(normalized, candidate.PhysicalRoot) && AssetPathPolicy::IsSameOrChild(resolved, candidate.PhysicalRoot))
            {
                mount = candidate;
                found = true;
                break;
            }
        }
    }
    if (!found)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, physicalPath, TEXT("Physical asset path does not belong to a registered mount."));
    String relative = FileSystem::ConvertAbsolutePathToRelative(mount.PhysicalRoot, normalized);
    relative.Replace('\\', '/');
    result.Found = true;
    result.Mount = mount;
    result.RelativePath = relative == TEXT(".") ? String::Empty : relative;
    result.LogicalPath = result.RelativePath.IsEmpty() ? mount.LogicalPrefix : mount.LogicalPrefix + TEXT("/") + result.RelativePath;
    result.PhysicalPath = normalized;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetMountTable::IsWritable(const StringView& logicalPath) const
{
    AssetMountResolution result;
    AssetPipelineDiagnostic diagnostic;
    return !ResolveLogical(logicalPath, result, diagnostic) && result.Mount.Writable;
}

AssetMountTable& AssetMountRegistry::Get()
{
    return Registry;
}

Array<AssetMount> AssetMountRegistry::GetMounts()
{
    return Registry.GetMounts();
}

bool AssetMountRegistry::TryResolveLogical(const StringView& logicalPath, AssetMountResolution& result)
{
    AssetPipelineDiagnostic diagnostic;
    return !Registry.ResolveLogical(logicalPath, result, diagnostic);
}

bool AssetMountRegistry::TryResolvePhysical(const StringView& physicalPath, AssetMountResolution& result)
{
    AssetPipelineDiagnostic diagnostic;
    return !Registry.ResolvePhysical(physicalPath, result, diagnostic);
}

bool AssetMountRegistry::IsWritable(const StringView& logicalPath)
{
    return Registry.IsWritable(logicalPath);
}
