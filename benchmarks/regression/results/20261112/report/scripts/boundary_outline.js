// The variant changes the compiler call boundary, not the state transition.
// Reuse the audited event model and make the outlined call/return explicit.
function outline(scene){
 const original=primal(scene),out=[];
 for(const frame of original){
  const f=copy(frame);
  if(f.code==='structured_boundary'&&f.line==='auto node ='){
   const entry=copy(f);entry.line='[[gnu::noinline]]';
   entry.message='进入独立的边界处理函数；noinline 是编译属性，本身不执行状态更新。';
   entry.state.call_stack=['process_event','handle_tree_hitting_boundary'];out.push(entry);
  }
  const inside=f.code!=='structured_dispatch';
  f.state.call_stack=['process_event'];
  if(inside)f.state.call_stack.push('handle_tree_hitting_boundary');
  if(f.code==='structured_shatter')f.state.call_stack.push('shatter_descendants_into_matches_and_freeze');
  if(f.code==='structured_root')f.state.call_stack.push('become_root');
  if(f.state.returned)f.state.call_stack=[];
  out.push(f);
  if(f.code==='structured_boundary'&&f.line==='--num_active_trees'){
   const ret=copy(f);ret.line='}';ret.message='边界函数返回 void；返回 process_event，匹配与冻结结果保留。';
   ret.state.call_stack=['process_event'];out.push(ret);
  }
 }
 return out;
}
