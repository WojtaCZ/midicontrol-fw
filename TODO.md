# TODO

## Stuck base / stuck notes (watchdog)

- If the base firmware hangs, the organ can be left with notes stuck on.
- Use a watchdog: on a hang the base resets, and on the way back up it must clear the
  stuck notes, probably by switching off the organ's current source (power) and/or sending
  All Notes Off / Note Off for all notes.
- Make sure the outputs that drive the organ power go to a safe state during and after the reset.
- Note: the base already has an IWDG (~2 s timeout, `src/wdg.cpp`). What is
  missing is the stuck-note cleanup after a watchdog reset and checking that a reset really
  fires in every way the base can get stuck.
