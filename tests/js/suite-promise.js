// SECOND golden run, kept separate from suite.js because that one shares a single 40MB
// arena run-to-completion with only ~350KB headroom (its adversarial cap cases fill it),
// so appending there OOMs an unrelated case mid-run. This file gets its OWN fresh arena
// (js_run_doc resets g_arena_off), so newer/heavier features land here: Promises +
// async/await/fetch (M679-M687), and other overflow golden. Ends with "-- done --" so the
// harness can detect a truncated (arena-OOM) run.
print("-- Promise: construct + then/catch/finally --");
print((function(){var o="?";new Promise(function(res){res(42);}).then(function(v){o=v;});return o;})());  // 42
print((function(){var o="?";Promise.resolve(10).then(function(v){return v*2;}).then(function(v){o=v;});return o;})());  // 20 (then-chain transform)
print((function(){var o="?";new Promise(function(r,j){j("boom");}).catch(function(e){o="caught:"+e;});return o;})());  // caught:boom
print((function(){var o="?";new Promise(function(){throw "oops";}).catch(function(e){o=e;});return o;})());  // oops (executor throw -> rejection)
print((function(){var log=[];Promise.resolve(1).then(function(v){log.push("a"+v);return v+1;}).catch(function(){log.push("C");}).then(function(v){log.push("b"+v);});return log.join(",");})());  // a1,b2 (catch skipped on success)
print((function(){var o="?";Promise.reject("E").then(function(){o="F";}).catch(function(e){o="got:"+e;});return o;})());  // got:E (rejection propagates through then with no onRejected)
print((function(){var log=[];Promise.resolve(5).finally(function(){log.push("fin");}).then(function(v){log.push("v"+v);});return log.join(",");})());  // fin,v5 (finally runs, value passes through)
print("-- Promise: statics --");
print((function(){var o="?";Promise.all([Promise.resolve(1),2,Promise.resolve(3)]).then(function(a){o=a.join(",");});return o;})());  // 1,2,3 (non-promise members pass through)
print((function(){var o="?";Promise.all([Promise.resolve(1),Promise.reject("X")]).then(function(){o="F";}).catch(function(e){o="rej:"+e;});return o;})());  // rej:X (first rejection short-circuits)
print((function(){var o="?";Promise.race([Promise.resolve("first"),Promise.resolve("second")]).then(function(v){o=v;});return o;})());  // first
print((function(){var o="?";Promise.allSettled([Promise.resolve(1),Promise.reject("e")]).then(function(a){o=a.map(function(r){return r.status;}).join(",");});return o;})());  // fulfilled,rejected
print("-- Promise: identity + flatten + captured resolver --");
print((Promise.resolve(1) instanceof Promise)+" "+(new Promise(function(r){r(1);}) instanceof Promise));  // true true
print(typeof Promise);  // function
print((function(){var o="?";Promise.resolve(1).then(function(v){return Promise.resolve(v+10);}).then(function(v){o=v;});return o;})());  // 11 (a returned promise is adopted/flattened)
print((function(){var r;new Promise(function(res){r=res;});var o="?";r(7);return "resolver-callable-after-executor";})());  // resolver-callable-after-executor (the resolver carries its promise as a bound arg)
print("-- async / await (M680) --");
print((function(){async function f(){return 5;}var o="?";f().then(function(v){o=v;});return o;})());  // 5 (async fn returns a promise)
print((function(){async function g(){var x=await Promise.resolve(10);return x*2;}var o="?";g().then(function(v){o=v;});return o;})());  // 20 (await unwraps)
print((function(){async function h(){throw "err";}var o="?";h().catch(function(e){o=e;});return o;})());  // err (async throw -> rejection)
print((function(){async function f(){try{await Promise.reject("X");return "no";}catch(e){return "handled:"+e;}}var o="?";f().then(function(v){o=v;});return o;})());  // handled:X (await of a rejection throws, caught by try/catch)
print((function(){async function f(){var a=await Promise.resolve(1);var b=await Promise.resolve(a+1);return a+b;}var o="?";f().then(function(v){o=v;});return o;})());  // 3 (sequential awaits)
print((function(){async function f(){return await Promise.resolve(10)+5;}var o="?";f().then(function(v){o=v;});return o;})());  // 15 (await binds tighter than +)
print((function(){var async=5;var await=3;return async+await;})());  // 8 (async/await remain ordinary identifiers outside an async body)
print("-- async arrows (M681) --");
print((function(){var f=async()=>42;var o="?";f().then(function(v){o=v;});return o;})());  // 42 (async () => expr)
print((function(){var f=async x=>await Promise.resolve(x*2);var o="?";f(10).then(function(v){o=v;});return o;})());  // 20 (async x => await …)
print((function(){var f=async(a,b)=>await Promise.resolve(a+b);var o="?";f(3,4).then(function(v){o=v;});return o;})());  // 7 (async (a,b) => …)
print((function(){function async(x){return x*3;}return async(5);})());  // 15 (a function named `async` is still callable — `async(5)` is NOT mis-parsed as an arrow)
print("-- async methods (M682) --");
print((function(){class C{async m(){return 5;}}var o="?";new C().m().then(function(v){o=v;});return o;})());  // 5 (class async method)
print((function(){class C{constructor(){this.n=7;}async get(){return await Promise.resolve(this.n);}}var o="?";new C().get().then(function(v){o=v;});return o;})());  // 7 (async method: await + correct this)
print((function(){var obj={async go(){return await Promise.resolve(42);}};var o="?";obj.go().then(function(v){o=v;});return o;})());  // 42 (object async method)
print((function(){class C{async m(){throw "e";}}var o="?";new C().m().catch(function(e){o=e;});return o;})());  // e (async method throw -> rejection)
print((function(){var obj={async(){return 3;}};return obj.async();})());  // 3 (a method NAMED `async` is NOT an async method)
print("-- fetch(url) -> Promise<Response> (M684; host mock) --");
print((function(){var o="?";fetch("http://x/").then(function(r){return r.text();}).then(function(t){o=t;});return o;})());  // hello from fetch (fetch -> .text())
print((function(){var o="?";fetch("http://x/404").then(function(r){o=r.status+","+r.ok;}).catch(function(){o="REJ";});return o;})());  // 404,false (an HTTP error status RESOLVES with ok=false; it does not reject)
print((function(){var o="?";fetch("http://x/json").then(function(r){return r.json();}).then(function(j){o=j.a+","+j.b.join("-");});return o;})());  // 1,2-3 (Response.json())
print((function(){async function f(){var r=await fetch("http://x/");return await r.text();}var o="?";f().then(function(t){o=t;});return o;})());  // hello from fetch (await fetch + await r.text())
print((function(){var o="?";fetch("http://x/fail").then(function(){o="F";}).catch(function(){o="caught";});return o;})());  // caught (a network failure rejects)
print("-- fetch POST + options (M703; mock echoes POST <ctype>:<body>) --");
print((function(){var o="?";fetch("http://x/api",{method:"POST",body:"hi"}).then(function(r){return r.text();}).then(function(t){o=t;});return o;})(), (function(){var o="?";fetch("http://x/api",{method:"POST",body:'{"a":1}',headers:{"Content-Type":"application/json"}}).then(function(r){return r.text();}).then(function(t){o=t;});return o;})());  // POST text/plain:hi POST application/json:{"a":1} (method + body + Content-Type reach the backing; default ctype is text/plain)
print("-- Promise.any + thenable assimilation (M687) --");
print((function(){var o="?";Promise.any([Promise.reject("a"),Promise.resolve("b"),Promise.resolve("c")]).then(function(v){o=v;});return o;})());  // b (first fulfilment wins)
print((function(){var o="?";Promise.any([Promise.reject("x"),Promise.reject("y")]).catch(function(e){o=e.name+":"+e.errors.join(",");});return o;})());  // AggregateError:x,y (all reject -> AggregateError.errors)
print((function(){var o="?";Promise.resolve({then:function(res){res(77);}}).then(function(v){o=v;});return o;})());  // 77 (Promise.resolve assimilates a thenable {then})
print((function(){async function f(){return await {then:function(res){res(55);}};}var o="?";f().then(function(v){o=v;});return o;})());  // 55 (await x === await Promise.resolve(x): assimilates a thenable)
print("-- JSON.stringify toJSON + non-finite (M691) --");
print(JSON.stringify({a:1,toJSON:function(){return {b:2};}}));   // {"b":2} (toJSON hook replaces the value)
print(JSON.stringify({x:{toJSON:function(){return "X";}}}));     // {"x":"X"} (nested toJSON)
print(JSON.stringify({n:Infinity,m:-Infinity,ok:5}));            // {"n":null,"m":null,"ok":5} (the ±Infinity sentinel serializes as null)
print("-- Array.lastIndexOf(fromIndex) + reduce-of-empty throws (M692) --");
print([1,2,3,2,1].lastIndexOf(2,2), [1,2,3,2,1].lastIndexOf(2,-3), [1,2,3,2,1].lastIndexOf(2), [1,2,3,2,1].lastIndexOf(9));   // 1 1 3 -1 (lastIndexOf searches backward from fromIndex; neg fromIndex is from the end)
print((function(){try{[].reduce(function(a,b){return a+b;});return "no";}catch(e){return "threw";}})(), [].reduce(function(a,b){return a+b;},0), [10,20].reduce(function(a,b){return a+b;}));   // threw 0 30 (empty+no-init throws; empty+init returns init; normal reduce)
print("-- ToNumber 0b / 0o string prefixes (M693) --");
print(Number("0b101"), Number("0o17"), Number("0B11"), +"0xff", +"  0b1111  ");   // 5 15 3 255 15 (binary/octal join the existing 0x hex parse; +coercion + surrounding whitespace too)
print("-- String startsWith/endsWith position + repeat(-1) throws (M694) --");
print("hello".startsWith("llo",2), "hello".endsWith("ell",4), "hello".startsWith("he",1), "hello".endsWith("lo"), (function(){try{"x".repeat(-1);return "no";}catch(e){return "threw";}})());   // true true false true threw (startsWith honors the start position; endsWith treats the string as ending at endPos; repeat(neg)->RangeError)
print("-- Date string parsing: new Date(str) + Date.parse (M695) --");
print(new Date("2024-01-15").getFullYear(), Date.parse("2024-01-01T00:00:00.000Z"), Date.parse("1970-01-02"), new Date("2024-03-20T10:30:45").getMonth(), new Date("2024-03-20T10:30:45").getHours());   // 2024 1704067200000 86400000 2 10 (ISO date/datetime parse; trailing .sss/Z accepted+ignored as UTC)
print("-- regex replace $<name> named-group substitution (M696) --");
print("2024-01".replace(/(?<y>\d+)-(?<m>\d+)/,"$<m>/$<y>"), "a1 b2".replace(/(?<L>[a-z])(?<N>[0-9])/g,"$<N>$<L>"), "ab".replace(/(?<f>a)(b)/,"$<f>-$2"));   // 01/2024 1a 2b a-b ($<name> substitutes the named capture; mixes with $2; works under /g)
print("-- classic prototype inheritance (B.prototype = Object.create(A.prototype)) (M698) --");
print((function(){function A(n){this.n=n;}A.prototype.speak=function(){return this.n+" sound";};function B(n){A.call(this,n);}B.prototype=Object.create(A.prototype);B.prototype.speak=function(){return this.n+" bark";};var b=new B("Rex");return b.speak()+"|"+(b instanceof A)+"|"+(b instanceof B);})());   // Rex bark|true|true (reassigning .prototype reroutes new-instance [[Prototype]]; the inherited chain + instanceof both walk to the grandparent)
print("-- obj.constructor + fn/class .name (M699) --");
print((function(){function A(){}return new A().constructor===A;})(), (function(){class Dog{}return new Dog().constructor.name;})(), (function foo(){}).name, (function(){class A{} class B extends A{constructor(){super();}} return new B().constructor.name;})());   // true Dog foo B (new X().constructor is the constructor; fn/class .name is the declared name; a subclass with its own ctor keeps its name)
print("-- new.target meta-property (M700) --");
print((function(){var r;function F(){r=new.target;}new F();return r===F;})(), (function(){var r="x";function F(){r=new.target;}F();return r===undefined;})(), (function(){class Base{constructor(){if(new.target===Base)throw "abstract";}}class Sub extends Base{constructor(){super();this.ok=1;}}try{new Base();return "no";}catch(e){return new Sub().ok;}})());   // true true 1 (new.target is the ctor under `new`, undefined in a plain call; the abstract-class guard works)
print("-- floating point (M906-M909): real IEEE-754 doubles --");
print(3.14 * 2, 7 / 2, 10 / 4, 1 / 8);                                       // 6.28 3.5 2.5 0.125
print(1 / 0, -1 / 0, 0 / 0, isFinite(1 / 0), Number.isNaN(0 / 0));           // Infinity -Infinity NaN false true
print(Math.floor(3.7), Math.ceil(3.2), Math.round(2.5), Math.trunc(-3.9), Math.sqrt(16));   // 3 4 3 -3 4
print(Math.atan(1) * 4, Math.sin(0), Math.cos(0), Math.PI);                  // 3.14159265358979 0 1 3.14159265358979 (~pi; num_to_str prints 15 sig figs)
print((3.14159).toFixed(2), (5).toFixed(2), (1234.5678).toFixed(2));         // 3.14 5.00 1234.57
print(parseFloat("3.14abc"), parseFloat("1e3"), parseFloat("abc"));          // 3.14 1000 NaN
var _jf = JSON.parse('{"price":3.99,"rate":-0.5,"big":1.5e3}'); print(_jf.price, _jf.rate, _jf.big, JSON.stringify(_jf));   // 3.99 -0.5 1500 {"price":3.99,"rate":-0.5,"big":1500}
print("-- done --");
