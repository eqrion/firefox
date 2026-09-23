;; A method named `name` has a name reserved for class members.
(component
  (type $counter (resource (rep i32)))
  (core module $M (func (export "f") (param i32)))
  (core instance $m (instantiate $M))
  (export $c "counter" (type $counter))
  (func $f (param "self" (borrow $c)) (canon lift (core func $m "f")))
  (export "[method]counter.name" (func $f))
)
