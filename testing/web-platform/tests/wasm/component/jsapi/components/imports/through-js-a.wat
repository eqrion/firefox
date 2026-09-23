;; Producer side of the through-JS linking test: a function, a resource class
;; and an instance that instance B imports as ordinary JS values.
(component
  (type $counter (resource (rep i32)))
  (core func $new (canon resource.new $counter))
  (core module $M
    (import "" "new" (func $new (param i32) (result i32)))
    (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1)))
    (func (export "mul") (param i32 i32) (result i32) (i32.mul (local.get 0) (local.get 1)))
    (func (export "make") (param i32) (result i32) (call $new (local.get 0)))
    (func (export "get") (param i32) (result i32) (local.get 0)))
  (core instance $m (instantiate $M (with "" (instance (export "new" (func $new))))))
  (func $add (param "a" u32) (param "b" u32) (result u32) (canon lift (core func $m "add")))
  (func $mul (param "a" u32) (param "b" u32) (result u32) (canon lift (core func $m "mul")))
  (func $make (param "start" u32) (result (own $counter)) (canon lift (core func $m "make")))
  (func $get (param "self" (borrow $counter)) (result u32) (canon lift (core func $m "get")))
  (export "add" (func $add))
  (export $counter' "counter" (type $counter))
  (export "[constructor]counter" (func $make) (func (param "start" u32) (result (own $counter'))))
  (export "[method]counter.get" (func $get) (func (param "self" (borrow $counter')) (result u32)))
  (instance $iface (export "mul" (func $mul)))
  (export "iface" (instance $iface))
)
