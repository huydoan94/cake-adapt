namespace CakeAdaptMonitor;

internal static class DisplayText
{
    public static string ShortTopic(string topic) => topic.Length <= UiLayout.TopicLength
        ? topic : UiText.Ellipsis + topic[^UiLayout.TopicTailLength..];
}
