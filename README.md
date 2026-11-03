# df_pcc_relay

A small native daemon for **HyperOS ports on the Redmi 9T (I think it will work on other older phones too)** with a MIUI vendor.
It takes the colour coefficients computed by the vendor `displayfeature` service and passes them to
SurfaceFlinger itself through the standard code `1015` (colour matrix). This makes the
"Eye care" mode and the display colour modes work.


[![build](../../actions/workflows/build.yml/badge.svg)](../../actions/workflows/build.yml)

## Why this is needed

The `displayfeature` service from the MIUI vendor computes the colour and writes it to the log, but SurfaceFlinger in HyperOS
**does not accept** these values: the screen stays neutral, eye care is "on", yet the colour does not change.

A workaround that has been verified in practice is to call code 1015 yourself:

```
service call SurfaceFlinger 1015 i32 1 <16 float>
```

## How it works

```
        logcat -b main -b system -v raw -T 1 displayfeature:D DisplayFeatureManagerService:D '*:S'
             │  (child process: fork/exec + pipe, dies together with its parent)
             ▼
   ┌────────────────────┐    new value        ┌────────────────────────┐
   │ main thread        │ ──(mutex + cond)──▶ │ applier thread         │
   │ blocking read()    │                     │ cond_wait, then:       │
   │ line parsing       │                     │ service call … 1015    │
   └────────────────────┘                     │ 30 ms pause            │
                                              └────────────────────────┘
```

1. **Single-instance.** On start, stale copies are killed (the binary, the old `.sh`, and their logcat readers),
   then a `flock` is taken on `/dev/df_pcc_relay.lock`.
2. **Waiting for SurfaceFlinger.** `service check SurfaceFlinger` once a second, waiting for "found"
   (the string "not found" counts as a failure).
3. **Reader.** A child `logcat` with a filter; lines are read with blocking reads. If logcat dies,
   it is restarted after 1.5 s. The `'*:S'` filter also hides the daemon's own logs (tag `df_pcc_relay`),
   so there is no feedback loop.
4. **Parsing.**
   * `setScreenEffect displayId=… mode=M value=V`
     * `M=3` (eye care): `V<=0` gives the neutral `1 1 1`, otherwise the colour is computed with the formula below.
     * `M=0|1|2|23`: the mode is remembered so that `PccData` values get applied.
     * If `V` is not a number, the line is skipped.
   * `PccData value R, G, B`: used as is, but **ignored while eye care (M=3) is active**.
5. **Applying.** The latest value wins. It is applied only if it differs from the last
   applied one, followed by a 30 ms pause. A 4×4 matrix with the diagonal `R, G, B, 1` is sent through `/system/bin/service`.

### Eye care formula

Integer arithmetic (`e = 580·STRENGTH`, values in millionths):

```
R = 1.000000
G = 0.921569 − 4.973e-6 · e
B = 0.901961 − 10.126e-6 · e
```

| STRENGTH |        R |        G |        B |
|---------:|---------:|---------:|---------:|
| 0        | 1.000000 | 0.921569 | 0.901961 |
| 50       | 1.000000 | 0.777352 | 0.608307 |
| 100      | 1.000000 | 0.633135 | 0.314653 |

## Configuration

| What             | How                                          |
|------------------|----------------------------------------------|
| Eye care strength | argument `--strength N` (0..100)            |
|                  | or property `persist.df_pcc.strength`        |
| Verbose logs     | argument `-v`                                |

Priority: **argument → property → default (50)**. The property is read when the daemon starts;
after changing it, restart the service: `stop df_pcc_relay; start df_pcc_relay`.

## Logs

All logs go to Android logcat with the tag `df_pcc_relay` (to stderr if `liblog.so` failed to load):

```
logcat -s df_pcc_relay
```

Always logged: version and pid, STRENGTH and its source, the single-instance result, waiting for and readiness of
SurfaceFlinger, the logcat reader's pid, the first accepted event, every `APPLY R G B rc=… t=…ms`,
errors, reader restarts, signals and exit. Incoming events are logged only with `-v`.

## Building

All you need is a C compiler; there are no dependencies beyond libc/libdl (`liblog` is loaded via `dlopen`).

**Termux (on the phone, aarch64):**

```
pkg install clang make binutils patchelf
make            # produces ./df_pcc_relay
```

Termux adds a `RUNPATH` pointing to `/data/data/com.termux/…`. Check for it and remove it:

```
readelf -d df_pcc_relay | grep -E 'NEEDED|RPATH|RUNPATH'
patchelf --remove-rpath df_pcc_relay
```

**Android NDK (on a PC):**

```
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
    -std=gnu11 -O2 -Wall -Wextra -pthread -s -o df_pcc_relay src/df_pcc_relay.c -ldl
```

**GitHub Actions:** `.github/workflows/build.yml` builds the arm64 binary with the NDK, checks the ELF
dependencies and publishes an artifact. A release is created when a `v*` tag is pushed.

## Installing into the port


```
system/bin/df_pcc_relay              # 0755, root:root
system/etc/init/df_pcc_relay.rc      # 0644
```

The service is not `oneshot`, so init restarts it if it crashes. SELinux context: `u:r:hal_graphics_composer_default:s0`

## Testing

```
su
setprop persist.df_pcc.strength 70
/data/local/tmp/df_pcc_relay -v &
logcat -s df_pcc_relay                 # in another window
kill %1; start df_pcc_relay
top -H -p $(pidof df_pcc_relay)        # when idle: 0% CPU, threads in state S
```

**Tests on a PC.** `make check` builds the daemon for the host with fake `service` and `logcat`, feeds it
a scripted sequence of lines and checks the matrices that were sent, deduplication, eye care precedence and clean shutdown.

## Limitations

* The `service call` exit code only reveals a failure of the utility itself, not a rejected SurfaceFlinger transaction.
* There is no watchdog for a hung `service call`.
* In SELinux enforcing mode, rules are needed for the lock file, for launching `logcat`/`service`, and for reading `/proc`.
* Calling binder directly through `libbinder_ndk` would remove the dependency on `service`, but it is not implemented yet.
* Verified by tests on a PC. Behaviour on a device depends on the specific port, so check it yourself.

## Repository layout

```
src/df_pcc_relay.c           implementation
src/df_pcc_relay.h           settings, types, log macros
init/df_pcc_relay.rc         init service
tests/run.sh                 PC test (make check)
.github/workflows/build.yml  build and releases
Makefile                     make / make check / make clean
```

## License

[GPL-2.0-only](LICENSE). SPDX-License-Identifier: `GPL-2.0-only`.

Copyright (C) 2026 SlakkyStar
Copyright (C) 2026 Claude (Anthropic)
