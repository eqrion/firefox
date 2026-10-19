;; `get` returns a counter, and its post-return bumps the counter and records
;; the flat value it was passed, so JS can observe that post-return ran before
;; `get` returned to it. The post-return of `get-notifying` calls the `notify`
;; import, which traps, since the instance may not leave during post-return.
(component
  (import "notify" (func $notify (param "value" u32)))
  (core func $notify' (canon lower (func $notify)))
  (core module $M
    (import "" "notify" (func $notify (param i32)))
    (global $counter (mut i32) (i32.const 0))
    (global $post-runs (mut i32) (i32.const 0))
    (global $post-arg (mut i32) (i32.const -1))
    (func (export "get") (result i32) (global.get $counter))
    (func (export "post-get") (param $returned i32)
      (global.set $post-runs (i32.add (global.get $post-runs) (i32.const 1)))
      (global.set $counter (i32.add (global.get $counter) (i32.const 1)))
      (global.set $post-arg (local.get $returned)))
    (func (export "post-notify") (param $returned i32)
      (call $notify (local.get $returned)))
    (func (export "post-runs") (result i32) (global.get $post-runs))
    (func (export "post-arg") (result i32) (global.get $post-arg)))
  (core instance $m (instantiate $M (with "" (instance (export "notify" (func $notify'))))))
  (func $get (result u32) (canon lift (core func $m "get") (post-return (core func $m "post-get"))))
  (func $get-notifying (result u32) (canon lift (core func $m "get") (post-return (core func $m "post-notify"))))
  (func $post-runs (result u32) (canon lift (core func $m "post-runs")))
  (func $post-arg (result s32) (canon lift (core func $m "post-arg")))
  (export "get" (func $get))
  (export "get-notifying" (func $get-notifying))
  (export "post-runs" (func $post-runs))
  (export "post-arg" (func $post-arg))
)
