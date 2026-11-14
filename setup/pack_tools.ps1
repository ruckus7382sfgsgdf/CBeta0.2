# Packs Triforce-Tools.zip, the release asset Triforce Setup downloads: a "tools" folder with everything a build needs.
#   powershell -File setup\pack_tools.ps1 -Tag v1.0.0 [-Deps D:\src\release\triforce-deps] [-Out setup\out\release]
# From this checkout: nx2, host, gpu, scripts, third_party (tracked files, as they are on disk), the speed profile in work\.
# From -Deps (not in git, too big or binary): python\ (with lz4 numpy capstone unicorn zstandard), cpm\ (Vulkan headers, FFmpeg),
# wgpu-native\, llvm-mingw\ (the compiler; without it a player's PC would need Visual Studio), third_party\ (SDL2.dll, zbic.dll/.lib).
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Deps = 'D:\src\release\triforce-deps',
    [string]$Out = (Join-Path $PSScriptRoot 'out\release'),
    [switch]$AllowNoCompiler,
    [switch]$AllowNoStateList
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
foreach ($need in 'python\python.exe', 'cpm', 'wgpu-native') {
    if (-not (Test-Path (Join-Path $Deps $need))) { throw "missing $need in $Deps" }
}
if (-not (Test-Path (Join-Path $Deps 'llvm-mingw\bin\clang.exe')) -and -not $AllowNoCompiler) {
    throw "no llvm-mingw in ${Deps}: players' builds would need Visual Studio. (-AllowNoCompiler packs anyway, for tests)"
}
# (v0.3.5 shipped without them: no prompts folder was made on any player's PC and the Button prompts setting did nothing)
$pins = Get-Content (Join-Path $repo 'nx2\prompt_requirements.txt') | ForEach-Object { ($_ -split '#')[0].Trim() } | Where-Object { $_ }
foreach ($pin in $pins) {
    $n, $v = $pin -split '=='
    $info = Join-Path $Deps ('python\Lib\site-packages\{0}-{1}.dist-info' -f ($n -replace '-', '_'), $v)
    if (-not (Test-Path $info)) { throw "the tools' Python in $Deps lacks $pin (nx2\prompt_requirements.txt): players would get no button prompts" }
}
$stage = Join-Path $Out 'stage'
$tools = Join-Path $stage 'tools'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $tools | Out-Null

# 1. this checkout's sources (tracked files, working-tree versions)
# (11-13: and assets/triforce.png. nx2/build.py makes the start-up screen's crest from it (nx2/logo_rgba.py), and a source
# tree without the picture builds a screen with the wordmark alone: every pack up to v0.3.5 left the assets folder out, so
# no player's game showed the crest while its shaders were prepared. Reported for AMD cards, where that screen stays longest)
if (-not (Test-Path (Join-Path $repo 'assets\triforce.png') -PathType Leaf)) { throw 'assets\triforce.png is missing: the tools would build a game without the crest on its start-up screen' }
$files = git -C $repo ls-files -- nx2 host gpu scripts third_party LICENSE assets/triforce.png
foreach ($f in $files) {
    $src = Join-Path $repo $f
    if (-not (Test-Path $src -PathType Leaf)) { continue }
    $dst = Join-Path $tools $f
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item $src $dst
}
"sources: $($files.Count) files"

# 2. the dependencies
foreach ($d in 'python', 'cpm', 'wgpu-native', 'llvm-mingw', 'third_party') {
    $src = Join-Path $Deps $d
    if (Test-Path $src) { robocopy $src (Join-Path $tools $d) /E /NFL /NDL /NJH /NJS /NP | Out-Null; "deps: $d" }
}

# 3. the speed profile (hot functions, link order, play profile, chunk list) and the Triforce icon for the game's exe
$work = Join-Path $tools 'work'
New-Item -ItemType Directory -Force $work | Out-Null
foreach ($n in 'hot_funcs.txt', 'hot_order.txt', 'hookshot_pieces.prof') {
    if (Test-Path (Join-Path $repo "work\$n")) { Copy-Item (Join-Path $repo "work\$n") $work }
}
# 3b. the pipeline state list (nx2/pipeline_states.py --out work\pipeline_states.bin <lists from play>): nx2/make_game.py puts it
# beside the game as triforce_pipeline_states_shipped.bin and the first start builds its pipelines before the title. Without it
# every player compiles each pipeline when it is first drawn: the stutter of every first visit (v0.2.0 shipped none: "0 shipped"
# in every log). Hashes and draw state only, no game code.
$states = Join-Path $repo 'work\pipeline_states.bin'
if (Test-Path $states) {
    Copy-Item $states $work
    "work: pipeline state list, {0:N0} pipelines" -f (((Get-Item $states).Length - 12) / 528)
} elseif (-not $AllowNoStateList) {
    throw "no work\pipeline_states.bin: players would compile every pipeline in play. Make it with nx2/pipeline_states.py --out (or -AllowNoStateList)"
}
if (Test-Path (Join-Path $repo 'work\image')) { robocopy (Join-Path $repo 'work\image') (Join-Path $work 'image') /E /NFL /NDL /NJH /NJS /NP | Out-Null }

