#include <array>
#include <chrono>
#include <future>
#include <random>

#include "gpu.h"

#include "array_utils.h"
#include "llmc/reference_impls.h" // for CPU reference implementation
#include "utils/logging.h"

using namespace gpu;

static const char *kShaderMatmul1 = R"(
@group(0) @binding(0) var<storage, read_write> A: array<{{precision}}>;
@group(0) @binding(1) var<storage, read_write> B: array<{{precision}}>;
@group(0) @binding(2) var<storage, read_write> C: array<{{precision}}>;
@compute @workgroup_size({{workgroupSize}})
fn main(
    @builtin(global_invocation_id) globalID : vec3<u32>) {
    let row = globalID.x; // Use x as row makes mapping to Shape more intuitive
    let col = globalID.y;
    if (row >= {{M}} || col >= {{N}}) {
        return;
    }
    var total: {{precision}} = A[row * {{K}}] * B[col * {{K}}]; // assumes size >= 1
    for (var k = 1u; k < {{K}}; k = k + 1u) {
        // B is stored as B^T, effectively column-major
        total += A[row * {{K}} + k] * B[col * {{K}} + k];
    }
    C[row * {{N}} + col] = total;
}
)";

inline ShaderCode createMatmul1(const char *shaderTemplate, const size_t M,
                                const size_t K, const size_t N,
                                const Shape &workgroupSize = {256, 1, 1},
                                NumType precision = kf32) {
  std::string codeString(shaderTemplate);

  replaceAll(codeString, {{"{{workgroupSize}}", toString(workgroupSize)},
                          {"{{precision}}", toString(precision)},
                          {"{{M}}", toString(M)},
                          {"{{K}}", toString(K)},
                          {"{{N}}", toString(N)}});

  return ShaderCode{codeString, workgroupSize};
}

// Shared memory cache-blocking
static const char *kShaderMatmul2 = R"(
@group(0) @binding(0) var<storage, read_write> A: array<{{precision}}>;
@group(0) @binding(1) var<storage, read_write> B: array<{{precision}}>;
@group(0) @binding(2) var<storage, read_write> C: array<{{precision}}>;
var<workgroup> As: array<{{precision}}, {{tileSize}} * {{tileSize}}>;
var<workgroup> Bs: array<{{precision}}, {{tileSize}} * {{tileSize}}>;
@compute @workgroup_size({{workgroupSize}})
fn main(
  @builtin(local_invocation_index) localIdx : u32,
  @builtin(workgroup_id) groupID: vec3<u32>) {
    let loadRow = localIdx /  {{tileSize}};
    let loadCol = localIdx % {{tileSize}};
    let row = groupID.x * {{tileSize}} + loadRow;
    let col = groupID.y * {{tileSize}} + loadCol;
    let aRow = groupID.x * {{tileSize}} + loadRow;
    let bRow = groupID.y * {{tileSize}} + loadCol;
    var total: {{precision}} = 0.0;
    for (var tile = 0u;
         tile < ({{K}} + {{tileSize}} - 1) / {{tileSize}};
         tile = tile + 1u) {
      let aCol = tile * {{tileSize}} + loadCol;
      let bCol = tile * {{tileSize}} + loadRow;
      // We can skip masking here *iff* tileSize is evenly
      // divisible into M, K, and N dimensions
      As[loadRow * {{tileSize}} + loadCol] =
        A[aRow * {{K}} + aCol];
        // A[aRow * {{K}} + aCol] * {{precision}}(aRow < {{M}} && aCol < {{K}}); // masked version
      Bs[loadCol * {{tileSize}} + loadRow] =
        B[bRow * {{K}} + bCol];
        // B[bRow * {{K}} + bCol] * {{precision}}(bRow < {{N}} && bCol < {{K}}); // masked version
      workgroupBarrier();
      for (var k = 0u; k < {{tileSize}}; k = k + 1u) {
        total += As[loadRow * {{tileSize}} + k] *
                 Bs[loadCol * {{tileSize}} + k];
      }
      workgroupBarrier();
    }
    if (row >= {{M}} || col >= {{N}}) {
      return;
    }
    C[row * {{N}} + col] = total;
}
)";

