# ASIOMTAShim

An ASIO pass-through driver that fixes **vMix hanging on shutdown** when using a **TASCAM US-16x08** as an ASIO audio input.

The shim registers as its own ASIO device, loads the real TASCAM driver behind it, and passes every call through unchanged, except that it moves vMix's buffer callbacks off the TASCAM driver's thread. No extra buffering is added, so latency is unaffected.

> **Status:** working (v1.1.0). Tested with vMix 64-bit and a US-16x08 at 48 kHz / 128 samples (TASCAM software 4.00 0064, firmware 2.10 0024): multiple shim inputs work, and vMix shuts down normally.

> [!IMPORTANT]
> In vMix, **everything** that uses the US-16x08 (inputs *and* outputs, including the master, headphones, and bus outputs) must use **US-16x08 (MTA Shim)**. vMix treats the shim and the original driver as two separate devices. If anything still uses "US-16x08 ASIO Driver", that instance holds the hardware, and the shim fails to start with `Could not start ASIO device US-16x08 (MTA Shim), BASS_ERROR_UNKNOWN`.

---

## The problem

With the US-16x08 selected as an ASIO input, closing vMix hangs indefinitely and the process has to be killed. Updating TASCAM's software (4.00 0064) and firmware (2.10 0024) doesn't help; both are the latest releases and haven't been updated in years. Setting the interface to 48 kHz, or making it vMix's audio output device, doesn't help either. vMix support's recommendation was to use a different interface, which isn't practical when the US-16x08 is the only unit with enough front-panel inputs for the rack.

## Root cause

Analysis of a full memory dump of the hung `vMix64.exe` showed a three-thread deadlock, plus a TASCAM driver thread that can never break it:

| Thread | State |
|---|---|
| UI thread (STA) | In `FormClosing` → `Dispose()`, waiting with no timeout for a background dispose job |
| Dispose worker | Blocked in `Monitor.Enter` on a lock held by the thread below |
| Device worker (MTA) | Holding that lock while calling `Marshal.FinalReleaseComObject` on a **Windows Audio Resampler DSP** (`resampledmo.dll`) object; the release must be marshaled to the apartment that owns the object |
| TASCAM streaming thread (STA) | Owns that apartment, but sits permanently in a USB `DeviceIoControl` loop and never pumps messages, so the marshaled release is never serviced |

How it gets there:

1. The TASCAM driver delivers ASIO buffer callbacks on its own streaming thread, which is initialized as a COM single-threaded apartment (STA).
2. During those callbacks, vMix creates a resampler object (apparently for clock-drift correction, since it happens even at a matched 48 kHz). The object becomes bound to the driver thread's STA.
3. At shutdown, vMix releases the object from a different thread while holding a lock. A cross-apartment call into an STA that never pumps messages blocks forever, and everything behind it blocks too.

Most interfaces deliver callbacks on a thread that is multithreaded (MTA) or not COM-initialized, which is why the hang appears with this device and not with others.

## How the shim works

```
vMix (BASS ASIO)  ──►  AsioMtaShim64.dll  ──►  TASCAM us_asio.dll
                         │
                         └─ buffer callbacks handed to a dedicated
                            MTA worker thread (MMCSS "Pro Audio")
```

- All `IASIO` calls (init, start/stop, channel info, sample rate, control panel, and so on) are forwarded directly to the TASCAM driver.
- `createBuffers` substitutes the shim's own callbacks. When the TASCAM driver calls `bufferSwitch` / `bufferSwitchTimeInfo`, the shim signals its worker thread, which runs vMix's callback and returns before the driver continues.
- The same buffers are used and nothing is queued, so there is no added latency, only a thread handoff of microseconds per buffer.
- Anything vMix creates inside its callback now lives in the MTA and can be released from any thread, so the shutdown deadlock can't form.

The shim reads the real driver's CLSID from `HKLM\SOFTWARE\AsioMtaShim\TargetCLSID`, so it isn't tied to a specific machine.

## Requirements

- Windows x64, vMix 64-bit
- TASCAM US-16x08 ASIO driver installed
- To build:
  - Visual Studio 2022 Build Tools with the C++ workload (MSVC)
  - [Inno Setup](https://jrsoftware.org/isinfo.php) 6.3 or later (only for building the installer)

## Building

Install the build tools if needed:

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install JRSoftware.InnoSetup
```

Build the DLL from **x64 Native Tools Command Prompt for VS 2022**:

```
cl /LD /O2 /MT /EHsc AsioMtaShim.cpp /Fe:AsioMtaShim64.dll
```

Build the installer from PowerShell in the repo folder:

```powershell
$iscc = Get-ChildItem "$env:LOCALAPPDATA\Programs","$env:ProgramFiles","${env:ProgramFiles(x86)}" -Filter ISCC.exe -Recurse -Depth 2 -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName; & $iscc .\AsioMtaShimSetup.iss
```

This produces `AsioMtaShimSetup.exe`.

## Installing

Run `AsioMtaShimSetup.exe`, or install silently:

```powershell
.\AsioMtaShimSetup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

The installer:

- copies the DLL to `C:\Program Files\AsioMtaShim`
- detects the installed TASCAM ASIO driver and saves its CLSID as the shim's target (setup stops if the TASCAM driver isn't installed)
- registers a new ASIO device named **US-16x08 (MTA Shim)**; the original TASCAM entry is left untouched
- adds **ASIO MTA Shim for TASCAM US-16x08** to Settings → Apps

## Using with vMix

1. Close vMix, then start it again.
2. In **Settings → Audio Output**, make sure no output (master, headphones, buses) is set to "US-16x08 ASIO Driver". Use another device, or **US-16x08 (MTA Shim)** if you want audio out of the TASCAM.
3. Replace every existing TASCAM ASIO input with **US-16x08 (MTA Shim)**, and redo the channel mapping, since vMix tracks the device by name.
4. Check the audio for clicks or dropouts, then test a shutdown.

