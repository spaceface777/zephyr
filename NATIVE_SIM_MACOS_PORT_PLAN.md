# Native Simulator Mach-O Port Report and Plan

## Document status

- Status: Apple Silicon `hello_world` milestone implemented
- Target: `native_sim//64` on Apple Silicon macOS
- First application: `samples/hello_world`
- Primary command: `west build -p always -b native_sim//64 samples/hello_world`
- Primary run target: `west build -t run`
- Scope owner: not assigned

This file is a working report and implementation checklist. If a task state changes or new evidence appears, update this file.

## Objective

Port the Zephyr native simulator to Mach-O without bypassing Zephyr kernel behavior.

The first milestone must compile and run `samples/hello_world` as a native macOS process. The process must use the Zephyr scheduler and initialization path.

The port must not print a hard-coded sample message. It must not replace Zephyr with a host-only test program.

## Initial scope

The first implementation targets these components:

- Apple Silicon (`arm64`)
- a 64-bit Mach-O executable
- Apple Clang and the Apple linker
- the default minimal C library used by `samples/hello_world`
- one simulated MCU
- the console and native simulator timer
- the standard Zephyr initialization sequence
- the native simulator task sequence

The first implementation does not include these components:

- Intel macOS
- 32-bit targets
- multiple simulated MCUs
- networking and TAP devices
- external C or C++ libraries
- static host linking
- coverage
- `gprof`
- sanitizers
- libFuzzer
- all `native_sim` tests

These exclusions reduce the first milestone. The design must not prevent later support for these components.

## Executive report

The Linux host gate is not the only blocker. The current implementation depends on the ELF object format and GNU linker behavior.

The native simulator runtime mostly uses portable POSIX interfaces. Its build and registration systems are not portable to Mach-O.

The investigation passed four early failures with temporary prototypes. The fifth failure exposed the main architecture problem.

The temporary investigation changes were removed before implementation started. The current worktree now contains the experimental port.

## Implemented milestone

The experimental Apple Silicon path now completes the requested pristine build. It creates these artifacts:

```text
build/zephyr/zephyr.elf: Mach-O 64-bit object arm64
build/zephyr/zephyr.exe: Mach-O 64-bit executable arm64
```

The executable starts the Zephyr kernel and prints this output:

```text
*** Booting Zephyr OS build v4.4.1 ***
Hello World! native_sim/native/64
```

The implementation uses these mechanisms:

- a Darwin Zephyr linker backend
- Apple linker section remapping and order files
- Mach-O section start and end symbols
- runtime priority metadata for Zephyr initialization entries
- Apple linker symbol localization
- Darwin pthread stack and thread-name adapters
- a pthread condition-variable semaphore adapter
- Mach-O absolute-symbol extraction through Apple `nm`

The following checks pass:

- the exact pristine build command
- direct execution with a bounded simulated stop time
- `west build -t run`
- 100 repeated direct runs
- a focused reversed-priority initialization test
- a focused static semaphore iterable test
- Mach-O and ELF absolute-symbol extraction probes
- three build-script unit tests
- a clean build with no compiler or linker warnings

## Current limitations

This implementation completes the first `hello_world` milestone. It does not complete every expansion task in this file.

- The Mach-O iterable-section map contains core types needed by the initial scope.
- An application with an unmapped iterable type stops during compilation.
- The generic Mach-O data-section fallback does not preserve all specialized linker-section semantics.
- The ELF-only `check_init_priorities.py` post-link check is disabled for the Darwin backend.
- The kernel enforces Mach-O initialization priority from stored runtime metadata instead.
- Multiple simulated MCUs remain unsupported.
- Networking, coverage, sanitizers, and filesystem samples remain untested.
- Darwin can set only the current pthread name through the available API.
- Legacy OpenOCD alias symbols are not emitted in Mach-O builds.
- Linux behavior is isolated by compile and CMake conditions, but Linux regression tests remain pending.

## Investigation environment

The investigation used this host:

```text
Host OS: Darwin
Host architecture: arm64
Compiler: Apple clang 21.0.0
Object format: Mach-O 64-bit arm64
CMake: 4.4.3
Zephyr: 4.4.1
Zephyr SDK host tools: 1.0.1
```

The command selected `/usr/bin/gcc`. On macOS, that path runs Apple Clang.

