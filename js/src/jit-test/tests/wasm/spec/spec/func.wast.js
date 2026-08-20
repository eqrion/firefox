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

// ./test/core/func.wast

// ./test/core/func.wast:3
let $0 = instantiate(`(module
  ;; Auxiliary definition
  (type \$sig (func))
  (func \$dummy)

  ;; Syntax

  (func)
  (func (export "f"))
  (func \$f)
  (func \$h (export "g"))

  (func (local))
  (func (local) (local))
  (func (local i32))
  (func (local \$x i32))
  (func (local i32 f64 i64))
  (func (local i32) (local f64))
  (func (local i32 f32) (local \$x i64) (local) (local i32 f64))

  (func (param))
  (func (param) (param))
  (func (param i32))
  (func (param \$x i32))
  (func (param i32 f64 i64))
  (func (param i32) (param f64))
  (func (param i32 f32) (param \$x i64) (param) (param i32 f64))

  (func (result i32) (unreachable))

  (type \$sig-1 (func))
  (type \$sig-2 (func (result i32)))
  (type \$sig-3 (func (param \$x i32)))
  (type \$sig-4 (func (param i32 f64 i32) (result i32)))

  (func (export "type-use-1") (type \$sig-1))
  (func (export "type-use-2") (type \$sig-2) (i32.const 0))
  (func (export "type-use-3") (type \$sig-3))
  (func (export "type-use-4") (type \$sig-4) (i32.const 0))
  (func (export "type-use-5") (type \$sig-2) (result i32) (i32.const 0))
  (func (export "type-use-6") (type \$sig-3) (param i32))
  (func (export "type-use-7")
    (type \$sig-4) (param i32) (param f64 i32) (result i32) (i32.const 0)
  )

  (func (type \$sig))
  (func (type \$forward))  ;; forward reference

  (func \$complex
    (param i32 f32) (param \$x i64) (param) (param i32)
    (result) (result i32) (result)
    (local f32) (local \$y i32) (local i64 i32) (local) (local f64 i32)
    (unreachable) (unreachable)
  )
  (func \$complex-sig
    (type \$sig)
    (local f32) (local \$y i32) (local i64 i32) (local) (local f64 i32)
    (unreachable) (unreachable)
  )

  (type \$forward (func))

  ;; Typing of locals

  (func (export "local-first-i32") (result i32) (local i32 i32) (local.get 0))
  (func (export "local-first-i64") (result i64) (local i64 i64) (local.get 0))
  (func (export "local-first-f32") (result f32) (local f32 f32) (local.get 0))
  (func (export "local-first-f64") (result f64) (local f64 f64) (local.get 0))
  (func (export "local-second-i32") (result i32) (local i32 i32) (local.get 1))
  (func (export "local-second-i64") (result i64) (local i64 i64) (local.get 1))
  (func (export "local-second-f32") (result f32) (local f32 f32) (local.get 1))
  (func (export "local-second-f64") (result f64) (local f64 f64) (local.get 1))
  (func (export "local-mixed") (result f64)
    (local f32) (local \$x i32) (local i64 i32) (local) (local f64 i32)
    (drop (f32.neg (local.get 0)))
    (drop (i32.eqz (local.get 1)))
    (drop (i64.eqz (local.get 2)))
    (drop (i32.eqz (local.get 3)))
    (drop (f64.neg (local.get 4)))
    (drop (i32.eqz (local.get 5)))
    (local.get 4)
  )

  ;; Typing of parameters

  (func (export "param-first-i32") (param i32 i32) (result i32) (local.get 0))
  (func (export "param-first-i64") (param i64 i64) (result i64) (local.get 0))
  (func (export "param-first-f32") (param f32 f32) (result f32) (local.get 0))
  (func (export "param-first-f64") (param f64 f64) (result f64) (local.get 0))
  (func (export "param-second-i32") (param i32 i32) (result i32) (local.get 1))
  (func (export "param-second-i64") (param i64 i64) (result i64) (local.get 1))
  (func (export "param-second-f32") (param f32 f32) (result f32) (local.get 1))
  (func (export "param-second-f64") (param f64 f64) (result f64) (local.get 1))
  (func (export "param-mixed") (param f32 i32) (param) (param \$x i64) (param i32 f64 i32)
    (result f64)
    (drop (f32.neg (local.get 0)))
    (drop (i32.eqz (local.get 1)))
    (drop (i64.eqz (local.get 2)))
    (drop (i32.eqz (local.get 3)))
    (drop (f64.neg (local.get 4)))
    (drop (i32.eqz (local.get 5)))
    (local.get 4)
  )

  ;; Typing of result

  (func (export "empty"))
  (func (export "value-void") (call \$dummy))
  (func (export "value-i32") (result i32) (i32.const 77))
  (func (export "value-i64") (result i64) (i64.const 7777))
  (func (export "value-f32") (result f32) (f32.const 77.7))
  (func (export "value-f64") (result f64) (f64.const 77.77))
  (func (export "value-block-void") (block (call \$dummy) (call \$dummy)))
  (func (export "value-block-i32") (result i32)
    (block (result i32) (call \$dummy) (i32.const 77))
  )

  (func (export "return-empty") (return))
  (func (export "return-i32") (result i32) (return (i32.const 78)))
  (func (export "return-i64") (result i64) (return (i64.const 7878)))
  (func (export "return-f32") (result f32) (return (f32.const 78.7)))
  (func (export "return-f64") (result f64) (return (f64.const 78.78)))
  (func (export "return-block-i32") (result i32)
    (return (block (result i32) (call \$dummy) (i32.const 77)))
  )

  (func (export "break-empty") (br 0))
  (func (export "break-i32") (result i32) (br 0 (i32.const 79)))
  (func (export "break-i64") (result i64) (br 0 (i64.const 7979)))
  (func (export "break-f32") (result f32) (br 0 (f32.const 79.9)))
  (func (export "break-f64") (result f64) (br 0 (f64.const 79.79)))
  (func (export "break-block-i32") (result i32)
    (br 0 (block (result i32) (call \$dummy) (i32.const 77)))
  )

  (func (export "break-br_if-empty") (param i32)
    (br_if 0 (local.get 0))
  )
  (func (export "break-br_if-num") (param i32) (result i32)
    (drop (br_if 0 (i32.const 50) (local.get 0))) (i32.const 51)
  )

  (func (export "break-br_table-empty") (param i32)
    (br_table 0 0 0 (local.get 0))
  )
  (func (export "break-br_table-num") (param i32) (result i32)
    (br_table 0 0 (i32.const 50) (local.get 0)) (i32.const 51)
  )
  (func (export "break-br_table-nested-empty") (param i32)
    (block (br_table 0 1 0 (local.get 0)))
  )
  (func (export "break-br_table-nested-num") (param i32) (result i32)
    (i32.add
      (block (result i32)
        (br_table 0 1 0 (i32.const 50) (local.get 0)) (i32.const 51)
      )
      (i32.const 2)
    )
  )

  ;; Default initialization of locals

  (func (export "init-local-i32") (result i32) (local i32) (local.get 0))
  (func (export "init-local-i64") (result i64) (local i64) (local.get 0))
  (func (export "init-local-f32") (result f32) (local f32) (local.get 0))
  (func (export "init-local-f64") (result f64) (local f64) (local.get 0))
)`);

