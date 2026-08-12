JAVAC ?= javac
JAVA  ?= java
SRC    = $(wildcard src/btree/*.java)

.PHONY: all test bench clean

all: test

test:
	@mkdir -p build
	$(JAVAC) -d build $(SRC) test/btree/TestBTree.java
	$(JAVA) -cp build btree.TestBTree

bench:
	@mkdir -p build
	$(JAVAC) -d build $(SRC) bench/btree/Bench.java
	$(JAVA) -cp build btree.Bench

clean:
	rm -rf build
