;; Imports a component, instantiates it internally with a host-provided
;; function, and re-exports its `run`.
(component
  (import "c" (component $C
    (import "log" (func (param "message" string)))
    (export "run" (func (result u32)))))
  (import "log" (func $log (param "message" string)))
  (instance $c (instantiate $C (with "log" (func $log))))
  (export "run" (func $c "run"))
)
