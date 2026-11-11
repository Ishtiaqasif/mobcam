using System;
using System.IO;
using System.Text.Json;
using Microsoft.Win32;

namespace MobCam;

/// <summary>User preferences, stored in %AppData%\MobCam\settings.json.</summary>
public sealed class Settings
{
    public bool AutoConnect { get; set; } = true;
    public bool CloseToTray { get; set; } = true;
    public int Rotation { get; set; } = -1; // -1 = follow the phone
    public bool Fill { get; set; }
    public int Mode { get; set; } = Native.LinkAuto;
    public string Host { get; set; } = "";
    public bool TrayHintShown { get; set; }

    private static string FilePath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "MobCam", "settings.json");

    public static Settings Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)) ?? new Settings();
        }
        catch
        {
            // corrupt file: fall back to defaults
        }
        return new Settings();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch
        {
            // not fatal
        }
    }

    // ---- Start with Windows (HKCU Run key) ----

    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string RunValue = "MobCam";

    public static bool StartWithWindows
    {
        get
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey);
            return key?.GetValue(RunValue) is string;
        }
        set
        {
            using var key = Registry.CurrentUser.CreateSubKey(RunKey);
            if (value)
                key.SetValue(RunValue, $"\"{Environment.ProcessPath}\" --tray");
            else
                key.DeleteValue(RunValue, false);
        }
    }
}
