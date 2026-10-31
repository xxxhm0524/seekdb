/* Standalone CAGRA build/search tuner (no seekdb, no bridge).
 * usage: tune <base.f32> <n> <dim> <query.f32> <nq> <gt.i32> <topk>
 *        [algo] [graph_degree] [inter_degree] [niter] [search_width] [itopk]
 */
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void* rd(const char* p, size_t bytes) {
  FILE* f = fopen(p, "rb");
  if (!f) { fprintf(stderr, "open %s failed\n", p); exit(2); }
  void* d = malloc(bytes);
  if (fread(d, 1, bytes, f) != bytes) { fprintf(stderr, "short read %s\n", p); exit(2); }
  fclose(f);
  return d;
}

int main(int argc, char** argv) {
  if (argc < 8) { fprintf(stderr, "usage: %s base n dim query nq gt topk [algo deg inter niter sw itopk]\n", argv[0]); return 2; }
  const char* base_p = argv[1];
  long n = atol(argv[2]), dim = atol(argv[3]);
  const char* q_p = argv[4];
  long nq = atol(argv[5]);
  const char* gt_p = argv[6];
  long topk = atol(argv[7]);
  int algo = argc > 8 ? atoi(argv[8]) : 2;
  int deg = argc > 9 ? atoi(argv[9]) : 128;
  int ideg = argc > 10 ? atoi(argv[10]) : 256;
  int niter = argc > 11 ? atoi(argv[11]) : 100;
  int sw = argc > 12 ? atoi(argv[12]) : 4;
  int itopk = argc > 13 ? atoi(argv[13]) : 0;

  float* base = (float*)rd(base_p, (size_t)n * dim * 4);
  float* q = (float*)rd(q_p, (size_t)nq * dim * 4);
  int* gt = (int*)rd(gt_p, (size_t)nq * 100 * 4);

  cuvsResources_t res; cuvsResourcesCreate(&res);
  cuvsCagraIndexParams_t ip; cuvsCagraIndexParamsCreate(&ip);
  ip->build_algo = (enum cuvsCagraGraphBuildAlgo)algo;
  ip->graph_degree = deg;
  ip->intermediate_graph_degree = ideg;
  ip->nn_descent_niter = niter;
  cuvsCagraIndex_t idx; cuvsCagraIndexCreate(&idx);

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = base;
  dset.dl_tensor.device.device_type = kDLCPU;
  dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  double t0 = now_s();
  cuvsError_t rc = cuvsCagraBuild(res, ip, &dset, idx);
  cudaDeviceSynchronize();
  double t_build = now_s() - t0;
  if (rc != CUVS_SUCCESS) { fprintf(stderr, "build failed: %s\n", cuvsGetLastErrorText()); return 1; }

  float* qd; unsigned* nd; float* dd;
  cuvsRMMAlloc(res, (void**)&qd, sizeof(float) * nq * dim);
  cuvsRMMAlloc(res, (void**)&nd, sizeof(unsigned) * nq * topk);
  cuvsRMMAlloc(res, (void**)&dd, sizeof(float) * nq * topk);
  double t1 = now_s();
  cudaMemcpy(qd, q, sizeof(float) * nq * dim, cudaMemcpyDefault);
  double t_h2d = now_s() - t1;

  cuvsCagraSearchParams_t sp; cuvsCagraSearchParamsCreate(&sp);
  if (itopk > 0) sp->itopk_size = itopk;
  else if ((size_t)topk * 4 > sp->itopk_size) sp->itopk_size = (size_t)topk * 4;
  sp->search_width = sw;

  double t2 = now_s();
  DLManagedTensor qt, nt, dt; long qs[2] = {nq, dim}, nsh[2] = {nq, topk}, dsh[2] = {nq, topk};
  qt.dl_tensor.data = qd; qt.dl_tensor.device.device_type = kDLCUDA; qt.dl_tensor.device.device_id = 0;
  qt.dl_tensor.ndim = 2; qt.dl_tensor.dtype.code = kDLFloat; qt.dl_tensor.dtype.bits = 32;
  qt.dl_tensor.dtype.lanes = 1; qt.dl_tensor.shape = qs; qt.dl_tensor.strides = 0;
  qt.dl_tensor.byte_offset = 0;
  nt = qt; nt.dl_tensor.data = nd; nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = nsh;
  dt = qt; dt.dl_tensor.data = dd; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.shape = dsh;
  cuvsFilter f; f.type = NO_FILTER; f.addr = (uintptr_t)0;
  rc = cuvsCagraSearch(res, sp, idx, &qt, &nt, &dt, f);
  if (rc != CUVS_SUCCESS) { fprintf(stderr, "search failed: %s\n", cuvsGetLastErrorText()); return 1; }
  unsigned* hid = (unsigned*)malloc(sizeof(unsigned) * nq * topk);
  float* hdist = (float*)malloc(sizeof(float) * nq * topk);
  cudaMemcpy(hid, nd, sizeof(unsigned) * nq * topk, cudaMemcpyDefault);
  cudaMemcpy(hdist, dd, sizeof(float) * nq * topk, cudaMemcpyDefault);
  double t_search = now_s() - t2;

  int sorted = 1;
  for (long i = 0; i < nq && sorted; i++)
    for (long j = 1; j < topk; j++)
      if (hdist[i * topk + j] < hdist[i * topk + j - 1]) { sorted = 0; break; }

  /* recall@K compares against the first K returned ids, not the whole list;
     in100 is the looser "true top-10 anywhere in the top-100" figure. */
  long j10 = topk < 10 ? topk : 10;
  long h10 = 0, h100 = 0, h10_in100 = 0;
  for (long i = 0; i < nq; i++) {
    for (long k = 0; k < 10; k++) {
      for (long j = 0; j < j10; j++)
        if (hid[i * topk + j] == (unsigned)gt[i * 100 + k]) { h10++; break; }
      for (long j = 0; j < topk; j++)
        if (hid[i * topk + j] == (unsigned)gt[i * 100 + k]) { h10_in100++; break; }
    }
    for (long k = 0; k < 100; k++) {
      for (long j = 0; j < topk; j++)
        if (hid[i * topk + j] == (unsigned)gt[i * 100 + k]) { h100++; break; }
    }
  }
  printf("algo=%d deg=%d ideg=%d niter=%d sw=%d itopk=%zu sorted=%d build=%.3fs h2d=%.3fs search=%.3fs "
         "recall@10=%.4f recall@10_in100=%.4f recall@100=%.4f\n",
         algo, deg, ideg, niter, sw, sp->itopk_size, sorted, t_build, t_h2d, t_search,
         h10 / (nq * 10.0), h10_in100 / (nq * 10.0), h100 / (nq * 100.0));
  return 0;
}
