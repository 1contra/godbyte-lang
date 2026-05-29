const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

let client;

const irTokenTypes = ['keyword', 'type', 'variable', 'function', 'number', 'operator', 'label'];
const irLegend = new vscode.SemanticTokensLegend(irTokenTypes, []);

const irKeywords = [
    'define', 'param', 'store_local', 'load_local', 'call', 
    'store', 'load', 'into', 'from', 'const', 'add', 'mul', 
    'cmp_lt', 'jmp_false', 'jmp', 'ret', 'void'
];
const irTypes = ['i32', 'i64', 'void'];

const irHoverDocs = {
    'define': 'Defines a new function block.',
    'param': 'Retrieves a parameter passed to the function.',
    'store_local': 'Stores a value into a local variable slot.',
    'load_local': 'Loads a value from a local variable slot.',
    'call': 'Calls a function execution.',
    'store': 'Stores a value into memory address.',
    'load': 'Loads a value from memory address.',
    'const': 'Loads a constant literal into a virtual register.',
    'add': 'Addition operation: `add <type> <op1>, <op2>`',
    'mul': 'Multiplication operation: `mul <type> <op1>, <op2>`',
    'cmp_lt': 'Compare Less Than: Returns true if first operand is less than the second.',
    'jmp_false': 'Conditional jump: Jumps to a label if the condition is false.',
    'jmp': 'Unconditional jump: Jumps directly to a label.',
    'ret': 'Returns from the current function.',
    'into': 'Target memory location indicator.',
    'from': 'Source memory location indicator.'
};

function activate(context) {
    const serverOptions = { command: "gbpp-lsp.exe" };
    const clientOptions = { documentSelector: [{ scheme: 'file', language: 'gbpp' }] };

    client = new LanguageClient(
        'gbppLSP',
        'GodByte++ Language Server',
        serverOptions,
        clientOptions
    );
    client.start();

    const hoverProvider = vscode.languages.registerHoverProvider('gbpp-ir', {
        provideHover(document, position, token) {
            const range = document.getWordRangeAtPosition(position, /[@.\w]+/);
            if (!range) return;
            const word = document.getText(range);

            if (irHoverDocs[word]) {
                return new vscode.Hover(new vscode.MarkdownString(`**${word}**\n\n${irHoverDocs[word]}`));
            } else if (word.startsWith('@')) {
                return new vscode.Hover(new vscode.MarkdownString(`Function call: **${word}**`));
            } else if (word.match(/^v\d+$/)) {
                return new vscode.Hover(new vscode.MarkdownString(`Virtual Register: **${word}**`));
            } else if (word.match(/^\.\w+$/)) {
                return new vscode.Hover(new vscode.MarkdownString(`Basic Block Label: **${word}**`));
            } else if (irTypes.includes(word)) {
                return new vscode.Hover(new vscode.MarkdownString(`Data Type: **${word}**`));
            }
        }
    });

    const semanticProvider = vscode.languages.registerDocumentSemanticTokensProvider('gbpp-ir', {
        provideDocumentSemanticTokens(document) {
            const builder = new vscode.SemanticTokensBuilder(irLegend);
            const text = document.getText();
            const lines = text.split('\n');
            
            for (let i = 0; i < lines.length; i++) {
                const line = lines[i];
                const regex = /([a-zA-Z_]\w*)|(@\w+)|(\.\w+)|(\d+)|(:=|->|\[|\]|,)/g;
                let match;
                
                while ((match = regex.exec(line)) !== null) {
                    const word = match[0];
                    const startChar = match.index;
                    const length = word.length;
                    
                    if (irKeywords.includes(word)) {
                        builder.push(i, startChar, length, irTokenTypes.indexOf('keyword'), 0);
                    } else if (irTypes.includes(word)) {
                        builder.push(i, startChar, length, irTokenTypes.indexOf('type'), 0);
                    } else if (word.match(/^v\d+$/) || word === 'local') {
                        builder.push(i, startChar, length, irTokenTypes.indexOf('variable'), 0);
                    } else if (word.startsWith('@')) {
                        builder.push(i, startChar, length, irTokenTypes.indexOf('function'), 0);
                    } else if (word.startsWith('.')) {
                        builder.push(i, startChar, length, irTokenTypes.indexOf('label'), 0);
                    } else if (word.match(/^\d+$/)) {
                        builder.push(i, startChar, length, irTokenTypes.indexOf('number'), 0);
                    } else if ([':=', '->', '[', ']', ','].includes(word)) {
                        builder.push(i, startChar, length, irTokenTypes.indexOf('operator'), 0);
                    }
                }
            }
            return builder.build();
        }
    }, irLegend);

    context.subscriptions.push(hoverProvider, semanticProvider);
}

function deactivate() {
    if (!client) return undefined;
    return client.stop();
}

module.exports = { activate, deactivate };