/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <folly/container/F14Set.h>

#include "velox/common/base/IOUtils.h"
#include "velox/common/memory/HashStringAllocator.h"
#include "velox/exec/AddressableNonNullValueList.h"
#include "velox/exec/Strings.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::aggregate::prestosql {

namespace detail {

/// Maintains a set of unique values. Non-null values are stored in F14FastSet.
/// A separate flag tracks presence of the null value.
/// The SetAccumulator also tracks the order in which the values are added to
/// the accumulator (for ordered aggregations). So each value is associated with
/// an index of its position.

/// SetAccumulator supports serialization/deserialization to/from a bytestream.
/// These are used in the spilling logic of operators using SetAccumulator.

/// The serialization format is :
/// i) index of the null value (or -1 if no null value).
/// ii) The number of unique entries serialized.
/// iii) The values (and optionally some metadata) are then serialized.
/// For a scalar type, only the value is serialized in the order of their
/// indexes in the accumulator. For a string type, a tuple of string (index,
/// length, value) are serialized. For a complex type, a tuple of (index,
/// length, hash, value) are serialized.
template <
    typename T,
    typename Hash = std::hash<T>,
    typename EqualTo = std::equal_to<T>>
struct SetAccumulator {
  std::optional<vector_size_t> nullIndex;

  folly::F14FastMap<
      T,
      int32_t,
      Hash,
      EqualTo,
      AlignedStlAllocator<std::pair<const T, vector_size_t>, 16>>
      uniqueValues;

  SetAccumulator(const TypePtr& /*type*/, HashStringAllocator* allocator)
      : uniqueValues{AlignedStlAllocator<std::pair<const T, vector_size_t>, 16>(
            allocator)} {}

  SetAccumulator(Hash hash, EqualTo equalTo, HashStringAllocator* allocator)
      : uniqueValues{
            0,
            hash,
            equalTo,
            AlignedStlAllocator<std::pair<const T, vector_size_t>, 16>(
                allocator)} {}

  /// Adds value if new. No-op if the value was added before.
  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* /*allocator*/) {
    const auto cnt = uniqueValues.size();
    if (decoded.isNullAt(index)) {
      if (!nullIndex.has_value()) {
        nullIndex = cnt;
      }
    } else {
      uniqueValues.insert(
          {decoded.valueAt<T>(index), nullIndex.has_value() ? cnt + 1 : cnt});
    }
  }

  /// Adds new values from an array.
  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  /// Deserializes accumulator from previously serialized value.
  void deserialize(
      const StringView& serialized,
      HashStringAllocator* /*allocator*/) {
    // The serialized value is the nullIndex (kNoNullIndex if no null is
    // present) followed by the unique values ordered by index.
    common::InputByteStream stream(serialized.data());
    deserializeNullIndex(stream);

    const auto numValues = stream.read<size_t>();
    const auto numAllValues = nullIndex.has_value() ? numValues + 1 : numValues;
    for (auto i = 0; i < numAllValues; i++) {
      if (!isNullIndex(i)) {
        uniqueValues.insert({stream.read<T>(), i});
      }
    }
  }

  /// Returns number of unique values including null.
  size_t size() const {
    return uniqueValues.size() + (nullIndex.has_value() ? 1 : 0);
  }

  /// Copies the unique values and null into the specified vector starting at
  /// the specified offset.
  vector_size_t extractValues(FlatVector<T>& values, vector_size_t offset) {
    for (auto value : uniqueValues) {
      values.set(offset + value.second, value.first);
    }

    if (nullIndex.has_value()) {
      values.setNull(offset + nullIndex.value(), true);
    }

    return nullIndex.has_value() ? uniqueValues.size() + 1
                                 : uniqueValues.size();
  }

