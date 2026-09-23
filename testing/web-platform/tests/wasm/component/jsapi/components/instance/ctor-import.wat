;; Imports a resource type and its constructor; `make` calls the constructor.
(component
  (import "counter" (type $counter (sub resource)))
  (import "[constructor]counter" (func $ctor (param "n" u32) (result (own $counter))))
  (core func $ctor' (canon lower (func $ctor)))
  (core module $M
    (import "" "ctor" (func $ctor (param i32) (result i32)))
    (func (export "make") (param i32) (result i32) (call $ctor (local.get 0))))
  (core instance $m (instantiate $M (with "" (instance (export "ctor" (func $ctor'))))))
  (func $make (param "n" u32) (result (own $counter)) (canon lift (core func $m "make")))
  (export "make" (func $make))
)
