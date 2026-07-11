import { NativeVectorStore } from './NativeVectorStore';

export async function openVectorStore(filesDir: string): Promise<NativeVectorStore> {
  const store = new NativeVectorStore();
  await store.open({
    baseIndexPath: `${filesDir}/index/base.idx`,
    pqModelPath: `${filesDir}/index/base.idx.pq`,
    dynamicDir: `${filesDir}/dynamic`,
  });
  return store;
}
