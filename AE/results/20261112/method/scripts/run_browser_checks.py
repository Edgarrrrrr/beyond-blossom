#!/usr/bin/env python3
"""Run four local Chromium checks; preserve each attempt and browser profile."""
import argparse
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


class BrowserReceipt(HTMLParser):
    def __init__(self):
        super().__init__();self.capture=False;self.value=''
    def handle_starttag(self,tag,attrs):
        if tag=='pre' and dict(attrs).get('id')=='browser-verification':self.capture=True
    def handle_endtag(self,tag):
        if tag=='pre':self.capture=False
    def handle_data(self,value):
        if self.capture:self.value+=value


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--attempt',required=True)
    parser.add_argument('--pages',nargs='+',choices=['mechanism','diff'],default=['mechanism','diff'])
    parser.add_argument('--prepare-only',action='store_true')
    args=parser.parse_args()
    if not args.attempt.replace('-','').replace('_','').isalnum():parser.error('Use a simple attempt name')
    output=args.output.resolve();attempt=output/'attempts'/args.attempt
    if attempt.exists():parser.error('Preserve the existing attempt; choose another name')
    attempt.mkdir(parents=True)
    commands=[]
    for page,relative in [('mechanism','index.html'),('diff','code-diff/index.html')]:
        if page not in args.pages:continue
        for width in [1440,760]:
            name=f'{page}-{width}';profile=output/'profiles'/args.attempt/name
            url=(ROOT/relative).as_uri()+'?verify=1'+('&scene=partial&step=50' if page=='mechanism' else '')
            command=['chromium','--headless','--no-sandbox','--no-zygote','--disable-gpu','--disable-dev-shm-usage',
                     '--disable-background-networking','--no-first-run','--no-default-browser-check','--hide-scrollbars',
                     '--user-data-dir='+str(profile),'--window-size='+str(width)+',1000','--virtual-time-budget=2000',
                     '--screenshot='+str(attempt/(name+'.png')),'--dump-dom',url]
            commands.append({'name':name,'html':str(ROOT/relative),'html_sha256':sha(ROOT/relative),
                             'argv':command,'stdout':str(attempt/(name+'.dom.html')),'stderr':str(attempt/(name+'.log'))})
    (attempt/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    if args.prepare_only:print(attempt/'commands.json');return
    results=[]
    for call in commands:
        try:
            with Path(call['stdout']).open('w')as stdout,Path(call['stderr']).open('w')as stderr:
                completed=subprocess.run(call['argv'],stdout=stdout,stderr=stderr,timeout=45)
            if completed.returncode:raise RuntimeError(f'Chromium exited {completed.returncode}')
            parsed=BrowserReceipt();parsed.feed(Path(call['stdout']).read_text())
            result=json.loads(parsed.value)
            if not result['passed']:raise RuntimeError(result.get('error','Browser control check failed'))
            if sha(Path(call['html']))!=call['html_sha256']:raise RuntimeError('HTML changed during validation')
            result.update(name=call['name'],html_sha256=call['html_sha256'],screenshot_sha256=sha(attempt/(call['name']+'.png')))
            (attempt/(call['name']+'.json')).write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
            results.append(result);print(call['name'],'passed',flush=True)
        except Exception as error:
            (attempt/'failure.json').write_text(json.dumps({'complete':False,'call':call,'error':str(error)},indent=2)+'\n')
            raise
    (attempt/'receipt.json').write_text(json.dumps({'complete':True,'scope':'Selected standalone local pages at two widths; DOM controls and screenshots. No native decoder or performance execution.','pages':args.pages,'results':results},ensure_ascii=False,indent=2)+'\n')
    print(attempt/'receipt.json')


if __name__=='__main__':main()
