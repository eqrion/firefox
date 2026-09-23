;; A guest resource whose destructor traps.
(component
  (core module $Dtor
    (func (export "dtor") (param i32) (unreachable)))
  (core instance $dtor (instantiate $Dtor))

  (type $counter (resource (rep i32) (dtor (core func $dtor "dtor"))))
  (core func $resource.new (canon resource.new $counter))
  (core func $resource.drop (canon resource.drop $counter))

  (core module $CM
    (import "" "resource.new" (func $resource.new (param i32) (result i32)))
    (import "" "resource.drop" (func $resource.drop (param i32)))
    (func (export "ctor") (result i32) (call $resource.new (i32.const 1)))
    (func (export "take") (param $h i32) (call $resource.drop (local.get $h)))
    (func (export "value") (param $rep i32) (result i32) (local.get $rep)))
  (core instance $cm (instantiate $CM (with "" (instance
    (export "resource.new" (func $resource.new))
    (export "resource.drop" (func $resource.drop))))))

  (export $counter' "counter" (type $counter))
  (func $ctor (result (own $counter')) (canon lift (core func $cm "ctor")))
  (export "[constructor]counter" (func $ctor))
  (func $value (param "self" (borrow $counter')) (result u32) (canon lift (core func $cm "value")))
  (export "[method]counter.value" (func $value))
  (func $take (param "c" (own $counter')) (canon lift (core func $cm "take")))
  (export "take" (func $take))
)
