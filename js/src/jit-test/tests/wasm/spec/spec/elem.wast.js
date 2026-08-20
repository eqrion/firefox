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

// ./test/core/elem.wast

// ./test/core/elem.wast:4
let $0 = instantiate(`(module
  (table \$t 10 funcref)
  (func \$f)
  (elem (i32.const 0))
  (elem (i32.const 0) \$f \$f)
  (elem (offset (i32.const 0)))
  (elem (offset (i32.const 0)) \$f \$f)
  (elem 0 (i32.const 0))
  (elem 0x0 (i32.const 0) \$f \$f)
  (elem 0x000 (offset (i32.const 0)))
  (elem 0 (offset (i32.const 0)) \$f \$f)
)`);

// ./test/core/elem.wast:23
let $1 = instantiate(`(module
  (table 10 funcref)
  (func \$f)
  (elem (i32.const 0) \$f)
)`);

// ./test/core/elem.wast:28
let $2 = instantiate(`(module
  (import "spectest" "table" (table 10 funcref))
  (func \$f)
  (elem (i32.const 0) \$f)
)`);

// ./test/core/elem.wast:34
let $3 = instantiate(`(module
  (table 10 funcref)
  (func \$f)
  (elem (i32.const 0) \$f)
  (elem (i32.const 3) \$f)
  (elem (i32.const 7) \$f)
  (elem (i32.const 5) \$f)
  (elem (i32.const 3) \$f)
)`);

// ./test/core/elem.wast:43
let $4 = instantiate(`(module
  (import "spectest" "table" (table 10 funcref))
  (func \$f)
  (elem (i32.const 9) \$f)
  (elem (i32.const 3) \$f)
  (elem (i32.const 7) \$f)
  (elem (i32.const 3) \$f)
  (elem (i32.const 5) \$f)
)`);

// ./test/core/elem.wast:53
let $5 = instantiate(`(module
  (global (import "spectest" "global_i32") i32)
  (table 1000 funcref)
  (func \$f)
  (elem (global.get 0) \$f)
)`);

// ./test/core/elem.wast:60
let $6 = instantiate(`(module
  (global \$g (import "spectest" "global_i32") i32)
  (table 1000 funcref)
  (func \$f)
  (elem (global.get \$g) \$f)
)`);

// ./test/core/elem.wast:67
let $7 = instantiate(`(module
  (type \$out-i32 (func (result i32)))
  (table 10 funcref)
  (elem (i32.const 7) \$const-i32-a)
  (elem (i32.const 9) \$const-i32-b)
  (func \$const-i32-a (type \$out-i32) (i32.const 65))
  (func \$const-i32-b (type \$out-i32) (i32.const 66))
  (func (export "call-7") (type \$out-i32)
    (call_indirect (type \$out-i32) (i32.const 7))
  )
  (func (export "call-9") (type \$out-i32)
    (call_indirect (type \$out-i32) (i32.const 9))
  )
)`);

// ./test/core/elem.wast:81
assert_return(() => invoke($7, `call-7`, []), [value("i32", 65)]);

// ./test/core/elem.wast:82
assert_return(() => invoke($7, `call-9`, []), [value("i32", 66)]);

// ./test/core/elem.wast:86
let $8 = instantiate(`(module
  (table 10 funcref)
  (func \$f)
  (elem (i32.const 9) \$f)
)`);

// ./test/core/elem.wast:91
let $9 = instantiate(`(module
  (import "spectest" "table" (table 10 funcref))
  (func \$f)
  (elem (i32.const 9) \$f)
)`);

// ./test/core/elem.wast:97
let $10 = instantiate(`(module
  (table 0 funcref)
  (elem (i32.const 0))
)`);

// ./test/core/elem.wast:101
let $11 = instantiate(`(module
  (import "spectest" "table" (table 0 funcref))
  (elem (i32.const 0))
)`);

// ./test/core/elem.wast:106
let $12 = instantiate(`(module
  (table 0 0 funcref)
  (elem (i32.const 0))
)`);

// ./test/core/elem.wast:111
let $13 = instantiate(`(module
  (table 20 funcref)
  (elem (i32.const 20))
)`);

