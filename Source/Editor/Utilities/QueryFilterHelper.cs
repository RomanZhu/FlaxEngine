// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEngine;

namespace FlaxEditor.Utilities
{
    /// <summary>
    /// Helper class to filter items based on a input filter query.
    /// </summary>
    [HideInEditor]
    public static class QueryFilterHelper
    {
        /// <summary>
        /// The minimum text match length.
        /// </summary>
        public const int MinLength = 1;

        /// <summary>
        /// Matches the specified text with the filter.
        /// </summary>
        /// <param name="filter">The filter.</param>
        /// <param name="text">The text.</param>
        /// <returns>True if text has one or more matches, otherwise false.</returns>
        public static bool Match(string filter, string text)
        {
            // Empty inputs
            if (string.IsNullOrEmpty(filter) || string.IsNullOrEmpty(text))
                return false;

            // Full match
            if (string.Equals(filter, text, StringComparison.CurrentCultureIgnoreCase))
            {
                return true;
            }

            bool hasMatch = false;

            // Find matching sequences
            // We do simple iteration over the characters
            int textLength = text.Length;
            int filterLength = filter.Length;
            int searchEnd = textLength - filterLength;
            for (int textPos = 0; textPos <= searchEnd; textPos++)
            {
                // Skip if the current text position doesn't match the filter start
                if (char.ToLower(filter[0]) != char.ToLower(text[textPos]))
                    continue;

                int matchStartPos = -1;
                int endPos = textPos + filterLength;
                int filterPos = 0;

                for (int i = textPos; i < endPos; i++, filterPos++)
                {
                    var filterChar = char.ToLower(filter[filterPos]);
                    var textChar = char.ToLower(text[i]);

                    if (filterChar == textChar)
                    {
                        // Check if start the matching sequence
                        if (matchStartPos == -1)
                        {
                            matchStartPos = textPos;
                        }
                    }
                    else
                    {
                        // Check if stop matching sequence
                        if (matchStartPos != -1)
                        {
                            var length = textPos - matchStartPos;
                            if (length >= MinLength)
                                hasMatch = true;
                            textPos = matchStartPos + length;
                            matchStartPos = -1;
                        }
                        break;
                    }
                }

                // Check sequence on the end
                if (matchStartPos != -1 && filterPos == filterLength)
                {
                    var length = endPos - matchStartPos;
                    if (length >= MinLength)
                        hasMatch = true;
                    textPos = matchStartPos + length;
                }
            }

            return hasMatch;
        }

        /// <summary>
        /// Matches the specified text with the filter.
        /// </summary>
        /// <param name="filter">The filter.</param>
        /// <param name="text">The text.</param>
        /// <param name="matches">The found matches.</param>
        /// <returns>True if text has one or more matches, otherwise false.</returns>
        public static bool Match(string filter, string text, out Range[] matches)
        {
            // Empty inputs
            matches = null;
            if (string.IsNullOrEmpty(filter) || string.IsNullOrEmpty(text))
                return false;

            // Full match
            if (string.Equals(filter, text, StringComparison.CurrentCultureIgnoreCase))
            {
                matches = new[] { new Range(0, filter.Length) };
                return true;
            }

            List<Range> ranges = null;

            // Find matching sequences by doing simple iteration over the characters
            int textLength = text.Length;
            int filterLength = filter.Length;
            int searchEnd = textLength - filterLength;
            for (int textPos = 0; textPos <= searchEnd; textPos++)
            {
                // Skip if the current text position doesn't match the filter start
                if (char.ToLower(filter[0]) != char.ToLower(text[textPos]))
                    continue;

                int matchStartPos = -1;
                int endPos = textPos + filterLength;
                int filterPos = 0;

                for (int i = textPos; i < endPos; i++, filterPos++)
                {
                    var filterChar = char.ToLower(filter[filterPos]);
                    var textChar = char.ToLower(text[i]);

                    if (filterChar == textChar)
                    {
                        // Check if start the matching sequence
                        if (matchStartPos == -1)
                        {
                            ranges ??= new List<Range>();
                            matchStartPos = textPos;
                        }
                    }
                    else
                    {
                        // Check if stop matching sequence
                        if (matchStartPos != -1)
                        {
                            var length = textPos - matchStartPos;
                            if (length >= MinLength)
                                ranges!.Add(new Range(matchStartPos, length));
                            textPos = matchStartPos + length;
                            matchStartPos = -1;
                        }
                        break;
                    }
                }

                // Check sequence on the end
                if (matchStartPos != -1 && filterPos == filterLength)
                {
                    var length = endPos - matchStartPos;
                    if (length >= MinLength)
                        ranges!.Add(new Range(matchStartPos, length));
                    textPos = matchStartPos + length;
                }
            }

            // Check if has any range
            if (ranges is { Count: > 0 })
            {
                matches = ranges.ToArray();
                return true;
            }

            return false;
        }

