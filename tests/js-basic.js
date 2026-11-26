// 基本
var a = 1 + 2 * 3; console.log("a", a);
let s = "abc" + 5; console.log(s, s.length, s.toUpperCase(), s.charAt(1), s.slice(-2));
const arr = [3, 1, 2]; arr.push(10); console.log(arr, arr.length, arr.sort().join("-"), arr.map(x => x * 2));
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } console.log("fib", fib(20));
const o = {x: 1, y: "two", f() { return this.x + 1; }}; console.log(o.x, o["y"], o.f(), JSON.stringify(o));
for (let i = 0; i < 3; i++) { if (i == 1) continue; console.log("i", i); }
let sum = 0; for (const v of [1, 2, 3, 4]) sum += v; console.log("sum", sum);
for (const k in {p: 1, q: 2}) console.log("key", k);
const name = "世界"; console.log(`こんにちは ${name}! ${1 + 1}`);
try { null.x; } catch (e) { console.log("caught", e.message); }
try { throw new Error("boom"); } catch (e) { console.log("thrown", e.message); }
switch (3) { case 1: console.log("one"); break; case 3: console.log("three"); default: console.log("fall"); }
console.log(typeof undefinedVar, typeof 1, typeof "s", typeof {}, typeof fib, typeof null);
console.log(0.1 + 0.2, 1 / 3, 1e21, 123.456.toFixed(1), (255).toString(16), parseInt("42px"), parseFloat("3.5e2"), Number("x"));
console.log(Math.max(1, 5, 3), Math.floor(-1.5), Math.round(2.5), Math.sqrt(2), Math.pow(2, 10), Math.PI);
const fns = []; for (let i = 0; i < 3; i++) fns.push(() => i); console.log(fns.map(f => f()));
function Counter() { this.n = 0; } Counter.prototype.inc = function () { return ++this.n; };
const c = new Counter(); c.inc(); console.log("counter", c.inc(), c instanceof Counter);
const {x, y: yy} = o; const [p, q = 9] = [7]; console.log(x, yy, p, q);
console.log([1, [2, 3]].concat([4]), [..."ab"], Object.keys({a: 1, b: 2}), "a,b,,c".split(","));
let w = 0; while (true) { if (++w > 5) break; } console.log("w", w);
console.log("x".padStart(3, "0"), "a-b-c".replaceAll("-", "+"), [5, 1, 10].sort((a, b) => a - b));
const d = new Date(2026, 10, 26, 9, 30, 0); console.log(d.getFullYear(), d.getMonth(), d.getDate(), d.getHours());
console.log([1, 2, 3].reduce((a, b) => a + b, 0), [1, 2, 3].filter(n => n % 2).length, [1, 2, 3].includes(2));
label: for (var z = 0; z < 2; z++) {} console.log("z", z);
console.log(1 == "1", 1 === "1", null == undefined, NaN == NaN, [] + [], "5" * "2", "5" - 2, "5" + 2);
