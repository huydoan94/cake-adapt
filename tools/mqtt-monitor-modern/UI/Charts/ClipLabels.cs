using Avalonia.Media;

namespace CakeAdaptMonitor;

public sealed record ClipLabel(double X, double Value, Color Color);

// Peak labels for readings above a clipped scale. Spikes are taken left to right; a
// spike closer than gap pixels to the current label joins it, and the label moves to
// the highest of them, so neighbouring labels stay at least gap pixels apart.
public static class ClipLabels
{
    public static ClipLabel[] Merge(IEnumerable<ClipLabel> spikes, double gap)
    {
        var labels = new List<ClipLabel>();
        foreach (var spike in spikes.OrderBy(spike => spike.X))
        {
            if (labels.Count > 0 && spike.X - labels[^1].X < gap)
            {
                if (spike.Value > labels[^1].Value)
                    labels[^1] = spike;
            }
            else
                labels.Add(spike);
        }
        return labels.ToArray();
    }
}