## Uninstalling

Use Settings → Apps → **ASIO MTA Shim for TASCAM US-16x08** → Uninstall. Close vMix first, or let the uninstaller close it. Any vMix input still set to the shim will show as missing until it's switched back to the regular TASCAM driver.

## Manual registration (without the installer)

Elevated PowerShell, with the DLL at `C:\AsioShim\AsioMtaShim64.dll`:

```powershell
$c='{6F1D2C84-3A57-4E9B-B0C2-9D41E7A5F318}'; $dll='C:\AsioShim\AsioMtaShim64.dll'; $t=(Get-ChildItem HKLM:\SOFTWARE\ASIO | Where-Object { $_.PSChildName -like '*16x08*' -and $_.PSChildName -notlike '*Shim*' } | Select-Object -First 1 | Get-ItemProperty).CLSID; if(-not $t){throw 'TASCAM ASIO key not found'}; $k="HKLM:\SOFTWARE\Classes\CLSID\$c\InprocServer32"; New-Item $k -Force | Out-Null; Set-Item $k -Value $dll; Set-ItemProperty $k -Name ThreadingModel -Value Apartment; $a='HKLM:\SOFTWARE\ASIO\US-16x08 (MTA Shim)'; New-Item $a -Force | Out-Null; Set-ItemProperty $a -Name CLSID -Value $c; Set-ItemProperty $a -Name Description -Value 'US-16x08 (MTA Shim)'; New-Item HKLM:\SOFTWARE\AsioMtaShim -Force | Out-Null; Set-ItemProperty HKLM:\SOFTWARE\AsioMtaShim -Name TargetCLSID -Value $t; "Shim registered, target $t"
```

To remove a manual registration:

```powershell
Remove-Item 'HKLM:\SOFTWARE\ASIO\US-16x08 (MTA Shim)','HKLM:\SOFTWARE\Classes\CLSID\{6F1D2C84-3A57-4E9B-B0C2-9D41E7A5F318}','HKLM:\SOFTWARE\AsioMtaShim','C:\ProgramData\AsioMtaShim' -Recurse -Force -EA 0
```

## Troubleshooting

**`BASS_ERROR_UNKNOWN` when adding a shim input.** Something in vMix is still using the original "US-16x08 ASIO Driver", usually an audio output. See the note at the top. To check, start vMix with no inputs and run:

```powershell
(Get-Process vMix64).Modules | Where-Object { $_.ModuleName -match 'us_asio|AsioMtaShim' } | Format-List ModuleName, FileName
```

If `us_asio.dll` is listed before you've added any shim input, a vMix setting is using the original driver.

**Logging.** The shim can log every ASIO call (arguments, return codes, the driver's error text, and which thread and COM apartment each call arrives on) to `C:\ProgramData\AsioMtaShim\shim.log`. It's off by default. To turn it on (elevated PowerShell, then restart vMix):

```powershell
New-ItemProperty HKLM:\SOFTWARE\AsioMtaShim -Name EnableLog -PropertyType DWord -Value 1 -Force | Out-Null
```

Set the value to `0` to turn it off again. The log folder is removed on uninstall.

**Troubleshooting switches.** These are REG_DWORD values under `HKLM\SOFTWARE\AsioMtaShim`, read each time vMix opens the device. All default to `0`, which is the working configuration.

| Value | Effect when set to 1 |
|---|---|
| `Passthrough` | Gives the driver vMix's own callbacks, with no worker thread. The shim becomes a pure proxy, so this does **not** fix the hang; it's useful for checking whether a problem is caused by the callback handoff. |
| `FreshLoad` | The shim loads the driver DLL itself (`LoadLibrary`) and unloads it when released, so every open gets a freshly loaded driver. |
| `OwnerThread` | Runs every control call (`init`, `createBuffers`, `start`, ...) on the thread that created the driver, for drivers that care which thread calls them. |

**vMix still hangs on shutdown.** Capture a dump while it's hung (Task Manager → Details → right-click `vMix64.exe` → *Create memory dump file*), open it in WinDbg, and run:

```
!syncblk
!comstate
~*kb
~*e !clrstack
```

If a `FinalReleaseComObject` is still being marshaled into an STA context, the object is being created somewhere other than the buffer callback. Please open an issue with the output and a `shim.log`.

**Emergency shutdown without the shim.** Saving the preset through the vMix API and then killing the process avoids the hang:

```powershell
Invoke-RestMethod "http://127.0.0.1:8088/api/?Function=SavePreset&Value=C:\vMix\LastSession.vmix" | Out-Null; Start-Sleep 3; Stop-Process -Name vMix64 -Force -EA 0
```

## Other interfaces

Nothing in the shim is specific to TASCAM. Another ASIO driver with the same behavior could be wrapped by pointing `HKLM\SOFTWARE\AsioMtaShim\TargetCLSID` at that driver's CLSID (listed under `HKLM\SOFTWARE\ASIO`). The installer currently looks only for the US-16x08.

## Version history

- **1.1.0**: First working release. Logging is opt-in (`EnableLog`); `Passthrough`, `FreshLoad`, and `OwnerThread` troubleshooting switches; owner-thread marshaling available but off by default; the installer removes the log folder on uninstall.
- **1.0.x**: Development builds with diagnostic logging.

## Disclaimer

This is an independent project, not affiliated with or endorsed by StudioCoast (vMix), TEAC/TASCAM, or Steinberg. ASIO is a trademark of Steinberg Media Technologies GmbH. Use at your own risk, and test before using it in a live production.
