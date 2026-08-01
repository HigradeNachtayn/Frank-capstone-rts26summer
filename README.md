# Capstone — UAV-01 Avionics Beacon & RADAR Contact Monitor

## 1. Theme banner

**Theme: Avionics.** I picked this because I currently work as an Unmanned
Aircraft System Engineer and build/race quadcopters outside of work — the
beacon/telemetry framing maps directly onto things I already reason about
day to day.

## 2. System summary

This capstone integrates App 1 (dual-core setup, blink, web monitor) and
App 3 (interrupt + bottom-half pattern) into one system: a simulated UAV
ground-station dashboard. Core 1 runs a 1 Hz **status beacon** task
(`blink_task`) that reports the UAV's online/offline state, plus two
**RADAR-contact bottom-half tasks** that are woken from a hardware
interrupt (a button on GPIO 18) simulating an incoming RADAR pulse. Core 0
runs Wi-Fi + an HTTP server that exposes both the beacon state and live
ISR-to-bottom-half latency telemetry as JSON, polled by a browser dashboard
at 4 Hz. App 2's synthetic background-load fixture was intentionally left
out of this integration — the scope here is beacon + interrupt handling,
not scheduler contention under load.

## 3. Wokwi link
https://wokwi.com/projects/471114279885448193

## 4. System architecture / concurrency diagram

See `system-architecture.svg` in this repo

Summary of the concurrency structure:

```
Core 0 (PRO_CPU)                         Core 1 (APP_CPU)
─────────────────                        ─────────────────
Wi-Fi driver                             blink_task (pri 5, periodic 1000 ms)
HTTP server task (pri 5)                   writes: led_on, toggle_count
  handle_root()  → static HTML+JS
  handle_state() → reads shared state    button_isr() [HW interrupt context]
                                            writes: isr_entry_time_us, presses_observed
        ▲ reads (all fields)               signals via sem + notify
        │                                        │           │
        │                                        ▼           ▼
        │                              btn_task_sem (12)  btn_task_notif (12)
        │                                writes: radar_hit_count,
        │                                latency_last/max_sem_us,
        │                                latency_last/max_notif_us
        └────────────────────────────────────────┘
                  shared volatile state, no mutex
                  (see Engineering Analysis §3 for why)
```

## 5. Task table + WCET evidence

| Task / ISR         | Core | Priority | Type / Period            | Shared state touched                                   | Measured max latency or WCET |
|---------------------|------|----------|---------------------------|----------------------------------------------------------|-------------------------------|
| `blink_task`        | 1    | 5        | Periodic, 1000 ms          | writes `led_on`, `toggle_count`                          | N/A (deadline is the period; report jitter if measured) |
| `button_isr`        | 1    | HW ISR (preempts all tasks) | Aperiodic, on GPIO 18 NEGEDGE, 200 µs debounce gate | writes `isr_entry_time_us`, `presses_observed`; signals sem + notify | *(re-measure: GPIO18 fall → GPIO19 pulse)* |
| `btn_task_sem`       | 1    | 12       | Aperiodic, wakes on binary semaphore | reads `isr_entry_time_us`; writes `radar_hit_count`, `latency_*_sem_us` | *(re-measure without App 2 load; prior App 3 idle baseline: 2320 µs)* |
| `btn_task_notif`     | 1    | 12       | Aperiodic, wakes on task notification | writes `latency_*_notif_us`                              | *(re-measure without App 2 load; prior App 3 idle baseline: 1922 µs)* |
| HTTP server task     | 0    | 5        | Event-driven, per HTTP request | reads all shared state fields via `handle_state()`        | Not latency-critical to the RT path — isolated on Core 0 |

**Fill in the "re-measure" cells** from your own logic-analyzer or serial-log
run on the merged build — see App 3's "Capturing latency with Wokwi's logic
analyzer" instructions, unchanged in this integration.

## 6. Hazard analysis

