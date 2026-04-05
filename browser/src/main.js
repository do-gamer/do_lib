const {app, BrowserWindow} = require('electron')
const path = require('path')
const net = require('net')
const {initSplashScreen} = require("@trodi/electron-splashscreen")

let mainWindow;

function log(...args) { console.log('[browser]', ...args); }

app.commandLine.appendSwitch('ppapi-flash-path', getFlashPath())

const { handleKeyClick, handleKeyDown, handleKeyUp, handleText } = require('./key_handler');

var server = net.createServer(function (sock) {
    sock.setEncoding('utf8');

    sock.on('data', (data) => {
        if (!mainWindow) {
            log("Received command but mainWindow is not initialized, ignoring");
            return;
        }

        try {
            const obj = JSON.parse(data);
            switch (obj.cmd) {
                case "refresh":
                    log("Received refresh command, reloading...");
                    mainWindow.reload();
                    break;
                case "setSize":
                    // resize the browser window to the given width and height
                    mainWindow.setSize(obj.w, obj.h);
                    break;
                case "keyClick":
                    handleKeyClick(mainWindow.webContents, obj.key);
                    break;
                case "keyDown":
                    handleKeyDown(mainWindow.webContents, obj.key);
                    break;
                case "keyUp":
                    handleKeyUp(mainWindow.webContents, obj.key);
                    break;
                case "text":
                    handleText(mainWindow.webContents, obj.text);
                    break;
            }
        } catch (e) {
            log("Failed to parse command:", data, e);
            return;
        }

        // Send acknowledgment back to the sender.
        sock.write(data + "|ok");
    });

    sock.on('error', (err) => {
        log("Socket error", err);
    });

    sock.on('close', (hadError) => {
        log("Socket closed" + (hadError ? " (error)" : ""));
        // client may reconnect later; the server stays listening and will emit
        // a new connection event when that happens.  nothing to do here other
        // than logging/debugging.
    });
});
server.listen("/tmp/darkbot_ipc_" + process.pid);

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

    window.webContents.userAgent = 'BigpointClient/1.6.7'
    window.webContents.on('new-window', (event, url) => {
        event.preventDefault()
        window.loadURL(url)
    })

    window.on('page-title-updated', (evt) => {
        evt.preventDefault();
    });

    window.webContents.on('before-input-event', (event, input) => {
        let focus = () => BrowserWindow.getFocusedWindow();

        if (!focus() || input.type != "keyUp") {
            return;
        }
    });

    log(url, sid, launchGame);
    if (url && sid) {
        window.webContents.session.cookies.set({url: url, name: 'dosid', value: sid})
            .then(() => window.loadURL(url + '/indexInternal.es?action=' + ((launchGame) ? 'internalMapRevolution ': 'internalStart')))
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
    let url, sid, apiVersion, launchGame = false

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
        }
    }

    return {url, sid, apiVersion, launchGame};
}

function getFlashPath() {
    app.commandLine.appendSwitch("--no-sandbox")
    return path.join(process.resourcesPath.split("/")[1] === "tmp" ? process.resourcesPath : app.getAppPath(), './res/linux/libpepflashplayer.so');
}
