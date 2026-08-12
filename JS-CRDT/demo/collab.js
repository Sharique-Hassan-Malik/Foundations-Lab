// A tiny simulation of collaborative editing: three users type into their own
// replica of a shared document at the same time, their edits are delivered to
// each other in a scrambled order (as a real network would), and every replica
// ends up with the identical text — with no central server and no locking.
//
//     node demo/collab.js

import { RGA } from '../src/index.js';

function type(replica, text, atStart = false) {
  // append `text`, returning the ops produced
  const ops = [];
  let pos = atStart ? 0 : replica.length;
  for (const ch of text) ops.push(replica.insertAt(pos++, ch));
  return ops;
}

const alice = new RGA('alice');
const bob = new RGA('bob');
const carol = new RGA('carol');

// they start from a shared opening line
const seed = type(alice, 'CRDTs are ');
for (const op of seed) { bob.apply(op); carol.apply(op); }

console.log('starting document: ' + JSON.stringify(alice.toString()));
console.log('\nthree users now type concurrently, each unaware of the others…\n');

// concurrent, conflicting edits — all appended "at the same time"
const aliceOps = type(alice, 'conflict-free.');
const bobOps = type(bob, 'replicated. ');
const carolOps = type(carol, 'eventually consistent. ');

console.log(`  alice sees: ${JSON.stringify(alice.toString())}`);
console.log(`  bob   sees: ${JSON.stringify(bob.toString())}`);
console.log(`  carol sees: ${JSON.stringify(carol.toString())}`);

// gather every edit and deliver them to everyone in a shuffled order
const all = [...aliceOps, ...bobOps, ...carolOps];
const shuffled = all
  .map((op) => ({ op, k: Math.random() }))
  .sort((x, y) => x.k - y.k)
  .map((x) => x.op);

for (const op of shuffled) {
  alice.apply(op);
  bob.apply(op);
  carol.apply(op);
}

console.log('\nafter the edits sync (in a random order)…\n');
console.log(`  alice: ${JSON.stringify(alice.toString())}`);
console.log(`  bob:   ${JSON.stringify(bob.toString())}`);
console.log(`  carol: ${JSON.stringify(carol.toString())}`);

const converged = alice.toString() === bob.toString() && bob.toString() === carol.toString();
console.log('\nall three replicas identical: ' + converged);
process.exit(converged ? 0 : 1);
