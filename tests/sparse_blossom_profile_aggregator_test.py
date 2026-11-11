import argparse
import csv
import importlib
import importlib.util
import json
import subprocess
import re
import sqlite3
from pathlib import Path

import pytest


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "experiments"
    / "sparse-blossom-benchmarks"
    / "scripts"
    / "aggregate_sparse_blossom_profile_results.py"
)


def load_script_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


aggregator = load_script_module("sparse_blossom_profile_aggregator", SCRIPT_PATH)
merge_tasks = load_script_module(
    "sparse_blossom_profile_merge_tasks",
    SCRIPT_PATH.parent / "merge_sparse_blossom_profile_tasks.py",
)
repair_status = load_script_module(
    "sparse_blossom_profile_repair_status",
    SCRIPT_PATH.parent / "repair_sparse_blossom_task_status.py",
)
make_case_matrix = load_script_module(
    "sparse_blossom_profile_make_case_matrix",
    SCRIPT_PATH.parent / "make_sparse_blossom_profile_case_matrix.py",
)
profile_schema = importlib.import_module("sparse_blossom_profile.schema")
stats_figures = importlib.import_module("sparse_blossom_profile.stats_figures")
SUBMIT_SCRIPT = SCRIPT_PATH.parent / "submit_sparse_blossom_profile_array_run.sh"
INIT_SCRIPT = SCRIPT_PATH.parent / "init_sparse_blossom_profile_array_run.sh"


SETTINGS_HEADER = [
    "setting_id",
    "distance",
    "error_rate",
    "round_spec",
    "rounds",
    "shots",
    "num_detectors",
    "seed",
]
RUNS_HEADER = SETTINGS_HEADER + [
    "total_decode_us",
    "us_per_shot",
    "us_per_round",
    "mistakes",
    "total_detection_events",
    "logical_error_rate",
]
SHOT_MEASUREMENTS_HEADER = [
    "setting_id",
    "shot_index",
    "decode_us",
    "decode_us_per_round",
    "timing_repeats",
    "timing_warmups",
    "decode_us_first",
    "decode_us_min",
    "decode_us_mean",
    "decode_us_max",
    "detection_events",
    "mistake",
]


def write_csv(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=header)
        writer.writeheader()
        writer.writerows(rows)


