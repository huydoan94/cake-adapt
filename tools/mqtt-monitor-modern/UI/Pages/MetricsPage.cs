using Avalonia.Controls;
using Avalonia.Data;

namespace CakeAdaptMonitor;

internal sealed class MetricsPage : Grid
{
    private readonly MetricRows rows = new();
    private readonly TextBox search = new()
    {
        Watermark = UiText.SearchHint,
        MinWidth = UiLayout.SearchWidth,
        Margin = UiLayout.SectionBottomMargin
    };
    private readonly DataGrid table = new()
    {
        IsReadOnly = true,
        AutoGenerateColumns = false,
        GridLinesVisibility = DataGridGridLinesVisibility.Horizontal,
        SelectionMode = DataGridSelectionMode.Single,
        Background = UiTheme.Panel,
        RowHeight = UiLayout.TableRowHeight
    };
    private MetricView[] metrics = [];
    public event Action<MetricKey>? MetricSelected;

    public MetricsPage()
    {
        RowDefinitions = new RowDefinitions(UiLayout.MetricsRows);
        Children.Add(search);
        table.ItemsSource = rows.Items;
        foreach (var (heading, property, width) in MetricColumns.All)
            table.Columns.Add(new DataGridTextColumn { Header = heading, Binding = new Binding(property), Width = new DataGridLength(width) });
        table.DoubleTapped += (_, _) =>
        {
            if (table.SelectedItem is MetricRow metric)
                MetricSelected?.Invoke(metric.Key);
        };
        Grid.SetRow(table, 1);
        Children.Add(table);
        search.TextChanged += (_, _) => Filter();
    }

    public void Update(MetricView[] snapshot)
    {
        metrics = snapshot;
        Filter();
    }

    private void Filter()
    {
        var text = search.Text ?? "";
        var filtered = metrics.Where(metric => string.Join(' ', metric.Name, metric.Topic, metric.Record, metric.Dimension)
            .Contains(text, StringComparison.OrdinalIgnoreCase)).OrderBy(metric => metric.Name).ToArray();
        rows.Update(filtered);
    }
}
