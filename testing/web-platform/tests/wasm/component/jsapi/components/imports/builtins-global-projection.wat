;; Imports of paths below wasm:js/global, which project properties off of the
;; global object.
(component
  (import "wasm:js/global/component-projection" (instance $p
    (export "value" (func (result u32)))))
  (import "wasm:js/global/component-projection/nested/value" (func $nested-value (result u32)))
  (alias export $p "value" (func $value))
  (core func $value' (canon lower (func $value)))
  (core func $nested-value' (canon lower (func $nested-value)))
  (func $instance-value (result u32) (canon lift (core func $value')))
  (func $nested (result u32) (canon lift (core func $nested-value')))
  (export "instance-value" (func $instance-value))
  (export "nested-value" (func $nested))
)
