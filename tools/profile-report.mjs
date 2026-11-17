#!/usr/bin/env node
// Generate an offline report from a4091d dumps and their matching build.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

const help = `Usage: node tools/profile-report.mjs --results DIR --objects DIR [options]

Required inputs:
  --results DIR       gmon.out and/or coverage/*.gcda from a4091d
  --objects DIR       Matching build's OBJDIR (objects, map and .gcno files)
Options:
  --driver FILE       Matching unstripped driver (required for gmon.out)
  --source-root DIR   Matching source checkout (default: current directory)
  --output DIR        Report directory (default: RESULTS/report)
  --tool-prefix TEXT  Tool prefix (default: m68k-amigaos-); may include a path
  --metadata FILE     Optional JSON with title, description, environment,
                      steps and notes; see tools/profile-report.example.json
  --help              Show this help

Uses Node.js 18+ and the matching Amiga GCC gcov, nm and gprof tools.
Does not run the driver, merge counters, or modify the input build/dumps.
Generated index.html is self-contained; no server or network is needed.
`;

export function parseArgs(args) {
    const options = {};
    const names = ['results', 'objects', 'driver', 'source-root', 'output',
        'tool-prefix', 'metadata'];
    for (let i = 0; i < args.length; i++) {
        if (args[i] === '--help') return {help: true};
        const name = args[i].slice(2);
        if (!args[i].startsWith('--') || !names.includes(name) ||
            !args[i + 1] || args[i + 1].startsWith('--'))
            throw new Error(`Unknown option or missing value: ${args[i]}`);
        options[name] = args[++i];
    }
    for (const name of ['results', 'objects'])
        if (!options[name]) throw new Error(`Missing --${name}; use --help`);
    return options;
}

function run(command, args, cwd) {
    const result = spawnSync(command, args, {
        cwd, encoding: 'utf8', maxBuffer: 32 * 1024 * 1024,
        env: {...process.env, LC_ALL: 'C', LANG: 'C'},
    });
    if (result.error || result.status !== 0)
        throw new Error(`${command} failed: ${result.error?.message ??
            result.stderr + result.stdout}`);
    if (result.stderr) process.stderr.write(result.stderr);
    return result.stdout;
}

function walk(directory, suffix) {
    return fs.readdirSync(directory, {withFileTypes: true}).flatMap(entry => {
        const filename = path.join(directory, entry.name);
        return entry.isDirectory() ? walk(filename, suffix) :
            entry.isFile() && entry.name.endsWith(suffix) ? [filename] : [];
    }).sort();
}

export function matchNotes(gcda, notes) {
    const basename = path.basename(gcda, '.gcda') + '.gcno';
    const matches = notes.filter(note => path.basename(note) === basename);
    if (matches.length !== 1)
        throw new Error(`Expected one matching ${basename}, found ${matches.length}`);
    return matches[0];
}

export function parseCoverage(summary, sourceRoot, readAnnotation) {
    const files = [];
    for (const match of summary.matchAll(/File '([^']+)'\n([\s\S]*?)(?=\nFile '|$)/g)) {
        const name = path.relative(sourceRoot, path.resolve(sourceRoot, match[1]));
        if (!name.endsWith('.c') || name.startsWith('..' + path.sep) ||
            path.isAbsolute(name)) continue;
        const body = match[2];
        const lines = body.match(/Lines executed:([\d.]+)% of (\d+)/);
        const branches = body.match(/Branches executed:([\d.]+)% of (\d+)/);
        const taken = body.match(/Taken at least once:([\d.]+)%/);
        const annotation = body.match(/Creating '([^']+\.gcov)'/);
        if (!lines || !annotation) throw new Error(`Missing coverage for ${name}`);
        const source = readAnnotation(annotation[1]).split('\n').flatMap(line => {
            const m = line.match(/^\s*([^:]+):\s*(\d+):(.*)$/);
            return m && +m[2] > 0 ? [{count: m[1].trim(), line: +m[2], text: m[3]}] : [];
        });
        if (!source.length) throw new Error(`Missing annotated source for ${name}`);
        if (files.some(file => file.name === name))
            throw new Error(`Repeated coverage for ${name}; merging is not supported`);
        files.push({name, lines: +lines[2], percent: +lines[1],
            branches: branches ? +branches[2] : 0,
            branchPercent: branches ? +branches[1] : null,
            taken: taken ? +taken[1] : null, source});
    }
    return files;
}

