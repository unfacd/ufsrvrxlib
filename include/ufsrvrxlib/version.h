/**
 * @file version.h
 * @brief Version macros and accessor declarations.
 *
 * MAJOR/MINOR/PATCH are maintained manually here.  CMake regex-parses this
 * file at configure time to set project(VERSION ...).  The function bodies
 * are generated at build time by src/version.c.sh → version.c.
 */
#ifndef UFSRVRXLIB_VERSION_H
#define UFSRVRXLIB_VERSION_H

#include <ufsrvrxlib/ufsrvrxlib_defs.h>

#define UFSRVRXLIB_MAJOR 0
#define UFSRVRXLIB_MINOR 1
#define UFSRVRXLIB_PATCH 21

/*!
 * @brief Full dotted version string (e.g. "0.1.0").
 * @return Static version string.
 */
PUBLIC_API const char *UfsrvRxLibVersion(void)      __attribute__((const));

/*!
 * @brief Major version component.
 * @return Static major-version string.
 */
PUBLIC_API const char *UfsrvRxLibVersionMajor(void) __attribute__((const));

/*!
 * @brief Minor version component.
 * @return Static minor-version string.
 */
PUBLIC_API const char *UfsrvRxLibVersionMinor(void) __attribute__((const));

/*!
 * @brief Patch version component.
 * @return Static patch-version string.
 */
PUBLIC_API const char *UfsrvRxLibVersionPatch(void) __attribute__((const));

#endif /* UFSRVRXLIB_VERSION_H */
