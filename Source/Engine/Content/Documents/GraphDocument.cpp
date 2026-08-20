// Copyright (c) Wojciech Figat. All rights reserved.

#include "GraphDocument.h"
#include "CanonicalJsonWriter.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Utilities/Encryption.h"

namespace
{
    constexpr uint32 VisjectMagic = 1963542358u;

    uint32 ReadUint32(const byte* data)
    {
        return static_cast<uint32>(data[0]) |
               (static_cast<uint32>(data[1]) << 8) |
               (static_cast<uint32>(data[2]) << 16) |
               (static_cast<uint32>(data[3]) << 24);
    }

    StringAnsi HexToken(const char* prefix, uint32 value, int32 digits)
    {
        static const char Hex[] = "0123456789abcdef";
        StringAnsi result(prefix);
        const int32 start = result.Length();
        result.Resize(start + digits);
        for (int32 i = digits - 1; i >= 0; i--)
        {
            result[start + i] = Hex[value & 15];
            value >>= 4;
        }
        return result;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.ProcessorId = TEXT("Flax.GraphDocument");
        diagnostic.Message = message;
        return true;
    }

    bool IsBase64(const StringAnsiView& value)
    {
        if ((value.Length() & 3) != 0)
            return false;
        for (int32 i = 0; i < value.Length(); i++)
        {
            const char c = value[i];
            const bool data = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '+' || c == '/';
            if (!data && c != '=')
                return false;
            if (c == '=' && i < value.Length() - 2)
                return false;
        }
        return true;
    }

    void AppendEscaped(StringAnsi& output, const StringAnsiView& value)
    {
        for (int32 i = 0; i < value.Length(); i++)
        {
            const char c = value[i];
            if (c == '"') output += "\\\"";
            else if (c == '\\') output += "\\\\";
            else if (c == '\n') output += "\\n";
            else if (c == '\r') output += "\\r";
            else if (c == '\t') output += "\\t";
            else if (static_cast<byte>(c) >= 0x20) output += c;
        }
    }
}

StringAnsi GraphDocumentNodeHeader::GetStableID() const
{
    return HexToken("node-", LegacyID, 8);
}

StringAnsi GraphDocumentNodeHeader::GetStableType() const
{
    StringAnsi result = HexToken("group:", GroupID, 4);
    result += "/";
    result += HexToken("node:", TypeID, 4);
    return result;
}

