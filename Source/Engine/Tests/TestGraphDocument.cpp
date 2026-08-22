// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Documents/GraphDocument.h"
#include "Engine/Content/Assets/MaterialFunction.h"
#include "Engine/Content/Assets/VisualScript.h"
#include "Engine/Core/Math/Quaternion.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Core/Math/Vector2.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Visject/VisjectGraph.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    Array<byte> SaveSurface(VisjectGraph<>& graph)
    {
        MemoryWriteStream stream(512);
        REQUIRE_FALSE(graph.Save(&stream, true));
        Array<byte> bytes;
        bytes.Set(stream.GetHandle(), static_cast<int32>(stream.GetPosition()));
        return bytes;
    }
}

TEST_CASE("Graph documents round-trip Visject surfaces as canonical JSON")
{
    VisjectGraph<> graph;
    auto& output = graph.Nodes.AddOne();
    output.ID = 1;
    output.Type = GRAPH_NODE_MAKE_TYPE(16, 2);
    output.Values.Resize(2);
    output.Values[0] = TEXT("System.Single");
    output.Values[1] = TEXT("Output");
    auto& box = output.Boxes.AddOne();
    box.Parent = &output;
    box.ID = 0;
    box.Type = VariantType::Float;

    const Array<byte> surface = SaveSurface(graph);
    AssetPipelineDiagnostic diagnostic;
    StringAnsi json;
    REQUIRE_FALSE(GraphDocumentCodec::Encode(MaterialFunction::TypeName, ToSpan(surface), json, diagnostic));
    StringAnsi again;
    REQUIRE_FALSE(GraphDocumentCodec::Encode(MaterialFunction::TypeName, ToSpan(surface), again, diagnostic));
    CHECK(json == again);
    CHECK(json.Contains("\"documentVersion\": 1"));
    CHECK(json.Contains("node-00000001"));
    CHECK(json.Contains("box:0"));
    CHECK_FALSE(json.Contains("\"$type\": \"VariantBinary\""));

    GraphDocumentCodec codec;
    GraphDocumentSnapshot snapshot;
    REQUIRE_FALSE(codec.DecodeGraph(json, snapshot, diagnostic));
    CHECK(snapshot.TypeName == MaterialFunction::TypeName);
    CHECK(snapshot.Document.Nodes.Count() == 1);
    CHECK(snapshot.Document.Nodes[0].LegacyID == 1);
    CHECK(snapshot.CompatibilitySurface.Count() > 16);

    Array<byte> compiled;
    REQUIRE_FALSE(GraphDocumentCompiler::CompileDocument(snapshot.Document, compiled, diagnostic));
    VisjectGraph<> loaded;
    MemoryReadStream compiledStream(compiled.Get(), compiled.Count());
    REQUIRE_FALSE(loaded.Load(&compiledStream, true));
    REQUIRE(loaded.Nodes.Count() == 1);
    CHECK(loaded.Nodes[0].ID == 1);
    CHECK(loaded.Nodes[0].Type == GRAPH_NODE_MAKE_TYPE(16, 2));
    CHECK((StringView)loaded.Nodes[0].Values[1] == TEXT("Output"));

    GraphDocument roundTrip;
    REQUIRE_FALSE(GraphDocumentCodec::FromSurface(MaterialFunction::TypeName, ToSpan(compiled), roundTrip, diagnostic));
    CHECK(roundTrip.Nodes.Count() == 1);
    CHECK(roundTrip.Nodes[0].TypeID == 2);
    CHECK(roundTrip.Nodes[0].GroupID == 16);
    CHECK((StringView)roundTrip.Nodes[0].Values[1] == TEXT("Output"));
}

