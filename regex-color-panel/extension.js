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
 *   'p' – percentage 0-100                     → 0..255 (without byte rounding)
 */
function parseChannelValue(str, type) {
    if (type === 'p') {
        const v = parseFloat(str);
        return isNaN(v) ? null : Math.max(0, Math.min(100, v)) * 255 / 100;
    }
    if (type === 'f') {
        const v = parseFloat(str);
        return isNaN(v) ? null : Math.max(0, Math.min(1, v)) * 255;
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
 *   Each char is 'h' (hex), 'd' (decimal 0-255), 'f' (float 0.0-1.0),
 *   or 'p' (percentage 0-100).
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
    const hasLower = /[a-f]/.test(originalText);
    const hasUpper = /[A-F]/.test(originalText);
    const useLower = hasLower && !hasUpper;
    const fh = v => { const s = hex2(v * 255); return useLower ? s : s.toUpperCase(); };
    const fd = v => String(Math.max(0, Math.min(255, Math.round(v * 255))));
    const ff = v => {
        let s = v.toFixed(3).replace(/0+$/, '');
        if (s.endsWith('.')) s += '0';
        return s;
    };

    const fp = v => String(Number((Math.max(0, Math.min(1, v)) * 100).toFixed(3)));
    const vals = { A: color.alpha, R: color.red, G: color.green, B: color.blue };
    return fmt.template.replace(/\{([ARGB])([hdfp]?)\}/g, (_, ch, type) => {
        const val = vals[ch] ?? 1;
        if (type === 'p') return fp(val);
        if (type === 'd') return fd(val);
        if (type === 'f') return ff(val);
        return fh(val);
    });
}

/** Find unique, simple custom-property definitions in the current document. */
function getCssVariables(text) {
    // Keep offsets intact while ignoring commented-out declarations.
    const source = text.replace(/\/\*[\s\S]*?\*\//g, comment => comment.replace(/[^\r\n]/g, ' '));
    const variables = new Map();
    const regex = /(^|[;{])\s*(--[\w-]+)\s*:\s*([^;{}]*)(?=[;}])/gm;
    let match;
    while ((match = regex.exec(source)) !== null) {
        const name = match[2];
        const value = match[3].replace(/\s*!\s*important\s*$/i, '').trim();
        const start = match.index + match[0].length - match[3].length + match[3].indexOf(value);
        // Multiple definitions may belong to different selectors/themes. Do not guess.
        const definition = value && text.slice(start, start + value.length) === value
            ? { value, start, end: start + value.length }
            : null;
        variables.set(name, variables.has(name) ? null : definition);
    }
    return { source, variables };
}

/** Resolve rgb(var(--rgb) / var(--alpha)), including mixed literal/variable forms. */
function getCssVariableColors(text, formats) {
    const enabled = [];
    for (const fmt of formats) {
        if (!fmt.resolveCssVariables) continue;
        try { enabled.push({ fmt, regex: new RegExp('^(?:' + fmt.pattern + ')$', 'i') }); }
        catch { continue; }
    }
    if (enabled.length === 0) return [];

    const { source, variables } = getCssVariables(text);
    const variable = String.raw`var\(\s*--[\w-]+\s*\)`;
    const rgb = String.raw`(?:${variable}|\d{1,3}\s+\d{1,3}\s+\d{1,3})`;
    const alpha = String.raw`(?:${variable}|\d*\.?\d+\s*%?)`;
    const regex = new RegExp(String.raw`\brgba?\(\s*(${rgb})\s*/\s*(${alpha})\s*\)`, 'gi');
    const results = [];
    let match;
    while ((match = regex.exec(source)) !== null) {
        const parts = [match[1], match[2]].map(value => {
            const reference = /^var\(\s*(--[\w-]+)\s*\)$/i.exec(value);
            const definition = reference ? variables.get(reference[1]) : null;
            if (reference && !definition) return null;
            return { text: value, definition, value: definition ? definition.value : value };
        });
        if (parts.some(part => !part) || !parts.some(part => part.definition)) continue;
        const [rgbPart, alphaPart] = parts;
        rgbPart.start = match[0].indexOf(rgbPart.text);
        alphaPart.start = match[0].lastIndexOf(alphaPart.text);
        const resolved = match[0].slice(0, match[0].indexOf('(') + 1)
            + rgbPart.value + ' / ' + alphaPart.value + ')';
        for (const { fmt, regex: pattern } of enabled) {
            const resolvedMatch = pattern.exec(resolved);
            if (!resolvedMatch) continue;
            const channels = parseChannels(resolvedMatch, fmt.channels, fmt.channelTypes);
            if (!channels) continue;
            results.push({
                start: match.index,
                end: match.index + match[0].length,
                fmt, channels, rgbPart, alphaPart,
            });
            break;
        }
    }
    return results;
}

/** Update referenced definitions and keep var() expressions in the color literal. */
function buildVariablePresentation(color, item, document, range, originalText) {
    const { channels, rgbPart, alphaPart } = item;
    const additionalTextEdits = [];
    const literalEdits = [];
    const update = (part, value) => {
        if (part.definition) {
            const { start, end } = part.definition;
            additionalTextEdits.push(vscode.TextEdit.replace(
                new vscode.Range(document.positionAt(start), document.positionAt(end)), value
            ));
        } else {
            literalEdits.push({ start: part.start, end: part.start + part.text.length, value });
        }
    };
    const changed = (next, previous) => Math.abs(next - previous / 255) > 1e-6;
    if (changed(color.red, channels.r) || changed(color.green, channels.g) || changed(color.blue, channels.b)) {
        update(rgbPart, buildColorString(color, { template: '{Rd} {Gd} {Bd}' }, originalText));
    }
    if (changed(color.alpha, channels.a)) {
        // Alpha definitions use percentages; literal alpha keeps its existing unit.
        const template = alphaPart.definition || alphaPart.text.includes('%') ? '{Ap}%' : '{Af}';
        update(alphaPart, buildColorString(color, { template }, originalText));
    }
    let replacement = originalText;
    for (const edit of literalEdits.sort((a, b) => b.start - a.start)) {
        replacement = replacement.slice(0, edit.start) + edit.value + replacement.slice(edit.end);
    }
    const template = alphaPart.definition ? item.fmt.template.replace(/\{Af\}/g, '{Ap}%') : item.fmt.template;
    const presentation = new vscode.ColorPresentation(buildColorString(color, { template }, originalText));
    presentation.textEdit = vscode.TextEdit.replace(range, replacement);
    presentation.additionalTextEdits = additionalTextEdits;
    return presentation;
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

        for (const item of getCssVariableColors(text, formats)) {
            const ch = item.channels;
            raw.push({
                start: item.start,
                end: item.end,
                color: new vscode.Color(ch.r / 255, ch.g / 255, ch.b / 255, ch.a / 255),
            });
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

        const variableColor = getCssVariableColors(document.getText(), formats).find(item =>
            item.start === document.offsetAt(range.start) && item.end === document.offsetAt(range.end)
        );
        if (variableColor) {
            return [buildVariablePresentation(color, variableColor, document, range, origText)];
        }
        // A variable reference that cannot be resolved safely must not be overwritten.
        if (/\bvar\(/i.test(origText)) return [];

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
            // VS Code collects color results in reverse selector-score order.
            // A wildcard puts our format-preserving result before language providers.
            { scheme: '*' },
            new HexColorProvider()
        )
    );
}

function deactivate() {}

module.exports = { activate, deactivate };