export function parseFlatProfile(gprof) {
    return [...gprof.split('\f')[0].matchAll(
        /^\s*[\d.]+\s+[\d.]+\s+[\d.]+\s+(\d+)\s+[\d.]+\s+[\d.]+\s+(\S+)\s*$/gm
    )].map(m => ({name: m[2], calls: +m[1]}));
}

export function countArcs(buffer) {
    // a4091d writes the old BSD gmon format, in m68k byte order.
    if (buffer.length < 12 || buffer.readUInt32BE(0) !== 0 ||
        buffer.readUInt32BE(8) !== 12 || (buffer.length - 12) % 12)
        throw new Error('Not an a4091d gmon.out file (invalid BSD header/size)');
    return (buffer.length - 12) / 12;
}

function symbolsForBuild(driver, objects, sourceRoot, prefix) {
    let symbols = run(prefix + 'nm', ['-n', driver]);
    const map = fs.readFileSync(path.join(objects, path.basename(driver) + '.map'), 'utf8');
    const sections = [...map.matchAll(
        /^ \.text\s+0x([\da-f]+)\s+0x[\da-f]+\s+([^\n]+\.o)$/gm
    )];
    if (!sections.length) throw new Error('No object text sections in linker map');
    const seen = new Set();
    for (const match of sections) {
        const original = match[2];
        // These runtimes deliberately have no instrumentation. coverage.o
        // can contain a second .text hunk for the cold abort() stub.
        if (['coverage.o', 'profiling.o'].includes(path.basename(original))) continue;
        if (seen.has(original))
            throw new Error(`Multiple .text hunks in ${original}; cannot relocate local symbols safely`);
        seen.add(original);
        const relative = path.relative(objects, path.resolve(sourceRoot, original));
        // Also support moving an OBJDIR with its original directory name.
        const marker = path.basename(objects) + path.sep;
        const relocated = original.indexOf(marker);
        const object = !relative.startsWith('..' + path.sep) ?
            path.join(objects, relative) : relocated >= 0 ?
                path.join(objects, original.slice(relocated + marker.length)) :
                path.resolve(sourceRoot, original);
        const nm = run(prefix + 'nm', [object]);
        for (const symbol of nm.matchAll(/^([\da-f]+) t (\S+)$/gm))
            symbols += (parseInt(match[1], 16) + parseInt(symbol[1], 16))
                .toString(16) + ' t ' + symbol[2] + '\n';
    }
    return symbols;
}

export function renderReport(template, data) {
    if (!template.includes('/*REPORT_DATA*/')) throw new Error('Missing template data marker');
    // Keep source text and user-supplied metadata out of HTML/script syntax.
    return template.replace('/*REPORT_DATA*/', () =>
        JSON.stringify(data).replaceAll('<', '\\u003c'));
}

