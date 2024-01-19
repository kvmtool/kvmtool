# kvmtool-test-x86_64

A simple baremetal program for kvmtool test.

## Usage

Edit kvmtool first:

```c
diff --git a/x86/kvm.c b/x86/kvm.c
index 6383b714..ab35648d 100644
--- a/x86/kvm.c
+++ b/x86/kvm.c
@@ -230,8 +230,8 @@ void kvm__irq_trigger(struct kvm *kvm, int irq)
        kvm__irq_line(kvm, irq, 0);
 }
 
-#define BOOT_LOADER_SELECTOR   0x1000
-#define BOOT_LOADER_IP         0x0000
+#define BOOT_LOADER_SELECTOR   0x0000
+#define BOOT_LOADER_IP         0x0600
 #define BOOT_LOADER_SP         0x8000
 #define BOOT_CMDLINE_OFFSET    0x20000
```

Then re-compile kvmtool and kvmtool-test-x86_64.

```bash
make
lkvm run -k kvmtool_test/kernel.bin -c 1
```

## Debug Tips

Unfortunately, kvmtool does not support debug with gdb. You can print registers/segmentations/memory infomation and so on by editing /path_to_kvmtool/ like this:

```c
diff --git a/kvm-cpu.c b/kvm-cpu.c
index de740cdd..a4d00344 100644
--- a/kvm-cpu.c
+++ b/kvm-cpu.c
@@ -203,6 +203,10 @@ int kvm_cpu__start(struct kvm_cpu *cpu)
                        kvm_cpu__run_task(cpu);
 
                kvm_cpu__run(cpu);
+                kvm_cpu__show_registers(cpu);
+                kvm_cpu__show_code(cpu);
+                kvm_cpu__show_page_tables(cpu);
 
                switch (cpu->kvm_run->exit_reason) {
                case KVM_EXIT_UNKNOWN:
```

You can also use `kvm__dump_mem()` to view memory content.

## Credits

Inspired by the following projects:

- [xv6](https://github.com/mit-pdos/xv6-public)
- [xv6-x86_64](https://github.com/tiqwab/xv6-x86_64)
- [hello-x86_64](https://github.com/fwmiller/hello-x86_64)
- [KVM-Unit-Tests](https://gitlab.com/kvm-unit-tests/kvm-unit-tests/)
- [kvmtool](https://github.com/kvmtool/kvmtool)
