;; `constructor` as a static and `prototype` as a method are not reserved.
(component
  (type $counter (resource (rep i32)))
  (canon resource.new $counter (core func $new))
  (core module $M
    (import "" "new" (func $new (param i32) (result i32)))
    (func (export "ctor") (result i32) (call $new (i32.const 1)))
    (func (export "method") (param i32) (result i32) (i32.const 2))
    (func (export "static") (result i32) (i32.const 3)))
  (core instance $m (instantiate $M (with "" (instance (export "new" (func $new))))))
  (export $c "counter" (type $counter))
  (func $ctor (result (own $c)) (canon lift (core func $m "ctor")))
  (func $method (param "self" (borrow $c)) (result u32) (canon lift (core func $m "method")))
  (func $static (result u32) (canon lift (core func $m "static")))
  (export "[constructor]counter" (func $ctor))
  (export "[method]counter.prototype" (func $method))
  (export "[static]counter.constructor" (func $static))
)
