#!/usr/bin/env bash
# Build, test and benchmark. Needs only a JDK (17+) on PATH.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf out && mkdir -p out
echo "== compiling =="
javac -d out src/hll/*.java
echo "== tests =="
javac -cp out -d out test/hll/HllTest.java
java -cp out hll.HllTest
echo
echo "== benchmark =="
javac -cp out -d out bench/Benchmark.java
java -cp out Benchmark
