;; A start function that traps after calling an import.
(component
  (import "notify" (func $notify))
  (core func $notify' (canon lower (func $notify)))
  (core module $M
    (import "" "notify" (func $notify))
    (func (export "start") (call $notify) unreachable))
  (core instance $m (instantiate $M (with "" (instance (export "notify" (func $notify'))))))
  (func $start (canon lift (core func $m "start")))
  (start $start)
)
