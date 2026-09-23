;; Imports a function that does not exist on the global object.
(component
  (import "wasm:js/global" (instance $g (export "no-such-fn" (func (result u32)))))
  (alias export $g "no-such-fn" (func $f))
  (core func $f' (canon lower (func $f)))
  (core module $M
    (import "" "f" (func $f (result i32)))
    (func (export "run") (result i32) (call $f)))
  (core instance $m (instantiate $M (with "" (instance (export "f" (func $f'))))))
  (func $run (result u32) (canon lift (core func $m "run")))
  (export "run" (func $run))
)
