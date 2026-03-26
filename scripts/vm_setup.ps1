#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Sets up the Windows VM for the OpenClaw Twitch development stream.
    Installs: VS2022 (C++ workload), Python 3.13, Node.js 22, Git, OpenClaw, NDI Tools.
    Clones the d2-mod-toolkit repo.

.NOTES
    Run this as Administrator in PowerShell on the VM.
    Some installs require reboots — the script will tell you when.

.USAGE
    Set-ExecutionPolicy Bypass -Scope Process -Force
    .\vm_setup.ps1
#>

$ErrorActionPreference = "Stop"

# ============================================================
# Configuration — edit these if needed
# ============================================================
$RepoUrl      = "https://github.com/yourusername/d2-mod-toolkit.git"  # TODO: set your actual repo URL
$RepoDir      = "C:\Users\$env:USERNAME\source\cpp\d2-mod-toolkit"
$GameDir      = "C:\Diablo2\ProjectD2_dlls_removed"
$DownloadDir  = "$env:TEMP\vm_setup_downloads"

# ============================================================
# Helper functions
# ============================================================
function Write-Step($msg) {
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host "  $msg" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
}

function Download-File($url, $outFile) {
    if (Test-Path $outFile) {
        Write-Host "  Already downloaded: $outFile"
        return
    }
    Write-Host "  Downloading: $url"
    Invoke-WebRequest -Uri $url -OutFile $outFile -UseBasicParsing
}

function Test-Command($cmd) {
    try { Get-Command $cmd -ErrorAction Stop; return $true }
    catch { return $false }
}

# Create download directory
New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null

# ============================================================
# 1. Install Git
# ============================================================
Write-Step "1. Git"
if (Test-Command "git") {
    Write-Host "  Git already installed: $(git --version)"
} else {
    Write-Host "  Installing Git via winget..."
    winget install --id Git.Git -e --accept-package-agreements --accept-source-agreements
    # Refresh PATH
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")
    Write-Host "  Git installed: $(git --version)"
}

# ============================================================
# 2. Install Python 3.13
# ============================================================
Write-Step "2. Python 3.13"
if (Test-Command "python") {
    $pyVer = python --version 2>&1
    Write-Host "  Python already installed: $pyVer"
} else {
    Write-Host "  Installing Python via winget..."
    winget install --id Python.Python.3.13 -e --accept-package-agreements --accept-source-agreements
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")
    Write-Host "  Python installed: $(python --version)"
}

# Install Python dependencies
Write-Host "  Installing Python packages..."
python -m pip install --upgrade pip 2>$null
python -m pip install requests python-dotenv 2>$null

# ============================================================
# 3. Install Node.js 22 (for OpenClaw)
# ============================================================
Write-Step "3. Node.js 22"
if (Test-Command "node") {
    $nodeVer = node --version 2>&1
    Write-Host "  Node.js already installed: $nodeVer"
} else {
    Write-Host "  Installing Node.js via winget..."
    winget install --id OpenJS.NodeJS.LTS -e --accept-package-agreements --accept-source-agreements
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")
    Write-Host "  Node.js installed: $(node --version)"
}

# ============================================================
# 4. Install Visual Studio 2022 Community (C++ workload)
# ============================================================
Write-Step "4. Visual Studio 2022 Community (C++ Desktop)"
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstalled = $false
if (Test-Path $vsWhere) {
    $vsPath = & $vsWhere -latest -property installationPath 2>$null
    if ($vsPath) { $vsInstalled = $true }
}

if ($vsInstalled) {
    Write-Host "  VS2022 already installed at: $vsPath"
} else {
    $vsInstaller = "$DownloadDir\vs_community.exe"
    Download-File "https://aka.ms/vs/17/release/vs_community.exe" $vsInstaller

    Write-Host "  Installing VS2022 with C++ Desktop workload..."
    Write-Host "  This will take 10-30 minutes. Please wait."
    $vsArgs = @(
        "--add", "Microsoft.VisualStudio.Workload.NativeDesktop",
        "--includeRecommended",
        "--passive",
        "--norestart",
        "--wait"
    )
    Start-Process -FilePath $vsInstaller -ArgumentList $vsArgs -Wait -NoNewWindow
    Write-Host "  VS2022 installation complete."
}