// ./test/core/func.wast:171
assert_return(() => invoke($0, `type-use-1`, []), []);

// ./test/core/func.wast:172
assert_return(() => invoke($0, `type-use-2`, []), [value("i32", 0)]);

// ./test/core/func.wast:173
assert_return(() => invoke($0, `type-use-3`, [1]), []);

// ./test/core/func.wast:174
assert_return(() => invoke($0, `type-use-4`, [1, value("f64", 1), 1]), [value("i32", 0)]);

// ./test/core/func.wast:178
assert_return(() => invoke($0, `type-use-5`, []), [value("i32", 0)]);

// ./test/core/func.wast:179
assert_return(() => invoke($0, `type-use-6`, [1]), []);

// ./test/core/func.wast:180
assert_return(() => invoke($0, `type-use-7`, [1, value("f64", 1), 1]), [value("i32", 0)]);

// ./test/core/func.wast:185
assert_return(() => invoke($0, `local-first-i32`, []), [value("i32", 0)]);

// ./test/core/func.wast:186
assert_return(() => invoke($0, `local-first-i64`, []), [value("i64", 0n)]);

// ./test/core/func.wast:187
assert_return(() => invoke($0, `local-first-f32`, []), [value("f32", 0)]);

// ./test/core/func.wast:188
assert_return(() => invoke($0, `local-first-f64`, []), [value("f64", 0)]);

