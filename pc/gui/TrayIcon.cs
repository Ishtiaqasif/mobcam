using System;
using System.Windows.Forms;

namespace MobCam;

/// <summary>Notification-area icon: keeps MobCam feeding the camera while the window is closed.</summary>
internal sealed class TrayIcon : IDisposable
{
    private readonly NotifyIcon _icon;
    private readonly ToolStripMenuItem _connectItem;

    public TrayIcon(System.Drawing.Icon icon, Action open, Action toggleConnect, Action exit)
    {
        var menu = new ContextMenuStrip();
        menu.Items.Add("Open MobCam", null, (_, _) => open());
        _connectItem = new ToolStripMenuItem("Connect", null, (_, _) => toggleConnect());
        menu.Items.Add(_connectItem);
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Exit", null, (_, _) => exit());

        _icon = new NotifyIcon
        {
            Icon = icon,
            Text = "MobCam",
            Visible = true,
            ContextMenuStrip = menu,
        };
        _icon.MouseClick += (_, e) =>
        {
            if (e.Button == MouseButtons.Left) open();
        };
    }

    public void Update(string tooltip, bool connected)
    {
        _icon.Text = tooltip.Length > 63 ? tooltip[..63] : tooltip;
        _connectItem.Text = connected ? "Disconnect" : "Connect";
    }

    public void ShowHint(string title, string text) => _icon.ShowBalloonTip(4000, title, text, ToolTipIcon.None);

    public void Dispose()
    {
        _icon.Visible = false;
        _icon.Dispose();
    }
}
