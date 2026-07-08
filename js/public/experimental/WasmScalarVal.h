/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A value-typed, GC-ref-free scalar core-wasm value. Unlike wasm::Val
 * (js/src/wasm/WasmValue.h), it never holds a GC reference, so it needs no
 * rooting and is freely copyable. */

#ifndef js_experimental_WasmScalarVal_h
#define js_experimental_WasmScalarVal_h

#include "mozilla/Vector.h"  // mozilla::Vector

#include <stdint.h>

#include "js/AllocPolicy.h"  // js::SystemAllocPolicy

namespace JS {

// This is an untagged union: which member is active is never recorded on the
// value itself, only known out-of-band from the static component/wasm type
// that produced it (a WebIDLCoerceStep's srcBits, or a wasm ValType). Reading
// the wrong member is undefined; callers must agree with the writer.
class WasmScalarVal {
  union {
    uint32_t i32_;
    uint64_t i64_;
    float f32_;
    double f64_;
  };

 public:
  WasmScalarVal() : i32_(0) {}

  static WasmScalarVal fromI32(uint32_t v) {
    WasmScalarVal val;
    val.i32_ = v;
    return val;
  }
  static WasmScalarVal fromI64(uint64_t v) {
    WasmScalarVal val;
    val.i64_ = v;
    return val;
  }
  static WasmScalarVal fromF32(float v) {
    WasmScalarVal val;
    val.f32_ = v;
    return val;
  }
  static WasmScalarVal fromF64(double v) {
    WasmScalarVal val;
    val.f64_ = v;
    return val;
  }

  uint32_t i32() const { return i32_; }
  uint64_t i64() const { return i64_; }
  float f32() const { return f32_; }
  double f64() const { return f64_; }

  // Layout, exposed for JIT-generated code that stores into a WasmScalarVal
  // via raw address writes instead of the constructors above. `payload` is
  // the offset of the union regardless of which member is active; the
  // active member's own width (1/4/8 bytes) is written starting there.
  static constexpr size_t offsetOfPayload() {
    return offsetof(WasmScalarVal, i32_);
  }
};

static_assert(sizeof(WasmScalarVal) == 8,
              "JIT-generated code assumes this exact size/layout");

using WasmScalarValVector =
    mozilla::Vector<WasmScalarVal, 16, js::SystemAllocPolicy>;

}  // namespace JS

#endif  // js_experimental_WasmScalarVal_h
