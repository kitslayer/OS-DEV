// ArrayBuffer + Uint8Array (M1850): binary byte stores. Byte indexing clamps to
// 0-255; a Uint8Array over an ArrayBuffer shares its bytes.
var ab = new ArrayBuffer(8);
print("ab.byteLength=" + ab.byteLength);
var u = new Uint8Array(4);
print("u.length=" + u.length + " byteLength=" + u.byteLength);
u[0] = 10; u[1] = 20; u[2] = 300; u[3] = -1;              // 300->44, -1->255 (clamp)
print("u=" + u);
print("u[2]=" + u[2] + " oob=" + u[5]);
var v = new Uint8Array([1, 2, 3, 255, 256]);             // 256->0
print("v=" + v + " len=" + v.length);
var w = new Uint8Array(ab);                              // view over ab (shares bytes)
print("w.length=" + w.length + " sharesBuffer=" + (w.buffer === ab));
w[0] = 65;
print("w=" + w);
var s = new Uint8Array(5); s.set([9, 8, 7]); s.set([1, 1], 3);
print("s=" + s);
var f = new Uint8Array(4); f.fill(7);
print("f=" + f);
f.fill(0, 1, 3);
print("f2=" + f);
var sub = v.slice(1, 4);
print("sub=" + sub + " len=" + sub.length);
print("idxOf255=" + v.indexOf(255) + " idxOf99=" + v.indexOf(99));
var sum = 0; for (var b of v) sum += b;
print("sum=" + sum);
print("join=" + v.join("-"));
print("typeof=" + typeof u + " bpe=" + u.BYTES_PER_ELEMENT);
print("-- done --");
