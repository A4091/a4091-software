import assert from 'node:assert/strict';
import fs from 'node:fs';
import {test} from 'node:test';
import vm from 'node:vm';
import {countArcs, matchNotes, parseArgs, parseCoverage, parseFlatProfile,
    renderReport} from './profile-report.mjs';

test('CLI validates required inputs and unknown options', () => {
    assert.deepEqual(parseArgs(['--help']), {help: true});
    assert.throws(() => parseArgs([]), /Missing --results/);
    assert.throws(() => parseArgs(['--results']), /missing value/);
    assert.throws(() => parseArgs(['--typo', 'x']), /Unknown option/);
    assert.equal(parseArgs(['--results', 'a b', '--objects', 'c']).results, 'a b');
});

test('flattened gcda names require an unambiguous gcno match', () => {
    const notes = ['obj/device.gcno', 'obj/3rdparty/mounter/mounter.gcno'];
    assert.equal(matchNotes('dump/mounter.gcda', notes), notes[1]);
    assert.throws(() => matchNotes('dump/missing.gcda', notes), /found 0/);
    assert.throws(() => matchNotes('device.gcda', [...notes, 'other/device.gcno']), /found 2/);
});

test('BSD gmon header and arc count, including empty and truncated dumps', () => {
    const bytes = Buffer.alloc(36);
    bytes.writeUInt32BE(1000, 4);
    bytes.writeUInt32BE(12, 8);
    assert.equal(countArcs(bytes), 2);
    assert.equal(countArcs(bytes.subarray(0, 12)), 0);
    assert.throws(() => countArcs(bytes.subarray(0, 35)), /invalid BSD/);
    assert.throws(() => countArcs(Buffer.alloc(5)), /invalid BSD/);
    bytes.writeUInt32LE(12, 8);
    assert.throws(() => countArcs(bytes), /invalid BSD/);
});

test('gcov parses C sources, excludes headers/external files, keeps missed lines', () => {
    const summary = `File 'device.c'
Lines executed:50.00% of 2
Branches executed:100.00% of 2
Taken at least once:50.00% of 2
Creating 'device.c.gcov'

File '/src/quickints.c'
Lines executed:0.00% of 2
No branches
Creating 'quickints.c.gcov'

File '/sdk/external.c'
Lines executed:0.00% of 2
Creating 'external.c.gcov'

File 'port.h'
Lines executed:0.00% of 2
Creating 'port.h.gcov'
`;
    const files = parseCoverage(summary, '/src', () =>
        '        -:    0:Source:device.c\n        1:    1:int x;\n    #####:    2:x++;');
    assert.equal(files.length, 2);
    assert.equal(files[0].percent, 50);
    assert.equal(files[0].taken, 50);
    assert.equal(files[0].source[1].count, '#####');
    assert.equal(files[1].name, 'quickints.c');
    assert.equal(files[1].branchPercent, null);
    assert.throws(() => parseCoverage(summary, '/src', () => ''), /Missing annotated/);
    assert.throws(() => parseCoverage(summary + summary, '/src', () => '1:1:x'), /Repeated/);
});

test('flat profile counts retain optimizer suffixes and ignore call graph', () => {
    const functions = parseFlatProfile(`Flat profile:
  0.00 0.00 0.00 688 0.00 0.00 bsd_splbio.isra.0
  0.00 0.00 0.00 226 0.00 0.00 callout_reset
\f 0.00 0.00 0.00 999 0.00 0.00 ignore
`);
    assert.deepEqual(functions, [
        {name: 'bsd_splbio.isra.0', calls: 688},
        {name: 'callout_reset', calls: 226},
    ]);
    assert.deepEqual(parseFlatProfile(''), []);
});

test('embedded source and metadata cannot terminate the script element', () => {
    const template = fs.readFileSync(new URL('./profile-report.html', import.meta.url), 'utf8');
    const data = {metadata: {title: '</script><script>throw 1</script>'},
        source: '$& <script>source text</script>'};
    const html = renderReport(template, data);
    assert.equal((html.match(/<script>/g) ?? []).length, 1);
    assert.equal((html.match(/<\/script>/g) ?? []).length, 1);
    assert.ok(html.includes('\\u003c/script>'));
    // Parse the whole browser script, not just the embedded JSON.
    new vm.Script(html.match(/<script>([\s\S]*?)<\/script>/)[1]);
    assert.throws(() => renderReport('', data), /Missing template/);
});
