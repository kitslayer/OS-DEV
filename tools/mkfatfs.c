/*
 * mkfatfs.c — a tiny host-side FAT32 image builder.
 *
 * Runs on the HOST (built with the system gcc), not in the kernel. It writes a
 * small but valid FAT32 filesystem image with a few files in the root
 * directory, which QEMU then attaches as a virtual disk. We build the image
 * ourselves (rather than mkfs.fat + mtools) so the project is self-contained
 * and the on-disk layout is guaranteed to match our kernel reader.
 *
 *   usage: mkfatfs <output.img>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR        512
#define TOTAL_SECTORS 8192          /* 4 MiB image */
#define RESERVED      32
#define NUM_FATS      2
#define SPC           1             /* sectors per cluster */

static uint8_t *img;

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* The files baked into the root directory. Names are 8.3, space-padded to 11. */
static const struct {
    const char *name83;
    const char *content;
} files[] = {
    { "README  TXT", "OS-DEV: a from-scratch x86_64 OS.\nThis file lives on a FAT32 disk read by our own driver.\n" },
    { "HELLO   TXT", "Hello from a real file on a virtual disk!\n" },
    { "MOTD    TXT", "Milestone 10 reached: VFS + FAT32 + ATA driver.\nTry: ls   and   cat hello.txt\n" },
    { "PRE     HTM", "<h2>Preformatted</h2><p>This paragraph is normal flow: whitespace    collapses and the text wraps to the window width as usual.</p><pre>function hello() {\n    return 1 + 2;      // spaces   kept\n\n    blank line above is preserved\n}</pre><p>Back to normal flow after the pre block.</p>" },
    { "LIST    HTM", "<h2>Lists</h2><p>An ordered list with a nested bullet list:</p><ol><li>First item<li>Second item<ul><li>nested bullet<li>another nested</ul><li>Third item</ol><p>And a plain bullet list:</p><ul><li>alpha<li>beta<li>gamma</ul><p>Ordered-list variants &mdash; <code>type</code> and <code>start</code>:</p><ol type=\"a\"><li>lower-alpha<li>second</ol><ol type=\"I\"><li>upper-roman<li>second<li>third</ol><ol start=\"8\"><li>starts at eight<li>nine</ol>" },
    { "TABLE   HTM", "<h2>Table</h2><p>A small table renders as pipe-separated rows with bold headers:</p><table><tr><th>Name<th>Role<th>Year<tr><td>Alice<td>Engineer<td>2021<tr><td>Bob<td>Designer<td>2022<tr><td>Carol<td>Manager<td>2019</table><p>After the table.</p>" },
    { "ENT     HTM", "<h2>Entities</h2><p>Named: &lsquo;single&rsquo; and &ldquo;double&rdquo;, dash &mdash; here, ellipsis&hellip; bullet &bull; copy &copy;.</p><p>Symbols fold to ASCII: arrows &larr; &uarr; &rarr; &darr;, plus-minus &plusmn;, times &times; divide &divide;, degree &deg;, section &sect;, paragraph &para;, prime &prime;.</p><p>Numeric decimal: &#39;apos&#39; and &#8220;quote&#8221;.</p><p>Numeric hex: &#x27;hex-apos&#x27; and &#x2014; em-dash.</p>" },
    { "UTF8    HTM", "<h2>UTF-8</h2><p>Raw UTF-8 bytes (not entities): smart quotes “hello” and ‘hi’, em dash — here, ellipsis… bullet •.</p><p>Accents fold to ASCII: café, naïve, jalapeño, Über, straße.</p><p>Symbols: euro € and 30°C.</p><!-- a comment with a > and <b>markup</b> inside: none of THIS must render -->A<!--[if IE]><p>conditional</p><![endif]-->B<p>After comments.</p><svg width=\"12\" height=\"12\"><title>svgtitle-must-not-show</title><text x=\"0\" y=\"9\">SVGLEAK</text><path d=\"M0 0 L12 12\"/></svg><p>After svg.</p>" },
    { "IMG     HTM", "<h2>Images</h2><p>Local images render inline, decoded by our own PNG, GIF and JPEG code: <img src=\"file:test.png\" alt=\"the test image\"> and an icon <img src=\"file:icon.png\" alt=\"an icon\">. Here is a baseline JPEG photo, scaled with width=\"240\" (its natural size is 120): <img src=\"file:photo.jpg\" alt=\"a jpeg photo\" width=\"240\">. Remote images stay clickable links you follow to view full-size.</p>" },
    { "FORM    HTM", "<h1>Forms: type and process input</h1>"
        "<p>Fields are now editable: Tab/n to a field, Enter to focus it, type, Enter to finish. A button reads <code>.value</code> and does something with it.</p>"
        "<p>Your name: <input id=\"name\" placeholder=\"(click here, then type)\"></p>"
        "<p>Favourite number: <input id=\"num\" placeholder=\"a number\"></p>"
        "<p><button onclick=\"document.getElementById('out').textContent='Hello '+document.getElementById('name').value+'! Your number doubled is '+(parseInt(document.getElementById('num').value)*2)\">[ Greet &amp; compute ]</button></p>"
        "<p id=\"out\">(fill the fields, then click the button)</p>"
        "<p>The button reads each field's value via <code>getElementById(id).value</code>, computes, and writes the result into the paragraph &mdash; real input-&gt;process-&gt;output.</p>" },
    { "RPS     HTM", "<h1>Rock Paper Scissors</h1>"
        "<p>Tab/n to a button, Enter to play. The CPU's move is <code>Math.random(3)</code>; the score persists across clicks via the page's JS environment.</p>"
        "<p>You played: <b><span id=\"you\">-</span></b> &nbsp;&nbsp; CPU played: <b><span id=\"cpu\">-</span></b></p>"
        "<h2 id=\"result\">Make your move:</h2>"
        "<p><button onclick=\"play(0)\">[ Rock ]</button> &nbsp; <button onclick=\"play(1)\">[ Paper ]</button> &nbsp; <button onclick=\"play(2)\">[ Scissors ]</button></p>"
        "<p>Wins: <b><span id=\"w\">0</span></b> &nbsp; Losses: <b><span id=\"l\">0</span></b> &nbsp; Ties: <b><span id=\"t\">0</span></b></p>"
        "<script>\n"
        "var names=['Rock','Paper','Scissors'];\n"
        "var wins=0, losses=0, ties=0;\n"
        "function play(you){\n"
        "  var cpu=Math.random(3);\n"
        "  document.getElementById('you').textContent=names[you];\n"
        "  document.getElementById('cpu').textContent=names[cpu];\n"
        "  var out;\n"
        "  if(you==cpu){ ties=ties+1; out='Tie!'; }\n"
        "  else if(cpu==(you+2)%3){ wins=wins+1; out='You win!'; }\n"
        "  else { losses=losses+1; out='You lose.'; }\n"
        "  document.getElementById('result').textContent=out;\n"
        "  document.getElementById('w').textContent=wins;\n"
        "  document.getElementById('l').textContent=losses;\n"
        "  document.getElementById('t').textContent=ties;\n"
        "}\n"
        "</script>" },
    { "BASE    HTM", "<h1>Number Base Converter</h1>"
        "<p>Tab/n to the field, Enter to type a decimal number, Enter to finish, then Convert. Uses the JS engine's <code>parseInt</code> + <code>Number.toString(radix)</code>.</p>"
        "<p>Decimal: <input id=\"dec\" placeholder=\"e.g. 255\"></p>"
        "<p><button onclick=\"conv()\">[ Convert ]</button></p>"
        "<p>Binary: <b><span id=\"bin\">-</span></b></p>"
        "<p>Octal: &nbsp;<b><span id=\"oct\">-</span></b></p>"
        "<p>Hex: &nbsp;&nbsp;&nbsp;<b><span id=\"hex\">-</span></b></p>"
        "<script>\n"
        "function conv(){\n"
        "  var n=parseInt(document.getElementById('dec').value);\n"
        "  document.getElementById('bin').textContent=n.toString(2);\n"
        "  document.getElementById('oct').textContent=n.toString(8);\n"
        "  document.getElementById('hex').textContent='0x'+n.toString(16);\n"
        "}\n"
        "</script>" },
    { "GUESS   HTM", "<h1>Guess the Number</h1>"
        "<p>I'm thinking of a number from 1 to 100. Tab/n to the field, Enter, type a guess, Enter, then Guess. The secret persists across clicks.</p>"
        "<p>Your guess: <input id=\"g\"></p>"
        "<p><button onclick=\"guess()\">[ Guess ]</button> &nbsp; <button onclick=\"newgame()\">[ New game ]</button></p>"
        "<h2 id=\"msg\">Make your first guess!</h2>"
        "<p>Tries: <b><span id=\"cnt\">0</span></b></p>"
        "<script>\n"
        "var secret=Math.random(100)+1;\n"
        "var tries=0;\n"
        "function guess(){\n"
        "  var g=parseInt(document.getElementById('g').value);\n"
        "  tries=tries+1; document.getElementById('cnt').textContent=tries;\n"
        "  var m;\n"
        "  if(g==secret) m='Correct! '+secret+' in '+tries+' tries. (New game to replay)';\n"
        "  else if(g<secret) m=g+' is too LOW -- guess higher.';\n"
        "  else m=g+' is too HIGH -- guess lower.';\n"
        "  document.getElementById('msg').textContent=m;\n"
        "}\n"
        "function newgame(){\n"
        "  secret=Math.random(100)+1; tries=0;\n"
        "  document.getElementById('cnt').textContent=0;\n"
        "  document.getElementById('msg').textContent='New number chosen -- guess!';\n"
        "}\n"
        "</script>" },
    { "ASCII   HTM", "<h1>ASCII Table (printable 32-126)</h1>"
        "<p>Generated at page load by a JS loop &mdash; <code>String.fromCharCode</code> + <code>document.write</code>.</p>"
        "<script>\n"
        "document.write('<pre>');\n"
        "for(var c=32; c<127; c++){\n"
        "  var ch=String.fromCharCode(c);\n"
        "  if(c==60) ch='&lt;'; else if(c==62) ch='&gt;'; else if(c==38) ch='&amp;';\n"
        "  var cs=''+c; while(cs.length<3) cs=' '+cs;\n"
        "  document.write(cs+' '+ch+'   ');\n"
        "  if((c-31)%8==0) document.write('\\n');\n"
        "}\n"
        "document.write('</pre>');\n"
        "</script>" },
    { "SEARCH  HTM", "<h1>Search the web</h1>"
        "<p>A real working web search, from scratch. Type a query and follow <b>[Search]</b>: the browser builds <code>action?q=YOUR+QUERY</code>, URL-encodes it, and navigates over <b>HTTPS</b> to DuckDuckGo's HTML results &mdash; a genuine HTML GET form submission to the live web.</p>"
        "<form action=\"https://html.duckduckgo.com/html/\">"
        "<p>Query: <input id=\"q\" name=\"q\" placeholder=\"e.g. operating system\"></p>"
        "<p><button type=\"submit\">Search DuckDuckGo</button></p>"
        "</form>"
        "<p>How: Tab/n to the field, Enter to edit, type, Enter to finish; then Tab/n to <b>[Search]</b> and Enter. Spaces become <code>+</code> and specials are percent-encoded, so <code>?q=operating+system</code> is a valid request. The results page is itself a form &mdash; search again right from it.</p>"
        "<p>The same mechanism works with any GET form (try pointing <code>action</code> at example.com to watch the query string appear in the address bar).</p>" },
    { "LOGIN   HTM", "<h1>Password masking</h1>"
        "<p>A <code>type=\"password\"</code> field shows <code>*</code> on screen, but JavaScript still reads the real value. Tab/n to a field, Enter to edit, type, Enter to finish.</p>"
        "<p>User: <input id=\"user\" placeholder=\"name\"></p>"
        "<p>Pass: <input id=\"pw\" type=\"password\" placeholder=\"secret\"></p>"
        "<p><button onclick=\"document.getElementById('msg').textContent='Signed in as '+document.getElementById('user').value+' -- password had '+document.getElementById('pw').value.length+' chars'\">[ Sign in ]</button></p>"
        "<p id=\"msg\">(type a name and password, then Sign in)</p>"
        "<p>The password renders as <code>***</code>, but <code>getElementById('pw').value</code> returns the real text &mdash; here the button reports its length to prove it read the actual characters.</p>" },
    { "ATTR    HTM", "<h1>Reading &amp; writing attributes</h1>"
        "<p>JavaScript can read and change any element's HTML attributes with <code>getAttribute</code> / <code>setAttribute</code> &mdash; the href of a link, custom <code>data-*</code> values, a field's type, and so on.</p>"
        "<p>Here is a link: <a id=\"lnk\" href=\"https://example.com/page\" data-note=\"from-the-tag\">example link</a></p>"
        "<p><button onclick=\"var e=document.getElementById('lnk'); document.getElementById('out').textContent='href = '+e.getAttribute('href')+'   data-note = '+e.getAttribute('data-note')\">[ Read its attributes ]</button></p>"
        "<p><button onclick=\"var e=document.getElementById('lnk'); e.setAttribute('data-note','CHANGED-by-setAttribute'); document.getElementById('out').textContent='after setAttribute: data-note = '+e.getAttribute('data-note')\">[ Change data-note ]</button></p>"
        "<p id=\"out\">(read the link's attributes, or change one and read it back)</p>"
        "<p>Reading pulls values straight out of the page's HTML (a missing attribute returns <code>null</code>); <code>setAttribute</code> rewrites the tag in place, so reading it back shows the new value.</p>" },
    { "LOC     HTM", "<h1>window.location</h1>"
        "<p>Page JavaScript can read the current page's URL via <code>window.location</code> &mdash; how a search-results page knows what you searched for (its <code>?q=</code>).</p>"
        "<p><button onclick=\"var l=location; document.getElementById('out').innerHTML='href: '+l.href+'<br>protocol: '+l.protocol+'<br>host: '+(l.host||'(none)')+'<br>pathname: '+l.pathname+'<br>search: '+(l.search||'(none)')\">[ Show window.location ]</button></p>"
        "<p id=\"out\">(click to read the location)</p>"
        "<p>This page is <code>file:loc.htm</code>, so it has no host and no query. On <code>https://html.duckduckgo.com/html/?q=os</code>, <code>location.search</code> would be <code>?q=os</code>.</p>" },
    { "CHECK   HTM", "<h1>Checkboxes &amp; radios</h1>"
        "<p>Tab/n to a box and Enter to toggle it. A button reads each one's state via <code>.value</code> (<code>on</code> when checked, empty when not).</p>"
        "<p><input id=\"c1\" type=\"checkbox\" name=\"news\" checked> Subscribe to news</p>"
        "<p><input id=\"c2\" type=\"checkbox\" name=\"beta\"> Join the beta</p>"
        "<p>Plan: <input id=\"r1\" type=\"radio\" name=\"plan\"> Free &nbsp; <input id=\"r2\" type=\"radio\" name=\"plan\" checked> Pro</p>"
        "<p><button onclick=\"document.getElementById('out').textContent='news='+(document.getElementById('c1').value||'off')+'  beta='+(document.getElementById('c2').value||'off')+'  pro='+(document.getElementById('r2').value||'off')\">[ Read states ]</button></p>"
        "<p id=\"out\">(toggle the boxes, then read their states)</p>"
        "<p>A checked box submits <code>name=on</code> with the form; an unchecked one is omitted. Selecting a radio unchecks the others sharing its <code>name</code> (try Free, then Pro).</p>" },
    { "ONCHG   HTM", "<h1>onchange handler</h1>"
        "<p>An input's <code>onchange</code> runs JavaScript the moment its value changes &mdash; no button needed. Tab/n to the box and Enter to toggle it:</p>"
        "<p><input id=\"agree\" type=\"checkbox\" onchange=\"document.getElementById('msg').textContent = document.getElementById('agree').value=='on' ? 'Thanks for agreeing!' : 'You unchecked it.'\"> I agree to the terms</p>"
        "<p id=\"msg\">(toggle the checkbox above)</p>"
        "<p>Text fields fire <code>onchange</code> when they lose focus (Enter or Esc). Name: <input id=\"nm\" onchange=\"document.getElementById('msg2').textContent='Hi, '+document.getElementById('nm').value+'!'\" placeholder=\"type, then Enter\"></p>"
        "<p id=\"msg2\">(type a name, then press Enter)</p>"
        "<p><code>oninput</code> fires on <i>every</i> keystroke (live). Search: <input id=\"live\" oninput=\"document.getElementById('echo').textContent='You typed: '+document.getElementById('live').value+' ('+document.getElementById('live').value.length+' chars)'\" placeholder=\"type here\"></p>"
        "<p id=\"echo\">(start typing &mdash; updates as you go)</p>"
        "<p>Toggling/blurring fires <code>onchange</code>; typing fires <code>oninput</code> &mdash; each reads the new <code>.value</code> and rewrites the page. Reactive forms, from scratch.</p>" },
    { "ANCHOR  HTM", "<h1 id=\"top\">In-page anchors</h1>"
        "<p>Links to <code>#id</code> jump to the element with that id &mdash; the way a table of contents or a &lsquo;back to top&rsquo; link works. Tab/n to a link, Enter to jump:</p>"
        "<ul><li><a href=\"#a\">Section A</a><li><a href=\"#b\">Section B</a><li><a href=\"#c\">Section C (bottom)</a><li><a href=\"#legacy\">Legacy target (&lt;a name&gt;)</a></ul>"
        "<h2 id=\"a\">Section A</h2>"
        "<p>This is section A. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls.</p>"
        "<p>More of section A. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls.</p>"
        "<h2 id=\"b\">Section B</h2>"
        "<p>This is section B. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls.</p>"
        "<p>More of section B. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls. Filler so the page scrolls.</p>"
        "<h2 id=\"c\">Section C</h2>"
        "<p>This is section C, the last one. Clicking the TOC link above jumped straight here.</p>"
        "<h2><a name=\"legacy\">Legacy target</a></h2>"
        "<p>This heading is an old-style <code>&lt;a name=\"legacy\"&gt;</code> anchor &mdash; the TOC link still jumps here.</p>"
        "<p><a href=\"#top\">Back to top</a></p>" },
    { "DETAILS HTM", "<h1>Collapsible details</h1>"
        "<p>Tab/n to a <b>[+]</b> summary and Enter to expand it (Enter again to collapse). The body shows/hides in place.</p>"
        "<details><summary>What is OS-DEV?</summary>"
        "<p>A from-scratch x86_64 operating system with its own kernel, TLS stack, JavaScript engine, and graphical web browser &mdash; all written in C. This text is hidden until you expand the section.</p></details>"
        "<details><summary>How do collapsible sections work here?</summary>"
        "<p>Each &lt;details&gt; gets an index; clicking its &lt;summary&gt; toggles a per-page open/closed bit, and the parser suppresses the body's tokens while it is collapsed.</p></details>"
        "<details open><summary>This one starts expanded</summary>"
        "<p>Because it has the <code>open</code> attribute, its body renders on load. Collapse it with Enter.</p></details>"
        "<p>Three independent sections &mdash; each toggles on its own.</p>" },
    { "COLOR   HTM", "<h2>Colours</h2><p>Text can be <font color=\"red\">red</font>, <font color=\"green\">green</font>, <font color=\"blue\">blue</font>, <font color=\"#E07000\">hex orange</font>, and <font color=\"purple\">purple</font>. Back to normal.</p>" },
    { "INDEX   HTM", "<title>OS-DEV Demos</title><style> h1{color:#2C66D6} h2{color:#800080} </style><h1>OS-DEV Demo Index</h1><p>Local demo pages baked onto the FAT32 disk &mdash; select with Tab/n and press Enter:</p><ul><li><a href=\"file:pre.htm\">Preformatted text</a><li><a href=\"file:list.htm\">Lists (nested &amp; ordered)</a><li><a href=\"file:nested.htm\">Deeply nested lists</a><li><a href=\"file:table.htm\">Tables</a><li><a href=\"file:ent.htm\">HTML entities</a><li><a href=\"file:img.htm\">Images</a><li><a href=\"file:color.htm\">Coloured text</a><li><a href=\"file:style.htm\">Inline CSS &mdash; color, bold (font-weight), italic (font-style)</a><li><a href=\"file:css.htm\">CSS &lt;style&gt; blocks &mdash; tag / .class / #id rules</a><li><a href=\"file:nest.htm\">Nested style scopes &mdash; colours compose to any depth</a><li><a href=\"file:article.htm\">A styled article &mdash; the CSS engine on realistic prose</a><li><a href=\"file:code.htm\">Inline code</a><li><a href=\"file:anchor.htm\">In-page anchors (#id jump-to-section)</a><li><a href=\"file:details.htm\">Collapsible &lt;details&gt; sections</a></ul><h2>Interactive (JavaScript + DOM)</h2><ul><li><a href=\"file:dom.htm\">Interactive DOM &mdash; click to rewrite the page</a><li><a href=\"file:form.htm\">Editable forms &mdash; type, then process input</a><li><a href=\"file:rps.htm\">Rock Paper Scissors &mdash; a playable game (Math.random + a score that persists)</a><li><a href=\"file:base.htm\">Number base converter &mdash; decimal to binary/octal/hex (parseInt + toString)</a><li><a href=\"file:guess.htm\">Guess the Number &mdash; a game; the secret (Math.random) persists across guesses</a><li><a href=\"file:ascii.htm\">ASCII table &mdash; generated at load by a JS loop (fromCharCode + document.write)</a><li><a href=\"file:search.htm\">Web search &mdash; HTTPS form submit to DuckDuckGo</a><li><a href=\"file:login.htm\">Password masking &mdash; * on screen, real value to JS</a><li><a href=\"file:attr.htm\">get/setAttribute &mdash; JS reads &amp; writes HTML attributes</a><li><a href=\"file:loc.htm\">window.location &mdash; JS reads the page URL</a><li><a href=\"file:check.htm\">Checkboxes &amp; radios &mdash; toggle, read via .value</a><li><a href=\"file:onchg.htm\">onchange &mdash; a handler runs the moment a box toggles</a><li><a href=\"file:jstest.htm\">Page script (document.write)</a><li><a href=\"file:oop.htm\">Object-oriented page script</a><li><a href=\"file:jsnew.htm\">New engine features &mdash; operators, number literals, stdlib</a><li><a href=\"file:remove.htm\">element.remove() &mdash; JS removes an element from the page</a><li><a href=\"file:qsa.htm\">querySelector(All) &mdash; find elements by CSS selector</a><li><a href=\"file:qsaw.htm\">querySelector write &mdash; rewrite a matched element</a><li><a href=\"file:domq.htm\">Live DOM query &mdash; onclick runs CSS selectors + classList</a><li><a href=\"file:persist.htm\">Persistent page JS &mdash; onclick calls load-defined functions</a><li><a href=\"file:todo.htm\">Persistent array state &mdash; a to-do list across clicks</a><li><a href=\"file:events.htm\">JS-assigned handlers &mdash; el.onclick=fn / addEventListener</a><li><a href=\"file:app.htm\">Mini task app &mdash; addEventListener + array + querySelectorAll</a><li><a href=\"file:schange.htm\">Scripted onchange &mdash; checkbox.onchange = fn</a><li><a href=\"file:evtarg.htm\">Event argument &mdash; e.type / e.target / this</a><li><a href=\"file:oncefn.htm\">Remove a handler &mdash; el.onclick = null</a><li><a href=\"file:matches.htm\">element.matches() &mdash; event-delegation selector test</a><li><a href=\"file:rmattr.htm\">removeAttribute &mdash; drop an attribute from an element</a><li><a href=\"file:closest.htm\">element.closest() &mdash; walk up to a matching ancestor</a><li><a href=\"file:children.htm\">element.children &mdash; the direct child elements</a><li><a href=\"file:create.htm\">createElement + appendChild &mdash; build DOM nodes</a><li><a href=\"file:parent.htm\">element.parentElement &mdash; the enclosing element</a><li><a href=\"file:domshow.htm\">DOM showcase &mdash; createElement + query + traverse together</a><li><a href=\"file:sibling.htm\">nextElementSibling / previousElementSibling</a><li><a href=\"file:tagname.htm\">element.tagName &mdash; the element's tag</a></ul><p>Backspace returns here. Press <b>h</b> for the start page.</p>" },
    { "CODE    HTM", "<h2>Inline code</h2><p>Run the <code>browse</code> command, then press <kbd>Enter</kbd> to follow a link. The call <code>memcpy(dst, src, n)</code> copies <samp>n</samp> bytes; configuration lives in <tt>/etc/config</tt>. Inline code renders in a distinct colour so it stands out from <b>bold</b> and <i>italic</i> text.</p><p>Edits show too: <del>this was removed</del> and <s>so was this</s>, but <b>this stays</b>. Price: <s>$50</s> now $30. And <mark>this part is highlighted</mark> like a marker pen.</p><p>Science: H<sub>2</sub>O, CO<sub>2</sub>, and E = mc<sup>2</sup>; see the footnote<sup>3</sup>.</p><p>Back to normal flow.</p>" },
    { "NESTED  HTM", "<h2>Deeply nested lists</h2><ol><li>Top one<ol><li>One-A<li>One-B<ul><li>bullet under 1-B<li>another bullet<ol><li>deep one<li>deep two</ol></ul></ol><li>Top two<ul><li>plain bullet<li>plain bullet</ul><li>Top three</ol><p>Numbering and indentation track the nesting depth.</p>" },
    { "GUIDE   TXT", "OS-DEV quick guide\n==================\nDesktop: Apps menu launches programs. Drag titlebars; drag edges to resize.\n  F2 cycle focus, F4 maximise, F5/F6 tile left/right.\nBrowser: type a host, file:NAME, or a search query then Enter (a query that\n  isn't a URL searches DuckDuckGo). Tab/n/p pick links, Enter follows.\n  g/G top/bottom, h home, r reload, s save, u view-source, a bookmark, \\ find.\n  Interactive: Enter on a [field] to type into it (Enter/Esc when done);\n  follow a button/link to run the page's JavaScript (it can read fields and\n  rewrite the page live). See file:dom.htm and file:form.htm.\nShell: ls, cat, cd, tree, find, grep, df, run NAME.ELF, browse URL, wget URL.\nStart here: browse file:index.htm\n" },
    { "SAMPLE  JS ", "// sample.js  --  run with: js sample.js   (edit with: edit sample.js)\n"
        "print(\"FizzBuzz 1..15:\");\n"
        "var line = \"\";\n"
        "for (var i = 1; i <= 15; i = i + 1) {\n"
        "  if (i % 15 == 0) line += \"FizzBuzz\";\n"
        "  else if (i % 3 == 0) line += \"Fizz\";\n"
        "  else if (i % 5 == 0) line += \"Buzz\";\n"
        "  else line += i;\n"
        "  line += \" \";\n"
        "}\n"
        "print(line);\n"
        "function isPrime(n){ if (n < 2) return false; for (var d = 2; d*d <= n; d = d+1) if (n % d == 0) return false; return true; }\n"
        "var primes = \"\";\n"
        "for (var k = 2; k < 30; k = k+1) if (isPrime(k)) primes += k + \" \";\n"
        "print(\"primes < 30: \" + primes);\n"
        "var os = { name: \"OS-DEV\", lang: \"JavaScript\" };\n"
        "print(os.lang + \" running on \" + os.name + \"!\");\n" },
    { "JSTEST  HTM", "<h1>JavaScript in the browser</h1>"
        "<p>Everything below this line is generated <b>live</b> by JavaScript running inside the page (via document.write):</p>"
        "<script>\n"
        "for (var i = 1; i <= 5; i++) document.write(\"<p>Item \" + i + \": \" + i + \" squared = \" + (i*i) + \"</p>\");\n"
        "var sum = 0; for (var k = 1; k <= 100; k++) sum += k;\n"
        "document.write(\"<h2>Sum of 1..100 = \" + sum + \"</h2>\");\n"
        "console.log(\"page script ran; sum=\" + sum);\n"
        "</script>" },
    { "QSA     HTM", "<h1>querySelector &amp; querySelectorAll</h1>"
        "<p>This page has three &lt;p class=&quot;fruit&quot;&gt; items. A script finds them with CSS selectors (by <code>.class</code> and by <code>tag.class</code>), counts them, iterates with <code>for...of</code>, and writes what it found into the elements below (located by id):</p>"
        "<p class=\"fruit\">Apple</p>"
        "<p class=\"fruit\">Banana</p>"
        "<p class=\"fruit\">Cherry</p>"
        "<h2 id=\"title\">(first match appears here)</h2>"
        "<div id=\"out\">(the full list appears here)</div>"
        "<script>\n"
        "var items = document.querySelectorAll(\".fruit\");\n"
        "console.log(\"qsa .fruit count=\" + items.length);\n"
        "var names = \"\";\n"
        "for (var it of items) names += it.textContent + \" \";\n"
        "document.getElementById(\"out\").textContent = \"Found \" + items.length + \" fruit: \" + names;\n"
        "var first = document.querySelector(\"p.fruit\");\n"
        "console.log(\"first p.fruit=\" + first.textContent);\n"
        "document.getElementById(\"title\").textContent = \"First match: \" + first.textContent;\n"
        "console.log(\"missing selector -> \" + document.querySelector(\".nope\"));\n"
        "console.log(\"byTag p=\" + document.getElementsByTagName(\"p\").length);\n"
        "console.log(\"byClass fruit=\" + document.getElementsByClassName(\"fruit\").length);\n"
        "console.log(\"first fruit class=\" + document.querySelector(\"p.fruit\").getAttribute(\"class\"));\n"
        "console.log(\"byAttr [class]=\" + document.querySelectorAll(\"[class]\").length);\n"
        "console.log(\"byAttr p[class]=\" + document.querySelectorAll(\"p[class]\").length);\n"
        "console.log(\"hasAttr class=\" + document.querySelector(\"p.fruit\").hasAttribute(\"class\") + \" title=\" + document.querySelector(\"p.fruit\").hasAttribute(\"title\"));\n"
        "</script>" },
    { "QSAW    HTM", "<h1>querySelector write</h1>"
        "<p>A load-time script finds the first &lt;p class=&quot;msg&quot;&gt; by CSS selector, rewrites its text and sets an attribute on it, then the page re-renders &mdash; no reload. The position handle resolves to the element's byte span and splices the page source:</p>"
        "<p class=\"msg\">(original first paragraph)</p>"
        "<p class=\"msg\">(second paragraph &mdash; left unchanged)</p>"
        "<script>\n"
        "var first = document.querySelector(\"p.msg\");\n"
        "first.textContent = \"Rewritten via a querySelector position handle!\";\n"
        "first.setAttribute(\"data-done\", \"yes\");\n"
        "console.log(\"qsaw read-back=\" + document.querySelector(\"p.msg\").textContent);\n"
        "console.log(\"qsaw attr=\" + document.querySelector(\"p.msg\").getAttribute(\"data-done\"));\n"
        "first.classList.add(\"hi\"); first.classList.add(\"hi\"); first.classList.toggle(\"big\");\n"
        "console.log(\"qsaw classList=\" + document.querySelector(\"p.msg\").getAttribute(\"class\") + \" has-hi=\" + first.classList.contains(\"hi\"));\n"
        "</script>" },
    { "DOMQ    HTM", "<h1>Live DOM query &amp; classList</h1>"
        "<p>Each button's <code>onclick</code> runs a CSS-selector query against the list below, live &mdash; no reload. (The handler re-queries each click, so it always sees the current page.)</p>"
        "<ul><li class=\"task\">Buy milk</li><li class=\"task done\">Walk the dog</li><li class=\"task\">Write an OS</li></ul>"
        "<p><button onclick=\"var t=document.querySelectorAll('.task').length, d=document.querySelectorAll('.done').length; document.getElementById('out').textContent='Tasks: '+t+', done: '+d; console.log('count t='+t+' d='+d)\">Count tasks</button> "
        "<button onclick=\"var f=document.querySelector('li.task'); document.getElementById('out').textContent='First: '+f.textContent+' (done? '+f.classList.contains('done')+')'; console.log('first='+f.textContent+' done='+f.classList.contains('done'))\">Inspect first</button></p>"
        "<div id=\"out\">(click a button above)</div>" },
    { "PERSIST HTM", "<h1>Persistent page JS</h1>"
        "<p>The load script defines <code>greet()</code> and a counter <code>clicks</code>. The button's <code>onclick</code> calls the function and increments the counter &mdash; which works only because the page's JavaScript environment now <b>persists</b> across the load script and every click handler (the counter even survives between clicks, with no localStorage):</p>"
        "<button onclick=\"++clicks; var m=greet('Ada'); document.getElementById('out').textContent=m+' (clicks: '+clicks+')'; console.log('click '+clicks+': '+m)\">Greet</button>"
        "<div id=\"out\">(click the button)</div>"
        "<script>\n"
        "function greet(name){ return 'Hello, '+name+'!'; }\n"
        "var clicks = 0;\n"
        "console.log('load: greet+clicks defined');\n"
        "</script>" },
    { "TODO    HTM", "<h1>Persistent state: a to-do list</h1>"
        "<p>Each click pushes onto / pops from a JavaScript <b>array</b> that lives in the page's persistent environment &mdash; no localStorage, and the state is never read back from the DOM; the array object itself survives between clicks &mdash; then a load-defined <code>render()</code> redraws the list:</p>"
        "<button onclick=\"todos.push('Task #'+(todos.length+1)); render()\">Add a task</button> "
        "<button onclick=\"if(todos.length)todos.pop(); render()\">Remove last</button>"
        "<div id=\"list\">(no tasks yet)</div>"
        "<script>\n"
        "var todos = [];\n"
        "function render(){ document.getElementById('list').textContent = todos.length ? todos.join('  |  ') : '(no tasks yet)'; console.log('todos now: '+todos.length); }\n"
        "console.log('todo load: array+render defined');\n"
        "</script>" },
    { "EVENTS  HTM", "<h1>JS-assigned event handlers</h1>"
        "<p>The load script attaches handlers to the buttons below via <code>document.getElementById('go').onclick = function(){...}</code> and <code>.addEventListener('click', ...)</code> &mdash; there is <b>no</b> inline <code>onclick=</code> attribute. The handler function (defined at load) is stored and fires on click, its state persisting:</p>"
        "<button id=\"go\">Run onclick handler</button> "
        "<button id=\"add\">addEventListener button</button>"
        "<div id=\"out\">(no clicks yet)</div>"
        "<script>\n"
        "var n = 0;\n"
        "document.getElementById('go').onclick = function(){ n++; document.getElementById('out').textContent = 'onclick fired '+n+' time(s)'; console.log('go clicked: '+n); };\n"
        "document.getElementById('add').addEventListener('click', function(){ document.getElementById('out').textContent = 'addEventListener handler ran!'; console.log('add clicked'); });\n"
        "console.log('handlers attached');\n"
        "</script>" },
    { "APP     HTM", "<h1>Mini task app</h1>"
        "<p>Built entirely with this browser's from-scratch JS DOM &mdash; <code>addEventListener</code> handlers, a persistent array, <code>querySelectorAll</code>, and <code>innerHTML</code>, with no page reloads:</p>"
        "<button id=\"add\">Add task</button> <button id=\"count\">Count via querySelectorAll</button> <button id=\"clear\">Clear all</button>"
        "<div id=\"status\">(loading)</div>"
        "<ul id=\"list\"></ul>"
        "<script>\n"
        "var tasks = [];\n"
        "function draw(){\n"
        "  var h = '';\n"
        "  for (var i=0;i<tasks.length;i++) h += '<li class=\"task\">' + tasks[i] + '</li>';\n"
        "  document.getElementById('list').innerHTML = h || '<li>(no tasks)</li>';\n"
        "  document.getElementById('status').textContent = tasks.length + ' task(s)';\n"
        "}\n"
        "document.getElementById('add').addEventListener('click', function(){ tasks.push('Task ' + (tasks.length+1)); draw(); console.log('added; now ' + tasks.length); });\n"
        "document.getElementById('count').addEventListener('click', function(){ var n = document.querySelectorAll('.task').length; document.getElementById('status').textContent = 'querySelectorAll found ' + n + ' .task'; console.log('counted ' + n); });\n"
        "document.getElementById('clear').addEventListener('click', function(){ tasks = []; draw(); console.log('cleared'); });\n"
        "draw();\n"
        "console.log('app ready');\n"
        "</script>" },
    { "SCHANGE HTM", "<h1>Scripted onchange</h1>"
        "<p>The load script sets <code>checkbox.onchange = function(){...}</code> &mdash; there is no inline <code>onchange=</code> attribute. Toggling the box runs the JS-assigned handler (state persists):</p>"
        "<input type=\"checkbox\" id=\"cb\"> Enable feature"
        "<div id=\"out\">(toggle the box)</div>"
        "<script>\n"
        "var toggles = 0;\n"
        "document.getElementById('cb').onchange = function(){ toggles++; document.getElementById('out').textContent = 'onchange fired ' + toggles + ' time(s)'; console.log('onchange fired: ' + toggles); };\n"
        "console.log('scripted onchange attached');\n"
        "</script>" },
    { "EVTARG  HTM", "<h1>Event argument</h1>"
        "<p>The handler receives an <code>event</code> object (<code>e.type</code>, <code>e.target</code>) and runs with <code>this</code> bound to the element it fired on:</p>"
        "<button id=\"b\">Click me</button>"
        "<div id=\"out\">(click the button)</div>"
        "<script>\n"
        "document.getElementById('b').addEventListener('click', function(e){ var s = 'type=' + e.type + ' target.id=' + e.target.id + ' this.id=' + this.id; document.getElementById('out').textContent = s; console.log('evt ' + s); });\n"
        "console.log('event-arg handler attached');\n"
        "</script>" },
    { "ONCEFN  HTM", "<h1>Remove a handler (el.onclick = null)</h1>"
        "<p>This button's onclick removes itself after firing once (<code>el.onclick = null</code>), so a second click does nothing:</p>"
        "<button id=\"b\">Click (fires once)</button>"
        "<div id=\"out\">(click the button)</div>"
        "<script>\n"
        "var c = 0;\n"
        "document.getElementById('b').onclick = function(){ c++; document.getElementById('out').textContent = 'fired ' + c + ' time(s); handler now removed'; console.log('fired ' + c); document.getElementById('b').onclick = null; };\n"
        "console.log('once-handler attached');\n"
        "</script>" },
    { "MATCHES HTM", "<h1>element.matches()</h1>"
        "<p>The click handler tests <code>e.target.matches(selector)</code> against several selectors &mdash; the event-delegation idiom:</p>"
        "<button id=\"b\" class=\"btn\">Click me</button>"
        "<div id=\"out\">(click the button)</div>"
        "<script>\n"
        "document.getElementById('b').addEventListener('click', function(e){ var r = 'button=' + e.target.matches('button') + ' .btn=' + e.target.matches('.btn') + ' #b=' + e.target.matches('#b') + ' .x=' + e.target.matches('.x'); document.getElementById('out').textContent = r; console.log('matches ' + r); });\n"
        "console.log('matches handler attached');\n"
        "</script>" },
    { "RMATTR  HTM", "<h1>removeAttribute</h1>"
        "<p>The load script reads two attributes, removes one with <code>removeAttribute</code>, and re-reads &mdash; the removed one is now <code>null</code> while the other stays:</p>"
        "<p id=\"x\" data-foo=\"bar\" title=\"hi there\">A paragraph with attributes.</p>"
        "<div id=\"out\">?</div>"
        "<script>\n"
        "var p = document.getElementById('x');\n"
        "console.log('before: data-foo=' + p.getAttribute('data-foo') + ' title=' + p.getAttribute('title'));\n"
        "p.removeAttribute('data-foo');\n"
        "console.log('after: data-foo=' + p.getAttribute('data-foo') + ' title=' + p.getAttribute('title'));\n"
        "document.getElementById('out').textContent = 'data-foo=' + p.getAttribute('data-foo') + ', title=' + p.getAttribute('title');\n"
        "</script>" },
    { "CLOSEST HTM", "<h1>element.closest()</h1>"
        "<p>The click handler walks up from the clicked button to the nearest ancestor matching a selector &mdash; the event-delegation idiom (and shows that a matched element's <code>.id</code> is readable):</p>"
        "<div class=\"card\" id=\"card1\"><p>A card containing a button.</p><button id=\"b\">Click me</button></div>"
        "<div id=\"out\">(click the button)</div>"
        "<script>\n"
        "document.getElementById('b').addEventListener('click', function(e){ var c=e.target.closest('.card'); var r='closest .card id=' + (c?c.id:'null') + ', closest(button)=' + (e.target.closest('button')?'self':'null') + ', closest(.nope)=' + e.target.closest('.nope'); document.getElementById('out').textContent=r; console.log('closest: ' + r); });\n"
        "console.log('closest handler attached');\n"
        "</script>" },
    { "CHILDRENHTM", "<h1>element.children</h1>"
        "<p>The script reads the <b>direct</b> child elements of the list &mdash; the nested item is not a direct child, so the count is 3, not 4:</p>"
        "<ul id=\"list\"><li>One</li><li>Two<ul><li>Nested</li></ul></li><li>Three</li></ul>"
        "<div id=\"out\">?</div>"
        "<script>\n"
        "var c = document.getElementById('list').children;\n"
        "document.getElementById('out').textContent = 'count=' + c.length + ', first=' + c[0].textContent + ', last=' + c[c.length-1].textContent;\n"
        "console.log('children count=' + c.length + ' first=' + c[0].textContent + ' last=' + c[c.length-1].textContent);\n"
        "</script>" },
    { "CREATE  HTM", "<h1>createElement + appendChild</h1>"
        "<p>Each click builds a new &lt;li&gt; with <code>document.createElement</code>, sets its text and class, and <code>appendChild</code>s it to the list &mdash; real DOM node construction. The created items are then found by <code>querySelectorAll('.item')</code>, proving they're live elements:</p>"
        "<ul id=\"list\"></ul>"
        "<button id=\"add\">Add an item</button>"
        "<div id=\"out\">(click to add)</div>"
        "<script>\n"
        "var count = 0;\n"
        "document.getElementById('add').addEventListener('click', function(){ count++; var li=document.createElement('li'); li.textContent='Item '+count+' (created)'; li.className='item'; document.getElementById('list').appendChild(li); document.getElementById('out').textContent=count+' items added; '+document.querySelectorAll('.item').length+' match .item'; console.log('appended item '+count+', .item count='+document.querySelectorAll('.item').length); });\n"
        "console.log('createElement demo ready');\n"
        "</script>" },
    { "PARENT  HTM", "<h1>element.parentElement</h1>"
        "<p>The script reads the parent element of a nested paragraph &mdash; it finds the enclosing &lt;div&gt; by id:</p>"
        "<div id=\"outer\"><p id=\"inner\">A nested paragraph.</p></div>"
        "<div id=\"out\">?</div>"
        "<script>\n"
        "var inner = document.getElementById('inner');\n"
        "var p = inner.parentElement;\n"
        "document.getElementById('out').textContent = 'parent id = ' + (p ? p.id : 'null') + ', grandparent = ' + (p && p.parentElement ? p.parentElement.id || '(body)' : 'null');\n"
        "console.log('parentElement id=' + (p ? p.id : 'null'));\n"
        "</script>" },
    { "DOMSHOW HTM", "<h1>DOM showcase</h1>"
        "<p>The whole from-scratch DOM working together &mdash; <code>createElement</code>, <code>appendChild</code>, <code>querySelectorAll</code>, <code>element.children</code>, and <code>parentElement</code>, driven by <code>addEventListener</code>:</p>"
        "<button id=\"add\">Add item</button> <button id=\"info\">Inspect</button>"
        "<ul id=\"list\"></ul>"
        "<div id=\"out\">(Add a few items, then Inspect)</div>"
        "<script>\n"
        "var n = 0;\n"
        "document.getElementById('add').addEventListener('click', function(){ n++; var li=document.createElement('li'); li.textContent='Item '+n; li.className='item'; document.getElementById('list').appendChild(li); document.getElementById('out').textContent=document.querySelectorAll('.item').length+' items'; console.log('added '+n+', total '+document.querySelectorAll('.item').length); });\n"
        "document.getElementById('info').addEventListener('click', function(){ var items=document.querySelectorAll('.item'); var first=items.length?items[0]:null; document.getElementById('out').textContent='querySelectorAll .item='+items.length+', list.children='+document.getElementById('list').children.length+', first.parentElement.id='+(first?first.parentElement.id:'none'); console.log('inspect: qsa='+items.length+' children='+document.getElementById('list').children.length+' parent='+(first?first.parentElement.id:'none')); });\n"
        "console.log('DOM showcase ready');\n"
        "</script>" },
    { "SIBLING HTM", "<h1>nextElementSibling / previousElementSibling</h1>"
        "<p>The script reads the siblings of the middle list item &mdash; one before, one after:</p>"
        "<ul id=\"list\"><li id=\"a\">One</li><li id=\"b\">Two</li><li id=\"c\">Three</li></ul>"
        "<div id=\"out\">?</div>"
        "<script>\n"
        "var mid = document.getElementById('b');\n"
        "var nxt = mid.nextElementSibling, prv = mid.previousElementSibling;\n"
        "document.getElementById('out').textContent = 'next = ' + (nxt?nxt.textContent:'null') + ', prev = ' + (prv?prv.textContent:'null');\n"
        "console.log('sibling next=' + (nxt?nxt.textContent:'null') + ' prev=' + (prv?prv.textContent:'null'));\n"
        "</script>" },
    { "TAGNAME HTM", "<h1>element.tagName</h1>"
        "<p>The script reads the tag names of a few elements (uppercased, per the DOM), by id and by querySelector:</p>"
        "<p id=\"x\">a paragraph</p><div id=\"y\"><span id=\"z\">a span</span></div>"
        "<div id=\"out\">?</div>"
        "<script>\n"
        "var s = document.getElementById('x').tagName + ', ' + document.getElementById('y').tagName + ', ' + document.getElementById('z').tagName + ', qs(span)=' + document.querySelector('span').tagName;\n"
        "document.getElementById('out').textContent = s;\n"
        "console.log('tagNames: ' + s);\n"
        "</script>" },
    { "STYLE   HTM", "<title>Inline CSS</title><h1>Inline CSS</h1>"
        "<p>A small subset of CSS &mdash; the <code>color</code>, <code>font-weight</code> and <code>font-style</code> properties in an inline <code>style=\"...\"</code> attribute &mdash; now style text:</p>"
        "<p style=\"color: red\">This paragraph is red (a named colour).</p>"
        "<p style=\"color: #008000\">This one is green (a #hex colour).</p>"
        "<p>Normal text, then <span style=\"color: blue\">this span is blue</span>, then normal again &mdash; the colour scope ends at the span's close.</p>"
        "<p style=\"background-color: yellow; color: purple\">Purple text &mdash; <code>background-color</code> is ignored; only <code>color</code> applies.</p>"
        "<p style=\"font-weight: bold\">This paragraph is bold (font-weight: bold).</p>"
        "<p style=\"font-weight: 700\">Numeric weight 700 is bold too.</p>"
        "<p style=\"font-style: italic\">This one is italic (font-style: italic).</p>"
        "<p>Normal, then <span style=\"font-weight:bold; color:#cc0000\">bold red</span> (weight + colour together), then normal.</p>"
        "<p style=\"color: rgb(192, 20, 60)\">This uses <code>rgb(192, 20, 60)</code> &mdash; functional colour notation.</p>"
        "<p style=\"color: rgb(10%, 40%, 75%)\">And this is <code>rgb(10%, 40%, 75%)</code> &mdash; percentage components.</p>"
        "<p style=\"color: crimson\">Named colours expanded too: <span style=\"color:indigo\">indigo</span>, <span style=\"color:teal\">teal</span>, <span style=\"color:steelblue\">steelblue</span>, <span style=\"color:darkgreen\">darkgreen</span>.</p>"
        "<p>An <u>underlined</u> word (the <code>&lt;u&gt;</code> tag), and <span style=\"text-decoration: underline\">text-decoration: underline</span> set inline.</p>"
        "<p style=\"text-decoration: underline; color: #cc0000; font-weight: bold\">Underlined, red, <i>and</i> bold &mdash; all three compose on one element.</p>" },
    { "CSS     HTM", "<title>CSS style blocks</title><style>\n"
        "  h2 { color: #800080 }\n"
        "  p { color: #333333 }\n"
        "  .warn { color: red; font-weight: bold }\n"
        "  #lead { color: #008000; font-style: italic }\n"
        "  .b { font-weight: bold }\n"
        "</style>"
        "<h1>CSS &lt;style&gt; blocks</h1>"
        "<p>A <code>&lt;style&gt;</code> block in the page now drives a small CSS engine: simple rules (<code>tag</code>, <code>.class</code>, <code>#id</code>) set <code>color</code> / <code>font-weight</code> / <code>font-style</code>, cascading under inline <code>style=</code>.</p>"
        "<h2>This heading is purple (h2 rule)</h2>"
        "<p id=\"lead\">This lead paragraph is green italic &mdash; matched by <code>#lead</code>.</p>"
        "<p>This paragraph is dark gray &mdash; matched by the bare <code>p</code> rule.</p>"
        "<p class=\"warn\">This is a bold red warning &mdash; <code>.warn</code> wins over <code>p</code> (later rule).</p>"
        "<p class=\"b\">This whole paragraph is bold <i>and</i> gray &mdash; the <code>.b</code> rule (bold) cascades with the <code>p</code> rule (gray) on one element.</p>"
        "<p style=\"color: #0000cc\">Inline <code>style=\"color:blue\"</code> overrides the <code>p</code> rule &mdash; this line is blue.</p>" },
    { "NEST    HTM", "<title>Nested style scopes</title><style>\n"
        "  p { color: #333333 }\n"
        "  .red { color: #cc0000 }\n"
        "</style>"
        "<h1>Nested style scopes</h1>"
        "<p>Styled elements now <i>nest</i>: a colour applies to its element's content and the previous colour is restored at its close, to any depth.</p>"
        "<p>This paragraph is gray (the <code>p</code> rule), with a <span class=\"red\">red span</span> inside, then gray again.</p>"
        "<p style=\"color: #008000\">Green (inline) with a <span style=\"color:#0000cc\">blue span</span> nested inside, then green again &mdash; inline nesting composes too.</p>"
        "<p style=\"color: #800080\">Purple, with <b style=\"color:#cc0000\">bold red</b> nested, then <span style=\"font-style:italic\">italic purple</span>, then purple.</p>" },
    { "ARTICLE HTM", "<title>A styled article</title>"
        "<style> h1 { color:#2c66d6 }  h2 { color:#800080 }  .lead { color:#555555; font-style:italic }  .tip { color:#006400; font-weight:bold } </style>"
        "<h1>Rendering CSS from scratch</h1>"
        "<p class=\"lead\">How a from-scratch OS browser styles text without a layout engine.</p>"
        "<p>This page is drawn by OS-DEV's own browser and styled by its own small CSS engine &mdash; no libraries, just a token-stream renderer with a per-element "
        "<span style=\"color:#cc0000\">colour</span> / <b>weight</b> / <span style=\"font-style:italic\">style</span> / <u>underline</u> scope that nests.</p>"
        "<h2>What works</h2>"
        "<p>Inline <code>style=</code> and <code>&lt;style&gt;</code> rules with <code>tag</code>, <code>.class</code> and <code>#id</code> selectors; a real cascade (rules &lt; inline, per property); and a scope stack so styled elements nest to any depth.</p>"
        "<p class=\"tip\">Tip: see style.htm, css.htm and nest.htm for the focused feature tests.</p>"
        "<h2>What doesn't (yet)</h2>"
        "<p>Layout &mdash; <code>font-size</code>, <code>text-align</code>, the box model &mdash; needs a real layout engine the flat token stream lacks. That's the honest boundary; the <i>machinery</i> of CSS (selectors, cascade, nesting) is here.</p>" },
    { "HELP    HTM", "<html><head><title>OS-DEV Help</title>"
        "<style> h1 { color:#2C66D6 }  h2 { color:#800080 }  dt { color:#b8860b; font-weight:bold }  .key { color:#006400; font-weight:bold } </style></head><body>"
        "<h1>OS-DEV Help</h1>"
        "<p>A quick guide to the system &mdash; this page is itself styled by the from-scratch CSS engine.</p>"
        "<h2>Apps</h2>"
        "<p>Open from the <b>Apps</b> menu (<span class=\"key\">F9</span>) or the shell with <code>run NAME</code>:</p>"
        "<dl>"
        "<dt>shell, clock, calc, editor</dt><dd>a terminal, a clock dashboard, a calculator (<code>+ - * / % ^ &amp; | &lt;&lt; &gt;&gt; ~</code>, parens, <code>0x</code> hex), a text editor</dd>"
        "<dt>games</dt><dd>snake, 2048, life, tetris, breakout, mines, sudoku, maze, hangman, ttt (tic-tac-toe vs an unbeatable AI), bj (blackjack), simon (a tone-memory game), c4 (Connect Four vs an AI), adv (a text adventure) &mdash; all in colour, several with saved best scores</dd>"
        "<dt>tools</dt><dd>calendar (month view), mandel (a Mandelbrot explorer), piano, jukebox (built-in tunes), matrix (a digital-rain screensaver), paint (an ASCII-art canvas, saves to PAINT.TXT), typing (a WPM speed test)</dd>"
        "</dl>"
        "<h2>Shell commands</h2>"
        "<p>Files: <code>ls cat head tail sort nl tac uniq cut edit write rm cp mv mkdir cd pwd tree find grep hexdump wc</code>. "
        "Net: <code>get URL</code>, <code>headers URL</code>, <code>wget URL FILE</code>, <code>browse URL</code>, <code>ping [HOST]</code>, <code>resolve HOST</code>, <code>ifconfig</code>. "
        "Crypto: <code>sha256 FILE</code>, <code>sha512 FILE</code>, <code>crypt</code>, <code>base64</code>. "
        "Also: <code>apps</code>, <code>js</code>, <code>cal</code>, <code>date</code>, <code>mem</code>, <code>ps</code>, <code>df</code>, <code>beep</code>, <code>morse TEXT</code>, <code>factor N</code>, <code>clear</code>. Type <code>help</code> in the shell for the full list.</p>"
        "<h2>Keyboard</h2>"
        "<p>Desktop: <span class=\"key\">F2</span> cycle windows, <span class=\"key\">F3</span> minimise, <span class=\"key\">F4</span> maximise, <span class=\"key\">F5</span>/<span class=\"key\">F6</span> tile, <span class=\"key\">F9</span> Apps menu.</p>"
        "<p>Browser: <span class=\"key\">Tab</span>/<span class=\"key\">n</span> next link, <span class=\"key\">p</span> previous, <span class=\"key\">Enter</span> follow, <span class=\"key\">Backspace</span> back, <span class=\"key\">h</span> home, <span class=\"key\">s</span> save, <span class=\"key\">a</span> bookmark, <span class=\"key\">\\</span> find text.</p>"
        "</body></html>" },
    { "JSDEEP  HTM", "<h1>Deep recursion guard</h1>"
        "<p>This page's script recurses 500 deep on purpose. The interpreter must"
        " stop it at the depth cap and report an error &mdash; <b>without</b> crashing"
        " the OS (the browser runs page scripts on the kernel stack, which has no"
        " guard page):</p>"
        "<script>\n"
        "function r(n){ return n <= 0 ? 0 : 1 + r(n - 1); }\n"
        "document.write(\"<p>r(500) = \" + r(500) + \"</p>\");\n"
        "</script>"
        "<p>If you can read this paragraph and the OS is still responsive, the guard held.</p>" },
    { "JSNEW   HTM", "<h1>New JS engine features</h1>"
        "<p>Every value below is computed <b>live</b> by the from-scratch JavaScript engine &mdash; operators, number-literal formats, and standard-library methods added recently:</p>"
        "<script>\n"
        "document.write(\"<h2>Operators</h2>\");\n"
        "document.write(\"<p>2 ** 10 = \" + (2 ** 10) + \" | 2 ** 3 ** 2 = \" + (2 ** 3 ** 2) + \" (right-associative)</p>\");\n"
        "document.write(\"<p>5 in [1,2,3,4,5] = \" + (5 in [1,2,3,4,5]) + \" | 12 ^ 10 = \" + (12 ^ 10) + \" | ~5 = \" + (~5) + \"</p>\");\n"
        "var o = {a:1, b:2, c:3}; delete o.b;\n"
        "document.write(\"<p>after delete o.b, keys = \" + Object.keys(o).join(\", \") + \"</p>\");\n"
        "class Animal { constructor(n){ this.n = n; } } class Dog extends Animal {}\n"
        "document.write(\"<p>new Dog() instanceof Animal = \" + (new Dog(\"Rex\") instanceof Animal) + \"</p>\");\n"
        "document.write(\"<h2>Number literals</h2>\");\n"
        "document.write(\"<p>0b1010 = \" + 0b1010 + \" | 0o17 = \" + 0o17 + \" | 0xFF = \" + 0xFF + \" | 1e3 = \" + 1e3 + \"</p>\");\n"
        "document.write(\"<h2>Standard library</h2>\");\n"
        "document.write(\"<p>Array.from({length:5}, i =&gt; i*i) = [\" + Array.from({length:5}, function(x,i){return i*i;}).join(\", \") + \"]</p>\");\n"
        "document.write(\"<p>[3,1,2,5,4].fill(0,1,3) = [\" + [3,1,2,5,4].fill(0,1,3).join(\", \") + \"]</p>\");\n"
        "document.write(\"<p>Math.hypot(3,4) = \" + Math.hypot(3,4) + \" | Math.cbrt(27) = \" + Math.cbrt(27) + \" | Math.log2(1024) = \" + Math.log2(1024) + \"</p>\");\n"
        "var fz = Object.freeze({x:1}); fz.x = 99;\n"
        "document.write(\"<p>Object.freeze({x:1}): after fz.x=99, x is still \" + fz.x + \"</p>\");\n"
        "document.write(\"<h2>Modern classes &amp; assignment</h2>\");\n"
        "class Pt { x = 0; y = 0; dist2(){ return this.x*this.x + this.y*this.y; } }\n"
        "var pt = new Pt(); pt.x = 3; pt.y = 4;\n"
        "document.write(\"<p>class fields + method: new Pt(), set x=3 y=4, dist2() = \" + pt.dist2() + \"</p>\");\n"
        "var cfg = {}; cfg.theme ||= \"dark\"; cfg.theme ||= \"light\";\n"
        "document.write(\"<p>logical assign: cfg.theme ||= dark then ||= light gives \" + cfg.theme + \"</p>\");\n"
        "class Counter { static total = 0; static add(){ Counter.total = Counter.total + 1; return Counter.total; } }\n"
        "document.write(\"<p>static counter: add() three times = \" + Counter.add() + \", \" + Counter.add() + \", \" + Counter.add() + \"</p>\");\n"
        "function hl(parts, v){ return parts[0] + \"[\" + v + \"]\" + parts[1]; }\n"
        "document.write(\"<p>tagged template (cooked strings + a ${6*7} value) = \" + hl`a ${6*7} b` + \"</p>\");\n"
        "document.write(\"<h2>Getters &amp; setters (M261)</h2>\");\n"
        "var temp = { _c: 20, get f(){ return this._c * 9 / 5 + 32; }, set f(v){ this._c = (v - 32) * 5 / 9; } };\n"
        "document.write(\"<p>object literal: get f() reads 20&deg;C as \" + temp.f + \"&deg;F</p>\");\n"
        "temp.f = 212;\n"
        "document.write(\"<p>then set f = 212 runs the setter, so _c is now \" + temp._c + \"&deg;C</p>\");\n"
        "class Circle { constructor(r){ this.r = r; } get area(){ return (314 * this.r * this.r) / 100; } }\n"
        "document.write(\"<p>class getter: new Circle(5).area = \" + new Circle(5).area + \" (using pi as 314/100 in the integer engine)</p>\");\n"
        "var counter = { _n: 5, get n(){ return this._n; }, set n(v){ this._n = v; } }; counter.n++;\n"
        "document.write(\"<p>counter.n++ routes through get+set, leaving the accessor intact: n = \" + counter.n + \"</p>\");\n"
        "document.write(\"<h2>Object.is &amp; immutable arrays (M259&ndash;260)</h2>\");\n"
        "document.write(\"<p>Object.is({},{}) = \" + Object.is({},{}) + \" (distinct objects) | Object.is(2,2) = \" + Object.is(2,2) + \"</p>\");\n"
        "var base = [3,1,2]; var sorted = base.toSorted();\n"
        "document.write(\"<p>[3,1,2].toSorted() = [\" + sorted.join(\", \") + \"], original still [\" + base.join(\", \") + \"] (immutable)</p>\");\n"
        "document.write(\"<p>[1,2,3].with(1, 9) = [\" + [1,2,3].with(1,9).join(\", \") + \"]</p>\");\n"
        "document.write(\"<h2>Prototype chain (M263)</h2>\");\n"
        "var animal = { describe: function(){ return this.name + \" says \" + this.sound; } };\n"
        "var dog = Object.create(animal); dog.name = \"Rex\"; dog.sound = \"woof\";\n"
        "document.write(\"<p>Object.create(animal): dog.describe() = \" + dog.describe() + \"</p>\");\n"
        "function Counter(){ this.n = 0; } Counter.prototype.inc = function(){ return ++this.n; };\n"
        "var c = new Counter();\n"
        "document.write(\"<p>new Counter() + Counter.prototype.inc(): \" + c.inc() + \", \" + c.inc() + \", \" + c.inc() + \"</p>\");\n"
        "document.write(\"<p>c instanceof Counter = \" + (c instanceof Counter) + \" | Object.getPrototypeOf(dog) === animal = \" + (Object.getPrototypeOf(dog) === animal) + \"</p>\");\n"
        "document.write(\"<h2>Metaprogramming &amp; collections (M262&ndash;279)</h2>\");\n"
        "var person={_name:\"Ada\"}; Object.defineProperty(person,\"name\",{get:function(){return this._name;},set:function(v){this._name=v;}});\n"
        "document.write(\"<p>Object.defineProperty accessor: person.name = \" + person.name + \"</p>\");\n"
        "document.write(\"<p>Reflect: has(name) = \" + Reflect.has(person,\"name\") + \", ownKeys = [\" + Reflect.ownKeys(person).join(\", \") + \"]</p>\");\n"
        "var wmap=new WeakMap(); var wkey={}; wmap.set(wkey,42);\n"
        "document.write(\"<p>WeakMap: set a key, get it back = \" + wmap.get(wkey) + \"</p>\");\n"
        "document.write(\"<p>Equality: two distinct {}=={} is \" + ({}=={}) + \" | loose 1 == (string)1 is \" + (1==\"1\") + \" | strict 1 === (string)1 is \" + (1===\"1\") + \"</p>\");\n"
        "document.write(\"<p>(1234567).toLocaleString() = \" + (1234567).toLocaleString() + \"</p>\");\n"
        "</script>"
        "<p><a href=\"file:index.htm\">Back to the demo index</a></p>" },
    { "REMOVE  HTM", "<h1>element.remove()</h1>"
        "<p>The middle paragraph is removed at load by JavaScript &mdash; only Alpha and Gamma should remain below:</p>"
        "<p id=\"a\">Alpha (kept)</p>"
        "<p id=\"b\">Beta (removed by JS)</p>"
        "<p id=\"c\">Gamma (kept)</p>"
        "<script>document.getElementById(\"b\").remove();</script>"
        "<p>Backspace or the link returns to the index.</p>"
        "<p><a href=\"file:index.htm\">Back to the demo index</a></p>" },
    { "DOM     HTM", "<h1>Interactive DOM</h1>"
        "<p>Each link runs JavaScript that updates the page <b>in place</b> by mutating an element through <code>document.getElementById</code> &mdash; no document.write. Tab/n to a link, Enter to run it:</p>"
        "<p>Counter: <b id=\"count\">0</b> &nbsp; fib(counter) = <b id=\"fib\">0</b></p>"
        "<p>Message: <span id=\"msg\">(click below)</span></p><ul>"
        "<li><a href=\"javascript:var c=parseInt(document.getElementById('count').textContent)+1; document.getElementById('count').textContent=''+c\">increment the counter</a></li>"
        "<li><a href=\"javascript:function f(k){return k<2?k:f(k-1)+f(k-2);} document.getElementById('fib').textContent=''+f(parseInt(document.getElementById('count').textContent))\">compute fib(counter) &mdash; DOM read, JS recursion, DOM write</a></li>"
        "<li><a href=\"javascript:document.getElementById('msg').textContent='Hello from a real DOM!'\">set the message text</a></li>"
        "<li><a href=\"javascript:document.getElementById('msg').innerHTML='now <b>bold</b> via innerHTML'\">set message HTML</a></li>"
        "<li><a href=\"javascript:document.getElementById('count').textContent='0'; document.getElementById('fib').textContent='0'; document.getElementById('msg').textContent='(reset)'\">reset</a></li>"
        "</ul>"
        "<p>And an inline <code>&lt;button onclick=...&gt;</code> (a real HTML event handler, not a javascript: link): "
        "<button onclick=\"document.getElementById('count').textContent='42'\">[ set counter to 42 ]</button></p>"
        "<p>The values above change when you click &mdash; the element is found by id, its content replaced (after computing), and the page re-renders.</p>" },
    { "OOP     HTM", "<h1>Object-oriented JavaScript in the browser</h1>"
        "<p>The list below is generated live by a page &lt;script&gt; using classes, inheritance, <code>super</code>, destructuring, spread and template literals:</p><ul>"
        "<script>\n"
        "class Shape {\n"
        "  constructor(name){ this.name = name; }\n"
        "  describe(){ return this.name; }\n"
        "}\n"
        "class Circle extends Shape {\n"
        "  constructor(r){ super(\"circle\"); this.r = r; }\n"
        "  describe(){ return super.describe() + \" r=\" + this.r + \" area=\" + (3*this.r*this.r); }\n"
        "}\n"
        "var shapes = [new Circle(2), new Circle(5)];\n"
        "for (var s of shapes) document.write(\"<li>\" + s.describe() + \"</li>\");\n"
        "var [first, ...others] = shapes;\n"
        "document.write(\"<p>first is a \" + first.describe() + \"; \" + others.length + \" others</p>\");\n"
        "var nums = [1, 2, 3];\n"
        "document.write(\"<p>spread: [\" + [0, ...nums, 4].join(\", \") + \"]</p>\");\n"
        "document.write(`<p>template literal: ${nums.length} numbers, max ${Math.max(...nums)}</p>`);\n"
        "</script>" },
    { "SHOWCASEJS ", "print(\"== OS-DEV from-scratch JavaScript ==\");\n"
        "var n = [5, 3, 8, 1, 9, 2, 7];\n"
        "print(\"squares: \" + n.map(x => x * x).join(\", \"));\n"
        "print(\"evens:   \" + n.filter(x => x % 2 == 0).join(\", \"));\n"
        "var s = 0; n.forEach(x => s += x); print(\"sum:     \" + s);\n"
        "function fib(k){ return k < 2 ? k : fib(k-1) + fib(k-2); }\n"
        "print(\"fib:     \" + [0,1,2,3,4,5,6,7,8,9,10].map(fib).join(\", \"));\n"
        "var d = { os: \"OS-DEV\", ms: 169, features: [\"TLS\", \"browser\", \"JS\"] };\n"
        "print(\"json:    \" + JSON.stringify(d));\n"
        "var back = JSON.parse(JSON.stringify(d));\n"
        "print(\"parse:   \" + back.os + \", \" + back.features.length + \" features, sqrt(2025)=\" + Math.sqrt(2025));\n" },
    { "JSCLICK HTM", "<h1>Interactive JavaScript</h1>"
        "<p>Each link runs JavaScript <b>in the page</b> when you click it (or Tab/n to select, then Enter). The result is appended below:</p><ul>"
        "<li><a href=\"javascript:var c=(parseInt(localStorage.getItem('c'))||0)+1; localStorage.setItem('c',c); document.write('<p><b>counter = '+c+'</b> (click again &mdash; it remembers, via localStorage)</p>')\">a stateful counter</a></li>"
        "<li><a href=\"javascript:document.write('<p><b>6 * 7 = '+(6*7)+'</b></p>')\">compute 6 * 7</a></li>"
        "<li><a href=\"javascript:var s=0; for(var i=1;i<=100;i++) s+=i; document.write('<p>sum 1..100 = '+s+'</p>')\">sum 1..100</a></li>"
        "<li><a href=\"javascript:document.write('<p>squares: '+[1,2,3,4,5].map(x=>x*x).join(', ')+'</p>')\">squares via map(x =&gt; x*x)</a></li>"
        "<li><a href=\"javascript:document.write('<p>primes up to 50: '+(function(){var r=[];for(var n=2;n<50;n++){var p=true;for(var d=2;d*d<=n;d++)if(n%d==0)p=false;if(p)r.push(n);}return r;})().join(', ')+'</p>')\">primes under 50</a></li>"
        "<li><a href=\"javascript:document.write('<p>8! = '+[1,2,3,4,5,6,7,8].reduce(function(a,b){return a*b;})+'</p>')\">8 factorial via reduce</a></li>"
        "<li><a href=\"javascript:document.write('<p>reversed: '+'OS-DEV rocks'.split('').reverse().join('')+'</p>')\">reverse a string</a></li>"
        "</ul><hr>" },
    { "CLASS   JS ", "// class.js  --  object-oriented JavaScript, from scratch.  Run: js class.js\n"
        "class Animal {\n"
        "  constructor(name){ this.name = name; }\n"
        "  speak(){ return this.name + \" makes a sound\"; }\n"
        "}\n"
        "class Dog extends Animal {\n"
        "  constructor(name){ super(name); this.sound = \"woof\"; }\n"
        "  speak(){ return super.speak() + \" (\" + this.sound + \")\"; }\n"
        "}\n"
        "var d = new Dog(\"Rex\");\n"
        "print(d.speak());\n"
        "print(\"name=\" + d.name + \" sound=\" + d.sound);\n"
        "// a little stack, with method chaining via `this`\n"
        "class Stack {\n"
        "  constructor(){ this.items = []; }\n"
        "  push(x){ this.items.push(x); return this; }\n"
        "  size(){ return this.items.length; }\n"
        "  top(){ return this.items[this.size() - 1]; }\n"
        "}\n"
        "var st = new Stack().push(10).push(20).push(30);\n"
        "print(\"stack size=\" + st.size() + \" top=\" + st.top());\n"
        "// three-level inheritance with super at each step\n"
        "class A { who(){ return \"A\"; } }\n"
        "class B extends A { who(){ return super.who() + \"B\"; } }\n"
        "class C extends B { who(){ return super.who() + \"C\"; } }\n"
        "print(\"chain=\" + new C().who());\n" },
    { "ES6     JS ", "// es6.js  --  modern JavaScript: spread, rest, destructuring.  Run: js es6.js\n"
        "var nums = [3, 1, 4, 1, 5, 9];\n"
        "var [head, ...rest] = nums;\n"
        "print(\"head=\" + head + \" rest=\" + rest.join(\",\"));\n"
        "print(\"spread: \" + [0, ...nums, 10].join(\",\"));\n"
        "print(\"max=\" + Math.max(...nums));\n"
        "function tag(first, ...others){ return first + \" + \" + others.length + \" more\"; }\n"
        "print(tag(\"a\", \"b\", \"c\", \"d\"));\n"
        "var person = { name: \"Ada\", role: \"pioneer\", year: 1843 };\n"
        "var { name, ...info } = person;\n"
        "print(\"name=\" + name + \" info=\" + JSON.stringify(info));\n"
        "var { role: job = \"x\" } = person;\n"
        "print(\"job=\" + job);\n"
        "for (var [k, v] of [[\"x\", 1], [\"y\", 2], [\"z\", 3]]) print(\"  \" + k + \" => \" + v);\n"
        "print(\"merged: \" + JSON.stringify({ ...person, role: \"legend\" }));\n" },
    { "COLLECT JS ", "// collect.js  --  Map, Set, optional chaining, nullish.  Run: js collect.js\n"
        "var words = \"the cat sat on the mat the cat ran\".split(\" \");\n"
        "var freq = new Map();\n"
        "for (var w of words) freq.set(w, (freq.get(w) ?? 0) + 1);\n"
        "for (var [word, count] of freq) print(word + \": \" + count);\n"
        "var nums = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5];\n"
        "var uniq = new Set(); nums.forEach(n => uniq.add(n));\n"
        "print(\"unique: \" + uniq.values().join(\",\") + \" (\" + uniq.size + \" of \" + nums.length + \")\");\n"
        "var cfg = { server: { port: 8080 } };\n"
        "print(\"port=\" + (cfg?.server?.port ?? 3000));\n"
        "print(\"host=\" + (cfg?.server?.host ?? \"localhost\"));\n"
        "print(\"missing=\" + (cfg?.db?.name ?? \"none\"));\n" },
    { "REGEX   JS ", "// regex.js  --  from-scratch regular expressions, with /literal/ syntax.  Run: js regex.js\n"
        "var prices = \"apples $3, bread $2, milk $4\";\n"
        "print(\"prices: \" + prices.match(/\\d+/g).join(\", \"));\n"
        "var ok = /^[a-z0-9_]+$/i;\n"
        "print(\"user_42 valid? \" + ok.test(\"user_42\"));\n"
        "print(\"bad name! valid? \" + ok.test(\"bad name!\"));\n"
        "print(\"ISO: \" + \"01/15/2024\".replace(/(\\d+)\\/(\\d+)\\/(\\d+)/, \"$3-$1-$2\"));\n"
        "print(\"tokens: \" + \"red,  green ,blue\".split(/\\s*,\\s*/).join(\" | \"));\n"
        "print(\"first word: \" + \"  Hello there\".match(/[A-Za-z]+/)[0]);\n"
        "print(\"collapsed: [\" + \"too   many    spaces\".replace(/\\s+/g,\" \") + \"]\");\n" },
    { "RXTEST  JS ", "// rxtest.js  --  regex hardening: pathological patterns fail gracefully, never crash.\n"
        "print(\"deep groups: \" + new RegExp(\"(\".repeat(1800)).test(\"x\"));\n"   /* parser depth-capped at 400 */
        "print(\"long match: \" + new RegExp(\"a+\").test(\"a\".repeat(2500)));\n"  /* matcher depth-capped at 900 */
        "print(\"redos: \" + new RegExp(\"(a+)+$\").test(\"aaaaaaaaaaaaaaaaaaaaX\"));\n"  /* step-budget, no hang */
        "print(\"normal: \" + \"id-42\".replace(new RegExp(\"(\\\\w+)-(\\\\d+)\"), \"$2:$1\"));\n"
        "print(\"SURVIVED\");\n" },
};
#define NUM_FILES (int)(sizeof(files) / sizeof(files[0]))
#define CLUSTER_BYTES (SPC * SECTOR)
#define DIR_PER_CLUSTER (CLUSTER_BYTES / 32)   /* 32-byte dir entries per cluster */

