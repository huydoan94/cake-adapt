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
            EndTime = chart.EndTime,
            EmptyMessage = chart.EmptyMessage,
            EmptyHint = chart.EmptyHint,
            Margin = UiLayout.SnapshotMargin
        };
        new Window
        {
            Title = Report + UiText.SnapshotSuffix,
            Width = UiLayout.SnapshotWidth,
            Height = UiLayout.SnapshotHeight,
            Background = UiTheme.Panel,
            Content = enlarged
        }.Show(owner);
    }

    public void Update(MetricView[] metrics, MetricKey? selected, double seconds, string selectedTopic)
    {
        topic = selectedTopic;
        candidates = ReportCatalog.Candidates(Report, metrics, selected).ToArray();
        var selection = selections.Get((Report, topic));
        var custom = selection != null;
        var chosen = custom ? candidates.Where(metric => selection!.Contains(TraceKey.From(metric))).ToArray()
            : ReportCatalog.Defaults(candidates);
        drawnKeys = chosen.Select(TraceKey.From).ToHashSet();
        chart.EmptyMessage = custom ? UiText.NothingSelected : UiText.Waiting;
        chart.EmptyHint = custom ? UiText.SelectionHelp : UiText.ConnectionHelp;
        chart.Unit = PlotSeries.Unit(Report, chosen);
        chart.Seconds = seconds;
        chart.EndTime = Telemetry.Now;
        chart.Lines = PlotSeries.Create(Report, chosen);
        UpdateLegend(custom);
        chart.InvalidateVisual();
    }

    private void UpdateLegend(bool custom)
    {
        legend.Children.Clear();
        foreach (var line in chart.Lines)
            legend.Children.Add(new TextBlock
            {
                Text = line.Label,
                Foreground = new SolidColorBrush(line.Color),
                FontSize = UiTypography.Small,
                Margin = UiLayout.LegendItemMargin
            });
        if (chart.Lines.Length == 0 || (!custom && candidates.Length > ChartDefaults.AutomaticTraces))
            legend.Children.Add(new TextBlock
            {
                Text = chart.Lines.Length == 0 ? UiText.SettingsHint : UiText.AutomaticHint,
                FontSize = UiTypography.Small,
                Foreground = UiTheme.LegendHint
            });
    }
}
