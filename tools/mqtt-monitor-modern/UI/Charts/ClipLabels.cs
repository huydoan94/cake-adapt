using Avalonia.Media;

namespace CakeAdaptMonitor;

// One peak label: its box's left edge and width in pixels, the value and its trace color.
public sealed record ClipLabel(double Left, double Width, double Value, Color Color);

// Peak labels for readings above a clipped scale. Labels are placed left to right; a label
// that would come closer than spacing pixels (edge to edge) to the previous one merges with
// it, keeping the higher value. The kept label never moves left, so every remaining pair
// stays at least spacing apart.
public static class ClipLabels
{
    public static ClipLabel[] Merge(IEnumerable<ClipLabel> spikes, double spacing)
    {
        var labels = new List<ClipLabel>();
        foreach (var spike in spikes.OrderBy(spike => spike.Left))
        {
            if (labels.Count > 0 && spike.Left < labels[^1].Left + labels[^1].Width + spacing)
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
