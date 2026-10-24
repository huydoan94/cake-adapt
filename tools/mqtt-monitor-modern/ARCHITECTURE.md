# Maintaining the monitor

Read this file, then follow the named entry point for the part you want to
change. The app is a read-only MQTT viewer. Data, chart choices and credentials
stay in memory; no export or telemetry files are written by normal operation.

## Follow one message

```text
CaptureSession.Receive          MQTT callback: bound payload, enqueue, return
  -> CaptureSession.ProcessAsync  one worker: UTF-8 decode
  -> MetricStore.Accept           acquire the store lock
     -> RecordDecoder             split records, resolve schemas and validity
     -> MetricSeries              latest reading, pending peak and history
     -> RatePeaks                 shared traffic-card peaks per topic/direction

MainWindow timer                  publish pending batches once per screen tick
  -> MetricStore.Snapshot         copy measurements under the same store lock
  -> MetricsPage                  search and update stable rows
  -> DashboardPage                update cards and chart panels
  -> EventsPage                   display bounded raw messages
```

The parser never updates controls. Controls never touch live series state.
Snapshots contain the retained history points for the selected time window.
Manual refreshes do not publish a new batch.

## Ownership

- **CaptureSession** owns the MQTT client, queue, worker tasks and cancellation.
  `Start` validates settings and creates options before starting workers.
  `RunNetworkAsync` connects/subscribes, retries and finally disposes the client
  and completes the queue. `StopAsync` cancels networking and waits, with time
  limits, for networking and decoding. **MqttOptions** owns option construction
  and private-CA certificate validation; it preserves hostname and server-auth
  checks. **BrokerSettings** owns broker/topic input validation.
- **MetricStore** is the only synchronization boundary for measurements. All
  decoding, header-cache changes, series updates, batch publication and history
  reads happen while holding its lock. `RecordDecoder`, `RecordSchemas`,
  `MetricSeries`, `MetricHistory` and `RatePeaks` need no independent locks.
  Store counters are read under the same lock.
- **RecordSchemas** owns built-in defaults and the bounded dynamic header cache.
  Headers are topic/record scoped. Each dynamic entry owns one FIFO node;
  replacement removes that node before appending the new definition. Entry
  count and UTF-8 byte limits both apply. **RecordDecoder** handles cells,
  timestamp fallback, reflector/CPU identity and invalid queue markers.
- **MetricSeries** owns raw latest, history and pending/displayed batch peaks
  for one complete `MetricKey`. Removing a series removes all that state.
  **RatePeaks** intentionally shares only traffic-card peaks across records of
  the same topic/direction. Table and card batch rules differ: invalid-only
  table batches display invalid data; traffic cards retain the last valid peak.
- **MainWindow** owns the active session, timer, source/time filters and selected
  metric. It routes snapshots to pages. Its layout partial builds the shell;
  its connection partial handles connect/disconnect/close. Pages own their
  controls and view-specific state. All UI operations run on the UI thread.
- **ChartPanel** owns one report selector and its topic-specific trace choices.
  It receives data through `Update` and raises `RefreshRequested` when needed.
  Its containing window is used only to display dialogs/snapshots; there is no
  dependency on `MainWindow`. **PlotSeries** prepares labels and display units.
  **PlotScale** converts measurements to coordinates. **TrendChart** only draws
  axes, continuous traces and hover text. Expanded charts are frozen snapshots.
- **MetricsPage** owns search and the table. **MetricRows** reconciles changes
  without replacing retained row objects or the collection; this preserves
  selection and both scroll axes. **MetricRow** exposes batch display values
  and historical statistics without changing the underlying snapshot.

## Where to make a change

| Change | Start here | Verify with |
| --- | --- | --- |
| Broker input/subscription validation | `Core/Connection/BrokerSettings.cs` | Connection/BrokerSettingsChecks |
| TLS or MQTT options | `Core/Connection/MqttOptions.cs` | Connection/TlsChecks |
| Retry, queue or shutdown | `Core/Connection/CaptureSession.cs` | Connection/MqttChecks and TlsChecks |
| New built-in record | `schemas.json` | Metrics/MetricStoreChecks and DecoderChecks |
| Dynamic records, dimensions or invalid values | `Core/Metrics/RecordDecoder.cs`, `RecordSchemas.cs` | Metrics/DecoderChecks |
| Table batch peaks | `Core/Metrics/MetricSeries.cs` | Metrics/BatchPeakChecks and UI/MetricTableChecks |
| Card traffic peaks | `Core/Metrics/RatePeaks.cs` | Metrics/RatePeakChecks and UI/DashboardChecks |
| History retention/downsampling | `Core/Metrics/MetricHistory.cs` | Metrics/HistoryChecks |
| Report fields or trace preferences | `Core/Reports/ReportCatalog.cs`, `ChartSelections.cs` | Reports/SelectionChecks and UI/ChartSettingsChecks |
| Chart names, units or drawing | `UI/Charts/PlotSeries.cs`, `PlotScale.cs`, `TrendChart.cs` | UI/ChartPanelChecks and ChartRenderingChecks |
| Table columns or row updates | `UI/Resources/MetricColumns.cs`, `UI/Metrics/` | UI/MetricTableChecks |
| Colors, labels or spacing | `UI/Resources/` | Headless previews and relevant UI checks |
| Page layout | `UI/Pages/` | Relevant UI checks |
| Shell or connection controls | `UI/MainWindow.Layout.cs`, `MainWindow.Connection.cs` | Headless UI and connection checks |

Each semantic default or visual value has a named home in `Core/Constants/` or
`UI/Resources/`. Reuse these instead of putting policy in event handlers. Keep
ordinary structural zero/one arithmetic and explicit test expectations local.
Do not add a wrapper/interface unless another real implementation needs it.

## Build and test

Use the commands in [README.md](README.md#build-and-checks). `Tests/Program.cs`
is the short test runner. Its partial files group checks by subsystem; sample
telemetry is confined to `Tests/UI/SampleTelemetry.cs`. All previous protocol,
history, batch, selection and scrolling checks remain part of that runner.
Tests use loopback brokers and temporary certificates; they do not connect to
a user's broker. UI checks render in Avalonia's Linux headless backend.

A successful Windows cross-publish verifies compilation and packaging, not
Windows execution. Dependencies and the two-project build boundary are unchanged.