Add-Type -AssemblyName System.Drawing
$crest = [System.Drawing.Image]::FromFile((Join-Path $repo 'assets\triforce.png'))
$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = foreach ($s in $sizes) {
    $b = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b); $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $k = [math]::Min($s / $crest.Width, $s / $crest.Height); $w = $crest.Width * $k; $h = $crest.Height * $k
    $g.DrawImage($crest, [single](($s - $w) / 2), [single](($s - $h) / 2), [single]$w, [single]$h); $g.Dispose()
    $ms = New-Object IO.MemoryStream; $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); , $ms.ToArray()
}
$crest.Dispose()
# .ico
$ico = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter $ico
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count); $off = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) { $d = $sizes[$i] -band 255; $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$off); $off += $pngs[$i].Length }
foreach ($p in $pngs) { $w.Write($p) }; $w.Flush(); [IO.File]::WriteAllBytes((Join-Path $work 'hookshot.ico'), $ico.ToArray())
# .res (as nx2/make_icon.py: RT_ICON entries + RT_GROUP_ICON 1, which the game's window class loads)
function ResEntry([int]$kind, [int]$name, [byte[]]$data, [int]$flags) {
    $m = New-Object IO.MemoryStream; $x = New-Object IO.BinaryWriter $m
    $x.Write([uint32]$data.Length); $x.Write([uint32]32); $x.Write([uint16]0xFFFF); $x.Write([uint16]$kind); $x.Write([uint16]0xFFFF); $x.Write([uint16]$name)
    $x.Write([uint32]0); $x.Write([uint16]$flags); $x.Write([uint16]0x0409); $x.Write([uint32]0); $x.Write([uint32]0)
    $x.Write($data); $pad = (4 - ($data.Length % 4)) % 4; for ($j = 0; $j -lt $pad; $j++) { $x.Write([byte]0) }
    $x.Flush(); , $m.ToArray()
}
$group = New-Object IO.MemoryStream; $gw = New-Object IO.BinaryWriter $group
$gw.Write([uint16]0); $gw.Write([uint16]1); $gw.Write([uint16]$sizes.Count)
for ($i = 0; $i -lt $sizes.Count; $i++) { $d = $sizes[$i] -band 255; $gw.Write([byte]$d); $gw.Write([byte]$d); $gw.Write([uint16]0); $gw.Write([uint16]1); $gw.Write([uint16]32); $gw.Write([uint32]$pngs[$i].Length); $gw.Write([uint16]($i + 1)) }
$gw.Flush()
$res = New-Object IO.MemoryStream
$res.Write(($e = ResEntry 0 0 ([byte[]]@()) 0), 0, $e.Length)
for ($i = 0; $i -lt $sizes.Count; $i++) { $e = ResEntry 3 ($i + 1) $pngs[$i] 0x1010; $res.Write($e, 0, $e.Length) }
$e = ResEntry 14 1 $group.ToArray() 0x1030; $res.Write($e, 0, $e.Length)
[IO.File]::WriteAllBytes((Join-Path $work 'hookshot.res'), $res.ToArray())
"work: profile + Triforce icon"

# 4. the release name, read by nx2/version.py and Triforce Setup
Set-Content (Join-Path $tools 'version.txt') $Tag -Encoding ASCII

# 5. zip (tools/... inside)
$zip = Join-Path $Out 'Triforce-Tools.zip'
if (Test-Path $zip) { Remove-Item $zip }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# (entries written one by one: Windows PowerShell's CreateFromDirectory names them with backslashes)
$fs = [IO.File]::Open($zip, 'CreateNew')
$archive = New-Object IO.Compression.ZipArchive $fs, ([IO.Compression.ZipArchiveMode]::Create)
$base = (Resolve-Path $stage).Path.TrimEnd('\') + '\'
foreach ($f in Get-ChildItem $tools -Recurse -File) {
    $name = $f.FullName.Substring($base.Length).Replace('\', '/')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
}
$archive.Dispose(); $fs.Dispose()
Remove-Item -Recurse -Force $stage
"packed $zip ({0:N0} MB)" -f ((Get-Item $zip).Length / 1MB)
