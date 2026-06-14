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
    { "LIST    HTM", "<h2>Lists</h2><p>An ordered list with a nested bullet list:</p><ol><li>First item<li>Second item<ul><li>nested bullet<li>another nested</ul><li>Third item</ol><p>And a plain bullet list:</p><ul><li>alpha<li>beta<li>gamma</ul>" },
    { "TABLE   HTM", "<h2>Table</h2><p>A small table renders as pipe-separated rows with bold headers:</p><table><tr><th>Name<th>Role<th>Year<tr><td>Alice<td>Engineer<td>2021<tr><td>Bob<td>Designer<td>2022<tr><td>Carol<td>Manager<td>2019</table><p>After the table.</p>" },
    { "ENT     HTM", "<h2>Entities</h2><p>Named: &lsquo;single&rsquo; and &ldquo;double&rdquo;, dash &mdash; here, ellipsis&hellip; bullet &bull; copy &copy;.</p><p>Numeric decimal: &#39;apos&#39; and &#8220;quote&#8221;.</p><p>Numeric hex: &#x27;hex-apos&#x27; and &#x2014; em-dash.</p>" },
    { "UTF8    HTM", "<h2>UTF-8</h2><p>Raw UTF-8 bytes (not entities): smart quotes “hello” and ‘hi’, em dash — here, ellipsis… bullet •.</p><p>Accents fold to ASCII: café, naïve, jalapeño, Über, straße.</p><p>Symbols: euro € and 30°C.</p><!-- a comment with a > and <b>markup</b> inside: none of THIS must render -->A<!--[if IE]><p>conditional</p><![endif]-->B<p>After comments.</p><svg width=\"12\" height=\"12\"><title>svgtitle-must-not-show</title><text x=\"0\" y=\"9\">SVGLEAK</text><path d=\"M0 0 L12 12\"/></svg><p>After svg.</p>" },
    { "IMG     HTM", "<h2>Images</h2><p>Local images render inline, decoded by our own PNG, GIF and JPEG code: <img src=\"file:test.png\" alt=\"the test image\"> and an icon <img src=\"file:icon.png\" alt=\"an icon\">. Here is a baseline JPEG photo, scaled with width=\"240\" (its natural size is 120): <img src=\"file:photo.jpg\" alt=\"a jpeg photo\" width=\"240\">. Remote images stay clickable links you follow to view full-size.</p>" },
    { "FORM    HTM", "<h1>Forms: type and process input</h1>"
        "<p>Fields are now editable: Tab/n to a field, Enter to focus it, type, Enter to finish. A button reads <code>.value</code> and does something with it.</p>"
        "<p>Your name: <input id=\"name\" placeholder=\"(click here, then type)\"></p>"
        "<p>Favourite number: <input id=\"num\" placeholder=\"a number\"></p>"
        "<p><button onclick=\"document.getElementById('out').textContent='Hello '+document.getElementById('name').value+'! Your number doubled is '+(parseInt(document.getElementById('num').value)*2)\">[ Greet &amp; compute ]</button></p>"
        "<p id=\"out\">(fill the fields, then click the button)</p>"
        "<p>The button reads each field's value via <code>getElementById(id).value</code>, computes, and writes the result into the paragraph &mdash; real input-&gt;process-&gt;output.</p>" },
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
        "<p>Toggling or blurring fires <code>onchange</code>, which reads the new <code>.value</code> and rewrites the message &mdash; reactive forms, from scratch.</p>" },
    { "COLOR   HTM", "<h2>Colours</h2><p>Text can be <font color=\"red\">red</font>, <font color=\"green\">green</font>, <font color=\"blue\">blue</font>, <font color=\"#E07000\">hex orange</font>, and <font color=\"purple\">purple</font>. Back to normal.</p>" },
    { "INDEX   HTM", "<h1>OS-DEV Demo Index</h1><p>Local demo pages baked onto the FAT32 disk &mdash; select with Tab/n and press Enter:</p><ul><li><a href=\"file:pre.htm\">Preformatted text</a><li><a href=\"file:list.htm\">Lists (nested &amp; ordered)</a><li><a href=\"file:nested.htm\">Deeply nested lists</a><li><a href=\"file:table.htm\">Tables</a><li><a href=\"file:ent.htm\">HTML entities</a><li><a href=\"file:img.htm\">Images</a><li><a href=\"file:color.htm\">Coloured text</a><li><a href=\"file:code.htm\">Inline code</a></ul><p><b>Interactive</b> (JavaScript + DOM):</p><ul><li><a href=\"file:dom.htm\">Interactive DOM &mdash; click to rewrite the page</a><li><a href=\"file:form.htm\">Editable forms &mdash; type, then process input</a><li><a href=\"file:search.htm\">Web search &mdash; HTTPS form submit to DuckDuckGo</a><li><a href=\"file:login.htm\">Password masking &mdash; * on screen, real value to JS</a><li><a href=\"file:attr.htm\">get/setAttribute &mdash; JS reads &amp; writes HTML attributes</a><li><a href=\"file:loc.htm\">window.location &mdash; JS reads the page URL</a><li><a href=\"file:check.htm\">Checkboxes &amp; radios &mdash; toggle, read via .value</a><li><a href=\"file:onchg.htm\">onchange &mdash; a handler runs the moment a box toggles</a><li><a href=\"file:jstest.htm\">Page script (document.write)</a><li><a href=\"file:oop.htm\">Object-oriented page script</a></ul><p>Backspace returns here. Press <b>h</b> for the start page.</p>" },
    { "CODE    HTM", "<h2>Inline code</h2><p>Run the <code>browse</code> command, then press <kbd>Enter</kbd> to follow a link. The call <code>memcpy(dst, src, n)</code> copies <samp>n</samp> bytes; configuration lives in <tt>/etc/config</tt>. Inline code renders in a distinct colour so it stands out from <b>bold</b> and <i>italic</i> text.</p><p>Back to normal flow.</p>" },
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
    struct { const char *name83; const uint8_t *data; uint32_t len; } ent[64];
    int ne = 0;
    for (int i = 0; i < NUM_FILES; i++)
        ent[ne++] = (typeof(ent[0])){ files[i].name83, (const uint8_t *)files[i].content,
                                      (uint32_t)strlen(files[i].content) };
    for (int i = 0; i < NUM_HOST && ne < 64; i++) {
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
