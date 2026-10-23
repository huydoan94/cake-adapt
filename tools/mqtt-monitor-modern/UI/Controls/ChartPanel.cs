using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace CakeAdaptMonitor;

public sealed class ChartPanel : Grid
{
    private readonly ComboBox group;
    private readonly TrendChart chart = new();
    private readonly WrapPanel legend = new() { Margin = UiLayout.LegendMargin };
    private readonly MainWindow owner;
    private readonly ChartSelections selections = new();
    private MetricKey? selectedMetric;
    private HashSet<TraceKey> drawnKeys = [];
    public ChartPanel(string name, MainWindow owner)
    {
        this.owner = owner;
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
        group.SelectionChanged += (_, _) => owner.Refresh(true);
        header.Children.Add(group);
        var settings = new Button
        {
            Content = UiText.Settings,
            FontSize = UiTypography.Small,
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(0),
            Foreground = UiTheme.Link,
            VerticalAlignment = VerticalAlignment.Center
        };
        settings.Click += async (_, _) =>
        {
            owner.Refresh(true);
            var report = group.SelectedItem as string ?? ReportNames.Throughput;
            var scope = (report, owner.SelectedTopic);
            var available = Candidates(owner.ChartCandidates, selectedMetric).ToArray();
            var initial = selections.Get(scope) ?? drawnKeys;
            var result = await new ChartSettingsDialog(report, owner.SelectedTopic, available, initial).ShowDialog<ChartSelection?>(owner);
            if (result == null)
                return;
            if (result.Automatic)
                selections.Reset(scope);
            else
                selections.Apply(scope, result.Keys);
            owner.Refresh(true);
        };
        Grid.SetColumn(settings, 1);
        header.Children.Add(settings);
        var expand = new Button
        {
            Content = UiText.Expand,
            FontSize = UiTypography.Small,
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(0),
            Foreground = UiTheme.Link,
            VerticalAlignment = VerticalAlignment.Center
        };
        expand.Click += (_, _) =>
        {
            var enlarged = new TrendChart { Lines = chart.Lines, Seconds = chart.Seconds, Unit = chart.Unit, EndTime = chart.EndTime, EmptyMessage = chart.EmptyMessage, EmptyHint = chart.EmptyHint, Margin = UiLayout.SnapshotMargin };
            new Window
            {
                Title = (group.SelectedItem as string) + UiText.SnapshotSuffix,
                Width = UiLayout.SnapshotWidth,
                Height = UiLayout.SnapshotHeight,
                Background = UiTheme.Panel,
                Content = enlarged
            }.Show(owner);
        };
        Grid.SetColumn(expand, 2);
        header.Children.Add(expand);
        Children.Add(header);
        Grid.SetRow(legend, 1);
        Children.Add(legend);
        Grid.SetRow(chart, 2);
        chart.Margin = UiLayout.ChartMargin;
        Children.Add(chart);
    }
    public void Select(string name)
    {
        if (name == ReportNames.SelectedMetric)
            selections.Reset((name, owner.SelectedTopic));
        group.SelectedItem = name;
    }
    private IEnumerable<MetricView> Candidates(MetricView[] all, MetricKey? selected) =>
        ReportCatalog.Candidates(group.SelectedItem as string ?? ReportNames.Throughput, all, selected);
    public void Update(MetricView[] all, MetricKey? selected, double seconds)
    {
        selectedMetric = selected;
        var name = group.SelectedItem as string ?? ReportNames.Throughput;
        var distinct = Candidates(all, selected).ToArray();
        var selection = selections.Get((name, owner.SelectedTopic));
        var custom = selection != null;
        var chosen = custom ? distinct.Where(m => selection!.Contains(TraceKey.From(m))).ToArray() : distinct.Take(ChartDefaults.AutomaticTraces).ToArray();
        drawnKeys = chosen.Select(TraceKey.From).ToHashSet();
        chart.EmptyMessage = custom ? UiText.NothingSelected : UiText.Waiting;
        chart.EmptyHint = custom ? UiText.SelectionHelp : UiText.ConnectionHelp;
        var multipleSources = chosen.Select(m => m.Topic).Distinct().Count() > 1;
        var divisor = name == ReportNames.Throughput ? ReportUnits.RateDivisor : name is ReportNames.Latency or ReportNames.TcpQueues ? ReportUnits.DelayDivisor : name == ReportNames.Memory ? ReportUnits.MemoryDivisor : 1;
        chart.Unit = name switch
        {
            ReportNames.Throughput => ReportUnits.Rate,
            ReportNames.Latency or ReportNames.TcpQueues => ReportUnits.Delay,
            ReportNames.Memory => ReportUnits.Memory,
            ReportNames.Cpu => ReportUnits.Cpu,
            _ => chosen.FirstOrDefault()?.Unit ?? ReportUnits.Count
        };
        chart.Seconds = seconds;
        chart.EndTime = Telemetry.Now;
        chart.Lines = chosen.Select((m, i) => new PlotLine(m.Name.Replace(UiText.TrafficFieldSuffix, UiText.TrafficLabel)
            .Replace(UiText.RateFieldSuffix, UiText.RateLabel).Replace(UiText.DelayFieldSuffix, UiText.DelayLabel).Replace(UiText.MemoryFieldSuffix, "") +
            (multipleSources ? UiText.SourceSeparator + MainWindow.ShortTopic(m.Topic) : "") + (m.Dimension.Length > 0 ? UiText.SourceSeparator + m.Dimension : ""),
            m.Samples.Select(s => s with { Value = s.Value / divisor }).ToArray(), TrendChart.Palette[i % TrendChart.Palette.Length])).ToArray();
        legend.Children.Clear();
        foreach (var line in chart.Lines)
            legend.Children.Add(new TextBlock
            {
                Text = line.Label,
                Foreground = new SolidColorBrush(line.Color),
                FontSize = UiTypography.Small,
                Margin = UiLayout.LegendItemMargin
            });
        if (chosen.Length == 0 || (!custom && distinct.Length > ChartDefaults.AutomaticTraces))
            legend.Children.Add(new TextBlock
            {
                Text = chosen.Length == 0 ? UiText.SettingsHint : UiText.AutomaticHint,
                FontSize = UiTypography.Small,
                Foreground = UiTheme.LegendHint
            });
        chart.InvalidateVisual();
    }
}
