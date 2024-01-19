#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <uart.h>

static uint64_t kpow(uint64_t x, uint64_t y);

int kputs(const char* msg)
{
    const char* ret = msg;
    for (; *msg != '\0'; ++msg) {
        uart_putchar(*msg);
    }
    uart_putchar('\n');
    return ret == msg;
}

void do_panic(const char* file, int line, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kprintf("--------------------------------------------------------------------------\n");
    kprintf("Panic at %s: %u\n", file, line);
    if (strlen(fmt)) {
        kprintf("Assert message: ");
        kprintf(fmt, ap);
    }
    uart_putchar('\n');
    va_end(ap);
    kprintf("Shutdown machine\n");
    kprintf("--------------------------------------------------------------------------\n");
    //shutdown();
}

int kprintf(const char* fmt, ...)
{
    va_list ap;
    uint64_t val;
    uint64_t temp;
    uint64_t len;
    uint64_t rev = 0;
    int ch;
    const char* str = NULL;

    va_start(ap, fmt);
    while (*fmt != '\0')
    {
        switch (*fmt)
        {
        case '%':
            fmt++;
            switch (*fmt)
            {
            case 'u':        //Decimal
                val = va_arg(ap, uint64_t);
                temp = val;
                len = 0;
                do
                {
                    len++;
                    temp /= 10;
                } while (temp);
                rev += len;
                temp = val;
                while (len)
                {
                    ch = temp / kpow(10, len - 1);
                    temp %= kpow(10, len - 1);
                    uart_putchar(ch + '0');
                    len--;
                }
                break;
            case 'p':
                uart_putchar('0');
                uart_putchar('x');
            case 'x':
                val = va_arg(ap, uint64_t);
                temp = val;
                len = 0;
                do
                {
                    len++;
                    temp /= 16;
                } while (temp);
                rev += len;
                temp = val;
                while (len)
                {
                    ch = temp / kpow(16, len - 1);
                    temp %= kpow(16, len - 1);
                    if (ch <= 9)
                    {
                        uart_putchar(ch + '0');
                    }
                    else
                    {
                        uart_putchar(ch - 10 + 'a');
                    }
                    len--;
                }
                break;
            case 's':
                str = va_arg(ap, const char*);

                while (*str)
                {
                    uart_putchar(*str);
                    str++;
                }
                break;
            case 'c':        //character
                uart_putchar(va_arg(ap, int));
                rev += 1;
                break;
            default:
                break;
            }
            break;
        case '\n':
            uart_putchar('\n');
            rev += 1;
            break;
        case '\r':
            uart_putchar('\r');
            rev += 1;
            break;
        case '\t':
            uart_putchar('\t');
            rev += 1;
            break;
        default:
            uart_putchar(*fmt);
        }
        fmt++;
    }
    va_end(ap);
    return rev;
}

static uint64_t kpow(uint64_t x, uint64_t y)
{
    uint64_t sum = 1;
    while (y--) {
        sum *= x;
    }
    return sum;
}
