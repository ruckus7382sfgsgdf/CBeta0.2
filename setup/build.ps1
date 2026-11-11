# Builds TriforceSetup.exe: .NET Framework 4.8 (in every Windows 10/11), one file, no installers needed to run it.
#   powershell -File setup\build.ps1 [-Version v1.0.0]      -> setup\out\TriforceSetup.exe
# Needs the C# compiler from Visual Studio (Build Tools) for modern C#; the window (Main.xaml) and its pictures are embedded.
#   -Out <folder> puts the exe (and the generated pictures) somewhere else, e.g. a scratch folder for previews.
param([string]$Version = 'dev', [string]$Out = '')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path $here
$out = if ($Out) { $Out } else { Join-Path $here 'out' }
$gen = Join-Path $out 'gen'
New-Item -ItemType Directory -Force $gen | Out-Null

$csc = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\*\*\MSBuild\Current\Bin\Roslyn\csc.exe' -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $csc) { throw 'No C# compiler found (Visual Studio Build Tools: MSBuild\Current\Bin\Roslyn\csc.exe)' }

# ---- pictures: the crest, the exe's icon ----
Add-Type -AssemblyName System.Drawing
function Resize([System.Drawing.Image]$src, [int]$w) {
    $h = [int][math]::Round($src.Height * $w / $src.Width)
    $b = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.CompositingMode = 'SourceCopy'
    $ia = New-Object System.Drawing.Imaging.ImageAttributes; $ia.SetWrapMode('TileFlipXY')
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $w, $h), 0, 0, $src.Width, $src.Height, 'Pixel', $ia)
    $g.Dispose(); return $b
}
function Square([System.Drawing.Image]$src, [int]$s) {
    # (the crest centred in a square, for the icon)
    $b = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $scale = [math]::Min($s / $src.Width, $s / $src.Height)
    $w = $src.Width * $scale; $h = $src.Height * $scale
    $g.DrawImage($src, [single](($s - $w) / 2), [single](($s - $h) / 2), [single]$w, [single]$h)
    $g.Dispose(); return $b
}
$crest = [System.Drawing.Image]::FromFile((Join-Path $repo 'assets\triforce.png'))
# (the crest whole, as painted: Setup shows it large and cuts its stones out of it, Art.cs)
Copy-Item (Join-Path $repo 'assets\triforce.png') (Join-Path $gen 'triforce.png') -Force
(Square $crest 64).Save((Join-Path $gen 'icon.png'), [System.Drawing.Imaging.ImageFormat]::Png)

# icon: PNG entries 16..256
$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$pngs = foreach ($s in $sizes) { $ms = New-Object IO.MemoryStream; (Square $crest $s).Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); , $ms.ToArray() }
$ico = New-Object IO.MemoryStream; $bw = New-Object IO.BinaryWriter $ico
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$d); $bw.Write([byte]$d); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush(); [IO.File]::WriteAllBytes((Join-Path $gen 'triforce.ico'), $ico.ToArray())
$crest.Dispose()

# ---- version + manifest ----
$numeric = if ($Version -match '(\d+)\.(\d+)\.(\d+)') { "$($Matches[1]).$($Matches[2]).$($Matches[3]).0" } else { '0.0.0.0' }
@"
[assembly: System.Reflection.AssemblyTitle("Triforce Setup")]
[assembly: System.Reflection.AssemblyProduct("Triforce")]
[assembly: System.Reflection.AssemblyDescription("Installs, builds and updates Triforce")]
[assembly: System.Reflection.AssemblyCopyright("GPL-3.0-or-later")]
[assembly: System.Reflection.AssemblyVersion("$numeric")]
[assembly: System.Reflection.AssemblyFileVersion("$numeric")]
[assembly: System.Reflection.AssemblyInformationalVersion("$Version")]
"@ | Set-Content (Join-Path $gen 'AssemblyInfo.cs') -Encoding UTF8
@'
<?xml version="1.0" encoding="utf-8"?>
<assembly manifestVersion="1.0" xmlns="urn:schemas-microsoft-com:asm.v1">
  <assemblyIdentity version="1.0.0.0" name="TriforceSetup"/>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v2">
    <security><requestedPrivileges xmlns="urn:schemas-microsoft-com:asm.v3"><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application><supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/></application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
</assembly>
'@ | Set-Content (Join-Path $gen 'app.manifest') -Encoding UTF8

# ---- compile ----
$fw = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319'
$refs = "$fw\WPF\PresentationCore.dll", "$fw\WPF\PresentationFramework.dll", "$fw\WPF\WindowsBase.dll", "$fw\System.Xaml.dll",
        "$fw\System.dll", "$fw\System.Core.dll", "$fw\System.Net.Http.dll", "$fw\System.IO.Compression.dll",
        "$fw\System.IO.Compression.FileSystem.dll", "$fw\Microsoft.CSharp.dll", "$fw\System.Web.Extensions.dll"
$exe = Join-Path $out 'TriforceSetup.exe'
$cscArgs = @('/nologo', '/target:winexe', '/langversion:latest', '/nullable:enable', '/optimize+', '/platform:anycpu', '/utf8output',
             "/out:$exe", "/win32icon:$gen\triforce.ico", "/win32manifest:$gen\app.manifest", '/nowarn:CS8618,CS8632,CS1701,CS1702')
$cscArgs += $refs | ForEach-Object { "/reference:$_" }
$cscArgs += "/resource:$here\Main.xaml,Main.xaml", "/resource:$gen\triforce.png,triforce.png", "/resource:$gen\icon.png,icon.png"
$cscArgs += "$here\Setup.cs", "$here\Shell.cs", "$here\Shell.Home.cs", "$here\Shell.Saves.cs", "$here\Shell.Test.cs", "$here\Stage.cs", "$here\Art.cs", "$here\Look.cs", "$here\Saves.cs", "$here\Strings.cs", "$gen\AssemblyInfo.cs"
& $csc @cscArgs
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
"built $exe ($Version, {0:N0} KB)" -f ((Get-Item $exe).Length / 1KB)
