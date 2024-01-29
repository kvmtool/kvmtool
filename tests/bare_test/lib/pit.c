#include <io.h>
#include <stddef.h>
#include <pit.h>

#define PIT_FREQ 1193182

#define PIT_BASE_ADDRESS	((short) 0x40)
#define PIT_CHANNEL_0	(PIT_BASE_ADDRESS)
#define PIT_CHANNEL_1	(PIT_BASE_ADDRESS + 1)
#define PIT_CHANNEL_2	(PIT_BASE_ADDRESS + 2)
#define PIT_CONTROL_REG	(PIT_BASE_ADDRESS + 3)

void pit_set_control_word(uint8_t control_word) {
    outb(PIT_CONTROL_REG, control_word);
}

void pit_set_frequency(uint8_t channel, uint16_t frequency) {
    uint16_t divisor = PIT_FREQ / frequency;
    outb(PIT_BASE_ADDRESS + channel, divisor & 0xFF);
    outb(PIT_BASE_ADDRESS + channel, divisor >> 8);
}

void pit_set(uint8_t mode, uint8_t channel, uint16_t frequency) {
    pit_set_control_word((channel << 6) | (mode << 1) | (0x3 << 4));
    pit_set_frequency(channel, frequency);
}

uint16_t pit_read_time(uint8_t channel) {
    uint8_t control_word = channel << 6;
    uint8_t low, high;

    outb(PIT_CONTROL_REG, control_word);
    low = inb(PIT_BASE_ADDRESS + channel);
    high = inb(PIT_BASE_ADDRESS + channel);
    return (high << 8) | low;
}
