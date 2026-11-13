using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace MobCam;

/// <summary>A phone shown in the "Phones nearby" list.</summary>
public sealed class DeviceItem
{
    public int Link { get; init; }
    public string Name { get; init; } = "";
    public string Host { get; init; } = "";
    public string Glyph => Link == Native.LinkUsb ? "" : "";
    public string Detail => Link == Native.LinkUsb ? "USB cable" : $"Wi-Fi · {Host}";
    public string StateGlyph { get; set; } = "";
}

/// <summary>A phone in the "Paired phones" list (trusted for Wi-Fi by this Windows user).</summary>
public sealed class PairedPhoneItem
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public DateTime PairedAt { get; init; }
    public string Detail => $"Paired {PairedAt.ToLocalTime():d}";
}

public partial class MainWindow : Window
{
    private const int PreviewWidth = 960, PreviewHeight = 540;

    private readonly Settings _settings;
    private readonly WriteableBitmap _preview =
        new(PreviewWidth, PreviewHeight, 96, 96, PixelFormats.Bgra32, null);
    private readonly DispatcherTimer _statusTimer = new() { Interval = TimeSpan.FromMilliseconds(250) };
    // Wakes up hourly; UpdateCheck itself runs at most once a day (MobCam may sit in the tray for weeks).
    private readonly DispatcherTimer _updateTimer = new() { Interval = TimeSpan.FromHours(1) };
    private bool _updateChecking;
    private readonly ObservableCollection<DeviceItem> _devices = new();
    private readonly ObservableCollection<PairedPhoneItem> _paired = new();

    private Native.Status _status;
    private long _previewSeq;
    private bool _wantConnected;
    private int _status0State = Native.StateIdle; // state at the previous status update
    private bool _discovering;
    private bool _vcamBusy;
    private bool _shuttingDown;
    private DateTime _phoneSyncPausedUntil = DateTime.MinValue;

    internal TrayIcon? Tray { get; set; }
    public event Action? ExitRequested;

    public MainWindow(Settings settings, ImageSource icon)
    {
        _settings = settings;
        InitializeComponent();
        Icon = icon;
        LogoImage.Source = icon;
        // Which build is running: version in the title bar, version + commit in the App card.
        Title = $"MobCam {BuildInfo.Version}";
        VersionText.Text = $"Version {BuildInfo.Label}";
        Preview.Source = _preview;
        DeviceList.ItemsSource = _devices;
        PairedList.ItemsSource = _paired;
    }

    private void OnAbout(object sender, RoutedEventArgs e) => new AboutWindow(Icon) { Owner = this }.ShowDialog();

    /// <summary>Starts the engine side. Called once, even when MobCam starts hidden in the tray.</summary>
    public void Start()
    {
        ApplySettingsToUi();
        Native.mc_set_rotation(_settings.Rotation);
        Native.mc_set_fill(_settings.Fill ? 1 : 0);
        StartVirtualCamera();

        _statusTimer.Tick += (_, _) => UpdateStatus();
        _statusTimer.Start();
        CompositionTarget.Rendering += OnRendering;
        DiscoveryLoop();
        RefreshPairedPhones();

        if (_settings.AutoConnect) Connect(_settings.Mode, _settings.Mode == Native.LinkWifi ? _settings.Host : "");
        UpdateStatus();

        RefreshUpdateCard();
        _updateTimer.Tick += async (_, _) => await CheckForUpdatesAsync();
        _updateTimer.Start();
        _ = CheckForUpdatesAsync();
    }

    public void Shutdown()
    {
        _shuttingDown = true;
        _statusTimer.Stop();
        _updateTimer.Stop();
        CompositionTarget.Rendering -= OnRendering;
        _settings.Save();
        Close();
    }

    // ================================================================ connection

    public void ToggleConnection()
    {
        if (_wantConnected) Disconnect();
        else Connect(SelectedMode, SelectedMode == Native.LinkWifi ? HostBox.Text.Trim() : "");
    }

    private int SelectedMode =>
        ModeUsb.IsChecked == true ? Native.LinkUsb : ModeWifi.IsChecked == true ? Native.LinkWifi : Native.LinkAuto;

    private void Connect(int link, string host)
    {
        _wantConnected = true;
        _settings.Mode = SelectedMode;
        _settings.Host = HostBox.Text.Trim();
        _settings.Save();
        // mc_connect joins the previous worker, which can take a moment: keep the UI responsive.
        Task.Run(() => Native.mc_connect(link, host, 0));
        UpdateStatus();
    }

