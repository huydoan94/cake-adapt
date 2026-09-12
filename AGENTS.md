# sqm-mon — Project Instructions

## Project purpose

`sqm-mon` is an OpenWrt-native C program intended to progressively replace:

1. `cake-autorate`
2. the relevant orchestration currently provided by `sqm-scripts`

CAKE itself remains a Linux kernel qdisc. We are **not** reimplementing CAKE or packet scheduling in userspace.

The intended progression is:

```text
observe
  -> verify
  -> control
  -> replace cake-autorate
  -> replace sqm-scripts
```

Do not jump directly to later phases before the earlier phase is working and verified.

---

## High-level architecture

Keep Linux/OpenWrt-specific machinery separated from the autorate algorithm.

Initial source layout:

```text
sqm-mon/
├── AGENTS.md
├── Makefile
├── .gitignore
├── .vscode/
│   ├── settings.json
│   └── tasks.json
│
├── src/
│   ├── Makefile
│   ├── main.c
│   ├── config.c
│   ├── config.h
│   ├── controller.c
│   ├── controller.h
│   ├── cake.c
│   ├── cake.h
│   ├── traffic.c
│   ├── traffic.h
│   ├── latency.c
│   ├── latency.h
│   ├── ingress.c
│   ├── ingress.h
│   ├── netlink.c
│   ├── netlink.h
│   ├── log.c
│   └── log.h
│
├── files/
│   ├── sqm-mon.init
│   └── sqm-mon.config
│
└── tests/
    ├── Makefile
    ├── test_controller.c
    ├── test_config.c
    └── test_helpers.c
```

Keep `src/` flat unless a subsystem genuinely becomes large enough to justify its own directory.

Do not create unnecessary abstraction layers or libraries.

---

## Module responsibilities

### main.c

Only top-level program lifecycle and orchestration.

Responsibilities may include:

* startup
* initialization
* event loop
* signal handling
* orderly shutdown
* coordinating modules

Do not put CAKE implementation, UCI parsing, probing logic, or autorate policy directly in `main.c`.

### config.c / config.h

OpenWrt configuration handling.

Use OpenWrt's provided configuration libraries, primarily `libuci`.

Do **not** manually parse `/etc/config/sqm-mon`.

Configuration should provide interface names and other device-specific values. Do not hardcode a WAN interface such as `vlw`.

### controller.c / controller.h

This is the core autorate policy.

It must remain as independent as practical from:

* OpenWrt
* UCI
* netlink
* `tc`
* sockets
* IFB
* CAKE implementation details
* filesystem state

The controller should accept measured state as input and return decisions as output.

It must be possible to unit-test the controller on a normal development machine with synthetic inputs.

Example conceptual boundary:

```c
struct controller_input {
    uint64_t rx_rate;
    uint64_t tx_rate;
    uint32_t baseline_rtt;
    uint32_t current_rtt;
    uint32_t current_download_rate;
    uint32_t current_upload_rate;
};

struct controller_output {
    uint32_t download_rate;
    uint32_t upload_rate;
};
```

The exact API may evolve, but preserve this architectural separation.

### cake.c / cake.h

CAKE-specific policy and qdisc operations.

Examples:

* find existing CAKE qdisc
* read CAKE state/statistics
* set/change bandwidth
* create/delete CAKE when sqm-mon later takes ownership of setup

Do not put raw low-level netlink message construction here when it can live in `netlink.c`.

### netlink.c / netlink.h

Low-level rtnetlink communication.

Long-term preference is direct netlink rather than repeatedly spawning:

```text
tc
ip
```

However, do not introduce a large custom netlink framework prematurely.

Use existing supported Linux/OpenWrt APIs or libraries when they are suitable.

### traffic.c / traffic.h

Traffic/load measurements only.

Keep measurement separate from controller policy.

### latency.c / latency.h

Latency measurement/probing only.

Keep probe mechanics separate from controller decisions.

### ingress.c / ingress.h

Own the ingress shaping path once sqm-mon replaces SQM setup.

The target ingress path is:

```text
WAN ingress
    |
    v
ctinfo zone 0
    |
    | restore DSCP from conntrack
    v
mirred redirect
    |
    v
IFB
    |
    v
CAKE
```

This is important because the deployment uses restored DSCP before traffic reaches ingress CAKE.

---

## Existing DSCP design that must be preserved

The network already stores outbound DSCP in the lower six bits of the conntrack mark.

Other conntrack mark bits must remain untouched.

Conceptually:

```text
bits 0-5   = DSCP
bits 6-31  = preserve existing value
```

Existing nftables logic follows the equivalent of:

```text
ct mark = (ct mark & 0xffffffc0) | dscp
```

Do not casually reuse other conntrack mark bits.

Ingress currently uses a `tc` action sequence equivalent to:

```text
ctinfo zone 0 pipe
mirred redirect to IFB
```

The restored DSCP must be visible to CAKE.

Do not replace this with ordinary SQM ingress redirection and accidentally remove DSCP restoration.

---

## Relationship with sqm-scripts

`sqm-scripts` is primarily shell orchestration around kernel facilities.

We do not need compatibility with every historical SQM script.

`sqm-mon` only needs to support the configuration and behavior we deliberately choose.

Do not attempt to clone all of `functions.sh` or reproduce every feature of `sqm-scripts`.

The objective is a smaller native implementation for the required use case.

---

## Relationship with cake-autorate

The first meaningful functional replacement is the continuously running autorate controller.

Initially, existing SQM may continue creating CAKE and IFB while `sqm-mon` only observes or adjusts an already-created CAKE instance.

Only after that behavior is verified should sqm-mon assume ownership of:

* CAKE creation
* IFB creation
* ingress redirect
* ctinfo action
* cleanup

---

## OpenWrt conventions

Prefer OpenWrt-provided libraries and APIs instead of custom parsers or duplicated functionality.

Examples:

* use `libuci` for UCI configuration
* use procd for service lifecycle
* use kernel netlink interfaces where appropriate
* use existing kernel CAKE, IFB, ctinfo and mirred facilities

Avoid adding daemons or external runtime dependencies unless there is a clear reason.

Keep package dependencies minimal.

Before adding a dependency, check whether OpenWrt already provides the needed functionality.

---

## Build system

Use plain Make.

There are two distinct Makefiles:

```text
sqm-mon/Makefile
```

OpenWrt package definition.

And:

```text
sqm-mon/src/Makefile
```

actual program compilation.

The source should remain buildable/testable outside the OpenWrt SDK where practical.

Do not make ordinary host unit tests depend on rebuilding the entire OpenWrt toolchain.

In previous OpenWrt work, unnecessary host-tool builds caused significant wasted work. Avoid broad OpenWrt targets when a narrower package/test build is sufficient.

---

## Compiler warnings

Start strict.

Preferred baseline:

```text
-Wall
-Wextra
-Wpedantic
-Wformat=2
-Wshadow
-Wconversion
-Werror
```

If a warning needs to be disabled, do it for a specific documented reason rather than broadly weakening warning policy.

---

## C coding conventions

Use C, not C++.

Prefer standard/library-supported types, constants, enums and APIs instead of creating custom equivalents without need.

Avoid custom wrappers when the platform/library already provides a clear abstraction.

Keep code simple and explicit.

Avoid clever macros unless they genuinely improve correctness or reduce unavoidable repetition.

### Function calls

When a function call has multiple substantial arguments, put the arguments on separate lines.

Preferred:

```c
result = controller_update(
    controller,
    &input,
    &output
);
```

Avoid:

```c
result = controller_update(controller, &input, &output);
```

for non-trivial multi-argument calls.

### Function declarations and definitions

Use the same multiline style.

Preferred:

```c
int controller_update(
    struct sqm_mon_controller *controller,
    const struct controller_input *input,
    struct controller_output *output
)
{
```

Do not force a traditional fixed source width just to wrap lines.

Readability determines wrapping.

### Variable declarations

Do not split ordinary variable declarations unnecessarily.

Preferred:

```c
struct sqm_mon_controller controller;
```

Avoid:

```c
struct sqm_mon_controller
    controller;
```

### Formatting

There is no arbitrary strict file-width rule.

Do not reformat working code solely to satisfy an old 80-column convention.

Keep changes focused.

---

## Naming

Project/executable/package name:

```text
sqm-mon
```

C identifiers should use underscore form.

Prefer project-specific names for public/internal structures where ambiguity is possible:

```c
struct sqm_mon_config;
struct sqm_mon_controller;
```

Avoid unnecessarily generic global identifiers.

Do not add a blanket `sqm_mon_` prefix to functions. Prefer concise,
module-oriented names such as:

```c
config_load();
log_message();
controller_update();
```

Add a project-specific function prefix only when it prevents a concrete naming
collision or ambiguity.

Do not put a redundant `sqm-mon:` prefix into every log message if the logging backend/service already identifies the program.

---

## Logging

Logging should be centralized in `log.c`.

Do not scatter different logging mechanisms across modules.

Logs should provide useful operational information without becoming noisy in normal operation.

Important state changes should be observable, especially:

* startup
* configuration loaded
* interfaces discovered
* CAKE discovered/created
* rate change
* measurement failure
* netlink failure
* degraded operation
* shutdown

Verbose/high-frequency measurement logging should be optional.

When setting kernel state, important configuration should be read back or otherwise verified where practical rather than assuming a successful syscall means the resulting configuration is exactly what was intended.

---

## Failure handling

Do not terminate the whole daemon merely because one optional operation or one monitored object failed.

