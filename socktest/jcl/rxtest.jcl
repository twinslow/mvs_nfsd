//TONYWRXT JOB (SOCKT),
//            'Socket recv reproducer',
//            CLASS=A,NOTIFY=TONYW,REGION=8M,
//            MSGCLASS=X,MSGLEVEL=(1,1)
//*
//********************************************************************
//*
//* Name: RXTEST
//*
//* Desc: Compile, link and run the socket receive reproducer.
//*
//*       The program listens on a TCP port, accepts ONE connection,
//*       and reads framed self-describing messages until the sender
//*       sends a zero-length frame.  It then prints a summary.
//*
//*       It verifies that the bytes handed back by recv() are the
//*       bytes that were sent, in order and exactly once.  See
//*       socktest/src/rxtest.c for the full description of the
//*       defect being demonstrated.
//*
//* Before running:
//*   1. Upload socktest/src/rxtest.c to TONYW.SOCKTEST.C(RXTEST)
//*   2. Submit this job.  It will WAIT at accept() until the
//*      sender connects, so start the sender promptly:
//*
//*         python3 sender.py --host <mvs-host> --port 5555
//*
//*   3. Read the summary in the GO step's STDOUT.
//*
//* PARM.GO options:
//*      -p nnnn   port to listen on          (default 5555)
//*      -f        use the FIONREAD strategy  (never request more
//*                bytes than are actually available)
//*      -v        one line per message
//*
//* Run it BOTH ways with identical sender settings: the plain loop
//* first, then with -f.  If the plain loop reports corruption and
//* -f does not, the fault is in the short-read path and clamping
//* the request to FIONREAD is a viable workaround.
//*
//* NOTE: the step names below (COMPILE / PRELINK / LKED / GO) follow
//* the JCCCLG procedure as used by tests-jcl/testexp.jcl.  If your
//* JCCCLG differs, adjust the qualified DD names to match.
//*
//********************************************************************
//*
//JCCCLG  EXEC JCCCLG,INFILE='TONYW.SOCKTEST.C(RXTEST)',
//             PARM.PRELINK='-s //DDN:L //DDN:O //DDN:I',
//             JOPTS='-D_MVS -D__MVS__ -o -list=//DDN:SYSPRINT'
//COMPILE.SYSPRINT DD SYSOUT=*
//LKED.SYSPRINT    DD SYSOUT=*
//*
//* ---- run: plain recv loop.  Add -f for the FIONREAD strategy ----
//*
//GO.SYSPRINT DD SYSOUT=*
//GO.STDOUT   DD SYSOUT=*
//GO.STDERR   DD SYSOUT=*
//GO.SYSIN    DD DUMMY
//