def read_csv(path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def write_manifest(run_dir, **overrides):
    values = {
        "run_id": "run_1",
        "run_stats": "1",
        "run_baseline": "1",
        "run_nsys": "0",
        "trace_level_for_nsys": "event",
        "nsys_trace": "nvtx,osrt",
        "nsys_sample": "process-tree",
        "nsys_cpuctxsw": "process-tree",
        "nsys_capture_decode": "1",
    }
    values.update(overrides)
    run_dir.mkdir(parents=True, exist_ok=True)
    import json

    (run_dir / "manifest.json").write_text(json.dumps(values))


def base_setting(**overrides):
    row = {
        "setting_id": "0",
        "distance": "5",
        "error_rate": "0.01",
        "round_spec": "d",
        "rounds": "5",
        "shots": "10",
        "num_detectors": "24",
        "seed": "123",
    }
    row.update(overrides)
    return row


def base_run(**overrides):
    row = base_setting()
    row.update(
        {
            "total_decode_us": "20",
            "us_per_shot": "2",
            "us_per_round": "0.4",
            "mistakes": "1",
            "total_detection_events": "30",
            "logical_error_rate": "0.1",
        }
    )
    row.update(overrides)
    return row


def write_case_pass(run_dir, pass_name, settings=None, runs=None, shot_measurements=None):
    raw_dir = run_dir / "raw" / pass_name
    settings = [base_setting()] if settings is None else settings
    runs = [base_run()] if runs is None else runs
    write_csv(raw_dir / "settings.csv", SETTINGS_HEADER, settings)
    write_csv(raw_dir / "runs.csv", RUNS_HEADER, runs)
    if pass_name == "baseline":
        if shot_measurements is None:
            shot_measurements = [
                {
                    "setting_id": "0",
                    "shot_index": "0",
                    "decode_us": "1",
                    "decode_us_per_round": "0.2",
                    "detection_events": "2",
                    "mistake": "0",
                },
                {
                    "setting_id": "0",
                    "shot_index": "1",
                    "decode_us": "3",
                    "decode_us_per_round": "0.6",
                    "detection_events": "4",
                    "mistake": "1",
                },
            ]
        write_csv(raw_dir / "shot_measurements.csv", SHOT_MEASUREMENTS_HEADER, shot_measurements)
        write_csv(
            raw_dir / "tail_shots.csv",
            [
                "setting_id",
                "metric",
                "unit",
                "interval_label",
                "interval_start_k",
                "interval_end_k",
                "interval_start_cutoff",
                "interval_end_cutoff",
                "rank_in_interval",
                "shot_index",
                "setting_seed",
                "shot_generation_chunk_size",
                "global_chunk_start",
                "chunk_seed",
                "chunk_local_index",
                "decode_us",
                "decode_us_per_round",
                "timing_repeats",
                "timing_warmups",
                "decode_us_first",
                "decode_us_min",
                "decode_us_mean",
                "decode_us_max",
                "detection_events",
                "mistake",
                "obs_mask",
                "predicted_obs_mask",
                "detector_hits",
            ],
            [
                {
                    "setting_id": "0",
                    "metric": "decode_us_per_round",
                    "unit": "us/round",
                    "interval_label": "[1,0.1]",
                    "interval_start_k": "1",
                    "interval_end_k": "0.1",
                    "interval_start_cutoff": "0.4",
                    "interval_end_cutoff": "0.8",
                    "rank_in_interval": "1",
                    "shot_index": "1",
                    "setting_seed": "123",
                    "shot_generation_chunk_size": "1024",
                    "global_chunk_start": "0",
                    "chunk_seed": "456",
                    "chunk_local_index": "1",
                    "decode_us": "3",
                    "decode_us_per_round": "0.6",
                    "timing_repeats": "10",
                    "timing_warmups": "1",
                    "decode_us_first": "4",
                    "decode_us_min": "3",
                    "decode_us_mean": "3.5",
                    "decode_us_max": "4",
                    "detection_events": "4",
                    "mistake": "1",
                    "obs_mask": "1",
                    "predicted_obs_mask": "0",
                    "detector_hits": "2 7 9 11",
                }
            ],
        )
    if pass_name == "stats":
        write_csv(
            raw_dir / "counters.csv",
            ["setting_id", "scope", "metric", "value", "per_shot"],
            [
                {"setting_id": "0", "scope": "setting", "metric": "timeline_runs", "value": "3", "per_shot": "0.3"},
                {"setting_id": "0", "scope": "setting", "metric": "regions_created", "value": "12", "per_shot": "1.2"},
                {"setting_id": "0", "scope": "setting", "metric": "region_active_max", "value": "9", "per_shot": "0.9"},
                {"setting_id": "0", "scope": "setting", "metric": "alt_tree_nodes_active_max", "value": "4", "per_shot": "0.4"},
                {"setting_id": "0", "scope": "setting", "metric": "queue_max_total", "value": "7", "per_shot": "0.7"},
                {"setting_id": "0", "scope": "shot.0", "metric": "queue_max_total", "value": "999", "per_shot": "999"},
                {"setting_id": "0", "scope": "shot.0", "metric": "region_active_max", "value": "999", "per_shot": "999"},
            ],
        )
        write_csv(
            raw_dir / "aggregates.csv",
            ["setting_id", "metric", "normalization", "n", "mean", "median", "p90", "p95", "p99", "stddev", "min", "max", "cv"],
            [
                {
                    "setting_id": "0",
                    "metric": "decode_us",
                    "normalization": "per_shot",
                    "n": "10",
                    "mean": "2",
                    "median": "2",
                    "p90": "3",
                    "p95": "3",
                    "p99": "3",
                    "stddev": "0",
                    "min": "1",
                    "max": "3",
                    "cv": "0",
                }
            ],
        )
        write_csv(
            raw_dir / "histograms.csv",
            ["setting_id", "histogram", "bucket", "count"],
            [
                {"setting_id": "0", "histogram": "queue_total.setting", "bucket": "7", "count": "1"},
                {"setting_id": "0", "histogram": "queue_total.shot.0", "bucket": "99", "count": "100"},
                {"setting_id": "0", "histogram": "region_active.setting", "bucket": "0", "count": "1"},
                {"setting_id": "0", "histogram": "region_active.setting", "bucket": "9", "count": "1"},
                {"setting_id": "0", "histogram": "region_shell_area.setting", "bucket": "0", "count": "1"},
                {"setting_id": "0", "histogram": "region_shell_area.setting", "bucket": "1", "count": "2"},
                {"setting_id": "0", "histogram": "region_shell_area.setting", "bucket": "4", "count": "1"},
                {"setting_id": "0", "histogram": "region_shell_area.shot.0", "bucket": "99", "count": "100"},
                {"setting_id": "0", "histogram": "alt_tree_nodes_active.setting", "bucket": "0", "count": "1"},
                {"setting_id": "0", "histogram": "alt_tree_nodes_active.setting", "bucket": "4", "count": "1"},
                {"setting_id": "0", "histogram": "alt_tree_nodes_active_peak.setting", "bucket": "4", "count": "1"},
                {"setting_id": "0", "histogram": "alt_tree_nodes_active_peak.setting", "bucket": "5", "count": "1"},
                {"setting_id": "0", "histogram": "alt_tree_nodes_per_tree.setting", "bucket": "1", "count": "1"},
                {"setting_id": "0", "histogram": "alt_tree_nodes_per_tree.setting", "bucket": "3", "count": "1"},
            ],
        )
        write_csv(
            raw_dir / "histogram_quantiles.csv",
            ["setting_id", "histogram", "mean", "p50", "p90", "p99", "max", "total_count"],
            [
                {"setting_id": "0", "histogram": "queue_total", "mean": "7", "p50": "7", "p90": "7", "p99": "7", "max": "7", "total_count": "1"},
                {"setting_id": "0", "histogram": "region_active", "mean": "4.5", "p50": "0", "p90": "9", "p99": "9", "max": "9", "total_count": "2"},
                {"setting_id": "0", "histogram": "region_shell_area", "mean": "1.5", "p50": "1", "p90": "4", "p99": "4", "max": "4", "total_count": "4"},
                {"setting_id": "0", "histogram": "alt_tree_nodes_active", "mean": "2", "p50": "0", "p90": "4", "p99": "4", "max": "4", "total_count": "2"},
                {"setting_id": "0", "histogram": "alt_tree_nodes_active_peak", "mean": "4.5", "p50": "4", "p90": "5", "p99": "5", "max": "5", "total_count": "2"},
                {"setting_id": "0", "histogram": "alt_tree_nodes_per_tree", "mean": "2", "p50": "1", "p90": "3", "p99": "3", "max": "3", "total_count": "2"},
            ],
        )


def test_stats_and_baseline_join_into_performance_summary(tmp_path):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir)
    write_case_pass(run_dir, "stats")
    write_case_pass(
        run_dir,
        "baseline",
        settings=[base_setting(shots="20")],
        runs=[base_run(shots="20", total_decode_us="80", us_per_shot="4", us_per_round="0.8")],
    )

    aggregator.aggregate(run_dir)

    with (run_dir / "tables" / "performance_summary.csv").open(newline="") as f:
        reader = csv.DictReader(f)
        assert "setting_id" not in reader.fieldnames
        assert "stats_us_per_shot" not in reader.fieldnames
        assert "stats_us_per_round" not in reader.fieldnames
        assert "stats_over_baseline_us_per_shot" not in reader.fieldnames
        assert "stats_shots" not in reader.fieldnames
        assert "nsys_us_per_shot" not in reader.fieldnames
        assert "nsys_us_per_round" not in reader.fieldnames
        assert "baseline_mistakes" not in reader.fieldnames
        assert "baseline_logical_error_rate" not in reader.fieldnames
        assert "baseline_total_detection_events" not in reader.fieldnames
        rows = list(reader)
    assert len(rows) == 1
    row = rows[0]
    assert row["distance"] == "5"
    assert row["error_rate"] == "0.01"
    assert row["round_spec"] == "d"
    assert row["rounds"] == "5"
    assert row["baseline_shots"] == "20"
    assert row["baseline_us_per_round"] == "0.8"

    settings_rows = read_csv(run_dir / "tables" / "settings.csv")
    assert [row["pass"] for row in settings_rows] == ["stats", "baseline"]
    counter_rows = read_csv(run_dir / "tables" / "stats_counters.csv")
    assert counter_rows[0]["distance"] == "5"
    assert counter_rows[0]["metric"] == "timeline_runs"
    region_peak = [row for row in counter_rows if row["metric"] == "region_active_max"][0]
    assert region_peak["value"] == "9"
    assert region_peak["per_shot"] == "0.9"
    histogram_rows_for_stats = read_csv(run_dir / "tables" / "stats_histograms.csv")
    assert any(row["histogram"] == "region_shell_area.setting" for row in histogram_rows_for_stats)
    assert any(row["histogram"] == "region_shell_area.shot.0" for row in histogram_rows_for_stats)
    quantile_rows = read_csv(run_dir / "tables" / "stats_histogram_quantiles.csv")
    shell_quantile = [row for row in quantile_rows if row["histogram"] == "region_shell_area"][0]
    assert shell_quantile["total_count"] == "4"
    assert shell_quantile["p99"] == "4"

    readme = run_dir / "README.md"
    summary = run_dir / "summary.md"
    assert readme.exists()
    assert summary.exists()
    assert "从这里开始" in readme.read_text()
    summary_text = summary.read_text()
    assert "统计量说明" in summary_text
    assert "文件目录" in summary_text
    assert "stats_counters.csv" in summary_text
    assert "region_active_max` 和 `alt_tree_nodes_active_max` 是 raw peak count" in summary_text
    assert "region_shell_area` 记录 direct `shell_area.size()`" in summary_text
    assert "baseline/performance pass 是唯一性能 latency 来源" in summary_text
    assert "p_logical_ref=max(logical_error_rate, 1/shots)" in summary_text
    assert "alternating-tree 维护压力" in summary_text
    assert "理论上等于 `alt_tree_nodes_active_peak.max`" in summary_text
    assert "alt_tree_nodes_per_tree` 只在 root traversal 能稳定覆盖所有 active node" in summary_text
    assert "queue_max_total=7" in summary_text
    assert "queue_max_total=999" not in summary_text
    assert "stats/baseline" not in summary_text
    assert (run_dir / "figures" / "performance" / "baseline_decode_us_per_round_quantiles_rounds_d.png").exists()
    assert not (run_dir / "figures" / "performance" / "baseline_decode_us_per_round_quantiles_rounds_d.svg").exists()
    assert (run_dir / "figures" / "performance" / "latency_tail_ratio_p99_p50.png").exists()
    assert (run_dir / "figures" / "appendix" / "d5_p0p01_roundsd_latency_distribution.png").exists()
    assert (run_dir / "tables" / "baseline_shot_measurements.csv").exists()
    assert (run_dir / "tables" / "baseline_logical_error_rates.csv").exists()
    assert (run_dir / "tables" / "baseline_latency_histograms.csv").exists()
    assert (run_dir / "tables" / "baseline_latency_quantiles.csv").exists()
    assert (run_dir / "tables" / "baseline_latency_tail_cutoffs.csv").exists()
    assert (run_dir / "tables" / "selected_tail_shots.csv").exists()
    shot_rows = read_csv(run_dir / "tables" / "baseline_shot_measurements.csv")
    assert len(shot_rows) == 2
    assert shot_rows[0]["distance"] == "5"
    logical_rows = read_csv(run_dir / "tables" / "baseline_logical_error_rates.csv")
    assert len(logical_rows) == 1
    assert logical_rows[0]["shot_count"] == "2"
    assert logical_rows[0]["mistakes"] == "1"
    assert logical_rows[0]["logical_error_rate"] == "0.5"
    assert logical_rows[0]["raw_run_logical_error_rate"] == "0.1"
    cutoff_rows = read_csv(run_dir / "tables" / "baseline_latency_tail_cutoffs.csv")
    assert len(cutoff_rows) == 8
    assert {row["k"] for row in cutoff_rows} == {"0.1", "0.01", "1", "2"}
    assert {row["metric"] for row in cutoff_rows} == {"decode_us", "decode_us_per_round"}
    assert {row["unit"] for row in cutoff_rows} == {"us", "us/round"}
    assert {row["p_logical_ref"] for row in cutoff_rows} == {"0.5"}
    decode_cutoffs_by_k = {row["k"]: row for row in cutoff_rows if row["metric"] == "decode_us"}
    per_round_cutoffs_by_k = {row["k"]: row for row in cutoff_rows if row["metric"] == "decode_us_per_round"}
    assert decode_cutoffs_by_k["2"]["target_tail_probability"] == "1.0"
    assert decode_cutoffs_by_k["2"]["cutoff_value"] == "1.0"
    assert decode_cutoffs_by_k["1"]["cutoff_value"] == "2.737544264281482"
    assert per_round_cutoffs_by_k["2"]["cutoff_value"] == "0.20000000000000004"
    assert per_round_cutoffs_by_k["1"]["cutoff_value"] == "0.5475088528562964"
    interval_rows = read_csv(run_dir / "tables" / "tail_latency_case_intervals.csv")
    assert {row["interval_label"] for row in interval_rows} == {"[2,1]", "[1,0.1]", "[0.1,0.01]", "[0.01,inf]"}
    assert (run_dir / "tables" / "selected_tail_cases_k2_to_1.csv").exists()
    selected_shot_rows = read_csv(run_dir / "tables" / "selected_tail_shots.csv")
    assert selected_shot_rows == [
        {
            "run_id": "run_1",
            "distance": "5",
            "error_rate": "0.01",
            "round_spec": "d",
            "rounds": "5",
            "shots": "20",
            "seed": "123",
            "num_detectors": "24",
            "setting_id": "0",
            "metric": "decode_us_per_round",
            "unit": "us/round",
            "interval_label": "[1,0.1]",
            "interval_start_k": "1",
            "interval_end_k": "0.1",
            "interval_start_cutoff": "0.4",
            "interval_end_cutoff": "0.8",
            "rank_in_interval": "1",
            "shot_index": "1",
            "setting_seed": "123",
            "shot_generation_chunk_size": "1024",
            "global_chunk_start": "0",
            "chunk_seed": "456",
            "chunk_local_index": "1",
            "decode_us": "3",
            "decode_us_per_round": "0.6",
            "timing_repeats": "10",
            "timing_warmups": "1",
            "decode_us_first": "4",
            "decode_us_min": "3",
            "decode_us_mean": "3.5",
            "decode_us_max": "4",
            "detection_events": "4",
            "mistake": "1",
            "obs_mask": "1",
            "predicted_obs_mask": "0",
            "detector_hits": "2 7 9 11",
        }
    ]
    histogram_rows = read_csv(run_dir / "tables" / "baseline_latency_histograms.csv")
    decode_counts = [
        int(float(row["count"]))
        for row in histogram_rows
        if row["metric"] == "decode_us" and row["distance"] == "5"
    ]
    assert sum(decode_counts) == 2
    assert (run_dir / "figures" / "performance" / "baseline_logical_error_rate.png").exists()
    assert not (run_dir / "figures" / "performance" / "baseline_logical_error_rate.svg").exists()
    assert not (run_dir / "figures" / "performance" / "detection_events_per_shot.svg").exists()
    assert not (run_dir / "figures" / "performance" / "stats_us_per_round.svg").exists()
    assert not (run_dir / "figures" / "performance" / "nsys_us_per_round.svg").exists()
    assert not (run_dir / "figures" / "performance" / "stats_over_baseline.svg").exists()
    assert (run_dir / "figures" / "events" / "counter_family_region_phase.png").exists()
    assert (run_dir / "figures" / "events" / "region_active_max.png").exists()
    assert (run_dir / "figures" / "events" / "region_shell_area_p99.png").exists()
    assert (run_dir / "figures" / "primal" / "alt_tree_nodes_active_max.png").exists()
    assert (run_dir / "figures" / "primal" / "alt_tree_nodes_active_p99.png").exists()
    assert (run_dir / "figures" / "primal" / "alt_tree_nodes_active_peak_mean.png").exists()
    assert (run_dir / "figures" / "primal" / "alt_tree_nodes_active_peak_p99.png").exists()
    assert not (run_dir / "figures" / "primal" / "alt_tree_nodes_active_peak_max.png").exists()
    assert (run_dir / "figures" / "primal" / "alt_tree_nodes_per_tree_max.png").exists()
    assert (run_dir / "figures" / "primal" / "primal_tree_pressure_vs_distance_p.png").exists()
    assert (run_dir / "figures" / "appendix" / "d5_p0p01_roundsd_stats_histogram_atlas.png").exists()
    assert (run_dir / "figures" / "appendix" / "d5_p0p01_roundsd_cpu_decoder_latency_distribution.png").exists()

    file_roles = {row["role"] for row in read_csv(run_dir / "tables" / "files.csv")}
    assert {
        "readme",
        "summary",
        "figures_dir",
        "performance_summary",
        "baseline_shot_measurements",
        "baseline_latency_tail_cutoffs",
        "baseline_logical_error_rates",
        "selected_tail_shots",
        "replay_tail_shot_measurements",
        "tail_latency_case_intervals",
        "selected_tail_cases",
        "files",
    }.issubset(file_roles)


