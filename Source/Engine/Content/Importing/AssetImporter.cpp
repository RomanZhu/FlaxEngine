// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImporter.h"

AssetImporterDescriptor AssetImporterDescriptor::FromBuildImplementation(const AssetProcessorDescriptor& processor, int32 priority)
{
    AssetImporterDescriptor result;
    result.ID = processor.ID;
    result.ProviderID = processor.ProviderID;
    result.ImporterVersion = processor.ImplementationVersion;
    result.SettingsSchemaVersion = processor.SettingsSchemaVersion;
    result.ImplementationHash = processor.ProviderSemanticIdentity;
    result.ProviderKind = processor.ProviderKind;
    if (result.ImplementationHash.IsZero())
    {
        const StringAnsi identity = StringAnsi(processor.ID) + ":" + StringAnsi::Format("{0}", processor.ImplementationVersion);
        result.ImplementationHash = ContentHash::Compute(identity.Get(), identity.Length());
    }
    result.Extensions = processor.SourceExtensions;
    result.Priority = priority;
    result.ProducesMainObject = !processor.MainOutputType.IsEmpty();
    result.ProducesSubObjects = processor.SupportsSubAssets;
    result.SupportsParallelImport = processor.PrepareAffinity == AssetProcessorThreadAffinity::AnyWorker && processor.BuildAffinity == AssetProcessorThreadAffinity::AnyWorker;
    result.ProcessSafe = processor.ProviderKind == AssetProcessorProviderKind::Native &&
        processor.TrustMode == AssetProcessorTrustMode::IsolatedProcess && result.SupportsParallelImport;
    result.RequiresMainThread = !result.SupportsParallelImport;
    result.PathSensitive = processor.IsPathSensitive;
    result.MaximumMemoryBytes = processor.MemoryEstimate > 64ull * 1024ull * 1024ull
        ? processor.MemoryEstimate
        : 64ull * 1024ull * 1024ull;
    result.Processor = processor;
    return result;
}
