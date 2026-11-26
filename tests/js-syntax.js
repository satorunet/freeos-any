// class
class Animal { constructor(name) { this.name = name; } speak() { return `${this.name} が鳴く`; } get upper() { return this.name.toUpperCase(); } static kind() { return "animal"; } }
class Dog extends Animal { legs = 4; constructor(n) { super(n); this.dog = true; } speak() { return super.speak() + " (ワン)"; } }
const d = new Dog("pochi"); console.log(d.speak(), d.legs, d.upper, Dog.kind(), d instanceof Dog, d instanceof Animal, typeof Dog);
// getter / setter
const o = { _v: 1, get v() { return this._v * 10; }, set v(x) { this._v = x; } }; o.v = 5; console.log(o.v);
// destructuring
function f({a, b = 2, c: {d} = {d: 9}}, [x, , y = 7], ...rest) { return [a, b, d, x, y, rest.length].join(","); }
console.log(f({a: 1}, [10, 20]), f({a: 1, b: 3, c: {d: 4}}, [5, 6, 8], 0, 0));
for (const [k, v] of Object.entries({p: 1, q: 2})) console.log("entry", k, v);
let p = 1, q = 2; [p, q] = [q, p]; console.log("swap", p, q);
const {name, ...others} = {name: "n", x: 1, y: 2}; console.log(name, Object.keys(others).join(""));
const [[n1, n2], {z}] = [[1, 2], {z: 3}]; console.log(n1 + n2 + z);
// optional chaining
const nul = null; console.log(nul?.a.b.c, nul?.[0], nul?.(), ({a: {b: 1}}).a?.b);
// Map / Set
const m = new Map([["a", 1]]); m.set("b", 2); console.log(m.get("b"), m.size, m.has("a"), [...m.keys()].join(""));
for (const [k, v] of m) console.log("map", k, v);
const s = new Set([1, 2, 2, 3]); s.add(4); console.log(s.size, s.has(2), [...s].join(""));
// bind
function hi(g) { return g + " " + this.who; } const b = hi.bind({who: "taro"}, "やあ"); console.log(b());
// Promise / async
let log = [];
Promise.resolve(1).then(v => log.push("then " + v));
async function af() { const v = await Promise.resolve(41); return v + 1; }
af().then(v => { log.push("async " + v); console.log(log.join(" | ")); });
new Promise((res) => res("ok")).then(v => console.log("promise", v));
Promise.all([1, Promise.resolve(2)]).then(v => console.log("all", v.join("+")));
// builtins
console.log([1, [2, [3]]].flat(2).join(""), [1, 2, 3].at(-1), new Array(3).fill(0).join(""), Number.isInteger(5), Math.hypot(3, 4), Object.fromEntries([["a", 1]]).a);
const proto = {hello() { return "hi"; }}; const c = Object.create(proto); console.log(c.hello(), Object.getPrototypeOf(c) === proto);
Object.defineProperty(c, "x", {get() { return 99; }}); console.log(c.x);
console.log([3, 1, 2].toString(), "abc".at(-1), "  x ".trimStart() + "|", "b".localeCompare("a"), Array.from({length: 3}, (_, i) => i * 2).join(""));
label1: for (let i = 0; i < 3; i++) { for (let j = 0; j < 3; j++) { if (j == 1) continue label1; if (i == 2) break label1; console.log("lab", i, j); } }
