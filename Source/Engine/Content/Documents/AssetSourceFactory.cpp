// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetSourceFactory.h"
#include "GraphDocument.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Level/ScenePrefabDocument.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Utilities/StringConverter.h"

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    class JsonAssetSourceFactory final : public AssetSourceFactory
    {
        String _extension;

    public:
        explicit JsonAssetSourceFactory(const StringView& extension)
            : _extension(extension)
        {
        }

        StringView GetDefaultExtension() const override
        {
            return _extension;
        }

        bool WriteInitialSource(SourceDocumentWriter& output, const AssetCreationParameters& parameters,
            AssetPipelineDiagnostic& diagnostic) const override
        {
            rapidjson_flax::Document payload;
            payload.Parse(parameters.Payload.Get(), parameters.Payload.Length());
            if (payload.HasParseError() || (!payload.IsObject() && !payload.IsArray()))
                return Fail(diagnostic, StringView::Empty, TEXT("Authored asset payload must be a JSON object or array."));
            if (parameters.TypeName.IsEmpty())
                return Fail(diagnostic, StringView::Empty, TEXT("Authored asset type is missing."));
            if ((_extension == TEXT("scene") || _extension == TEXT("prefab")) && !payload.IsArray())
                return Fail(diagnostic, StringView::Empty, TEXT("Scene and prefab object tables must be JSON arrays."));

            rapidjson_flax::StringBuffer buffer;
            PrettyJsonWriter writer(buffer);
            writer.StartObject();
            if (_extension == TEXT("scene"))
            {
                rapidjson_flax::Document converted;
                converted.SetArray();
                rapidjson_flax::Value objects;
                String error;
                if (ScenePrefabDocument::ToSourceObjects(payload, objects, converted.GetAllocator(), true, error))
                    return Fail(diagnostic, StringView::Empty, error);
                writer.JKEY("sceneVersion");
                writer.Uint(4);
                writer.JKEY("objects");
                objects.Accept(writer.GetWriter());
            }
            else if (_extension == TEXT("prefab"))
            {
                rapidjson_flax::Document converted;
                converted.SetArray();
                rapidjson_flax::Value objects;
                String error;
                if (ScenePrefabDocument::ToSourceObjects(payload, objects, converted.GetAllocator(), false, error))
                    return Fail(diagnostic, StringView::Empty, error);
                writer.JKEY("prefabVersion");
                writer.Uint(4);
                writer.JKEY("objects");
                objects.Accept(writer.GetWriter());
            }
            else
            {
                if (_extension == TEXT("settings"))
                    writer.JKEY("settingsVersion");
                else
                    writer.JKEY("documentVersion");
                writer.Uint(1);
                writer.JKEY("type");
                const StringAsUTF8<> typeName(parameters.TypeName.Get(), parameters.TypeName.Length());
                writer.String(typeName.Get(), typeName.Length());
                writer.JKEY("data");
                payload.Accept(writer.GetWriter());
            }
            writer.EndObject();
            output.Text.Set(buffer.GetString(), static_cast<int32>(buffer.GetSize()));
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
    };

    const AssetSourceFactory* FindFactory(const StringView& extension)
    {
        static JsonAssetSourceFactory settings(TEXT("settings"));
        static JsonAssetSourceFactory json(TEXT("json"));
        static JsonAssetSourceFactory scene(TEXT("scene"));
        static JsonAssetSourceFactory prefab(TEXT("prefab"));
        if (extension == TEXT("settings"))
            return &settings;
        if (extension == TEXT("json"))
            return &json;
        if (extension == TEXT("scene"))
            return &scene;
        if (extension == TEXT("prefab"))
            return &prefab;
        return nullptr;
    }
}

bool AssetSourceFactory::CreateOrReplace(const StringView& path, const AssetCreationParameters& parameters,
    AssetPipelineDiagnostic& diagnostic)
{
    const String extension = FileSystem::GetExtension(path).ToLower();
    const AssetSourceFactory* factory = FindFactory(extension);
    if (!factory)
        return Fail(diagnostic, path, TEXT("No source factory is registered for this authored asset extension."));
    if (!AssetPathPolicy::IsSameOrChild(path, Globals::ProjectContentFolder))
        return Fail(diagnostic, path, TEXT("Authored assets must be created under the project Content folder."));

    const String directory = StringUtils::GetDirectoryName(path);
    if (!FileSystem::DirectoryExists(directory) && FileSystem::CreateDirectory(directory))
        return Fail(diagnostic, path, TEXT("Cannot create the authored asset directory."));

    SourceDocumentWriter writer;
    if (factory->WriteInitialSource(writer, parameters, diagnostic))
    {
        diagnostic.SourcePath = path;
        return true;
    }
    if (GraphDocumentCodec::SaveJsonAtomic(path, writer.Text, diagnostic))
        return true;
    if (!AuthoredAssetDocumentService::CreateMetadata(path).IsValid())
        return Fail(diagnostic, path, TEXT("Cannot create authored asset metadata."));
    if (AssetPipelineService::ImportAsset(path, ImportAssetOptions::ForceUpdate | ImportAssetOptions::ForceSynchronousImport))
        return Fail(diagnostic, path, TEXT("Cannot import authored asset source document."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
