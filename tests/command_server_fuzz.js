// Fuzzes the production browser command server (browser/src/command_server.js):
// garbage, oversized lines, byte-by-byte splits, parallel clients, abrupt disconnects.
// The server must stay alive, bounded in memory, and keep answering valid commands.
const net = require('net')
const path = require('path')
const crypto = require('crypto')
const { createCommandServer } = require(path.join(__dirname, '..', 'browser', 'src', 'command_server.js'))

const ipcPath = '/tmp/darktanos_fuzz_' + process.pid
let executed = 0
createCommandServer(ipcPath, (obj) => { executed++; return obj.cmd !== 'fail' }, () => {})

function client(fn) {
    return new Promise((resolve) => {
        const s = net.connect(ipcPath, () => fn(s, resolve))
        s.on('error', () => resolve())
    })
}

function valid(s, id) {
    return new Promise((resolve, reject) => {
        let buf = ''
        const timer = setTimeout(() => reject(new Error('no ack for ' + id)), 2000)
        const onData = (d) => {
            buf += d
            if (buf.includes(id + '|ok\n')) { clearTimeout(timer); s.removeListener('data', onData); resolve() }
        }
        s.on('data', onData)
        s.write(JSON.stringify({ id, cmd: 'keyClick', key: 65 }) + '\n')
    })
}

async function main() {
    await new Promise(r => setTimeout(r, 200))
    const rss0 = process.memoryUsage().rss
    const t0 = Date.now()

    const jobs = []
    for (let c = 0; c < 20; c++) {
        jobs.push(client((s, done) => {
            let n = 0
            const step = () => {
                if (n++ > 200) { s.destroy(); return done() }
                const r = Math.random()
                if (r < 0.3) s.write(crypto.randomBytes(1 + Math.floor(Math.random() * 512)))
                else if (r < 0.4) s.write('{"id":' + n + ',"cmd":"x"'.repeat(1000))          // no newline, grows buffer
                else if (r < 0.5) s.write('\n\n\n{}\n[]\nnull\n"str"\n123\n')               // valid JSON, not objects
                else if (r < 0.6) { const m = JSON.stringify({ id: n, cmd: 'keyUp', key: 66 }) + '\n'; for (const ch of m) s.write(ch) }
                else if (r < 0.65) s.write('{"id":' + n + ',"cmd":"fail"}\n')
                else if (r < 0.7) s.write(Buffer.from([0xff, 0xfe, 0x00, 0x0a]))           // invalid utf8 + newline
                else s.write(JSON.stringify({ id: n, cmd: 'keyDown', key: 67, junk: 'x'.repeat(Math.floor(Math.random() * 5000)) }) + '\n')
                setImmediate(step)
            }
            step()
        }))
    }
    // a huge line without newline, then disconnect
    jobs.push(client((s, done) => { s.write('a'.repeat(3 * 1024 * 1024)); setTimeout(() => { s.destroy(); done() }, 300) }))
    await Promise.all(jobs)

    // server still healthy?
    await client(async (s, done) => {
        try { for (let i = 1; i <= 50; i++) await valid(s, 900000 + i); console.log('PASS server answers valid commands after fuzzing') }
        catch (e) { console.log('FAIL ' + e.message); process.exitCode = 1 }
        s.destroy(); done()
    })
    if (global.gc) global.gc()
    const growth = (process.memoryUsage().rss - rss0) >> 20
    console.log((growth < 100 ? 'PASS' : 'FAIL') + ' memory growth ' + growth + ' MB, ' + executed + ' commands executed in ' + (Date.now() - t0) + ' ms')
    if (growth >= 100) process.exitCode = 1
    process.exit()
}
main()
