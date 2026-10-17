// Command server for the native client (DarkTanos.so).
// Messages are newline delimited JSON ({"id":N,"cmd":...}); every message is answered
// with "N|ok", "N|ok|<value>" or "N|err" so the client never waits for a timeout on failures.
// A handler returns true (ok), a number or string (ok with that value) or anything else (err),
// or a Promise of one; the answer is then sent once it settles.
// Kept free of Electron APIs so it can be tested with plain Node.
const net = require('net')
const fs = require('fs')

function replyFor(result) {
    if (result === true) return "|ok";
    // a newline in the value would break the line framing
    if (typeof result === 'number' || typeof result === 'string') return "|ok|" + String(result).replace(/[\r\n]/g, ' ');
    return "|err";
}

function createCommandServer(ipcPath, handleCommand, log) {
    const server = net.createServer(function (sock) {
        sock.setEncoding('utf8');
        let buffer = '';

        sock.on('data', (chunk) => {
            buffer += chunk;

            let newline;
            while ((newline = buffer.indexOf('\n')) >= 0) {
                const line = buffer.slice(0, newline);
                buffer = buffer.slice(newline + 1);
                if (!line) continue;

                let id = 0, result = false;
                try {
                    const obj = JSON.parse(line);
                    id = obj.id || 0;
                    result = handleCommand(obj);
                } catch (e) {
                    log("Failed to handle command:", line, e);
                }

                // Send acknowledgment back to the sender (after completion for async commands).
                const ack = (res) => { if (!sock.destroyed) sock.write(id + replyFor(res) + "\n"); };
                if (result instanceof Promise) {
                    result.then(ack, (e) => { log("Command failed:", line, e); ack(false); });
                } else {
                    ack(result);
                }
            }

            // protect against a peer that never sends newlines
            if (buffer.length > 1024 * 1024) buffer = '';
        });

        sock.on('error', (err) => {
            log("Socket error", err);
        });

        sock.on('close', (hadError) => {
            log("Socket closed" + (hadError ? " (error)" : ""));
            // client may reconnect later; the server stays listening and will emit
            // a new connection event when that happens.
        });
    });

    // a socket file left behind by a killed browser would make listen() fail
    try { fs.unlinkSync(ipcPath); } catch (e) { /* no stale socket */ }

    server.on('error', (err) => log("IPC server error", err));
    server.listen(ipcPath, () => log("IPC server listening on", ipcPath));

    const cleanup = () => {
        try { server.close(); fs.unlinkSync(ipcPath); } catch (e) { /* already gone */ }
    };
    process.on('exit', cleanup);

    return server;
}

module.exports = { createCommandServer };
