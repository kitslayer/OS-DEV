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
print(parseInt("ff",16), parseInt("0x1A"), [0,0,0].fill(5).join(","), [1,2,1].lastIndexOf(1));
print([3,1,2].sort().join(","), [10,9,2].sort((a,b)=>a-b).join(","), ["pear","fig","kiwi"].sort().join(","));
print("-- strings --");
print("Hello".toUpperCase(), "WORLD".toLowerCase(), "  hi  ".trim()+"!");
print("a,b,c".split(",").length, "ab".repeat(3), "hello world".replace("world","JS"));
print("hello".includes("ell"), "hi".padStart(4,"."), "x".charCodeAt(0), "abcabc".lastIndexOf("bc"));
print("-- objects --");
var o={a:1,b:2}; o.c=3; print(o.a+o.b+o.c, Object.keys(o).join(","), Object.values(o).join(","));
var k=7,v=8; print(JSON.stringify({k,v}));
print(JSON.stringify(Object.assign({a:1},{b:2})), [[1,2],[3]].flat().join(","), Math.sign(-5));
print((255).toString(16), (255).toString(2), (-10).toString(16), (42).toString());
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
var ks=""; for(var k in {a:1,b:2,c:3}) ks+=k; print(ks);
var d=0,i=0; do { d+=i; i++; } while(i<5); print(d);
print("-- exceptions --");
try { throw "boom"; } catch(e){ print("caught "+e); } finally { print("fin"); }
try { JSON.parse("{bad"); } catch(e){ print("json err caught"); }
function safe(){ try { return undefinedVar; } catch(e){ return "err:"+e; } } print(safe());
print("-- this / new / methods --");
function Ctr(s){ this.n=s; this.inc=function(){ this.n++; return this.n; }; }
var ct=new Ctr(10); print(ct.inc(), ct.inc(), ct.n);
var om={ base:100, add(x){ return this.base+x; } }; print(om.add(5));
function Bx(v){ this.v=v; this.scale=function(a){ return a.map(x=> x*this.v); }; }
print(new Bx(3).scale([1,2,3]).join(","));
var oc={n:5}; oc.n++; var ac=[10,20]; ac[1]++; print(oc.n, ac[1]);
print("-- class --");
class Shape { constructor(n){ this.n=n; } kind(){ return this.n; } }
class Square extends Shape { area(s){ return s*s; } }
var sq=new Square("square"); print(sq.kind(), sq.area(5));
class Acc { constructor(){ this.t=0; } add(x){ this.t+=x; return this; } }
print(new Acc().add(3).add(4).t);
print("-- super --");
class Vh { constructor(w){ this.wheels=w; } kind(){ return "vehicle"; } }
class Car extends Vh { constructor(){ super(4); } kind(){ return super.kind()+":car"; } }
var car=new Car(); print(car.wheels, car.kind());
print("-- spread / rest --");
var sa=[2,3]; print([1,...sa,4].join(","), [..."ab"].join("."));
function ssum(...xs){ return xs.reduce((a,b)=>a+b,0); } print(ssum(...sa,5));
var so={x:1}; print(JSON.stringify({...so,y:2}));
print("-- destructuring --");
var [da,db,...dr]=[1,2,3,4]; print(da, db, dr.join(","));
var {dn, dx=9}={dn:"z"}; print(dn, dx);
var ds=0; for (var [dk,dv] of [[2,3],[4,5]]) ds+=dk*dv; print(ds);
print("-- param destructuring --");
function pd({a, b=5}){ return a+b; } print(pd({a:1}), pd({a:1,b:2}));
print([[1,2],[3,4]].map(([x,y]) => x*y).join(","));
var swap=([a,b])=>[b,a]; print(swap([7,9]).join(","));
print("-- assignment destructuring --");
var aa=1, ab=2; [aa,ab]=[ab,aa]; print(aa, ab);
var ax,ay; ({x:ax,y:ay}={x:8,y:9}); print(ax, ay);
var af, ar2; [af,...ar2]=[1,2,3]; print(af, ar2.join(","));
print("-- nullish / optional chaining --");
print(null ?? "d", 0 ?? "x", undefined ?? 7);
var u={a:{b:5}}; print(u?.a?.b, u?.a?.z, u?.x?.y);
var nn=null; print(nn?.p, nn?.[0], nn?.());
var ob={f(){return 9;}}; print(ob.f?.(), ob.g?.());
var nz=null; print(nz?.a.b.c(), ({u:{n:5}})?.u.n);
var ck="dyn"; print(JSON.stringify({[ck]:1,["a"+"b"]:2}));
print("-- Map / Set --");
var mm=new Map(); mm.set("a",1).set("b",2).set("a",9);
print(mm.size, mm.get("a"), mm.has("b"), mm.get("z"));
var ss=new Set(); [1,2,2,3,1].forEach(x=>ss.add(x));
print(ss.size, ss.has(2), ss.values().join(","));
var mt=0; var mk=new Map(); mk.set(1,10).set(2,20); for(var [k,v] of mk) mt+=k*v; print(mt);
print(Array.from(new Set([3,1,3,2,1])).join(","), new Set([1,1,2]).size, Array.from([1,2,3],x=>x*2).join(","), Array.of(9).length);
print(JSON.stringify({a:1,b:2},null,2).split("\n").length, JSON.stringify([1,2],null,1).indexOf("\n")>=0, JSON.stringify({a:1,b:2}));
print([1,[2,[3,[4]]]].flat(64).length, "  x  ".trimStart()+"|", "|"+"  y  ".trimEnd());
var dd=new Date(); print(typeof dd.getFullYear(), typeof dd.getHours(), (""+dd).length);
print(dd.nope, JSON.stringify(dd).length>2, Object.keys(dd).length, JSON.stringify(new Map().set("a",1)), JSON.stringify({...new Set([1,2])}));
print(encodeURIComponent("a b&c"), decodeURIComponent("x%20y"), encodeURI("p/q?x=1"));
print("a=1 b=2".matchAll(/(\w)=(\d)/g).map(m=>m[1]+m[2]).join(","), "x=9".matchAll(/(\w)=(\d)/g).length);
print("a1b2".replace(/\d/g, m=>m*2), "x-y".replace(/(\w)-(\w)/, (m,a,b)=>b+a));
print("-- more stdlib --");
print([10,20,30].at(-1), [1,2,3].flatMap(x=>[x,x]).join(","), [4,9,2,8].findLast(x=>x<5));
print("a.b.c".replaceAll(".","/"), "hi".at(-1));
print(JSON.stringify(Object.fromEntries([["k",1]])), JSON.stringify(Object.fromEntries(new Map().set("z",9))));
print("-- regex --");
print(new RegExp("\\d+").test("abc42"), new RegExp("z").test("abc"));
print(new RegExp("(\\d+)x(\\d+)").exec("a3x7b").join(","));
print("2024-01-15".replace(new RegExp("(\\d+)-(\\d+)-(\\d+)"), "$3/$2/$1"));
print("a1b2c3".match(new RegExp("\\d","g")).join(""), "a, b ,c".split(new RegExp("\\s*,\\s*")).join("|"));
print(/\d+/.test("x9"), "a1b2".replace(/\d/g,"#"), 20/4, [1,2,3].length/3);
print(/x/gi.flags, /y/.flags+"!", JSON.stringify(/z/g));
print("-- dom getAttribute --");
var gel = document.getElementById("box");
print(gel.getAttribute("href"), gel.getAttribute("data-id"), document.querySelector("#q").getAttribute("type"));
print("-- done --");
