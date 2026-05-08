# kvmtool GDB Stub Architecture

## 1. Background and goals

kvmtool now provides a built-in GDB Remote Serial Protocol (RSP) stub for
guest kernel debugging on x86 and arm64.

Design goals:

1. Provide practical remote debugging (`target remote`) for `lkvm run`
2. Support breakpoints, single-step, register access, and memory access
3. Keep protocol handling generic and architecture-specific behavior isolated
4. Improve stepping stability in kernel-heavy interrupt contexts

---

## 2. Top-level architecture

```
+--------------------------------------------------------------+
|  Host                                                        |
|                                                              |
|  +---------+  GDB RSP over TCP   +------------------------+ |
|  |  GDB    | <------------------> |  kvmtool GDB stub      | |
|  | (client)|  localhost:PORT     |  (gdb.c / x86/gdb.c /  | |
|  +---------+                     |   arm/aarch64/gdb.c)   | |
|                                  +----------+-------------+ |
|                                             | KVM ioctls    |
|                                  +----------v-------------+ |
|                                  |  KVM vCPU threads      | |
|                                  |  KVM_EXIT_DEBUG        | |
|                                  |  KVM_SET_GUEST_DEBUG   | |
|                                  +----------+-------------+ |
|                                             |               |
|  +------------------------------------------v-------------+ |
|  |  Guest VM (Linux kernel/userspace)                     | |
|  +--------------------------------------------------------+ |
+--------------------------------------------------------------+
```

### 2.1 Generic layer (`gdb.c`)

Responsibilities:

- RSP packet transport and command dispatch
- stop-reply generation
- software/hardware breakpoint bookkeeping
- coordination between vCPU threads and the GDB thread
- guest virtual memory access with controlled translation fallback

### 2.2 Architecture layer (`x86/gdb.c`, `arm/aarch64/gdb.c`)

Responsibilities:

- map GDB register layout to KVM register interfaces
- program architecture debug controls (single-step / hw breakpoints)
- classify debug exit reasons
- apply architecture-specific resume fixes
  - x86: `RFLAGS` handling (`TF`/`RF` and step window IRQ behavior)
  - arm64: `PSTATE/DAIF` handling for single-step windows

---

## 3. Thread model and synchronization

Two cooperating runtime contexts:

1. **vCPU thread**
   - Executes `KVM_RUN`
   - On `KVM_EXIT_DEBUG`, enters `kvm_gdb__handle_debug()`

2. **GDB thread**
   - Accepts TCP connection from GDB
   - Runs packet-level debug sessions while guest is stopped
   - Decides resume behavior (`continue`, `step`, detach)

Synchronization primitives:

- `stopped_vcpu`: currently trapped vCPU
- `vcpu_stopped`: condvar for vCPU -> GDB notification
- `vcpu_resume`: condvar for GDB -> vCPU release
- VM-wide pause/continue via `kvm__pause()` / `kvm__continue()`

---

## 4. Control-flow highlights

### 4.1 Debug trap flow

```text
guest executes
 -> KVM_EXIT_DEBUG
 -> vCPU thread marks stopped_vcpu and waits
 -> GDB thread runs debug session and handles packets
 -> debug state is updated for resume
 -> vCPU is signaled and VM continues
```

### 4.2 Software breakpoint step-over flow

```text
hit software breakpoint
 -> restore original instruction bytes
 -> run single-step over current instruction
 -> reinsert software breakpoint bytes
 -> resume according to user command semantics
```

This avoids immediate retrap on the same breakpoint byte.

### 4.3 Step stability strategy

- x86: adjust resume flags before stepping and restore state after stop
- arm64: save and restore DAIF around the step window

Goal: reduce interrupt noise during `next/finish` style stepping without
changing guest behavior permanently.

---

## 5. Protocol support boundary

Core packet handling includes:

- `?`, `g/G`, `p/P`, `m/M`, `X`
- `Z/z` software/hardware breakpoints
- `c/s`, `C/S`
- `qSupported`, `qXfer:features:read`

Protocol safety hardening in the common layer includes:

- binary write length handling based on packet boundaries (not `strlen`)
- bounded thread-list formatting for `qfThreadInfo`

---

## 6. Practical boundaries

- Kernel stepping is inherently noisy under interrupts and scheduling
- For stable stepping sessions, prefer `-c 1` and `nokaslr`
- The architecture split is designed for maintainability and incremental
  extension of protocol features over time
