namespace CakeAdaptMonitor;

public sealed record RangeStats(double Min, double Mean, double Max, double MaxTime, long Count, double Last, double LastTime);

// One series of readings in fixed time buckets, stored as parallel arrays rather
// than objects: about 24 bytes per occupied bucket. After Seal, coarser levels
// (HistoryModeDefaults.LevelSteps) are built so wide ranges read few buckets.
public sealed class BucketSeries
{
    private sealed class Level(int step)
    {
        public readonly int Step = step;
        public int Count;
        public int[] Keys = new int[HistoryModeDefaults.InitialCapacity];
        public float[] Min = new float[HistoryModeDefaults.InitialCapacity];
        public float[] Max = new float[HistoryModeDefaults.InitialCapacity];
        public double[] Sum = new double[HistoryModeDefaults.InitialCapacity];
        public int[] Readings = new int[HistoryModeDefaults.InitialCapacity];
        public float[] Last = new float[HistoryModeDefaults.InitialCapacity];

        // Insert a new bucket at index (keys stay sorted) and return it.
        public int Insert(int index, int key)
        {
            if (Count == Keys.Length)
            {
                var size = Keys.Length * 2;
                Array.Resize(ref Keys, size); Array.Resize(ref Min, size); Array.Resize(ref Max, size);
                Array.Resize(ref Sum, size); Array.Resize(ref Readings, size); Array.Resize(ref Last, size);
            }
            if (index < Count)
            {
                var tail = Count - index;
                Array.Copy(Keys, index, Keys, index + 1, tail); Array.Copy(Min, index, Min, index + 1, tail);
                Array.Copy(Max, index, Max, index + 1, tail); Array.Copy(Sum, index, Sum, index + 1, tail);
                Array.Copy(Readings, index, Readings, index + 1, tail); Array.Copy(Last, index, Last, index + 1, tail);
            }
            Keys[index] = key; Min[index] = float.PositiveInfinity; Max[index] = float.NegativeInfinity;
            Sum[index] = 0; Readings[index] = 0; Last[index] = 0;
            Count++;
            return index;
        }

        // First index whose key is at least key.
        public int LowerBound(long key)
        {
            int low = 0, high = Count;
            while (low < high)
            {
                var middle = (low + high) / 2;
                if (Keys[middle] < key)
                    low = middle + 1;
                else
                    high = middle;
            }
            return low;
        }

        public int Find(int key)
        {
            var index = Array.BinarySearch(Keys, 0, Count, key);
            return index >= 0 ? index : Insert(~index, key);
        }

        // First bucket whose end is after start.
        public int FirstEndingAfter(double start, double origin)
        {
            int low = 0, high = Count;
            while (low < high)
            {
                var middle = (low + high) / 2;
                if (origin + ((double)Keys[middle] + 1) * Step <= start)
                    low = middle + 1;
                else
                    high = middle;
            }
            return low;
        }
    }

    private readonly Level baseLevel;
    private readonly List<Level> levels = [];
    private double origin = double.NaN;
    private double lastReadingTime = double.NegativeInfinity;
    private bool isSealed;
    public double First { get; private set; } = double.PositiveInfinity;
    public double Last { get; private set; } = double.NegativeInfinity;

    public BucketSeries(int baseStepSeconds)
    {
        baseLevel = new Level(baseStepSeconds);
        levels.Add(baseLevel);
    }

    public void Add(double time, double value)
    {
        if (isSealed)
            throw new InvalidOperationException("History series is sealed.");
        if (!double.IsFinite(time) || !double.IsFinite(value))
            return;
        if (double.IsNaN(origin))
            origin = Math.Floor(time / baseLevel.Step) * baseLevel.Step - HistoryModeDefaults.KeyMarginSeconds;
        var key = (int)Math.Floor((time - origin) / baseLevel.Step);
        // Readings arrive almost in order: check the last bucket before searching.
        var count = baseLevel.Count;
        var index = count > 0 && baseLevel.Keys[count - 1] == key ? count - 1
            : count == 0 || baseLevel.Keys[count - 1] < key ? baseLevel.Insert(count, key) : baseLevel.Find(key);
        var number = (float)value;
        // Sums stay double: a 10-minute sum of microsecond delays exceeds float precision.
        baseLevel.Min[index] = Math.Min(baseLevel.Min[index], number);
        baseLevel.Max[index] = Math.Max(baseLevel.Max[index], number);
        baseLevel.Sum[index] += number;
        baseLevel.Readings[index]++;
        // The first reading of a bucket, or the newest overall, is the bucket's latest value.
        if (baseLevel.Readings[index] == 1 || time >= lastReadingTime || index == baseLevel.Count - 1)
            baseLevel.Last[index] = number;
        lastReadingTime = Math.Max(lastReadingTime, time);
        First = Math.Min(First, time);
        Last = Math.Max(Last, time);
    }

    public void Seal()
    {
        if (isSealed)
            return;
        isSealed = true;
        foreach (var step in HistoryModeDefaults.LevelSteps.Where(step => step > baseLevel.Step))
        {
            var source = levels[^1];
            var level = new Level(step);
            var ratio = step / source.Step;
            for (var i = 0; i < source.Count; i++)
            {
                var key = (int)Math.Floor((double)source.Keys[i] / ratio);
                var j = level.Count > 0 && level.Keys[level.Count - 1] == key ? level.Count - 1 : level.Insert(level.Count, key);
                level.Min[j] = Math.Min(level.Min[j], source.Min[i]);
                level.Max[j] = Math.Max(level.Max[j], source.Max[i]);
                level.Sum[j] += source.Sum[i];
                level.Readings[j] += source.Readings[i];
                level.Last[j] = source.Last[i];
            }
            levels.Add(level);
        }
    }

