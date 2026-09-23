;; Exports a component definition, which becomes a WebAssembly.Component.
(component
  (component $C
    (core module $M
      (global $n (mut i32) (i32.const 0))
      (func (export "next") (result i32)
        (global.set $n (i32.add (global.get $n) (i32.const 1)))
        (global.get $n)))
    (core instance $m (instantiate $M))
    (func $next (result u32) (canon lift (core func $m "next")))
    (export "next" (func $next)))
  (export "inner" (component $C))
)
