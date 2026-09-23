// |jit-test| skip-if: !wasmComponentsEnabled()

// Functions with more than 16 flat params, which are passed in memory.

const params17 = Array.from({ length: 17 }, (_, i) => `(param "p${i}" u32)`).join(" ");
const f32Params = Array.from({ length: 14 }, (_, i) => `(param "f${i}" f32)`).join(" ");

// Lifted functions: the params are stored to memory allocated with realloc.
// `reallocs` returns the alignment and size of each realloc call since the
// last call.
{
  const { first, last, mixed, reallocs } = new WebAssembly.ComponentInstance(
    new WebAssembly.Component(wasmTextToBinary(`(component
      (core module $m
        (memory (export "mem") 1)
        (global $bump (mut i32) (i32.const 1024))
        (global $count (mut i32) (i32.const 0))
        (func (export "realloc") (param $old i32) (param $oldSize i32)
                                 (param $align i32) (param $newSize i32) (result i32)
          (local $ptr i32)
          (i32.store (i32.add (i32.const 16) (i32.shl (global.get $count) (i32.const 3)))
                     (local.get $align))
          (i32.store (i32.add (i32.const 20) (i32.shl (global.get $count) (i32.const 3)))
                     (local.get $newSize))
          (global.set $count (i32.add (global.get $count) (i32.const 1)))
          (local.set $ptr
            (i32.and (i32.add (global.get $bump) (i32.sub (local.get $align) (i32.const 1)))
                     (i32.sub (i32.const 0) (local.get $align))))
          (global.set $bump (i32.add (local.get $ptr) (local.get $newSize)))
          (local.get $ptr))
        (func (export "first") (param $p i32) (result i32)
          (i32.load (local.get $p)))
        (func (export "last") (param $p i32) (result i32)
          (i32.load offset=64 (local.get $p)))
        ;; (u8, u64, string, f32 x 14) is laid out at offsets 0, 8, 16 and 24.
        ;; Returns the u64 plus the string's length plus the last f32.
        (func (export "mixed") (param $p i32) (result i64)
          (i64.add
            (i64.add (i64.load offset=8 (local.get $p))
                     (i64.extend_i32_u (i32.load offset=20 (local.get $p))))
            (i64.trunc_f32_s (f32.load offset=76 (local.get $p)))))
        (func (export "reallocs") (result i32)
          (i32.store (i32.const 0) (i32.const 16))
          (i32.store (i32.const 4) (i32.shl (global.get $count) (i32.const 1)))
          (global.set $count (i32.const 0))
          (i32.const 0)))
      (core instance $i (instantiate $m))
      (func (export "first") ${params17} (result u32)
        (canon lift (core func $i "first")
          (memory (core memory $i "mem")) (realloc (core func $i "realloc"))))
      (func (export "last") ${params17} (result u32)
        (canon lift (core func $i "last")
          (memory (core memory $i "mem")) (realloc (core func $i "realloc"))))
      (func (export "mixed") (param "a" u8) (param "b" u64) (param "c" string)
                             ${f32Params} (result u64)
        (canon lift (core func $i "mixed")
          (memory (core memory $i "mem")) (realloc (core func $i "realloc"))))
      (func (export "reallocs") (result (list u32))
        (canon lift (core func $i "reallocs") (memory (core memory $i "mem"))))
    )`))).exports;

  const args = Array.from({ length: 17 }, (_, i) => i + 100);
  assertEq(first(...args), 100);
  assertEq(last(...args), 116);
  assertEq(reallocs().join(), "4,68,4,68");

  const floats = Array.from({ length: 14 }, (_, i) => i);
  assertEq(mixed(1, 1000n, "abc", ...floats), 1016n);
  assertEq(reallocs().join(), "8,80,1,3");

  // Arguments are converted after the params are allocated, so a failing
  // conversion locks down the instance.
  assertErrorMessage(() => first(...args.slice(0, 16), 1n), TypeError, /./);
  assertErrorMessage(() => first(...args), WebAssembly.RuntimeError,
                     /locked down/);
}

// Lowered functions: the params are loaded from memory at a pointer.
const lowered = new WebAssembly.Component(wasmTextToBinary(`(component
  (import "f" (func $f ${params17} (result u32)))
  (import "g" (func $g ${params17} (result (tuple u32 u32))))
  (core module $mem (memory (export "mem") 1))
  (core instance $mem (instantiate $mem))
  (core func $f' (canon lower (func $f) (memory (core memory $mem "mem"))))
  (core func $g' (canon lower (func $g) (memory (core memory $mem "mem"))))
  (core module $m
    (import "" "mem" (memory 1))
    (import "" "f" (func $f (param i32) (result i32)))
    (import "" "g" (func $g (param i32 i32)))
    (func $init (local $i i32)
      (loop $l
        (i32.store (i32.shl (local.get $i) (i32.const 2))
                   (i32.add (local.get $i) (i32.const 200)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br_if $l (i32.lt_u (local.get $i) (i32.const 17)))))
    (func (export "call-f") (param $p i32) (result i32)
      (call $init)
      (call $f (local.get $p)))
    (func (export "call-g") (result i32)
      (call $init)
      (call $g (i32.const 0) (i32.const 128))
      (i32.add (i32.load (i32.const 128)) (i32.load (i32.const 132)))))
  (core instance $m (instantiate $m (with "" (instance
    (export "mem" (memory $mem "mem"))
    (export "f" (func $f'))
    (export "g" (func $g'))))))
  (func (export "call-f") (param "p" u32) (result u32)
    (canon lift (core func $m "call-f")))
  (func (export "call-g") (result u32) (canon lift (core func $m "call-g")))
)`));

function instantiateLowered() {
  let received;
  const { callF, callG } = new WebAssembly.ComponentInstance(lowered, {
    f: (...args) => { received = args; return args[0] + args[16]; },
    g: (...args) => [args[1], args[15]],
  }).exports;
  return { callF, callG, received: () => received };
}

{
  const { callF, callG, received } = instantiateLowered();
  assertEq(callF(0), 416);
  assertEq(received().join(), Array.from({ length: 17 }, (_, i) => i + 200).join());
  assertEq(callG(), 201 + 215);
}

// The pointer must be aligned and in bounds.
for (const ptr of [2, 65536 - 64, 0xfffffffc]) {
  const { callF } = instantiateLowered();
  assertErrorMessage(() => callF(ptr), WebAssembly.RuntimeError, /./);
}
