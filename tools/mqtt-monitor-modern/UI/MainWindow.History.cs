using System.Globalization;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace CakeAdaptMonitor;

// Start choice (live or history) and the history session: loaded store, chosen range, strip.
public sealed partial class MainWindow
{
    private HistoryStore? history;
    private double rangeStart, rangeEnd;
    public bool InHistory => history != null;

    public async Task ChooseModeAsync()
    {
        var choice = await new StartWindow().ShowDialog<StartChoice?>(this);
        if (choice == null)
        {
            Close();
            return;
        }
        if (choice == StartChoice.Live)
        {
            await ToggleConnectionAsync();
            if (session == null)
                await ChooseModeAsync();
            return;
        }
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType(UiText.CaptureFilter) { Patterns = UiText.CapturePatterns }]
        });
        var paths = files.Select(file => file.TryGetLocalPath()).OfType<string>().ToArray();
        if (paths.Length == 0 || !await OpenHistoryAsync(paths))
            await ChooseModeAsync();
    }

    // Load capture files and enter history mode; false (with the reason shown) when
    // cancelled or failed.
    public async Task<bool> OpenHistoryAsync(IReadOnlyList<string> paths)
    {
        var load = new LoadWindow();
        var loaded = await load.RunAsync(this, paths);
        if (loaded == null)
        {
            notice.Text = load.Error.Length > 0 ? UiText.LoadFailed + load.Error : "";
            return false;
        }
        // Only headers or only bad lines (a new day's file just after midnight): nothing to show.
        if (!double.IsFinite(loaded.Start) || !double.IsFinite(loaded.End))
        {
            notice.Text = UiText.NoRecords;
            return false;
        }
        EnterHistory(loaded, paths.Select(Path.GetFileName).OfType<string>().Order(StringComparer.Ordinal).ToArray());
        return true;
    }

    public void EnterHistory(HistoryStore store, IReadOnlyList<string> names)
    {
        history = store;
        // A capture shorter than the minimum range (a single moment) still gets one minute.
        var dataStart = Math.Min(store.Start, store.End - HistoryModeDefaults.MinimumRangeSeconds);
        rangeEnd = store.End;
        rangeStart = Math.Max(dataStart, store.End - HistoryModeDefaults.InitialRangeSeconds);
        strip.DataStart = dataStart;
        strip.DataEnd = store.End;
        strip.RangeStart = rangeStart;
        strip.RangeEnd = rangeEnd;
        strip.Overview = store.Overview(UiLayout.OverviewColumns);
        strip.Bloat = store.Bufferbloat(store.Start, store.End);
        SetHistoryControls(true);
        subtitle.Text = string.Format(UiText.HistorySubtitleFormat, string.Join(", ", names), Clock(store.Start), Clock(store.End));
        status.Text = UiText.HistoryStatus;
        strip.InvalidateVisual();
        Refresh(true);
    }

    public async Task StartOverAsync()
    {
        if (session != null)
            await DisconnectAsync();
        history = null;
        store = new MetricStore();
        selectedMetric = null;
        SetHistoryControls(false);
        subtitle.Text = UiText.InitialSubtitle;
        status.Text = ConnectionStatus.Disconnected;
        notice.Text = "";
        Refresh(true);
        await ChooseModeAsync();
    }

    private void SetHistoryControls(bool historyMode)
    {
        metricsPage.SetHistoryMode(historyMode);
        strip.IsVisible = historyMode;
        timeRange.IsVisible = !historyMode;
        pause.IsVisible = !historyMode;
        connect.IsVisible = !historyMode;
    }

    // Dragging raises many range changes per frame: move the strip at once, but redraw
    // the pages at most once per UI cycle.
    private bool redrawPending;

    private void OnRangeChanged(double start, double end)
    {
        rangeStart = start;
        rangeEnd = end;
        if (redrawPending)
            return;
        redrawPending = true;
        Dispatcher.UIThread.Post(() =>
        {
            redrawPending = false;
            if (history != null)
                Refresh(true);
        }, DispatcherPriority.Background);
    }

    public void SetHistoryRange(double start, double end)
    {
        rangeStart = start;
        rangeEnd = end;
        strip.RangeStart = start;
        strip.RangeEnd = end;
        strip.InvalidateVisual();
        Refresh(true);
    }

    // Every page shows the chosen range: charts end at its end, cards show its peaks,
    // the table its statistics and Events its text records.
    private void RefreshHistory(HistoryStore loaded)
    {
        var snapshot = loaded.Snapshot(rangeStart, rangeEnd);
        UpdateSources(snapshot);
        var topic = source.SelectedItem as string ?? ChartDefaults.AllSources;
        bool Shown(string item) => topic == ChartDefaults.AllSources || item == topic;
        var visible = snapshot.Where(metric => Shown(metric.Topic)).ToArray();
        metricsPage.Update(visible);
        dashboard.Update(visible, selectedMetric, rangeEnd - rangeStart, topic, true,
            loaded.Bufferbloat(rangeStart, rangeEnd).Where(view => Shown(view.Topic)).ToArray(), rangeEnd,
            key => loaded.Samples(key, rangeStart, rangeEnd, UiLayout.HistoryColumns));
        eventsPage.Update(loaded.Events(rangeStart, rangeEnd), topic);
        counts.Text = string.Format(UiText.HistoryCountsFormat, loaded.Records, loaded.BadLines, snapshot.Length, loaded.SchemaErrors);
    }

    private static string Clock(double time) => DateTimeOffset.FromUnixTimeMilliseconds((long)(time * TelemetryDefaults.MillisecondsPerSecond))
        .ToLocalTime().ToString(UiText.HistoryRangeFormat, CultureInfo.InvariantCulture);
}
