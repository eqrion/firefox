;; A record used by an imported function must be imported as a type. The
;; JS-API has nothing to bind it to, so the import is skipped.
(component
  (type $rec (record (field "x" u32)))
  (import "rec" (type $rec' (eq $rec)))
  (import "f" (func $f (param "r" $rec') (result u32)))
  (core func $f' (canon lower (func $f)))
  (core module $M
    (import "" "f" (func $f (param i32) (result i32)))
    (func (export "run") (result i32) (call $f (i32.const 5))))
  (core instance $m (instantiate $M (with "" (instance (export "f" (func $f'))))))
  (func $run (result u32) (canon lift (core func $m "run")))
  (export "run" (func $run))
)
