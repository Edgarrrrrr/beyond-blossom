#!/usr/bin/env python3
"""Freeze two small historical prefixes, then compare old ELF and the public API.

This is a correctness/lifecycle regression. It does not reproduce a performance
experiment or accept historical timings for a rebuilt binary. All outputs use
new directories; original plans, inputs, and executables remain unchanged.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys


MASK = (1 << 64) - 1
MAGIC = 0x5354454E43494C31
THREADS = dict(upstream=1, gridsimd=1, predecode=1, component=2, tailreset=4, spatial=8)
BASELINES = dict(upstream="predecode-official", predecode="candidate-predecode",
                 component="candidate-predecode", tailreset="candidate-tailreset", spatial="candidate-spatial")


def need(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            result.update(block)
    return result.hexdigest()


def load(path):
    return json.loads(Path(path).read_text())


def save(path, value):
    with Path(path).open("x") as target:
        json.dump(value, target, indent=2, allow_nan=False)
        target.write("\n")


def frozen(reference):
    path = Path(reference["path"]).resolve()
    need(path.is_file() and sha(path) == reference["sha256"], "Frozen file changed: " + str(path))
    return path


def mix(state, value):
    state ^= (value + 0x9E3779B97F4A7C15 + (state << 6) + (state >> 2)) & MASK
    state ^= state >> 30
    state = state * 0xBF58476D1CE4E5B9 & MASK
    state ^= state >> 27
    state = state * 0x94D049BB133111EB & MASK
    return (state ^ (state >> 31)) & MASK


def read_exact(source, count):
    result = source.read(count)
    need(len(result) == count, "Truncated historical input")
    return result


def prefix(case, destination, count):
    """Copy records unchanged, updating only the new prefix's size/checksums."""
    destination.mkdir()
    original = Path(case["inputs"]["original"]["path"])
    reference = frozen(case["inputs"]["reference"])
    source_digest = hashlib.sha256()
    records, refs, packed_rows = [], [], []
    with original.open("rb") as source, reference.open(newline="") as reference_file:
        raw_header = read_exact(source, 96)
        header = list(struct.unpack("<12Q", raw_header))
        source_digest.update(raw_header)
        need(header[0] == MAGIC and header[1] == 2 and count <= header[4], "Expected the archived v2 input")
        need(header[2] == case["case"]["d"] and header[11] == case["case"]["stream_id"], "Historical input identity differs")
        rows = csv.DictReader(reference_file)
        events = syndrome_hash = result_hash = 0
        for index in range(count):
            raw_count = read_exact(source, 8)
            size, = struct.unpack("<Q", raw_count)
            need(size <= header[5], "Invalid detector count")
            raw_hits = read_exact(source, size * 8)
            raw_result = read_exact(source, 16)
            hits = struct.unpack("<" + "Q" * size, raw_hits)
            observable, weight = struct.unpack("<Qq", raw_result)
            need(all(x < header[5] for x in hits) and list(hits) == sorted(set(hits)), "Invalid detector IDs/order")
            row = next(rows)
            need((int(row["shot_index"]), int(row["detection_events"]), int(row["observable_mask"]), int(row["integer_weight"]))
                 == (index, size, observable, weight), "Historical binary and independent reference CSV differ")
            raw = raw_count + raw_hits + raw_result
            records.append(raw)
            source_digest.update(raw)
            events += size
            syndrome_hash = mix(syndrome_hash, size)
            for hit in hits:
                syndrome_hash = mix(syndrome_hash, hit)
            packed = bytearray((header[5] + 7) // 8)
            for hit in hits:
                packed[hit >> 3] |= 1 << (hit & 7)
            packed_rows.append(packed)
            result_hash = mix(mix(result_hash, observable), weight & MASK)
            refs.append(dict(shot_index=index, detection_events=size, observable_mask=observable, integer_weight=weight))
        consumed = source.tell()
    source_header = list(header)
    header[4], header[6], header[7], header[8] = count, events, syndrome_hash, result_hash
    dataset = destination / "dataset.bin"
    with dataset.open("xb") as target:
        target.write(struct.pack("<12Q", *header))
        for raw in records:
            target.write(raw)
    with (destination / "reference.csv").open("x", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=list(refs[0]))
        writer.writeheader()
        writer.writerows(refs)
    with (destination / "shots.b8").open("xb") as target:
        for packed in packed_rows:
            target.write(packed)
    copied = {}
    for kind, filename in [("model", "model.dem"), ("circuit", "circuit.stim")]:
        source = frozen(case["inputs"][kind])
        shutil.copy2(source, destination / filename)
        copied[kind] = dict(path=filename, sha256=sha(destination / filename))
    receipt = dict(complete=True, shots=count, distance=case["case"]["d"], error_rate=case["case"]["p"],
                   root_seed=case["case"]["root_seed"], stream_id=case["case"]["stream_id"],
                   dataset=dict(path="dataset.bin", sha256=sha(dataset)),
                   reference=dict(path="reference.csv", sha256=sha(destination / "reference.csv")), **copied,
                   cli_input=dict(path="shots.b8", sha256=sha(destination / "shots.b8")),
                   original=dict(path=str(original), declared_full_sha256=case["inputs"]["original"]["sha256"],
                       full_hash_revalidated=False, consumed_bytes=consumed, consumed_prefix_sha256=source_digest.hexdigest(),
                       header=source_header),
                   reference_source=dict(path=str(reference), sha256=case["inputs"]["reference"]["sha256"]),
                   transformation="Same first records in the same order; only shot count and aggregate header checksums updated. Model/circuit unchanged.")
    save(destination / "complete.json", receipt)
    return receipt


def prepare(args):
    source_plan = Path(args.legacy_plan).resolve()
    plan = load(source_plan)
    need(plan.get("formal_shots") == 100000, "Use the completed 1103 100k paired plan")
    need(0 < args.shots <= 100000, "Invalid regression prefix length")
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    shutil.copy2(source_plan, output / "source-plan.json")
    baselines = {}
    for key in sorted(set(BASELINES.values())):
        build = plan["builds"][key]
        binary, receipt = frozen(build["binary"]), frozen(build["receipt"])
        destination = output / "baselines" / key
        destination.mkdir(parents=True)
        shutil.copy2(binary, destination / "benchmark")
        shutil.copy2(receipt, destination / "source-build.json")
        baselines[key] = dict(path=str((destination / "benchmark").relative_to(output)), sha256=sha(binary),
                              source_commit=build["source_commit"], original_binary=build["binary"], original_receipt=build["receipt"])
    cases = []
    inputs = output / "inputs"
    inputs.mkdir()
    for distance, probability in [(9, .0005), (33, .003)]:
        matches = [x for x in plan["cases"] if x["family"] == "predecode" and x["case"]["d"] == distance and x["case"]["p"] == probability]
        need(len(matches) == 1, "Missing/ambiguous frozen input case")
        case = matches[0]
        frozen(case["input_manifest"])
        name = case["case"]["id"]
        receipt = prefix(case, inputs / name, args.shots)
        cases.append(dict(id=name, manifest=str((inputs / name / "complete.json").relative_to(output)),
                          manifest_sha256=sha(inputs / name / "complete.json"), shots=receipt["shots"]))
    save(output / "complete.json", dict(complete=True, kind="decoder-core-differential-input", cases=cases, baselines=baselines,
         source_plan=dict(path=str(source_plan), sha256=sha(source_plan)), baseline_methods=BASELINES,
         gridsimd="New shared-core predecode-disabled implementation; old official ELF is its integer optimum/observable oracle.",
         scope="Frozen two-case correctness prefixes and archived executable copies; historical large datasets unchanged."))
    print(output / "complete.json")


def cpu_map(requested):
    allowed = sorted(os.sched_getaffinity(0))
    def topology(cpu):
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}")
        return int((path / "topology/physical_package_id").read_text()), int((path / "topology/core_id").read_text())
    if requested:
        result = [int(x) for x in requested.split(",")]
    else:
        groups = {}
        for cpu in allowed:
            socket, core = topology(cpu)
            nodes = tuple(x.name for x in Path(f"/sys/devices/system/cpu/cpu{cpu}").glob("node[0-9]*"))
            groups.setdefault((socket, nodes), {}).setdefault(core, cpu)
        result = next((list(cores.values())[:8] for _, cores in sorted(groups.items()) if len(cores) >= 8), [])
    need(len(result) >= 8 and len(set(result)) == len(result) and set(result).issubset(allowed), "Need eight distinct allowed physical CPUs")
    need(len({topology(cpu) for cpu in result}) == len(result), "CPU map contains SMT siblings")
    return result[:8]


