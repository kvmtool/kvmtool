CFLAGS := -nostdinc -fno-builtin -I ../include -c
objects := $(patsubst %.c, %.o, $(wildcard *.c))

vpath %.h ../include

.PHONY : clean build

build : boot.o main.o

boot.o : boot.S
	$(CC) $(CFLAGS) -c boot.S
main.o : main.c
	$(CC) $(CFLAGS) -c main.c

clean:
	-find . -name '*.o' -exec rm {} \;
	-find . -name '*.a' -exec rm {} \;
