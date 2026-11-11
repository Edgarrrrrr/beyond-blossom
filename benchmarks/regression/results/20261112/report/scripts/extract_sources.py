#!/usr/bin/env python3
"""Read immutable Git blobs and export bounded exact-byte source excerpts."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo',type=Path,required=True,help='Git checkout containing the fixed recovered tag and implementation commit')
REPO=parser.parse_args().repo.resolve()
BEFORE='9651df809d8c2c034717bd3b036c35fda1c90fcb'
AFTER='edd29b6780713dc78f6a99d67c4c4d192e9c52b5'
ORIGIN='241160314087e5ae698c6e76c3c055a11c3ad422'
def sha(b):return hashlib.sha256(b).hexdigest()
def blob(commit,path):return subprocess.check_output(['git','-C',str(REPO),'show',f'{commit}:{path}'])
def function(text,token):
    lines=text.splitlines(keepends=True)
    start=next(i for i,line in enumerate(lines) if token in line)
    depth=0;opened=False
    for end in range(start,len(lines)):
        code=re.sub(r'//.*','',lines[end]);code=re.sub(r'"(?:\\.|[^"\\])*"','""',code)
        depth+=code.count('{')-code.count('}')
        opened |= '{' in code
        if opened and depth==0:return start+1,end+1
    raise ValueError(token)
def select(commit,path,token,trim=None):
    raw=blob(commit,path);text=raw.decode();start,end=function(text,token)
    if trim: start,end=trim(text,start,end)
    excerpt=''.join(text.splitlines(keepends=True)[start-1:end]).encode()
    return {'commit':commit,'path':path,'function':token,'start':start,'end':end,
            'source_sha256':sha(raw),'excerpt_sha256':sha(excerpt),'text':excerpt.decode()}
old='src/pymatching/sparse_blossom/structured/matcher/'
new='src/beyond_blossom/structured/matcher/'
generic_old='src/pymatching/sparse_blossom/driver/mwpm_decoding.cc'
generic_new='src/beyond_blossom/driver/mwpm_decoding.cc'
pre='src/beyond_blossom/unstructured/predecode/one_hop_predecoder.'
specs=[
 ('structured_dispatch',old+'mwpm.cc',new+'mwpm.inl','void Mwpm::process_event(',None),
 ('structured_boundary',old+'mwpm.cc',new+'mwpm.inl','void Mwpm::handle_tree_hitting_boundary(',None),
 ('structured_shatter',old+'mwpm.cc',new+'mwpm.inl','void Mwpm::shatter_descendants_into_matches_and_freeze(',None),
 ('structured_root',old+'alternating_tree.cc',new+'alternating_tree.inl','void AltTreeNode::become_root(',None),
 ('generic_grow',None,pre+'cc','void OneHopPredecoder::grow(',None),
 ('generic_match',None,pre+'cc','void OneHopPredecoder::run_one(',None),
 ('density_gate',None,pre+'cc','bool OneHopPredecoder::should_skip_sparse_high_degree(',None),
 ('seed_index',None,pre+'h','size_t seed_index(',None),
]
items={}
for name,before,after,token,trim in specs:
    items[name]={'before':select(BEFORE,before,token,trim) if before else None,'after':select(AFTER,after,token,trim)}
# Source-visible dispatch and handoff excerpts; no implication that these fragments are whole functions.
def around(text,start,end,first,last):
    lines=text.splitlines();a=next(i for i in range(start-1,end) if first in lines[i]);b=next(i for i in range(a,end) if last in lines[i]);return a+1,b+1
items['generic_dispatch']={
 'before':select(BEFORE,generic_old,'void process_timeline_until_completion(',lambda t,s,e:around(t,s,e,'if (mwpm.flooder.negative_weight_detection_events.empty())','mwpm.create_detection_event(&mwpm.flooder.graph.nodes[detection]);')),
 'after':select(AFTER,generic_new,'void process_timeline_until_completion(',lambda t,s,e:around(t,s,e,'if (mwpm.predecode_enabled)','} else if (mwpm.flooder.negative_weight_detection_events.empty())'))}
items['generic_import']={'before':None,'after':select(AFTER,'src/beyond_blossom/unstructured/matcher/mwpm.cc','void Mwpm::import_predecode_state()')}
items['generic_completion']={'before':None,'after':select(AFTER,pre+'cc','void OneHopPredecoder::run()',lambda t,s,e:around(t,s,e,'complete = std::all_of','result_pending = complete;'))}
for name,entry in items.items():
    for side,item in entry.items():
        if item is None:continue
        target=ROOT/'sources'/side/(name+'.cc');target.parent.mkdir(parents=True,exist_ok=True)
        target.write_text(item['text']);item['export_path']=str(target.relative_to(ROOT))
        # Full raw files are downloadable evidence, not rendered code blocks.
        rawpath=ROOT/'sources'/'files'/side/item['path'];rawpath.parent.mkdir(parents=True,exist_ok=True)
        rawpath.write_bytes(blob(item['commit'],item['path']));item['full_source']=str(rawpath.relative_to(ROOT))
        item['fragment']=name in ('generic_dispatch','generic_completion')
manifest={'schema':1,'origin_commit':ORIGIN,'recovered_tag':'source-2411603','recovered_commit':BEFORE,'candidate_commit':AFTER,
 'scope':'Exact function bodies and explicitly marked dispatch/completion fragments for three mechanisms; all other repository files excluded from the compact diff.',
 'original_identity':'The local source-2411603 tag preserves source files recovered from original commit 2411603; its local commit SHA differs from that original repository commit.',
 'excluded':['Unrelated architecture/namespace-only changes','Tests, packaging, AE wrappers','Large binaries and input corpora','No full-repository performance attribution'],
 'extractor_sha256':sha(Path(__file__).read_bytes()),'excerpts':items}
(ROOT/'sources'/'manifest.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n')
print('Extracted',len(items),'exact source scopes; original and recovered identities retained')
