using System.Collections.ObjectModel;
namespace CakeAdaptMonitor;

public sealed class MetricRows
{
    public ObservableCollection<MetricRow> Items { get; } = [];
    public void Update(MetricView[] filtered)
    {
        var wanted = filtered.Select(m => m.Key).ToHashSet();
        for (var i = Items.Count - 1; i >= 0; i--)
        if (!wanted.Contains(Items[i].Key))
            Items.RemoveAt(i);
        var retained = Items.ToDictionary(row => row.Key);
        for (var i = 0; i < filtered.Length; i++)
        {
            var snapshot = filtered[i];
            if (i < Items.Count && Items[i].Key == snapshot.Key)
                Items[i].Update(snapshot);
            else if (retained.TryGetValue(snapshot.Key, out var row))
            {
                Items.Move(Items.IndexOf(row), i);
                row.Update(snapshot);
            }
            else
                Items.Insert(i, new MetricRow(snapshot));
        }
    }
}
