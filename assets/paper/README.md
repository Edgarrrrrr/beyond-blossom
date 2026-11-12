# Paper illustrations

These images reproduce the method figures in *Beyond Blossom: Accelerating MWPM Decoding for Surface Codes With Local Predecoding and Architecture-Aware Optimizations*, by Jiayu Fu, Zirong Shen, Yiming Zhang, Wenjing Han, Lin Gan, Xiaoming Sun, Riling Li, and Guangwen Yang.

They are direct raster exports of the PDFs included by `sections/03-method.tex` in the supplied manuscript. The artwork, labels, and diagram contents are unchanged. These method illustrations contain no benchmark results.

| README image | Original manuscript PDF | Figure label |
| --- | --- | --- |
| [Parallel architecture](parallel-architecture.png) | [PDF](source/multilevel_parallelism.pdf) | `fig:multilevel-parallelism` |
| [One-hop predecoding](one-hop-predecoding.png) | [PDF](source/beyond_blossom_predecoder_v6.pdf) | `fig:predecode` |
| [Structured grid](structured-grid.png) | [PDF](source/structured_grid_stencil.pdf) | `fig:structured-grid` |

## Export

Each PNG is 2,400 pixels wide, preserves the PDF page's aspect ratio, and uses an opaque white background for consistent display on GitHub. Full-size images are linked from the main README. Raster exports keep the asset bundle small; the parallel architecture's direct SVG conversion alone was over 11 MB.

The original PDFs are included in `source/`. With Poppler's `pdftocairo` installed, regenerate the PNGs from the repository root:

```sh
pdftocairo -png -singlefile -scale-to-x 2400 -scale-to-y -1 \
  assets/paper/source/multilevel_parallelism.pdf assets/paper/parallel-architecture
pdftocairo -png -singlefile -scale-to-x 2400 -scale-to-y -1 \
  assets/paper/source/beyond_blossom_predecoder_v6.pdf assets/paper/one-hop-predecoding
pdftocairo -png -singlefile -scale-to-x 2400 -scale-to-y -1 \
  assets/paper/source/structured_grid_stencil.pdf assets/paper/structured-grid
```

[provenance.json](provenance.json) records the included source PDF hashes, exported image hashes and dimensions, and conversion tool version. No separate manuscript checkout is required.

## Attribution and rights

The illustrations are manuscript artwork by the authors listed above, reproduced from the supplied manuscript for the project README. No separate artwork license accompanied the supplied PDFs. These exports preserve that status and do not apply an additional license grant to the figures. The repository's [Apache-2.0 license](../../LICENSE) and [NOTICE](../../NOTICE) document the software's licensing and attribution.
