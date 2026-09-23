;; Two function imports, to check that the first failure stops reading.
(component
  (import "first" (func $first (result u32)))
  (import "second" (func $second (result u32)))
  (core func $first' (canon lower (func $first)))
  (core func $second' (canon lower (func $second)))
  (core module $M
    (import "" "first" (func $first (result i32)))
    (import "" "second" (func $second (result i32)))
    (func (export "run") (result i32) (i32.add (call $first) (call $second))))
  (core instance $m (instantiate $M (with "" (instance
    (export "first" (func $first'))
    (export "second" (func $second'))))))
  (func $run (result u32) (canon lift (core func $m "run")))
  (export "run" (func $run))
)
