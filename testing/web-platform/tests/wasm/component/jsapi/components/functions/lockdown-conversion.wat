;; `take-point` takes a record whose first field needs `realloc` and whose
;; second is flat, so a bad `n` fails after the guest has been entered while a
;; bad `s` fails before. `flat` never enters the guest before the call.
(component
  (type $point (record (field "s" string) (field "n" u32)))
  (export $point' "point" (type $point))
  (core module $M
    (memory (export "mem") 1)
    (global $next (mut i32) (i32.const 16))
    (global $reallocs (mut i32) (i32.const 0))
    (func (export "realloc") (param $old i32) (param $old-size i32) (param $align i32) (param $new-size i32) (result i32)
      (local $ptr i32)
      (global.set $reallocs (i32.add (global.get $reallocs) (i32.const 1)))
      (local.set $ptr
        (i32.and
          (i32.add (global.get $next) (i32.sub (local.get $align) (i32.const 1)))
          (i32.sub (i32.const 0) (local.get $align))))
      (global.set $next (i32.add (local.get $ptr) (local.get $new-size)))
      (local.get $ptr))
    (func (export "flat") (param i32) (result i32) (local.get 0))
    (func (export "take-point") (param $ptr i32) (param $len i32) (param $n i32) (result i32) (local.get $n))
    (func (export "reallocs") (result i32) (global.get $reallocs)))
  (core instance $m (instantiate $M))
  (func $flat (param "x" u32) (result u32) (canon lift (core func $m "flat")))
  (func $take-point (param "p" $point') (result u32)
    (canon lift (core func $m "take-point")
      (memory (core memory $m "mem")) (realloc (core func $m "realloc"))))
  (func $reallocs (result u32) (canon lift (core func $m "reallocs")))
  (export "flat" (func $flat))
  (export "take-point" (func $take-point))
  (export "reallocs" (func $reallocs))
)
