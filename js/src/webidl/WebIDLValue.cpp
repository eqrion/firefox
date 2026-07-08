/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/BigInt.h"  // JS::BigInt
#include "js/experimental/WebIDL.h"
#include "js/TracingAPI.h"  // JS::TraceEdge

using namespace JS;

WebIDLValue WebIDLValue::makeNull(const WebIDLType& type) {
  MOZ_ASSERT(type.kind() == WebIDLTypeKind::Nullable);
  return WebIDLValue(type, Repr(mozilla::Nothing()));
}
WebIDLValue WebIDLValue::makeUndefined(const WebIDLType& type) {
  return WebIDLValue(type, Repr(mozilla::Nothing()));
}
WebIDLValue WebIDLValue::makeBool(const WebIDLType& type, bool b) {
  return WebIDLValue(type, Repr(b));
}
WebIDLValue WebIDLValue::makeSigned(const WebIDLType& type, int64_t v) {
  return WebIDLValue(type, Repr(v));
}
WebIDLValue WebIDLValue::makeUnsigned(const WebIDLType& type, uint64_t v) {
  return WebIDLValue(type, Repr(v));
}
WebIDLValue WebIDLValue::makeDouble(const WebIDLType& type, double v) {
  return WebIDLValue(type, Repr(v));
}
WebIDLValue WebIDLValue::makeBigInt(const WebIDLType& type, JS::BigInt* v) {
  return WebIDLValue(type, Repr(JS::Heap<JS::BigInt*>(v)));
}
WebIDLValue WebIDLValue::makeString(const WebIDLType& type, JSString* v) {
  return WebIDLValue(type, Repr(JS::Heap<JSString*>(v)));
}
WebIDLValue WebIDLValue::makeObject(const WebIDLType& type, JSObject* v) {
  return WebIDLValue(type, Repr(JS::Heap<JSObject*>(v)));
}
WebIDLValue WebIDLValue::makeAny(const WebIDLType& type, const JS::Value& v) {
  return WebIDLValue(type, Repr(JS::Heap<JS::Value>(v)));
}
WebIDLValue WebIDLValue::makeEnum(const WebIDLType& type, uint32_t index) {
  return WebIDLValue(type, Repr(WebIDLEnumValue{index}));
}
WebIDLValue WebIDLValue::makeComposite(const WebIDLType& type,
                                       RefPtr<WebIDLValueData> data) {
  return WebIDLValue(type, Repr(std::move(data)));
}

void WebIDLValue::trace(JSTracer* trc) {
  if (repr_.is<JS::Heap<JS::BigInt*>>()) {
    JS::TraceEdge(trc, &repr_.as<JS::Heap<JS::BigInt*>>(),
                  "WebIDLValue bigint");
  } else if (repr_.is<JS::Heap<JSString*>>()) {
    JS::TraceEdge(trc, &repr_.as<JS::Heap<JSString*>>(), "WebIDLValue string");
  } else if (repr_.is<JS::Heap<JSObject*>>()) {
    JS::TraceEdge(trc, &repr_.as<JS::Heap<JSObject*>>(), "WebIDLValue object");
  } else if (repr_.is<JS::Heap<JS::Value>>()) {
    JS::TraceEdge(trc, &repr_.as<JS::Heap<JS::Value>>(), "WebIDLValue any");
  } else if (repr_.is<RefPtr<WebIDLValueData>>()) {
    WebIDLValueData* data = repr_.as<RefPtr<WebIDLValueData>>();
    if (data) {
      data->trace(trc);
    }
  }
}

void WebIDLValueData::trace(JSTracer* trc) {
  data_.match(
      [trc](SequenceData& s) {
        for (WebIDLValue& e : s.elements) {
          e.trace(trc);
        }
      },
      [trc](DictionaryData& d) {
        for (WebIDLValue& m : d.members) {
          m.trace(trc);
        }
      },
      [trc](RecordData& r) {
        for (JS::Heap<JSString*>& k : r.keys) {
          JS::TraceEdge(trc, &k, "WebIDLValue record key");
        }
        for (WebIDLValue& v : r.values) {
          v.trace(trc);
        }
      });
}
