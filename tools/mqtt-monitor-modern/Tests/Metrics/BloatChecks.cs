using CakeAdaptMonitor;

static partial class Tests
{
    private static void BloatChecks()
    {
        var store = new MetricStore();
        const double now = 1_800_000_000;
        void Data(string topic, double time, string dl, string ul) => store.Accept(topic,
            $"DATA_HEADER; LOG_TIMESTAMP; REFLECTOR; DL_LOAD_CONDITION; UL_LOAD_CONDITION\nDATA; {time:R}; 1.1.1.1; {dl}; {ul}", time);
        void Summary(string topic, double time, string dl, string ul) => store.Accept(topic,
            $"SUMMARY; date; {time:R}; 1; 1; 1; 1; 1; 1; {dl}; {ul}; 1; 1", time);
        for (var i = 0; i < 10; i++)
            Data("a", now - 100 + i, i < 3 ? "dl_high_bb" : "dl_high", i < 1 ? "ul_low_bb" : "ul_low");
        Data("a", now - 500, "dl_idle_bb", "ul_idle");
        var five = store.Bufferbloat(300, now);
        int Count(string topic, string direction) => five.Where(v => v.Topic == topic && v.Direction == direction).Sum(v => v.Count);
        Check(Count("a", "DL") == 3 && Count("a", "UL") == 1, "bufferbloat counts _bb samples per direction");
        Check(store.Bufferbloat(600, now).Where(v => v.Topic == "a" && v.Direction == "DL").Sum(v => v.Count) == 4,
            "bufferbloat counts follow the selected window");
        Summary("a", now - 50, "dl_high_bb", "ul_high_bb");
        Check(store.Bufferbloat(300, now).Where(v => v.Topic == "a").Sum(v => v.Count) == 4,
            "a source publishing DATA is not counted again from SUMMARY");
        Summary("b", now - 50, "dl_high_bb", "ul_low");
        var b = store.Bufferbloat(300, now).Where(v => v.Topic == "b").ToArray();
        Check(b.Single(v => v.Direction == "DL").Count == 1 && b.Single(v => v.Direction == "UL").Count == 0,
            "a source without DATA is counted from SUMMARY, separately per source");
        var hourly = new MetricStore();
        for (var t = 0; t < 7200; t += 1)
            hourly.Accept("h", $"DATA_HEADER; LOG_TIMESTAMP; REFLECTOR; DL_LOAD_CONDITION; UL_LOAD_CONDITION\nDATA; {now + t:R}; x; dl_high_bb; ul_low", now + t);
        var hour = hourly.Bufferbloat(3600, now + 7200).Single(v => v.Direction == "DL");
        Check(hour.Buckets.Length <= 1801 && hour.Count is >= 3598 and <= 3601,
            "bufferbloat history keeps one hour in bounded buckets");
    }
}
