# hqdac — the receiver `aplay` could not be

`aplay` is not a protocol peer. It fills its ALSA buffer before it starts, keeps
its own stdin buffer, and gives you no way to set or verify the fill depth. The
main README's own summary says so: in-flight latency under aplay "settles at
about one buffer depth plus whatever ssh holds, and the settlement point is not
fully under the Mac's control."

`hqdac` is a single static aarch64 binary that issues the PCM ioctls itself.
No alsa-lib, no shared-library dependency, no compiler on the Pi — the constraint
that ruled out building anything there. It is a drop-in for the aplay command
line pibridge already builds, so `--receiver=~/hqdac` is the whole integration.

```
Mac: pibridge/PiBridge ──ssh──▶ Pi: hqdac ──/dev/snd/pcmC0D0p──▶ FiiO K11 R2R
                          │      device buffer: exact, polled
                          └────── ring buffer: the latency budget
```

## Build and install

```bash
./build-static.sh                                    # Docker, linux/arm64, gcc -static
scp hqdac-aarch64 pi:~/hqdac && ssh pi 'chmod +x ~/hqdac'
```

Docker Desktop on Apple Silicon runs arm64 natively, so this is a native
compile in a container, not a cross-compile. The only header needed is the
kernel UAPI's `<sound/asound.h>` from `linux-libc-dev`.

Then, from the Mac:

```bash
./build/pibridge play track.flac --receiver='~/hqdac' --buffer-ms 8 --target-ms 6
./build/pibridge play track.flac --receiver='~/hqdac' --buffer-ms 20 --target-ms 15 --mode dsd
```

## What the two buffers mean

| buffer | flag | who owns it | what it does |
|---|---|---|---|
| device (DMA) | `--buffer-time` | the kernel/USB stack | measured with `DELAY`, reported every 250 ms |
| ring | `--target-time` | this program | latency budget and jitter absorption |

The ring's **capacity** is the target, not four times it: a sender at line rate
keeps it full, so its depth is what the listener hears as delay. Total in-flight
is `ring + device`, and both numbers are exact rather than inferred.

Two behaviours that matter, both learned the hard way here:

- **Prefill, then start.** A playback stream xruns the instant its buffer is
  empty, so `START` before any write is an immediate underrun. `hqdac` fills the
  device buffer first and then starts, explicitly — it also sets
  `start_threshold` to two buffers so the kernel never auto-starts behind its back.
- **Write the whole free space**, not a fixed chunk, so the device buffer spends
  its life near full instead of sawtoothing down toward an underrun.

## Measured on the K11, not estimated

All figures are the **device fill** (`SNDRV_PCM_IOCTL_DELAY`) sampled on the Pi
every 5–10 ms, not pibridge's own 80 ms poll — that poll aliases against a small
buffer and reports phantom minimums.

| test | result |
|---|---|
| device caps via `HW_REFINE` | formats `S16_LE S32_LE S24_3LE DSD_U32_BE`, rate `44100..384000`, channels `2..2`, period floor `125 µs`, buffer floor `41 µs` |
| 20 s steady state, 20 ms buffer | true min **19.00**, mean 19.99 ms, no dip |
| 20 s steady state, 8 ms buffer | true min **6.25**, mean 7.49 ms of a 7.5 ms buffer, no dip |
| 20 s steady state, 6 ms buffer | true min **4.00**, mean 4.99 ms of a 5.0 ms buffer, no dip |
| 20 s steady state, 4 ms buffer | true min **0.006–0.060 ms**, 1–2 dips per 20 s |
| 20 s steady state, 5 ms buffer | true min **0.011 ms**, 1 dip per 20 s |
| 185 s continuous, sampled 1,848× | block means flat — **no drift** |

`true min` is the minimum of every `DELAY` reading in each 250 ms window, not one
sample of it.

Three findings that changed the recommendation:

- **6 ms is the safe floor, not 4 ms.** The K11 will hold a 4 ms buffer
  (`1536` frames) and 2/3 ms requests silently become 4 ms, but at 4–5 ms the
  buffer is reproducibly driven to nearly empty once or twice per 20 s. No hard
  xrun is reported, but the margin is gone and that is audible risk. 6 ms and
  above held with zero dips in every run.
- **The steady-state minimum is ~83% of the buffer.** A 7.5 ms buffer sits at
  6.25 ms worst case, not zero: the periodic dip people see in `/proc` sampling
  is the sawtooth of refills, not an underrun.
- **No drift correction is needed.** Over 185 s the mean fill stayed flat, so
  pibridge's experimental `--steer` loop stays off by default.

## Bit-exactness through hqdac

Verified, not assumed: the bytes hqdac hands to `WRITEI_FRAMES` are identical to
what pibridge renders on the Mac.

Method: `--tee PATH` records every frame the device accepted (written after the
write returns, so it is what the DAC was actually given, including short-write
handling), then both sides are hashed.

| path | frames | audio bytes | sha256 local = Pi |
|---|---|---|---|
| PCM S32_LE 384 kHz, 40 s | 15,360,769 | 122,886,152 | `a8685d4e…dccd669` **match** |
| DSD256 `DSD_U32_BE`, 40 s | 15,360,769 | 122,886,152 | `12d6d785…55cdae` **match** |

