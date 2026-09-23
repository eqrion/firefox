;; A host resource type import with exports that force conversions to it:
;; `identity` and `check-borrow` lower a JS value to own/borrow handles, and
;; `make-via-import` lifts whatever the imported `make` returns.
(component
  (import "r" (type $r (sub resource)))
  (import "make" (func $make (result (own $r))))
  (core func $make' (canon lower (func $make)))
  (core func $drop-r (canon resource.drop $r))
  (core module $M
    (import "" "make" (func $make (result i32)))
    (import "" "drop-r" (func $drop-r (param i32)))
    (func (export "identity") (param i32) (result i32) (local.get 0))
    (func (export "check-borrow") (param i32) (result i32) (call $drop-r (local.get 0)) (i32.const 1))
    (func (export "make-via-import") (result i32) (call $make)))
  (core instance $m (instantiate $M (with "" (instance
    (export "make" (func $make'))
    (export "drop-r" (func $drop-r))))))
  (func $identity (param "x" (own $r)) (result (own $r)) (canon lift (core func $m "identity")))
  (func $check-borrow (param "x" (borrow $r)) (result u32) (canon lift (core func $m "check-borrow")))
  (func $make-via-import (result (own $r)) (canon lift (core func $m "make-via-import")))
  (export "identity" (func $identity))
  (export "check-borrow" (func $check-borrow))
  (export "make-via-import" (func $make-via-import))
)
