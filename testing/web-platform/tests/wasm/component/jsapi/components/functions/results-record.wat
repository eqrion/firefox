;; A `result` whose error payload is a record. The record type has to be
;; imported for the import's type to be externally visible.
(component
  (type $err (record (field "code" u32) (field "message" string)))
  (import "err" (type $err' (eq $err)))
  (import "fallible-record" (func $fallible-record (result (result (error $err')))))

  (core module $Memory
    (memory (export "mem") 1)
    (global $next (mut i32) (i32.const 16))
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

  (core func $fallible-record' (canon lower (func $fallible-record)
    (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))

  (core module $M
    (import "" "realloc" (func $realloc (param i32 i32 i32 i32) (result i32)))
    (import "" "fallible-record" (func $fallible-record (param i32)))
    (func (export "call-fallible-record") (result i32)
      (local $ret i32)
      (local.set $ret (call $realloc (i32.const 0) (i32.const 0) (i32.const 4) (i32.const 16)))
      (call $fallible-record (local.get $ret))
      (local.get $ret)))
  (core instance $m (instantiate $M (with "" (instance
    (export "realloc" (func $memory "realloc"))
    (export "fallible-record" (func $fallible-record'))))))

  (func $call-fallible-record (result (result (error $err')))
    (canon lift (core func $m "call-fallible-record")
      (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (export "call-fallible-record" (func $call-fallible-record))
)
