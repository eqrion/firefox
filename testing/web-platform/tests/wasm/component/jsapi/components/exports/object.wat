;; One export of every sort the JS-API gives a JS definition, in an order that
;; interleaves them, plus an interface-named export.
(component
  (import "v" (value $v string))
  (type $counter (resource (rep i32)))
  (core module $Inner (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1))))
  (core module $M (func (export "f") (result i32) (i32.const 42)))
  (core instance $m (instantiate $M))
  (func $f (result u32) (canon lift (core func $m "f")))
  (instance $iface (export "f" (func $f)))
  (export "zeta" (func $f))
  (export "counter" (type $counter))
  (export "alpha-beta" (instance $iface))
  (export "v" (value $v))
  (export "inner-module" (core module $Inner))
  (export "test:pkg/iface@1.2.3" (instance $iface))
  (export "omega" (func $f))
)