    private void Disconnect()
    {
        _wantConnected = false;
        Task.Run(Native.mc_disconnect);
        UpdateStatus();
    }

    private void OnConnectClick(object sender, RoutedEventArgs e) => ToggleConnection();

    // Two-sided pairing: this PC's user compares the code too.
    private void OnPairMatch(object sender, RoutedEventArgs e)
    {
        Native.mc_confirm_pairing(1);
        UpdateStatus();
    }

    private void OnPairCancel(object sender, RoutedEventArgs e)
    {
        Native.mc_confirm_pairing(0);
        UpdateStatus();
    }

    private void OnDeviceClick(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not DeviceItem device) return;
        Connect(device.Link, device.Link == Native.LinkWifi ? device.Host : "");
    }

    private void OnModeClick(object sender, RoutedEventArgs e)
    {
        UpdateModeUi();
        _settings.Mode = SelectedMode;
        _settings.Save();
    }

    private void UpdateModeUi()
    {
        HostPanel.Visibility = SelectedMode == Native.LinkWifi ? Visibility.Visible : Visibility.Collapsed;
        ModeHint.Text = SelectedMode switch
        {
            Native.LinkUsb => "Uses the USB cable (USB debugging must be on). Lowest delay, charges the phone.",
            Native.LinkWifi => "Finds the phone on your network. PC and phone must be on the same Wi-Fi.",
            _ => "Uses USB when the phone is plugged in, otherwise finds it on Wi-Fi.",
        };
    }

    // ================================================================ discovery

    private async void DiscoveryLoop()
    {
        while (!_shuttingDown)
        {
            if (IsVisible && _status.State != Native.StateStreaming) await RefreshDevices();
            await Task.Delay(4000);
        }
    }

    private async Task RefreshDevices()
    {
        if (_discovering) return;
        _discovering = true;
        RefreshButton.IsEnabled = false;
        try
        {
            var found = await Task.Run(() =>
            {
                var buffer = new Native.Device[8];
                int n = Native.mc_discover(buffer, buffer.Length, 1200);
                return buffer.Take(n).ToArray();
            });
            _devices.Clear();
            foreach (var d in found)
                _devices.Add(new DeviceItem { Link = d.Link, Name = d.Name, Host = d.Host });
            MarkConnectedDevice();
        }
        finally
        {
            _discovering = false;
            RefreshButton.IsEnabled = true;
        }
    }

    private async void OnRefresh(object sender, RoutedEventArgs e) => await RefreshDevices();

    private void MarkConnectedDevice()
    {
        bool live = _status.State == Native.StateStreaming;
        bool changed = false;
        foreach (var d in _devices)
        {
            bool isCurrent = live && d.Link == _status.Link &&
                             (d.Link == Native.LinkUsb || _status.Peer == d.Name || _status.Peer == d.Host);
            string glyph = isCurrent ? "" : "";
            if (d.StateGlyph != glyph) { d.StateGlyph = glyph; changed = true; }
        }
        if (changed) DeviceList.Items.Refresh();
        NoDevicesText.Visibility = _devices.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    // ================================================================ virtual camera

    private void StartVirtualCamera()
    {
        int state = Native.mc_vcam_state();
        if (state == Native.VcamInstalled || state == Native.VcamFailed) Native.mc_vcam_start();
    }

    private async void OnVcamButton(object sender, RoutedEventArgs e)
    {
        if (_vcamBusy) return;
        _vcamBusy = true;
        VcamButton.IsEnabled = false;
        VcamButton.Content = "Waiting for permission…";
        try
        {
            int state = Native.mc_vcam_state();
            if (state is Native.VcamNotInstalled or Native.VcamOutdated)
            {
                int rc = await Task.Run(Native.mc_vcam_install);
                if (rc != 0)
                {
                    VcamText.Text = "Install was cancelled or failed. Administrator permission is needed once.";
                    return;
                }
            }
            await Task.Run(Native.mc_vcam_start);
        }
        finally
        {
            _vcamBusy = false;
            VcamButton.IsEnabled = true;
            UpdateStatus();
        }
    }

    private void UpdateVcamUi(int state, int error)
    {
        if (_vcamBusy) return;
        (string glyph, Brush brush, string text, string? button) = state switch
        {
            Native.VcamRunning => ("", Res("SystemFillColorSuccessBrush"),
                "“MobCam” is available to your apps.", (string?)null),
            Native.VcamNotInstalled => ("", Res("SystemFillColorCautionBrush"),
                "Not installed yet. Needs administrator permission once.", "Install virtual camera"),
            Native.VcamOutdated => ("", Res("SystemFillColorCautionBrush"),
                "A newer version is ready to install.", "Update virtual camera"),
            Native.VcamFailed => ("", Res("SystemFillColorCriticalBrush"),
                $"Could not start (0x{error:X8}). Try reinstalling.", "Reinstall"),
            _ => ("", Res("TextFillColorSecondaryBrush"), "Installed, starting…", "Start"),
        };
        VcamIcon.Text = glyph;
        VcamIcon.Foreground = brush;
        VcamText.Text = text;
        VcamButton.Visibility = button == null ? Visibility.Collapsed : Visibility.Visible;
        if (button != null) VcamButton.Content = button;
    }

    // ================================================================ status

    private void UpdateStatus()
    {
        Native.mc_get_status(out _status);
        var s = _status;
        bool streaming = s.State == Native.StateStreaming;
        bool live = streaming && s.Width > 0;
        string link = s.Link == Native.LinkUsb ? "USB" : "Wi-Fi";

        // Header pill
        (string pill, Brush dot) = s.State switch
        {
            Native.StateStreaming => (live ? $"Live · {link}" : "Starting video…", Res("SystemFillColorSuccessBrush")),
            Native.StateConnecting => ("Connecting…", Res("SystemFillColorCautionBrush")),
            Native.StatePairing => ("Pairing…", Res("SystemFillColorCautionBrush")),
            Native.StateSearching => ("Searching…", Res("SystemFillColorCautionBrush")),
            _ => ("Not connected", Res("TextFillColorTertiaryBrush")),
        };
        StateText.Text = pill;
        StateDot.Fill = dot;

        // Preview overlay
        Overlay.Visibility = live ? Visibility.Collapsed : Visibility.Visible;
        Preview.Opacity = live ? 1.0 : 0.25;
        bool askUser = s.State == Native.StatePairing && s.PairingStage is Native.PairingConfirm or Native.PairingWaitUser;
        PairingButtons.Visibility = askUser ? Visibility.Visible : Visibility.Collapsed;
        OverlayProgress.Visibility = s.State is Native.StateSearching or Native.StateConnecting || (streaming && !live) ||
                                     (s.State == Native.StatePairing && !askUser)
            ? Visibility.Visible : Visibility.Collapsed;
        // Pairing needs this PC's user: bring the window up if MobCam is in the tray.
        if (s.State == Native.StatePairing && _status0State != Native.StatePairing && !IsVisible)
        {
            Show();
            Activate();
        }
        if (s.State != _status0State) RefreshPairedPhones(); // e.g. a pairing just finished
        _status0State = s.State;

        // The engine stopped by itself (denied, cancelled, timed out, not paired): offer Connect again.
        if (_wantConnected && s.State == Native.StateIdle && s.Problem != Native.ProblemNone) _wantConnected = false;
        if (!live)
        {
            switch (s.State)
            {
                case Native.StateIdle:
                    OverlayIcon.Text = "";
                    OverlayTitle.Text = s.Problem == Native.ProblemNone ? "Not connected" : "Couldn't connect";
                    OverlayText.Text = s.Problem == Native.ProblemNone
                        ? "Open the MobCam app on your phone, then click Connect or pick it from Phones nearby."
                        : s.Message;
                    break;
                case Native.StatePairing:
                    OverlayIcon.Text = ""; // lock
                    OverlayTitle.Text = $"Pairing code  {s.PairingCode / 1000:000} {s.PairingCode % 1000:000}";
                    OverlayText.Text = s.PairingStage switch
                    {
                        Native.PairingWaitPhone => $"Thanks. Now tap Allow on {s.Peer}.",
                        Native.PairingWaitUser => $"{s.Peer} allowed the connection. Does it show the same code?",
                        _ => $"Does {s.Peer} show the same code? Confirm here and tap Allow on the phone.",
                    };
                    break;
                case Native.StateStreaming:
                    OverlayIcon.Text = "";
                    OverlayTitle.Text = $"Connected to {s.Peer}";
                    OverlayText.Text = "Waiting for video… keep the MobCam app open on the phone.";
                    break;
                default:
                    OverlayIcon.Text = s.State == Native.StateConnecting ? "" : "";
                    OverlayTitle.Text = s.State == Native.StateConnecting ? "Connecting" : "Looking for your phone";
                    OverlayText.Text = s.Message;
                    break;
            }
        }

        // Footer
        Stats.Text = live
            ? $"{s.Width}×{s.Height}  ·  {s.Fps} fps  ·  {s.Kbps / 1000.0:0.0} Mbps  ·  {link}  ·  {s.Peer}"
            : s.Message;
        bool inUse = s.CameraInUse != 0;
        InUseText.Text = inUse ? "An app is using the camera" : "No app is using the camera";
        InUseDot.Fill = inUse ? Res("SystemFillColorSuccessBrush") : Res("TextFillColorTertiaryBrush");

        // Connection card
        ConnectButton.Content = _wantConnected ? "Disconnect" : "Connect";
        MarkConnectedDevice();
        UpdateVcamUi(s.Vcam, s.VcamError);

        // Phone remote control mirrors what the phone reports
        PhonePanel.IsEnabled = live;
        PhoneHint.Text = live
            ? "Changes are sent to the phone right away."
            : "Connect a phone to control its camera from here.";
        UpdateLenses(s, live);
        if (live && DateTime.Now > _phoneSyncPausedUntil)
        {
            if (s.PhoneFacing >= 0)
            {
                FacingBack.IsChecked = s.PhoneFacing == 0;
                FacingFront.IsChecked = s.PhoneFacing == 1;
            }
            if (s.PhoneTargetHeight > 0)
            {
                Res480.IsChecked = s.PhoneTargetHeight == 480;
                Res720.IsChecked = s.PhoneTargetHeight == 720;
                Res1080.IsChecked = s.PhoneTargetHeight == 1080;
            }
        }

        Tray?.Update(live ? $"MobCam · Live over {link} ({s.Width}×{s.Height})" : $"MobCam · {pill}", _wantConnected);
    }

    private void OnRendering(object? sender, EventArgs e)
    {
        if (!IsVisible || WindowState == WindowState.Minimized) return;
        _preview.Lock();
        try
        {
            unsafe
            {
                int r = Native.mc_get_preview((byte*)_preview.BackBuffer, PreviewWidth, PreviewHeight,
                    _preview.BackBufferStride, ref _previewSeq);
                if (r == 1) _preview.AddDirtyRect(new Int32Rect(0, 0, PreviewWidth, PreviewHeight));
            }
        }
        finally
        {
            _preview.Unlock();
        }
    }

    // ================================================================ phone / picture / app options

    private void PausePhoneSync() => _phoneSyncPausedUntil = DateTime.Now.AddSeconds(3);

    private string _lensSignature = "";

    /// <summary>Builds one button per phone lens (0.6×, 1×, 3.4×, Front…) and mirrors the active one.</summary>
    private void UpdateLenses(Native.Status s, bool live)
    {
        var lenses = Array.Empty<Native.Lens>();
        if (live && s.LensCount > 0)
        {
            var buffer = new Native.Lens[16];
            int n = Native.mc_get_lenses(buffer, buffer.Length);
            lenses = buffer.Take(n).ToArray();
        }

        string signature = string.Join("|", lenses.Select(l => $"{l.Label}:{l.Zoom}:{l.Front}"));
        if (signature != _lensSignature)
        {
            _lensSignature = signature;
            LensPanel.Children.Clear();
            for (int i = 0; i < lenses.Length; i++)
            {
                var lens = lenses[i];
                var button = new RadioButton
                {
                    GroupName = "Lens",
                    Style = (Style)FindResource("Segment"),
                    Tag = i,
                    Content = lens.Front != 0
                        ? lens.Label
                        : lens.Zoom.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + "×",
                    ToolTip = lens.Label,
                    Margin = new Thickness(0, 0, 4, 4),
                };
                button.Click += OnLensClick;
                LensPanel.Children.Add(button);
            }
        }

        bool haveLenses = lenses.Length > 0;
        LensPanel.Visibility = haveLenses ? Visibility.Visible : Visibility.Collapsed;
        FacingPanel.Visibility = haveLenses ? Visibility.Collapsed : Visibility.Visible;  // older phone app
        LensTitle.Text = haveLenses && s.PhoneLens >= 0 && s.PhoneLens < lenses.Length
            ? $"Lens  ·  {lenses[s.PhoneLens].Label}"
            : "Lens";
        if (haveLenses && DateTime.Now > _phoneSyncPausedUntil)
        {
            foreach (var child in LensPanel.Children.OfType<RadioButton>())
                child.IsChecked = (int)child.Tag == s.PhoneLens;
        }
    }

    private void OnLensClick(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not int index) return;
        PausePhoneSync();
        Native.mc_phone_command($"LENS {index}");
    }

    private void OnFacingClick(object sender, RoutedEventArgs e)
    {
        PausePhoneSync();
        Native.mc_phone_command(FacingFront.IsChecked == true ? "FACING front" : "FACING back");
    }

    private void OnResolutionClick(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not string tag) return;
        PausePhoneSync();
        Native.mc_phone_command($"RES {tag}");
    }

    private void OnRotationClick(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not string tag || !int.TryParse(tag, out int degrees)) return;
        _settings.Rotation = degrees;
        Native.mc_set_rotation(degrees);
        _settings.Save();
    }

    private void OnFramingClick(object sender, RoutedEventArgs e)
    {
        _settings.Fill = FrameFill.IsChecked == true;
        Native.mc_set_fill(_settings.Fill ? 1 : 0);
        _settings.Save();
    }

    // ================================================================ paired phones

    private void RefreshPairedPhones()
    {
        var buffer = new Native.PairedPhone[32];
        int n = Native.mc_get_paired_phones(buffer, buffer.Length);
        var phones = buffer.Take(n)
            .Select(p => new PairedPhoneItem
            {
                Id = p.Id,
                Name = string.IsNullOrWhiteSpace(p.Name) ? "Phone" : p.Name,
                PairedAt = DateTimeOffset.FromUnixTimeSeconds(p.PairedAt).UtcDateTime,
            })
            .OrderByDescending(p => p.PairedAt)
            .ToList();
        if (!phones.Select(p => p.Id).SequenceEqual(_paired.Select(p => p.Id)))
        {
            _paired.Clear();
            foreach (var p in phones) _paired.Add(p);
        }
        NoPairedText.Visibility = _paired.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnForgetPhone(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not PairedPhoneItem phone) return;
        var answer = MessageBox.Show(this,
            $"Forget {phone.Name}?\n\nTo use it over Wi-Fi again, also forget this PC under Paired PCs on the phone, then pair again. USB keeps working.",
            "MobCam", MessageBoxButton.OKCancel, MessageBoxImage.Question);
        if (answer != MessageBoxResult.OK) return;
        Native.mc_forget_phone(phone.Id);
        RefreshPairedPhones();
    }

    private void OnAppOptionClick(object sender, RoutedEventArgs e)
    {
        _settings.AutoConnect = AutoConnectBox.IsChecked == true;
        _settings.CloseToTray = TrayBox.IsChecked == true;
        try
        {
            Settings.StartWithWindows = StartupBox.IsChecked == true;
        }
        catch
        {
            StartupBox.IsChecked = Settings.StartWithWindows;
        }
        _settings.Save();
    }

    // ================================================================ new version available (#16)

    /// <summary>The latest release on GitHub if it's newer than this build and not skipped, else null.</summary>
    private Version? AvailableUpdate =>
        _settings.CheckForUpdates == true && UpdateCheck.ParseVersion(_settings.LatestVersion) is { } latest &&
        latest > UpdateCheck.Current && _settings.LatestVersion != _settings.SkippedVersion
            ? latest
            : null;

    /// <summary>Asks GitHub when the user agreed and a day has passed (or <paramref name="now"/>).</summary>
    private async Task CheckForUpdatesAsync(bool now = false)
    {
        if (_settings.CheckForUpdates != true || _updateChecking) return;
        if (!now && DateTime.UtcNow - _settings.LastUpdateCheck < UpdateCheck.Interval) return;
        _updateChecking = true;
        try
        {
            // Unreachable (offline, GitHub down): try again at the next hourly tick.
            if (await UpdateCheck.FetchLatestAsync() is not { } latest || _shuttingDown) return;
            _settings.LastUpdateCheck = DateTime.UtcNow;
            _settings.LatestVersion = latest.Version.ToString();
            _settings.LatestUrl = latest.Url;
            _settings.Save();
            RefreshUpdateCard();
            if (AvailableUpdate is { } version && !IsVisible && _settings.NotifiedVersion != _settings.LatestVersion)
            {
                Tray?.ShowHint($"MobCam {version} is available", "Open MobCam to download it.");
                _settings.NotifiedVersion = _settings.LatestVersion;
                _settings.Save();
            }
        }
        finally
        {
            _updateChecking = false;
        }
    }

    private void RefreshUpdateCard()
    {
        if (_settings.CheckForUpdates is null)
        {
            UpdateTitle.Text = "Check for new versions?";
            UpdateText.Text = "Once a day, MobCam can ask GitHub for the latest version number and tell you when " +
                              "there's a new one. Nothing else is sent, and nothing is downloaded or installed. " +
                              "You can change this later under App.";
            UpdateNoButton.Content = "No thanks";
            UpdateYesButton.Content = "Yes, check daily";
            UpdateCard.Visibility = Visibility.Visible;
        }
        else if (AvailableUpdate is { } version)
        {
            UpdateTitle.Text = $"MobCam {version} is available";
            UpdateText.Text = $"You have {BuildInfo.Version}. Download the installer and the APK from the release page.";
            UpdateNoButton.Content = "Later";
            UpdateYesButton.Content = "Download";
            UpdateCard.Visibility = Visibility.Visible;
        }
        else
        {
            UpdateCard.Visibility = Visibility.Collapsed;
        }
        UpdateBox.IsChecked = _settings.CheckForUpdates == true;
    }

    private void OnUpdateYes(object sender, RoutedEventArgs e)
    {
        if (_settings.CheckForUpdates is null)
        {
            SetCheckForUpdates(true);
        }
        else if (AvailableUpdate is not null && _settings.LatestUrl.StartsWith("https://github.com/Ishtiaqasif/mobcam/releases/"))
        {
            Process.Start(new ProcessStartInfo(_settings.LatestUrl) { UseShellExecute = true });
        }
    }

    private void OnUpdateNo(object sender, RoutedEventArgs e)
    {
        if (_settings.CheckForUpdates is null)
        {
            SetCheckForUpdates(false);
            return;
        }
        _settings.SkippedVersion = _settings.LatestVersion; // back for the next version
        _settings.Save();
        RefreshUpdateCard();
    }

    private void OnUpdateBoxClick(object sender, RoutedEventArgs e) => SetCheckForUpdates(UpdateBox.IsChecked == true);

    private void SetCheckForUpdates(bool on)
    {
        _settings.CheckForUpdates = on;
        _settings.Save();
        RefreshUpdateCard();
        if (on) _ = CheckForUpdatesAsync(now: true);
    }

    private void ApplySettingsToUi()
    {
        ModeAuto.IsChecked = _settings.Mode == Native.LinkAuto;
        ModeUsb.IsChecked = _settings.Mode == Native.LinkUsb;
        ModeWifi.IsChecked = _settings.Mode == Native.LinkWifi;
        HostBox.Text = _settings.Host;
        UpdateModeUi();

        RotAuto.IsChecked = _settings.Rotation < 0;
        Rot0.IsChecked = _settings.Rotation == 0;
        Rot90.IsChecked = _settings.Rotation == 90;
        Rot180.IsChecked = _settings.Rotation == 180;
        Rot270.IsChecked = _settings.Rotation == 270;
        FrameFit.IsChecked = !_settings.Fill;
        FrameFill.IsChecked = _settings.Fill;

        AutoConnectBox.IsChecked = _settings.AutoConnect;
        TrayBox.IsChecked = _settings.CloseToTray;
        StartupBox.IsChecked = Settings.StartWithWindows;
    }

    // ================================================================ window lifetime

    protected override void OnClosing(CancelEventArgs e)
    {
        if (!_shuttingDown && _settings.CloseToTray)
        {
            // Keep feeding the virtual camera from the tray.
            e.Cancel = true;
            Hide();
            if (!_settings.TrayHintShown)
            {
                Tray?.ShowHint("MobCam is still running",
                    "The camera keeps working. Right-click the tray icon to exit.");
                _settings.TrayHintShown = true;
                _settings.Save();
            }
            return;
        }
        if (!_shuttingDown)
        {
            e.Cancel = true;
            ExitRequested?.Invoke();
            return;
        }
        base.OnClosing(e);
    }

    private Brush Res(string key) => TryFindResource(key) as Brush ?? Brushes.Gray;
}
