;; Publicly re-exports an imported resource type.
(component
  (import "counter" (type $counter (sub resource)))
  (export "counter2" (type $counter))
)
