'use strict';
(() => {
  const $=s=>document.querySelector(s);let trace,step=0,timer=null,font=15;
  const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const xy={A:[145,85],B:[395,85],C:[145,285],D:[395,285]};
  function displayCode(key,follow=true) {
    const block=SOURCE.blocks.find(x=>x.lines.some(l=>l.key===key))||SOURCE.blocks[0];
    $('#sourceSelect').value=block.id;
    $('#sourceName').textContent=block.title;
    $('#sourceLink').href=block.source;
    $('#sourceRows').innerHTML=block.lines.map(l=>l.gap?'<div class="gap">'+esc(l.gap)+'</div>':
      '<div class="source-row '+(l.key===key?'current':'')+'" data-source-key="'+l.key+'"><span class="lineno">'+l.line+'</span><div><code>'+esc(l.code)+'</code><p class="explain">'+esc(l.explanation)+'</p></div></div>').join('');
    $('#sourceRows').style.height=innerWidth>1050?Math.max(350,innerHeight-$('#sourceRows').getBoundingClientRect().top-20)+'px':'';
    if(follow) {const el=$('#sourceRows .current');if(el) $('#sourceRows').scrollTop=el.offsetTop-40;}
    else $('#sourceRows').scrollTop=0;
    if(new URLSearchParams(location.search).get('verify')==='1') {
      const row=$('#sourceRows .current'),pane=$('#sourceRows').getBoundingClientRect(),box=row.getBoundingClientRect();
      if(box.top<pane.top-1 || box.top>=pane.bottom || (innerWidth>1050 && box.top>=innerHeight)) throw Error('Current source line is outside the visible reading area');
      document.body.dataset.sourceVisibility='passed';
    }
  }
  function graph(frame) {
    const matched=new Set(frame.partial.edges);let svg='';
    trace.scenario.edges.forEach(([a,b,w],i)=>{
      const [x,y]=xy[a], end=b===null?[a==='A'?45:495,y+70]:xy[b],hot=frame.local.edge===i;
      const cls=matched.has(i)?'matched':hot?'hot':'edge';
      svg+='<line class="'+cls+(b===null?' boundary':'')+'" x1="'+x+'" y1="'+y+'" x2="'+end[0]+'" y2="'+end[1]+'"/>';
      const tx=(x+end[0])/2,ty=(y+end[1])/2;
      svg+='<rect class="weight-bg" x="'+(tx-25)+'" y="'+(ty-14)+'" width="50" height="28" rx="8"/><text class="weight" x="'+tx+'" y="'+(ty+5)+'">w='+w+'</text>';
      if(b===null) svg+='<rect class="boundary-node" x="'+(end[0]-13)+'" y="'+(end[1]-13)+'" width="26" height="26"/><text class="tiny" x="'+end[0]+'" y="'+(end[1]+35)+'">边界</text>';
    });
    trace.scenario.nodes.forEach(id=>{
      const state=frame.storage.find(s=>s.id===id),[x,y]=xy[id];
      const cls=!state?'inactive':state.mate!==null?'paired':frame.local.seed===id?'selected':'active';
      svg+='<circle class="node '+cls+'" cx="'+x+'" cy="'+y+'" r="29"/><text class="node-label" x="'+x+'" y="'+(y+6)+'">'+id+'</text>';
      svg+='<text class="radius" x="'+x+'" y="'+(y+51)+'">'+(!state?'inactive · r=0':'r='+state.radius+(state.published?' · 已发布':' · 待发布'))+'</text>';
    });
    $('#graph').innerHTML=svg;
  }
  function render() {
    const f=trace.frames[step];$('#progress').max=trace.frames.length-1;$('#progress').value=step;
    $('#position').textContent=(step+1)+' / '+trace.frames.length;$('#frameTitle').textContent=f.title;$('#detail').textContent=f.detail;
    graph(f);
    $('#states').innerHTML=f.storage.map(s=>'<tr><td>'+s.id+' <small>#'+s.index+'</small></td><td>'+s.radius+'</td><td>'+(s.cap===null?'—':s.cap)+'</td><td>'+(s.mask===null?'—':'0x'+s.mask.toString(16))+'</td><td>'+(s.mate===null?'unmatched':s.mate==='boundary'?'boundary':f.storage[s.mate].id)+'</td></tr>').join('');
    $('#local').textContent=JSON.stringify(f.local,null,2);
    $('#partial').textContent='weight='+f.partial.weight+' · obs=0x'+f.partial.obs.toString(16)+' · edges=['+f.partial.edges.join(', ')+']';
    $('#return').hidden=!f.finished;
    if(f.finished) {
      const solution=f.oracle.solutions[0];
      $('#return').innerHTML='<strong>返回 void；调用方 finish：complete='+f.complete+'</strong><p>'+(f.complete?
        '每个活动端点均匹配，全部匹配边 tight。匹配权重等于可行对偶半径之和，得到整个输入的最优性证书。':
        '还存在 unmatched seed。导入的是全部 seed 的半径与 mate；已有局部匹配保留为可撤销状态。不能锁死已配对边，也不能只将残余节点单独求解后加上 partial.weight。')+'</p><p>独立小图穷举对照：最优整数权重 '+f.oracle.weight+'，一种最优边集合 ['+solution.edges.join(', ')+']，obs=0x'+solution.obs.toString(16)+'。这里只枚举边子集核验输入，未模拟完整 MWPM 内部，也没有执行或计时原生解码器。</p>';
    }
    displayCode(f.line);document.body.dataset.frame=String(step);document.body.dataset.scenario=$('#scenario').value;
  }
  function pause(){if(timer!==null)clearInterval(timer);timer=null;$('#play').textContent='播放';}
  function load(){pause();trace=Mode3.execute($('#scenario').value,$('#reverse').checked);step=0;render();}
  $('#scenario').innerHTML=Object.entries(Mode3.scenarios).map(([k,s])=>'<option value="'+k+'">'+s.title+'</option>').join('');
  $('#sourceSelect').innerHTML=SOURCE.blocks.map(b=>'<option value="'+b.id+'">'+b.title+'</option>').join('');
  $('#scenario').onchange=load;$('#reverse').onchange=load;
  $('#play').onclick=()=>{if(timer!==null){pause();return;}if(step===trace.frames.length-1)step=0;$('#play').textContent='暂停';timer=setInterval(()=>{if(step<trace.frames.length-1){step++;render();}else pause();},650);};
  $('#prev').onclick=()=>{pause();step=Math.max(0,step-1);render();};
  $('#next').onclick=()=>{pause();step=Math.min(trace.frames.length-1,step+1);render();};
  $('#reset').onclick=load;$('#end').onclick=()=>{pause();step=trace.frames.length-1;render();};
  $('#progress').oninput=e=>{pause();step=Number(e.target.value);render();};
  $('#sourceSelect').onchange=e=>{pause();displayCode(SOURCE.blocks.find(b=>b.id===e.target.value).lines.find(l=>l.key).key,false);};
  function resize(delta){font=Math.min(22,Math.max(13,font+delta));document.documentElement.style.setProperty('--code-size',font+'px');$('#fontSize').textContent=font;}
  $('#smaller').onclick=()=>resize(-1);$('#larger').onclick=()=>resize(1);
  load();
  window.MODE3_VIEW={load,render,trace:()=>trace,step:()=>step,select:(key,reversed=false)=>{$('#scenario').value=key;$('#reverse').checked=reversed;load();},last:()=>{$('#end').click();}};
})();
