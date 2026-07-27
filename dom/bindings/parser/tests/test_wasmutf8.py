import WebIDL


def WebIDLTest(parser, harness):
    # [WasmUtf8] on a DOMString attribute is fine.
    parser.parse(
        """
        interface Foo {
          [WasmUtf8] attribute DOMString bar;
        };
    """
    )
    parser.finish()
    harness.ok(True, "Should not have thrown for [WasmUtf8] on a DOMString attribute")

    # [WasmUtf8] on a nullable DOMString attribute is fine.
    parser = parser.reset()
    parser.parse(
        """
        interface Foo {
          [WasmUtf8] attribute DOMString? bar;
        };
    """
    )
    parser.finish()
    harness.ok(
        True,
        "Should not have thrown for [WasmUtf8] on a nullable DOMString attribute",
    )

    # [WasmUtf8] on a USVString attribute is fine.
    parser = parser.reset()
    parser.parse(
        """
        interface Foo {
          [WasmUtf8] attribute USVString bar;
        };
    """
    )
    parser.finish()
    harness.ok(True, "Should not have thrown for [WasmUtf8] on a USVString attribute")

    # [WasmUtf8] on a non-string attribute should throw.
    parser = parser.reset()
    threw = False
    try:
        parser.parse(
            """
            interface Foo {
              [WasmUtf8] attribute boolean bar;
            };
        """
        )
        parser.finish()
    except WebIDL.WebIDLError:
        threw = True
    harness.ok(threw, "Should have thrown for [WasmUtf8] on a non-string attribute")

    # [WasmUtf8] with an argument should throw.
    parser = parser.reset()
    threw = False
    try:
        parser.parse(
            """
            interface Foo {
              [WasmUtf8(1)] attribute DOMString bar;
            };
        """
        )
        parser.finish()
    except WebIDL.WebIDLError:
        threw = True
    harness.ok(threw, "Should have thrown for [WasmUtf8] with an argument")

    # [WasmUtf8] on an operation with a DOMString argument is fine.
    parser = parser.reset()
    parser.parse(
        """
        interface Foo {
          [WasmUtf8] undefined setAttribute(DOMString name, DOMString value);
        };
    """
    )
    parser.finish()
    harness.ok(
        True,
        "Should not have thrown for [WasmUtf8] on an operation with a "
        "DOMString argument",
    )

    # [WasmUtf8] on an operation with a DOMString return value is fine.
    parser = parser.reset()
    parser.parse(
        """
        interface Foo {
          [WasmUtf8] DOMString getName();
        };
    """
    )
    parser.finish()
    harness.ok(
        True,
        "Should not have thrown for [WasmUtf8] on an operation with a "
        "DOMString return value",
    )

    # [WasmUtf8] on an operation with no plain string anywhere should throw.
    parser = parser.reset()
    threw = False
    try:
        parser.parse(
            """
            interface Foo {
              [WasmUtf8] undefined compute(long x, long y);
            };
        """
        )
        parser.finish()
    except WebIDL.WebIDLError:
        threw = True
    harness.ok(
        threw,
        "Should have thrown for [WasmUtf8] on an operation with no plain string",
    )
