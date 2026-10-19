;; No exports at all.
(component
  (core module $M (func (export "f")))
  (core instance $m (instantiate $M))
)