  /// Extracts in result[index] a serialized VARBINARY for the Set Values.
  /// This is used for the spill of this accumulator.
  void serialize(const VectorPtr& result, vector_size_t index) {
    const size_t totalBytes =
        kSizeOfVector + kSizeOfSize + kSizeOfValue * uniqueValues.size();

    auto* flatResult = result->as<FlatVector<StringView>>();
    auto* rawBuffer = flatResult->getRawStringBufferWithSpace(totalBytes, true);

    auto nullIndexValue = nullIndexSerializationValue();
    memcpy(rawBuffer, &nullIndexValue, kSizeOfVector);

    auto numValues = uniqueValues.size();
    memcpy(rawBuffer + kSizeOfVector, &numValues, kSizeOfSize);

    // The null position is skipped when serializing values, so setting an out
    // of bound value for no null position.
    const auto nullPosition =
        nullIndex.has_value() ? nullIndex : uniqueValues.size();
    for (const auto& value : uniqueValues) {
      auto index = value.second;
      auto offset = (kSizeOfVector + kSizeOfSize) +
          (index < nullPosition ? index : index - 1) * kSizeOfValue;
      memcpy(rawBuffer + offset, &(value.first), kSizeOfValue);
    }

    flatResult->setNoCopy(index, StringView(rawBuffer, totalBytes));
  }

  void free(HashStringAllocator& allocator) {
    using UT = decltype(uniqueValues);
    uniqueValues.~UT();
  }

  void deserializeNullIndex(common::InputByteStream& stream) {
    VELOX_CHECK(!nullIndex.has_value());
    const auto streamNullIndex = stream.read<vector_size_t>();
    if (streamNullIndex != kNoNullIndex) {
      nullIndex = streamNullIndex;
    }
  }

  inline bool isNullIndex(size_t i) {
    return nullIndex.has_value() && i == nullIndex.value();
  }

  vector_size_t nullIndexSerializationValue() {
    return nullIndex.has_value() ? nullIndex.value() : kNoNullIndex;
  }

  static const vector_size_t kNoNullIndex = -1;
  static constexpr size_t kSizeOfVector = sizeof(vector_size_t);
  static constexpr size_t kSizeOfValue = sizeof(T);
  static constexpr size_t kSizeOfSize = sizeof(size_t);
};

/// Maintains a set of unique strings.
struct StringViewSetAccumulator {
  /// A set of unique StringViews pointing to storage managed by 'strings'.
  SetAccumulator<StringView> base;

  /// Stores unique non-null non-inline strings.
  Strings strings;

  /// Size (in bytes) of the serialized string values (this includes inline and
  /// non-inline) strings. This value also includes the bytes for serializing
  /// the length and index values (2 * base.kSizeOfVector) of the strings.
  /// Used for computing serialized buffer size for spilling.
  /// It is initialized for the size of nullIndex and number of unique values.
  size_t stringSetBytes = base.kSizeOfVector + base.kSizeOfSize;

