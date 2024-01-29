CFLAGS := -nostdinc -fno-builtin -I ../include
objects := $(patsubst %.c, %.o, $(wildcard *.c))

vpath %.h ../include

.PHONY : clean build

build : libintr.a

libintr.a : $(objects) trapasm.o vectors.o
	$(AR) r $@ $^

trapasm.o : trapasm.S
	$(CC) $(CFLAGS) -c trapasm.S

vectors.o : vectors.S
	$(CC) $(CFLAGS) -c vectors.S

$(objects) : %.o : %.c
	$(CC) $(CFLAGS) -c $<

vectors.S: vectors.pl
	./vectors.pl > vectors.S

clean:
	-find . -name '*.o' -exec rm {} \;
	-find . -name '*.a' -exec rm {} \;
	-rm vectors.S
