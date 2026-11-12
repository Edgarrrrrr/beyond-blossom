#!/usr/bin/env python3
"""Check the compact final archive; never decode, compile, submit, or read large inputs.

Run only after the final manifest and presentation assets have been frozen:
  python3 final-check.py
The machine-readable result is final-check.json beside this script.
"""
from __future__ import annotations
import argparse
import collections
import csv
import datetime
import hashlib
import html
from html.parser import HTMLParser
import io
import json
import math
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

SOURCE = '75aedd433d1b129e5a973635320f199b5f26cbae'
METHODS = ['reference', 'structured', 'predecoded', 'component-parallel', 'pipelined', 'parallel-predecode']
EXTERNAL = ['parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf']
STEMS = ['incremental_ablation_1107_v1', 'decoder_comparison_shows_speedup_v3', 'decoding_time', 'remaining_syndromes']
MAX_FILE = 64 * 1024 * 1024
MAX_TOTAL = 2 * 1024 * 1024 * 1024
FORBIDDEN = {'dataset.bin', 'native_shots.csv', 'input_features.csv', 'native-input.json', 'graph.json', 'oracle.jsonl'}

class HtmlLinks(HTMLParser):
    def __init__(self):
        super().__init__(); self.links = []; self.anchors = set()
    def handle_starttag(self, tag, attrs):
        fields = dict(attrs)
        if fields.get('id'): self.anchors.add(fields['id'])
        if tag == 'a' and fields.get('name'): self.anchors.add(fields['name'])
        for attr in ['href', 'src', 'poster']:
            if fields.get(attr): self.links.append(fields[attr])

