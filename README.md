1.0.0: blocks the crash report upload added in the 2026-10-28 update (build 0x6ae218ec). The game's crash handler
runs its uploader on a thread (connects to geometa.co.uk, /crash_report_api/submit, then uploads the report's .json,
.log and .dmp files); the uploader now returns at once. Reports stay in the crashes folder, the crash message still
shows. The uploader is found at runtime (the only code that loads the "geometa.co.uk" string), so it doesn't depend
on fixed addresses; if a later build changes that, the mod logs "NOT blocked" and its status says so.