CMake reported the toolchain as `host (gcc/ld)`. This text does not mean that GNU GCC or GNU `ld` is active.

## Investigation findings

### F-01: The host gate rejects all macOS builds

`arch/posix/CMakeLists.txt` rejects Apple hosts before compilation starts.

```cmake
if(NOT CMAKE_HOST_UNIX OR CMAKE_HOST_APPLE)
  message(FATAL_ERROR ...)
endif()
```

This gate prevents unsupported builds. Removal of this gate does not provide macOS support.

### F-02: Apple Silicon has an incorrect host mapping

The host mapping tests `arm.*` before it tests a 64-bit ARM name.

`CMAKE_HOST_SYSTEM_PROCESSOR` is `arm64` on this host. The current expression maps `arm64` to the 32-bit `arm.cmake` file.

The result is this error:

```text
CONFIG_64BIT=y while targeting a 32-bit ARM processor.
```

The mapping must recognize `arm64` and `aarch64` before 32-bit ARM names.

### F-03: Generated constants require ELF

`GEN_ABSOLUTE_SYM()` in `include/zephyr/toolchain/gcc.h` emits the ELF `.type` assembler directive.

The Mach-O assembler rejects this directive:

```text
error: unknown directive
.type ___z_heap_struct_SIZEOF,@object
```

Mach-O accepts global absolute symbols with `.globl` and `.equ`. External Mach-O symbols also use a leading underscore.

`scripts/build/gen_offset_header.py` opens each constants object with `pyelftools`. The script rejects a Mach-O object before symbol extraction.

A temporary prototype emitted Mach-O absolute symbols. Another prototype extracted the symbols with `nm`.

These prototypes generated `heap_constants.h` and `offsets.h`. This result proves that this blocker has a bounded solution.

### F-04: The Darwin AArch64 `va_list` ABI differs from the Linux ABI

`lib/os/cbprintf_packaged.c` selects one AArch64 `va_list` layout for all AArch64 targets.

The Linux AArch64 layout is a 32-byte structure. On this macOS host, `sizeof(va_list)` is 8 bytes.

The build stopped at this assertion:

```text
sizeof(char *) == sizeof(struct __va_list)
architecture specific support is wrong
```

The existing pointer-style implementation matches the observed Darwin ABI. A temporary target condition passed this compile stage.

This change needs focused `cbprintf` tests. A size match alone does not prove correct argument decoding.

### F-05: Mach-O section declarations differ from ELF declarations

Zephyr places initialization entries and iterable data in named ELF sections.

For example, `Z_INIT_ENTRY_SECTION()` creates names such as:

```text
.z_init_PRE_KERNEL_1_P_0_SUB_0_
```

Mach-O section attributes require this form:

```text
segment,section
```

Apple Clang rejected Zephyr and native simulator declarations with this error:

```text
mach-o section specifier requires a segment and section separated by a comma
```

Mach-O limits each segment name and section name to 16 bytes. Many Zephyr ELF section names exceed this limit.

A string replacement from `section` to `segment,section` is not sufficient. The current names also encode type, priority, and sort order.

### F-06: Zephyr depends on contiguous iterable ranges

`include/zephyr/sys/iterable_sections.h` exposes each iterable set as a contiguous array.

Callers subtract start and end addresses. Other callers index from a start address.

Examples include these operations:

- static device handles
- kernel object initialization
- static thread initialization
- logging source indexes
- initialization entries

The Mach-O port must preserve these properties:

1. Each set contains objects of one compatible type.
2. Each set has stable start and end symbols.
3. No unrelated object occurs inside the range.
4. Required entries survive dead stripping.
5. Entries use the required alignment.
6. Ordered sets have deterministic order.

A generic runtime list of pointers does not preserve these properties. Such a design requires broad kernel changes.

### F-07: Native simulator tasks depend on GNU linker scripts

`NSI_TASK()` stores function pointers in priority-coded sections.

`scripts/native_simulator/common/other/linker_script.pre.ld` performs these operations:

- keeps each task section
- sorts task sections by numeric priority
- combines tasks into one range
- creates a boundary for each task level
- creates a final range boundary

`scripts/native_simulator/common/src/nsi_tasks.c` walks the generated pointer ranges directly.

Mach-O needs an equivalent registry. This registry is separate from the Zephyr kernel initialization registry.

### F-08: The two-stage link depends on GNU tools

