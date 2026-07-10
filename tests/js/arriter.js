// M1792: Array iteration-method callbacks receive the 3rd `array` argument
// (element, index, array), and reduce/reduceRight get (acc, cur, index, array).
// Sort's comparator stays 2-arg (a,b); Array.from's mapFn stays (element,index).
// Own fresh arena (like suite-promise.js/timers.js). A revert of the ca[3]/recv
// change flips the "array-arg" lines back to undefined (",,"/NaN), failing here.
console.log("map3", [10,20,30].map((x,i,a)=>a.length).join(","));        // 3,3,3
console.log("mapI", [10,20,30].map((x,i)=>x+i).join(","));               // 10,21,32
console.log("filt", [5,6,7].filter((x,i,a)=>i===a.length-1).join(","));  // 7
console.log("find", [5,6,7].find((x,i,a)=>i===a.length-1));              // 7
console.log("findIdx", [5,6,7].findIndex((x,i,a)=>x===a[a.length-1]));  // 2
console.log("some", [1,2,3].some((x,i,a)=>x===a.length));               // true
console.log("every", [3,3,3].every((x,i,a)=>x===a.length));            // true
console.log("fe", (function(){var s="";[1,2].forEach((x,i,a)=>s+=x+":"+a.length+" ");return s;})());  // 1:2 2:2
console.log("flatMap", [1,2].flatMap((x,i,a)=>[x,a.length]).join(","));  // 1,2,2,2
console.log("red4", [1,2,3].reduce((acc,x,i,a)=>acc+a.length,0));        // 9
console.log("redR4", [1,2,3].reduceRight((acc,x,i,a)=>acc+a.length,0));  // 9
// regressions: sort comparator stays (a,b); Array.from mapFn stays (elem,index)
console.log("redI", [1,2,3,4].reduce((a,b)=>a+b,0));                     // 10
console.log("sort", [3,1,2].sort((a,b)=>a-b).join(","));                // 1,2,3
console.log("sortD", [30,4,200].sort().join(","));                     // 200,30,4 (lexicographic default)
console.log("from", Array.from([9,8],(x,i)=>x+i).join(","));            // 9,9
console.log("-- done --");
