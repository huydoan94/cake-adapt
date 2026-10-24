using CakeAdaptMonitor;

static partial class Tests
{
    private static MetricStore SampleTelemetry()
    {
        var store = new MetricStore();
        var now = Telemetry.Now;
        for (var i = 0; i < 300; i++)
        {
            var t = now - 299 + i;
            var dl = 80000 + 35000 * Math.Sin(i / 20.0) + 25000 * Math.Sin(i / 6.0);
            store.Accept("cake-adapt/sample/main", $"LOAD; date; {t:R}; 100; {dl:F0}; {5000 + i * 20}; 180000; 25000", t);
            store.Accept("cake-adapt/sample/main", $"MEMORY; date; {t:R}; 100; {18000 + i * 2}; 19000; 10000; 30000", t);
            store.Accept("cake-adapt/sample/main", $"CPU; date; {t:R}; {t:R}; {30 + 15 * Math.Sin(i / 17.0):F0}; {20 + 10 * Math.Sin(i / 11.0):F0}; {40 + 25 * Math.Sin(i / 7.0):F0}", t);
            store.Accept("cake-adapt/sample/main", $"SUMMARY; date; {t:R}; {dl:F0}; {5000 + i * 20}; 1; 1; {2000 + 1200 * Math.Sin(i / 12.0):F0}; {1000 + 500 * Math.Sin(i / 8.0):F0}; low; low; 180000; 25000", t);
        }
        return store;
    }

}