def environment(strategy):
    env = {key: value for key, value in os.environ.items() if not key.startswith("BB_") and key not in ["OMP_PLACES", "GOMP_CPU_AFFINITY"]}
    env.update(OMP_NUM_THREADS="1", OMP_DYNAMIC="FALSE", OMP_PROC_BIND="FALSE", OMP_WAIT_POLICY="PASSIVE",
               GOMP_SPINCOUNT="0", OMP_THREAD_LIMIT="8", OMP_MAX_ACTIVE_LEVELS="1", OPENBLAS_NUM_THREADS="1",
               MKL_NUM_THREADS="1", NUMEXPR_NUM_THREADS="1", LD_PRELOAD="")
    if strategy == "spatial":
        env.update(BB_FIXED_SEED_CONSTRUCTION="fused", BB_FIXED_SEED_INPUT="indexed",
                   BB_IMPORT_PREPARATION="captained_serial", BB_IMPORT_RESET_OWNER="captain")
    return env


def invoke(argv, cpus, strategy, log, timeout):
    with log.open("x") as target:
        result = subprocess.run([str(x) for x in argv], stdout=target, stderr=subprocess.STDOUT,
                                env=environment(strategy), timeout=timeout,
                                preexec_fn=lambda: os.sched_setaffinity(0, set(cpus)))
    save(log.with_suffix(".json"), dict(argv=[str(x) for x in argv], cpus=cpus, returncode=result.returncode))
    need(result.returncode == 0, "Native regression failed: " + str(log))


