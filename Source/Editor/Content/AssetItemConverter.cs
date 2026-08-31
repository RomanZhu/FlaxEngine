// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Serialize references to the FlaxEngine.Object as Guid (format N).
    /// </summary>
    /// <seealso cref="Newtonsoft.Json.JsonConverter" />
    internal class AssetItemConverter : JsonConverter
    {
        /// <inheritdoc />
        public override void WriteJson(JsonWriter writer, object value, Newtonsoft.Json.JsonSerializer serializer)
        {
            var id = value is AssetItem obj ? obj.ObjectID : default;
            writer.WriteStartObject();
            writer.WritePropertyName("guid");
            writer.WriteValue(id.Asset.ToString());
            writer.WritePropertyName("fileId");
            writer.WriteValue(id.LocalId);
            writer.WriteEndObject();
        }

        /// <inheritdoc />
        public override object ReadJson(JsonReader reader, Type objectType, object existingValue, Newtonsoft.Json.JsonSerializer serializer)
        {
            if (reader.TokenType == JsonToken.StartObject)
            {
                var json = JObject.Load(reader);
                if (Guid.TryParseExact((string)json["guid"], "N", out var source) && json["fileId"]?.Type == JTokenType.Integer)
                {
                    var objectId = new AssetObjectId(new AssetGuid(source), (long)json["fileId"]);
                    var backing = AssetDatabaseQueryService.GetBackingAssetID(objectId);
                    return backing == Guid.Empty ? null : Editor.Instance.ContentDatabase.Find(backing);
                }
            }

            return null;
        }

        /// <inheritdoc />
        public override bool CanConvert(Type objectType)
        {
            return typeof(AssetItem).IsAssignableFrom(objectType);
        }
    }
}
