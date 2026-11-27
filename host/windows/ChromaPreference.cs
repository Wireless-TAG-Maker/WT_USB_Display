using System;
using System.IO;
using System.Security;
using System.Threading;
using Microsoft.Win32;

static class ChromaPreference
{
    internal const string SettingsKey = @"HKEY_CURRENT_USER\Software\WTUSBDisplay";
    internal const string FollowApp = "WT_USB_DISPLAY_APP_SETTINGS";
    const string ChangedEvent = @"Local\WTUSBDisplay.ChromaChanged";

    internal static EventWaitHandle OpenSignal()
    {
        return new EventWaitHandle(false, EventResetMode.AutoReset, ChangedEvent);
    }

    internal static bool ReadFull()
    {
        try { return (Registry.GetValue(SettingsKey, "ChromaSampling", null) as string) == "444"; }
        catch (SecurityException) { }
        catch (UnauthorizedAccessException) { }
        catch (IOException) { }
        return false;
    }

    internal static void Save(bool full)
    {
        using (var signal = OpenSignal())
        {
            Registry.SetValue(SettingsKey, "ChromaSampling", full ? "444" : "420");
            signal.Set();
        }
    }
}
