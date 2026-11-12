#!/usr/bin/env python3
"""Regenerate this small offline method note from its sealed source payloads.

No compiler, decoder, network, or Git checkout is needed. The original Git patch
and source manifest are immutable inputs; only the presentation is regenerated.
"""
from pathlib import Path
import difflib
import hashlib
import html
import importlib.util
import json

from explanations import CORE, KERNEL, DRIVER, GUARD

ROOT = Path(__file__).resolve().parents[1]
PATHS = {
    'core': 'src/beyond_blossom/structured/predecode/one_hop_predecoder.cc',
    'kernel': 'src/beyond_blossom/structured/predecode/one_hop_kernels.h',
    'driver': 'src/beyond_blossom/structured/driver/mwpm_decoding.cc',
    'guard': 'AE/native/benchmark.cc',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def main():
    manifest = json.loads((ROOT / 'sources/manifest.json').read_text())
    for relative, info in manifest['files'].items():
        if sha(ROOT / relative) != info['sha256']:
            raise ValueError('Frozen source changed: ' + relative)
    sources = {(side, key): (ROOT / 'sources' / side / path).read_text().splitlines()
               for side in ['before', 'after'] for key, path in PATHS.items()
               if (ROOT / 'sources' / side / path).exists()}
    annotations = {}
    old_to_new = {}
    for block in difflib.SequenceMatcher(a=sources['before', 'core'], b=sources['after', 'core'], autojunk=False).get_matching_blocks():
        for k in range(block.size):
            old_to_new[block.a + k + 1] = block.b + k + 1

    def explain(side, key, number):
        code = sources[side, key][number - 1]
        if not code.strip():
            return ''
        if side == 'before':
            if number in old_to_new:
                text = CORE[old_to_new[number]]
            elif code.strip() in ['#if BB_TAILRESET_PREDECODE_MODE == 2', '#elif BB_TAILRESET_PREDECODE_MODE == 2']:
                text = '旧提交只为物理模式2选择这条坐标路径；模式2仍采用 full-cap/grow，没有新增模式3的半权规则。'
            else:
                raise ValueError(f'Missing before explanation {number}: {code}')
        else:
            text = {'core': CORE, 'kernel': KERNEL, 'driver': DRIVER, 'guard': GUARD}[key][number]
        identifier = f'{side}:{key}:{number}'
        annotations[identifier] = dict(side=side, path=PATHS[key], line=number, code=code, explanation=text,
                                       commit=manifest[side + '_commit'])
        return text

    blocks = []
    ranges = [
        ('fused', 'core', 'run_fused_range · mode3 完整编译分支', [(693, 751), (799, 800)]),
        ('order', 'core', 'prepare_order · 坐标分支', [(508, 534), (562, 563)]),
        ('mask', 'kernel', 'syndrome mask · 十二方向读取', [(20, 31)]),
        ('coordinate', 'core', 'coordinate_neighborhood · 已绑定入口', [(21, 25)]),
        ('pair', 'core', 'pair · 完整辅助函数', [(338, 356)]),
        ('finish', 'core', 'finish · 返回后的完整性判断', [(655, 668)]),
        ('caller', 'core', '外层 run · 调用/merge/finish 节选', [(834, 837)]),
        ('handoff', 'driver', 'prepare_timeline · handoff 边界节选', [(35, 47)]),
        ('guard', 'guard', 'AE入口 · mode3 串行限制节选', [(298, 301)]),
    ]
    for identifier, key, title, selected in ranges:
        rows = []
        for index, (start, end) in enumerate(selected):
            if index:
                rows.append(dict(gap='省略未选中的其他编译期策略；原始行号不重排，完整源文件可下载。'))
            for number in range(start, end + 1):
                code = sources['after', key][number - 1]
                rows.append(dict(key=f'{key}:{number}', line=number, code=code,
                                 explanation=explain('after', key, number) if code.strip() else '空行：分隔代码段。'))
        blocks.append(dict(id=identifier, title=title, source='sources/after/' + PATHS[key], lines=rows))

    # Use the retained official skill renderer; enhance its row functions with
    # separately stored commentary. The raw original Git patch stays untouched.
    spec = importlib.util.spec_from_file_location('method_diff_renderer', ROOT / 'scripts/render_code_diff.py')
    renderer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(renderer)
    old_side, old_unified = renderer.row_side, renderer.row_unified

    def note(side, number, text):
        if number is None or not text or not text.strip():
            return ''
        return '<span class="line-explanation" data-explanation="' + f'{side}:core:{number}' + '">' + html.escape(explain(side, 'core', number)) + '</span>'

    def side_row(old_num, old, new_num, new, old_class='', new_class=''):
        row = old_side(old_num, old, new_num, new, old_class, new_class)
        parts = row.split('</td>')
        parts[1] += note('before', old_num, old)
        parts[3] += note('after', new_num, new)
        return '</td>'.join(parts)

    def unified_row(old_num, new_num, line, cls=''):
        row = old_unified(old_num, new_num, line, cls)
        side = 'before' if cls == 'del' else 'after'
        number = old_num if side == 'before' else new_num
        return row.replace('</td></tr>', note(side, number, line) + '</td></tr>')

    renderer.row_side = side_row
    renderer.row_unified = unified_row
    diff_path = ROOT / 'code-diff/manifest.json'
    diff = json.loads(diff_path.read_text())
    if sha(ROOT / 'code-diff/diff.patch') != diff['artifacts']['diff.patch']['sha256']:
        raise ValueError('Original Git patch changed')
    records = []
    for record in diff['files']:
        record = dict(record)
        record['_old'] = (ROOT / 'sources/before' / record['old_path']).read_bytes()
        record['_new'] = (ROOT / 'sources/after' / record['new_path']).read_bytes()
        records.append(record)
    rendered = [renderer.render_file(record, i) for i, record in enumerate(records)]
    diff_html = renderer.document(diff['identity'], diff['scope']['literal_paths'], records, rendered, 1, 1, 1, 'full-page')
    diff_html = diff_html.replace('</style>', '.line-explanation{display:block;font:14px/1.6 system-ui,sans-serif;color:#3d586b;white-space:normal;margin:4px 0 8px;border-top:1px dotted #c1d0d7;padding-top:3px}.method-link{font:13px system-ui;margin-right:12px}</style>')
    diff_html = diff_html.replace('<h1>版本代码对照</h1>', '<a class="method-link" href="../index.html">← 模式3机制</a><h1>模式3 · 范围内完整差异</h1>')
    browser_checks = (ROOT / 'scripts/browser_checks.js').read_text()
    diff_html = diff_html.replace('</body>', '<script>' + browser_checks + '</script></body>')
    (ROOT / 'code-diff/index.html').write_text(diff_html)
    save(ROOT / 'line-explanations.json', dict(schema=1, revision='r1', entries=annotations))
    diff['artifacts']['index.html'] = dict(sha256=sha(ROOT / 'code-diff/index.html'), bytes=(ROOT / 'code-diff/index.html').stat().st_size)
    diff['annotation_generator'] = dict(path='../scripts/build_material.py', sha256=sha(Path(__file__)),
                                         explanations_sha256=sha(ROOT / 'line-explanations.json'))
    save(diff_path, diff)

    template = (ROOT / 'scripts/template.html').read_text()
    values = {'CSS': (ROOT / 'scripts/style.css').read_text(), 'SOURCE': json.dumps(dict(blocks=blocks), ensure_ascii=False),
              'MODEL': (ROOT / 'scripts/model.js').read_text(), 'APP': (ROOT / 'scripts/app.js').read_text()}
    for name, value in values.items():
        template = template.replace('@@' + name + '@@', value)
    template = template.replace('</body>', '<script>' + browser_checks + '</script></body>')
    (ROOT / 'index.html').write_text(template)
    coverage = dict(schema=1, optimization_id='simple-equal-share-mode3', decision='AE comparison policy; production default remains mode0',
                    performance=dict(status='not_included_in_method_note', numbers=None, official_baseline='e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'),
                    before=manifest['before_commit'], after=manifest['after_commit'],
                    html=dict(path='index.html', sha256=sha(ROOT / 'index.html'), id='simple-equal-share-mode3'),
                    publication=dict(kind='repository offline material', network_publication='not requested', entry='AE/results/20261112/method/index.html'),
                    comparison=dict(source_scope=manifest['diff_scope'], code_diff='code-diff/index.html', raw_patch='code-diff/diff.patch',
                                    note='before/after implementation increment; physical2 retained, physical3 added; not a performance denominator'),
                    interaction=dict(entry='OneHopPredecoder::run_fused_range', physical_mode=3, statistics=False,
                                     scenarios=['partial','complete','boundary','inactive'], storage_orders=['forward','reversed'],
                                     boundary='preinitialized grid/active/scratch through void return; caller finish/import shown separately'),
                    line_explanations=dict(path='line-explanations.json',sha256=sha(ROOT / 'line-explanations.json'),
                                           distinct_nonblank_lines=len(annotations), explained=len(annotations),missing=[]),
                    evidence=dict(native_correctness='evidence/mode3-validation.json',scope='existing development correctness checks; no new native executions',
                                  model='scripts/model.js', verification='verification/receipt.json'))
    save(ROOT / 'optimization-coverage.json', coverage)
    print(json.dumps({'complete': True, 'explained_source_lines': len(annotations), 'html_bytes': len(template.encode())}))


if __name__ == '__main__':
    main()
