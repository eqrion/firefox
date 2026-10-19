;; A bare resource type import and a value import, both of which ESM
;; satisfies with the default export of the specifier module.
(component
  (import "elem" (type $elem (sub resource)))
  (import "[method]elem.tag" (func $tag (param "self" (borrow $elem)) (result u32)))
  (import "greeting" (value $greeting string))
  (core func $tag' (canon lower (func $tag)))
  (core func $drop-elem (canon resource.drop $elem))
  (core module $M
    (import "" "tag" (func $tag (param i32) (result i32)))
    (import "" "drop-elem" (func $drop-elem (param i32)))
    (func (export "tag-of") (param i32) (result i32) (local $result i32)
      (local.set $result (call $tag (local.get 0)))
      (call $drop-elem (local.get 0))
      (local.get $result)))
  (core instance $m (instantiate $M (with "" (instance
    (export "tag" (func $tag'))
    (export "drop-elem" (func $drop-elem))))))
  (func $tag-of (param "e" (borrow $elem)) (result u32) (canon lift (core func $m "tag-of")))
  (export "tag-of" (func $tag-of))
  (export "greeting" (value $greeting))
)
