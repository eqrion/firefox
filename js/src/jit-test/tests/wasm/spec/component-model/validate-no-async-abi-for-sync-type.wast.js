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

// ./test/async/validate-no-async-abi-for-sync-type.wast

// ./test/async/validate-no-async-abi-for-sync-type.wast:1
assert_invalid(
  () => component(`(component
    (core module \$M
      (func (export "f"))
    )
    (core instance \$i (instantiate \$M))
    (func (export "f") (canon lift (core func \$i "f") async))
  )`),
  `the \`async\` canonical option requires an async function type`,
);

// ./test/async/validate-no-async-abi-for-sync-type.wast:11
assert_invalid(
  () => component(`(component
    (core module \$M
      (func (export "f") (param i32) (result i32) unreachable)
      (func (export "f_cb") (param i32 i32 i32) (result i32) unreachable)
    )
    (core instance \$i (instantiate \$M))
    (func (export "f") (canon lift (core func \$i "f") async (callback (core func \$i "f_cb"))))
  )`),
  `the \`async\` canonical option requires an async function type`,
);

// ./test/async/validate-no-async-abi-for-sync-type.wast:22
assert_invalid(
  () => component(`(component
    (import "f" (func \$f))
    (core func \$f' (canon lower (func \$f) async))
  )`),
  `the \`async\` canonical option requires an async function type`,
);
