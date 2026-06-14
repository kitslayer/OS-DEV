# Milestone 148 — JSON.parse

**Goal:** complete the JSON round-trip. Milestone 146 added `JSON.stringify`; this
adds `JSON.parse`, so scripts can both produce and consume JSON — the lingua
franca of web data.

## What

`JSON.parse(str)` — a recursive-descent parser in `kernel/js.c` producing JS
values: objects, arrays, strings (with `\n \t \r \" \\ \/` escapes; `\uXXXX` is
accepted and emitted as `?`), `true`/`false`/`null`, and numbers (integer — the
fractional part is parsed and dropped, since Number is integer here).

## Safety

- **Bounds**: every read is guarded against `jp_end`; string buffers are sized
  from the raw token length (escapes only shrink), not the remaining input, so a
  document with many strings stays linear in arena use.
- **Depth-guarded**: nesting recurses through `json_parse_val`, bounded by the
  shared `MAXDEPTH` — deeply nested `[[[[…` input reports `invalid JSON` rather
  than overflowing the stack.
- **Malformed input** sets an error and returns cleanly (no crash). (There is no
  `try/catch` in the engine, so a parse error aborts the script — acceptable.)

## Verified

Host-tested under ASan+UBSan:

```
JSON.parse('{"name":"OS-DEV","ver":147,"tags":["js","tls"],"ok":true}')  // object
JSON.parse('[1,2,[3,4],{"k":5}]')[2][1]            // 4   (nested)
JSON.stringify(JSON.parse('{"a":1,"b":[2,3]}'))    // {"a":1,"b":[2,3]}  (round-trip)
JSON.parse('"he\\"llo"')                            // he"llo  (escapes)
```
Malformed (`{bad`, `[1,2,`) and 500-deep nesting all fail gracefully.

## Files
- `kernel/js.c` — `json_parse_val`, `jp_string`, `nat_json_parse`; registered as
  `JSON.parse`.
