using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Data;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

namespace CakeAdaptMonitor;

public sealed partial class MainWindow : Window
{
    private readonly ComboBox source = new() { MinWidth = UiLayout.SourceWidth, ItemsSource = new[] { ChartDefaults.AllSources }, SelectedIndex = 0 };
    private readonly ComboBox timeRange = new() { MinWidth = UiLayout.TimeRangeWidth, ItemsSource = ChartDefaults.WindowLabels, SelectedIndex = ChartDefaults.DefaultWindowIndex };
    private readonly TextBox search = new() { Watermark = UiText.SearchHint, MinWidth = UiLayout.SearchWidth };
    private readonly TextBlock status = Label(ConnectionStatus.Disconnected, UiTypography.Body);
    private readonly TextBlock counts = Label(UiText.InitialCounts, UiTypography.Caption, UiTheme.Muted);
    private readonly TextBlock title = Label(UiText.OverviewTitle, UiTypography.PageHeading);
    private readonly TextBlock subtitle = Label(UiText.InitialSubtitle, UiTypography.Caption, UiTheme.Muted);
    private readonly TextBlock notice = Label("", UiTypography.Caption, UiTheme.Warning);
    private readonly TextBlock[] kpiValues = Enumerable.Range(0, MetricNames.Rates.Length).Select(_ => Label(UiText.EmptyValue, UiTypography.Rate)).ToArray();
    private readonly TextBlock[] kpiAges = Enumerable.Range(0, MetricNames.Rates.Length).Select(_ => Label(UiText.NoSamples, UiTypography.Small, UiTheme.Muted)).ToArray();
    private readonly Button connect = ActionButton(UiText.ConnectBroker);
    private readonly Grid dashboard = new() { RowDefinitions = new RowDefinitions(UiLayout.DashboardRows), ColumnDefinitions = new ColumnDefinitions(UiLayout.DashboardColumns) };
    private readonly Grid metricsPage = new() { RowDefinitions = new RowDefinitions(UiLayout.MetricsRows), IsVisible = false };
    private readonly TextBox eventsPage = new()
    {
        AcceptsReturn = true,
        IsReadOnly = true,
        TextWrapping = TextWrapping.NoWrap,
        FontFamily = new FontFamily(UiTheme.MonospaceFont),
        FontSize = UiTypography.Caption,
        IsVisible = false
    };
    private readonly MetricRows metricRows = new();
    private readonly DataGrid metricTable = new()
    {
        IsReadOnly = true,
        AutoGenerateColumns = false,
        GridLinesVisibility = DataGridGridLinesVisibility.Horizontal,
        SelectionMode = DataGridSelectionMode.Single,
        Background = UiTheme.Panel,
        RowHeight = UiLayout.TableRowHeight
    };
    private readonly ChartPanel[] panels;
    private readonly DispatcherTimer timer = new() { Interval = UiLayout.RefreshInterval };
    private readonly List<Button> navigation = [];
    private CaptureSession? session;
    private BrokerSettings lastSettings = new();
    private MetricStore store = new();
    private MetricKey? selectedMetric;
    private bool busy, closingAllowed, closeAttempted;
    private bool chartsPaused;
    private string[] topics = [ChartDefaults.AllSources];
    private MetricView[] visibleMetrics = [];
    internal MetricView[] ChartCandidates { get; private set; } = [];
    internal string SelectedTopic => source.SelectedItem as string ?? ChartDefaults.AllSources;
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
        panels = ReportNames.Initial.Select(name => new ChartPanel(name, this)).ToArray();
        BuildLayout();
        timer.Tick += (_, _) => { store.PublishRatePeaks(); Refresh(); };
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
        var updatedTopics = new[] { ChartDefaults.AllSources }.Concat(snapshot.Select(m => m.Topic).Distinct().Order()).ToArray();
        if (!updatedTopics.SequenceEqual(topics))
        {
            var previous = source.SelectedItem as string;
            topics = updatedTopics;
            source.ItemsSource = topics;
            source.SelectedItem = topics.Contains(previous) ? previous : topics[0];
        }
        var topic = source.SelectedItem as string;
        visibleMetrics = snapshot.Where(m => topic is null or ChartDefaults.AllSources || m.Topic == topic).ToArray();
        ChartCandidates = visibleMetrics;
        var filter = search.Text ?? "";
        var filtered = visibleMetrics.Where(m => string.Join(' ', m.Name, m.Topic, m.Record, m.Dimension)
            .Contains(filter, StringComparison.OrdinalIgnoreCase)).OrderBy(m => m.Name).ToArray();
        metricRows.Update(filtered);
        var names = MetricNames.Rates;
        for (var i = 0; i < names.Length; i++)
        {
            var metric = visibleMetrics.Where(m => m.Name == names[i]).MaxBy(m => m.ReceivedTime);
            var peak = metric?.RatePeak;
            var value = peak?.Value ?? (metric == null ? null : Telemetry.Number(metric.Latest));
            kpiValues[i].Text = value.HasValue ? Telemetry.Format(value.Value / ReportUnits.RateDivisor) + UiText.RateSuffix : UiText.EmptyValue;
            kpiAges[i].Text = metric == null ? UiText.NoSamples : (peak == null ? metric.Age : Telemetry.Age(peak.ReceivedTime)) + UiText.AgeSourceSeparator + ShortTopic(metric.Topic);
        }
        if (!chartsPaused || forceCharts)
            foreach (var panel in panels)
                panel.Update(visibleMetrics, selectedMetric, WindowSeconds);
        var rawEvents = store.Events();
        eventsPage.Text = string.Join(UiText.EventSeparator, rawEvents.Where(e => topic is null or ChartDefaults.AllSources || e.Topic == topic).Select(e => e.Text));
        if (session != null)
        {
            status.Text = session.Status;
            status.Foreground = session.Status == ConnectionStatus.Subscribed ? UiTheme.Connected : UiTheme.Muted;
            counts.Text = string.Format(UiText.CountsFormat, session.Received, session.Dropped, snapshot.Length, store.SchemaErrors, store.EvictedSeries, store.EvictedHeaders);
            counts.Text += UiText.HistoryFooter;
            if (session.Error.Length > 0)
                notice.Text = session.Error;
        }
    }

    public static string ShortTopic(string topic) => topic.Length <= UiLayout.TopicLength ? topic : UiText.Ellipsis + topic[^UiLayout.TopicTailLength..];
    public void Preview(MetricStore sample)
    {
        store = sample;
        notice.Text = UiText.SampleNotice;
        status.Text = UiText.Preview;
        Refresh();
    }
}
