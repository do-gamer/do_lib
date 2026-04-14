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

// VK codes 96–111 are the numpad range; sendInputEvent needs the isKeypad modifier
// so Chromium routes them to VK_NUMPAD0-9 / VK_ADD / VK_SUBTRACT etc. instead of
// their non-numpad equivalents (VK_0 / VK_OEM_PLUS / VK_OEM_MINUS, etc.)
const numpadCodes = {
    // Digits
    96: '0',
    97: '1',
    98: '2',
    99: '3',
    100: '4',
    101: '5',
    102: '6',
    103: '7',
    104: '8',
    105: '9',

    // Operations
    106: '*',
    107: '+',
    108: 'Enter',
    109: '-',
    110: '.',
    111: '/'
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
    if (code in specialKeys) return specialKeys[code];
    if (code in numpadCodes) return numpadCodes[code];
    if (code in oemKeys) return oemKeys[code];

    return String.fromCharCode(code);
}

/**
 * Returns the Electron input event modifiers for a given key code.
 */
function getModifiers(code) {
    return code in numpadCodes ? ['isKeypad'] : [];
}

/**
 * Dispatches key events based on the provided code and action (press/release).
 */
function dispatchKeyEvent(webContents, code, press, release) {
    if (!webContents.isFocused()) {
        webContents.focus();
    }

    const keyCode = resolveKey(code);
    const modifiers = getModifiers(code);

    if (press) {
        webContents.sendInputEvent({ type: 'keyDown', keyCode, modifiers });
    }
    if (release) {
        webContents.sendInputEvent({ type: 'keyUp', keyCode, modifiers });
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
function handleText(webContents, text) {
    webContents.focus();

    let delay = 0;
    for (const ch of text) {
        setTimeout((char) => {
            webContents.sendInputEvent({ type: 'char', keyCode: char });
        }, delay, ch);
        delay += 10; // small delay
    }
}

module.exports = { handleKeyClick, handleKeyDown, handleKeyUp, handleText };
