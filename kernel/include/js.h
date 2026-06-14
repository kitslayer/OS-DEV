/* js.h — the from-scratch JavaScript interpreter entry point. */
#pragma once

/* Run JS source `src`; append print()/console.log() output into out[0..outmax).
 * Returns the output length, or -1 on a parse/runtime error (the message is
 * appended to out). Uses a shared static arena, so calls are serialized. */
int js_run(const char *src, char *out, int outmax);

/* Like js_run, but document.write(s) calls write_cb(s) instead of appending to
 * out — used by the browser to splice script-generated HTML into the page. */
int js_run_doc(const char *src, char *out, int outmax, void (*write_cb)(const char *));
