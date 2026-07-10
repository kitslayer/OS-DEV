// M1790: ToPrimitive(default hint) for the `+` and `+=` operators. An object with
// a numeric valueOf() must add numerically (not stringify); plain objects/arrays/
// Date still stringify; each operand's valueOf/toString fires exactly once. Runs in
// its own fresh arena (like suite-promise.js / timers.js) so it never touches the
// near-cap main suite.js. A revert of to_primitive() flips A/C back to the old
// "[object Object]..." output, failing this suite.
// --- FIX: object with numeric valueOf -> numeric add ---
console.log("A", ({valueOf(){return 5}}) + 1);          // 6
console.log("B", 1 + ({valueOf(){return 5}}));          // 6
console.log("C", ({valueOf(){return 5}}) + ({valueOf(){return 3}})); // 8
console.log("D", ({valueOf(){return 5}}) + "x");        // 5x  (rhs string -> concat ToString(5))
console.log("E", ({valueOf(){return "5"}}) + 1);        // 51  (valueOf string -> concat)
// --- REGRESSIONS: unchanged ---
console.log("F", ({}) + 1);                             // [object Object]1
console.log("G", [1,2] + [3,4]);                        // 1,23,4
console.log("H", [5] + 3);                              // 53
console.log("I", "a" + "b");                            // ab
console.log("J", 1 + 2);                                // 3
console.log("K", 1 + "2");                              // 12
console.log("L", true + 1);                             // 2
console.log("M", null + 1);                             // 1
console.log("N", undefined + 1);                        // NaN
console.log("O", 2 + 3 + "x");                          // 5x
console.log("P", "x" + 2 + 3);                          // x23
// --- Date: default hint = string (NOT epoch) ---
console.log("Q", (new Date(0) + "").length > 5);        // true
console.log("R", (new Date(0) + "") === "0");           // false
// --- += / compound assignment ---
var o={valueOf(){return 5}}; var x=1; x+=o; console.log("S", x);       // 6
var s="a"; s+={}; console.log("T", s);                                 // a[object Object]
var n=10; n-={valueOf(){return 3}}; console.log("U", n);               // 7
var m=2; m*={valueOf(){return 4}}; console.log("V", m);                // 8
// --- side-effect fires EXACTLY once ---
var c=0; var ob={valueOf(){c++; return 5}}; var y=1; y+=ob; console.log("W", y, c);   // 6 1
var c2=0; var o2={valueOf(){c2++; return 5}}; var z=10; z-=o2; console.log("X", z, c2); // 5 1
var c3=0; var o3={valueOf(){c3++; return 5}}; console.log("Y", (o3+1), c3);            // 6 1
console.log("-- done --");
