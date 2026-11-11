import sys
from pathlib import Path
from typing import Callable, List
from unittest.mock import patch

import pytest

import beyond_blossom
from beyond_blossom import cli
from beyond_blossom._cli_argv import cli_argv


def predict_args(dem_file: Path, input_file: Path, output_file: Path) -> List[str]:
    return [
        "predict",
        "--dem", str(dem_file),
        "--in", str(input_file),
        "--in_format", input_file.suffix[1:],
        "--out", str(output_file),
        "--out_format", "dets",
    ]


@pytest.mark.parametrize("cli_function", [cli, beyond_blossom._cpp_beyond_blossom.main])
@pytest.mark.parametrize("input_format", ["dets", "b8"])
def test_calling_cli_creates_expected_file(
    tmp_path: Path,
    data_dir: Path,
    cli_function: Callable[[List[str]], None],
    input_format: str
):
    output_file = tmp_path / "three_errors_predictions.dets"
    cli_function(command_line_args=predict_args(
        data_dir / "three_errors.dem",
        data_dir / f"three_errors.{input_format}",
        output_file))
    assert output_file.is_file()
    with open(output_file, encoding="utf-8") as prediction_file:
        assert prediction_file.readlines() == ["shot\n", "shot L0\n", "shot L2\n", "shot L1\n"]


@pytest.mark.parametrize("input_format", ["dets", "b8"])
def test_patching_cli_argv_creates_expected_file(
    tmp_path: Path,
    data_dir: Path,
    input_format: str
):
    output_file = tmp_path / "three_errors_predictions.dets"
    args = predict_args(
        data_dir / "three_errors.dem",
        data_dir / f"three_errors.{input_format}",
        output_file)
    with patch.object(sys, "argv", ["cli"] + args):
        cli_argv()
        assert output_file.is_file()
        with open(output_file) as prediction_file:
            assert prediction_file.readlines() == ["shot\n", "shot L0\n", "shot L2\n", "shot L1\n"]


def test_load_surface_code_b8_cli(tmp_path: Path, data_dir: Path):
    dem_path = data_dir / "surface_code_rotated_memory_x_13_0.01.dem"
    dets_b8_in_path = data_dir / "surface_code_rotated_memory_x_13_0.01_1000_shots.b8"
    out_fn = tmp_path / "surface_code_rotated_memory_x_13_0.01_1000_shots_temp_predictions.b8"

    beyond_blossom._cpp_beyond_blossom.main(command_line_args=[
        "predict",
        "--dem", str(dem_path),
        "--in", str(dets_b8_in_path),
        "--in_format", "b8",
        "--out", str(out_fn),
        "--out_format", "b8",
        "--in_includes_appended_observables"
    ])


def test_console_entry_returns_native_failure(tmp_path, capfd):
    with patch.object(sys, "argv", ["beyond-blossom", "predict", "--dem", str(tmp_path / "missing.dem")]):
        assert cli_argv() != 0
    assert "Failed to open" in capfd.readouterr().err


def test_console_unknown_command_has_no_traceback(capsys):
    with patch.object(sys, "argv", ["beyond-blossom", "unknown-command"]):
        assert cli_argv() == 2
    message = capsys.readouterr().err
    assert "Unrecognized command" in message
    assert "Traceback" not in message


def test_console_help_is_success(capfd):
    with patch.object(sys, "argv", ["beyond-blossom", "--help"]):
        assert cli_argv() == 0
    output = capfd.readouterr()
    assert "unstructured|structured|predecoded" in output.out
    assert not output.err
