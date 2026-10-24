using Avalonia.Media;
namespace CakeAdaptMonitor;

public static class UiTheme
{
    public static readonly IBrush Background = Brush.Parse("#10141D");
    public static readonly IBrush Panel = Brush.Parse("#171E2B");
    public static readonly IBrush Muted = Brush.Parse("#8F9CB3");
    public static readonly IBrush Foreground = Brush.Parse("#DDE5F2");
    public static readonly IBrush Warning = Brush.Parse("#E8B866");
    public static readonly IBrush ActionBackground = Brush.Parse("#263C60");
    public static readonly IBrush ActionForeground = Brush.Parse("#B9D7FF");
    public static readonly IBrush CardBorder = Brush.Parse("#283144");
    public static readonly IBrush Rail = Brush.Parse("#121925");
    public static readonly IBrush Brand = Brush.Parse("#70AEFF");
    public static readonly IBrush StatusBackground = Brush.Parse("#1D2B36");
    public static readonly IBrush NavigationSelected = Brush.Parse("#233650");
    public static readonly IBrush NavigationForeground = Brush.Parse("#86BAFF");
    public static readonly IBrush Connected = Brush.Parse("#52D5B4");
    public static readonly IBrush Link = Brush.Parse("#79ACEE");
    public static readonly IBrush LegendHint = Brush.Parse("#94A3BD");
    public static readonly IBrush Help = Brush.Parse("#9DAAC0");
    public static readonly IBrush MetricSource = Brush.Parse("#91A0B9");
    public static readonly IBrush SelectionCount = Brush.Parse("#A7B7D0");
    public static readonly IBrush Primary = Brush.Parse("#3168B7");
    public static readonly IBrush Error = Brush.Parse("#ED9B8F");
    public static readonly IBrush AxisText = Brush.Parse("#8A96AD");
    public static readonly IBrush GridLine = Brush.Parse("#252D3E");
    public static readonly IBrush Crosshair = Brush.Parse("#667896");
    public static readonly IBrush TooltipBackground = Brush.Parse("#202A3C");
    public static readonly IBrush TooltipBorder = Brush.Parse("#394761");
    public static readonly IBrush TooltipForeground = Brushes.White;
    public static readonly Color[] Palette = [Color.Parse("#5A9BFF"), Color.Parse("#42D6BD"), Color.Parse("#E8B866"), Color.Parse("#AD8CFF"), Color.Parse("#ED7C98"), Color.Parse("#5AC9E8")];
    public const string Font = "Inter, Segoe UI";
    public const string MonospaceFont = "Cascadia Mono, Consolas, monospace";
    public const string ApplicationUri = "avares://CakeAdaptMonitor/";
    public const string DataGridStyleUri = "avares://Avalonia.Controls.DataGrid/Themes/Fluent.xaml";
}
