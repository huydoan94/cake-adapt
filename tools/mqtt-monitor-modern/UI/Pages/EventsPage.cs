using Avalonia.Controls;
using Avalonia.Media;

namespace CakeAdaptMonitor;

internal sealed class EventsPage : TextBox
{
    // A TextBox subclass gets no template unless it keeps the TextBox style key.
    protected override Type StyleKeyOverride => typeof(TextBox);

    public EventsPage()
    {
        AcceptsReturn = true;
        IsReadOnly = true;
        TextWrapping = TextWrapping.NoWrap;
        FontFamily = new FontFamily(UiTheme.MonospaceFont);
        FontSize = UiTypography.Caption;
    }

    // Newest first, so the latest messages stay in view while the text is replaced every tick.
    public void Update(RecentEvent[] events, string topic) => Text = string.Join(UiText.EventSeparator,
        events.Where(item => topic == ChartDefaults.AllSources || item.Topic == topic).Reverse().Select(item => item.Text));
}