def old_flags(strategy):
    execution = "shared" if strategy == "component" else "pipeline" if strategy in ["tailreset", "spatial"] else "serial"
    mode = dict(serial="batch", shared="full-batch", pipeline="pipe-batch")[execution]
    flags = ["--mode", mode, "--backend", "generic" if strategy == "upstream" else "avx512vl", "--execution", execution]
    if execution != "serial":
        spatial = strategy == "spatial"
        values = dict(partitions=2 if spatial else 1, component_parallelism=2, component_threshold=0 if spatial else 64,
                      shot_threshold=32, parallel_import=int(spatial), prefix_schedule="component_group_owned" if spatial else "component_sequential",
                      conflict_geometry="three_planes" if spatial else "symmetric_width", conflict_width=0 if spatial else 3,
                      continuation_placement="component_captain")
        if execution == "pipeline":
            values["pipeline_schedule"] = "overlap"
        flags += [x for key, value in values.items() for x in ["--" + key.replace("_", "-"), str(value)]]
    return flags


def rows(path, count):
    with path.open(newline="") as source:
        result = list(csv.DictReader(source))
    need(len(result) == count and all(int(row["shot_index"]) == k for k, row in enumerate(result)), "Wrong result count or order: " + str(path))
    return [(int(row["detection_events"] if "detection_events" in row else row["input_events"]),
             int(row["integer_weight"]), int(row["observable_mask"])) for row in result]


