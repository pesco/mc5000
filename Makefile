.PHONY: all test

all: mc5000

test: all
	./mc5000 -o test.bin test.s
