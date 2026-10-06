# Tuple-lifetime capability inventory

✓ Local SDK metadata and compiler ABI inventory recorded for x86/generic and
mediatek/filogic. This is configuration evidence, not a kernel load or JIT result.
The [work orders](TUPLE_LIFETIME_WORK_ORDERS.md) retain every required architecture.
No additional SDK or runtime has been acquired or guessed.

## Common requirements

One producer/consumer protocol applies to every target. Required kernel facilities
are AF_PACKET socket filters, BPF syscalls, hash and LRU hash maps, per-CPU arrays,
ring buffers, map lookup/update, monotonic time, skb byte loading (including the
network-relative helper), and ring reserve/submit without wakeups. Capability
failure must visibly disable optional TCP measurement while the daemon continues.

Userspace libbpf must expose `ring_buffer__consume_n`, `ring_buffer__ring`, and
`ring__avail_data_size`, plus the existing object/map/socket integration APIs.
Kernel configuration and library version alone do not prove program-type helper
availability. An actual load of the exact object and its execution-mode report
remain required. The SDK kernel trees inspected here contain configuration and
headers but omit the helper implementation sources needed for a source audit.

Shared records are 64 bytes with fixed offsets. Scalars use the matching host/BPF
byte order; addresses and ports retain network byte order. Per-CPU 32-bit fault
values use the kernel's eight-byte value stride. Neither native pointer width nor
CPU-specific atomic instructions define lifetime identity. The SDK's `bpf.mk`
selects `bpfel` or `bpfeb` from `CONFIG_BIG_ENDIAN`; the source algorithm is shared.

## Available target metadata

These values were read from the existing SDK `.config`, compiler preprocessor,
staged library files and kernel `.config`, without accessing a VM.

| Field | x86/generic | mediatek/filogic |
| --- | --- | --- |
| Package ABI | `i386_pentium4` | `aarch64_cortex-a53` |
| Compiler triplet | `i486-openwrt-linux-musl` | `aarch64-openwrt-linux-musl` |
| GCC / libc | 14.3.0 / musl | 14.3.0 / musl |
| Pointer / long / long long bytes | 4 / 4 / 8 | 8 / 8 / 8 |
| Compiler byte order | Little-endian | Little-endian |
| BPF object byte order | `bpfel` | `bpfel` |
| Local SDK kernel | 6.12.94 | 6.12.94 |
| BPF, BPF_SYSCALL, BPF_JIT, HAVE_EBPF_JIT | Enabled | Enabled |
| PACKET, INET, IPV6 | Enabled | Enabled |
| BPF_JIT_DEFAULT_ON | Not set | Enabled |
| BPF_JIT_ALWAYS_ON | Not set | Not set |
| Staged libbpf / libelf / zlib | 1.6.2 / 0.192 / 1.3.1 | 1.6.2 / 0.192 / 1.3.1 |
| Bounded ring APIs in headers | Present | Present |
| New package build / target ABI assertions | ✓ Pass | ✓ Pass |
| New verifier / actual JIT / kernel cases | Pending | Pending |
| Available runtime | Authorized x86 VM; no live refresh in this inventory | Authorized emulated arm64 VM; functional only |

The arm64 runtime is `armsr/armv8`, not mediatek/filogic hardware. It can check the
Filogic userspace ABI and common filter behavior, not platform-specific runtime
coverage or CPU cost. Historical x86 VM kernel 6.12.108 must not be confused with
the local SDK's 6.12.94. Live target configuration and execution mode must be
recorded during the planned preflight before interpreting results.

Kernel configuration identities:

- x86: `build_dir/target-i386_pentium4_musl/linux-x86_generic/linux-6.12.94/.config`,
  SHA-256 `74ee6b16b9e205afbd32a8458e41216a4e900b736c93b7365479f61acc8f19b0`.
- Filogic: `build_dir/target-aarch64_cortex-a53_musl/linux-mediatek_filogic/linux-6.12.94/.config`,
  SHA-256 `0a5f3999395978e021d183ecc3410869b5e97442b20e096110ca5fdc7a56dbb1`.

## Packaged object and instruction checks

✓ Both existing package builds passed for production checkpoint `885f7d9`, with
unchanged version `0.2.11-r1`. The shared header's fixed size/offset assertions
compiled with both native target compilers and BPF compilers. APK extraction
confirmed each packaged BPF object is byte-identical to its staged counterpart.
The packaged native executable is stripped by the SDK; its hash differs from
its unstripped staging copy, so use the APK-extracted identity below.

