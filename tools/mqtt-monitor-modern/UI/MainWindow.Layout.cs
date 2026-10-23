using System.Collections.ObjectModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Data;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

namespace CakeAdaptMonitor;

public sealed partial class MainWindow
{
    private static TextBlock Label(string text, double size = UiTypography.Body, IBrush? brush = null) =>
        new()
        {
            Text = text,
            FontSize = size,
            Foreground = brush ?? UiTheme.Foreground,
            VerticalAlignment = VerticalAlignment.Center
        };
    private static Button ActionButton(string text) => new()
    {
        Content = text,
        Padding = UiLayout.ActionPadding,
        Background = UiTheme.ActionBackground,
        Foreground = UiTheme.ActionForeground,
        BorderThickness = new Thickness(0),
        CornerRadius = new CornerRadius(UiLayout.ActionRadius)
    };
    private static Border Card(Control content) => new()
    {
        Background = UiTheme.Panel,
        BorderBrush = UiTheme.CardBorder,
        BorderThickness = UiLayout.CardBorderThickness,
        CornerRadius = new CornerRadius(UiLayout.CardRadius),
        Child = content
    };

    private void BuildLayout()
    {
        var shell = new Grid { ColumnDefinitions = new ColumnDefinitions(UiLayout.ShellColumns) };
        var rail = new Grid { Background = UiTheme.Rail, RowDefinitions = new RowDefinitions(UiLayout.RailRows), Margin = new Thickness(0) };
        var brand = new StackPanel { Margin = UiLayout.BrandMargin, Spacing = UiLayout.SmallSpacing };
        brand.Children.Add(Label(UiText.Brand, UiTypography.Brand, UiTheme.Brand));
        brand.Children.Add(Label(UiText.BrandCaption, UiTypography.Tiny, UiTheme.Muted));
        rail.Children.Add(brand);
        var nav = new StackPanel { Spacing = UiLayout.MediumSpacing, Margin = UiLayout.NavigationMargin };
        var labels = UiText.Navigation;
        for (var i = 0; i < labels.Length; i++)
        {
            var index = i;
            var button = new Button
            {
                Content = labels[i],
                HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Left,
                Padding = UiLayout.NavigationPadding,
                BorderThickness = new Thickness(0),
                CornerRadius = new CornerRadius(UiLayout.ActionRadius)
            };
            button.Click += (_, _) => Navigate(index);
            navigation.Add(button);
            nav.Children.Add(button);
        }
        Grid.SetRow(nav, 1);
        rail.Children.Add(nav);
        var railFoot = new StackPanel { Margin = UiLayout.RailFooterMargin, Spacing = UiLayout.ItemSpacing };
        railFoot.Children.Add(Label(UiText.ReadOnly, UiTypography.Tiny, UiTheme.Muted));
        railFoot.Children.Add(Label(UiText.MemoryOnly, UiTypography.Small, UiTheme.Muted));
        Grid.SetRow(railFoot, 2);
        rail.Children.Add(railFoot);
        shell.Children.Add(rail);

        var main = new Grid { RowDefinitions = new RowDefinitions(UiLayout.MainRows), Margin = UiLayout.MainMargin };
        Grid.SetColumn(main, 1);
        shell.Children.Add(main);
        var header = new Grid { ColumnDefinitions = new ColumnDefinitions(UiLayout.HeaderColumns), Margin = UiLayout.HeaderMargin };
        var heading = new StackPanel { Spacing = UiLayout.ItemSpacing, VerticalAlignment = VerticalAlignment.Center };
        heading.Children.Add(title);
        heading.Children.Add(subtitle);
        header.Children.Add(heading);
        var connectionState = new Border
        {
            Background = UiTheme.StatusBackground,
            CornerRadius = new CornerRadius(UiLayout.StatusRadius),
            Padding = UiLayout.StatusPadding,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = UiLayout.StatusMargin
        };
        connectionState.Child = status;
        Grid.SetColumn(connectionState, 1);
        header.Children.Add(connectionState);
        Grid.SetColumn(connect, 2);
        connect.VerticalAlignment = VerticalAlignment.Center;
        header.Children.Add(connect);
        main.Children.Add(header);
        var toolbar = new Grid { ColumnDefinitions = new ColumnDefinitions(UiLayout.ToolbarColumns), Margin = UiLayout.ToolbarMargin };
        toolbar.Children.Add(source);
        Grid.SetColumn(timeRange, 1);
        timeRange.Margin = UiLayout.TimeRangeMargin;
        toolbar.Children.Add(timeRange);
        var pause = new ToggleButton { Content = UiText.Pause, Padding = UiLayout.NavigationMargin };
        pause.IsCheckedChanged += (_, _) =>
        {
            chartsPaused = pause.IsChecked == true;
            if (!chartsPaused)
                Refresh();
            pause.Content = pause.IsChecked == true ? UiText.Resume : UiText.Pause;
        };
        Grid.SetColumn(pause, 3);
        toolbar.Children.Add(pause);
        Grid.SetRow(toolbar, 1);
        main.Children.Add(toolbar);
        Grid.SetRow(notice, 2);
        notice.TextWrapping = TextWrapping.Wrap;
        main.Children.Add(notice);
        var pages = new Grid { Margin = UiLayout.PagesMargin };
        Grid.SetRow(pages, 3);
        main.Children.Add(pages);
        pages.Children.Add(dashboard);
        pages.Children.Add(metricsPage);
        pages.Children.Add(eventsPage);
        var kpis = new Grid { ColumnDefinitions = new ColumnDefinitions(UiLayout.RateColumns), Margin = UiLayout.RatesMargin };
        var kpiLabels = UiText.RateLabels;
        for (var i = 0; i < kpiValues.Length; i++)
        {
            var stack = new StackPanel { Spacing = UiLayout.MediumSpacing };
            stack.Children.Add(Label(kpiLabels[i], UiTypography.Tiny, UiTheme.Muted));
            stack.Children.Add(kpiValues[i]);
            stack.Children.Add(kpiAges[i]);
            var card = Card(stack);
            card.Padding = UiLayout.RatePadding;
            card.MinHeight = UiLayout.MinimumRateHeight;
            card.Margin = new Thickness(i == 0 ? 0 : UiLayout.RateGap, 0, 0, 0);
            Grid.SetColumn(card, i);
            kpis.Children.Add(card);
        }
        Grid.SetColumnSpan(kpis, UiLayout.ChartColumns);
        dashboard.Children.Add(kpis);
        for (var i = 0; i < panels.Length; i++)
        {
            var card = Card(panels[i]);
            card.Margin = new Thickness(i % UiLayout.ChartColumns == 0 ? 0 : UiLayout.ChartColumnGap, 0, i % UiLayout.ChartColumns == 0 ? UiLayout.ChartColumnGap : 0, i < UiLayout.ChartColumns ? UiLayout.ChartRowGap : 0);
            Grid.SetColumn(card, i % UiLayout.ChartColumns);
            Grid.SetRow(card, i / UiLayout.ChartColumns + 1);
            dashboard.Children.Add(card);
        }
        search.Margin = UiLayout.SectionBottomMargin;
        metricsPage.Children.Add(search);
        metricTable.ItemsSource = metricRows.Items;
        foreach (var (headingText, property, width) in MetricColumns.All)
            metricTable.Columns.Add(new DataGridTextColumn { Header = headingText, Binding = new Binding(property), Width = new DataGridLength(width) });
        metricTable.DoubleTapped += (_, _) =>
        {
            if (metricTable.SelectedItem is MetricRow metric)
            {
                selectedMetric = metric.Key;
                panels[3].Select(ReportNames.SelectedMetric);
                Navigate(0);
                Refresh();
            }
        };
        Grid.SetRow(metricTable, 1);
        metricsPage.Children.Add(metricTable);
        Grid.SetRow(counts, 4);
        main.Children.Add(counts);
        source.SelectionChanged += (_, _) => Refresh(true);
        timeRange.SelectionChanged += (_, _) => Refresh();
        search.TextChanged += (_, _) => Refresh();
        Content = shell;
        Navigate(0);
    }

}