bool GraphDocumentCodec::InspectSurface(const Span<byte>& surface, Array<GraphDocumentNodeHeader>& nodes, AssetPipelineDiagnostic& diagnostic)
{
    nodes.Clear();
    if (!surface.IsValid() || surface.Length() < 16 || surface.Length() > MaximumSurfaceBytes)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph compatibility surface size is invalid."));
    const byte* data = surface.Get();
    if (ReadUint32(data) != VisjectMagic || ReadUint32(data + 4) != CompatibilityGraphVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph compatibility surface header is unsupported."));
    const uint32 nodeCount = ReadUint32(data + 8);
    const uint32 parameterCount = ReadUint32(data + 12);
    if (nodeCount > 65535 || parameterCount > 65535 || 16ull + static_cast<uint64>(nodeCount) * 8ull > static_cast<uint64>(surface.Length()))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph compatibility surface counts are invalid."));
    HashSet<uint32> ids;
    nodes.EnsureCapacity(static_cast<int32>(nodeCount));
    for (uint32 i = 0; i < nodeCount; i++)
    {
        const byte* header = data + 16 + i * 8;
        GraphDocumentNodeHeader node;
        node.LegacyID = ReadUint32(header);
        const uint32 type = ReadUint32(header + 4);
        node.TypeID = static_cast<uint16>(type & 0xffff);
        node.GroupID = static_cast<uint16>(type >> 16);
        if (node.LegacyID == 0 || !ids.Add(node.LegacyID))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph node identifiers must be non-zero and unique."));
        nodes.Add(node);
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentCodec::Encode(const StringView& typeName, const Span<byte>& surface, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    output.Clear();
    Array<GraphDocumentNodeHeader> nodes;
    if (typeName.IsEmpty() || InspectSurface(surface, nodes, diagnostic))
        return true;
    Array<char> base64;
    Encryption::Base64Encode(surface.Get(), surface.Length(), base64);
    const StringAnsi type(typeName);

    StringAnsi json("{\"documentVersion\":1,\"graphVersion\":1,\"type\":\"");
    AppendEscaped(json, type);
    json += "\",\"properties\":{},\"parameters\":{},\"graph\":{\"nodes\":{";
    for (int32 i = 0; i < nodes.Count(); i++)
    {
        if (i != 0)
            json += ',';
        const StringAnsi id = nodes[i].GetStableID();
        const StringAnsi typeToken = nodes[i].GetStableType();
        json += '"';
        json += id;
        json += "\":{\"type\":\"";
        json += typeToken;
        json += "\",\"typeVersion\":1,\"position\":[0,0],\"values\":{},\"custom\":{\"legacyId\":";
        json += StringAnsi::Format("{0}", nodes[i].LegacyID);
        json += "}}";
    }
    json += "},\"connections\":[],\"editor\":{},\"compatibility\":{\"format\":\"visject-7000\",\"surface\":\"";
    json.Append(base64.Get(), base64.Count());
    json += "\"}}}";

    Array<StringAnsi> rootOrder;
    rootOrder.Add("documentVersion");
    rootOrder.Add("graphVersion");
    rootOrder.Add("type");
    rootOrder.Add("properties");
    rootOrder.Add("parameters");
    rootOrder.Add("graph");
    CanonicalJsonError jsonError;
    if (CanonicalJsonWriter::Canonicalize(json, output, jsonError, &rootOrder))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document canonical serialization failed."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentCodec::DecodeGraph(const StringAnsiView& source, GraphDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const
{
    snapshot = GraphDocumentSnapshot();
    CanonicalJsonError jsonError;
    if (CanonicalJsonWriter::Canonicalize(source, snapshot.CanonicalText, jsonError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MetaParseError, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document is not valid canonicalizable JSON."));

    rapidjson_flax::Document document;
    document.Parse(snapshot.CanonicalText.Get(), snapshot.CanonicalText.Length());
    if (document.HasParseError() || !document.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MetaParseError, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document root must be an object."));
    const auto documentVersion = document.FindMember("documentVersion");
    const auto graphVersion = document.FindMember("graphVersion");
    const auto type = document.FindMember("type");
    const auto properties = document.FindMember("properties");
    const auto parameters = document.FindMember("parameters");
    const auto graph = document.FindMember("graph");
    if (documentVersion == document.MemberEnd() || !documentVersion->value.IsInt() ||
        graphVersion == document.MemberEnd() || !graphVersion->value.IsInt() ||
        type == document.MemberEnd() || !type->value.IsString() || type->value.GetStringLength() == 0 ||
        properties == document.MemberEnd() || !properties->value.IsObject() ||
        parameters == document.MemberEnd() || !parameters->value.IsObject() ||
        graph == document.MemberEnd() || !graph->value.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document is missing required typed fields."));
    snapshot.DocumentVersion = documentVersion->value.GetInt();
    snapshot.GraphVersion = graphVersion->value.GetInt();
    snapshot.TypeName = String(StringAnsiView(type->value.GetString(), type->value.GetStringLength()));
    if (snapshot.DocumentVersion != CurrentDocumentVersion || snapshot.GraphVersion != CurrentGraphVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MigrationFailed, AssetPipelineDiagnosticStage::Migration, TEXT("Graph document requires an explicit tracked migration."));

    const auto nodes = graph->value.FindMember("nodes");
    const auto connections = graph->value.FindMember("connections");
    const auto editor = graph->value.FindMember("editor");
    const auto compatibility = graph->value.FindMember("compatibility");
    if (nodes == graph->value.MemberEnd() || !nodes->value.IsObject() ||
        connections == graph->value.MemberEnd() || !connections->value.IsArray() ||
        editor == graph->value.MemberEnd() || !editor->value.IsObject() ||
        compatibility == graph->value.MemberEnd() || !compatibility->value.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document graph shape is invalid."));
    const auto format = compatibility->value.FindMember("format");
    const auto surface = compatibility->value.FindMember("surface");
    if (format == compatibility->value.MemberEnd() || !format->value.IsString() ||
        StringAnsiView(format->value.GetString(), format->value.GetStringLength()) != "visject-7000" ||
        surface == compatibility->value.MemberEnd() || !surface->value.IsString())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document compatibility payload is missing or unsupported."));
    const StringAnsiView encoded(surface->value.GetString(), surface->value.GetStringLength());
    if (!IsBase64(encoded) || Encryption::Base64DecodeLength(encoded.Get(), encoded.Length()) > MaximumSurfaceBytes)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document compatibility payload is not bounded base64."));
    Encryption::Base64Decode(encoded.Get(), encoded.Length(), snapshot.CompatibilitySurface);
    if (InspectSurface(ToSpan(snapshot.CompatibilitySurface), snapshot.Nodes, diagnostic))
        return true;
    if (nodes->value.MemberCount() != static_cast<rapidjson::SizeType>(snapshot.Nodes.Count()))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document stable node projection does not match its compatibility payload."));
    for (const GraphDocumentNodeHeader& node : snapshot.Nodes)
    {
        const StringAnsi id = node.GetStableID();
        const auto projected = nodes->value.FindMember(id.Get());
        if (projected == nodes->value.MemberEnd() || !projected->value.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document is missing a stable node projection."));
    }
    snapshot.FullHash = ContentHash::Compute(snapshot.CanonicalText.Get(), snapshot.CanonicalText.Length());
    snapshot.SemanticHash = ContentHash::Compute(snapshot.CompatibilitySurface.Get(), snapshot.CompatibilitySurface.Count());
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentCodec::Decode(const StringAnsiView& source, AssetDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const
{
    GraphDocumentSnapshot graph;
    if (DecodeGraph(source, graph, diagnostic))
        return true;
    snapshot.TypeName = graph.TypeName;
    snapshot.DocumentVersion = graph.DocumentVersion;
    snapshot.CanonicalText = MoveTemp(graph.CanonicalText);
    snapshot.FullHash = graph.FullHash;
    snapshot.SemanticHash = graph.SemanticHash;
    snapshot.Dependencies = MoveTemp(graph.Dependencies);
    return false;
}

bool GraphDocumentValidator::Validate(const AssetDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const
{
    GraphDocumentCodec codec;
    GraphDocumentSnapshot graph;
    if (snapshot.CanonicalText.IsEmpty() || codec.DecodeGraph(snapshot.CanonicalText, graph, diagnostic))
        return true;
    if (graph.TypeName != snapshot.TypeName || graph.DocumentVersion != snapshot.DocumentVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph snapshot identity differs from its canonical bytes."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentMigrator::Migrate(const AssetDocumentSnapshot& source, int32 targetVersion, StringAnsi& canonicalText, AssetPipelineDiagnostic& diagnostic) const
{
    if (source.DocumentVersion != targetVersion || targetVersion != GraphDocumentCodec::CurrentDocumentVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MigrationFailed, AssetPipelineDiagnosticStage::Migration, TEXT("No ordered graph migration is registered for the requested version range."));
    canonicalText = source.CanonicalText;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentCompiler::Compile(const AssetDocumentSnapshot& snapshot, Array<byte>& output, AssetPipelineDiagnostic& diagnostic) const
{
    GraphDocumentCodec codec;
    GraphDocumentSnapshot graph;
    if (codec.DecodeGraph(snapshot.CanonicalText, graph, diagnostic))
        return true;
    output = MoveTemp(graph.CompatibilitySurface);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
