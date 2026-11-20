# Third-party code

Every dependency is fetched at configure time as an archive pinned by URL and SHA-256
(`cmake/IronthornDependencies.cmake`). None is committed to the repository. Adding one needs:

- a license check (permissive licenses only in shipped code; no GPL/LGPL/CC BY-SA);
- a maintenance check;
- a platform check, including consoles;
- a line in the table below.

| Library | Version | License | Used for | Shipped in games |
|---|---|---|---|---|
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT | unit tests | no |
