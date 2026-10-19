;; Imports a core module exporting `answer: () -> i32` and instantiates it.
(component
  (import "m" (core module $M (export "answer" (func (result i32)))))
  (core instance $m (instantiate $M))
  (func $answer (result u32) (canon lift (core func $m "answer")))
  (export "answer" (func $answer))
)
