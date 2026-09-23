;; Three plain function imports whose lookups on the imports object are
;; observable, in declaration order.
(component
  (import "alpha" (func $alpha (result u32)))
  (import "bravo" (func $bravo (result u32)))
  (import "charlie-delta" (func $charlie-delta (result u32)))
  (core func $alpha' (canon lower (func $alpha)))
  (core func $bravo' (canon lower (func $bravo)))
  (core func $charlie-delta' (canon lower (func $charlie-delta)))
  (core module $M
    (import "" "alpha" (func $alpha (result i32)))
    (import "" "bravo" (func $bravo (result i32)))
    (import "" "charlie-delta" (func $charlie-delta (result i32)))
    (func (export "run") (result i32)
      (i32.add (i32.add (call $alpha) (call $bravo)) (call $charlie-delta))))
  (core instance $m (instantiate $M (with "" (instance
    (export "alpha" (func $alpha'))
    (export "bravo" (func $bravo'))
    (export "charlie-delta" (func $charlie-delta'))))))
  (func $run (result u32) (canon lift (core func $m "run")))
  (export "run" (func $run))
)
