# Takes a playback device out of Windows and puts it back (what the Sound control panel's
# "Disable" does; no administrator needed), and measures what is being played on one. For
# testing how the emulator copes with a sound device that goes away while it plays.
#   audio-endpoint.ps1 list
#   audio-endpoint.ps1 hide <name>
#   audio-endpoint.ps1 show <name>
#   audio-endpoint.ps1 peak <name> [seconds]     highest level played in that time, 0..1
#   audio-endpoint.ps1 default                   the name of the device Windows plays on
#   audio-endpoint.ps1 default <name>            makes that one the device Windows plays on
#                                                (what a program that streams to a headset
#                                                does when the headset connects)
#   audio-endpoint.ps1 preferred                 the device Windows would play on if every
#                                                device were there
# Mind what "default <name>" leaves behind: Windows remembers the order in which devices were
# chosen, and plays on the one chosen last of those that are there. A device that is not
# there just now (a headset's, while nothing streams to it) is behind the one chosen with
# this from then on, and Windows no longer changes over to it when it comes back. Choose it
# again afterwards, there or not: tools/pc-audio-default-test.sh does.
param([Parameter(Mandatory = $true)][string]$Action, [string]$Name, [double]$Seconds = 2)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace AudioEndpoint {
[StructLayout(LayoutKind.Sequential)]
public struct PropertyKey { public Guid fmtid; public int pid; }

[StructLayout(LayoutKind.Explicit)]
public struct PropVariant {
    [FieldOffset(0)] public ushort vt;
    [FieldOffset(8)] public IntPtr pointer;
    [FieldOffset(16)] public IntPtr filler;
}

[ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPropertyStore {
    int GetCount(out int count);
    int GetAt(int index, out PropertyKey key);
    int GetValue(ref PropertyKey key, out PropVariant value);
}

[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice {
    int Activate(ref Guid iid, int context, IntPtr parameters,
                 [MarshalAs(UnmanagedType.IUnknown)] out object result);
    int OpenPropertyStore(int access, out IPropertyStore properties);
    int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    int GetState(out int state);
}

[ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceCollection {
    int GetCount(out int count);
    int Item(int index, out IMMDevice device);
}

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator {
    int EnumAudioEndpoints(int flow, int states, out IMMDeviceCollection devices);
    [PreserveSig]
    int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice device);
}

[ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioMeterInformation {
    int GetPeakValue(out float peak);
}

[ComImport, Guid("f8679f50-850a-41cf-9c72-430f290290c8"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPolicyConfig {
    int GetMixFormat();
    int GetDeviceFormat();
    int ResetDeviceFormat();
    int SetDeviceFormat();
    int GetProcessingPeriod();
    int SetProcessingPeriod();
    int GetShareMode();
    int SetShareMode();
    int GetPropertyValue();
    int SetPropertyValue();
    [PreserveSig]
    int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string id, int role);
    [PreserveSig]
    int SetEndpointVisibility([MarshalAs(UnmanagedType.LPWStr)] string id, int visible);
}

public class Device {
    public string Id;
    public string Name;
    public int State;
}

public static class Endpoints {
    static readonly Guid EnumeratorClass = new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E");
    static readonly Guid PolicyClass = new Guid("870af99c-171d-4f9e-af0d-e63df40c2bc9");

    static IMMDeviceEnumerator Enumerator() {
        return (IMMDeviceEnumerator)Activator.CreateInstance(
            Type.GetTypeFromCLSID(EnumeratorClass));
    }

    static IMMDevice Find(string name, out Device found) {
        IMMDeviceCollection devices;
        // Playback devices in any state but "not present".
        Marshal.ThrowExceptionForHR(Enumerator().EnumAudioEndpoints(0, 0x3, out devices));
        int count;
        devices.GetCount(out count);
        found = null;
        for (int i = 0; i < count; ++i) {
            IMMDevice device;
            devices.Item(i, out device);
            Device described = Describe(device);
            if (name == null || described.Name == name) {
                found = described;
                if (name != null) {
                    return device;
                }
            }
        }
        return null;
    }

    static Device Describe(IMMDevice device) {
        Device described = new Device();
        device.GetId(out described.Id);
        device.GetState(out described.State);
        IPropertyStore properties;
        if (device.OpenPropertyStore(0, out properties) == 0) {
            PropertyKey key = new PropertyKey {
                fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 14 };
            PropVariant value;
            if (properties.GetValue(ref key, out value) == 0 && value.vt == 31) {
                described.Name = Marshal.PtrToStringUni(value.pointer);
            }
        }
        return described;
    }

    public static List<Device> List() {
        List<Device> all = new List<Device>();
        IMMDeviceCollection devices;
        Marshal.ThrowExceptionForHR(Enumerator().EnumAudioEndpoints(0, 0x3, out devices));
        int count;
        devices.GetCount(out count);
        for (int i = 0; i < count; ++i) {
            IMMDevice device;
            devices.Item(i, out device);
            all.Add(Describe(device));
        }
        return all;
    }

    public static void SetVisible(string name, bool visible) {
        Device found;
        if (Find(name, out found) == null) {
            throw new Exception("no playback device named " + name);
        }
        IPolicyConfig policy =
            (IPolicyConfig)Activator.CreateInstance(Type.GetTypeFromCLSID(PolicyClass));
        Marshal.ThrowExceptionForHR(policy.SetEndpointVisibility(found.Id, visible ? 1 : 0));
    }

    public static string Default() {
        IMMDevice device;
        if (Enumerator().GetDefaultAudioEndpoint(0, 0, out device) != 0 || device == null) {
            return "";
        }
        return Describe(device).Name;
    }

    public static void SetDefault(string name) {
        Device found;
        if (Find(name, out found) == null) {
            throw new Exception("no playback device named " + name);
        }
        IPolicyConfig policy =
            (IPolicyConfig)Activator.CreateInstance(Type.GetTypeFromCLSID(PolicyClass));
        // For everything it can be the default of: console, multimedia, communications.
        for (int role = 0; role < 3; ++role) {
            Marshal.ThrowExceptionForHR(policy.SetDefaultEndpoint(found.Id, role));
        }
    }

    public static float Peak(string name, double seconds) {
        Device found;
        IMMDevice device = Find(name, out found);
        if (device == null) {
            throw new Exception("no playback device named " + name);
        }
        Guid iid = typeof(IAudioMeterInformation).GUID;
        object meter_object;
        Marshal.ThrowExceptionForHR(device.Activate(ref iid, 23, IntPtr.Zero, out meter_object));
        IAudioMeterInformation meter = (IAudioMeterInformation)meter_object;
        float highest = 0;
        DateTime end = DateTime.UtcNow.AddSeconds(seconds);
        while (DateTime.UtcNow < end) {
            float peak;
            meter.GetPeakValue(out peak);
            highest = Math.Max(highest, peak);
            System.Threading.Thread.Sleep(20);
        }
        return highest;
    }
}
}
'@

switch ($Action) {
    'list' {
        [AudioEndpoint.Endpoints]::List() | ForEach-Object {
            '{0}  {1}' -f $(if ($_.State -eq 1) { 'active  ' } else { 'disabled' }), $_.Name
        }
    }
    'hide' { [AudioEndpoint.Endpoints]::SetVisible($Name, $false) }
    'show' { [AudioEndpoint.Endpoints]::SetVisible($Name, $true) }
    'peak' { '{0:0.0000}' -f [AudioEndpoint.Endpoints]::Peak($Name, $Seconds) }
    'default' {
        if ($Name) { [AudioEndpoint.Endpoints]::SetDefault($Name) }
        [AudioEndpoint.Endpoints]::Default()
    }
    'preferred' {
        # Windows counts the choices: the highest count among the devices that are plugged
        # in (in use or taken out of the list) is the one it would play on.
        $base = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
        $best = $null
        foreach ($key in Get-ChildItem $base -ErrorAction SilentlyContinue) {
            $item = Get-Item $key.PSPath
            $state = $item.GetValue("DeviceState")
            $level = $item.GetValue("Level:0")
            if ($null -eq $level -or ($state -band 0xF) -ne 1) { continue }
            if ($null -eq $best -or $level -gt $best.Level) {
                $properties = Get-ItemProperty (Join-Path $key.PSPath "Properties") -ErrorAction SilentlyContinue
                $best = [pscustomobject]@{
                    Level = $level
                    Name = ('{0} ({1})' -f $properties.'{a45c254e-df1c-4efd-8020-67d146a850e0},2',
                            $properties.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6')
                }
            }
        }
        if ($best) { $best.Name }
    }
    default { throw "unknown action $Action" }
}
