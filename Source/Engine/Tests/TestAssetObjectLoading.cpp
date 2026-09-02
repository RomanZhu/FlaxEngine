// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Loading/AssetHotReloadCoordinator.h"
#include <ThirdParty/catch2/catch.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
    ContentHash LoadingTestHash(const char* value)
    {
        return ContentHash::Compute(value, StringUtils::Length(value));
    }

    bool ContainsAscii(const Array<byte>& bytes, const char* value)
    {
        const int32 length = StringUtils::Length(value);
        for (int32 i = 0; i + length <= bytes.Count(); i++)
        {
            if (Platform::MemoryCompare(bytes.Get() + i, value, length) == 0)
                return true;
        }
        return false;
    }

    AssetObjectLoadLocation TestLocation(const Guid& object, uint64 revision, const AssetObjectId& storageObject = AssetObjectId())
    {
        AssetObjectLoadLocation result;
        result.Object = object;
        result.StorageObject = storageObject.IsValid() ? storageObject : AssetObjectId::Main(AssetGuid(object));
        result.InstanceID = object;
        result.StorageKind = AssetObjectStorageKind::EditorArtifact;
        result.TypeName = "FlaxEngine.Texture";
        result.StorageName = "artifact/object.bin";
        result.Size = 32;
        result.Content = LoadingTestHash("object-content");
        result.Artifact = ArtifactKey(LoadingTestHash("artifact-key"));
        result.Revision = revision;
        return result;
    }

    class TestObjectResolver : public IEditorAssetObjectResolver, public IRuntimeAssetObjectResolver
    {
    private:
        std::mutex _gateMutex;
        std::condition_variable _gate;
        bool _entered = false;
        bool _released = false;

        bool Resolve(const Guid& object, AssetObjectLoadLocation& location,
            AssetPipelineDiagnostic& diagnostic)
        {
            Calls.fetch_add(1);
            if (Block)
            {
                std::unique_lock<std::mutex> lock(_gateMutex);
                _entered = true;
                _gate.notify_all();
                _gate.wait(lock, [&]() { return _released; });
            }
            if (FailResolution || !Locations.TryGet(object, location))
            {
                diagnostic = AssetPipelineDiagnostic();
                diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactMissing;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
                diagnostic.AssetGuid = object;
                diagnostic.Message = TEXT("Test object is unresolved.");
                return true;
            }
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

    public:
        Dictionary<Guid, AssetObjectLoadLocation> Locations;
        std::atomic<int32> Calls{0};
        bool Block = false;
        bool FailResolution = false;

        bool ResolveArtifactObject(const Guid& object, AssetObjectLoadLocation& location,
            AssetPipelineDiagnostic& diagnostic) override
        {
            return Resolve(object, location, diagnostic);
        }

        bool ResolveCatalogObject(const Guid& object, AssetObjectLoadLocation& location,
            AssetPipelineDiagnostic& diagnostic) override
        {
            return Resolve(object, location, diagnostic);
        }

        void Set(const AssetObjectLoadLocation& location)
        {
            AssetObjectLoadLocation* existing = Locations.TryGet(location.Object);
            if (existing)
                *existing = location;
            else
                Locations.Add(location.Object, location);
        }

        void WaitUntilEntered()
        {
            std::unique_lock<std::mutex> lock(_gateMutex);
            _gate.wait(lock, [&]() { return _entered; });
        }

        void Release()
        {
            std::lock_guard<std::mutex> lock(_gateMutex);
            _released = true;
            _gate.notify_all();
        }
    };

    class TestObjectFactory : public IAssetObjectFactory
    {
    public:
        std::atomic<int32> Creates{0};
        std::atomic<int32> Destroys{0};
        String LastStorage;
        String LastSource;
        AssetObjectId LastStorageObject;
        AssetObjectStorageKind LastStorageKind = AssetObjectStorageKind::EditorArtifact;
        Array<Guid> LastDependencies;

        bool CreateObject(const AssetObjectLoadLocation& location, void*& instance,
            AssetPipelineDiagnostic& diagnostic) override
        {
            const int32 number = Creates.fetch_add(1) + 1;
            LastStorage = location.StorageName;
            LastSource = location.SourceName;
            LastStorageObject = location.StorageObject;
            LastStorageKind = location.StorageKind;
            LastDependencies = location.Dependencies;
            instance = reinterpret_cast<void*>(static_cast<uintptr_t>(number));
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

        void DestroyObject(void* instance) override
        {
            REQUIRE(instance != nullptr);
            Destroys.fetch_add(1);
        }
    };

    class TestMainThreadDispatcher : public IAssetMainThreadDispatcher
    {
    public:
        int32 Calls = 0;

        bool InvokeAndWait(const Function<void()>& action) override
        {
            Calls++;
            action();
            return false;
        }
    };

    class TestReloadListener : public IAssetObjectReloadListener
    {
    public:
        LoadedAssetRegistry* Registry = nullptr;
        Array<Guid> Objects;
        Array<uint64> PreviousRevisions;
        Array<uint64> Revisions;
        Array<StringAnsi> PreviousTypeNames;
        Array<StringAnsi> TypeNames;
        Array<ContentHash> PreviousContents;
        Array<ContentHash> Contents;
        Array<LoadedAssetRecord> PublishedRecords;
        Array<Guid> InvalidatedObjects;
        Array<LoadedAssetState> InvalidatedStates;
        Array<AssetPipelineDiagnostic> InvalidationDiagnostics;
        bool ObserveAtomicInventory = false;
        Array<Guid> ExpectedPublishedObjects;

        void OnAssetObjectReplaced(const LoadedAssetSwap& swap) override
        {
            Objects.Add(swap.Object);
            PreviousRevisions.Add(swap.PreviousRevision);
            Revisions.Add(swap.Revision);
            PreviousTypeNames.Add(swap.PreviousTypeName);
            TypeNames.Add(swap.TypeName);
            PreviousContents.Add(swap.PreviousContent);
            Contents.Add(swap.Content);
            if (Registry)
            {
                LoadedAssetRecord record;
                REQUIRE(Registry->TryGet(swap.Object, record));
                PublishedRecords.Add(MoveTemp(record));
            }
        }

        void OnAssetObjectInvalidated(const LoadedAssetInvalidation& invalidation) override
        {
            InvalidatedObjects.Add(invalidation.Object);
            InvalidatedStates.Add(invalidation.State);
            InvalidationDiagnostics.Add(invalidation.Diagnostic);
            if (Registry && ObserveAtomicInventory)
            {
                LoadedAssetRecord record;
                REQUIRE(Registry->TryGet(invalidation.Object, record));
                CHECK(record.State == LoadedAssetState::Deleted);
                CHECK(record.Instance == nullptr);
                CHECK(record.StaleInstance == invalidation.PreviousInstance);
                for (const Guid& object : ExpectedPublishedObjects)
                {
                    REQUIRE(Registry->TryGet(object, record));
                    CHECK(record.Revision == 2);
                }
            }
        }
    };
}

TEST_CASE("Object loader preserves unresolved persistent GUID identity")
{
    const Guid object(101, 0, 0, 0);
    LoadedAssetRegistry registry;
    TestObjectResolver resolver;
    resolver.FailResolution = true;
    TestObjectFactory factory;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetObjectLoadResult result;
    AssetPipelineDiagnostic diagnostic;
    CHECK(loader.Load(object, result, diagnostic));
    CHECK(result.Object == object);
    CHECK(result.State == LoadedAssetState::Unresolved);
    CHECK(result.Instance == nullptr);
    LoadedAssetRecord record;
    REQUIRE(registry.TryGet(object, record));
    CHECK(record.Object == object);
    CHECK(record.State == LoadedAssetState::Unresolved);
    CHECK(record.Diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactMissing);
}

TEST_CASE("Object loader deduplicates simultaneous exact object loads")
{
    const Guid object(102, 0, 0, 0);
    LoadedAssetRegistry registry;
    TestObjectResolver resolver;
    resolver.Set(TestLocation(object, 1));
    resolver.Block = true;
    TestObjectFactory factory;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetObjectLoadResult first;
    AssetObjectLoadResult second;
    AssetPipelineDiagnostic firstDiagnostic;
    AssetPipelineDiagnostic secondDiagnostic;
    bool firstFailed = true;
    bool secondFailed = true;
    std::thread firstThread([&]() { firstFailed = loader.Load(object, first, firstDiagnostic); });
    resolver.WaitUntilEntered();
    std::atomic<bool> secondStarted{false};
    std::thread secondThread([&]()
    {
        secondStarted.store(true);
        secondFailed = loader.Load(object, second, secondDiagnostic);
    });
    while (!secondStarted.load())
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(resolver.Calls.load() == 1);
    resolver.Release();
    firstThread.join();
    secondThread.join();
    CHECK_FALSE(firstFailed);
    CHECK_FALSE(secondFailed);
    CHECK(first.Instance == second.Instance);
    CHECK(first.Object == object);
    CHECK(second.Object == object);
    CHECK(factory.Creates.load() == 1);
    CHECK(registry.Count() == 1);
}

TEST_CASE("Cooked object loader resolves persistent GUID entries through runtime catalog")
{
    const Guid object(103, 0, 0, 0);
    RuntimeAssetCatalogEntry entry;
    entry.Object = object;
    entry.TypeName = "FlaxEngine.Texture";
    entry.PackageName = "base/objects.pak";
    entry.Offset = 128;
    entry.Size = 64;
    entry.Content = LoadingTestHash("runtime-object");
    Array<RuntimeAssetCatalogEntry> entries;
    entries.Add(entry);
    RuntimeAssetCatalog catalog;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(catalog.Set(StringAnsiView("runtime-build"), LoadingTestHash("target"), entries, diagnostic));
    RuntimeCatalogAssetObjectResolver runtimeResolver(catalog, 19);
    TestObjectResolver editorResolver;
    TestObjectFactory factory;
    LoadedAssetRegistry registry;
    AssetObjectLoader loader(registry, runtimeResolver, factory);
    AssetObjectLoadResult result;
    REQUIRE_FALSE(loader.Load(object, result, diagnostic));
    CHECK(result.Object == object);
    CHECK(result.Revision == 19);
    CHECK(result.State == LoadedAssetState::Loaded);
    CHECK(factory.LastStorage == TEXT("base/objects.pak"));
    CHECK(factory.LastStorageObject == AssetObjectId::Main(AssetGuid(object)));
    CHECK(editorResolver.Calls.load() == 0);
}

TEST_CASE("Cooked player loads exact main and subasset GUIDs after registry reconstruction")
{
    const Guid mainObject(115, 0, 0, 1);
    const Guid subObject(115, 0, 0, 2);
    RuntimeAssetCatalogEntry mainEntry;
    mainEntry.Object = mainObject;
    mainEntry.TypeName = "FlaxEngine.Model";
    mainEntry.PackageName = "base/model-objects.pak";
    mainEntry.Offset = 64;
    mainEntry.Size = 32;
    mainEntry.Content = LoadingTestHash("player-main");
    RuntimeAssetCatalogEntry subEntry = mainEntry;
    subEntry.Object = subObject;
    subEntry.TypeName = "FlaxEngine.Material";
    subEntry.Offset = 128;
    subEntry.Content = LoadingTestHash("player-subasset");
    Array<RuntimeAssetCatalogEntry> entries;
    entries.Add(mainEntry);
    entries.Add(subEntry);
    RuntimeAssetCatalog catalog;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(catalog.Set(StringAnsiView("runtime-restart"), LoadingTestHash("player-target"), entries, diagnostic));
    RuntimeCatalogAssetObjectResolver resolver(catalog, 23);
    TestObjectFactory factory;

    LoadedAssetRegistry firstRegistry;
    AssetObjectLoader firstLoader(firstRegistry, resolver, factory);
    AssetObjectLoadResult mainResult;
    AssetObjectLoadResult subResult;
    REQUIRE_FALSE(firstLoader.Load(mainObject, mainResult, diagnostic));
    CHECK(factory.LastStorageObject == AssetObjectId::Main(AssetGuid(mainObject)));
    REQUIRE_FALSE(firstLoader.Load(subObject, subResult, diagnostic));
    CHECK(factory.LastStorageObject == AssetObjectId::Main(AssetGuid(subObject)));
    CHECK(mainResult.Object == mainObject);
    CHECK(subResult.Object == subObject);
    CHECK(mainResult.Instance != subResult.Instance);
    CHECK(mainResult.Revision == 23);
    CHECK(subResult.Revision == 23);
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::None);

    LoadedAssetRegistry restartedRegistry;
    AssetObjectLoader restartedLoader(restartedRegistry, resolver, factory);
    AssetObjectLoadResult restartedSub;
    REQUIRE_FALSE(restartedLoader.Load(subObject, restartedSub, diagnostic));
    CHECK(restartedSub.Object == subObject);
    CHECK(restartedSub.State == LoadedAssetState::Loaded);
    CHECK(restartedSub.Revision == 23);

    const Guid missing(115, 0, 0, 3);
    AssetObjectLoadResult unresolved;
    CHECK(restartedLoader.Load(missing, unresolved, diagnostic));
    CHECK(unresolved.Object == missing);
    CHECK(unresolved.State == LoadedAssetState::Unresolved);
    CHECK(unresolved.Instance == nullptr);
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactMissing);
}

