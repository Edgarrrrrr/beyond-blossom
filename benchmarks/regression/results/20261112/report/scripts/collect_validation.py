#!/usr/bin/env python3
"""Copy small actual validation receipts; never substitute smoke for performance."""
from pathlib import Path
import hashlib,json,re,shutil,xml.etree.ElementTree as ET
R=Path(__file__).resolve().parents[1]
paths={
 'asan-final-validation.json':'/tmp/beyond-build/asan-final-validation.json',
 'release-final-ctest.log':'/tmp/beyond-build/final-release-ctest.log',
 'asan-final-ctest.log':'/tmp/beyond-build/asan-final-ctest.log',
 'package-audit.json':'/tmp/beyond-package-final/artifacts-r2/audit.json',
 'package-pytest.xml':'/tmp/beyond-package-final/artifacts-r2/pytest.xml',
 'ae-tests.log':'/tmp/beyond-ae-final-tests01.log',
 'native-build.json':'/tmp/beyond-ae-native-final01/build.json',
 'native-build-audit.json':'/tmp/beyond-ae-native-final01-audit.json',
 'native-smoke-plan.json':'/tmp/beyond-ae-native-smoke-final01/plan.json',
 'native-smoke-complete.json':'/tmp/beyond-ae-native-smoke-final01/complete.json',
 'external-smoke-plan.json':'/tmp/beyond-external-smoke-final03/plan.json',
 'external-smoke-complete.json':'/tmp/beyond-external-smoke-final03/complete.json',
 'external-smoke-validation.json':'/tmp/beyond-external-smoke-final03/d09-p0.003/n64/run/summary.json',
 'export-smoke-complete.json':'/tmp/beyond-ae-comparison-export-final01/complete.json',
 'generic-protocol.json':'/tmp/beyond-build/generic-final-protocol.json',
 'hardware.json':'/tmp/beyond-build/final-hardware.json',
 'generic-codegen.txt':'/tmp/beyond-build/predecode-inline-disassembly.txt',
}
refs=[];target=R/'evidence/validation';target.mkdir(parents=True,exist_ok=True)
for name,source in paths.items():
 src=Path(source)
 if not src.is_file():continue
 dst=target/name;shutil.copyfile(src,dst);refs.append({'path':str(dst.relative_to(R)),'sha256':hashlib.sha256(dst.read_bytes()).hexdigest(),'acquisition_path':str(src)})
package=json.loads((target/'package-audit.json').read_text());tests=ET.parse(target/'package-pytest.xml').getroot();suites=tests.findall('testsuite') if tests.tag!='testsuite' else [tests]
external=json.loads((target/'external-smoke-validation.json').read_text());native=json.loads((target/'native-smoke-complete.json').read_text());plan=json.loads((target/'native-smoke-plan.json').read_text())
log=(target/'ae-tests.log').read_text();match=re.search(r'(\d+) passed, (\d+) subtests passed',log)
ctest={}
for label in ('asan','release'):
 text=(target/(label+'-final-ctest.log')).read_text();matched=re.search(r'100% tests passed, 0 tests failed out of (\d+)',text)
 if not matched:raise ValueError('Missing successful final CTest result')
 ctest[label]={'passed':int(matched[1]),'log':f'evidence/validation/{label}-final-ctest.log'}
result={'ctest':ctest,'schema':1,'implementation_commit':package['commit'],'package':{'complete':package['complete'],'tests':sum(int(s.attrib['tests']) for s in suites),'failures':sum(int(s.attrib['failures']) for s in suites),'errors':sum(int(s.attrib['errors']) for s in suites),'wheel':package['wheel'],'sdist':package['sdist'],'installed_extension':package['installed_extension']},
 'asan':{'ctest_log':'evidence/validation/asan-final-ctest.log','profile':'ASan+UBSan with halt_on_error; detect_leaks=0 because ptrace environment does not support LSan','runtime_commit_reported_by_builder':'edd29b6780713dc78f6a99d67c4c4d192e9c52b5','final_source_manifest':'evidence/validation/asan-final-validation.json; 196 tracked source/config/license files match the runtime commit; older 354-test sanitizer manifest excluded'},
 'ae_tests':{'passed':int(match[1]),'subtests_passed':int(match[2])},
 'native_smoke':{'complete':native['complete'],'methods':len(plan['methods']),'shots_per_method':plan['shots'],'host':plan['host']},
 'external_smoke':{'complete':external['complete'],'methods':len(external['results']),'shots_per_method':external['results'][0]['shots'],'native_parity_checks':sum(r['native_parity_checks'] for r in external['results']),'independent_chain_checks':sum(r['independent_chain_checks'] for r in external['results']),'exact_methods':['parity','fusion'],'weight_gaps':{r['method']:r['weight_gaps'] for r in external['results']}},
 'codegen':{'kind':'release-object evidence','command':'objdump -Cd /tmp/beyond-build/candidate/CMakeFiles/libbeyond_blossom.dir/src/beyond_blossom/unstructured/predecode/one_hop_predecoder.cc.o','qualification':'Previously recorded final-core Release object disassembly, not a fresh dump of the final linked executable. Source equivalence attested by the build agent; no new objdump during timed runs.'},
 'references':refs,'large_artifacts':'Binaries and full input corpora are excluded from this compact report; workspace archive and retrieval receipts are supplied separately by the parent task.'}
(R/'validation.json').write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n');print(R/'validation.json')
