;; A `[get]`/`[set]` export pair next to a plain export. Module namespace
;; bindings cannot be accessors, so `counter` gets no binding.
(component
  (core module $M
    (global $v (mut i32) (i32.const 1))
    (func (export "get") (result i32) (global.get $v))
    (func (export "set") (param i32) (global.set $v (local.get 0)))
    (func (export "answer") (result i32) (i32.const 42)))
  (core instance $m (instantiate $M))
  (func $get (result u32) (canon lift (core func $m "get")))
  (func $set (param "value" u32) (canon lift (core func $m "set")))
  (func $answer (result u32) (canon lift (core func $m "answer")))
  (export "[get]counter" (func $get))
  (export "[set]counter" (func $set))
  (export "answer" (func $answer))
)
