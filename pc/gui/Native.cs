using System.Runtime.InteropServices;

namespace MobCam;

/// <summary>P/Invoke bindings for mobcam-core.dll (see pc/core/mobcam_core.h).</summary>
internal static unsafe class Native
{
    private const string Dll = "mobcam-core.dll";

    public const int LinkAuto = 0, LinkUsb = 1, LinkWifi = 2;
    public const int StateIdle = 0, StateSearching = 1, StateConnecting = 2, StateStreaming = 3, StatePairing = 4;
    public const int ProblemNone = 0, ProblemPhoneTooOld = 1, ProblemPcTooOld = 2, ProblemDenied = 3,
        ProblemPairingTimeout = 4, ProblemNotPaired = 5, ProblemAuthFailed = 6, ProblemCancelled = 7;
    public const int PairingNone = 0, PairingConfirm = 1, PairingWaitPhone = 2, PairingWaitUser = 3;
    public const int VcamNotInstalled = 0, VcamInstalled = 1, VcamOutdated = 2, VcamRunning = 3, VcamFailed = 4;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct Status
    {
        public int State;
        public int Link;
        public int Width;
        public int Height;
        public int Fps;
        public int Kbps;
        public int Rotation;
        public int PhoneRotation;
        public int PhoneFacing;
        public int PhoneTargetHeight;
        public int CameraInUse;
        public int Vcam;
        public int VcamError;
        public int PhoneLens;
        public int LensCount;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Peer;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Message;
        public int PairingCode; // 0..999999 while State == StatePairing, else -1
        public int Problem;
        public int PairingStage; // PairingNone / PairingConfirm / PairingWaitPhone / PairingWaitUser
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct PairedPhone
    {
        public long PairedAt; // Unix seconds
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 72)] public string Id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Name;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct Lens
    {
        public int Front;
        public float Zoom;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Label;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct Device
    {
        public int Link;
        public int Port;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string Host;
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int mc_init();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void mc_shutdown();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int mc_vcam_state();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int mc_vcam_install();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern int mc_vcam_start();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void mc_vcam_stop();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern void mc_connect(int link, string host, int port);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void mc_disconnect();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void mc_get_status(out Status status);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int mc_discover([Out] Device[] devices, int maxDevices, int timeoutMs);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void mc_set_rotation(int degrees);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void mc_set_fill(int fill);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int mc_get_preview(byte* bgra, int width, int height, int stride, ref long seq);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int mc_get_lenses([Out] Lens[] lenses, int maxLenses);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern int mc_phone_command(string command);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void mc_confirm_pairing(int accept);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int mc_get_paired_phones([Out] PairedPhone[] phones, int maxPhones);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern int mc_forget_phone(string id);
}
