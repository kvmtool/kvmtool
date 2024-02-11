CFLAGS := -nostdinc -fno-builtin -I ../include -c
objects := $(patsubst %.c, %.o, $(wildcard *.c))

vpath %.h ../include

.PHONY : clean build
build : libstd.a

libstd.a : $(objects)
	$(AR) r $@ $^

$(objects) : %.o : %.c
	$(CC) $(CFLAGS) -c $<

clean:
	-find . -name '*.o' -exec rm {} \;
	-find . -name '*.a' -exec rm {} \;
