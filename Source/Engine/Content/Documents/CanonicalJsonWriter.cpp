// Copyright (c) Wojciech Figat. All rights reserved.

#include "CanonicalJsonWriter.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Platform/StringUtils.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    typedef rapidjson_flax::PrettyWriter<rapidjson_flax::StringBuffer> Writer;
    typedef rapidjson_flax::Value Value;
    typedef Value::ConstMemberIterator MemberIterator;

    bool Fail(CanonicalJsonError& error, CanonicalJsonErrorCode code, const StringView& path, const StringView& message)
    {
        error.Code = code;
        error.Path = path;
        error.Message = message;
        return true;
    }

    bool ValidateValue(const Value& value, const String& path, CanonicalJsonError& error)
    {
        if (value.IsDouble() && !std::isfinite(value.GetDouble()))
            return Fail(error, CanonicalJsonErrorCode::NonFiniteNumber, path, TEXT("JSON contains a non-finite number."));
        if (value.IsArray())
        {
            for (rapidjson::SizeType i = 0; i < value.Size(); i++)
            {
                if (ValidateValue(value[i], path + TEXT("/") + StringUtils::ToString((int32)i), error))
                    return true;
            }
        }
        else if (value.IsObject())
        {
            Array<MemberIterator> members;
            for (MemberIterator i = value.MemberBegin(); i != value.MemberEnd(); ++i)
                members.Add(i);
            if (members.Count() > 1)
                std::sort(members.Get(), members.Get() + members.Count(), [](const MemberIterator& a, const MemberIterator& b)
            {
                const int32 length = Math::Min((int32)a->name.GetStringLength(), (int32)b->name.GetStringLength());
                const int32 comparison = Platform::MemoryCompare(a->name.GetString(), b->name.GetString(), length);
                return comparison != 0 ? comparison < 0 : a->name.GetStringLength() < b->name.GetStringLength();
                });
            for (int32 i = 1; i < members.Count(); i++)
            {
                if (members[i - 1]->name.GetStringLength() == members[i]->name.GetStringLength() &&
                    Platform::MemoryCompare(members[i - 1]->name.GetString(), members[i]->name.GetString(), members[i]->name.GetStringLength()) == 0)
                    return Fail(error, CanonicalJsonErrorCode::DuplicateKey, path, TEXT("JSON object contains a duplicate key."));
            }
            for (const MemberIterator& member : members)
            {
                const String memberName(StringAnsi(member->name.GetString(), member->name.GetStringLength()));
                if (ValidateValue(member->value, path + TEXT("/") + memberName, error))
                    return true;
            }
        }
        return false;
    }

    bool LessMember(const MemberIterator& a, const MemberIterator& b)
    {
        const int32 length = Math::Min((int32)a->name.GetStringLength(), (int32)b->name.GetStringLength());
        const int32 comparison = Platform::MemoryCompare(a->name.GetString(), b->name.GetString(), length);
        return comparison != 0 ? comparison < 0 : a->name.GetStringLength() < b->name.GetStringLength();
    }

    void WriteValue(Writer& writer, const Value& value, const StringAnsi& path, const Array<StringAnsi>* rootFieldOrder, const Dictionary<StringAnsi, Array<StringAnsi>>* objectFieldOrders)
    {
        if (value.IsNull())
            writer.Null();
        else if (value.IsBool())
            writer.Bool(value.GetBool());
        else if (value.IsInt())
            writer.Int(value.GetInt());
        else if (value.IsUint())
            writer.Uint(value.GetUint());
        else if (value.IsInt64())
            writer.Int64(value.GetInt64());
        else if (value.IsUint64())
            writer.Uint64(value.GetUint64());
        else if (value.IsDouble())
            writer.Double(value.GetDouble());
        else if (value.IsString())
            writer.String(value.GetString(), value.GetStringLength());
        else if (value.IsArray())
        {
            writer.StartArray();
            for (const Value& element : value.GetArray())
            {
                WriteValue(writer, element, path + "/*", rootFieldOrder, objectFieldOrders);
            }
            writer.EndArray();
        }
        else
        {
            writer.StartObject();
            Array<MemberIterator> members;
            for (MemberIterator i = value.MemberBegin(); i != value.MemberEnd(); ++i)
                members.Add(i);
            if (members.Count() > 1)
                std::sort(members.Get(), members.Get() + members.Count(), LessMember);

            HashSet<StringAnsi> written;
            const Array<StringAnsi>* fieldOrder = path.IsEmpty() ? rootFieldOrder : (objectFieldOrders ? objectFieldOrders->TryGet(path) : nullptr);
            if (fieldOrder)
            {
                for (const StringAnsi& key : *fieldOrder)
                {
                    for (const MemberIterator& member : members)
                    {
                        if (StringAnsiView(member->name.GetString(), member->name.GetStringLength()) == key)
                        {
                            writer.Key(member->name.GetString(), member->name.GetStringLength());
                            WriteValue(writer, member->value, path + "/" + key, rootFieldOrder, objectFieldOrders);
                            written.Add(key);
                            break;
                        }
                    }
                }
            }
            for (const MemberIterator& member : members)
            {
                const StringAnsi key(member->name.GetString(), member->name.GetStringLength());
                if (written.Contains(key))
                    continue;
                writer.Key(member->name.GetString(), member->name.GetStringLength());
                WriteValue(writer, member->value, path + "/" + key, rootFieldOrder, objectFieldOrders);
            }
            writer.EndObject();
        }
    }
}

bool CanonicalJsonWriter::Canonicalize(const StringAnsiView& input, StringAnsi& output, CanonicalJsonError& error, const Array<StringAnsi>* rootFieldOrder)
{
    error = CanonicalJsonError();
    rapidjson_flax::Document document;
    document.Parse(input.Get(), input.Length());
    if (document.HasParseError())
    {
        error.Code = CanonicalJsonErrorCode::ParseError;
        error.Offset = (int32)document.GetErrorOffset();
        error.Message = TEXT("JSON parsing failed.");
        return true;
    }
    return Write(document, output, error, rootFieldOrder);
}

bool CanonicalJsonWriter::Write(const rapidjson_flax::Value& value, StringAnsi& output, CanonicalJsonError& error, const Array<StringAnsi>* rootFieldOrder, const Dictionary<StringAnsi, Array<StringAnsi>>* objectFieldOrders)
{
    error = CanonicalJsonError();
    if (Validate(value, error))
        return true;
    rapidjson_flax::StringBuffer buffer;
    Writer writer(buffer);
    writer.SetIndent(' ', 2);
    WriteValue(writer, value, StringAnsi(), rootFieldOrder, objectFieldOrders);
    output.Set(buffer.GetString(), (int32)buffer.GetSize());
    output.Append("\n", 1);
    return false;
}

bool CanonicalJsonWriter::Validate(const rapidjson_flax::Value& value, CanonicalJsonError& error)
{
    error = CanonicalJsonError();
    return ValidateValue(value, String(), error);
}
