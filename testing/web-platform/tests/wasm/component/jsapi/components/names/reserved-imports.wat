;; `constructor` and `prototype` are only reserved for exported members. An
;; import named that way just reads the property of that name.
(component
  (import "r" (type $r (sub resource)))
  (import "[method]r.constructor" (func $ctor (param "self" (borrow $r)) (result u32)))
  (import "[static]r.prototype" (func $proto (result u32)))
  (core func $ctor' (canon lower (func $ctor)))
  (core func $proto' (canon lower (func $proto)))
  (core func $drop-r (canon resource.drop $r))
  (core module $M
    (import "" "ctor" (func $ctor (param i32) (result i32)))
    (import "" "proto" (func $proto (result i32)))
    (import "" "drop-r" (func $drop-r (param i32)))
    (func (export "call-constructor") (param i32) (result i32) (local $result i32)
      (local.set $result (call $ctor (local.get 0)))
      (call $drop-r (local.get 0))
      (local.get $result))
    (func (export "call-prototype") (result i32) (call $proto)))
  (core instance $m (instantiate $M (with "" (instance
    (export "ctor" (func $ctor'))
    (export "proto" (func $proto'))
    (export "drop-r" (func $drop-r))))))
  (func $call-constructor (param "self" (borrow $r)) (result u32) (canon lift (core func $m "call-constructor")))
  (func $call-prototype (result u32) (canon lift (core func $m "call-prototype")))
  (export "call-constructor" (func $call-constructor))
  (export "call-prototype" (func $call-prototype))
)
