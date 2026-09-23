;; The component that `component-import.wat` expects as its `c` import.
(component
  (import "log" (func $log (param "message" string)))
  (core module $Memory (memory (export "mem") 1))
  (core instance $memory (instantiate $Memory))
  (core func $log' (canon lower (func $log) (memory (core memory $memory "mem"))))
  (core module $M
    (import "" "mem" (memory 1))
    (import "" "log" (func $log (param i32 i32)))
    (data (i32.const 0) "from inner")
    (func (export "run") (result i32)
      (call $log (i32.const 0) (i32.const 10))
      (i32.const 42)))
  (core instance $m (instantiate $M (with "" (instance
    (export "mem" (memory $memory "mem"))
    (export "log" (func $log'))))))
  (func $run (result u32) (canon lift (core func $m "run")))
  (export "run" (func $run))
)