The Zephyr image and the native simulator runner use separate link stages.

The first stage creates the embedded Zephyr image. The second stage localizes image symbols and links the host runner.

`scripts/native_simulator/Makefile` uses these GNU operations:

```text
objcopy --localize-hidden
objcopy --localize-symbol=<pattern>
-Wl,--whole-archive
-Wl,--no-whole-archive
-Wl,--gc-sections
-T <linker-script>
```

The final linker scripts use `KEEP` and `INSERT AFTER`.

Apple `ld` rejected the tested GNU options:

```text
ld: unknown options: --gc-sections --whole-archive --no-whole-archive -T
```

The port needs a Darwin link path. Flag substitution alone cannot process GNU linker scripts.

### F-09: Symbol isolation is a functional requirement

The embedded Zephyr image uses hidden visibility by default. Selected `NATIVE_SIMULATOR_IF*` symbols remain visible to the runner.

The current build localizes hidden and private symbols before the final link. This step prevents collisions with runner and host symbols.

The port must preserve this boundary. Removal of localization can cause silent symbol interposition or duplicate definitions.

This concern exists for `hello_world`. It does not depend on the networking stack.

### F-10: Some runtime APIs need Darwin adapters

Most reviewed runtime calls exist on macOS:

- `pthread_create()`
- `pthread_detach()`
- `pthread_exit()`
- unnamed thread-local `sem_init()`
- `sem_wait()` and `sem_post()`
- `clock_gettime()`
- `isatty()`

The reviewed code also uses Linux-specific pthread extensions:

- `pthread_getattr_np()`
- the Linux form of `pthread_setname_np()`

Darwin provides different APIs for thread names and stack bounds. These differences need small host adapter functions.

`CONFIG_ARCH_POSIX_UPDATE_STACK_INFO` can remain disabled for the first milestone. The final port still needs a defined behavior for this option.

### F-11: Apple defines `__weak` as a language token

When no definition exists, Zephyr defines `__weak`.

Apple Clang already recognizes `__weak` as an ownership qualifier. As a result, Zephyr does not replace it with `__attribute__((weak))`.

The compiler produced warnings on Zephyr weak functions during the prototype build. This behavior can change symbol selection.

The port needs a narrow toolchain fix for `CONFIG_ARCH_POSIX` on Apple targets.

## Main architecture decision

The port needs a Mach-O registration and link backend. It does not need a new scheduler or native simulator time model.

The recommended design keeps the current C APIs and contiguous ranges. It replaces ELF-specific build operations only for Mach-O targets.

The first design spike must compare three approaches.

### Approach A: Mach-O sections plus generated order and boundary data

This approach maps registration objects into short Mach-O sections. A build tool generates ordering and boundary information.

Potential Mach-O mechanisms include:

- `section$start$SEGMENT$SECTION` symbols
- `section$end$SEGMENT$SECTION` symbols
- an `-order_file`
- `-force_load` or `-all_load`
- `-dead_strip`
- `no_dead_strip` attributes
- generated assembly aliases or boundary labels

Benefits:

- The existing contiguous-range APIs can remain intact.
- Most runtime code can remain unchanged.
- Linux keeps the current ELF backend.

Risks:

- The 16-byte section-name limit prevents direct use of current names.
- One common section needs atom ordering and generated boundaries.
- The Apple linker can add alignment padding between atoms.
- Archive extraction and dead stripping can remove indirect entries.
- The build tool must create deterministic names and collision detection.

### Approach B: Generated C or assembly tables

This approach scans registration metadata before the final link. It then creates explicit tables in generated source files.

Benefits:

- The tables can have exact order, type, and boundaries.
- The design does not depend on long Mach-O section names.
- Generated tables are easy to inspect in build artifacts.

Risks:

- The scan must discover every registration entry before the final link.
- Static symbols need a reference strategy.
- The build graph can require an additional link or generation pass.
- Generic iterable sections contain objects, not only pointers.
- Pointer tables do not satisfy existing contiguous-object APIs.

This approach fits native simulator task pointers better than generic Zephyr iterable objects.

### Approach C: Runtime constructor registries

This approach uses Mach-O constructors to register objects before `main()`.

Benefits:

- It uses native Mach-O initialization behavior.
- It avoids custom linker scripts for simple pointer registries.

Risks:

