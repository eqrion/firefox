;; One resource exported under two names: `counter` is the abstract export
;; and `counter2` is transparently equal to it. `iface` re-exports it again.
(component
  (type $counter (resource (rep i32)))
  (core func $resource.new (canon resource.new $counter))

  (core module $CM
    (import "" "resource.new" (func $resource.new (param i32) (result i32)))
    (func (export "ctor") (param i32) (result i32) (call $resource.new (local.get 0)))
    (func (export "increment") (param $rep i32) (result i32) (i32.add (local.get $rep) (i32.const 1)))
    (func (export "decrement") (param $rep i32) (result i32) (i32.sub (local.get $rep) (i32.const 1))))
  (core instance $cm (instantiate $CM (with "" (instance
    (export "resource.new" (func $resource.new))))))

  (export $c1 "counter" (type $counter))
  (export $c2 "counter2" (type $c1))

  (func $ctor (param "start" u32) (result (own $c1)) (canon lift (core func $cm "ctor")))
  (export "[constructor]counter" (func $ctor))
  (func $increment (param "self" (borrow $c1)) (result u32) (canon lift (core func $cm "increment")))
  (export "[method]counter.increment" (func $increment))
  (func $decrement (param "self" (borrow $c2)) (result u32) (canon lift (core func $cm "decrement")))
  (export "[method]counter2.decrement" (func $decrement))
  (func $create (param "start" u32) (result (own $c2)) (canon lift (core func $cm "ctor")))
  (export "[static]counter2.create" (func $create))

  (instance $iface
    (export "counter" (type $c1)))
  (export "iface" (instance $iface))
)
