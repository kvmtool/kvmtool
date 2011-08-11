#include <poll.h>
#include <stdbool.h>
#include <termios.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/uio.h>
#include <signal.h>

#include "kvm/read-write.h"
#include "kvm/term.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"

extern struct kvm *kvm;
static struct termios	orig_term;

int term_escape_char	= 0x01; /* ctrl-a is used for escape */
bool term_got_escape	= false;

int active_console;

int term_getc(int who)
{
	int c;

	if (who != active_console)
		return -1;

	if (read_in_full(STDIN_FILENO, &c, 1) < 0)
		return -1;

	c &= 0xff;

	if (term_got_escape) {
		term_got_escape = false;
		if (c == 'x')
			kvm_cpu__reboot();
		if (c == term_escape_char)
			return c;
	}

	if (c == term_escape_char) {
		term_got_escape = true;
		return -1;
	}

	return c;
}

int term_putc(int who, char *addr, int cnt)
{
	if (who != active_console)
		return -1;

	while (cnt--)
		fprintf(stdout, "%c", *addr++);

	fflush(stdout);
	return cnt;
}

int term_getc_iov(int who, struct iovec *iov, int iovcnt)
{
	int c;

	if (who != active_console)
		return 0;

	c = term_getc(who);

	if (c < 0)
		return 0;

	*((int *)iov[0].iov_base)	= c;

	return sizeof(char);
}

int term_putc_iov(int who, struct iovec *iov, int iovcnt)
{
	if (who != active_console)
		return 0;

	return writev(STDOUT_FILENO, iov, iovcnt);
}

bool term_readable(int who)
{
	struct pollfd pollfd = (struct pollfd) {
		.fd	= STDIN_FILENO,
		.events	= POLLIN,
		.revents = 0,
	};

	if (who != active_console)
		return false;

	return poll(&pollfd, 1, 0) > 0;
}

static void term_cleanup(void)
{
	tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
}

static void term_sig_cleanup(int sig)
{
	term_cleanup();
	signal(sig, SIG_DFL);
	raise(sig);
}

void term_init(void)
{
	struct termios term;

	if (tcgetattr(STDIN_FILENO, &orig_term) < 0)
		die("unable to save initial standard input settings");

	term = orig_term;
	term.c_lflag &= ~(ICANON | ECHO | ISIG);
	tcsetattr(STDIN_FILENO, TCSANOW, &term);

	signal(SIGTERM, term_sig_cleanup);
	atexit(term_cleanup);
}