// ./test/core/func.wast:189
assert_return(() => invoke($0, `local-second-i32`, []), [value("i32", 0)]);

// ./test/core/func.wast:190
assert_return(() => invoke($0, `local-second-i64`, []), [value("i64", 0n)]);

// ./test/core/func.wast:191
assert_return(() => invoke($0, `local-second-f32`, []), [value("f32", 0)]);

// ./test/core/func.wast:192
assert_return(() => invoke($0, `local-second-f64`, []), [value("f64", 0)]);

// ./test/core/func.wast:193
assert_return(() => invoke($0, `local-mixed`, []), [value("f64", 0)]);

// ./test/core/func.wast:195
assert_return(() => invoke($0, `param-first-i32`, [2, 3]), [value("i32", 2)]);

// ./test/core/func.wast:198
assert_return(() => invoke($0, `param-first-i64`, [2n, 3n]), [value("i64", 2n)]);

// ./test/core/func.wast:201
assert_return(
  () => invoke($0, `param-first-f32`, [value("f32", 2), value("f32", 3)]),
  [value("f32", 2)],
);

// ./test/core/func.wast:204
assert_return(
  () => invoke($0, `param-first-f64`, [value("f64", 2), value("f64", 3)]),
  [value("f64", 2)],
);

// ./test/core/func.wast:207
assert_return(() => invoke($0, `param-second-i32`, [2, 3]), [value("i32", 3)]);

// ./test/core/func.wast:210
assert_return(() => invoke($0, `param-second-i64`, [2n, 3n]), [value("i64", 3n)]);

// ./test/core/func.wast:213
assert_return(
  () => invoke($0, `param-second-f32`, [value("f32", 2), value("f32", 3)]),
  [value("f32", 3)],
);

// ./test/core/func.wast:216
assert_return(
  () => invoke($0, `param-second-f64`, [value("f64", 2), value("f64", 3)]),
  [value("f64", 3)],
);

// ./test/core/func.wast:220
assert_return(
  () => invoke($0, `param-mixed`, [value("f32", 1), 2, 3n, 4, value("f64", 5.5), 6]),
  [value("f64", 5.5)],
);

// ./test/core/func.wast:228
assert_return(() => invoke($0, `empty`, []), []);

// ./test/core/func.wast:229
assert_return(() => invoke($0, `value-void`, []), []);

// ./test/core/func.wast:230
assert_return(() => invoke($0, `value-i32`, []), [value("i32", 77)]);

// ./test/core/func.wast:231
assert_return(() => invoke($0, `value-i64`, []), [value("i64", 7777n)]);

// ./test/core/func.wast:232
assert_return(() => invoke($0, `value-f32`, []), [value("f32", 77.7)]);

// ./test/core/func.wast:233
assert_return(() => invoke($0, `value-f64`, []), [value("f64", 77.77)]);

// ./test/core/func.wast:234
assert_return(() => invoke($0, `value-block-void`, []), []);

// ./test/core/func.wast:235
assert_return(() => invoke($0, `value-block-i32`, []), [value("i32", 77)]);

// ./test/core/func.wast:237
assert_return(() => invoke($0, `return-empty`, []), []);

// ./test/core/func.wast:238
assert_return(() => invoke($0, `return-i32`, []), [value("i32", 78)]);

// ./test/core/func.wast:239
assert_return(() => invoke($0, `return-i64`, []), [value("i64", 7878n)]);

// ./test/core/func.wast:240
assert_return(() => invoke($0, `return-f32`, []), [value("f32", 78.7)]);

// ./test/core/func.wast:241
assert_return(() => invoke($0, `return-f64`, []), [value("f64", 78.78)]);

// ./test/core/func.wast:242
assert_return(() => invoke($0, `return-block-i32`, []), [value("i32", 77)]);

// ./test/core/func.wast:244
assert_return(() => invoke($0, `break-empty`, []), []);

// ./test/core/func.wast:245
assert_return(() => invoke($0, `break-i32`, []), [value("i32", 79)]);

