;; Imports a value and re-exports it.
(component
  (import "v" (value $v u32))
  (export "the-answer" (value $v))
)
