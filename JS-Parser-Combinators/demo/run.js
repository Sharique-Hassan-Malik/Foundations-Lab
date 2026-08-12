// A quick tour: the same tiny combinator toolkit parses JSON and evaluates
// arithmetic, and reports errors with a line/column.
//
//     node demo/run.js

import { parseJSON } from '../src/json.js';
import { evalExpr } from '../src/arith.js';
import { ParseError } from '../src/combinators.js';

console.log('JSON parser (built from combinators):');
const doc = '{"name": "ada", "langs": ["c", "js"], "born": 1815, "ok": true}';
console.log('  input :', doc);
console.log('  parsed:', parseJSON(doc));

console.log('\nArithmetic evaluator (precedence + associativity):');
for (const e of ['1 + 2 * 3', '(1 + 2) * 3', '10 - 4 - 3', '-2 * (3 + 4)']) {
  console.log(`  ${e.padEnd(16)} = ${evalExpr(e)}`);
}

console.log('\nError reporting points at the problem:');
for (const [fn, bad] of [[parseJSON, '[1, 2 3]'], [evalExpr, '1 + * 2']]) {
  try {
    fn(bad);
  } catch (e) {
    if (e instanceof ParseError) console.log(`  ${JSON.stringify(bad)} → ${e.message}`);
  }
}
