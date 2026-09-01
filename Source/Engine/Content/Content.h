// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingType.h"
#ifndef _MSC_VER
#include "Engine/Core/Collections/Array.h"
#endif
#include "AssetInfo.h"
#include "Asset.h"
#include "Config.h"

class Engine;
class FlaxFile;
class BinaryAsset;
class IAssetFactory;
class AssetObjectRegistry;

// Content and assets statistics container.
API_STRUCT() struct FLAXENGINE_API ContentStats
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(ContentStats);

    // Amount of asset objects in memory.
    API_FIELD() int32 AssetsCount = 0;
    // Amount of loaded assets.
    API_FIELD() int32 LoadedAssetsCount = 0;
    // Amount of loading assets. Zero if all assets are loaded in.
    API_FIELD() int32 LoadingAssetsCount = 0;
    // Amount of virtual assets (don't have representation in file).
    API_FIELD() int32 VirtualAssetsCount = 0;
};

/// <summary>
/// Loads and manages assets.
/// </summary>
API_CLASS(Static) class FLAXENGINE_API Content
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(Content);
    friend Engine;
    friend Asset;
public:
    /// <summary>
    /// The time between content pool updates.
    /// </summary>
    static TimeSpan AssetsUpdateInterval;

    /// <summary>
    /// The time after asset with no references will be unloaded.
    /// </summary>
    static TimeSpan AssetsUnloadInterval;

public:
    /// <summary>
    /// Gets the transient/runtime object registry.
    /// </summary>
    /// <returns>The object registry.</returns>
    static AssetObjectRegistry* GetObjectRegistry();

    /// <summary>Gets the exact GameSettings bootstrap object from the cooked runtime catalog.</summary>
    static Guid GetRuntimeGameSettingsObject();

public:
    /// <summary>
    /// Finds asset information by an explicit transient/runtime scripting object identifier.
    /// </summary>
    /// <param name="runtimeId">The transient/runtime object identifier.</param>
    /// <param name="info">The output asset info. Filled with valid values only if method returns true.</param>
    /// <returns>True if found any asset, otherwise false.</returns>
    API_FUNCTION() static bool GetRuntimeAssetInfo(const Guid& runtimeId, API_PARAM(Out) AssetInfo& info);

    /// <summary>Finds asset information by persistent object GUID.</summary>
    API_FUNCTION() static bool GetAssetInfo(const Guid& id, API_PARAM(Out) AssetInfo& info);

    /// <summary>
    /// Finds the asset info by path.
    /// </summary>
    /// <param name="path">The asset path.</param>
    /// <param name="info">The output asset info. Filled with valid values only if method returns true.</param>
    /// <returns>True if found any asset, otherwise false.</returns>
    API_FUNCTION() static bool GetAssetInfo(const StringView& path, API_PARAM(Out) AssetInfo& info);

    /// <summary>
    /// Finds the editor package path for an exact persistent asset object.
    /// </summary>
    /// <param name="objectId">The persistent object identity.</param>
    /// <returns>The asset path, or empty if failed to find.</returns>
    API_FUNCTION() static StringView GetEditorAssetPath(const Guid& objectId);

    /// <summary>
    /// Finds all the asset IDs. Uses asset registry.
    /// </summary>
    /// <returns>The list of all asset IDs.</returns>
    API_FUNCTION() static Array<Guid, HeapAllocation> GetAllAssets();

    /// <summary>
    /// Finds all the asset IDs by type (exact type, without inheritance checks). Uses asset registry.
    /// </summary>
    /// <param name="type">The asset type.</param>
    /// <returns>The list of asset IDs that match the given type.</returns>
    API_FUNCTION() static Array<Guid, HeapAllocation> GetAllAssetsByType(const MClass* type);

