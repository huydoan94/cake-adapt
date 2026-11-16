using System.Globalization;
using Avalonia.Platform.Storage;

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
        var load = new LoadWindow();
        var loaded = paths.Length == 0 ? null : await load.RunAsync(this, paths);
        if (loaded == null)
        {
            notice.Text = load.Error.Length > 0 ? UiText.LoadFailed + load.Error : "";
            await ChooseModeAsync();
            return;
        }
        EnterHistory(loaded, paths.Select(Path.GetFileName).OfType<string>().Order(StringComparer.Ordinal).ToArray());
    }

    public void EnterHistory(HistoryStore store, IReadOnlyList<string> names)
    {
        history = store;
        rangeEnd = store.End;
        rangeStart = Math.Max(store.Start, store.End - HistoryModeDefaults.InitialRangeSeconds);
        strip.DataStart = store.Start;
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

    private void OnRangeChanged(double start, double end) => SetHistoryRange(start, end);

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
