;; A static named `prototype` would clobber the class prototype.
(component
  (type $counter (resource (rep i32)))
  (core module $M (func (export "answer") (result i32) (i32.const 42)))
  (core instance $m (instantiate $M))
  (export $counter' "counter" (type $counter))
  (func $answer (result u32) (canon lift (core func $m "answer")))
  (export "[static]counter.prototype" (func $answer))
)