def test_replay_tail_shot_measurements_join_source_and_replay_latency(tmp_path):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir, replay_shots_csv="replay_tail_shots.csv")
    write_case_pass(run_dir, "stats")
    write_case_pass(
        run_dir,
        "baseline",
        settings=[base_setting(shots="1")],
        runs=[base_run(shots="1")],
        shot_measurements=[
            {
                "setting_id": "0",
                "shot_index": "1",
                "decode_us": "1",
                "decode_us_per_round": "0.2",
                "detection_events": "4",
                "mistake": "0",
            }
        ],
    )
    replay_row = {field: "" for field in profile_schema.SELECTED_TAIL_SHOTS_HEADER}
    replay_row.update(
        {
            "run_id": "source_run",
            "distance": "5",
            "error_rate": "0.01",
            "round_spec": "d",
            "rounds": "5",
            "shots": "100000000",
            "seed": "123",
            "num_detectors": "24",
            "setting_id": "0",
            "metric": "decode_us_per_round",
            "unit": "us/round",
            "interval_label": "[1,0.1]",
            "interval_start_k": "1",
            "interval_end_k": "0.1",
            "interval_start_cutoff": "0.4",
            "interval_end_cutoff": "0.8",
            "rank_in_interval": "1",
            "shot_index": "1",
            "setting_seed": "123",
            "shot_generation_chunk_size": "1024",
            "global_chunk_start": "0",
            "chunk_seed": "456",
            "chunk_local_index": "1",
            "decode_us": "3",
            "decode_us_per_round": "0.6",
            "detection_events": "4",
            "mistake": "1",
            "obs_mask": "1",
            "predicted_obs_mask": "0",
            "detector_hits": "2 7 9 11",
        }
    )
    write_csv(run_dir / "replay_tail_shots.csv", profile_schema.SELECTED_TAIL_SHOTS_HEADER, [replay_row])

    aggregator.aggregate(run_dir)

    rows = read_csv(run_dir / "tables" / "replay_tail_shot_measurements.csv")
    assert len(rows) == 1
    row = rows[0]
    assert row["source_run_id"] == "source_run"
    assert row["source_shots"] == "100000000"
    assert row["expected_replay_shots"] == "1"
    assert row["replay_shots"] == "1"
    assert row["validation_status"] == "ok"
    assert row["source_duplicate_count"] == "1"
    assert row["replay_match_count"] == "1"
    assert row["source_decode_us_per_round"] == "0.6"
    assert row["replay_decode_us_per_round"] == "0.2"
    assert row["source_metric_value"] == "0.6"
    assert row["replay_metric_value"] == "0.2"
    assert row["source_in_interval"] == "1"
    assert row["replay_above_interval_start_cutoff"] == "0"
    assert row["replay_below_interval_end_cutoff"] == "1"
    assert row["replay_in_interval"] == "0"
    assert float(row["replay_to_source_decode_us_per_round"]) == pytest.approx(1 / 3)
    assert float(row["replay_to_source_metric_value"]) == pytest.approx(1 / 3)