/* Files copied verbatim from a host path (e.g. a built ELF) so the OS can load
 * real programs from disk. Skipped with a warning if the path is missing. */
static const struct {
    const char *name83;
    const char *hostpath;
} hostfiles[] = {
    { "CALC    ELF", "build/calc.elf" },   /* run it in-OS with: run calc.elf */
    { "SUITE   JS ", "tests/js/suite.js" },/* the JS regression suite — run in-OS with: js suite.js */
    { "TEST    PNG", "tools/test.png" },   /* view in-OS with: browse file:test.png */
    { "BIG     PNG", "tools/big.png" },    /* 113 KB image — needs the 128 KB fetch buffer */
    { "ICON    PNG", "tools/icon.png" },  /* palette + tRNS transparency */
    { "LOGO    GIF", "tools/logo.gif" },  /* GIF: LZW + transparency + interlace */
    { "PHOTO   JPG", "tools/photo.jpg" }, /* baseline JPEG (integer IDCT, 4:2:0) */
    { "INTER   PNG", "tools/inter.png" }, /* interlaced (Adam7) truecolour PNG */
    { "PPHOTO  JPG", "tools/pphoto.jpg" },/* PROGRESSIVE JPEG (multi-scan, integer) */
    { "ANIM    GIF", "tools/anim.gif" },  /* ANIMATED GIF (multi-frame, timer-driven) */
};
#define NUM_HOST (int)(sizeof(hostfiles) / sizeof(hostfiles[0]))

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output.img>\n", argv[0]);
        return 1;
    }

    /* Solve for the FAT size (sectors) that covers the cluster count. */
    uint32_t fatsz = 1, clusters;
    for (;;) {
        uint32_t data = TOTAL_SECTORS - RESERVED - NUM_FATS * fatsz;
        clusters = data / SPC;
        uint32_t need = ((clusters + 2) * 4 + SECTOR - 1) / SECTOR;
        if (need <= fatsz) break;
        fatsz = need;
    }

    uint32_t fat_start  = RESERVED;
    uint32_t data_start = RESERVED + NUM_FATS * fatsz;   /* cluster 2 lands here */

    img = calloc(TOTAL_SECTORS, SECTOR);
    if (!img) { perror("calloc"); return 1; }

    /* ---- boot sector / BPB ---- */
    uint8_t *b = img;
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;        /* jmp + nop */
    memcpy(b + 3, "OSDEV1.0", 8);                 /* OEM name */
    put16(b + 11, SECTOR);                        /* bytes per sector */
    b[13] = SPC;                                  /* sectors per cluster */
    put16(b + 14, RESERVED);                      /* reserved sectors */
    b[16] = NUM_FATS;                             /* number of FATs */
    put16(b + 17, 0);                             /* root entries (0 on FAT32) */
    put16(b + 19, 0);                             /* total16 (0 -> use total32) */
    b[21] = 0xF8;                                 /* media descriptor */
    put16(b + 22, 0);                             /* FAT size 16 (0 on FAT32) */
    put16(b + 24, 32);                            /* sectors per track */
    put16(b + 26, 2);                             /* heads */
    put32(b + 28, 0);                             /* hidden sectors */
    put32(b + 32, TOTAL_SECTORS);                 /* total sectors 32 */
    put32(b + 36, fatsz);                         /* FAT size 32 */
    put16(b + 40, 0);                             /* ext flags */
    put16(b + 42, 0);                             /* fs version */
    put32(b + 44, 2);                             /* root cluster */
    put16(b + 48, 1);                             /* FSInfo sector */
    put16(b + 50, 6);                             /* backup boot sector */
    b[64] = 0x80;                                 /* drive number */
    b[66] = 0x29;                                 /* extended boot signature */
    put32(b + 67, 0x12345678);                    /* volume id */
    memcpy(b + 71, "OSDEV VOL  ", 11);            /* volume label */
    memcpy(b + 82, "FAT32   ", 8);                /* fs type */
    put16(b + 510, 0xAA55);                       /* boot signature */

    /* ---- FAT + directory + file data ---- */
    uint32_t *fat = calloc(clusters + 2, sizeof(uint32_t));
    fat[0] = 0x0FFFFFF8;          /* media in low byte */
    fat[1] = 0x0FFFFFFF;
    /* root directory FAT chain is set once we know the file count (below) */

    /* Build a unified list of {name, bytes, len}: inline strings first, then any
     * host files read from disk (e.g. build/calc.elf). */
    struct { const char *name83; const uint8_t *data; uint32_t len; } ent[128];
    int ne = 0;
    for (int i = 0; i < NUM_FILES; i++)
        ent[ne++] = (typeof(ent[0])){ files[i].name83, (const uint8_t *)files[i].content,
                                      (uint32_t)strlen(files[i].content) };
    for (int i = 0; i < NUM_HOST && ne < 128; i++) {
        FILE *hf = fopen(hostfiles[i].hostpath, "rb");
        if (!hf) { fprintf(stderr, "mkfatfs: skip %s (not found)\n", hostfiles[i].hostpath); continue; }
        fseek(hf, 0, SEEK_END); long sz = ftell(hf); fseek(hf, 0, SEEK_SET);
        uint8_t *buf = malloc(sz > 0 ? (size_t)sz : 1);
        if (fread(buf, 1, (size_t)sz, hf) != (size_t)sz) { fprintf(stderr, "mkfatfs: read %s failed\n", hostfiles[i].hostpath); fclose(hf); free(buf); continue; }
        fclose(hf);
        ent[ne++] = (typeof(ent[0])){ hostfiles[i].name83, buf, (uint32_t)sz };
    }

    /* The root directory itself spans as many clusters as it needs (16 dir
     * entries per 512-byte cluster), chained in the FAT.  We size it one slot
     * past the last entry so a zeroed end-of-directory marker always exists. */
    uint32_t rootcl = (uint32_t)ne / DIR_PER_CLUSTER + 1;
    if (rootcl > clusters) {   /* highest root cluster (1+rootcl) must be <= clusters+1 */
        fprintf(stderr, "mkfatfs: %u files need a %u-cluster root, volume holds %u\n",
                (unsigned)ne, rootcl, clusters);
        return 1;
    }
    for (uint32_t c = 0; c < rootcl; c++)
        fat[2 + c] = (c == rootcl - 1) ? 0x0FFFFFFF : (2 + c + 1);

    /* Lay each file out as a chain of clusters (multi-cluster supported),
     * starting right after the root directory clusters. */
    uint32_t next_cluster = 2 + rootcl;
    for (int i = 0; i < ne; i++) {
        uint32_t len = ent[i].len;
        uint32_t need = len ? (len + CLUSTER_BYTES - 1) / CLUSTER_BYTES : 1;
        if (next_cluster + need - 1 > clusters + 1) {   /* would run past the last data cluster */
            fprintf(stderr, "mkfatfs: out of space writing %.11s (needs %u clusters)\n",
                    ent[i].name83, need);
            return 1;
        }
        uint32_t first = next_cluster;
        for (uint32_t c = 0; c < need; c++) {
            uint32_t cl = next_cluster++;
            fat[cl] = (c == need - 1) ? 0x0FFFFFFF : cl + 1;   /* chain, EOC on last */
            uint8_t *data = img + ((uint64_t)data_start + (cl - 2) * SPC) * SECTOR;
            uint32_t off = c * CLUSTER_BYTES, n = len - off;
            if (n > CLUSTER_BYTES) n = CLUSTER_BYTES;
            memcpy(data, ent[i].data + off, n);
        }
        /* directory entry, placed in the right root-directory cluster */
        uint8_t *de = img + ((uint64_t)data_start + (uint32_t)(i / DIR_PER_CLUSTER) * SPC) * SECTOR
                          + (i % DIR_PER_CLUSTER) * 32;
        memcpy(de, ent[i].name83, 11);
        de[11] = 0x20;                            /* attribute: archive */
        put16(de + 20, (uint16_t)(first >> 16));     /* first cluster high */
        put16(de + 26, (uint16_t)(first & 0xFFFF));  /* first cluster low */
        put32(de + 28, len);                      /* file size */
    }

    /* write both FAT copies (little-endian 32-bit entries) */
    for (int f = 0; f < NUM_FATS; f++) {
        uint8_t *dst = img + (uint64_t)(fat_start + f * fatsz) * SECTOR;
        for (uint32_t i = 0; i < clusters + 2; i++)
            put32(dst + i * 4, fat[i]);
    }

    FILE *out = fopen(argv[1], "wb");
    if (!out) { perror("fopen"); return 1; }
    fwrite(img, SECTOR, TOTAL_SECTORS, out);
    fclose(out);

    printf("mkfatfs: wrote %s (%d sectors, fatsz=%u, %d files)\n",
           argv[1], TOTAL_SECTORS, fatsz, ne);
    return 0;
}
