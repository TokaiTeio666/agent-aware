# External ANN Checkouts

Place optional same-machine comparison checkouts here:

- `third_party/DiskANN/`
- `third_party/FreshDiskANN/`

The repository does not vendor or download external source automatically.
Use `scripts/linux/build_diskann.sh` after placing a DiskANN checkout, then set
`DISKANN_RUN_COMMAND` or `FRESHDISKANN_RUN_COMMAND` when invoking the matching
comparison wrapper.

