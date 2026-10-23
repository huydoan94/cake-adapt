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

## Dashboard

- Existing chart selectors: throughput, latency, CPU, memory, TCP queues,
  TCP counters, and a metric selected from All metrics.
- Download/Upload cards show the highest valid reading received between one-second
  screen updates. Their labels stay unchanged. Source/search/settings changes do
  not reset this interval. With no new valid reading, the previous peak remains
  and its age increases. CAKE rate cards and All metrics still show latest values.
- Dark panels with colored line traces, hover values and expandable snapshots.
  Each panel has **Settings** to choose which source/metric traces to draw
  within its selected report, up to 24 traces. **Use defaults** restores the
  automatic first six. Settings lists only the selected topic. Choices stay in memory separately
  per panel, report and topic, including a separate All sources view. Global
  fields use their LOAD/SUMMARY series when available instead of repeating
  the same field for each reflector. Closing the settings window cancels changes.
- Charts draw continuous lines between available valid readings across gaps.
  These lines are visual interpolation and do not invent additional readings.
- Source and time-window filters apply across views. Pause charts freezes the
  plots while reception, decoding, status and the table continue updating.
- All metrics shows structured fields, latest values, min/mean/max, retained point count,
  source and age. Its live updates preserve scroll position and selection.
  Double-click a row to visualize that metric in the fourth panel.
- Events shows the latest raw messages in memory for the selected source.

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
History begins when this app receives data; there is no historical backfill. Incoming messages are bounded to 2,048 queued payloads of at
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
protocol checks run on Linux; Windows execution and the user's broker have
not been tested here.

## Code structure

The app remains Avalonia. The executable project contains only presentation
and startup code and references a platform-independent .NET core project.

| Location | Responsibility |
| --- | --- |
| `Core/CaptureSession.cs`, `BrokerSettings.cs` | Existing MQTT/TLS transport, bounded queue, decode worker and shutdown |
| `Core/MetricStore.cs`, `MetricHistory.cs` | Schema parsing, source identity and bounded in-memory history |
| `Core/MetricView.cs`, `MetricRow.cs`, `MetricRows.cs` | Snapshot models and stable table-row reconciliation |
| `Core/ReportCatalog.cs`, `ChartSelections.cs` | Report membership and topic-specific selection policy |
| `Core/RatePeaks.cs` | Bounded per-topic/direction peaks for traffic cards, advanced on the screen timer |
| `Core/Constants/` | Protocol identities, defaults, units, limits and history policy |
| `UI/MainWindow.cs` | UI state, navigation and refresh orchestration |
| `UI/MainWindow.Layout.cs` | Construction and binding of the native window |
| `UI/MainWindow.Connection.cs` | Connection dialog orchestration and window-close lifecycle |
| `UI/Controls/` | Chart panels and drawing |
| `UI/Dialogs/` | Connection and chart selection windows |
| `UI/Resources/` | Named colors, text, fonts, dimensions, chart geometry and column definitions |
| `UI/Program.cs` | Avalonia startup and dark-theme registration |

Core has no Avalonia dependency. Presentation refers to named resources;
semantic defaults and protocol values belong in their matching constants file.
Structural zero/one arithmetic, layout row/column indices, diagnostics, project
metadata and explicit expected test values remain local. Do not add new style
or policy values inside event handlers. `schemas.json` is embedded by the core
project with the original resource name. The existing headless UI and real
MQTT/TLS tests remain in `Tests/`; additional checks cover the project boundary,
selection edit isolation, trace limits and time-window defaults.
