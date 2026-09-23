;; Imports an instance containing `echo`.
(component
  (import "iface" (instance $iface
    (export "echo" (func (param "x" u32) (result u32)))))
  (alias export $iface "echo" (func $echo))
  (core func $echo' (canon lower (func $echo)))
  (core module $M
    (import "" "echo" (func $echo (param i32) (result i32)))
    (func (export "roundtrip") (param i32) (result i32) (call $echo (local.get 0))))
  (core instance $m (instantiate $M (with "" (instance (export "echo" (func $echo'))))))
  (func $roundtrip (param "x" u32) (result u32) (canon lift (core func $m "roundtrip")))
  (export "roundtrip" (func $roundtrip))
)
