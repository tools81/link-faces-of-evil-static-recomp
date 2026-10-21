<#
  Build the distribution packages:

    dist\LinkFacesOfEvil-v<ver>-portable.zip   unzip anywhere and run
    dist\LinkFacesOfEvil-v<ver>-setup.exe      single-file installer (needs Inno Setup 6)

  The version comes from project(... VERSION x.y.z) in CMakeLists.txt.  The
  disc image is never packaged: players supply their own.

  -SkipBuild  package the executables already in build\cmake\Release
  -Publish    also create GitHub release v<ver> with both files (uses gh); the
              game's update check downloads the *-portable.zip asset from it
#>
param(
    [switch]$SkipBuild,
    [switch]$Publish
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$cm = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cm -notmatch 'project\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') { throw 'No VERSION in CMakeLists.txt' }
$ver = $Matches[1]
$repo = if ($cm -match 'set\(LINKFOE_UPDATE_REPO\s+"([^"]*)"') { $Matches[1] } else { '' }
Write-Host "Packaging v$ver"

if (-not $SkipBuild) {
    & cmd /c (Join-Path $root 'build.bat')
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
}

$rel = Join-Path $root 'build\cmake\Release'
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($f in 'linkfoe.exe', 'linkfoe_hd.exe') {
    Copy-Item (Join-Path $rel $f) $stage
}
Copy-Item (Join-Path $root 'README.md') $stage
New-Item -ItemType Directory -Force (Join-Path $stage 'rom') | Out-Null
Set-Content -Encoding ascii (Join-Path $stage 'rom\PUT YOUR DISC IMAGE HERE.txt') @'
Put your Link: The Faces of Evil disc image (.cue + .bin) in this folder,
or just start the game: it asks where the disc image is the first time.
'@

# portable zip
$zip = Join-Path $dist "LinkFacesOfEvil-v$ver-portable.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "  $zip"

# installer
$iscc = @(
    (Get-Command iscc.exe -ErrorAction SilentlyContinue | ForEach-Object Source),
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
$setup = Join-Path $dist "LinkFacesOfEvil-v$ver-setup.exe"
if ($iscc) {
    & $iscc /Q "/DAppVersion=$ver" "/DStageDir=$stage" "/DOutputDir=$dist" (Join-Path $root 'installer\linkfoe.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed' }
    Write-Host "  $setup"
} else {
    Write-Warning 'Inno Setup 6 not found: skipped the installer (winget install JRSoftware.InnoSetup)'
    $setup = $null
}

if ($Publish) {
    if (-not $repo) { throw 'LINKFOE_UPDATE_REPO is empty in CMakeLists.txt' }
    $files = @($zip) + @($setup | Where-Object { $_ })
    gh release create "v$ver" @files --repo $repo --title "v$ver" --notes "Link: The Faces of Evil recomp v$ver"
    if ($LASTEXITCODE -ne 0) { throw 'gh release create failed' }
}
