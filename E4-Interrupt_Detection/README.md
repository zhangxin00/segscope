# Compiler-Assisted Interrupt Detection

Repurposes SegScope as a **defence primitive**: an LLVM compiler pass instruments security-critical code regions with SegScope-based prepare/check pairs so that victim applications can detect interrupt-driven interference at run time without kernel modifications.

## Overview

The LLVM module pass (`intmon-branch`) performs static taint analysis to identify secret-dependent conditional branches, then inserts lightweight timing checks around target instructions. Three instrumentation granularities are supported:

| Mode | Scope | Description |
|------|-------|-------------|
| **Full** | Entire module | Every eligible instruction is wrapped with prepare/check pairs (research upper bound). |
| **Branch** | Secret region | Only instructions between a secret-dependent branch and its post-dominator join point are instrumented. |
| **Block** | Syscall boundaries | Prepare/check pairs are inserted only around syscall-like calls within the secret region (lowest overhead). |

We evaluate on four real-world applications with known secret-dependent branches: MbedTLS 2.6.1, MbedTLS 3.6.1, WolfSSL 5.7.2, and Libjpeg 9f. An IPI (Inter-Processor Interrupt) kernel module provides controlled interrupt injection for reproducible stress testing.

## Quick Start

To run the complete E4 benchmark matrix with one command:

```bash
sudo ./run_all.sh
```

The script automatically handles LLVM 18 setup, first-run benchmark downloads, IPI module preparation, and writes all results into `E4-Interrupt_Detection/results/`.

In the current benchmark matrix, all `api` modes are designed as minimal complete algorithm flows:

- RSA: public operation + private operation round-trip
- DH: key generation/public exchange/shared-secret derivation
- ECDSA: sign + verify

For offline/internal-network environments, `run_all.sh` and `known_cases.sh` will prefer local tarballs when present:

- `E4-Interrupt_Detection/clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04.tar.xz`
- `E4-Interrupt_Detection/distfiles/mbedtls-2.6.1.tar.gz`
- `E4-Interrupt_Detection/distfiles/mbedtls-3.6.1.tar.gz`
- `E4-Interrupt_Detection/distfiles/wolfssl-5.7.2.tar.gz`
- `E4-Interrupt_Detection/distfiles/jpegsrc.v9f.tar.gz`

If the script installs the kthread IPI module itself, it will automatically uninstall that module after the matrix finishes.

## Prerequisites