// ./test/core/func.wast:246
assert_return(() => invoke($0, `break-i64`, []), [value("i64", 7979n)]);

// ./test/core/func.wast:247
assert_return(() => invoke($0, `break-f32`, []), [value("f32", 79.9)]);

// ./test/core/func.wast:248
assert_return(() => invoke($0, `break-f64`, []), [value("f64", 79.79)]);

// ./test/core/func.wast:249
assert_return(() => invoke($0, `break-block-i32`, []), [value("i32", 77)]);

// ./test/core/func.wast:251
assert_return(() => invoke($0, `break-br_if-empty`, [0]), []);

// ./test/core/func.wast:252
assert_return(() => invoke($0, `break-br_if-empty`, [2]), []);

// ./test/core/func.wast:253
assert_return(() => invoke($0, `break-br_if-num`, [0]), [value("i32", 51)]);

// ./test/core/func.wast:254
assert_return(() => invoke($0, `break-br_if-num`, [1]), [value("i32", 50)]);

// ./test/core/func.wast:256
assert_return(() => invoke($0, `break-br_table-empty`, [0]), []);

// ./test/core/func.wast:257
assert_return(() => invoke($0, `break-br_table-empty`, [1]), []);

// ./test/core/func.wast:258
assert_return(() => invoke($0, `break-br_table-empty`, [5]), []);

// ./test/core/func.wast:259
assert_return(() => invoke($0, `break-br_table-empty`, [-1]), []);

// ./test/core/func.wast:260
assert_return(() => invoke($0, `break-br_table-num`, [0]), [value("i32", 50)]);

// ./test/core/func.wast:261
assert_return(() => invoke($0, `break-br_table-num`, [1]), [value("i32", 50)]);

// ./test/core/func.wast:262
assert_return(() => invoke($0, `break-br_table-num`, [10]), [value("i32", 50)]);

// ./test/core/func.wast:263
assert_return(() => invoke($0, `break-br_table-num`, [-100]), [value("i32", 50)]);

// ./test/core/func.wast:264
assert_return(() => invoke($0, `break-br_table-nested-empty`, [0]), []);

// ./test/core/func.wast:265
assert_return(() => invoke($0, `break-br_table-nested-empty`, [1]), []);

// ./test/core/func.wast:266
assert_return(() => invoke($0, `break-br_table-nested-empty`, [3]), []);

// ./test/core/func.wast:267
assert_return(() => invoke($0, `break-br_table-nested-empty`, [-2]), []);

// ./test/core/func.wast:268
assert_return(() => invoke($0, `break-br_table-nested-num`, [0]), [value("i32", 52)]);

// ./test/core/func.wast:271
assert_return(() => invoke($0, `break-br_table-nested-num`, [1]), [value("i32", 50)]);

// ./test/core/func.wast:274
assert_return(() => invoke($0, `break-br_table-nested-num`, [2]), [value("i32", 52)]);

// ./test/core/func.wast:277
assert_return(() => invoke($0, `break-br_table-nested-num`, [-3]), [value("i32", 52)]);

// ./test/core/func.wast:281
assert_return(() => invoke($0, `init-local-i32`, []), [value("i32", 0)]);

// ./test/core/func.wast:282
assert_return(() => invoke($0, `init-local-i64`, []), [value("i64", 0n)]);

// ./test/core/func.wast:283
assert_return(() => invoke($0, `init-local-f32`, []), [value("f32", 0)]);

// ./test/core/func.wast:284
assert_return(() => invoke($0, `init-local-f64`, []), [value("f64", 0)]);

// ./test/core/func.wast:289
let $1 = instantiate(`(module
  (func \$f (result f64) (f64.const 0))  ;; adds implicit type definition
  (func \$g (param i32))                 ;; reuses explicit type definition
  (type \$t (func (param i32)))

  (func \$i32->void (type 0))                ;; (param i32)
  (func \$void->f64 (type 1) (f64.const 0))  ;; (result f64)
  (func \$check
    (call \$i32->void (i32.const 0))
    (drop (call \$void->f64))
  )
)`);