def test_replay_tail_shot_measurements_are_metric_aware_and_report_missing_rows(tmp_path):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir, replay_shots_csv="replay_tail_shots.csv")
    write_case_pass(run_dir, "stats")
    write_case_pass(
        run_dir,
        "baseline",
        settings=[base_setting(shots="2")],
        runs=[base_run(shots="2")],
        shot_measurements=[
            {
                "setting_id": "0",
                "shot_index": "1",
                "decode_us": "1",
                "decode_us_per_round": "0.2",
                "detection_events": "4",
                "mistake": "0",
            }
        ],
    )

    rows = []
    for shot_index in ["1", "2"]:
        replay_row = {field: "" for field in profile_schema.SELECTED_TAIL_SHOTS_HEADER}
        replay_row.update(
            {
                "run_id": "source_run",
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "100000000",
                "seed": "123",
                "num_detectors": "24",
                "setting_id": "0",
                "metric": "decode_us",
                "unit": "us",
                "interval_label": "[1,0.1]",
                "interval_start_k": "1",
                "interval_end_k": "0.1",
                "interval_start_cutoff": "2",
                "interval_end_cutoff": "4",
                "rank_in_interval": shot_index,
                "shot_index": shot_index,
                "setting_seed": "123",
                "shot_generation_chunk_size": "1024",
                "global_chunk_start": "0",
                "chunk_seed": "456",
                "chunk_local_index": shot_index,
                "decode_us": "3",
                "decode_us_per_round": "0.6",
                "detection_events": "4",
                "mistake": "1",
                "obs_mask": "1",
                "predicted_obs_mask": "0",
                "detector_hits": "2 7 9 11",
            }
        )
        rows.append(replay_row)
    write_csv(run_dir / "replay_tail_shots.csv", profile_schema.SELECTED_TAIL_SHOTS_HEADER, rows)

    aggregator.aggregate(run_dir)

    out = {row["shot_index"]: row for row in read_csv(run_dir / "tables" / "replay_tail_shot_measurements.csv")}
    assert out["1"]["validation_status"] == "ok"
    assert out["1"]["source_metric_value"] == "3"
    assert out["1"]["replay_metric_value"] == "1"
    assert out["1"]["source_in_interval"] == "1"
    assert out["1"]["replay_in_interval"] == "0"
    assert out["1"]["replay_to_source_metric_value"] == str(1 / 3)

    assert out["2"]["validation_status"] == "missing_replay_measurement"
    assert out["2"]["source_in_interval"] == "1"
    assert out["2"]["replay_metric_value"] == ""
    assert out["2"]["replay_in_interval"] == ""


def test_replay_tail_shot_measurements_flag_missing_finite_interval_end(tmp_path):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir, replay_shots_csv="replay_tail_shots.csv")
    write_case_pass(run_dir, "stats")
    write_case_pass(
        run_dir,
        "baseline",
        settings=[base_setting(shots="1")],
        runs=[base_run(shots="1")],
        shot_measurements=[
            {
                "setting_id": "0",
                "shot_index": "1",
                "decode_us": "3",
                "decode_us_per_round": "0.6",
                "detection_events": "4",
                "mistake": "0",
            }
        ],
    )
    replay_row = {field: "" for field in profile_schema.SELECTED_TAIL_SHOTS_HEADER}
    replay_row.update(
        {
            "run_id": "source_run",
            "distance": "5",
            "error_rate": "0.01",
            "round_spec": "d",
            "rounds": "5",
            "shots": "100000000",
            "metric": "decode_us_per_round",
            "unit": "us/round",
            "interval_label": "[1,0.1]",
            "interval_start_k": "1",
            "interval_end_k": "0.1",
            "interval_start_cutoff": "0.4",
            "interval_end_cutoff": "",
            "shot_index": "1",
            "decode_us": "3",
            "decode_us_per_round": "0.6",
            "detection_events": "4",
            "mistake": "1",
        }
    )
    write_csv(run_dir / "replay_tail_shots.csv", profile_schema.SELECTED_TAIL_SHOTS_HEADER, [replay_row])

    aggregator.aggregate(run_dir)

    row = read_csv(run_dir / "tables" / "replay_tail_shot_measurements.csv")[0]
    assert row["validation_status"] == "missing_interval_end_cutoff"
    assert row["source_above_interval_start_cutoff"] == "1"
    assert row["source_below_interval_end_cutoff"] == ""
    assert row["source_in_interval"] == ""
    assert row["replay_in_interval"] == ""


def test_missing_manifest_replay_shots_csv_fails(tmp_path):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir, replay_shots_csv="missing.csv")
    write_case_pass(run_dir, "stats")
    write_case_pass(run_dir, "baseline")

    with pytest.raises(aggregator.AggregationError, match="replay_shots_csv does not exist"):
        aggregator.aggregate(run_dir)


def test_stats_baseline_key_mismatch_reports_missing_pass_without_failing(tmp_path):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir)
    write_case_pass(run_dir, "stats")
    write_case_pass(
        run_dir,
        "baseline",
        settings=[base_setting(distance="7", rounds="7")],
        runs=[base_run(distance="7", rounds="7")],
    )

    aggregator.aggregate(run_dir)

    rows = read_csv(run_dir / "tables" / "performance_summary.csv")
    assert {row["distance"] for row in rows} == {"5", "7"}
    status_rows = read_csv(run_dir / "tables" / "case_status.csv")
    assert any(row["pass"] == "baseline" and row["distance"] == "5" and row["status"] == "missing" for row in status_rows)
    assert any(row["pass"] == "stats" and row["distance"] == "7" and row["status"] == "missing" for row in status_rows)


def test_ler_tail_cutoff_uses_one_over_shots_floor_for_zero_mistakes(tmp_path):
    run_dir = tmp_path / "run_zero_ler"
    write_manifest(run_dir, run_stats="0", run_nsys="0")
    write_case_pass(
        run_dir,
        "baseline",
        settings=[base_setting(shots="100")],
        runs=[base_run(shots="100", mistakes="0", logical_error_rate="0", total_decode_us="80")],
        shot_measurements=[],
    )
    write_csv(
        run_dir / "raw" / "baseline" / "latency_histograms.csv",
        ["setting_id", "metric", "bucket", "bucket_start", "bucket_end", "count", "density", "total_count"],
        [
            {"setting_id": "0", "metric": "decode_us_per_round", "bucket": "0", "bucket_start": "0.1", "bucket_end": "0.2", "count": "90", "density": "0.9", "total_count": "100"},
            {"setting_id": "0", "metric": "decode_us_per_round", "bucket": "1", "bucket_start": "0.2", "bucket_end": "0.3", "count": "10", "density": "0.1", "total_count": "100"},
            {"setting_id": "0", "metric": "decode_us", "bucket": "0", "bucket_start": "1", "bucket_end": "2", "count": "90", "density": "0.9", "total_count": "100"},
            {"setting_id": "0", "metric": "decode_us", "bucket": "1", "bucket_start": "2", "bucket_end": "3", "count": "10", "density": "0.1", "total_count": "100"},
        ],
    )
    write_csv(
        run_dir / "raw" / "baseline" / "latency_quantiles.csv",
        ["setting_id", "metric", "mean", "p50", "p90", "p95", "p99", "max", "total_count"],
        [
            {"setting_id": "0", "metric": "decode_us_per_round", "mean": "0.12", "p50": "0.1", "p90": "0.2", "p95": "0.2", "p99": "0.2", "max": "0.3", "total_count": "100"},
            {"setting_id": "0", "metric": "decode_us", "mean": "1.2", "p50": "1", "p90": "2", "p95": "2", "p99": "2", "max": "3", "total_count": "100"},
        ],
    )

    aggregator.aggregate(run_dir)

    cutoff_rows = read_csv(run_dir / "tables" / "baseline_latency_tail_cutoffs.csv")
    assert {row["logical_error_rate"] for row in cutoff_rows} == {"0.0"}
    assert {row["p_logical_ref"] for row in cutoff_rows} == {"0.01"}
    k2 = [row for row in cutoff_rows if row["metric"] == "decode_us_per_round" and row["k"] == "2"][0]
    assert k2["target_tail_probability"] == "0.02"
    assert k2["cutoff_value"] == "0.2"


