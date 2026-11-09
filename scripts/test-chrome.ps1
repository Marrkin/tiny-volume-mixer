param(
  [string]$Executable = (Join-Path $PSScriptRoot '..\build\windows-x64\Release\TinyVolumeMixer.exe'),
  [string]$Chrome = "$env:ProgramFiles\Google\Chrome\Application\chrome.exe"
)
# A silent, local Web Audio page in a disposable Chrome profile. No volumes are
# written and the user's normal Chrome windows/profile are left alone.
$ErrorActionPreference='Stop'
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public class TVMChrome {
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls,string name);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h,EnumProc callback,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string name);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PostThreadMessage(uint t,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
}
'@
function HasChromeRow($handle) {
 $script:chromeFound=$false
 [TVMChrome]::EnumChildWindows($handle,{param($child,$unused)
  $name=New-Object Text.StringBuilder 256;$cls=New-Object Text.StringBuilder 128
  [TVMChrome]::GetWindowText($child,$name,256)|Out-Null
  [TVMChrome]::GetClassName($child,$cls,128)|Out-Null
  if($cls.ToString() -eq 'TinyVolumeMixer.Row' -and $name.ToString() -match 'Chrome'){$script:chromeFound=$true}
  return $true
 },[IntPtr]::Zero)|Out-Null
 return $script:chromeFound
}
function StopMixer($process) {
 if($process -and !$process.HasExited){
  $handle=[TVMChrome]::FindWindow('TinyVolumeMixer.Popup','TinyVolumeMixer');[uint32]$owner=0
  $thread=[TVMChrome]::GetWindowThreadProcessId($handle,[ref]$owner)
  [TVMChrome]::PostThreadMessage($thread,0x0012,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
  if(!$process.WaitForExit(10000)){$process.Kill();throw 'Mixer did not exit'}
 }
}
function AwaitChrome($process) {
 $handle=[TVMChrome]::FindWindow('TinyVolumeMixer.Popup','TinyVolumeMixer')
 [TVMChrome]::PostMessage($handle,[TVMChrome]::RegisterWindowMessage('TinyVolumeMixer.Show.v1'),[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
 for($i=0;$i -lt 150;$i++){
  if(HasChromeRow $handle){return}
  if($process.HasExited){throw 'Mixer exited unexpectedly'}
  Start-Sleep -Milliseconds 100
 }
 throw 'FAIL: Google Chrome audio session was not displayed'
}
if(!(Test-Path -LiteralPath $Chrome)){throw "Chrome executable not found: $Chrome"}
if([TVMChrome]::FindWindow('TinyVolumeMixer.Popup','TinyVolumeMixer') -ne [IntPtr]::Zero){throw 'Close the existing mixer before this check'}
$local=Join-Path $env:TEMP ('tvm-chrome-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $local|Out-Null
Copy-Item -LiteralPath $Executable -Destination $local
$html=Join-Path $local 'silent.html'
@'
<!doctype html><meta charset="utf-8"><title>TinyVolumeMixer - teste silencioso</title>
<p>Teste local de descoberta de audio. Nenhum som sera emitido.</p>
<script>
const context = new AudioContext();
const oscillator = context.createOscillator();
const gain = context.createGain();
gain.gain.value = 0;
oscillator.connect(gain).connect(context.destination);
oscillator.start(); context.resume();
</script>
'@ | Set-Content -LiteralPath $html -Encoding UTF8
$mixer=$null;$browser=$null
try {
 $mixer=Start-Process -FilePath (Join-Path $local 'TinyVolumeMixer.exe') -ArgumentList '--background' -WorkingDirectory $local -PassThru
 $mixer.WaitForInputIdle(10000)|Out-Null
 Start-Sleep -Milliseconds 1000
 $profile=Join-Path $local 'profile'
 $page=([Uri]$html).AbsoluteUri
 $arguments='--user-data-dir="'+$profile+'" --no-first-run --no-default-browser-check --disable-background-networking --disable-sync --disable-extensions --disable-component-update --autoplay-policy=no-user-gesture-required --window-size=480,240 --app="'+$page+'"'
 $browser=Start-Process -FilePath $Chrome -ArgumentList $arguments -PassThru
 AwaitChrome $mixer
 Write-Output 'PASS: real Chrome audio created after mixer launch is displayed'
 StopMixer $mixer;$mixer=$null
 $mixer=Start-Process -FilePath (Join-Path $local 'TinyVolumeMixer.exe') -WorkingDirectory $local -PassThru
 $mixer.WaitForInputIdle(10000)|Out-Null
 AwaitChrome $mixer
 Write-Output 'PASS: an existing real Chrome audio session is discovered at mixer startup'
} finally {
 StopMixer $mixer
 if($browser -and !$browser.HasExited){
  $browser.CloseMainWindow()|Out-Null
  if(!$browser.WaitForExit(5000)){$browser.Kill();$browser.WaitForExit()}
 }
 # Chrome can briefly retain a profile lock after its main window exits.
 for($i=0;$i -lt 20;$i++){
  try {Remove-Item -LiteralPath $local -Recurse -Force -ErrorAction Stop;break}
  catch {if($i -eq 19){throw};Start-Sleep -Milliseconds 250}
 }
}
