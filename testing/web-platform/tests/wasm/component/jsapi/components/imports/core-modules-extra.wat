;; Core module with an extra export, still a subtype of the import type.
(module
  (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1)))
  (func (export "sub") (param i32 i32) (result i32) (i32.sub (local.get 0) (local.get 1))))
