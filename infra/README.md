# Azure infrastructure (Bicep)

Azure Developer CLI (azd) artifacts for the C++ Hong Kong Mahjong web server.

## Files

| File | Purpose |
|---|---|
| `main.bicep` | Container Apps + ACR + Log Analytics + App Insights + user-assigned MI. AZD-compliant: `environmentName` parameter, `azd-env-name` tag on every resource, `azd-service-name: 'web'` on the Container App, standard outputs. |
| `main.parameters.json` | Maps AZD env vars (`AZURE_ENV_NAME`, `AZURE_LOCATION`) to bicep parameters. Used implicitly by `azd up`. |
| `../azure.yaml` | Top-level service manifest. Declares the `web` containerapp service backed by the repo-root `Dockerfile`. |

## Why this shape

The bicep is intentionally a near-clone of the sibling TypeScript project at
`..\..\hongkong_mahjong\infra\main.bicep`, with three deliberate deltas:

1. **No Cosmos DB.** The C++ server keeps `RoomManager` state in process
   memory; nothing to persist between revisions.
2. **No Key Vault.** No secrets are referenced.
3. **Placeholder image on first deploy.** The bicep defaults
   `containerImageName` to `mcr.microsoft.com/azuredocs/containerapps-helloworld:latest`
   so the very first `azd up` can complete its Provision phase before the build
   has produced a real image. AZD then re-deploys the Container App with the
   freshly built image.

## Validate locally (no Azure changes)

```powershell
az bicep build --file infra\main.bicep --outfile infra\main.json
```

That emits an ARM JSON template you can sanity-check without touching Azure.

## Deploy (will create resources)

See `..\docs\azure-deployment.md` for the full walkthrough. TL;DR:

```powershell
.\scripts\azd-up.ps1 -EnvName mahjong-cpp-dev -Location westus3 -Deploy
```

## Tear down

```powershell
azd down --purge
```
