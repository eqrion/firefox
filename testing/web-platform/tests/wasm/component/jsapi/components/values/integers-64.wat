;; Passthroughs for s64 and u64.
(component
  (import "echo-s64" (func $echo-s64 (param "x" s64) (result s64)))
  (import "echo-u64" (func $echo-u64 (param "x" u64) (result u64)))
  (core func $echo-s64' (canon lower (func $echo-s64)))
  (core func $echo-u64' (canon lower (func $echo-u64)))
  (core module $M
    (import "" "echo-s64" (func $echo-s64 (param i64) (result i64)))
    (import "" "echo-u64" (func $echo-u64 (param i64) (result i64)))
    (func (export "roundtrip-s64") (param i64) (result i64)
      (call $echo-s64 (local.get 0)))
    (func (export "roundtrip-u64") (param i64) (result i64)
      (call $echo-u64 (local.get 0)))
  )
  (core instance $m (instantiate $M (with "" (instance
    (export "echo-s64" (func $echo-s64'))
    (export "echo-u64" (func $echo-u64'))
  ))))
  (func $roundtrip-s64 (param "x" s64) (result s64) (canon lift (core func $m "roundtrip-s64")))
  (export "roundtrip-s64" (func $roundtrip-s64))
  (func $roundtrip-u64 (param "x" u64) (result u64) (canon lift (core func $m "roundtrip-u64")))
  (export "roundtrip-u64" (func $roundtrip-u64))
)
