using System.Text.Json;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void BrokerSettingsChecks()
    {
        var settings = new BrokerSettings { Host = "example", Username = "user", Password = "secret" };
        Check(!JsonSerializer.Serialize(settings).Contains("secret"), "password never serialized");
        foreach (var topic in new[] { "#", "cake-adapt/#", "cake-adapt/+/main" })
            BrokerSettings.ValidateTopic(topic);
        foreach (var topic in new[] { "", "a#", "a/#/b", "a/+b" })
        {
            var rejected = false;
            try
            {
                BrokerSettings.ValidateTopic(topic);
            }
            catch (ArgumentException) { rejected = true; }
            Check(rejected, "reject invalid subscription " + topic);
        }
    }

}
