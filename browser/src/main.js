const {app, BrowserWindow} = require('electron')
const path = require('path')
const fs = require('fs')
const {initSplashScreen} = require("@trodi/electron-splashscreen")

let mainWindow;
// time of the last "refresh" command: the native client kills the old flash process right
// after it for a faster reload, which Electron reports as a plugin crash
let lastRefreshTime = 0;

function log(...args) { console.log('[browser]', ...args); }

app.commandLine.appendSwitch('ppapi-flash-path', getFlashPath())

// Keep the game running at full speed while the window is hidden, unmapped or covered;
// otherwise Chromium throttles timers/rendering and the bot tick increases.
app.commandLine.appendSwitch('disable-background-timer-throttling')
app.commandLine.appendSwitch('disable-renderer-backgrounding')
app.commandLine.appendSwitch('disable-backgrounding-occluded-windows')

const { handleKeyClick, handleKeyDown, handleKeyUp, handleText } = require('./key_handler');
const { createCommandServer } = require('./command_server');

/**
 * Executes one command, returns true on success.
 */
function handleCommand(obj) {
    if (!mainWindow || mainWindow.isDestroyed()) {
        log("Received command but mainWindow is not initialized, ignoring");
        return false;
    }

    switch (obj.cmd) {
        case "refresh":
            log("Received refresh command, reloading...");
            lastRefreshTime = Date.now();
            mainWindow.reload();
            return true;
        case "setSize":
            // resize the browser window to the given width and height
            mainWindow.setSize(obj.w, obj.h);
            return true;
        case "keyClick":
            handleKeyClick(mainWindow.webContents, obj.key);
            return true;
        case "keyDown":
            handleKeyDown(mainWindow.webContents, obj.key);
            return true;
        case "keyUp":
            handleKeyUp(mainWindow.webContents, obj.key);
            return true;
        case "text": {
            // with "wait" the answer comes after the last character was typed (text paste)
            const typing = handleText(mainWindow.webContents, obj.text);
            return obj.wait ? typing.then(() => true) : true;
        }
        default:
            log("Unknown command:", obj.cmd);
            return false;
    }
}

createCommandServer(parseArgv().ipcPath || ("/tmp/darkbot_ipc_" + process.pid), handleCommand, log);

// Close together with the bot, also when it's killed or crashes (otherwise the browser
// keeps running in the background with a live game session).
const parentPid = parseInt(parseArgv().parentPid || '0', 10);
if (parentPid > 0) {
    setInterval(() => {
        try {
            process.kill(parentPid, 0);
        } catch (e) {
            if (e.code === 'ESRCH') {
                log("Bot process", parentPid, "is gone, exiting");
                app.exit(0);
            }
        }
    }, 2000).unref();
}

