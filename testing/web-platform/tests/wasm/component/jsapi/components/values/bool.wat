(component
  (import "echo" (func $echo (param "x" bool) (result bool)))
  (import "recv" (func $recv (param "x" bool)))
  (core func $echo' (canon lower (func $echo)))
  (core func $recv' (canon lower (func $recv)))
  (core module $M
    (import "" "echo" (func $echo (param i32) (result i32)))
    (import "" "recv" (func $recv (param i32)))
    (func (export "roundtrip") (param i32) (result i32)
      (call $echo (local.get 0)))
    ;; Lifting a bool treats any non-zero i32 as true.
    (func (export "send")
      (call $recv (i32.const 0))
      (call $recv (i32.const 1))
      (call $recv (i32.const 2))
      (call $recv (i32.const -1))))
  (core instance $m (instantiate $M (with "" (instance
    (export "echo" (func $echo'))
    (export "recv" (func $recv'))))))
  (func $roundtrip (param "x" bool) (result bool) (canon lift (core func $m "roundtrip")))
  (func $send (canon lift (core func $m "send")))
  (export "roundtrip" (func $roundtrip))
  (export "send" (func $send))
)