Where safe, continue operating with the portions that are available and clearly log degraded state.

Fatal errors should be reserved for conditions where continued operation would be incorrect or dangerous.

Timeouts should exist around operations that could otherwise wait indefinitely.

Do not "wait and hope" forever for interfaces or kernel objects to appear.

---

## Configuration safety

Do not embed machine-specific absolute paths in committed files.

Do not commit developer usernames or home-directory paths.

Do not hardcode SDK paths.

VS Code configuration should use workspace-relative paths, environment variables, or generated configuration where appropriate.

For C language tooling, prefer generating:

```text
compile_commands.json
```

rather than maintaining long manual include-path lists.

Do not commit environment-specific generated files unless we intentionally decide they are portable.

---

## Testing philosophy

Unit tests are a first-class part of the project.

The controller should receive especially thorough testing, including:

* idle behavior
* sustained download
* sustained upload
* simultaneous upload/download
* clean low-latency load
* congestion
* sudden RTT increase
* baseline RTT changes
* recovery after congestion
* rate increase behavior
* rate decrease behavior
* min/max limits
* invalid samples
* missing probes
* counter rollover/reset where relevant
* startup state
* transitions between operating states

Prefer testing branches and behavior rather than merely increasing line coverage.

### Important rule

Do not modify correct production logic just because a host-side unit-test environment lacks some OpenWrt/kernel feature.

If a test cannot exercise production code because of an environment or API mismatch, first find a better test strategy, mock, adapter, or appropriate host dependency.

Production code should not be weakened merely to make tests convenient.

---

## Dependency review

Before considering a feature complete, review its dependencies.

Remove libraries/packages that are no longer required.

Do not leave dependencies in the OpenWrt package simply because they were useful during development.

---

## Troubleshooting convention

Before changing known-working code/configuration in response to a surprising observation, verify the observation once.

This is especially important when:

* logs appear contradictory
* a package appears missing
* configuration may not have been reloaded
* generated output may be stale
* an issue may be caused by checking the wrong profile/device/build

Do not rush into a "fix" that changes working behavior before confirming the problem exists.

---

## Change discipline

Prefer small, reviewable steps.

Do not perform broad unrelated refactors while implementing a targeted feature.

When changing behavior:

1. identify the existing behavior
2. explain what will change
3. make the smallest appropriate change
4. build
5. test
6. verify runtime/kernel state where applicable

Do not silently change architectural decisions.

If a proposed implementation conflicts with this file, call out the conflict before changing direction.

---

## Cost-aware delegation

When testing or deployment is part of the user's request, delegate routine
verification and deployment to one subagent using `gpt-5.6-luna` with low
reasoning effort.

The delegated work may include:

* running the x86 and Filogic SDK tasks from `.vscode/tasks.json` concurrently
* locating and copying the x86 APK
* deploying it to an authorized OpenWrt test VM
* running the prescribed smoke or stress tests
* collecting logs, exit codes and relevant runtime state

Give the subagent only the context needed for these operations. Use a
minimal-context fork, preferably `fork_turns: "none"`, rather than copying the
entire conversation.

Keep these responsibilities on the primary model:

* architecture and production-code changes
* diagnosing unexpected or ambiguous failures
* destructive or otherwise unapproved operations
* interpreting results and making the final acceptance decision

Use one combined verification subagent where practical. Do not create multiple
agents merely for individual shell commands. Do not store credentials in this
file.

### OpenWrt VM test log

Use only this file for deployed runtime test logging:

```text
/tmp/sqm-mon-test.log
```

At the start of each test, stop the process that writes the log, then truncate
the existing file in place:

```sh
: > /tmp/sqm-mon-test.log
```

Do not delete, move or replace this file. Preserving the same file allows the
user's existing `tail -f /tmp/sqm-mon-test.log` process to remain attached.
Do not start or terminate that `tail` process. Configure `sqm-mon` to write
directly to this file; do not use a second log file or `tee`.

---

## Initial development milestone

The first runnable `sqm-mon` should **not modify CAKE**.

Initial behavior:

```text
start
  -> initialize logging
  -> load /etc/config/sqm-mon with libuci
  -> validate configuration
  -> enter event loop
  -> respond cleanly to SIGTERM/SIGINT
  -> shut down
```

Then add observation:

```text
read traffic
read latency
read current CAKE configuration/statistics
```

Still no modification.

Only after observation is verified should rate adjustment be enabled.

---

## Design principle

Keep these four concerns distinct:

```text
measurement
    |
    v
controller decision
    |
    v
desired CAKE state
    |
    v
kernel/netlink implementation
```

A measurement module should not decide rates.

The controller should not know how netlink works.

The netlink layer should not contain autorate policy.

The CAKE layer should not implement latency measurement.

Preserve these boundaries unless there is a strong technical reason not to.
