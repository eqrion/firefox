;; Value imports re-exported as values, so JS can observe ToComponentValue at
;; instantiation and ToJSValue on the exports object. Only types that need no
;; named type declaration are used.
(component
  (import "greeting" (value $greeting string))
  (import "port" (value $port u16))
  (import "tags" (value $tags (list string)))
  (import "limit" (value $limit (option u32)))
  (export "default-greeting" (value $greeting))
  (export "default-port" (value $port))
  (export "default-tags" (value $tags))
  (export "default-limit" (value $limit))
)
