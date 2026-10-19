;; One export of every sort the JS-API gives a JS definition to: a function, a
;; resource type with a constructor and a method, an exported instance, a
;; value and a core module. Instantiate with `{ v: <u32> }`.
(component
  (import "v" (value $v u32))
  (type $counter (resource (rep i32)))
  (canon resource.new $counter (core func $new))
  (core module $M
    (import "" "resource.new" (func $new (param i32) (result i32)))
    (func (export "answer") (result i32) (i32.const 42))
    (func (export "ctor") (param i32) (result i32) (call $new (local.get 0)))
    (func (export "get") (param i32) (result i32) (local.get 0)))
  (core instance $m (instantiate $M (with "" (instance (export "resource.new" (func $new))))))
  (func $answer (result u32) (canon lift (core func $m "answer")))
  (export "answer" (func $answer))
  (export $counter' "counter" (type $counter))
  (func $ctor (param "n" u32) (result (own $counter')) (canon lift (core func $m "ctor")))
  (export "[constructor]counter" (func $ctor))
  (func $get (param "self" (borrow $counter')) (result u32) (canon lift (core func $m "get")))
  (export "[method]counter.get" (func $get))
  (instance $nested (export "answer" (func $answer)))
  (export "nested" (instance $nested))
  (export "the-answer" (value $v))
  (core module $Empty (func (export "nop")))
  (export "empty" (core module $Empty))
)
