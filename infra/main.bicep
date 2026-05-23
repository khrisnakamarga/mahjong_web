// Azure Container Apps deployment for the C++ Hong Kong Mahjong web server.
//
// AZD-compliant: required `environmentName` parameter, `azd-env-name` tag on
// every resource, `azd-service-name: 'web'` tag on the Container App so
// `azd deploy` knows which service to update, and standard outputs that AZD
// surfaces to the user and pipes back into the Dockerfile build context.
//
// Resources (mirrors the sibling TypeScript project, minus Cosmos/Key Vault
// because the C++ server keeps room state in-memory):
//   - User-assigned managed identity (Container App pulls images via this)
//   - Azure Container Registry (Basic SKU)  — AcrPull granted to the identity
//   - Log Analytics workspace                — required by Container Apps env
//   - Application Insights (web kind)        — wired through APPLICATIONINSIGHTS_CONNECTION_STRING
//   - Container Apps managed environment     — consumption plan
//   - Container App                          — placeholder image on first deploy,
//                                              azd replaces it with the built one
//
// Initial replicas are pinned to 1 because rooms are held in-memory; scaling out
// would require a shared coordination store, the same caveat as the original
// project carried at first deploy.
targetScope = 'resourceGroup'

@minLength(1)
@maxLength(64)
@description('Name of the AZD environment. Provided automatically as AZURE_ENV_NAME.')
param environmentName string

@minLength(1)
@description('Azure region for all resources.')
param location string = 'westus3'

@minLength(1)
@maxLength(40)
@description('Short application name; used as a prefix in resource names.')
param appName string = 'mahjong-cpp'

@allowed([
  'dev'
  'test'
  'prod'
])
@description('Environment classification, used as a tag and to nudge SKU sizing.')
param environmentType string = 'prod'

@description('Container image to deploy. AZD overrides this once it has built and pushed the real image. The default is a public placeholder that lets the first `azd up` complete cleanly.')
param containerImageName string = 'mcr.microsoft.com/azuredocs/containerapps-helloworld:latest'

@minValue(1)
@maxValue(10)
@description('Minimum replicas. Keep at 1 because room state is in-memory.')
param minReplicas int = 1

@minValue(1)
@maxValue(10)
@description('Maximum replicas. Capped low for the same in-memory reason.')
param maxReplicas int = 1

@description('Container CPU. Bicep "json()" wrapper because Container Apps wants a number not a string.')
param containerCpu string = '0.5'

@description('Container memory.')
param containerMemory string = '1Gi'

@description('Optional tag map merged into every resource.')
param tags object = {}

var resourceToken = uniqueString(subscription().id, environmentName, location)
var safeEnvironmentName = toLower(replace(environmentName, '_', '-'))
var safeAppName = toLower(replace(appName, '_', '-'))
var alphanumericAppName = replace(safeAppName, '-', '')
var alphanumericEnvironmentName = replace(safeEnvironmentName, '-', '')

var baseTags = union(tags, {
  application: appName
  environment: environmentName
  environmentType: environmentType
  'azd-env-name': environmentName
})

var managedIdentityName    = take('id-${safeAppName}-${safeEnvironmentName}', 128)
var logAnalyticsName       = take('log-${safeAppName}-${safeEnvironmentName}-${resourceToken}', 63)
var appInsightsName        = take('appi-${safeAppName}-${safeEnvironmentName}-${resourceToken}', 255)
var containerEnvironmentName = take('cae-${safeAppName}-${safeEnvironmentName}-${resourceToken}', 60)
var containerAppName       = take('ca-${safeAppName}-${safeEnvironmentName}', 32)
var acrName                = take('${alphanumericAppName}${alphanumericEnvironmentName}${resourceToken}', 50)

resource managedIdentity 'Microsoft.ManagedIdentity/userAssignedIdentities@2023-01-31' = {
  name: managedIdentityName
  location: location
  tags: baseTags
}

resource logAnalytics 'Microsoft.OperationalInsights/workspaces@2022-10-01' = {
  name: logAnalyticsName
  location: location
  tags: baseTags
  properties: {
    sku: {
      name: 'PerGB2018'
    }
    retentionInDays: 30
    publicNetworkAccessForIngestion: 'Enabled'
    publicNetworkAccessForQuery: 'Enabled'
  }
}

resource appInsights 'Microsoft.Insights/components@2020-02-02' = {
  name: appInsightsName
  location: location
  tags: baseTags
  kind: 'web'
  properties: {
    Application_Type: 'web'
    WorkspaceResourceId: logAnalytics.id
  }
}

