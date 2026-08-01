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
| `blink_task`        | 1    | 5        | Periodic, 1000 ms          | writes `led_on`, `toggle_count`                          | N/A |
| `button_isr`        | 1    | HW ISR (preempts all tasks) | Aperiodic, on GPIO 18 NEGEDGE, 200 µs debounce gate | writes `isr_entry_time_us`, `presses_observed`; signals sem + notify | 21 µs |
| `btn_task_sem`       | 1    | 12       | Aperiodic, wakes on binary semaphore | reads `isr_entry_time_us`; writes `radar_hit_count`, `latency_*_sem_us` | 2280 µs |
| `btn_task_notif`     | 1    | 12       | Aperiodic, wakes on task notification | writes `latency_*_notif_us`                              | 31 µs |
| HTTP server task     | 0    | 5        | Event-driven, per HTTP request | reads all shared state fields via `handle_state()`        | Not latency-critical to the RT path — isolated on Core 0 |

**// The ~70× gap between the semaphore path (2280 µs) and 
notification path (31 µs) is larger than expected — see README &sec;5 
for a hypothesis on why (likely a round-robin scheduling tie-break 
between the two equal-priority bottom-half tasks).

## 6. Hazard analysis

| Hazard | Cause | Effect | Mitigation in this design | Severity (informal) |
|---|---|---|---|---|
| Beacon appears "online" after firmware hang | `blink_task` starved or crashed but last `led_on` value stays in memory | Ground station shows stale "Online" status — a UAV engineer's nightmare: false confidence | HTTP layer could add a last-updated timestamp / staleness check (not yet implemented — noted as future work) | High |
| RADAR contact missed | Binary semaphore already "given" (not yet taken) when a second press arrives before debounce window elapses | Contact count under-reports; no error surfaced | Documented in App 3 as a known binary-semaphore limitation; a counting semaphore or queue would close this gap | Medium |
| Torn read of latency stats | Bottom-half tasks write two related fields (`latency_last_*`, `latency_max_*`) as separate writes, not atomically | HTTP snapshot could show a "last" value newer than "max" for one instant | Benign for a dashboard (self-corrects next poll); would need a mutex or copy-on-read if this fed a safety-rated system | Low |
| Wi-Fi drop mid-mission | Simulated AP or real network instability | Dashboard goes dark; onboard tasks (beacon, ISR path) are unaffected since they don't depend on Wi-Fi | Core separation already isolates flight-critical-style tasks from the network stack — the actual design win to call out here | Medium (dashboard only, not "flight") |
| ISR starvation of lower-priority tasks | Rapid, sustained button presses each re-entering the ISR | HTTP task and blink task could be delayed if presses came fast enough and debounce didn't gate them | 200 µs debounce gate bounds worst-case interrupt rate | Low |

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
   This guarantees a RADAR contact is always serviced before the beacon gets another chance to run — the beacon can be delayed by a contact, but a contact can never be delayed by the beacon.
   With my measured worst case (2280 µs on the semaphore path) against a 1000 ms beacon period, a single contact costs the beacon about 0.23% of one period — negligible in isolation.
   This is a guarantee about ordering, not magnitude; a sustained burst of contacts is a different story.

2. **Since `blink_task` and both bottom-half tasks now share Core 1, could
   the beacon ever visibly stutter?**
   Yes, under sustained load. The 200 µs debounce gate caps the theoretical worst case around 5,000 contacts/second, and since both bottom-half tasks outrank the beacon, a burst anywhere near that rate would let priority-12 work monopolize Core 1 and starve blink_task for the burst's duration.
   Because vTaskDelayUntil tracks an absolute wake time instead of accumulating delay, the beacon wouldn't drift — it would skip its toggle windows during the burst, then resume on schedule the instant Core 1 frees up.
   That's a stutter (missed toggles, then a snap back to cadence), not gradual drift.
  
3. **Is there a priority-inversion risk introduced by combining these two
   apps?**
   No. Priority inversion requires a low-priority task holding a lock that a high-priority task is waiting on, while a medium-priority task preempts the lock holder.
   Nothing here fits that shape — blink_task and the bottom-half tasks don't share a mutex, only plain volatile variables with no locking at all. The only thing they compete for is Core 1's CPU time, arbitrated purely by scheduler priority, not by resource ownership.
   There's no lock, so there's nothing to invert.

4. **The HTTP handler reads six separate volatile fields with no lock.
   Why is this "benign-racy" rather than a real bug, and where would that
   stop being true?**
   Each field (led_on, toggle_count, presses_observed, radar_hit_count, and the two latency values) is a word-aligned volatile read, atomic on Xtensa, so no individual field is ever torn.
   What isn't guaranteed is that all six reflect the same instant — handle_state() could read presses_observed just after a new contact bumped it, but read radar_hit_count just before the bottom-half task caught up, giving a snapshot that's inconsistent by one event.
   That's harmless for a dashboard; the next poll 250 ms later self-corrects. It would stop being benign the moment this data drove a decision instead of a display — for example, if fault-detection logic combined led_on and toggle_count to decide something acted on in the real world, an inconsistent snapshot could       trigger a wrong call with real consequences. At that point I'd need either a mutex around the read or a designed snapshot copy taken under a short lock, so the whole set is atomic together, not just each field individually.

## 8. AI disclosure

- App 1 development: AI used to simulate the HTML browser display.
- App 3 development: AI used to assist with GPIO setup in `diagram.json`,
  fix an overflow error under `WITH_LOAD`, and generate the concurrency
  diagram.
- Capstone integration: AI (Claude) used to merge App 1 and App 3 into a
  single `main.c`, extend the `/state` JSON and web dashboard to report
  ISR/bottom-half latency alongside beacon state
- Organization: AI was used to organize the steps for the project
