;; A guest resource without a `[constructor]` export.
(component
  (type $counter (resource (rep i32)))
  (core func $resource.new (canon resource.new $counter))

  (core module $CM
    (import "" "resource.new" (func $resource.new (param i32) (result i32)))
    (func (export "make") (result i32) (call $resource.new (i32.const 42)))
    (func (export "value") (param $rep i32) (result i32) (local.get $rep)))
  (core instance $cm (instantiate $CM (with "" (instance
    (export "resource.new" (func $resource.new))))))

  (export $counter' "counter" (type $counter))
  (func $make (result (own $counter')) (canon lift (core func $cm "make")))
  (export "make" (func $make))
  (func $value (param "self" (borrow $counter')) (result u32) (canon lift (core func $cm "value")))
  (export "[method]counter.value" (func $value))
)
