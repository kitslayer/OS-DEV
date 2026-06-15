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
print(gel.setAttribute("data-id", "99"), typeof gel.setAttribute);   /* call returns undefined; member-access is undefined (not a real prop) */
print("-- array mutators --");
var sp=[1,2,3,4,5]; var rm=sp.splice(1,2,"a","b","c"); print(rm.join(","), sp.join(","));
var q=[2,3]; q.unshift(0,1); print(q.shift(), q.join(","));
print(["a","b","c"].reduceRight(function(a,x){return a+x;}), [1,2,3].reduceRight(function(a,b){return a+b;}, 10));
print("-- call/apply --");
function _ad(a,b){return a+b;} function _gr(g){return g+" "+this.name;}
print(_ad.call(null,5,6), _ad.apply(null,[3,4]), _gr.call({name:"Ada"},"Hi"), Math.max.apply(null,[2,9,4]));
function _bf(a,b){return this.x+a+b;}
print(_bf.bind({x:10},1)(2), _bf.bind({x:100})(5,6), typeof _bf.bind({x:1}));
print("-- delete --");
var _do={a:1,b:2,c:3}; print(delete _do.b, _do.a+","+_do.c, _do.b);
print(Object.keys(_do).join(","));
var _dp={x:10,y:20}; delete _dp["x"]; print(_dp.x, _dp.y);
var _da=[1,2,3]; print(delete _da[1], _da[0]+"/"+_da[1]+"/"+_da[2]);
print(delete _do.zzz, delete 5);
var _dm=new Map(); _dm.set("k",9); print(_dm.delete("k"), _dm.has("k"));
print("-- in --");
var _io={a:1,b:0}; print("a" in _io, "b" in _io, "z" in _io);
var _ia=[5,6,7]; print(0 in _ia, 2 in _ia, 9 in _ia);
var _ik=""; for (var _k in _io) _ik+=_k; print(_ik);
var _is=0; for (var _v of _ia) _is+=_v; print(_is);
print(("a" in _io) ? "Y" : "N");
print("-- bitwise --");
print(5 ^ 3, ~5, 12 ^ 10, 1 | 2 ^ 3 & 3, ~1 ^ 1, ~~7);
print("-- instanceof --");
class _An{constructor(n){this.n=n;}} class _Dg extends _An{bark(){return "w";}}
var _d=new _Dg("R");
print(_d instanceof _Dg, _d instanceof _An, _d instanceof Map, ({}) instanceof _An, _d.bark());
print("-- pow/void --");
print(2 ** 10, 2 ** 3 ** 2, 2 * 3 ** 2, void 0, typeof void 1);
var _pm=3; _pm*=4; print(2*5, _pm);   /* regression: * and *= unaffected by adding ** */
print("-- cmp-assign --");
var _ca=12; _ca&=10; var _cb=12; _cb|=3; var _cc=12; _cc^=10;
var _cd=1; _cd<<=4; var _ce=256; _ce>>=3; var _cf=2; _cf**=10;
print(_ca,_cb,_cc,_cd,_ce,_cf);
var _cs="a"; _cs+="b"; var _co={n:8}; _co.n|=1; print(_cs,_co.n);
print("-- num-literals --");
print(0b1010, 0o17, 1e3, 0xFF, 5E2, 0b11111111);
print(0b1010 + 0o17 + 0xF, 3.7, 1e-3);
print(1_000_000, 0xFF_FF, 0b1010_1010);
print("-- stdlib --");
print([1,2,3,4].fill(0).join(","), [1,2,3,4].fill(9,1,3).join(","));
print(Math.hypot(3,4), Math.hypot(5,12), Math.log2(1024), Math.sqrt(16));
print(Array.from({length:4}, function(_,i){return i;}).join(","));
print(Object.getOwnPropertyNames({x:1,y:2}).join(","));
print(Math.cbrt(27), Math.cbrt(-64), Math.clz32(1), Math.imul(0xFFFFFFFF,5));
var _fo={a:1}; Object.freeze(_fo); _fo.a=9; _fo.b=2; delete _fo.a; print(_fo.a, _fo.b, Object.isFrozen(_fo));
var _fn={a:1}; _fn.a=9; _fn.c=3; print(_fn.a, _fn.c, Object.isFrozen(_fn));
var _dt=new Date(); print(_dt.valueOf()===_dt.getTime(), _dt.getTime()%1000, _dt.getDay()>=0&&_dt.getDay()<=6, _dt.getMilliseconds());
var _la=0; _la||=5; var _lb=3; _lb||=9; var _lc=null; _lc??=7; var _ld=0; _ld??=8;
var _cnt=0; function _sf(){_cnt++;return 1;} var _lg=5; _lg||=_sf(); var _lh=1; _lh&&=8;
print(_la,_lb,_lc,_ld,_lg,_cnt,_lh);
print("-- class-fields --");
class _Cf { n = 5; m = 2*3; inc(){ this.n++; return this.n; } }
var _cf = new _Cf(); print(_cf.n, _cf.m, _cf.inc());
class _Ca { a = 1; } class _Cb extends _Ca { b = 2; }
var _cb = new _Cb(); print(_cb.a, _cb.b);
class _Sm { static sq(x){ return x*x; } static V = 7; m(){ return this.V || 1; } static both(){ return _Sm.sq(2) + _Sm.V; } }
print(_Sm.sq(6), _Sm.V, new _Sm().m(), _Sm.both());
class _Rg { static n = 0; static inc(){ this.n += 1; return this.n; } static d = _Rg.n + 100; }
print(_Rg.inc(), _Rg.inc(), _Rg.n, _Rg.d);
class _Bse { static k = "b"; static who(){ return this.k; } } class _Der extends _Bse { static k = "d"; } class _Pln extends _Bse {}
print(_Der.who(), _Pln.who(), _Pln.k);
print(typeof _undeclared_xyz, typeof _undeclared_xyz === "undefined", typeof print);
var _ck="go"; var _cmo={ [_ck](){return 7;}, speed:3, [_ck+"f"](){return this.speed;} }; print(_cmo.go(), _cmo.gof());
print("-- location --");
print(location.protocol, location.host, location.pathname, location.search);
print(window.location.href);
print("-- done --");