def test_mixed_shot_and_histogram_baseline_keeps_all_tail_cases(tmp_path):
    run_dir = tmp_path / "run_mixed_baseline"
    write_manifest(run_dir, run_stats="0", run_nsys="0")
    write_case_pass(
        run_dir,
        "baseline",
        settings=[
            base_setting(setting_id="0", distance="5", shots="2"),
            base_setting(setting_id="1", distance="7", shots="100", num_detectors="48"),
        ],
        runs=[
            base_run(setting_id="0", distance="5", shots="2", mistakes="1", logical_error_rate="0.5"),
            base_run(setting_id="1", distance="7", shots="100", mistakes="25", logical_error_rate="0.25", total_decode_us="70"),
        ],
        shot_measurements=[
            {
                "setting_id": "0",
                "shot_index": "0",
                "decode_us": "1",
                "decode_us_per_round": "0.2",
                "detection_events": "2",
                "mistake": "0",
            },
            {
                "setting_id": "0",
                "shot_index": "1",
                "decode_us": "3",
                "decode_us_per_round": "0.6",
                "detection_events": "4",
                "mistake": "1",
            },
        ],
    )
    write_csv(
        run_dir / "raw" / "baseline" / "latency_histograms.csv",
        ["setting_id", "metric", "bucket", "bucket_start", "bucket_end", "count", "density", "total_count"],
        [
            {"setting_id": "1", "metric": "decode_us_per_round", "bucket": "0", "bucket_start": "0.1", "bucket_end": "0.2", "count": "80", "density": "0.8", "total_count": "100"},
            {"setting_id": "1", "metric": "decode_us_per_round", "bucket": "1", "bucket_start": "0.2", "bucket_end": "0.3", "count": "20", "density": "0.2", "total_count": "100"},
            {"setting_id": "1", "metric": "decode_us", "bucket": "0", "bucket_start": "1", "bucket_end": "2", "count": "80", "density": "0.8", "total_count": "100"},
            {"setting_id": "1", "metric": "decode_us", "bucket": "1", "bucket_start": "2", "bucket_end": "3", "count": "20", "density": "0.2", "total_count": "100"},
        ],
    )

    aggregator.aggregate(run_dir)

    logical_rows = read_csv(run_dir / "tables" / "baseline_logical_error_rates.csv")
    assert {(row["distance"], row["shot_count"], row["logical_error_rate"]) for row in logical_rows} == {
        ("5", "2", "0.5"),
        ("7", "100", "0.25"),
    }
    cutoff_rows = read_csv(run_dir / "tables" / "baseline_latency_tail_cutoffs.csv")
    assert {
        (row["distance"], row["metric"])
        for row in cutoff_rows
        if row["k"] == "1"
    } == {
        ("5", "decode_us"),
        ("5", "decode_us_per_round"),
        ("7", "decode_us"),
        ("7", "decode_us_per_round"),
    }
    interval_rows = read_csv(run_dir / "tables" / "tail_latency_case_intervals.csv")
    assert {"5", "7"}.issubset({row["distance"] for row in interval_rows})
    selected_rows = read_csv(run_dir / "tables" / "selected_tail_cases_k2_to_1.csv")
    assert {"5", "7"}.issubset({row["distance"] for row in selected_rows})
    assert (run_dir / "figures" / "appendix" / "d5_p0p01_roundsd_cpu_decoder_latency_distribution.png").exists()
    assert (run_dir / "figures" / "appendix" / "d7_p0p01_roundsd_cpu_decoder_latency_distribution.png").exists()


def test_selected_cases_csv_filters_case_matrix_by_interval_and_preserves_source_shots(tmp_path):
    selected_csv = tmp_path / "selected.csv"
    write_csv(
        selected_csv,
        profile_schema.SELECTED_TAIL_CASES_HEADER,
        [
            {
                "run_id": "source",
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "100000000",
                "seed": "42",
                "num_detectors": "24",
                "setting_id": "0",
                "metric": "decode_us_per_round",
                "unit": "us/round",
                "interval_label": "[1,0.1]",
                "interval_start_k": "1",
                "interval_end_k": "0.1",
                "interval_start_cutoff": "1",
                "interval_end_cutoff": "2",
                "logical_error_rate": "0.01",
                "p_logical_ref": "0.01",
                "interval_probability_mass": "0.1",
                "interval_count": "10",
                "interval_fraction": "0.1",
                "tail_pressure": "1",
                "rank_in_interval": "1",
                "selected": "1",
                "total_count": "100",
            },
            {
                "run_id": "source",
                "distance": "7",
                "error_rate": "0.003",
                "round_spec": "100",
                "rounds": "100",
                "shots": "100000000",
                "seed": "42",
                "num_detectors": "48",
                "setting_id": "1",
                "metric": "decode_us_per_round",
                "unit": "us/round",
                "interval_label": "[2,1]",
                "interval_start_k": "2",
                "interval_end_k": "1",
                "interval_start_cutoff": "1",
                "interval_end_cutoff": "2",
                "logical_error_rate": "0.01",
                "p_logical_ref": "0.01",
                "interval_probability_mass": "0.1",
                "interval_count": "10",
                "interval_fraction": "0.1",
                "tail_pressure": "2",
                "rank_in_interval": "1",
                "selected": "1",
                "total_count": "100",
            },
        ],
    )

    rows = make_case_matrix.build_rows(
        argparse.Namespace(
            passes="stats,baseline,nsys",
            rounds="d,100",
            distances="5,7",
            error_rates="0.01",
            full_cases="",
            stats_cases="",
            baseline_cases="",
            nsys_cases="",
            stats_shots="1000",
            baseline_shots="1000",
            nsys_shots="20",
            seed="42",
            selected_cases_csv=selected_csv,
            tail_interval="[1,0.1]",
            selected_cases_per_interval=10,
        )
    )

    assert [(row["pass"], row["distance"], row["round_spec"], row["shots"]) for row in rows] == [
        ("stats", "5", "d", "100000000"),
        ("baseline", "5", "d", "100000000"),
        ("nsys", "5", "d", "20"),
    ]


def test_selected_tail_shots_csv_builds_replay_case_matrix_with_selected_shot_counts(tmp_path):
    selected_csv = tmp_path / "selected_tail_shots.csv"

    def row(distance: str, shot_index: str, interval: str) -> dict[str, str]:
        result = {field: "" for field in profile_schema.SELECTED_TAIL_SHOTS_HEADER}
        result.update(
            {
                "run_id": "source",
                "distance": distance,
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": distance,
                "shots": "100000000",
                "seed": "42",
                "num_detectors": "24",
                "setting_id": "0",
                "metric": "decode_us_per_round",
                "unit": "us/round",
                "interval_label": interval,
                "rank_in_interval": shot_index,
                "shot_index": shot_index,
                "setting_seed": "123",
                "shot_generation_chunk_size": "1024",
                "global_chunk_start": "0",
                "chunk_seed": "456",
                "chunk_local_index": shot_index,
                "decode_us": "3",
                "decode_us_per_round": "0.6",
                "detection_events": "4",
                "mistake": "1",
                "obs_mask": "1",
                "predicted_obs_mask": "0",
                "detector_hits": "2 7 9 11",
            }
        )
        return result

    write_csv(
        selected_csv,
        profile_schema.SELECTED_TAIL_SHOTS_HEADER,
        [
            row("5", "1", "[1,0.1]"),
            row("5", "2", "[1,0.1]"),
            row("5", "4", "[2,1]"),
            row("7", "3", "[2,1]"),
        ],
    )

    rows = make_case_matrix.build_rows(
        argparse.Namespace(
            passes="stats,baseline,nsys",
            rounds="d,100",
            distances="5,7",
            error_rates="0.01",
            full_cases="",
            stats_cases="",
            baseline_cases="",
            nsys_cases="",
            stats_shots="1000",
            baseline_shots="1000",
            nsys_shots="20",
            seed="42",
            selected_cases_csv=None,
            selected_tail_shots_csv=selected_csv,
            tail_interval="[1,0.1]",
            selected_cases_per_interval=10,
        )
    )

    assert [(row["pass"], row["distance"], row["round_spec"], row["shots"]) for row in rows] == [
        ("stats", "5", "d", "2"),
        ("baseline", "5", "d", "2"),
        ("nsys", "5", "d", "2"),
    ]


