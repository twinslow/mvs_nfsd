# Unit Test Setup

Unit tests for the dino-nfs project use the
[munit](https://github.com/nemequ/munit) C testing framework.  Tests run on
Linux during development and on MVS via JCL for integration verification.

---

## Directory layout

```
tests/
    munit.c             munit framework source (vendored)
    munit.h             munit framework header (vendored)
    tstubs.c            Stub implementations of exports_count() / exports_get()
    tstubs.h            Declarations for the stub helper functions
    runall.c            Top-level runner: aggregates all test suites
    tmvsio.c            Tests for mvsio.c: mvs_path_type()
    tmvsio2.c           Tests for mvsio.c: mvs_get_pds_dsn_and_member()

tests-jcl/
    testrun.jcl         JCL to compile, link, and run tests on MVS
```

---

## Building and running on Linux

From the project root:

```bash
cc -std=c99 -Wall -I src -I tests \
   tests/runall.c tests/tstubs.c tests/tmvsio.c tests/tmvsio2.c \
   src/mvsio.c tests/munit.c \
   -o tests/runall

tests/runall
```

Add each new test module source file and its production dependency to the
command as new suites are introduced.

### Selecting tests to run

munit supports filtering by test name on the command line:

```bash
# Run a specific sub-suite
tests/runall /mvsio/mvs_path_type/single

# Run all tests whose name contains "member"
tests/runall --filter member

# List all registered tests without running them
tests/runall --list
```

---

## Building and running on MVS

`tests-jcl/testrun.jcl` compiles all modules and links them into a single
load module that is executed in the final step.

Submit the JCL from ISPF (or via the FTP scripts after uploading):

```
//TONYWUT1 JOB ...
```

The job:

1. Compiles production modules (e.g. `MVSIO`) from `TONYW.DINONFS.C` using
   the `JCCCMOD` proc.
2. Compiles test modules (`TSTUBS`, `TMVSIO`, `TMVSIO2`) from
   `TONYW.DINONFS.TESTS.C` using the `JCCCTST` proc.
3. Compiles and links `RUNALL` from `TONYW.DINONFS.TESTS.C`, linking in
   munit, production objects, and test objects, then runs the result.

Output is written to `SYSOUT=*` and visible in SDSF (class X).

---

## Framework: munit

munit is a single-file C testing framework (`munit.c` + `munit.h`).

Key concepts:

| Concept | Description |
|---|---|
| `MunitTest` | One test function plus optional setup/teardown hooks |
| `MunitSuite` | A named group of tests and/or nested sub-suites |
| `munit_suite_main()` | Entry point — runs the suite tree and returns an exit code |

### Test function signature

```c
static MunitResult test_something(const MunitParameter params[], void *data)
{
    /* declarations at top (C89 requirement for JCC) */
    int result;

    result = function_under_test(...);

    munit_assert_int(result, ==, EXPECTED_VALUE);
    munit_assert_string_equal(some_string, "expected");
    return MUNIT_OK;
}
```

Common assertion macros:

| Macro | Checks |
|---|---|
| `munit_assert_int(a, op, b)` | Integer comparison, e.g. `==`, `!=`, `<` |
| `munit_assert_size(a, op, b)` | `size_t` comparison |
| `munit_assert_string_equal(a, b)` | `strcmp(a, b) == 0` |
| `munit_assert_not_null(p)` | Pointer is not NULL |
| `munit_assert_null(p)` | Pointer is NULL |
| `munit_assert(expr)` | Generic boolean assertion |

### Fixture functions

Setup and teardown hooks run before and after each individual test.  They
are used to put the stub exports table into a known state:

```c
static void *setup_c_export(const MunitParameter params[], void *user_data)
{
    (void)params; (void)user_data;
    stub_clear_exports();
    stub_add_export("/dinonfs/src", "TEMP.DINONFS.C", "c");
    return NULL;   /* return value is passed as 'data' to the test */
}
```

---

## Stub infrastructure

Production code under test calls `exports_count()` and `exports_get()` from
`exports.c`.  In the test build, `exports.c` is **not** linked.  Instead,
`tests/tstubs.c` provides lightweight replacements backed by a static array.

### tstubs.h interface

```c
/* Reset the stub table to empty -- call at the start of each fixture */
void stub_clear_exports(void);

/* Add one export entry to the stub table */
void stub_add_export(const char *export_path,
                     const char *host_path,
                     const char *file_ext);
```

### How it works

`tstubs.c` owns a static `export_t s_exports[MAX_EXPORTS]` array and a
counter `s_nexports`.  `stub_clear_exports()` zeroes the counter;
`stub_add_export()` appends an entry.  `exports_count()` and `exports_get()`
read directly from the same array, satisfying the linker in place of
`exports.c`.

Because the stubs live in a single translation unit (`tstubs.c`) there are
no duplicate-symbol errors when multiple test modules are linked together.

---

## Test file structure

Each test module follows this layout:

```
tests/t<module>.c
│
├── #includes
│   ├── munit.h
│   ├── relevant src/ headers
│   └── tstubs.h
│
├── Fixture setup functions (one per test group)
│   └── static void *setup_<name>(...)
│
├── Test functions
│   └── static MunitResult test_<name>(...)
│
├── MunitTest arrays (one per sub-suite)
│   └── static MunitTest <group>_tests[] = { ... }
│
├── Sub-suite array
│   └── static MunitSuite sub_suites[] = { ... }
│
└── Exported top-level suite
    └── MunitSuite t<module>_suite = { ... }
```

The exported `MunitSuite` is the only non-static symbol; everything else is
file-scoped.

---

## Existing test suites

### tmvsio.c — `mvs_path_type()`

Tests whether a given NFS path resolves to a PDS dataset root, a PDS member,
or is not exported.

| Sub-suite | What is tested |
|---|---|
| `/single` | Exact match, member paths, and non-matching paths against one export |
| `/multi` | Correct `export_idx` returned when multiple exports are configured |
| `/empty` | Behaviour when no exports are configured |

### tmvsio2.c — `mvs_get_pds_dsn_and_member()`

Tests extraction of the PDS dataset name and member name from a path, given
a known export index.

| Sub-suite | What is tested |
|---|---|
| `/dsname` | `pds_dsname` equals the export `host_path` |
| `/dsname_trunc` | `pds_dsname` is truncated to 44 characters when `host_path` is longer |
| `/dataset_path` | Path equal to `export_path` produces an empty member name |
| `/member` | Uppercasing, 8-character truncation, filename with no extension |
| `/extension` | Matching, mismatching, and case-insensitive extension checks |
| `/no_ext` | Exports with no extension restriction accept any filename |
| `/export_idx` | Correct `host_path` is used for each `export_idx` value |

---

## Adding a new test suite

### 1. Create the test file

Create `tests/t<module>.c`.  Export exactly one `MunitSuite` at the bottom:

```c
MunitSuite t<module>_suite = {
    "/<module>/<function>",
    NULL,           /* no top-level tests */
    sub_suites,
    1,
    MUNIT_SUITE_OPTION_NONE
};
```

Use `tstubs.h` for export table control; declare any other stubs needed for
the module under test directly in the file.

### 2. Register the suite in runall.c

```c
/* Add an extern declaration with the others */
extern MunitSuite t<module>_suite;

/* Increment NUM_SUITES */
#define NUM_SUITES 3   /* was 2 */

/* Add the suite in main() */
all_suites[2] = t<module>_suite;
```

### 3. Update the Linux build command

Add the new source file to the `cc` command:

```bash
cc -std=c99 -Wall -I src -I tests \
   tests/runall.c tests/tstubs.c \
   tests/tmvsio.c tests/tmvsio2.c tests/t<module>.c \
   src/mvsio.c src/<module>.c tests/munit.c \
   -o tests/runall
```

### 4. Update testrun.jcl

Add a compile step for the new test module:

```jcl
//T<MOD>   EXEC JCCCTST,MODNAME=T<MOD>
```

Add the compiled object to the `PRELINK.I` list:

```jcl
//          DD DISP=SHR,DSN=TONYW.DINONFS.OBJLIB(T<MOD>)
```

If the new test module depends on a production module that is not already in
`PRELINK.I`, add a compile step for that module under the
"Compile application modules" section and add it to `PRELINK.I` as well.

---

## JCC compiler compatibility

The JCC compiler used on MVS 3.8j enforces strict C89 rules.  All test code
must comply:

- **All variable declarations must appear at the top of their enclosing
  block**, before any executable statements.  Mixing declarations and code
  is not permitted.
- C99 features (`//` comments in some contexts, `_Bool`, designated
  initialisers, `restrict`, etc.) should be avoided in test files.
- `size_t` loops should use an explicit `int` or `size_t` variable declared
  at the top of the function.

The `(void)params; (void)data;` casts at the start of every test and fixture
function suppress unused-parameter warnings on both GCC and JCC.