# ============================================================
# 5. Install OpenClaw
# ============================================================
Write-Step "5. OpenClaw"
if (Test-Command "openclaw") {
    Write-Host "  OpenClaw already installed: $(openclaw --version 2>&1)"
} else {
    Write-Host "  Installing OpenClaw globally..."
    npm install -g openclaw
    Write-Host "  OpenClaw installed."
}

# ============================================================
# 6. Install NDI Tools (for streaming VM -> Host)
# ============================================================
Write-Step "6. NDI Tools"
$ndiReg = Get-ItemProperty "HKLM:\SOFTWARE\NDI\*" -ErrorAction SilentlyContinue
if ($ndiReg) {
    Write-Host "  NDI Tools already installed."
} else {
    Write-Host "  NDI Tools must be downloaded manually from:"
    Write-Host "    https://ndi.video/tools/" -ForegroundColor Yellow
    Write-Host "  Install 'NDI Tools' which includes NDI Screen Capture."
    Write-Host "  After installing, NDI Screen Capture will broadcast this VM's screen"
    Write-Host "  to any NDI receiver on the local network (your host OBS)."
    Write-Host ""
    Write-Host "  Press Enter to continue after noting this..." -ForegroundColor Yellow
    Read-Host
}

# ============================================================
# 7. Clone the repo
# ============================================================
Write-Step "7. Clone d2-mod-toolkit"
if (Test-Path "$RepoDir\.git") {
    Write-Host "  Repo already exists at $RepoDir"
    Push-Location $RepoDir
    git pull --ff-only 2>$null
    Pop-Location
} else {
    Write-Host "  Cloning to $RepoDir..."
    $parentDir = Split-Path $RepoDir -Parent
    New-Item -ItemType Directory -Force -Path $parentDir | Out-Null
    git clone $RepoUrl $RepoDir
}

# ============================================================
# 8. Create directory structure
# ============================================================
Write-Step "8. Directory structure"
@($GameDir, "$RepoDir\scripts") | ForEach-Object {
    if (!(Test-Path $_)) {
        New-Item -ItemType Directory -Force -Path $_ | Out-Null
        Write-Host "  Created: $_"
    } else {
        Write-Host "  Exists: $_"
    }
}

# ============================================================
# 9. Create .env template
# ============================================================
Write-Step "9. Environment file"
$envFile = "$RepoDir\scripts\.env"
if (!(Test-Path $envFile)) {
    @"
# Twitch credentials — NEVER commit this file
TWITCH_CHANNEL=bethington
TWITCH_BOT_USERNAME=bethington
TWITCH_OAUTH_TOKEN=oauth:your_token_here
TWITCH_CLIENT_ID=your_client_id_here
"@ | Set-Content $envFile
    Write-Host "  Created $envFile — edit with your Twitch credentials"
} else {
    Write-Host "  .env already exists"
}

# ============================================================
# Summary
# ============================================================
Write-Step "SETUP COMPLETE"
Write-Host ""
Write-Host "  Installed:" -ForegroundColor Green
Write-Host "    Git:          $(if (Test-Command 'git') { git --version } else { 'MISSING' })"
Write-Host "    Python:       $(if (Test-Command 'python') { python --version 2>&1 } else { 'MISSING' })"
Write-Host "    Node.js:      $(if (Test-Command 'node') { node --version 2>&1 } else { 'MISSING' })"
Write-Host "    MSBuild:      $(if (Test-Path '${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe') { 'Found' } else { 'MISSING — may need reboot' })"
Write-Host "    OpenClaw:     $(if (Test-Command 'openclaw') { 'Installed' } else { 'MISSING — run: npm i -g openclaw' })"
Write-Host ""
Write-Host "  Remaining manual steps:" -ForegroundColor Yellow
Write-Host "    1. Copy Game.exe + D2 files to $GameDir"
Write-Host "    2. Edit $envFile with Twitch credentials"
Write-Host "    3. Run: openclaw onboard --install-daemon"
Write-Host "    4. Run: openclaw plugins install @openclaw/twitch"
Write-Host "    5. Install NDI Tools from https://ndi.video/tools/"
Write-Host "    6. Start NDI Screen Capture on this VM"
Write-Host "    7. On host: Add NDI source in OBS"
Write-Host ""
Write-Host "  To test the build skill:" -ForegroundColor Yellow
Write-Host "    cd $RepoDir"
Write-Host "    python scripts\build_and_deploy.py --compile-only --json"
Write-Host ""