  StringViewSetAccumulator(const TypePtr& type, HashStringAllocator* allocator)
      : base{type, allocator} {}

  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* allocator) {
    const auto cnt = base.uniqueValues.size();
    if (decoded.isNullAt(index)) {
      if (!base.nullIndex.has_value()) {
        base.nullIndex = cnt;
      }
    } else {
      auto value = decoded.valueAt<StringView>(index);
      addValue(value, base.nullIndex.has_value() ? cnt + 1 : cnt, allocator);
    }
  }

  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  void deserialize(
      const StringView& serialized,
      HashStringAllocator* allocator) {
    common::InputByteStream stream(serialized.data());
    const auto size = serialized.size();
    base.deserializeNullIndex(stream);
    auto numValues = stream.read<size_t>();

    while (stream.offset() < size) {
      auto index = stream.read<vector_size_t>();
      auto length = stream.read<vector_size_t>();
      addUniqueValue(
          StringView(stream.read<char>(length), length), index, allocator);
    }

    VELOX_CHECK_EQ(numValues, base.uniqueValues.size());
  }

  size_t size() const {
    return base.size();
  }

  vector_size_t extractValues(
      FlatVector<StringView>& values,
      vector_size_t offset) {
    return base.extractValues(values, offset);
  }

  /// Extracts in result[index] a serialized VARBINARY for the String Values.
  /// This is used for the spill of this accumulator.
  void serialize(const VectorPtr& result, vector_size_t index) {
    auto* flatResult = result->as<FlatVector<StringView>>();
    auto* rawBuffer =
        flatResult->getRawStringBufferWithSpace(stringSetBytes, true);
    common::OutputByteStream stream(rawBuffer);
    auto nullIndexValue = base.nullIndexSerializationValue();
    stream.appendOne(nullIndexValue);
    stream.appendOne(base.uniqueValues.size());

    for (const auto& value : base.uniqueValues) {
      // Index.
      stream.appendOne(value.second);
      // String length.
      const vector_size_t length = value.first.size();
      stream.appendOne(length);
      // String value.
      stream.append(value.first.data(), length);
    }

    flatResult->setNoCopy(index, StringView(rawBuffer, stringSetBytes));
  }

  void free(HashStringAllocator& allocator) {
    strings.free(allocator);
    using Base = decltype(base);
    base.~Base();
  }

 private:
  void addValue(
      const StringView& value,
      vector_size_t index,
      HashStringAllocator* allocator) {
    if (base.uniqueValues.contains(value)) {
      return;
    }

    addUniqueValue(value, index, allocator);
  }

  void addUniqueValue(
      const StringView& value,
      vector_size_t index,
      HashStringAllocator* allocator) {
    VELOX_CHECK(!base.uniqueValues.contains(value));
    StringView valueCopy = value;
    if (!valueCopy.isInline()) {
      valueCopy = strings.append(value, *allocator);
    }

    base.uniqueValues.insert({valueCopy, index});
    // Accounts for serializing the index and length of the string as well.
    stringSetBytes += 2 * base.kSizeOfVector + valueCopy.size();
  }
};

/// Maintains a set of unique arrays, maps or structs.
struct ComplexTypeSetAccumulator {
  /// A set of pointers to values stored in AddressableNonNullValueList.
  SetAccumulator<
      AddressableNonNullValueList::Entry,
      AddressableNonNullValueList::Hash,
      AddressableNonNullValueList::EqualTo>
      base;

  /// Stores unique non-null values.
  AddressableNonNullValueList values;

  /// Tracks allocated bytes for sizing during serialization for spill.
  /// Initialized to account for the serialization of the null index and number
  /// of unique values.
  size_t totalSize = base.kSizeOfVector + base.kSizeOfSize;

  static constexpr size_t kSizeOfHash = sizeof(uint64_t);

  ComplexTypeSetAccumulator(const TypePtr& type, HashStringAllocator* allocator)
      : base{
            AddressableNonNullValueList::Hash{},
            AddressableNonNullValueList::EqualTo{type},
            allocator} {}

  void addValue(
      const DecodedVector& decoded,
      vector_size_t index,
      HashStringAllocator* allocator) {
    const auto cnt = base.uniqueValues.size();
    if (decoded.isNullAt(index)) {
      if (!base.nullIndex.has_value()) {
        base.nullIndex = cnt;
      }
    } else {
      const auto entry = values.append(decoded, index, allocator);
      const auto position = base.nullIndex.has_value() ? cnt + 1 : cnt;
      addEntry(entry, position);
    }
  }

  void addValues(
      const ArrayVector& arrayVector,
      vector_size_t index,
      const DecodedVector& values,
      HashStringAllocator* allocator) {
    const auto size = arrayVector.sizeAt(index);
    const auto offset = arrayVector.offsetAt(index);

    for (auto i = 0; i < size; ++i) {
      addValue(values, offset + i, allocator);
    }
  }

