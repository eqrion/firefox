;; Identity exports for a record, flags, variant and enum whose labels are the
;; rows of the casing table. Each export lowers its argument and lifts it
;; straight back, so it exercises both conversions without an import. The
;; named types are exported because validation requires it.
(component
  (type $record (record
    (field "element" u32) (field "query-selector" u32) (field "inner-HTML" u32)
    (field "XML-http-request" u32) (field "URL" u32) (field "a1-2-3" u32) (field "HTTP-request" u32)))
  (type $flags (flags "element" "query-selector" "inner-HTML" "XML-http-request" "URL" "a1-2-3" "HTTP-request"))
  (type $variant (variant
    (case "element") (case "query-selector") (case "inner-HTML") (case "XML-http-request")
    (case "URL") (case "a1-2-3") (case "HTTP-request" u32)))
  (type $enum (enum "element" "query-selector" "inner-HTML" "XML-http-request" "URL" "a1-2-3" "HTTP-request"))
  (export $record' "record-type" (type $record))
  (export $flags' "flags-type" (type $flags))
  (export $variant' "variant-type" (type $variant))
  (export $enum' "enum-type" (type $enum))

  (core module $M
    (memory (export "mem") 1)
    (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 16))
    (func (export "roundtrip-record") (param i32 i32 i32 i32 i32 i32 i32) (result i32)
      (i32.store (i32.const 64) (local.get 0))
      (i32.store (i32.const 68) (local.get 1))
      (i32.store (i32.const 72) (local.get 2))
      (i32.store (i32.const 76) (local.get 3))
      (i32.store (i32.const 80) (local.get 4))
      (i32.store (i32.const 84) (local.get 5))
      (i32.store (i32.const 88) (local.get 6))
      (i32.const 64))
    (func (export "roundtrip-flags") (param i32) (result i32) (local.get 0))
    (func (export "roundtrip-variant") (param i32 i32) (result i32)
      (i32.store (i32.const 64) (local.get 0))
      (i32.store (i32.const 68) (local.get 1))
      (i32.const 64))
    (func (export "roundtrip-enum") (param i32) (result i32) (local.get 0)))
  (core instance $m (instantiate $M))

  (func $r (param "x" $record') (result $record')
    (canon lift (core func $m "roundtrip-record") (memory (core memory $m "mem")) (realloc (core func $m "realloc"))))
  (func $f (param "x" $flags') (result $flags') (canon lift (core func $m "roundtrip-flags")))
  (func $v (param "x" $variant') (result $variant')
    (canon lift (core func $m "roundtrip-variant") (memory (core memory $m "mem")) (realloc (core func $m "realloc"))))
  (func $e (param "x" $enum') (result $enum') (canon lift (core func $m "roundtrip-enum")))
  (export "roundtrip-record" (func $r))
  (export "roundtrip-flags" (func $f))
  (export "roundtrip-variant" (func $v))
  (export "roundtrip-enum" (func $e))
)
