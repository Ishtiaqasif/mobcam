using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Threading.Tasks;

namespace MobCam;

/// <summary>
/// "New version available" check (issue #16). Only with the user's consent (asked on first launch,
/// changeable in the App card), at most once a day: asks GitHub for the latest release's version
/// number and page. Nothing is downloaded or installed; the user opens the release page.
/// </summary>
public static class UpdateCheck
{
    private const string LatestReleaseApi = "https://api.github.com/repos/Ishtiaqasif/mobcam/releases/latest";
    private const string ReleasesPage = "https://github.com/Ishtiaqasif/mobcam/releases/";
    public static readonly TimeSpan Interval = TimeSpan.FromDays(1);

    public sealed record Release(Version Version, string Url);

    private static readonly HttpClient Http = CreateClient();

    private static HttpClient CreateClient()
    {
        var http = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
        // GitHub requires a User-Agent; it identifies the app and version, nothing about the user.
        http.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("MobCam", BuildInfo.Version));
        http.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        return http;
    }

    /// <summary>The latest published release, or null if GitHub can't be reached or answers unexpectedly.</summary>
    public static async Task<Release?> FetchLatestAsync()
    {
        try
        {
            using var response = await Http.GetAsync(LatestReleaseApi);
            if (!response.IsSuccessStatusCode) return null;
            using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            var tag = doc.RootElement.GetProperty("tag_name").GetString();
            var url = doc.RootElement.GetProperty("html_url").GetString();
            // Only ever open MobCam's own release pages, whatever the response says.
            if (ParseVersion(tag) is not { } version || url is null || !url.StartsWith(ReleasesPage, StringComparison.Ordinal))
                return null;
            return new Release(version, url);
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or JsonException
                                      or InvalidOperationException or KeyNotFoundException)
        {
            return null;
        }
    }

    /// <summary>"v1.2.0" or "1.2.0" → 1.2.0. Pre-releases ("1.3.0-rc.1") and anything else → null.</summary>
    public static Version? ParseVersion(string? text)
    {
        if (string.IsNullOrEmpty(text)) return null;
        if (text[0] is 'v' or 'V') text = text[1..];
        var parts = text.Split('.');
        if (parts.Length != 3) return null;
        return int.TryParse(parts[0], out int major) && int.TryParse(parts[1], out int minor) &&
               int.TryParse(parts[2], out int patch) && major >= 0 && minor >= 0 && patch >= 0
            ? new Version(major, minor, patch)
            : null;
    }

    public static Version Current => ParseVersion(BuildInfo.Version) ?? new Version(0, 0, 0);
}
