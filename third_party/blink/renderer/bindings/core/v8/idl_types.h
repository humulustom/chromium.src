// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_IDL_TYPES_H_
#define THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_IDL_TYPES_H_

#include <type_traits>

#include "base/optional.h"
#include "base/template_util.h"
#include "base/time/time.h"
#include "third_party/blink/renderer/bindings/core/v8/idl_types_base.h"
#include "third_party/blink/renderer/bindings/core/v8/native_value_traits.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_string_resource.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/wtf/forward.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

class EventListener;
class ScriptPromise;
class ScriptValue;

// The type names below are named as "IDL" prefix + Web IDL type name.
// https://heycam.github.io/webidl/#dfn-type-name

// Boolean
struct IDLBoolean final : public IDLBaseHelper<bool> {};

// Integer types

namespace bindings {

enum class IDLIntegerConvMode {
  kDefault,
  kClamp,
  kEnforceRange,
};

}  // namespace bindings

template <typename T,
          bindings::IDLIntegerConvMode mode =
              bindings::IDLIntegerConvMode::kDefault>
struct IDLIntegerTypeBase final : public IDLBaseHelper<T> {};

// Integers
using IDLByte = IDLIntegerTypeBase<int8_t>;
using IDLOctet = IDLIntegerTypeBase<uint8_t>;
using IDLShort = IDLIntegerTypeBase<int16_t>;
using IDLUnsignedShort = IDLIntegerTypeBase<uint16_t>;
using IDLLong = IDLIntegerTypeBase<int32_t>;
using IDLUnsignedLong = IDLIntegerTypeBase<uint32_t>;
using IDLLongLong = IDLIntegerTypeBase<int64_t>;
using IDLUnsignedLongLong = IDLIntegerTypeBase<uint64_t>;

// [Clamp] Integers
using IDLByteClamp =
    IDLIntegerTypeBase<int8_t, bindings::IDLIntegerConvMode::kClamp>;
using IDLOctetClamp =
    IDLIntegerTypeBase<uint8_t, bindings::IDLIntegerConvMode::kClamp>;
using IDLShortClamp =
    IDLIntegerTypeBase<int16_t, bindings::IDLIntegerConvMode::kClamp>;
using IDLUnsignedShortClamp =
    IDLIntegerTypeBase<uint16_t, bindings::IDLIntegerConvMode::kClamp>;
using IDLLongClamp =
    IDLIntegerTypeBase<int32_t, bindings::IDLIntegerConvMode::kClamp>;
using IDLUnsignedLongClamp =
    IDLIntegerTypeBase<uint32_t, bindings::IDLIntegerConvMode::kClamp>;
using IDLLongLongClamp =
    IDLIntegerTypeBase<int64_t, bindings::IDLIntegerConvMode::kClamp>;
using IDLUnsignedLongLongClamp =
    IDLIntegerTypeBase<uint64_t, bindings::IDLIntegerConvMode::kClamp>;

// [EnforceRange] Integers
using IDLByteEnforceRange =
    IDLIntegerTypeBase<int8_t, bindings::IDLIntegerConvMode::kEnforceRange>;
using IDLOctetEnforceRange =
    IDLIntegerTypeBase<uint8_t, bindings::IDLIntegerConvMode::kEnforceRange>;
using IDLShortEnforceRange =
    IDLIntegerTypeBase<int16_t, bindings::IDLIntegerConvMode::kEnforceRange>;
using IDLUnsignedShortEnforceRange =
    IDLIntegerTypeBase<uint16_t, bindings::IDLIntegerConvMode::kEnforceRange>;
using IDLLongEnforceRange =
    IDLIntegerTypeBase<int32_t, bindings::IDLIntegerConvMode::kEnforceRange>;
using IDLUnsignedLongEnforceRange =
    IDLIntegerTypeBase<uint32_t, bindings::IDLIntegerConvMode::kEnforceRange>;
using IDLLongLongEnforceRange =
    IDLIntegerTypeBase<int64_t, bindings::IDLIntegerConvMode::kEnforceRange>;
using IDLUnsignedLongLongEnforceRange =
    IDLIntegerTypeBase<uint64_t, bindings::IDLIntegerConvMode::kEnforceRange>;

// Float
struct IDLFloat final : public IDLBaseHelper<float> {};
struct IDLUnrestrictedFloat final : public IDLBaseHelper<float> {};

