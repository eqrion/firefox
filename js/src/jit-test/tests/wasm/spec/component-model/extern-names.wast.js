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

// ./test/validation/extern-names.wast

// ./test/validation/extern-names.wast:8
let _anon_7 = component(`(component
  (import "wasi:http/types" (func))
  (import "wasi:http/types@1.0.0" (func))
  (import "wasi:http/types@2.0.0" (func))
  (import "a-b:c-d/e-f@123456.7890.488" (func))
  (import "a:b/c@0.0.0+abcd" (func))
  (import "a:b/c@0.0.0+abcd-efg" (func))
  (import "a:b/c@0.0.0-abcd+efg" (func))
  (import "a:b/c@0.0.0-abcd.1.2+efg.4.ee.5" (func))
)`);

// ./test/validation/extern-names.wast:18
assert_invalid(
  () => component(`(component
    (import "wasi:http/types@1.0.0" (func))
    (import "wasi:http/types@1.0.0" (func)))`),
  `conflicts with previous name`,
);

// ./test/validation/extern-names.wast:26
assert_invalid(() => component(`(component (import "a:b/c@" (func)))`), `empty string`);

// ./test/validation/extern-names.wast:29
assert_invalid(
  () => component(`(component (import "a:b/c@." (func)))`),
  `unexpected character '.'`,
);

// ./test/validation/extern-names.wast:32
assert_invalid(
  () => component(`(component (import "a:b/c@1." (func)))`),
  `unexpected end of input`,
);

// ./test/validation/extern-names.wast:35
assert_invalid(
  () => component(`(component (import "a:b/c@a.2" (func)))`),
  `unexpected character 'a'`,
);

// ./test/validation/extern-names.wast:38
assert_invalid(
  () => component(`(component (import "a:b/c@2.b" (func)))`),
  `unexpected character 'b'`,
);

// ./test/validation/extern-names.wast:41
assert_invalid(
  () => component(`(component (import "a:b/c@2.0x0" (func)))`),
  `unexpected character 'x'`,
);

// ./test/validation/extern-names.wast:44
assert_invalid(
  () => component(`(component (import "a:b/c@2.0.0+" (func)))`),
  `empty identifier segment`,
);

// ./test/validation/extern-names.wast:47
assert_invalid(
  () => component(`(component (import "a:b/c@2.0.0-" (func)))`),
  `empty identifier segment`,
);

// ./test/validation/extern-names.wast:53
assert_invalid(
  () => component(`(component (import "foo:bar:baz/qux" (func)))`),
  `expected \`/\` after package name`,
);

// ./test/validation/extern-names.wast:56
assert_invalid(
  () => component(`(component (import "foo:bar/baz/qux" (func)))`),
  `trailing characters found: \`/qux\``,
);