TEST_CASE("Cold cooked player bootstraps GameSettings and packaged scene without Editor sources")
{
    const Guid gameSettings(116, 0, 0, 1);
    const Guid scene(116, 0, 0, 2);
    RuntimeAssetCatalogEntry settingsEntry;
    settingsEntry.Object = gameSettings;
    settingsEntry.TypeName = "FlaxEditor.Content.Settings.GameSettings";
    settingsEntry.PackageName = "Data_0.flaxpac";
    settingsEntry.Size = 48;
    settingsEntry.Content = LoadingTestHash("cooked-game-settings");
    settingsEntry.Dependencies.Add(scene);
    RuntimeAssetCatalogEntry sceneEntry;
    sceneEntry.Object = scene;
    sceneEntry.TypeName = "FlaxEngine.SceneAsset";
    sceneEntry.PackageName = "Data_0.flaxpac";
    sceneEntry.Size = 96;
    sceneEntry.Content = LoadingTestHash("cooked-scene");
    Array<RuntimeAssetCatalogEntry> entries;
    entries.Add(settingsEntry);
    entries.Add(sceneEntry);

    AssetPipelineDiagnostic diagnostic;
    RuntimeAssetCatalog cookedCatalog;
    REQUIRE_FALSE(cookedCatalog.Set(StringAnsiView("player-cold-start"), LoadingTestHash("player-target"), entries, diagnostic));
    cookedCatalog.SetGameSettingsObject(gameSettings);
    Array<byte> catalogBytes;
    REQUIRE_FALSE(cookedCatalog.ToBytes(catalogBytes, diagnostic));
    CHECK_FALSE(ContainsAscii(catalogBytes, ".meta"));
    CHECK_FALSE(ContainsAscii(catalogBytes, "Content/"));
    CHECK_FALSE(ContainsAscii(catalogBytes, "Migration"));
    CHECK_FALSE(ContainsAscii(catalogBytes, "Thumbnail"));

    RuntimeAssetCatalog playerCatalog;
    REQUIRE_FALSE(RuntimeAssetCatalog::FromBytes(Span<byte>(catalogBytes.Get(), catalogBytes.Count()), playerCatalog, diagnostic));
    REQUIRE(playerCatalog.GetGameSettingsObject() == gameSettings);
    RuntimeCatalogAssetObjectResolver runtimeResolver(playerCatalog, 1);
    TestObjectResolver poisonEditorResolver;
    poisonEditorResolver.FailResolution = true;
    TestObjectFactory factory;
    LoadedAssetRegistry registry;
    AssetObjectLoader playerLoader(registry, static_cast<IRuntimeAssetObjectResolver&>(runtimeResolver), factory);
    CHECK_FALSE(playerLoader.AllowsStaleContinuity());

    AssetObjectLoadResult settingsResult;
    REQUIRE_FALSE(playerLoader.Load(playerCatalog.GetGameSettingsObject(), settingsResult, diagnostic));
    CHECK(settingsResult.Object == gameSettings);
    CHECK(factory.LastStorage == TEXT("Data_0.flaxpac"));
    CHECK(factory.LastSource == factory.LastStorage);
    CHECK(factory.LastStorageKind == AssetObjectStorageKind::RuntimePackage);
    REQUIRE(factory.LastDependencies.Count() == 1);
    CHECK(factory.LastDependencies[0] == scene);

    AssetObjectLoadResult sceneResult;
    REQUIRE_FALSE(playerLoader.Load(scene, sceneResult, diagnostic));
    CHECK(sceneResult.Object == scene);
    CHECK(factory.LastStorage == TEXT("Data_0.flaxpac"));
    CHECK(factory.LastSource == factory.LastStorage);
    CHECK(factory.LastStorageObject == AssetObjectId::Main(AssetGuid(scene)));
    CHECK(factory.LastStorageKind == AssetObjectStorageKind::RuntimePackage);
    CHECK(poisonEditorResolver.Calls.load() == 0);
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::None);
}

