# Convenience wrapper around the Azure Developer CLI for the C++ Mahjong web
# server. It does *not* deploy unless you pass -Deploy. The default flow:
#
#   1. Confirms azd is installed.
#   2. Creates/loads the AZD environment (default name: mahjong-cpp-<user>).
#   3. Sets AZURE_LOCATION (default westus3).
#   4. Optionally runs `azd up` end-to-end.
#
# Typical first-time use:
#
#   .\scripts\azd-up.ps1                     # dry-run, prints what azd up would do
#   .\scripts\azd-up.ps1 -EnvName mj-dev     # create/select env, print plan
#   .\scripts\azd-up.ps1 -Deploy             # actually provision + deploy
#
# Subsequent code-only iterations:
#
#   azd deploy
#
# Tear-down:
#
#   azd down --purge
[CmdletBinding()]
param(
    [string]$EnvName = "mahjong-cpp-$($env:USERNAME.ToLower())",
    [string]$Location = "westus3",
    [string]$Subscription = "",
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $repoRoot
try {
    if (-not (Get-Command azd -ErrorAction SilentlyContinue)) {
        Write-Host "azd is not installed. Install: winget install Microsoft.Azd" -ForegroundColor Red
        exit 1
    }

    Write-Host "Repo root: $repoRoot"
    Write-Host "AZD environment: $EnvName"
    Write-Host "Region: $Location"

    # Make sure we are logged in.
    $whoami = & azd auth login --check-status 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Not logged in to azd. Running 'azd auth login'..." -ForegroundColor Yellow
        & azd auth login
        if ($LASTEXITCODE -ne 0) { throw "azd auth login failed" }
    }

    # Create or select the environment.
    $envList = & azd env list --output json 2>$null | ConvertFrom-Json -ErrorAction SilentlyContinue
    $exists = $envList | Where-Object { $_.Name -eq $EnvName }
    if (-not $exists) {
        Write-Host "Creating AZD environment '$EnvName'..." -ForegroundColor Cyan
        & azd env new $EnvName --location $Location $(if ($Subscription) { @('--subscription', $Subscription) }) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "azd env new failed" }
    } else {
        Write-Host "Selecting existing environment '$EnvName'..." -ForegroundColor Cyan
        & azd env select $EnvName | Out-Null
    }

    # Ensure location is set (azd will use it on `up`).
    & azd env set AZURE_LOCATION $Location | Out-Null

    Write-Host ""
    Write-Host "Resources that will be provisioned by 'azd up':" -ForegroundColor Green
    Write-Host "  - Resource group rg-$EnvName"
    Write-Host "  - User-assigned managed identity"
    Write-Host "  - Azure Container Registry (Basic SKU)"
    Write-Host "  - Log Analytics workspace"
    Write-Host "  - Application Insights"
    Write-Host "  - Container Apps managed environment"
    Write-Host "  - Container App 'ca-mahjong-cpp-<env>' (1 replica)"

    if (-not $Deploy) {
        Write-Host ""
        Write-Host "Dry run complete. To actually deploy, re-run with -Deploy" -ForegroundColor Yellow
        Write-Host "    .\scripts\azd-up.ps1 -EnvName $EnvName -Location $Location -Deploy"
        exit 0
    }

    Write-Host ""
    Write-Host "Running 'azd up' (this builds the Docker image, pushes to ACR, and deploys)..." -ForegroundColor Cyan
    & azd up
    if ($LASTEXITCODE -ne 0) { throw "azd up failed" }

    $endpoint = & azd env get-values --output json 2>$null | ConvertFrom-Json | Select-Object -ExpandProperty SERVICE_WEB_URI
    if ($endpoint) {
        Write-Host ""
        Write-Host "Deployment complete." -ForegroundColor Green
        Write-Host "  Web UI: $endpoint" -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
