all: examples/a4091.svg examples/a4092.svg

card2svg: examples/a4091.h examples/a4092.h
	$(CC) card2svg.c -o card2svg

examples/a4091.svg examples/a4092.svg: card2svg
	./card2svg

edit:
	python3 editor.py

clean:
	rm -f card2svg examples/a4091.svg examples/a4092.svg
