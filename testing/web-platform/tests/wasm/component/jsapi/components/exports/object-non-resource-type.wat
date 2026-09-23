;; Exports a record type, which has no JS definition.
(component
  (type $rec (record (field "a" u32)))
  (export "t" (type $rec))
)