// ./test/core/elem.wast:116
let $14 = instantiate(`(module
  (import "spectest" "table" (table 0 funcref))
  (func \$f)
  (elem (i32.const 0) \$f)
)`);

// ./test/core/elem.wast:122
let $15 = instantiate(`(module
  (import "spectest" "table" (table 0 100 funcref))
  (func \$f)
  (elem (i32.const 0) \$f)
)`);

// ./test/core/elem.wast:128
let $16 = instantiate(`(module
  (import "spectest" "table" (table 0 funcref))
  (func \$f)
  (elem (i32.const 1) \$f)
)`);

// ./test/core/elem.wast:134
let $17 = instantiate(`(module
  (import "spectest" "table" (table 0 30 funcref))
  (func \$f)
  (elem (i32.const 1) \$f)
)`);

// ./test/core/elem.wast:142
assert_trap(
  () => instantiate(`(module
    (table 0 funcref)
    (func \$f)
    (elem (i32.const 0) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:151
assert_trap(
  () => instantiate(`(module
    (table 0 0 funcref)
    (func \$f)
    (elem (i32.const 0) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:160
assert_trap(
  () => instantiate(`(module
    (table 0 1 funcref)
    (func \$f)
    (elem (i32.const 0) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:169
assert_trap(
  () => instantiate(`(module
    (table 0 funcref)
    (elem (i32.const 1))
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:177
assert_trap(
  () => instantiate(`(module
    (table 10 funcref)
    (func \$f)
    (elem (i32.const 10) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:185
assert_trap(
  () => instantiate(`(module
    (import "spectest" "table" (table 10 funcref))
    (func \$f)
    (elem (i32.const 10) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:194
assert_trap(
  () => instantiate(`(module
    (table 10 20 funcref)
    (func \$f)
    (elem (i32.const 10) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:202
assert_trap(
  () => instantiate(`(module
    (import "spectest" "table" (table 10 funcref))
    (func \$f)
    (elem (i32.const 10) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:211
assert_trap(
  () => instantiate(`(module
    (table 10 funcref)
    (func \$f)
    (elem (i32.const -1) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:219
assert_trap(
  () => instantiate(`(module
    (import "spectest" "table" (table 10 funcref))
    (func \$f)
    (elem (i32.const -1) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:228
assert_trap(
  () => instantiate(`(module
    (table 10 funcref)
    (func \$f)
    (elem (i32.const -10) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:236
assert_trap(
  () => instantiate(`(module
    (import "spectest" "table" (table 10 funcref))
    (func \$f)
    (elem (i32.const -10) \$f)
  )`),
  `out of bounds`,
);

// ./test/core/elem.wast:247
assert_invalid(
  () => instantiate(`(module
    (func \$f)
    (elem (i32.const 0) \$f)
  )`),
  `unknown table`,
);

// ./test/core/elem.wast:257
assert_invalid(
  () => instantiate(`(module
    (table 1 funcref)
    (elem (i64.const 0))
  )`),
  `type mismatch`,
);

// ./test/core/elem.wast:265
assert_invalid(
  () => instantiate(`(module
    (table 1 funcref)
    (elem (i32.ctz (i32.const 0)))
  )`),
  `constant expression required`,
);

// ./test/core/elem.wast:273
assert_invalid(
  () => instantiate(`(module
    (table 1 funcref)
    (elem (nop))
  )`),
  `constant expression required`,
);

// ./test/core/elem.wast:281
assert_invalid(
  () => instantiate(`(module
    (table 1 funcref)
    (elem (offset (nop) (i32.const 0)))
  )`),
  `constant expression required`,
);

// ./test/core/elem.wast:289
assert_invalid(
  () => instantiate(`(module
    (table 1 funcref)
    (elem (offset (i32.const 0) (nop)))
  )`),
  `constant expression required`,
);

// ./test/core/elem.wast:305
let $18 = instantiate(`(module
  (type \$out-i32 (func (result i32)))
  (table 10 funcref)
  (elem (i32.const 9) \$const-i32-a)
  (elem (i32.const 9) \$const-i32-b)
  (func \$const-i32-a (type \$out-i32) (i32.const 65))
  (func \$const-i32-b (type \$out-i32) (i32.const 66))
  (func (export "call-overwritten") (type \$out-i32)
    (call_indirect (type \$out-i32) (i32.const 9))
  )
)`);

// ./test/core/elem.wast:316
assert_return(() => invoke($18, `call-overwritten`, []), [value("i32", 66)]);

// ./test/core/elem.wast:318
let $19 = instantiate(`(module
  (type \$out-i32 (func (result i32)))
  (import "spectest" "table" (table 10 funcref))
  (elem (i32.const 9) \$const-i32-a)
  (elem (i32.const 9) \$const-i32-b)
  (func \$const-i32-a (type \$out-i32) (i32.const 65))
  (func \$const-i32-b (type \$out-i32) (i32.const 66))
  (func (export "call-overwritten-element") (type \$out-i32)
    (call_indirect (type \$out-i32) (i32.const 9))
  )
)`);

// ./test/core/elem.wast:329
assert_return(() => invoke($19, `call-overwritten-element`, []), [value("i32", 66)]);

// ./test/core/elem.wast:333
let $20 = instantiate(`(module \$module1
  (type \$out-i32 (func (result i32)))
  (table (export "shared-table") 10 funcref)
  (elem (i32.const 8) \$const-i32-a)
  (elem (i32.const 9) \$const-i32-b)
  (func \$const-i32-a (type \$out-i32) (i32.const 65))
  (func \$const-i32-b (type \$out-i32) (i32.const 66))
  (func (export "call-7") (type \$out-i32)
    (call_indirect (type \$out-i32) (i32.const 7))
  )
  (func (export "call-8") (type \$out-i32)
    (call_indirect (type \$out-i32) (i32.const 8))
  )
  (func (export "call-9") (type \$out-i32)
    (call_indirect (type \$out-i32) (i32.const 9))
  )
)`);
let $module1 = $20;

// ./test/core/elem.wast:351
register($module1, `module1`);

// ./test/core/elem.wast:353
assert_trap(() => invoke($module1, `call-7`, []), `uninitialized element`);

// ./test/core/elem.wast:354
assert_return(() => invoke($module1, `call-8`, []), [value("i32", 65)]);

// ./test/core/elem.wast:355
assert_return(() => invoke($module1, `call-9`, []), [value("i32", 66)]);

// ./test/core/elem.wast:357
let $21 = instantiate(`(module \$module2
  (type \$out-i32 (func (result i32)))
  (import "module1" "shared-table" (table 10 funcref))
  (elem (i32.const 7) \$const-i32-c)
  (elem (i32.const 8) \$const-i32-d)
  (func \$const-i32-c (type \$out-i32) (i32.const 67))
  (func \$const-i32-d (type \$out-i32) (i32.const 68))
)`);
let $module2 = $21;

// ./test/core/elem.wast:366
assert_return(() => invoke($module1, `call-7`, []), [value("i32", 67)]);

// ./test/core/elem.wast:367
assert_return(() => invoke($module1, `call-8`, []), [value("i32", 68)]);

// ./test/core/elem.wast:368
assert_return(() => invoke($module1, `call-9`, []), [value("i32", 66)]);

// ./test/core/elem.wast:370
let $22 = instantiate(`(module \$module3
  (type \$out-i32 (func (result i32)))
  (import "module1" "shared-table" (table 10 funcref))
  (elem (i32.const 8) \$const-i32-e)
  (elem (i32.const 9) \$const-i32-f)
  (func \$const-i32-e (type \$out-i32) (i32.const 69))
  (func \$const-i32-f (type \$out-i32) (i32.const 70))
)`);
let $module3 = $22;

// ./test/core/elem.wast:379
assert_return(() => invoke($module1, `call-7`, []), [value("i32", 67)]);

// ./test/core/elem.wast:380
assert_return(() => invoke($module1, `call-8`, []), [value("i32", 69)]);

// ./test/core/elem.wast:381
assert_return(() => invoke($module1, `call-9`, []), [value("i32", 70)]);
