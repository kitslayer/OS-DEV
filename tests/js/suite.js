// OS-DEV JavaScript engine regression suite (run on host via tests/run-js-tests.sh,
// or `js suite.js` in the OS). Each line prints a result; suite.expected is the
// golden output. Covers M144-M157.
print("-- core --");
print(2 + 3 * 4, "a"+"b", (1<2?"y":"n"), typeof 5, typeof "s", typeof undefined);
var x=5; x+=3; print(x); print(10%3, 1<<4, 7>2, 2>=2, "p"=="p");
print("-- functions / closures / recursion --");
function fib(n){ return n<2?n:fib(n-1)+fib(n-2); } print(fib(12));
function adder(a){ return function(b){ return a+b; }; } print(adder(10)(5));
var sq = n => n*n; print(sq(9)); print(((a,b)=>a+b)(3,4));
function dflt(a, b=a+1){ return a+","+b; } print(dflt(10), dflt(10,99));
print("-- arrays --");
var ar=[3,1,2]; ar.push(4); print(ar.length, ar.join("-"));
print([1,2,3,4].map(n=>n*n).filter(n=>n>4).join(","));
print([1,2,3,4].reduce((a,b)=>a+b,0), [5,12,3].find(n=>n>10), [2,4,6].every(n=>n%2==0));
print([1,2,3].reverse().join(","), [1,2,3,4,5].slice(1,3).join(","));
print(Array.isArray([1]), Array.from("abc").join("."));
print([1,2,3].includes(2), [1,2].concat([3,4],5).join(","));
print([3,1,2].sort().join(","), [10,9,2].sort((a,b)=>a-b).join(","), ["pear","fig","kiwi"].sort().join(","));
print("-- strings --");
print("Hello".toUpperCase(), "WORLD".toLowerCase(), "  hi  ".trim()+"!");
print("a,b,c".split(",").length, "ab".repeat(3), "hello world".replace("world","JS"));
print("hello".includes("ell"), "hi".padStart(4,"."), "x".charCodeAt(0), "abcabc".lastIndexOf("bc"));
print("-- objects --");
var o={a:1,b:2}; o.c=3; print(o.a+o.b+o.c, Object.keys(o).join(","), Object.values(o).join(","));
var k=7,v=8; print(JSON.stringify({k,v}));
print("-- JSON --");
print(JSON.stringify({n:"OS",a:[1,2],ok:true}));
var p=JSON.parse('{"x":1,"y":[2,3],"s":"hi"}'); print(p.x, p.y[1], p.s);
print("-- template literals --");
var who="OS-DEV"; print(`hi ${who}, ${1+2} and ${[1,2,3].map(n=>n*2).join(",")}`);
print(`nested ${`in ${2*3}`}`);
print("-- control flow --");
function cls(n){ switch(n){ case 1: return "one"; case 2: case 3: return "two/three"; default: return "many"; } }
print(cls(1), cls(3), cls(9));
var s=0; for(var e of [10,20,30]) s+=e; print(s);
var d=0,i=0; do { d+=i; i++; } while(i<5); print(d);
print("-- exceptions --");
try { throw "boom"; } catch(e){ print("caught "+e); } finally { print("fin"); }
try { JSON.parse("{bad"); } catch(e){ print("json err caught"); }
function safe(){ try { return undefinedVar; } catch(e){ return "err:"+e; } } print(safe());
print("-- done --");
