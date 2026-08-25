# Installation Guide

## Summary

Upload the XMIT file `nfsd.v*.xmi` to MVS, to a dataset name
of your choice and then "receive" the XMIT data into a PDS, for example
`SYSS.NFSD.V0R1M0.DISTRIB`.

This will be a `RECFM=FB,LRECL=80` format dataset that contains --

* Install JCL.
* A sample started task JES2 stored procedure.
* A sample configuration which you can place in its own dataset,
  or `SYS*.PARMLIB`.
* XMIT format data to create the runtime load library.
* XMIT format data for the source datasets.

There are two executable programs which are used by the started task.

* **RESSOCK** - This program is similar to Jason Winter's **RESET**
  sample program. However, *RESSOCK* takes a list of port numbers for
  which sockets are to be reset. This program was very useful when NFSD
  was unstable and would crash leaving socket connections behind it.
  However, as NFSD (famous last words) is quite stable this probably
  isn't necessary at this time.
* **NFSD** - This is the executable for the NFS server as you might
  expect.

Note that the configuration file, or started task stored procedure
are not created/placed automatically (I consider that this would
be rude) so it is left for you to do this manually.

Note that the full source and development tree can be found on
github at

# Installation Steps

## Step 1 - Load the `nfsd_v*.xmi` file to MVS

Use FTP or your terminal emulator (such as the wonderful Vista TN3270)
to load the file to MVS. Then use RECV370 to create the distribution
dataset.

Note, that I had to create the dataset first for FTPD and a
IND$FILE transfer using Vista TN3270.

The full screen interface provided in MVS-TK5 works (option M.R).
The following inputs, with option 1 were successful.

```
--------------------   Receive XMIT File   ---------------------------------
OPTION  ===>

 1  Receive XMIT file on MVS to PDS/SEQ file                        HERC01
 2  Receive XMIT file on PC  to PDS/SEQ file                        PRECV372
                                                                    PXMI
 XMIT Input File
1 MVSFILE: SYSS.NFSD.V0R1M0.XMI
-or-
2 PCFILE:
  Reader Control for PCFILE
   HercRDR Jes2RDR DEVINIT Reset Command


 MVS Output File
  DSN: SYSS.NFSD.V0R1M0.DISTRIB                     VOL: TSO003  UNIT: SYSDA
  SPACE: (CYL,(5,2,3))               DISP: (NEW,CATLG)

 JOB STATEMENT INFORMATION
  ===> //HERC01N JOB CLASS=A,
  ===> //     MSGLEVEL=1,MSGCLASS=X,NOTIFY=HERC01
  ===>
  ===>

```


In these instructions I am assuming this output dataset is --
`SYSS.NFSD.V0R1M0.DISTRIB`.

## Step 2 - Create the NFSD load library

Customize the install job `SYSS.NFSD.V0R1M0.DISTRIB(INSTALL)`. You will
likely wish to change --

* Job card information such as job name, job class, message class,
  notify userid etc.
* The target load library dataset to be created with the executables
  for NFSD.

The NFSD server does not need to run authorized, but it will need to
have appropriate permissions to its configuration and the datasets to
be exported (the datasets that are to be remotely mounted and accessed).

## Step 3 - Customize, or create the started task stored procedure

The member `SYSS.NFSD.V0R1M0.DISTRIB(NFSD)` is a sample stored procedure
you can copy to a suitable JES2 procedure library. If you are using
the MVS TK5 system then I suggest using `SYS2.PROCLIB`.

* Update the load library dataset as required.
* Uncomment and change SYSOUT class, or destination of SYSUDUMP as
  appropriate.
* Update the source of the configuration file (see below).

## Step 4 - Customize, or create the NFSD configuration file

The member `SYSS.NFSD.V0R1M0.DISTRIB(CONFIG)` contains a sample
configuration for the NFSD server. You can copy this to either a
sequential dataset, or a member of a PDS. The dataset can be record
format FB or VB. You way wish to keep the the configuration as a
member of `SYS1.PARMLIB`, such as `NFSDCFG0`.

The sample JES2 stored procedure assume exactly this setup.

## Step 5 - Start NFSD

Enter a start command on the MVS console to run NFSD, `S NFSD`.

# MVS Modify Commands

The logging level and options can be changed via a MVS modify command.

For example to set the level of messages being output to the log use
the command --

```
F NFSD,SET LOGLVL INFO
```

The log levels (INFO is show above) are --
* DEBUG
* TRACE
* INFO
* WARN
* ERROR
* FATAL

You can separately set the level of messages that are output as
WTO messages to the console.

```
F NFSD,SET WTOLVL WARN
```

The above command will cause NFSD to output only WARN messages and worse,
to the MVS console.

You can also independently set logging level for different NFS operations.

```
F NFSD,SET LOGLVL DEBUG PROC=WRITE
```

This would cause any NFS WRITE operation to output DEBUG level messages,
overriding (for example) a global INFO logging level.

The NFS operations that can be logged using the `PROC=` keyword are --
* GETATTR
* SETATTR
* LOOKUP
* ACCESS
* READ
* WRITE
* CREATE
* REMOVE
* RENAME
* READDIR
* READDIRPLUS (or the alias of RDIRPLUS)
* FSSTAT
* FSINFO
* PATHCONF
* COMMIT
* NULL

# Shutting down NFSD

Issue an MVS STOP command to shutdown NFSD --

```
P NFSD
```
