PROGRAM	= kvm

OBJS	+= cpu.o
OBJS	+= interrupt.o
OBJS	+= kvm.o
OBJS	+= main.o
OBJS	+= util.o

CFLAGS	+= $(CPPFLAGS) -Iinclude -g

WARNINGS += -Wall
WARNINGS += -Wcast-align
WARNINGS += -Wformat=2
WARNINGS += -Winit-self
WARNINGS += -Wmissing-declarations
WARNINGS += -Wmissing-prototypes
WARNINGS += -Wnested-externs
WARNINGS += -Wno-system-headers
WARNINGS += -Wold-style-definition
WARNINGS += -Wredundant-decls
WARNINGS += -Wsign-compare
WARNINGS += -Wstrict-prototypes
WARNINGS += -Wundef
WARNINGS += -Wvolatile-register-var
WARNINGS += -Wwrite-strings

CFLAGS	+= $(WARNINGS)

all: $(PROGRAM)

$(PROGRAM): $(OBJS)
	$(CC) $(OBJS) -o $@

$(OBJS):

clean:
	rm -f $(OBJS) $(PROGRAM)
.PHONY: clean
