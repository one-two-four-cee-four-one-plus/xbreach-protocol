╭─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╮
│ Plan to implement                                                                                                                   │
│                                                                                                                                     │
│ Cyberpunk 2077 Breach Protocol Auth Module for XSecureLock                                                                          │
│                                                                                                                                     │
│ Context                                                                                                                             │
│                                                                                                                                     │
│ XSecureLock is an X11 screen lock utility. We're creating a new auth module auth_x11_grid that replaces the standard password       │
│ prompt with a Cyberpunk 2077 "Breach Protocol" hacking minigame visualization. The actual password is still typed normally          │
│ underneath — the grid UI is a purely visual layer. Each keypress advances a hardcoded path through a 5x5 hex code matrix, filling a │
│  buffer and completing target sequences.                                                                                            │
│                                                                                                                                     │
│ Files to Modify                                                                                                                     │
│ ┌─────────────────────────┬──────────────────────────────────────────────────────────────┐                                          │
│ │          File           │                            Action                            │                                          │
│ ├─────────────────────────┼──────────────────────────────────────────────────────────────┤                                          │
│ │ TASK.md                 │ CREATE — Task description and plan summary                   │                                          │
│ ├─────────────────────────┼──────────────────────────────────────────────────────────────┤                                          │
│ │ helpers/auth_x11_grid.c │ CREATE — New auth module (forked from helpers/auth_x11.c)    │                                          │
│ ├─────────────────────────┼──────────────────────────────────────────────────────────────┤                                          │
│ │ Makefile.am             │ MODIFY — Add build target for auth_x11_grid (after line 160) │                                          │
│ └─────────────────────────┴──────────────────────────────────────────────────────────────┘                                          │
│ No other files need changes. The module is self-contained.                                                                          │
│                                                                                                                                     │
│ Implementation Steps                                                                                                                │
│                                                                                                                                     │
│ Step 1: Scaffolding                                                                                                                 │
│                                                                                                                                     │
│ - Create TASK.md at repo root                                                                                                       │
│ - Copy helpers/auth_x11.c → helpers/auth_x11_grid.c                                                                                 │
│ - Add build target to Makefile.am (identical sources/flags to auth_x11, pointing to new .c)                                         │
│ - Verify it compiles with make auth_x11_grid                                                                                        │
│                                                                                                                                     │
│ Step 2: Data Structures & Cleanup                                                                                                   │
│                                                                                                                                     │
│ - Remove unused password prompt code (PasswordPrompt enum, disco/emoji/emoticon/kaomoji arrays, BumpDisplayMarker, ShowFromArray,   │
│ GetPasswordPromptFromFlags)                                                                                                         │
│ - Add grid constants: GRID_SIZE=5, BUFFER_SIZE=6, NUM_HEX_CODES=4, hex code strings                                                 │
│ - Add CODE_MATRIX[5][5] — indices into hex codes for each cell                                                                      │
│ - Add HACK_SEQUENCE[] — array of {row, col} pairs defining the path through the grid                                                │
│ - Add TargetSequence TARGETS[3] — DATAMINE_V1/V2/V3 as subsequences of hack sequence                                                │
│ - Add GridState struct: current_step, buffer_codes[], buffer_count, current_axis, active_row/col, sequence_complete[]               │
│                                                                                                                                     │
│ Step 3: Color Scheme                                                                                                                │
│                                                                                                                                     │
│ - Add 6 new XColor globals: cyber_green (#d0ed57), cyber_dim (#4a6a20), cyber_yellow (#ffd700), cyber_highlight (#2a3a10),          │
│ cyber_red (#ff3333), cyber_complete (#00ffcc)                                                                                       │
│ - Add corresponding GC arrays (gcs_cyber_*[MAX_WINDOWS]) and Xft colors                                                             │
│ - Allocate colors in main(), create GCs in CreateOrUpdatePerMonitorWindow(), free in cleanup                                        │
│ - Add DrawColor enum and extend DrawString() to accept it instead of is_warning boolean                                             │
│ - Add FillRect() and DrawRect() helpers                                                                                             │
│                                                                                                                                     │
│ Step 4: CODE MATRIX rendering                                                                                                       │
│                                                                                                                                     │
│ - Implement DrawCodeMatrix() — draws 5x5 grid of hex pairs                                                                          │
│ - Cells in active row/column get highlight background (FillRect with COLOR_CYBER_HIGHLIGHT)                                         │
│ - Current cell (next in hack sequence) gets bright yellow highlight                                                                 │
│ - Already-used cells get dimmed color                                                                                               │
│ - Normal cells get cyber green                                                                                                      │
│                                                                                                                                     │
│ Step 5: BUFFER rendering                                                                                                            │
│                                                                                                                                     │
│ - Implement DrawBufferSection() — draws [ BD ][ 1C ][ __ ][ __ ]... slots                                                           │
│ - Filled slots show hex code in yellow, empty slots show blanks in dim color                                                        │
│                                                                                                                                     │
│ Step 6: BREACH TIME REMAINING                                                                                                       │
│                                                                                                                                     │
│ - Implement DrawTimerSection() — header text, MM:SS countdown, progress bar                                                         │
│ - Timer maps to deadline - time(NULL) (existing auth timeout)                                                                       │
│ - Progress bar: DrawRect outline + FillRect proportional fill                                                                       │
│ - Turns red when < 30 seconds                                                                                                       │
│                                                                                                                                     │
│ Step 7: Selection Highlight & Grid Logic in Prompt()                                                                                │
│                                                                                                                                     │
│ - Add GridState to priv struct in Prompt()                                                                                          │
│ - Initialize grid state (axis=HORIZONTAL, active_row from first hack sequence entry)                                                │
│ - On keypress (default case): advance current_step, record hex code in buffer, toggle axis, update active_row/col from next hack    │
│ sequence entry                                                                                                                      │
│ - On backspace: rewind current_step and buffer_count, recompute axis/position                                                       │
│ - On Ctrl-U: reset grid state entirely                                                                                              │
│ - Replace DisplayMessage() call with DisplayBreachProtocol(&priv.grid, time_remaining, prompt_timeout)                              │
│                                                                                                                                     │
│ Step 8: SEQUENCE REQUIRED TO UPLOAD                                                                                                 │
│                                                                                                                                     │
│ - Implement DrawSequenceSection() — draws 3 target sequences with names and hex codes                                               │
│ - Implement CheckSequenceCompletion() — checks if each target appears as contiguous subsequence in buffer                           │
│ - Called after each keypress/backspace                                                                                              │
│ - Completed sequences change to COLOR_CYBER_COMPLETE (#00ffcc)                                                                      │
│                                                                                                                                     │
│ Step 9: Integration & Layout                                                                                                        │
│                                                                                                                                     │
│ - Implement DisplayBreachProtocol() — computes region size, calls UpdatePerMonitorWindows, then draws all sections                  │
│ - Compute layout based on font metrics (cell_w from TextWidth, cell_h from TextAscent+Descent)                                      │
│ - Matrix on left, timer+sequences on right, buffer on top                                                                           │
│ - Keep DisplayMessage() as simplified fallback for non-grid states ("Processing...", "Error", etc.)                                 │
│                                                                                                                                     │
│ Key Design Decisions                                                                                                                │
│                                                                                                                                     │
│ - Hardcoded path: HACK_SEQUENCE is a fixed path. Every keypress advances along it regardless of which key. The visual is            │
│ decorative; actual password flows to authproto normally.                                                                            │
│ - Axis alternation: Step 0=horizontal, step 1=vertical, step 2=horizontal, etc. Per the Cyberpunk 2077 mechanic.                    │
│ - Color via GC arrays: One GC per color per window (6 colors x 16 windows = 96 GCs max).                                            │
│ - No double-buffering: Matches existing auth_x11 pattern. XClearWindow → draw → XFlush.                                             │
│ - Security preserved: priv struct is MLOCK_PAGE'd and memset'd. GridState is non-secret but cleared anyway.                         │
│                                                                                                                                     │
│ Verification                                                                                                                        │
│                                                                                                                                     │
│ # Build                                                                                                                             │
│ make auth_x11_grid                                                                                                                  │
│                                                                                                                                     │
│ # Test (from another terminal or via Xephyr)                                                                                        │
│ XSECURELOCK_AUTH=auth_x11_grid xsecurelock                                                                                          │
│                                                                                                                                     │
│ # Or for development, set XSECURELOCK_DEBUG_ALLOW_LOCKING_IF_INEFFECTIVE=1                                                          │
│                                                                                                                                     │
│ Test checklist:                                                                                                                     │
│ - Grid renders with hex codes in 5x5 layout                                                                                         │
│ - Buffer slots appear and fill on keypress                                                                                          │
│ - Row/column highlight alternates on each step                                                                                      │
│ - Timer counts down and progress bar shrinks                                                                                        │
│ - Target sequences highlight when completed                                                                                         │
│ - Backspace rewinds grid state                                                                                                      │
│ - Ctrl-U resets everything                                                                                                          │
│ - Escape cancels auth                                                                                                               │
│ - Enter submits password (actual auth works)                                                                                        │
│ - Multi-monitor rendering works                                                                                                     │
╰─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
