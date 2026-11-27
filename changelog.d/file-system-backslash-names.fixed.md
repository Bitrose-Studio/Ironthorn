- On Linux, `fs::listDirectory` reports a name that holds `\` with `validName == false`. Every
  `file_system` function takes `\` for a separator, so such a name can't address its entry, and a
  caller that joined a name like `..\..` into a path could reach a folder outside the one it
  listed.