public:
    /// <summary>
    /// Gets the asset factory used by the given asset type id.
    /// </summary>
    /// <param name="typeName">The asset type name identifier.</param>
    /// <returns>Asset factory or null if not found.</returns>
    static IAssetFactory* GetAssetFactory(const StringView& typeName);

    /// <summary>
    /// Gets the asset factory used by the given asset type id.
    /// </summary>
    /// <param name="assetInfo">The asset info.</param>
    /// <returns>Asset factory or null if not found.</returns>
    static IAssetFactory* GetAssetFactory(const AssetInfo& assetInfo);

public:
    /// <summary>
    /// Generates temporary asset path.
    /// </summary>
    /// <returns>Asset path for a temporary usage.</returns>
    API_FUNCTION() static String CreateTemporaryAssetPath();

public:
    /// <summary>
    /// Gets content statistics.
    /// </summary>
    API_PROPERTY() static ContentStats GetStats();

    /// <summary>
    /// Gets the assets (loaded or during load).
    /// </summary>
    /// <returns>The collection of assets.</returns>
    API_FUNCTION() static Array<Asset*, HeapAllocation> GetAssets();

    /// <summary>
    /// Gets the assets (loaded or during load).
    /// </summary>
    /// <param name="type">Type of the assets to search for. Includes any assets derived from the type.</param>
    /// <returns>Found actors list.</returns>
    API_FUNCTION() static Array<Asset*, HeapAllocation> GetAssets(API_PARAM(Attributes="TypeReference(typeof(Actor))") const MClass* type);

    /// <summary>
    /// Gets the assets (loaded or during load).
    /// </summary>
    /// <typeparam name="T">Type of the object.</typeparam>
    /// <returns>Found actors list.</returns>
    template<typename T>
    static Array<T*, HeapAllocation> GetAssets()
    {
        Array<Asset*, HeapAllocation> assets = GetAssets(T::GetStaticClass());
        return *(Array<T*, HeapAllocation>*) & assets;
    }

    /// <summary>
    /// Gets the raw dictionary of assets (loaded or during load).
    /// </summary>
    /// <returns>The collection of assets.</returns>
    static const Dictionary<Guid, Asset*, HeapAllocation>& GetAssetsRaw();

    /// <summary>Loads one persistent asset object by GUID.</summary>
    API_FUNCTION() static Asset* LoadAssetAsync(const Guid& objectId, API_PARAM(Attributes="TypeReference(typeof(Asset))") const MClass* type);

    /// <summary>Loads one persistent imported object without collapsing its local file identity. Cooked builds require an exact runtime catalog entry.</summary>
    static Asset* LoadAssetAsync(const Guid& objectId, const ScriptingTypeHandle& type);

    /// <summary>Loads the explicit main object for a source asset.</summary>
    API_FUNCTION() static Asset* LoadMainAssetAsync(const AssetGuid& asset, API_PARAM(Attributes="TypeReference(typeof(Asset))") const MClass* type);

    /// <summary>Loads the explicit main object for a source asset.</summary>
    static Asset* LoadMainAssetAsync(const AssetGuid& asset, const ScriptingTypeHandle& type);

    /// <summary>Loads one persistent imported object without collapsing its local file identity.</summary>
    template<typename T>
    FORCE_INLINE static T* LoadAssetAsync(const Guid& objectId)
    {
        return static_cast<T*>(LoadAssetAsync(objectId, T::TypeInitializer));
    }

    /// <summary>Loads the explicit main object for a source asset.</summary>
    template<typename T>
    FORCE_INLINE static T* LoadMainAssetAsync(const AssetGuid& asset)
    {
        return static_cast<T*>(LoadMainAssetAsync(asset, T::TypeInitializer));
    }

    /// <summary>Loads an exact asset object for passive editor presentation without scheduling artifact builds, including dependencies.</summary>
    static Asset* LoadAsyncPreview(const Guid& objectId, const ScriptingTypeHandle& type);

    /// <summary>
    /// Loads asset and holds it until it won't be referenced by any object. Returns null if asset is missing. Actual asset data loading is performed on a other thread in async.
    /// </summary>
    /// <param name="path">The path of the asset (absolute or relative to the current workspace directory).</param>
    /// <param name="type">The asset type. If loaded object has different type (excluding types derived from the given) the loading fails.</param>
    /// <returns>Loaded asset or null if cannot</returns>
    API_FUNCTION(Attributes="HideInEditor") static Asset* LoadAsync(const StringView& path, const MClass* type);

    /// <summary>
    /// Loads asset and holds it until it won't be referenced by any object. Returns null if asset is missing. Actual asset data loading is performed on a other thread in async.
    /// </summary>
    /// <param name="path">The path of the asset (absolute or relative to the current workspace directory).</param>
    /// <param name="type">The asset type. If loaded object has different type (excluding types derived from the given) the loading fails.</param>
    /// <returns>Loaded asset or null if cannot</returns>
    static Asset* LoadAsync(const StringView& path, const ScriptingTypeHandle& type);

    /// <summary>
    /// Loads asset and holds it until it won't be referenced by any object. Returns null if asset is missing. Actual asset data loading is performed on a other thread in async.
    /// </summary>
    /// <param name="path">The path of the asset (absolute or relative to the current workspace directory).</param>
    /// <typeparam name="T">Type of the asset to load. Includes any asset types derived from the type.</typeparam>
    /// <returns>Loaded asset or null if cannot</returns>
    template<typename T>
    FORCE_INLINE static T* LoadAsync(const StringView& path)
    {
        return static_cast<T*>(LoadAsync(path, T::TypeInitializer));
    }

    /// <summary>
    /// Loads internal engine asset and holds it until it won't be referenced by any object. Returns null if asset is missing. Actual asset data loading is performed on a other thread in async.
    /// </summary>
    /// <param name="internalPath">The path of the asset relative to the engine internal content (excluding the extension).</param>
    /// <param name="type">The asset type. If loaded object has different type (excluding types derived from the given) the loading fails.</param>
    /// <returns>The loaded asset or null if failed.</returns>
    API_FUNCTION(Attributes="HideInEditor") static Asset* LoadAsyncInternal(const StringView& internalPath, const MClass* type);

    /// <summary>
    /// Loads internal engine asset and holds it until it won't be referenced by any object. Returns null if asset is missing. Actual asset data loading is performed on a other thread in async.
    /// </summary>
    /// <param name="internalPath">The path of the asset relative to the engine internal content (excluding the extension).</param>
    /// <param name="type">The asset type. If loaded object has different type (excluding types derived from the given) the loading fails.</param>
    /// <returns>The loaded asset or null if failed.</returns>
    static Asset* LoadAsyncInternal(const StringView& internalPath, const ScriptingTypeHandle& type);

    /// <summary>
    /// Loads internal engine asset and holds it until it won't be referenced by any object. Returns null if asset is missing. Actual asset data loading is performed on a other thread in async.
    /// </summary>
    /// <param name="internalPath">The path of the asset relative to the engine internal content (excluding the extension).</param>
    /// <param name="type">The asset type. If loaded object has different type (excluding types derived from the given) the loading fails.</param>
    /// <returns>The loaded asset or null if failed.</returns>
    static Asset* LoadAsyncInternal(const Char* internalPath, const ScriptingTypeHandle& type);

    /// <summary>
    /// Loads internal engine asset and holds it until it won't be referenced by any object. Returns null if asset is missing. Actual asset data loading is performed on a other thread in async.
    /// </summary>
    /// <param name="internalPath">The path of the asset relative to the engine internal content (excluding the extension).</param>
    /// <returns>The loaded asset or null if failed.</returns>
    template<typename T>
    FORCE_INLINE static T* LoadAsyncInternal(const Char* internalPath)
    {
        return static_cast<T*>(LoadAsyncInternal(internalPath, T::TypeInitializer));
    }

    /// <summary>Loads and waits for one persistent asset object by GUID.</summary>
    template<typename T>
    static T* LoadAsset(const Guid& objectId, double timeoutInMilliseconds = 30000.0)
    {
        auto asset = LoadAssetAsync<T>(objectId);
        if (asset && !asset->WaitForLoaded(timeoutInMilliseconds))
            return asset;
        return nullptr;
    }

    /// <summary>Loads and waits for the explicit main object of a source asset.</summary>
    template<typename T>
    static T* LoadMainAsset(const AssetGuid& assetId, double timeoutInMilliseconds = 30000.0)
    {
        auto asset = LoadMainAssetAsync<T>(assetId);
        if (asset && !asset->WaitForLoaded(timeoutInMilliseconds))
            return asset;
        return nullptr;
    }

    /// <summary>
    /// Loads asset to the Content Pool and holds it until it won't be referenced by any object. Returns null if asset is missing. Actual asset data loading is performed on a other thread in async.
    /// Waits until asset will be loaded. It's equivalent to LoadAsync + WaitForLoaded.
    /// </summary>
    /// <param name="path">The path of the asset (absolute or relative to the current workspace directory).</param>
    /// <param name="timeoutInMilliseconds">Custom timeout value in milliseconds.</param>
    /// <typeparam name="T">Type of the asset to load. Includes any asset types derived from the type.</typeparam>
    /// <returns>Asset instance if loaded, null otherwise.</returns>
    template<typename T>
    static T* Load(const StringView& path, double timeoutInMilliseconds = 30000.0)
    {
        auto asset = LoadAsync<T>(path);
        if (asset && !asset->WaitForLoaded(timeoutInMilliseconds))
            return asset;
        return nullptr;
    }

    /// <summary>
    /// Determines whether input asset type name identifier is invalid.
    /// </summary>
    /// <param name="type">The requested type of the asset to be.</param>
    /// <param name="assetType">The actual type of the asset.</param>
    /// <returns><c>true</c> if asset type identifier is invalid otherwise, <c>false</c>.</returns>
    static bool IsAssetTypeIdInvalid(const ScriptingTypeHandle& type, const ScriptingTypeHandle& assetType);