// ./test/core/func.wast:302
assert_invalid(
  () => instantiate(`(module
    (func \$f (result f64) (f64.const 0))  ;; adds implicit type definition
    (func \$g (param i32))                 ;; reuses explicit type definition
    (func \$h (result f64) (f64.const 1))  ;; reuses implicit type definition
    (type \$t (func (param i32)))

    (func (type 2))  ;; does not exist
  )`),
  `unknown type`,
);

// ./test/core/func.wast:315
let $2 = instantiate(`(module
  (type \$sig (func))

  (func \$empty-sig-1)  ;; should be assigned type \$sig
  (func \$complex-sig-1 (param f64 i64 f64 i64 f64 i64 f32 i32))
  (func \$empty-sig-2)  ;; should be assigned type \$sig
  (func \$complex-sig-2 (param f64 i64 f64 i64 f64 i64 f32 i32))
  (func \$complex-sig-3 (param f64 i64 f64 i64 f64 i64 f32 i32))
  (func \$complex-sig-4 (param i64 i64 f64 i64 f64 i64 f32 i32))
  (func \$complex-sig-5 (param i64 i64 f64 i64 f64 i64 f32 i32))

  (type \$empty-sig-duplicate (func))
  (type \$complex-sig-duplicate (func (param i64 i64 f64 i64 f64 i64 f32 i32)))
  (table funcref
    (elem
      \$complex-sig-3 \$empty-sig-2 \$complex-sig-1 \$complex-sig-3 \$empty-sig-1
      \$complex-sig-4 \$complex-sig-5
    )
  )

  (func (export "signature-explicit-reused")
    (call_indirect (type \$sig) (i32.const 1))
    (call_indirect (type \$sig) (i32.const 4))
  )

  (func (export "signature-implicit-reused")
    ;; The implicit index 3 in this test depends on the function and
    ;; type definitions, and may need adapting if they change.
    (call_indirect (type 3)
      (f64.const 0) (i64.const 0) (f64.const 0) (i64.const 0)
      (f64.const 0) (i64.const 0) (f32.const 0) (i32.const 0)
      (i32.const 0)
    )
    (call_indirect (type 3)
      (f64.const 0) (i64.const 0) (f64.const 0) (i64.const 0)
      (f64.const 0) (i64.const 0) (f32.const 0) (i32.const 0)
      (i32.const 2)
    )
    (call_indirect (type 3)
      (f64.const 0) (i64.const 0) (f64.const 0) (i64.const 0)
      (f64.const 0) (i64.const 0) (f32.const 0) (i32.const 0)
      (i32.const 3)
    )
  )

  (func (export "signature-explicit-duplicate")
    (call_indirect (type \$empty-sig-duplicate) (i32.const 1))
  )

  (func (export "signature-implicit-duplicate")
    (call_indirect (type \$complex-sig-duplicate)
      (i64.const 0) (i64.const 0) (f64.const 0) (i64.const 0)
      (f64.const 0) (i64.const 0) (f32.const 0) (i32.const 0)
      (i32.const 5)
    )
    (call_indirect (type \$complex-sig-duplicate)
      (i64.const 0) (i64.const 0) (f64.const 0) (i64.const 0)
      (f64.const 0) (i64.const 0) (f32.const 0) (i32.const 0)
      (i32.const 6)
    )
  )
)`);

// ./test/core/func.wast:378
assert_return(() => invoke($2, `signature-explicit-reused`, []), []);

// ./test/core/func.wast:379
assert_return(() => invoke($2, `signature-implicit-reused`, []), []);

// ./test/core/func.wast:380
assert_return(() => invoke($2, `signature-explicit-duplicate`, []), []);

// ./test/core/func.wast:381
assert_return(() => invoke($2, `signature-implicit-duplicate`, []), []);

// ./test/core/func.wast:386
assert_malformed(
  () => instantiate(`(type \$sig (func (param i32) (result i32))) (func (type \$sig) (result i32) (param i32) (i32.const 0)) `),
  `unexpected token`,
);

// ./test/core/func.wast:393
assert_malformed(
  () => instantiate(`(type \$sig (func (param i32) (result i32))) (func (param i32) (type \$sig) (result i32) (i32.const 0)) `),
  `unexpected token`,
);

// ./test/core/func.wast:400
assert_malformed(
  () => instantiate(`(type \$sig (func (param i32) (result i32))) (func (param i32) (result i32) (type \$sig) (i32.const 0)) `),
  `unexpected token`,
);

