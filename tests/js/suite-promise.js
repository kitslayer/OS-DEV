// Promise (synchronous-resolution model, M679) — a SECOND golden run, kept separate
// from suite.js because that one shares a single 40MB arena run-to-completion with only
// ~350KB headroom; this file gets its own fresh arena (js_run_doc resets g_arena_off),
// so Promise coverage can be comprehensive without starving the main suite. Ends with
// "-- done --" so the harness can detect a truncated (arena-OOM) run.
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
print("-- done --");
