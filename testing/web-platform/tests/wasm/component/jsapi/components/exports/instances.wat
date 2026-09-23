;; Exported instances: a flat one, one nested two deep, one keyed by an
;; interface name, and one containing a resource type.
(component
  (type $counter (resource (rep i32)))
  (core module $M
    (func (export "f") (result i32) (i32.const 1))
    (func (export "g") (result i32) (i32.const 2)))
  (core instance $m (instantiate $M))
  (func $f (result u32) (canon lift (core func $m "f")))
  (func $g (result u32) (canon lift (core func $m "g")))
  (export $c "counter" (type $counter))
  (instance $inner (export "g" (func $g)))
  (instance $iface
    (export "f" (func $f))
    (export "inner" (instance $inner))
    (export "counter" (type $c)))
  (export "iface" (instance $iface))
  (export "test:pkg/handler@0.2.0" (instance $inner))
)