        /// <summary>
        /// Fuzzy-matches a query against text and returns ordered highlight ranges and a relevance score.
        /// Exact, prefix, contiguous and word-boundary matches receive the highest scores while gaps are penalized.
        /// </summary>
        /// <param name="filter">The filter query.</param>
        /// <param name="text">The text to search.</param>
        /// <param name="score">The match relevance score.</param>
        /// <param name="matches">The matching character ranges.</param>
        /// <returns>True if every query character can be matched in order.</returns>
        public static bool FuzzyMatch(string filter, string text, out float score, out Range[] matches)
        {
            score = 0.0f;
            matches = null;
            if (string.IsNullOrWhiteSpace(filter) || string.IsNullOrWhiteSpace(text))
                return false;

            filter = filter.Trim();
            if (string.Equals(filter, text, StringComparison.CurrentCultureIgnoreCase))
            {
                score = 1000.0f;
                matches = new[] { new Range(0, text.Length) };
                return true;
            }

            var positions = new int[filter.Length];
            int filterIndex = 0;
            int previous = -1;
            for (int textIndex = 0; textIndex < text.Length && filterIndex < filter.Length; textIndex++)
            {
                if (char.ToLowerInvariant(filter[filterIndex]) != char.ToLowerInvariant(text[textIndex]))
                    continue;

                positions[filterIndex++] = textIndex;
                bool boundary = textIndex == 0 || !char.IsLetterOrDigit(text[textIndex - 1]) ||
                                (char.IsLower(text[textIndex - 1]) && char.IsUpper(text[textIndex]));
                score += boundary ? 14.0f : 4.0f;
                if (previous + 1 == textIndex)
                    score += 9.0f;
                else if (previous >= 0)
                    score -= Math.Min(8, textIndex - previous - 1);
                previous = textIndex;
            }

            if (filterIndex != filter.Length)
                return false;

            if (positions[0] == 0)
                score += 80.0f;
            score += Math.Max(0, 40 - positions[0] * 2);
            score -= Math.Max(0, text.Length - filter.Length) * 0.15f;

            var ranges = new List<Range>();
            int rangeStart = positions[0];
            int rangeLength = 1;
            for (int i = 1; i < positions.Length; i++)
            {
                if (positions[i] == positions[i - 1] + 1)
                {
                    rangeLength++;
                }
                else
                {
                    ranges.Add(new Range(rangeStart, rangeLength));
                    rangeStart = positions[i];
                    rangeLength = 1;
                }
            }
            ranges.Add(new Range(rangeStart, rangeLength));
            matches = ranges.ToArray();
            return true;
        }

        /// <summary>
        /// Describes sub range of the text.
        /// </summary>
        public readonly struct Range
        {
            /// <summary>
            /// The start index of the range.
            /// </summary>
            public readonly int StartIndex;

            /// <summary>
            /// The length.
            /// </summary>
            public readonly int Length;

            /// <summary>
            /// The end index of the range.
            /// </summary>
            public int EndIndex => StartIndex + Length;

            /// <summary>
            /// Initializes a new instance of the <see cref="Range"/> struct.
            /// </summary>
            /// <param name="start">The start.</param>
            /// <param name="length">The length.</param>
            public Range(int start, int length)
            {
                StartIndex = start;
                Length = length;
            }

            /// <summary>
            /// Tests for equality between two objects.
            /// </summary>
            /// <param name="left">The first value to compare.</param>
            /// <param name="right">The second value to compare.</param>
            /// <returns><c>true</c> if <paramref name="left"/> has the same value as <paramref name="right"/>; otherwise, <c>false</c>.</returns>
            public static bool operator ==(Range left, Range right)
            {
                return left.Equals(right);
            }

            /// <summary>
            /// Tests for equality between two objects.
            /// </summary>
            /// <param name="left">The first value to compare.</param>
            /// <param name="right">The second value to compare.</param>
            /// <returns><c>true</c> if <paramref name="left"/> has the same value as <paramref name="right"/>; otherwise, <c>false</c>.</returns>
            public static bool operator !=(Range left, Range right)
            {
                return !left.Equals(right);
            }

            /// <summary>
            /// Compares this object with the other instance.
            /// </summary>
            /// <param name="other">The other object.</param>
            /// <returns>True if objects are equal.</returns>
            public bool Equals(Range other)
            {
                return StartIndex == other.StartIndex && Length == other.Length;
            }

            /// <inheritdoc />
            public override bool Equals(object obj)
            {
                if (ReferenceEquals(null, obj))
                    return false;
                return obj is Range && Equals((Range)obj);
            }

            /// <inheritdoc />
            public override int GetHashCode()
            {
                unchecked
                {
                    return (StartIndex * 397) ^ Length;
                }
            }

            /// <inheritdoc />
            public override string ToString()
            {
                return $"StartIndex: {StartIndex}, Length: {Length}";
            }
        }
    }
}
