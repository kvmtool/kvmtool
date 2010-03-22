PROGRAM	= kvm

OBJS	+= kvm.o

CFLAGS	+= -Iinclude

all: $(PROGRAM)

$(PRORAM): $(OBJS)
	$(CC) $(OBJS) -o $@

clean:
	rm -f $(OBJS) $(PROGRAM)
.PHONY: clean
