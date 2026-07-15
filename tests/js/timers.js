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
// WebSocket (M1844): one-shot request/reply over a real WS connection. Its deferred
// pump (also a delay-0 task, enqueued after the EventSource tasks above) opens the
// connection, fires onopen, drains ws.send() — INCLUDING sends issued from inside
// onopen — reads the echo replies (host mock echoes "echo:<msg>"), fires onmessage
// per reply, then onclose. A "/fail" URL fires onerror+onclose; a socket closed
// before its pump runs delivers nothing.
print("WebSocket is " + typeof WebSocket);
var ws = new WebSocket("ws://x/echo");
print("ws.readyState(connecting)=" + ws.readyState + " typeof ws.send=" + typeof ws.send);
ws.onopen = function(e){ print("ws-onopen " + e.type + " state=" + ws.readyState); ws.send("hi"); ws.send("yo"); };
ws.onmessage = function(e){ print("ws-onmessage " + e.data); };
ws.onclose = function(){ print("ws-onclose state=" + ws.readyState); };
var wsFail = new WebSocket("ws://x/fail");
wsFail.onerror = function(){ print("wsFail-onerror"); };
wsFail.onclose = function(){ print("wsFail-onclose"); };
var wsClosed = new WebSocket("ws://x/echo");
wsClosed.onmessage = function(){ print("wsClosed-MUST-NOT-FIRE"); };
wsClosed.close();
// Binary WebSocket (M1859): ws.send(Uint8Array) sends a BINARY frame; the echo
// mock returns the same bytes. Default binaryType is "blob" — with no Blob type
// we deliver a Uint8Array; set "arraybuffer" to receive an ArrayBuffer instead.
// A text send in the same session still round-trips as "echo:<msg>".
var wsBin = new WebSocket("ws://x/echo");
wsBin.onopen = function(){ wsBin.send("mix"); wsBin.send(new Uint8Array([1,2,3,255])); };
wsBin.onmessage = function(e){
  if (typeof e.data === "string") print("wsBin-text " + e.data);
  else print("wsBin-u8 len=" + e.data.length + " [" + e.data.join(",") + "]");
};
wsBin.onclose = function(){ print("wsBin-close binaryType=" + wsBin.binaryType); };
var wsAB = new WebSocket("ws://x/echo");
wsAB.binaryType = "arraybuffer";
wsAB.onopen = function(){ wsAB.send(new Uint8Array([9,8,7])); };
wsAB.onmessage = function(e){ print("wsAB-ab byteLength=" + e.data.byteLength + " [" + new Uint8Array(e.data).join(",") + "]"); };
print("end-of-main");
setTimeout(function(){ print("-- done --"); }, 9999999);
