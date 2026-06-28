# Repository Grounding: DJI Payload SDK Weather Payload

## Inspection Context

This document captures the read-only repository inspection performed for a DJI Payload SDK firmware repo intended for a real hardware payload compatible with the DJI Matrice 300 RTK.

No files were modified during the inspection pass. No build or test commands were run during the inspection pass. No branch checkout, merge, rebase, reset, stash, cleanup, deletion, or commit was performed.

Sensitive credential values are intentionally not included. Credential-bearing files are listed only as `sensitive value present`.

## Executive Findings

- Current working directory observed: `/mnt/c/Users/zande/OneDrive/Desktop/RMBL Docs/Drone-Weather-Station/DRONE_PAYLOAD_FIRMWARE_DEV/drone-weather-station`.
- Current branch observed: `master`.
- Local branches observed: `master`, `dev`.
- Remote branches observed: `origin/master`, `origin/dev`, `origin/HEAD -> origin/master`.
- Remote observed: `origin https://github.com/zanderalbaz/drone-weather-station.git`.
- `master`, `dev`, `origin/master`, and `origin/dev` all pointed to commit `5a2ff87 Starting Git versioning`.
- Commit-level `dev` vs `master` comparison was identical: no unique commits either way.
- The working tree was dirty before any documentation work: `git status --short` reported hundreds of modified tracked paths.
- The dirty working tree appeared dominated by line-ending churn: non-build/log diffs showed equal insertions and deletions across whole files.
- The repository has 776 tracked files.
- The initial commit is a single import of DJI SDK source, project edits, logs, generated build files, object files, executable output, binary libraries, and runtime logs. There is no clean upstream DJI base commit in this repo history.
- Stock-vs-custom classification is therefore inferred from file names, DJI SDK conventions, custom identifiers, and runtime paths rather than from a clean commit history.
- Active build path appears to be the C sample Linux `manifold2` target, not Raspberry Pi, Jetson, or RTOS.
- Custom payload work appears centered on a TriSonica/weather logger integrated into DJI FC subscription sample code.
- DJI application credentials are present in tracked app-info headers. Values are not reproduced here.
- Generated build artifacts, runtime logs, object files, CMake cache files, and a built executable are tracked.
- No `.gitignore` or `.gitattributes` file was observed at the repository root.

## Repo Map

| Path | Classification | Findings |
| --- | --- | --- |
| `CMakeLists.txt` | Mostly stock DJI SDK build entry | Defaults `USE_SYSTEM_ARCH` to `LINUX` and adds `samples/sample_c/platform/linux/manifold2`. |
| `README.md` | Stock DJI SDK documentation | Identifies Payload SDK version `V3.11.0`; mentions latest release support for Matrice 4TD/4D. This does not by itself verify M300 RTK compatibility for this payload. |
| `psdk_lib/include/` | Stock DJI SDK headers | Includes PSDK API headers such as `dji_core.h`, `dji_platform.h`, `dji_fc_subscription.h`, and others. |
| `psdk_lib/lib/` | Stock DJI SDK binary libraries | Contains prebuilt SDK libraries for `x86_64-linux-gnu-gcc`, `aarch64-linux-gnu-gcc`, `arm-linux-gnueabi-gcc`, `arm-linux-gnueabihf-gcc`, and `armcc_cortex-m4`. |
| `samples/sample_c/` | Stock DJI C sample tree plus custom project edits | Active target and custom TriSonica work are under this tree. |
| `samples/sample_c++/` | Mostly stock DJI C++ sample tree | Not active in top-level CMake because the C++ manifold2 target is commented out. |
| `samples/sample_c/platform/linux/manifold2/` | Active Linux C target | CMake target `dji_sdk_demo_linux`; likely active runtime path. |
| `samples/sample_c/module_sample/fc_subscription/` | Stock DJI sample edited in place | Contains modified FC subscription sample and custom logger subdirectory. |
| `samples/sample_c/module_sample/fc_subscription/logger/` | Custom payload code | Contains TriSonica reader, fusion loop, CSV writer, and shared logging structures. |
| `build/` | Generated/compiled artifacts | Tracked. Contains CMake cache/files, object files, built executable, and runtime logs. |
| `Logs/` | Runtime logs | Tracked DJI log files and index/latest log. |
| `doc/` | Stock DJI docs | Includes reference design PDFs and code style docs. |
| `tools/` | Stock DJI tools | Includes `file2c` tooling and a Windows executable. |

## Build And Runtime Hypothesis