export function generate(options) {
    const results = path.resolve(options.results);
    const objects = path.resolve(options.objects);
    const sourceRoot = path.resolve(options['source-root'] ?? '.');
    const output = path.resolve(options.output ?? path.join(results, 'report'));
    const prefix = options['tool-prefix'] ?? 'm68k-amigaos-';
    const gmon = path.join(results, 'gmon.out');
    const coverageDir = path.join(results, 'coverage');
    const gcda = fs.existsSync(coverageDir) ? walk(coverageDir, '.gcda') : [];
    const hasProfile = fs.existsSync(gmon);
    if (!hasProfile && !gcda.length) throw new Error('No gmon.out or coverage/*.gcda found');
    if (hasProfile && !options.driver) throw new Error('gmon.out requires --driver');
    const metadata = options.metadata ? JSON.parse(fs.readFileSync(options.metadata, 'utf8')) : {};
    for (const key of ['steps', 'notes'])
        if (metadata[key] && (!Array.isArray(metadata[key]) ||
            metadata[key].some(value => typeof value !== 'string')))
            throw new Error(`Metadata ${key} must be an array of strings`);
    if (metadata.environment && (typeof metadata.environment !== 'object' ||
        Array.isArray(metadata.environment) ||
        Object.values(metadata.environment).some(value => typeof value !== 'string')))
        throw new Error('Metadata environment must map labels to strings');

    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'a4091-report-'));
    try {
        let summary = '', gprof = '', symbols = '';
        if (gcda.length) {
            const notes = walk(objects, '.gcno');
            // gcov resolves relative source names against its working directory.
            // Copy project sources there, never write .gcov files into the checkout.
            for (const name of fs.readdirSync(sourceRoot).filter(n => /\.[ch]$/.test(n)))
                fs.copyFileSync(path.join(sourceRoot, name), path.join(temporary, name));
            for (const dump of gcda) {
                const note = matchNotes(dump, notes);
                const relative = path.relative(objects, note);
                const target = path.join(temporary, 'objects', relative);
                fs.mkdirSync(path.dirname(target), {recursive: true});
                fs.copyFileSync(note, target);
                fs.copyFileSync(dump, target.replace(/\.gcno$/, '.gcda'));
                const source = relative.replace(/\.gcno$/, '.c');
                fs.mkdirSync(path.dirname(path.join(temporary, source)), {recursive: true});
                fs.copyFileSync(path.join(sourceRoot, source), path.join(temporary, source));
                summary += run(prefix + 'gcov', ['-b', '-c', '-p', '-o', target, source], temporary);
            }
        }
        if (hasProfile) {
            symbols = symbolsForBuild(path.resolve(options.driver), objects, sourceRoot, prefix);
            const bytes = fs.readFileSync(gmon);
            countArcs(bytes);
            const start = symbols.match(/^([\da-f]+) T __stext$/m);
            const end = symbols.match(/^([\da-f]+) T __etext$/m);
            if (!start || !end || parseInt(start[1], 16) !== 0 ||
                parseInt(end[1], 16) !== bytes.readUInt32BE(4))
                throw new Error('Driver text bounds do not match gmon.out');
            const symbolFile = path.join(temporary, 'symbols.txt');
            fs.writeFileSync(symbolFile, symbols);
            gprof = run(prefix + 'gprof', ['-b', '--external-symbol-table=' + symbolFile,
                path.resolve(options.driver), gmon]);
        }
        const files = parseCoverage(summary, sourceRoot,
            name => fs.readFileSync(path.join(temporary, name), 'utf8'));
        const downloads = [];
        const addDownload = (name, bytes) => downloads.push({name,
            base64: Buffer.from(bytes).toString('base64')});
        const logs = {};
        // Only known diagnostic logs are embedded, not arbitrary files in RESULTS.
        for (const name of ['RESULTS', 'driver', 'probe', 'geometry', 'benchmark',
            'profile', 'coverage', 'progress']) {
            const filename = path.join(results, name + '.txt');
            if (!fs.existsSync(filename)) continue;
            logs[name] = fs.readFileSync(filename, 'utf8');
            addDownload(name + '.txt', logs[name]);
        }
        const generated = {'coverage-summary.txt': summary, 'gprof.txt': gprof,
            'symbols.txt': symbols};
        for (const [name, contents] of Object.entries(generated))
            if (contents) addDownload('reports/' + name, contents);
        for (const filename of [...(hasProfile ? [gmon] : []), ...gcda])
            addDownload(path.relative(results, filename), fs.readFileSync(filename));
        const profileLog = logs.profile ?? '';
        const extraction = profileLog.match(/Wrote (\d+) profile arcs[^\n]*/);
        const dropped = extraction ? +(extraction[0].match(/\((\d+) dropped\)/)?.[1] ?? 0) : null;
        const data = {files, functions: parseFlatProfile(gprof),
            arcs: hasProfile ? countArcs(fs.readFileSync(gmon)) : null,
            interrupts: profileLog.match(/Interrupts serviced: (\d+)/)?.[1] ?? null,
            dropped, coverageFiles: gcda.length, downloads, logs, metadata};
        if (extraction && +extraction[1] !== data.arcs)
            throw new Error('profile.txt arc count does not match gmon.out');
        const template = fs.readFileSync(new URL('./profile-report.html', import.meta.url), 'utf8');
        const html = renderReport(template, data);
        fs.mkdirSync(output, {recursive: true});
        for (const [name, contents] of Object.entries(generated))
            fs.writeFileSync(path.join(output, name), contents);
        fs.writeFileSync(path.join(output, 'index.html'), html);
        console.log(`Created ${path.join(output, 'index.html')} (${files.length} source files, ` +
            `${data.functions.length} functions, ${gcda.length} coverage dumps)`);
    } finally {
        // Only remove the scratch directory created by this invocation.
        fs.rmSync(temporary, {recursive: true, force: true});
    }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    try {
        const options = parseArgs(process.argv.slice(2));
        if (options.help) console.log(help);
        else generate(options);
    } catch (error) {
        console.error(`profile-report: ${error.message}`);
        process.exitCode = 1;
    }
}
