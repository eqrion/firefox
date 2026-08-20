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

// ./test/core/data.wast

// ./test/core/data.wast:5
let $0 = instantiate(`(module
  (memory \$m 1)
  (data (i32.const 0))
  (data (i32.const 1) "a" "" "bcd")
  (data (offset (i32.const 0)))
  (data (offset (i32.const 0)) "" "a" "bc" "")
  (data 0 (i32.const 0))
  (data 0x0 (i32.const 1) "a" "" "bcd")
  (data 0x000 (offset (i32.const 0)))
  (data 0 (offset (i32.const 0)) "" "a" "bc" "")
)`);

// ./test/core/data.wast:23
let $1 = instantiate(`(module
  (memory 1)
  (data (i32.const 0) "a")
)`);

// ./test/core/data.wast:27
let $2 = instantiate(`(module
  (import "spectest" "memory" (memory 1))
  (data (i32.const 0) "a")
)`);

// ./test/core/data.wast:32
let $3 = instantiate(`(module
  (memory 1)
  (data (i32.const 0) "a")
  (data (i32.const 3) "b")
  (data (i32.const 100) "cde")
  (data (i32.const 5) "x")
  (data (i32.const 3) "c")
)`);

// ./test/core/data.wast:40
let $4 = instantiate(`(module
  (import "spectest" "memory" (memory 1))
  (data (i32.const 0) "a")
  (data (i32.const 1) "b")
  (data (i32.const 2) "cde")
  (data (i32.const 3) "f")
  (data (i32.const 2) "g")
  (data (i32.const 1) "h")
)`);

// ./test/core/data.wast:50
let $5 = instantiate(`(module
  (global (import "spectest" "global_i32") i32)
  (memory 1)
  (data (global.get 0) "a")
)`);

// ./test/core/data.wast:55
let $6 = instantiate(`(module
  (global (import "spectest" "global_i32") i32)
  (import "spectest" "memory" (memory 1))
  (data (global.get 0) "a")
)`);

// ./test/core/data.wast:61
let $7 = instantiate(`(module
  (global \$g (import "spectest" "global_i32") i32)
  (memory 1)
  (data (global.get \$g) "a")
)`);

// ./test/core/data.wast:66
let $8 = instantiate(`(module
  (global \$g (import "spectest" "global_i32") i32)
  (import "spectest" "memory" (memory 1))
  (data (global.get \$g) "a")
)`);

// ./test/core/data.wast:78
let $9 = instantiate(`(module
  (memory 1)
  (data (i32.const 0) "a")
  (data (i32.const 0xffff) "b")
)`);

// ./test/core/data.wast:83
let $10 = instantiate(`(module
  (import "spectest" "memory" (memory 1))
  (data (i32.const 0) "a")
  (data (i32.const 0xffff) "b")
)`);

// ./test/core/data.wast:89
let $11 = instantiate(`(module
  (memory 2)
  (data (i32.const 0x1_ffff) "a")
)`);

// ./test/core/data.wast:94
let $12 = instantiate(`(module
  (memory 0)
  (data (i32.const 0))
)`);

// ./test/core/data.wast:98
let $13 = instantiate(`(module
  (import "spectest" "memory" (memory 0))
  (data (i32.const 0))
)`);

// ./test/core/data.wast:103
let $14 = instantiate(`(module
  (memory 0 0)
  (data (i32.const 0))
)`);

// ./test/core/data.wast:108
let $15 = instantiate(`(module
  (memory 1)
  (data (i32.const 0x1_0000) "")
)`);

// ./test/core/data.wast:113
let $16 = instantiate(`(module
  (memory 0)
  (data (i32.const 0) "" "")
)`);

// ./test/core/data.wast:117
let $17 = instantiate(`(module
  (import "spectest" "memory" (memory 0))
  (data (i32.const 0) "" "")
)`);

// ./test/core/data.wast:122
let $18 = instantiate(`(module
  (memory 0 0)
  (data (i32.const 0) "" "")
)`);

