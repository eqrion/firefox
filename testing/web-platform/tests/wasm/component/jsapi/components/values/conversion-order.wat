;; Three flat parameters, to observe the order arguments are converted in.
(component
  (import "echo" (func $echo (param "a" u32) (param "b" u32) (param "c" u32) (result u32)))

  (core func $echo' (canon lower (func $echo)))

  (core module $M
    (import "" "echo" (func $echo (param i32 i32 i32) (result i32)))
    (func (export "roundtrip") (param i32 i32 i32) (result i32)
      (call $echo (local.get 0) (local.get 1) (local.get 2)))
  )
  (core instance $m (instantiate $M (with "" (instance
    (export "echo" (func $echo'))
  ))))

  (func $roundtrip (param "a" u32) (param "b" u32) (param "c" u32) (result u32)
    (canon lift (core func $m "roundtrip")))
  (export "roundtrip" (func $roundtrip))
)
