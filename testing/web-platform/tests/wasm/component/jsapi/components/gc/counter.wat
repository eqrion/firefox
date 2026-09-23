;; A guest resource whose destructor is observable through `drop-count`.
;; `take` consumes an own handle inside the guest so JS can check that a
;; transferred instance is never destroyed a second time by the collector.
(component
  (core module $Indirect
    (table (export "ftbl") 1 funcref)
    (type $FT (func (param i32)))
    (func (export "dtor") (param i32)
      (call_indirect (type $FT) (local.get 0) (i32.const 0))))
  (core instance $indirect (instantiate $Indirect))

  (type $counter (resource (rep i32) (dtor (core func $indirect "dtor"))))
  (core func $new (canon resource.new $counter))
  (core func $drop (canon resource.drop $counter))

  (core module $M
    (import "" "ftbl" (table 1 funcref))
    (import "" "new" (func $new (param i32) (result i32)))
    (import "" "drop" (func $drop (param i32)))
    (global $drops (mut i32) (i32.const 0))
    (func (export "ctor") (param $value i32) (result i32)
      (call $new (local.get $value)))
    (func (export "get") (param $rep i32) (result i32)
      (local.get $rep))
    (func (export "take") (param $handle i32)
      (call $drop (local.get $handle)))
    (func (export "drop-count") (result i32)
      (global.get $drops))
    (func $dtor (param $rep i32)
      (global.set $drops (i32.add (global.get $drops) (i32.const 1))))
    (elem (i32.const 0) $dtor))
  (core instance $m (instantiate $M (with "" (instance
    (export "ftbl" (table $indirect "ftbl"))
    (export "new" (func $new))
    (export "drop" (func $drop))))))

  (func $ctor (param "value" u32) (result (own $counter)) (canon lift (core func $m "ctor")))
  (func $get (param "self" (borrow $counter)) (result u32) (canon lift (core func $m "get")))
  (func $take (param "c" (own $counter)) (canon lift (core func $m "take")))
  (func $drop-count (result u32) (canon lift (core func $m "drop-count")))

  (export $counter' "counter" (type $counter))
  (export "[constructor]counter" (func $ctor) (func (param "value" u32) (result (own $counter'))))
  (export "[method]counter.get" (func $get) (func (param "self" (borrow $counter')) (result u32)))
  (export "take" (func $take) (func (param "c" (own $counter'))))
  (export "drop-count" (func $drop-count))
)
