use fusion_blossom::mwpm_solver::{PrimalDualSolver, SolverParallel, SolverSerial};
use fusion_blossom::pointers::*;
use fusion_blossom::util::{PartitionConfig, SolverInitializer, SyndromePattern, VertexIndex};
use serde_json::{json, Value};
use std::fs::File;
use std::io::{BufReader, BufWriter, Write};
use std::time::Instant;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    assert_eq!(args.len(),4,"usage: blossom INPUT OUTPUT_PREFIX parity|fusion|parity-uf");
    let j: Value = serde_json::from_reader(BufReader::new(File::open(&args[1])?))?;
    let initializer: SolverInitializer=serde_json::from_value(j["initializer"].clone())?;
    let mut solver: Box<dyn PrimalDualSolver> = match args[3].as_str() {
        "parity"|"parity-uf" => {
            let serial=SolverSerial::new(&initializer);
            if args[3]=="parity-uf" { serial.primal_module.write().max_tree_size=0; }
            Box::new(serial)
        },
        "fusion" => {
            let partitions: PartitionConfig=serde_json::from_value(j["partition"].clone())?;
            assert!(partitions.partitions.len()>1,"Fusion must have actual subproblems");
            Box::new(SolverParallel::new(&initializer,&partitions.info(),
                json!({"primal":{"thread_pool_size":2,"pin_threads_to_cores":false},
                       "dual":{"thread_pool_size":1,"enable_parallel_execution":false}})))
        },
        _ => panic!("unknown decoder")
    };
    let load = |key:&str| -> Vec<SyndromePattern> {
        j[key].as_array().unwrap().iter().map(|s| {
            let v:Vec<VertexIndex>=serde_json::from_value(s.clone()).unwrap();
            SyndromePattern::new_vertices(v)
        }).collect()
    };
    let samples=load("blossom_samples"); let warm=load("blossom_warmup");
    let mut parity=vec![false;initializer.vertex_num as usize];
    let mut virtuals=vec![false;initializer.vertex_num as usize];
    for &v in &initializer.virtual_vertices {virtuals[v as usize]=true;}
    let original: Vec<usize>=serde_json::from_value(j["blossom_edge_to_original"].clone())?;
    let edges=j["edges"].as_array().unwrap();
    let mut validate=|s:&SyndromePattern, chain:&Vec<fusion_blossom::util::EdgeIndex>| {
        parity.fill(false);
        let mut weight=0.0; let mut iw=0_i64; let mut obs=0_u64;
        for &e in chain {
            let (u,v,q)=initializer.weighted_edges[e as usize];
            parity[u as usize]^=true; parity[v as usize]^=true;
            let e=&edges[original[e as usize]];
            weight+=e[2].as_f64().unwrap(); iw+=q as i64; obs^=e[3].as_u64().unwrap();
        }
        for &v in &s.defect_vertices {parity[v as usize]^=true;}
        assert!(parity.iter().zip(&virtuals).all(|(&p,&v)|!p||v),"Hc != syndrome");
        (weight,iw,obs)
    };
    assert!(!warm.is_empty()&&!samples.is_empty());
    let start=Instant::now(); let mut warmed=0;
    loop {
        for s in &warm {solver.solve(s); let c=solver.subgraph(); validate(s,&c); solver.clear(); warmed+=1;}
        if start.elapsed().as_secs_f64()>=j["warmup_seconds"].as_f64().unwrap() {break;}
    }
    let warm_seconds=start.elapsed().as_secs_f64();
    let mut output=BufWriter::new(File::create(format!("{}.csv",args[2]))?);
    let mut chains=BufWriter::new(File::create(format!("{}.corrections.jsonl",args[2]))?);
    writeln!(output,"repeat,shot,decode_ns,weight,integer_weight,observable")?;
    for r in 0..j["repeats"].as_u64().unwrap() {for (i,s) in samples.iter().enumerate() {
        // Input arrays/patterns and partitioning are ready before timing.
        let start=Instant::now(); solver.solve(s); let c=solver.subgraph();
        let elapsed=start.elapsed().as_nanos();
        let (weight,iw,obs)=validate(s,&c);
        solver.clear();
        writeln!(output,"{r},{i},{elapsed},{weight:.17},{iw},{obs}")?;
        if r==0 {let c:Vec<_>=c.iter().map(|&e|original[e as usize]).collect();
            serde_json::to_writer(&mut chains,&c)?; writeln!(chains)?;}
    }}
    output.flush()?; chains.flush()?;
    println!("{}",json!({"completed":true,"warmup_calls":warmed,"warmup_wall_seconds":warm_seconds}));
    Ok(())
}