function createWindow(url, sid, apiVersion, launchGame = false) {
    let icon = path.join(process.resourcesPath, "res", "icon.png")

    let window = initSplashScreen({
        windowOpts: {
            width: 1400,
            height: 900,
            icon: icon,
            show: false,
            darkTheme: true,
            autoHideMenuBar: true,
            title: "DarkBot Browser" + (apiVersion ? ` [Tanos v${apiVersion}]` : ""),
            webPreferences: {
                plugins: true,
                sandbox: false,
                contextIsolation: true,
                nodeIntegration: false,
                enableRemoteModule: false,
                // don't throttle timers/animations when the window is hidden or in background
                backgroundThrottling: false,
                spellcheck: false,
                preload: path.join(__dirname, 'preload.js')
            }
        },
        templateUrl: `${__dirname}/splash.html`,
        splashScreenOpts: {
            width: 300,
            height: 300,
            frame: true,
            alwaysOnTop: true,
            webPreferences: {
                contextIsolation: true,
                nodeIntegration: false,
                enableRemoteModule: false
            }
        },
        minVisible: 0,
        delay: 0
    })

    window.webContents.userAgent = 'BigpointClient/1.6.9'
    window.webContents.on('new-window', (event, url) => {
        event.preventDefault()
        window.loadURL(url)
    })

    window.on('page-title-updated', (evt) => {
        evt.preventDefault();
    });

    window.on('minimize', (event) => {
        event.preventDefault();
        window.restore();
    });

    window.on('close', (event) => {
        event.preventDefault();
    });

    // Recovery: a crashed renderer shows a dead page forever, reload it. Flash plugin
    // crashes are detected and handled by the native client (it refreshes).
    window.webContents.on('render-process-gone', (event, details) => {
        log("Renderer process gone:", details.reason, "exit code", details.exitCode);
        if (details.reason !== 'clean-exit' && !window.isDestroyed()) {
            setTimeout(() => { if (!window.isDestroyed()) window.reload(); }, 1000);
        }
    });

    window.webContents.on('plugin-crashed', (event, name, version) => {
        // fires for any pepper plugin (Flash, but e.g. also Chromium's PDF viewer); right after a
        // refresh it's expected (the client kills the old plugin process), so stay quiet then
        if (Date.now() - lastRefreshTime >= 5000) {
            log("Plugin crashed:", name, version);
        }
    });

    // Page load diagnostics and recovery (server lag / outage): log failed, HTTP error and slow
    // main page loads, and retry failed loads with a growing delay instead of leaving a dead
    // page until the bot's stuck timer (~150 s) refreshes. Loads that are slow but progressing
    // are left alone, so a lagging server doesn't get its load restarted over and over.
    const RETRY_MIN_MS = 10000, RETRY_MAX_MS = 30000, SLOW_LOAD_MS = 20000;
    const shortUrl = (u) => { try { const p = new URL(u); return p.origin + p.pathname; } catch (e) { return String(u); } };
    let loadStart = 0, lastHttpCode = 0, loadFailed = false, retryDelay = 0, retryTimer = null;

    const scheduleRetry = (failedUrl, reason) => {
        if (retryTimer) clearTimeout(retryTimer);
        retryDelay = retryDelay ? Math.min(retryDelay * 2, RETRY_MAX_MS) : RETRY_MIN_MS;
        log(`Page load failed: ${reason} (${shortUrl(failedUrl)}), retrying in ${retryDelay / 1000} s`);
        retryTimer = setTimeout(() => {
            retryTimer = null;
            if (!window.isDestroyed()) window.loadURL(failedUrl);
        }, retryDelay);
    };

    window.webContents.on('did-start-loading', () => {
        loadStart = Date.now();
        loadFailed = false;
        // a new load (refresh, retry) supersedes a pending retry
        if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
    });

    window.webContents.on('did-navigate', (event, navUrl, httpCode, httpStatus) => {
        lastHttpCode = httpCode;
        if (httpCode >= 500) scheduleRetry(navUrl, `HTTP ${httpCode} ${httpStatus || ''}`.trim());
        else if (httpCode >= 400) log(`Page load HTTP ${httpCode} ${httpStatus || ''} (${shortUrl(navUrl)})`);
    });

    window.webContents.on('did-fail-load', (event, errorCode, errorDescription, failedUrl, isMainFrame) => {
        // -3 = ERR_ABORTED: replaced by a newer navigation (e.g. a refresh), not a failure
        if (!isMainFrame || errorCode === -3) return;
        loadFailed = true;
        scheduleRetry(failedUrl, `${errorDescription} (${errorCode})`);
    });

    window.webContents.on('did-finish-load', () => {
        // Chromium's error page (after did-fail-load) or an HTTP 5xx page also "finish loading";
        // the retry is already scheduled
        if (loadFailed || lastHttpCode >= 500) return;
        const took = Date.now() - loadStart;
        if (retryDelay) log(`Page loaded after retrying (${(took / 1000).toFixed(1)} s)`);
        else if (took > SLOW_LOAD_MS) log(`Page loaded slowly: ${(took / 1000).toFixed(1)} s`);
        retryDelay = 0;
    });

    window.on('unresponsive', () => log("Window unresponsive"));
    window.on('responsive', () => log("Window responsive again"));

    log(url, launchGame); // sid is a session credential, keep it out of the logs
    if (url && sid) {
        window.webContents.session.cookies.set({url: url, name: 'dosid', value: sid})
            .then(() => window.loadURL(url + '/indexInternal.es?action=' + ((launchGame) ? 'internalMapRevolution' : 'internalStart')))
    } else {
        window.loadURL('https://darkorbit.com')
        //window.loadFile(path.join(__dirname, 'index.html'))
    }
    return window;
}

function createMainWindow() {
    const {url, sid, apiVersion, launchGame} = parseArgv();
    mainWindow = createWindow(url, sid, apiVersion, launchGame);
}

app.whenReady().then(() => {
    createMainWindow();

    app.on('activate', function () {
        if (BrowserWindow.getAllWindows().length === 0) {
            createMainWindow();
        }
    })
})

app.on('window-all-closed', function () {
    app.quit()
})

function parseArgv() {
    let url, sid, apiVersion, ipcPath, parentPid, launchGame = false

    for (let i = 1; i < process.argv.length; i++) {
        const arg = process.argv[i]

        if (arg === '--launch') {
            launchGame = true
            continue
        }

        const [key, value] = arg.split('=', 2)
        switch (key) {
            case '--url':
                url = value
                break
            case '--sid':
                sid = value
                break
            case '--api-version':
                apiVersion = value
                break
            case '--ipc-path':
                ipcPath = value
                break
            case '--parent-pid':
                parentPid = value
                break
        }
    }

    return {url, sid, apiVersion, ipcPath, parentPid, launchGame};
}

function getFlashPath() {
    app.commandLine.appendSwitch("no-sandbox")

    // Packaged (AppImage, mounted or extracted anywhere, whatever TMPDIR is): extraResources
    // live next to app.asar. Development (npm start): relative to the app directory.
    const packaged = path.join(process.resourcesPath, 'res', 'linux', 'libpepflashplayer.so');
    if (fs.existsSync(packaged)) return packaged;
    return path.join(app.getAppPath(), 'res', 'linux', 'libpepflashplayer.so');
}
