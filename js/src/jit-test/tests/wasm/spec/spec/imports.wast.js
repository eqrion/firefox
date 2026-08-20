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

// ./test/core/imports.wast

// ./test/core/imports.wast:3
let $0 = instantiate(`(module
  (func (export "func"))
  (func (export "func-i32") (param i32))
  (func (export "func-f32") (param f32))
  (func (export "func->i32") (result i32) (i32.const 22))
  (func (export "func->f32") (result f32) (f32.const 11))
  (func (export "func-i32->i32") (param i32) (result i32) (local.get 0))
  (func (export "func-i64->i64") (param i64) (result i64) (local.get 0))
  (global (export "global-i32") i32 (i32.const 55))
  (global (export "global-f32") f32 (f32.const 44))
  (table (export "table-10-inf") 10 funcref)
  ;; (table (export "table-10-20") 10 20 funcref)
  (memory (export "memory-2-inf") 2)
  ;; (memory (export "memory-2-4") 2 4)
)`);

// ./test/core/imports.wast:19
register($0, `test`);

// ./test/core/imports.wast:24
let $1 = instantiate(`(module
  (type \$func_i32 (func (param i32)))
  (type \$func_i64 (func (param i64)))
  (type \$func_f32 (func (param f32)))
  (type \$func_f64 (func (param f64)))

  (import "spectest" "print_i32" (func (param i32)))
  ;; JavaScript can't handle i64 yet.
  ;; (func (import "spectest" "print_i64") (param i64))
  (import "spectest" "print_i32" (func \$print_i32 (param i32)))
  ;; JavaScript can't handle i64 yet.
  ;; (import "spectest" "print_i64" (func \$print_i64 (param i64)))
  (import "spectest" "print_f32" (func \$print_f32 (param f32)))
  (import "spectest" "print_f64" (func \$print_f64 (param f64)))
  (import "spectest" "print_i32_f32" (func \$print_i32_f32 (param i32 f32)))
  (import "spectest" "print_f64_f64" (func \$print_f64_f64 (param f64 f64)))
  (func \$print_i32-2 (import "spectest" "print_i32") (param i32))
  (func \$print_f64-2 (import "spectest" "print_f64") (param f64))
  (import "test" "func-i64->i64" (func \$i64->i64 (param i64) (result i64)))

  (func (export "p1") (import "spectest" "print_i32") (param i32))
  (func \$p (export "p2") (import "spectest" "print_i32") (param i32))
  (func (export "p3") (export "p4") (import "spectest" "print_i32") (param i32))
  (func (export "p5") (import "spectest" "print_i32") (type 0))
  (func (export "p6") (import "spectest" "print_i32") (type 0) (param i32) (result))

  (import "spectest" "print_i32" (func (type \$forward)))
  (func (import "spectest" "print_i32") (type \$forward))
  (type \$forward (func (param i32)))

  (table funcref (elem \$print_i32 \$print_f64))

  (func (export "print32") (param \$i i32)
    (local \$x f32)
    (local.set \$x (f32.convert_i32_s (local.get \$i)))
    (call 0 (local.get \$i))
    (call \$print_i32_f32
      (i32.add (local.get \$i) (i32.const 1))
      (f32.const 42)
    )
    (call \$print_i32 (local.get \$i))
    (call \$print_i32-2 (local.get \$i))
    (call \$print_f32 (local.get \$x))
    (call_indirect (type \$func_i32) (local.get \$i) (i32.const 0))
  )

  (func (export "print64") (param \$i i64)
    (local \$x f64)
    (local.set \$x (f64.convert_i64_s (call \$i64->i64 (local.get \$i))))
    ;; JavaScript can't handle i64 yet.
    ;; (call 1 (local.get \$i))
    (call \$print_f64_f64
      (f64.add (local.get \$x) (f64.const 1))
      (f64.const 53)
    )
    ;; JavaScript can't handle i64 yet.
    ;; (call \$print_i64 (local.get \$i))
    (call \$print_f64 (local.get \$x))
    (call \$print_f64-2 (local.get \$x))
    (call_indirect (type \$func_f64) (local.get \$x) (i32.const 1))
  )
)`);

