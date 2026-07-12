import agentAwareNative from 'libagentAwareNative.so';
import type {
  NativeEngineInfo,
  NativeEngineMetrics,
  NativeMergeStatus,
  NativeSearchResponse,
  ResourceProfile,
  VectorStoreOpenConfig,
  WriteMode,
} from './agentAwareNative';

export interface NativeVectorStoreContract {
  open(config: VectorStoreOpenConfig): Promise<NativeEngineInfo>;
  searchVector(vector: Float32Array, topK: number): Promise<NativeSearchResponse>;
  upsertVector(id: number, vector: Float32Array, mode: WriteMode): Promise<void>;
  deleteVector(id: number): Promise<void>;
  setResourceProfile(profile: ResourceProfile): Promise<void>;
  flush(): Promise<void>;
  triggerMerge(): Promise<void>;
  getMergeStatus(): Promise<NativeMergeStatus>;
  cancelMerge(): Promise<void>;
  getMetrics(): Promise<NativeEngineMetrics>;
  close(): Promise<void>;
}

export class NativeVectorStore implements NativeVectorStoreContract {
  open(config: VectorStoreOpenConfig): Promise<NativeEngineInfo> {
    return agentAwareNative.open({
      expectedDimension: 512,
      cachePolicy: 'graph-aware-2q',
      cachePages: 0,
      ioMode: 'thread_pool_pread',
      ioBatchSize: 1,
      ioDepth: 8,
      searchWidth: 64,
      beamWidth: 0,
      entryCount: 1,
      rerankTopK: 0,
      prefetchPolicy: 'pq',
      prefetchSubmitTopK: 4,
      prefetchMaxInflight: 8,
      ...config,
    });
  }

  searchVector(vector: Float32Array, topK: number): Promise<NativeSearchResponse> {
    return agentAwareNative.searchVector(vector, topK);
  }

  upsertVector(id: number, vector: Float32Array, mode: WriteMode): Promise<void> {
    return agentAwareNative.upsertVector(id, vector, mode);
  }

  deleteVector(id: number): Promise<void> {
    return agentAwareNative.deleteVector(id);
  }

  setResourceProfile(profile: ResourceProfile): Promise<void> {
    return agentAwareNative.setResourceProfile(profile);
  }

  flush(): Promise<void> {
    return agentAwareNative.flush();
  }

  triggerMerge(): Promise<void> {
    return agentAwareNative.triggerMerge();
  }

  getMergeStatus(): Promise<NativeMergeStatus> {
    return agentAwareNative.getMergeStatus();
  }

  cancelMerge(): Promise<void> {
    return agentAwareNative.cancelMerge();
  }

  getMetrics(): Promise<NativeEngineMetrics> {
    return agentAwareNative.getMetrics();
  }

  close(): Promise<void> {
    return agentAwareNative.close();
  }
}

// AgentMemoryService should receive this dependency through its constructor.
export type AgentMemoryNativeDependency = NativeVectorStoreContract;
