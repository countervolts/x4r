# Frame-overlap trial

This is a diagnostic trial, not a confirmed ghosting fix. `FrameSync = true`
in `x4r/x4r.ini` waits for a GPU fence after a command list containing translated
XeSS work. Such lists are submitted separately when an application batches
several lists. This prevents the submitting CPU from advancing past that work
while it is still queued on the GPU. It can reduce FPS.

The trial does not prove that the game supplied current-frame inputs or that
XeSS selected the correct history texture. Inputs may already be stale before
submission; resource contents and actual game frame numbers remain unverified.
The normal shader algorithms, motion-vector conventions and temporal history
are preserved.

Queue dependencies get up to 50 ms to complete before submission. If a GPU wait
still needs a later CPU signal/submission, the trial skips CPU blocking rather
than deadlocking that submitting thread. A completion wait times out after two
seconds and disables the trial for that process. Skips or timeouts make the
frame-overlap test incomplete.

Validation on RX 7700 XT:

- The ordinary 20-frame responsive-mask fixture completed 20 fences without
  skips/timeouts and produced the previous output byte-for-byte.
- A queued-frame probe copied a CPU-written frame marker back from the GPU.
  All 20 markers matched in both normal and synchronized modes. Thus this
  fixture did not reproduce incorrect frame age, even with the caller's normal
  per-frame GPU wait removed. This is not evidence that the game is correct.
- A three-frame probe queued GPU waits whose CPU signals were issued only after
  `ExecuteCommandLists` returned. All three were skipped safely, with no fence
  timeouts, and the probe completed.
- Command-list pipeline tracking now handles successful `Reset` and
  `ClearState`, including the initial PSO, rather than carrying old bindings.

Ready or Not is deployed with `FrameSync = true` and tracing enabled for this
trial. The release ZIP defaults to `FrameSync = false`.

Fully restart the game after deployment. Play the same scene, then inspect
`x4r/frame-sync-status.txt` or run `bash x4r/x4r-check.sh`. A useful trial has
`frame_sync_active=1`, increasing `frame_sync_completed`, no timeouts and ideally
no skipped waits. Those counters verify fence completion, not visual quality.

To restore normal queuing, set `FrameSync = false` and restart. The prior plugin
and config are backed up under the development checkout's
`private/frame-age-trial/game-backup/` directory.

Submission and fence behavior follows Microsoft's D3D12 documentation:
https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists
https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/direct3d12/user-mode-heap-synchronization.md