def test_selected_cases_csv_without_matches_does_not_expand_full_grid(tmp_path):
    selected_csv = tmp_path / "selected.csv"
    write_csv(
        selected_csv,
        profile_schema.SELECTED_TAIL_CASES_HEADER,
        [
            {
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "100000000",
                "interval_label": "[2,1]",
                "rank_in_interval": "1",
                "selected": "1",
            },
        ],
    )

    with pytest.raises(ValueError, match="no selected cases matched"):
        make_case_matrix.build_rows(
            argparse.Namespace(
                passes="baseline",
                rounds="d,100",
                distances="5,7",
                error_rates="0.01",
                full_cases="",
                stats_cases="",
                baseline_cases="",
                nsys_cases="",
                stats_shots="1000",
                baseline_shots="1000",
                nsys_shots="20",
                seed="42",
                selected_cases_csv=selected_csv,
                tail_interval="[1,0.1]",
                selected_cases_per_interval=10,
            )
        )

    write_csv(
        selected_csv,
        profile_schema.SELECTED_TAIL_CASES_HEADER,
        [
            {
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "100000000",
                "interval_label": "[2,1]",
                "rank_in_interval": "1",
                "selected": "0",
            },
        ],
    )
    with pytest.raises(ValueError, match="no selected cases matched"):
        make_case_matrix.build_rows(
            argparse.Namespace(
                passes="baseline",
                rounds="d,100",
                distances="5,7",
                error_rates="0.01",
                full_cases="",
                stats_cases="",
                baseline_cases="",
                nsys_cases="",
                stats_shots="1000",
                baseline_shots="1000",
                nsys_shots="20",
                seed="42",
                selected_cases_csv=selected_csv,
                tail_interval="",
                selected_cases_per_interval=10,
            )
        )


def test_selected_cases_per_interval_limits_each_interval_independently(tmp_path):
    selected_csv = tmp_path / "tail_latency_case_intervals.csv"
    rows = []
    for distance, interval, rank, pressure in [
        ("5", "[2,1]", "1", "10"),
        ("7", "[2,1]", "2", "9"),
        ("11", "[1,0.1]", "1", "8"),
        ("13", "[1,0.1]", "2", "7"),
    ]:
        rows.append(
            {
                "distance": distance,
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": distance,
                "shots": "100000000",
                "interval_label": interval,
                "tail_pressure": pressure,
                "rank_in_interval": rank,
                "selected": "1",
            }
        )
    write_csv(selected_csv, profile_schema.SELECTED_TAIL_CASES_HEADER, rows)

    matrix_rows = make_case_matrix.build_rows(
        argparse.Namespace(
            passes="baseline",
            rounds="d,100",
            distances="5,7,11,13",
            error_rates="0.01",
            full_cases="",
            stats_cases="",
            baseline_cases="",
            nsys_cases="",
            stats_shots="1000",
            baseline_shots="1000",
            nsys_shots="20",
            seed="42",
            selected_cases_csv=selected_csv,
            tail_interval="",
            selected_cases_per_interval=1,
        )
    )

    assert [(row["distance"], row["shots"]) for row in matrix_rows] == [
        ("5", "100000000"),
        ("11", "100000000"),
    ]


def test_selected_rerun_source_interval_records_multiple_for_full_interval_table(tmp_path):
    selected_csv = tmp_path / "source_run" / "tables" / "tail_latency_case_intervals.csv"
    selected_csv.parent.mkdir(parents=True)
    write_manifest(selected_csv.parent.parent, run_id="source_run", tail_k_values="3,1,0.1")
    write_csv(
        selected_csv,
        profile_schema.SELECTED_TAIL_CASES_HEADER,
        [
            {
                "run_id": "source_run",
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "100000000",
                "interval_label": "[3,1]",
                "rank_in_interval": "1",
                "selected": "1",
            },
            {
                "run_id": "source_run",
                "distance": "7",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "7",
                "shots": "100000000",
                "interval_label": "[1,0.1]",
                "rank_in_interval": "1",
                "selected": "1",
            },
        ],
    )
    case_matrix = tmp_path / "case_matrix.csv"
    write_csv(
        case_matrix,
        ["task_index", "pass", "distance", "error_rate", "round_spec", "rounds", "shots", "seed", "case_label"],
        [
            {"task_index": "0", "pass": "baseline", "distance": "5", "error_rate": "0.01", "round_spec": "d", "rounds": "5", "shots": "100000000", "seed": "42", "case_label": "d5_p0p01_roundsd"},
            {"task_index": "1", "pass": "baseline", "distance": "7", "error_rate": "0.01", "round_spec": "d", "rounds": "7", "shots": "100000000", "seed": "42", "case_label": "d7_p0p01_roundsd"},
        ],
    )

    script_text = INIT_SCRIPT.read_text()
    start = script_text.index("import csv\nimport json")
    end = script_text.index("PY\n    )", start)
    meta_script = script_text[start:end]
    result = subprocess.run(
        ["python3", "-", str(selected_csv), str(case_matrix), ""],
        input=meta_script,
        text=True,
        capture_output=True,
        check=True,
    )

    assert "source_interval=multiple" in result.stdout
    assert "source_k_values=3,1,0.1" in result.stdout


def test_existing_run_restore_includes_custom_nsys_knobs(tmp_path):
    run_dir = tmp_path / "run_existing"
    write_csv(
        run_dir / "case_matrix.csv",
        ["task_index", "pass", "distance", "error_rate", "round_spec", "rounds", "shots", "seed", "case_label"],
        [
            {"task_index": "0", "pass": "nsys", "distance": "5", "error_rate": "0.01", "round_spec": "d", "rounds": "5", "shots": "20", "seed": "42", "case_label": "d5_p0p01_roundsd"},
        ],
    )
    manifest = {
        "run_id": "run_existing",
        "case_matrix": "case_matrix.csv",
        "nsys_trace": "nvtx,cuda,osrt",
        "nsys_reports": "nvtx_sum,osrt_sum",
        "nsys_spack_spec": "cuda@12.8.0",
        "nsys_sample": "cpu",
        "nsys_cpuctxsw": "none",
        "nsys_backtrace": "fp",
        "nsys_sampling_period": "777",
        "nsys_osrt_threshold": "222",
        "nsys_osrt_backtrace_threshold": "333",
        "nsys_nvtx_capture": "custom@domain",
        "nsys_capture_range_end": "repeat",
        "nsys_allow_unregistered_nvtx_capture": "1",
        "nsys_nvtx_profiler_register_only": "1",
    }
    (run_dir / "manifest.json").write_text(json.dumps(manifest))

    script_text = SUBMIT_SCRIPT.read_text()
    start = script_text.index("import json\nimport sys")
    end = script_text.index("PY\n    )", start)
    restore_script = script_text[start:end]
    result = subprocess.run(
        ["python3", "-", str(run_dir)],
        input=restore_script,
        text=True,
        capture_output=True,
        check=True,
    )

    restored = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    for key, expected in {
        "nsys_trace": "nvtx,cuda,osrt",
        "nsys_spack_spec": "cuda@12.8.0",
        "nsys_sample": "cpu",
        "nsys_cpuctxsw": "none",
        "nsys_backtrace": "fp",
        "nsys_sampling_period": "777",
        "nsys_osrt_threshold": "222",
        "nsys_osrt_backtrace_threshold": "333",
        "nsys_nvtx_capture": "custom@domain",
        "nsys_capture_range_end": "repeat",
        "nsys_allow_unregistered_nvtx_capture": "1",
        "nsys_nvtx_profiler_register_only": "1",
    }.items():
        assert restored[key] == expected