    // Coarsest level that still gives at least one bucket per column; its buckets
    // are merged into column-wide groups, and each group becomes its minimum and
    // maximum reading, so a read returns at most about two points per column and
    // drawing keeps spikes.
    public Sample[] Read(double start, double end, int columns)
    {
        if (baseLevel.Count == 0 || end < start)
            return [];
        var wanted = (end - start) / Math.Max(1, columns);
        var level = levels.LastOrDefault(level => level.Step <= wanted) ?? baseLevel;
        var width = Math.Max(wanted, level.Step);
        var points = new List<Sample>();
        long group = long.MinValue;
        float min = 0, max = 0;
        double groupStart = 0;
        void Flush()
        {
            if (group == long.MinValue)
                return;
            var minTime = Math.Clamp(groupStart, start, end);
            points.Add(new Sample(minTime, minTime, min));
            if (max != min)
            {
                var maxTime = Math.Clamp(groupStart + width / 2, start, end);
                points.Add(new Sample(maxTime, maxTime, max));
            }
        }
        for (var i = level.FirstEndingAfter(start, origin); i < level.Count; i++)
        {
            var bucketStart = origin + (double)level.Keys[i] * level.Step;
            if (bucketStart > end)
                break;
            var key = (long)Math.Floor((bucketStart - start) / width);
            if (key != group)
            {
                Flush();
                group = key;
                groupStart = start + key * width;
                min = level.Min[i];
                max = level.Max[i];
            }
            else
            {
                min = Math.Min(min, level.Min[i]);
                max = Math.Max(max, level.Max[i]);
            }
        }
        Flush();
        return points.ToArray();
    }

    // Exact statistics over the base buckets overlapping [start, end], read from the
    // coarsest level buckets that fit inside the range and finer levels at its edges.
    public RangeStats? Stats(double start, double end)
    {
        if (baseLevel.Count == 0 || end < start)
            return null;
        var step = baseLevel.Step;
        var first = (long)Math.Floor((start - origin) / step);
        if (origin + (first + 1) * (double)step <= start)
            first++;
        var last = (long)Math.Floor((end - origin) / step);
        var total = new Totals();
        Accumulate(levels.Count - 1, first, last, ref total);
        if (total.Count == 0)
            return null;
        // Peak and last times come from the base bucket inside the level bucket that held them.
        var maxKey = Refine(total.MaxLevel, total.MaxKey, first, last, i => baseLevel.Max[i] == total.Max, fromEnd: false);
        var lastKey = Refine(total.LastLevel, total.LastKey, first, last, _ => true, fromEnd: true);
        return new RangeStats(total.Min, total.Sum / total.Count, total.Max, origin + (double)maxKey * step,
            total.Count, total.Last, origin + (double)lastKey * step);
    }

    private struct Totals
    {
        public double Min, Max, Sum, Last;
        public long Count;
        public int MaxLevel, LastLevel;
        public long MaxKey, LastKey;
    }

    // Base keys [first, last] (inclusive), using level index `index` for whole buckets.
    private void Accumulate(int index, long first, long last, ref Totals total)
    {
        if (first > last)
            return;
        var level = levels[index];
        var ratio = level.Step / baseLevel.Step;
        var whole = index == 0 ? (first, last) : ((long)Math.Ceiling((double)first / ratio), (long)Math.Floor((double)(last + 1) / ratio) - 1);
        if (index > 0 && whole.Item1 > whole.Item2)
        {
            Accumulate(index - 1, first, last, ref total);
            return;
        }
        if (index > 0)
            Accumulate(index - 1, first, whole.Item1 * ratio - 1, ref total);
        for (var i = level.LowerBound(whole.Item1); i < level.Count && level.Keys[i] <= whole.Item2; i++)
        {
            if (total.Count == 0 || level.Min[i] < total.Min)
                total.Min = level.Min[i];
            if (total.Count == 0 || level.Max[i] > total.Max)
                (total.Max, total.MaxLevel, total.MaxKey) = (level.Max[i], index, level.Keys[i]);
            total.Sum += level.Sum[i];
            total.Count += level.Readings[i];
            (total.Last, total.LastLevel, total.LastKey) = (level.Last[i], index, level.Keys[i]);
        }
        if (index > 0)
            Accumulate(index - 1, (whole.Item2 + 1) * ratio, last, ref total);
    }

    // The base key, within level bucket `key` and inside [first, last], of the first
    // (or last) base bucket matching.
    private long Refine(int index, long key, long first, long last, Func<int, bool> match, bool fromEnd)
    {
        if (index == 0)
            return key;
        var ratio = levels[index].Step / baseLevel.Step;
        var low = Math.Max(first, key * ratio);
        var high = Math.Min(last, (key + 1) * ratio - 1);
        var begin = baseLevel.LowerBound(low);
        var stop = baseLevel.LowerBound(high + 1);
        if (fromEnd)
        {
            for (var i = stop - 1; i >= begin; i--)
                if (match(i))
                    return baseLevel.Keys[i];
        }
        else
            for (var i = begin; i < stop; i++)
                if (match(i))
                    return baseLevel.Keys[i];
        return key * ratio;
    }
}