The device-bound stream is 18,432 bytes longer than the local render. That is
**pibridge's own end-of-stream padding**: three blocks of digital silence
(`src/main.cpp`, "pad the tail so the DAC plays out the buffer instead of being
cut"), 3 × 768 frames × 8 bytes. In PCM that tail is all zero bytes; in DSD it is
*modulated* silence, so the tail is nonzero by design. Masking the tail, the
hashes are identical.

### A data-loss bug this test found

Both write sites popped the frames from the ring **before** calling
`WRITEI_FRAMES`. A short write (the device taking fewer frames than offered) then
silently discarded the remainder — missing samples in the middle of playback,
audible as a click and invisible in every counter. Frames are now peeked, written,
and popped by the amount the device reports accepting, so anything it refuses is
retried instead of lost.

### Idle watchdog: the DAC can no longer be stranded

A receiver whose sender vanishes **without closing stdin** used to hold the PCM
device forever. That is not hypothetical: a `hqdac` orphaned this way was found
still holding the DAC, `state: RUNNING`, with no engine attached.

`hqdac` now exits on its own if no bytes arrive from stdin for 5 s mid-stream
(10 s before the first byte). A legitimate stream never idles that long — the ring
is a few milliseconds deep and a stalled producer drains it in that time.

Measured, all three cases:

| scenario | result |
|---|---|
| pibridge `SIGKILL`ed mid-stream (ssh closes, stdin EOF) | released in **<1 s** via the normal EOF path |
| sender `SIGSTOP`ped — stdin open, no data | released at **t+5 s**, `state: closed` |
| stdin open and never used at all | released at **t+10 s** |
| full 35 s file, normal playback | **no false trigger**, 0 underruns, clean exit |

### An orphan leak this test also found

Three copies of pibridge's `/proc` telemetry poller were found still running on
the Pi, hours after their runs ended: the remote `sh -c 'while :; do …'` outlives
its ssh client when pibridge is killed. The loop now exits by itself after ~2 s of
`state: closed`. Verified: no pollers remain after a run.

## Two measurement bugs that produced phantom dips

Both were mine, found by cross-checking against an independent `/proc` sampler,
and both are fixed. They are recorded because the numbers above would otherwise
be untrustworthy:

- **A failed `DELAY` ioctl was clamped to `0`** and counted as "buffer empty",
  inventing dips to 0.003 ms that no external sampler could see. Failures are now
  held over and excluded from the window.
- **A window with no samples was published as `min 0`.** During the fill ramp
  that meant every stream opened with a dip to zero. Empty windows are no longer
  published, and the first second after the pipe primes is excluded because it
  describes the ramp rather than the stream.

```
HQDAC_DEBUG  touch /tmp/hqdac.debug and every reading below 25% of the buffer
             is printed with the raw value, so the next suspicion can be
             checked instead of guessed at.
```

## Limits, stated plainly

- `plughw`/`default` are refused. There is no conversion layer, and silently
  accepting a mismatched format would hide exactly the fault this tool exists to
  expose.
- One instance only — the K11 has a single playback subdevice. A second one now
  fails **immediately** with `Device or resource busy` instead of hanging in a
  blocking `open()`.
- Requests below the hardware floors are clamped, not rejected: `--buffer-time=100
  --period-time=50` becomes period 48 frames / buffer 96 frames (0.25 ms).
- 4 ms is the smallest buffer the K11 will actually hold; 2 ms and 3 ms requests
  silently become 4 ms. It is not recommended over ssh — see above.
- `--stats` writes fill telemetry to stderr. pibridge swallows its child's
  stderr (it only counts `underrun` occurrences), so pass
  `--receiver='~/hqdac --stats'` when running the receiver by hand.
- `--stats-file PATH` publishes one windowed line instead:
  `min N max N mean N xruns N rate N buffer N ring N frames N seq N`. `seq` is a
  monotonically increasing window counter, so a reader can tell a new window from
  the same file being re-read.

## How pibridge uses it

`--receiver=~/hqdac` is the whole integration, and it changes two defaults
automatically:

- **Pacing becomes `backpressure`.** The clock pacer was written against aplay
  (wait for aplay to fill, then drain it). Against hqdac it fought the receiver:
  measured, clock pacing left a 20 ms request at 5 ms of fill while backpressure
  held 20.08 ms. An explicit `--pace` still wins.
- **`--target-time` becomes half the buffer**, i.e. the ring depth.

The PiBridge app has a **Receiver** field in Settings (default `~/hqdac`), sends
`--receiver` and `--target-ms` on every play, and migrates the old 60 ms buffer
default to 8 ms once. `pibridge diag` reports the installed receiver and its
confirmed formats; a missing receiver is one clear error before any stream opens
rather than ssh exiting 127 mid-playback.

## Files

```
hqdac.c            the receiver, one translation unit
build-static.sh    Docker build -> hqdac-aarch64 (static, stripped, ~714 KB)
```
