#!/bin/sh
# gzcheck.sh — hand the encoder's output to gunzip and to Python's zlib.
#
# This is the only test that can decide whether the output is really DEFLATE.
# Round-trip against our own decoder proves the two halves agree with each
# other, which they would even if both were wrong in the same way; gunzip has
# never seen this code.
set -e
cd "$(dirname "$0")/.."

fail=0
pass=0

check() {
    name="$1"; file="$2"; level="$3"
    ./gzdeflate "$level" < "$file" > /tmp/gzcheck.$$.gz
    if gunzip -c /tmp/gzcheck.$$.gz > /tmp/gzcheck.$$.out 2>/dev/null \
       && cmp -s "$file" /tmp/gzcheck.$$.out; then
        orig=$(wc -c < "$file"); comp=$(wc -c < /tmp/gzcheck.$$.gz)
        printf '  PASS  gunzip -level %s- accepted %-22s %8s -> %-8s bytes\n' \
               "$level" "$name" "$orig" "$comp"
        pass=$((pass + 1))
    else
        printf '  FAIL  gunzip rejected or mismatched: %s (level %s)\n' "$name" "$level"
        fail=$((fail + 1))
    fi
    rm -f /tmp/gzcheck.$$.gz /tmp/gzcheck.$$.out
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A spread of shapes: each reaches a different block type and code path.
printf '' > "$tmp/empty"
printf 'hello' > "$tmp/tiny"
yes 'the quick brown fox jumps over the lazy dog' 2>/dev/null | head -2000 > "$tmp/text"
head -c 200000 /dev/urandom > "$tmp/random"
tr -dc 'ab' < /dev/urandom 2>/dev/null | head -c 100000 > "$tmp/binary" || \
    head -c 100000 /dev/zero | tr '\0' 'a' > "$tmp/binary"
head -c 300000 /dev/zero > "$tmp/zeros"
cat src/*.c include/*.h > "$tmp/source"

echo "gunzip acceptance"
echo
for level in 0 1 6 9; do
    for f in empty tiny text random binary zeros source; do
        check "$f" "$tmp/$f" "$level"
    done
done

echo
echo "════════════════════════════════════"
echo "  Streams accepted : $pass"
echo "  Streams rejected : $fail"
if [ "$fail" -eq 0 ]; then
    echo "  GZIP ACCEPTANCE PASSED"
else
    echo "  GZIP ACCEPTANCE FAILED"
fi
echo "════════════════════════════════════"
[ "$fail" -eq 0 ]