### Observed Build Wiring

- Top-level `CMakeLists.txt`:
  - Defaults `USE_SYSTEM_ARCH` to `LINUX`.
  - Adds `samples/sample_c/platform/linux/manifold2` for Linux builds.
  - Comments out the C++ manifold2 subdirectory.
  - Uses `uname -m` to choose the SDK library path.
  - Supports RTOS path only when `USE_SYSTEM_ARCH` matches `RTOS`.
- Active target CMake file: `samples/sample_c/platform/linux/manifold2/CMakeLists.txt`.
- Active target name: `dji_sdk_demo_linux`.
- Active language: C.
- Active executable output path: `${CMAKE_BINARY_DIR}/bin` when not otherwise set.
- Active target compiles:
  - `samples/sample_c/platform/linux/manifold2/application/*.c`
  - `samples/sample_c/platform/linux/manifold2/hal/*.c`
  - `samples/sample_c/platform/linux/common/*.c`
  - `samples/sample_c/module_sample/**/*.c`
- Important build-system concern: the target uses `file(GLOB_RECURSE MODULE_SAMPLE_SRC ../../../module_sample/*.c)`, so every C sample module is compiled into the executable, including modules that may not be used at runtime.

### Likely Build Command

Likely build command, not run during inspection:

```sh
cmake -S . -B build
cmake --build build
```

This command is a hypothesis based on CMake layout only. It was not executed during inspection.

### Likely Runtime Entry Point

- `samples/sample_c/platform/linux/manifold2/application/main.c`

Observed custom behavior in this file:

- Reads `SIM` environment variable.
- In non-SIM mode:
  - Fills DJI user info from `dji_sdk_app_info.h`.
  - Calls `DjiCore_Init`.
  - Calls aircraft info APIs.
  - Sets alias to `TriSonica Mini App`.
  - Sets firmware version `1.0.0.0`.
  - Sets serial number to a placeholder-like value.
  - Starts data transmission service if configured.
  - Calls `DjiCore_ApplicationStart`.
- In SIM mode:
  - Skips DJI core initialization.
- Starts a monitor thread.
- Calls `DjiTest_FcSubscriptionRunSample()`.
- Sleeps forever after that call if the sample does not exit.

### Runtime Hardware And File Assumptions Observed

- DJI PSDK hardware connection mode is configured as UART-only in `samples/sample_c/platform/linux/manifold2/application/dji_sdk_config.h`.
- Active DJI app baud rate is present in `samples/sample_c/platform/linux/manifold2/application/dji_sdk_app_info.h`; value not repeated here.
- `samples/sample_c/platform/linux/manifold2/hal/hal_uart.h` defines:
  - `LINUX_UART_DEV1` as `/dev/ttyUSB0`
  - `LINUX_UART_DEV2` as `/dev/ttyACM0`
- `samples/sample_c/module_sample/fc_subscription/logger/tri_reader.c` opens TriSonica on `/dev/serial0`.
- `tri_reader.c` configures the TriSonica serial port to 115200 baud, 8N1 raw, no flow control.
- `tri_reader.c` sends TriSonica output-rate commands such as `{outputrate 5}`, `{outputrate 10}`, `{outputrate 20}`, `{outputrate 40}`.
- `samples/sample_c/module_sample/fc_subscription/logger/log_writer.c` writes a temp CSV to `/home/rmbl/Desktop/Collected Data/fusion_400hz_temp.csv`.
- `samples/sample_c/module_sample/fc_subscription/test_fc_subscription.c` renames the temp CSV under `/home/rmbl/Desktop/Collected Data/`.
- `test_fc_subscription.c` registers widget config from `/home/rmbl/my_widget_config`.
- `test_fc_subscription.c` can call `system("sleep 5; sudo shutdown -h now")` in non-SIM mode after shutdown is requested.

## dev vs master Comparison

### Branch Facts

| Item | Observed result |
| --- | --- |
| Current branch | `master` |
| Local branches | `master`, `dev` |
| Remote branches | `origin/master`, `origin/dev` |
| Shared commit | `5a2ff87 Starting Git versioning` |
| `master..dev` commits | none |
| `dev..master` commits | none |
| `master...dev` left/right count | `0 0` |
| Relationship | Identical commit tips |
| Commit-level diff | Empty |

### Branch Recommendation

`dev` and `master` are identical at the commit level, so there is no meaningful branch content to merge between them right now. However, the working tree is dirty and the repository tracks generated artifacts and logs, so branch operations should wait until repository hygiene and line-ending state are understood.

