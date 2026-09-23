;; Imports a resource type and nothing else.
(component
  (import "counter" (type $counter (sub resource)))
  (core module $M (func (export "answer") (result i32) (i32.const 42)))
  (core instance $m (instantiate $M))
  (func $answer (result u32) (canon lift (core func $m "answer")))
  (export "answer" (func $answer))
)