// ./test/core/func.wast:407
assert_malformed(
  () => instantiate(`(type \$sig (func (param i32) (result i32))) (func (result i32) (type \$sig) (param i32) (i32.const 0)) `),
  `unexpected token`,
);

// ./test/core/func.wast:414
assert_malformed(
  () => instantiate(`(type \$sig (func (param i32) (result i32))) (func (result i32) (param i32) (type \$sig) (i32.const 0)) `),
  `unexpected token`,
);

// ./test/core/func.wast:421
assert_malformed(
  () => instantiate(`(func (result i32) (param i32) (i32.const 0)) `),
  `unexpected token`,
);

// ./test/core/func.wast:428
assert_malformed(
  () => instantiate(`(type \$sig (func)) (func (type \$sig) (result i32) (i32.const 0)) `),
  `inline function type`,
);

// ./test/core/func.wast:435
assert_malformed(
  () => instantiate(`(type \$sig (func (param i32) (result i32))) (func (type \$sig) (result i32) (i32.const 0)) `),
  `inline function type`,
);

// ./test/core/func.wast:442
assert_malformed(
  () => instantiate(`(type \$sig (func (param i32) (result i32))) (func (type \$sig) (param i32) (i32.const 0)) `),
  `inline function type`,
);

// ./test/core/func.wast:449
assert_malformed(
  () => instantiate(`(type \$sig (func (param i32 i32) (result i32))) (func (type \$sig) (param i32) (result i32) (unreachable)) `),
  `inline function type`,
);

// ./test/core/func.wast:460
assert_invalid(
  () => instantiate(`(module (func \$type-local-num-vs-num (result i64) (local i32) (local.get 0)))`),
  `type mismatch`,
);

// ./test/core/func.wast:464
assert_invalid(
  () => instantiate(`(module (func \$type-local-num-vs-num (local f32) (i32.eqz (local.get 0))))`),
  `type mismatch`,
);

// ./test/core/func.wast:468
assert_invalid(
  () => instantiate(`(module (func \$type-local-num-vs-num (local f64 i64) (f64.neg (local.get 1))))`),
  `type mismatch`,
);

// ./test/core/func.wast:476
assert_invalid(
  () => instantiate(`(module (func \$type-param-num-vs-num (param i32) (result i64) (local.get 0)))`),
  `type mismatch`,
);

// ./test/core/func.wast:480
assert_invalid(
  () => instantiate(`(module (func \$type-param-num-vs-num (param f32) (i32.eqz (local.get 0))))`),
  `type mismatch`,
);

// ./test/core/func.wast:484
assert_invalid(
  () => instantiate(`(module (func \$type-param-num-vs-num (param f64 i64) (f64.neg (local.get 1))))`),
  `type mismatch`,
);

// ./test/core/func.wast:492
assert_invalid(
  () => instantiate(`(module (func \$type-multiple-result (result i32 i32) (unreachable)))`),
  `invalid result arity`,
);

// ./test/core/func.wast:496
assert_invalid(
  () => instantiate(`(module
    (type (func (result i32 i32)))
    (func \$type-multiple-result (type 0) (unreachable))
  )`),
  `invalid result arity`,
);

// ./test/core/func.wast:505
assert_invalid(
  () => instantiate(`(module (func \$type-empty-i32 (result i32)))`),
  `type mismatch`,
);

// ./test/core/func.wast:509
assert_invalid(
  () => instantiate(`(module (func \$type-empty-i64 (result i64)))`),
  `type mismatch`,
);

// ./test/core/func.wast:513
assert_invalid(
  () => instantiate(`(module (func \$type-empty-f32 (result f32)))`),
  `type mismatch`,
);

// ./test/core/func.wast:517
assert_invalid(
  () => instantiate(`(module (func \$type-empty-f64 (result f64)))`),
  `type mismatch`,
);

