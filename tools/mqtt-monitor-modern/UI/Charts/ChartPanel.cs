using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using static CakeAdaptMonitor.UiControls;

namespace CakeAdaptMonitor;

public sealed class ChartPanel : Grid
{
    private readonly ComboBox group;
    private readonly TrendChart chart = new();
    private readonly WrapPanel legend = new() { Margin = UiLayout.LegendMargin };
    // Trace-choice hint, under the chart.
    private readonly TextBlock hint = new() { Margin = UiLayout.ChartHintMargin, FontSize = UiTypography.Small, Foreground = UiTheme.LegendHint, IsVisible = false };
    private readonly ChartSelections selections = new();
    private MetricView[] candidates = [];
    private string topic = ChartDefaults.AllSources;
    private HashSet<TraceKey> drawnKeys = [];
    private string Report => group.SelectedItem as string ?? ReportNames.Throughput;
    public event Action? RefreshRequested;

    public ChartPanel(string name)
    {
        RowDefinitions = new RowDefinitions(UiLayout.ChartRows);
        var header = new Grid { ColumnDefinitions = new ColumnDefinitions(UiLayout.HeaderColumns), Margin = UiLayout.ChartHeaderMargin };
        group = new ComboBox
        {
            ItemsSource = ReportNames.All,
            SelectedItem = name,
            BorderThickness = new Thickness(0),
            Background = Brushes.Transparent,
            MinWidth = UiLayout.ReportWidth
        };
        group.SelectionChanged += (_, _) => RefreshRequested?.Invoke();
        header.Children.Add(group);
        var settings = LinkButton(UiText.Settings);
        settings.Click += async (_, _) => await EditSelectionAsync();
        Grid.SetColumn(settings, 1);
        header.Children.Add(settings);
        var expand = LinkButton(UiText.Expand);
        expand.Click += (_, _) => ShowSnapshot();
        Grid.SetColumn(expand, 2);
        header.Children.Add(expand);
        Children.Add(header);
        Grid.SetRow(legend, 1);
        Children.Add(legend);
        Grid.SetRow(chart, 2);
        chart.Margin = UiLayout.ChartMargin;
        Children.Add(chart);
        Grid.SetRow(hint, 3);
        Children.Add(hint);
    }

    public void Select(string name)
    {
        if (name == ReportNames.SelectedMetric)
            selections.Reset((name, topic));
        group.SelectedItem = name;
    }

    private async Task EditSelectionAsync()
    {
        if (TopLevel.GetTopLevel(this) is not Window owner)
            return;
        RefreshRequested?.Invoke();
        var scope = (Report, topic);
        var initial = selections.Get(scope) ?? drawnKeys;
        var result = await new ChartSettingsDialog(Report, topic, candidates, initial).ShowDialog<ChartSelection?>(owner);
        if (result == null)
            return;
        if (result.Automatic)
            selections.Reset(scope);
        else
            selections.Apply(scope, result.Keys);
        RefreshRequested?.Invoke();
    }

    private void ShowSnapshot()
    {
        if (TopLevel.GetTopLevel(this) is not Window owner)
            return;
        var enlarged = new TrendChart
        {
            Lines = chart.Lines,
            Seconds = chart.Seconds,
            Unit = chart.Unit,
            Ceiling = chart.Ceiling,
            Marks = chart.Marks,
            EndTime = chart.EndTime,
            EmptyMessage = chart.EmptyMessage,
            EmptyHint = chart.EmptyHint,
            Margin = UiLayout.SnapshotMargin
        };
        var snapshotLegend = new WrapPanel { Margin = UiLayout.SnapshotLegendMargin };
        AddLegendItems(snapshotLegend, chart.Lines);
        var content = new Grid { RowDefinitions = new RowDefinitions(UiLayout.SnapshotRows) };
        content.Children.Add(snapshotLegend);
        Grid.SetRow(enlarged, 1);
        content.Children.Add(enlarged);
        var window = new Window
        {
            Title = Report + UiText.SnapshotSuffix,
            Width = UiLayout.SnapshotWidth,
            Height = UiLayout.SnapshotHeight,
            Background = UiTheme.Panel,
            Content = content
        };
        DarkTitleBar.Apply(window);
        window.Show(owner);
    }

