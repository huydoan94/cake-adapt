using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Threading;
using static CakeAdaptMonitor.UiControls;

namespace CakeAdaptMonitor;

// Owns session lifetime and distributes one snapshot to independent pages.
public sealed partial class MainWindow : Window
{
    private readonly ComboBox source = new()
    {
        MinWidth = UiLayout.SourceWidth,
        ItemsSource = new[] { ChartDefaults.AllSources },
        SelectedIndex = 0
    };
    private readonly ComboBox timeRange = new()
    {
        MinWidth = UiLayout.TimeRangeWidth,
        ItemsSource = ChartDefaults.WindowLabels,
        SelectedIndex = ChartDefaults.DefaultWindowIndex
    };
    private readonly TextBlock status = Label(ConnectionStatus.Disconnected, UiTypography.Body);
    private readonly TextBlock counts = Label(UiText.InitialCounts, UiTypography.Caption, UiTheme.Muted);
    private readonly TextBlock title = Label(UiText.OverviewTitle, UiTypography.PageHeading);
    private readonly TextBlock subtitle = Label(UiText.InitialSubtitle, UiTypography.Caption, UiTheme.Muted);
    private readonly TextBlock notice = Label("", UiTypography.Caption, UiTheme.Warning);
    private readonly Button connect = ActionButton(UiText.ConnectBroker);
    private readonly DashboardPage dashboard = new();
    private readonly MetricsPage metricsPage = new() { IsVisible = false };
    private readonly EventsPage eventsPage = new() { IsVisible = false };
    private readonly DispatcherTimer timer = new() { Interval = UiLayout.RefreshInterval };
    private readonly List<Button> navigation = [];
    private CaptureSession? session;
    private BrokerSettings lastSettings = new();
    private MetricStore store = new();
    private MetricKey? selectedMetric;
    private bool busy, closingAllowed, closeAttempted;
    private bool chartsPaused;
    private string[] topics = [ChartDefaults.AllSources];
    public double WindowSeconds => ChartDefaults.Window(timeRange.SelectedIndex);

    public MainWindow()
    {
        Title = UiText.WindowTitle;
        Width = UiLayout.WindowWidth;
        Height = UiLayout.WindowHeight;
        MinWidth = UiLayout.MinimumWindowWidth;
        MinHeight = UiLayout.MinimumWindowHeight;
        Background = UiTheme.Background;
        Foreground = UiTheme.Foreground;
        FontFamily = new FontFamily(UiTheme.Font);
        dashboard.RefreshRequested += () => Refresh(forceCharts: true);
        metricsPage.MetricSelected += key =>
        {
            selectedMetric = key;
            dashboard.SelectMetric();
            Navigate(0);
        };
        BuildLayout();
        timer.Tick += (_, _) =>
        {
            store.PublishBatch();
            Refresh();
        };
        timer.Start();
        Closed += (_, _) => timer.Stop();
        AttachConnectionLifecycle();
    }

    private void Navigate(int index)
    {
        dashboard.IsVisible = index == 0;
        metricsPage.IsVisible = index == 1;
        eventsPage.IsVisible = index == 2;
        title.Text = index switch
        {
            1 => UiText.AllMetrics,
            2 => UiText.EventTitle,
            _ => UiText.OverviewTitle
        };
        for (var i = 0; i < navigation.Count; i++)
        {
            navigation[i].Background = i == index ? UiTheme.NavigationSelected : Brushes.Transparent;
            navigation[i].Foreground = i == index ? UiTheme.NavigationForeground : UiTheme.Muted;
        }
        Refresh();
    }

    public void Refresh(bool forceCharts = false)
    {
        var snapshot = store.Snapshot(WindowSeconds);
        UpdateSources(snapshot);
        var topic = source.SelectedItem as string ?? ChartDefaults.AllSources;
        var visible = snapshot.Where(metric => topic == ChartDefaults.AllSources || metric.Topic == topic).ToArray();
        metricsPage.Update(visible);
        dashboard.Update(visible, selectedMetric, WindowSeconds, topic, !chartsPaused || forceCharts);
        eventsPage.Update(store.Events(), topic);
        UpdateConnectionStatus(snapshot.Length);
    }

    private void UpdateSources(MetricView[] snapshot)
    {
        var updated = new[] { ChartDefaults.AllSources }.Concat(snapshot.Select(metric => metric.Topic).Distinct().Order()).ToArray();
        if (updated.SequenceEqual(topics))
            return;
        var previous = source.SelectedItem as string;
        topics = updated;
        source.ItemsSource = topics;
        source.SelectedItem = topics.Contains(previous) ? previous : topics[0];
    }

    private void UpdateConnectionStatus(int metricCount)
    {
        if (session == null)
            return;
        status.Text = session.Status;
        status.Foreground = session.Status == ConnectionStatus.Subscribed ? UiTheme.Connected : UiTheme.Muted;
        counts.Text = string.Format(UiText.CountsFormat, session.Received, session.Dropped, metricCount,
            store.SchemaErrors, store.EvictedSeries, store.EvictedHeaders) + UiText.HistoryFooter;
        if (session.Error.Length > 0)
            notice.Text = session.Error;
    }

    public void Preview(MetricStore sample)
    {
        store = sample;
        notice.Text = UiText.SampleNotice;
        status.Text = UiText.Preview;
        Refresh();
    }
}
