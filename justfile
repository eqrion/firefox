# Tasks for the itasca prototype branch.

js := "obj-shell/dist/bin/js"
corpus := env("HOME") + "/src/itasca/tests/spec"

# Build the JS shell.
build:
    ./mach build

# Run the wasm jit-tests.
jit-test *ARGS:
    ./mach jit-test wasm/ {{ARGS}}

# Check WebAssembly.validate against itasca's own extracted spec corpus.
#
# The two directories are the labels: everything under valid/ has to be
# accepted and everything under invalid/ has to be rejected. This is the
# cheapest signal that the validator is wired up correctly, and it does not go
# through the text-to-binary compiler the way the jit-tests do.
difftest:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p artifacts
    {
      echo 'let fail = 0, n = 0;'
      echo 'function check(path, expect) {'
      echo '  n++;'
      echo '  let got;'
      echo '  try { got = WebAssembly.validate(read(path, "binary")); }'
      echo '  catch (e) { print("THREW  " + path + ": " + e); fail++; return; }'
      echo '  if (got !== expect) {'
      echo '    print((expect ? "REJECTED " : "ACCEPTED ") + path);'
      echo '    fail++;'
      echo '  }'
      echo '}'
      for f in {{corpus}}/valid/*.wasm; do echo "check(\"$f\", true);"; done
      for f in {{corpus}}/invalid/*.wasm; do echo "check(\"$f\", false);"; done
      echo 'print(`${n - fail}/${n} agreed`);'
      echo 'if (fail) { quit(1); }'
    } > artifacts/difftest.js
    {{js}} artifacts/difftest.js

# Widen jit-test error-message regexes to accept itasca's phrasing.
#
# Replacing the validator changes what a rejection is called but not which
# modules are rejected, so a test that pins the message needs the new wording
# added as an alternative. This runs each test, reads the expected/got pair out
# of the failure, and appends the "got" phrasing to the regex, repeating until
# the test passes. Review the diff: it will happily paper over a test whose
# module now fails for a genuinely different reason.
widen-messages *TESTS:
    python3 tools/itasca/widen-error-messages.py {{TESTS}}
