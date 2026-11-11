# Copyright 2026 Beyond Blossom Contributors
# Licensed under the Apache License, Version 2.0.
"""Explicit geometry and construction-bound strategies for surface-code decoding."""

from __future__ import annotations

import numpy as np

from beyond_blossom._cpp_beyond_blossom import _StructuredDecoder


class StructuredDecoder:
    """Decode a surface-code detector error model using its regular geometry.

    ``strategy`` is ``structured`` (without predecode), ``predecoded`` (the
    default), ``component-parallel``, ``pipelined``, or ``parallel-predecode``.
    Parallel strategies require an explicit ``cpus`` sequence of respectively
    2, 4, or 8 CPU IDs. On macOS these IDs designate distinct logical slots.
    ``same_algorithm_single_thread=True`` runs a parallel algorithm on one CPU.

    Models must contain detector coordinates compatible with ``geometry``,
    ``distance``, and ``rounds``. Use :class:`beyond_blossom.Matching` for arbitrary
    graphs. Each decoder supports one caller at a time. ``decode_batch`` creates
    one worker team for the entire batch and releases the Python GIL.
    """

    def __init__(
        self, model, *, distance: int, rounds: int, geometry: str = "rotated",
        strategy: str = "predecoded", simd: str = "auto", cpus=(),
        same_algorithm_single_thread: bool = False,
        include_search_graph: bool = False,
    ):
        self._decoder = _StructuredDecoder(
            str(model), distance, rounds, geometry, strategy, simd, list(cpus),
            same_algorithm_single_thread, include_search_graph,
        )
        info = self._decoder.metadata
        self._num_detectors = info["num_detectors"]
        self._num_observables = info["num_observables"]

    @classmethod
    def from_detector_error_model(cls, model, **options) -> StructuredDecoder:
        """Construct from a Stim DetectorErrorModel or its textual representation."""
        return cls(model, **options)

    @property
    def num_detectors(self) -> int:
        return self._num_detectors

    @property
    def num_observables(self) -> int:
        return self._num_observables

    @property
    def metadata(self) -> dict:
        """Return geometry, selected strategy/SIMD, capabilities, and team size."""
        return self._decoder.metadata

    @staticmethod
    def _bytes(values, *, packed: bool = False):
        array = np.asarray(values)
        if array.dtype.kind not in "biu":
            raise ValueError("Syndromes must contain integer or boolean values.")
        maximum = 255 if packed else 1
        if array.dtype != np.uint8 and array.dtype != np.bool_:
            if np.any(array < 0) or np.any(array > maximum):
                raise ValueError(f"Syndrome values must be between 0 and {maximum}.")
        return np.ascontiguousarray(array, dtype=np.uint8)

    def decode(self, syndrome, *, return_weight: bool = False):
        """Return observable bits for one dense detector syndrome.

        ``return_weight=True`` also returns the matching weight in the model's
        log-likelihood units. Integer weight quantization follows ``Matching``.
        """
        array = self._bytes(syndrome)
        if array.ndim != 1:
            raise ValueError("A syndrome must be a one-dimensional array.")
        predictions, weights = self._decoder.decode_batch(array.reshape(1, -1), False, False)
        return (predictions[0], weights[0]) if return_weight else predictions[0]

    def decode_batch(
        self, shots, *, bit_packed_shots: bool = False,
        bit_packed_predictions: bool = False, return_weights: bool = False,
    ):
        """Decode rows of syndromes, optionally using little-endian packed bytes.

        Unpacked input has shape ``(shots, num_detectors)``. Packed input has
        ``ceil(num_detectors / 8)`` columns; unused bits must be zero. Output
        columns similarly represent observables. An empty batch is supported.
        """
        array = self._bytes(shots, packed=bit_packed_shots)
        predictions, weights = self._decoder.decode_batch(
            array, bit_packed_shots, bit_packed_predictions,
        )
        return (predictions, weights) if return_weights else predictions

    def decode_detection_events(self, events, *, return_weight: bool = False):
        """Decode sparse detector indices instead of a dense syndrome."""
        predictions, weight = self._decoder.decode_sparse(events)
        return (predictions, weight) if return_weight else predictions

    def _events(self, syndrome):
        array = self._bytes(syndrome)
        if array.ndim != 1 or array.size != self.num_detectors:
            raise ValueError("A syndrome must have one entry per detector.")
        if np.any(array > 1):
            raise ValueError("Syndrome entries must be 0 or 1.")
        return np.flatnonzero(array).tolist()

    def decode_to_edges_array(self, syndrome):
        """Return correction edges, using -1 for boundary endpoints.

        Requires a serial strategy and ``include_search_graph=True`` when
        constructing this decoder. Capability flags are available in metadata.
        """
        return self._decoder.decode_edges(self._events(syndrome), False)

    def decode_to_matched_dets_array(self, syndrome):
        """Return matched detector pairs, using -1 for boundary endpoints."""
        return self._decoder.decode_edges(self._events(syndrome), True)