private:
    static Asset* LoadAssetObjectAsyncInternal(const AssetObjectId& objectId, const ScriptingTypeHandle& type);
    static void BeginPassiveLoad();
    static void EndPassiveLoad();
    static bool IsPassiveLoad();

public:
    /// <summary>
    /// Finds the asset with at given path. Checks all loaded assets.
    /// </summary>
    /// <param name="path">The path.</param>
    /// <returns>The found asset or null if not loaded.</returns>
    API_FUNCTION() static Asset* GetAsset(const StringView& path);

    /// <summary>
    /// Finds the loaded asset with a transient/runtime scripting object identifier.
    /// </summary>
    /// <param name="runtimeId">The transient/runtime identifier.</param>
    /// <returns>The found asset or null if not loaded.</returns>
    API_FUNCTION() static Asset* GetRuntimeObject(const Guid& runtimeId);

    /// <summary>Finds the loaded asset with the exact persistent object identity.</summary>
    API_FUNCTION() static Asset* GetAsset(const Guid& objectId);

public:
    /// <summary>
    /// Deletes the specified asset.
    /// </summary>
    /// <param name="asset">The asset.</param>
    API_FUNCTION() static void DeleteAsset(Asset* asset);
    
    /// <summary>
    /// Deletes the script item at the specified path.
    /// </summary>
    /// <param name="path">The script path.</param>
    API_FUNCTION() static void DeleteScript(const StringView& path);

    /// <summary>
    /// Deletes the asset at the specified path.
    /// </summary>
    /// <param name="path">The asset path.</param>
    API_FUNCTION(Attributes="HideInEditor") static void DeleteAsset(const StringView& path);

