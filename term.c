#include <poll.h>
#include <stdbool.h>
#include <termios.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/uio.h>
#include <signal.h>
#include <pty.h>
#include <utmp.h>

#include "kvm/read-write.h"
#include "kvm/term.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"

#define TERM_FD_IN      0
#define TERM_FD_OUT     1

extern struct kvm *kvm;
static struct termios	orig_term;

int term_escape_char	= 0x01; /* ctrl-a is used for escape */
bool term_got_escape	= false;

int active_console;

int term_fds[4][2];

int term_getc(int who, int term)
{
	unsigned char c;

	if (who != active_console)
		return -1;
	if (read_in_full(term_fds[term][TERM_FD_IN], &c, 1) < 0)
		return -1;

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

int term_putc(int who, char *addr, int cnt, int term)
{
	int ret;

	if (who != active_console)
		return -1;

	while (cnt--) {
		ret = write(term_fds[term][TERM_FD_OUT], addr++, 1);
		if (ret < 0)
			return 0;
	}

	return cnt;
}

int term_getc_iov(int who, struct iovec *iov, int iovcnt, int term)
{
	int c;

	if (who != active_console)
		return 0;

	c = term_getc(who, term);

	if (c < 0)
		return 0;

	*((char *)iov[TERM_FD_IN].iov_base)	= (char)c;

	return sizeof(char);
}

int term_putc_iov(int who, struct iovec *iov, int iovcnt, int term)
{
	if (who != active_console)
		return 0;

	return writev(term_fds[term][TERM_FD_OUT], iov, iovcnt);
}

bool term_readable(int who, int term)
{
	struct pollfd pollfd = (struct pollfd) {
		.fd	= term_fds[term][TERM_FD_IN],
		.events	= POLLIN,
		.revents = 0,
	};

	if (who != active_console)
		return false;

	return poll(&pollfd, 1, 0) > 0;
}

static void term_cleanup(void)
{
	int i;

	for (i = 0; i < 4; i++)
		tcsetattr(term_fds[i][TERM_FD_IN], TCSANOW, &orig_term);
}

static void term_sig_cleanup(int sig)
{
	term_cleanup();
	signal(sig, SIG_DFL);
	raise(sig);
}

void term_set_tty(int term)
{
	struct termios orig_term;
	int master, slave;
	char new_pty[PATH_MAX];

	if (tcgetattr(STDIN_FILENO, &orig_term) < 0)
		die("unable to save initial standard input settings");

	orig_term.c_lflag &= ~(ICANON | ECHO | ISIG);

	if (openpty(&master, &slave, new_pty, &orig_term, NULL) < 0)
		return;

	close(slave);

	pr_info("Assigned terminal %d to pty %s\n", term, new_pty);

	term_fds[term][TERM_FD_IN] = term_fds[term][TERM_FD_OUT] = master;
}

void term_init(void)
{
	struct termios term;
	int i;

	if (tcgetattr(STDIN_FILENO, &orig_term) < 0)
		die("unable to save initial standard input settings");

	term = orig_term;
	term.c_lflag &= ~(ICANON | ECHO | ISIG);
	tcsetattr(STDIN_FILENO, TCSANOW, &term);

	for (i = 0; i < 4; i++)
		if (term_fds[i][TERM_FD_IN] == 0) {
			term_fds[i][TERM_FD_IN] = STDIN_FILENO;
			term_fds[i][TERM_FD_OUT] = STDOUT_FILENO;
		}

	signal(SIGTERM, term_sig_cleanup);
	atexit(term_cleanup);
}
