;; An `external-id` naming a builtin specifier resolves to the builtin.
(component
  (import "g" (external-id "wasm:js/global")
    (instance $g (export "btoa" (func (param "data" string) (result string)))))
  (alias export $g "btoa" (func $btoa))
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

  (core func $btoa' (canon lower (func $btoa) (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (core module $M
    (import "" "realloc" (func $realloc (param i32 i32 i32 i32) (result i32)))
    (import "" "btoa" (func $btoa (param i32 i32 i32)))
    (func (export "encode") (param $ptr i32) (param $len i32) (result i32)
      (local $ret i32)
      (local.set $ret (call $realloc (i32.const 0) (i32.const 0) (i32.const 4) (i32.const 8)))
      (call $btoa (local.get $ptr) (local.get $len) (local.get $ret))
      (local.get $ret)))
  (core instance $m (instantiate $M (with "" (instance
    (export "realloc" (func $memory "realloc"))
    (export "btoa" (func $btoa'))))))
  (func $encode (param "data" string) (result string) (canon lift (core func $m "encode") (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (export "encode" (func $encode))
)
