# Starfish 1.2.0 physical acceptance

The owner completed final physical testing of the Starfish candidate at `9af36d1b2e3902bfa26ab349140af394b3b95a3f` on an LG OLED B7 running webOS 3.9 and reported that all tested functionality worked correctly. The accepted IPK has SHA-256 `25307e8ba375d16e92fbb892c5de28234699a53d38e5e0dcc92ab39d2f401f9d`. The owner did not provide a feature-by-feature result or a separate fresh-install and upgrade matrix for this run.

The earlier `148bc961` baseline has separate 30-minute playback and installer evidence. See the [branch review](B7_BRANCH_FINAL_REVIEW.md). A rebuild for the final documentation revision has its own build commit and checksum. The release check compares the shipped files to the accepted candidate and records the differences. NDL remains physically unvalidated on the current build.
