// RGA — a Replicated Growable Array: a sequence CRDT, the data structure behind
// real-time collaborative text editors.
//
// Every character is a node with a globally-unique id and a reference to the id
// of the node it was inserted *after*. That "insert-after" relation forms a tree
// rooted at the start of the document; the visible sequence is a depth-first
// walk of that tree in which siblings inserted after the same node are ordered
// by their id, newest first. Deletions are tombstones, not removals.
//
// The point: two replicas that have applied the same set of operations hold the
// same tree and therefore render the identical sequence — no matter what order
// the operations arrived in. Operations are idempotent (applying one twice is a
// no-op) and an operation whose predecessor has not arrived yet is buffered
// until it does, so even randomly-shuffled delivery converges.

const ROOT = ''; // the id every top-level node is "inserted after"

const idKey = (id) => `${id.c}:${id.r}`;

// Sibling order: newer (higher Lamport clock) first; ties broken by replica id.
function cmpNodes(a, b) {
  if (a.id.c !== b.id.c) return b.id.c - a.id.c;
  return a.id.r < b.id.r ? 1 : a.id.r > b.id.r ? -1 : 0;
}

export class RGA {
  constructor(replicaId) {
    this.replicaId = replicaId;
    this.clock = 0;
    this.nodes = new Map();          // key -> { id, key, after, value, deleted }
    this.pending = new Map();        // depKey -> [ops waiting for that node]
  }

  // ---- local editing: mutate locally and return the op to broadcast ----

  insertAt(index, value) {
    const keys = this._visibleKeys();
    const after = index <= 0 ? ROOT : keys[index - 1];
    const id = { c: ++this.clock, r: this.replicaId };
    const op = { type: 'ins', id, after, value };
    this.apply(op);
    return op;
  }

  deleteAt(index) {
    const keys = this._visibleKeys();
    const key = keys[index];
    if (key === undefined) return null;
    const op = { type: 'del', target: key };
    this.apply(op);
    return op;
  }

  // ---- applying a (local or remote) operation, idempotently ----

  apply(op) {
    if (op.type === 'ins') {
      const key = idKey(op.id);
      if (this.nodes.has(key)) return this;                 // already applied
      this.clock = Math.max(this.clock, op.id.c);            // Lamport catch-up
      if (op.after !== ROOT && !this.nodes.has(op.after)) {  // predecessor not here yet
        this._defer(op.after, op);
        return this;
      }
      this.nodes.set(key, { id: op.id, key, after: op.after, value: op.value, deleted: false });
      this._flush(key);
    } else { // 'del'
      if (!this.nodes.has(op.target)) {                      // target not here yet
        this._defer(op.target, op);
        return this;
      }
      this.nodes.get(op.target).deleted = true;              // idempotent
    }
    return this;
  }

  _defer(depKey, op) {
    if (!this.pending.has(depKey)) this.pending.set(depKey, []);
    this.pending.get(depKey).push(op);
  }

  _flush(key) {
    const waiting = this.pending.get(key);
    if (!waiting) return;
    this.pending.delete(key);
    for (const op of waiting) this.apply(op);
  }

  // ---- reading the sequence ----

  _ordered() {
    // group children by their "after" key, sort each group, DFS from ROOT
    const children = new Map();
    for (const node of this.nodes.values()) {
      if (!children.has(node.after)) children.set(node.after, []);
      children.get(node.after).push(node);
    }
    for (const list of children.values()) list.sort(cmpNodes);

    const out = [];
    const walk = (key) => {
      for (const child of children.get(key) ?? []) {
        out.push(child);
        walk(child.key);
      }
    };
    walk(ROOT);
    return out;
  }

  _visibleKeys() {
    return this._ordered().filter((n) => !n.deleted).map((n) => n.key);
  }

  toArray() {
    return this._ordered().filter((n) => !n.deleted).map((n) => n.value);
  }

  toString() {
    return this.toArray().join('');
  }

  get length() {
    return this._visibleKeys().length;
  }
}
