// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
import {
  FileHelper, IMPOSSIBLE, StartSdk, VersionGraph, VersionInfo,
  buildManifest, setupManifest, utils,
} from '@start9labs/start-sdk'

const packageDefinition = setupManifest({
  id: 'dispatch-preview',
  title: 'DISPATCH Preview',
  license: 'LicenseRef-Spectre-Dispatch-1.0',
  packageRepo: 'https://github.com/Spectre-Works/dispatch',
  upstreamRepo: 'https://github.com/Spectre-Works/dispatch',
  marketingUrl: 'https://spectreworks.io/',
  donationUrl: null,
  description: {
    short: { en_US: 'Preview a mining operating plan with live equipment readings.' },
    long: { en_US: 'Pair LAN equipment, review proposed actions and their reasons, and export an operating plan for DISPATCH/01. The service observes equipment without controlling it.' },
  },
  volumes: ['data'],
  images: {
    dispatch: {
      source: { dockerBuild: {
        workdir: '../..',
        dockerfile: '../../packaging/startos/Dockerfile',
        buildArgs: {
          DISPATCH_VERSION: { env: 'DISPATCH_PACKAGE_RELEASE_VERSION' },
        },
      } },
      arch: ['x86_64', 'aarch64'],
      emulateMissing: false,
    },
  },
})

const sdk = StartSdk.of().withManifest(packageDefinition).build(true)
const dependencies = sdk.Dependencies.of()
const current = VersionInfo.of({
  version: '1.0.0:0', // Replaced by the release packager in the archived checkout.
  releaseNotes: { en_US: 'Run DISPATCH Preview with live equipment on your StartOS node.' },
  migrations: { down: IMPOSSIBLE },
})
const versionGraph = VersionGraph.of({ current, other: [] })

const executable = '/dispatch-preview'
const port = 8080
const ownerKey = FileHelper.string({ base: sdk.volumes.data, subpath: 'owner.key' })

const showOwnerKey = sdk.Action.withoutInput(
  'show-owner-key',
  async () => ({
    name: 'Show Owner Key',
    description: 'Read the owner key for the DISPATCH dashboard.',
    allowedStatuses: 'any',
    visibility: 'enabled',
    access: 'user',
    group: null,
  }),
  async () => {
    const key = await ownerKey.read().once()
    if (!key || !/^[a-f0-9]{32}$/.test(key)) throw new Error('The DISPATCH owner key is unavailable.')
    return {
      version: '1',
      title: 'DISPATCH Owner Key',
      message: 'Enter this key on the DISPATCH dashboard.',
      result: { type: 'single', value: key, masked: true, copyable: true, qr: false, launchable: false },
    }
  },
)
export const actions = sdk.Actions.of().addAction(showOwnerKey)

const seedOwnerKey = sdk.setupOnInit(async (effects, kind) => {
  if (kind !== 'install' || await ownerKey.read().once()) return
  await ownerKey.write(effects, utils.getDefaultString({ charset: 'a-f,0-9', len: 32 }))
})

const setInterfaces = sdk.setupInterfaces(async ({ effects }) => {
  const host = sdk.MultiHost.of(effects, 'dashboard')
  const origin = await host.bindPort(port, { protocol: 'http', preferredExternalPort: 443 })
  const dashboard = sdk.createInterface(effects, {
    name: 'DISPATCH Dashboard',
    id: 'dashboard',
    description: 'Read equipment and review proposed actions.',
    type: 'ui',
    masked: false,
    schemeOverride: null,
    username: null,
    path: '',
    query: {},
  })
  return [await origin.export([dashboard])]
})

export const main = sdk.setupMain(async ({ effects }) => {
  const service = sdk.SubContainer.of(effects, { imageId: 'dispatch' },
    sdk.Mounts.of().mountVolume({ volumeId: 'data', subpath: null, readonly: false, mountpoint: '/data' }),
    'preview')
  return sdk.Daemons.of(effects).addDaemon('preview', {
    subcontainer: service,
    exec: { command: [executable, '/data', String(port)] },
    ready: {
      display: 'Dashboard and observation worker',
      fn: () => sdk.healthCheck.runHealthScript([executable, '--health'], service, {
        timeout: 3000,
        errorMessage: 'The DISPATCH service is unavailable.',
        message: () => 'The dashboard and observation worker are ready.',
      }),
    },
    requires: [],
  })
})

const { createBackup, restoreInit } = sdk.setupBackups(async () => sdk.Backups.ofVolumes('data'))
export { createBackup }
export const init = sdk.setupInit(restoreInit, versionGraph, setInterfaces, actions, dependencies, seedOwnerKey)
export const uninit = sdk.setupUninit(versionGraph)
export const manifest = buildManifest(versionGraph, packageDefinition, dependencies)
