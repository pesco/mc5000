.PHONY: all test clean doc

all: mc5000
doc: mc5000.1 mc5000.1.txt

test: all
	./mc5000 -o test.bin test.s
	./mc5000 -o test2.bin test2.s
clean:
	rm -f mc5000 mc5000.1 test.bin test2.bin

.SUFFIXES: .mdoc .txt
.mdoc:
	mandoc -Ios= -Tlint $<
	cp -f $< $@
.mdoc.txt:
	mandoc -Ios= -Tascii $< | col -b > $@
