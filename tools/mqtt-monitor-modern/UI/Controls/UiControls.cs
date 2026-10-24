using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace CakeAdaptMonitor;

// Shared visual construction only; pages own their state and event handling.
internal static class UiControls
{
    public static TextBlock Label(string text, double size = UiTypography.Body, IBrush? brush = null) => new()
    {
        Text = text,
        FontSize = size,
        Foreground = brush ?? UiTheme.Foreground,
        VerticalAlignment = VerticalAlignment.Center
    };

    public static Button ActionButton(string text) => new()
    {
        Content = text,
        Padding = UiLayout.ActionPadding,
        Background = UiTheme.ActionBackground,
        Foreground = UiTheme.ActionForeground,
        BorderThickness = new Thickness(0),
        CornerRadius = new CornerRadius(UiLayout.ActionRadius)
    };

    public static Button LinkButton(string text) => new()
    {
        Content = text,
        FontSize = UiTypography.Small,
        Background = Brushes.Transparent,
        BorderThickness = new Thickness(0),
        Foreground = UiTheme.Link,
        VerticalAlignment = VerticalAlignment.Center
    };

    public static Border Card(Control content) => new()
    {
        Background = UiTheme.Panel,
        BorderBrush = UiTheme.CardBorder,
        BorderThickness = UiLayout.CardBorderThickness,
        CornerRadius = new CornerRadius(UiLayout.CardRadius),
        Child = content
    };
}
