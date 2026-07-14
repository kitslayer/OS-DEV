// Regex lookbehind (?<=…) / (?<!…) — fixed-length body only (M1814). Runs in its
// own fresh arena (like toprim.js / timers.js) so it never touches suite.js's cap.
print("1", "$100".replace(/(?<=\$)\d+/, "X"));                        // $X
print("2", "a1b2c3".replace(/(?<=[a-z])\d/g, "#"));                   // a#b#c#
print("3", ("price is $50 not X90".match(/(?<=\$)\d+/) || ["?"])[0]); // 50
print("4", "cat scat".replace(/(?<!s)cat/g, "X"));                    // X scat
print("5", /(?<=\d)px/.test("10px"), /(?<=\d)px/.test("px"));         // true false
print("6", "ab5 cd7 xy9".match(/(?<=ab|cd)\d/g).join(","));           // 5,7 (alternation, both branches length 2)
print("7", ("foobar".match(/(?<=foo)bar/) || ["?"])[0]);             // bar
print("8", /(?<!foo)bar/.test("xxbar"), /(?<!foo)bar/.test("foobar")); // true false
print("9", ("abc".replace(/(?<=^a)bc/, "X")));                       // aX (^ anchor inside a fixed-length lookbehind)
print("A", "varlen", /(?<=a+)b/.test("aab"));                        // varlen false (variable-length lookbehind is unsupported -> compiles to a never-match)
print("-- done --");
