// Date UTC accessors + setTime + getTimezoneOffset (M1815). Runs in its own fresh
// arena. getUTC* already aliased the local getters; setTime and getTimezoneOffset
// are the new pieces. The OS runs UTC so the offset is 0 and getUTC* == getX.
var e = new Date(0);
print("1", e.getUTCFullYear(), e.getUTCMonth(), e.getUTCDate(), e.getUTCDay());  // 1970 0 1 4 (Thu)
print("2", e.getTime(), e.getTimezoneOffset());                                   // 0 0
var d = new Date(0);
d.setTime(86400000);
print("3", d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate());                  // 1970 0 2 (one day past epoch)
d.setTime(3661000);
print("4", d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds());               // 1 1 1
var a = new Date(2024, 0, 15, 10, 30, 45);
var b = new Date(0); b.setTime(a.getTime());                                      // round-trip through epoch ms
print("5", b.getUTCFullYear(), b.getUTCMonth(), b.getUTCDate(), b.getUTCHours(), b.getUTCMinutes(), b.getUTCSeconds());
print("6", new Date(0).getTimezoneOffset());                                      // 0
print("-- done --");
