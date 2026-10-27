# Builds the downloadable release folder from a finished build.
#   powershell -ExecutionPolicy Bypass -File pc\package_release.ps1
# Output: release\ACGC-Desktop\ (zip that folder). Only the program and its
# runtime files go in: never a disc image, saves, settings or logs.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $PSScriptRoot "build32\bin"
$dst = Join-Path $root "release\ACGC-Desktop"

if (-not (Test-Path (Join-Path $bin "AnimalCrossing.exe"))) {
    throw "Build the game first: $bin\AnimalCrossing.exe is missing."
}

if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
New-Item -ItemType Directory -Force $dst, "$dst\shaders", "$dst\rom", "$dst\save", "$dst\docs" | Out-Null

Copy-Item (Join-Path $bin "AnimalCrossing.exe"), (Join-Path $bin "SDL2.dll") $dst
Copy-Item (Join-Path $bin "shaders\*") "$dst\shaders"
Copy-Item (Join-Path $PSScriptRoot "dist\Start Wallpaper.bat") $dst
Copy-Item (Join-Path $root "README.md"), (Join-Path $root "LICENSE"), (Join-Path $root "FAQ.md") $dst
Copy-Item (Join-Path $root "docs\wallpaper.md"), (Join-Path $root "docs\pc-port.md") "$dst\docs"
Copy-Item (Join-Path $root "docs\images") "$dst\docs" -Recurse

Set-Content "$dst\rom\PUT YOUR DISC IMAGE HERE.txt" -Encoding ascii -Value @(
    "Put your Animal Crossing disc image in this folder.",
    "",
    "Animal Crossing (USA), GAFE01 Rev 0, as an .iso, .gcm or .ciso file.",
    "This download contains no game files; use your own copy of the game.")
Set-Content "$dst\save\PUT YOUR SAVE HERE.txt" -Encoding ascii -Value @(
    "Optional: put your Dolphin save files (.gci) in this folder to use your existing town.",
    "",
    "The game also saves here when you play. The wallpaper itself never writes to your save.")

$size = (Get-ChildItem $dst -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
"Release folder ready: $dst ({0:N1} MB). Zip the ACGC-Desktop folder." -f $size
