#!/usr/bin/env python3
"""Prepare/collect Chromium file-URL checks without a debugging socket.

Use --prepare, run the printed direct Chromium commands, then use --collect.
The temporary harness appends checks to the exact shipped HTML bytes. Separate
screenshots open the unmodified shipped pages. No server or external URL is used.
"""
from pathlib import Path
from html.parser import HTMLParser
import argparse, hashlib, html, json, shlex

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "browser-verification"
HARNESS = Path("/tmp/beyond-report-browser-harness")
PAGES = ["index.html", "mechanisms/structured-primal-inlining.html", "mechanisms/certified-generic-predecode.html", "mechanisms/bounded-density-gate.html", "mechanisms/boundary-outline.html", "code-diff/index.html", "boundary-diff/index.html"]
TEST = r"""
<script>
(async()=>{
const result={page:PAGE,source_sha256:SHA,passed:false,scenarios:[],width:innerWidth};
const errors=[];window.addEventListener('error',e=>errors.push(String(e.message)));
const assert=(value,message)=>{if(!value)throw Error(message)};
const by=id=>document.querySelector(id);
const click=id=>by(id).click();
const select=(id,value)=>{by(id).value=value;by(id).dispatchEvent(new Event('change',{bubbles:true}))};
const slider=value=>{by('#progress').value=value;by('#progress').dispatchEvent(new Event('input',{bubbles:true}))};
const wait=ms=>new Promise(r=>setTimeout(r,ms));
try {
 if(PAGE.startsWith('mechanisms/')){
  assert(window.reportUI?.frames.length>0,'model initialized');
  await wait(30);result.initial_scroll_y=scrollY;assert(scrollY===0,'initial source highlight must not scroll the page');
  click('#next');assert(reportUI.index===1,'next');click('#prev');assert(reportUI.index===0,'previous');
  click('#play');await wait(580);click('#play');assert(reportUI.index>0,'play advances');
  click('#reset');assert(reportUI.index===0,'reset');
  click('#larger');assert(getComputedStyle(document.documentElement).getPropertyValue('--code-size').trim()==='16px','font larger');click('#smaller');
  for(const scene of Array.from(by('#scene').options,x=>x.value)){
   select('#scene',scene);const last=reportUI.frames.length-1;slider(last);
   assert(reportUI.index===last,'slider');assert(document.querySelectorAll('.code-row.current').length===1,'single source highlight');
   const state=JSON.parse(by('#state').textContent);assert(JSON.stringify(state)===JSON.stringify(reportUI.frames[last].state),'rendered state');
   result.scenarios.push({scene,frames:last+1,final_state:state});
  }
  for(const source of Array.from(by('#source').options,x=>x.value)){
   select('#source',source);const rows=document.querySelectorAll('.code-row');assert(rows.length>0,'source dropdown');
   assert(document.querySelectorAll('.code-row p').length===rows.length,'visible line explanations');
  }
  assert(getComputedStyle(by('#code')).resize==='vertical','resizable source pane');
  click('#reset');
 }else if(PAGE.startsWith('boundary-diff/')){
  assert(document.querySelectorAll('.line').length===28,'complete function on both sides');
  assert(document.querySelectorAll('.line p').length===28,'default line explanations');
  select('#view','unified');assert(document.body.classList.contains('unified'),'stacked view');
  select('#view','side');assert(!document.body.classList.contains('unified'),'side view');
  click('#up');assert(getComputedStyle(document.documentElement).getPropertyValue('--size').trim()==='16px','font larger');click('#down');
  assert([...document.querySelectorAll('.code')].every(x=>getComputedStyle(x).resize==='vertical'),'resizable code panes');
 }else if(PAGE.startsWith('code-diff/')){
  assert(document.querySelectorAll('details.file').length===11,'all source scopes');
  select('#view','side');assert(!document.body.classList.contains('unified-mode'),'side view');
  select('#view','unified');assert(document.body.classList.contains('unified-mode'),'unified view');
  click('#font-up');assert(by('#font-size').textContent==='16','diff font larger');click('#font-down');
  by('#search').value='missing-symbol-19854';by('#search').dispatchEvent(new Event('input',{bubbles:true}));assert(!by('#no-match').hidden,'search no match');
  by('#search').value='';by('#search').dispatchEvent(new Event('input',{bubbles:true}));
  click('#next');click('#prev');click('#focus');assert(document.body.classList.contains('focus-mode'),'focus');click('#focus');
  result.explained_rows=document.querySelectorAll('.line-explanation').length;assert(result.explained_rows>300,'diff line explanations');
 }
 await wait(100);scrollTo(0,0);
 result.document_width=document.documentElement.scrollWidth;result.body_width=document.body.scrollWidth;
 assert(result.document_width<=innerWidth+1&&result.body_width<=innerWidth+1,'horizontal overflow');
 assert(errors.length===0,errors.join(';'));
 result.resource_urls=performance.getEntriesByType('resource').map(x=>x.name);
 assert(result.resource_urls.every(x=>x.startsWith('file:')||x.startsWith('data:')),'external resource');
 result.passed=true;
}catch(error){result.error=String(error.stack||error)}
result.runtime_errors=errors;
const pre=document.createElement('pre');pre.id='browser-check-result';pre.textContent=JSON.stringify(result);document.body.replaceChildren(pre);
})()
</script>
"""


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare():
    assert json.loads((ROOT / "summary.json").read_text())["status"] == "complete"
    OUT.mkdir(exist_ok=True); HARNESS.mkdir(exist_ok=True)
    commands = []
    records = []
    for page in PAGES:
        source = ROOT / page
        stem = "report" if page == "index.html" else "code-diff" if page.startswith("code-diff") else "boundary-diff" if page.startswith("boundary-diff") else source.stem
        # The base element only resolves original relative resources in this temporary copy.
        document = source.read_text()
        document = document.replace('<meta charset="utf-8">', '<meta charset="utf-8"><base href="' + html.escape(source.as_uri()) + '">', 1)
        document += TEST.replace('PAGE', json.dumps(page)).replace('SHA', json.dumps(sha(source)))
        target = HARNESS / (stem + ".html");target.write_text(document)
        for width in (1440, 760):
            flags = "chromium --headless --no-sandbox --no-zygote --disable-gpu --disable-dev-shm-usage --disable-background-networking --no-first-run --user-data-dir=/tmp/beyond-report-r2-profile --hide-scrollbars"
            dump = OUT / f"{stem}-{width}.dom.html"
            screenshot = OUT / f"{stem}-{width}.png"
            # Keep output out of the terminal; the collector extracts one small JSON result.
            commands.append(f"{flags} --window-size={width},960 --virtual-time-budget=2500 --dump-dom {shlex.quote(target.as_uri())} > {shlex.quote(str(dump))}")
            commands.append(f"{flags} --window-size={width},960 --virtual-time-budget=400 --screenshot={shlex.quote(str(screenshot))} {shlex.quote(source.as_uri())}")
            records.append({"page": page, "width": width, "dump": dump.name, "screenshot": screenshot.name, "source_sha256": sha(source)})
    (OUT / "plan.json").write_text(json.dumps(records, indent=2) + "\n")
    (OUT / "run.sh").write_text("#!/bin/sh\nset -eu\n" + "\n".join(commands) + "\n")
    print("\n".join(commands))


