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
print("end-of-main");
setTimeout(function(){ print("-- done --"); }, 9999999);
