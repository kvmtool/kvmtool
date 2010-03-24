PROGRAM	= kvm

OBJS	+= kvm.o
OBJS	+= cpu.o
OBJS	+= util.o

CFLAGS	+= -Iinclude

all: $(PROGRAM)

$(PROGRAM): $(OBJS)
	$(CC) $(OBJS) -o $@

$(OBJS):

clean:
	rm -f $(OBJS) $(PROGRAM)
.PHONY: clean
