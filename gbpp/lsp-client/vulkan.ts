import * as fs from 'fs';
import * as https from 'https';

const VK_XML_URL = "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/main/registry/vk.xml";
const OUTPUT_FILE = "vulkan.lib.gbpp";

const TYPE_MAP: Record<string, string> = {
    "uint8_t": "u8",
    "int8_t": "i8",
    "char": "u8",
    "uint16_t": "u16",
    "int16_t": "i16",
    "uint32_t": "u32",
    "int32_t": "i32",
    "int": "i32",
    "uint64_t": "u64",
    "size_t": "u64",
    "int64_t": "i64",
    "float": "f32",
    "double": "f64",
    "void": "void",
    "VkBool32": "u32",
    "bool": "bool",
    "nullptr": "null",
    "zx_handle_t": "u32"
};

function mapType(cType: string): string {
    return TYPE_MAP[cType] || cType;
}

function stripXml(str: string): string {
    return str.replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').trim();
}

function parseAttributes(attrString: string): Record<string, string> {
    const attrs: Record<string, string> = {};
    const regex = /([a-zA-Z0-9_]+)="([^"]+)"/g;
    let match;
    while ((match = regex.exec(attrString)) !== null) attrs[match[1]] = match[2];
    return attrs;
}

function extractBlocks(xml: string, tag: string) {
    const blocks: { attrs: Record<string, string>, body: string, isSelfClosing: boolean }[] = [];
    const openTag = `<${tag}`;
    const closeTag = `</${tag}>`;
    let i = 0;

    while (true) {
        let start = xml.indexOf(openTag, i);
        if (start === -1) break;

        const nextChar = xml[start + openTag.length];
        if (nextChar !== ' ' && nextChar !== '>' && nextChar !== '/') {
            i = start + openTag.length;
            continue;
        }

        let endOfOpen = xml.indexOf('>', start);
        if (endOfOpen === -1) break;

        let attrStr = xml.substring(start + openTag.length, endOfOpen);
        let isSelfClosing = attrStr.trim().endsWith('/');

        if (isSelfClosing) {
            attrStr = attrStr.substring(0, attrStr.length - 1);
            blocks.push({ attrs: parseAttributes(attrStr), body: '', isSelfClosing: true });
            i = endOfOpen + 1;
        } else {
            let depth = 1;
            let searchIndex = endOfOpen + 1;
            let bodyEnd = -1;

            while (depth > 0) {
                let nextOpen = xml.indexOf(openTag, searchIndex);
                while (nextOpen !== -1) {
                    const nc = xml[nextOpen + openTag.length];
                    if (nc !== ' ' && nc !== '>' && nc !== '/') nextOpen = xml.indexOf(openTag, nextOpen + 1);
                    else break;
                }

                let nextClose = xml.indexOf(closeTag, searchIndex);
                if (nextClose === -1) break;

                if (nextOpen !== -1 && nextOpen < nextClose) {
                    let endOfNextOpen = xml.indexOf('>', nextOpen);
                    if (xml[endOfNextOpen - 1] === '/') searchIndex = endOfNextOpen + 1;
                    else {
                        depth++;
                        searchIndex = endOfNextOpen + 1;
                    }
                } else {
                    depth--;
                    bodyEnd = nextClose;
                    searchIndex = nextClose + closeTag.length;
                }
            }

            if (bodyEnd !== -1) {
                blocks.push({
                    attrs: parseAttributes(attrStr),
                    body: xml.substring(endOfOpen + 1, bodyEnd),
                    isSelfClosing: false
                });
                i = searchIndex;
            } else break;
        }
    }

    return blocks;
}