class Result(HTMLParser):
    def __init__(self):
        super().__init__();self.inside=False;self.parts=[]
    def handle_starttag(self, tag, attrs):
        if tag=='pre' and dict(attrs).get('id')=='browser-check-result':self.inside=True
    def handle_endtag(self, tag):
        if tag=='pre':self.inside=False
    def handle_data(self, data):
        if self.inside:self.parts.append(data)


def collect():
    plan=json.loads((OUT/'plan.json').read_text());records=[]
    for item in plan:
        assert sha(ROOT/item['page'])==item['source_sha256'],item['page']
        parser=Result();parser.feed((OUT/item['dump']).read_text())
        assert parser.parts,('No Chromium result',item)
        result=json.loads(''.join(parser.parts))
        assert result['passed'],result
        assert result['source_sha256']==item['source_sha256'] and result['width']==item['width']
        assert (OUT/item['screenshot']).stat().st_size>1000
        result['screenshot']=str((OUT/item['screenshot']).relative_to(ROOT))
        records.append(result)
    receipt={'status':'passed','browser':'Chromium 143.0.7499.40 headless','mode':'File-URL self-checks; exact shipped content plus appended temporary test script; screenshots use original shipped HTML',
             'pages':records,'page_sha256':{r['page']:r['source_sha256'] for r in plan},
             'scenarios_per_viewport':sum(len(r['scenarios']) for r in records if r['width']==1440),
             'viewport_widths':[1440,760],'runtime_errors':[],
             'external_network_requests':[],
             'qualification':'Browser resource entries verified local-only; no CDP request interception. DOM controls dispatch real click/change/input events; native correctness is separate.'}
    (ROOT/'browser-verification.json').write_text(json.dumps(receipt,indent=2,ensure_ascii=False)+'\n')
    # Generated DOM dumps duplicate every source page and are intentionally excluded.
    for item in plan:(OUT/item['dump']).unlink()
    print(json.dumps({'status':'passed','pages':len(PAGES),'viewports':2,'scenarios_per_viewport':receipt['scenarios_per_viewport']}))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);g=p.add_mutually_exclusive_group(required=True);g.add_argument('--prepare',action='store_true');g.add_argument('--collect',action='store_true');a=p.parse_args()
    prepare() if a.prepare else collect()
