;; Trapping in the guest and in import result conversion. `ok` is a healthy
;; export used to observe that the instance is locked down afterwards.
(component
  (import "plain" (func $plain (result u32)))
  (core func $plain' (canon lower (func $plain)))
  (core module $M
    (import "" "plain" (func $plain (result i32)))
    (global $calls (mut i32) (i32.const 0))
    (func (export "ok") (result i32)
      (global.set $calls (i32.add (global.get $calls) (i32.const 1)))
      (global.get $calls))
    (func (export "trap") unreachable)
    (func (export "call-plain") (result i32) (call $plain)))
  (core instance $m (instantiate $M (with "" (instance (export "plain" (func $plain'))))))
  (func $ok (result u32) (canon lift (core func $m "ok")))
  (func $trap (canon lift (core func $m "trap")))
  (func $call-plain (result u32) (canon lift (core func $m "call-plain")))
  (export "ok" (func $ok))
  (export "trap" (func $trap))
  (export "call-plain" (func $call-plain))
)
