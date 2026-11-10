param(
  [string]$Executable = (Join-Path $PSScriptRoot '..\build\windows-x64\Release\TinyVolumeMixer.exe'),
  [string]$ArtifactsPath = (Join-Path $PSScriptRoot '..\build\desktop-check'),
  [switch]$CaptureScreenshots
)
# Runs against this app only. Restores its saved preferences and named startup value.
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if ($CaptureScreenshots) { New-Item -ItemType Directory -Force $ArtifactsPath | Out-Null }
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TVMNative {
  [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
  public delegate bool EnumProc(IntPtr h,IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h,EnumProc callback,IntPtr l);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls,string title);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostThreadMessage(uint t,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string name);
  [DllImport("user32.dll")] public static extern bool RegisterHotKey(IntPtr h,int id,uint modifiers,uint key);
  [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr h,int id);
  [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process,uint flag);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr context);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
}
'@
[TVMNative]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$regPath = 'HKCU:\Software\TinyVolumeMixer'
$existed = Test-Path $regPath
$original = @{}
$runPath='HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runKey=Get-Item $runPath
$hadRun=$runKey.GetValueNames() -contains 'TinyVolumeMixer'
$oldRun=$runKey.GetValue('TinyVolumeMixer')
$oldRunKind=if($hadRun){$runKey.GetValueKind('TinyVolumeMixer')}else{'String'}
if ($existed) {
  $key = Get-Item $regPath
  foreach ($name in $key.GetValueNames()) { $original[$name] = @($key.GetValue($name),$key.GetValueKind($name)) }
}
$p = $null
$producer = $null
$collision = $false
$localTest = Join-Path $env:TEMP ('tvm-check-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $localTest | Out-Null
Copy-Item -LiteralPath $Executable -Destination $localTest
function Check($condition,$label) { if (!$condition) { throw "FAIL: $label" }; Write-Output "PASS: $label" }
function Caption($handle) {
  $s = New-Object System.Text.StringBuilder 1024
  [TVMNative]::GetWindowText($handle,$s,1024) | Out-Null
  return $s.ToString()
}
function Background($handle) {
  $r=New-Object TVMNative+Rect
  [TVMNative]::GetWindowRect($handle,[ref]$r)|Out-Null
  $bitmap=New-Object System.Drawing.Bitmap ($r.Right-$r.Left),($r.Bottom-$r.Top)
  $graphics=[System.Drawing.Graphics]::FromImage($bitmap);$dc=$graphics.GetHdc()
  [TVMNative]::PrintWindow($handle,$dc,2)|Out-Null
  $graphics.ReleaseHdc($dc);$color=$bitmap.GetPixel(10,45)
  $graphics.Dispose();$bitmap.Dispose();return $color.R
}
function FindRow($handle,$name) {
  $script:foundRow=[IntPtr]::Zero
  [TVMNative]::EnumChildWindows($handle,{param($child,$unused)
    $cls=New-Object System.Text.StringBuilder 128
    [TVMNative]::GetClassName($child,$cls,128)|Out-Null
    if($cls.ToString() -eq 'TinyVolumeMixer.Row' -and (Caption $child) -eq $name){$script:foundRow=$child}
    return $true
  },[IntPtr]::Zero)|Out-Null
  return $script:foundRow
}
function ThumbCenter($slider,$vertical) {
  $rect=New-Object TVMNative+Rect
  [TVMNative]::GetWindowRect($slider,[ref]$rect)|Out-Null
  $width=$rect.Right-$rect.Left;$height=$rect.Bottom-$rect.Top
  $bitmap=New-Object System.Drawing.Bitmap $width,$height
  $graphics=[System.Drawing.Graphics]::FromImage($bitmap);$dc=$graphics.GetHdc()
  try {
    [TVMNative]::PrintWindow($slider,$dc,2)|Out-Null
    $graphics.ReleaseHdc($dc);$dc=[IntPtr]::Zero
    # Scan the painted white thumb in the dark theme, excluding the focus border.
    $sum=0;$count=0;$length=if($vertical){$height}else{$width}
    for($pixel=2;$pixel -lt $length-2;$pixel++){
      $color=if($vertical){$bitmap.GetPixel([int]($width/2),$pixel)}else{$bitmap.GetPixel($pixel,[int]($height/2))}
      if($color.R -gt 220 -and $color.G -gt 220 -and $color.B -gt 220){$sum+=$pixel;$count++}
    }
    if($count -eq 0){throw 'No painted slider thumb found'}
    return $sum/$count
  }finally{
    if($dc -ne [IntPtr]::Zero){$graphics.ReleaseHdc($dc)}
    $graphics.Dispose();$bitmap.Dispose()
  }
}
function Shot($handle,$name) {
  if (!$CaptureScreenshots) { return }
  [TVMNative]::SetForegroundWindow($handle) | Out-Null
  Start-Sleep -Milliseconds 400
  $r = New-Object TVMNative+Rect
  [TVMNative]::GetWindowRect($handle,[ref]$r) | Out-Null
  $bitmap = New-Object System.Drawing.Bitmap ($r.Right-$r.Left),($r.Bottom-$r.Top)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $dc = $graphics.GetHdc()
  [TVMNative]::PrintWindow($handle,$dc,2) | Out-Null
  $graphics.ReleaseHdc($dc)
  $bitmap.Save((Join-Path $ArtifactsPath $name))
  $graphics.Dispose(); $bitmap.Dispose()
}
try {
  Check ([TVMNative]::FindWindow('TinyVolumeMixer.Popup','TinyVolumeMixer') -eq [IntPtr]::Zero) 'no prior test instance'
  $p = Start-Process -FilePath (Join-Path $localTest 'TinyVolumeMixer.exe') -WorkingDirectory $localTest -PassThru
  $p.WaitForInputIdle(10000) | Out-Null
  Start-Sleep -Seconds 2
  $h = [TVMNative]::FindWindow('TinyVolumeMixer.Popup','TinyVolumeMixer')
  Check ($h -ne [IntPtr]::Zero) 'native popup was created'
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]101,[IntPtr]::Zero) | Out-Null
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]101,[IntPtr]::Zero) | Out-Null
  # Normalize to horizontal solely for this capture, then restore the saved key in finally.
  if ((Get-ItemProperty $regPath).Vertical -ne 0) {
    [TVMNative]::SendMessage($h,0x0111,[IntPtr]101,[IntPtr]::Zero) | Out-Null
  }
  $show = [TVMNative]::RegisterWindowMessage('TinyVolumeMixer.Show.v1')
  [TVMNative]::SendMessage($h,$show,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]302,[IntPtr]::Zero) | Out-Null
  Check ((Get-ItemProperty $regPath).Theme -eq 1 -and (Background $h) -gt 220) 'light theme is painted and persisted'
  Shot $h 'light-horizontal.png'
  [TVMNative]::SendMessage($h,0x001A,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  Check ((Background $h) -gt 220) 'explicit light theme survives Windows setting notifications'
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]303,[IntPtr]::Zero) | Out-Null
  Check ((Get-ItemProperty $regPath).Theme -eq 2 -and (Background $h) -lt 64) 'dark theme is painted and persisted'
  Shot $h 'horizontal.png'
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]301,[IntPtr]::Zero) | Out-Null
  $personalize='HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
  $appsLight=(Get-ItemProperty $personalize -ErrorAction SilentlyContinue).AppsUseLightTheme
  Check (((Background $h) -gt 220) -eq ($null -eq $appsLight -or $appsLight -ne 0)) 'automatic theme follows the current Windows app preference'
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]303,[IntPtr]::Zero) | Out-Null
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]101,[IntPtr]::Zero) | Out-Null
  Check ((Get-ItemProperty $regPath).Vertical -eq 1) 'vertical orientation persists'
  Shot $h 'vertical.png'
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]302,[IntPtr]::Zero) | Out-Null
  Shot $h 'light-vertical.png'
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]303,[IntPtr]::Zero) | Out-Null
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]102,[IntPtr]::Zero) | Out-Null
  $hotkey = [TVMNative]::GetDlgItem($h,107)
  Check ($hotkey -ne [IntPtr]::Zero) 'settings contains native hotkey editor'
  [TVMNative]::SendMessage($hotkey,0x0401,[IntPtr](0x0600 -bor [int][char]'K'),[IntPtr]::Zero) | Out-Null
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]105,[IntPtr]::Zero) | Out-Null
  Check ((Get-ItemProperty $regPath).HotkeyKey -eq [int][char]'K') 'hotkey changes are saved'
  $startup=[TVMNative]::GetDlgItem($h,106)
  [TVMNative]::SendMessage($startup,0x00F1,[IntPtr]1,[IntPtr]::Zero) | Out-Null
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]105,[IntPtr]::Zero) | Out-Null
  $expected='"'+(Join-Path $localTest 'TinyVolumeMixer.exe')+'" --background'
  Check ((Get-ItemPropertyValue $runPath -Name TinyVolumeMixer) -eq $expected) 'startup stores quoted executable path with background flag'
  [TVMNative]::SendMessage($startup,0x00F1,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]105,[IntPtr]::Zero) | Out-Null
  Check (!((Get-Item $runPath).GetValueNames() -contains 'TinyVolumeMixer')) 'startup can be disabled'
  $collision = [TVMNative]::RegisterHotKey([IntPtr]::Zero,997,3,[int][char]'J')
  Check $collision 'test reserves a conflicting shortcut'
  [TVMNative]::SendMessage($hotkey,0x0401,[IntPtr](0x0600 -bor [int][char]'J'),[IntPtr]::Zero) | Out-Null
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]105,[IntPtr]::Zero) | Out-Null
  Check ((Get-ItemProperty $regPath).HotkeyKey -eq [int][char]'K') 'collision retains previous saved hotkey'
  Check ((Caption ([TVMNative]::GetDlgItem($h,108))) -match 'em uso') 'collision has visible feedback'
  Shot $h 'settings.png'
  [TVMNative]::SendMessage($h,0x0111,[IntPtr]102,[IntPtr]::Zero) | Out-Null
  [TVMNative]::SendMessage($h,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  Check (![TVMNative]::IsWindowVisible($h)) 'close hides instead of exiting'
  $p.Refresh(); Check (!$p.HasExited) 'tray process remains alive'
  [TVMNative]::SendMessage($h,0x0312,[IntPtr]1,[IntPtr]::Zero) | Out-Null
  Check ([TVMNative]::IsWindowVisible($h)) 'shortcut handler opens hidden popup'
  [TVMNative]::SendMessage($h,0x0006,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  Check (![TVMNative]::IsWindowVisible($h)) 'focus loss hides the popup'
  $audioProducer=Join-Path (Split-Path $Executable -Parent) 'tvm_audio_smoke.exe'
  if(Test-Path $audioProducer){
    Copy-Item $audioProducer $localTest
    $producer=New-Object System.Diagnostics.Process
    $producer.StartInfo.FileName=Join-Path $localTest 'tvm_audio_smoke.exe'
    $producer.StartInfo.Arguments='--producer'
    $producer.StartInfo.UseShellExecute=$false
    $producer.StartInfo.RedirectStandardInput=$true
    $producer.StartInfo.CreateNoWindow=$true
    $producer.Start()|Out-Null
    [TVMNative]::SendMessage($h,$show,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
    $row=[IntPtr]::Zero
    for($i=0;$i -lt 100 -and $row -eq [IntPtr]::Zero;$i++){Start-Sleep -Milliseconds 100;$row=FindRow $h 'tvm_audio_smoke'}
    Check ($row -ne [IntPtr]::Zero) 'late audio sessions create a real application row'
    $slider=[TVMNative]::GetDlgItem($row,2);$mute=[TVMNative]::GetDlgItem($row,1)
    $vertical=(Get-ItemProperty $regPath).Vertical -eq 1
    $masterRow=FindRow $h 'Volume geral'
    $masterSlider=[TVMNative]::GetDlgItem($masterRow,2)
    $masterPosition=[int][TVMNative]::SendMessage($masterSlider,0x0400,[IntPtr]::Zero,[IntPtr]::Zero)
    $masterPercent=if($vertical){100-$masterPosition}else{$masterPosition}
    if($masterPercent -gt 2){
      # Inspect the painted thumb position, not only TBM_GETPOS.
      # External changes exercise the same follower update used by master changes.
      foreach($orientation in @($true,$false)){
        if($vertical -ne $orientation){
          [TVMNative]::SendMessage($h,0x0111,[IntPtr]101,[IntPtr]::Zero)|Out-Null
          $vertical=$orientation
        }
        [TVMNative]::SendMessage($h,$show,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
        $row=FindRow $h 'tvm_audio_smoke'
        $slider=[TVMNative]::GetDlgItem($row,2)
        $zeroPosition=if($vertical){100}else{0}
        [TVMNative]::SendMessage($slider,0x0405,[IntPtr]1,[IntPtr]$zeroPosition)|Out-Null
        [TVMNative]::SendMessage($row,0x0114,[IntPtr]8,$slider)|Out-Null
        Start-Sleep -Milliseconds 300
        Check ([TVMNative]::IsWindowVisible($h)) 'popup remains open before the external change'
        $before=ThumbCenter $slider $vertical
        $producer.StandardInput.Write('v');$producer.StandardInput.Flush()
        $expected=[int][Math]::Round(0.67*$masterPercent)
        $moved=$false
        for($attempt=0;$attempt -lt 100;$attempt++){
          Start-Sleep -Milliseconds 50
          $logical=[int][TVMNative]::SendMessage($slider,0x0400,[IntPtr]::Zero,[IntPtr]::Zero)
          if($vertical){$logical=100-$logical}
          $center=ThumbCenter $slider $vertical
          $direction=if($vertical){$before-$center}else{$center-$before}
          if([Math]::Abs($logical-$expected) -le 1 -and $direction -gt 0){$moved=$true;break}
        }
        $axis=if($vertical){'vertical'}else{'horizontal'}
        Check $moved "external volume moves the $axis thumb while the popup remains open (logical=$logical expected=$expected center=$before->$center visible=$([TVMNative]::IsWindowVisible($h)))"
      }
    }
    $row=FindRow $h 'tvm_audio_smoke'
    $slider=[TVMNative]::GetDlgItem($row,2);$mute=[TVMNative]::GetDlgItem($row,1)
    for($i=1;$i -le 80;$i++){
      # Stay below the output so this painting check never raises the master.
      $level=[int][Math]::Floor($i*$masterPercent/100)
      $position=if($vertical){100-$level}else{$level}
      [TVMNative]::SendMessage($slider,0x0405,[IntPtr]1,[IntPtr]$position)|Out-Null
      [TVMNative]::SendMessage($row,0x0114,[IntPtr]5,$slider)|Out-Null
      Start-Sleep -Milliseconds 2
    }
    [TVMNative]::SendMessage($row,0x0114,[IntPtr]8,$slider)|Out-Null
    Start-Sleep -Milliseconds 300
    Check ((FindRow $h 'tvm_audio_smoke') -eq $row -and [TVMNative]::GetDlgItem($row,1) -eq $mute) '80 real volume updates preserve row, label, and icon controls'
    Check ([TVMNative]::SendMessage($slider,0x0400,[IntPtr]::Zero,[IntPtr]::Zero) -eq $position) 'slider settles at the last requested value without stale readback'
    $producer.StandardInput.Write('q');$producer.StandardInput.Flush()
    Check ($producer.WaitForExit(10000)) 'silent producer exits cleanly'
    $producer.Dispose();$producer=$null
    for($i=0;$i -lt 100 -and (FindRow $h 'tvm_audio_smoke') -ne [IntPtr]::Zero;$i++){Start-Sleep -Milliseconds 100}
    Check ((FindRow $h 'tvm_audio_smoke') -eq [IntPtr]::Zero) 'application row is removed after its producer exits'
    [TVMNative]::SendMessage($h,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
  }
  $p.Refresh()
  $gdi0=[TVMNative]::GetGuiResources($p.Handle,0); $user0=[TVMNative]::GetGuiResources($p.Handle,1)
  for ($i=0;$i -lt 40;$i++) {
    [TVMNative]::SendMessage($h,$show,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
    [TVMNative]::SendMessage($h,0x0111,[IntPtr]101,[IntPtr]::Zero) | Out-Null
    [TVMNative]::SendMessage($h,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  }
  $gdi1=[TVMNative]::GetGuiResources($p.Handle,0); $user1=[TVMNative]::GetGuiResources($p.Handle,1)
  Check ($gdi1 -le $gdi0+4 -and $user1 -le $user0+4) "40 orientation/open cycles settle handles (GDI $gdi0->$gdi1; USER $user0->$user1)"
  $p.Refresh()
  Write-Output "MEASURE: working_set=$($p.WorkingSet64) private_bytes=$($p.PrivateMemorySize64) threads=$($p.Threads.Count)"
} finally {
  if($producer -and !$producer.HasExited){$producer.StandardInput.Write('q');$producer.StandardInput.Flush();if(!$producer.WaitForExit(10000)){$producer.Kill()}}
  if ($collision) { [TVMNative]::UnregisterHotKey([IntPtr]::Zero,997) | Out-Null }
  if ($p -and !$p.HasExited) {
    $h=[TVMNative]::FindWindow('TinyVolumeMixer.Popup','TinyVolumeMixer')
    [uint32]$processIdForWindow=0
    $thread=[TVMNative]::GetWindowThreadProcessId($h,[ref]$processIdForWindow)
    [TVMNative]::PostThreadMessage($thread,0x0012,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
    if (!$p.WaitForExit(10000)) { $p.Kill(); throw 'shutdown did not complete' }
    Write-Output "MEASURE: exit_code=$($p.ExitCode)"
  }
  if (Test-Path $regPath) { Remove-Item $regPath -Recurse }
  if($hadRun) { New-ItemProperty $runPath -Name TinyVolumeMixer -Value $oldRun -PropertyType $oldRunKind -Force | Out-Null }
  else { Remove-ItemProperty $runPath -Name TinyVolumeMixer -ErrorAction SilentlyContinue }
  if ($existed) {
    New-Item $regPath -Force | Out-Null
    foreach ($name in $original.Keys) {
      New-ItemProperty $regPath -Name $name -Value $original[$name][0] -PropertyType $original[$name][1] | Out-Null
    }
  }
  Remove-Item $localTest -Recurse
}
