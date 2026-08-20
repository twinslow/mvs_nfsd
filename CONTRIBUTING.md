# Contributing to mvs_nfsd

Thanks for your interest in the project. mvs_nfsd is a minimal NFSv3 server
for MVS 3.8J on the Hercules emulator.

## Where things stand

**I am not looking to take on additional developers at the moment.** The
codebase is being developed by one person, and keeping it that way for now
suits both the pace and the fairly unusual environment it targets — the
build, test and debug cycle all happen on a running MVS 3.8J system, which
makes onboarding a slow business.

That is a decision about *code contributions*, not about people. Everything
below is genuinely welcome, and none of it is a formality.

## What I would very much like to hear

**Bug reports.** Easily the most valuable thing you can send. If something
does not work, I want to know about it.

**Enhancement ideas.** What is missing, what is awkward, what you expected to
work and did not. Ideas about how the server *should* behave are useful even
when they come with no code attached.

**Your experience of using it.** Which client you mount from, what you use it
for, what surprised you. This project has been exercised against a small
number of setups, so almost any new one tells me something.

Open a [GitHub issue](https://github.com/twinslow/mvs_nfsd/issues) for any of
the above — that is the best place for all of it.

## Reporting a bug

The single most useful thing in a bug report is **how to reproduce it**. A
problem I can recreate is usually most of the way to being fixed; one I
cannot may go nowhere, however clearly it is described.

Please include as much of this as you reasonably can:

1. **Steps to reproduce**, as short and specific as you can make them. The
   exact commands are ideal. If it only happens sometimes, say so and say
   roughly how often — an intermittent fault is a different hunt from a
   reliable one, and knowing which saves a lot of time.
2. **What you expected, and what actually happened.**
3. **Server log output** from around the failure. Raise the detail first with
   `set loglvl debug` in the `[Init]` section of the config, or at run time
   with `F NFSD,SET LOGLVL DEBUG`. Messages carry an ID such as `NFSIW500I` —
   including those verbatim helps me find the exact spot in the code.
4. **The console log** as well as STDERR if the server hung or ended
   abnormally. When a task dies, buffered log output can be lost, so the
   console (WTO) messages are sometimes all that survives.
5. **Your setup**: client OS and mount options, MVS level, and the RECFM and
   LRECL of the dataset involved.

Do not worry about having all of it. A partial report is far better than no
report, and I will ask if something important is missing.

## This may change

If the project turns out to be useful to people and there is interest in
working on it, I would be glad to revisit this and set up properly for
contributions — sign-off requirements, review process and the rest. Until
then, issues and ideas are the way in, and they are appreciated.

## License

The project is licensed under the [MIT License](LICENSE). Anything you send
in an issue — a suggestion, a snippet, a description of behaviour — is taken
as offered for use in the project under those same terms.