- Constructor priority behavior differs across toolchains.
- The registry runs before native simulator setup.
- Dynamic pointer lists break contiguous-object assumptions.
- Process constructors can hide ordering and lifetime errors.

Do not use this approach for generic Zephyr iterable sections. It remains an option for native simulator tasks after focused tests.

### Initial recommendation

Use Approach A for Zephyr iterable objects and initialization entries. Use Approach A or B for native simulator task pointers.

Do not select a final mechanism before the linker capability spikes pass. The spikes can disprove the initial recommendation.

## Design constraints

### D-01: Preserve Linux behavior

Linux must continue to use ELF, GNU linker scripts, and the current native simulator Makefile path.

Host-specific conditions must not change ordinary embedded cross-builds from macOS.

Use the target object format for decisions. Do not use `CMAKE_HOST_APPLE` as the only condition.

### D-02: Preserve Zephyr semantics

The port must preserve initialization levels, priorities, iterable ranges, weak symbols, and interface visibility.

A successful print from `main()` does not prove correct initialization order.

### D-03: Keep host ABI code separate

Place Darwin pthread and linker differences behind narrow host interfaces.

Do not add Darwin conditions throughout scheduler code.

### D-04: Use deterministic generated artifacts

Generated section maps, symbol lists, and order files must have stable content.

The build must report a collision instead of silently merging two logical ranges.

### D-05: Avoid extra package-manager requirements

The first port must prefer tools from Xcode, CMake, Python, and the Zephyr environment.

If an LLVM utility becomes mandatory, detect it during CMake configuration. Report its full path and required version.

### D-06: Keep symbol boundaries explicit

The final executable must expose only the interface symbols required by the runner.

The build must inspect this symbol set as an acceptance step.

## Work plan and checklist

### Phase 0: Record the baseline

- [ ] **P0-01** Record `sw_vers`, `uname -m`, `xcodebuild -version`, and `clang --version`.
- [ ] **P0-02** Record CMake values for the host processor, target triple, linker, archiver, `nm`, and object copy tool.
- [ ] **P0-03** Save the original guarded build log.
- [ ] **P0-04** Add a small script that identifies ELF and Mach-O objects by magic bytes.
- [ ] **P0-05** Define the minimum supported macOS and Xcode versions.
- [ ] **P0-06** Add a feature switch for experimental Mach-O support.
- [ ] **P0-07** Keep the default error for unsupported macOS configurations.

**Gate P0:** CMake reports the selected target format and every required host tool.

### Phase 1: Run Mach-O linker capability spikes

Create isolated probes outside the Zephyr build graph. Keep each probe small and executable.

- [x] **P1-01** Create a Mach-O object with one custom data section.
- [x] **P1-02** Read its synthesized `section$start` and `section$end` symbols.
- [x] **P1-03** Place objects from two archives into the same section.
- [x] **P1-04** Prove that `-force_load` retains unreferenced registration objects.
- [x] **P1-05** Prove that `-dead_strip` does not remove required registration atoms.
- [x] **P1-06** Measure padding between objects with different alignments.
- [x] **P1-07** Determine whether an order file orders data atoms.
- [ ] **P1-08** Determine how the linker handles duplicate order-file entries.
- [ ] **P1-09** Create boundary labels around ordered atoms.
- [x] **P1-10** Prove that pointer subtraction across the generated range is valid.
- [x] **P1-11** Test `ld -r` with hidden and default-visibility symbols.
- [x] **P1-12** Test exported and unexported symbol lists during a relocatable link.
- [ ] **P1-13** If `llvm-objcopy` is present, test its available Mach-O operations.
- [x] **P1-14** Write the selected link design in this document.

**Gate P1:** A probe preserves a typed range, deterministic order, retention, and symbol isolation.

If no probe passes Gate P1, stop implementation. Reassess generated tables or a larger iterable-section redesign.

### Phase 2: Add target detection and host configuration

