'use strict';
const fs=require('fs'),path=require('path'),vm=require('vm'),crypto=require('crypto');
const root=path.dirname(__dirname),file=path.join(__dirname,'mechanisms.js'),text=fs.readFileSync(file,'utf8');
const source=JSON.parse(fs.readFileSync(path.join(root,'sources/line-explanations.json')));
const context={window:{MECHANISM_DATA:{}},console};vm.createContext(context);vm.runInContext(text.slice(0,text.indexOf('const SELECT=')),context);
const models=context.window.models,records=[];
function need(v,m){if(!v)throw Error(m)}
for(const [model,scenes] of Object.entries({primal:['leaf','child','none'],generic:['pair','partial','untight','boundary'],gate:['sparse','remainder','mixed','dense','threshold','low','boundary','empty']})){
 for(const scene of scenes){const frames=models[model](scene);need(frames.length>1,scene+' empty trace');
  for(const frame of frames){const lines=source.excerpts[frame.code].after.filter(r=>r.source.includes(frame.line));need(lines.length>(frame.occurrence||0),scene+' unknown source target '+frame.line);
   if(model==='generic')for(const [u,v,w] of frame.state.edges){const radii=Object.fromEntries(frame.state.seeds.map(s=>[s.id,s.radius]));need((radii[u]||0)+(radii[v]||0)<=w,scene+' violates dual feasibility');}
  }
  const end=frames.at(-1).state;need(end.returned,scene+' no return');
  if(model==='primal')need(scene==='none'?end.num_active_trees===1:end.verified,scene+' incorrect tree final state');
  if(model==='generic'){
   if(['pair','boundary'].includes(scene))need(end.complete&&end.weight===end.dual_lower_bound&&end.verified,scene+' missing complete certificate');
   else need(!end.complete&&end.imported.length===end.seeds.length,scene+' partial state discarded');
   if(scene==='partial')need(end.seeds[2].radius===4&&end.seeds[2].mate===null,'C growth skipped');
  }
  if(model==='gate'){
   need(end.skip===['sparse','remainder','mixed'].includes(scene),scene+' bad gate result');
   if(scene==='threshold')need(end.active_neighbors*8===end.sampled,'strict threshold fixture wrong');
   if(scene==='remainder')need(JSON.stringify(end.sample_positions)==='[0,3,6,10,13]','nondivisible recurrence differs');
   if(scene==='mixed')need(end.sampled===48,'short-neighbor scan differs');
  }
  records.push({model,scene,frames:frames.length,source_targets:'verified',final_state:end});
 }
}
const receipt={complete:true,scope:'Light Node VM execution of semantic models; not browser/UI or native runtime verification',model_sha256:crypto.createHash('sha256').update(text).digest('hex'),scenarios:records.length,records};
fs.writeFileSync(path.join(root,'model-verification.json'),JSON.stringify(receipt,null,2)+'\n');console.log(`${records.length} semantic model scenarios passed; no browser/native timing executed`);
