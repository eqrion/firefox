/* Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// ./test/validation/defined-types.wast

// ./test/validation/defined-types.wast:8
let $0 = instantiateComponent(`(component
  (type bool)
  (type u8) (type s8) (type u16) (type s16)
  (type u32) (type s32) (type u64) (type s64)
  (type f32) (type f64)
  (type char) (type string)

  (type \$r (record (field "x" (result)) (field "y" string)))
  (type \$u (variant (case "r" \$r) (case "s" string)))
  (type \$e (result \$u (error u32)))
  (type (result \$u))
  (type (result (error \$u)))
  (type (result))
  (type (func (param "a" \$e) (result (option \$r))))

  (type \$errno (enum "a" "b" "e"))
  (type (list \$errno))
  (type \$oflags (flags "read" "write" "exclusive"))
  (type (tuple \$oflags \$errno \$r))
)`);

// ./test/validation/defined-types.wast:31
assert_invalid(
  () => component(`(component (type (record (field "" s32))))`),
  `record field name cannot be empty`,
);

// ./test/validation/defined-types.wast:34
assert_invalid(
  () => component(`(component (type (variant (case "" s32))))`),
  `variant case name cannot be empty`,
);

// ./test/validation/defined-types.wast:37
assert_invalid(
  () => component(`(component (type (flags "")))`),
  `flag name cannot be empty`,
);

// ./test/validation/defined-types.wast:40
assert_invalid(
  () => component(`(component (type (enum "")))`),
  `enum tag name cannot be empty`,
);

// ./test/validation/defined-types.wast:43
assert_invalid(
  () => component(`(component (type (func (param "" string))))`),
  `function parameter name cannot be empty`,
);

// ./test/validation/defined-types.wast:49
assert_invalid(
  () => component(`(component (type (record (field "a-B-c-D" string) (field "A-b-C-d" u8))))`),
  `record field name \`A-b-C-d\` conflicts with previous field name \`a-B-c-D\``,
);

// ./test/validation/defined-types.wast:52
assert_invalid(
  () => component(`(component (type (variant (case "x" s64) (case "x" s64))))`),
  `variant case name \`x\` conflicts with previous case name \`x\``,
);

// ./test/validation/defined-types.wast:55
assert_invalid(
  () => component(`(component (type (variant (case "x" s64) (case "X" s64))))`),
  `variant case name \`X\` conflicts with previous case name \`x\``,
);

// ./test/validation/defined-types.wast:58
assert_invalid(
  () => component(`(component (type (flags "x" "y" "X")))`),
  `flag name \`X\` conflicts with previous flag name \`x\``,
);

// ./test/validation/defined-types.wast:61
assert_invalid(
  () => component(`(component (type (enum "x" "y" "X")))`),
  `enum tag name \`X\` conflicts with previous tag name \`x\``,
);

// ./test/validation/defined-types.wast:64
assert_invalid(
  () => component(`(component (type (func (param "foo" string) (param "FOO" u32))))`),
  `function parameter name \`FOO\` conflicts with previous parameter name \`foo\``,
);

// ./test/validation/defined-types.wast:70
assert_invalid(
  () => component(`(component (type (enum "NevEr")))`),
  `enum tag name \`NevEr\` is not in kebab case`,
);

// ./test/validation/defined-types.wast:73
assert_invalid(
  () => component(`(component (type (record (field "GoNnA" string))))`),
  `record field name \`GoNnA\` is not in kebab case`,
);

// ./test/validation/defined-types.wast:76
assert_invalid(
  () => component(`(component (type (variant (case "GIVe" string))))`),
  `variant case name \`GIVe\` is not in kebab case`,
);

// ./test/validation/defined-types.wast:79
assert_invalid(
  () => component(`(component (type (func (param "yOu" string))))`),
  `function parameter name \`yOu\` is not in kebab case`,
);

// ./test/validation/defined-types.wast:85
assert_invalid(
  () => component(`(component (type (variant)))`),
  `variant type must have at least one case`,
);

// ./test/validation/defined-types.wast:88
assert_invalid(
  () => component(`(component (type (enum)))`),
  `enum type must have at least one variant`,
);

// ./test/validation/defined-types.wast:91
assert_invalid(
  () => component(`(component (type (record)))`),
  `record type must have at least one field`,
);

// ./test/validation/defined-types.wast:94
assert_invalid(
  () => component(`(component (type (flags)))`),
  `flags must have at least one entry`,
);

// ./test/validation/defined-types.wast:97
assert_invalid(
  () => component(`(component (type (tuple)))`),
  `tuple type must have at least one type`,
);

// ./test/validation/defined-types.wast:103
let $1 = instantiateComponent(`(component
  (type (flags
    "f1" "f2" "f3" "f4" "f5" "f6" "f7" "f8"
    "f9" "f10" "f11" "f12" "f13" "f14" "f15" "f16"
    "f17" "f18" "f19" "f20" "f21" "f22" "f23" "f24"
    "f25" "f26" "f27" "f28" "f29" "f30" "f31" "f32"))
)`);

// ./test/validation/defined-types.wast:110
assert_invalid(
  () => component(`(component
    (type (flags
      "f1" "f2" "f3" "f4" "f5" "f6" "f7" "f8"
      "f9" "f10" "f11" "f12" "f13" "f14" "f15" "f16"
      "f17" "f18" "f19" "f20" "f21" "f22" "f23" "f24"
      "f25" "f26" "f27" "f28" "f29" "f30" "f31" "f32" "f33")))`),
  `cannot have more than 32 flags`,
);

// ./test/validation/defined-types.wast:121
assert_invalid(
  () => component(`(component
    (type \$t (func))
    (type (func (param "t" \$t))))`),
  `type index 0 is not a defined type`,
);

// ./test/validation/defined-types.wast:126
assert_invalid(
  () => component(`(component
    (type \$t (instance))
    (type (func (result \$t))))`),
  `type index 0 is not a defined type`,
);

// ./test/validation/defined-types.wast:131
assert_invalid(
  () => component(`(component
    (type \$t (component))
    (type (option \$t)))`),
  `type index 0 is not a defined type`,
);

// ./test/validation/defined-types.wast:139
assert_invalid(
  () => component(`(component (type (option 0)))`),
  `type index out of bounds`,
);

// ./test/validation/defined-types.wast:142
assert_invalid(() => component(`(component (type (list 0)))`), `type index out of bounds`);

// ./test/validation/defined-types.wast:145
assert_invalid(
  () => component(`(component (type (record (field "x" 0))))`),
  `type index out of bounds`,
);

// ./test/validation/defined-types.wast:148
assert_invalid(
  () => component(`(component (type (variant (case "x" 0))))`),
  `type index out of bounds`,
);

// ./test/validation/defined-types.wast:151
assert_invalid(
  () => component(`(component (type (result 0 (error 1))))`),
  `type index out of bounds`,
);

// ./test/validation/defined-types.wast:154
assert_invalid(
  () => component(`(component (type (tuple 0)))`),
  `type index out of bounds`,
);

// ./test/validation/defined-types.wast:161
assert_invalid(
  () => component(`(component (import "a" (func (type 100))))`),
  `type index out of bounds`,
);

// ./test/validation/defined-types.wast:164
assert_invalid(
  () => component(`(component
    (core module \$m (func (export "f")))
    (core instance \$i (instantiate \$m))
    (func (type 100) (canon lift (core func \$i "f"))))`),
  `type index out of bounds`,
);

// ./test/validation/defined-types.wast:170
assert_invalid(
  () => component(`(component (export "a" (core module 0)))`),
  `module index out of bounds`,
);

// ./test/validation/defined-types.wast:173
assert_invalid(
  () => component(`(component (export "a" (instance 0)))`),
  `instance index out of bounds`,
);

// ./test/validation/defined-types.wast:179
assert_invalid(
  () => component(`(component
    (type \$t (instance))
    (import "a" (func (type \$t))))`),
  `type index 0 is not a function type`,
);

// ./test/validation/defined-types.wast:184
assert_invalid(
  () => component(`(component
    (type \$t (instance))
    (core module \$m (func (export "f")))
    (core instance \$i (instantiate \$m))
    (func \$f (canon lift (core func \$i "f")))
    (export "a" (func \$f) (func (type \$t))))`),
  `type index 0 is not a function type`,
);

// ./test/validation/defined-types.wast:192
assert_invalid(
  () => component(`(component
    (type \$t (instance))
    (type (component
      (import "a" (func (type \$t))))))`),
  `type index 0 is not a function type`,
);

// ./test/validation/defined-types.wast:198
assert_invalid(
  () => component(`(component
    (type \$t (record (field "x" u8)))
    (type (instance
      (export "a" (func (type \$t))))))`),
  `type index 0 is not a function type`,
);

// ./test/validation/defined-types.wast:204
assert_invalid(
  () => component(`(component
    (type \$t (instance))
    (type (instance
      (export "a" (instance
        (export "b" (func (type \$t))))))))`),
  `type index 0 is not a function type`,
);

// ./test/validation/defined-types.wast:214
assert_invalid(
  () => component(`(component
    (type \$t (func))
    (import "a" (instance (type \$t))))`),
  `type index 0 is not an instance type`,
);

// ./test/validation/defined-types.wast:219
assert_invalid(
  () => component(`(component
    (type \$t (func))
    (type (component
      (import "a" (instance (type \$t))))))`),
  `type index 0 is not an instance type`,
);

// ./test/validation/defined-types.wast:225
assert_invalid(
  () => component(`(component
    (type \$t (func))
    (type (instance
      (export "a" (instance (type \$t))))))`),
  `type index 0 is not an instance type`,
);

// ./test/validation/defined-types.wast:234
assert_invalid(
  () => component(`(component
    (core type \$t (func))
    (import "a" (core module (type \$t))))`),
  `core type index 0 is not a module type`,
);

// ./test/validation/defined-types.wast:239
assert_invalid(
  () => component(`(component
    (core type \$t (func))
    (type (component
      (import "a" (core module (type \$t))))))`),
  `core type index 0 is not a module type`,
);

// ./test/validation/defined-types.wast:245
assert_invalid(
  () => component(`(component
    (core type \$t (func))
    (type (instance
      (export "a" (core module (type \$t))))))`),
  `core type index 0 is not a module type`,
);
