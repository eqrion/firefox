// |jit-test| skip-if: !wasmComponentsEnabled()

// End-to-end greeter: a component exports "greet" (canon lift), which
// forwards to an imported "log" func (canon lower), sharing memory/realloc
// between the two.

const component = new WebAssembly.Component(wasmTextToBinary(`
  (component
    (type $logType (func (param "s" string)))
    (import "log" (func $log (type $logType)))

    (core module $libc
      (memory (export "mem") 1)
      (global $nextFree (mut i32) (i32.const 8))
      (func (export "realloc")
        (param $oldPtr i32) (param $oldSize i32) (param $align i32) (param $newSize i32)
        (result i32)
        (local $ret i32)
        global.get $nextFree
        local.set $ret
        global.get $nextFree
        local.get $newSize
        i32.add
        global.set $nextFree
        local.get $ret
      )
    )
    (core instance (instantiate $libc))
    (alias core export 0 "mem" (core memory $mem))
    (alias core export 0 "realloc" (core func $realloc))

    (core func $lowered (canon lower (func $log) (memory $mem) (realloc $realloc)))
    (core instance $env (export "log" (func $lowered)))

    (core module $main
      (import "env" "log" (func $log_import (param i32 i32)))
      (func (export "greet") (param i32 i32)
        local.get 0
        local.get 1
        call $log_import
      )
    )
    (core instance (instantiate $main (with "env" (instance $env))))
    (alias core export 2 "greet" (core func $greetCore))

    (type $greetType (func (param "s" string)))
    (func $greet (type $greetType) (canon lift (core func $greetCore) (memory $mem) (realloc $realloc)))
    (export "greet" (func $greet))
  )
`));

let received;
const instance = new WebAssembly.ComponentInstance(component, {
  log(s) { received = s; },
});

assertEq(typeof instance.exports.greet, "function");
instance.exports.greet("hello, world");
assertEq(received, "hello, world");
