using Avalonia;
using Avalonia.Controls;
using static CakeAdaptMonitor.UiControls;

namespace CakeAdaptMonitor;

internal sealed class DashboardPage : Grid
{
    private readonly RateCard[] cards;
    private readonly ChartPanel[] panels;
    public event Action? RefreshRequested;

    public DashboardPage()
    {
        RowDefinitions = new RowDefinitions(UiLayout.DashboardRows);
        ColumnDefinitions = new ColumnDefinitions(UiLayout.DashboardColumns);
        cards = MetricNames.Rates.Select((field, i) => new RateCard(UiText.RateLabels[i], field)).ToArray();
        var rates = new Grid { ColumnDefinitions = new ColumnDefinitions(UiLayout.RateColumns), Margin = UiLayout.RatesMargin };
        for (var i = 0; i < cards.Length; i++)
        {
            cards[i].Margin = new Thickness(i == 0 ? 0 : UiLayout.RateGap, 0, 0, 0);
            Grid.SetColumn(cards[i], i);
            rates.Children.Add(cards[i]);
        }
        Grid.SetColumnSpan(rates, UiLayout.ChartColumns);
        Children.Add(rates);

        panels = ReportNames.Initial.Select(name => new ChartPanel(name)).ToArray();
        for (var i = 0; i < panels.Length; i++)
        {
            panels[i].RefreshRequested += () => RefreshRequested?.Invoke();
            var card = Card(panels[i]);
            var left = i % UiLayout.ChartColumns == 0;
            card.Margin = new Thickness(left ? 0 : UiLayout.ChartColumnGap, 0,
                left ? UiLayout.ChartColumnGap : 0, i < UiLayout.ChartColumns ? UiLayout.ChartRowGap : 0);
            Grid.SetColumn(card, i % UiLayout.ChartColumns);
            Grid.SetRow(card, i / UiLayout.ChartColumns + 1);
            Children.Add(card);
        }
    }

    public void SelectMetric() => panels[^1].Select(ReportNames.SelectedMetric);

    public void Update(MetricView[] metrics, MetricKey? selected, double seconds, string topic, bool updateCharts)
    {
        foreach (var card in cards)
            card.Update(metrics);
        if (updateCharts)
            foreach (var panel in panels)
                panel.Update(metrics, selected, seconds, topic);
    }
}
