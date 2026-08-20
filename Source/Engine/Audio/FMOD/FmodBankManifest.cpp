// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodBankManifest.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Serialization/Json.h"

bool FmodBankManifest::ResolveBank(const StringView& requestedPath, String& resolvedPath)
{
    resolvedPath.Clear();
    String requested(requestedPath);
    requested.Replace(TEXT("\\"), TEXT("/"));
    while (requested.StartsWith(TEXT("/")))
        requested = requested.Right(requested.Length() - 1);

    const String audioRoot = Globals::StartupFolder / TEXT("Audio");
    const String manifestPath = audioRoot / TEXT("AudioCookManifest.json");
    StringAnsi json;
    if (File::ReadAllText(manifestPath, json))
        return false;

    rapidjson_flax::Document document;
    document.Parse(json.Get(), json.Length());
    if (document.HasParseError() || !document.IsObject())
        return false;
    const auto schema = document.FindMember("schema");
    const auto banks = document.FindMember("banks");
    if (schema == document.MemberEnd() || !schema->value.IsInt() || schema->value.GetInt() != 2 ||
        banks == document.MemberEnd() || !banks->value.IsArray())
        return false;

    for (auto it = banks->value.Begin(); it != banks->value.End(); ++it)
    {
        if (!it->IsObject())
            continue;
        const auto file = it->FindMember("file");
        if (file == it->MemberEnd() || !file->value.IsString())
            continue;
        String manifestFile(file->value.GetString());
        manifestFile.Replace(TEXT("\\"), TEXT("/"));
        String requestedSuffix(TEXT("/"));
        requestedSuffix += requested;
        if (manifestFile != requested && !manifestFile.EndsWith(requestedSuffix))
            continue;
        const String candidate = audioRoot / manifestFile;
        if (FileSystem::FileExists(candidate))
        {
            resolvedPath = candidate;
            return true;
        }
        return false;
    }
    return false;
}