function parseDecl(declString: string): { name: string, type: string } {
    let clean = declString.replace(/\s+/g, ' ').trim();
    if (clean === "...") return { name: "...", type: "" };

    let nameMatch = clean.match(/<name>([^<]+)<\/name>/);
    if (!nameMatch) return { name: "", type: "" };

    let name = nameMatch[1].trim();
    let beforeName = clean.substring(0, nameMatch.index!).trim();

    let arraySuffix = "";
    const arrays = clean.match(/\[(.*?)\]/g);
    if (arrays) {
        arraySuffix = arrays.map(x => `[${stripXml(x).replace(/\[|\]/g, '')}]`).join('');
        beforeName = beforeName.replace(/\[.*?\]/g, '');
    }

    let typeMatch = beforeName.match(/<type>([^<]+)<\/type>/);
    if (!typeMatch) {
        let raw = stripXml(beforeName);
        raw = raw.replace(/^typedef\s+/, '').replace(/^struct\s+/, '').replace(/^union\s+/, '').trim();
        return { name, type: mapType(raw) + arraySuffix };
    }

    let baseType = typeMatch[1].trim();
    let prefix = beforeName.substring(0, typeMatch.index!).trim();
    let suffix = beforeName.substring(typeMatch.index! + typeMatch[0].length).trim();

    let baseConst = /\bconst\b/.test(prefix);
    let pointerParts = suffix.match(/\*\s*(const)?/g) || [];

    let type = mapType(baseType);
    if (baseConst) type = `const ${type}`;

    for (let i = pointerParts.length - 1; i >= 0; i--) {
        const pointerConst = /\bconst\b/.test(pointerParts[i]);
        type = `ref ${pointerConst ? `const ${type}` : type}`;
    }

    return { name, type: type + arraySuffix };
}

function cleanValue(value: string): string {
    return value.replace(/ULL/g, '').replace(/LL/g, '').replace(/UL/g, '').replace(/LU/g, '').replace(/U/g, '').trim();
}

function resolveValue(name: string, values: Map<string, string>, seen = new Set<string>()): string | undefined {
    if (seen.has(name)) return undefined;
    seen.add(name);

    const value = values.get(name);
    if (value === undefined) return undefined;

    const cleaned = cleanValue(value);
    if (/^-?(0x[0-9a-fA-F]+|\d+(\.\d+)?)$/.test(cleaned)) return cleaned;
    if (values.has(cleaned)) return resolveValue(cleaned, values, seen);

    return undefined;
}

function typedFloat(value: string): string {
    if (/^-?\d+\.\d+(e[+-]?\d+)?$/i.test(value)) return `${value}f32`;
    return value;
}

function alignUp(value: number, align: number): number {
    return Math.ceil(value / align) * align;
}

function typeSize(type: string, structs: Map<string, any>): { size: number, align: number } {
    type = type.replace(/^const /, '');

    const arrayMatch = type.match(/^(.*)\[(\d+)\]$/);
    if (arrayMatch) {
        const base = typeSize(arrayMatch[1], structs);
        return { size: base.size * Number(arrayMatch[2]), align: base.align };
    }

    if (type.startsWith("ref ")) return { size: 8, align: 8 };

    switch (type) {
        case "u8":
        case "i8": return { size: 1, align: 1 };
        case "u16":
        case "i16": return { size: 2, align: 2 };
        case "u32":
        case "i32":
        case "f32": return { size: 4, align: 4 };
        case "u64":
        case "i64":
        case "f64": return { size: 8, align: 8 };
        case "bool": return { size: 1, align: 1 };
    }

    const struct = structs.get(type);
    if (!struct) return { size: 8, align: 8 };

    if (struct.category === "union") {
        let size = 0;
        let align = 1;

        for (const member of struct.members) {
            const layout = typeSize(member.type, structs);
            size = Math.max(size, layout.size);
            align = Math.max(align, layout.align);
        }

        return { size: alignUp(size, align), align };
    }

    let offset = 0;
    let align = 1;

    for (const member of struct.members) {
        const layout = typeSize(member.type, structs);
        offset = alignUp(offset, layout.align);
        offset += layout.size;
        align = Math.max(align, layout.align);
    }

    return { size: alignUp(offset, align), align };
}