TEST_CASE("Graph documents preserve unknown nodes and ignore layout in the semantic hash")
{
    GraphDocument document;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(GraphDocumentCodec::CreateStarter(MaterialFunction::TypeName, document, diagnostic));
    REQUIRE(document.Nodes.Count() == 1);

    GraphDocumentParameter parameter;
    parameter.ID = Guid(1, 2, 3, 4);
    parameter.Name = TEXT("OldName");
    parameter.Type = VariantType::Float;
    parameter.Default = 1.0f;
    document.Parameters.Add(parameter);

    GraphDocumentNode unknown;
    unknown.LegacyID = 42;
    unknown.GroupID = 0xabcd;
    unknown.TypeID = 0x1234;
    unknown.Unknown = true;
    unknown.CustomJson = "{\n  \"pluginData\": {\n    \"keep\": true\n  }\n}\n";
    unknown.PositionX = 120.0f;
    unknown.PositionY = 80.0f;
    GraphDocumentPin pin;
    pin.BoxID = 0;
    pin.Type = VariantType::Float;
    unknown.Pins.Add(pin);
    document.Nodes.Add(unknown);

    StringAnsi first;
    REQUIRE_FALSE(GraphDocumentCodec::ToCanonicalJson(document, first, diagnostic));
    GraphDocumentCodec codec;
    GraphDocumentSnapshot snapshot;
    REQUIRE_FALSE(codec.DecodeGraph(first, snapshot, diagnostic));
    REQUIRE(snapshot.Document.Nodes.Count() == 2);
    CHECK(snapshot.Document.Nodes[1].Unknown);
    CHECK(snapshot.Document.Nodes[1].GroupID == 0xabcd);
    CHECK(snapshot.Document.Parameters.Count() == 1);
    CHECK(snapshot.Document.Parameters[0].ID == Guid(1, 2, 3, 4));
    CHECK(first.Contains("pluginData"));

    const ContentHash semantic = snapshot.SemanticHash;
    snapshot.Document.Nodes[1].PositionX = 999.0f;
    snapshot.Document.Nodes[1].PositionY = -12.0f;
    StringAnsi moved;
    REQUIRE_FALSE(GraphDocumentCodec::ToCanonicalJson(snapshot.Document, moved, diagnostic));
    GraphDocumentSnapshot movedSnapshot;
    REQUIRE_FALSE(codec.DecodeGraph(moved, movedSnapshot, diagnostic));
    CHECK(movedSnapshot.SemanticHash == semantic);
    CHECK(movedSnapshot.FullHash != snapshot.FullHash);

    snapshot.Document.Parameters[0].Name = TEXT("Renamed");
    StringAnsi renamed;
    REQUIRE_FALSE(GraphDocumentCodec::ToCanonicalJson(snapshot.Document, renamed, diagnostic));
    GraphDocumentSnapshot renamedSnapshot;
    REQUIRE_FALSE(codec.DecodeGraph(renamed, renamedSnapshot, diagnostic));
    CHECK(renamedSnapshot.Document.Parameters[0].ID == Guid(1, 2, 3, 4));
    CHECK(renamedSnapshot.Document.Parameters[0].Name == TEXT("Renamed"));
    CHECK(renamed.Contains("pluginData"));
}

