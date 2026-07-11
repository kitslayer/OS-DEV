/* js.h — the from-scratch JavaScript interpreter entry point. */
#pragma once

/* Run JS source `src`; append print()/console.log() output into out[0..outmax).
 * Returns the output length, or -1 on a parse/runtime error (the message is
 * appended to out). Uses a shared static arena, so calls are serialized. */
int js_run(const char *src, char *out, int outmax);

/* Like js_run, but document.write(s) calls write_cb(s) instead of appending to
 * out — used by the browser to splice script-generated HTML into the page. */
int js_run_doc(const char *src, char *out, int outmax, void (*write_cb)(const char *));

/* Page-session runs: js_page_load runs a page's load <script> and PERSISTS its
 * global env; js_page_event runs an event handler (onclick/onchange) in that same
 * persistent env, so it can call functions / read vars the load script defined.
 * The env is rebuilt on the next js_page_load (arena reset then). */
int js_page_load(const char *src, char *out, int outmax, void (*write_cb)(const char *));
int js_page_event(const char *src, char *out, int outmax, void (*write_cb)(const char *));
void js_page_reset(void);   /* drop the persistent page env (call on navigation) */
/* Fire the JS handler registered (el.onclick=fn / addEventListener) for element
 * `id` and event `type` (e.g. "click"), in the persistent page env. 1 if one ran. */
int js_fire_event(const char *id, const char *type, char *out, int outmax, void (*write_cb)(const char *));

/* Register a localStorage backing store (key->value strings) used by page JS;
 * cleared automatically by js_run (the shell path). remove_fn/clear_fn back
 * localStorage.removeItem/clear (M1619); either may be NULL if a host has no
 * use for them (getItem/setItem alone still work). */
void js_set_storage(const char *(*get)(const char *), void (*set)(const char *, const char *),
                     void (*remove_fn)(const char *), void (*clear_fn)(void));

/* Register document.title get/set (reads/writes the page <title> + window bar). */
void js_set_title(int (*get)(char *, int), void (*set)(const char *));

/* Register a blocking HTTP backing for the JS fetch() global (M684; method/body M703): the
 * callback fills `out` (up to outmax) with the response body, sets *status to the HTTP
 * status, and returns the body length, or <0 on a network error. method/ctype/body are
 * NULL for a GET; for a POST they carry the request method, Content-Type, and body string.
 * Without it, fetch() rejects. */
void js_set_fetch(int (*fn)(const char *url, const char *method, const char *ctype, const char *body, char *out, int outmax, int *status));

/* Register a blocking backing for the JS EventSource global (M-eventsource): the
 * callback does a GET of `url`, reads the FIRST complete SSE event, fills `out` (up
 * to outmax) with that event's joined `data:` payload, sets *status to the HTTP
 * status, and returns the payload length, or <0 on a network error. It must CLOSE
 * the connection after the first event (SSE streams stay open). Without it, a
 * constructed EventSource fires onerror. */
void js_set_eventsource(int (*fn)(const char *url, char *out, int outmax, int *status));

/* M1796/M1797: register the canvas 2D drawing op callback — the browser resolves the
 * canvas id to its writable image slot and draws. op: 0=fillRect 1=strokeRect
 * 2=clearRect 3=line; (a,b,c,d) are the op's coords; color is fill/strokeStyle. */
void js_set_canvas(void (*op)(const char *id, int op, int a, int b, int c, int d, const char *color, const char *text, int lw));

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
void js_set_dom_pos(int (*get_at)(int, char *, int, int),
                    void (*set_at)(int, const char *, int),
                    int (*getattr_at)(int, const char *, char *, int),
                    void (*setattr_at)(int, const char *, const char *),
                    int (*query)(const char *, int *, int));
/* element.matches()/closest() backings (id + position variants; closest returns an offset or -1). */
void js_set_dom_match(int (*matches)(const char *, const char *), int (*matches_at)(int, const char *),
                      int (*closest)(const char *, const char *), int (*closest_at)(int, const char *));
/* element.removeAttribute(name) backings (id + position variants). */
void js_set_dom_rmattr(void (*rmattr)(const char *, const char *), void (*rmattr_at)(int, const char *));
/* element.children + parentElement + sibling backings (id + position variants; parent/sibling return an offset or -1; sibling dir<0 = previous). */
void js_set_dom_children(int (*children)(const char *, int *, int), int (*children_at)(int, int *, int),
                         int (*parent)(const char *), int (*parent_at)(int),
                         int (*sibling)(const char *, int), int (*sibling_at)(int, int));
/* element.tagName backings (id + position variants; fill the uppercased tag, return >0 if found). */
void js_set_dom_tag(int (*tag)(const char *, char *, int), int (*tag_at)(int, char *, int));
void js_set_location(const char *url);
