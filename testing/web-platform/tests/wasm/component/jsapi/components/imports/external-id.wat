;; Imports carrying an `external-id`, which replaces the JS name as the lookup
;; key on the imports object. Covers every sort the JS-API reads.
(component
  (import "log" (external-id "https://example.com/log") (func $log (param "message" string)))
  (import "dup1" (external-id "same") (func $dup1 (result u32)))
  (import "dup2" (external-id "same") (func $dup2 (result u32)))
  (import "r" (external-id "resource id") (type $r (sub resource)))
  (import "cfg" (external-id "value id") (value $cfg u32))
  (import "iface" (external-id "instance id") (instance $iface (export "f" (func (result u32)))))
  (alias export $iface "f" (func $f))

  (core module $Memory
    (memory (export "mem") 1)
    (global $next (mut i32) (i32.const 4096))
    (func (export "realloc") (param $old i32) (param $old-size i32) (param $align i32) (param $new-size i32) (result i32)
      (local $ptr i32) (local $pages i32)
      (local.set $ptr
        (i32.and
          (i32.add (global.get $next) (i32.sub (local.get $align) (i32.const 1)))
          (i32.sub (i32.const 0) (local.get $align))))
      (global.set $next (i32.add (local.get $ptr) (local.get $new-size)))
      (local.set $pages
        (i32.sub
          (i32.shr_u (i32.add (global.get $next) (i32.const 65535)) (i32.const 16))
          (memory.size)))
      (if (i32.gt_s (local.get $pages) (i32.const 0))
        (then (drop (memory.grow (local.get $pages)))))
      (if (local.get $old)
        (then (memory.copy (local.get $ptr) (local.get $old) (select (local.get $old-size) (local.get $new-size) (i32.lt_u (local.get $old-size) (local.get $new-size))))))
      (local.get $ptr)))
  (core instance $memory (instantiate $Memory))

  (core func $log' (canon lower (func $log) (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (core func $dup1' (canon lower (func $dup1)))
  (core func $dup2' (canon lower (func $dup2)))
  (core func $f' (canon lower (func $f)))
  (core module $M
    (import "" "mem" (memory 1))
    (import "" "log" (func $log (param i32 i32)))
    (import "" "dup1" (func $dup1 (result i32)))
    (import "" "dup2" (func $dup2 (result i32)))
    (import "" "f" (func $f (result i32)))
    (data (i32.const 1024) "hello")
    (func (export "run") (call $log (i32.const 1024) (i32.const 5)))
    (func (export "sum") (result i32)
      (i32.add (i32.add (call $dup1) (call $dup2)) (call $f))))
  (core instance $m (instantiate $M (with "" (instance
    (export "mem" (memory $memory "mem"))
    (export "log" (func $log'))
    (export "dup1" (func $dup1'))
    (export "dup2" (func $dup2'))
    (export "f" (func $f'))))))
  (func $run (canon lift (core func $m "run") (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (func $sum (result u32) (canon lift (core func $m "sum")))
  (export "run" (func $run))
  (export "sum" (func $sum))
  (export "cfg" (value $cfg))
)
