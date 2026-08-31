// Copyright (c) Wojciech Figat. All rights reserved.

using System;

namespace FlaxEngine
{
    /// <summary>
    /// The scripting object soft reference. Objects gets referenced on use (ID reference is resolving it).
    /// </summary>
    public struct SoftObjectReference : IComparable, IComparable<SoftObjectReference>
    {
        private Guid _id;
        private Object _object;
        private AssetObjectId _objectId;

        /// <summary>
        /// Gets or sets the object identifier.
        /// </summary>
        public Guid ID
        {
            get => _id;
            set
            {
                if (_id == value)
                    return;
                _id = value;
                _object = null;
                _objectId = default;
                if (_id != Guid.Empty)
                    Content.GetAssetObjectId(_id, out _objectId);
            }
        }

        /// <summary>Gets the persistent identity when this reference targets an asset object.</summary>
        public AssetObjectId ObjectId
        {
            get
            {
                if (_object is Asset asset && Content.GetAssetObjectId(asset.ID, out var id))
                    return id;
                if (_objectId.IsValid)
                    return _objectId;
                return _id != Guid.Empty && Content.GetAssetObjectId(_id, out id) ? id : default;
            }
        }

        /// <summary>
        /// Gets the object reference.
        /// </summary>
        /// <typeparam name="T">The object type.</typeparam>
        /// <returns>The resolved object or null.</returns>
        public T Get<T>() where T : Object
        {
            if (!_object && _objectId.IsValid)
                _object = Content.LoadAsync(_objectId, typeof(Asset));
            if (!_object)
                _object = Object.Find(ref _id, typeof(T));
            return _object as T;
        }

        /// <summary>
        /// Sets the object reference.
        /// </summary>
        /// <param name="obj">The object.</param>
        public void Set(Object obj)
        {
            _object = obj;
            _id = obj?.ID ?? Guid.Empty;
            _objectId = default;
            if (obj is Asset asset)
                Content.GetAssetObjectId(asset.ID, out _objectId);
        }

        /// <summary>Sets an exact persistent asset object identity.</summary>
        public void Set(AssetObjectId objectId)
        {
            _objectId = objectId;
            _object = objectId.IsValid ? Content.LoadAsync(objectId, typeof(Asset)) : null;
            _id = _object?.ID ?? (objectId.LocalId == 1 ? objectId.Guid : Guid.Empty);
        }

        /// <inheritdoc />
        public override string ToString()
        {
            if (_object)
                return _object.ToString();
            return _id.ToString();
        }

        /// <inheritdoc />
        public override int GetHashCode()
        {
            return ObjectId.IsValid ? ObjectId.GetHashCode() : _id.GetHashCode();
        }

        /// <inheritdoc />
        public int CompareTo(object obj)
        {
            if (obj is SoftObjectReference other)
                return CompareTo(other);
            return 0;
        }

        /// <inheritdoc />
        public int CompareTo(SoftObjectReference other)
        {
            if (ObjectId.IsValid || other.ObjectId.IsValid)
                return string.CompareOrdinal(ObjectId.ToString(), other.ObjectId.ToString());
            return _id.CompareTo(other._id);
        }
    }
}