def cli_check(binary, strategy, identity, directory, destination, lane, actual, timeout):
    predictions = destination / ("cli-" + strategy + ".01")
    log = destination / ("cli-" + strategy + ".log")
    argv = [binary, "predict", "--decoder-strategy", strategy, "--decoder-info", "--dem", directory / "model.dem",
            "--in", directory / "shots.b8", "--in_format", "b8", "--out", predictions, "--out_format", "01"]
    if strategy != "upstream":
        argv += ["--grid-distance", str(identity["distance"]), "--grid-rounds", str(identity["distance"]),
                 "--grid-geometry", "rotated", "--grid-simd", "avx512vl"]
    if THREADS[strategy] > 1:
        argv += ["--decoder-cpus", ",".join(map(str, lane))]
    invoke(argv, lane, strategy, log, timeout)
    lines = predictions.read_text().splitlines()
    need(len(lines) == len(actual), "CLI prediction count/order differs")
    for line, result in zip(lines, actual):
        need(set(line).issubset({"0", "1"}) and line, "Invalid CLI prediction format")
        mask = sum(int(bit) << index for index, bit in enumerate(line))
        need(mask == result[2], "CLI observable prediction differs from the weighted public API")
    metadata = [json.loads(line) for line in log.read_text().splitlines() if line.startswith('{"decoder_strategy":')]
    need(len(metadata) == 1 and metadata[0]["decoder_strategy"] == strategy
         and metadata[0]["actual_threads"] == THREADS[strategy], "CLI actual decoder metadata differs")
    info = metadata[0]
    need(len(info["threads"]) == len(lane) and {tuple(t["affinity"]) for t in info["threads"]} == {(cpu,) for cpu in lane},
         "CLI actual CPU affinity differs")
    if strategy != "upstream":
        need(info["selected_simd"] and set(info["selected_simd"]) == {"avx512vl"}, "CLI actual ISA differs")
    return dict(complete=True, metadata=info, predictions_sha256=sha(predictions),
                scope="Formal predict stream returned with the StrategyDecoder session's final drain; observable predictions only. Integer weights are checked through the public C++ API.")