- [ ] **P2-01** Detect Mach-O as a target property.
- [x] **P2-02** Map `arm64` and `aarch64` to `arch/posix/aarch64.cmake`.
- [x] **P2-03** Keep 32-bit ARM matching after the 64-bit match.
- [x] **P2-04** Allow supported Mach-O targets through the host gate.
- [x] **P2-05** Reject unsupported macOS architectures with a specific error.
- [ ] **P2-06** Select Apple Clang explicitly for the native target.
- [x] **P2-07** Select the Apple archiver, linker, and `nm` explicitly.
- [x] **P2-08** Separate GNU linker flags from Darwin linker flags.
- [x] **P2-09** Remove `-ldl` from the Darwin link path.
- [ ] **P2-10** Decide whether the Mach-O executable needs `-no_pie`.
- [ ] **P2-11** Add CMake configure tests for every Darwin flag.
- [ ] **P2-12** Keep Linux output unchanged in a CMake configuration comparison.

**Gate P2:** Configuration succeeds and emits valid Apple Clang compile commands.

### Phase 3: Support generated absolute constants

- [x] **P3-01** Add Mach-O syntax for `GEN_ABSOLUTE_SYM()`.
- [x] **P3-02** Add Mach-O syntax for `GEN_ABSOLUTE_SYM_KCONFIG()`.
- [x] **P3-03** Add the required external-symbol underscore only for Mach-O.
- [x] **P3-04** Extend `gen_offset_header.py` with an object-format dispatch.
- [x] **P3-05** Pass the selected `nm` path from CMake.
- [x] **P3-06** Parse a stable machine-readable output format.
- [x] **P3-07** Reject duplicate generated constant names.
- [x] **P3-08** Reject non-absolute matching symbols.
- [ ] **P3-09** Add unit tests for 32-bit and 64-bit Mach-O magic values.
- [x] **P3-10** Add a Mach-O constants object fixture or generate one during the test.
- [x] **P3-11** Keep the existing ELF extraction tests and behavior.
- [x] **P3-12** Compare generated heap and architecture offset headers with expected values.

**Gate P3:** `heap_constants.h` and `offsets.h` are correct on macOS and Linux.

### Phase 4: Implement Mach-O registration ranges

This phase contains the main port work.

- [x] **P4-01** Inventory all iterable ranges in the minimal `hello_world` configuration.
- [x] **P4-02** Inventory all initialization entries in the same configuration.
- [x] **P4-03** Assign short Mach-O segment and section names.
- [ ] **P4-04** Add a deterministic mapping from logical range names to Mach-O names.
- [ ] **P4-05** Detect mapping collisions during the build.
- [x] **P4-06** Preserve each range alignment.
- [x] **P4-07** Preserve numeric order where GNU scripts use `SORT`.
- [ ] **P4-08** Preserve lexical order where GNU scripts use `SORT_BY_NAME`.
- [x] **P4-09** Generate start and end symbols with the existing C names.
- [x] **P4-10** Retain registration objects without retaining unrelated code.
- [x] **P4-11** Support empty iterable ranges.
- [ ] **P4-12** Support read-only and read-write iterable ranges.
- [ ] **P4-13** Support the `symbol_to_keep` mechanism.
- [ ] **P4-14** Add an artifact report with each range name, address, size, and count.
- [ ] **P4-15** Add assertions for range size and element alignment.
- [ ] **P4-16** Add focused tests for `TYPE_SECTION_COUNT()`.
- [ ] **P4-17** Add focused tests for `TYPE_SECTION_GET()`.
- [ ] **P4-18** Add focused tests for `STRUCT_SECTION_FOREACH()`.
- [x] **P4-19** Add an initialization-order test with entries at several levels.
- [x] **P4-20** Add an initialization-priority test with one-digit and three-digit priorities.

**Gate P4:** A small Zephyr kernel image links with valid iterable ranges and initialization order.

### Phase 5: Port native simulator task registration

- [ ] **P5-01** Select Mach-O sections or generated pointer tables for `NSI_TASK()`.
- [x] **P5-02** Preserve all seven native simulator task levels.
- [x] **P5-03** Preserve numeric priority order within each level.
- [x] **P5-04** Generate or synthesize every current boundary symbol.
- [x] **P5-05** Retain unreferenced task pointer entries.
- [ ] **P5-06** Preserve `NSI_NOASAN` behavior.
- [ ] **P5-07** Add a standalone native simulator task-order test.
- [ ] **P5-08** Include two tasks with the same priority in the test.
- [ ] **P5-09** Define the tie-order rule and make it deterministic.

**Gate P5:** The runner calls each task once at the correct level and priority.

### Phase 6: Port the embedded-image and runner link

