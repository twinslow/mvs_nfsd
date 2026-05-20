#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// Represents the exact offset layout inside the PDS Directory Entry User Data field (PDS2USRD)
typedef struct __attribute__((packed)) {
    uint8_t  version;             // X'00' : Version Number (VV)
    uint8_t  mod_level;           // X'01' : Modification Level (MM)
    uint8_t  flags;               // X'02' : Bit 2 = Extended stats exist
    uint8_t  mod_seconds;         // X'03' : Packed Dec - Seconds (1 byte: e.g. 0x34s)
    uint8_t  create_date[4];      // X'04' : Packed Dec - Century, Year, Julian Day (CYYDDDF)
    uint8_t  mod_date[4];         // X'08' : Packed Dec - Century, Year, Julian Day (CYYDDDF)
    uint8_t  mod_time[2];         // X'0C' : Packed Dec - HHMMF (Hours and minutes)
    // ... remaining line counts and User ID are ignored for this time conversion
} IspfStatsBase;

// Helper: Safely converts raw packed decimal bytes to a standard integer
uint32_t unpack_comp3(const uint8_t *packed_bytes, size_t num_bytes) {
    uint32_t result = 0;
    for (size_t i = 0; i < num_bytes; i++) {
        uint8_t high_nibble = (packed_bytes[i] >> 4) & 0x0F;
        uint8_t low_nibble  = packed_bytes[i] & 0x0F;

        result = (result * 10) + high_nibble;
        
        // If the low nibble is a sign indicator (A-F), we have reached the end of the packed field
        if (low_nibble >= 0x0A) {
            break;
        }
        result = (result * 10) + low_nibble;
    }
    return result;
}

// Core Logic: Converts raw packed decimal components to standard Unix Epoch time
time_t ispf_to_unix_epoch(const uint8_t *date_4b, const uint8_t *time_2b, uint8_t seconds_1b) {
    // 1. Unpack raw structures
    uint32_t packed_date = unpack_comp3(date_4b, 4); // Format: Cyyddd (e.g., 126045)
    uint32_t packed_time = unpack_comp3(time_2b, 2); // Format: hhmm   (e.g., 1430)
    uint32_t packed_secs = unpack_comp3(&seconds_1b, 1); // Format: ss     (e.g., 45)

    // 2. Extract Date Components
    uint32_t julian_day = packed_date % 1000;         // Last 3 digits = Day of year (1-366)
    uint32_t year_short = (packed_date / 1000) % 100; // Middle 2 digits = Year (00-99)
    uint32_t century    = packed_date / 100000;       // First digit = 0 for 1900, 1 for 2000

    uint32_t full_year = (century == 1 ? 2000 : 1900) + year_short;

    // 3. Extract Time Components
    uint32_t hours   = packed_time / 100;
    uint32_t minutes = packed_time % 100;
    uint32_t seconds = packed_secs;

    // 4. Fill standard POSIX C Time structure
    struct tm target_time;
    memset(&target_time, 0, sizeof(struct tm));

    target_time.tm_year = full_year - 1900; // Years since 1900
    target_time.tm_mday = julian_day;       // Inject day straight into tm_mday (with tm_mon=0)
    target_time.tm_mon  = 0;                // January 1st reference point
    target_time.tm_hour = hours;
    target_time.tm_min  = minutes;
    target_time.tm_sec  = seconds;
    target_time.tm_isdst = -1;              // Let OS handle Daylight Saving Time

    // 5. Compute epoch from struct tm
    // Use timegm() to process as pure UTC/GMT. Use mktime() if processing as local mainframe time.
#if defined(_WIN32) || defined(_WIN64)
    return _mkgmtime(&target_time); 
#else
    return timegm(&target_time); // Standard POSIX/Linux
#endif
}

int main() {
    // Real-world scenario simulation:
    // Feb 14, 2026 at 14:30:45 UTC
    // Julian Date: Year 2026, Day 45 -> 126045F
    // Time: 14:30 -> 1430F | Sec: 45 -> 45F
    IspfStatsBase mock_directory_entry = {
        .version      = 0x01,
        .mod_level    = 0x00,
        .flags        = 0x00,
        .mod_seconds  = 0x45,               // "45" packed
        .create_date  = {0x01, 0x26, 0x04, 0x5F}, // "126045" packed
        .mod_date     = {0x01, 0x26, 0x04, 0x5F}, // "126045" packed
        .mod_time     = {0x14, 0x30}        // "1430" packed (last digit shares space with sign nibble F if mapped unevenly)
    };

    time_t unix_seconds = ispf_to_unix_epoch(
        mock_directory_entry.mod_date, 
        mock_directory_entry.mod_time, 
        mock_directory_entry.mod_seconds
    );

    printf("Successfully converted ISPF Statistics.\n");
    printf("Unix Epoch Seconds: %ld\n", (long)unix_seconds);
    printf("Human Readable UTC: %s", asctime(gmtime(&unix_seconds)));

    return 0;
}