TEST_CASE("Object loader rematerializes an unloaded registry instance")
{
    const Guid object(106, 0, 0, 0);
    LoadedAssetRegistry registry;
    TestObjectResolver resolver;
    resolver.Set(TestLocation(object, 1));
    TestObjectFactory factory;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetObjectLoadResult first;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(loader.Load(object, first, diagnostic));
    REQUIRE_FALSE(registry.Remove(object, first.Instance));
    CHECK(registry.Count() == 0);

    AssetObjectLoadResult second;
    REQUIRE_FALSE(loader.Load(object, second, diagnostic));
    CHECK(second.Instance != first.Instance);
    CHECK(factory.Creates.load() == 2);
}

TEST_CASE("Object loader keys exact subassets and rematerialization by persistent GUID")
{
    const Guid source(107, 0, 0, 0);
    const Guid firstObject(107, 0, 0, 11);
    const Guid secondObject(107, 0, 0, 12);
    const AssetObjectId firstStorage(AssetGuid(source), 11);
    const AssetObjectId secondStorage(AssetGuid(source), 12);
    LoadedAssetRegistry registry;
    TestObjectResolver resolver;
    resolver.Set(TestLocation(firstObject, 4, firstStorage));
    resolver.Set(TestLocation(secondObject, 4, secondStorage));
    TestObjectFactory factory;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetPipelineDiagnostic diagnostic;
    AssetObjectLoadResult first;
    AssetObjectLoadResult second;
    REQUIRE_FALSE(loader.Load(firstObject, first, diagnostic));
    REQUIRE_FALSE(loader.Load(secondObject, second, diagnostic));
    CHECK(first.Object == firstObject);
    CHECK(second.Object == secondObject);
    CHECK(first.Instance != second.Instance);
    CHECK(registry.Count() == 2);

    REQUIRE_FALSE(registry.Remove(firstObject, first.Instance));
    AssetObjectLoadResult reloaded;
    REQUIRE_FALSE(loader.Load(firstObject, reloaded, diagnostic));
    CHECK(reloaded.Object == firstObject);
    CHECK(reloaded.Instance != first.Instance);
    CHECK(registry.Count() == 2);
    CHECK(factory.Creates.load() == 3);
}

