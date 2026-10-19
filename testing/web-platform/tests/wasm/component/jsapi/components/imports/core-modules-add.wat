;; Core module matching the import type of core-modules.wat.
(module
  (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1))))
