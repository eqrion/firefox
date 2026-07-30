/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsapi-tests/tests.h"

#ifdef ENABLE_WASM_COMPONENTS

#  include "js/experimental/WebIDL.h"
#  include "js/PropertyAndElement.h"  // JS_NewPlainObject, JS_DefineProperty, JS_GetProperty
#  include "js/Utility.h"  // js_new, js::DuplicateString
#  include "wasm/WasmComponent.h"

using namespace js;
using namespace js::wasm;

namespace {

// A fake refcounted native type, standing in for a real Gecko DOM object.
struct FakeNative {
  int32_t refcount = 1;
  int32_t tag;
  explicit FakeNative(int32_t tag) : tag(tag) {}
};

void FakeNative_addRef(void* p) { static_cast<FakeNative*>(p)->refcount++; }
void FakeNative_release(void* p) {
  auto* n = static_cast<FakeNative*>(p);
  if (--n->refcount == 0) {
    js_delete(n);
  }
}
JSObject* FakeNative_reflect(JSContext* cx, void* p) {
  auto* n = static_cast<FakeNative*>(p);
  JS::Rooted<JSObject*> obj(cx, JS_NewPlainObject(cx));
  if (!obj) {
    return nullptr;
  }
  if (!JS_DefineProperty(cx, obj, "tag", n->tag, JSPROP_ENUMERATE)) {
    return nullptr;
  }
  return obj;
}

// WebIDLInterface::unwrapNative's hook is a raw function pointer (no
// closures allowed), so the test drives it through this file-scope slot
// instead of a capturing lambda.
FakeNative* gAdoptTestNative = nullptr;
void* GetAdoptTestNative(JSObject*) { return gAdoptTestNative; }
void* GetAdoptTestNativeFails(JSObject*) { return nullptr; }

}  // namespace

// hasNativeOps()/addRefNative()/releaseNative()/reflect() dispatch to the
// embedder-supplied ops, and are absent (safe to query, not to call) when
// an interface declares none.
BEGIN_TEST(testWebIDLResourceRepr_interfaceOps) {
  RefPtr<JS::WebIDLInterface> withOps = js_new<JS::WebIDLInterface>(
      DuplicateString("WithOps"), nullptr, FakeNative_addRef,
      FakeNative_release, FakeNative_reflect);
  CHECK(withOps);
  CHECK(withOps->hasNativeOps());

  RefPtr<JS::WebIDLInterface> noOps =
      js_new<JS::WebIDLInterface>(DuplicateString("NoOps"), nullptr);
  CHECK(noOps);
  CHECK(!noOps->hasNativeOps());

  FakeNative* native = js_new<FakeNative>(42);
  CHECK(native);
  CHECK(native->refcount == 1);
  withOps->addRefNative(native);
  CHECK(native->refcount == 2);
  withOps->releaseNative(native);
  CHECK(native->refcount == 1);
  withOps->releaseNative(native);  // drops to 0, deletes `native`
  return true;
}
END_TEST(testWebIDLResourceRepr_interfaceOps)

