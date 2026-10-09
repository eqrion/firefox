// Assertions shared by the component JS-API tests.

function assert_function_name(fn, name, description) {
  const propdesc = Object.getOwnPropertyDescriptor(fn, "name");
  assert_equals(typeof propdesc, "object", `${description} should have name property`);
  assert_false(propdesc.writable, `${description} name should not be writable`);
  assert_false(propdesc.enumerable, `${description} name should not be enumerable`);
  assert_true(propdesc.configurable, `${description} name should be configurable`);
  assert_equals(propdesc.value, name, `${description} name should be ${name}`);
}

function assert_function_length(fn, length, description) {
  const propdesc = Object.getOwnPropertyDescriptor(fn, "length");
  assert_equals(typeof propdesc, "object", `${description} should have length property`);
  assert_false(propdesc.writable, `${description} length should not be writable`);
  assert_false(propdesc.enumerable, `${description} length should not be enumerable`);
  assert_true(propdesc.configurable, `${description} length should be configurable`);
  assert_equals(propdesc.value, length, `${description} length should be ${length}`);
}

// A built-in function object created by the JS-API: a plain function that
// is not a constructor.
function assert_builtin_function(fn, { name, length }, description) {
  assert_equals(typeof fn, "function", `${description}: typeof`);
  assert_equals(Object.getPrototypeOf(fn), Function.prototype, `${description}: prototype`);
  assert_function_name(fn, name, description);
  assert_function_length(fn, length, description);
  assert_false(Object.hasOwn(fn, "prototype"), `${description}: has no prototype property`);
  assert_throws_js(TypeError, () => new fn(), `${description}: not a constructor`);
  assert_true(Function.prototype.toString.call(fn).includes("[native code]"),
              `${description}: toString is native`);
}

// A frozen null-prototype object with exactly `expectedKeys` as own enumerable
// properties, in that order. Data properties are non-writable and
// non-configurable; accessor properties are non-configurable.
function assert_exports_object(exports, expectedKeys, description = "exports") {
  assert_equals(typeof exports, "object", `${description}: typeof`);
  assert_equals(Object.getPrototypeOf(exports), null, `${description}: prototype`);
  assert_true(Object.isFrozen(exports), `${description}: frozen`);
  assert_array_equals(Reflect.ownKeys(exports), expectedKeys, `${description}: keys`);
  for (const key of expectedKeys) {
    const desc = Object.getOwnPropertyDescriptor(exports, key);
    assert_true(desc.enumerable, `${description}.${String(key)}: enumerable`);
    assert_false(desc.configurable, `${description}.${String(key)}: configurable`);
    if (Object.hasOwn(desc, "value")) {
      assert_false(desc.writable, `${description}.${String(key)}: writable`);
    }
  }
}

function assert_ComponentInstance(instance, expectedKeys, description = "instance") {
  assert_equals(Object.getPrototypeOf(instance), WebAssembly.ComponentInstance.prototype,
                `${description}: prototype`);
  assert_true(Object.isExtensible(instance), `${description}: extensible`);
  assert_equals(instance.exports, instance.exports, `${description}: exports should be idempotent`);
  assert_exports_object(instance.exports, expectedKeys, `${description}.exports`);
}

// Checks the property descriptor of a method installed on a class or prototype.
function assert_method_descriptor(target, key, { name = key, length }, description) {
  const desc = Object.getOwnPropertyDescriptor(target, key);
  assert_equals(typeof desc, "object", `${description}: present`);
  assert_true(desc.writable, `${description}: writable`);
  assert_false(desc.enumerable, `${description}: enumerable`);
  assert_true(desc.configurable, `${description}: configurable`);
  assert_builtin_function(desc.value, { name, length }, description);
}

