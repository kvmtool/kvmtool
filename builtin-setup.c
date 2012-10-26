#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-setup.h>
#include <kvm/kvm.h>
#include <kvm/parse-options.h>
#include <kvm/read-write.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>

extern char _binary_guest_init_start;
extern char _binary_guest_init_size;

static const char *instance_name;

static const char * const setup_usage[] = {
	"lkvm setup [name]",
	NULL
};

static const struct option setup_options[] = {
	OPT_END()
};

static void parse_setup_options(int argc, const char **argv)
{
	while (argc != 0) {
		argc = parse_options(argc, argv, setup_options, setup_usage,
				PARSE_OPT_STOP_AT_NON_OPTION);
		if (argc != 0 && instance_name)
			kvm_setup_help();
		else
			instance_name = argv[0];
		argv++;
		argc--;
	}
}

void kvm_setup_help(void)
{
	printf("\n%s setup creates a new rootfs under %s.\n"
		"This can be used later by the '-d' parameter of '%s run'.\n",
		KVM_BINARY_NAME, kvm__get_dir(), KVM_BINARY_NAME);
	usage_with_options(setup_usage, setup_options);
}

static int copy_file(const char *from, const char *to)
{
	int in_fd, out_fd;
	void *src, *dst;
	struct stat st;
	int err = -1;

	in_fd = open(from, O_RDONLY);
	if (in_fd < 0)
		return err;

	if (fstat(in_fd, &st) < 0)
		goto error_close_in;

	out_fd = open(to, O_RDWR | O_CREAT | O_TRUNC, st.st_mode & (S_IRWXU|S_IRWXG|S_IRWXO));
	if (out_fd < 0)
		goto error_close_in;

	src = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, in_fd, 0);
	if (src == MAP_FAILED)
		goto error_close_out;

	if (ftruncate(out_fd, st.st_size) < 0)
		goto error_munmap_src;

	dst = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, out_fd, 0);
	if (dst == MAP_FAILED)
		goto error_munmap_src;

	memcpy(dst, src, st.st_size);

	if (fsync(out_fd) < 0)
		goto error_munmap_dst;

	err = 0;

error_munmap_dst:
	munmap(dst, st.st_size);
error_munmap_src:
	munmap(src, st.st_size);
error_close_out:
	close(out_fd);
error_close_in:
	close(in_fd);

	return err;
}

static const char *guestfs_dirs[] = {
	"/dev",
	"/etc",
	"/home",
	"/host",
	"/proc",
	"/root",
	"/sys",
	"/tmp",
	"/var",
	"/var/lib",
	"/virt",
	"/virt/home",
};

static const char *guestfs_symlinks[] = {
	"/bin",
	"/lib",
	"/lib64",
	"/sbin",
	"/usr",
	"/etc/ld.so.conf",
};

static int copy_init(const char *guestfs_name)
{
	char path[PATH_MAX];
	size_t size;
	int fd, ret;
	char *data;

	size = (size_t)&_binary_guest_init_size;
	data = (char *)&_binary_guest_init_start;
	snprintf(path, PATH_MAX, "%s%s/virt/init", kvm__get_dir(), guestfs_name);
	remove(path);
	fd = open(path, O_CREAT | O_WRONLY, 0755);
	if (fd < 0)
		die("Fail to setup %s", path);
	ret = xwrite(fd, data, size);
	if (ret < 0)
		die("Fail to setup %s", path);
	close(fd);

	return 0;
}

static int copy_passwd(const char *guestfs_name)
{
	char path[PATH_MAX];
	FILE *file;
	int ret;

	snprintf(path, PATH_MAX, "%s%s/etc/passwd", kvm__get_dir(), guestfs_name);

	file = fopen(path, "w");
	if (!file)
		return -1;

	ret = fprintf(file, "root:x:0:0:root:/root:/bin/sh\n");
	if (ret < 0)
		return ret;

	fclose(file);

	return 0;
}

static int make_guestfs_symlink(const char *guestfs_name, const char *path)
{
	char target[PATH_MAX];
	char name[PATH_MAX];

	snprintf(name, PATH_MAX, "%s%s%s", kvm__get_dir(), guestfs_name, path);

	snprintf(target, PATH_MAX, "/host%s", path);

	return symlink(target, name);
}

static int make_dir(const char *dir)
{
	char name[PATH_MAX];

	snprintf(name, PATH_MAX, "%s%s", kvm__get_dir(), dir);

	return mkdir(name, 0777);
}

static void make_guestfs_dir(const char *guestfs_name, const char *dir)
{
	char name[PATH_MAX];

	snprintf(name, PATH_MAX, "%s%s", guestfs_name, dir);

	make_dir(name);
}

void kvm_setup_resolv(const char *guestfs_name)
{
	char path[PATH_MAX];

	snprintf(path, PATH_MAX, "%s%s/etc/resolv.conf", kvm__get_dir(), guestfs_name);

	copy_file("/etc/resolv.conf", path);
}

static int do_setup(const char *guestfs_name)
{
	unsigned int i;
	int ret;

	ret = make_dir(guestfs_name);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(guestfs_dirs); i++)
		make_guestfs_dir(guestfs_name, guestfs_dirs[i]);

	for (i = 0; i < ARRAY_SIZE(guestfs_symlinks); i++) {
		make_guestfs_symlink(guestfs_name, guestfs_symlinks[i]);
	}

	ret = copy_init(guestfs_name);
	if (ret < 0)
		return ret;

	return copy_passwd(guestfs_name);
}

int kvm_setup_create_new(const char *guestfs_name)
{
	return do_setup(guestfs_name);
}

int kvm_cmd_setup(int argc, const char **argv, const char *prefix)
{
	int r;

	parse_setup_options(argc, argv);

	if (instance_name == NULL)
		kvm_setup_help();

	r = do_setup(instance_name);
	if (r == 0)
		printf("A new rootfs '%s' has been created in '%s%s'.\n\n"
			"You can now start it by running the following command:\n\n"
			"  %s run -d %s\n",
			instance_name, kvm__get_dir(), instance_name,
			KVM_BINARY_NAME,instance_name);
	else
		printf("Unable to create rootfs in %s%s: %s\n",
			kvm__get_dir(), instance_name, strerror(errno));

	return r;
}
