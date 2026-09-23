;; `get` returns a counter and its post-return bumps the counter and notifies
;; the `notify` import, so JS can observe that post-return ran before `get`
;; returned to it.
(component
  (import "notify" (func $notify (param "value" u32)))
  (core func $notify' (canon lower (func $notify)))
  (core module $M
    (import "" "notify" (func $notify (param i32)))
    (global $counter (mut i32) (i32.const 0))
    (global $post-runs (mut i32) (i32.const 0))
    (func (export "get") (result i32) (global.get $counter))
    (func (export "post-get") (param $returned i32)
      (global.set $post-runs (i32.add (global.get $post-runs) (i32.const 1)))
      (global.set $counter (i32.add (global.get $counter) (i32.const 1)))
      (call $notify (local.get $returned)))
    (func (export "post-runs") (result i32) (global.get $post-runs)))
  (core instance $m (instantiate $M (with "" (instance (export "notify" (func $notify'))))))
  (func $get (result u32) (canon lift (core func $m "get") (post-return (core func $m "post-get"))))
  (func $post-runs (result u32) (canon lift (core func $m "post-runs")))
  (export "get" (func $get))
  (export "post-runs" (func $post-runs))
)