// ./test/core/data.wast:127
let $19 = instantiate(`(module
  (import "spectest" "memory" (memory 0))
  (data (i32.const 0) "a")
)`);

// ./test/core/data.wast:132
let $20 = instantiate(`(module
  (import "spectest" "memory" (memory 0 3))
  (data (i32.const 0) "a")
)`);

// ./test/core/data.wast:137
let $21 = instantiate(`(module
  (global (import "spectest" "global_i32") i32)
  (import "spectest" "memory" (memory 0))
  (data (global.get 0) "a")
)`);

// ./test/core/data.wast:143
let $22 = instantiate(`(module
  (global (import "spectest" "global_i32") i32)
  (import "spectest" "memory" (memory 0 3))
  (data (global.get 0) "a")
)`);

// ./test/core/data.wast:149
let $23 = instantiate(`(module
  (import "spectest" "memory" (memory 0))
  (data (i32.const 1) "a")
)`);

// ./test/core/data.wast:154
let $24 = instantiate(`(module
  (import "spectest" "memory" (memory 0 3))
  (data (i32.const 1) "a")
)`);

// ./test/core/data.wast:161
assert_trap(
  () => instantiate(`(module
    (memory 0)
    (data (i32.const 0) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:169
assert_trap(
  () => instantiate(`(module
    (memory 0 0)
    (data (i32.const 0) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:177
assert_trap(
  () => instantiate(`(module
    (memory 0 1)
    (data (i32.const 0) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:185
assert_trap(
  () => instantiate(`(module
    (memory 0)
    (data (i32.const 1))
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:193
assert_trap(
  () => instantiate(`(module
    (memory 0 1)
    (data (i32.const 1))
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:210
assert_trap(
  () => instantiate(`(module
    (global (import "spectest" "global_i32") i32)
    (memory 0)
    (data (global.get 0) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:219
assert_trap(
  () => instantiate(`(module
    (memory 1 2)
    (data (i32.const 0x1_0000) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:226
assert_trap(
  () => instantiate(`(module
    (import "spectest" "memory" (memory 1))
    (data (i32.const 0x1_0000) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:234
assert_trap(
  () => instantiate(`(module
    (memory 2)
    (data (i32.const 0x2_0000) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:242
assert_trap(
  () => instantiate(`(module
    (memory 2 3)
    (data (i32.const 0x2_0000) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:250
assert_trap(
  () => instantiate(`(module
    (memory 1)
    (data (i32.const -1) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:257
assert_trap(
  () => instantiate(`(module
    (import "spectest" "memory" (memory 1))
    (data (i32.const -1) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:265
assert_trap(
  () => instantiate(`(module
    (memory 2)
    (data (i32.const -100) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:272
assert_trap(
  () => instantiate(`(module
    (import "spectest" "memory" (memory 1))
    (data (i32.const -100) "a")
  )`),
  `out of bounds`,
);

// ./test/core/data.wast:282
assert_invalid(
  () => instantiate(`(module
    (data (i32.const 0) "")
  )`),
  `unknown memory`,
);

// ./test/core/data.wast:291
assert_invalid(
  () => instantiate(`(module
    (memory 1)
    (data (i64.const 0))
  )`),
  `type mismatch`,
);

// ./test/core/data.wast:299
assert_invalid(
  () => instantiate(`(module
    (memory 1)
    (data (i32.ctz (i32.const 0)))
  )`),
  `constant expression required`,
);

// ./test/core/data.wast:307
assert_invalid(
  () => instantiate(`(module
    (memory 1)
    (data (nop))
  )`),
  `constant expression required`,
);

// ./test/core/data.wast:315
assert_invalid(
  () => instantiate(`(module
    (memory 1)
    (data (offset (nop) (i32.const 0)))
  )`),
  `constant expression required`,
);

// ./test/core/data.wast:323
assert_invalid(
  () => instantiate(`(module
    (memory 1)
    (data (offset (i32.const 0) (nop)))
  )`),
  `constant expression required`,
);
