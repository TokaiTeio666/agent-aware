# agent-aware Archive

This directory stores evidence that should be preserved across iterations.

- `archive/results/`: raw command outputs or summarized benchmark results.
- `archive/configs/`: JSON experiment configs, including commands, seeds,
  dataset paths, run type, and index parameters.
- `archive/logs/`: raw execution logs.
- `archive/build_info/`: compiler, OS, git, CPU, memory, and build metadata.
- `archive/validation/`: snapshots of the validation method used by reports.

Large temporary experiment outputs can go under the ignored top-level
`results/` directory. Curated results used in reports should be copied here.