    // end: the history range end (charts then show [end - seconds, end] with range ticks); null for live.
    public void Update(MetricView[] metrics, MetricKey? selected, double seconds, string selectedTopic, BloatView[]? bloat = null, double? end = null,
        Func<MetricKey, Sample[]>? samples = null)
    {
        topic = selectedTopic;
        candidates = ReportCatalog.Candidates(Report, metrics, selected).ToArray();
        var selection = selections.Get((Report, topic));
        var custom = selection != null;
        var chosen = custom ? candidates.Where(metric => selection!.Contains(TraceKey.From(metric))).ToArray()
            : ReportCatalog.Defaults(candidates, end.HasValue ? metric => metric.Stats?.Count ?? 0 : null);
        // History snapshots carry no points; read them only for the traces drawn.
        if (samples != null)
            chosen = chosen.Select(metric => metric with { Samples = samples(metric.Key) }).ToArray();
        drawnKeys = chosen.Select(TraceKey.From).ToHashSet();
        chart.EmptyMessage = custom ? UiText.NothingSelected : UiText.Waiting;
        chart.EmptyHint = custom ? UiText.SelectionHelp : UiText.ConnectionHelp;
        chart.Unit = PlotSeries.Unit(Report, chosen);
        chart.Seconds = seconds;
        chart.EndTime = end ?? Telemetry.Now;
        chart.RangeTicks = end.HasValue;
        chart.Lines = PlotSeries.Create(Report, chosen, candidates.Select(metric => metric.Topic).Distinct().Count() > 1);
        chart.Ceiling = PlotSeries.Ceiling(Report, chart.Lines);
        chart.Marks = Report == ReportNames.Latency ? BloatMarks(bloat ?? []) : [];
        UpdateLegend(custom, Report == ReportNames.Latency ? bloat ?? [] : []);
        chart.InvalidateVisual();
    }

    // One tick row per source at the start of each 2-second bucket holding a bufferbloat sample.
    private static PlotMarks[] BloatMarks(BloatView[] bloat) => bloat.GroupBy(view => view.Topic)
        .Select((topic, i) => new PlotMarks(DisplayText.ShortTopic(topic.Key),
            topic.SelectMany(view => view.Buckets).Where(bucket => bucket.Count > 0).Select(bucket => bucket.Time).Distinct().Order().ToArray(),
            UiTheme.BloatPalette[i % UiTheme.BloatPalette.Length]))
        .ToArray();

    private void UpdateLegend(bool custom, BloatView[] bloat)
    {
        legend.Children.Clear();
        AddLegendItems(legend, chart.Lines);
        AddBloatItems(legend, bloat);
        hint.IsVisible = chart.Lines.Length == 0 || (!custom && candidates.Length > ChartDefaults.AutomaticTraces);
        hint.Text = chart.Lines.Length == 0 ? UiText.SettingsHint : UiText.AutomaticHint;
    }

    // "Bufferbloat samples:  router-a  DL 0 · UL 0" in each source's tick color.
    private static void AddBloatItems(WrapPanel target, BloatView[] bloat)
    {
        if (bloat.Length == 0)
            return;
        target.Children.Add(new TextBlock { Text = UiText.BloatHeading, FontSize = UiTypography.Small, Foreground = UiTheme.LegendHint, Margin = UiLayout.LegendItemMargin });
        var i = 0;
        foreach (var topic in bloat.GroupBy(view => view.Topic))
            target.Children.Add(new TextBlock
            {
                Text = string.Format(UiText.BloatFormat, DisplayText.ShortTopic(topic.Key),
                    topic.Where(view => view.Direction == ProtocolNames.Download).Sum(view => view.Count),
                    topic.Where(view => view.Direction == ProtocolNames.Upload).Sum(view => view.Count)),
                Foreground = new SolidColorBrush(UiTheme.BloatPalette[i++ % UiTheme.BloatPalette.Length]),
                FontSize = UiTypography.Small,
                Margin = UiLayout.LegendItemMargin
            });
    }

    private static void AddLegendItems(WrapPanel target, PlotLine[] lines)
    {
        foreach (var line in lines)
            target.Children.Add(new TextBlock
            {
                Text = line.Label,
                Foreground = new SolidColorBrush(line.Color),
                FontSize = UiTypography.Small,
                Margin = UiLayout.LegendItemMargin
            });
    }
}
