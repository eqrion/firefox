;; A `[method][get]` accessor on a WebIDL interface. Needs wasm-tools#2660.
(component
  (import "wasm:js/global" (instance $g
    (export "URL" (type $url (sub resource)))
    (export "[constructor]URL" (func (param "url" string) (result (own $url))))
    (export "[method][get]URL.href" (func (param "self" (borrow $url)) (result string)))))
  (alias export $g "URL" (type $url))
  (alias export $g "[constructor]URL" (func $new-url))
  (alias export $g "[method][get]URL.href" (func $href))
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

  (core func $new-url' (canon lower (func $new-url) (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (core func $href' (canon lower (func $href) (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (core func $drop-url (canon resource.drop $url))
  (core module $M
    (import "" "realloc" (func $realloc (param i32 i32 i32 i32) (result i32)))
    (import "" "new-url" (func $new-url (param i32 i32) (result i32)))
    (import "" "href" (func $href (param i32 i32)))
    (import "" "drop-url" (func $drop-url (param i32)))
    (func (export "href") (param $ptr i32) (param $len i32) (result i32)
      (local $ret i32) (local $h i32)
      (local.set $ret (call $realloc (i32.const 0) (i32.const 0) (i32.const 4) (i32.const 8)))
      (local.set $h (call $new-url (local.get $ptr) (local.get $len)))
      (call $href (local.get $h) (local.get $ret))
      (call $drop-url (local.get $h))
      (local.get $ret)))
  (core instance $m (instantiate $M (with "" (instance
    (export "realloc" (func $memory "realloc"))
    (export "new-url" (func $new-url'))
    (export "href" (func $href'))
    (export "drop-url" (func $drop-url))))))
  (func $href-export (param "url" string) (result string) (canon lift (core func $m "href") (memory (core memory $memory "mem")) (realloc (core func $memory "realloc"))))
  (export "href" (func $href-export))
)
