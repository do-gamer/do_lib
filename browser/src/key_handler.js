// utilities for translating numeric codes into Electron input events
// this module lives alongside main.js and encapsulates all key-related logic.

// Windows VK / DOM numeric keyCode → Electron key string
const specialKeys = {
    // Control keys
    8: 'Backspace',
    9: 'Tab',
    13: 'Enter',
    16: 'Shift',
    17: 'Control',
    18: 'Alt',
    19: 'Pause',
    20: 'CapsLock',
    27: 'Escape',
    32: 'Space',

    // Navigation
    33: 'PageUp',
    34: 'PageDown',
    35: 'End',
    36: 'Home',
    37: 'ArrowLeft',
    38: 'ArrowUp',
    39: 'ArrowRight',
    40: 'ArrowDown',
    45: 'Insert',
    46: 'Delete',

    // Meta keys
    91: 'Meta',       // Left Windows / Command
    92: 'Meta',       // Right Windows / Command
    93: 'ContextMenu',

    // Numpad digits
    96: 'Numpad0',
    97: 'Numpad1',
    98: 'Numpad2',
    99: 'Numpad3',
    100: 'Numpad4',
    101: 'Numpad5',
    102: 'Numpad6',
    103: 'Numpad7',
    104: 'Numpad8',
    105: 'Numpad9',

    // Numpad operations
    106: 'NumpadMultiply',
    107: 'NumpadAdd',
    109: 'NumpadSubtract',
    110: 'NumpadDecimal',
    111: 'NumpadDivide',
    108: 'NumpadEnter',

    // Function keys
    112: 'F1',
    113: 'F2',
    114: 'F3',
    115: 'F4',
    116: 'F5',
    117: 'F6',
    118: 'F7',
    119: 'F8',
    120: 'F9',
    121: 'F10',
    122: 'F11',
    123: 'F12',

    // Lock keys
    144: 'NumLock',
    145: 'ScrollLock'
};

const oemKeys = {
    186: ';',
    187: '=',
    188: ',',
    189: '-',
    190: '.',
    191: '/',
    192: '`',
    219: '[',
    220: '\\',
    221: ']',
    222: "'"
};

/**
 * Resolves a numeric key code to an Electron key string.
 */
function resolveKey(code) {
    if (specialKeys[code]) return specialKeys[code];
    if (oemKeys[code]) return oemKeys[code];

    return String.fromCharCode(code);
}

/**
 * Dispatches key events based on the provided code and action (press/release).
 */
function dispatchKeyEvent(webContents, code, press, release) {
    if (!webContents.isFocused()) {
        webContents.focus();
    }

    const keyCode = resolveKey(code);

    if (press) {
        webContents.sendInputEvent({ type: 'keyDown', keyCode });
    }
    if (release) {
        webContents.sendInputEvent({ type: 'keyUp', keyCode });
    }
}

/**
 * Simulates a key click (press and release) for the given key code.
 */
function handleKeyClick(webContents, code) {
    dispatchKeyEvent(webContents, code, true, true);
}

/**
 * Simulates a key press event for the given key code.
 */
function handleKeyDown(webContents, code) {
    dispatchKeyEvent(webContents, code, true, false);
}

/**
 * Simulates a key release event for the given key code.
 */
function handleKeyUp(webContents, code) {
    dispatchKeyEvent(webContents, code, false, true);
}

/**
 * Simulates typing a string of text by sending individual character events with a small delay between them.
 */
async function handleText(webContents, text) {
    if (!webContents.isFocused()) {
        webContents.focus();
    }

    for (const ch of text) {
        webContents.sendInputEvent({ type: 'char', keyCode: ch });
        await new Promise(r => setTimeout(r, 10)); // small delay
    }
}

module.exports = { handleKeyClick, handleKeyDown, handleKeyUp, handleText };
