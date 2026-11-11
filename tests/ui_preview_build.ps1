# (Triforce) Builds tests/ui_preview.exe: the menu's own source (gpu/vk/vk_menu.cpp) with a software stand-in for the GPU side,
# for pictures of the menu without the game (tests/ui_preview.cpp says how it is used).
#   powershell -File tests\ui_preview_build.ps1 [-Out <folder>] [-Cpm <CPM cache with vulkanheaders, vulkanmemoryallocator, spirv-headers>]
param([string]$Out = (Join-Path $PSScriptRoot 'ui_preview_build'), [string]$Cpm = $env:RX_CPM)
$ErrorActionPreference = 'Continue'   # (clang's warnings on stderr are not failures: the exit codes are checked)
$root = Split-Path $PSScriptRoot -Parent
if (-not $Cpm) { throw 'the CPM cache: -Cpm <folder>, or RX_CPM' }
$clang = 'C:\Program Files\LLVM\bin\clang++.exe'; $cc = 'C:\Program Files\LLVM\bin\clang.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
function Pkg([string]$name) { (Get-ChildItem (Join-Path $Cpm $name) -Directory | Select-Object -First 1).FullName }
$vk = "$root\gpu\vk"; $gpu = "$root\gpu"; $imgui = "$root\third_party\imgui"; $zstd = "$root\third_party\zstd\lib"
$inc = @("-I$vk\compat", "-I$gpu\compat", "-I$vk\eden", "-I$gpu\third_party\suyu", "-I$vk\sirit\include", "-I$vk\sirit\src",
         "-I$(Pkg 'spirv-headers')\include", "-I$(Pkg 'spirv-headers')\include\spirv\unified1", "-I$(Pkg 'vulkanheaders')\include",
         "-I$(Pkg 'vulkanmemoryallocator')\include", "-I$vk", "-I$imgui", "-I$zstd", "-I$root\host")
$flags = @('--target=x86_64-pc-windows-msvc', '-fms-extensions', '-D_CRT_SECURE_NO_WARNINGS', '-include', 'algorithm', '-include', 'cstdio',
           '-include', 'cstdlib', '-include', 'cstring', '-include', 'functional', '-include', 'string', '-include', 'vector', '-std=c++20', '-O2',
           '-g0', '-DNDEBUG', '-Wno-everything', '-DVK_NO_PROTOTYPES', '-DMKWVK_BUILDING', '-DMKWVK_INCLUDES=2',
           '-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES', '-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS', '-DHOOKSHOT_VERSION=\"preview\"') + $inc
$objs = @()
function Obj([string]$src, [string[]]$f, [string]$compiler = $clang) {
    $o = Join-Path $Out (([IO.Path]::GetFileNameWithoutExtension($src)) + '.o')
    if (-not (Test-Path $o) -or (Get-Item $src).LastWriteTime -gt (Get-Item $o).LastWriteTime -or $script:force) {
        & $compiler @f -c $src -o $o
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
    }
    $script:objs += $o
}
# (the menu and the stand-in always: their headers change)
$script:force = $true
Obj "$vk\vk_menu.cpp" $flags
Obj "$PSScriptRoot\ui_preview.cpp" $flags
$script:force = $false
foreach ($f in 'imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp') { Obj "$imgui\$f" $flags }
Obj "$gpu\third_party\suyu\video_core\textures\astc.cpp" $flags
Obj "$gpu\third_party\suyu\video_core\textures\decoders.cpp" $flags
$cflags = @('--target=x86_64-pc-windows-msvc', '-O2', '-g0', '-DNDEBUG', '-Wno-everything', "-I$zstd", '-DZSTD_DISABLE_ASM')
foreach ($f in (Get-ChildItem "$zstd\common\*.c", "$zstd\decompress\*.c")) { Obj $f.FullName $cflags $cc }
& $clang '--target=x86_64-pc-windows-msvc' '-fuse-ld=lld' @objs '-o' (Join-Path $Out 'ui_preview.exe')
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
"built $(Join-Path $Out 'ui_preview.exe')"
