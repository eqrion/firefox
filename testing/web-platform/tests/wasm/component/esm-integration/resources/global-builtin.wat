;; Uses `btoa` from `wasm:js/global`, which ESM-integration enables by default.
(component
  (import "wasm:js/global" (instance $g
    (export "btoa" (func (param "data" string) (result string)))))
  (core module $Memory
    (memory (export "mem") 1)
    (global $next (mut i32) (i32.const 16))
    (func (export "realloc") (param $old i32) (param $old-size i32) (param $align i32) (param $new-size i32) (result i32)
      (local $ptr i32)
      (local.set $ptr
        (i32.and
          (i32.add (global.get $next) (i32.sub (local.get $align) (i32.const 1)))
          (i32.sub (i32.const 0) (local.get $align))))
      (global.set $next (i32.add (local.get $ptr) (local.get $new-size)))
      (if (local.get $old)
        (then (memory.copy (local.get $ptr) (local.get $old) (select (local.get $old-size) (local.get $new-size) (i32.lt_u (local.get $old-size) (local.get $new-size))))))
      (local.get $ptr)))
  (core instance $memory (instantiate $Memory))
  (core func $btoa' (canon lower (func $g "btoa")
    (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (core module $M
    (import "" "mem" (memory 1))
    (import "" "realloc" (func $realloc (param i32 i32 i32 i32) (result i32)))
    (import "" "btoa" (func $btoa (param i32 i32 i32)))
    (func (export "encode") (param $ptr i32) (param $len i32) (result i32)
      (local $ret i32)
      (local.set $ret (call $realloc (i32.const 0) (i32.const 0) (i32.const 4) (i32.const 8)))
      (call $btoa (local.get $ptr) (local.get $len) (local.get $ret))
      (local.get $ret)))
  (core instance $m (instantiate $M (with "" (instance
    (export "mem" (memory $memory "mem"))
    (export "realloc" (func $memory "realloc"))
    (export "btoa" (func $btoa'))))))
  (func $encode (param "s" string) (result string)
    (canon lift (core func $m "encode")
      (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (export "encode" (func $encode))
)
