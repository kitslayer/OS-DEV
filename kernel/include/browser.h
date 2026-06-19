/* browser.h — a minimal graphical web browser window.
 *
 * Fetches a URL over HTTP (http_get), strips the HTML to a styled flow of
 * words (headings, links, paragraphs), and renders it word-wrapped in a
 * scrollable window with an address bar. Not a layout engine — but it shows
 * real, live web pages. The window manager owns the window; this owns content.
 */
#pragma once

typedef struct browser browser_t;

/* Create the shared async-fetch worker task (idempotent). */
void       browser_init(void);

/* Create a browser, seeded with `url` (NULL/"" -> example.com), and fetch it. */
browser_t *browser_create(const char *url);
void       browser_destroy(browser_t *b);

/* WM calls this each frame: finishes a delivered load. Returns 1 if repainting
 * is needed. */
int        browser_poll(browser_t *b);

/* The current page's <title> (or "Browser"), for the window title bar. */
const char *browser_title(browser_t *b);

/* Navigate back to the previous page in history (no-op if none). */
void        browser_back(browser_t *b);

/* Navigate to a given URL (pushes the current page onto the back stack). */
void        browser_go(browser_t *b, const char *url);

/* Draw the browser into the window body at (x,y) with size (w,h). */
void browser_render(browser_t *b, int x, int y, int w, int h);

/* Deliver a keystroke (address-bar editing, or scroll keys when browsing). */
void browser_key(browser_t *b, int c);
/* Middle-click paste: clipboard text into the focused field or the address bar. */
void browser_paste(browser_t *b, const char *s, int n);
/* Right-click: copy the URL of the link under (rx,ry) into out; returns its length or 0. */
int  browser_rclick(browser_t *b, int rx, int ry, char *out, int max);
/* Mouse text selection (WM-driven): begin/extend a word-range, commit copies it. */
void browser_sel_begin(browser_t *b, int rx, int ry);
void browser_sel_extend(browser_t *b, int rx, int ry);
void browser_sel_clear(browser_t *b);
int  browser_sel_commit(browser_t *b, char *out, int max);  /* -> out (len), 0 if not a drag */
int  browser_sel_word(browser_t *b, int rx, int ry, char *out, int max);  /* double-click: 1 word -> out */
/* Draggable scrollbar (coords relative to the browser origin). */
int  browser_in_scrollbar(browser_t *b, int rx, int ry, int w, int h);
void browser_scroll_track(browser_t *b, int ry, int h);

/* A click inside the window body, relative to its top-left. Returns 1 if the
 * browser consumed it (so the WM knows to repaint). */
int browser_click(browser_t *b, int rx, int ry, int w, int h);
