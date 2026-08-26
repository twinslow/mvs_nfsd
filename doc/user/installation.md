# Installation Guide

## Summary

Upload the XMIT file `nfsd.v*.xmi` to MVS, to a dataset name
of your choice and then "receive" the XMIT data into a PDS, for example
`SYSS.NFSD.V0R2M0.DISTRIB`.

This will be a partitioned dataset, `RECFM=FB,LRECL=80` format that
contains --

* Install JCL.
* A sample started task JES2 stored procedure.
* A sample configuration which you can place in its own dataset,
  or a member in `SYS*.PARMLIB`.
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

Note that the installation job does not copy the configuration file,
or started task stored procedure. It is left for you to do this
manually.

Note that the full source and development tree can be found on
github at --

https://github.com/twinslow/mvs_nfsd

# Installation Steps

## Step 1 - What HLQ are you going to use?

In the examples for this installation document I have been using a
HLQ of *SYSS*. This is not already in use on a standard *MVS TK5*
installation.

You should avoid having the install datasets cataloged in the MVS
master catalog, as this is may result in the catalog entries being
lost after maintenance.

There is a job provided in *MVS TK5* which defines all catalog
aliases, pointing specific HLQ values to *USER catalogs*. This job
is *SYS2.JCLLIB(ALLALIAS)*. I like to update that job with any new
HLQs, as a record of what has been defined. The *IDCAMS* command is
shown below to create the alias for *SYSS*.

```
DEF ALIAS(NAME('SYSS') REL('SYS1.UCAT.TK5'))
```

You can copy the *IDCAMS* command for the *define alias* and execute
it directly via TSO option 6. Note that executing the command
provides no output back to the screen, which is a little disconcerting.

You can verify that the command did indeed create the alias using the
following command in option 6 (assuming you are using the *SYSS* HLQ) --

```
LISTC ENT('SYSS') ALL
```

You'll see output something like this ...
```
ALIAS --------- SYSS
     IN-CAT --- SYS1.MCAT.TK5
     HISTORY
       RELEASE----------------2
     ASSOCIATIONS
       USERCAT--SYS1.UCAT.TK5
***
```

## Step 2 - Create the DISTRIBution PDS from the XMIT file

There are a multitude of ways you can get the XMIT file onto your
MVS system. If you are using *MVS TK5* I recommend using the full
screen interface to *RECV370* provided as option *M.R*.

Option 2 will generate a batch job that will initialize the hercules
reader device with the specified (PC or Linux) file and then execute
the RECV370 program to read the *card deck* directly from the reader
unit device specified.

Below are screen shots for both options (you only need to use one
of them).

In these instructions I am assuming this output dataset is --
`SYSS.NFSD.V0R2M0.DISTRIB`.

### If the XMIT file is NOT on MVS

This option submits a job that will --

1. Issue a DEVINIT command to hercules, to place the specified XMIT format
   (EBCDIC/BINARY) file on the specified reader device.
2. Run RECV370 to process the XMIT file and create the output dataset.
3. Issue another DEVINIT command to hercules, to reset the reader with
   no file loaded.

Note the use of forward slashes in the PC file name.

```
--------------------   Receive XMIT File   ---------------------------------
OPTION  ===> 2

 1  Receive XMIT file on MVS to PDS/SEQ file                        HERC01
 2  Receive XMIT file on PC  to PDS/SEQ file                        PRECV372
                                                                    PXMI
 XMIT Input File
1 MVSFILE:
-or-
2 PCFILE: C:/mvs-clean/nfsd_v0r2m0/nfsd_v0r2m0.xmi
  Reader Control for PCFILE
   HercRDR Jes2RDR DEVINIT Reset Command
   10C             DEVINIT 10C

 MVS Output File
  DSN: SYSS.NFSD.V0R2M0.DISTRIB                     VOL: TSO003  UNIT: SYSDA
  SPACE: (CYL,(5,2,3))               DISP: (NEW,CATLG)

 JOB STATEMENT INFORMATION
  ===> //HERC01R JOB CLASS=A,
  ===> //      MSGLEVEL=1,MSGCLASS=X,NOTIFY=HERC01
  ===>
  ===>

```

### If the XMIT file is already on MVS

After using FTP or your terminal emulator (such as the wonderful Vista TN3270)
to load the file to MVS, you can use *RECV370* to create the distribution
dataset. **Note,** I had to create the dataset first for both FTPD and
IND$FILE transfer using Vista TN3270.

In the screen shot below, I've shown the selected options using
TK5 full screen interface to *RECV370* that will submit a batch job
and create the distribution dataset.

```
--------------------   Receive XMIT File   ---------------------------------
OPTION  ===> 1

 1  Receive XMIT file on MVS to PDS/SEQ file                        HERC01
 2  Receive XMIT file on PC  to PDS/SEQ file                        PRECV372
                                                                    PXMI
 XMIT Input File
1 MVSFILE: SYSS.NFSD.V0R2M0.XMI
-or-
2 PCFILE:
  Reader Control for PCFILE
   HercRDR Jes2RDR DEVINIT Reset Command


 MVS Output File
  DSN: SYSS.NFSD.V0R2M0.DISTRIB                     VOL: TSO003  UNIT: SYSDA
  SPACE: (CYL,(5,2,3))               DISP: (NEW,CATLG)

 JOB STATEMENT INFORMATION
  ===> //HERC01N JOB CLASS=A,
  ===> //     MSGLEVEL=1,MSGCLASS=X,NOTIFY=HERC01
  ===>
  ===>

```

## Step 3 - Create the NFSD source and load library datasets

Customize the install job `SYSS.NFSD.V0R2M0.DISTRIB(INSTALL)`. You will
likely wish to change --

* Job card information such as job name, job class, message class,
  notify userid etc.
* The dataset name of your `DISTRIB` PDS.
* The target output dataset prefix for load library and source datasets.
* The target volume for these datasets.

Note that these datasets are created as DISP=(NEW,CATLG) and the job
will fail if they already exist.

The NFSD server does not need to run authorized, but it will need to
have appropriate permissions to its configuration and the datasets to
be exported (the datasets that are to be remotely mounted and accessed).

In the *MVS TK5* environment, the started task will run under the `STC`
userid, and have a group of `STCGROUP`. The permissions in the default
setup should be suitable, allowing read/write access to any dataset
you wish to export. See *TK5* and *RAKF* documentation for more
information on this. If you have RAKF access failures, check the
settings in *SYS1.SECURE.CNTL(PROFILES)*.

## Step 4 - Customize, or create the started task stored procedure

The member `SYSS.NFSD.V0R2M0.DISTRIB(NFSD)` is a sample stored procedure
you can copy to a suitable JES2 procedure library. If you are using
the MVS TK5 system then I suggest using `SYS2.PROCLIB`.

* Update the load library dataset as required.
* Uncomment and change SYSOUT class, or destination of SYSUDUMP as
  appropriate.
* Update the source of the configuration file (see below).

## Step 5 - Customize, or create the NFSD configuration file

The member `SYSS.NFSD.V0R2M0.DISTRIB(CONFIG)` contains a sample
configuration for the NFSD server. You can copy this to either a
sequential dataset, or a member of a PDS. The dataset can be record
format FB or VB. You way wish to keep the the configuration as a
member of `SYS1.PARMLIB`, such as `NFSDCFG0`.

The sample JES2 stored procedure assume exactly this setup.

Note that at this time the dataset names in the export list cannot
include wilcards to perform generic exports based on system
catalog. You must explicitly list each dataset you wish to export.

## Step 6 - Start NFSD

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
