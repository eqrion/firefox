;; Passthrough for an enum. Labels stay kebab-case in JS.
(component
  (type $color-def (enum "red" "green" "blue" "dark-blue" "HTML" "a1-b2"))
  (import "color" (type $color (eq $color-def)))
  (import "echo" (func $echo (param "x" $color) (result $color)))

  (core func $echo' (canon lower (func $echo)))

  (core module $M
    (import "" "echo" (func $echo (param i32) (result i32)))
    (func (export "roundtrip") (param i32) (result i32)
      (call $echo (local.get 0)))
  )
  (core instance $m (instantiate $M (with "" (instance
    (export "echo" (func $echo'))
  ))))

  (func $roundtrip (param "x" $color) (result $color)
    (canon lift (core func $m "roundtrip")))
  (export "roundtrip" (func $roundtrip))
)
