- The editor application, `ironthorn_editor --project <dir>`: the editor shell over a project, with
  the layered configuration (`--user-dir` for the user layer), the UI shader compiled at start-up
  for the device, and `--automation <script>` to run an automation script and exit 0 or 1. CTest
  runs it on SDL's offscreen driver, on the null device and on the software GPU.
