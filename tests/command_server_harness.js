// Runs the production browser command server (browser/src/command_server.js) with a
// fake command handler, for tests/sock_ipc_test.cpp. Plain Node, no Electron.
const path = require('path')
const { createCommandServer } = require(path.join(__dirname, '..', 'browser', 'src', 'command_server.js'))

const ipcPath = process.argv[2]
let executed = 0

createCommandServer(ipcPath, (obj) => {
    switch (obj.cmd) {
        case 'fail':
            return false
        case 'block': {
            // simulates a busy Electron main thread
            const until = Date.now() + obj.ms
            while (Date.now() < until) { }
            return true
        }
        case 'async':
            // like "text" with wait: answered once the work is done
            return new Promise((resolve) => setTimeout(() => resolve(true), obj.ms))
        case 'asyncfail':
            return Promise.reject(new Error('failed'))
        case 'count':
            process.stdout.write('executed=' + executed + '\n')
            return true
        default:
            executed++
            return true
    }
}, () => {})
