# cake-adapt Monitor — live visualization only

A native Windows x64 MQTT viewer built with Avalonia's Fluent dark theme and
.NET 10. Extract the Windows ZIP and run **CakeAdaptMonitor.exe**. Its runtime
is included; no Python or separate .NET installation is needed.

## Connect

Choose **Connect broker**, enter the broker address and credentials, and use
**cake-adapt/#**. TLS is enabled by default on port 8883; disabling TLS selects
1883. An optional custom CA PEM path supports private certificate authorities.
Trust, hostname and server-auth certificate usage are verified.

The app only subscribes and displays data. It has **no CSV export, telemetry
file logging, or saved connection settings**. Broker details are held only for
the current run. Closing the app discards its metric history, event view and
connection details. Reconnect and resubscribe are automatic while it is open.
The optional CA certificate is read from the path you provide.

## History mode

At start the program asks: **Watch live** (connect to a broker, as described
above) or **Open history**. A session is one or the other; **Start over** in
the header returns to this choice.

Starting the program with a capture file as its argument (Windows "Open
with", or dropping the file on the executable) opens it in history mode
directly. Open history loads one recorder capture file (the recorder writes one per
router per day, `<YYYY-MM-DD>/<router>.jsonl` or `.jsonl.gz`; each line is a
JSON object with `tst`, `topic` and `payload`), with progress and Cancel. Lines that are not valid recorder
JSON (for example a cut-off last line) are counted and skipped. Readings are
placed by the router's `LOG_TIMESTAMP`; a router time more than a day away
from the recorder's `tst` falls back to `tst`.

A strip along the bottom shows the whole loaded history (download traffic per
source and a bufferbloat row). Drag its start and end handles to choose the
range (at least one minute), or drag the shaded range to move it. It opens on
the last hour of the data. Every page shows that range:

- Charts end at the range end and label the time axis to fit it (per-minute
  ticks for 5 minutes, hourly for 6 hours, with dates across midnight).
  Automatic traces are the series with the most readings in the range.
- Cards show each source's peak in the range and when it happened; CAKE rate
  cards show the last value in the range.
- All metrics shows exact min/mean/max/points over the range; Age is hidden.
- Events shows the text records (INFO, SHAPER, warnings) in the range.

Loaded data stays in memory only. Dashboard report fields (rates, delays,
CPU, memory, TCP queues and counters) keep one-second detail; other fields
keep one-minute detail. Range statistics are read from 10 s, 1 min and 10 min
summaries (exact at the edges), and chart points are read only for the traces
drawn, so moving the range stays quick on a full day.

Text fields (such as load conditions) are kept to the minute: the table shows
the last text of the minute the range ends in.

Measured on real captures (2026-10-31, one router per file, about 3.2-3.7
million records and 950-1,300 series a day): loading takes 30-35 s and holds
about 110-120 MB of history; a redraw for a 5-minute, 6-hour or 24-hour range
takes about 10 ms or less. While dragging, the strip follows the pointer and the
pages redraw once per UI cycle.

## Dashboard

- Existing chart selectors: throughput, latency, CPU, memory, TCP queues,
  TCP counters, and a metric selected from All metrics.
- The window opens maximized when its default size does not fit the screen, and
  the dashboard scrolls when the window is too short for its cards and charts.
  The minimum window size (1000 x 520) fits a 1366 x 768 screen at 125% scaling.
- Each card lists up to three sources (the most recently updated), one line
  each in topic order, so a line never jumps between routers. Captions name the
  source first, so a narrow card trims the age rather than the router.
- Download/Upload cards show the highest valid reading received between one-second
  screen updates. Their labels stay unchanged. Source/search/settings changes do
  not reset this interval. With no new valid reading, the previous peak remains
  and its age increases. CAKE rate cards still show latest configured values.
- On Windows every window asks for a dark title bar.
- Dark panels with colored line traces, hover values and expandable snapshots
  that keep the chart's legend.
  Each panel has **Settings** to choose which source/metric traces to draw
  within its selected report, up to 24 traces. **Use defaults** restores the
  automatic six most recently updated traces within each topic/field priority;
  with several sources the six are shared between them, and every label names
  its source.
  Automatic choices follow reflector rotation as new telemetry arrives, without
  pressing Use defaults again. Manually chosen traces stay pinned. Settings lists only the selected topic. Choices stay in memory separately
  per panel, report and topic, including a separate All sources view. Global
  fields use their LOAD/SUMMARY series when available instead of repeating
  the same field for each reflector. Closing the settings window cancels changes.
- Traces are averaged over fixed one-second windows on the clock (10:01:03 to
  10:01:04, and so on): a faint band runs from each second's lowest to highest
  reading, with a line through the second's mean, so spikes stay visible, the
  shape stays readable, and the line does not change while the chart scrolls.
  When a second is narrower than a pixel (wide windows), the window grows to the
  next of 2, 5, 10, 15, 30 or 60 seconds. Sparse traces keep exact lines.
- Latency: when the highest reading is more than three times the 99th
  percentile, the scale tops out at 1.25 x the 99th percentile; clipped spikes
  get a marker at the top edge, the unit label says so, and hover shows the
  real values.
- Latency also shows bufferbloat: samples whose load condition ends in `_bb`
  (the router measured delay above its threshold) are counted per source and
  direction for the shown window and listed in the legend, and drawn as one row
  of ticks per source along the plot bottom (2-second buckets, router time).
  A source publishing DATA is counted from DATA only; otherwise from SUMMARY.
  Counts are kept for one hour in memory, like the rest of the history.
- Charts draw continuous lines between available valid readings across gaps.
  These lines are visual interpolation and do not invent additional readings.
- Source and time-window filters apply across views. Pause charts freezes the
  plots while reception, decoding, status and the table continue updating.
- All metrics lists Metric, Topic and Reflector / CPU first, groups rows by
  metric and source, and fits a maximized 1280-pixel window without scrolling
  sideways. Numbers use one plain format without thousands separators. Latest shows the highest valid numeric
  reading received between screen updates, separately for each topic, record,
  reflector/CPU and field. It preserves original precision and the peak's age.
  Text fields show their last text; invalid-only batches show the invalid state.
  If no readings arrive, the previous display remains and its age increases.
  Source/search/settings refreshes do not reset batches. Historical min/mean/max and point counts keep
  their existing meaning. Live updates preserve scroll position and selection.
  Double-click a row to visualize that metric in the fourth panel.
- Events shows the latest raw messages in memory for the selected source,
  newest first.

Supported records are DATA, LOAD, SUMMARY, REFLECTOR, TCP_QUEUE, TCP_INJECT,
MEMORY, CPU and CPU_RAW. Built-in schemas come from the daemon's
`src/logging/records.c`; per-topic `*_HEADER` messages support additional
fields/types. Unknown text remains visible in Events. Only measurements
actually enabled and published by the router can be shown.

Chart units: Mbit/s for rates, milliseconds for delay, MiB for memory, percent
for CPU usage. Table values retain raw daemon units. CPU ticks and injection
counters remain cumulative totals, not derived rates. Min/mean/max summarize
retained representative points, not every received reading or time-weighted averages. Reflectors, CPUs and source
instances remain distinct. Overview cards name their latest source rather
than summing different routers.

History retains up to **one hour per metric and 2,048 series** in memory.
The newest ten minutes use two-second buckets; older history uses ten-second
buckets. Each bucket keeps actual first, last, minimum and maximum readings
and an invalid marker when present, preserving spikes without retaining every
message. Window boundaries and statistics use these representative points.
In live mode history begins when this app receives data; saved captures are viewed in history mode. Incoming messages are bounded to 2,048 queued payloads of at
most 64 KiB each; received/dropped counts are visible. Dynamic headers are
bounded to 256 definitions and 1 MiB of text. Events retains 150 messages with
up to 2,000 characters each. All these buffers are in memory only.

The time window uses PC receive time; plot labels use router Unix timestamps
in PC-local time. The axis advances when publishing stops, so stale traces move
left. Invalid timestamps fall back to receive time. Routers should have an
accurate clock. Expanded charts are labeled snapshots.

## Build and checks

With a .NET 10 SDK:

```sh
dotnet build CakeAdaptMonitor.csproj -c Release
dotnet run --project Tests/Tests.csproj -c Release -- preview.png
dotnet publish CakeAdaptMonitor.csproj -c Release -r win-x64 --self-contained true \
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o dist/win-x64
```

On Windows, `build-windows.cmd` publishes the executable. Restore obtains
Avalonia, MQTTnet and runtime packages from NuGet. Framework references:
[Avalonia themes](https://docs.avaloniaui.net/docs/styling/theme-variants),
[MQTTnet client samples](https://github.com/dotnet/MQTTnet/tree/master/Samples/Client).

Checks cover parser/cache bounds, schema and source separation, numeric
handling, real loopback MQTT reception and in-memory decoding, TLS trust/name/EKU
verification, shutdown including pending handshakes, native dark rendering,
metric search, both scroll axes, selection retention, per-chart trace selection/defaults/cancellation, full-hour
retention, peak preservation, history bounds and rendered gap bridging.
Tests create temporary test certificates and preview images; the normal viewer
does not save telemetry or settings. Preview data is explicitly labeled SAMPLE
DATA and is accessible only from the test program.

The Windows executable is cross-published from WSL. Native headless UI and
protocol checks run on Linux; Windows execution has been checked on a Windows 10
test VM against a local replay of saved captures; the user's broker has not
been tested here.

## GitHub Actions

The **Build OpenWrt packages and Windows dashboard** workflow tests and builds
this viewer on dashboard changes pushed to main, version-tag pushes, and manual
runs. Dashboard-only changes do not require a router package version bump.
The job runs the unit, loopback MQTT/TLS and headless UI checks on Linux, then
cross-publishes the same self-contained Windows x64 executable used locally.
This validates the build; it does not execute the application on Windows.

Download **dashboard-windows-x64** from the run's Artifacts section. It contains
`CakeAdaptMonitor-Windows-x64.zip` and its `.sha256` checksum. Extract the inner
ZIP and run `CakeAdaptMonitor.exe`. Successful release runs attach these same
two files alongside the OpenWrt/LuCI packages; a failed dashboard build blocks
that release along with failed router package builds.

## Code structure

Start with [ARCHITECTURE.md](ARCHITECTURE.md) for the data flow, ownership rules
and a guide to common changes. The tool uses two projects: `Core/` has no UI
dependency; the executable contains Avalonia presentation and startup code.

| Location | Owns |
| --- | --- |
| `Core/Connection/` | Broker validation, MQTT/TLS setup, bounded reception and shutdown |
| `Core/Metrics/` | Record/schema decoding, synchronized store, per-series batches and history |
| `Core/Reports/` | Report membership and independent topic/report trace choices |
| `Core/Constants/` | Protocol names, units, limits and defaults |
| `UI/MainWindow*.cs` | Shell, snapshot distribution and connection lifetime |
| `UI/Pages/` | Dashboard, All metrics and Events |
| `UI/Metrics/` | Stable table rows that preserve scrolling and selection |
| `UI/Charts/` | Chart choices, display series, coordinate conversion and drawing |
| `UI/Controls/`, `UI/Dialogs/` | Shared controls and input windows |
| `UI/Resources/` | Text, colors, typography, spacing and column definitions |
| `Tests/` | Matching Connection, Metrics, Reports and UI checks |

No dependency injection container, extra UI framework or persistence layer is
needed. Pages receive snapshots; chart panels request refresh through an event
and never read the main window's internal state. Constants stay in their owning
resource/default files. Structural arithmetic, diagnostics and explicit test
expectations stay local. `schemas.json` remains the embedded built-in protocol
schema. The build commands and executable name are unchanged.
