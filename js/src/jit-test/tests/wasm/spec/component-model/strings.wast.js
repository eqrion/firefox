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

// ./test/values/strings.wast

// ./test/values/strings.wast:1
let $0 = instantiateComponent(`(component
  (core module \$M
    (memory (export "mem") 1)
    (func (export "f1") (result i32)
      (i32.store (i32.const 0) (i32.const 8))
      (i32.store (i32.const 4) (i32.const 1))
      (i32.store8 (i32.const 8) (i32.const 97))
      (i32.const 0)
    )
    (func (export "f2") (result i32)
      (i32.store (i32.const 0) (i32.const 8))
      (i32.store (i32.const 4) (i32.const 14))
      (i64.store (i32.const 8) (i64.const 0xb8_ef_ba_98_e2_83_98_e2))
      (i32.store (i32.const 16) (i32.const 0xe3_b6_c3_8f))
      (i32.store16 (i32.const 20) (i32.const 0x84_83))
      (i32.const 0)
    )
  )
  (core instance \$m (instantiate \$M))
  (func (export "f1") (result string) (canon lift (core func \$m "f1") (memory (core memory \$m "mem"))))
  (func (export "f2") (result string) (canon lift (core func \$m "f2") (memory (core memory \$m "mem"))))
)`);

// ./test/values/strings.wast:23
assert_return_component(() => invokeComponent($0, `f1`, []), [cv.string(`a`)]);

// ./test/values/strings.wast:24
assert_return_component(
  () => invokeComponent($0, `f2`, []),
  [cv.string(`\u{2603}\u{263a}\u{fe0f}\u{f6}\u{30c4}`)],
);

// ./test/values/strings.wast:27
let $1 = instantiateComponent(`(component
  (core module \$M
    (memory (export "mem") 1)
    (func (export "f") (result i32)
      (i32.store (i32.const 0) (i32.const 0))
      (i32.store (i32.const 4) (i32.const 0))
      (i32.const 0)
    )
  )
  (core instance \$m (instantiate \$M))
  (func (export "f") (result string) (canon lift (core func \$m "f") (memory (core memory \$m "mem"))))
)`);

// ./test/values/strings.wast:39
assert_return_component(() => invokeComponent($1, `f`, []), [cv.string(``)]);

// ./test/values/strings.wast:42
let $2 = instantiateComponent(`(component
  (core module \$M
    (memory (export "mem") 1)
    (func (export "f") (result i32)
      (i32.store (i32.const 0) (i32.const 100))
      (i32.store (i32.const 4) (i32.const 0))
      (i32.const 0)
    )
  )
  (core instance \$m (instantiate \$M))
  (func (export "f") (result string) (canon lift (core func \$m "f") (memory (core memory \$m "mem"))))
)`);

// ./test/values/strings.wast:54
assert_return_component(() => invokeComponent($2, `f`, []), [cv.string(``)]);

// ./test/values/strings.wast:57
let $3 = instantiateComponent(`(component
  (core module \$M
    (memory (export "mem") 1)
    (func (export "f") (result i32)
      (i32.store (i32.const 0) (i32.const 0xdeadbeef))
      (i32.store (i32.const 4) (i32.const 0))
      (i32.const 0)
    )
  )
  (core instance \$m (instantiate \$M))
  (func (export "f") (result string) (canon lift (core func \$m "f") (memory (core memory \$m "mem"))))
)`);

// ./test/values/strings.wast:69
assert_trap(
  () => invokeComponent($3, `f`, []),
  `string pointer/length out of bounds of memory`,
);

// ./test/values/strings.wast:72
let $4 = instantiateComponent(`(component
  (core module \$M
    (memory (export "mem") 1)
    (func (export "f") (result i32)
      (i32.store (i32.const 0) (i32.const 8))
      (i32.store (i32.const 4) (i32.const 1))
      (i32.store8 (i32.const 8) (i32.const 0xff))
      (i32.const 0)
    )
  )
  (core instance \$m (instantiate \$M))
  (func (export "f") (result string) (canon lift (core func \$m "f") (memory (core memory \$m "mem"))))
)`);

// ./test/values/strings.wast:85
assert_trap(() => invokeComponent($4, `f`, []), `invalid utf-8`);

// ./test/values/strings.wast:88
let $5 = instantiateComponent(`(component
  (core module \$M
    (memory (export "mem") 1)
    (func (export "f") (result i32)
      (i32.store (i32.const 0) (i32.const 8))
      (i32.store (i32.const 4) (i32.const 1))
      (i32.store8 (i32.const 8) (i32.const 0xc3))
      (i32.const 0)
    )
  )
  (core instance \$m (instantiate \$M))
  (func (export "f") (result string) (canon lift (core func \$m "f") (memory (core memory \$m "mem"))))
)`);

// ./test/values/strings.wast:101
assert_trap(() => invokeComponent($5, `f`, []), `incomplete utf-8 byte sequence`);

// ./test/values/strings.wast:104
let $6 = instantiateComponent(`(component
  (core module \$M
    (memory (export "mem") 1)
    (func (export "f") (result i32)
      ;; place "ok" at the very end of the first page (65536 - 2 = 65534)
      (i32.store (i32.const 0) (i32.const 65534))
      (i32.store (i32.const 4) (i32.const 2))
      (i32.store8 (i32.const 65534) (i32.const 111)) ;; 'o'
      (i32.store8 (i32.const 65535) (i32.const 107)) ;; 'k'
      (i32.const 0)
    )
  )
  (core instance \$m (instantiate \$M))
  (func (export "f") (result string) (canon lift (core func \$m "f") (memory (core memory \$m "mem"))))
)`);

// ./test/values/strings.wast:119
assert_return_component(() => invokeComponent($6, `f`, []), [cv.string(`ok`)]);

// ./test/values/strings.wast:122
let $7 = instantiateComponent(`(component
  (core module \$M
    (memory (export "mem") 1)
    (func (export "f") (result i32)
      (i32.store (i32.const 0) (i32.const 65535))
      (i32.store (i32.const 4) (i32.const 2))
      (i32.store8 (i32.const 65535) (i32.const 111))
      (i32.const 0)
    )
  )
  (core instance \$m (instantiate \$M))
  (func (export "f") (result string) (canon lift (core func \$m "f") (memory (core memory \$m "mem"))))
)`);

// ./test/values/strings.wast:135
assert_trap(
  () => invokeComponent($7, `f`, []),
  `string pointer/length out of bounds of memory`,
);
