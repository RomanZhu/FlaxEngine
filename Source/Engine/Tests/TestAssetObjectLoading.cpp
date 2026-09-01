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

        bool CreateObject(const AssetObjectLoadLocation& location, void*& instance,
            AssetPipelineDiagnostic& diagnostic) override
        {
            const int32 number = Creates.fetch_add(1) + 1;
            LastStorage = location.StorageName;
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
            if (Registry && ObserveAtomicInventory)
            {
                LoadedAssetRecord record;
                CHECK_FALSE(Registry->TryGet(invalidation.Object, record));
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
    CHECK(editorResolver.Calls.load() == 0);
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
    CHECK_FALSE(registry.TryGet(removed, record));
    CHECK_FALSE(registry.TryGet(added, record));
    REQUIRE(registry.TryGet(retained, record));
    CHECK(record.Instance == retainedInstance);
    CHECK(record.Revision == 1);
    REQUIRE(registry.TryGet(dependent, record));
    CHECK(record.Revision == 2);
    CHECK(dispatcher.Calls == 1);
    CHECK(factory.Destroys.load() == 4);
}