def run(args):
    manifest_path = Path(args.input).resolve()
    manifest = load(manifest_path)
    need(manifest.get("complete") and manifest.get("kind") == "decoder-core-differential-input", "Invalid regression manifest")
    base = manifest_path.parent
    executable = Path(args.new_binary).resolve()
    binary_sha = sha(executable)
    cli_binary = Path(args.new_cli).resolve() if args.new_cli else None
    cli_sha = sha(cli_binary) if cli_binary else None
    cpus = cpu_map(args.cpus)
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    comparisons = []
    for case in manifest["cases"]:
        case_manifest = base / case["manifest"]
        need(sha(case_manifest) == case["manifest_sha256"], "Input manifest changed")
        identity = load(case_manifest)
        directory = case_manifest.parent
        for kind in ["dataset", "reference", "model", "circuit", "cli_input"]:
            need(sha(directory / identity[kind]["path"]) == identity[kind]["sha256"], "Input file changed")
        destination = output / case["id"]
        destination.mkdir()
        expected = rows(directory / "reference.csv", identity["shots"])
        old = {}
        for strategy, key in BASELINES.items():
            reference = manifest["baselines"][key]
            binary = base / reference["path"]
            need(sha(binary) == reference["sha256"], "Archived executable changed")
            native_output = destination / ("old-" + strategy)
            native_output.mkdir()
            lane = cpus[:THREADS[strategy]]
            values = dict(distance=identity["distance"], error_rate=identity["error_rate"], shots=identity["shots"],
                          root_seed=identity["root_seed"], stream_id=identity["stream_id"], block_size=1024, repeats=1,
                          verify="weight", witness_all=0, replay_kind="none", model_source="frozen", dataset=directory / "dataset.bin",
                          warmup_seconds=0, main_cpu=lane[0], cpus=",".join(map(str, lane)), output_dir=native_output)
            argv = [binary] + [item for key, value in values.items() for item in ["--" + key.replace("_", "-"), str(value)]] + old_flags(strategy)
            invoke(argv, lane, strategy, destination / ("old-" + strategy + ".log"), args.timeout)
            correctness = load(native_output / "correctness.json")
            need(correctness["status"] == "passed" and correctness["weight_differences"] == 0, "Old ELF optimum validation failed")
            old[strategy] = rows(native_output / "native_shots.csv", identity["shots"])
            need([x[:2] for x in old[strategy]] == [x[:2] for x in expected], "Old ELF input/weight differs from frozen reference")
        for strategy, count in THREADS.items():
            native_output = destination / ("new-" + strategy)
            lane = cpus[:count]
            invoke([executable, "--strategy", strategy, "--dataset", directory / "dataset.bin", "--model", directory / "model.dem",
                    "--output", native_output, "--cpus", ",".join(map(str, lane))], lane, strategy,
                   destination / ("new-" + strategy + ".log"), args.timeout)
            completion = load(native_output / "complete.json")
            need(completion["complete"] and completion["shots"] == identity["shots"]
                 and completion["metadata"]["actual_threads"] == count
                 and completion["explicit_stream_drain_returned"] and completion["empty_batch_returned"]
                 and completion["post_drain_reuse_equal"], "New public API lifecycle receipt differs")
            actual = rows(native_output / "shots.csv", identity["shots"])
            oracle = old["upstream" if strategy == "gridsimd" else strategy]
            weight_differences = sum(a[:2] != b[:2] for a, b in zip(actual, oracle))
            observable_differences = sum(a[2] != b[2] for a, b in zip(actual, oracle))
            comparison = dict(case=case["id"], strategy=strategy, shots=identity["shots"], weight_differences=weight_differences,
                              observable_differences=observable_differences, actual_threads=count,
                              oracle="archived official ELF" if strategy == "gridsimd" else "archived same-strategy ELF",
                              lifecycle=completion, new_raw_sha256=sha(native_output / "shots.csv"))
            if cli_binary:
                comparison["cli"] = cli_check(cli_binary, strategy, identity, directory, destination, lane, actual, args.timeout)
            comparisons.append(comparison)
            save(destination / (strategy + "-comparison.json"), comparison)
            need(weight_differences == 0, "Old/new integer optimum or input order differs")
            need(observable_differences == 0, "Observable differences retained; inspect for an equal-weight tie and an actual correction witness before acceptance")
    need(sha(executable) == binary_sha, "New executable changed during validation")
    need(cli_binary is None or sha(cli_binary) == cli_sha, "New CLI changed during validation")
    save(output / "complete.json", dict(complete=True, input=dict(path=str(manifest_path), sha256=sha(manifest_path)),
         new_binary=dict(path=str(executable), sha256=binary_sha), cpus=cpus, comparisons=comparisons,
         new_cli=dict(path=str(cli_binary), sha256=cli_sha) if cli_binary else None,
         scope="Two frozen prefixes; old/new integer weights, observable masks, order, actual threads, explicit drain, empty batch and reuse. No performance acceptance."))
    print(output / "complete.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    freeze = commands.add_parser("prepare")
    freeze.add_argument("--legacy-plan", required=True, type=Path)
    freeze.add_argument("--output", required=True, type=Path)
    freeze.add_argument("--shots", type=int, default=1025)
    check = commands.add_parser("run")
    check.add_argument("--input", required=True, type=Path)
    check.add_argument("--new-binary", required=True, type=Path)
    check.add_argument("--new-cli", type=Path, help="Also verify the formal beyond_blossom predict entry for all six methods")
    check.add_argument("--output", required=True, type=Path)
    check.add_argument("--cpus")
    check.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    (prepare if args.command == "prepare" else run)(args)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, StopIteration, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