TEST_CASE("Persistent GUID loads survive registry reconstruction without changing storage identity")
{
    const Guid source(114, 0, 0, 0);
    const Guid mainObject(114, 0, 0, 1);
    const Guid subObject(114, 0, 0, 2);
    const AssetObjectId mainStorage(AssetGuid(source), 1);
    const AssetObjectId subStorage(AssetGuid(source), 42);
    TestObjectResolver resolver;
    resolver.Set(TestLocation(mainObject, 7, mainStorage));
    resolver.Set(TestLocation(subObject, 7, subStorage));
    TestObjectFactory factory;
    AssetPipelineDiagnostic diagnostic;

    {
        LoadedAssetRegistry firstRegistry;
        AssetObjectLoader firstLoader(firstRegistry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
        AssetObjectLoadResult mainResult;
        AssetObjectLoadResult subResult;
        REQUIRE_FALSE(firstLoader.Load(mainObject, mainResult, diagnostic));
        CHECK(factory.LastStorageObject == mainStorage);
        REQUIRE_FALSE(firstLoader.Load(subObject, subResult, diagnostic));
        CHECK(factory.LastStorageObject == subStorage);
        CHECK(mainResult.Object == mainObject);
        CHECK(subResult.Object == subObject);
        CHECK(mainResult.Instance != subResult.Instance);
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::None);
    }

    LoadedAssetRegistry restartedRegistry;
    AssetObjectLoader restartedLoader(restartedRegistry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetObjectLoadResult restartedMain;
    AssetObjectLoadResult restartedSub;
    REQUIRE_FALSE(restartedLoader.Load(mainObject, restartedMain, diagnostic));
    CHECK(factory.LastStorageObject == mainStorage);
    REQUIRE_FALSE(restartedLoader.Load(subObject, restartedSub, diagnostic));
    CHECK(factory.LastStorageObject == subStorage);
    CHECK(restartedMain.Object == mainObject);
    CHECK(restartedSub.Object == subObject);
    CHECK(restartedMain.Revision == 7);
    CHECK(restartedSub.Revision == 7);
    CHECK(restartedMain.Instance != restartedSub.Instance);
    CHECK(restartedRegistry.Count() == 2);
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::None);
}

