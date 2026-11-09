'use strict';

const assert = require('node:assert/strict');
const vscode = require('vscode');

/** Run with --extensionTestsPath inside a real VS Code Extension Development Host. */
async function run() {
    const extension = vscode.extensions.getExtension('sollapse.regex-color-panel');
    assert.ok(extension, 'RegEx Color Panel must be loaded');
    await extension.activate();

    // Keep the competing built-in provider active: disabling it hides this regression.
    const css = vscode.extensions.getExtension('vscode.css-language-features');
    assert.ok(css, 'The built-in CSS extension must be available');
    await css.activate();

    const nextColor = new vscode.Color(26 / 255, 54 / 255, 114 / 255, 0.25);
    const cases = [
        ['rgb(37 99 235 / 12.345%)', 'rgb(26 54 114 / 25%)'],
        ['rgba(37, 99, 235, 10%)', 'rgba(26, 54, 114, 25%)'],
        ['rgb(37 99 235 / 0.1)', 'rgb(26 54 114 / 0.25)'],
        ['#2563EB80', '#1A367240'],
    ];
    for (const [literal, expected] of cases) {
        const document = await vscode.workspace.openTextDocument({
            language: 'css', content: `.test { color: ${literal}; }`,
        });
        const start = document.getText().indexOf(literal);
        const range = new vscode.Range(document.positionAt(start), document.positionAt(start + literal.length));
        // The CSS language server may still be registering its features after activation.
        let colors;
        for (let attempt = 0; attempt < 40; attempt++) {
            colors = await vscode.commands.executeCommand('vscode.executeDocumentColorProvider', document.uri);
            if (colors?.length >= 2) break;
            await new Promise(resolve => setTimeout(resolve, 100));
        }
        assert.ok(colors.length >= 2, 'Both custom and built-in providers must return colors');
        const presentations = await vscode.commands.executeCommand(
            'vscode.executeColorPresentationProvider', nextColor, { uri: document.uri, range }
        );
        assert.equal(presentations[0]?.label, expected, `Custom presentation must precede CSS results: ${literal}`);
    }

    const literal = 'rgb(var(--rgb) / var(--alpha))';
    const source = `:root { --rgb: 37 99 235; --alpha: 0.1; }\n.test { color: ${literal}; }`;
    const document = await vscode.workspace.openTextDocument({ language: 'css', content: source });
    const start = source.indexOf(literal);
    const range = new vscode.Range(document.positionAt(start), document.positionAt(start + literal.length));
    const presentations = await vscode.commands.executeCommand(
        'vscode.executeColorPresentationProvider', nextColor, { uri: document.uri, range }
    );
    const presentation = presentations[0];
    assert.equal(presentation.textEdit?.newText, literal, 'Keep variable references intact');
    assert.equal(presentation.additionalTextEdits?.length, 2, 'Update the two definitions');
    const edits = new vscode.WorkspaceEdit();
    edits.set(document.uri, [presentation.textEdit, ...presentation.additionalTextEdits]);
    assert.ok(await vscode.workspace.applyEdit(edits));
    assert.equal(document.getText(), source.replace('37 99 235;', '26 54 114;').replace('0.1;', '25%;'));

    console.log('PASS: 5 real extension-host checks with the built-in CSS provider active.');
}

module.exports = { run };
