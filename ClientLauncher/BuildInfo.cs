using System.Reflection;

namespace SkyrimMPLauncher;

internal static class BuildInfo
{
    public const string ReleaseVersion = "0.1.0-alpha.4";
    public const string Channel = "re-0.1-runtime-probe";
    public const ushort WireProtocol = 2;
    public const ushort ReplicationProtocol = 10;

    public static string Version =>
        typeof(BuildInfo).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? ReleaseVersion;

    public static string Display => $"{Version} | wire {WireProtocol} | replication {ReplicationProtocol}";
}
