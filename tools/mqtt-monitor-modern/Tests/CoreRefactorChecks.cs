using CakeAdaptMonitor;
static partial class Tests
{
    private static void CoreRefactorChecks()
    {
        Check(typeof(MetricStore).Assembly != typeof(MainWindow).Assembly &&
            !typeof(MetricStore).Assembly.GetReferencedAssemblies().Any(a => a.Name!.StartsWith("Avalonia")),
            "telemetry core has no UI dependency");
        var state = new ChartSelections();
        var key = new TraceKey("source-a", "", "metric");
        state.Apply(("Throughput", "source-a"), [key]);
        var edited = state.Get(("Throughput", "source-a"))!; edited.Clear();
        Check(state.Get(("Throughput", "source-a"))!.Count == 1, "dialog choices cannot mutate active selections before Apply");
        state.Apply(("Throughput", "source-b"), []);state.Reset(("Throughput", "source-b"));
        Check(state.Get(("Throughput", "source-b")) == null && state.Get(("Throughput", "source-a"))!.Count == 1,
            "default reset remains isolated by topic");
        try { state.Apply(("Throughput", "source-a"), Enumerable.Range(0,25).Select(i=>new TraceKey("source-a","",i.ToString())));throw new Exception("limit accepted"); }
        catch(ArgumentOutOfRangeException) { Check(state.Get(("Throughput", "source-a"))!.Count == 1,"core enforces trace limit without replacing previous choices"); }
        Check(ChartDefaults.Window(-1)==300 && ChartDefaults.Window(3)==3600,"time-window defaults and one-hour choice preserved");
    }
}
