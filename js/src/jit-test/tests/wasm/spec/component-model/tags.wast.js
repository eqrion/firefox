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

// ./test/linking/tags.wast

// ./test/linking/tags.wast:7
let $0 = instantiateComponent(`(component
  (core module \$Tag (tag (export "t") (param i32)))
  (core module \$Thrower
    (import "tag" "t" (tag \$t (param i32)))
    (func (export "throw-if") (param i32)
      (if (local.get 0)
        (then (throw \$t (local.get 0))))))
  (core module \$Catcher
    (import "tag" "t" (tag \$t (param i32)))
    (import "thrower" "throw-if" (func \$throw-if (param i32)))
    (func (export "run") (param i32) (result i32)
      (block \$h (result i32)
        (try_table (catch \$t \$h)
          (call \$throw-if (local.get 0)))
        (return (i32.const 99)))))
  (core instance \$tag (instantiate \$Tag))
  (core instance \$thrower (instantiate \$Thrower (with "tag" (instance \$tag))))
  (core instance \$catcher (instantiate \$Catcher
    (with "tag" (instance \$tag))
    (with "thrower" (instance \$thrower))))
  (func (export "run") (param "x" u32) (result u32)
    (canon lift (core func \$catcher "run")))
)`);

// ./test/linking/tags.wast:31
assert_return_component(() => invokeComponent($0, `run`, [cv.u32(42)]), [cv.u32(42)]);

// ./test/linking/tags.wast:32
assert_return_component(() => invokeComponent($0, `run`, [cv.u32(0)]), [cv.u32(99)]);

// ./test/linking/tags.wast:36
let $1 = instantiateComponent(`(component
  (core module \$Tag (tag (export "t") (param i32)))
  (core module \$Thrower
    (import "env" "exn" (tag \$t (param i32)))
    (func (export "throw") (param i32)
      (throw \$t (local.get 0))))
  (core module \$Catcher
    (import "env" "exn" (tag \$t (param i32)))
    (import "thrower" "throw" (func \$throw (param i32)))
    (func (export "run") (param i32) (result i32)
      (block \$h (result i32)
        (try_table (catch \$t \$h)
          (call \$throw (local.get 0)))
        (return (i32.const 99)))))
  (core instance \$tag (instantiate \$Tag))
  (core instance \$thrower (instantiate \$Thrower
    (with "env" (instance (export "exn" (tag \$tag "t"))))))
  (core instance \$catcher (instantiate \$Catcher
    (with "env" (instance (export "exn" (tag \$tag "t"))))
    (with "thrower" (instance \$thrower))))
  (func (export "run") (param "x" u32) (result u32)
    (canon lift (core func \$catcher "run")))
)`);

// ./test/linking/tags.wast:60
assert_return_component(() => invokeComponent($1, `run`, [cv.u32(7)]), [cv.u32(7)]);

// ./test/linking/tags.wast:64
let $2 = instantiateComponent(`(component
  (core module \$Tag (tag (export "t") (param i32)))
  (core module \$Thrower
    (import "tag" "t" (tag \$t (param i32)))
    (func (export "throw") (param i32)
      (throw \$t (local.get 0))))
  (core module \$Catcher
    (import "tag" "t" (tag \$t (param i32)))
    (import "thrower" "throw" (func \$throw (param i32)))
    (func (export "run") (param i32) (result i32)
      (block \$h (result i32)
        (try_table (catch \$t \$h)
          (call \$throw (local.get 0)))
        (return (i32.const 99)))))
  (core instance \$tag (instantiate \$Tag))
  (alias core export \$tag "t" (core tag \$t))
  (core instance \$syn (export "t" (tag \$t)))
  (core instance \$thrower (instantiate \$Thrower (with "tag" (instance \$syn))))
  (core instance \$catcher (instantiate \$Catcher
    (with "tag" (instance \$syn))
    (with "thrower" (instance \$thrower))))
  (func (export "run") (param "x" u32) (result u32)
    (canon lift (core func \$catcher "run")))
)`);

// ./test/linking/tags.wast:89
assert_return_component(() => invokeComponent($2, `run`, [cv.u32(13)]), [cv.u32(13)]);

// ./test/linking/tags.wast:93
let $3 = instantiateComponent(`(component
  (core module \$Tag (tag (export "t") (param i32)))
  (core module \$Thrower
    (import "tag" "t" (tag \$t (param i32)))
    (func (export "throw") (param i32)
      (throw \$t (local.get 0))))
  (core module \$Catcher
    (import "tag" "t" (tag \$t (param i32)))
    (import "a" "throw" (func \$throw-a (param i32)))
    (import "b" "throw" (func \$throw-b (param i32)))
    (func (export "run-a") (param i32) (result i32)
      (block \$all
        (block \$h (result i32)
          (try_table (catch \$t \$h) (catch_all \$all)
            (call \$throw-a (local.get 0)))
          (return (i32.const 99)))
        (return))
      (i32.const 77))
    (func (export "run-b") (param i32) (result i32)
      (block \$all
        (block \$h (result i32)
          (try_table (catch \$t \$h) (catch_all \$all)
            (call \$throw-b (local.get 0)))
          (return (i32.const 99)))
        (return))
      (i32.const 77)))
  (core instance \$tag-a (instantiate \$Tag))
  (core instance \$tag-b (instantiate \$Tag))
  (core instance \$ta (instantiate \$Thrower (with "tag" (instance \$tag-a))))
  (core instance \$tb (instantiate \$Thrower (with "tag" (instance \$tag-b))))
  (core instance \$c (instantiate \$Catcher
    (with "tag" (instance \$tag-a))
    (with "a" (instance \$ta))
    (with "b" (instance \$tb))))
  (func (export "run-a") (param "x" u32) (result u32)
    (canon lift (core func \$c "run-a")))
  (func (export "run-b") (param "x" u32) (result u32)
    (canon lift (core func \$c "run-b")))
)`);

// ./test/linking/tags.wast:133
assert_return_component(() => invokeComponent($3, `run-a`, [cv.u32(42)]), [cv.u32(42)]);

// ./test/linking/tags.wast:134
assert_return_component(() => invokeComponent($3, `run-b`, [cv.u32(42)]), [cv.u32(77)]);

// ./test/linking/tags.wast:137
assert_invalid(
  () => component(`(component
    (core module \$M (func (export "f")))
    (core instance \$i (instantiate \$M))
    (alias core export \$i "f" (core tag \$t)))`),
  `export \`f\` for core instance 0 is not a tag`,
);

// ./test/linking/tags.wast:145
assert_invalid(
  () => component(`(component
    (core module \$M (func (export "f")))
    (core instance \$i (instantiate \$M))
    (core instance (export "t" (tag 0))))`),
  `unknown tag 0: tag index out of bounds`,
);
