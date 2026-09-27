// Command server for the native client (DarkTanos.so).
// Messages are newline delimited JSON ({"id":N,"cmd":...}); every message is answered
// with "N|ok" or "N|err" so the client never waits for a timeout on failures.
// Kept free of Electron APIs so it can be tested with plain Node.
const net = require('net')
const fs = require('fs')

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

                let id = 0, ok = false;
                try {
                    const obj = JSON.parse(line);
                    id = obj.id || 0;
                    ok = handleCommand(obj) === true;
                } catch (e) {
                    log("Failed to handle command:", line, e);
                }

                // Send acknowledgment back to the sender.
                if (!sock.destroyed) sock.write(id + (ok ? "|ok\n" : "|err\n"));
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
