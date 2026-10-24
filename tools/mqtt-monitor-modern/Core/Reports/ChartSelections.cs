namespace CakeAdaptMonitor;
// One instance belongs to one chart; topic/report choices are independent.
public sealed class ChartSelections
{
    private readonly Dictionary<(string Report, string Topic), HashSet<TraceKey>> selections = new();
    public HashSet<TraceKey>? Get((string Report, string Topic) scope) =>
        selections.TryGetValue(scope, out var keys) ? new(keys) : null;
    public void Apply((string Report, string Topic) scope, IEnumerable<TraceKey> keys)
    {
        var chosen = keys.ToHashSet();
        if (chosen.Count > ChartDefaults.MaximumTraces)
            throw new ArgumentOutOfRangeException(nameof(keys));
        selections[scope] = chosen;
    }
    public void Reset((string Report, string Topic) scope) => selections.Remove(scope);
}
