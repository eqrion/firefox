;; A guest resource whose constructor returns `result<own<counter>, string>`;
;; a `start` of 0 is the error case and a `start` of 0xffffffff traps.
(component
  (core module $Dtor
    (global $count (mut i32) (i32.const 0))
    (func (export "dtor") (param i32)
      (global.set $count (i32.add (global.get $count) (i32.const 1))))
    (func (export "drop-count") (result i32) (global.get $count)))
  (core instance $dtor (instantiate $Dtor))

  (type $counter (resource (rep i32) (dtor (core func $dtor "dtor"))))
  (core func $resource.new (canon resource.new $counter))

  (core module $CM
    (import "" "resource.new" (func $resource.new (param i32) (result i32)))
    (memory (export "mem") 1)
    (data (i32.const 8) "zero")
    (global $next (mut i32) (i32.const 1024))
    (func $alloc (param $size i32) (result i32)
      (local $ptr i32)
      (local.set $ptr (global.get $next))
      (global.set $next (i32.add (local.get $ptr) (local.get $size)))
      (local.get $ptr))
    (func (export "realloc") (param $old i32) (param $old-size i32) (param $align i32) (param $new-size i32) (result i32)
      (call $alloc (local.get $new-size)))
    (func (export "ctor") (param $start i32) (result i32)
      (local $ret i32) (local $rep i32)
      (if (i32.eq (local.get $start) (i32.const -1)) (then (unreachable)))
      (local.set $ret (call $alloc (i32.const 12)))
      (if (i32.eqz (local.get $start))
        (then
          (i32.store (local.get $ret) (i32.const 1))
          (i32.store offset=4 (local.get $ret) (i32.const 8))
          (i32.store offset=8 (local.get $ret) (i32.const 4)))
        (else
          (local.set $rep (call $alloc (i32.const 4)))
          (i32.store (local.get $rep) (local.get $start))
          (i32.store (local.get $ret) (i32.const 0))
          (i32.store offset=4 (local.get $ret) (call $resource.new (local.get $rep)))))
      (local.get $ret))
    (func (export "value") (param $rep i32) (result i32) (i32.load (local.get $rep))))
  (core instance $cm (instantiate $CM (with "" (instance
    (export "resource.new" (func $resource.new))))))

  (export $counter' "counter" (type $counter))
  (func $ctor (param "start" u32) (result (result (own $counter') (error string)))
    (canon lift (core func $cm "ctor") (memory (core memory $cm "mem")) (realloc (core func $cm "realloc"))))
  (export "[constructor]counter" (func $ctor))
  (func $value (param "self" (borrow $counter')) (result u32) (canon lift (core func $cm "value")))
  (export "[method]counter.value" (func $value))
  (func $drop-count (result u32) (canon lift (core func $dtor "drop-count")))
  (export "drop-count" (func $drop-count))
)