| Hazard | Cause | Effect | Mitigation in this design | Severity (informal) |
|---|---|---|---|---|
| Beacon appears "online" after firmware hang | `blink_task` starved or crashed but last `led_on` value stays in memory | Ground station shows stale "Online" status — a UAV engineer's nightmare: false confidence | HTTP layer could add a last-updated timestamp / staleness check (not yet implemented — noted as future work) | High |
| RADAR contact missed | Binary semaphore already "given" (not yet taken) when a second press arrives before debounce window elapses | Contact count under-reports; no error surfaced | Documented in App 3 as a known binary-semaphore limitation; a counting semaphore or queue would close this gap | Medium |
| Torn read of latency stats | Bottom-half tasks write two related fields (`latency_last_*`, `latency_max_*`) as separate writes, not atomically | HTTP snapshot could show a "last" value newer than "max" for one instant | Benign for a dashboard (self-corrects next poll); would need a mutex or copy-on-read if this fed a safety-rated system | Low |
| Wi-Fi drop mid-mission | Simulated AP or real network instability | Dashboard goes dark; onboard tasks (beacon, ISR path) are unaffected since they don't depend on Wi-Fi | Core separation already isolates flight-critical-style tasks from the network stack — the actual design win to call out here | Medium (dashboard only, not "flight") |
| ISR starvation of lower-priority tasks | Rapid, sustained button presses each re-entering the ISR | HTTP task and blink task could be delayed if presses came fast enough and debounce didn't gate them | 200 µs debounce gate bounds worst-case interrupt rate | Low |

*(Optional industry-standard mapping: if you want to push this further,
DO-178C's hazard/failure-condition categories — Catastrophic / Hazardous /
Major / Minor / No Effect — map reasonably well onto the severity column
above; ARP4761 is the standard that formalizes the hazard-analysis process
itself if you want to cite something specific in your reflection.)*

## 7. Engineering analysis — carried over + extended

### From App 1 (unchanged)
1. A single super-loop polling the web server *and* blinking the LED could
   miss its 1 Hz deadline if a client request took a long time to service;
   two tasks let each run independently without impacting the other.
2. Core pinning means a CPU-heavy HTTP server can't steal cycles from the
   time-critical blink task, since they're not competing for the same core.
3. `led_on` is a single word-aligned bool — one atomic read/write, and
   `volatile` stops the compiler from caching it in a register. A struct
   with two fields needs two separate operations, so a reader could
   observe one field updated and the other not yet.

### From App 3 (unchanged, measured under App 2's load — see note above)
1. ISR contents and rationale — unchanged from the App 3 scaffold; the same
   division (hardware-safe work in the ISR, everything blocking/logging in
   the bottom half) applies here.
2. Notification path was faster than the semaphore path (1922 µs vs 2320 µs
   idle) because a notification writes directly into the task's control
   block, while a semaphore updates a kernel object the task must then take.
3. Under App 2's load, Task A (priority 15) was the only load task able to
   delay the bottom half, since B/C/D (10/5/2) rank below it.

### New questions for this integration
1. **Why does `btn_task_sem`/`btn_task_notif` (priority 12) still outrank
   `blink_task` (priority 5) in this merged build, and what does that
   ordering guarantee?**

2. **Since `blink_task` and both bottom-half tasks now share Core 1, could
   the beacon ever visibly stutter?**

3. **Is there a priority-inversion risk introduced by combining these two
   apps?**

4. **The HTTP handler reads six separate volatile fields with no lock.
   Why is this "benign-racy" rather than a real bug, and where would that
   stop being true?**


## 8. AI disclosure

- App 1 development: AI used to simulate the HTML browser display.
- App 3 development: AI used to assist with GPIO setup in `diagram.json`,
  fix an overflow error under `WITH_LOAD`, and generate the concurrency
  diagram.
- Capstone integration: AI (Claude) used to merge App 1 and App 3 into a
  single `main.c`, extend the `/state` JSON and web dashboard to report
  ISR/bottom-half latency alongside beacon state
