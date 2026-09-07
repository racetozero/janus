[CmdletBinding()]
param(
    [string]$Version = $env:JANUS_VERSION,
    [string]$Target = $env:JANUS_TARGET,
    [string]$InstallDirectory = $env:JANUS_INSTALL_DIR,
    [string]$Repository = $env:JANUS_REPOSITORY,
    [string]$ReleasesUrl = $env:JANUS_RELEASES_URL
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0

function Fail([string]$Message) {
    throw "janus installer: $Message"
}

function Normalize-Tag([string]$Requested) {
    $Normalized = if ($Requested.StartsWith("v")) { $Requested } else { "v$Requested" }
    $PlainVersion = $Normalized.Substring(1)
    if (-not $PlainVersion -or $PlainVersion -notmatch '^[0-9A-Za-z.+-]+$') {
        Fail "invalid version: $Requested"
    }
    return @($Normalized, $PlainVersion)
}

function Receive-File([string]$Source, [string]$Destination) {
    $Uri = [Uri]$Source
    if ($Uri.Scheme -eq "file") {
        Copy-Item -LiteralPath $Uri.LocalPath -Destination $Destination
        return
    }
    if ($Uri.Scheme -ne "https") {
        Fail "release downloads must use https or file URLs"
    }
    Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $Destination
}

try {
    if (-not $Repository) {
        $Repository = "racetozero/janus"
    }
    if (-not $ReleasesUrl) {
        $ReleasesUrl = "https://github.com/$Repository/releases"
    }

    if (-not $Version -or $Version -eq "latest") {
        if ($env:JANUS_RELEASES_URL) {
            Fail "set JANUS_VERSION when JANUS_RELEASES_URL is set"
        }
        try {
            $Release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/releases/latest"
            $Version = $Release.tag_name
        } catch {
            Fail "could not find the latest release"
        }
    }
    $Tag, $PlainVersion = Normalize-Tag $Version

    if (-not $Target) {
        $Architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        $Target = switch ($Architecture) {
            "X64" { "x86_64-pc-windows-msvc" }
            "Arm64" { "aarch64-pc-windows-msvc" }
            default { Fail "unsupported Windows processor: $Architecture" }
        }
    }
    if ($Target -notin @("x86_64-pc-windows-msvc", "aarch64-pc-windows-msvc")) {
        Fail "unsupported target: $Target"
    }

    if (-not $InstallDirectory) {
        if (-not $HOME) {
            Fail "HOME is not set; set JANUS_INSTALL_DIR"
        }
        $InstallDirectory = Join-Path $HOME ".local\bin"
    }

    $ArchiveName = "janus-$Target.zip"
    $ChecksumName = "$ArchiveName.sha256"
    $TemporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ("janus-install-" + [Guid]::NewGuid())
    New-Item -ItemType Directory -Path $TemporaryDirectory | Out-Null

    try {
        Write-Host "Installing janus $PlainVersion for $Target"
        $AssetBase = "$($ReleasesUrl.TrimEnd('/'))/download/$Tag"
        Receive-File "$AssetBase/$ArchiveName" (Join-Path $TemporaryDirectory $ArchiveName)
        Receive-File "$AssetBase/$ChecksumName" (Join-Path $TemporaryDirectory $ChecksumName)

        $ArchivePath = Join-Path $TemporaryDirectory $ArchiveName
        $Expected = ((Get-Content -LiteralPath (Join-Path $TemporaryDirectory $ChecksumName) |
            Where-Object { $_.Trim() } | Select-Object -First 1) -split '\s+')[0]
        if ($Expected -notmatch '^[0-9A-Fa-f]{64}$') {
            Fail "the checksum file is invalid"
        }
        $Actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $ArchivePath).Hash
        if ($Actual -ne $Expected) {
            Fail "checksum verification failed for $ArchiveName"
        }

        $ExtractDirectory = Join-Path $TemporaryDirectory "extract"
        Expand-Archive -LiteralPath $ArchivePath -DestinationPath $ExtractDirectory
        $Binary = Join-Path $ExtractDirectory "janus.exe"
        if (-not (Test-Path -LiteralPath $Binary -PathType Leaf)) {
            Fail "the release archive does not contain janus.exe"
        }

        New-Item -ItemType Directory -Force -Path $InstallDirectory | Out-Null
        $Destination = Join-Path $InstallDirectory "janus.exe"
        $Staged = Join-Path $InstallDirectory (".janus.install." + [Guid]::NewGuid() + ".exe")
        Copy-Item -LiteralPath $Binary -Destination $Staged
        Move-Item -Force -LiteralPath $Staged -Destination $Destination
        Write-Host "Installed janus $PlainVersion to $Destination"

        if ($InstallDirectory -notin ($env:PATH -split ';')) {
            Write-Host "Add $InstallDirectory to PATH to run janus from any directory."
        }
    } finally {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $TemporaryDirectory
    }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
