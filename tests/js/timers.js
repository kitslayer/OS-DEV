// setTimeout / setInterval regression (M745). The engine has no event loop, so
// timers drain after the top-level script in (delay, registration) order. We make
// the largest-delay timer print "-- done --" so it fires last (the harness wants
// that line last) and the rest exercise ordering, chaining, and cancellation.
print("start");
setTimeout(function(){ print("A-delay-10"); }, 10);
setTimeout(function(){ print("C-delay-100"); }, 100);
var id = setTimeout(function(){ print("CANCELLED-must-not-print"); }, 50);
clearTimeout(id);
setTimeout(function(){ print("B-delay-20"); setTimeout(function(){ print("D-chained"); }, 5); }, 20);
print("setInterval is " + typeof setInterval + ", clearInterval is " + typeof clearInterval);
print("rAF=" + typeof requestAnimationFrame + " qm=" + typeof queueMicrotask + " caf=" + typeof cancelAnimationFrame);
requestAnimationFrame(function(){ print("raf-fired"); });
queueMicrotask(function(){ print("microtask-fired"); });
// EventSource (M-eventsource): one-shot SSE snapshot. Its deferred first-event task
// is enqueued at delay 0 (so it drains before the delay-10 timer). The host mock
// returns a canned data payload; onopen then onmessage fire with it. A "/fail" URL
// fires onerror; an EventSource closed before its task runs delivers nothing.
print("EventSource is " + typeof EventSource);
var es = new EventSource("http://x/stream");
print("es.readyState(connecting)=" + es.readyState + " typeof es.close=" + typeof es.close);
es.onopen = function(e){ print("es-onopen " + e.type); };
es.onmessage = function(e){ print("es-onmessage data=" + e.data + " state=" + es.readyState); };
var esFail = new EventSource("http://x/fail");
esFail.onerror = function(){ print("esFail-onerror"); };
var esClosed = new EventSource("http://x/stream");
esClosed.onmessage = function(){ print("esClosed-MUST-NOT-FIRE"); };
esClosed.close();
print("end-of-main");
setTimeout(function(){ print("-- done --"); }, 9999999);
