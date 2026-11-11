#!/usr/bin/env python3
"""Separate Chinese explanation layer; original source bytes remain unchanged."""
from pathlib import Path
import hashlib,json,re
ROOT=Path(__file__).resolve().parents[1]
M=json.loads((ROOT/'sources/manifest.json').read_text())
D={
'auto range =':'建立编译期可禁用的统计范围对象；正式纯计时构建关闭内部统计。',
'record_primal_tree_snapshot();':'记录调试统计的树快照；关闭 profile 的构建使用空操作。',
'switch (event.event_type) {':'读取事件类别，进入对应的 primal 状态更新分支。',
'case REGION_HIT_REGION:':'区域与区域碰撞：按两端树/匹配状态选择连接两树、形成 blossom 或吸收已有匹配。',
'handle_region_hit_region(event);':'把原事件交给区域碰撞处理器；此辅助分支不属于边界场景的演示。',
'case REGION_HIT_BOUNDARY:':'区域碰到边界：进入边界增广路径。',
'handle_tree_hitting_boundary(event.region_hit_boundary_event_data);':'传递碰撞区域和压缩边，调用下方完整展示的边界处理函数。',
'case BLOSSOM_SHATTER:':'blossom 收缩到可裂解状态：进入裂解处理分支。',
'handle_blossom_shattering(event.blossom_shatter_event_data);':'调用 blossom 裂解器；该分支在本页所选输入中不执行。',
'case NO_EVENT:':'没有 MWPM 事件：保持匹配状态。',
'// Do nothing.':'无事件分支不修改状态。',
'break;':'结束当前 switch 分支，继续函数尾部。',
'default:':'拦截未定义的事件类别。',
'throw std::invalid_argument("Unrecognized event type");':'以异常报告未知事件，避免静默接受无效状态。',
'auto node = region_for(event.region)->alt_tree_node;':'由 RegionId 取得碰撞区域，读取它所属的交替树节点。',
'node->become_root();':'沿父链旋转交替树，使碰撞节点成为根；演示场景已是根，立即返回。',
'// Match descendents, deleting AltTreeNodes and freezing GraphFillRegions':'先把后代的交替配对转成匹配，回收树节点并冻结相应区域。',
'shatter_descendants_into_matches_and_freeze(*node);':'递归处理当前根的后代；匹配区域保留，树节点回收到 arena。',
'// Now match the event region to the boundary and freeze':'后代处理完毕，碰撞区域可以与边界直接配对。',
'region_for(event.region)->match = Match{NO_REGION, event.edge};':'把边界匹配写入区域；NO_REGION 表示没有另一个实体区域。',
'flooder.set_region_frozen(*region_for(event.region));':'调用 dual 辅助函数冻结碰撞区域；该辅助函数内部队列操作不在本页展开。',
'assert(num_active_trees > 0);':'调试构建检查至少存在一棵活动树。',
'--num_active_trees;':'边界增广消除一个未匹配根，活动树数减一。',
'for (auto &child_edge : alt_tree_node.children) {':'按子边遍历这棵交替树的后代。',
'shatter_descendants_into_matches_and_freeze(*child_edge.alt_tree_node);':'对当前子树递归完成匹配、冻结和树节点回收。',
'if (alt_tree_node.inner_region) {':'只有含 inner/outer 区域对的非根节点执行配对。',
'alt_tree_node.parent = AltTreeEdge();':'清空已处理节点的父边。',
'alt_tree_node.inner()->add_match(alt_tree_node.outer(), alt_tree_node.inner_to_outer_edge);':'用保存的 inner→outer 压缩边恢复这对区域的匹配。',
'flooder.set_region_frozen(*alt_tree_node.inner());':'冻结 inner 区域，停止其半径随时间变化。',
'flooder.set_region_frozen(*alt_tree_node.outer());':'冻结 outer 区域，保留刚建立的匹配。',
'alt_tree_node.inner()->alt_tree_node = nullptr;':'解除 inner 区域到即将回收树节点的指针。',
'alt_tree_node.outer()->alt_tree_node = nullptr;':'解除 outer 区域到即将回收树节点的指针。',
'if (alt_tree_node.outer_region) {':'根节点也可能仅有 outer 区域，需要解除其树指针。',
'node_arena.del(&alt_tree_node);':'将交替树节点归还 arena；区域对象本身仍存在。',
'// Performs a tree rotation turning this node into the root of the tree/':'把当前节点旋转为交替树根。',
'if (!parent.alt_tree_node)':'父节点为空时，当前节点已是根。',
'return;':'沿当前路径提前返回；此前写入的局部状态保留。',
'auto old_parent = parent.alt_tree_node;':'保存旋转前的父节点指针。',
'old_parent->become_root();':'先递归把旧父节点变成根，随后反向接回当前节点。',
'parent.alt_tree_node->inner_region = inner_region;':'把当前 inner 区域交给旧父节点，维持交替关系。',
'parent.alt_tree_node->inner_to_outer_edge = parent.edge;':'旧父节点使用原父边作为新的 inner→outer 边。',
'inner_region = NO_REGION;':'新根不再拥有 inner 区域。',
'unstable_erase(parent.alt_tree_node->children, [&](const AltTreeEdge &x) {':'从旧父节点的子边列表移除指向当前节点的边。',
'return x.alt_tree_node == this;':'仅删除目标为当前节点的子边。',
'parent = AltTreeEdge();':'清空新根的父边。',
'add_child(AltTreeEdge(old_parent, inner_to_outer_edge.reversed()));':'把旧父节点作为新子节点接回，并翻转连接边的方向。',
'inner_to_outer_edge = CompressedEdge{nullptr, nullptr, 0};':'清空新根不再使用的 inner→outer 边与 observable 摘要。',
'auto& seed = seeds[index];':'取得当前活动 syndrome 的 seed 状态引用。',
'if (seed.saturated)':'已完成一次合法半径增长的 seed 不重复增长。',
'const auto& node = *seed.node;':'读取 seed 对应的图节点及邻接表。',
'weight_int limit = seed.base_cap;':'从本节点最小邻边权重开始，约束它相对非 seed 节点的最大势能。',
'const size_t end = index + 1 < seeds.size() ? seeds[index + 1].candidate_begin : candidates.size();':'当前 seed 的候选段在下一 seed 起点或 candidates 末尾结束。',
'for (size_t position = seed.candidate_begin; position < end; ++position) {':'顺序扫描一次构建的活动邻居候选段。',
'const auto& candidate = candidates[position];':'读取候选邻居的 seed 索引和原邻接槽。',
'const auto other_radius = seeds[candidate.seed].radius;':'读取另一端已分配的半径，计算这条边剩余的 dual 余量。',
'assert(other_radius <= node.neighbor_weights[candidate.slot]);':'检查另一端半径未超过这条边的非负整数权重。',
'limit = std::min(limit, node.neighbor_weights[candidate.slot] - other_radius);':'更新约束 y_this ≤ weight − y_other，保持所有扫描边的 dual 可行性。',
'if (!node.neighbors.empty()) {':'孤立节点没有可用增长约束，留给完整求解器处理。',
'// The full solver\'s two-growing-front collision times use an even':'完整求解器的双增长前沿使用偶数半径格点。',
'// radius lattice. UserGraph quantizes all edge weights onto it.':'UserGraph 已把边权量化到相同格点。',
'seed.radius = limit & ~weight_int{1};':'清除最低位，把允许半径向下取为偶数，维持碰撞时间的整数格点约束。',
'seed.saturated = true;':'记录本 seed 已完成增长，后续调用直接返回。',
'grow(index);':'先调用 grow，建立当前 seed 的合法半径。',
'// Try detector partners first, preserving the structured decoder\'s local':'优先尝试探测器之间的配对。',
'// preference without relying on a fixed stencil or a degree limit.':'该遍历适用于任意邻接表，不要求固定 stencil 或最大度数。',
'const auto slot = candidate.slot;':'取原图邻接槽，后面从同一槽读取权重和 observable 掩码。',
'if (seeds[candidate.seed].mate != UNMATCHED)':'已经配对的另一端不作为当前局部匹配的空闲端点。',
'continue;':'跳过当前候选，继续检查下一条边。',
'auto& mate = seeds[candidate.seed];':'取得候选另一端的可写 seed 状态。',
'if (uint64_t(seed.radius) + mate.radius < node.neighbor_weights[slot])':'当前两端半径之和还未达到边权时，尝试增长另一端。',
'grow(candidate.seed);':'调用同一 dual 可行增长函数更新候选另一端。',
'if (uint64_t(seed.radius) + mate.radius != node.neighbor_weights[slot])':'只有 tight 边（两端势能和恰等于权重）可以成为暂配边。',
'seed.mate = candidate.seed;':'写入当前 seed 的配对索引。',
'mate.mate = index;':'对称写入另一端的配对索引。',
'seed.match_observables = mate.match_observables = node.neighbor_observables[slot];':'两端保存相同的边 observable 摘要，便于导入或提取时直接使用。',
'// Both endpoints retain the selected edge summary, avoiding a reverse':'两端都保留选中边的摘要。',
'// adjacency lookup when importing or extracting the provisional pair.':'因此导入或提取该暂配对时无需反查反向邻接边。',
'obs_mask ^= node.neighbor_observables[slot];':'异或合并这条匹配边跨越的 observable。',
'weight += node.neighbor_weights[slot];':'把选中整数边权加入局部匹配总权重。',
'if (!node.neighbors.empty() && !node.neighbors[0] && seed.radius == node.neighbor_weights[0]) {':'没有匹配探测器时，仅在槽0为边界且半径等于边界权重时接收边界匹配。',
'seed.mate = BOUNDARY;':'标记当前 seed 已与边界配对。',
'seed.match_observables = node.neighbor_observables[0];':'保存边界边的 observable 摘要。',
'obs_mask ^= node.neighbor_observables[0];':'把边界边的 observable 异或到结果。',
'weight += node.neighbor_weights[0];':'把边界边的整数权重加入结果。',
'return node ? active[size_t(node - nodes)] : 0;':'非空邻居用节点数组偏移查 active 索引；0表示非活动 seed，空指针表示边界也返回0。',
'if (seeds.empty())':'空 syndrome 不需要密度判断。',
'return false;':'保留预解码路径，外层仍可检查完整匹配。',
'size_t degree_sum = 0;':'初始化本次 syndrome 的邻接度总和。',
'for (const auto& seed : seeds) {':'只遍历当前活动 seed，不扫描全图节点。',
'const auto& neighbors = seed.node->neighbors;':'读取当前 seed 的邻接向量。',
'if (!neighbors.empty() && !neighbors[0])':'只要有直接边界边，就保留可能立即完成的预解码机会。',
'degree_sum += neighbors.size();':'累加本次活动节点的度数。',
'if (degree_sum / seeds.size() < 32)':'活动节点平均度低于32时，不启用稀疏高阶回退。',
'size_t sampled = 0, active_neighbors = 0;':'分别计数探测的邻接条目和其中仍为活动 seed 的邻居。',
'const size_t seed_samples = std::min(size_t{8}, (seeds.size() - 1) / 4 + 1);':'样本节点数为 min(8, ceil(seed数/4))，缩短小 syndrome 的探测预算。',
'const size_t seed_step = seeds.size() / seed_samples;':'预先计算均匀节点步长的商，循环内复用。',
'const size_t seed_remainder = seeds.size() % seed_samples;':'预先计算余数，用累加进位补偿均匀节点位置。',
'size_t seed_position = 0, seed_error = 0;':'从第0个 seed 开始，余数累积器置零。',
'for (size_t sample = 0; sample < seed_samples; ++sample) {':'最多处理8个均匀抽样的活动节点。',
'const auto& neighbors = seeds[seed_position].node->neighbors;':'读取本轮抽中的 seed 邻接向量。',
'if (neighbors.size() >= 32) {':'度数至少32时，固定抽32个均匀邻接槽。',
'for (size_t position = 0; position < 32; ++position)':'循环访问32个邻接采样位置。',
'active_neighbors += seed_index(neighbors[(position * neighbors.size()) >> 5]) != 0;':'槽号为 floor(position×degree/32)，右移5位实现除32；内联查表累计活动邻居。',
'sampled += 32;':'把本轮32次邻接探测加入分母。',
'} else {':'对较短邻接表走完整扫描分支。',
'for (const auto* neighbor : neighbors)':'度数不足32时扫描所有邻居。',
'active_neighbors += seed_index(neighbor) != 0;':'累计当前邻居是否属于本次活动 syndrome。',
'sampled += neighbors.size();':'将实际扫描条目数加入密度分母。',
'seed_position += seed_step;':'推进商步长，避免循环里重新执行可变整数除法。',
'seed_error += seed_remainder;':'累加每步余数。',
'if (seed_error >= seed_samples) {':'余数达到除数时需要一次进位。',
'++seed_position;':'给下一个 seed 位置补一个单位。',
'seed_error -= seed_samples;':'消去已进位的除数，保留剩余误差。',
'return sampled && active_neighbors * 8 < sampled;':'采样非空且局部活动邻居比例严格小于1/8时跳过预解码。',
'if (mwpm.predecode_enabled) {':'解码入口先选择是否运行通用图预解码器。',
'mwpm.predecoder.prepare(graph, detection_events, mwpm.flooder.negative_weight_detection_events);':'验证输入，跳过用户边界，并用负权边端点做异或规范化。',
'mwpm.predecoder.run();':'按当前图权重执行合法半径增长与 tight 边暂配。',
'if (mwpm.predecoder.complete)':'只有所有规范化 seed 都获得 tight 匹配时才允许结束求解时间线。',
'mwpm.import_predecode_state();':'不完整结果导入所有 seed 的状态，交给完整 blossom 求解器继续求解。',
'} else if (mwpm.flooder.negative_weight_detection_events.empty()) {':'关闭预解码时保留原普通 root 创建路径。',
'if (mwpm.flooder.negative_weight_detection_events.empty()) {':'旧实现按是否含负权边端点选择普通 root 创建路径。',
'// Just add detection events if graph has no negative weights':'没有负权修正时，原实现直接创建每个探测事件的 root。',
'for (auto& detection : detection_events) {':'遍历保存的探测器索引。',
'if (detection >= mwpm.flooder.graph.nodes.size())':'检查探测器索引是否超出图节点范围。',
'throw std::invalid_argument(':'以参数异常报告非法输入索引。',
'"The detection event with index " + std::to_string(detection) +':'把非法探测器索引写入异常信息。',
'" does not correspond to a node in the graph, which only has " +':'异常信息说明该索引不属于图节点集合。',
'std::to_string(mwpm.flooder.graph.nodes.size()) + " nodes.");':'在异常中补充图节点总数。',
'if (detection + 1 > mwpm.flooder.graph.is_user_graph_boundary_node.size() ||':'没有显式边界标记条目时，该索引按普通图节点处理。',
'!mwpm.flooder.graph.is_user_graph_boundary_node[detection])':'有边界标记时，只创建非边界节点的 root。',
'mwpm.create_detection_event(&mwpm.flooder.graph.nodes[detection]);':'为该探测器创建普通 blossom 初始 root。',
'if (predecoder.skipped) {':'密度门控选择回退时，不导入暂配或非零半径。',
'for (const auto& seed : predecoder.seeds)':'复用已验证、已规范化的活动 seed 列表。',
'create_detection_event(seed.node);':'为每个 seed 创建原求解器的零半径未匹配 root。',
'for (const auto& seed : predecoder.seeds) {':'遍历全部 seed；暂配 seed 也保留在完整求解问题中。',
'auto* region = flooder.region_arena.alloc_default_constructed();':'从区域 arena 构造一个 singleton 区域。',
'auto* node = seed.node;':'取得此 singleton 对应的探测器节点。',
'region->radius = VaryingCT::from_base_and_growth(':'建立完整求解器使用的随时间变化半径。',
'seed.radius, seed.mate == OneHopPredecoder::UNMATCHED ? +1 : 0);':'导入合法起始半径：未匹配者增长率+1，暂配者保持冻结。',
'region->shell_area.push_back(node);':'singleton 的区域外壳初始只包含其源节点。',
'node->reached_from_source = node;':'记录源探测器为节点自身。',
'node->radius_of_arrival = 0;':'源节点的到达半径初始化为0。',
'node->region_that_arrived = region;':'把源节点归属设置为新建 singleton 区域。',
'node->region_that_arrived_top = region;':'该 singleton 尚未嵌入 blossom，顶层区域就是自身。',
'node->wrapped_radius_cached = 0;':'初始不存在外层 blossom 包裹半径。',
'auto* region = seed.node->region_that_arrived;':'第二遍读取第一遍已发布的 singleton 区域。',
'if (seed.mate == OneHopPredecoder::UNMATCHED) {':'未匹配 seed 建立活动交替树 root。',
'auto* root = node_arena.alloc_unconstructed();':'从树节点 arena 取得未构造的存储。',
'new (root) AltTreeNode(region);':'在该存储上构造单区域交替树根。',
'region->alt_tree_node = root;':'区域回指它所属的活动树根。',
'flooder.reschedule_events_at_detector_node(*seed.node);':'从已导入的合法半径重新安排这个 root 的 dual 事件。',
'auto* mate = seed.mate == OneHopPredecoder::BOUNDARY ? nullptr : predecoder.seeds[seed.mate].node;':'读取暂配另一端；边界匹配使用空节点。',
'region->match = {mate ? mate->region_that_arrived : nullptr,':'把另一端的已发布区域写入暂配摘要；边界没有区域。',
'{seed.node, mate, seed.match_observables}};':'保存源节点、目标节点和 observable；完整求解器后续仍可撤销或改配。',
'complete = std::all_of(seeds.begin(), seeds.end(), [](const Seed& seed) { return seed.mate != UNMATCHED; });':'所有 seed 都有 tight 匹配时，完整匹配达到 dual 下界，标记局部求解完成。',
'result_pending = complete;':'仅完整解建立待提取结果，防止同一结果被重复消费。',
}
COMMENT={
'// In a sparse high-degree neighborhood the cap/candidate scan duplicates':'稀疏高阶邻域的完整 cap/候选扫描可能与后续 flooder 重复工作。',
'// substantial flooder work, while few local pairs become available. Bound':'当可用局部配对很少时，先限制分派探测的工作量。',
'// the dispatch cost before that scan: inspect seed degrees, then at most':'门控位于完整扫描之前：先读活动节点度，再进行有界探测。',
'// 8 * 32 evenly spaced adjacency entries. This is a performance heuristic;':'最多探测8×32个均匀邻接条目；这是性能启发式。',
'// the fallback is the full solver on the same normalized syndrome.':'回退仍在同一个规范化 syndrome 上调用完整求解器。',
'// A direct boundary can finish a seed even without an active neighbor.':'存在直接边界边时，即使没有活动邻居也可能完成匹配。',
'// Short shots should not pay the maximum fixed probe cost. Inspect at':'短 syndrome 不承担最大固定探测预算。',
'// most one neighborhood per four seeds, with a nonzero rounded-up count.':'每四个 seed 最多抽一个邻域，向上取整以保证非零预算。',
'// Exactly floor((sample + 1) * seeds.size() / seed_samples), without':'递推严格产生 floor((sample+1)×seed数/样本数) 的下一位置。',
'// a variable integer division on each sampled neighborhood.':'把可变除法移出每个邻域的循环。',
'// Local density preserves dense active components even when the graph has':'按局部活动密度判断，保留稠密活动子图的预解码机会。',
'// many disconnected or irrelevant vertices. No graph-level cache is used.':'无关节点和连通分量不影响判定；没有跨 shot 图级缓存。',
'// prepare() already validated and normalized these events. A dispatch':'prepare 已完成合法性检查和负权规范化。',
'// fallback needs no radius/matching import or second syndrome pass.':'门控回退无需再导入半径/暂配，也无需再次规范化 syndrome。',
'// Publish every singleton before scheduling any event: a provisional':'安排任何事件前先发布全部 singleton 区域。',
'// match must remain visible to later alternating trees and blossoms.':'暂配区域必须对后续交替树和 blossom 可见，不能被贪心删去。',
}
FUNCTIONS={'structured_dispatch':'接收一个 dual 通知并分派 primal 更新；候选定义放入 .inl，使调用点可见函数体。','structured_boundary':'处理树碰到边界的完整状态更新。','structured_shatter':'递归把交替树后代转换为匹配并回收树节点。','structured_root':'沿父链旋转当前交替树节点为根。','generic_grow':'把一个 seed 的半径增长到当前约束允许的偶数格点。','generic_match':'为一个 seed 尝试 tight 探测器配对或 tight 边界配对。','density_gate':'按当前活动邻域的有界采样决定是否跳过预解码。','seed_index':'类内定义的小访问器，允许编译器在门控循环中内联索引读取。','generic_import':'把全部预解码状态导入完整求解器，保留暂配的可撤销性。'}
def explain(name,line):
    s=line.strip()
    if not s:return '代码段分隔。'
    if name=='generic_import' and s=='} else {':return '已经暂配的 seed 记录区域匹配摘要；后续交替树仍可改配。'
    if s in D:return D[s]
    if s in COMMENT:return COMMENT[s]
    if s in ('}', '});'):return '结束当前控制或函数代码块。'
    if s.startswith('#if'):return '仅在编译期开启 profile 时包含后续诊断代码。'
    if s=='#endif':return '结束 profile 条件编译区。'
    if ('counted_range' in s or 'record_counter' in s or 'record_region_' in s or 'ScopedRange' in s):return '编译期诊断钩子；正式计时配置关闭内部 profile 统计。'
    if 'profile::RangeId::MatchBoundary' in s:return '把该范围关联到边界匹配计数器；纯计时构建不采集此计数。'
    if name in FUNCTIONS and (('Mwpm::' in s or 'AltTreeNode::' in s or 'OneHopPredecoder::' in s or 'size_t seed_index(' in s) and s.endswith('{')):return FUNCTIONS[name]
    if s.startswith('pm::profile::RangeId') or s.startswith('beyond_blossom::profile::RangeId'):return '关联当前阶段的 profile 范围与调用计数器。'
    raise KeyError((name,s))
result={'revision':1,'language':'zh-CN','source_manifest_sha256':hashlib.sha256((ROOT/'sources/manifest.json').read_bytes()).hexdigest(),'excerpts':{}}
missing=[]
for name,entry in M['excerpts'].items():
    result['excerpts'][name]={}
    for side,source in entry.items():
        if source is None:continue
        rows=[]
        for offset,line in enumerate(source['text'].splitlines()):
            try:note=explain(name,line)
            except KeyError:missing.append((name,side,source['start']+offset,line));continue
            rows.append({'line':source['start']+offset,'source':line,'explanation':note})
        result['excerpts'][name][side]=rows
if missing:
    print(json.dumps(missing,indent=2,ensure_ascii=False));raise SystemExit(1)
result['nonempty_lines']=sum(bool(row['source'].strip()) for v in result['excerpts'].values() for rows in v.values() for row in rows)
result['explained_nonempty_lines']=result['nonempty_lines'];result['missing']=[]
(ROOT/'sources/line-explanations.json').write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n')
print('Explained',result['nonempty_lines'],'nonempty original lines')
