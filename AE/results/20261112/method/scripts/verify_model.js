'use strict';
const assert=require('node:assert/strict');
const Mode3=require('./model.js');
let cases=0,frames=0;
for(const key of Object.keys(Mode3.scenarios)) {
  let reference;
  for(const reversed of [false,true]) {
    const t=Mode3.execute(key,reversed);cases++;frames+=t.frames.length;
    assert.equal(t.frames[0].line,'core:693');assert.equal(t.frames.at(-1).line,'core:800');
    assert.equal(t.frames.find(f=>f.line==='core:533').local.already_sorted,!reversed);
    assert(t.frames.at(-1).finished);assert(t.frames.at(-1).dual_feasible);
    for(const f of t.frames) {
      for(const [a,b,w] of t.scenario.edges) {
        const radius=id=>f.storage.find(s=>s.id===id)?.radius||0;
        assert(radius(a)+(b===null?0:radius(b))<=w);
      }
      if(f.partial.edges.length) assert(f.storage.every(s=>s.published),'pair before all radii published');
      // The real pair helper writes source first, then partner. Its first write
      // is a visible intermediate frame, not yet a completed symmetric pair.
      if(f.line!=='core:347') f.storage.forEach(s=>{if(typeof s.mate==='number') assert.equal(f.storage[s.mate].mate,s.index);});
    }
    const result={complete:t.complete,partial:t.partial,oracle:t.oracle,radii:Object.fromEntries(t.storage.map(s=>[s.id,s.radius]))};
    if(reference) assert.deepEqual(result,reference);else reference=result;
    if(key==='partial') {assert.equal(t.partial.weight,4);assert.equal(t.oracle.weight,10);assert.equal(t.complete,false);assert.deepEqual(t.partial.edges,[0]);assert.deepEqual(t.oracle.solutions[0].edges,[0,3]);}
    if(key==='complete') {assert.equal(t.partial.weight,4);assert.equal(t.oracle.weight,4);assert.equal(t.complete,true);}
    if(key==='boundary') {assert.deepEqual(t.partial.edges,[1]);assert.deepEqual(t.oracle.solutions[0].edges,[0]);assert.equal(t.partial.weight,2);assert.equal(t.oracle.weight,8);assert.equal(t.complete,false);}
    if(key==='inactive') {assert.equal(t.storage.find(s=>s.id==='A').radius,2);assert.equal(t.storage.find(s=>s.id==='B').radius,4);assert.equal(t.partial.weight,0);assert.equal(t.oracle.weight,8);}
  }
}
console.log(JSON.stringify({passed:true,cases,frames,scope:'JavaScript teaching-model semantics only; zero native decoder or timing executions'}));
