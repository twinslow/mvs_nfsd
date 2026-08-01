# Predicting if a given member can be written to PDS

The goal of this change is for the NFSD server to predict, on each NFS write request,
whether or not there is sufficient space remaining in the PDS to satisfy the request.

## Info we need on the PDS

* Record Format F(B), or V(B).
* Logical record length.
* Block size.
* Number of (full) blocks per track when PDS member is writtent to disk.
* Size of secondary extents (or no secondary extents) and how many secondary
  extents are already allocated.
* The number of tracks currently allocated in the PDS.
* The TTR of the last block that was written to the PDS.
* The amount of space remaining in the last track that contains the last
  written block.

# 1. MVSDSCB Utility

The current utility program will need to return two additional fields from the VTOC
DSCB Format 1 for the dataset -- The DS1LSTAR and DS1TRBAL fields.

The information from these fields will be part of the new `dataset_dscb_info_t` structure
(see next section).

# 2. Config file processing

* Get the DSCB information from the VTOC for all exported datasets.
* Call the MVSDSCB utility to get this information.
* Reject exported datasets if DSORG is not PO.
* Reject exported datasets with error if they are not RECFM=F/FB/V/VB.
* Reject any exported datasets for which we could not get the DSCB info
  (any error that came back from MVSDSCB).
* Save DSCB information in a new structure type `dataset_dscb_info_t` (to be defined)
  which will be a member of `pds_dataset_t`.
* I have deliberately not prefixed the new structure with "pds" in case we later start
  supporting the export of sequential datasets.
* `dataset_dscb_info_t` should contain DSORG, RECFM, BLKSIZE, LRECL, all space allocation
  information, allocated extends, current track allocation, secondary allocation size,
  creation date and referenced date, device type etc. In short just about everything.
  It should the current DS1LSTAT information field from the DSCB so we know the location
  of the last block written to the PDS.
* The new structure type `dataset_dscb_info_t` will be "C" friendly, unlike the `mvs_dscb_info_t`
  which is based around the fields coming from the DSCBs in the VTOC.
* Calculate the number of full blocks that can be written to a track in this PDS, based on
  block size and device type. This count will be added to a new member of the `pds_dataset_t`
  structure.

# 3. NFS Write Processing

When each NFS WRITE request is processed, it will update an estimation of the blocks
that will be needed in the PDS to write the member to disk.

After updating the estimated total block count and size of last block, it will be
determined (estimated) if the member could currently be written to the PDS given
the dataset space allocation and the current end of the PDS as written.

**Multiple pending writes for same dataset:** This decision needs to take into account
that there may be multiple pending writes for the same dataset. This need to be included
in the decision process.

## 3.1 Can this write request be satisfied?

* First, it will be necessary to get the predicted full block count for

* **VTOC Format 1 DSCB (DS1LSTAR):** Stores the TTR (Relative Track and Record) of the last block
  written in the entire data set.
* **Directory (STOW / BLDL):** While the DSCB points to the absolute physical end of the data
  area used so far (DS1LSTAR), individual member start locations are tracked via 3-byte TTR pointers
  inside the PDS directory entries.
* **

## 3.2 If determined member could NOT be written

If it is determined (estimated) that the member could not be written then the WRITE will be
rejected with a NOSPC error.

## 3.3 If determined member could be written
If it is determined (estimated) that the member could be written, the new WRITE data will be
accepted and added to the slot, either in memory, or out to the spill temporary dataset. In
this case an OK response will be sent.

## 2.1 From the data to be written

This calculation can be done for each segment of NFS WRITE operations and accumulated for a
current member total.

From these calculations we get a number of full or partial blocks needed to write the
member.



### For RECFM=F(B) datasets

* Count number of records (by linefeeds) of regular length.
* Count records that exceed LRECL and will be wrapped.
* Gives total number of logical records to be written, which translates to a number.
  of whole blocks and size of last partial block (if any).

### For RECFM=V(B) datasets

* Count number of records (by linefeeds)
* Have to work through each text line and see what will fit in each block of given size.
* Accound for RDW and BDW 32 bit words for each logical record and each block.
* Will give a count of full blocks and a size for the last partial block if any.
* Blocks will be variable size, but can be assumed to be the maximum size for track
  usage calculation, which is the worst case scenario.