### Important Distinction

The branch relationship is clean, but the working tree is not clean. These are separate facts:

- Branch state: `dev` and `master` point to the same commit.
- Working tree state: many tracked files are modified locally, apparently mostly from line-ending churn plus generated build/log updates.

## Custom Modification Inventory

| File | What it appears to do | Confidence |
| --- | --- | --- |
| `samples/sample_c/platform/linux/manifold2/application/main.c` | Customizes DJI sample main for TriSonica app behavior, SIM mode, app alias, FC subscription logger startup. | High |
| `samples/sample_c/platform/linux/manifold2/application/dji_sdk_app_info.h` | Active app identity/config for manifold2 target. Sensitive value present. | High |
| `samples/sample_c/platform/linux/raspberry_pi/application/dji_sdk_app_info.h` | App identity/config for Raspberry Pi target. Sensitive value present. This target was not active in top-level CMake. | High |
| `samples/sample_c/platform/linux/nvidia_jetson/application/dji_sdk_app_info.h` | App identity/config for Jetson target; observed as placeholder/empty except baud-like config. Target was not active in top-level CMake. | Medium |
| `samples/sample_c/platform/rtos_freertos/stm32f4_discovery/application/dji_sdk_app_info.h` | App identity/config for RTOS target; observed as placeholder/empty except baud-like config. Target was not active in top-level CMake. | Medium |
| `samples/sample_c/platform/linux/manifold2/application/dji_sdk_config.h` | Selects UART-only transport and enables many DJI sample modules. | High |
| `samples/sample_c/module_sample/fc_subscription/test_fc_subscription.c` | Heavily customized FC subscription sample: telemetry subscriptions, widget callbacks, text input file naming, logger thread startup, SIM flow, shutdown. | High |
| `samples/sample_c/module_sample/fc_subscription/logger/logger.h` | Shared `TriData`, `LogRow`, atomics, and queue APIs for custom logger. | High |
| `samples/sample_c/module_sample/fc_subscription/logger/tri_reader.c` | Custom TriSonica serial reader/parser and output-rate controller on `/dev/serial0`. | High |
| `samples/sample_c/module_sample/fc_subscription/logger/fusion.c` | Custom 400 Hz fusion loop combining DJI FC topics and latest TriSonica sample into `LogRow`. | High |
| `samples/sample_c/module_sample/fc_subscription/logger/log_writer.c` | Custom CSV writer and ring buffer consumer for fused rows. | High |
| `samples/sample_c/module_sample/fc_subscription/logger/fusion.c.save` | Backup or stale copy of custom fusion work. | Medium |
| `samples/sample_c/module_sample/fc_subscription/test_fc_subscription.c.save` | Backup or stale copy of custom FC subscription work. | Medium |

## Build Artifacts, Logs, And Repo Hygiene

### Observed Tracked Generated Or Runtime Files

Tracked generated/runtime paths include:

- `build/CMakeCache.txt`
- `build/CMakeFiles/...`
- `build/Makefile`
- `build/cmake_install.cmake`
- `build/bin/dji_sdk_demo_linux`
- `build/bin/Logs/...`
- `build/samples/sample_c/platform/linux/manifold2/CMakeFiles/...`
- `Logs/DJI_*.log`
- `Logs/index`
- `Logs/latest.log`

Observed tracked binary/library files include:

- `psdk_lib/lib/aarch64-linux-gnu-gcc/libpayloadsdk.a`
- `psdk_lib/lib/arm-linux-gnueabi-gcc/libpayloadsdk.a`
- `psdk_lib/lib/arm-linux-gnueabihf-gcc/libpayloadsdk.a`
- `psdk_lib/lib/armcc_cortex-m4/libpayload.lib`
- `psdk_lib/lib/x86_64-linux-gnu-gcc/libpayloadsdk.a`
- `tools/file2c/file2c.exe`
- DJI sample media files and perception model binaries under `samples/`

The SDK libraries and sample media/model assets may be expected in a DJI SDK tree, but generated build outputs and runtime logs are repository hygiene problems for normal development.

### Working Tree Dirtiness

Observed status summary by top-level path during inspection:

| Top-level path | Modified tracked paths observed |
| --- | ---: |
| `samples` | 514 |
| `build` | 92 |
| `psdk_lib` | 32 |
| `doc` | 3 |
| `tools` | 1 |
| `README.md` | 1 |
| `LICENSE.txt` | 1 |
| `EULA.txt` | 1 |
| `CMakeLists.txt` | 1 |