public:
#if USE_EDITOR

    /// <summary>
    /// Renames the asset.
    /// </summary>
    /// <param name="oldPath">The old asset path.</param>
    /// <param name="newPath">The new asset path.</param>
    /// <returns>True if failed, otherwise false.</returns>
    API_FUNCTION() static bool RenameAsset(const StringView& oldPath, const StringView& newPath);

    /// <summary>
    /// Renames an asset folder as a single filesystem operation and updates loaded asset paths.
    /// </summary>
    /// <param name="oldPath">The old folder path.</param>
    /// <param name="newPath">The new folder path.</param>
    /// <returns>True if failed, otherwise false.</returns>
    API_FUNCTION() static bool RenameAssetFolder(const StringView& oldPath, const StringView& newPath);

    /// <summary>
    /// Performs the fast temporary asset clone to the temporary folder.
    /// </summary>
    /// <param name="path">The source path.</param>
    /// <param name="resultPath">The result path.</param>
    /// <returns>True if failed, otherwise false.</returns>
    static bool FastTmpAssetClone(const StringView& path, String& resultPath);

    /// <summary>
    /// Clones the asset file.
    /// </summary>
    /// <param name="dstPath">The destination path.</param>
    /// <param name="srcPath">The source path.</param>
    /// <param name="dstId">The destination id.</param>
    /// <returns>True if failed, otherwise false.</returns>
    static bool CloneAssetFile(const StringView& dstPath, const StringView& srcPath, const Guid& dstId, bool overwrite = false);