- [x] **P6-01** Add a Darwin branch to the native simulator link configuration.
- [x] **P6-02** Replace `--whole-archive` with a proven Darwin mechanism.
- [x] **P6-03** Replace `--gc-sections` with a proven Darwin mechanism.
- [x] **P6-04** Replace GNU linker scripts with generated Mach-O inputs and flags.
- [x] **P6-05** Replace `objcopy --localize-hidden` with a proven Mach-O mechanism.
- [x] **P6-06** Preserve every `NATIVE_SIMULATOR_IF*` symbol.
- [x] **P6-07** Hide embedded symbols that are not interface symbols.
- [x] **P6-08** Handle `CONFIG_*` absolute symbols without exporting them.
- [x] **P6-09** Prove that host C library calls resolve to the intended implementation.
- [x] **P6-10** Prove that embedded C library calls resolve to the intended implementation.
- [x] **P6-11** Produce a final Mach-O executable at `build/zephyr/zephyr.exe`.
- [x] **P6-12** Generate a final link map.
- [x] **P6-13** Add a symbol-audit command to the build.
- [ ] **P6-14** If an unexpected embedded symbol is externally visible, fail the build.
- [x] **P6-15** Keep the current Linux Makefile behavior unchanged.

**Gate P6:** The final executable links without duplicate symbols or unintended exports.

### Phase 7: Add Darwin ABI adapters

- [x] **P7-01** Add the Darwin AArch64 `va_list` selection in `cbprintf_packaged.c`.
- [ ] **P7-02** Test packaged integers, pointers, strings, 64-bit values, and floating-point values.
- [ ] **P7-03** Correct the `__weak` toolchain definition for Apple POSIX targets.
- [ ] **P7-04** Add a weak-symbol override test.
- [x] **P7-05** Add a Darwin implementation for thread names.
- [ ] **P7-06** Define truncation behavior for Darwin thread-name limits.
- [x] **P7-07** Add a Darwin implementation for pthread stack bounds.
- [ ] **P7-08** Keep stack-bound updates disabled until their tests pass.
- [x] **P7-09** Review semaphore behavior during thread abort and process exit.
- [ ] **P7-10** Review clock selection and timer precision on macOS.
- [ ] **P7-11** Add compile-time assertions for pointer size and `va_list` size.

**Gate P7:** Focused ABI tests pass without warnings about weak declarations or `va_list` layout.

### Phase 8: Run `hello_world`

- [x] **P8-01** Run the exact pristine build command from the objective.
- [x] **P8-02** Make sure that `file build/zephyr/zephyr.exe` reports an `arm64` Mach-O executable.
- [x] **P8-03** Run `build/zephyr/zephyr.exe` directly.
- [x] **P8-04** Run the executable through `west build -t run`.
- [x] **P8-05** Make sure that the output contains `Hello World! native_sim`.
- [x] **P8-06** Make sure that the process exits or reaches the normal simulator idle state.
- [x] **P8-07** Run the application 100 times to find startup races.
- [x] **P8-08** Run a non-pristine rebuild after one source change.
- [x] **P8-09** Run a pristine rebuild after the non-pristine rebuild.
- [ ] **P8-10** Inspect initialization and native task order from trace output.

**Gate P8:** The exact build and run commands pass reliably on Apple Silicon.

### Phase 9: Regression tests

- [ ] **P9-01** Build and run `samples/hello_world` on Linux `native_sim//64`.
- [ ] **P9-02** Compare the Linux symbol visibility before and after the port.
- [ ] **P9-03** Run native simulator scheduler tests on macOS.
- [ ] **P9-04** Run kernel thread tests on macOS.
- [ ] **P9-05** Run semaphore, timer, and busy-wait tests on macOS.
- [ ] **P9-06** Run initialization-priority tests on macOS.
- [ ] **P9-07** Run iterable-section tests on macOS.
- [ ] **P9-08** Run `cbprintf` tests on macOS.
- [x] **P9-09** Run Python tests for changed build scripts.
- [ ] **P9-10** Run the CMake formatter and the Python style tests.
- [ ] **P9-11** Run a warnings-as-errors build.
- [ ] **P9-12** Record unsupported configurations in Kconfig or CMake errors.

**Gate P9:** Focused macOS tests and current Linux tests pass.

### Phase 10: Expand support