TEST_CASE("Hot reload publishes atomically and notifies dependencies first")
{
    const Guid owner(104, 0, 0, 2);
    const Guid dependency(104, 0, 0, 3);
    TestObjectResolver resolver;
    AssetObjectLoadLocation ownerLocation = TestLocation(owner, 1);
    ownerLocation.Dependencies.Add(dependency);
    resolver.Set(ownerLocation);
    resolver.Set(TestLocation(dependency, 1));
    TestObjectFactory factory;
    LoadedAssetRegistry registry;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetPipelineDiagnostic diagnostic;
    AssetObjectLoadResult loaded;
    REQUIRE_FALSE(loader.Load(owner, loaded, diagnostic));
    REQUIRE_FALSE(loader.Load(dependency, loaded, diagnostic));

    ownerLocation.Revision = 2;
    resolver.Set(ownerLocation);
    resolver.Set(TestLocation(dependency, 2));
    Array<AssetObjectRevision> changes;
    changes.Add({owner, 2});
    changes.Add({dependency, 2});
    TestMainThreadDispatcher dispatcher;
    TestReloadListener listener;
    listener.Registry = &registry;
    AssetHotReloadCoordinator coordinator(registry, loader, dispatcher, listener);
    REQUIRE_FALSE(coordinator.Reload(changes, diagnostic));
    REQUIRE(listener.Objects.Count() == 2);
    CHECK(listener.Objects[0] == dependency);
    CHECK(listener.Objects[1] == owner);
    CHECK(listener.PreviousRevisions[0] == 1);
    CHECK(listener.Revisions[0] == 2);
    CHECK(factory.Destroys.load() == 2);
    CHECK(dispatcher.Calls == 1);

    resolver.Set(TestLocation(owner, 2));
    resolver.Set(TestLocation(dependency, 3));
    changes.Clear();
    changes.Add({owner, 2});
    changes.Add({dependency, 3});
    CHECK(coordinator.Reload(changes, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
    LoadedAssetRecord record;
    REQUIRE(registry.TryGet(dependency, record));
    CHECK(record.Revision == 2);
    CHECK(listener.Objects.Count() == 2);
    CHECK(factory.Destroys.load() == 4);
}

TEST_CASE("Hot reload replaces payload and concrete type under the persistent GUID")
{
    const Guid object(108, 0, 0, 0);
    LoadedAssetRegistry registry;
    TestObjectResolver resolver;
    AssetObjectLoadLocation location = TestLocation(object, 1);
    const ContentHash previousContent = location.Content;
    resolver.Set(location);
    TestObjectFactory factory;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetPipelineDiagnostic diagnostic;
    AssetObjectLoadResult loaded;
    REQUIRE_FALSE(loader.Load(object, loaded, diagnostic));
    void* previousInstance = loaded.Instance;

    location.Revision = 2;
    location.TypeName = "FlaxEngine.Material";
    location.Content = LoadingTestHash("changed-object-content");
    resolver.Set(location);
    Array<AssetObjectRevision> changes;
    changes.Add({object, 2});
    TestMainThreadDispatcher dispatcher;
    TestReloadListener listener;
    listener.Registry = &registry;
    AssetHotReloadCoordinator coordinator(registry, loader, dispatcher, listener);
    REQUIRE_FALSE(coordinator.Reload(changes, diagnostic));

    LoadedAssetRecord record;
    REQUIRE(registry.TryGet(object, record));
    CHECK(record.Object == object);
    CHECK(record.Instance != previousInstance);
    CHECK(record.TypeName == location.TypeName);
    CHECK(record.Content == location.Content);
    CHECK(record.Revision == 2);
    REQUIRE(listener.Objects.Count() == 1);
    CHECK(listener.Objects[0] == object);
    CHECK(listener.PreviousTypeNames[0] == "FlaxEngine.Texture");
    CHECK(listener.TypeNames[0] == "FlaxEngine.Material");
    CHECK(listener.PreviousContents[0] == previousContent);
    CHECK(listener.Contents[0] == location.Content);
    REQUIRE(listener.PublishedRecords.Count() == 1);
    CHECK(listener.PublishedRecords[0].Instance == record.Instance);
    CHECK(listener.PublishedRecords[0].TypeName == location.TypeName);
    CHECK(listener.PublishedRecords[0].Content == location.Content);
    CHECK(factory.Destroys.load() == 1);
    CHECK(dispatcher.Calls == 1);
}

TEST_CASE("Hot reload treats subasset reorder as identity preserving")
{
    const Guid source(109, 0, 0, 0);
    const Guid mainObject(109, 0, 0, 1);
    const Guid childA(109, 0, 0, 2);
    const Guid childB(109, 0, 0, 3);
    LoadedAssetRegistry registry;
    TestObjectResolver resolver;
    TestObjectFactory factory;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    TestMainThreadDispatcher dispatcher;
    TestReloadListener listener;
    AssetHotReloadCoordinator coordinator(registry, loader, dispatcher, listener);
    AssetObjectInventoryChange change;
    change.Source = source;
    change.PreviousMainObject = mainObject;
    change.MainObject = mainObject;
    change.PreviousObjects.Add(mainObject);
    change.PreviousObjects.Add(childA);
    change.PreviousObjects.Add(childB);
    change.Objects.Add(childB);
    change.Objects.Add(mainObject);
    change.Objects.Add(childA);
    AssetPipelineDiagnostic diagnostic;
    CHECK_FALSE(coordinator.ReloadInventory(change, diagnostic));
    CHECK(dispatcher.Calls == 0);
    CHECK(factory.Creates.load() == 0);
    CHECK(listener.Objects.IsEmpty());
    CHECK(listener.InvalidatedObjects.IsEmpty());
}

TEST_CASE("Hot reload publishes subasset removal and main-object changes atomically")
{
    const Guid source(110, 0, 0, 0);
    const Guid previousMain(110, 0, 0, 1);
    const Guid mainObject(110, 0, 0, 2);
    const Guid removed(110, 0, 0, 3);
    const Guid retained(110, 0, 0, 4);
    const Guid added(110, 0, 0, 5);
    const Guid dependent(111, 0, 0, 1);
    LoadedAssetRegistry registry;
    TestObjectResolver resolver;
    resolver.Set(TestLocation(previousMain, 1));
    resolver.Set(TestLocation(mainObject, 1));
    resolver.Set(TestLocation(removed, 1));
    resolver.Set(TestLocation(retained, 1));
    AssetObjectLoadLocation dependentLocation = TestLocation(dependent, 1);
    dependentLocation.Dependencies.Add(removed);
    resolver.Set(dependentLocation);
    TestObjectFactory factory;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetPipelineDiagnostic diagnostic;
    AssetObjectLoadResult loaded;
    REQUIRE_FALSE(loader.Load(previousMain, loaded, diagnostic));
    REQUIRE_FALSE(loader.Load(mainObject, loaded, diagnostic));
    REQUIRE_FALSE(loader.Load(removed, loaded, diagnostic));
    REQUIRE_FALSE(loader.Load(retained, loaded, diagnostic));
    void* retainedInstance = loaded.Instance;
    REQUIRE_FALSE(loader.Load(dependent, loaded, diagnostic));

    resolver.Set(TestLocation(previousMain, 2));
    resolver.Set(TestLocation(mainObject, 2));
    dependentLocation.Revision = 2;
    resolver.Set(dependentLocation);
    AssetObjectInventoryChange change;
    change.Source = source;
    change.PreviousMainObject = previousMain;
    change.MainObject = mainObject;
    change.PreviousObjects.Add(previousMain);
    change.PreviousObjects.Add(mainObject);
    change.PreviousObjects.Add(removed);
    change.PreviousObjects.Add(retained);
    change.Objects.Add(added);
    change.Objects.Add(retained);
    change.Objects.Add(mainObject);
    change.Objects.Add(previousMain);
    change.Revisions.Add({previousMain, 2});
    change.Revisions.Add({mainObject, 2});
    change.Revisions.Add({dependent, 2});
    TestMainThreadDispatcher dispatcher;
    TestReloadListener listener;
    listener.Registry = &registry;
    listener.ObserveAtomicInventory = true;
    listener.ExpectedPublishedObjects.Add(previousMain);
    listener.ExpectedPublishedObjects.Add(mainObject);
    listener.ExpectedPublishedObjects.Add(dependent);
    AssetHotReloadCoordinator coordinator(registry, loader, dispatcher, listener);
    REQUIRE_FALSE(coordinator.ReloadInventory(change, diagnostic));

    REQUIRE(listener.InvalidatedObjects.Count() == 1);
    CHECK(listener.InvalidatedObjects[0] == removed);
    CHECK(listener.Objects.Count() == 3);
    LoadedAssetRecord record;
    REQUIRE(registry.TryGet(removed, record));
    CHECK(record.State == LoadedAssetState::Deleted);
    CHECK(record.Instance == nullptr);
    CHECK(record.StaleInstance != nullptr);
    CHECK_FALSE(registry.TryGet(added, record));
    REQUIRE(registry.TryGet(retained, record));
    CHECK(record.Instance == retainedInstance);
    CHECK(record.Revision == 1);
    REQUIRE(registry.TryGet(dependent, record));
    CHECK(record.Revision == 2);
    CHECK(dispatcher.Calls == 1);
    CHECK(factory.Destroys.load() == 3);
}

TEST_CASE("Import failure is current and last-good loaded data is explicitly stale")
{
    const Guid failed(112, 0, 0, 1);
    const Guid dependent(112, 0, 0, 2);
    const Guid unrelated(112, 0, 0, 3);
    LoadedAssetRegistry registry;
    TestObjectResolver resolver;
    resolver.Set(TestLocation(failed, 1));
    AssetObjectLoadLocation dependentLocation = TestLocation(dependent, 1);
    dependentLocation.Dependencies.Add(failed);
    resolver.Set(dependentLocation);
    resolver.Set(TestLocation(unrelated, 1));
    TestObjectFactory factory;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetPipelineDiagnostic diagnostic;
    AssetObjectLoadResult loaded;
    REQUIRE_FALSE(loader.Load(failed, loaded, diagnostic));
    void* failedLastGood = loaded.Instance;
    REQUIRE_FALSE(loader.Load(dependent, loaded, diagnostic));
    void* dependentLastGood = loaded.Instance;
    REQUIRE_FALSE(loader.Load(unrelated, loaded, diagnostic));

    TestMainThreadDispatcher dispatcher;
    TestReloadListener listener;
    listener.Registry = &registry;
    AssetHotReloadCoordinator coordinator(registry, loader, dispatcher, listener);
    AssetPipelineDiagnostic importFailure;
    importFailure.Code = AssetPipelineDiagnosticCode::BuildFailed;
    importFailure.Stage = AssetPipelineDiagnosticStage::Build;
    importFailure.Message = TEXT("Injected import failure.");
    Array<Guid> failedObjects;
    failedObjects.Add(failed);
    REQUIRE_FALSE(coordinator.HandleImportFailure(failedObjects, importFailure, diagnostic));

    REQUIRE(listener.InvalidatedObjects.Count() == 2);
    CHECK(listener.InvalidatedObjects[0] == failed);
    CHECK(listener.InvalidatedObjects[1] == dependent);
    CHECK(listener.InvalidatedStates[0] == LoadedAssetState::Failed);
    CHECK(listener.InvalidatedStates[1] == LoadedAssetState::Failed);
    LoadedAssetRecord record;
    REQUIRE(registry.TryGet(failed, record));
    CHECK(record.State == LoadedAssetState::Failed);
    CHECK(record.Instance == nullptr);
    CHECK(record.StaleInstance == failedLastGood);
    CHECK(record.StaleRevision == 1);
    CHECK(record.Diagnostic.Code == AssetPipelineDiagnosticCode::BuildFailed);
    REQUIRE(registry.TryGet(dependent, record));
    CHECK(record.State == LoadedAssetState::Failed);
    CHECK(record.Instance == nullptr);
    CHECK(record.StaleInstance == dependentLastGood);
    CHECK(record.Diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactRebuildRequired);
    REQUIRE(record.Diagnostic.Related.Count() == 1);
    CHECK(record.Diagnostic.Related[0] == failed.ToString());
    REQUIRE(registry.TryGet(unrelated, record));
    CHECK(record.State == LoadedAssetState::Loaded);

    const int32 resolutionCalls = resolver.Calls.load();
    CHECK(loader.Load(failed, loaded, diagnostic));
    CHECK(loaded.Object == failed);
    CHECK(loaded.State == LoadedAssetState::Failed);
    CHECK(loaded.Instance == nullptr);
    CHECK(resolver.Calls.load() == resolutionCalls);

    resolver.Set(TestLocation(failed, 2));
    dependentLocation.Revision = 2;
    resolver.Set(dependentLocation);
    Array<AssetObjectRevision> recovery;
    recovery.Add({dependent, 2});
    recovery.Add({failed, 2});
    REQUIRE_FALSE(coordinator.Reload(recovery, diagnostic));
    REQUIRE(registry.TryGet(failed, record));
    CHECK(record.State == LoadedAssetState::Loaded);
    CHECK(record.StaleInstance == nullptr);
    REQUIRE(registry.TryGet(dependent, record));
    CHECK(record.State == LoadedAssetState::Loaded);
    CHECK(record.StaleInstance == nullptr);
    CHECK(factory.Destroys.load() == 2);
}

TEST_CASE("Source deletion preserves unresolved GUIDs and fails only exact loaded dependents")
{
    const Guid deleted(113, 0, 0, 1);
    const Guid dependent(113, 0, 0, 2);
    const Guid unrelated(113, 0, 0, 3);
    LoadedAssetRegistry registry;
    TestObjectResolver resolver;
    resolver.Set(TestLocation(deleted, 1));
    AssetObjectLoadLocation dependentLocation = TestLocation(dependent, 1);
    dependentLocation.Dependencies.Add(deleted);
    resolver.Set(dependentLocation);
    resolver.Set(TestLocation(unrelated, 1));
    TestObjectFactory factory;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetPipelineDiagnostic diagnostic;
    AssetObjectLoadResult loaded;
    REQUIRE_FALSE(loader.Load(deleted, loaded, diagnostic));
    void* deletedLastGood = loaded.Instance;
    REQUIRE_FALSE(loader.Load(dependent, loaded, diagnostic));
    REQUIRE_FALSE(loader.Load(unrelated, loaded, diagnostic));

    TestMainThreadDispatcher dispatcher;
    TestReloadListener listener;
    listener.Registry = &registry;
    AssetHotReloadCoordinator coordinator(registry, loader, dispatcher, listener);
    Array<Guid> deletedObjects;
    deletedObjects.Add(deleted);
    REQUIRE_FALSE(coordinator.HandleSourceDeletion(deletedObjects, diagnostic));

    REQUIRE(listener.InvalidatedObjects.Count() == 2);
    CHECK(listener.InvalidatedObjects[0] == deleted);
    CHECK(listener.InvalidatedObjects[1] == dependent);
    CHECK(listener.InvalidatedStates[0] == LoadedAssetState::Deleted);
    CHECK(listener.InvalidatedStates[1] == LoadedAssetState::Failed);
    LoadedAssetRecord record;
    REQUIRE(registry.TryGet(deleted, record));
    CHECK(record.Object == deleted);
    CHECK(record.State == LoadedAssetState::Deleted);
    CHECK(record.Instance == nullptr);
    CHECK(record.StaleInstance == deletedLastGood);
    CHECK(record.Diagnostic.Code == AssetPipelineDiagnosticCode::SourceMissing);
    REQUIRE(registry.TryGet(dependent, record));
    CHECK(record.Object == dependent);
    CHECK(record.State == LoadedAssetState::Failed);
    CHECK(record.Instance == nullptr);
    CHECK(record.Diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactRebuildRequired);
    REQUIRE(record.Diagnostic.Related.Count() == 1);
    CHECK(record.Diagnostic.Related[0] == deleted.ToString());
    REQUIRE(registry.TryGet(unrelated, record));
    CHECK(record.State == LoadedAssetState::Loaded);
    CHECK(factory.Destroys.load() == 0);
}

TEST_CASE("Unresolved GUID observes deletion and recovers when the same GUID is republished")
{
    const Guid object(116, 0, 0, 1);
    LoadedAssetRegistry registry;
    TestObjectResolver resolver;
    TestObjectFactory factory;
    AssetObjectLoader loader(registry, static_cast<IEditorAssetObjectResolver&>(resolver), factory);
    AssetPipelineDiagnostic diagnostic;
    AssetObjectLoadResult result;
    CHECK(loader.Load(object, result, diagnostic));
    CHECK(result.Object == object);
    CHECK(result.State == LoadedAssetState::Unresolved);
    CHECK(result.Instance == nullptr);

    TestMainThreadDispatcher dispatcher;
    TestReloadListener listener;
    listener.Registry = &registry;
    AssetHotReloadCoordinator coordinator(registry, loader, dispatcher, listener);
    Array<Guid> deletedObjects;
    deletedObjects.Add(object);
    REQUIRE_FALSE(coordinator.HandleSourceDeletion(deletedObjects, diagnostic));
    REQUIRE(listener.InvalidatedObjects.Count() == 1);
    CHECK(listener.InvalidatedObjects[0] == object);
    CHECK(listener.InvalidatedStates[0] == LoadedAssetState::Deleted);
    LoadedAssetRecord record;
    REQUIRE(registry.TryGet(object, record));
    CHECK(record.Object == object);
    CHECK(record.State == LoadedAssetState::Deleted);
    CHECK(record.Instance == nullptr);
    CHECK(record.StaleInstance == nullptr);
    CHECK(record.Diagnostic.Code == AssetPipelineDiagnosticCode::SourceMissing);

    const int32 resolutionCalls = resolver.Calls.load();
    CHECK(loader.Load(object, result, diagnostic));
    CHECK(result.Object == object);
    CHECK(result.State == LoadedAssetState::Deleted);
    CHECK(result.Instance == nullptr);
    CHECK(resolver.Calls.load() == resolutionCalls);

    resolver.Set(TestLocation(object, 2));
    Array<AssetObjectRevision> recovery;
    recovery.Add({object, 2});
    REQUIRE_FALSE(coordinator.Reload(recovery, diagnostic));
    REQUIRE(registry.TryGet(object, record));
    CHECK(record.Object == object);
    CHECK(record.State == LoadedAssetState::Loaded);
    CHECK(record.Instance != nullptr);
    CHECK(record.StaleInstance == nullptr);
    CHECK(record.Revision == 2);
    CHECK(record.Diagnostic.Code == AssetPipelineDiagnosticCode::None);
    REQUIRE(listener.Objects.Count() == 1);
    CHECK(listener.Objects[0] == object);
    CHECK(listener.PreviousRevisions[0] == 0);
    CHECK(listener.Revisions[0] == 2);
    CHECK(dispatcher.Calls == 2);
}
