# Build, test and release process

# 1. Build and Test

1. Commit and push everything in current branch (master).
2. Upload latest code to MVS
3. Run unit test jobs
    * Submit `TESTRUN`
    * Submit `TESTEXP`
    * Verify max condition code 0000 for both jobs.
4. Run build job
    * Submit `MAKEJCC`
    * Verify max condition code 0000 for build.
    * Restart NFSD server.
5. Run integration tests
    * On Windows
    * On Linux
    * On MacOS

# 2. Build Release

1. Assign release designation -- VnRnMn
2. Update all jobs docs that refer to release number
    * Do a *replace in files* from regex "V\d+R\d+M\d+" to "V20R3M0" etc.
3. Recreate PDF files for installation and user_guide markdown files.
4. Copy ./doc/user/installation.md to ./jcl/$readme.jcl
5. Upload modified files to MVS.
6. Run the job `MKDISTR`. Verify max condition code zero.
7. Create new build directory `./build/mvs/nfsd_vnrnmn`
8. Download the newly created XMI file 'SYSS.NFSD.VnRnMn.XMI' as nfsd_vnrnmn.xmi
9. Copy contents of `./doc/user` to build/release directory
10. Create a zip file of the directory called `nfsd_vnrnmn.zip`

# 3. Test Release

1. Create new MVS-TK5 distribution install
2. Get release zip file
3. Unzip file to appropriate directory
4. Upload the XMI file
    * Login to HERC01 - Create destination dataset for XMI file
    * Start FTPD
    * Use FTP client for binary upload of local XMI to new MVS dataset.
5. Run RECV370 to create DISTRIB dataset.
6. Install the NFSD server
    * Run the `INSTALL` job.
    * Copy the configuration file to 'SYS1.PARMLIB(NFSDCFG0)'.
    * Copy the started task JCL proc to 'SYS2.PROCLIB'.
    * Customize configuration exports and STC JCL.
7. Start NFSD

# 4. Test Mount from Windows

1. Mount from the configured export
2. Test --
     * Directory listing
     * Edit a file (existing and new).

# 5. Create Release Tag in Repo

Create a release tag *VnRnMn* to indicate the release point in the repository.

Commit and Push etc.






