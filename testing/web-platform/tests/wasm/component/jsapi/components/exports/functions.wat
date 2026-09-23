;; Exported functions of several arities.
(component
  (core module $M
    (func (export "nullary"))
    (func (export "unary") (param i32) (result i32) (i32.add (local.get 0) (i32.const 1)))
    (func (export "ternary") (param i32 i32 i32) (result i32)
      (i32.add (i32.add (local.get 0) (local.get 1)) (local.get 2))))
  (core instance $m (instantiate $M))
  (func $nullary (canon lift (core func $m "nullary")))
  (func $unary (param "a" u32) (result u32) (canon lift (core func $m "unary")))
  (func $ternary (param "a" u32) (param "b" u32) (param "c" u32) (result u32) (canon lift (core func $m "ternary")))
  (export "nullary" (func $nullary))
  (export "add-one" (func $unary))
  (export "sum-3" (func $ternary))
)
