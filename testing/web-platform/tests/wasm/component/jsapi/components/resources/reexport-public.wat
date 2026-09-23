;; Publicly re-exports an imported resource type as `(eq $r)`.
(component
  (import "r" (type $r (sub resource)))
  (import "make" (func $make (result (own $r))))
  (core func $make' (canon lower (func $make)))
  (core module $M
    (import "" "make" (func $make (result i32)))
    (func (export "run") (result i32) (call $make)))
  (core instance $m (instantiate $M (with "" (instance (export "make" (func $make'))))))
  (func $run (result (own $r)) (canon lift (core func $m "run")))
  (export "x" (type $r))
  (export "run" (func $run))
)
