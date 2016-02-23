struct cmdname_help
{
    char name[16];
    char help[80];
};

static struct cmdname_help common_cmds[] = {
  {"run", "Start the virtual machine"},
  {"setup", "Setup a new virtual machine"},
  {"pause", "Pause the virtual machine"},
  {"resume", "Resume the virtual machine"},
  {"version", "Print the version of the kernel tree kvm tools"},
  {"list", "Print a list of running instances on the host."},
  {"debug", "Print debug information from a running instance"},
  {"balloon", "Inflate or deflate the virtio balloon"},
  {"stop", "Stop a running instance"},
  {"stat", "Print statistics about a running instance"},
  {"sandbox", "Run a command in a sandboxed guest"},
};
