# Installation Guide

## Summary

Upload the XMIT file `nfsd_distribution.xmi` to MVS, to a dataset name
of your choice and then "receive" the XMIT data into a PDS, for example
`SYSS.NFSD.DISTRIB`.
This will be a `RECFM=FB,LRECL=80` format dataset that contains --

* Install JCL.
* A sample started task JES2 stored procedure.
* A sample configuration which you can place in its own dataset,
  or `SYS*.PARMLIB`.
* XMIT format data to create the runtime load library.

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

# Installation Steps

## Step 1 - Load the `nfsd_distribution.xmi` file to MVS

Use FTP or your terminal emulator (such as the wonderful Vista TN3270)
to load the file to MVS. Then use RECV370 to create the distrubution
dataset.

In these instructions I am assuming this dataset is --
`SYSS.NFSD.DISTRIB`.

## Step 2 - Create the NFSD load library

Customize the install job `SYSS.NFSD.DISTRIB(INSTALL)`. You will likely
wish to change --

* Job card information such as job name, job class, message class,
  notify userid etc.
* The target load library dataset to be created with the executables
  for NFSD.

The NFSD server does not need to run authorized, but it will need to
have appropriate permissions to its configuration and the datasets to
be exported (the datasets that are to be remotely mounted and accessed).

## Step 3 - Customize, or create the started task stored procedure

The member `SYSS.NFSD.DISTRIB(NFSD)` is a sample stored procedure
you can copy to a suitable JES2 procedure library. If you are using
the MVS TK5 system then I suggest using `SYS2.PROCLIB`.

* Update the load library dataset as required.
* Uncomment and change SYSOUT class, or destination of SYSUDUMP as
  appropriate.
* Update the source of the configuration file (see below).

## Step 4 - Customize, or create the NFSD configuration file

The member `SYSS.NFSD.DISTRIB(CONFIG)` contains a sample
configuration for the NFSD server. You can copy this to either a
sequential dataset, or a member of a PDS. The dataset can be record
format FB or VB. You way wish to keep the the configuration as a
member of `SYS1.PARMLIB`, such as `NFSDCFG0`.

The sample JES2 stored procedure assume exactly this setup.

## Step 5 - Start NFSD

Enter a start command on the MVS console to run NFSD, `S NFSD`.