* Linux x86\_64 (bare-metal recommended)
* [LLVM 18](https://github.com/llvm/llvm-project/releases/tag/llvmorg-18.1.8) (`clang`, `opt`, `llvm-dis` from the official 18.1.8 tarball or system LLVM 18)
* CMake 3.16+, GCC/G++ (for kernel module and runtime)
* Linux kernel headers (for IPI kernel module, optional)

## Step 1: Build the LLVM Pass Plugin

```
mkdir -p build && cd build
cmake -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm ..
cmake --build .
cd ..
```

This produces `build/AutoInstrumentPass.so`.

## Step 2: Quick Verification — View Instrumented IR

Run the demo script to generate instrumented IR and inspect `__intmon_prepare`/`__intmon_check` insertion points:
```
./scripts/intmon_demo.sh
```

Expected output (excerpt):
```
call void @__intmon_prepare(i64 <branch_id>)
br i1 %cmp, label %if.then, label %if.else

if.then:
  call void @__intmon_check(i64 <branch_id>, i32 1)
  ...

if.else:
  call void @__intmon_check(i64 <branch_id>, i32 0)
  ...
```

## Step 3: Compile, Instrument, and Run

Compile a C program, instrument it with the pass, link the runtime, and execute:
```
./scripts/intmon_run.sh
```

The program prints interrupt detection events and timing statistics at exit, including total execution time and instrumented-function execution time.

Custom input:
```
./scripts/intmon_run.sh -i examples/hello.c -o /tmp/hello.intmon -r runtime/intmon_runtime.c
```

## Step 4: Evaluate Known CVE Cases

Run the full benchmark against four real-world cryptographic libraries and Libjpeg (downloads ~200 MB of third-party sources on first run):
```
./scripts/known_cases.sh
```

Skip download on subsequent runs:
```
./scripts/known_cases.sh --skip-download
```

Evaluate with IPI injection (requires kernel module, see Step 5):
```
./scripts/known_cases.sh --with-ipi
```

The script instruments each library with preset secret-line annotations, runs 100 iterations per program execution by default, and outputs TSV/summary files with binary size increase, total overhead (%), function-level overhead (%), and explicit metadata fields such as `library`, `version`, `algorithm`, `target_branch`, `instrument_mode`, and `ipi_mode`.

## Step 4.5: One-Click Full Matrix Execution

For the complete E4 benchmark matrix, run the top-level one-click script from inside `E4-Interrupt_Detection`:

```
sudo ./run_all.sh
```

This script:

- automatically checks LLVM 18 / Clang 18 / CMake dependencies and, when missing, downloads the official LLVM 18.1.8 prebuilt tarball:
  - `https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04.tar.xz`
- automatically triggers first-run downloads of third-party test programs (`mbedtls`, `wolfssl`, `libjpeg`) through `known_cases.sh`
- automatically checks `/dev/ipi_ctl` and attempts to install/load the IPI kernel module at the start
- automatically uninstalls the kthread IPI module at the end if the script installed it
- runs the full matrix:
  - instrumentation modes: `branch / once / block / full`
  - IPI modes: `noipi / kthread`
  - run modes: `api / core`
  - outer repeats: `100`

Outputs are written to:

- `E4-Interrupt_Detection/results/`
- `E4-Interrupt_Detection/build-base/`

The result root contains a `manifest.tsv` index plus per-combination directories of the form:

```
results/<variant>/<instr_mode>/rep_<N>/
```

Expected overhead ranges (branch mode, crypto workloads):
```
MbedTLS 2.6.1:  Tot 159–213%,  Func 358–360%
MbedTLS 3.6.1:  Tot  73–151%,  Func 194–195%
WolfSSL 5.7.2:  Tot  93–109%,  Func 206–238%
```

## Step 5 (Optional): IPI Kernel Module

Build and install the IPI kernel module for controlled interrupt injection:
```
./scripts/ipi_module.sh build
./scripts/ipi_module.sh install
```

Verify the device is available:
```
ls -l /dev/ipi_ctl
```

The module supports kernel-thread flood mode at a configurable rate (default 100 kHz). Uninstall when done:
```
./scripts/ipi_module.sh uninstall
```

## Directory Structure

```
.
├── CMakeLists.txt              # Build system for LLVM pass plugin
├── src/                        # LLVM pass implementations
│   ├── AutoInstrumentPass.cpp    # Instruction-level pass + plugin registration
│   └── IntMonBranchPass.cpp      # Branch monitoring pass (taint, divergence, IPI region)
├── include/AutoInstrument/     # Pass headers
├── runtime/                    # C runtime linked into instrumented binaries
│   ├── intmon_runtime.c          # TLS-based prepare/check, timing, count table
│   ├── intr_runtime.c            # Legacy auto-instrument runtime
│   └── intr_runtime.h
├── examples/                   # Test programs
│   ├── hello.c                   # Minimal demo
│   ├── secret_demo.c             # Secret-dependent branch demo
│   └── known_cases/              # Real-world test harnesses (MbedTLS, WolfSSL, Libjpeg)
├── scripts/                    # Build, instrument, and benchmark scripts
│   ├── intmon_demo.sh            # View instrumented IR
│   ├── intmon_run.sh             # Compile + instrument + run
│   ├── known_cases.sh            # Full benchmark runner
│   ├── run_all.sh                # Full matrix runner (100 repeats)
│   └── ipi_module.sh             # Build/install/uninstall IPI kernel module
├── kernel/ipi_kmod/            # IPI kernel module source
│   ├── ipi_kmod.c
│   └── Makefile
└── tools/ipi/
    └── ipi_sender.c            # Userspace IPI sender
```

## Key Command-Line Options

Pass options via `opt` directly or via `clang -mllvm`:

| Option | Description |
|--------|-------------|
| `-instrument-mode=all\|secret\|list` | Instrumentation scope (default: `all`) |
| `-secret-lines=<path>` | Mark secret branches by source line (`file:line`) |
| `-f2-at=succ\|join` | Where to insert check (successor entry or join point) |
| `-divergence=on\|off` | Instrument control-flow divergence points |
| `-emit-branch-map=<path>` | Output branch ID → source location mapping |

## Notes

* **LLVM 18 required** — the pass uses the new PassManager API (`PassInfoMixin`).
* **`run_all.sh`** prefers an existing system LLVM 18, otherwise downloads the official LLVM 18.1.8 tarball into `E4-Interrupt_Detection/llvm-18/`.
* **GS segment mode** (`--use-gs`) is available for bare-metal x86\_64 Linux only; use the default TLS mode in containers/VMs.
* **`known_cases.sh`** downloads third-party sources (~200 MB) on first run; use `--skip-download` afterwards.
* **`run_all.sh`** is the recommended one-click entry for E4 experiments and defaults to 100 outer repeats.
* **IPI kernel module** requires root privileges and kernel headers matching the running kernel.