resource containerRegistry 'Microsoft.ContainerRegistry/registries@2023-07-01' = {
  name: acrName
  location: location
  tags: baseTags
  sku: {
    name: 'Basic'
  }
  properties: {
    adminUserEnabled: false
  }
}

// AcrPull role for the user-assigned MI so the Container App can pull images.
resource acrPull 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(containerRegistry.id, managedIdentity.id, 'acrpull')
  scope: containerRegistry
  properties: {
    roleDefinitionId: subscriptionResourceId('Microsoft.Authorization/roleDefinitions', '7f951dda-4ed3-4680-a7ca-43fe172d538d')
    principalId: managedIdentity.properties.principalId
    principalType: 'ServicePrincipal'
  }
}

resource containerAppsEnvironment 'Microsoft.App/managedEnvironments@2024-03-01' = {
  name: containerEnvironmentName
  location: location
  tags: baseTags
  properties: {
    appLogsConfiguration: {
      destination: 'log-analytics'
      logAnalyticsConfiguration: {
        customerId: logAnalytics.properties.customerId
        sharedKey: logAnalytics.listKeys().primarySharedKey
      }
    }
  }
}

resource containerApp 'Microsoft.App/containerApps@2024-03-01' = {
  name: containerAppName
  location: location
  // The `azd-service-name` tag is the contract by which `azd deploy` finds the
  // Container App to update with the freshly built image. Must match the
  // service name in azure.yaml ("web").
  tags: union(baseTags, {
    'azd-service-name': 'web'
  })
  identity: {
    type: 'UserAssigned'
    userAssignedIdentities: {
      '${managedIdentity.id}': {}
    }
  }
  properties: {
    managedEnvironmentId: containerAppsEnvironment.id
    configuration: {
      activeRevisionsMode: 'Single'
      registries: [
        {
          server: containerRegistry.properties.loginServer
          identity: managedIdentity.id
        }
      ]
      ingress: {
        external: true
        targetPort: 8080
        // `auto` lets Container Apps upgrade HTTP/1.1 connections to WebSocket
        // so the Crow server can serve `/api/ws/...` over WSS.
        transport: 'auto'
        allowInsecure: false
        traffic: [
          {
            weight: 100
            latestRevision: true
          }
        ]
      }
    }
    template: {
      containers: [
        {
          name: 'web'
          image: containerImageName
          env: [
            {
              name: 'PORT'
              value: '8080'
            }
            {
              name: 'MAHJONG_WEB_DIR'
              value: '/app/web'
            }
            {
              name: 'APPLICATIONINSIGHTS_CONNECTION_STRING'
              value: appInsights.properties.ConnectionString
            }
            {
              name: 'AZURE_CLIENT_ID'
              value: managedIdentity.properties.clientId
            }
          ]
          resources: {
            cpu: json(containerCpu)
            memory: containerMemory
          }
          probes: [
            {
              type: 'Liveness'
              httpGet: {
                path: '/api/health'
                port: 8080
                scheme: 'HTTP'
              }
              initialDelaySeconds: 30
              periodSeconds: 30
            }
            {
              type: 'Readiness'
              httpGet: {
                path: '/api/health'
                port: 8080
                scheme: 'HTTP'
              }
              initialDelaySeconds: 10
              periodSeconds: 10
            }
          ]
        }
      ]
      scale: {
        minReplicas: minReplicas
        maxReplicas: maxReplicas
        rules: [
          {
            name: 'http-concurrency'
            http: {
              metadata: {
                concurrentRequests: '100'
              }
            }
          }
        ]
      }
    }
  }
  dependsOn: [
    acrPull
  ]
}

// Standard AZD outputs. AZD writes most of these back into .azure/<env>/.env
// so subsequent `azd deploy` invocations can target the right registry/app.
output AZURE_CONTAINER_REGISTRY_ENDPOINT string = containerRegistry.properties.loginServer
output AZURE_CONTAINER_REGISTRY_NAME string = containerRegistry.name
output AZURE_CONTAINER_APP_NAME string = containerApp.name
output AZURE_CONTAINER_APP_ENVIRONMENT_NAME string = containerAppsEnvironment.name
output AZURE_RESOURCE_GROUP string = resourceGroup().name
output APPLICATIONINSIGHTS_CONNECTION_STRING string = appInsights.properties.ConnectionString
output SERVICE_WEB_URI string = 'https://${containerApp.properties.configuration.ingress.fqdn}'
output SERVICE_WEB_NAME string = containerApp.name