class Checker:
    def __init__(self, repo, work, archive):
        self.repo, self.work, self.archive = repo, work, archive
        self.issues = []; self.sections = []; self.cache = {}; self.bytes = 0
        self.link_records = []; self.notes = []
    def need(self, ok, message, **details):
        if not ok: self.issues.append({'message': message, **details})
    def section(self, name, action):
        before = len(self.issues)
        try: detail = action()
        except (OSError, ValueError, KeyError, TypeError, AttributeError, AssertionError, StopIteration) as ex:
            self.issues.append({'message': 'Section failed', 'section': name, 'error': str(ex)}); detail = None
        self.sections.append({'name': name, 'passed': len(self.issues) == before, 'detail': detail})
    def payload(self, path):
        p = Path(path).resolve()
        if p.name in FORBIDDEN or p.name.endswith('.corrections.jsonl'):
            raise ValueError('Refusing large raw-input/per-shot artifact: ' + str(p))
        stat = p.stat(); stamp = (stat.st_size, stat.st_mtime_ns)
        if p in self.cache:
            cached = self.cache[p]
            if cached[0] != stamp: raise ValueError('File changed during check: ' + str(p))
            return cached[1]
        if stat.st_size > MAX_FILE or self.bytes + stat.st_size > MAX_TOTAL:
            raise ValueError('Compact-audit byte limit exceeded: ' + str(p))
        value = p.read_bytes(); after = p.stat()
        if stamp != (after.st_size, after.st_mtime_ns): raise ValueError('File changed while reading: ' + str(p))
        self.cache[p] = (stamp, value, hashlib.sha256(value).hexdigest()); self.bytes += len(value)
        return value
    def digest(self, path):
        self.payload(path); return self.cache[Path(path).resolve()][2]
    def load(self, path): return json.loads(self.payload(path))
    def sealed(self, path, expected):
        actual = self.digest(path)
        self.need(actual == expected, 'SHA256 mismatch', path=str(path), expected=expected, actual=actual)
    def local(self, relative):
        p = (self.archive / relative).resolve()
        if not p.is_relative_to(self.archive): raise ValueError('Archive path escapes root: ' + str(relative))
        return p
    def manifest_check(self):
        self.manifest = self.load(self.archive / 'manifest.json')
        self.manifest_sha = self.digest(self.archive / 'manifest.json')
        m = self.manifest
        self.need(m.get('schema') == 1 and m.get('source_commit') == SOURCE, 'Unexpected final manifest schema/source')
        for name, expected in m['files'].items(): self.sealed(self.local(name), expected)
        actual = {str(p.relative_to(self.archive)) for p in self.archive.rglob('*')
                  if p.is_file() and p != self.archive / 'manifest.json' and '__pycache__' not in p.parts}
        self.need(actual == set(m['files']), 'Manifest file membership differs', unsealed=sorted(actual-set(m['files'])), missing=sorted(set(m['files'])-actual))
        self.sealed(self.repo / 'AE/export_results.py', m['exporter_sha256'])
        self.sealed(self.work / 'source/AE/export_results.py', m['original_exporter_sha256'])
        return {'files': len(m['files']), 'manifest_sha256': self.manifest_sha,
                'current_exporter_sha256': m['exporter_sha256'], 'original_exporter_sha256': m['original_exporter_sha256']}
    def original_export(self):
        receipt = self.load(self.archive / 'export-complete.json'); original = self.work / 'slurm-paper/exports'
        self.sealed(self.archive / 'export-complete.json', self.digest(original / 'complete.json'))
        self.need(receipt.get('complete') and receipt['profile'] == 'paper', 'Original export is not completed paper data')
        self.need(receipt['exporter']['sha256'] == self.manifest['original_exporter_sha256'], 'Original exporter identity overwritten')
        for relative, expected in receipt['files'].items():
            self.sealed(self.local(relative), expected); self.sealed(original / relative, expected)
        for figure in receipt['figures']:
            relative = Path(figure['path']).relative_to(original)
            self.sealed(self.local(relative), figure['sha256'])
        self.need(len(receipt['figures']) == 12, 'Expected twelve original figure assets')
        return {'original_export_files': len(receipt['files']), 'original_figures': len(receipt['figures']), 'preserved': True}
    def provenance(self):
        p = self.load(self.archive / 'archive-provenance.json')
        self.need(p['source_commit'] == SOURCE and Path(p['persistent_raw_root']).resolve() == self.work, 'Archive provenance source/root mismatch')
        for relative, source in p['files'].items():
            self.sealed(self.local(relative), source['sha256'])
            self.sealed(Path(source['original_path']), source['sha256'])
        return {'byte_preserved_files': len(p['files']), 'original_and_archived_bytes_compared': True}
    def presentation(self):
        names = set(self.manifest['files'])
        presentation = [name for name in names if name.startswith('presentation/')]
        for stem in STEMS:
            for suffix in ['.pdf', '.svg', '.png']:
                matches = [name for name in presentation if Path(name).name == stem + suffix]
                self.need(len(matches) == 1, 'Missing/duplicated presentation figure', stem=stem, suffix=suffix, matches=matches)
        receipts = [name for name in presentation if Path(name).name == 'replot.json']
        self.need(len(receipts) == 1, 'Expected one presentation redraw receipt', candidates=receipts)
        if receipts:
            path = self.local(receipts[0]); receipt = self.load(path)
            self.need(receipt.get('complete') and receipt.get('profile') == 'paper', 'Presentation redraw is not complete paper scope')
            self.need(receipt['sha256']['canonical'] == self.digest(self.archive/'canonical.json') and receipt['sha256']['exporter'] == self.manifest['exporter_sha256'], 'Presentation input/renderer differs from final manifest')
            snapshot_path=self.archive/'evidence/validation/redraw-input-manifest.json'
            self.sealed(snapshot_path,receipt['sha256']['manifest'])
            snapshot=self.load(snapshot_path)
            self.need(snapshot['source_commit']==SOURCE and snapshot['files']['canonical.json']==receipt['sha256']['canonical'] and snapshot['exporter_sha256']==receipt['sha256']['exporter'],'Redraw input-manifest snapshot differs from canonical/renderer identities')
            relocated={}
            for relative, expected in receipt['files'].items():
                original=Path(relative)
                if len(original.parts)!=2 or original.parts[0]!='figures':
                    raise ValueError('Unexpected redraw figure path: '+relative)
                target=path.parent/original.name
                self.sealed(target,expected)
                relocated[relative]=str(target.relative_to(self.archive))
            self.notes.append('The redraw receipt preserves original figures/<name> paths. Archive assets are explicitly relocated to presentation/<name>; all twelve hashes are checked. The recorded input-manifest hash is verified against evidence/validation/redraw-input-manifest.json, avoiding final-manifest self-reference.')
        cards = [name for name in names if not name.startswith('evidence/') and re.search(r'(summary|card)', Path(name).stem, re.I) and Path(name).suffix in {'.svg','.png','.pdf','.webp'}]
        self.need(bool(cards), 'No sealed summary-card image found')
        return {'presentation_assets': len(presentation), 'summary_card_assets': sorted(cards), 'receipt_to_archive_paths': relocated if receipts else {}}
    def data(self):
        c = self.load(self.archive/'canonical.json'); self.need(c['complete'] and c['profile']=='paper', 'Canonical is not completed paper data')
        cases = {(d,p) for d in [9,13,17,21,25,29,33] for p in ['0.0005','0.001','0.003']}
        for group, methods, shots, count in [('native', METHODS, 1000000, 126),('external',EXTERNAL,10000,105)]:
            rows = c[group]; keys = {(r['distance'],str(r['error_rate']),r['method']) for r in rows}
            self.need(len(rows)==count and len(keys)==count and keys=={(d,p,m) for d,p in cases for m in methods}, 'Incomplete/duplicate canonical matrix', group=group)
            for row in rows:
                self.need(row['shots']==shots and row['repeats']==3 and len(row['repeat_elapsed_ns'])==3, 'Wrong sample/repeat count', group=group)
                computed=sum(row['repeat_elapsed_ns'])/(shots*3*1000)
                self.need(math.isclose(computed,row['mean_us'],rel_tol=1e-12), 'Mean does not match integer elapsed', group=group,distance=row['distance'],method=row['method'])
        policy=c['predecoder']; self.need(len(policy)==21 and {(r['distance'],str(r['error_rate']),r['policy']) for r in policy}=={(d,'0.003',p) for d in [9,13,17,21,25,29,33] for p in [0,1,2]},'Incomplete policy matrix')
        for row in policy:
            self.need(row['execution']=='serial' and row['threads']==1 and row['physical_policy']=={0:0,1:1,2:3}[row['policy']] and row['shots']==1000000 and row['repeats']==3, 'Wrong serial policy scope')
            self.need(math.isclose(sum(row['repeat_elapsed_ns'])/3000000000,row['mean_us'],rel_tol=1e-12), 'Wrong policy integer mean')
        csv_counts={}
        for name, expected in [('native.csv',126),('external.csv',105),('predecoder.csv',21),('selected-configurations.csv',21)]:
            rows=list(csv.DictReader(io.StringIO(self.payload(self.archive/name).decode())))
            csv_counts[name]=len(rows);self.need(len(rows)==expected,'CSV row count differs',file=name,expected=expected,actual=len(rows))
        audits={name:self.load(self.archive/'evidence/validation'/name) for name in ['main-matrix-audit.json','policy-matrix-audit.json','pmu-audit.json']}
        self.need(audits['main-matrix-audit.json']['audit_passed'] and audits['policy-matrix-audit.json']['complete'] and audits['pmu-audit.json']['complete'],'Independent audits did not pass')
        self.need(audits['main-matrix-audit.json']['counts']['native_calls']==378 and audits['policy-matrix-audit.json']['checks']['timing_calls']==63 and audits['policy-matrix-audit.json']['checks']['statistics_calls']==21 and audits['pmu-audit.json']['checks']['calls']==4,'Unexpected independent audit scopes')
        self.need(not c['m4'],'Unexpected fresh M4 measurements')
        return {'canonical_rows':{g:len(c[g]) for g in ['native','external','predecoder']},'csv_rows':csv_counts,'native_calls':378,'policy_timing_calls':63,'policy_statistics_calls':21,'pmu_calls':4,'fresh_m4_rows':0}
    def scheduler(self):
        run=self.work/'slurm-paper'; done=self.load(run/'complete.json'); submissions=self.load(run/'submissions.json'); states=self.load(run/'scheduler/states.json'); plan=self.load(run/'plan.json')
        jobs={items[-1]['job_id'] for items in submissions['workers'].values()}|{submissions['exports'][-1]['job_id']}
        self.need(done['complete'] and done['profile']=='paper' and done['cases']==21 and len(jobs)==7 and set(states)==jobs,'Full completion or seven scheduler jobs missing')
        self.sealed(run/'plan.json',done['plan_sha256']); self.sealed(run/'shards.json',done['shards_sha256']); self.sealed(Path(done['export'])/'complete.json',done['export_receipt_sha256'])
        actual={}
        for job in sorted(jobs):
            state=states[job]; text=self.payload(run/'scheduler'/f'job-{job}.txt').decode(); fields=dict(re.findall(r'(?<!\S)([A-Za-z][A-Za-z0-9/]*)=([^\s]+)',text))
            self.need(state['state']=='COMPLETED' and state['exit_code']=='0:0' and fields.get('JobId')==job and fields.get('JobState')=='COMPLETED' and fields.get('ExitCode')=='0:0','Scheduler terminal evidence differs',job=job)
            self.need(fields.get('NodeList') in {f'ares{i}' for i in range(1,7)},'Job ran outside requested nodes',job=job)
            actual[job]={k:fields.get(k) for k in ['JobState','ExitCode','NodeList','RunTime','StartTime','EndTime']}
        for shard in plan['shards']:
            job=submissions['workers'][str(shard['index'])][-1]['job_id']
            self.need(actual[job]['NodeList']==shard['node'],'Worker scheduler node disagrees with assignment',job=job)
        return {'jobs':actual,'evidence':'Saved terminal scontrol records and structured states; no new scheduler request or job submission.'}
    def anchors(self,path):
        text=self.payload(path).decode(); parser=HtmlLinks();parser.feed(text);found=set(parser.anchors)
        if path.suffix.lower() in {'.md','.markdown'}:
            counts=collections.Counter()
            for heading in re.findall(r'^\s{0,3}#{1,6}\s+(.+?)\s*#*$',text,re.M):
                heading=re.sub(r'\[([^]]+)\]\([^)]*\)',r'\1',heading)
                slug=html.unescape(re.sub(r'<[^>]+>','',heading)).lower().strip()
                slug=''.join(ch for ch in slug if ch.isalnum() or ch in ' -_')
                slug=re.sub(r'\s','-',slug);n=counts[slug];counts[slug]+=1;found.add(slug+(f'-{n}' if n else ''))
        return found
    def links(self):
        pages=[self.repo/'README.md',self.repo/'AE/README.md',self.repo/'AE/REFERENCE.md',self.archive/'README.md',self.archive/'REPRODUCE.md',self.archive/'index.html']
        for page in pages:
            text=self.payload(page).decode();parser=HtmlLinks();parser.feed(text);links=parser.links
            if page.suffix=='.md':
                text=re.sub(r'^\s*(`{3,}|~{3,})[^\n]*\n.*?^\s*\1\s*$', '',text,flags=re.M|re.S)
                links += re.findall(r'!?\[[^\]\n]*\]\(\s*(<[^>]+>|[^\s)]+)(?:\s+[\"\'][^\n]*?[\"\'])?\s*\)',text)
                links += re.findall(r'^\s{0,3}\[[^\]]+\]:\s*(<[^>]+>|\S+)',text,re.M)
            for link in links:
                link=html.unescape(link.strip('<>'));u=urlsplit(link)
                if u.scheme or u.netloc or link.startswith('//'):continue
                path=unquote(u.path);target=page if not path else ((self.repo/path.lstrip('/')) if path.startswith('/') else page.parent/path).resolve()
                exists=target.exists();self.need(exists,'Broken local link',page=str(page),link=link,target=str(target))
                if exists and u.fragment and target.suffix.lower() in {'.md','.markdown','.html','.htm'}:
                    self.need(unquote(u.fragment) in self.anchors(target),'Missing local fragment',page=str(page),link=link,target=str(target))
                self.link_records.append({'page':str(page.relative_to(self.repo)),'link':link,'exists':exists})
        return {'pages':len(pages),'local_links':len(self.link_records),'links':self.link_records}
    def stable(self):
        changed=[]
        for p,(stamp,_,_) in self.cache.items():
            stat=p.stat()
            if stamp!=(stat.st_size,stat.st_mtime_ns):changed.append(str(p))
        self.need(not changed,'Inputs changed during final check',paths=changed)
        return {'unique_compact_files_read':len(self.cache),'unique_bytes_read':self.bytes,'per_file_limit':MAX_FILE,'total_limit':MAX_TOTAL,'no_raw_datasets_or_per_shot_streams':True}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo',type=Path,default=Path('/home/fujy/dev/beyond_blossom/repo'))
    parser.add_argument('--work',type=Path,default=Path(__file__).resolve().parent)
    parser.add_argument('--archive',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args(); repo=args.repo.resolve();work=args.work.resolve();archive=(args.archive or repo/'AE/results/20261112').resolve()
    c=Checker(repo,work,archive)
    for name,fn in [('final_manifest',c.manifest_check),('original_export',c.original_export),('byte_preserved_provenance',c.provenance),('presentation_and_summary_card',c.presentation),('matrix_and_audit_scopes',c.data),('scheduler_terminal',c.scheduler),('local_links',c.links),('inputs_stable',c.stable)]:c.section(name,fn)
    report={'schema':1,'complete':not c.issues,'observed_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'repo':str(repo),'archive':str(archive),'work':str(work),'scope':'Final compact archive integrity, row counts, link validity and saved scheduler terminal records. No decoding, compilation, new measurement, network query, job submission or large input/per-shot reads.','sections':c.sections,'issues':c.issues,'notes':c.notes}
    output=(args.output or work/'final-check.json').resolve();output.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
    print(json.dumps({'complete':report['complete'],'issues':len(c.issues),'output':str(output),'sections':[{k:s[k] for k in ['name','passed']} for s in c.sections]},ensure_ascii=False))
    return 0 if report['complete'] else 1
if __name__=='__main__':raise SystemExit(main())
