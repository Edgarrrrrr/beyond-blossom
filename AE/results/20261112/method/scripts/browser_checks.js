/* Only activated by ?verify=1; exercises the visible DOM controls offline. */
(() => {
  if(new URLSearchParams(location.search).get('verify')!=='1') return;
  window.addEventListener('load',()=>{
    let result={passed:false,viewport:[innerWidth,innerHeight],checks:[]};
    const check=(test,label)=>{if(!test)throw Error(label);result.checks.push(label);};
    try {
      if(window.MODE3_VIEW) {
        for(const key of Object.keys(Mode3.scenarios)) for(const reverse of [false,true]) {
          const select=document.querySelector('#scenario');select.value=key;select.dispatchEvent(new Event('change'));
          const checkbox=document.querySelector('#reverse');checkbox.checked=reverse;checkbox.dispatchEvent(new Event('change'));
          document.querySelector('#next').click();check(MODE3_VIEW.step()===1,key+' next');
          document.querySelector('#prev').click();check(MODE3_VIEW.step()===0,key+' previous');
          const slider=document.querySelector('#progress');slider.value=5;slider.dispatchEvent(new Event('input'));check(MODE3_VIEW.step()===5,key+' seek');
          document.querySelector('#end').click();check(!document.querySelector('#return').hidden,key+' return visible');
          check(document.querySelector('.source-row.current .explain').textContent.length>0,key+' line explanation visible');
          document.querySelector('#reset').click();check(MODE3_VIEW.step()===0,key+' reset');
        }
        document.querySelector('#play').click();check(document.querySelector('#play').textContent==='暂停','play');document.querySelector('#play').click();check(document.querySelector('#play').textContent==='播放','pause');
        document.querySelector('#larger').click();check(document.querySelector('#fontSize').textContent==='16','font larger');document.querySelector('#smaller').click();
        const source=document.querySelector('#sourceSelect');for(const option of source.options){source.value=option.value;source.dispatchEvent(new Event('change'));check(document.querySelectorAll('.source-row .explain').length>0,'source '+option.value);}
        const params=new URLSearchParams(location.search);MODE3_VIEW.select(params.get('scene')||'partial');
        if(params.get('step')==='end') MODE3_VIEW.last();
        else if(params.has('step')) {const slider=document.querySelector('#progress');slider.value=Number(params.get('step'));slider.dispatchEvent(new Event('input'));}
      } else {
        const select=document.querySelector('#view');for(const value of ['side','unified']){select.value=value;select.dispatchEvent(new Event('change'));check(document.body.classList.contains('unified-mode')===(value==='unified'),'diff '+value);}
        document.querySelector('#font-up').click();check(document.querySelector('#font-size').textContent==='16','diff larger font');document.querySelector('#font-down').click();
        const search=document.querySelector('#search');search.value='__not_a_real_symbol__';search.dispatchEvent(new Event('input'));check(!document.querySelector('#no-match').hidden,'diff missing search');search.value='';search.dispatchEvent(new Event('input'));
        document.querySelector('#next').click();document.querySelector('#prev').click();
        check(document.querySelectorAll('.line-explanation').length>0,'diff default explanations');
        select.value=innerWidth<1100?'unified':'side';select.dispatchEvent(new Event('change'));
      }
      check(document.documentElement.scrollWidth<=innerWidth+1,'no document horizontal overflow');
      result.passed=true;
    } catch(error) {result.error=String(error.stack||error);}
    document.body.dataset.browserVerification=result.passed?'passed':'failed';
    const output=document.createElement('pre');output.id='browser-verification';output.hidden=true;output.textContent=JSON.stringify(result);document.body.append(output);
  });
})();
