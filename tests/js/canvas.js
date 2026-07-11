// M1800: canvas 2D context host regression test. Locks the JS->browser dispatch
// (eval_canvas_method): getContext returns a context object; fillStyle/strokeStyle
// round-trip as properties; every draw op reaches g_canvas_op with the right op code,
// coords, colour, and text (the host mock `hcanvas` records each op as a CVOP line).
// Own fresh arena. Reliable host verification of the canvas built over M1796-M1799
// (osdrive screenshots proved the pixels; this locks the dispatch against regression).
var x = document.getElementById('c').getContext('2d');
console.log('ctx', typeof x);             // ctx object
console.log('fs0', x.fillStyle);          // fs0 #000000  (getContext default)
x.fillStyle = '#ff0000';
console.log('fs1', x.fillStyle);          // fs1 #ff0000  (property round-trip)
x.fillRect(1, 2, 3, 4);                    // op 0, fillStyle
x.strokeStyle = '#00ff00';
x.strokeRect(5, 6, 7, 8);                  // op 1, strokeStyle
x.clearRect(9, 10, 11, 12);                // op 2, colour ignored
x.moveTo(20, 21); x.lineTo(30, 31);        // op 3: (20,21)->(30,31), strokeStyle
x.lineTo(40, 41);                          // op 3: current point advanced -> (30,31)->(40,41)
x.fillStyle = '#0000ff'; x.fillText('Hi', 50, 51);  // op 4, fillStyle, text
x.arc(60, 61, 15);                         // op 5 (radius 15), strokeStyle
console.log('-- done --');
