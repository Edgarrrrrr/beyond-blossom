# Independent structured regression audit

This audit covers the sealed 819-call run: 21 saved-input cases, six candidate methods, three repeats, adjacent source/candidate comparisons, and one official-reference call per case/repeat.

`audit.json` retains validation results and all 126 comparisons. `case_method_comparisons.csv` includes both named baseline ratios, absolute batch means, and observed repeat ranges. `all_positive_median_latency_changes.csv` retains every positive median latency change against source revision `241160314087e5ae698c6e76c3c055a11c3ad422`; none are omitted because they are small. `method_summary.csv` provides equal-weight geometric means across settings alongside minima, maxima, and regression counts.

The primary speedup is fixed official reference time divided by candidate time. The separate migration speedup is source241 time divided by candidate time. Values below one indicate a slowdown against that named baseline. Three repeated batch means do not establish shot-latency percentiles or confidence intervals; overlapping ranges do not prove equivalence.

Recompute from the complete sealed run, choosing a new output directory:

```bash
python3 audit_structured_final.py --run /path/to/structured-final-run --output /tmp/new-structured-audit
```

The script refuses partial runs, verifies recorded plan/raw/binary hashes and native correctness/warmup receipts, checks the full measurement grid, and independently recomputes the collector summary. Absolute collection paths in `audit.json` are provenance records.
