// Table of roundtrip cases shared by values/roundtrip.any.js. Each entry names
// a passthrough fixture, the export to call, and [input, expected] pairs
// (expected defaults to input). Every import of the fixture is an identity
// echo. Expected values use the record/variant/flags builders from
// assertions.js.

const valueCases = [
  {
    fixture: "values/u32", exportName: "roundtrip",
    cases: [[0], [1], [0xffffffff], [1.9, 1], ["3", 3]],
  },
  {
    fixture: "values/string", exportName: "roundtrip",
    cases: [[""], ["hello"], ["😀"], ["\uD800", "�"]],
  },
  {
    fixture: "values/enum", exportName: "roundtrip",
    cases: [["red"], ["dark-blue"], ["HTML"], ["a1-b2"]],
  },
  {
    fixture: "values/flags", exportName: "roundtripEight",
    cases: [
      [flags({ read: false, write: false, exec: false, isDir: false, isFile: false, hidden: false, readOnly: false, html: false })],
      [flags({ read: true, write: false, exec: true, isDir: false, isFile: true, hidden: false, readOnly: true, html: true })],
    ],
  },
  {
    fixture: "values/option", exportName: "roundtripU32",
    cases: [[undefined], [0], [7], [null, undefined]],
  },
  {
    fixture: "values/option", exportName: "roundtripString",
    cases: [[undefined], [""], ["x"]],
  },
  {
    fixture: "values/option", exportName: "roundtripNested",
    cases: [[undefined], [variant("none")], [variant("some", 3)]],
  },
  {
    fixture: "values/record", exportName: "roundtrip",
    cases: [
      [record({ firstName: "Ada", age: 36, nickName: "ada", url: "u", maybe: variant("some", 1) })],
      [record({ firstName: "", age: 0, nickName: undefined, url: "", maybe: undefined })],
    ],
  },
  {
    fixture: "values/variant", exportName: "roundtrip",
    cases: [[variant("none-case")], [variant("num", 1)], [variant("text", "t")], [variant("pair", [1, 2])], [variant("undefined")]],
  },
  {
    fixture: "values/result-value", exportName: "roundtripList",
    cases: [[[]], [[variant("ok", 1), variant("error", "e")]]],
  },
  {
    fixture: "values/map", exportName: "roundtripStringU32",
    cases: [[new Map()], [new Map([["a", 1], ["b", 2]])], [[["a", 1], ["a", 2]], new Map([["a", 2]])]],
  },
  {
    fixture: "values/map", exportName: "roundtripS64String",
    cases: [[new Map([[-1n, "x"], [2n, "y"]])]],
  },
  {
    fixture: "values/nesting", exportName: "roundtripTuple",
    cases: [[["s", new Uint8Array([1, 2]), "c"]], [["", new Uint8Array(0), undefined]]],
  },
  {
    fixture: "values/nesting", exportName: "roundtripShape",
    cases: [[variant("point", record({ x: 1, y: 2 }))], [variant("name", "n")]],
  },
];