inline ShaderCode createMatmul2(const char *shaderTemplate, const size_t M,
                                const size_t K, const size_t N,
                                const Shape &workgroupSize = {256, 1, 1},
                                NumType precision = kf32) {
  std::string codeString(shaderTemplate);
  replaceAll(codeString,
             {{"{{workgroupSize}}", toString(workgroupSize)},
              {"{{precision}}", toString(precision)},
              {"{{M}}", toString(M)},
              {"{{K}}", toString(K)},
              {"{{N}}", toString(N)},
              {"{{tileSize}}",
               toString(static_cast<size_t>(sqrt(workgroupSize[0])))}});
  return ShaderCode{codeString, workgroupSize};
}

/* 1D block-tiling
 *
 * - A block tile in C is of size BM x BN
 * - Each workgroup computes a BM x BN block of C
 * - The BM rows of a block tile in As are split into TM x TK
 *   tiles
 *
 * There are three nested loops in the kernel:
 * - The outer loop over block tiles which increments
 *   from 0..K by increments of BK
 *
 *   In this outer loop we load BM x BK tiles shared by
 *   the threads in the workgroup.
 *
 * - The second loop which iterates from 0..BK aggregating the partial dot
 *   product contribution of a single tile
 *
 *  - The innermost loop iterates from 0..TM. Each thread in the workgroup
 *  computes a different row of the block tile in C.
 *
 */
static const char *kShaderMatmul3 = R"(

@group(0) @binding(0) var<storage, read_write> A: array<{{precision}}>;
@group(0) @binding(1) var<storage, read_write> B: array<{{precision}}>;
@group(0) @binding(2) var<storage, read_write> C: array<{{precision}}>;
var<workgroup> tileA: array<{{precision}}, {{BM}} * {{BK}}>;
var<workgroup> tileB: array<{{precision}}, {{BK}} * {{BN}}>;

@compute @workgroup_size({{workgroupSize}})
fn main(
    @builtin(global_invocation_id) globalID : vec3<u32>,
    @builtin(local_invocation_id) localID : vec3<u32>,
    @builtin(local_invocation_index) localIdx : u32,
    @builtin(workgroup_id) groupID : vec3<u32>) {

    var threadResults: array<{{precision}}, {{TM}}>;

    let cRow: u32 = groupID.x;
    let cCol: u32 = groupID.y;

    // Position of the first C element computed by the thread
    let threadRow: u32 = localID.x / {{BN}};
    let threadCol: u32 = localID.x % {{BN}};

    // Value of A to cache in As
    let loadColA = localID.x % {{BK}};
    let loadRowA = localID.x / {{BK}};

    // Value of B to cache in Bs (B is stored as B^T)
    let loadColB = localID.x % {{BK}};
    let loadRowB = localID.x / {{BK}};

    // aPtr and bPtr are the starting positions of the tiles in A and B,
    // incremented in the bkIdx loop. 
    // cPtr is the starting position of the tile in C which is fixed.

    var aPtr = cRow * {{BM}} * {{K}};
    var bPtr = (cCol * {{BN}})  // cCol corresponds to the row in B^T
                * {{K}}; // K columns per row (column-major)
    var cPtr = cRow * {{BM}} * {{N}} + cCol * {{BN}};

    for (var bkIdx = 0; bkIdx < {{K}}; bkIdx += {{BK}}) {
      tileA[loadRowA * {{BK}} + loadColA] = A[aPtr + loadRowA * {{K}} + loadColA];
      tileB[loadRowB * {{BK}} + loadColB] = B[bPtr + loadRowB * {{K}} + loadColB];

      aPtr += {{BK}};
      bPtr += {{BK}};

      workgroupBarrier();

      for (var dotIdx: u32 = 0; dotIdx < {{BK}}; dotIdx = dotIdx + 1) {
        let tmp = tileB[threadCol * {{BK}} + dotIdx];
        for (var resIdx: u32 = 0; resIdx < {{TM}}; resIdx = resIdx + 1) {
          let mask = {{precision}}(threadRow * {{TM}} + resIdx < {{BM}} 
                          && threadCol < {{BN}}
                          && threadRow * {{TM}} + resIdx < {{M}}
                          && cCol * {{BN}} + threadCol < {{N}}
                          && cRow * {{BM}} + threadRow < {{M}}
                          );
          threadResults[resIdx] += mask * tileA[(threadRow * {{TM}} + resIdx) * {{BK}} + dotIdx] * tmp;
        }
      }

      workgroupBarrier();

    }

    for (var resIdx: u32 = 0; resIdx < {{TM}}; resIdx = resIdx + 1) {
      C[cPtr + (threadRow * {{TM}} + resIdx) * {{N}} + threadCol] = threadResults[resIdx];
    }
    
}
)";

