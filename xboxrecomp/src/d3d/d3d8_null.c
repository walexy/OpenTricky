/* d3d8_null.c -- headless backend for the D3D8 / PGRAPH seam (OT_HEADLESS).
 *
 * Accepts every call and draws nothing. The entry points to provide are the
 * d3d8_* and pgraph_d3d11_* functions that the rest of the runtime calls; the
 * list is in macos/d3d_seam.txt. Behaviour that matters (MACOS_PORT_PLAN.md 0.5):
 *   - count each surface/flip change as a frame and advance PresentSeq;
 *   - complete NV097_GET_REPORT immediately, or the title spins on it;
 *   - report reports/draws/flips as idle.
 *
 * TODO(0.5): not written yet. Until then a link against this target fails with
 * the list of missing symbols, which is the inventory step 0.5 works from.
 */
