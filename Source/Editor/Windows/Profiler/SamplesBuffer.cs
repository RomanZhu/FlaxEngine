// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;

namespace FlaxEditor.Windows.Profiler
{
    /// <summary>
    /// Profiler samples storage buffer. Support recording new frame samples.
    /// </summary>
    /// <typeparam name="T">Single sample data type.</typeparam>
    public class SamplesBuffer<T>
    {
        private readonly List<T> _data;

        /// <summary>
        /// Gets the amount of samples in the buffer.
        /// </summary>
        public int Count => _data.Count;

        /// <summary>
        /// Gets the last sample value. Check buffer <see cref="Count"/> before calling this property.
        /// </summary>
        public T Last => _data[_data.Count - 1];

        /// <summary>
        /// Gets or sets the sample value at the specified index.
        /// </summary>
        /// <param name="index">The index.</param>
        /// <returns>The sample value.</returns>
        public T this[int index]
        {
            get => _data[index];
            set => _data[index] = value;
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="SamplesBuffer{T}"/> class.
        /// </summary>
        /// <param name="capacity">The initial buffer capacity.</param>
#if USE_PROFILER
        public SamplesBuffer(int capacity = ProfilerMode.InitialSamplesCapacity)
#else
        public SamplesBuffer(int capacity = 600)
#endif
        {
            _data = new List<T>(capacity);
        }

        /// <summary>
        /// Gets the sample at the specified index or the last sample if index is equal to -1.
        /// </summary>
        /// <param name="index">The index.</param>
        /// <returns>The sample value</returns>
        public T Get(int index)
        {
            if (_data.Count == 0 || index < -1 || index >= _data.Count)
                return default;
            return index == -1 ? _data[_data.Count - 1] : _data[index];
        }

        /// <summary>
        /// Clears this buffer.
        /// </summary>
        public void Clear()
        {
            _data.Clear();
        }

        /// <summary>
        /// Adds the specified sample to the buffer.
        /// </summary>
        /// <param name="sample">The sample.</param>
        public void Add(T sample)
        {
            _data.Add(sample);
        }

        /// <summary>
        /// Adds the specified sample to the buffer.
        /// </summary>
        /// <param name="sample">The sample.</param>
        public void Add(ref T sample)
        {
            _data.Add(sample);
        }
    }
}