| Artifact | x86 SHA-256 | Filogic SHA-256 |
| --- | --- | --- |
| APK | `0cad228933b8e35b606bfab503ee0f958d0cfc84f914ac770edd1c68f4df43ff` | `f47408709fd694c7961edeb2112b07afddddf8d2a046e01491903a7176d65f1f` |
| Packaged filter | `1c8b9082581e838ea898f4c02fca2e3e23a0d0bcaeb8e6b3b7daf68fda78de22` | `485b2af48ca5527998e1f26ef3c9d160b46357f9220dc14a1c0d7e210f30431f` |
| Packaged executable | `08fb16730831d8b0d8a46bd27b8645f1546b08a7f6f7fe7741d388c99dd7bdfd` | `83b8837613cb173ac158af6e0fb686a572b782d42e1e46c1b9f71703e323110f` |

Packaged filter sizes are 28,928 / 28,952 bytes; native executables are
110,597 / 102,409 bytes (x86 / Filogic). These are sizes, not runtime memory cost.

✓ Direct ELF instruction decoding and SDK `llvm-objdump -d` inspection found
868 eight-byte instruction slots in each object's `socket` section. Neither
contains a BPF atomic store, spin-lock/unlock helper or BPF-to-BPF call. Calls
use only helper IDs 1, 2, 5, 26, 68, 131 and 132: map lookup/update, monotonic
time, skb byte loading, network-relative skb loading and ring reserve/submit.
The inspected SDK UAPI maps these IDs to the required helpers. This establishes
the emitted instruction inventory, not verifier acceptance or actual JIT mode.

The Filogic first build failed before compilation when its prepared-stamp hash
changed. The SDK derives that hash from the whole package checkout; writing
build logs inside it or changing tools/docs during the build changes the input.
✓ The scoped retry passed with checkout writers idle and logs outside the
checkout. Future SDK builds must keep those inputs stable. No production-code
or SDK-policy change was needed, and the passing x86 build was reused.

## Missing target resources

The following rows are required investigation slots, not verified target
selections. A package ABI name alone does not pin a target, SDK or kernel.

| Required family / representative ABI | Missing information and resources |
| --- | --- |
| x86-64 / `x86_64` | Exact target/subtarget SDK, kernel config, build and runtime evidence. |
| ARM32 little-endian / `arm_cortex-a7` | Exact SDK/target/compiler/kernel config and runtime evidence. |
| ARM32 big-endian / `armeb_xscale` | Exact SDK/target/compiler/kernel config, helper/JIT capability and runtime evidence. |
| MIPS32 big-endian / `mips_24kc` | Exact SDK/target/compiler/kernel config and runtime evidence. |
| MIPS32 little-endian / `mipsel_24kc` | Exact SDK/target/compiler/kernel config and runtime evidence. |
| MIPS64 big-endian / `mips64_mips64r2` | Exact SDK/target/compiler/kernel config and runtime evidence. |
| MIPS64 little-endian / `mips64el_mips64r2` | Exact SDK/target/compiler/kernel config and runtime evidence. |
| PowerPC32 big-endian / `powerpc_8548` | Exact SDK/target/compiler/kernel config and runtime evidence. |
| PowerPC64 big-endian / `powerpc64_e5500` | Exact SDK/target/compiler/kernel config and runtime evidence. |
| RISC-V64 little-endian / `riscv64_generic` | Exact SDK/target/compiler/kernel config and runtime evidence. |
| LoongArch64 little-endian / `loongarch64_generic` | Exact SDK/target/compiler/kernel config and runtime evidence. |

To close a missing row, supply the exact OpenWrt 25.12 SDK directory and target
kernel configuration, then a representative runtime or emulator for kernel cases.
At least one big-endian kernel with 32-bit ABI coverage must exercise actual
packet parsing and map/ring transfer. Compiling `bpfeb` alone cannot close that
runtime gap. Nothing in this inventory removes a missing row from the support
requirement or declares broad OpenWrt support complete.

## Acceptance limits fixed before target runs

✓ The primary freezes the existing design's proposed limits for the planned
acceptance batches: 1,024 retained lifecycle slots, 8,192 departure entries,
eight pairs per tuple, native cache at most 1 MiB, a 256 KiB ring, at most 4,096
records drained per call, a one-second recovery cooldown and 64 epochs per
capture. No spins, acquisition retries or deletion of attached loss markers.

With uninterrupted qualifying timestamp traffic, fresh usable calibration must
recover within four seconds of detected recoverable loss. Missing qualifying
traffic or independent confidence does not imply an unconditional recovery bound.
Steady eligible-flow coverage must retain at least 90% of the same-workload
baseline. Forced internal evidence loss must withdraw confidence even if that
reduces coverage.

The later identical-workload, identical-mode cost comparison permits at most
twice baseline BPF time per call and at most two additional CPU percentage points
of one core for combined BPF/daemon cost. Keep raw call counts, execution mode,
affinity, sample loss and qualified memory accounting. These are acceptance limits,
not measurements; do not relax them after seeing a failed batch. Existing latency
and approximately 85%-capacity requirements, and the separate ACK-control
regression, remain unchanged.
