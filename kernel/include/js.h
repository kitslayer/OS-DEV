/* js.h — the from-scratch JavaScript interpreter entry point. */
#pragma once

/* Run JS source `src`; append print()/console.log() output into out[0..outmax).
 * Returns the output length, or -1 on a parse/runtime error (the message is
 * appended to out). Uses a shared static arena, so calls are serialized. */
int js_run(const char *src, char *out, int outmax);

/* Like js_run, but document.write(s) calls write_cb(s) instead of appending to
 * out — used by the browser to splice script-generated HTML into the page. */
int js_run_doc(const char *src, char *out, int outmax, void (*write_cb)(const char *));

/* Register a localStorage backing store (key->value strings) used by page JS;
 * cleared automatically by js_run (the shell path). */
void js_set_storage(const char *(*get)(const char *), void (*set)(const char *, const char *));

/* Register DOM callbacks for document.getElementById(id) handles:
 *  get(id, out, max, html) -> 1 if found, fills out with the element's
 *    textContent (html=0) or innerHTML (html=1);
 *  set(id, value, html) -> replace the element's text/HTML and re-render.
 * Cleared automatically by js_run (the shell path). */
void js_set_dom(int (*get)(const char *, char *, int, int), void (*set)(const char *, const char *, int));
void js_set_dom_attr(int (*getattr)(const char *, const char *, char *, int),
                     void (*setattr)(const char *, const char *, const char *));
/* Register position-keyed DOM callbacks for querySelector(All) matches (id-less):
 *  get_at(off, out, max, html) -> read the element at byte offset `off`;
 *  query(sel, offs, max) -> fill offs[] with match offsets, return the count. */
void js_set_dom_pos(int (*get_at)(int, char *, int, int), int (*query)(const char *, int *, int));
void js_set_location(const char *url);
