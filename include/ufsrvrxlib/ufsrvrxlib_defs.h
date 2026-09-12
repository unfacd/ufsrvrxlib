/**
 * @file ufsrvrxlib_defs.h
 * @brief Preprocessor definitions shared across all public headers of ufsrvrxlib.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFSRVRXLIB_DEFS_H
#define UFSRVRXLIB_DEFS_H

/*!
 * Marker for public API functions — exported from the shared library.
 *
 * Place before the return type of every function that is part of a module's
 * public contract.  With -fvisibility=hidden, only PUBLIC_API symbols are
 * visible to consumers.  The attribute is harmless (no-op) in static builds.
 *
 * @code{.c}
 * PUBLIC_API UfsrvScheduler *UfsrvSchedulerCreate(const char *name_ptr);
 * PUBLIC_API bool UfsrvSchedulerSubmit(UfsrvScheduler *scheduler_ptr, UfsrvSchedulerJobCallback callback, void *context_ptr);
 * @endcode
 */
#ifndef PUBLIC_API
  #if defined(__GNUC__) || defined(__clang__)
    #define PUBLIC_API __attribute__((visibility("default")))
  #else
    #define PUBLIC_API
  #endif
#endif

#endif /* UFSRVRXLIB_DEFS_H */