// Checks the property descriptor of an accessor installed on a class,
// prototype or the exports object. `settable` says whether a setter exists.
function assert_accessor_descriptor(target, key, { settable, enumerable = false, configurable = true },
                                    description) {
  const desc = Object.getOwnPropertyDescriptor(target, key);
  assert_equals(typeof desc, "object", `${description}: present`);
  assert_false(Object.hasOwn(desc, "value"), `${description}: is an accessor`);
  assert_equals(desc.enumerable, enumerable, `${description}: enumerable`);
  assert_equals(desc.configurable, configurable, `${description}: configurable`);
  assert_builtin_function(desc.get, { name: `get ${key}`, length: 0 }, `${description} getter`);
  if (settable) {
    assert_builtin_function(desc.set, { name: `set ${key}`, length: 1 }, `${description} setter`);
  } else {
    assert_equals(desc.set, undefined, `${description}: no setter`);
  }
}

// A guest resource class as created by `create a guest resource class`.
// `methods`, `statics` are arrays of [key, length]; `accessors` and
// `staticAccessors` are arrays of [key, settable].
function assert_guest_class(cls, { name, length = 0, methods = [], statics = [], accessors = [],
                                   staticAccessors = [] }, description = name) {
  assert_equals(typeof cls, "function", `${description}: typeof`);
  assert_equals(Object.getPrototypeOf(cls), Function.prototype, `${description}: prototype chain`);
  assert_function_name(cls, name, description);
  assert_function_length(cls, length, description);
  assert_throws_js(TypeError, () => cls(), `${description}: call without new`);

  const protoDesc = Object.getOwnPropertyDescriptor(cls, "prototype");
  assert_equals(typeof protoDesc, "object", `${description}.prototype: present`);
  assert_false(protoDesc.writable, `${description}.prototype: writable`);
  assert_false(protoDesc.enumerable, `${description}.prototype: enumerable`);
  assert_false(protoDesc.configurable, `${description}.prototype: configurable`);
  const proto = protoDesc.value;
  assert_equals(Object.getPrototypeOf(proto), Object.prototype, `${description}.prototype: prototype`);

  const ctorDesc = Object.getOwnPropertyDescriptor(proto, "constructor");
  assert_equals(ctorDesc.value, cls, `${description}.prototype.constructor`);
  assert_true(ctorDesc.writable, `${description}.prototype.constructor: writable`);
  assert_false(ctorDesc.enumerable, `${description}.prototype.constructor: enumerable`);
  assert_true(ctorDesc.configurable, `${description}.prototype.constructor: configurable`);

  const tagDesc = Object.getOwnPropertyDescriptor(proto, Symbol.toStringTag);
  assert_equals(tagDesc.value, name, `${description}.prototype[Symbol.toStringTag]`);
  assert_false(tagDesc.writable, `${description}.prototype[Symbol.toStringTag]: writable`);
  assert_false(tagDesc.enumerable, `${description}.prototype[Symbol.toStringTag]: enumerable`);
  assert_true(tagDesc.configurable, `${description}.prototype[Symbol.toStringTag]: configurable`);

  const disposeDesc = Object.getOwnPropertyDescriptor(proto, Symbol.dispose);
  assert_equals(typeof disposeDesc, "object", `${description}.prototype[Symbol.dispose]: present`);
  assert_true(disposeDesc.writable, `${description}.prototype[Symbol.dispose]: writable`);
  assert_false(disposeDesc.enumerable, `${description}.prototype[Symbol.dispose]: enumerable`);
  assert_true(disposeDesc.configurable, `${description}.prototype[Symbol.dispose]: configurable`);
  assert_equals(typeof disposeDesc.value, "function", `${description}.prototype[Symbol.dispose]: function`);
  assert_function_name(disposeDesc.value, "[Symbol.dispose]", `${description}.prototype[Symbol.dispose]`);
  assert_function_length(disposeDesc.value, 0, `${description}.prototype[Symbol.dispose]`);

  for (const [key, methodLength] of methods) {
    assert_method_descriptor(proto, key, { length: methodLength }, `${description}.prototype.${key}`);
  }
  for (const [key, methodLength] of statics) {
    assert_method_descriptor(cls, key, { length: methodLength }, `${description}.${key}`);
  }
  for (const [key, settable] of accessors) {
    assert_accessor_descriptor(proto, key, { settable }, `${description}.prototype.${key}`);
  }
  for (const [key, settable] of staticAccessors) {
    assert_accessor_descriptor(cls, key, { settable }, `${description}.${key}`);
  }

  const expectedProtoKeys = ["constructor", ...methods.map(([k]) => k), ...accessors.map(([k]) => k),
                             Symbol.dispose, Symbol.toStringTag];
  assert_array_equals(Reflect.ownKeys(proto).map(String).sort(), expectedProtoKeys.map(String).sort(),
                      `${description}.prototype: no extra properties`);
  const expectedClassKeys = ["length", "name", "prototype", ...statics.map(([k]) => k),
                             ...staticAccessors.map(([k]) => k)];
  assert_array_equals(Reflect.ownKeys(cls).map(String).sort(), expectedClassKeys.map(String).sort(),
                      `${description}: no extra properties`);
}

