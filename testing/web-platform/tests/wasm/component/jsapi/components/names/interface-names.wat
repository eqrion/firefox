;; Interface names are used verbatim as JS keys, both when reading imports and
;; when creating exports.
(component
  (import "wasi:http/handler@1.0.0" (instance $handler
    (export "handle" (func (param "x" u32) (result u32)))))
  (import "test:names/plain-func" (func $plain (result u32)))
  (alias export $handler "handle" (func $handle))
  (core func $handle' (canon lower (func $handle)))
  (core func $plain' (canon lower (func $plain)))
  (core module $M
    (import "" "handle" (func $handle (param i32) (result i32)))
    (import "" "plain" (func $plain (result i32)))
    (func (export "handle") (param i32) (result i32) (call $handle (local.get 0)))
    (func (export "plain") (result i32) (call $plain)))
  (core instance $m (instantiate $M (with "" (instance
    (export "handle" (func $handle'))
    (export "plain" (func $plain'))))))
  (func $h (param "x" u32) (result u32) (canon lift (core func $m "handle")))
  (func $p (result u32) (canon lift (core func $m "plain")))
  (instance $out (export "handle" (func $h)))
  (instance $inner (export "run" (func $p)))
  (instance $outer (export "test:nested/inner@2.0.0" (instance $inner)))
  (export "wasi:http/handler@1.0.0" (instance $out))
  (export "test:names/plain-func" (func $p))
  (export "outer" (instance $outer))
)