The large source diff appeared to be line-ending churn. For example, `git diff --stat -- . ':!build/' ':!Logs/'` reported 554 files changed with equal insertions and deletions. `git diff --check` flagged line-level whitespace changes beginning at line 1 of stock files.

No cleanup was performed.

## Sensitive Configuration Findings

Do not print or copy values from these files in chat or public docs.

| File | Finding |
| --- | --- |
| `samples/sample_c/platform/linux/manifold2/application/dji_sdk_app_info.h` | sensitive value present |
| `samples/sample_c/platform/linux/raspberry_pi/application/dji_sdk_app_info.h` | sensitive value present |

Additional app-info files were observed for Jetson and RTOS targets, but during inspection they appeared placeholder/empty for app identity fields except baud-like config. Treat all `dji_sdk_app_info.h` files as sensitive until cleaned.

## Glaring Issues And Risks

### Must Fix Before Development

- Dirty working tree: 646 modified tracked paths were observed before documentation work. This must be resolved or intentionally preserved before branch work, code review, or new development.
- Sensitive DJI app credentials are tracked in app-info headers. Rotate or reissue them if they are real credentials.
- No `.gitignore` or `.gitattributes` was observed. This likely contributes to generated files being tracked and line-ending churn.
- Generated build artifacts and logs are tracked, including `build/`, `build/bin/dji_sdk_demo_linux`, object files, CMake cache files, and `Logs/`.
- Active build target is `linux/manifold2`, while custom TriSonica code assumes `/dev/serial0`, which may indicate Raspberry Pi-style serial naming. Confirm actual companion computer and serial device map.
- `test_fc_subscription.c` can run `sudo shutdown -h now` through `system()`. This is dangerous on real hardware unless deliberately required and strongly guarded.
- CSV output paths are hardcoded under `/home/rmbl/Desktop/Collected Data/`. Missing directory or permissions will break logging.
- Widget config path is hardcoded as `/home/rmbl/my_widget_config`; this path was not observed in the repo.
- `g_csvFile` is shared across files/threads and is closed/reopened without an observed mutex.
- `logger_queue_push()` updates the write index before writing the row, so a reader can observe a row slot before the write is complete.
- `row_set_nan()` casts a full `LogRow` to `float *`, which can corrupt non-float fields and is not a safe way to initialize a mixed-type struct.

### Should Fix Soon

- The active target compiles every C sample module under `samples/sample_c/module_sample`, even though the app appears to need only a subset.
- `samples/sample_c/platform/linux/manifold2/application/dji_sdk_config.h` enables many sample modules, including camera, gimbal, xport, widget, data transmission, upgrade, FC subscription, HMS customization, and power management.
- Custom application code is edited directly into DJI sample files rather than isolated into project-owned modules.
- `SIM` handling is inconsistent: a helper variable `g_simulate_fc` exists but was not observed being set, while `simulate_fc` is used elsewhere.
- Thread creation return codes for custom logger threads were not observed being checked.
- File naming from low-speed data channel text input should be sanitized before use in filesystem paths.
- `tri_reader.c` does not document whether `/dev/serial0` is stable on the target hardware.
- `tri_reader.c` assumes TriSonica command syntax and output fields but no protocol doc was observed in repo.
- Many DJI telemetry topic return codes are intentionally discarded in `fusion.c`, which can hide runtime subscription or data retrieval problems.
- Rate and timing assumptions need validation: fusion loop attempts 400 Hz while writer flushes every 10 ms and TriSonica output rates are 5/10/20/40 Hz.

### Nice Cleanup

- Remove or document `.save` backup files.
- Add a project README or operator runbook for target aircraft, target computer, OS, serial wiring, launch command, expected directories, and log output format.
- Move custom logger code out of `module_sample` into a project-specific application directory.
- Add a minimal smoke-test mode that validates CSV output without requiring DJI hardware.
- Add explicit CMake options for target platform and required modules.
- Add line-ending policy with `.gitattributes`.

## DJI / M300 RTK Compatibility Questions

These were not fully verified during inspection and should be treated as open questions:

- What is the actual companion computer: Manifold2, Raspberry Pi, Jetson, or another Linux computer?
- What OS/version and CPU architecture run on the payload computer?
- Which M300 RTK interface is used: payload port, extension port, SkyPort, X-Port, E-Port, or another adapter?
- Is the intended DJI PSDK transport UART-only, USB bulk, or network?
- Which Linux device corresponds to the DJI PSDK UART on the target?
- Which Linux device corresponds to the TriSonica sensor on the target?
- Is `/dev/serial0` present, stable, and available on the deployed companion computer?
- Is DJI PSDK baud configured correctly for the selected M300 RTK port?
- Are the credentials in `dji_sdk_app_info.h` real, current, and authorized for this aircraft/payload configuration?
- Does DJI Assistant or DJI Pilot require additional payload/app configuration before the app will start?
- Are the subscribed FC telemetry topics and rates supported on M300 RTK with PSDK 3.11.0?
- Are RTK topic units and fields being logged correctly for M300 RTK?
- Is the payload allowed to shut down the companion computer from an in-flight or post-flight widget action?
- Is `/home/rmbl/Desktop/Collected Data/` guaranteed to exist and be writable by the runtime user?
- Where should widget config files actually live on the payload computer?
- Should this fork remain close to DJI upstream, or should custom code be split into a cleaner project application layered on top of the SDK?

## Recommended Next Development Steps

1. Preserve current evidence before cleanup.
   - Do not merge or switch branches until the dirty working tree is understood.
   - Capture a status snapshot if needed for audit.

2. Fix repository hygiene in a deliberate separate step.
   - Add `.gitignore` for generated build outputs and runtime logs.
   - Add `.gitattributes` for line-ending normalization.
   - Untrack generated files and logs without deleting local copies, if approved.

3. Handle secrets.
   - Move DJI app credentials out of tracked source.
   - Rotate/reissue credentials if the checked-in values are real.
   - Treat all `dji_sdk_app_info.h` files as sensitive until cleaned.

4. Confirm hardware target.
   - Decide whether active target should remain `manifold2` or move to Raspberry Pi/Jetson/other.
   - Confirm OS, architecture, serial device names, port wiring, and permissions.

5. Reduce build surface.
   - Stop compiling every sample module into the active binary.
   - Keep only the required DJI modules and custom logger modules.

6. Verify build.
   - Use a fresh build directory after hygiene is fixed.
   - Do not treat the existing tracked `build/bin/dji_sdk_demo_linux` as proof that the current tree is safe.

7. Run a minimal simulation smoke test.
   - Use SIM mode only after confirming it does not require hardware.
   - Verify CSV path handling and thread shutdown.

8. Run a bench hardware smoke test.
   - Connect DJI PSDK transport and TriSonica sensor.
   - Verify PSDK init, app registration, telemetry subscription, TriSonica parsing, widget interaction, and CSV output.

9. Only then begin feature work.
   - Add tests or instrumentation around parsing, logging, and topic retrieval before expanding behavior.

## Future Codex Handoff Summary

```markdown
Repo: DJI Payload SDK 3.11.0 based firmware for a TriSonica/weather payload intended for DJI Matrice 300 RTK compatibility.
Active branch at inspection: master.
Branch state: local/remote master and dev all point to 5a2ff87 "Starting Git versioning"; dev vs master diff is empty.
Working tree at inspection: dirty with 646 modified tracked files, likely broad CRLF/line-ending churn. Do not switch/merge until resolved.
Tracked file count: 776.
Repo hygiene: build outputs, runtime logs, CMake generated files, object files, and built executable are tracked. No .gitignore or .gitattributes observed.
Active build target: top-level CMake -> samples/sample_c/platform/linux/manifold2 -> target dji_sdk_demo_linux.
Likely runtime entry point: samples/sample_c/platform/linux/manifold2/application/main.c.
Custom code: samples/sample_c/module_sample/fc_subscription/test_fc_subscription.c and logger/{logger.h,tri_reader.c,fusion.c,log_writer.c}.
Custom behavior: TriSonica on /dev/serial0 at 115200; DJI PSDK UART-only config; fused DJI FC telemetry plus TriSonica rows to CSV; widget-controlled logging and sample rate; SIM mode exists.
Sensitive files: samples/sample_c/platform/linux/manifold2/application/dji_sdk_app_info.h and samples/sample_c/platform/linux/raspberry_pi/application/dji_sdk_app_info.h contain sensitive value present.
Major risks: checked-in credentials, tracked generated artifacts/logs, hardcoded /home/rmbl paths, hardcoded /dev/serial0, dangerous sudo shutdown command, thread/file races, unsafe row_set_nan implementation, active manifold2 target may not match actual companion computer.
Immediate next steps: repo hygiene, secret rotation/externalization, hardware target confirmation, clean build verification, SIM smoke test, then bench test with M300/PSDK UART and TriSonica.
```

