;; Exports a type that is not a resource, which has no JS binding.
(component
  (type $point (record (field "x" u32) (field "y" u32)))
  (export "point" (type $point))
)