TEST_CASE("Graph document migrations refuse silent upgrades and newer versions")
{
    GraphDocument document;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(GraphDocumentCodec::CreateStarter(MaterialFunction::TypeName, document, diagnostic));
    StringAnsi json;
    REQUIRE_FALSE(GraphDocumentCodec::ToCanonicalJson(document, json, diagnostic));

    GraphDocumentCodec codec;
    AssetDocumentSnapshot snapshot;
    REQUIRE_FALSE(codec.Decode(json, snapshot, diagnostic));
    GraphDocumentMigrator migrator;
    StringAnsi migrated;
    REQUIRE_FALSE(migrator.Migrate(snapshot, GraphDocumentCodec::CurrentDocumentVersion, migrated, diagnostic));
    CHECK(migrated == snapshot.CanonicalText);

    snapshot.DocumentVersion = GraphDocumentCodec::CurrentDocumentVersion + 1;
    CHECK(migrator.Migrate(snapshot, GraphDocumentCodec::CurrentDocumentVersion, migrated, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::MigrationFailed);
}

TEST_CASE("Graph validation reports dangling connections and unique identities")
{
    GraphDocument document;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(GraphDocumentCodec::CreateStarter(MaterialFunction::TypeName, document, diagnostic));
    GraphDocumentConnection dangling;
    dangling.FromNode = 1;
    dangling.FromPin = 0;
    dangling.ToNode = 99;
    dangling.ToPin = 0;
    document.Connections.Add(dangling);
    CHECK(GraphDocumentValidator::ValidateDocument(document, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
    CHECK(diagnostic.Location.GraphNode.Contains(TEXT("node-")));
}

TEST_CASE("Graph preview paths are excluded from current resolution")
{
    CHECK(GraphDocumentPreview::IsPreviewPath(TEXT("C:/Project/Library/Temp/Preview/abc/graph.flax")));
    CHECK_FALSE(GraphDocumentPreview::IsPreviewPath(TEXT("C:/Project/Content/Graphs/Test.materialfunction")));
}

TEST_CASE("Graph documents encode typed values and visject meta as text")
{
    GraphDocument document;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(GraphDocumentCodec::CreateStarter(MaterialFunction::TypeName, document, diagnostic));
    REQUIRE(document.Nodes.Count() == 1);

    document.Nodes[0].Values.Add(Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
    document.Nodes[0].Values.Add(Int2(3, 4));
    document.Nodes[0].Values.Add(Variant(Transform(Vector3(1.0f, 2.0f, 3.0f), Quaternion::Identity, Float3(1.0f, 1.0f, 1.0f))));
    Array<Variant> items;
    items.Add(5);
    items.Add(true);
    document.Nodes[0].Values.Add(Variant(items));

    float view[3] = { 12.0f, -8.0f, 1.5f };
    document.GraphMeta.AddEntry(10, reinterpret_cast<byte*>(view), sizeof(view));
    float layout[3] = { 40.0f, 60.0f, 0.0f };
    document.Nodes[0].Meta.AddEntry(11, reinterpret_cast<byte*>(layout), sizeof(layout));
    const uint16 attributes[] = { static_cast<uint16>('['), static_cast<uint16>(']') };
    document.Parameters.Resize(1);
    document.Parameters[0].ID = Guid(1, 2, 3, 4);
    document.Parameters[0].Name = TEXT("Tint");
    document.Parameters[0].Type = VariantType::Float;
    document.Parameters[0].Default = 1.0f;
    document.Parameters[0].Meta.AddEntry(13, const_cast<byte*>(reinterpret_cast<const byte*>(attributes)), sizeof(attributes));

    StringAnsi json;
    REQUIRE_FALSE(GraphDocumentCodec::ToCanonicalJson(document, json, diagnostic));
    CHECK_FALSE(json.Contains("\"$type\": \"VariantBinary\""));
    CHECK_FALSE(json.Contains("\"data\":"));
    CHECK(json.Contains("\"kind\": \"view\""));
    CHECK(json.Contains("\"kind\": \"layout\""));
    CHECK(json.Contains("\"kind\": \"attributes\""));
    CHECK(json.Contains("\"$type\": \"Quaternion\""));
    CHECK(json.Contains("\"$type\": \"Int2\""));
    CHECK(json.Contains("\"$type\": \"Transform\""));
    CHECK(json.Contains("\"$type\": \"Array\""));

    GraphDocumentCodec codec;
    GraphDocumentSnapshot snapshot;
    REQUIRE_FALSE(codec.DecodeGraph(json, snapshot, diagnostic));
    REQUIRE(snapshot.Document.Nodes.Count() == 1);
    REQUIRE(snapshot.Document.Nodes[0].Values.Count() >= 6);
    CHECK(snapshot.Document.Nodes[0].Values[2].Type.Type == VariantType::Quaternion);
    CHECK(snapshot.Document.Nodes[0].Values[3].AsInt2() == Int2(3, 4));
    CHECK(snapshot.Document.Nodes[0].Values[4].Type.Type == VariantType::Transform);
    CHECK(snapshot.Document.Nodes[0].Values[5].Type.Type == VariantType::Array);
    REQUIRE(snapshot.Document.GraphMeta.GetEntry(10) != nullptr);
    REQUIRE(snapshot.Document.Parameters.Count() == 1);
    REQUIRE(snapshot.Document.Parameters[0].Meta.GetEntry(13) != nullptr);
}

TEST_CASE("Graph documents support visual script, behavior tree, and particle function types")
{
    AssetPipelineDiagnostic diagnostic;
    CHECK(GraphDocumentCodec::IsSupportedType(VisualScript::TypeName));
    CHECK(GraphDocumentCodec::IsSupportedType(TEXT("FlaxEngine.BehaviorTree")));
    CHECK(GraphDocumentCodec::IsSupportedType(TEXT("FlaxEngine.ParticleEmitterFunction")));
    CHECK(StringView(GraphDocumentCodec::ExtensionForType(VisualScript::TypeName)) == TEXT(".visualscript"));
    CHECK(StringView(GraphDocumentCodec::ExtensionForType(TEXT("FlaxEngine.BehaviorTree"))) == TEXT(".behaviortree"));
    CHECK(StringView(GraphDocumentCodec::ExtensionForType(TEXT("FlaxEngine.ParticleEmitterFunction"))) == TEXT(".particlefunction"));

    String typeName;
    REQUIRE_FALSE(GraphDocumentCodec::TypeForExtension(TEXT("visualscript"), typeName));
    CHECK(typeName == VisualScript::TypeName);
    REQUIRE_FALSE(GraphDocumentCodec::TypeForExtension(TEXT(".behaviortree"), typeName));
    CHECK(typeName == TEXT("FlaxEngine.BehaviorTree"));
    REQUIRE_FALSE(GraphDocumentCodec::TypeForExtension(TEXT("particlefunction"), typeName));
    CHECK(typeName == TEXT("FlaxEngine.ParticleEmitterFunction"));
    CHECK(GraphDocumentCodec::TypeForExtension(TEXT("flax"), typeName));

    GraphDocument visualScript;
    REQUIRE_FALSE(GraphDocumentCodec::CreateStarter(VisualScript::TypeName, visualScript, diagnostic));
    CHECK(visualScript.TypeName == VisualScript::TypeName);
    CHECK(visualScript.PropertiesJson.Contains("baseType"));
    CHECK(visualScript.PropertiesJson.Contains("FlaxEngine.Script"));
    StringAnsi json;
    REQUIRE_FALSE(GraphDocumentCodec::ToCanonicalJson(visualScript, json, diagnostic));
    GraphDocumentCodec codec;
    GraphDocumentSnapshot snapshot;
    REQUIRE_FALSE(codec.DecodeGraph(json, snapshot, diagnostic));
    CHECK(snapshot.Document.PropertiesJson.Contains("baseType"));
    Array<byte> compiled;
    REQUIRE_FALSE(GraphDocumentCompiler::CompileDocument(snapshot.Document, compiled, diagnostic));
    CHECK(compiled.Count() > 0);

    GraphDocument behaviorTree;
    REQUIRE_FALSE(GraphDocumentCodec::CreateStarter(TEXT("FlaxEngine.BehaviorTree"), behaviorTree, diagnostic));
    CHECK(behaviorTree.TypeName == TEXT("FlaxEngine.BehaviorTree"));
    GraphDocument particleFunction;
    REQUIRE_FALSE(GraphDocumentCodec::CreateStarter(TEXT("FlaxEngine.ParticleEmitterFunction"), particleFunction, diagnostic));
    REQUIRE(particleFunction.Nodes.Count() == 1);
    CHECK(particleFunction.Nodes[0].GroupID == 16);
    CHECK(particleFunction.Nodes[0].TypeID == 2);
}
