---
alwaysApply: true
---
# HANDOFF.md Maintenance Rule

This rule assumes HANDOFF.md already exists in the project root with its 5-section structure: Last Completed Step, Files Touched This Session, Build Status, Known Issues, Next Step.

If HANDOFF.md is missing, stop and flag this rather than
creating a new one — a missing file signals a setup problem to
report, not a gap to silently fill.

After completing any code-generation task (writing or modifying
.c/.h files, CMakeLists.txt, or sdkconfig):

Update the existing HANDOFF.md's "Build Status" and "Last Completed Step" sections to reflect the verified outcome — do not regenerate the entire file, only update the relevant sections