def test_duplicate_case_key_fails(tmp_path):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir, run_baseline="0")
    settings = [base_setting(setting_id="0"), base_setting(setting_id="1")]
    runs = [base_run(setting_id="0"), base_run(setting_id="1")]
    write_case_pass(run_dir, "stats", settings=settings, runs=runs)

    with pytest.raises(aggregator.AggregationError, match="duplicate case key"):
        aggregator.aggregate(run_dir)


def test_stats_histogram_quantile_mismatch_fails(tmp_path):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir, run_baseline="0")
    write_case_pass(run_dir, "stats")
    write_csv(
        run_dir / "raw" / "stats" / "histogram_quantiles.csv",
        ["setting_id", "histogram", "mean", "p50", "p90", "p99", "max", "total_count"],
        [
            {"setting_id": "0", "histogram": "queue_total", "mean": "7", "p50": "7", "p90": "7", "p99": "7", "max": "7", "total_count": "2"},
            {"setting_id": "0", "histogram": "region_active", "mean": "4.5", "p50": "0", "p90": "9", "p99": "9", "max": "9", "total_count": "2"},
            {"setting_id": "0", "histogram": "region_shell_area", "mean": "1.5", "p50": "1", "p90": "4", "p99": "4", "max": "4", "total_count": "4"},
        ],
    )

    with pytest.raises(aggregator.AggregationError, match="stats histogram quantile mismatch"):
        aggregator.aggregate(run_dir)


def write_nsys_fixture(run_dir, *, with_sqlite=True):
    case_label = "d5_p0p01_roundsd"
    case_dir = run_dir / "raw" / "nsys" / "d5_p0p01_roundsd"
    trace_dir = case_dir / "trace"
    write_csv(trace_dir / "settings.csv", SETTINGS_HEADER, [base_setting(shots="1")])
    write_csv(trace_dir / "runs.csv", RUNS_HEADER, [base_run(shots="1", total_decode_us="2", us_per_shot="2")])
    (case_dir / f"{case_label}.nsys-rep").parent.mkdir(parents=True, exist_ok=True)
    (case_dir / f"{case_label}.nsys-rep").write_text("")
    if with_sqlite:
        with sqlite3.connect(case_dir / f"{case_label}.sqlite") as connection:
            for table in ["SAMPLING_CALLCHAINS", "SCHED_EVENTS", "NVTX_EVENTS", "OSRT_API"]:
                connection.execute(f"CREATE TABLE {table}(id INTEGER)")
            connection.execute("INSERT INTO SAMPLING_CALLCHAINS VALUES (1)")
            connection.execute("INSERT INTO SCHED_EVENTS VALUES (1)")
            connection.execute("INSERT INTO SCHED_EVENTS VALUES (2)")
            connection.execute("INSERT INTO NVTX_EVENTS VALUES (1)")
            connection.commit()
    write_csv(
        case_dir / "stats_nvtx_sum.csv",
        ["Time (%)", "Total Time (ns)", "Instances", "Avg (ns)", "Med (ns)", "Min (ns)", "Max (ns)", "StdDev (ns)", "Style", "Range"],
        [
            {
                "Time (%)": "100",
                "Total Time (ns)": "2000",
                "Instances": "1",
                "Avg (ns)": "2000",
                "Med (ns)": "2000",
                "Min (ns)": "2000",
                "Max (ns)": "2000",
                "StdDev (ns)": "0",
                "Style": "PushPop",
                "Range": "pymatching.sparse_blossom:decode_profile",
            }
        ],
    )
    write_csv(case_dir / "stats_osrt_sum.csv", ["Time (%)", "Total Time (ns)", "Name"], [])
    write_csv(
        run_dir / "raw" / "nsys_index.csv",
        [
            "run_id",
            "case_label",
            "distance",
            "error_rate",
            "round_spec",
            "rounds",
            "shots",
            "trace_dir",
            "report_path",
            "sqlite_path",
            "nvtx_sum_csv",
            "osrt_sum_csv",
        ],
        [
            {
                "run_id": "run_1",
                "case_label": "d5_p0p01_roundsd",
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "1",
                "trace_dir": "raw/nsys/d5_p0p01_roundsd/trace",
                "report_path": f"raw/nsys/{case_label}/{case_label}.nsys-rep",
                "sqlite_path": f"raw/nsys/{case_label}/{case_label}.sqlite",
                "nvtx_sum_csv": "raw/nsys/d5_p0p01_roundsd/stats_nvtx_sum.csv",
                "osrt_sum_csv": "raw/nsys/d5_p0p01_roundsd/stats_osrt_sum.csv",
            }
        ],
    )


def test_nsys_reports_counts_nvtx_and_empty_osrt_summary(tmp_path):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir, run_stats="0", run_baseline="0", run_nsys="1")
    write_nsys_fixture(run_dir)

    aggregator.aggregate(run_dir)

    reports = read_csv(run_dir / "tables" / "nsys_reports.csv")
    assert reports[0]["sampling_callchains"] == "1"
    assert reports[0]["sched_events"] == "2"
    assert reports[0]["nvtx_events"] == "1"
    assert reports[0]["osrt_api_events"] == "0"

    nvtx_rows = read_csv(run_dir / "tables" / "nsys_nvtx_ranges.csv")
    assert nvtx_rows[0]["range"] == "pymatching.sparse_blossom:decode_profile"
    assert nvtx_rows[0]["instances"] == "1"
    assert "total_time_ns" not in nvtx_rows[0]

    with (run_dir / "tables" / "nsys_osrt_summary.csv").open(newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = list(reader)
    assert header == [
        "run_id",
        "distance",
        "error_rate",
        "round_spec",
        "rounds",
        "shots",
        "case_label",
        "osrt_summary_rows",
    ]
    assert "Time (%)" not in header
    assert "Total Time (ns)" not in header
    assert rows == [["run_1", "5", "0.01", "d", "5", "1", "d5_p0p01_roundsd", "0"]]


def test_missing_sqlite_warns_without_failing(tmp_path, capsys):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir, run_stats="0", run_baseline="0", run_nsys="1")
    write_nsys_fixture(run_dir, with_sqlite=False)

    aggregator.aggregate(run_dir)

    captured = capsys.readouterr()
    assert "missing nsys sqlite report" in captured.err
    reports = read_csv(run_dir / "tables" / "nsys_reports.csv")
    assert reports[0]["sampling_callchains"] == ""


def test_repair_missing_task_status_is_nonfatal(tmp_path, capsys):
    run_dir = tmp_path / "run_1"

    assert repair_status.main(["--run-dir", str(run_dir), "--task-id", "0"]) == 0

    captured = capsys.readouterr()
    assert "status file missing" in captured.out
    assert "skipped_missing=1" in captured.out


def test_repair_invalid_task_status_is_nonfatal(tmp_path, capsys):
    run_dir = tmp_path / "run_1"
    status_path = run_dir / "raw" / "tasks" / "task_0" / "case_status.csv"
    status_path.parent.mkdir(parents=True, exist_ok=True)
    status_path.write_text("")

    assert repair_status.main(["--run-dir", str(run_dir), "--task-id", "0"]) == 0

    captured = capsys.readouterr()
    assert "invalid status file" in captured.out
    assert "skipped_invalid=1" in captured.out


def test_enabled_but_all_missing_passes_still_aggregate_report(tmp_path):
    run_dir = tmp_path / "run_1"
    write_manifest(run_dir, run_baseline="1", run_stats="1", run_nsys="1")
    write_csv(
        run_dir / "raw" / "case_status.csv",
        merge_tasks.CASE_STATUS_HEADER,
        [
            {
                "run_id": "run_1",
                "pass": pass_name,
                "case_label": "d5_p0p01_roundsd",
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "10",
                "status": "missing",
                "message": f"{pass_name} output missing",
                "task_id": str(task_id),
                "path": "",
            }
            for task_id, pass_name in enumerate(["baseline", "stats", "nsys"])
        ],
    )

    aggregator.aggregate(run_dir)

    assert (run_dir / "README.md").exists()
    assert (run_dir / "summary.md").exists()
    assert (run_dir / "tables" / "case_status.csv").exists()
    assert read_csv(run_dir / "tables" / "performance_summary.csv") == []
    summary_text = (run_dir / "summary.md").read_text()
    assert "本 run 未包含 baseline/performance pass" in summary_text
    assert "本 run 未包含 stats pass" in summary_text
    assert "本 run 未包含 nsys pass" in summary_text


