/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "TurboStructLiteDebugLibrary.h"
#include "TurboStructLiteTypesDebug.h"

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
#define TURBOSTRUCTLITE_DEBUG_LOG(x) TurboStructLiteDebugLog((x), ETurboStructLiteLogType::Normal)
#define TURBOSTRUCTLITE_DEBUG_LOG_WARNING(x) TurboStructLiteDebugLog((x), ETurboStructLiteLogType::Warning)
#define TURBOSTRUCTLITE_DEBUG_LOG_ERROR(x) TurboStructLiteDebugLog((x), ETurboStructLiteLogType::Error)
#else
#define TURBOSTRUCTLITE_DEBUG_LOG(x)
#define TURBOSTRUCTLITE_DEBUG_LOG_WARNING(x)
#define TURBOSTRUCTLITE_DEBUG_LOG_ERROR(x)
#endif

#if CPUPROFILERTRACE_ENABLED
#define TURBOSTRUCTLITE_TRACE_SCOPE(NameStr) TRACE_CPUPROFILER_EVENT_SCOPE_STR(NameStr)
#define TURBOSTRUCTLITE_TRACE_SCOPE_TEXT(Name) TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(Name)
#else
#define TURBOSTRUCTLITE_TRACE_SCOPE(NameStr)
#define TURBOSTRUCTLITE_TRACE_SCOPE_TEXT(Name)
#endif
