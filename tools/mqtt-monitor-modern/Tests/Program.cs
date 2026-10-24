static partial class Tests
{
    private static int count;

    private static void Check(bool condition, string name)
    {
        if (!condition)
            throw new Exception("FAIL: " + name);
        Console.WriteLine("PASS: " + name);
        count++;
    }

    public static async Task Main(string[] args)
    {
        MetricStoreChecks();
        DecoderChecks();
        BrokerSettingsChecks();
        HistoryChecks();
        await Integration();
        await CancelPendingConnection();
        await TlsIntegration("valid");
        await TlsIntegration("wrong-name");
        await TlsIntegration("client-only");
        SelectionChecks();
        RatePeakChecks();
        BatchPeakChecks();
        Preview(args.Length > 0 ? args[0] : "preview.png");
        Console.WriteLine($"{count} checks passed");
    }
}
