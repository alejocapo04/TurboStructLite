#pragma once

#include "CoreMinimal.h"

// Minimum element count to allow parallelization for complex inner types.
inline constexpr int32 TurboStructLiteParallelThresholdComplex = 2;
// Minimum element count to allow parallelization for simple inner types.
inline constexpr int32 TurboStructLiteParallelThresholdSimple = 1024;
// Default batch size used when chunking work in parallel serialization.
inline constexpr int32 TurboStructLiteParallelBatchSizeDefault = 1024;
// Minimum batch size for parallel chunking.
inline constexpr int32 TurboStructLiteParallelMinBatchSize = 1024;
// Target bytes per chunk for simple type batching.
inline constexpr int32 TurboStructLiteParallelTargetBytesPerChunk = 256 * 1024;
// Maximum number of chunks per thread for simple type batching.
inline constexpr int32 TurboStructLiteParallelMaxChunksPerThread = 4;
