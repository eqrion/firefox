;; A core module import, instantiated inside the component.
(component
  (import "m" (core module $M (export "add" (func (param i32 i32) (result i32)))))
  (core instance $m (instantiate $M))
  (func $add (param "a" u32) (param "b" u32) (result u32) (canon lift (core func $m "add")))
  (export "add" (func $add))
)
