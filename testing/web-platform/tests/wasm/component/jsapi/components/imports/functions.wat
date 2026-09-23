;; Function imports: a plain function, one without a result, and a
;; `[constructor]` that must be a constructor.
(component
  (import "plain" (func $plain (param "x" u32) (result u32)))
  (import "noresult" (func $noresult (param "x" u32)))
  (import "r" (type $r (sub resource)))
  (import "[constructor]r" (func $make (result (own $r))))
  (core func $plain' (canon lower (func $plain)))
  (core func $noresult' (canon lower (func $noresult)))
  (core func $make' (canon lower (func $make)))
  (core module $M
    (import "" "plain" (func $plain (param i32) (result i32)))
    (import "" "noresult" (func $noresult (param i32)))
    (import "" "make" (func $make (result i32)))
    (func (export "call-plain") (param i32) (result i32) (call $plain (local.get 0)))
    (func (export "call-noresult") (param i32) (call $noresult (local.get 0)))
    (func (export "make") (result i32) (call $make)))
  (core instance $m (instantiate $M (with "" (instance
    (export "plain" (func $plain'))
    (export "noresult" (func $noresult'))
    (export "make" (func $make'))))))
  (func $call-plain (param "x" u32) (result u32) (canon lift (core func $m "call-plain")))
  (func $call-noresult (param "x" u32) (canon lift (core func $m "call-noresult")))
  (func $make-export (result (own $r)) (canon lift (core func $m "make")))
  (export "call-plain" (func $call-plain))
  (export "call-noresult" (func $call-noresult))
  (export "make" (func $make-export))
)
