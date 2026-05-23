<#
.SYNOPSIS
  Redeploy the mahjong web app to an existing Azure environment.

.DESCRIPTION
  Use this AFTER `azd up` has been run at least once for the target
  environment. It rebuilds the container in Azure Container Registry and
  rolls out a new Container Apps revision with zero downtime.

  This script does NOT touch infrastructure. If you changed anything under
  infra/, run `azd provision` first (or `azd up`).

  Dry-run by default. Pass -Deploy to actually push the new revision.

.PARAMETER EnvName
  AZD environment name. Defaults to the currently-selected environment.

.PARAMETER Deploy
  Without this flag, prints the plan and exits. With it, runs `azd deploy`.

.PARAMETER Logs
  After a successful deploy, stream container logs until you press Ctrl+C.

.EXAMPLE
  .\scripts\azd-redeploy.ps1
  # dry-run: shows what would be deployed and which env

.EXAMPLE
  .\scripts\azd-redeploy.ps1 -EnvName mahjong-prod -Deploy

.EXAMPLE
  .\scripts\azd-redeploy.ps1 -Deploy -Logs
#>
[CmdletBinding()]
param(
  [string] $EnvName,
  [switch] $Deploy,
  [switch] $Logs
)

# Cmdlets/Set-Location errors -> terminating. Native exits are checked below.
$ErrorActionPreference = 'Stop'

# --- helpers ---------------------------------------------------------------

function Assert-Tool {
  param([Parameter(Mandatory)][string] $Name, [string] $Hint)
  if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
    throw "Required tool '$Name' is not on PATH. $Hint"
  }
}

# Runs azd with the given args. Throws if azd returns nonzero exit.
# -PassThruOutput captures stdout (for parsing); otherwise output streams live.
function Invoke-Azd {
  param(
    [Parameter(Mandatory, ValueFromRemainingArguments)][string[]] $Arguments,
    [switch] $PassThruOutput
  )
  Write-Host ("-> azd " + ($Arguments -join ' ')) -ForegroundColor Cyan
  if ($PassThruOutput) {
    $output = & azd @Arguments 2>&1
    $exit = $LASTEXITCODE
    if ($exit -ne 0) {
      $output | ForEach-Object { Write-Host $_ }
      throw "azd $($Arguments -join ' ') failed with exit code $exit"
    }
    return $output
  }
  & azd @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "azd $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
  }
}

# --- preflight -------------------------------------------------------------

Assert-Tool -Name 'azd' -Hint 'Install with: winget install Microsoft.Azd  (or follow https://aka.ms/azd)'

# Locate the repo root (azure.yaml must be present).
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not (Test-Path (Join-Path $repoRoot 'azure.yaml'))) {
  throw "azure.yaml not found at '$repoRoot'. Move the script into a sibling of azure.yaml."
}

# Push/pop so we never leak the CWD change into the caller's session.
Push-Location $repoRoot
try {

  # Switch env if asked. Errors propagate via Invoke-Azd.
  if ($EnvName) { Invoke-Azd env select $EnvName }

  # Show the active env so the user can sanity-check the target.
  Write-Host ""
  Write-Host "=== Active AZD environment ===" -ForegroundColor Cyan
  Invoke-Azd env list
  Write-Host ""

  # Courtesy hint: infra/ mtime. The script never blocks on this; if the
  # user changed infra they should run 'azd provision' (or 'azd up') first.
  $infraFiles = Get-ChildItem (Join-Path $repoRoot 'infra') -Recurse -File -ErrorAction SilentlyContinue
  if ($infraFiles) {
    $newest = ($infraFiles | Measure-Object LastWriteTime -Maximum).Maximum
    Write-Host "infra/ newest mtime: $newest"
    Write-Host "If you changed anything under infra/, run 'azd provision' (or re-run 'azd up') BEFORE this script." -ForegroundColor Yellow
    Write-Host ""
  }

  if (-not $Deploy) {
    Write-Host "DRY RUN -- no deploy will happen." -ForegroundColor Yellow
    Write-Host "To redeploy the web service, re-run with: -Deploy" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Would execute: azd deploy web"
    return
  }

  # --- deploy --------------------------------------------------------------
  # 'azd deploy web' rebuilds the image and rolls a new Container Apps
  # revision. Zero-downtime; the old revision drains naturally.
  Invoke-Azd deploy web

  Write-Host ""
  Write-Host "Deploy complete." -ForegroundColor Green

  # Extract the public URI from 'azd env get-values'. The output is
  # KEY="value" per line; quotes may or may not be present depending on azd
  # version, so accept both. Print whatever URI-like keys we find.
  try {
    $envValues = Invoke-Azd -PassThruOutput env get-values
    $uris = $envValues | Where-Object { $_ -match '^(SERVICE_WEB_URI|WEB_URI|API_URI)=' }
    if ($uris) {
      Write-Host "App endpoint(s):"
      foreach ($line in $uris) {
        $clean = $line -replace '^([A-Z_]+)="?([^"]+)"?$', '$1 = $2'
        Write-Host "  $clean"
      }
    } else {
      Write-Host "(no SERVICE_WEB_URI / WEB_URI / API_URI in azd env values -- check 'azd env get-values' manually)" -ForegroundColor Yellow
    }
  } catch {
    Write-Host "Could not read azd env values: $($_.Exception.Message)" -ForegroundColor Yellow
  }

  if ($Logs) {
    Write-Host ""
    Write-Host "-> azd monitor --logs (Ctrl+C to stop)" -ForegroundColor Cyan
    # azd monitor --logs is interactive; let it stream and inherit Ctrl+C.
    & azd monitor --logs
    # Don't fail the script on Ctrl+C exit code; user knows when to stop.
  }

}
finally {
  Pop-Location
}
