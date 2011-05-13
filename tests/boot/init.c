#include <linux/reboot.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	puts("hello, KVM guest!\r");

	reboot(LINUX_REBOOT_CMD_RESTART);

	return 0;
}
