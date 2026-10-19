;; Fixtures for argument counts: exports with 0, 1 and 3 parameters, an import
;; with 3 parameters whose JS `arguments.length` is observable, and a resource
;; whose method length excludes `self`.
(component
  (import "recv" (func $recv (param "a" u32) (param "b" u32) (param "c" u32)))
  (core func $recv' (canon lower (func $recv)))

  (type $counter (resource (rep i32)))
  (core func $new (canon resource.new $counter))

  (core module $M
    (import "" "recv" (func $recv (param i32 i32 i32)))
    (import "" "new" (func $new (param i32) (result i32)))
    (func (export "none"))
    (func (export "one") (param i32) (result i32) (local.get 0))
    (func (export "three") (param i32 i32 i32)
      (call $recv (local.get 0) (local.get 1) (local.get 2)))
    (func (export "make") (param i32) (result i32) (call $new (local.get 0)))
    (func (export "add") (param $self i32) (param $x i32) (param $y i32) (result i32)
      (i32.add (local.get $self) (i32.add (local.get $x) (local.get $y))))
    (func (export "twice") (param $x i32) (result i32) (i32.mul (local.get $x) (i32.const 2))))
  (core instance $m (instantiate $M (with "" (instance
    (export "recv" (func $recv'))
    (export "new" (func $new))))))

  (func $none (canon lift (core func $m "none")))
  (func $one (param "a" u32) (result u32) (canon lift (core func $m "one")))
  (func $three (param "a" u32) (param "b" u32) (param "c" u32) (canon lift (core func $m "three")))
  (func $make (param "start" u32) (result (own $counter)) (canon lift (core func $m "make")))
  (func $add (param "self" (borrow $counter)) (param "x" u32) (param "y" u32) (result u32)
    (canon lift (core func $m "add")))
  (func $twice (param "x" u32) (result u32) (canon lift (core func $m "twice")))

  (export "none" (func $none))
  (export "one" (func $one))
  (export "three" (func $three))
  (export $counter' "counter" (type $counter))
  (export "[constructor]counter" (func $make) (func (param "start" u32) (result (own $counter'))))
  (export "[method]counter.add" (func $add)
    (func (param "self" (borrow $counter')) (param "x" u32) (param "y" u32) (result u32)))
  (export "[static]counter.twice" (func $twice))
)
