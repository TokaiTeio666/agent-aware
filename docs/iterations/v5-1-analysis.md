# V5.1 Analysis - WAL Replay

## Goal

V5.1 closes the crash-recovery gap in the V5 dynamic write path. Insert records
written to the WAL can now be replayed at startup and restored into the Delta
Index before queries run.

## Implementation

- Added WAL parser and replay API in `dynamic_index`.
- Added `--wal-replay` to replay `--wal` before a mixed workload.
- `WalWriter` now supports append mode after replay, so recovered WAL files are
  not truncated.
- Added replay metrics: `wal_replay_records`, `wal_replay_bytes`, and
  `wal_replay_delta_size`.
- Added smoke script: `scripts/run_v5_1_wal_replay.ps1`.

## Validation

Command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_v5_1_wal_replay.ps1
```

Observed replay run:

```text
wal_replay_records=32
wal_replay_bytes=4624
wal_replay_delta_size=32
wal_records=32
delta_active_size=32
recall_at_10=0.9975
```

## Status

Pass. WAL replay restores all insert records into delta and preserves high
dynamic Recall@10 on the replayed workload.
