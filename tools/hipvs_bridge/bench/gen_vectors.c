/* Deterministic dataset generator: base/query f32, GT i32 (via GPU brute force), TSV for LOAD DATA. */
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/brute_force.h>
#include <cuvs/cuda_runtime.h>
#include <dlpack/dlpack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define CK(x) do { cuvsError_t e = (x); if (e != CUVS_SUCCESS) {   fprintf(stderr, "FAIL %s:%d %s: %s\n", __FILE__, __LINE__, #x, cuvsGetLastErrorText()); exit(1);} } while (0)
static DLManagedTensor tensor(void* data, long rows, long cols, int code, int bits, int dev) {
  static int64_t shape_pool[64][2]; static int idx = 0;
  DLManagedTensor t; memset(&t, 0, sizeof(t));
  int64_t* sh = shape_pool[idx++ & 63]; sh[0] = rows; sh[1] = cols;
  t.dl_tensor.data = data; t.dl_tensor.device.device_type = (DLDeviceType)dev; t.dl_tensor.device.device_id = 0;
  t.dl_tensor.ndim = 2; t.dl_tensor.dtype.code = code; t.dl_tensor.dtype.bits = bits; t.dl_tensor.dtype.lanes = 1;
  t.dl_tensor.shape = sh; t.dl_tensor.strides = NULL; return t;
}
static float* gen_data(long n, long dim, int clustered) {
  float* d = (float*)malloc((size_t)n * dim * sizeof(float));
  unsigned long long s = 88172645463325252ULL; int K = clustered ? 2000 : 1;
  float* centers = (float*)malloc((size_t)K * dim * sizeof(float));
  for (long i = 0; i < (long)K * dim; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; centers[i] = (float)((s % 100000) / 100000.0) * 2.0f - 1.0f; }
  for (long i = 0; i < n; i++) {
    long c = 0; if (K > 1) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; c = s % K; }
    for (long j = 0; j < dim; j++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17;
      float noise = (float)((s % 100000) / 100000.0) * 0.35f - 0.175f; d[i * dim + j] = centers[c * dim + j] + noise; }
  }
  free(centers); return d;
}
static void wr(const char* dir, const char* name, const void* p, size_t bytes) {
  char path[512]; snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE* f = fopen(path, "wb"); if (!f) { perror(path); exit(2); }
  if (fwrite(p, 1, bytes, f) != bytes) { perror("fwrite"); exit(2); }
  fclose(f);
}
int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : "/root/work/data/uniform256";
  long n = argc > 2 ? atol(argv[2]) : 250000;
  long dim = argc > 3 ? atol(argv[3]) : 256;
  int clustered = argc > 4 ? atoi(argv[4]) : 0;
  long nq = argc > 5 ? atol(argv[5]) : 4096;
  long K = 100;
  printf("gen dir=%s n=%ld dim=%ld clustered=%d nq=%ld\n", dir, n, dim, clustered, nq);
  float* hall = gen_data(n + nq, dim, clustered);
  float* hbase = hall; float* hq = hall + (size_t)n * dim;
  wr(dir, "base.f32", hbase, (size_t)n * dim * 4);
  wr(dir, "query.f32", hq, (size_t)nq * dim * 4);
  /* TSV: id \t [v,...] with the same decimal rint exactly reproducible */
  {
    char path[512]; snprintf(path, sizeof path, "%s/insert.tsv", dir);
    FILE* f = fopen(path, "wb"); if (!f) { perror(path); exit(2); }
    char* line = (char*)malloc((size_t)dim * 16 + 64);
    for (long i = 0; i < n; i++) {
      int off = sprintf(line, "%ld\t[", 101 + 17 * i);
      for (long j = 0; j < dim; j++) off += sprintf(line + off, j ? ",%.9g" : "%.9g", hbase[i * dim + j]);
      line[off] = ']'; line[off + 1] = '\n';
      fwrite(line, 1, off + 2, f);
    }
    fclose(f); free(line);
  }
  {
    char path[512]; snprintf(path, sizeof path, "%s/queries.txt", dir);
    FILE* f = fopen(path, "wb"); if (!f) { perror(path); exit(2); }
    char* line = (char*)malloc((size_t)dim * 16 + 16);
    for (long q = 0; q < nq; q++) {
      int off = sprintf(line, "[");
      for (long j = 0; j < dim; j++) off += sprintf(line + off, j ? ",%.9g" : "%.9g", hq[q * dim + j]);
      line[off] = ']'; line[off + 1] = '\n';
      fwrite(line, 1, off + 2, f);
    }
    fclose(f); free(line);
  }
  /* GT via cuVS brute force on device */
  cuvsResources_t res; CK(cuvsResourcesCreate(&res));

  void* dp = NULL; CK(cuvsRMMAlloc(res, &dp, (size_t)n * dim * 4));
  cudaMemcpy(dp, hbase, (size_t)n * dim * 4, cudaMemcpyHostToDevice);
  DLManagedTensor ddev = tensor(dp, n, dim, kDLFloat, 32, kDLCUDA);
  void* qp = NULL; CK(cuvsRMMAlloc(res, &qp, (size_t)nq * dim * 4));
  cudaMemcpy(qp, hq, (size_t)nq * dim * 4, cudaMemcpyHostToDevice);
  DLManagedTensor qdev = tensor(qp, nq, dim, kDLFloat, 32, kDLCUDA);
  cuvsBruteForceIndex_t bf; CK(cuvsBruteForceIndexCreate(&bf));
  CK(cuvsBruteForceBuild(res, &ddev, L2Expanded, 0.f, bf));
  void* np = NULL; CK(cuvsRMMAlloc(res, &np, (size_t)nq * K * 8));
  void* dpp = NULL; CK(cuvsRMMAlloc(res, &dpp, (size_t)nq * K * 4));
  DLManagedTensor nt = tensor(np, nq, K, kDLInt, 64, kDLCUDA);
  DLManagedTensor dt = tensor(dpp, nq, K, kDLFloat, 32, kDLCUDA);
  cuvsFilter nf; nf.type = NO_FILTER; nf.addr = (uintptr_t)0;
  CK(cuvsBruteForceSearch(res, bf, &qdev, &nt, &dt, nf));
  cudaDeviceSynchronize();
  int64_t* g64 = (int64_t*)malloc((size_t)nq * K * 8);
  cudaMemcpy(g64, np, (size_t)nq * K * 8, cudaMemcpyDeviceToHost);
  int32_t* g32 = (int32_t*)malloc((size_t)nq * K * 4);
  for (long i = 0; i < nq * K; i++) g32[i] = (int32_t)(101 + 17 * g64[i]);
  wr(dir, "gt.i32", g32, (size_t)nq * K * 4);
  printf("wrote base.f32 query.f32 insert.tsv queries.txt gt.i32\n");
  return 0;
}