// ComponentHandleTable's dual repr, exercised through add/get/drop (the real
// insertion/release path, not just a standalone value wrapper): a Native
// entry takes a ref on add and releases it exactly once on drop; a Wrapped
// entry round-trips the plain object with no ref-counting involved.
BEGIN_TEST(testWebIDLResourceRepr_dualRepr) {
  RefPtr<JS::WebIDLInterface> iface = js_new<JS::WebIDLInterface>(
      DuplicateString("R"), nullptr, FakeNative_addRef, FakeNative_release,
      FakeNative_reflect);
  CHECK(iface);
  FakeNative* native = js_new<FakeNative>(7);
  CHECK(native);

  {
    ComponentHandleTable table;
    ComponentResourceClassification classification{true, native, iface.get(),
                                                   nullptr};
    uint32_t index;
    CHECK(table.add(cx, ComponentResourceTypeId(), /* own */ true,
                    classification, &index));
    CHECK(native->refcount == 2);  // the table now holds a ref

    ComponentResourceHandle handle;
    CHECK(table.get(cx, index, &handle));
    CHECK(handle.isNative);
    CHECK(handle.nativePtr == native);
    CHECK(handle.nativeIface == iface.get());
    CHECK(handle.resolveNative(nullptr) == native);

    JS::RootedObject reflected(cx, handle.reflectToJS(cx));
    CHECK(reflected);
    RootedValue tagVal(cx);
    CHECK(JS_GetProperty(cx, reflected, "tag", &tagVal));
    CHECK(tagVal.isInt32());
    CHECK(tagVal.toInt32() == 7);

    CHECK(table.drop(cx, index, /* requireOwn */ false));
    CHECK(native->refcount == 1);  // drop released the table's ref exactly once
  }

  {
    JS::RootedObject obj(cx, JS_NewPlainObject(cx));
    CHECK(obj);
    ComponentHandleTable table;
    ComponentResourceClassification classification{false, nullptr, nullptr,
                                                   obj};
    uint32_t index;
    CHECK(table.add(cx, ComponentResourceTypeId(), /* own */ true,
                    classification, &index));

    ComponentResourceHandle handle;
    CHECK(table.get(cx, index, &handle));
    CHECK(!handle.isNative);
    CHECK(handle.wrappedObj == obj);
    CHECK(handle.reflectToJS(cx) == obj);

    CHECK(table.drop(cx, index, /* requireOwn */ false));
  }

  FakeNative_release(native);
  return true;
}
END_TEST(testWebIDLResourceRepr_dualRepr)

// ClassifyResourceObject (the boundary classifier): same-origin + native ops
// + a successful unwrap -> Native; any of those failing -> Wrapped. Doesn't
// take a ref itself - that's ComponentHandleTable::add's job on insertion.
BEGIN_TEST(testWebIDLResourceRepr_classifyResourceObject) {
  RefPtr<JS::WebIDLInterface> withOps = js_new<JS::WebIDLInterface>(
      DuplicateString("WithOps"), nullptr, FakeNative_addRef,
      FakeNative_release, FakeNative_reflect, GetAdoptTestNative);
  CHECK(withOps);
  RefPtr<JS::WebIDLInterface> noOps =
      js_new<JS::WebIDLInterface>(DuplicateString("NoOps"), nullptr);
  CHECK(noOps);
  RefPtr<JS::WebIDLInterface> failingUnwrap = js_new<JS::WebIDLInterface>(
      DuplicateString("FailingUnwrap"), nullptr, FakeNative_addRef,
      FakeNative_release, FakeNative_reflect, GetAdoptTestNativeFails);
  CHECK(failingUnwrap);

  JS::RootedObject obj(cx, JS_NewPlainObject(cx));
  CHECK(obj);
  gAdoptTestNative = js_new<FakeNative>(1);
  CHECK(gAdoptTestNative);

  {
    ComponentResourceClassification c =
        ClassifyResourceObject(obj, /* isSameOrigin */ true, withOps.get());
    CHECK(c.isNative);
    CHECK(c.nativePtr == gAdoptTestNative);
  }
  {
    ComponentResourceClassification c =
        ClassifyResourceObject(obj, /* isSameOrigin */ false, withOps.get());
    CHECK(!c.isNative);
    CHECK(c.wrappedObj == obj);
  }
  {
    ComponentResourceClassification c =
        ClassifyResourceObject(obj, /* isSameOrigin */ true, noOps.get());
    CHECK(!c.isNative);
  }
  {
    ComponentResourceClassification c = ClassifyResourceObject(
        obj, /* isSameOrigin */ true, failingUnwrap.get());
    CHECK(!c.isNative);
  }

  FakeNative_release(gAdoptTestNative);
  gAdoptTestNative = nullptr;
  return true;
}
END_TEST(testWebIDLResourceRepr_classifyResourceObject)

// A `reflect`-less interface: reflectToJS returns null rather than
// crashing - the embedder must keep such a resource Wrapped.
BEGIN_TEST(testWebIDLResourceRepr_noReflect) {
  RefPtr<JS::WebIDLInterface> iface = js_new<JS::WebIDLInterface>(
      DuplicateString("R"), nullptr, FakeNative_addRef, FakeNative_release,
      nullptr);
  CHECK(iface);
  CHECK(iface->hasNativeOps());  // addRef/release alone are enough
  CHECK(!iface->reflect(cx, nullptr));
  return true;
}
END_TEST(testWebIDLResourceRepr_noReflect)

#endif  // ENABLE_WASM_COMPONENTS