// ./test/core/func.wast:522
assert_invalid(
  () => instantiate(`(module (func \$type-value-void-vs-num (result i32)
    (nop)
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:528
assert_invalid(
  () => instantiate(`(module (func \$type-value-num-vs-void
    (i32.const 0)
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:534
assert_invalid(
  () => instantiate(`(module (func \$type-value-num-vs-num (result i32)
    (f32.const 0)
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:541
assert_invalid(
  () => instantiate(`(module (func \$type-return-last-empty-vs-num (result i32)
    (return)
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:547
assert_invalid(
  () => instantiate(`(module (func \$type-return-last-void-vs-num (result i32)
    (return (nop))
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:553
assert_invalid(
  () => instantiate(`(module (func \$type-return-last-num-vs-num (result i32)
    (return (i64.const 0))
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:560
assert_invalid(
  () => instantiate(`(module (func \$type-return-empty-vs-num (result i32)
    (return) (i32.const 1)
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:566
assert_invalid(
  () => instantiate(`(module (func \$type-return-void-vs-num (result i32)
    (return (nop)) (i32.const 1)
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:572
assert_invalid(
  () => instantiate(`(module (func \$type-return-num-vs-num (result i32)
    (return (i64.const 1)) (i32.const 1)
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:578
assert_invalid(
  () => instantiate(`(module (func \$type-return-first-num-vs-num (result i32)
    (return (i64.const 1)) (return (i32.const 1))
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:585
assert_invalid(
  () => instantiate(`(module (func \$type-break-last-void-vs-num (result i32)
    (br 0)
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:591
assert_invalid(
  () => instantiate(`(module (func \$type-break-last-num-vs-num (result i32)
    (br 0 (f32.const 0))
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:597
assert_invalid(
  () => instantiate(`(module (func \$type-break-void-vs-num (result i32)
    (br 0) (i32.const 1)
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:603
assert_invalid(
  () => instantiate(`(module (func \$type-break-num-vs-num (result i32)
    (br 0 (i64.const 1)) (i32.const 1)
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:609
assert_invalid(
  () => instantiate(`(module (func \$type-break-first-num-vs-num (result i32)
    (br 0 (i64.const 1)) (br 0 (i32.const 1))
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:616
assert_invalid(
  () => instantiate(`(module (func \$type-break-nested-empty-vs-num (result i32)
    (block (br 1)) (br 0 (i32.const 1))
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:622
assert_invalid(
  () => instantiate(`(module (func \$type-break-nested-void-vs-num (result i32)
    (block (br 1 (nop))) (br 0 (i32.const 1))
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:628
assert_invalid(
  () => instantiate(`(module (func \$type-break-nested-num-vs-num (result i32)
    (block (br 1 (i64.const 1))) (br 0 (i32.const 1))
  ))`),
  `type mismatch`,
);

// ./test/core/func.wast:638
assert_malformed(() => instantiate(`(func (nop) (local i32)) `), `unexpected token`);

// ./test/core/func.wast:642
assert_malformed(() => instantiate(`(func (nop) (param i32)) `), `unexpected token`);

// ./test/core/func.wast:646
assert_malformed(() => instantiate(`(func (nop) (result i32)) `), `unexpected token`);

// ./test/core/func.wast:650
assert_malformed(() => instantiate(`(func (local i32) (param i32)) `), `unexpected token`);

// ./test/core/func.wast:654
assert_malformed(
  () => instantiate(`(func (local i32) (result i32) (local.get 0)) `),
  `unexpected token`,
);

// ./test/core/func.wast:658
assert_malformed(
  () => instantiate(`(func (result i32) (param i32) (local.get 0)) `),
  `unexpected token`,
);

// ./test/core/func.wast:665
assert_malformed(() => instantiate(`(func \$foo) (func \$foo) `), `duplicate func`);

// ./test/core/func.wast:669
assert_malformed(
  () => instantiate(`(import "" "" (func \$foo)) (func \$foo) `),
  `duplicate func`,
);

// ./test/core/func.wast:673
assert_malformed(
  () => instantiate(`(import "" "" (func \$foo)) (import "" "" (func \$foo)) `),
  `duplicate func`,
);

// ./test/core/func.wast:678
assert_malformed(
  () => instantiate(`(func (param \$foo i32) (param \$foo i32)) `),
  `duplicate local`,
);

// ./test/core/func.wast:680
assert_malformed(
  () => instantiate(`(func (param \$foo i32) (local \$foo i32)) `),
  `duplicate local`,
);

// ./test/core/func.wast:682
assert_malformed(
  () => instantiate(`(func (local \$foo i32) (local \$foo i32)) `),
  `duplicate local`,
);
