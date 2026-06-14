# Tests

## JavaScript engine regression suite

`tests/js/suite.js` exercises the from-scratch JS interpreter (`kernel/js.c`) end
to end — core operators, closures/recursion, arrow functions, default params,
arrays + higher-order methods, strings, objects, `JSON.parse`/`stringify`,
template literals, `switch`/`for-of`/`do-while`, and `try/catch/finally/throw`.
`tests/js/suite.expected` is the golden output.

Run it on the host (builds `kernel/js.c` with `-DJS_HOSTTEST` under ASan+UBSan and
diffs against the golden file):

```sh
make jstest        # or: tests/run-js-tests.sh
```

You can also run the same suite inside the OS: `js suite.js` (if copied onto the
disk), or try the baked-in demos `js`, `js showcase.js`, `js sample.js`.
