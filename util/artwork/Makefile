.DELETE_ON_ERROR:
.PHONY: all edit clean

all: examples/a4091.svg examples/a4092.svg

card2svg: card2svg.c assets/a4091.h assets/a4092.h
	$(CC) card2svg.c -o card2svg

examples/%.svg: card2svg | examples
	./card2svg $*

examples:
	mkdir -p $@

edit:
	python3 editor.py

clean:
	rm -f card2svg examples/a4091.svg examples/a4092.svg