// Double
struct IDLDouble final : public IDLBaseHelper<double> {};
struct IDLUnrestrictedDouble final : public IDLBaseHelper<double> {};

// Strings
// The "Base" classes are always templatized and require users to specify how JS
// null and/or undefined are supposed to be handled.
template <V8StringResourceMode Mode>
struct IDLByteStringBase final : public IDLBaseHelper<String> {};
template <V8StringResourceMode Mode>
struct IDLStringBase final : public IDLBaseHelper<String> {};
template <V8StringResourceMode Mode>
struct IDLUSVStringBase final : public IDLBaseHelper<String> {};

// Define non-template versions of the above for simplicity.
using IDLByteString = IDLByteStringBase<V8StringResourceMode::kDefaultMode>;
using IDLString = IDLStringBase<V8StringResourceMode::kDefaultMode>;
using IDLUSVString = IDLUSVStringBase<V8StringResourceMode::kDefaultMode>;

// Nullable strings
using IDLByteStringOrNull =
    IDLByteStringBase<V8StringResourceMode::kTreatNullAndUndefinedAsNullString>;
using IDLStringOrNull =
    IDLStringBase<V8StringResourceMode::kTreatNullAndUndefinedAsNullString>;
using IDLUSVStringOrNull =
    IDLUSVStringBase<V8StringResourceMode::kTreatNullAndUndefinedAsNullString>;

// [TreatNullAs] Strings
using IDLStringTreatNullAsEmptyString =
    IDLStringBase<V8StringResourceMode::kTreatNullAsEmptyString>;

// Strings for the new bindings generator

namespace bindings {

enum class IDLStringConvMode {
  kDefault,
  kNullable,
  kTreatNullAsEmptyString,
};

}  // namespace bindings

// ByteString
template <bindings::IDLStringConvMode mode>
struct IDLByteStringBaseV2 final : public IDLBaseHelper<String> {};
using IDLByteStringV2 =
    IDLByteStringBaseV2<bindings::IDLStringConvMode::kDefault>;

// DOMString
template <bindings::IDLStringConvMode mode>
struct IDLStringBaseV2 final : public IDLBaseHelper<String> {};
using IDLStringV2 = IDLStringBaseV2<bindings::IDLStringConvMode::kDefault>;
using IDLStringTreatNullAsV2 =
    IDLStringBaseV2<bindings::IDLStringConvMode::kTreatNullAsEmptyString>;

// USVString
template <bindings::IDLStringConvMode mode>
struct IDLUSVStringBaseV2 final : public IDLBaseHelper<String> {};
using IDLUSVStringV2 =
    IDLUSVStringBaseV2<bindings::IDLStringConvMode::kDefault>;

// object
struct IDLObject final : public IDLBaseHelper<ScriptValue> {};

// Promise
struct IDLPromise final : public IDLBaseHelper<ScriptPromise> {};

// Sequence
template <typename T>
struct IDLSequence final : public IDLBase {
  using ImplType =
      VectorOf<std::remove_pointer_t<typename NativeValueTraits<T>::ImplType>>;
};

// Frozen array types
template <typename T>
using IDLArray = IDLSequence<T>;

// Record
template <typename Key, typename Value>
struct IDLRecord final : public IDLBase {
  static_assert(std::is_same<typename Key::ImplType, String>::value,
                "IDLRecord keys must be of a WebIDL string type");
  static_assert(
      std::is_same<typename NativeValueTraits<Key>::ImplType, String>::value,
      "IDLRecord keys must be of a WebIDL string type");

  using ImplType = VectorOfPairs<
      String,
      std::remove_pointer_t<typename NativeValueTraits<Value>::ImplType>>;
};

// Nullable
template <typename InnerType>
struct IDLNullable final : public IDLBase {
  using ImplType = std::conditional_t<
      NativeValueTraits<InnerType>::has_null_value,
      typename NativeValueTraits<InnerType>::ImplType,
      base::Optional<typename NativeValueTraits<InnerType>::ImplType>>;
};

// Date
struct IDLDate final : public IDLBaseHelper<base::Time> {};

// EventHandler types
struct IDLEventHandler final : public IDLBaseHelper<EventListener*> {};
struct IDLOnBeforeUnloadEventHandler final
    : public IDLBaseHelper<EventListener*> {};
struct IDLOnErrorEventHandler final : public IDLBaseHelper<EventListener*> {};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_IDL_TYPES_H_
