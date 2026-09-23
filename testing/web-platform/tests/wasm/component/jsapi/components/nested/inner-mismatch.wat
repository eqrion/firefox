;; Has the right import but the wrong export type, so it does not satisfy the
;; component type `component-import.wat` imports.
(component
  (import "log" (func $log (param "message" string)))
  (core module $M (func (export "run") (result i32) (i32.const 1)))
  (core instance $m (instantiate $M))
  (func $run (result s32) (canon lift (core func $m "run")))
  (export "run" (func $run))
)
