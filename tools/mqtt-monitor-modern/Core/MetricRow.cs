using System.ComponentModel;

namespace CakeAdaptMonitor;

// Preserve row identity while immutable measurement snapshots are replaced.
public sealed class MetricRow : INotifyPropertyChanged
{
    private MetricView value;
    public MetricRow(MetricView value)
    {
        this.value = value;
    }
    public MetricKey Key => value.Key;
    public string Name => value.Name;
    public string Topic => value.Topic;
    public string Record => value.Record;
    public string Dimension => value.Dimension;
    public string Latest => value.Latest;
    public string Unit => value.Unit;
    public string Min => value.Min;
    public string Mean => value.Mean;
    public string Max => value.Max;
    public int Count => value.Count;
    public string Age => value.Age;
    public event PropertyChangedEventHandler? PropertyChanged;
    public void Update(MetricView snapshot)
    {
        value = snapshot;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(null));
    }
}
