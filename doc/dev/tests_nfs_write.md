# Testing Windows 11 Pro

The /exports mount is mounted to X:

type C:\Users\Tony_\dev\test1.cntl

```
17:15:28.95 C:\Users\tony_>type C:\Users\Tony_\dev\test1.cntl
This is a small test file for NFS: Line 1
This is a small test file for NFS: Line 2
This is a small test file for NFS: Line 3
This is a small test file for NFS: Line 4
This is a small test file for NFS: Line 5
This is a small test file for NFS: Line 6
This is a small test file for NFS: Line 7
This is a small test file for NFS: Line 8
This is a small test file for NFS: Line 9
This is a small test file for NFS: Line 10

17:17:46.90 C:\Users\tony_>
```

## Test 1W.01 - Local file server

Copy local file C:\Users\Tony_\dev\test1.cntl to X:\temp.testproj.cntl 

Result -- Pass

```
C:\Users\tony_\dev>copy test1.cntl x:\temp.testproj.cntl
        1 file(s) copied.

C:\Users\tony_\dev>
```

On MVS console I see --
```
/16.52.20 STC 1927  +[INFO ] pdsflush_slot: stowed TEMP.TESTPROJ.CNTL(TEST1), 431 bytes
```

## Test 1W.02 - Verify file content on TSO/ISPF

Result -- Pass

## Test 1W.03 - List directory

dir x:\temp.testproj.cntl

Result -- Pass

```
C:\Users\tony_\dev>dir x:\temp.testproj.cntl
 Volume in drive X has no label.
 Volume Serial Number is 0000-0001

 Directory of x:\temp.testproj.cntl

05/18/2026  10:25 PM               267 hellow.cntl
05/18/2026  10:24 PM                22 temp.cntl
07/06/2026  09:53 AM               431 test1.cntl
               3 File(s)            720 bytes
               0 Dir(s)               0 bytes free

C:\Users\tony_\dev>
```

## Test 1W.04 - Retrieve new file

Retrieve new file

```
x:
cd \temp.testproj.cntl
type test1.cntl
```

Result -- Pass

```
C:\Users\tony_\dev>x:

X:\>cd temp.testproj.cntl

X:\temp.testproj.cntl>type test1.cntl
This is a small test file for NFS: Line 1
This is a small test file for NFS: Line 2
This is a small test file for NFS: Line 3
This is a small test file for NFS: Line 4
This is a small test file for NFS: Line 5
This is a small test file for NFS: Line 6
This is a small test file for NFS: Line 7
This is a small test file for NFS: Line 8
This is a small test file for NFS: Line 9
This is a small test file for NFS: Line 10

X:\temp.testproj.cntl>
```

## Test 1W.05 - Copy to a file with a file name that is an invalid member name

|Target file name|Result                                         |Pass/Fail|
|----------------|-----------------------------------------------|---------|
|test12345.cntl  | The filename or extension is too long         | Pass    |
|1test.cntl      | The filename or extension is too long         | Pass    |
|test-1.cntl     | The filename or extension is too long         | Pass    |
|test_1.cntl     | The filename or extension is too long         | Pass    |
|test1.2.cntl    | The filename or extension is too long         | Pass    |

## Test 1W.06 - Copy to a file with a file name that is a valid member name

|Target file name|Result                                         |Pass/Fail|
|----------------|-----------------------------------------------|---------|
|test1234.cntl   |  1 file(s) copied.                            | Pass    |
|@test1.cntl     |  1 file(s) copied.                            | Pass    |
|#test1.cntl     |  1 file(s) copied.                            | Pass    |
|$test1.cntl     |  1 file(s) copied.                            | Pass    |
|$1.cntl         |  1 file(s) copied.                            | Pass    |
|test#1.cntl     |  1 file(s) copied.                            | Pass    |
|test$1.cntl     |  1 file(s) copied.                            | Pass    |
|test@1.cntl     |  1 file(s) copied.                            | Pass    |


# Testing Linux

The /exports mount is mounted to /mnt/src
```
sudo mount -t nfs -o rdirplus=force,noacl,nfsvers=3,nolock,tcp,acregmin=30,timeo=150 192.168.1.168:/exports /mnt/src
```

cat ~/test1.cntl

```
twinslow@dev-desktop-vm:~$ cat ~/test1.cntl
This is a small test file for NFS: Line 1
This is a small test file for NFS: Line 2
This is a small test file for NFS: Line 3
This is a small test file for NFS: Line 4
This is a small test file for NFS: Line 5
This is a small test file for NFS: Line 6
This is a small test file for NFS: Line 7
This is a small test file for NFS: Line 8
This is a small test file for NFS: Line 9
This is a small test file for NFS: Line 10

twinslow@dev-desktop-vm:~$
```


## Test 1L.01 - Local file server

Copy local file ~/test1.cntl to /mnt/src/temp.testproj.cntl 

Result -- Pass

```bash
twinslow@dev-desktop-vm:~$ cp ~/test1.cntl /mnt/src/temp.testproj.cntl
twinslow@dev-desktop-vm:~$
```

This message shown on MVS console
```
/15.45.27 STC 1927  +[INFO ] pdsflush_slot: stowed TEMP.TESTPROJ.CNTL(TEST1), 422 bytes
```

## Test 1L.02 - Verify file content on TSO/ISPF

Result -- Pass

On TSO/ISPF the member shows as ...
```
****** *************************************************
000001 This is a small test file for NFS: Line 1        
000002 This is a small test file for NFS: Line 2        
000003 This is a small test file for NFS: Line 3        
000004 This is a small test file for NFS: Line 4        
000005 This is a small test file for NFS: Line 5        
000006 This is a small test file for NFS: Line 6        
000007 This is a small test file for NFS: Line 7        
000008 This is a small test file for NFS: Line 8        
000009 This is a small test file for NFS: Line 9        
000010 This is a small test file for NFS: Line 10       
000011                                                  
****** *************************************************
```

Note the extra line (11) is correct as the file on Linux does end with an additional new line.

## Test 1L.03 - List directory

ls -l /mnt/src/temp.testproj.cntl

Result -- Pass

```
twinslow@dev-desktop-vm:~$ ls -l /mnt/src
total 12
drwxrwxrwx 2 root root 4096 Jul  4 10:22 temp.testproj.c
drwxrwxrwx 2 root root 4096 Jul  6 08:45 temp.testproj.cntl
drwxrwxrwx 2 root root 4096 Jul  4 10:22 temp.testproj.jcllib
twinslow@dev-desktop-vm:~$ ls -l /mnt/src/temp.testproj.cntl
total 0
-rwxrwxrwx 1 root root 267 May 18 22:25 hellow.cntl
-rwxrwxrwx 1 root root  22 May 18 22:24 temp.cntl
-rwxrwxrwx 1 root root 422 Jul  6 08:50 test1.cntl
twinslow@dev-desktop-vm:~$
```

## Test 1L.04 - Retrieve new file

Retrieve new file

```
cd /mnt/src/temp.testproj.cntl
cat test1.cntl
```

Result -- Pass

```
twinslow@dev-desktop-vm:/mnt/src/temp.testproj.cntl$ cat test1.cntl
This is a small test file for NFS: Line 1
This is a small test file for NFS: Line 2
This is a small test file for NFS: Line 3
This is a small test file for NFS: Line 4
This is a small test file for NFS: Line 5
This is a small test file for NFS: Line 6
This is a small test file for NFS: Line 7
This is a small test file for NFS: Line 8
This is a small test file for NFS: Line 9
This is a small test file for NFS: Line 10

twinslow@dev-desktop-vm:/mnt/src/temp.testproj.cntl$
```


