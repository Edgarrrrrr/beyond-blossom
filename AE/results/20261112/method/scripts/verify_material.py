#!/usr/bin/env python3
"""Check sealed source bytes, Chinese line coverage and local HTML links."""
from pathlib import Path
import hashlib
from html.parser import HTMLParser
import json
import re
import subprocess
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()


class Links(HTMLParser):
    def __init__(self):
        super().__init__(); self.links=[]; self.resources=[]
    def handle_starttag(self, tag, attrs):
        attrs=dict(attrs)
        if 'href' in attrs:self.links.append(attrs['href'])
        if 'src' in attrs:self.resources.append(attrs['src'])


def main():
    source=json.loads((ROOT/'sources/manifest.json').read_text())
    for relative,record in source['files'].items():
        assert sha(ROOT/relative)==record['sha256'],relative
    explanations=json.loads((ROOT/'line-explanations.json').read_text())['entries']
    for key,entry in explanations.items():
        lines=(ROOT/'sources'/entry['side']/entry['path']).read_text().splitlines()
        assert lines[entry['line']-1]==entry['code'],key
        assert entry['explanation'] and re.search('[\u4e00-\u9fff]',entry['explanation']),key
    pages=['index.html','code-diff/index.html'];links=0
    for name in pages:
        parser=Links();parser.feed((ROOT/name).read_text())
        assert not parser.resources,'Pages must contain all rendering resources inline'
        for target in parser.links:
            url=urlsplit(target)
            assert not url.scheme and not url.netloc,'No external HTML links/resources in the offline page'
            if not url.path:continue
            path=((ROOT/name).parent/unquote(url.path)).resolve()
            assert ROOT in path.parents,'Link leaves this standalone material'
            if path==ROOT/'verification/receipt.json':continue
            assert path.is_file(),str(path);links+=1
    diff=json.loads((ROOT/'code-diff/manifest.json').read_text())
    for name,entry in diff['artifacts'].items():assert sha(ROOT/'code-diff'/name)==entry['sha256']
    text=(ROOT/'index.html').read_text()
    payload=json.loads(re.search(r'const SOURCE=(.*?);</script>',text,re.S).group(1))
    displayed=[]
    for block in payload['blocks']:
        assert (ROOT/block['source']).is_file()
        for row in block['lines']:
            if 'gap' in row or not row['code'].strip():continue
            key='after:'+row['key'];assert key in explanations and row['explanation']==explanations[key]['explanation'];displayed.append(key)
    for key in re.findall(r'data-explanation="([^"]+)"',(ROOT/'code-diff/index.html').read_text()):
        assert key in explanations;displayed.append(key)
    assert set(displayed)==set(explanations),'Coverage must count distinct displayed source lines only'
    before={name:sha(ROOT/name) for name in pages+['line-explanations.json','optimization-coverage.json','code-diff/manifest.json']}
    subprocess.run(['python3',str(ROOT/'scripts/build_material.py')],check=True,capture_output=True)
    assert before=={name:sha(ROOT/name) for name in before},'Regeneration differs'
    model=json.loads(subprocess.check_output(['node',str(ROOT/'scripts/verify_model.js')],text=True))
    browser={'scope':'Static checks alone do not prove rendering.'}
    browser_path=ROOT/'evidence/browser-validation.json'
    if browser_path.exists():
        verified=json.loads(browser_path.read_text());assert verified['complete']
        for item in verified['validated_pages']:
            html=ROOT/('index.html' if item['page']=='mechanism' else 'code-diff/index.html')
            assert item['html_sha256']==sha(html)
            assert item['receipt_sha256']==sha(ROOT/'evidence'/item['receipt'])
        browser={'path':'evidence/browser-validation.json','sha256':sha(browser_path),'validated_page_viewports':len(verified['validated_pages'])}
    receipt={'passed':True,'scope':'Frozen-source equality, displayed-line coverage, local links, deterministic regeneration, teaching-model semantics; no native build or benchmark',
             'source_files':len(source['files']),'distinct_explained_nonblank_lines':len(explanations),'local_links':links,'model':model,
             'presentation_sha256':before,'browser':browser}
    (ROOT/'verification/receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(receipt,ensure_ascii=False))


if __name__=='__main__':main()
