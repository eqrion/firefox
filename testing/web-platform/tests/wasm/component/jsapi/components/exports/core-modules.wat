;; Exported core modules.
(component
  (core module $Adder
    (memory (export "mem") 1)
    (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1))))
  (core module $Importer
    (import "env" "cb" (func $cb (result i32)))
    (func (export "run") (result i32) (call $cb)))
  (export "adder" (core module $Adder))
  (export "my-importer" (core module $Importer))
)
