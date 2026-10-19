;; Exports a type that is not a resource.
(component
  (type $rec (record (field "a" u32)))
  (export "rec" (type $rec))
  (core module $M (func (export "answer") (result i32) (i32.const 42)))
  (core instance $m (instantiate $M))
  (func $answer (result u32) (canon lift (core func $m "answer")))
  (export "answer" (func $answer))
)
