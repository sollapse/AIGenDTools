'use strict';

const vscode = require('vscode');

// ── helpers ───────────────────────────────────────────────────────────────────

/** Clamp, round, and zero-pad a 0-255 value to two hex digits. */
function hex2(v) {
    return Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, '0');
}

/**
 * Parse a single captured channel string according to its type:
 *   'h' – hex digits (optionally 0x-prefixed)  → 0..255
 *   'd' – decimal integer 0-255               → 0..255
 *   'f' – float 0.0-1.0                        → 0..255
 */
function parseChannelValue(str, type) {
    if (type === 'f') {
        const v = parseFloat(str);
        return isNaN(v) ? null : Math.round(Math.max(0, Math.min(1, v)) * 255);
    }
    if (type === 'd') {
        const v = parseInt(str, 10);
        return isNaN(v) ? null : Math.max(0, Math.min(255, v));
    }
    // 'h' or default: hex (optionally 0x-prefixed)
    const v = parseInt(str.replace(/^0x/i, ''), 16);
    return isNaN(v) ? null : Math.max(0, Math.min(255, v));
}

/**
 * Extract { r, g, b, a } (each 0-255) from a regex match array.
 * channelOrder: e.g. "ARGB", "RGB"
 * channelTypes: optional string, same length as channelOrder.
 *   Each char is 'h' (hex), 'd' (decimal 0-255), or 'f' (float 0.0-1.0).
 *   Defaults to 'h' for any missing position.
 */
function parseChannels(match, channelOrder, channelTypes) {
    const map = {};
    for (let i = 0; i < channelOrder.length; i++) {
        const ch   = channelOrder[i].toUpperCase();
        const type = channelTypes ? (channelTypes[i] ?? 'h') : 'h';
        const val  = parseChannelValue(match[i + 1] ?? '', type);
        if (val === null) return null;
        map[ch] = val;
    }
    return {
        r: map['R'] ?? 0,
        g: map['G'] ?? 0,
        b: map['B'] ?? 0,
        a: map['A'] ?? 255,
    };
}

/**
 * Reconstruct the color literal from a vscode.Color using the format's
 * template, preserving the case style of the original matched text.
 * Original text that mixes or uses only uppercase -> uppercase output.
 * Original text that uses only lowercase hex letters -> lowercase output.
 */
function buildColorString(color, fmt, originalText) {
    const r = Math.round(color.red   * 255);
    const g = Math.round(color.green * 255);
    const b = Math.round(color.blue  * 255);
    const a = Math.round(color.alpha * 255);

    const hasLower = /[a-f]/.test(originalText);
    const hasUpper = /[A-F]/.test(originalText);
    const useLower = hasLower && !hasUpper;
    const fh = v => { const s = hex2(v); return useLower ? s : s.toUpperCase(); };
    const fd = v => String(Math.max(0, Math.min(255, Math.round(v))));
    const ff = v => {
        let s = (v / 255).toFixed(3).replace(/0+$/, '');
        if (s.endsWith('.')) s += '0';
        return s;
    };

    const vals = { A: a, R: r, G: g, B: b };
    return fmt.template.replace(/\{([ARGB])([hdf]?)\}/g, (_, ch, type) => {
        const val = vals[ch] ?? 255;
        if (type === 'd') return fd(val);
        if (type === 'f') return ff(val);
        return fh(val);
    });
}

// ── color provider ────────────────────────────────────────────────────────────

class HexColorProvider {
    /**
     * Return the formats from settings that apply to the given language ID.
     * A format with an empty (or absent) languages array applies to all languages.
     */
    _getFormats(langId) {
        const config  = vscode.workspace.getConfiguration('regexColorPanel');
        const formats = config.get('formats') ?? [];
        return formats.filter(fmt => {
            if (!fmt.languages || fmt.languages.length === 0) return true;
            return fmt.languages.includes(langId);
        });
    }

    /** Find all color literals in the document and return ColorInformation[]. */
    provideDocumentColors(document) {
        const formats = this._getFormats(document.languageId);
        const text    = document.getText();

        const raw = [];
        for (const fmt of formats) {
            let regex;
            try { regex = new RegExp(fmt.pattern, 'gi'); }
            catch { continue; }

            let match;
            while ((match = regex.exec(text)) !== null) {
                const ch = parseChannels(match, fmt.channels, fmt.channelTypes);
                if (!ch) continue;
                raw.push({
                    start: match.index,
                    end:   match.index + match[0].length,
                    color: new vscode.Color(ch.r / 255, ch.g / 255, ch.b / 255, ch.a / 255),
                });
            }
        }

        // Longer match wins when two formats start at the same offset.
        raw.sort((a, b) => a.start - b.start || (b.end - b.start) - (a.end - a.start));

        const results = [];
        let lastEnd = -1;
        for (const item of raw) {
            if (item.start < lastEnd) continue;
            lastEnd = item.end;
            results.push(new vscode.ColorInformation(
                new vscode.Range(
                    document.positionAt(item.start),
                    document.positionAt(item.end)
                ),
                item.color
            ));
        }
        return results;
    }

    /**
     * Called when the user picks a new color in the VS Code color picker.
     * Returns the replacement text in the original format.
     */
    provideColorPresentations(color, { document, range }) {
        const formats  = this._getFormats(document.languageId);
        const origText = document.getText(range);

        for (const fmt of formats) {
            let regex;
            try { regex = new RegExp('^(?:' + fmt.pattern + ')$', 'i'); }
            catch { continue; }
            if (regex.test(origText)) {
                return [new vscode.ColorPresentation(
                    buildColorString(color, fmt, origText)
                )];
            }
        }

        // Fallback: standard uppercase #RRGGBB.
        return [new vscode.ColorPresentation(
            '#' +
            hex2(color.red   * 255).toUpperCase() +
            hex2(color.green * 255).toUpperCase() +
            hex2(color.blue  * 255).toUpperCase()
        )];
    }
}

// ── activation ────────────────────────────────────────────────────────────────

function activate(context) {
    context.subscriptions.push(
        vscode.languages.registerColorProvider(
            [{ scheme: 'file' }, { scheme: 'untitled' }],
            new HexColorProvider()
        )
    );
}

function deactivate() {}

module.exports = { activate, deactivate };
