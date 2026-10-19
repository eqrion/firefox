;; Imports `log` with an `external-id`, which becomes the module specifier.
(component
  (import "log" (external-id "./log-module.js") (func $log (param "message" string)))
  (core module $Memory (memory (export "mem") 1))
  (core instance $memory (instantiate $Memory))
  (core func $log' (canon lower (func $log) (memory (core memory $memory "mem"))))
  (core module $M
    (import "" "mem" (memory 1))
    (import "" "log" (func $log (param i32 i32)))
    (data (i32.const 0) "hello")
    (func (export "run") (result i32)
      (call $log (i32.const 0) (i32.const 5))
      (i32.const 1)))
  (core instance $m (instantiate $M (with "" (instance
    (export "mem" (memory $memory "mem"))
    (export "log" (func $log'))))))
  (func $run (result u32) (canon lift (core func $m "run")))
  (export "run" (func $run))
)
