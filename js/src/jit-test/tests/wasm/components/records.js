// |jit-test| skip-if: !wasmComponentsEnabled()

// Records in component adapters, flattened and in memory. The JS-API behavior
// is covered by the WPT tests in wasm/component/jsapi.

// `roundtrip` passes a record, flattened, and a list of them, in memory,
// through the import `echo`.
const component = new WebAssembly.Component(wasmTextToBinary(`(component
  (type $point (record (field "x" s32) (field "y-coord" f64)))
  (type $shape (record (field "name" string) (field "points" (list $point))))
  (import "point" (type $point' (eq $point)))
  (import "shape" (type $shape' (eq $shape)))
  (import "echo" (func $echo (param "s" $shape') (result $shape')))
  (core module $mem
    (memory (export "mem") 1)
    (global $bump (mut i32) (i32.const 16))
    (func (export "realloc") (param i32 i32) (param $align i32) (param $size i32) (result i32)
      (local $ptr i32)
      (local.set $ptr
        (i32.and (i32.add (global.get $bump) (i32.sub (local.get $align) (i32.const 1)))
                 (i32.sub (i32.const 0) (local.get $align))))
      (global.set $bump (i32.add (local.get $ptr) (local.get $size)))
      (local.get $ptr)))
  (core instance $mem (instantiate $mem))
  (core func $echo' (canon lower (func $echo) (memory (core memory $mem "mem"))
                    (realloc (core func $mem "realloc"))))
  (core module $m
    (import "" "echo" (func $echo (param i32 i32 i32 i32 i32)))
    (import "" "realloc" (func $realloc (param i32 i32 i32 i32) (result i32)))
    (func (export "roundtrip") (param i32 i32 i32 i32) (result i32)
      (local $ret i32)
      (local.set $ret (call $realloc (i32.const 0) (i32.const 0) (i32.const 4) (i32.const 16)))
      (call $echo (local.get 0) (local.get 1) (local.get 2) (local.get 3) (local.get $ret))
      (local.get $ret)))
  (core instance $i (instantiate $m (with "" (instance
    (export "echo" (func $echo'))
    (export "realloc" (func $mem "realloc"))))))
  (func $roundtrip (param "s" $shape') (result $shape')
    (canon lift (core func $i "roundtrip") (memory (core memory $mem "mem"))
                (realloc (core func $mem "realloc"))))
  (export "roundtrip" (func $roundtrip))
)`));

let seen;
const { roundtrip } = new WebAssembly.ComponentInstance(component, {
  echo: s => (seen = s),
}).exports;

const shape = { name: "tri", points: [{ x: -1, yCoord: 0.5 }, { x: 2, yCoord: -3 }] };
const result = roundtrip(shape);
assertEq(JSON.stringify(result), JSON.stringify(shape));
assertEq(JSON.stringify(seen), JSON.stringify(shape));
assertEq(Object.getPrototypeOf(result), null);
assertEq(Object.getPrototypeOf(result.points[0]), null);
assertDeepEq(Object.keys(result.points[0]), ["x", "yCoord"]);

// Fields are read in declaration order, with their JS names.
const log = [];
const logged = new Proxy({ name: "p", points: [] }, {
  get(target, key) { log.push(key); return target[key]; },
});
roundtrip(logged);
assertDeepEq(log, ["name", "points"]);

// A field that is missing, even if it's present in kebab-case, is undefined.
assertEq(roundtrip({ name: "p", points: [{ x: 1, "y-coord": 1 }] }).points[0].yCoord, NaN);
