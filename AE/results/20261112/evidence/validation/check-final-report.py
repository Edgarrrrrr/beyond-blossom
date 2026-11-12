#!/usr/bin/env python3
"""Check the final offline report in Chromium and retain exact-page screenshots."""
from pathlib import Path
from html.parser import HTMLParser
import hashlib, html, json, subprocess

work = Path(__file__).resolve().parent
root = work.parent.parent / 'repo/AE/results/20261112'
out = work / 'final-report-browser'
out.mkdir(exist_ok=True)
source = root / 'index.html'
digest = hashlib.sha256(source.read_bytes()).hexdigest()
script = r'''
<script>
window.addEventListener('load', () => {
 const result={passed:false,width:innerWidth};
 try {
  const assert=(v,m)=>{if(!v)throw Error(m)};
  assert(document.documentElement.scrollWidth<=innerWidth+1,'document overflow');
  const imgs=[...document.images];
  assert(imgs.length===5&&imgs.every(i=>i.complete&&i.naturalWidth>0),'images loaded');
  assert(document.querySelectorAll('tbody tr').length===9,'measurement tables');
  const resources=performance.getEntriesByType('resource').map(r=>r.name);
  assert(resources.every(r=>r.startsWith('file:')||r.startsWith('data:')),'local resources');
  result.resources=resources;result.image_sizes=imgs.map(i=>[i.naturalWidth,i.naturalHeight]);
  result.document_width=document.documentElement.scrollWidth;result.passed=true;
 }catch(e){result.error=String(e)}
 const p=document.createElement('pre');p.id='validation';p.textContent=JSON.stringify(result);
 document.body.replaceChildren(p);
});
</script>'''
harness = out / 'harness.html'
harness.write_text(source.read_text().replace('<meta charset="utf-8">', '<meta charset="utf-8"><base href="'+root.as_uri()+'/">') + script)
class Result(HTMLParser):
    def __init__(self): super().__init__(); self.inside=False; self.parts=[]
    def handle_starttag(self,tag,attrs):
        if tag=='pre' and dict(attrs).get('id')=='validation': self.inside=True
    def handle_endtag(self,tag):
        if tag=='pre': self.inside=False
    def handle_data(self,data):
        if self.inside: self.parts.append(data)
checks=[]
for width in (1440,760):
    base=['chromium','--headless','--no-sandbox','--no-zygote','--disable-gpu','--disable-dev-shm-usage','--disable-background-networking','--no-first-run','--no-default-browser-check','--hide-scrollbars',f'--window-size={width},1000','--virtual-time-budget=2500']
    dump=out/f'{width}.dom.html'
    with dump.open('w') as stdout, (out/f'{width}.log').open('w') as stderr:
        subprocess.run(base+[f'--user-data-dir={out}/profile-check-{width}','--dump-dom',harness.as_uri()],stdout=stdout,stderr=stderr,check=True,timeout=60)
    parsed=Result();parsed.feed(dump.read_text());result=json.loads(''.join(parsed.parts))
    assert result['passed'] and result['width']==width,result
    screenshot=out/f'{width}.png'
    with (out/f'{width}-screenshot.log').open('w') as stderr:
        subprocess.run(base+[f'--user-data-dir={out}/profile-shot-{width}',f'--screenshot={screenshot}',source.as_uri()],stdout=subprocess.DEVNULL,stderr=stderr,check=True,timeout=60)
    result['screenshot_sha256']=hashlib.sha256(screenshot.read_bytes()).hexdigest()
    checks.append(result)
assert hashlib.sha256(source.read_bytes()).hexdigest()==digest
receipt={'passed':True,'source_sha256':digest,'checks':checks,'scope':'Appended DOM checks; screenshots of the unmodified offline report. Visual review recorded separately.'}
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({'passed':True,'viewports':[1440,760]}))