// Builders for expected component values in their JS shape.
function record(fields) {
  return Object.assign(Object.create(null), fields);
}

function variant(kind, ...value) {
  const object = Object.create(null);
  object.kind = kind;
  if (value.length > 0) {
    object.value = value[0];
  }
  return object;
}

const flags = record;

// Deep structural equality for converted component values: prototypes must
// match (null-prototype objects for record/variant/flags, Array, Map, typed
// arrays), numbers compare with Object.is so NaN and -0 are distinguished,
// Map entries compare in insertion order.
function assert_value_equals(actual, expected, description = "value") {
  if (typeof expected !== "object" || expected === null) {
    if (typeof expected === "number") {
      assert_true(Object.is(actual, expected),
                  `${description}: expected ${format_value(expected)}, got ${format_value(actual)}`);
    } else {
      assert_equals(actual, expected, description);
    }
    return;
  }
  assert_equals(typeof actual, "object", `${description}: typeof`);
  assert_not_equals(actual, null, `${description}: null`);
  assert_equals(Object.getPrototypeOf(actual), Object.getPrototypeOf(expected), `${description}: prototype`);
  if (ArrayBuffer.isView(expected)) {
    assert_equals(actual.length, expected.length, `${description}: length`);
    for (let i = 0; i < expected.length; i++) {
      assert_true(Object.is(actual[i], expected[i]),
                  `${description}[${i}]: expected ${format_value(expected[i])}, got ${format_value(actual[i])}`);
    }
    return;
  }
  if (expected instanceof Map) {
    assert_equals(actual.size, expected.size, `${description}: size`);
    const actualEntries = [...actual];
    const expectedEntries = [...expected];
    for (let i = 0; i < expectedEntries.length; i++) {
      assert_value_equals(actualEntries[i][0], expectedEntries[i][0], `${description} key ${i}`);
      assert_value_equals(actualEntries[i][1], expectedEntries[i][1], `${description} value for key ${i}`);
    }
    return;
  }
  if (Array.isArray(expected)) {
    assert_equals(actual.length, expected.length, `${description}: length`);
    for (let i = 0; i < expected.length; i++) {
      assert_true(Object.hasOwn(actual, i), `${description}[${i}]: present`);
      assert_value_equals(actual[i], expected[i], `${description}[${i}]`);
    }
    return;
  }
  assert_array_equals(Reflect.ownKeys(actual), Reflect.ownKeys(expected), `${description}: keys`);
  for (const key of Reflect.ownKeys(expected)) {
    const desc = Object.getOwnPropertyDescriptor(actual, key);
    assert_true(desc.enumerable, `${description}.${String(key)}: enumerable`);
    assert_true(desc.writable, `${description}.${String(key)}: writable`);
    assert_true(desc.configurable, `${description}.${String(key)}: configurable`);
    assert_value_equals(actual[key], expected[key], `${description}.${String(key)}`);
  }
}

// `fn` must throw a ComponentError whose `data` deep-equals `expectedData`.
function assert_component_error(fn, expectedData, description = "call") {
  let thrown;
  try {
    fn();
  } catch (e) {
    thrown = e;
  }
  assert_not_equals(thrown, undefined, `${description}: should throw`);
  assert_equals(Object.getPrototypeOf(thrown), WebAssembly.ComponentError.prototype,
                `${description}: prototype`);
  assert_true(thrown instanceof Error, `${description}: instanceof Error`);
  assert_equals(typeof thrown.message, "string", `${description}: message`);
  assert_value_equals(thrown.data, expectedData, `${description}: data`);
  return thrown;
}
