/*
 * Voxel Launcher —— Tauri 前端适配层
 *
 * 职责：替代 Electron 的 preload.cjs，把 `window.voxel` 的方法名、参数顺序和返回形状
 *      原样保留（对齐 preload.cjs 的 34 个方法），内部改用 Tauri 的 invoke / listen。
 *      因此 renderer 侧的 app.js 一行都不用改，重构的爆炸半径被限制在这一层以下。
 *
 * 设计约束：
 *  1. 所有真实调用都走 invoke，Rust 是唯一数据源，这里不做业务计算。
 *  2. 后端尚未实现的命令以"安全兜底值" resolve，而不是 reject —— 旧 preload 的语义是
 *     invoke 失败即抛错，但骨架期大量命令还没接线，一旦抛错 app.js 会整体停在 boot 遮罩。
 *     兜底让 UI 先站起来，同时把缺失的命令打日志，便于逐模块补齐。
 *  3. 事件沿用旧名字（game:event / window:state），payload 形状不变。
 */
(function () {
  'use strict'

  var GAME_EVENTS = new Set([
    'state',
    'phase',
    'progress',
    'log',
    'error',
    'closed',
    'crash',
    'cancelled'
  ])

  var tauri = window.__TAURI__ || null
  var core = tauri && tauri.core ? tauri.core : null
  var eventApi = tauri && tauri.event ? tauri.event : null

  /* 与 config-store.js 的 DEFAULT_SETTINGS 保持逐字段一致，保证首次渲染不崩。 */
  var DEFAULT_SETTINGS = {
    username: '',
    version: '',
    loader: 'vanilla',
    loaderVersion: '',
    minMemory: 1024,
    maxMemory: 4096,
    windowWidth: 1280,
    windowHeight: 720,
    fullscreen: false,
    javaMode: 'auto',
    javaPath: '',
    jvmArgs: '',
    uiScale: 100,
    fontScale: 100,
    accentColor: '#eef0ea',
    themeMode: 'dark',
    gameDir: '',
    proxyUrl: '',
    mirrorMojang: true,
    mirrorFabric: true,
    mirrorForge: true,
    mirrorNeoForge: true,
    forceLocal: false,
    versionIsolation: false,
    downloadSource: '',
    lastPlayed: ''
  }

  /** 未接线命令的兜底返回值，按命令名分组。 */
  var FALLBACKS = {
    app_bootstrap: {
      settings: DEFAULT_SETTINGS,
      account: null,
      gameState: { status: 'idle', current: null, account: null },
      appInfo: {
        version: '0.1.0',
        emlVersion: '—',
        electron: '—',
        chrome: '—',
        node: '—',
        platform: 'win32',
        arch: 'x64',
        gameDir: '',
        defaultGameDir: '',
        gameDirIsCustom: false,
        versionsDir: '',
        instanceDir: '',
        dataDir: ''
      }
    },
    versions_list: { latest: { release: '', snapshot: '' }, versions: [], source: 'local', error: null },
    loaders_list: {
      vanilla: { type: 'vanilla', label: '原版', version: '', detail: '', available: true }
    },
    mods_search: { hits: [], total: 0, page: 1, pageSize: 20 },
    mods_installed: [],
    game_state: { status: 'idle', current: null, account: null },
    settings_save: DEFAULT_SETTINGS,
    ui_set_scale: 100
  }

  function fallbackFor(command) {
    return Object.prototype.hasOwnProperty.call(FALLBACKS, command) ? FALLBACKS[command] : null
  }

  /**
   * 统一的 invoke 包装。
   *
   * 关键：旧 main.js 的 `handle()` 把所有 IPC 结果包成 `{ ok, data }` / `{ ok, error }`，
   * 而 app.js 的 `invoke()` 会先 `unwrap()` 再取 `result.data`。所以这一层必须原样
   * 复刻那个信封 —— 否则前端拿到的是一坨裸数据，`result.ok` 为 undefined，
   * 直接抛"操作失败。"，整个初始化卡在 boot 遮罩上。
   *
   * 命令未接线时的处理：有兜底值就当作成功返回（让 UI 先站起来），
   * 没有兜底值就返回 `{ ok: false }`，让前端弹出明确提示而不是静默失效。
   */
  function call(command, args) {
    if (!core || typeof core.invoke !== 'function') {
      console.warn('[voxel] Tauri 未注入（window.__TAURI__ 缺失）')
      return Promise.resolve({ ok: true, data: fallbackFor(command) })
    }
    var payload = args === undefined ? {} : args
    return core
      .invoke(command, payload)
      .then(function (value) {
        return { ok: true, data: value }
      })
      .catch(function (error) {
        console.warn('[voxel] 命令未实现或执行失败：', command, error)
        if (Object.prototype.hasOwnProperty.call(FALLBACKS, command)) {
          return { ok: true, data: FALLBACKS[command], fallback: true }
        }
        return { ok: false, error: '该功能尚未接入 Rust 后端（' + command + '）' }
      })
  }

  /* ---------------------------- 事件订阅 ---------------------------- */

  function listen(name, handler) {
    if (!eventApi || typeof eventApi.listen !== 'function') return function () {}
    var stop = null
    eventApi
      .listen(name, function (e) {
        handler(e && e.payload)
      })
      .then(function (unlisten) {
        stop = unlisten
      })
      .catch(function (error) {
        console.warn('[voxel] 事件订阅失败：', name, error)
      })
    return function () {
      if (stop) stop()
    }
  }

  /* ------------------------- 对外暴露的 API ------------------------- */

  var api = {
    platform: 'win32',

    /* 应用与配置 */
    bootstrap: function () {
      return call('app_bootstrap')
    },
    getInfo: function () {
      return call('app_info')
    },
    saveSettings: function (patch) {
      return call('settings_save', { patch: patch })
    },
    setUiScale: function (value) {
      return call('ui_set_scale', { value: value })
    },

    /* 版本与加载器 */
    getVersions: function (force) {
      return call('versions_list', { force: Boolean(force) })
    },
    getLoaders: function (mcVersion, versionType) {
      return call('loaders_list', { mcVersion: mcVersion, versionType: versionType })
    },
    installLoader: function (payload) {
      return call('loader_install', { payload: payload })
    },
    deleteVersion: function (versionId) {
      return call('versions_delete', { versionId: versionId })
    },
    openVersionFolder: function (versionId) {
      return call('versions_open_folder', { versionId: versionId })
    },

    /* 模组 */
    searchMods: function (payload) {
      return call('mods_search', { payload: payload })
    },
    installMod: function (payload) {
      return call('mods_install', { payload: payload })
    },
    chooseModDir: function (currentDir) {
      return call('mods_choose_dir', { currentDir: currentDir })
    },
    getModVersions: function (payload) {
      return call('mods_versions', { payload: payload })
    },
    getModIcon: function (payload) {
      return call('mods_icon', { payload: payload })
    },
    getInstalledMods: function () {
      return call('mods_installed')
    },

    /* 账户 */
    loginOffline: function (username) {
      return call('auth_offline', { username: username })
    },
    loginMicrosoft: function () {
      return call('auth_microsoft')
    },
    loginYggdrasil: function (credentials) {
      return call('auth_yggdrasil', { credentials: credentials })
    },
    logout: function () {
      return call('auth_logout')
    },
    getSkin: function () {
      return call('skin_get')
    },
    getAccountFace: function () {
      return call('skin_face')
    },
    uploadSkin: function () {
      return call('skin_upload')
    },
    clearSkin: function () {
      return call('skin_clear')
    },

    /* 启动与校验 */
    launch: function () {
      return call('game_launch')
    },
    verifyFiles: function (versionId) {
      return call('game_verify', { versionId: versionId })
    },
    cancelLaunch: function () {
      return call('game_cancel')
    },
    getGameState: function () {
      return call('game_state')
    },

    /* 目录与系统 */
    chooseJava: function () {
      return call('dialog_choose_java')
    },
    chooseGameDir: function () {
      return call('directories_choose_game')
    },
    setGameDir: function (value) {
      return call('directories_set_game_dir', { value: value })
    },
    openPath: function (target) {
      return call('app_open_path', { target: target })
    },

    /* 窗口控制 */
    minimizeWindow: function () {
      return call('window_minimize')
    },
    toggleMaximizeWindow: function () {
      return call('window_toggle_maximize')
    },
    toggleFullScreen: function () {
      return call('window_toggle_fullscreen')
    },
    closeWindow: function () {
      return call('window_close')
    },

    onWindowState: function (callback) {
      return listen('window:state', function (payload) {
        if (payload) callback(payload)
      })
    },

    onGameEvent: function (callback) {
      return listen('game:event', function (payload) {
        if (!payload || !GAME_EVENTS.has(payload.type)) return
        callback(payload)
      })
    }
  }

  Object.defineProperty(window, 'voxel', {
    value: api,
    writable: false,
    configurable: false,
    enumerable: true
  })
})()