- [ ] **P10-01** Add Intel macOS after Apple Silicon passes.
- [ ] **P10-02** Add multiple simulated MCUs.
- [ ] **P10-03** Add ASan and UBSan.
- [ ] **P10-04** Add coverage with the LLVM coverage format.
- [ ] **P10-05** Add libFuzzer.
- [ ] **P10-06** Add external C++ library support.
- [ ] **P10-07** Review host networking drivers.
- [ ] **P10-08** Review file-system and host-device adapters.
- [ ] **P10-09** Add supported macOS builders to continuous integration.
- [ ] **P10-10** Update native simulator documentation and release notes.

## First implementation sequence

Use this order for the first coding session:

1. Complete Phase 0.
2. Complete the Phase 1 linker probes.
3. Select the registration design.
4. Add target detection from Phase 2.
5. Complete absolute constants from Phase 3.
6. Implement only the ranges required by minimal `hello_world`.
7. Port native simulator task registration.
8. Port the final runner link.
9. Add the required ABI adapters.
10. Run the Phase 8 acceptance steps.
11. Generalize the implementation and run Phase 9.

Do not start broad API adapters before Gate P1 passes. The linker design controls the rest of the port.

## Expected files

The port can change or add files in these areas:

```text
arch/posix/CMakeLists.txt
arch/posix/aarch64.cmake
boards/native/common/natsim_config.cmake
boards/native/common/natsim_linker_script.ld
cmake/linker/
include/zephyr/toolchain/gcc.h
include/zephyr/init.h
include/zephyr/sys/iterable_sections.h
lib/os/cbprintf_packaged.c
scripts/build/gen_offset_header.py
scripts/native_simulator/Makefile
scripts/native_simulator/common/other/
scripts/native_simulator/common/src/include/nsi_cpu_if.h
scripts/native_simulator/common/src/include/nsi_tasks.h
scripts/native_simulator/common/src/nct.c
tests/
```

Not every listed file must change. Prefer host-specific files and narrow dispatch points.

## Required diagnostic artifacts

Keep these files after each major build attempt:

- the CMake cache entries for tools and target format
- one failed or successful verbose compile command
- the embedded-image link command
- the final runner link command
- the link map
- the generated range map
- `nm` output for the embedded image
- `nm` output for the final executable
- the application run log

These artifacts make symbol and order failures reproducible.

## Acceptance criteria

All criteria in this section must pass before the initial port is complete.

- [x] The original pristine build command returns zero.
- [x] The output file is a native Apple Silicon Mach-O executable.
- [x] The executable prints the Zephyr `hello_world` message.
- [x] The executable uses the normal Zephyr initialization and scheduler paths.
- [x] Every required initialization entry runs once and in order.
- [ ] Every required native simulator task runs once and in order.
- [ ] Iterable ranges have correct boundaries, counts, and alignment.
- [x] Embedded symbols do not collide with runner or host symbols.
- [x] The final executable exports only the intended interface symbols.
- [ ] Focused `cbprintf`, weak-symbol, timer, semaphore, and thread tests pass.
- [ ] The Linux `native_sim//64` build and run behavior remains unchanged.
- [ ] Unsupported macOS configurations stop with specific messages.
- [x] The build has no new compiler or linker warnings.
- [x] The implementation includes focused automated tests.

## Stop and reassess conditions

If one of these conditions occurs, stop the current design and reassess it:

- Mach-O cannot provide contiguous ranges without broad kernel API changes.
- The Apple linker cannot preserve deterministic data-atom order.
- Symbol isolation requires a custom object-file rewriter.
- The link needs a custom ELF loader inside the macOS process.
- The design changes normal embedded builds from macOS.
- The minimal port requires changes across unrelated Zephyr subsystems.

A reassessment can select generated tables or a new iterable-section backend. Record the decision and evidence in this file.

## Open questions

- [ ] Can an Apple linker order file order data atoms from static archives?
- [ ] Can zero-size assembly labels define exact typed-range boundaries?
- [ ] Does `ld -r` preserve hidden visibility for the required final link?
- [ ] Can exported-symbol lists replace `objcopy` localization?
- [ ] Can one short Mach-O section hold multiple logical ranges with exact boundaries?
- [ ] Does atom alignment insert padding inside a typed iterable range?
- [ ] Which empty ranges exist in minimal `hello_world`?
- [ ] Which Zephyr link passes are required for `native_sim` on Mach-O?
- [ ] Can the Darwin path use CMake instead of adding more Makefile branches?
- [ ] Which Xcode versions synthesize section boundary symbols consistently?
- [ ] Does Apple Clang support all required weak and visibility combinations?
- [ ] Does the simulator need non-PIE code on Apple Silicon?

