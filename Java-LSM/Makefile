JAVAC ?= javac
JAVA  ?= java
SRC    = $(wildcard src/lsm/*.java)

.PHONY: all test bench clean

all: test

test:
	@mkdir -p build
	$(JAVAC) -d build $(SRC) test/lsm/TestLsm.java
	$(JAVA) -cp build lsm.TestLsm

bench:
	@mkdir -p build
	$(JAVAC) -d build $(SRC) bench/lsm/Bench.java
	$(JAVA) -cp build lsm.Bench

clean:
	rm -rf build
