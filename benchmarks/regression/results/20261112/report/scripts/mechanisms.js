'use strict';
const DATA=window.MECHANISM_DATA;
const copy=x=>JSON.parse(JSON.stringify(x));
function primal(scene){
 const pair=scene==='child',noop=scene==='none';
 const s={event:noop?'NO_EVENT':'REGION_HIT_BOUNDARY',num_active_trees:1,regions:{A:{tree:'root',match:null,frozen:false}},arena:['root'],returned:false};
 if(pair){s.regions.B={tree:'child',match:null,frozen:false};s.regions.C={tree:'child',match:null,frozen:false};s.arena.push('child');}
 let frames=[];
 const add=(code,line,message,occurrence=0)=>frames.push({code,line,message,occurrence,state:copy(s),kind:'primal'});
 add('structured_dispatch','inline void Mwpm::process_event','入口：一个事件与已有交替树状态；profile 在本构建中关闭。');
 add('structured_dispatch','switch (event.event_type)','读取事件类型。');
 if(noop){add('structured_dispatch','case NO_EVENT','无事件，保持树与区域不变。');s.returned=true;add('structured_dispatch','record_primal_tree_snapshot();','函数返回 void；没有状态更新。',1);return frames;}
 add('structured_dispatch','handle_tree_hitting_boundary(event','分派到边界增广处理函数。');
 add('structured_boundary','auto node =','从区域 A 找到根节点 root。');
 add('structured_root','if (!parent.alt_tree_node)','root 的父节点为空，become_root 立即返回。');
 add('structured_boundary','shatter_descendants','调用递归函数，处理已有交替树。');
 const shatter=(node,inner,outer)=>{
   add('structured_shatter','for (auto &child_edge',`进入 ${node}，检查子边。`);
   if(node==='root'&&pair){add('structured_shatter','shatter_descendants', '沿 root→child 子边递归。');shatter('child','B','C');}
   add('structured_shatter','if (alt_tree_node.inner_region)',inner?`${node} 有 inner/outer 区域对 ${inner}/${outer}。`:`${node} 仅有 outer 区域。`);
   if(inner){s.regions[inner].match=outer;s.regions[outer].match=inner;add('structured_shatter','inner()->add_match',`恢复 ${inner}↔${outer} 匹配。`);
     s.regions[inner].frozen=true;add('structured_shatter','set_region_frozen(*alt_tree_node.inner','冻结 inner 区域；dual 辅助函数内部队列操作作为调用边界。');
     s.regions[outer].frozen=true;add('structured_shatter','set_region_frozen(*alt_tree_node.outer','冻结 outer 区域。');
     s.regions[inner].tree=null;add('structured_shatter','inner()->alt_tree_node = nullptr','清除 inner 的树指针。');
     s.regions[outer].tree=null;add('structured_shatter','outer()->alt_tree_node = nullptr','清除 outer 的树指针。');}
   s.regions[outer].tree=null;add('structured_shatter','if (alt_tree_node.outer_region)','解除剩余 outer 区域的树关联。');
   s.arena=s.arena.filter(x=>x!==node);add('structured_shatter','node_arena.del',`${node} 回收到 arena；区域对象和匹配保留。`);
 };
 shatter('root',null,'A');s.regions.A.match='边界';add('structured_boundary','match = Match','区域 A 与边界匹配。');
 s.regions.A.frozen=true;add('structured_boundary','set_region_frozen','冻结 A，保留边界匹配。');
 if(!(s.num_active_trees>0))throw Error('Invalid tree count');s.num_active_trees--;add('structured_boundary','--num_active_trees','活动树计数从1变为0。');
 s.returned=true;s.verified=s.arena.length===0&&Object.values(s.regions).every(r=>r.match!==null&&r.frozen&&r.tree===null);
 add('structured_dispatch','record_primal_tree_snapshot();','返回 void；独立状态检查：所有区域已匹配且冻结，树节点全部回收。',1);return frames;
}
function generic(scene){
 const boundary=scene==='boundary',tight=scene!=='untight';
 const seeds=boundary?[{id:'A',radius:0,cap:6,mate:null,saturated:false}]:[{id:'A',radius:0,cap:tight?8:4,mate:null,saturated:false},{id:'B',radius:0,cap:tight?8:4,mate:null,saturated:false}];
 if(scene==='partial')seeds.push({id:'C',radius:0,cap:4,mate:null,saturated:false});
 const edgeWeight=boundary?6:tight?8:10;
 const edges=boundary?[['A','边界',6]]:[['A','B',edgeWeight]];
 if(scene==='untight')edges.push(['A','X',4],['B','Y',4]);
 if(scene==='partial')edges.push(['C','X',4],['X','边界',4]);
 const s={seeds,edges,weight:0,obs_mask:0,returned:false,outer_state:'尚未检查'};let frames=[];
 const add=(code,line,message)=>frames.push({code,line,message,state:copy(s),kind:'generic'});
 const grow=i=>{const a=seeds[i];add('generic_grow','if (seed.saturated)',`grow(${a.id})：检查是否已经增长。`);if(a.saturated)return;
  let limit=a.cap;add('generic_grow','weight_int limit',`初始上界来自最小邻边权重：${limit}。`);
  if(!boundary&&i<2){const b=seeds[1-i];limit=Math.min(limit,edgeWeight-b.radius);s.constraint=`${a.id}: min(cap, ${edgeWeight}−${b.radius}) = ${limit}`;add('generic_grow','limit = std::min','合并另一端当前半径约束，保持 y_A+y_B≤w。');}
  a.radius=limit&~1;add('generic_grow','seed.radius = limit',`向下取偶数半径 ${a.radius}。`);a.saturated=true;add('generic_grow','seed.saturated = true','记录本 seed 已完成增长。');
 };
 add('generic_match','void OneHopPredecoder::run_one','入口：演示 run_one(0)；prepare 和候选表构建已完成。');grow(0);
 if(!boundary){add('generic_match','const auto& candidate','读取唯一活动候选 B；其 mate 尚为空。');
  if(seeds[0].radius+seeds[1].radius<edgeWeight){add('generic_match','grow(candidate.seed)','两端半径之和不足边权，尝试增长 B。');grow(1);}
  const sum=seeds[0].radius+seeds[1].radius;s.tight=sum===edgeWeight;add('generic_match','if (uint64_t(seed.radius) + mate.radius !=',`比较 ${sum} 与边权 ${edgeWeight}，tight=${s.tight}。`);
  if(s.tight){seeds[0].mate='B';add('generic_match','seed.mate = candidate.seed','A 的配对索引指向 B。');seeds[1].mate='A';add('generic_match','mate.mate = index','B 的配对索引指回 A。');seeds[0].match_observables=seeds[1].match_observables=1;add('generic_match','seed.match_observables = mate.match_observables','两端保存同一条边的 observable 摘要。');s.obs_mask^=1;add('generic_match','obs_mask ^= node.neighbor_observables[slot]','把边 observable 异或到输出。');s.weight+=edgeWeight;add('generic_match','weight += node.neighbor_weights[slot]','把 tight 边权累加一次。');s.returned=true;add('generic_match','return;','run_one 返回，当前局部配对建立。');}
  else{add('generic_match','continue;','边不 tight，跳过；候选段结束。');s.returned=true;add('generic_match','if (!node.neighbors.empty() && !node.neighbors[0]','没有直接边界边，函数结束并保留未匹配状态。');}
 }else{seeds[0].mate='边界';add('generic_match','seed.mate = BOUNDARY','半径等于边界权重，写入边界匹配。');seeds[0].match_observables=2;add('generic_match','seed.match_observables = node.neighbor_observables[0]','保存边界边 observable 摘要。');s.obs_mask^=2;add('generic_match','obs_mask ^= node.neighbor_observables[0]','边界边 observable 异或到结果。');s.weight+=6;s.returned=true;add('generic_match','weight += node.neighbor_weights[0]','累计边界权重，run_one 返回。');}
 if(scene==='untight'){s.returned=false;add('generic_match','void OneHopPredecoder::run_one','外层随后调用 run_one(B)；B和A都已 saturated。');grow(1);add('generic_match','grow(candidate.seed)','两端半径之和仍小于10，再调用 grow(A)，立即返回。');grow(0);s.returned=true;add('generic_match','continue;','边仍不 tight；B也保持未匹配，外层遍历完成。');}
 if(scene==='partial'){s.returned=false;add('generic_match','void OneHopPredecoder::run_one','外层跳过已配对的 B，随后调用 run_one(C)；C 通过非活动 X 的两条边连接边界。');grow(2);s.returned=true;add('generic_match','if (!node.neighbors.empty() && !node.neighbors[0]','C 的候选段为空且没有直接边界边；半径4合法，C保持未匹配。');}
 s.complete=seeds.every(a=>a.mate!==null);s.dual_lower_bound=seeds.reduce((a,b)=>a+b.radius,0);
 s.outer_state=s.complete?'全部 tight 匹配，可直接提取结果':'有未匹配 seed，导入完整求解器';
 add('generic_completion','complete = std::all_of',`外层完成检查：${s.outer_state}。`);
 if(s.complete){s.verified=s.weight===s.dual_lower_bound;add('generic_completion','result_pending = complete','完整匹配权重等于 dual 下界；结果只允许消费一次。');}
 else{s.imported=seeds.map(a=>({id:a.id,radius:a.radius,growth:a.mate===null?1:0,mate:a.mate}));add('generic_import','for (const auto& seed : predecoder.seeds) {','第一遍发布全部 singleton；暂配 A/B 仍存在，不能被贪心删去。');s.outer_solver='下一步：完整 blossom，可撤销暂配；本页未模拟其内部求解';add('generic_import','if (seed.mate == OneHopPredecoder::UNMATCHED)','第二遍为未匹配 seed 建立树根，其余写暂配摘要；结束在完整求解器调用边界。');}
 return frames;
}
function gate(scene){
 let n=16,degree=64,direct=false;if(scene==='empty')n=0;if(scene==='low')degree=12;if(scene==='boundary')direct=true;if(scene==='dense')n=64;if(scene==='remainder')n=17;if(scene==='threshold'||scene==='mixed')n=8;
 const active=new Set(Array.from({length:n},(_,i)=>i));
 const neighbors=Array.from({length:n},(_,seed)=>Array.from({length:scene==='mixed'&&seed===0?16:degree},(_,j)=>scene==='dense'?(seed+j+1)%65:scene==='threshold'?(j<8&&j%2===0?(seed+[1,2,6,7][j/2])%n:128+j):128+j));
 if(direct&&n)neighbors[0][0]=null;
 const s={seed_count:n,degree:scene==='mixed'?'16–64':degree,seed_position:0,seed_error:0,sampled:0,active_neighbors:0,sample_positions:[],probed:[],returned:false};let frames=[];
 const add=(line,message)=>frames.push({code:'density_gate',line,message,state:copy(s),kind:'gate'});
 const finish=(value,line,message)=>{s.returned=true;s.skip=value;s.output=value?'回退：规范化 seed → 普通 roots → 完整求解器':'保留：候选扫描 → tight 预解码';add(line,message);return frames;};
 add('bool OneHopPredecoder::should_skip','入口：active 表已由本 shot 的 prepare 建立；图和数组均为演示输入。');
 if(!n)return finish(false,'if (seeds.empty())','空 syndrome 立即返回 false。');
 s.degree_sum=0;for(let i=0;i<n;i++){if(neighbors[i][0]===null)return finish(false,'if (!neighbors.empty() && !neighbors[0])','发现直接边界边，保留预解码机会。');s.degree_sum+=neighbors[i].length;}
 add('degree_sum += neighbors.size()','只累加活动 seed 的度数，没有扫描全图。');
 s.average_degree=Math.floor(s.degree_sum/n);if(s.average_degree<32)return finish(false,'if (degree_sum / seeds.size() < 32)','活动节点平均度小于32，立即返回 false。');
 s.seed_samples=Math.min(8,Math.floor((n-1)/4)+1);s.seed_step=Math.floor(n/s.seed_samples);s.seed_remainder=n%s.seed_samples;add('const size_t seed_samples','计算节点采样预算，以及循环外的商和余数。');
 for(let k=0;k<s.seed_samples;k++){
   s.sample=k;s.sample_positions.push(s.seed_position);const list=neighbors[s.seed_position];s.probed=[];
   add('const auto& neighbors = seeds[seed_position]','读取本次均匀抽中的邻接表。');
   for(let j=0;j<Math.min(32,list.length);j++){
    const slot=list.length>=32?Math.floor(j*list.length/32):j;const id=list[slot];const hit=id!==null&&active.has(id);s.active_neighbors+=hit?1:0;s.probed.push({slot,id,active:hit});s.current_slot=slot;s.current_neighbor=id;
    add(list.length>=32?'active_neighbors += seed_index(neighbors[':'active_neighbors += seed_index(neighbor)','计算槽号、内联读取 active 索引，再累计命中；当前 sampled 分母在这一邻域结束时更新。');
   }
   s.sampled+=Math.min(32,list.length);add(list.length>=32?'sampled += 32':'sampled += neighbors.size()','把实际读取的条目数加入 sampled。');
   s.seed_position+=s.seed_step;s.seed_error+=s.seed_remainder;if(s.seed_error>=s.seed_samples){s.seed_position++;s.seed_error-=s.seed_samples;}
   if(s.seed_position!==Math.floor((k+1)*n/s.seed_samples))throw Error('Recurrence mismatch');
   add('seed_position += seed_step','商余数递推产生下一均匀 seed 位置；用 floor 公式独立核对。');
 }
 s.active_fraction=s.active_neighbors/s.sampled;s.recurrence_verified=true;
 return finish(s.sampled>0&&s.active_neighbors*8<s.sampled,'return sampled &&',`局部活动比例 ${s.active_neighbors}/${s.sampled}，按严格小于1/8决定回退。`);
}
window.models={primal,generic,gate};
const SELECT=document.querySelector('#scene'),SOURCE=document.querySelector('#source'),SLIDER=document.querySelector('#progress');
let frames=[],index=0,timer=null,codeSize=15;
function svg(tag,attrs={},text=''){const e=document.createElementNS('http://www.w3.org/2000/svg',tag);for(const [k,v] of Object.entries(attrs))e.setAttribute(k,v);if(text)e.textContent=text;return e;}
function graph(frame){const root=document.querySelector('#graph');root.replaceChildren();root.setAttribute('viewBox','0 0 600 250');const s=frame.state;
 const title=(t,y=25)=>root.append(svg('text',{x:20,y,fill:'#476079','font-size':14},t));
 if(frame.kind==='gate'){
  title(`活动 seed ${s.seed_count} · 邻接度 ${s.degree} · 探测 ${s.sampled} 条`);
  const nodes=s.probed||[];for(let i=0;i<32;i++){const x=28+(i%8)*73,y=66+Math.floor(i/8)*43,entry=nodes[i];root.append(svg('rect',{x:x-18,y:y-16,width:58,height:34,rx:6,fill:entry?(entry.active?'#16847b':'#dee7ee'):'#f3f6f9',stroke:'#a4b9c9'}));root.append(svg('text',{x:x+10,y:y+5,'text-anchor':'middle','font-size':12,fill:entry&&entry.active?'white':'#344f65'},entry?String(entry.id):'待读'));}
  title(s.output||`当前节点 ${s.seed_position}；已读槽位按顺序出现`,246);
 }else{
  const names=frame.kind==='primal'?Object.keys(s.regions):[...new Set([...s.seeds.map(x=>x.id),...s.edges.flatMap(e=>e.slice(0,2))])];
  const pos=Object.fromEntries(names.map((n,i)=>[n,{x:60+i*(480/Math.max(1,names.length-1)),y:105}]));
  let edges=frame.kind==='generic'?s.edges:[];if(frame.kind==='primal'){for(const [name,r]of Object.entries(s.regions)){if(r.match&&r.match!=='边界'&&name<r.match)edges.push([name,r.match,'已匹配']);}if(names.includes('B')&&!s.regions.B.match)edges.push(['A','B','树子边']);}
  for(const [a,b,w]of edges){if(!pos[a]||!pos[b])continue;root.append(svg('line',{x1:pos[a].x,y1:105,x2:pos[b].x,y2:105,stroke:'#7f97ab','stroke-width':3}));root.append(svg('text',{x:(pos[a].x+pos[b].x)/2,y:81,'text-anchor':'middle','font-size':13},String(w)));}
  for(const name of names){const p=pos[name],item=frame.kind==='primal'?s.regions[name]:s.seeds.find(x=>x.id===name),done=item&&(item.frozen||item.mate!==null&&item.mate!==undefined);root.append(svg('circle',{cx:p.x,cy:p.y,r:29,fill:done?'#16847b':'#dce8f4',stroke:'#315e85','stroke-width':2}));root.append(svg('text',{x:p.x,y:p.y+5,'text-anchor':'middle','font-size':17,fill:done?'white':'#183952'},name));const label=frame.kind==='primal'?`tree=${item?.tree??'∅'} · match=${item?.match??'∅'}`:item?`y=${item.radius} · mate=${item.mate??'∅'}`:name==='边界'?'边界':'非活动节点 y=0';root.append(svg('text',{x:p.x,y:160,'text-anchor':'middle','font-size':12},label));}
  title(frame.kind==='primal'?`活动树 ${s.num_active_trees} · arena活节点 ${s.arena.join(', ')||'∅'}`:`已配权重 ${s.weight} · observable ${s.obs_mask} · ${s.outer_state}`,216);
 }
}
function code(name,highlight,occurrence=0){const rows=DATA.sources[name]?.after;if(!rows)return;SOURCE.value=name;const pane=document.querySelector('#code');pane.replaceChildren();let hits=0;for(const r of rows){const div=document.createElement('div');div.className='code-row'+(highlight&&r.source.includes(highlight)&&hits++===occurrence?' current':'');div.id=`line-${name}-${r.line}`;const n=document.createElement('span');n.className='number';n.textContent=r.line;const b=document.createElement('div');const c=document.createElement('code');c.textContent=r.source;const e=document.createElement('p');e.textContent=r.explanation;b.append(c,e);div.append(n,b);pane.append(div);}const src=DATA.origin.excerpts[name].after;document.querySelector('#source-path').textContent=`${src.path}:${src.start}–${src.end}`;requestAnimationFrame(()=>{const row=pane.querySelector('.current');if(!row)return;const top=row.getBoundingClientRect().top-pane.getBoundingClientRect().top+pane.scrollTop;const bottom=top+row.offsetHeight;if(top<pane.scrollTop)pane.scrollTop=top;else if(bottom>pane.scrollTop+pane.clientHeight)pane.scrollTop=bottom-pane.clientHeight;});}
function render(){const f=frames[index];SLIDER.max=frames.length-1;SLIDER.value=index;document.querySelector('#position').textContent=`${index+1} / ${frames.length}`;document.querySelector('#message').textContent=f.message;document.querySelector('#state').textContent=JSON.stringify(f.state,null,2);graph(f);code(f.code,f.line,f.occurrence||0);document.querySelector('#prev').disabled=index===0;document.querySelector('#next').disabled=index===frames.length-1;}
function stop(){if(timer)clearInterval(timer);timer=null;document.querySelector('#play').textContent='播放';}
function choose(){stop();frames=window.models[DATA.model](SELECT.value);index=0;render();}
SELECT.addEventListener('change',choose);SOURCE.addEventListener('change',()=>code(SOURCE.value,''));
document.querySelector('#prev').onclick=()=>{stop();index=Math.max(0,index-1);render();};document.querySelector('#next').onclick=()=>{stop();index=Math.min(frames.length-1,index+1);render();};document.querySelector('#reset').onclick=()=>{stop();index=0;render();};SLIDER.oninput=()=>{stop();index=Number(SLIDER.value);render();};
document.querySelector('#play').onclick=()=>{if(timer)return stop();if(index===frames.length-1)index=0;document.querySelector('#play').textContent='暂停';timer=setInterval(()=>{index++;render();if(index===frames.length-1)stop();},500);};
for(const [id,delta]of [['smaller',-1],['larger',1]])document.querySelector('#'+id).onclick=()=>{codeSize=Math.max(12,Math.min(22,codeSize+delta));document.documentElement.style.setProperty('--code-size',codeSize+'px');};
choose();window.reportUI={get frames(){return frames},get index(){return index},select:choose,render};
