# Local usage history

UxAir stores usage statistics in the app's private `state/usage-v1` file. The versioned format migrates prior `UXPLAY_USAGE_V1` lifetime counters and up to 16 summaries into the bounded `UXAIR_USAGE_V2` format on the next save. Migration does not invent old daily usage; chart coverage begins at the upgrade date.

The file is limited to 64 KiB, the recent-session list to 100 completed sessions, and daily aggregates to a rolling 365 days. Session summaries contain UTC start time, duration, mode, incoming dimensions/codec and receiver counters. They do not contain sender names, addresses, media titles, PINs or credentials. Pairing keys, receiver identity, video settings and logs are separate and are never reset with History.

History shows lifetime session/playback totals, last-30-day mirroring and music durations, a current-calendar-month daily chart, and a current-calendar-year monthly chart. Playback distribution percentages use last-30-day duration and round to a sum of 100 when there is data. Calendar coverage indicators are described below.

An unreadable or oversized history file is preserved and is not overwritten by normal saves. The owner can explicitly confirm **Reset history** to replace it with an empty history. A session already playing is not stopped by reset; if it later completes successfully, it is recorded as a new post-reset session.

## Calendar charts (UxAir 1.2.0; introduced during internal 1.1.0 development)

History charts use the same UTC day boundaries as storage. The left chart shows days 1 through the end of the current calendar month; the right shows January through December of the current year. Both refresh when the UTC date changes. Summary breakdowns and the duration-based distribution remain explicitly labelled Last 30 days; the top summary remains lifetime.

Future positions are blank. A dash means unavailable history, a small dot means a covered date with zero activity, and `*` means missing days within an otherwise covered month. Blue labels identify the current day/month. A current month with complete records to date does not receive a missing-data star. The existing 365-day retention and 64 KiB format are unchanged; expired dates are never extrapolated.

The chart axis rounds upward to a 1/2/5 scale using the full maximum. Stacked heights are bounded to the plot, and SDL clips all bars to the plot rectangle while restoring the previous clip afterward. Both ticks use one time unit per chart (`s`, `min`, or `h`). Host SDL previews exercise the actual renderer with a substitute font; physical B7/LG font validation remains required.

Hardware acceptance: inspect October/day positioning (or the current UTC calendar), stacked bars and readable legends with short and long sessions; check five recent rows and Reset History navigation; confirm About identifies Matej Skuhala (@depius) and retains upstream credits and the donation artwork.
