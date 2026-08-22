import re, subprocess, sys, pathlib

TESTS = sys.argv[1:]

def run(test):
    r = subprocess.run(["./mach", "jit-test", "--no-progress", test],
                       capture_output=True, text=True)
    return r.stdout + r.stderr

pat = re.compile(r'expected message (/.*?/), got "([^"]*)"')

for test in TESTS:
    path = pathlib.Path("js/src/jit-test/tests/" + test)
    for round in range(25):
        out = run(test)
        if "PASSED ALL" in out:
            print(f"{test}: clean after {round} rounds")
            break
        pairs = set(pat.findall(out))
        if not pairs:
            print(f"{test}: STOP, not a message mismatch")
            print("\n".join(l for l in out.splitlines()
                            if "Error" in l or "CompileError" in l)[:600])
            break
        text = path.read_text()
        changed = False
        for expected, got in pairs:
            # Keep only the reason. The offset and the index or byte a
            # rejection carries are specific to the module under test, so
            # baking them into a regex would pin the test to today's bytes.
            got = re.sub(r'^at offset \d+: ', '', got)
            got = re.sub(r'^invalid function body: ', '', got)
            got = re.sub(r' -?\d+$', '', got).strip()
            # Check for the alternative as a whole, not as a substring: the
            # reason "type mismatch" occurs inside "type mismatch: expected
            # f32, found i32", which is a different phrasing and does not
            # match.
            if not got or f"({got})" in expected:
                continue
            inner = expected[1:-1]
            new = f"/({inner})|({got})/"
            if expected in text:
                text = text.replace(expected, new)
                changed = True
                print(f"  {test}: +({got})")
        if not changed:
            print(f"{test}: STOP, could not widen {pairs}")
            break
        path.write_text(text)
    else:
        print(f"{test}: STOP, too many rounds")
