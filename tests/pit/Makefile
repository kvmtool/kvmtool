NAME	:= tick

BIN	:= $(NAME).bin
ELF	:= $(NAME).elf
OBJ	:= $(NAME).o

all: $(BIN)

$(BIN): $(ELF)
	objcopy -O binary $< $@

$(ELF): $(OBJ)
	ld -Ttext=0x00 -nostdlib -static $< -o $@

%.o: %.S
	gcc -nostdinc -c $< -o $@

clean:
	rm -f $(BIN) $(ELF) $(OBJ)
.PHONY: clean