inline ShaderCode createMatmul3(const char *shaderTemplate, const size_t M,
                                const size_t K, const size_t N, const size_t BM,
                                const size_t BK, const size_t BN,
                                const size_t TM,
                                const Shape &workgroupSize = {256, 1, 1},
                                NumType precision = kf32) {
  std::string codeString(shaderTemplate);
  replaceAll(codeString, {{"{{workgroupSize}}", toString(workgroupSize)},
                          {"{{precision}}", toString(precision)},
                          {"{{M}}", toString(M)},
                          {"{{K}}", toString(K)},
                          {"{{N}}", toString(N)},
                          {"{{BM}}", toString(BM)},
                          {"{{BK}}", toString(BK)},
                          {"{{BN}}", toString(BN)},
                          {"{{TM}}", toString(TM)}});
  return ShaderCode{codeString, workgroupSize};
}

void initData(size_t M, size_t K, size_t N, std::unique_ptr<float[]> &inputPtr,
              std::unique_ptr<float[]> &weightsPtr) {
  std::mt19937 gen(314159);
  randn(inputPtr.get(), M * K, gen);
  randn(weightsPtr.get(), N * K, gen);
  LOG(kDefLog, kInfo, "%s", show<float>(inputPtr.get(), M, K, "Input").c_str());
  LOG(kDefLog, kInfo, "%s",
      show<float>(weightsPtr.get(), N, K, "Weights").c_str());
}

void checkCPU(size_t M, size_t K, size_t N, std::unique_ptr<float[]> &inputPtr,
              std::unique_ptr<float[]> &weightsPtr,
              std::unique_ptr<float[]> &outputPtr) {
  LOG(kDefLog, kInfo, "Computing CPU reference implementation");
  std::unique_ptr<float[]> outputRefPtr = std::make_unique<float[]>(M * N);
  ref::matmul_forward_cpu(outputRefPtr.get(), inputPtr.get(), weightsPtr.get(),
                          nullptr, 1, M, K, N);
  // LOG(kDefLog, kInfo, "Reference Output: %s",
  // show<float>(outputRefPtr.get(), M, N, "Output (Reference)").c_str());
  LOG(kDefLog, kInfo,
      isclose(outputPtr.get(), outputRefPtr.get(), M * N) ? "PASS" : "FAIL");
}

