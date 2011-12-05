#include "kvm/builtin-sandbox.h"
#include "kvm/builtin-run.h"

int kvm_cmd_sandbox(int argc, const char **argv, const char *prefix)
{
	kvm_run_set_wrapper_sandbox();

	return kvm_cmd_run(argc, argv, prefix);
}