## Decision log

### 2026-09-13: Initial investigation

**Decision:** Treat the work as a Mach-O backend port, not as removal of a host gate.

**Reason:** Compilation reached section declarations after several bounded fixes. The final link also rejected the GNU toolchain model.

**Evidence:** Findings F-01 through F-11.

### 2026-09-13: Registration backend

**Decision:** Use dedicated short Mach-O sections and synthesized section boundaries.

The link wrapper scans registration metadata with Apple `nm`. It creates an order file and remaps priority sections with Apple `ld`.

Zephyr initialization entries also store priority metadata. The kernel uses this metadata to enforce order when one object file contains entries in reverse declaration order.

### 2026-09-13: Symbol-localization mechanism

**Decision:** Use an Apple relocatable link and an unexported-symbol list.

The relocatable link converts hidden symbols to local symbols. The generated unexported list also localizes absolute configuration symbols.


### 2026-09-13: Expanded non-networking sample verification

The following 14 samples build without warnings and pass bounded runtime checks:

| Area | Sample |
|---|---|
| Threads and synchronization | `samples/philosophers` |
| Message queues | `samples/kernel/msg_queue` |
| Condition variables | `samples/kernel/condition_variables/simple` |
| Kernel synchronization | `samples/synchronization` |
| Heap allocation | `samples/basic/sys_heap` |
| Logging | `samples/subsys/logging/logger` |
| CMSIS-RTOS v2 | `samples/subsys/portability/cmsis_rtos_v2/timer_synchronization` |
| State machines and shell transport | `samples/subsys/smf/hsm_psicc2` |
| Entropy and UUID | `samples/subsys/uuid` |
| RTIO | `samples/subsys/rtio/producer_consumer` |
| Zbus | `samples/subsys/zbus/hello_world` |
| Shell | `samples/subsys/shell/shell_module` |
| C++ | `samples/cpp/hello_world` |
| C++ synchronization | `samples/cpp/cpp_synchronization` |

This verification added short section mappings for logging, shell, device APIs, RTIO, and zbus. It also added Darwin adapters for debugger metadata, entropy, and pseudoterminal configuration. An interactive PTY test sent `help` to the shell and received its command list.

### 2026-09-13: Quercus `laser_alignment` integration probe

The exact `qbuild` command first stopped because `west` was not on `PATH`. Setting `QBUILD_WEST` to the workspace virtual-environment executable corrected this host-environment issue.

The application then exposed two missing Zephyr host-port features. `HWINFO_NATIVE` rejected every host except Linux even though its `gethostid()` implementation compiles on Darwin. Native-libc socket types also included a private glibc header. The Darwin path now enables the HWINFO implementation and obtains `struct timeval` from `<sys/time.h>`. A separate runtime probe returned a four-byte native device ID.

The remaining direct build failure is outside Zephyr. The Quercus SDK lockstep transport includes Linux futex headers and invokes `SYS_futex`. The Quercus simulator parent uses the same Linux-only futex protocol and `memfd_create()`. A temporary Darwin polling shim and Mach-O event-boundary declarations let `laser_alignment` link as an arm64 Mach-O executable without warnings. The shim was not retained because useful lockstep execution requires a coordinated portability design in both the Quercus parent and child transport.

### 2026-09-13: Apple Silicon hello_world milestone

```text
Date: 2026-09-13
Host: arm64 macOS 27.0 with Apple Clang 21.0.0
Commit or worktree state: experimental uncommitted port
Tasks completed: initial Mach-O build, link, ABI, run, and focused tests
Commands run: pristine west build, direct run, west run, 100-run probe, pytest
Result: requested build and hello_world execution pass
New evidence: runtime metadata is required for same-object initialization order
New risks: iterable type coverage remains limited to the initial scope
Next task: expand iterable mappings and run Linux regression tests
```

## Progress log template

Add one entry for each implementation session.

```text
Date:
Host:
Commit or worktree state:
Tasks completed:
Commands run:
Result:
New evidence:
New risks:
Next task:
```
