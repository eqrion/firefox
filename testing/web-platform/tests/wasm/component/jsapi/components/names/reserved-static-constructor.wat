;; A static named `constructor` has a name reserved for class members.
(component
  (type $counter (resource (rep i32)))
  (core module $M (func (export "f")))
  (core instance $m (instantiate $M))
  (export $c "counter" (type $counter))
  (func $f (canon lift (core func $m "f")))
  (export "[static]counter.constructor" (func $f))
)
