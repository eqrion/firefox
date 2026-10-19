;; Two abstract resource imports plus a transparent `(eq)` alias. `forward`
;; passes an `r1` handle index to an import expecting `r2`, which must trap.
(component
  (import "r1" (type $r1 (sub resource)))
  (import "r2" (type $r2 (sub resource)))
  (import "r3" (type $r3 (eq $r1)))
  (import "use-r2" (func $use-r2 (param "x" (own $r2))))
  (core func $use-r2' (canon lower (func $use-r2)))
  (core module $M
    (import "" "use-r2" (func $use-r2 (param i32)))
    (func (export "forward") (param i32) (call $use-r2 (local.get 0)))
    (func (export "identity") (param i32) (result i32) (local.get 0)))
  (core instance $m (instantiate $M (with "" (instance (export "use-r2" (func $use-r2'))))))
  (func $forward (param "x" (own $r1)) (canon lift (core func $m "forward")))
  (func $r3-to-r1 (param "x" (own $r3)) (result (own $r1)) (canon lift (core func $m "identity")))
  (func $r1-to-r3 (param "x" (own $r1)) (result (own $r3)) (canon lift (core func $m "identity")))
  (func $r2-identity (param "x" (own $r2)) (result (own $r2)) (canon lift (core func $m "identity")))
  (export "forward" (func $forward))
  (export "r3-to-r1" (func $r3-to-r1))
  (export "r1-to-r3" (func $r1-to-r3))
  (export "r2-identity" (func $r2-identity))
)
