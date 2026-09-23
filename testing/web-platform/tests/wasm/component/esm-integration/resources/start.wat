;; Calls `log` from its start function, so evaluation has a visible effect.
(component
  (import "log" (func $log (param "message" string)))
  (core module $Memory (memory (export "mem") 1))
  (core instance $memory (instantiate $Memory))
  (core func $log' (canon lower (func $log) (memory (core memory $memory "mem"))))
  (core module $M
    (import "" "mem" (memory 1))
    (import "" "log" (func $log (param i32 i32)))
    (data (i32.const 0) "started")
    (func (export "start") (call $log (i32.const 0) (i32.const 7)))
    (func (export "answer") (result i32) (i32.const 7)))
  (core instance $m (instantiate $M (with "" (instance
    (export "mem" (memory $memory "mem"))
    (export "log" (func $log'))))))
  (func $start (canon lift (core func $m "start")))
  (start $start)
  (func $answer (result u32) (canon lift (core func $m "answer")))
  (export "answer" (func $answer))
)
