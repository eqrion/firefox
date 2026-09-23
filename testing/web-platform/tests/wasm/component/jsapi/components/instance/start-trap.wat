;; The start function calls `recv(7)` and then traps.
(component
  (import "recv" (func $recv (param "x" u32)))
  (core func $recv' (canon lower (func $recv)))
  (core module $M
    (import "" "recv" (func $recv (param i32)))
    (func (export "run") (call $recv (i32.const 7)) (unreachable)))
  (core instance $m (instantiate $M (with "" (instance (export "recv" (func $recv'))))))
  (func $run (canon lift (core func $m "run")))
  (start $run)
)
