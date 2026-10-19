;; A resource type import, which must be satisfied by a callable.
(component
  (import "r" (type $r (sub resource)))
  (import "[constructor]r" (func $make (result (own $r))))
  (core func $make' (canon lower (func $make)))
  (core module $M
    (import "" "make" (func $make (result i32)))
    (func (export "make") (result i32) (call $make)))
  (core instance $m (instantiate $M (with "" (instance (export "make" (func $make'))))))
  (func $make-export (result (own $r)) (canon lift (core func $m "make")))
  (export "make" (func $make-export))
)
