export type ResourceProfile = 'NORMAL' | 'CONSTRAINED' | 'CRITICAL';
export type WriteMode = 'INSERT' | 'UPDATE';

export interface VectorStoreOpenConfig {
  baseIndexPath: string;
  pqModelPath: string;
  dynamicDir: string;
  expectedDimension?: number;
  cachePolicy?: 'none' | 'lru' | '2q' | 'graph-aware-2q';
  cachePages?: number;
  ioMode?: 'thread_pool_pread' | 'pread';
  ioBatchSize?: number;
  ioDepth?: number;
  searchWidth?: number;
  beamWidth?: number;
  entryCount?: number;
  rerankTopK?: number;
  prefetchPolicy?: 'none' | 'pq';
  prefetchSubmitTopK?: number;
  prefetchMaxInflight?: number;
}

export interface NativeEngineInfo {
  apiVersion: '1.0';
  engineVersion: string;
  dimension: number;
  baseVectorCount: number;
  basePageCount: number;
  dynamicSequence: number;
  cachePolicy: string;
  cachePages: number;
  requestedIoMode: string;
  effectiveIoMode: string;
  ioFallbackReason?: string;
  pqEnabled: boolean;
  pqSubspaces: number;
  indexFormatVersion: number;
}

export interface NativeSearchStats {
  usedGraphPath: boolean;
  dynamicReadSequence: number;
  nodeReads: number;
  expanded: number;
  visited: number;
  pageRequests: number;
  cacheHits: number;
  demandReads: number;
  prefetchReads: number;
  prefetchUsefulPages: number;
  prefetchWastedPages: number;
  prefetchReadyHits: number;
  prefetchPendingHits: number;
  ioSubmits: number;
  ioCompletions: number;
  demandReadWaitUs: number;
  dynamicSnapshotMs: number;
  baseSearchMs: number;
  deltaSearchMs: number;
  mergeMs: number;
}

export interface NativeSearchResponse {
  hits: Array<{ id: number; distance: number }>;
  stats: NativeSearchStats;
}

export interface NativeEngineMetrics {
  requestedIoMode: string;
  effectiveIoMode: string;
  ioFallbackReason?: string;
  ioDepth: number;
  ioBatchSize: number;
  cacheCapacityPages: number;
  cacheResidentPages: number;
  memoryBudgetBytes: number;
  totalSearches: number;
  totalWrites: number;
  totalDeletes: number;
  totalFlushes: number;
  totalIoErrors: number;
  aggregate: Record<string, number>;
}

export interface MemHarmonyError extends Error {
  name: 'MemHarmonyError';
  code: string;
  operation: string;
  retryable: boolean;
  nativeCode?: number;
}

declare const agentAwareNative: {
  open(config: VectorStoreOpenConfig): Promise<NativeEngineInfo>;
  searchVector(vector: Float32Array, topK: number): Promise<NativeSearchResponse>;
  upsertVector(id: number, vector: Float32Array, mode: WriteMode): Promise<void>;
  deleteVector(id: number): Promise<void>;
  setResourceProfile(profile: ResourceProfile): Promise<void>;
  flush(): Promise<void>;
  getMetrics(): Promise<NativeEngineMetrics>;
  close(): Promise<void>;
};

export default agentAwareNative;
