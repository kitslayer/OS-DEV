/* mdconv.h — Markdown->HTML and CSV->HTML converters for the browser.
 *
 * These render local .md / .csv files into HTML that the normal HTML renderer
 * (parse_html) then lays out. They parse UNTRUSTED input (any file on disk),
 * so every write is capped against `cap` and every read is bounded by the input
 * length; the inline emphasis parser uses flat toggles, not recursion (no kernel
 * guard page). Extracted verbatim from browser.c (M1755) into this header so the
 * converters can be unit/fuzz-tested on the host without dragging in the whole
 * browser — browser.c and tests/md/md_test.c both #include this single copy, so
 * there is no drift between what ships and what is fuzzed.
 *
 * Self-contained: depends on nothing but the C basics (char/int), so it drops
 * straight into the freestanding kernel and a hosted test alike. */
#ifndef MDCONV_H
#define MDCONV_H

/* ---- Markdown -> HTML (a useful subset), so the browser can render .md files.
 * Output goes to the normal HTML renderer (parse_html), which already tolerates
 * malformed markup. Bounded + NON-RECURSIVE (untrusted input, no kernel guard
 * page): every write is capped against `cap`, every read bounded by the line
 * length, and inline emphasis uses flat toggles instead of recursion. Handles
 * headings, bold, italic, inline code, fenced code blocks, bullet and numbered
 * lists, blockquotes, links, horizontal rules, and paragraphs. */
