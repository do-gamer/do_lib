const {app, BrowserWindow} = require('electron')
const path = require('path')
const net = require('net')
const {initSplashScreen} = require("@trodi/electron-splashscreen")

let mainWindow;

app.commandLine.appendSwitch('ppapi-flash-path', getFlashPath())

const { handleKeyClick, handleKeyDown, handleKeyUp, typeText } = require('./key_handler');

var server = net.createServer(function (sock) {
    sock.setEncoding('utf8');

    sock.on('data', async (data) => {
        let args = data.split("|");

        if (args.length == 0 || !mainWindow)
        {
            return;
        }

        if (args[0] == "refresh") {
           mainWindow.reload();
        } else if (args.length == 2) {
            switch (args[0]) {
                case "keyClick":
                    handleKeyClick(mainWindow.webContents, args[1]);
                    break
                case "keyDown":
                    handleKeyDown(mainWindow.webContents, args[1]);
                    break;
                case "keyUp":
                    handleKeyUp(mainWindow.webContents, args[1]);
                    break;
                case "text":
                    await typeText(mainWindow.webContents, args[1]);
                    break;
            }
        }
    });

    sock.on('error', (err) => {
        console.log("Socket erro", err);
    }); 
    sock.pipe(sock);
});
server.listen("/tmp/darkbot_ipc_" + process.pid);

function createWindow(url, sid, launchGame = false) {
    let icon = path.join(process.resourcesPath, "res", "icon.png")

    let window = initSplashScreen({
        windowOpts: {
            width: 1400,
            height: 900,
            icon: icon,
            show: false,
            darkTheme: true,
            autoHideMenuBar: true,
            title: "DarkBot Browser v" + app.getVersion(),
            webPreferences: {
                plugins: true,
                sandbox: false,
                // nodeIntegration: true,
                // contextIsolation: false,
                // enableRemoteModule: true,
                preload: path.join(__dirname, 'preload.js')
            }
        },
        templateUrl: `${__dirname}/splash.html`,
        splashScreenOpts: {
            width: 300,
            height: 300,
            transparent: true,
            alwaysOnTop: true
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

        if (input.control && input.code === "KeyN") {
            createWindow(url, sid);
        }
    });

    console.log(url, sid, launchGame);
    if (url && sid) {
        window.webContents.session.cookies.set({url: url, name: 'dosid', value: sid})
            .then(() => window.loadURL(url + '/indexInternal.es?action=' + ((launchGame) ? 'internalMapRevolution ': 'internalStart')))
    } else {
        window.loadURL('https://darkorbit.com')
        //window.loadFile(path.join(__dirname, 'index.html'))
    }
    return window;
}

app.whenReady().then(() => {
    const {url, sid, launchGame} = parseArgv();
    mainWindow = createWindow(url, sid, launchGame)

    app.on('activate', function () {
        if (BrowserWindow.getAllWindows().length === 0) {
            const {url, sid, launchGame} = parseArgv();
            mainWindow = createWindow(url, sid, launchGame)
        }
    })
})

app.on('window-all-closed', function () {
    app.quit()
})

function parseArgv() {
    let url, sid, launchGame = false
    for (let i = 0; i < process.argv.length; i++) {
        switch (process.argv[i]) {
            case '--url':
                url = process.argv[++i]
                break
            case '--sid':
                sid = process.argv[++i]
                break;
            case '--launch':
                launchGame = true
                break;
        }
    }
    return {url, sid, launchGame};
}

function getFlashPath() {
    app.commandLine.appendSwitch("--no-sandbox")
    return path.join(process.resourcesPath.split("/")[1] === "tmp" ? process.resourcesPath : app.getAppPath(), './res/linux/libpepflashplayer.so');
}
