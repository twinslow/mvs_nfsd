# Contributing to dino_nfs

Thanks for your interest in contributing! dino_nfs is a minimal NFSv3 server
for MVS 3.8J on the Hercules emulator. Contributions of all kinds are
welcome — bug fixes, new NFS procedures, VFS features, tests, and docs.

## License

The project is licensed under the [MIT License](LICENSE). By contributing,
you agree that your contributions will be licensed under the same terms.

## Developer Certificate of Origin (DCO)

We use the **Developer Certificate of Origin** to keep a clear record that
every contributor has the right to submit their work under the project
license. It is lightweight — there is no separate agreement to sign and you
keep the copyright to your contributions.

To accept a contribution we require that **every commit is signed off**. The
sign-off is a single line at the end of the commit message:

```
Signed-off-by: Your Real Name <your.email@example.com>
```

Git adds this line automatically when you pass `-s` (or `--signoff`):

```
git commit -s -m "Fix READDIRPLUS cookie handling"
```

The name and email must be your real identity and match your Git
configuration:

```
git config --global user.name  "Your Real Name"
git config --global user.email "your.email@example.com"
```

Adding the `Signed-off-by` line certifies that you agree to the DCO below.

### DCO 1.1

```
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 The Linux Foundation and its contributors.

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.


Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

### Fixing a missing sign-off

If a commit is missing its `Signed-off-by` line, amend it:

```
git commit --amend -s --no-edit
git push --force-with-lease
```

For several commits, rebase and sign them off:

```
git rebase --signoff main
```

## Submitting changes

1. Fork the repository and create a topic branch off `master`.
2. Make your change; keep commits focused and sign each one off (`-s`).
3. Follow the existing code style (see below).
4. Open a pull request describing what changed and why.

## Code style

dino_nfs targets the JCC C compiler on MVS 3.8J, so contributions to the
C sources must stay **C89-compatible**:

- All variable declarations precede executable statements within a block.
- Prefer block comments `/* ... */` (the codebase convention).
- Avoid C99/C11-only features; keep to what the JCC compiler accepts.
- The code is built and run on MVS (Hercules) — see the README for the JCL
  build. There is no local build for the MVS target, so review changes for
  correctness by reading the source.

## Reporting bugs

Open an issue with: what you did, what you expected, what happened, and any
relevant server log output (run with `-v` for debug logging) and client-side
details (OS, mount options).
