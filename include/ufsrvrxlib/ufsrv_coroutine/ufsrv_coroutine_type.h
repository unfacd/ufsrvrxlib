/**
 * @file ufsrv_coroutine_type.h
 * @brief Public types for the ufsrv_coroutine module.
 *
 * The coroutine is libaco's `aco_t`, consumed from uflib and exposed here as an
 * opaque handle (only a forward declaration is visible — libaco's header uses GNU
 * statement-expressions that are not C++-safe).
 */

#ifndef UFSRVRXLIB_UFSRV_COROUTINE_TYPE_H
#define UFSRVRXLIB_UFSRV_COROUTINE_TYPE_H

#include <uflib/standard_c_includes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Forward declaration — the coroutine is libaco's aco_t (opaque to consumers). */
struct aco_s;

/*! Opaque handle to a coroutine (aliases libaco's aco_t). */
typedef struct aco_s UfsrvCoroutine;

/*!
 * Coroutine entry point. The argument is retrieved with UfsrvCoroutineGetArg().
 */
typedef void (*UfsrvCoroutineEntry)(void);

#ifdef __cplusplus
}
#endif

#endif /* UFSRVRXLIB_UFSRV_COROUTINE_TYPE_H */