async function fetchXML(url: string): Promise<string> {
    return new Promise((resolve, reject) => {
        https.get(url, (res) => {
            let data = '';
            res.on('data', (chunk) => data += chunk);
            res.on('end', () => resolve(data));
        }).on('error', reject);
    });
}

async function generate() {
    console.log(`Fetching ${VK_XML_URL}...`);
    const xml = await fetchXML(VK_XML_URL);
    console.log(`Fetched ${xml.length} bytes. Parsing...`);

    let out = `/**\n * vulkan.lib.gbpp\n * Auto-generated Vulkan Bindings\n */\n\n`;
    out += `comptime if (USE_VULKAN) {\n\n`;

    console.log("Extracting types & aliases...");
    const typeBlocks = extractBlocks(xml, 'type');
    const structs: { name: string, body: string, category: string, members: { name: string, type: string }[] }[] = [];
    const structMap = new Map<string, any>();
    const opaqueStructs = new Set<string>();

    for (const block of typeBlocks) {
        const cat = block.attrs['category'];
        const nameAttr = block.attrs['name'];
        const aliasAttr = block.attrs['alias'];

        if (aliasAttr && nameAttr) {
            out += `    alias ${nameAttr} = ${mapType(aliasAttr)};\n`;
            continue;
        }

        if (cat === 'enum') continue;

        if (cat === 'bitmask') {
            if (nameAttr) out += `    alias ${nameAttr} = u32;\n`;
            else {
                const nameMatch = block.body.match(/<name>([^<]+)<\/name>/);
                if (nameMatch) out += `    alias ${nameMatch[1]} = u32;\n`;
            }
        } else if (cat === 'basetype') {
            const typeMatch = block.body.match(/<type>([^<]+)<\/type>/);
            const nameMatch = block.body.match(/<name>([^<]+)<\/name>/);

            if (typeMatch && nameMatch) {
                out += `    alias ${nameMatch[1]} = ${mapType(typeMatch[1])};\n`;
            } else {
                const opaqueMatch = block.body.match(/struct\s+<name>([^<]+)<\/name>/);
                if (opaqueMatch) opaqueStructs.add(opaqueMatch[1]);
            }
        } else if (cat === 'handle') {
            const name = nameAttr || block.body.match(/<name>([^<]+)<\/name>/)?.[1];
            if (name) out += `    alias ${name} = ref void;\n`;
        } else if (cat === 'struct' || cat === 'union') {
            if (!nameAttr) continue;

            const members: { name: string, type: string }[] = [];
            const memberBlocks = extractBlocks(block.body, 'member');

            for (const member of memberBlocks) {
                const decl = parseDecl(member.body);
                if (decl.name && decl.type) members.push(decl);
            }

            const info = { name: nameAttr, body: block.body, category: cat, members };
            structs.push(info);
            structMap.set(nameAttr, info);
        } else if (!cat && nameAttr && /windows\.h/.test(block.attrs['requires'] || '')) {
            if (nameAttr === "HANDLE") out += `    alias HANDLE = ref void;\n`;
        }
    }

    if (/\bHANDLE\b/.test(xml) && !out.includes("alias HANDLE")) {
        out += `    alias HANDLE = ref void;\n`;
    }

    for (const name of opaqueStructs) out += `    struct ${name} {};\n`;
    out += `\n`;

    console.log("Extracting enums & constants...");
    const enumsBlocks = extractBlocks(xml, 'enums');
    const enumValues = new Map<string, string>();

    for (const enums of enumsBlocks) {
        for (const e of extractBlocks(enums.body, 'enum')) {
            const name = e.attrs['name'];
            if (!name) continue;

            if (e.attrs['value'] !== undefined) {
                enumValues.set(name, cleanValue(e.attrs['value']));
            } else if (e.attrs['alias'] !== undefined) {
                const alias = e.attrs['alias'];
                const resolved = resolveValue(alias, enumValues);
                if (resolved !== undefined) enumValues.set(name, resolved);
                else enumValues.set(name, alias);
            } else if (e.attrs['bitpos'] !== undefined) {
                const n = BigInt(parseInt(e.attrs['bitpos'], 10));
                enumValues.set(name, "0x" + (1n << n).toString(16).padStart(8, '0').toUpperCase());
            }
        }
    }

    for (const enums of enumsBlocks) {
        const enumName = enums.attrs['name'];
        if (!enumName) continue;

        const values = extractBlocks(enums.body, 'enum');

        if (enumName === "API Constants") {
            for (const e of values) {
                const name = e.attrs['name'];
                if (!name) continue;

                let value = e.attrs['value'] ?? e.attrs['alias'];
                if (value === undefined) continue;

                if (value.includes('&quot;')) {
                    value = value.replace(/&quot;/g, '"');
                    out += `    ${name}: const ref u8 = ${value};\n`;
                    continue;
                }

                const resolved = resolveValue(name, enumValues) ?? cleanValue(value);
                const type = /[.eE]/.test(resolved) ? "f32" : "u64";
                const literal = type === "f32" ? typedFloat(resolved) : resolved;
                out += `    ${name}: const ${type} = ${literal};\n`;
            }

            out += `\n`;
        } else {
            out += `    enum ${enumName} {\n`;

            for (const e of values) {
                const name = e.attrs['name'];
                if (!name) continue;

                let value = resolveValue(name, enumValues);

                if (value === undefined && e.attrs['bitpos'] !== undefined) {
                    const n = BigInt(parseInt(e.attrs['bitpos'], 10));
                    value = "0x" + (1n << n).toString(16).padStart(8, '0').toUpperCase();
                }

                if (value !== undefined) out += `        ${name} = ${value},\n`;
            }

            out += `    };\n\n`;
        }
    }

    console.log("Extracting structs...");

    for (const struct of structs) {
        out += `    struct ${struct.name} {\n`;

        const memberBlocks = extractBlocks(struct.body, 'member');

        if (memberBlocks.length === 0) {
            out += `    };\n\n`;
            continue;
        }

        let offset = 0;

        for (const member of memberBlocks) {
            let decl = member.body;
            let comment = "";

            const commentMatch = decl.match(/<comment>([^<]+)<\/comment>/);
            if (commentMatch) {
                comment = ` // ${commentMatch[1]}`;
                decl = decl.replace(/<comment>.*?<\/comment>/, '');
            }

            const { name, type } = parseDecl(decl);
            if (!name || !type) continue;

            if (struct.category === "union") {
                out += `        ${name}: ${type} @0;${comment}\n`;
                continue;
            }

            const layout = typeSize(type, structMap);
            offset = alignUp(offset, layout.align);
            out += `        ${name}: ${type} @${offset};${comment}\n`;
            offset += layout.size;
        }

        out += `    };\n\n`;
    }

    console.log("Extracting functions...");

    const cmdBlocks = extractBlocks(xml, 'command');

    for (const cmd of cmdBlocks) {
        const aliasAttr = cmd.attrs['alias'];
        const nameAttr = cmd.attrs['name'];

        if (aliasAttr && nameAttr) {
            out += `    // alias function ${nameAttr} = ${aliasAttr};\n`;
            continue;
        }

        const protoBlocks = extractBlocks(cmd.body, 'proto');
        if (protoBlocks.length === 0) continue;

        const protoDecl = parseDecl(protoBlocks[0].body);
        const funcName = protoDecl.name;
        const returnType = protoDecl.type;

        const params: string[] = [];
        const paramBlocks = extractBlocks(cmd.body, 'param');

        for (const p of paramBlocks) {
            const pDecl = parseDecl(p.body);
            if (pDecl.name === "...") params.push("...");
            else if (pDecl.name && pDecl.type) params.push(`${pDecl.name}: ${pDecl.type}`);
        }

        out += `    [[@extern]] fn ${funcName}(${params.join(', ')}): ${returnType};\n`;
    }

    out += `\n}\n`;

    fs.writeFileSync(OUTPUT_FILE, out);
    console.log(`\nSuccessfully emitted bindings to ${OUTPUT_FILE}`);
}

generate().catch(console.error);