  void deserialize(
      const StringView& serialized,
      HashStringAllocator* allocator) {
    auto stream = common::InputByteStream(serialized.data());
    base.deserializeNullIndex(stream);
    auto numValues = stream.read<size_t>();

    const auto size = serialized.size();
    while (stream.offset() < size) {
      auto index = stream.read<vector_size_t>();
      auto length = stream.read<vector_size_t>();
      auto hash = stream.read<uint64_t>();
      auto contents = StringView(stream.read<char>(length), length);
      auto position = values.appendSerialized(contents, allocator);
      addEntry({position, contents.size(), hash}, index);
    }

    VELOX_CHECK_EQ(numValues, base.uniqueValues.size());
  }

  size_t size() const {
    return base.size();
  }

  vector_size_t extractValues(BaseVector& values, vector_size_t offset) {
    for (const auto& position : base.uniqueValues) {
      AddressableNonNullValueList::read(
          position.first, values, offset + position.second);
    }

    if (base.nullIndex.has_value()) {
      values.setNull(offset + base.nullIndex.value(), true);
    }

    return base.uniqueValues.size() + (base.nullIndex.has_value() ? 1 : 0);
  }

  /// Extracts in result[index] a serialized VARBINARY for the String Values.
  /// This is used for the spill of this accumulator.
  void serialize(const VectorPtr& result, vector_size_t index) {
    auto* flatResult = result->as<FlatVector<StringView>>();
    auto* rawBuffer = flatResult->getRawStringBufferWithSpace(totalSize, true);

    SerializationStream stream(rawBuffer, totalSize);

    auto nullIndexValue = base.nullIndexSerializationValue();
    stream.append(&nullIndexValue, base.kSizeOfVector);

    auto numValues = base.uniqueValues.size();
    stream.append(&numValues, base.kSizeOfSize);

    for (const auto& value : base.uniqueValues) {
      // Index.
      stream.append(&value.second, base.kSizeOfVector);
      // Complex type Length.
      stream.append(&value.first.size, base.kSizeOfVector);
      // Complex type hash.
      stream.append(&value.first.hash, kSizeOfHash);
      // Complex type value.
      stream.append(value.first);
    }

    flatResult->setNoCopy(index, StringView(rawBuffer, totalSize));
  }

  void free(HashStringAllocator& allocator) {
    values.free(allocator);
    using Base = decltype(base);
    base.~Base();
  }

 private:
  // Simple stream abstraction for serialization logic. 'append' calls to concat
  // values to the stream for the input buffer are exposed to the user.
  struct SerializationStream {
    char* rawBuffer;
    const vector_size_t totalSize;
    vector_size_t offset = 0;

    SerializationStream(char* buffer, vector_size_t totalSize)
        : rawBuffer(buffer), totalSize(totalSize) {}

    void append(const void* value, vector_size_t size) {
      VELOX_CHECK_LE(offset + size, totalSize);
      memcpy(rawBuffer + offset, value, size);
      offset += size;
    }

    void append(const AddressableNonNullValueList::Entry& entry) {
      VELOX_CHECK_LE(offset + entry.size, totalSize);
      AddressableNonNullValueList::readSerialized(entry, rawBuffer + offset);
      offset += entry.size;
    }
  };

  void addEntry(
      const AddressableNonNullValueList::Entry& entry,
      vector_size_t index) {
    if (!base.uniqueValues.insert({entry, index}).second) {
      values.removeLast(entry);
    } else {
      // Accounts for the length of the complex type along with its size and
      // hash.
      totalSize += 2 * base.kSizeOfVector + kSizeOfHash + entry.size;
    }
  }
};

template <typename T>
struct SetAccumulatorTypeTraits {
  using AccumulatorType = SetAccumulator<T>;
};

template <>
struct SetAccumulatorTypeTraits<StringView> {
  using AccumulatorType = StringViewSetAccumulator;
};

template <>
struct SetAccumulatorTypeTraits<ComplexType> {
  using AccumulatorType = ComplexTypeSetAccumulator;
};
} // namespace detail

template <typename T>
using SetAccumulator =
    typename detail::SetAccumulatorTypeTraits<T>::AccumulatorType;

} // namespace facebook::velox::aggregate::prestosql