def test_merge_allow_partial_synthesizes_missing_task_status(tmp_path):
    run_dir = tmp_path / "run_1"
    write_csv(
        run_dir / "case_matrix.csv",
        ["task_index", "pass", "distance", "error_rate", "round_spec", "rounds", "shots", "seed", "case_label"],
        [
            {
                "task_index": "0",
                "pass": "baseline",
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "10",
                "seed": "42",
                "case_label": "d5_p0p01_roundsd",
            }
        ],
    )

    with pytest.raises(merge_tasks.MergeError, match="not successful"):
        merge_tasks.merge(run_dir, allow_partial=False)

    merge_tasks.merge(run_dir, force=True, allow_partial=True)

    rows = read_csv(run_dir / "raw" / "case_status.csv")
    assert rows == [
        {
            "run_id": "run_1",
            "pass": "baseline",
            "case_label": "d5_p0p01_roundsd",
            "distance": "5",
            "error_rate": "0.01",
            "round_spec": "d",
            "rounds": "5",
            "shots": "10",
            "status": "missing",
            "message": "case_status.csv missing",
            "task_id": "0",
            "path": "",
        }
    ]


def test_merge_allow_partial_downgrades_invalid_success_status(tmp_path, capsys):
    run_dir = tmp_path / "run_1"
    case_label = "d5_p0p01_roundsd"
    write_csv(
        run_dir / "case_matrix.csv",
        ["task_index", "pass", "distance", "error_rate", "round_spec", "rounds", "shots", "seed", "case_label"],
        [
            {
                "task_index": "0",
                "pass": "baseline",
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "10",
                "seed": "42",
                "case_label": case_label,
            }
        ],
    )
    task_output = run_dir / "raw" / "tasks" / "task_0" / "baseline"
    write_csv(task_output / "settings.csv", SETTINGS_HEADER, [base_setting()])
    write_csv(task_output / "runs.csv", RUNS_HEADER, [base_run()])
    write_csv(
        run_dir / "raw" / "tasks" / "task_0" / "case_status.csv",
        merge_tasks.CASE_STATUS_HEADER,
        [
            {
                "run_id": "run_1",
                "pass": "baseline",
                "case_label": case_label,
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "10",
                "status": "success",
                "message": "",
                "task_id": "0",
                "path": "raw/tasks/task_0/baseline",
            }
        ],
    )

    with pytest.raises(merge_tasks.MergeError, match="marked success but has invalid output"):
        merge_tasks.merge(run_dir, force=True, allow_partial=False)

    merge_tasks.merge(run_dir, force=True, allow_partial=True)

    captured = capsys.readouterr()
    assert "downgrading invalid success task 0 baseline" in captured.err
    rows = read_csv(run_dir / "raw" / "case_status.csv")
    assert rows[0]["status"] == "failed"
    assert "invalid success output" in rows[0]["message"]
    assert not (run_dir / "raw" / "baseline" / "settings.csv").exists()


def test_merge_rejects_success_output_with_mismatched_shots(tmp_path):
    run_dir = tmp_path / "run_1"
    case_label = "d5_p0p01_roundsd"
    write_csv(
        run_dir / "case_matrix.csv",
        ["task_index", "pass", "distance", "error_rate", "round_spec", "rounds", "shots", "seed", "case_label"],
        [
            {
                "task_index": "0",
                "pass": "nsys",
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "2",
                "seed": "42",
                "case_label": case_label,
            }
        ],
    )
    task_output = run_dir / "raw" / "tasks" / "task_0" / "nsys"
    write_csv(
        task_output / "nsys_index.csv",
        merge_tasks.NSYS_INDEX_HEADER,
        [
            {
                "run_id": "run_1",
                "case_label": case_label,
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "3",
                "trace_dir": "trace",
                "report_path": f"{case_label}.nsys-rep",
                "sqlite_path": f"{case_label}.sqlite",
                "nvtx_sum_csv": "stats_nvtx_sum.csv",
                "osrt_sum_csv": "stats_osrt_sum.csv",
            }
        ],
    )
    write_csv(
        run_dir / "raw" / "tasks" / "task_0" / "case_status.csv",
        merge_tasks.CASE_STATUS_HEADER,
        [
            {
                "run_id": "run_1",
                "pass": "nsys",
                "case_label": case_label,
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "2",
                "status": "success",
                "message": "",
                "task_id": "0",
                "path": "raw/tasks/task_0/nsys",
            }
        ],
    )

    with pytest.raises(merge_tasks.MergeError, match="shot count mismatch"):
        merge_tasks.merge(run_dir, force=True, allow_partial=False)


def test_merge_requires_tail_shots_when_tail_capture_enabled(tmp_path):
    run_dir = tmp_path / "run_1"
    case_label = "d5_p0p01_roundsd"
    write_manifest(run_dir, tail_shot_capture="on", tail_shots_per_interval=10)
    write_csv(
        run_dir / "case_matrix.csv",
        ["task_index", "pass", "distance", "error_rate", "round_spec", "rounds", "shots", "seed", "case_label"],
        [
            {
                "task_index": "0",
                "pass": "baseline",
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "10",
                "seed": "42",
                "case_label": case_label,
            }
        ],
    )
    task_output = run_dir / "raw" / "tasks" / "task_0" / "baseline"
    write_csv(task_output / "settings.csv", SETTINGS_HEADER, [base_setting()])
    write_csv(task_output / "runs.csv", RUNS_HEADER, [base_run()])
    write_csv(task_output / "latency_histograms.csv", merge_tasks.FALLBACK_HEADERS["latency_histograms"], [])
    write_csv(task_output / "latency_quantiles.csv", merge_tasks.FALLBACK_HEADERS["latency_quantiles"], [])
    write_csv(task_output / "latency_tail_cutoffs.csv", merge_tasks.FALLBACK_HEADERS["latency_tail_cutoffs"], [])
    write_csv(
        run_dir / "raw" / "tasks" / "task_0" / "case_status.csv",
        merge_tasks.CASE_STATUS_HEADER,
        [
            {
                "run_id": "run_1",
                "pass": "baseline",
                "case_label": case_label,
                "distance": "5",
                "error_rate": "0.01",
                "round_spec": "d",
                "rounds": "5",
                "shots": "10",
                "status": "success",
                "message": "",
                "task_id": "0",
                "path": "raw/tasks/task_0/baseline",
            }
        ],
    )

    with pytest.raises(merge_tasks.MergeError, match="tail_shots"):
        merge_tasks.merge(run_dir, force=True, allow_partial=False)


def test_stats_metric_explanations_cover_cpp_schema():
    schema_path = (
        Path(__file__).resolve().parents[1]
        / "src"
        / "pymatching"
        / "sparse_blossom"
        / "profile"
        / "profile_schema.h"
    )
    schema_text = schema_path.read_text()
    cpp_counters = set(re.findall(r'CounterField\{"([^"]+)"', schema_text))
    cpp_histograms = set(re.findall(r'HistogramField\{"([^"]+)"', schema_text))

    explained_counters = {row[0] for row in profile_schema.STATS_METRIC_DETAILS}
    explained_counters.update(
        f"{prefix}{suffix}"
        for prefix, *_ in profile_schema.QUEUE_COUNTER_GROUPS
        for suffix, _ in profile_schema.QUEUE_KIND_SUFFIXES
    )
    explained_counters.update(row[0] for row in profile_schema.QUEUE_MAX_DETAILS)
    explained_histograms = {row[0] for row in profile_schema.STATS_HISTOGRAM_DETAILS}

    assert cpp_counters == explained_counters
    assert cpp_histograms == explained_histograms
    assert cpp_histograms.issubset(stats_figures.STATS_HISTOGRAM_AXIS_LABELS)
    assert cpp_histograms.issubset(stats_figures.STATS_HISTOGRAM_Y_LABELS)
    assert "other" not in {stats_figures.metric_family(metric) for metric in cpp_counters}
