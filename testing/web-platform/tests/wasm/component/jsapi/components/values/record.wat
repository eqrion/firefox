;; Passthrough for a record with string, integer, option and nested-option
;; fields. The record type must be imported by name so that it can appear in
;; the signatures of the import and the export.
(component
  (type $person-def (record
    (field "first-name" string)
    (field "age" u32)
    (field "nick-name" (option string))
    (field "URL" string)
    (field "maybe" (option (option u32)))))
  (import "person" (type $person (eq $person-def)))
  (import "echo" (func $echo (param "x" $person) (result $person)))

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

  (core func $echo' (canon lower (func $echo) (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))

  (core module $M
    (import "" "mem" (memory 1))
    (import "" "realloc" (func $realloc (param i32 i32 i32 i32) (result i32)))
    (import "" "echo" (func $echo (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)))
    (func (export "roundtrip") (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)
      (local $ret i32)
      (local.set $ret (call $realloc (i32.const 0) (i32.const 0) (i32.const 8) (i32.const 256)))
      (call $echo (local.get 0) (local.get 1) (local.get 2) (local.get 3) (local.get 4) (local.get 5) (local.get 6) (local.get 7) (local.get 8) (local.get 9) (local.get 10) (local.get $ret))
      (local.get $ret))
  )
  (core instance $m (instantiate $M (with "" (instance
    (export "mem" (memory $memory "mem"))
    (export "realloc" (func $memory "realloc"))
    (export "echo" (func $echo'))
  ))))

  (func $roundtrip (param "x" $person) (result $person)
    (canon lift (core func $m "roundtrip")
      (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (export "roundtrip" (func $roundtrip))
)
