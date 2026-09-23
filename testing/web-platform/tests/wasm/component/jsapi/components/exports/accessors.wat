;; Property exports on the exports object: a read-write `counter` backed by a
;; core global and a read-only `read-only`.
(component
  (core module $M
    (global $counter (mut i32) (i32.const 0))
    (func (export "get-counter") (result i32) (global.get $counter))
    (func (export "set-counter") (param i32) (global.set $counter (local.get 0)))
    (func (export "get-read-only") (result i32) (i32.const 7)))
  (core instance $m (instantiate $M))
  (func $get-counter (result u32) (canon lift (core func $m "get-counter")))
  (func $set-counter (param "value" u32) (canon lift (core func $m "set-counter")))
  (func $get-read-only (result u32) (canon lift (core func $m "get-read-only")))
  (export "[get]counter" (func $get-counter))
  (export "[set]counter" (func $set-counter))
  (export "[get]read-only" (func $get-read-only))
  (export "plain" (func $get-read-only))
)
