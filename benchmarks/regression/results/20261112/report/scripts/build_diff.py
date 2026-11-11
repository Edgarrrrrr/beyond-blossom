#!/usr/bin/env python3
"""Use the skill diff generator and add verified per-original-line explanations."""
from pathlib import Path
import hashlib,html,importlib.util,json
ROOT=Path(__file__).resolve().parents[1]
TOOL=ROOT/'scripts/render_code_diff.py'
spec=importlib.util.spec_from_file_location('skill_diff',TOOL);D=importlib.util.module_from_spec(spec);spec.loader.exec_module(D)
M=json.loads((ROOT/'sources/manifest.json').read_text());A=json.loads((ROOT/'sources/line-explanations.json').read_text())
active=None
render_file=D.render_file

def entry(side,num):
    if not num:return None
    rows=A['excerpts'][active].get(side,[])
    return rows[num-1] if rows else None

def cell(side,num,line,cls):
    e=entry(side,num)
    if line is None:return '<td class="ln"></td><td class="code"></td>'
    assert e and e['source']==line.rstrip('\n')
    marker='−' if cls=='del' else '+' if cls=='add' else ' '
    return f'<td class="ln">{e["line"]}</td><td class="code {cls}"><span class="marker">{marker}</span>{D.code_cell(line)}<span class="line-explanation">{html.escape(e["explanation"])}</span></td>'

def side(old_num,old,new_num,new,old_class='',new_class=''):
    return '<tr>'+cell('before',old_num,old,old_class)+cell('after',new_num,new,new_class)+'</tr>'

def unified(old_num,new_num,line,cls=''):
    side='after' if new_num else 'before';e=entry(side,new_num or old_num)
    assert e['source']==line.rstrip('\n')
    old=entry('before',old_num);new=entry('after',new_num)
    marker='−' if cls=='del' else '+' if cls=='add' else ' '
    return f'<tr><td class="ln">{old["line"] if old else ""}</td><td class="ln">{new["line"] if new else ""}</td><td class="code {cls}"><span class="marker">{marker}</span>{D.code_cell(line)}<span class="line-explanation">{html.escape(e["explanation"])}</span></td></tr>'

def file(record,index):
    global active
    active=Path(record['new_path'] or record['old_path']).stem
    body=render_file(record,index)
    entries=M['excerpts'][active]
    refs=[]
    for label,v in entries.items():
        if v:refs.append(f'{label}: {v["commit"][:12]} · {v["path"]}:{v["start"]}–{v["end"]}')
    note='<p class="source-map">'+'<br>'.join(html.escape(x) for x in refs)+'</p>'
    return body.replace('<div class="change"',note+'<div class="change"',1)
D.row_side=side;D.row_unified=unified;D.render_file=file
D.CSS+='\n.line-explanation{display:block;white-space:normal;font:13px/1.5 system-ui;color:#38516b;padding:3px 3px 7px;border-bottom:1px solid #dbe5ef}.source-map{font:12px/1.5 system-ui;margin:8px;overflow-wrap:anywhere}.code{white-space:pre-wrap;overflow-wrap:anywhere}.scroll{max-height:none}body.unified-mode .code{white-space:pre-wrap}html{scroll-behavior:smooth}\n'
args=['--base-dir',str(ROOT/'sources/before'),'--candidate-dir',str(ROOT/'sources/after'),
      '--base-label','Exact excerpts of source-2411603 (original 241160314087)',
      '--candidate-label','Exact excerpts of implementation edd29b678071',
      '--presentation','full-page','--initial-path','structured_dispatch.cc','--out',str(ROOT/'code-diff')]
if D.main(args):raise SystemExit(1)
p=ROOT/'code-diff/manifest.json';m=json.loads(p.read_text())
m['source_mapping']={'path':'../sources/manifest.json','sha256':hashlib.sha256((ROOT/'sources/manifest.json').read_bytes()).hexdigest()}
m['line_explanations']={'path':'../sources/line-explanations.json','sha256':hashlib.sha256((ROOT/'sources/line-explanations.json').read_bytes()).hexdigest(),'explained_nonempty_lines':A['nonempty_lines'],'missing':[]}
m['annotation_generator_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
m['scope']['note']='Complete diff of declared exact function/fragment excerpts, not all repository changes. Displayed line numbers are original-file line numbers; patch hunk coordinates are excerpt-file coordinates.'
p.write_text(json.dumps(m,indent=2,ensure_ascii=False)+'\n')