// ./test/core/imports.wast:87
assert_return(() => invoke($1, `print32`, [13]), []);

// ./test/core/imports.wast:88
assert_return(() => invoke($1, `print64`, [24n]), []);

// ./test/core/imports.wast:90
assert_invalid(
  () => instantiate(`(module 
    (type (func (result i32)))
    (import "test" "func" (func (type 1)))
  )`),
  `unknown type`,
);

// ./test/core/imports.wast:98
let $2 = instantiate(`(module (import "test" "func" (func)))`);

// ./test/core/imports.wast:99
let $3 = instantiate(`(module (import "test" "func-i32" (func (param i32))))`);

// ./test/core/imports.wast:100
let $4 = instantiate(`(module (import "test" "func-f32" (func (param f32))))`);

// ./test/core/imports.wast:101
let $5 = instantiate(`(module (import "test" "func->i32" (func (result i32))))`);

// ./test/core/imports.wast:102
let $6 = instantiate(`(module (import "test" "func->f32" (func (result f32))))`);

// ./test/core/imports.wast:103
let $7 = instantiate(`(module (import "test" "func-i32->i32" (func (param i32) (result i32))))`);

// ./test/core/imports.wast:104
let $8 = instantiate(`(module (import "test" "func-i64->i64" (func (param i64) (result i64))))`);

// ./test/core/imports.wast:106
assert_unlinkable(
  () => instantiate(`(module (import "test" "unknown" (func)))`),
  `unknown import`,
);

