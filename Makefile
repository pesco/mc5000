.PHONY: all test

all: mc5000

test: all
	./mc5000 -o test.bin test.s
	./mc5000 -o test2.bin test2.s