#endif

    /// <summary>
    /// Unloads the specified asset.
    /// </summary>
    /// <param name="asset">The asset.</param>
    API_FUNCTION() static void UnloadAsset(Asset* asset);

    /// <summary>
    /// Creates temporary and virtual asset of the given type.
    /// </summary>
    /// <returns>Created asset or null if failed.</returns>
    template<typename T>
    FORCE_INLINE static T* CreateVirtualAsset()
    {
        return static_cast<T*>(CreateVirtualAsset(T::TypeInitializer));;
    }

    /// <summary>
    /// Creates temporary and virtual asset of the given type.
    /// </summary>
    /// <param name="type">The asset type klass.</param>
    /// <returns>Created asset or null if failed.</returns>
    API_FUNCTION() static Asset* CreateVirtualAsset(API_PARAM(Attributes="TypeReference(typeof(Asset))") const MClass* type);

    /// <summary>
    /// Creates temporary and virtual asset of the given type.
    /// </summary>
    /// <param name="type">The asset type.</param>
    /// <returns>Created asset or null if failed.</returns>
    static Asset* CreateVirtualAsset(const ScriptingTypeHandle& type);

    /// <summary>
    /// Occurs when asset is being disposed and will be unloaded (by force). All references to it should be released.
    /// </summary>
    API_EVENT() static Delegate<Asset*> AssetDisposing;

    /// <summary>
    /// Occurs when asset is being reloaded and will be unloaded (by force) to be loaded again (e.g. after reimport). Always called from the main thread.
    /// </summary>
    API_EVENT() static Delegate<Asset*> AssetReloading;

    /// <summary>
    /// Occurs when a loaded binary asset is being switched to newly generated artifact storage. Always called from the main thread.
    /// </summary>
    API_EVENT() static Delegate<Asset*> AssetArtifactReloading;

private:
    friend class BinaryAsset;
    static void WaitForTask(ContentLoadTask* loadingTask, double timeoutInMilliseconds);
    static void tryCallOnLoaded(Asset* asset);
    static void onAssetLoaded(Asset* asset);
    static void onAssetUnload(Asset* asset);
    static void onAssetChangeId(Asset* asset, const Guid& oldId, const Guid& newId);
#if USE_EDITOR
    friend class ContentService;
    static void onAssetDepend(BinaryAsset* asset, const Guid& otherId);
    static void onAddDependencies(Asset* asset);
#endif
    static void deleteFileSafety(const StringView& path, const Guid* id = nullptr);

    // Internal bindings
#if !COMPILE_WITHOUT_CSHARP
    API_FUNCTION(NoProxy) static void* GetAssetsInternal();
#endif
};
