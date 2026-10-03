# Change Log — ufsrvrxlib

## Release-261003 — 0.2.0
`2a292607` → `d87b23a` · 2026-09-13 → 2026-10-03

### New
- Future combinators `UfsrvFutureMapEx` / `UfsrvFutureZipEx` take the value destructor explicitly, so a mapper or zipper is no longer assumed to return a `malloc` block
- `UfsrvCoroutineIsInCoroutine()` added; `UfsrvCoroutineSubmitResume()` now returns `bool`
- Three-stage dependency resolution (`cmake-modules/ufsrv_resolve_dependency.cmake`): `packages/`, system install (pkg-config → CMake CONFIG), or fetch — with the resolved origin (`PARENT`/`SOURCE`/`SYSTEM`/`FETCH`) recorded as INTERNAL cache entries
- Contract stress harness `tests/integration/ufsrv_contract_stress.c`
- `UFSRVRXLIB_API_GUIDE.md` added; `docs/man/ufsrvrxlib.7` expanded

### Fixes
- Continuation and waiter drains adopt uflib's closed-state API — five lost-wakeup guards and the orphan-waiter paths deleted
- Unstarted coroutines reclaimed at teardown instead of leaked
- libaco objects folded into `libufsrvrxlib.a` and restored to the pkg-config link line
- Promise/mover contract: unfulfilled `Destroy` completes `EPIPE`; movers claim `is_consumed` and answer `EINVAL`; `Zip` retains its inputs; `All`/`Any` fail instead of hanging
- Cancellation-token registry becomes a mutex-guarded list with a refcounted token
- The `.deb` is named `ufsrvrxlib-dev` and depends on the published `uflib-dev`, not the non-existent `libuflib`

### Deprecated
- (none)

### Removed
- `UfsrvCoroutineSubmitResume()` returning `void` — callers must handle the `bool`
