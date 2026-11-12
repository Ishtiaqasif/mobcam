using System.Linq;
using System.Reflection;

namespace MobCam;

/// <summary>Version and git commit this build was made from (set by the MobCamBuildInfo target).</summary>
internal static class BuildInfo
{
    private static readonly Assembly Self = typeof(BuildInfo).Assembly;

    /// <summary>"1.0.0"</summary>
    public static string Version { get; } =
        Self.GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "?";

    /// <summary>Short git commit, or "" if unknown.</summary>
    public static string Commit { get; } = Metadata("Commit");

    /// <summary>True if built from a working tree with uncommitted changes.</summary>
    public static bool Modified { get; } = Metadata("Modified") == "true";

    /// <summary>"1.0.0 · e964e90 · modified"</summary>
    public static string Label =>
        string.Join(" · ", new[] { Version, Commit, Modified ? "modified" : "" }.Where(s => s.Length > 0));

    private static string Metadata(string key) =>
        Self.GetCustomAttributes<AssemblyMetadataAttribute>().FirstOrDefault(a => a.Key == key)?.Value ?? "";
}
