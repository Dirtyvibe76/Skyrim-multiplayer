namespace SkyrimMPLauncher;

internal static class BuildInfo
{
    public const string Version = "0.1.0-alpha.3";
    public const string Channel = "re-0.1-runtime-probe";
    public const ushort WireProtocol = 2;
    public const ushort ReplicationProtocol = 9;

    public static string Display => $"{Version} | wire {WireProtocol} | replication {ReplicationProtocol}";
}
