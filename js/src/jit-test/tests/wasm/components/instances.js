// |jit-test| skip-if: !wasmComponentsEnabled()

// Instance imports. The JS-API behavior is also covered by the WPT tests in
// wasm/component/jsapi.

function componentBinary(sections) {
  return new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x01, 0x00, ...sections]);
}

// ----------------------------------------------------------------------------
// Validation

wasmValidateText(`(component
  (import "i" (instance
    (export "f" (func (param "x" u32) (result u32)))
    (export "n" (instance (export "g" (func))))))
  (alias export 0 "f" (func))
  (alias export 0 "n" (instance))
  (alias export 1 "g" (func))
)`);

// Types exported by an instance type can be used by later declarations, and
// aliased out of an instance.
wasmValidateText(`(component
  (import "i" (instance
    (type $point (record (field "x" u32) (field "y" u32)))
    (export "point" (type $p (eq $point)))
    (export "f" (func (param "p" $p) (result $p)))))
  (alias export 0 "point" (type $p))
  (type (list $p))
)`);

// Instance types can alias types from the enclosing component.
wasmValidateText(`(component
  (type $point (record (field "x" u32) (field "y" u32)))
  (import "i" (instance
    (export "f" (func (param "p" $point)))))
)`);

// Instance types can declare abstract resource types, but not define them.
wasmValidateText(`(component
  (import "i" (instance
    (export "r" (type $r (sub resource)))
    (export "f" (func (param "r" (borrow $r))))))
)`);
wasmFailValidateBinary(componentBinary([
  0x07, 0x07, 0x01,              // type section, 1 type
  0x42, 0x01,                    // instance type with 1 declaration
  0x01, 0x3f, 0x7f, 0x00,        // type (resource (rep i32))
]), /resource types cannot be defined in instance types/);

wasmFailValidateText(`(component
  (import "i" (instance
    (export "f" (func))
    (export "F" (func))))
)`, /export name "F" is not strongly-unique/);

wasmFailValidateText(`(component
  (type (func))
  (import "i" (instance (type 0)))
)`, /type 0 is not an instance type/);

wasmFailValidateText(`(component
  (import "i" (instance (export "f" (func))))
  (alias export 0 "g" (func))
)`, /instance 0 has no export "g"/);

wasmFailValidateText(`(component
  (import "i" (instance (export "f" (func))))
  (alias export 0 "f" (instance))
)`, /export "f" of instance 0 has a different sort/);

wasmFailValidateText(`(component
  (import "i" (instance (export "f" (func))))
  (alias export 1 "f" (func))
)`, /invalid instance index 1/);

wasmFailValidateBinary(componentBinary([
  0x07, 0x08, 0x01,              // type section, 1 type
  0x42, 0x01,                    // instance type with 1 declaration
  0x02, 0x03, 0x02, 0x02, 0x00,  // alias outer 2 0 (type)
]), /invalid outer alias count 2/);

wasmFailValidateBinary(componentBinary([
  0x07, 0x08, 0x01,              // type section, 1 type
  0x42, 0x01,                    // instance type with 1 declaration
  0x02, 0x03, 0x02, 0x01, 0x00,  // alias outer 1 0 (type)
]), /invalid type index 0/);

wasmFailValidateBinary(componentBinary([
  0x07, 0x08, 0x01,              // type section, 1 type
  0x42, 0x01,                    // instance type with 1 declaration
  0x02, 0x01, 0x00, 0x00, 0x00,  // alias export 0 "" (func)
]), /instance types can only contain outer aliases/);

// ----------------------------------------------------------------------------
// Instantiation

// `f` and `n.g` are lowered from the instance import `i`, and `h` from the
// top-level import after it.
const component = new WebAssembly.Component(wasmTextToBinary(`(component
  (import "i" (instance $i
    (type $point (record (field "x" u32) (field "y" u32)))
    (export "point" (type $p (eq $point)))
    (export "f" (func (param "p" $p) (result u32)))
    (export "n" (instance (export "g" (func (result u32)))))))
  (import "h" (func $h (result u32)))
  (alias export $i "point" (type $point))
  (alias export $i "f" (func $f))
  (alias export $i "n" (instance $n))
  (alias export $n "g" (func $g))
  (core func $f' (canon lower (func $f)))
  (core func $g' (canon lower (func $g)))
  (core func $h' (canon lower (func $h)))
  (core module $m
    (import "" "f" (func $f (param i32 i32) (result i32)))
    (import "" "g" (func $g (result i32)))
    (import "" "h" (func $h (result i32)))
    (func (export "call-f") (param i32 i32) (result i32)
      (call $f (local.get 0) (local.get 1)))
    (func (export "call-g") (result i32) (call $g))
    (func (export "call-h") (result i32) (call $h)))
  (core instance $m (instantiate $m (with "" (instance
    (export "f" (func $f'))
    (export "g" (func $g'))
    (export "h" (func $h'))))))
  (func (export "call-f") (param "p" $point) (result u32)
    (canon lift (core func $m "call-f")))
  (func (export "call-g") (result u32) (canon lift (core func $m "call-g")))
  (func (export "call-h") (result u32) (canon lift (core func $m "call-h")))
)`));

function makeImports() {
  return {
    i: { f: ({ x, y }) => x * 10 + y, n: { g: () => 2 } },
    h: () => 3,
  };
}

{
  const { callF, callG, callH } =
    new WebAssembly.ComponentInstance(component, makeImports()).exports;
  assertEq(callF({ x: 4, y: 5 }), 45);
  assertEq(callG(), 2);
  assertEq(callH(), 3);
}

// Imports are read in declaration order, and type exports are not read.
{
  const log = [];
  function logging(target, prefix) {
    return new Proxy(target, {
      get(t, key, r) {
        log.push(prefix + String(key));
        return Reflect.get(t, key, r);
      },
    });
  }
  const imports = makeImports();
  imports.i.n = logging(imports.i.n, "i.n.");
  imports.i = logging(imports.i, "i.");
  new WebAssembly.ComponentInstance(component, logging(imports, ""));
  assertEq(log.join(), "i,i.f,i.n,i.n.g,h");
}

for (const value of [undefined, null, 1, "i", Symbol(), true]) {
  const imports = makeImports();
  imports.i = value;
  assertErrorMessage(() => new WebAssembly.ComponentInstance(component, imports),
                     WebAssembly.LinkError, /'i' is not a object/);
}

{
  const imports = makeImports();
  delete imports.i.n.g;
  assertErrorMessage(() => new WebAssembly.ComponentInstance(component, imports),
                     WebAssembly.LinkError, /'g' is not a function/);
}

// A function is an object, so it can be an instance.
{
  const imports = makeImports();
  imports.i = Object.assign(() => {}, imports.i);
  const { callF } = new WebAssembly.ComponentInstance(component, imports).exports;
  assertEq(callF({ x: 1, y: 2 }), 12);
}
