#ifndef PIT_H
#define PIT_H
#include <stddef.h>

void pit_set(uint8_t mode, uint8_t channel, uint16_t frequency);
void pit_set_control_word(uint8_t control_word);
void pit_set_frequency(uint8_t channel, uint16_t frequency);
uint16_t pit_read_time(uint8_t channel);

#endif
