;; Core module whose `add` has the wrong signature.
(module
  (func (export "add") (param i32) (result i32) (local.get 0)))
