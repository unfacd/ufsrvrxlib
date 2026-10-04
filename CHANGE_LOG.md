# Change Log — ufsrvrxlib

## Release-261004 — 0.2.1
`1e43d34` → `8dc50af` · 2026-10-03 → 2026-10-04

### New
- `README.md` documents installing from the signed apt repository

### Fixes
- The `.deb` installs to the prefix its own pkg-config file advertises, so `pkg-config --cflags --libs ufsrvrxlib` no longer names paths that do not exist
- The installer configures the `uflib` repository as well as its own, so the documented one-liner completes on a clean host instead of failing with `uflib-dev is not installable`

### Deprecated
- (none)

### Removed
- (none)

## Release-261003 — 0.2.0
`2a292607` → `d87b23a` · 2026-09-13 → 2026-10-03

### New
- Future combinators `UfsrvFutureMapEx` / `UfsrvFutureZipEx` take the value destructor explicitly, `UfsrvCoroutineIsInCoroutine()` is added, and `UfsrvCoroutineSubmitResume()` returns `bool` rather than `void` — callers must handle the result
- Three-stage dependency resolution — `packages/`, the system install, or a fetch — recording the origin it resolved from
- `UFSRVRXLIB_API_GUIDE.md` added and `docs/man/ufsrvrxlib.7` expanded

### Fixes
- The completion, cancellation and coroutine contracts close by adopting uflib's closed-state API: five lost-wakeup guards and the orphan-waiter paths are deleted, unstarted coroutines are reclaimed at teardown, promises and movers complete with `EPIPE` and `EINVAL` rather than hanging, and the cancellation-token registry becomes mutex-guarded
- libaco objects are folded into `libufsrvrxlib.a` and restored to the pkg-config link line
- The `.deb` is named `ufsrvrxlib-dev` and depends on the published `uflib-dev`, not the non-existent `libuflib`

### Deprecated
- (none)

### Removed
- (none)
