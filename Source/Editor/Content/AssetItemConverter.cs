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
            writer.WriteValue(id.ToString("N"));
        }

        /// <inheritdoc />
        public override object ReadJson(JsonReader reader, Type objectType, object existingValue, Newtonsoft.Json.JsonSerializer serializer)
        {
            if (reader.TokenType == JsonToken.String && Guid.TryParseExact((string)reader.Value, "N", out var id))
            {
                var backing = AssetDatabaseQueryService.GetBackingAssetID(id);
                return backing == Guid.Empty ? null : Editor.Instance.ContentDatabase.Find(backing);
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
