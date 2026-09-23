;; Imports a core module, which ESM satisfies with a source phase import.
(component
  (import "m" (core module $M (export "f" (func (result i32)))))
  (core instance $m (instantiate $M))
  (func $call-f (result u32) (canon lift (core func $m "f")))
  (export "call-f" (func $call-f))
)