// ./test/core/imports.wast:110
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "unknown" (func)))`),
  `unknown import`,
);

// ./test/core/imports.wast:115
assert_unlinkable(
  () => instantiate(`(module (import "test" "func" (func (param i32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:119
assert_unlinkable(
  () => instantiate(`(module (import "test" "func" (func (result i32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:123
assert_unlinkable(
  () => instantiate(`(module (import "test" "func" (func (param i32) (result i32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:127
assert_unlinkable(
  () => instantiate(`(module (import "test" "func-i32" (func)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:131
assert_unlinkable(
  () => instantiate(`(module (import "test" "func-i32" (func (result i32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:135
assert_unlinkable(
  () => instantiate(`(module (import "test" "func-i32" (func (param f32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:139
assert_unlinkable(
  () => instantiate(`(module (import "test" "func-i32" (func (param i64))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:143
assert_unlinkable(
  () => instantiate(`(module (import "test" "func-i32" (func (param i32) (result i32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:147
assert_unlinkable(
  () => instantiate(`(module (import "test" "func->i32" (func)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:151
assert_unlinkable(
  () => instantiate(`(module (import "test" "func->i32" (func (param i32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:155
assert_unlinkable(
  () => instantiate(`(module (import "test" "func->i32" (func (result f32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:159
assert_unlinkable(
  () => instantiate(`(module (import "test" "func->i32" (func (result i64))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:163
assert_unlinkable(
  () => instantiate(`(module (import "test" "func->i32" (func (param i32) (result i32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:167
assert_unlinkable(
  () => instantiate(`(module (import "test" "func-i32->i32" (func)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:171
assert_unlinkable(
  () => instantiate(`(module (import "test" "func-i32->i32" (func (param i32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:175
assert_unlinkable(
  () => instantiate(`(module (import "test" "func-i32->i32" (func (result i32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:180
assert_unlinkable(
  () => instantiate(`(module (import "test" "global-i32" (func (result i32))))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:184
assert_unlinkable(
  () => instantiate(`(module (import "test" "table-10-inf" (func)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:188
assert_unlinkable(
  () => instantiate(`(module (import "test" "memory-2-inf" (func)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:192
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "global_i32" (func)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:196
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "table" (func)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:200
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "memory" (func)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:208
let $9 = instantiate(`(module
  (import "spectest" "global_i32" (global i32))
  (global (import "spectest" "global_i32") i32)

  (import "spectest" "global_i32" (global \$x i32))
  (global \$y (import "spectest" "global_i32") i32)

  ;; JavaScript can't handle i64 yet.
  ;; (import "spectest" "global_i64" (global i64))
  (import "spectest" "global_f32" (global f32))
  (import "spectest" "global_f64" (global f64))

  (func (export "get-0") (result i32) (global.get 0))
  (func (export "get-1") (result i32) (global.get 1))
  (func (export "get-x") (result i32) (global.get \$x))
  (func (export "get-y") (result i32) (global.get \$y))
)`);

// ./test/core/imports.wast:226
assert_return(() => invoke($9, `get-0`, []), [value("i32", 666)]);

// ./test/core/imports.wast:227
assert_return(() => invoke($9, `get-1`, []), [value("i32", 666)]);

// ./test/core/imports.wast:228
assert_return(() => invoke($9, `get-x`, []), [value("i32", 666)]);

// ./test/core/imports.wast:229
assert_return(() => invoke($9, `get-y`, []), [value("i32", 666)]);

// ./test/core/imports.wast:231
let $10 = instantiate(`(module (import "test" "global-i32" (global i32)))`);

// ./test/core/imports.wast:232
let $11 = instantiate(`(module (import "test" "global-f32" (global f32)))`);

// ./test/core/imports.wast:234
assert_unlinkable(
  () => instantiate(`(module (import "test" "unknown" (global i32)))`),
  `unknown import`,
);

// ./test/core/imports.wast:238
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "unknown" (global i32)))`),
  `unknown import`,
);

// ./test/core/imports.wast:243
assert_unlinkable(
  () => instantiate(`(module (import "test" "func" (global i32)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:247
assert_unlinkable(
  () => instantiate(`(module (import "test" "table-10-inf" (global i32)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:251
assert_unlinkable(
  () => instantiate(`(module (import "test" "memory-2-inf" (global i32)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:255
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "print_i32" (global i32)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:259
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "table" (global i32)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:263
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "memory" (global i32)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:271
let $12 = instantiate(`(module
  (type (func (result i32)))
  (import "spectest" "table" (table 10 20 funcref))
  (elem 0 (i32.const 1) \$f \$g)

  (func (export "call") (param i32) (result i32)
    (call_indirect (type 0) (local.get 0))
  )
  (func \$f (result i32) (i32.const 11))
  (func \$g (result i32) (i32.const 22))
)`);

// ./test/core/imports.wast:283
assert_trap(() => invoke($12, `call`, [0]), `uninitialized element`);

// ./test/core/imports.wast:284
assert_return(() => invoke($12, `call`, [1]), [value("i32", 11)]);

// ./test/core/imports.wast:285
assert_return(() => invoke($12, `call`, [2]), [value("i32", 22)]);

// ./test/core/imports.wast:286
assert_trap(() => invoke($12, `call`, [3]), `uninitialized element`);

// ./test/core/imports.wast:287
assert_trap(() => invoke($12, `call`, [100]), `undefined element`);

// ./test/core/imports.wast:290
let $13 = instantiate(`(module
  (type (func (result i32)))
  (table (import "spectest" "table") 10 20 funcref)
  (elem 0 (i32.const 1) \$f \$g)

  (func (export "call") (param i32) (result i32)
    (call_indirect (type 0) (local.get 0))
  )
  (func \$f (result i32) (i32.const 11))
  (func \$g (result i32) (i32.const 22))
)`);

// ./test/core/imports.wast:302
assert_trap(() => invoke($13, `call`, [0]), `uninitialized element`);

// ./test/core/imports.wast:303
assert_return(() => invoke($13, `call`, [1]), [value("i32", 11)]);

// ./test/core/imports.wast:304
assert_return(() => invoke($13, `call`, [2]), [value("i32", 22)]);

// ./test/core/imports.wast:305
assert_trap(() => invoke($13, `call`, [3]), `uninitialized element`);

// ./test/core/imports.wast:306
assert_trap(() => invoke($13, `call`, [100]), `undefined element`);

// ./test/core/imports.wast:309
assert_invalid(
  () => instantiate(`(module (import "" "" (table 10 funcref)) (import "" "" (table 10 funcref)))`),
  `multiple tables`,
);

// ./test/core/imports.wast:313
assert_invalid(
  () => instantiate(`(module (import "" "" (table 10 funcref)) (table 10 funcref))`),
  `multiple tables`,
);

// ./test/core/imports.wast:317
assert_invalid(
  () => instantiate(`(module (table 10 funcref) (table 10 funcref))`),
  `multiple tables`,
);

// ./test/core/imports.wast:322
let $14 = instantiate(`(module (import "test" "table-10-inf" (table 10 funcref)))`);

// ./test/core/imports.wast:323
let $15 = instantiate(`(module (import "test" "table-10-inf" (table 5 funcref)))`);

// ./test/core/imports.wast:324
let $16 = instantiate(`(module (import "test" "table-10-inf" (table 0 funcref)))`);

// ./test/core/imports.wast:325
let $17 = instantiate(`(module (import "spectest" "table" (table 10 funcref)))`);

// ./test/core/imports.wast:326
let $18 = instantiate(`(module (import "spectest" "table" (table 5 funcref)))`);

// ./test/core/imports.wast:327
let $19 = instantiate(`(module (import "spectest" "table" (table 0 funcref)))`);

// ./test/core/imports.wast:328
let $20 = instantiate(`(module (import "spectest" "table" (table 10 20 funcref)))`);

// ./test/core/imports.wast:329
let $21 = instantiate(`(module (import "spectest" "table" (table 5 20 funcref)))`);

// ./test/core/imports.wast:330
let $22 = instantiate(`(module (import "spectest" "table" (table 0 20 funcref)))`);

// ./test/core/imports.wast:331
let $23 = instantiate(`(module (import "spectest" "table" (table 10 25 funcref)))`);

// ./test/core/imports.wast:332
let $24 = instantiate(`(module (import "spectest" "table" (table 5 25 funcref)))`);

// ./test/core/imports.wast:334
assert_unlinkable(
  () => instantiate(`(module (import "test" "unknown" (table 10 funcref)))`),
  `unknown import`,
);

// ./test/core/imports.wast:338
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "unknown" (table 10 funcref)))`),
  `unknown import`,
);

// ./test/core/imports.wast:343
assert_unlinkable(
  () => instantiate(`(module (import "test" "table-10-inf" (table 12 funcref)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:347
assert_unlinkable(
  () => instantiate(`(module (import "test" "table-10-inf" (table 10 20 funcref)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:351
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "table" (table 12 funcref)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:355
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "table" (table 10 15 funcref)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:360
assert_unlinkable(
  () => instantiate(`(module (import "test" "func" (table 10 funcref)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:364
assert_unlinkable(
  () => instantiate(`(module (import "test" "global-i32" (table 10 funcref)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:368
assert_unlinkable(
  () => instantiate(`(module (import "test" "memory-2-inf" (table 10 funcref)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:372
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "print_i32" (table 10 funcref)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:381
let $25 = instantiate(`(module
  (import "spectest" "memory" (memory 1 2))
  (data 0 (i32.const 10) "\\10")

  (func (export "load") (param i32) (result i32) (i32.load (local.get 0)))
)`);

// ./test/core/imports.wast:388
assert_return(() => invoke($25, `load`, [0]), [value("i32", 0)]);

// ./test/core/imports.wast:389
assert_return(() => invoke($25, `load`, [10]), [value("i32", 16)]);

// ./test/core/imports.wast:390
assert_return(() => invoke($25, `load`, [8]), [value("i32", 1048576)]);

// ./test/core/imports.wast:391
assert_trap(() => invoke($25, `load`, [1000000]), `out of bounds memory access`);

// ./test/core/imports.wast:393
let $26 = instantiate(`(module
  (memory (import "spectest" "memory") 1 2)
  (data 0 (i32.const 10) "\\10")

  (func (export "load") (param i32) (result i32) (i32.load (local.get 0)))
)`);

// ./test/core/imports.wast:399
assert_return(() => invoke($26, `load`, [0]), [value("i32", 0)]);

// ./test/core/imports.wast:400
assert_return(() => invoke($26, `load`, [10]), [value("i32", 16)]);

// ./test/core/imports.wast:401
assert_return(() => invoke($26, `load`, [8]), [value("i32", 1048576)]);

// ./test/core/imports.wast:402
assert_trap(() => invoke($26, `load`, [1000000]), `out of bounds memory access`);

// ./test/core/imports.wast:404
assert_invalid(
  () => instantiate(`(module (import "" "" (memory 1)) (import "" "" (memory 1)))`),
  `multiple memories`,
);

// ./test/core/imports.wast:408
assert_invalid(
  () => instantiate(`(module (import "" "" (memory 1)) (memory 0))`),
  `multiple memories`,
);

// ./test/core/imports.wast:412
assert_invalid(() => instantiate(`(module (memory 0) (memory 0))`), `multiple memories`);

// ./test/core/imports.wast:417
let $27 = instantiate(`(module (import "test" "memory-2-inf" (memory 2)))`);

// ./test/core/imports.wast:418
let $28 = instantiate(`(module (import "test" "memory-2-inf" (memory 1)))`);

// ./test/core/imports.wast:419
let $29 = instantiate(`(module (import "test" "memory-2-inf" (memory 0)))`);

// ./test/core/imports.wast:420
let $30 = instantiate(`(module (import "spectest" "memory" (memory 1)))`);

// ./test/core/imports.wast:421
let $31 = instantiate(`(module (import "spectest" "memory" (memory 0)))`);

// ./test/core/imports.wast:422
let $32 = instantiate(`(module (import "spectest" "memory" (memory 1 2)))`);

// ./test/core/imports.wast:423
let $33 = instantiate(`(module (import "spectest" "memory" (memory 0 2)))`);

// ./test/core/imports.wast:424
let $34 = instantiate(`(module (import "spectest" "memory" (memory 1 3)))`);

// ./test/core/imports.wast:425
let $35 = instantiate(`(module (import "spectest" "memory" (memory 0 3)))`);

// ./test/core/imports.wast:427
assert_unlinkable(
  () => instantiate(`(module (import "test" "unknown" (memory 1)))`),
  `unknown import`,
);

// ./test/core/imports.wast:431
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "unknown" (memory 1)))`),
  `unknown import`,
);

// ./test/core/imports.wast:436
assert_unlinkable(
  () => instantiate(`(module (import "test" "memory-2-inf" (memory 3)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:440
assert_unlinkable(
  () => instantiate(`(module (import "test" "memory-2-inf" (memory 2 3)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:444
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "memory" (memory 2)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:448
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "memory" (memory 1 1)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:453
assert_unlinkable(
  () => instantiate(`(module (import "test" "func-i32" (memory 1)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:457
assert_unlinkable(
  () => instantiate(`(module (import "test" "global-i32" (memory 1)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:461
assert_unlinkable(
  () => instantiate(`(module (import "test" "table-10-inf" (memory 1)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:465
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "print_i32" (memory 1)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:469
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "global_i32" (memory 1)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:473
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "table" (memory 1)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:478
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "memory" (memory 2)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:482
assert_unlinkable(
  () => instantiate(`(module (import "spectest" "memory" (memory 1 1)))`),
  `incompatible import type`,
);

// ./test/core/imports.wast:487
let $36 = instantiate(`(module
  (import "spectest" "memory" (memory 0 3))  ;; actual has max size 2
  (func (export "grow") (param i32) (result i32) (memory.grow (local.get 0)))
)`);

// ./test/core/imports.wast:491
assert_return(() => invoke($36, `grow`, [0]), [value("i32", 1)]);

// ./test/core/imports.wast:492
assert_return(() => invoke($36, `grow`, [1]), [value("i32", 1)]);

// ./test/core/imports.wast:493
assert_return(() => invoke($36, `grow`, [0]), [value("i32", 2)]);

// ./test/core/imports.wast:494
assert_return(() => invoke($36, `grow`, [1]), [value("i32", -1)]);

// ./test/core/imports.wast:495
assert_return(() => invoke($36, `grow`, [0]), [value("i32", 2)]);

// ./test/core/imports.wast:500
assert_malformed(
  () => instantiate(`(func) (import "" "" (func)) `),
  `import after function`,
);

// ./test/core/imports.wast:504
assert_malformed(
  () => instantiate(`(func) (import "" "" (global i64)) `),
  `import after function`,
);

// ./test/core/imports.wast:508
assert_malformed(
  () => instantiate(`(func) (import "" "" (table 0 funcref)) `),
  `import after function`,
);

// ./test/core/imports.wast:512
assert_malformed(
  () => instantiate(`(func) (import "" "" (memory 0)) `),
  `import after function`,
);

// ./test/core/imports.wast:517
assert_malformed(
  () => instantiate(`(global i64 (i64.const 0)) (import "" "" (func)) `),
  `import after global`,
);

// ./test/core/imports.wast:521
assert_malformed(
  () => instantiate(`(global i64 (i64.const 0)) (import "" "" (global f32)) `),
  `import after global`,
);

// ./test/core/imports.wast:525
assert_malformed(
  () => instantiate(`(global i64 (i64.const 0)) (import "" "" (table 0 funcref)) `),
  `import after global`,
);

// ./test/core/imports.wast:529
assert_malformed(
  () => instantiate(`(global i64 (i64.const 0)) (import "" "" (memory 0)) `),
  `import after global`,
);

// ./test/core/imports.wast:534
assert_malformed(
  () => instantiate(`(table 0 funcref) (import "" "" (func)) `),
  `import after table`,
);

// ./test/core/imports.wast:538
assert_malformed(
  () => instantiate(`(table 0 funcref) (import "" "" (global i32)) `),
  `import after table`,
);

// ./test/core/imports.wast:542
assert_malformed(
  () => instantiate(`(table 0 funcref) (import "" "" (table 0 funcref)) `),
  `import after table`,
);

// ./test/core/imports.wast:546
assert_malformed(
  () => instantiate(`(table 0 funcref) (import "" "" (memory 0)) `),
  `import after table`,
);

// ./test/core/imports.wast:551
assert_malformed(
  () => instantiate(`(memory 0) (import "" "" (func)) `),
  `import after memory`,
);

// ./test/core/imports.wast:555
assert_malformed(
  () => instantiate(`(memory 0) (import "" "" (global i32)) `),
  `import after memory`,
);

// ./test/core/imports.wast:559
assert_malformed(
  () => instantiate(`(memory 0) (import "" "" (table 1 3 funcref)) `),
  `import after memory`,
);

// ./test/core/imports.wast:563
assert_malformed(
  () => instantiate(`(memory 0) (import "" "" (memory 1 2)) `),
  `import after memory`,
);

// ./test/core/imports.wast:571
let $37 = instantiate(`(module)`);

// ./test/core/imports.wast:572
register($37, `not wasm`);

// ./test/core/imports.wast:573
assert_unlinkable(
  () => instantiate(`(module
    (import "not wasm" "overloaded" (func))
    (import "not wasm" "overloaded" (func (param i32)))
    (import "not wasm" "overloaded" (func (param i32 i32)))
    (import "not wasm" "overloaded" (func (param i64)))
    (import "not wasm" "overloaded" (func (param f32)))
    (import "not wasm" "overloaded" (func (param f64)))
    (import "not wasm" "overloaded" (func (result i32)))
    (import "not wasm" "overloaded" (func (result i64)))
    (import "not wasm" "overloaded" (func (result f32)))
    (import "not wasm" "overloaded" (func (result f64)))
    (import "not wasm" "overloaded" (global i32))
    (import "not wasm" "overloaded" (global i64))
    (import "not wasm" "overloaded" (global f32))
    (import "not wasm" "overloaded" (global f64))
    (import "not wasm" "overloaded" (table 0 funcref))
    (import "not wasm" "overloaded" (memory 0))
  )`),
  `unknown import`,
);
