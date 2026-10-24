using Avalonia.Controls;
using Avalonia.Media;

namespace CakeAdaptMonitor;

internal sealed class EventsPage : TextBox
{
    public EventsPage()
    {
        AcceptsReturn = true;
        IsReadOnly = true;
        TextWrapping = TextWrapping.NoWrap;
        FontFamily = new FontFamily(UiTheme.MonospaceFont);
        FontSize = UiTypography.Caption;
    }

    public void Update(RecentEvent[] events, string topic) => Text = string.Join(UiText.EventSeparator,
        events.Where(item => topic == ChartDefaults.AllSources || item.Topic == topic).Select(item => item.Text));
}
