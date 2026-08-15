// The headline: a chain of diamonds. One write at the top; count how much work
// each system does to settle, and whether any intermediate value is a glitch.
//
//   run: node bench/diamond.js
//
// A diamond chain of depth D stacks D join nodes, each fed by two computeds that
// both read the previous join:
//
//     x → (a1,b1) → j1 → (a2,b2) → j2 → … → jD → sink
//
// There are 2^D distinct paths from x to the sink. The correct amount of work to
// settle a change is O(D): every node's value depends only on x, so every node
// needs recomputing exactly once. Any system whose cost tracks the number of
// *paths* rather than the number of *nodes* is doing exponential redundant work.

import { signal, computed, effect, stats } from "../src/signals.js";
import { naive, naiveStats } from "./naive.js";

// Build a diamond chain with a given set of primitives. Returns the sink getter.
function buildChain(depth, { signal: sig, computed: comp }) {
  const x = sig(0);
  let prev = comp(() => x());
  for (let i = 0; i < depth; i++) {
    // `source` must be a fresh per-iteration binding: the closures below read
    // *this* layer's input, not whatever `prev` is reassigned to at the end.
    const source = prev;
    const a = comp(() => source() + 1);
    const b = comp(() => source() + 1);
    prev = comp(() => a() + b());
  }
  return { x, sink: prev };
}

function table(rows, headers) {
  const w = headers.map((h, i) => Math.max(h.length, ...rows.map((r) => String(r[i]).length)));
  const line = (r) => r.map((c, i) => String(c).padStart(w[i])).join("  ");
  return [line(headers), w.map((n) => "-".repeat(n)).join("  "), ...rows.map(line)].join("\n");
}

// --- 1. work to settle one write, real vs naive --------------------------

console.log("Diamond chain of depth D. One write at the top. Recomputations to settle:\n");

const rows = [];
let naiveAlive = true;
for (const D of [2, 4, 6, 8, 10, 12, 14, 16, 18, 20]) {
  const real = buildChain(D, { signal, computed });
  // Force initial settle, then measure one update.
  real.sink();
  const realBefore = stats.computations;
  real.x.set(1);
  real.sink();
  const realWork = stats.computations - realBefore;

  let naiveWork = "—";
  if (naiveAlive) {
    const nv = buildChain(D, naive);
    nv.sink();
    const nBefore = naiveStats.computations;
    const t0 = performance.now();
    nv.x.set(1);
    const elapsed = performance.now() - t0;
    naiveWork = naiveStats.computations - nBefore;
    if (elapsed > 2000 || naiveWork > 5_000_000) naiveAlive = false;
  }

  rows.push([D, Math.pow(2, D), naiveWork, realWork]);
}

console.log(
  table(rows, ["depth", "paths (2^D)", "naive evals", "real evals"]) +
    "\n\nThe naive system's work tracks the number of paths — it doubles with every\n" +
    "layer, because nothing stops the same downstream node from being recomputed\n" +
    "once per path that reaches it. The real system's work tracks the number of\n" +
    "nodes: 3 per layer, linear in depth. Same graph, same result, different\n" +
    "complexity class — exactly the regex-engine story, one project over.",
);

// --- 2. the glitch, made visible -----------------------------------------

console.log("\n\nThe glitch: a single diamond, watching the sink after one write.\n");

function watchDiamond(prim, label) {
  const seen = [];
  const x = prim.signal(1);
  const b = prim.computed(() => x() + 1);
  const c = prim.computed(() => x() * 2);
  const sink = prim.computed(() => `${b()}+${c()}`);
  prim.effect(() => seen.push(sink()));
  x.set(10);
  console.log(`  ${label.padEnd(6)} sink observed: ${JSON.stringify(seen)}`);
  return seen;
}

const realSeen = watchDiamond({ signal, computed, effect }, "real");
const naiveSeen = watchDiamond(naive, "naive");

console.log(`
  With x: 1 -> 10, b goes 2 -> 11 and c goes 2 -> 20, so the only correct sink
  values are "2+2" then "11+20". The real system shows exactly those. The naive
  system also emits "11+2" — b already updated, c not yet: a value that never
  should have existed. An effect that fired a network request on it would have
  acted on impossible state.`);

const realGlitched = realSeen.some((v) => v !== "2+2" && v !== "11+20");
const naiveGlitched = naiveSeen.some((v) => v !== "2+2" && v !== "11+20");
console.log(`\n  real glitched: ${realGlitched}    naive glitched: ${naiveGlitched}`);
if (realGlitched) {
  console.error("REGRESSION: the real system produced a glitch");
  process.exit(1);
}