static void md_put(char *o, int *p, int cap, const char *s) { while (*s && *p < cap) o[(*p)++] = *s++; }
static void md_putc(char *o, int *p, int cap, char c) { if (*p < cap) o[(*p)++] = c; }
static void md_esc(char *o, int *p, int cap, char c) {            /* escape HTML metachars */
    if (c == '<') md_put(o, p, cap, "&lt;");
    else if (c == '>') md_put(o, p, cap, "&gt;");
    else if (c == '&') md_put(o, p, cap, "&amp;");
    else if (c == '"') md_put(o, p, cap, "&quot;");   /* M1755: the emphasis/link/image escapers write straight into href="..",
                                                         src="..", alt=".." attributes, so an unescaped " broke out of the attr.
                                                         &quot; decodes back to " in text content, so escaping it everywhere is safe. */
    else md_putc(o, p, cap, c);
}
static int md_wordch(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'); }   /* a "word" char, for '_' intra-word emphasis suppression (M1783) */
static void md_inline(char *o, int *p, int cap, const char *s, int len) {  /* spans within one line */
    int bold = 0, ital = 0, strike = 0;
    for (int i = 0; i < len; ) {
        char c = s[i];
        if (c == '\\' && i + 1 < len) { md_esc(o, p, cap, s[i + 1]); i += 2; continue; }   /* backslash escape */
        if (c == '`') {                                          /* `code` */
            int j = i + 1; while (j < len && s[j] != '`') j++;
            md_put(o, p, cap, "<code>");
            for (int k = i + 1; k < j; k++) md_esc(o, p, cap, s[k]);
            md_put(o, p, cap, "</code>");
            i = j < len ? j + 1 : j; continue;
        }
        if (c == '!' && i + 1 < len && s[i + 1] == '[') {        /* ![alt](url) image */
            int t = i + 2; while (t < len && s[t] != ']') t++;
            if (t + 1 < len && s[t + 1] == '(') {
                int u = t + 2; while (u < len && s[u] != ')') u++;
                if (u < len) {
                    md_put(o, p, cap, "<img src=\"");
                    for (int k = t + 2; k < u; k++) md_esc(o, p, cap, s[k]);
                    md_put(o, p, cap, "\" alt=\"");
                    for (int k = i + 2; k < t; k++) md_esc(o, p, cap, s[k]);
                    md_put(o, p, cap, "\">");
                    i = u + 1; continue;
                }
            }
            md_esc(o, p, cap, c); i++; continue;
        }
        if (c == '[') {                                          /* [text](url) */
            int t = i + 1; while (t < len && s[t] != ']') t++;
            if (t + 1 < len && s[t + 1] == '(') {
                int u = t + 2; while (u < len && s[u] != ')') u++;
                if (u < len) {
                    md_put(o, p, cap, "<a href=\"");
                    for (int k = t + 2; k < u; k++) md_esc(o, p, cap, s[k]);
                    md_put(o, p, cap, "\">");
                    for (int k = i + 1; k < t; k++) md_esc(o, p, cap, s[k]);
                    md_put(o, p, cap, "</a>");
                    i = u + 1; continue;
                }
            }
            md_esc(o, p, cap, c); i++; continue;
        }
        if (c == '*' && i + 1 < len && s[i + 1] == '*') { md_put(o, p, cap, bold ? "</b>" : "<b>"); bold = !bold; i += 2; continue; }
        if (c == '~' && i + 1 < len && s[i + 1] == '~') { md_put(o, p, cap, strike ? "</s>" : "<s>"); strike = !strike; i += 2; continue; }
        if (c == '*') { md_put(o, p, cap, ital ? "</i>" : "<i>"); ital = !ital; i++; continue; }
        if (c == '_') {                                          /* M1783: '_' emphasis only at a word boundary (GFM) — intra-word my_var / snake_case stays literal */
            if (i > 0 && md_wordch(s[i-1]) && i+1 < len && md_wordch(s[i+1])) { md_esc(o, p, cap, c); i++; continue; }
            md_put(o, p, cap, ital ? "</i>" : "<i>"); ital = !ital; i++; continue;
        }
        if (c == 'h') {                                          /* autolink: a bare http(s):// URL */
            int sch = 0;
            if (i + 7 <= len && s[i+1]=='t'&&s[i+2]=='t'&&s[i+3]=='p'&&s[i+4]==':'&&s[i+5]=='/'&&s[i+6]=='/') sch = 7;
            else if (i + 8 <= len && s[i+1]=='t'&&s[i+2]=='t'&&s[i+3]=='p'&&s[i+4]=='s'&&s[i+5]==':'&&s[i+6]=='/'&&s[i+7]=='/') sch = 8;
            if (sch) {
                int u = i + sch;
                while (u < len && s[u]!=' '&&s[u]!='\t'&&s[u]!=')'&&s[u]!='<'&&s[u]!='>'&&s[u]!='"') u++;
                md_put(o, p, cap, "<a href=\"");
                for (int k = i; k < u; k++) md_esc(o, p, cap, s[k]);
                md_put(o, p, cap, "\">");
                for (int k = i; k < u; k++) md_esc(o, p, cap, s[k]);
                md_put(o, p, cap, "</a>");
                i = u; continue;
            }
        }
        md_esc(o, p, cap, c); i++;
    }
    if (bold)   md_put(o, p, cap, "</b>");                       /* close any span left open at line end */
    if (ital)   md_put(o, p, cap, "</i>");
    if (strike) md_put(o, p, cap, "</s>");
}
/* Emit one GFM table row; cells are the runs of text between '|' delimiters. */
static void md_table_row(char *o, int *p, int cap, const char *s, int len, int hdr) {
    md_put(o, p, cap, "<tr>");
    int i = 0;
    while (i < len) {
        if (s[i] == '|') { i++; continue; }                  /* '|' is just a delimiter */
        int cs = i; while (i < len && s[i] != '|') i++;
        int ce = i;
        while (cs < ce && s[cs] == ' ') cs++;                /* trim surrounding spaces */
        while (ce > cs && (s[ce - 1] == ' ' || s[ce - 1] == '\r')) ce--;
        md_put(o, p, cap, hdr ? "<th>" : "<td>");
        md_inline(o, p, cap, s + cs, ce - cs);
        md_put(o, p, cap, hdr ? "</th>" : "</td>");
    }
    md_put(o, p, cap, "</tr>");
}
static int md_to_html(const char *md, int mdlen, char *out, int cap) {
    int p = 0, in_pre = 0, list = 0 /* 0 none, 1 ul, 2 ol */, para = 0, i = 0;
    while (i < mdlen) {
        int ls = i; while (i < mdlen && md[i] != '\n') i++;      /* line is [ls, le) */
        int le = i; if (i < mdlen) i++;                          /* consume '\n' */
        if (le > ls && md[le - 1] == '\r') le--;                 /* strip CR */
        const char *L = md + ls; int n = le - ls;
        if (n >= 3 && L[0] == '`' && L[1] == '`' && L[2] == '`') {   /* ``` fence toggles <pre> */
            if (!in_pre) { if (para) { md_put(out, &p, cap, "</p>"); para = 0; }
                           if (list) { md_put(out, &p, cap, list == 1 ? "</ul>" : "</ol>"); list = 0; }
                           md_put(out, &p, cap, "<pre>"); in_pre = 1; }
            else { md_put(out, &p, cap, "</pre>"); in_pre = 0; }
            continue;
        }
        if (in_pre) { for (int k = 0; k < n; k++) md_esc(out, &p, cap, L[k]); md_putc(out, &p, cap, '\n'); continue; }
        int blank = 1; for (int k = 0; k < n; k++) if (L[k] != ' ' && L[k] != '\t') { blank = 0; break; }
        if (blank) { if (para) { md_put(out, &p, cap, "</p>"); para = 0; }
                     if (list) { md_put(out, &p, cap, list == 1 ? "</ul>" : "</ol>"); list = 0; } continue; }
        int b0 = 0; while (b0 < n && L[b0] == ' ') b0++;
        const char *T = L + b0; int tn = n - b0;
        if (tn >= 3) {                                           /* --- *** ___ horizontal rule */
            char hc = T[0];
            if (hc == '-' || hc == '*' || hc == '_') {
                int all = 1, cnt = 0;
                for (int k = 0; k < tn; k++) { if (T[k] == hc) cnt++; else if (T[k] != ' ') { all = 0; break; } }
                if (all && cnt >= 3) { if (para) { md_put(out, &p, cap, "</p>"); para = 0; }
                                       if (list) { md_put(out, &p, cap, list == 1 ? "</ul>" : "</ol>"); list = 0; }
                                       md_put(out, &p, cap, "<hr>"); continue; }
            }
        }
        if (T[0] == '#') {                                       /* # .. ###### heading */
            int h = 0; while (h < tn && h < 6 && T[h] == '#') h++;
            if (h < tn && T[h] == ' ') {
                if (para) { md_put(out, &p, cap, "</p>"); para = 0; }
                if (list) { md_put(out, &p, cap, list == 1 ? "</ul>" : "</ol>"); list = 0; }
                char tag[3] = { 'h', (char)('0' + h), 0 };
                md_putc(out, &p, cap, '<'); md_put(out, &p, cap, tag); md_putc(out, &p, cap, '>');
                md_inline(out, &p, cap, T + h + 1, tn - h - 1);
                md_put(out, &p, cap, "</"); md_put(out, &p, cap, tag); md_putc(out, &p, cap, '>');
                continue;
            }
        }
        if (T[0] == '>') {                                       /* > blockquote */
            if (para) { md_put(out, &p, cap, "</p>"); para = 0; }
            if (list) { md_put(out, &p, cap, list == 1 ? "</ul>" : "</ol>"); list = 0; }
            int s2 = 1; if (s2 < tn && T[s2] == ' ') s2++;
            md_put(out, &p, cap, "<blockquote>"); md_inline(out, &p, cap, T + s2, tn - s2); md_put(out, &p, cap, "</blockquote>");
            continue;
        }
        if (tn >= 2 && (T[0] == '-' || T[0] == '*' || T[0] == '+') && T[1] == ' ') {   /* - * + bullet */
            if (para) { md_put(out, &p, cap, "</p>"); para = 0; }
            if (list != 1) { if (list == 2) md_put(out, &p, cap, "</ol>"); md_put(out, &p, cap, "<ul>"); list = 1; }
            md_put(out, &p, cap, "<li>"); md_inline(out, &p, cap, T + 2, tn - 2); md_put(out, &p, cap, "</li>");
            continue;
        }
        { int d = 0; while (d < tn && T[d] >= '0' && T[d] <= '9') d++;                  /* N. ordered item */
          if (d > 0 && d + 1 < tn && T[d] == '.' && T[d + 1] == ' ') {
              if (para) { md_put(out, &p, cap, "</p>"); para = 0; }
              if (list != 2) { if (list == 1) md_put(out, &p, cap, "</ul>"); md_put(out, &p, cap, "<ol>"); list = 2; }
              md_put(out, &p, cap, "<li>"); md_inline(out, &p, cap, T + d + 2, tn - d - 2); md_put(out, &p, cap, "</li>");
              continue;
          } }
        { int haspipe = 0; for (int k = 0; k < tn; k++) if (T[k] == '|') { haspipe = 1; break; }   /* GFM table */
          if (haspipe) {
              int ns = i, ne = i; while (ne < mdlen && md[ne] != '\n') ne++;   /* peek the next line */
              int sep = (ne > ns), dash = 0;                 /* is it a |---|:-: separator row? */
              for (int k = ns; k < ne; k++) { char ch = md[k];
                  if (ch == '-') dash = 1;
                  else if (ch != ':' && ch != '|' && ch != ' ' && ch != '\t' && ch != '\r') { sep = 0; break; } }
              if (sep && dash) {
                  if (para) { md_put(out, &p, cap, "</p>"); para = 0; }
                  if (list) { md_put(out, &p, cap, list == 1 ? "</ul>" : "</ol>"); list = 0; }
                  md_put(out, &p, cap, "<table>");
                  md_table_row(out, &p, cap, T, tn, 1);      /* the header row */
                  i = ne; if (i < mdlen) i++;                /* consume the separator line */
                  while (i < mdlen) {                        /* body: consecutive lines containing '|' */
                      int rs = i, re = i; while (re < mdlen && md[re] != '\n') re++;
                      int rl = re; if (rl > rs && md[rl - 1] == '\r') rl--;
                      int has = 0; for (int k = rs; k < rl; k++) if (md[k] == '|') { has = 1; break; }
                      if (!has) break;                       /* a non-table line ends it (left for the main loop) */
                      md_table_row(out, &p, cap, md + rs, rl - rs, 0);
                      i = re; if (i < mdlen) i++;
                  }
                  md_put(out, &p, cap, "</table>");
                  continue;
              }
          } }
        if (list) { md_put(out, &p, cap, list == 1 ? "</ul>" : "</ol>"); list = 0; }   /* paragraph text */
        if (!para) { md_put(out, &p, cap, "<p>"); para = 1; } else md_putc(out, &p, cap, ' ');
        md_inline(out, &p, cap, T, tn);
    }
    if (in_pre) md_put(out, &p, cap, "</pre>");
    if (para)   md_put(out, &p, cap, "</p>");
    if (list)   md_put(out, &p, cap, list == 1 ? "</ul>" : "</ol>");
    return p;
}

/* CSV -> an HTML <table>, so the browser can view local .csv data files. Bounded
 * for untrusted input: every write capped, every read within `len`. Handles RFC-
 * 4180 quoting ("" is a literal quote; commas inside quotes are not delimiters);
 * the first row becomes the <th> header. Embedded newlines inside quotes aren't
 * supported (rows split on '\n') — a documented simplification. */
static int csv_to_html(const char *s, int len, char *out, int cap) {
    int p = 0, i = 0, row = 0;
    md_put(out, &p, cap, "<table>");
    while (i < len) {
        md_put(out, &p, cap, "<tr>");
        int eol = 0;
        while (!eol) {
            md_put(out, &p, cap, row == 0 ? "<th>" : "<td>");
            if (i < len && s[i] == '"') {                /* quoted field */
                i++;
                while (i < len) {
                    if (s[i] == '"') {
                        if (i + 1 < len && s[i + 1] == '"') { md_esc(out, &p, cap, '"'); i += 2; }
                        else { i++; break; }             /* closing quote */
                    } else { md_esc(out, &p, cap, s[i]); i++; }
                }
            } else {                                     /* bare field */
                while (i < len && s[i] != ',' && s[i] != '\n' && s[i] != '\r')
                    { md_esc(out, &p, cap, s[i]); i++; }
            }
            md_put(out, &p, cap, row == 0 ? "</th>" : "</td>");
            if (i < len && s[i] == ',') i++;             /* another field follows */
            else eol = 1;                                /* newline or EOF ends the row */
        }
        md_put(out, &p, cap, "</tr>");
        while (i < len && s[i] != '\n') i++;             /* consume to end of line */
        if (i < len) i++;                                /* and the '\n' */
        row++;
    }
    md_put(out, &p, cap, "</table>");
    return p;
}

#endif /* MDCONV_H */
