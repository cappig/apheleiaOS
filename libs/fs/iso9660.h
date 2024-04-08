#pragma once

#include <base/attributes.h>
#include <base/types.h>

// ISO 9660 filesystem structures as defined by ECMA-119
// https://www.ecma-international.org/wp-content/uploads/ECMA-119_4th_edition_june_2019.pdf

#define ISO_SECTOR_SIZE  2048
#define ISO_MAX_VOLUMES  255
#define ISO_VOLUME_START 0x10

#define ISO_PATH_TERMINATOR ";1"

// With the rock ridge extension
#define ISO_FILE_NAME_LENGTH 255

typedef struct {
    u16 lsb;
    u16 msb;
} u16_lsb_msb;

typedef struct {
    u32 lsb;
    u32 msb;
} u32_lsb_msb;

typedef struct PACKED {
    char year[4];
    char month[2];
    char day[2];
    char hour[2];
    char minute[2];
    char second[2];
    char hundredth[2];
    u8 time_zone_offset;
} iso_date;

// Different date format for directory records for some reason
typedef struct PACKED {
    u8 year;
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
    u8 time_zone_offset;
} iso_dir_date;

typedef struct PACKED {
    u8 length;
    u8 extended_attribute_length;

    u32_lsb_msb extent_location;
    u32_lsb_msb extent_size;

    iso_dir_date time;

    u8 flags;
    u8 interleave_unit_size;
    u8 interleave_gap_size;

    u16_lsb_msb volume_sequence_number;

    u8 file_id_len;
    char file_id[];
} iso_dir;

typedef enum {
    ISO_BOOT_RECORD,
    ISO_PRIMARY,
    ISO_SUPPLEMENTARY,
    ISO_PARTITION,

    ISO_TERMINATOR = 255
} iso_volume_type;

// As defined in table 4 of the ECMA standard
typedef struct PACKED {
    u8 type;
    char id[5]; // "CD001"
    u8 version; // 0x01
    u8 _unused0;

    char system_id[32];
    char volume_id[32];
    char _unused1[8];

    u32_lsb_msb volume_space_size;
    u8 _unused2[32];

    u16_lsb_msb volume_set_size;
    u16_lsb_msb volume_sequence_number;
    u16_lsb_msb logical_block_size;
    u32_lsb_msb path_table_size;

    u32 path_table_lsb;
    u32 optional_path_table_lsb;
    u32 path_table_msb;
    u32 optional_path_table_msb;

    // iso_dir root;
    // flexible array member cannot be used in the middle of a struct
    u8 root[34];

    char volume_set_id[128];
    char publisher_id[128];
    char data_preparer_id[128];
    char application_id[128];

    char copyright_file_id[37];
    char abstract_file_id[37];
    char bibliographic_file_id[37];

    iso_date creation_date;
    iso_date modification_date;
    iso_date expiration_date;
    iso_date effective_date;

    u8 structure_version; // 0x01
    u8 _unused3;

    // extra padding; size of volume_descriptor is 2048 bytes
    char application_use[1170];
} iso_volume_descriptor;

typedef struct PACKED {
    u8 id_length;
    u8 extended_attribute_length;
    u32 extent_location;
    u16 directory_number;
    char file_id[];
} iso_path_table;
