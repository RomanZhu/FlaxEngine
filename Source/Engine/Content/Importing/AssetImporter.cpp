// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImporter.h"

AssetImporterDescriptor AssetImporterDescriptor::FromProcessor(const AssetProcessorDescriptor& processor, int32 priority)
{
    AssetImporterDescriptor result;
    result.ID = processor.ID;
    result.ImporterVersion = processor.ImplementationVersion;
    result.ImplementationHash = processor.ProviderSemanticIdentity;
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
    result.RequiresMainThread = !result.SupportsParallelImport;
    result.PathSensitive = processor.IsPathSensitive;
    result.Processor = processor;
    return result;
}
