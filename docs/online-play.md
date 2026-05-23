# Hong Kong Mahjong — online play deployment

## Status
- **Win32 GUI:** local-only, unchanged. Continue to use it as before.
- **Web server + browser client:** built locally and verified on `localhost`. **Not deployed to Azure yet.**

## Architecture

```
Azure Container Apps (Linux container, when deployed)
 ├── mahjong_web_server (C++ binary, Crow HTTP/WS)
 ├── Reuses src/core/mahjong_core via library
 └── Static-serves /app/web (HTML + Canvas)
        ▲
        │ JSON over WSS
   ┌────┴────┐
   │ Browser │  (parents in Indonesia)
   └─────────┘
```

The same Crow server can also be hit from a native client later if/when the
Win32 GUI gains an "online mode" — the JSON protocol is fully defined.

## Local development

```powershell
# Build the web server (downloads Crow, asio, nlohmann/json the first time):
.\scripts\build-web-msvc.ps1            # produces build-web\Release\mahjong_web_server.exe

# Run locally, serving the web/ folder:
$env:PORT='18080'
$env:MAHJONG_WEB_DIR='C:\Users\you\code\hongkong_mahjong_cpp\web'
.\build-web\Release\mahjong_web_server.exe
# Open http://localhost:18080/ in a browser.

# Optional sanity test of the WS protocol:
pip install websockets
python .\scripts\ws-smoke.py
```

The existing Win32 build script (`scripts\build-msvc.ps1`) is **untouched** and
keeps producing `mahjong_gui.exe`, `mahjong_server.exe` (CLI demo), etc.

## Network protocol (JSON over WebSocket at `/ws`)

Client → Server:
- `{"type":"hello","roomCode":"ABC123","seatIndex":N,"sessionToken":"..."}`
  (omit `seatIndex`+`sessionToken` for spectators)
- `{"type":"action","expectedVersion":N,"action":{...}}`
- `{"type":"ping"}`

Server → Client:
- `{"type":"welcome","roomCode":"...","seatIndex":N}` (seatIndex omitted for spectators)
- `{"type":"snapshot","snapshot":{...}}` (broadcast after each state change)
- `{"type":"error","code":"...","message":"..."}`
- `{"type":"pong"}`

HTTP routes:
- `GET  /api/health` → `ok`
- `POST /api/rooms` → `{ roomCode, version, claimLinks: [{seatIndex, token, url}] }`
- `GET  /api/rooms/:code` → public snapshot (spectator view)
- `POST /api/rooms/:code/seats/:seat` (body `{token, displayName}`) → `{ sessionToken, ... }`
- `GET  /` and `GET  /<file>` → static-served from `MAHJONG_WEB_DIR`

## Azure deployment (manual — NOT executed automatically)

When you're ready, from this folder:

```bash
az login
az group create -n mahjong-rg -l westus2

# 1. Provision the registry + log analytics + environment (no container app yet):
az deployment group create -g mahjong-rg -f infra/main.bicep -p envName=mahjong

# 2. Build & push the image to the registry (Azure-side build, no local Docker needed):
ACR_NAME=$(az deployment group show -g mahjong-rg -n main --query 'properties.outputs.acrName.value' -o tsv)
az acr build -r "$ACR_NAME" -t mahjong-web:latest .

# 3. Re-deploy the Bicep with the image parameter to create the Container App:
az deployment group create -g mahjong-rg -f infra/main.bicep \
    -p envName=mahjong image="${ACR_NAME}.azurecr.io/mahjong-web:latest"

# 4. Read the public FQDN:
az containerapp show -g mahjong-rg -n mahjong-web --query 'properties.configuration.ingress.fqdn' -o tsv
```

Share `https://<that-fqdn>/?room=...&seat=...&token=...` with each player.

### Cost estimate
- Container Apps Consumption, 0.25 vCPU / 0.5 GiB / 1 replica always-on: ≈ $8–12/month.
- ACR Basic: ≈ $5/month.
- Outbound bandwidth for a few hours of play: < $1.
- Easily inside the $200 free credits a new Azure subscription gets.

## What's missing (intentional non-goals for v1)
- Persistent storage (server restart loses in-progress rooms).
- Accounts, matchmaking, global lobby.
- Voice/video chat.
- Win32 GUI's "online mode" (the GUI is local-only today; the protocol exists for future use).
