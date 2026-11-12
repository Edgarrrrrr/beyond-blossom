# Saved reproduction measurements

This directory contains the completed 21-setting Xeon reproduction from source
`75aedd433d1b129e5a973635320f199b5f26cbae`. Each native configuration and the
independently built PyMatching reference used 1,000,000 inputs per setting;
each external decoder used the same inputs' 10,000-shot prefix. Timing was
repeated three times. The seven predecoder settings used serial execution.

The external sample counts are a custom uniform policy: 54 of the 84 external
points in the paper use one million shots instead. The current AE defaults to
the manuscript's mixed counts. This archived run retains its actual counts and
is useful for checking the standard rendering workflow.

Redraw these measurements through the standard AE entry point:

```sh
python AE/replot.py --input AE/data/reproduction/canonical.json --output /path/to/new-output
```

The canonical measurements and numerical tables are sealed by `manifest.json`.
The renderer uses the paper's layouts and the recorded values. These are new
measurements; their numerical labels differ from the historical paper figures.
Original paths inside the records identify acquisition provenance and are not
read during redraw. No original input stream, cluster account, or custom script
is needed. Hardware, source, sample counts, and aggregation remain in the JSON
and CSV files instead of being added to the figure artwork.
