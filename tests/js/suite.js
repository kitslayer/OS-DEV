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
var _cbc=[3,1,2];   // ES2023 change-array-by-copy (originals must stay intact)
print(_cbc.with(1,9).join(","), _cbc.toReversed().join(","), _cbc.toSorted().join(","), _cbc.toSorted((a,b)=>b-a).join(","), _cbc.join(","));
print([1,2,3,4].toSpliced(1,2,9).join(","), [1,2,3].toSpliced(1,0,8,9).join(","), [1,2,3].with(-1,7).join(","));
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
print(new Map() instanceof Map, new Error("x") instanceof Error, new Date() instanceof Date, ({}) instanceof Error);
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
print(Number.isInteger(5), Number.isNaN(1), Number.MAX_SAFE_INTEGER, String.fromCharCode(72,105), Number("17"), typeof Number);
print([...new Set([3,1,2,1,3])].join(","), Number.isSafeInteger(5), Number.isSafeInteger("x"));
print("hello".slice(-3), "ababab".indexOf("ab",1), [1,2,1,3].indexOf(1,1), [1,2,3].includes(1,1), "hello".substring(-3));
print("a,b,c,d".split(",",2).join("|"), "a,b,c".split(",").length);
print("hello".substring(3,1), "[" + "hello".slice(3,1) + "]");
var _er=new Error("oops"); print(_er.message, _er.name); try { throw new TypeError("t"); } catch(_x) { print(_x.name+":"+_x.message); }
var _fo={a:1}; Object.freeze(_fo); _fo.a=9; _fo.b=2; delete _fo.a; print(_fo.a, _fo.b, Object.isFrozen(_fo));
var _fn={a:1}; _fn.a=9; _fn.c=3; print(_fn.a, _fn.c, Object.isFrozen(_fn));
var _oi={}; print(Object.is(1,1), Object.is(1,2), Object.is("a","a"), Object.is(true,1), Object.is(_oi,_oi), Object.is({},{}), Object.is(null,null), Object.is(null,undefined));
var _dt=new Date(); print(_dt.valueOf()===_dt.getTime(), _dt.getTime()%1000, _dt.getDay()>=0&&_dt.getDay()<=6, _dt.getMilliseconds());
print(typeof Date.now, Date.now() > 0, Date.now() % 1000, Date.now() === new Date().getTime());
var _ds=new Date(); _ds.setFullYear(2030); _ds.setMonth(5); _ds.setDate(20); _ds.setHours(3); print(_ds.getFullYear(), _ds.getMonth(), _ds.getDate(), _ds.getHours());
var _la=0; _la||=5; var _lb=3; _lb||=9; var _lc=null; _lc??=7; var _ld=0; _ld??=8;
var _cnt=0; function _sf(){_cnt++;return 1;} var _lg=5; _lg||=_sf(); var _lh=1; _lh&&=8;
print(_la,_lb,_lc,_ld,_lg,_cnt,_lh);
function _argsum(){ var t=0; for(var i=0;i<arguments.length;i++) t+=arguments[i]; return t; } function _add(a,b){ return a+b; } print(_argsum(1,2,3,4), _argsum(), _add(2,3));
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
function _ttag(s,a,b){ return s.join("|")+":"+a+","+b; } print(_ttag`x${1}y${2}z`, _ttag`no-interp`);
print("-- location --");
print(location.protocol, location.host, location.pathname, location.search);
print(window.location.href);
print("-- accessors (getters/setters) --");
var _ac={ _x:10, get x(){ return this._x; }, set x(v){ this._x=v*2; } };
print(_ac.x); _ac.x=5; print(_ac.x, _ac._x); _ac["x"]=50; print(_ac["x"]);   // 10 / 10 10 / 100
var _go={ get v(){ return 7; } }; _go.v=999; print(_go.v);                     // 7 (getter-only: write ignored)
var _so={ set w(v){ this._w=v; } }; _so.w=3; print(_so._w, typeof _so.w);      // 3 undefined
var _gk={ get:1, set:2, get q(){ return 8; } }; print(_gk.get, _gk.set, _gk.q);// 1 2 8 (get/set as plain keys + an accessor)
print("y" in {get y(){return 1;}}, JSON.stringify({get z(){return 9;}}));      // true {} (in doesn't fire; JSON limitation)
class _Rc { constructor(){ this.w=3; this.h=4; } get area(){ return this.w*this.h; } set area(a){ this.w=a; } }
var _r=new _Rc(); print(_r.area); _r.area=10; print(_r.area, _r.w);            // 12 / 40 10
class _Ba { get nm(){ return "base"; } } class _Su extends _Ba { get nm(){ return "sub"; } }
print(new _Ba().nm, new _Su().nm, new (class extends _Ba {})().nm);            // base sub base
var _up={_n:5, get n(){return this._n;}, set n(v){this._n=v;}}; print(_up.n++, _up.n, ++_up.n); var _gu={get n(){return 7;}}; print(_gu.n++, _gu.n);  // 5 6 7 / 7 7 (++ routes thru get/set, doesn't corrupt the accessor)
var _dp={_v:1}; Object.defineProperty(_dp,"x",{get:function(){return this._v*10;},set:function(v){this._v=v;}}); _dp.x=5; Object.defineProperty(_dp,"d",{value:8});
print(_dp.x, _dp.d, Object.getOwnPropertyDescriptor({a:7},"a").value, typeof Object.getOwnPropertyDescriptor(_dp,"x").get, Object.getOwnPropertyDescriptor({},"z"));  // 50 8 7 function undefined
print("-- prototype chain --");
var _pc=Object.create({greet(){return "hi "+this.nm;}}); _pc.nm="Ada"; print(_pc.greet());                       // hi Ada
function _PF(){this.a=1;} _PF.prototype.m=function(){return this.a+1;}; var _pf=new _PF(); print(_pf.m(), _pf.a); // 2 1
var _pb={k:"base"}; var _pd=Object.create(_pb); print(_pd.k); _pd.k="own"; print(_pd.k, _pb.k);                   // base / own base
var _px={}; print(_px.__proto__); _px.__proto__={v:7}; print(_px.v, Object.getPrototypeOf(_px).v);                // null / 7 7
var _pa={get full(){return this.f+this.l;}}; var _pi=Object.create(_pa); _pi.f="X"; _pi.l="Y"; print(_pi.full);  // XY
var _c1={}, _c2={}; _c1.__proto__=_c2; _c2.__proto__=_c1; print(_c1.zzz);                                         // undefined (cycle, no hang)
print("k" in Object.create({k:1}), "z" in {a:1}, Object.create(_PF.prototype) instanceof _PF, new _PF() instanceof _PF);  // true false true true (in + instanceof walk the chain, M264)
var _dps={}; Object.defineProperties(_dps,{a:{value:1},b:{get:function(){return 8;}}}); var _crd=Object.create({},{c:{value:5}}); print(_dps.a, _dps.b, _crd.c);  // 1 8 5 (defineProperties + Object.create 2nd arg, M265)
var _e1={}, _e2={}; print(_e1===_e2, _e1===_e1, [1]===[1], _e1!==_e2);  // false true false true (object identity in ===/!==, was always-equal)
var _scs={v:1}; var _sco={p:_scs,q:_scs,a:[1,2]}; var _scc=structuredClone(_sco); _scc.a[0]=9; _scc.p.v=5;
print(_sco.a[0], _scc.a[0], _sco.p.v, _scc.p.v, _scc.p===_scc.q, _sco.p===_scc.p);  // 1 9 1 5 true false (deep clone: original intact, shared ref preserved, M266)
print("b">"a", "apple"<"banana", "2">"10", "5"<10, 2<3, 5>10);  // true true true true true false (string lexical + numeric relational, M267)
var _al=[1,2,3,4,5]; _al.length=2; var _ag=[1,2]; _ag.length=4; print(_al.join(","), _al.length, _ag.length, _ag[3]);  // 1,2 2 4 undefined (array .length assignment, M267)
print(new Array(3).length, Array(1,2,3).join(","), Array.isArray([1]), Array.from({length:2}).length, typeof Array);  // 3 1,2,3 true 2 function (Array() constructor + statics still resolve, M268)
print(-1>>>28, 16>>>2, -8>>1, -8>>>1, 8>>>1+1);  // 15 4 -4 2147483644 2 (>>> unsigned right shift, M269)
var _u=-1; _u>>>=0; var _ss=256; _ss>>>=4; var _sl=3; _sl<<=2; print(_u, _ss, _sl);  // 4294967295 16 12 (>>>= compound assignment, M270)
var _xu; print(null==undefined, null===undefined, "5"==5, "5"===5, 1==true, 1===true, _xu==null, {}=={}, [1]==1);  // true false true false true false true false true (loose == vs strict ===, M271)
print({}==[], []==[], [1]==1, [1,2]=="1,2");  // false false true true (distinct objects by identity; object->primitive coercion; M271 review fix)
print([1,2,3,4,5].copyWithin(0,3).join(","), [1,2,3,4,5].copyWithin(1,3,4).join(","));  // 4,5,3,4,5 1,4,3,4,5 (copyWithin in place, M272)
var _wm=new WeakMap(),_wk={}; _wm.set(_wk,7); var _ws=new WeakSet(),_wo={}; _ws.add(_wo); print(_wm.get(_wk), _wm.has(_wk), _ws.has(_wo), _wm instanceof WeakMap);  // 7 true true true (WeakMap/WeakSet, M273)
var _ho={a:1}; var _hi=Object.create({p:9}); _hi.own=1; print(_ho.hasOwnProperty("a"), _ho.hasOwnProperty("z"), Object.hasOwn(_ho,"a"), _hi.hasOwnProperty("own"), _hi.hasOwnProperty("p"), [1,2].hasOwnProperty(0));  // true false true true false true (hasOwnProperty own-only + Object.hasOwn, M274)
print(({}).toString(), [1,2,3].toString(), "s".toString(), (9).toString(2), typeof (new Date()).valueOf());  // [object Object] 1,2,3 s 1001 number (toString/valueOf method calls; number radix + Date.valueOf unaffected, M275)
var _es="";for(var [_i,_x] of ["p","q"].entries())_es+=_i+_x; print("hello".substr(1,3), ["a","b","c"].keys().join(","), [10,20].values().join(","), _es);  // ell 0,1,2 10,20 0p1q (substr + array iterators, M276)
var _rf={a:1}; Reflect.set(_rf,"b",2); print(Reflect.get(_rf,"a"), Reflect.has(_rf,"b"), Reflect.ownKeys(_rf).join(","), Reflect.deleteProperty(_rf,"a"), Reflect.has(_rf,"a"));  // 1 true a,b true false (Reflect, M277)
print((1234567).toLocaleString(), "a".localeCompare("b"), "abc".codePointAt(0), String.fromCodePoint(65,66));  // 1,234,567 -1 97 AB (toLocaleString grouping + localeCompare + codePointAt/fromCodePoint, M278)
var _pe={a:1}; var _gd=Object.getOwnPropertyDescriptors({x:5}); print(_pe.propertyIsEnumerable("a"), _pe.propertyIsEnumerable("z"), [1,2,3].toLocaleString(), _gd.x.value);  // true false 1,2,3 5 (propertyIsEnumerable + array toLocaleString + getOwnPropertyDescriptors, M279)
print("-- labeled break/continue --");
var _lr=""; _o1: for(var _i=0;_i<3;_i++){ for(var _j=0;_j<3;_j++){ if(_j==1) continue _o1; if(_i==2) break _o1; _lr+=_i+""+_j+" "; } } print(_lr.trim());  // 00 10
var _pp=0; _o2: for(var _q=0;_q<5;_q++){ if(_q==2) break _o2; _pp++; } print(_pp);  // 2
var _sv=""; _o3: for(var _s=0;_s<4;_s++){ switch(_s){ case 1: _sv+="a"; break; case 2: break _o3; default: _sv+="d"; } _sv+=_s; } print(_sv);  // d0a1
var _ub=0; for(var _u=0;_u<10;_u++){ if(_u==3) break; _ub++; } print(_ub, true?7:9);  // 3 7 (unlabeled break unchanged + ?: not a label)
print("-- querySelector(All) --");
print(document.querySelector("p").textContent, document.querySelector("nope"), document.querySelector("b.x").textContent);  // alpha null alpha (tag match; no-match->null; compound tag.class->position handle; M281, host mock)
var _qa=document.querySelectorAll(".item"); var _qc=0; _qa.forEach(function(e){_qc++;}); var _qf=""; for(var _qe of _qa)_qf+=_qe.textContent; print(_qa.length, _qa[0].textContent, _qa[1].textContent, _qc, _qf, document.querySelectorAll("zzz").length);  // 2 alpha beta 2 alphabeta 0 (querySelectorAll -> array of position handles: length/index/forEach/for-of all work; empty match->len 0; M281)
print(document.querySelector("[data-x]").textContent, document.querySelector("[nope]"));  // alpha null (attribute-presence selector [attr] + tag[attr] compounds; M284, host mock)
print(document.querySelector("p").matches("p"), document.querySelector("p").matches("zzz"), document.querySelectorAll(".item")[1].matches(".item"));  // true false true (element.matches: membership in the selector's matches; M294)
print("-- getElementsBy* + getAttribute --");
print(document.getElementsByTagName("p").length, document.getElementsByClassName("item").length, document.getElementsByClassName("zzz").length);  // 2 2 0 (getElementsByTagName/ClassName -> arrays of position handles; M282, host mock)
print(document.querySelector("p").getAttribute("href"), document.getElementsByClassName("item")[1].getAttribute("data"));  // href@1 data@2 (getAttribute on a position handle; mock echoes attr@offset; M282)
print("-- position write --");
document.querySelector("p").textContent = "W1"; print(document.querySelector("p").textContent);  // W1 (textContent write on a position handle; read-back via mock store; M283)
document.querySelectorAll(".item")[1].textContent = "W2"; print(document.querySelectorAll(".item")[1].textContent, document.querySelectorAll(".item")[0].textContent);  // W2 W1 (indexed write; distinct offsets independent; M283)
print("-- classList --");
var _cl=document.querySelector("p");
print(_cl.classList.contains("x"), _cl.classList.toggle("x"), _cl.classList.contains("x"), _cl.classList.toggle("x"), _cl.classList.contains("x"));  // false true true false false (toggle add/remove + contains; M285)
_cl.classList.add("a"); _cl.classList.add("a"); _cl.classList.add("b"); print(document.querySelector("p").getAttribute("class"));  // a b (add dedups; reads back via class attr)
_cl.classList.remove("a"); print(document.querySelector("p").getAttribute("class"));  // b (remove)
print(_cl.classList.toggle("ff", true), _cl.classList.contains("ff"), _cl.classList.toggle("ff", false), _cl.classList.contains("ff"));  // true true false false (toggle(name, force) 2-arg form; M293)
var _he=document.querySelectorAll(".item")[1]; print(_he.hasAttribute("class")); _he.classList.add("z"); print(_he.hasAttribute("class"), _he.getAttribute("class"));  // false / true z (hasAttribute reflects the class attr; M286)
var _ra=document.querySelector("p"); _ra.classList.add("k"); var _h1=_ra.hasAttribute("class"); _ra.removeAttribute("class"); print(_h1, _ra.hasAttribute("class"));  // true false (removeAttribute drops the attr; M295)
print("-- done --");
