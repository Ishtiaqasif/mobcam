using System;
using System.Linq;
using System.Threading;
using System.Windows;

namespace MobCam;

public partial class App : System.Windows.Application
{
    private const string InstanceMutex = "MobCam.SingleInstance";
    private const string ShowEvent = "MobCam.ShowWindow";

    private Mutex? _mutex;
    private EventWaitHandle? _showSignal;
    private MainWindow? _window;
    private TrayIcon? _tray;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // Single instance: a second launch just brings the running window forward.
        _mutex = new Mutex(true, InstanceMutex, out bool first);
        _showSignal = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEvent);
        if (!first)
        {
            _showSignal.Set();
            Shutdown();
            return;
        }

        try
        {
            if (Native.mc_init() != 0) throw new InvalidOperationException("Media Foundation or networking is unavailable.");
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or InvalidOperationException)
        {
            System.Windows.MessageBox.Show($"MobCam could not start its engine:\n\n{ex.Message}\n\nmobcam-core.dll must be next to MobCam.exe.",
                "MobCam", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown(1);
            return;
        }

        var settings = Settings.Load();
        var icon = AppIcon.Tray();
        _window = new MainWindow(settings, AppIcon.Image());
        _tray = new TrayIcon(icon, ShowWindow, () => _window.ToggleConnection(), ExitApp);
        _window.Tray = _tray;
        _window.ExitRequested += ExitApp;
        _window.Start();

        if (!e.Args.Contains("--tray")) ShowWindow();

        var listener = new Thread(() =>
        {
            while (_showSignal.WaitOne())
                Dispatcher.BeginInvoke(ShowWindow);
        }) { IsBackground = true, Name = "MobCam.ShowListener" };
        listener.Start();
    }

    private void ShowWindow()
    {
        if (_window == null) return;
        _window.Show();
        if (_window.WindowState == WindowState.Minimized) _window.WindowState = WindowState.Normal;
        _window.Activate();
    }

    private void ExitApp()
    {
        _window?.Shutdown();
        _tray?.Dispose();
        _tray = null;
        Native.mc_shutdown();
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _tray?.Dispose();
        _mutex?.Dispose();
        base.OnExit(e);
    }
}
