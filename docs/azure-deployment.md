# Azure deployment

The C++ web server (Crow HTTP/WebSocket) ships as a Linux container and runs on
**Azure Container Apps**, mirroring the deployment shape of the sibling
TypeScript project at `..\hongkong_mahjong`. This document describes how to go
from a fresh clone to `https://mahjong-cpp.<region>.azurecontainerapps.io` with
a single `azd up`.

> **Local-only by default.** Nothing in this repo currently auto-deploys.
> Running `.\scripts\run-web.ps1` keeps everything on `localhost:18080`.
> Provisioning Azure resources costs money — only run the commands below when
> you actually intend to ship the server.

---

## What gets provisioned

`infra/main.bicep` defines a minimal, AZD-compliant topology:

| Resource | Purpose | SKU |
|---|---|---|
| `Microsoft.ManagedIdentity/userAssignedIdentities` | Container App identity for ACR pull | — |
| `Microsoft.ContainerRegistry/registries` | Hosts the built image | Basic |
| `Microsoft.OperationalInsights/workspaces` | Required by Container Apps env | PerGB2018 |
| `Microsoft.Insights/components` | Optional telemetry (App Insights, web kind) | Standard |
| `Microsoft.App/managedEnvironments` | Container Apps environment | Consumption |
| `Microsoft.App/containerApps` | Single-replica web server, public HTTPS ingress, WebSocket upgrade | 0.5 vCPU / 1 GiB |

No Cosmos DB and no Key Vault — unlike the TypeScript project, the C++ server
keeps room/game state in process memory (`RoomManager`), so there is nothing
external to persist. `minReplicas` and `maxReplicas` are therefore both `1`;
scaling out would require migrating to a shared store.

---

## Prerequisites

| Tool | Install |
|---|---|
| Azure Developer CLI (`azd`) | `winget install Microsoft.Azd` |
| Azure CLI (`az`) | `winget install Microsoft.AzureCLI` |
| Docker Desktop | `winget install Docker.DockerDesktop` (started and running) |
| Azure subscription with Contributor rights | sign in with `azd auth login` |

Make sure Docker Desktop is **running** before `azd up` — AZD calls
`docker build` locally and pushes the image to ACR.

---

## First deploy (`azd up`)

The helper script is the easiest path, but `azd` invocations work directly too.

### Helper script

```powershell
# Dry run: prints the plan, no Azure changes.
.\scripts\azd-up.ps1

# Real deploy:
.\scripts\azd-up.ps1 -EnvName mahjong-cpp-dev -Location westus3 -Deploy
```

### Manual flow

```powershell
azd auth login
azd env new mahjong-cpp-dev --location westus3
azd up
```

`azd up` performs four phases:

1. **Provision** — runs `infra/main.bicep`, creates the resource group
   `rg-<envName>`, ACR, Container Apps environment, etc. The Container App
   initially launches with the `mcr.microsoft.com/.../helloworld:latest`
   placeholder image so the resource graph is valid.
2. **Build** — `docker build .` using the multi-stage `Dockerfile` at the repo
   root. The first build takes ~5 min because CMake `FetchContent` downloads
   Crow, asio, and nlohmann/json.
3. **Push** — image tagged `mahjong-cpp-dev/web-<sha>:azd-deploy-<ts>` and
   pushed to ACR.
4. **Update** — Container App revision flipped to the new image.

When it's done, you'll see something like:

```
SERVICE_WEB_URI: https://ca-mahjong-cpp-mahjong-cpp-dev.<hash>.<region>.azurecontainerapps.io
```

Open that URL — the lobby loads, you create or join a room, share the
`?room=XXXX&seat=N&token=...` link with your parents, and you're playing.

---

## Iterative updates

After the first `azd up`, you usually only change C++ or web/ code. Use:

```powershell
azd deploy            # rebuild image + roll Container App, no infra changes
```

If you change `infra/main.bicep` itself:

```powershell
azd provision         # apply infra delta
azd deploy            # then push a fresh image if needed
```

---

## Inspecting / troubleshooting

```powershell
# Show the resource group + endpoints
azd env get-values

# Stream container logs
az containerapp logs show -g rg-mahjong-cpp-dev -n ca-mahjong-cpp-mahjong-cpp-dev --follow

# Get a shell inside the running container
az containerapp exec -g rg-mahjong-cpp-dev -n ca-mahjong-cpp-mahjong-cpp-dev --command /bin/bash

# Hit the health endpoint directly
curl https://<fqdn>/api/health
```

The Container App liveness/readiness probes both hit `/api/health`. If the
revision is failing to start, check `az containerapp logs show` first — usually
it's the port (must be `8080`) or the static `MAHJONG_WEB_DIR` (must be
`/app/web` because that's where the Dockerfile puts the static files).

---

## Tear-down

```powershell
azd down --purge
```

`--purge` skips the soft-delete retention window for the ACR so the next
`azd up` can reuse the same name immediately.

---

## Comparison to the TypeScript project

| Aspect | `hongkong_mahjong` (TS) | `hongkong_mahjong_cpp` (this repo) |
|---|---|---|
| Runtime image base | `node:22-slim` | `debian:bookworm-slim` |
| Service binary | `node apps/server/dist/index.js` | `/app/mahjong_web_server` (C++) |
| Container Apps service name (`azd-service-name`) | `web` | `web` |
| Persistent state | Cosmos DB (`rooms`, `gameEvents`) | None — in-memory `RoomManager` |
| Secrets | Key Vault + MI for Cosmos | None needed |
| Replicas | 1 (until durable adapter wired) | 1 (in-memory only) |
| Observability | App Insights + Log Analytics | App Insights + Log Analytics |

The bicep, `azure.yaml`, and parameters file follow the exact AZD conventions
used by the TypeScript project so `azd up` behaves identically.
