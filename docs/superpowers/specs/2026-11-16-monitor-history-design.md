# Monitor history mode — design

Date: 2026-11-16. Component: `tools/mqtt-monitor-modern` (Windows MQTT monitor).

## Intent

The monitor shows only live telemetry, from the moment it connects, for one
hour. The user wants to look back at saved captures — the recorder's daily
`mqtt-YYYY-MM-DD.jsonl` / `.jsonl.gz` files on the NAS — and watch any time
range in them with the same dashboard, "like a video editor": a strip along
the bottom shows the whole loaded history, two handles choose the start and
end, and the panels above show that range, scaled to fit it.

## User decisions

- **Two separate modes, chosen when the program starts:** *Watch live*
  (today's behaviour: connect to a broker) or *Open history* (load capture
  files). A session is one or the other: live cannot load history and history
  cannot connect.
- **Timeline strip at the bottom** covering the whole loaded history, with a
  **start handle and an end handle** to choose the range.
- **Panels above work as in live mode, scaled to the range.** Time-axis ticks
  follow the range: about hourly ticks for 6 hours, per-minute ticks for
  5 minutes.
- **Approach A** (below): read the files once into compact in-memory buckets.

## Goals

- Open one or more capture files (plain or gzip) and browse any range inside
  them, from a few minutes to all loaded days.
- Moving or resizing the range redraws immediately (no file reading).
- Same cards, charts (envelopes, latency clipping, bufferbloat ticks), All
  metrics table and Events page as live mode, applied to the range.
- Live mode is unchanged.

## Non-goals

- Mixing live and loaded data on one timeline (a DVR-style view).
- Writing anything: no exports, caches, indexes or settings on disk. The app
  stays read-only, as its README promises; history only *reads* files.
- Raw per-message replay or playback animation.
- Other file formats (the old text captures, CSV).

## Anti-goals

- A range move that waits on disk or decompression.
- Memory that grows without bound with the number of days opened.
- Any change in how live mode behaves or performs.

## Constraints and facts

- Recorder format: one JSON object per line with `tst`, `topic`, `payload`
  (mosquitto_sub `-F %j`); `payload` is a cake-adapt record line.
- Volume (measured from the 2026-11-15 replay slice): about 8 million records
  per day for two routers (TCP_QUEUE and DATA about 45% each, LOAD about 11%),
  about 450-600 numeric series per day as reflectors rotate.
- Times: series are placed by the record's `LOG_TIMESTAMP` (router Unix time,
  as live charts do), falling back to `tst`. The strip and axes show PC-local
  time.
- Existing code to reuse: `RecordDecoder`/`RecordSchemas` (dynamic headers
  included), `ReportCatalog`, `PlotSeries`, `TrendChart` (envelope drawing),
  `BloatEvents`, `ChartPanel`, `RateCard`, `MetricRows`.

## Approaches

- **A. Read once into compact time buckets (chosen).** Stream the files once
  through the existing decoder; keep each series as per-second min/max/mean in
  plain numeric arrays (not objects), plus pre-computed 10 s, 1 min and
  10 min levels. Range changes read only memory. Estimated about 120 MB and
  about 30 s per day loaded (to be measured during the build).
- **B. Index files, re-read the range on demand.** Small memory, unlimited
  days, but every handle move waits seconds, and gzip cannot seek, so it is
  slowest exactly while dragging.
- **C. Reuse the live store with longer retention.** It keeps every reading as
  an object (up to five per bucket); gigabytes per day. Rejected.

## Design

### Start choice and mode switch

- At start a small window offers **Watch live** and **Open history**. Live opens
  today's Connect dialog; history opens a Windows file picker (multi-select,
  `*.jsonl;*.jsonl.gz`).
- A **Start over** header button returns to the choice and discards the session
  (disconnects, or frees loaded history). *(My call; the user asked only for the
  start choice.)*

### Loading

- A loader streams each file (`GZipStream` for `.gz`), parses each line's
  `topic` and `payload`, decodes with a fresh `RecordDecoder`, and feeds a
  `HistoryStore`. Files are read in parallel, one task per file, and merged.
- Progress (files, records, percent of bytes) with **Cancel**; cancelling
  returns to the start choice.
- Bad lines are counted and shown like the live schema-error counter; they never
  stop the load.
- Limit: at most 7 days per session (refuse more with a message). *(My call; it
  bounds memory to about 1 GB.)*

### HistoryStore (Core)

- Per series (same `MetricKey` as live): sorted second-level buckets holding
  min, max, sum and count (`float`/`int` arrays), and derived 10 s / 1 min /
  10 min levels built after loading.
- **Detail tiers:** report fields used by the dashboard charts (rates, delays,
  CPU, memory, TCP queues and counters) keep 1-second detail; every other field
  keeps 1-minute detail. *(My call, to keep memory near 120 MB/day.)*
- Bufferbloat: reuse `BloatEvents` counting, without the one-hour trim, kept per
  second.
- Text and non-data records (INFO, SHAPER, SYSLOG, WARNING, ERROR): kept as
  events with their times, for the Events page.
- Latest text per series is kept for text fields shown in the table.
- `Read(key, start, end, columns)` picks the coarsest level that still gives at
  least one bucket per column and returns `Sample`s (each bucket as its min and
  max reading, so the existing envelope drawing keeps spikes).

### Screen in history mode

- Header: title, loaded file names and total range; **Start over**.
- Toolbar: source filter as in live; the time-window choice and Pause are
  hidden (the range comes from the strip).
- Dashboard, All metrics and Events show only the chosen range:
  - **Cards:** the peak within the range per source (download/upload), and the
    latest CAKE rate within the range.
  - **Charts:** the existing panels, `EndTime`/`Seconds` set from the range;
    automatic trace choice ranks series by readings in the range instead of
    "most recently received".
  - **All metrics:** min/mean/max/points over the range; Latest is the last
    value in the range; Age is hidden.
  - **Events:** the kept events within the range, newest first.
- **Time ticks** (charts and strip) use "nice" steps chosen from 1 s, 5 s, 10 s,
  30 s, 1 min, 5 min, 10 min, 30 min, 1 h, 3 h, 6 h, 12 h and 1 day so that
  about 4-8 ticks fit: 6 hours gives hourly ticks, 5 minutes gives per-minute
  ticks. Labels show the date when the range crosses midnight. Live mode keeps
  its current axis. *(Applying nice ticks to live too is a later choice.)*

### Timeline strip

- Full width under the pages, about 70 px tall: an overview trace of download
  traffic per source (1-minute level, source colours) and a bufferbloat density
  row under it, with day and hour labels.
- Two handles (start, end); the range between them is shaded. Dragging the
  shaded part pans the range without changing its length. Minimum range
  1 minute. The first view is the last hour of the loaded data.
- Panels redraw while dragging (memory reads only).

## Tests

- Loader: plain and gzip files, multiple files, a bad line, cancel.
- HistoryStore: bucket values per level, tier selection, `Read` level choice,
  bufferbloat per second, events in range.
- Ticks: nice-step selection for 5 min, 1 h, 6 h, 3 days; midnight labels.
- UI (headless): start choice routes to each mode; strip handles set the
  panels' range; pan keeps the length; Start over clears the session.
- Performance: load one replayed day; record load time, memory, and redraw
  time while dragging. Check on the Windows VM.

## Left to the builder

- Exact strip and handle styling, and the start-window layout.
- Bucket array layout and how levels are stored.
- Parallel-load merge details, and how load progress is reported.
