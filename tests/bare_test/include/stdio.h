#ifndef _STDIO_H
#define _STDIO_H

int kputs(const char *msg);
int kprintf(const char *fmt, ...);
void do_panic(const char* file, int line, const char* fmt, ...);

#endif
