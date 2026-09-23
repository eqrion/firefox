;; A component with no imports: a function, a resource class and an exported
;; instance, so the module namespace shows every kind of binding.
(component
  (type $counter (resource (rep i32)))
  (core func $new (canon resource.new $counter))
  (core module $M
    (import "" "new" (func $new (param i32) (result i32)))
    (func (export "answer") (result i32) (i32.const 42))
    (func (export "double") (param i32) (result i32) (i32.mul (local.get 0) (i32.const 2)))
    (func (export "ctor") (param i32) (result i32) (call $new (local.get 0)))
    (func (export "get") (param i32) (result i32) (local.get 0)))
  (core instance $m (instantiate $M (with "" (instance (export "new" (func $new))))))
  (func $answer (result u32) (canon lift (core func $m "answer")))
  (func $double (param "x" u32) (result u32) (canon lift (core func $m "double")))
  (func $ctor (param "value" u32) (result (own $counter)) (canon lift (core func $m "ctor")))
  (func $get (param "self" (borrow $counter)) (result u32) (canon lift (core func $m "get")))
  (export "answer" (func $answer))
  (export $counter' "counter" (type $counter))
  (export "[constructor]counter" (func $ctor) (func (param "value" u32) (result (own $counter'))))
  (export "[method]counter.get" (func $get) (func (param "self" (borrow $counter')) (result u32)))
  (instance $math (export "double" (func $double)))
  (export "math" (instance $math))
)