void runTest(int version, size_t M, size_t K, size_t N,
             std::unique_ptr<float[]> &inputPtr,
             std::unique_ptr<float[]> &weightsPtr,
             std::unique_ptr<float[]> &outputPtr) {

  // Allocate GPU buffers and copy data
  Context ctx = createContext();
  Tensor input = createTensor(ctx, Shape{M, K}, kf32, inputPtr.get());
  Tensor weights =
      createTensor(ctx, Shape{N, K}, kf32, weightsPtr.get()); // column-major
  Tensor output = createTensor(ctx, Shape{M, N}, kf32);

  // Initialize Kernel and bind GPU buffers
  LOG(kDefLog, kInfo, "Creating Kernel");
  Kernel kernel;
  if (version == 1) {
    Shape wgSize = {16, 16, 1};
    LOG(kDefLog, kInfo, "wgSize: %s", toString(wgSize).c_str());
    ShaderCode matmul =
        createMatmul1(kShaderMatmul1, M, K, N, /*wgsize*/ wgSize);
    kernel = createKernel(ctx, matmul, Bindings{input, weights, output},
                          /*nWorkgroups*/ cdiv({M, N, 1}, wgSize));
  } else if (version == 2) {
    static constexpr size_t tileSize = 16;
    ShaderCode matmul = createMatmul2(kShaderMatmul2, M, K, N,
                                      /*wgSize*/ {tileSize * tileSize, 1, 1});
    kernel =
        createKernel(ctx, matmul, Bindings{input, weights, output},
                     /* nWorkgroups*/ cdiv({M, N, 1}, {tileSize, tileSize, 1}));
  } else if (version == 3) {
    // TODO(avh): fails for larger block dimensions
    static constexpr size_t BM = 4; // 32;
    static constexpr size_t BK = 4; // 8;
    static constexpr size_t BN = 4; // 32;
    static constexpr size_t TM = 1; // 8;
    // BM * BN values per workgroup, TM rows per thread => BM * BN / TM threads
    ShaderCode matmul = createMatmul3(kShaderMatmul3, M, K, N, BM, BK, BN, TM,
                                      /*wgSize*/ {BM * BN / TM, 1, 1});
    kernel =
        createKernel(ctx, matmul, Bindings{input, weights, output},
                     /*nWorkgroups*/ {cdiv(cdiv(M, BM), TM), cdiv(N, BN), 1});
    // /*nWorkgroups*/ cdiv({M, N, 1}, {BM, BN, 1}));
  }

  // Dispatch kernel execution
  LOG(kDefLog, kInfo, "Dispatching + waiting");

  // pre-allocate promises and futures for async dispatch
  // TODO(avh): implement a pooling mechanism for promises/futures in gpu.h
  constexpr size_t nIter = 4;
  std::array<std::promise<void>, nIter> promises;
  std::array<std::future<void>, nIter> futures;
  for (int i = 0; i < nIter; i++) {
    futures[i] = promises[i].get_future();
  }

  // Dispatch kernel nIter times
  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < nIter; i++) {
    dispatchKernel(ctx, kernel, promises[i]);
    wait(ctx, futures[i]);
    resetCommandBuffer(ctx.device, kernel);
  }
  auto end = std::chrono::high_resolution_clock::now();

  // Report performance
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  float gflops = 2 * M * N *
                 K / // factor of 2 for multiplication & accumulation
                 (static_cast<float>(duration.count()) / 1000.0) /
                 1000000000.0 * static_cast<float>(nIter);
  LOG(kDefLog, kInfo,
      "Execution Time: (M = %d, K = %d, N = %d) x %d iterations :  %.1f "
      "milliseconds / dispatch ~ %.2f "
      "GFLOPS/s",
      M, K, N, nIter, duration.count() / static_cast<float>(nIter), gflops);
  LOG(kDefLog, kInfo, "Copying result to CPU");
  toCPU(ctx, output, outputPtr.get(), M * N * sizeof(float));
  LOG(kDefLog, kInfo, "%s",
      show<float>(outputPtr.get(), M, N, "Output").c_str());
}

int main() {
  static constexpr int kTestSize = 1;
  size_t M, K, N;
  if constexpr (kTestSize == 0) {
    // Tiny test
    M = 16;
    K = 4;
    N = 8;
  } else if constexpr (kTestSize == 1) {
    // Small test
    M = 256;
    K = 128;
    N = 512;
  } else {
    // Large test
    M = 4096;
    K = 4096;
    N = 2 * 4096;
  }
  int version = 3; // 1 == naive
                   // 2 == tiling
                   // 3 == 1D blocktiling (WIP)

  std::unique_ptr<float[]> inputPtr = std::make_unique<float[]>(M * K);
  std::unique_ptr<float[]> weightsPtr = std::make_unique<float[]>(N * K);
  std::unique_ptr<float[]> outputPtr = std::make_unique<float[]>(M * N);

  initData(M, K, N, inputPtr, weightsPtr);
  runTest(version, M, K, N, inputPtr, weightsPtr, outputPtr);

  if constexpr (kTestSize <= 0) {
    // Check result with CPU reference implementation for tiny/small tests
    checkCPU(M, K, N, inputPtr, weightsPtr, outputPtr);
  }

  LOG(kDefLog, kInfo, "Done.");
  return 0;
}
