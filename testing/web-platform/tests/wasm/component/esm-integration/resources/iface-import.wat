;; Imports an instance under an interface name. ESM resolves the interface
;; name as the specifier and takes one named import per plain export.
(component
  (import "test:pkg/math@1.0.0" (instance $math
    (export "add" (func (param "a" u32) (param "b" u32) (result u32)))
    (export "thing" (type $thing (sub resource)))
    (export "[method]thing.get" (func (param "self" (borrow $thing)) (result u32)))))
  (alias export $math "thing" (type $thing))
  (core func $add' (canon lower (func $math "add")))
  (core func $get' (canon lower (func $math "[method]thing.get")))
  (core func $drop-thing (canon resource.drop $thing))
  (core module $M
    (import "" "add" (func $add (param i32 i32) (result i32)))
    (import "" "get" (func $get (param i32) (result i32)))
    (import "" "drop-thing" (func $drop-thing (param i32)))
    (func (export "add-three") (param i32 i32 i32) (result i32)
      (call $add (call $add (local.get 0) (local.get 1)) (local.get 2)))
    (func (export "get-thing") (param i32) (result i32) (local $result i32)
      (local.set $result (call $get (local.get 0)))
      (call $drop-thing (local.get 0))
      (local.get $result)))
  (core instance $m (instantiate $M (with "" (instance
    (export "add" (func $add'))
    (export "get" (func $get'))
    (export "drop-thing" (func $drop-thing))))))
  (func $add-three (param "a" u32) (param "b" u32) (param "c" u32) (result u32)
    (canon lift (core func $m "add-three")))
  (func $get-thing (param "t" (borrow $thing)) (result u32) (canon lift (core func $m "get-thing")))
  (export "add-three" (func $add-three))
  (export "get-thing" (func $get-thing))
)
