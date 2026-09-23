;; A method named `constructor` would clobber `prototype.constructor`.
(component
  (type $counter (resource (rep i32)))
  (core module $M (func (export "get") (param i32) (result i32) (local.get 0)))
  (core instance $m (instantiate $M))
  (export $counter' "counter" (type $counter))
  (func $get (param "self" (borrow $counter')) (result u32) (canon lift (core func $m "get")))
  (export "[method]counter.constructor" (func $get))
)
