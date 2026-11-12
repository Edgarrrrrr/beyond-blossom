/* Executable teaching model of the frozen physical-mode-3 branch.
 * It does not call or time the native decoder. The small exhaustive oracle
 * verifies example corrections independently of the radius/matching model. */
'use strict';
const Mode3 = (() => {
  const positions = {A:[0,0], B:[1,0], C:[0,1], D:[1,1]};
  const offsets = [[0,0,1],[0,0,-1],[1,0,0],[-1,0,0],[0,1,0],[0,-1,0],[-1,0,1],[1,0,-1],[0,-1,1],[0,1,-1],[-1,1,1],[1,-1,-1]];
  const scenarios = {
    partial: {title:'一对 tight，一对留给 MWPM', nodes:['A','B','C','D'], active:['A','B','C','D'], edges:[['A','B',4],['A','C',12],['B','D',12],['C','D',6]]},
    complete: {title:'全部 tight，完整证书', nodes:['A','B'], active:['A','B'], edges:[['A','B',4]]},
    boundary: {title:'部分边界匹配可以撤销', nodes:['A','B'], active:['A','B'], edges:[['A','B',8],['A',null,2],['B',null,20]]},
    inactive: {title:'非活动邻点限制半径', nodes:['A','B','C'], active:['A','B'], edges:[['A','B',8],['A','C',2],['B',null,20],['C',null,20]]}
  };
  const clone = x => JSON.parse(JSON.stringify(x));
  function oracle(s) {
    let weight = Infinity, solutions = [];
    for (let bits=0; bits<(1<<s.edges.length); bits++) {
      const parity = new Set(), chosen = []; let sum=0, obs=0;
      s.edges.forEach(([a,b,w],k) => {if(bits&(1<<k)) {chosen.push(k);sum+=w;obs^=1<<k;
        for(const id of [a,b]) if(id!==null) parity.has(id)?parity.delete(id):parity.add(id);}});
      if(parity.size!==s.active.length || s.active.some(id=>!parity.has(id))) continue;
      if(sum<weight) {weight=sum;solutions=[];}
      if(sum===weight) solutions.push({edges:chosen,obs});
    }
    return {weight,solutions};
  }
  function execute(key, reversed=false) {
    const s=clone(scenarios[key]); if(!s) throw Error('Unknown scenario');
    const storage=(reversed?s.active.slice().reverse():s.active).map((id,index)=>({id,index,cell:31+positions[id][0]+5*positions[id][1],radius:0,published:false,mate:null,match_slot:null,cap:null,mask:null}));
    const byId=Object.fromEntries(storage.map(x=>[x.id,x]));
    const rows=Object.fromEntries(s.nodes.map(id=>[id,Array(12).fill(null)]));
    s.edges.forEach(([a,b,w],edge)=>{
      if(b!==null) {
        const dx=positions[b][0]-positions[a][0],dy=positions[b][1]-positions[a][1];
        const slot=offsets.findIndex(v=>v[0]===dx&&v[1]===dy&&v[2]===0);
        if(slot<0 || rows[a][slot] || rows[b][slot^1]) throw Error('Invalid stencil input');
        rows[a][slot]={other:b,w,edge};rows[b][slot^1]={other:a,w,edge};
      }
    });
    s.edges.forEach(([a,b,w],edge)=>{if(b===null) {
      const slot=[2,3,4,5,0,1,6,7,8,9,10,11].find(k=>!rows[a][k]);
      rows[a][slot]={other:null,w,edge};
    }});
    const present=id=>rows[id].reduce((m,e,k)=>e?m|(1<<k):m,0);
    const boundary=id=>rows[id].reduce((m,e,k)=>e&&e.other===null?m|(1<<k):m,0);
    let partial={weight:0,obs:0,edges:[]}, ordered=[], local={}, frames=[];
    function emit(line,title,detail,extra={}) {
      frames.push({line,title,detail,storage:clone(storage),partial:clone(partial),ordered:ordered.slice(),local:clone(local),...extra});
    }
    function dual() {
      return s.edges.every(([a,b,w])=>(byId[a]?.radius||0)+(b===null?0:(byId[b]?.radius||0))<=w);
    }
    emit('core:693','进入 run_fused_range','mode3 是编译期分支。调用方已完成负权归一化、active 表和连续 scratch；本例权重均非负且为偶数。');
    emit('core:709','调用坐标顺序准备','begin=0，end='+storage.length+'；degrees 指针为空，mode3 不执行 degree 排序。');
    emit('core:511','验证输入范围','所有活动 seed 位于一个连续区间；storage 的身份保持不变。');
    for(const seed of storage) {
      let raw=0; const accesses=[];
      for(let slot=0;slot<12;slot++) {
        const [dx,dy,dt]=offsets[slot],p=positions[seed.id];
        const neighbor=dt===0?s.nodes.find(id=>positions[id][0]===p[0]+dx&&positions[id][1]===p[1]+dy):undefined;
        const value=neighbor&&byId[neighbor]?byId[neighbor].index+1:0;
        if(value) raw|=1<<slot;
        accesses.push({slot,offset:dx+5*dy+25*dt,active:value});
      }
      local={seed:seed.id,accesses,raw_mask:raw,present:present(seed.id)};
      emit('kernel:25','读取十二个 halo-safe 位置','先读取固定方向的 active 编号，再用 present 屏蔽无边方向；非活动节点与边界的 active 均为 0。');
      seed.mask=raw&present(seed.id);
      emit('core:519','保存 '+seed.id+' 的候选 mask','mask='+seed.mask+'；它只表示实际存在的活动邻点，不包含 boundary。');
      ordered.push(seed.index);
      emit('core:520','写入原 seed 索引','order_by_seed 保存 '+seed.index+'；排序只移动索引，不移动 seed。');
    }
    const alreadySorted=ordered.every((index,k)=>k===0||storage[ordered[k-1]].cell<=storage[index].cell);
    if(!alreadySorted) ordered.sort((a,b)=>storage[a].cell-storage[b].cell);
    local={already_sorted:alreadySorted,sort_calls:alreadySorted?0:1};
    emit('core:533','检查 cell 坐标顺序',(alreadySorted?'is_sorted=true，跳过 std::sort。':'is_sorted=false，调用 std::sort；只展示库调用的结果，不模拟其比较次数。')+' 当前顺序 '+ordered.map(i=>storage[i].id).join(' → ')+'。');
    emit('core:534','返回 PreparedOrder','返回索引 span 与 masks_by_seed，调用方开始第一轮半径发布。');
    for(const index of ordered) {
      const seed=storage[index];let bits=present(seed.id),cap=Infinity;
      local={seed:seed.id,cap:'UINT32_MAX',present:bits,active:seed.mask&~boundary(seed.id)};
      emit('core:713','初始化 '+seed.id+' 的 cap','无有限边时使用 UINT32_MAX 哨兵；此时 seed 半径仍为 0。');
      while(bits) {
        const slot=Math.log2(bits&-bits),e=rows[seed.id][slot];bits&=bits-1;
        const active=Boolean(local.active&(1<<slot)),limit=active?Math.floor(e.w/2):e.w;
        local={...local,slot,edge:e.edge,weight:e.w,remaining_present:bits,limit,active_edge:active};
        emit('core:719','读取 slot '+slot+' 的边权 '+e.w,'countr_zero 取最低置位，present &= present−1 只移除这条边。');
        cap=Math.min(cap,limit);seed.cap=cap;local.cap=cap;
        emit('core:720','约束合并：cap = '+cap,(active?'活动—活动端点各用 floor(w/2)':'非活动邻点或 boundary 使用完整 w')+'，与之前 cap 取最小值。');
      }
      seed.radius=cap===Infinity?0:2*Math.floor(cap/2);
      local={...local,radius:seed.radius};
      if(!dual()) throw Error('Dual infeasible during publication');
      emit('core:722','偶数向下对齐：r('+seed.id+') = '+seed.radius,'清掉 cap 的最低位；例如 w=6 的半权为 3，半径取 2，不能提前接受该边。');
      seed.published=true;
      emit('core:723','发布 '+seed.id+' 的半径','将已计算的 seed.radius 写入 grid.predecode.radii[cell]；这一轮不会建立匹配。');
    }
    local={};emit('core:725','全部半径已发布，开始 tight matching','此后不再 grow，不追赶邻点，不因邻点冻结而重新增加半径。');
    function pair(seed,other,slot,e) {
      local={seed:seed.id,other:other===null?'boundary':other.id,slot,edge:e.edge};
      seed.mate=other===null?'boundary':other.index;seed.match_slot=slot;
      emit('core:347','pair：写 source 的 mate','同时保存 source.match_slot='+slot+'。');
      if(other!==null) {
        other.mate=seed.index;other.match_slot=slot^1;
        emit('core:350','pair：反向端点写回','partner.mate='+seed.index+'；反向 slot = '+slot+' xor 1 = '+(slot^1)+'。');
      }
      partial.weight+=e.w;partial.obs^=1<<e.edge;partial.edges.push(e.edge);
      emit('core:354','累计局部结果','weight 加 '+e.w+'；observable mask XOR '+(1<<e.edge)+'。这是 partial.result，不保证已经完成整个 syndrome。');
      emit('core:356','pair 返回 void','返回 tight-match 循环；本次 source 的候选扫描停止。');
    }
    for(const index of ordered) {
      const seed=storage[index];local={seed:seed.id};
      emit('core:727','检查 '+seed.id+' 是否已配对',seed.mate!==null?'之前的 pair 已写入 mate；跳过此 source。':'仍未匹配，按 slot 顺序扫描活动候选。');
      if(seed.mate!==null) continue;
      let bits=seed.mask;
      while(bits) {
        const slot=Math.log2(bits&-bits),e=rows[seed.id][slot];bits&=bits-1;
        const other=byId[e.other];local={seed:seed.id,other:other.id,slot,edge:e.edge,other_active_id:other.index+1,remaining_candidates:bits};
        emit('core:734','由 active[cell] 找到邻 seed','读到 '+(other.index+1)+'，减 1 得 storage 索引 '+other.index+'。');
        const unmatched=other.mate===null, sum=unmatched?seed.radius+other.radius:null;
        local={...local,sum,weight:e.w};
        emit('core:735','检查未匹配且 tight',!unmatched?'邻点已匹配；&& 短路，不再求半径和。':'r_u+r_v='+sum+'，w='+e.w+'；'+(sum===e.w?'满足相等条件。':'不相等，保持未匹配。'));
        if(unmatched&&sum===e.w) {pair(seed,other,slot,e);emit('core:738','停止当前候选循环','只为此 source 接受一条 tight 边。');break;}
      }
      emit('core:741','是否继续检查 boundary',seed.mate!==null?'source 已匹配；跳过 boundary。':'没有活动配对，继续边界扫描。');
      if(seed.mate!==null) continue;
      bits=boundary(seed.id);
      if(!bits) emit('core:743','boundary mask = 0','没有边界边；该 seed 保留 unmatched 状态。');
      while(bits) {
        const slot=Math.log2(bits&-bits),e=rows[seed.id][slot];bits&=bits-1;
        local={seed:seed.id,slot,edge:e.edge,radius:seed.radius,weight:e.w};
        emit('core:746','检查边界 tight 条件','boundary 半径为 0；比较 '+seed.radius+' == '+e.w+'。');
        if(seed.radius===e.w) {pair(seed,null,slot,e);emit('core:748','停止边界循环','当前 source 已匹配 boundary。');break;}
      }
    }
    const complete=storage.every(x=>x.mate!==null), truth=oracle(s);
    if(!dual()) throw Error('Final dual infeasible');
    if(complete && (partial.weight!==truth.weight || !truth.solutions.some(x=>x.obs===partial.obs))) throw Error('Complete certificate mismatch');
    local={};emit('core:800','run_fused_range 返回 void','输出是全部 seed 的半径/mate 与 partial.result。随后调用方 merge/finish 判断 complete；partial 不能仅凭已匹配边数直接返回。', {finished:true,complete,oracle:truth,dual_feasible:true});
    return {scenario:s,rows,frames,complete,oracle:truth,storage:clone(storage),partial:clone(partial)};
  }
  return {scenarios,execute,oracle};
})();
if(typeof module!=='undefined') module.exports=Mode3;
