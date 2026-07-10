// M1794: Number.prototype.toString(radix) now emits the FRACTIONAL part for a
// non-decimal radix ((3.5).toString(16) === "3.8"), not just the integer part.
// Own fresh arena. A revert drops the fractional digits (A/B/C -> "3"/"101"/"0").
console.log("A", (3.5).toString(16));    // 3.8
console.log("B", (5.5).toString(2));     // 101.1
console.log("C", (0.5).toString(2));     // 0.1
console.log("D", (255.5).toString(16));  // ff.8
console.log("E", (-3.5).toString(2));    // -11.1
console.log("F", (0.25).toString(4));    // 0.1
// regressions: integer radix + base-10 unaffected
console.log("G", (255).toString(16));    // ff
console.log("H", (10).toString(2));      // 1010
console.log("I", (3.14).toString());     // 3.14
console.log("J", (255).toString());      // 255
console.log("-- done --");
