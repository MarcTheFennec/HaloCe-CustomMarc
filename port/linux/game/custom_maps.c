/*
CUSTOM_MAPS.C

The custom map loader: which non-retail maps are in the maps folder, and
the strings that name them in the menus.

The menus' level lists (the multiplayer map select and the single-player
mission select, both reached from the main menu) are built in code from a
fixed list of retail maps (ui_widget_event_handler_functions.c). On the
native builds this file adds the .map files of the maps folder after the
retail ones: custom_map_scan reads the folder when a list opens, and the
list's display code asks custom_map_string_index for a string past the
retail ones in the list's own string list tag (a text box draws its text
from the tag at definition->text_label_string_list, so the custom names
are appended to that tag: the retail strings keep their indices, and the
tag is reread from the map file whenever the ui tags reload).

The files the game itself uses are not offered: the campaign and the
multiplayer maps, and the ui tags map.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "tag_files/files.h"
#include "cache/cache_files.h"
#include "text/text_group.h"
#include "text/unicode.h"
#include "interface/custom_maps.h"

/* ---------- constants */

/* the maps the game itself loads, by the last part of their path: the
campaign (event_handler_functions.map_name), the multiplayer maps
(event_handler_functions.multiplayer_levels), and the ui tags */
static char const *custom_map_retail_map_names[] =
{
	"a10", "a30", "a50", "b30", "b40",
	"c10", "c20", "c40", "d20", "d40",
	"beavercreek", "sidewinder", "damnation", "ratrace", "prisoner",
	"hangemhigh", "chillout", "carousel", "boardingaction", "bloodgulch",
	"wizard", "putput", "longest",
	"ui", "mainmenu",
};

/* a string list tag the loader has appended to, and where its own strings
began. A tag is reread from its map file when the ui tags reload (the menu
is rebuilt after a game), which the count comparison below notices */
struct custom_map_string_record
{
	long tag_index;
	short retail_count;
	short appended;
	/* the entries array the loader allocated (never the tag's own) */
	void *owned_entries;
};

#define MAXIMUM_CUSTOM_MAP_STRING_RECORDS 32

/* ---------- structures */

/* ---------- globals */

static short custom_map_total = 0;
static char custom_map_name_storage[CUSTOM_MAP_MAXIMUM_COUNT][CUSTOM_MAP_MAXIMUM_NAME_LENGTH + 1];
static char *custom_map_name_pointers[CUSTOM_MAP_MAXIMUM_COUNT];
static wchar_t custom_map_name_wide[CUSTOM_MAP_MAXIMUM_COUNT][CUSTOM_MAP_MAXIMUM_NAME_LENGTH + 1];

static struct custom_map_string_record custom_map_string_records[MAXIMUM_CUSTOM_MAP_STRING_RECORDS];
static short custom_map_string_record_count = 0;

/* ---------- prototypes */

static boolean custom_map_name_is_retail(char const *name);
static boolean custom_map_name_is_valid(char const *name);
static void custom_map_sort(void);
static void custom_map_release_owned_entries(struct custom_map_string_record *record);

/* ---------- public code */

short custom_map_scan(void)
{
	struct file_reference directory;
	static struct file_reference found_files[CUSTOM_MAP_MAXIMUM_COUNT + 32];
	/* file_reference_get_name writes a MAXIMUM_FILENAME_LENGTH+1 byte field
	   (strncpy pads the whole width, then the terminator): both buffers must
	   be that wide or the pad smashes this frame and the return address */
	char filename[MAXIMUM_FILENAME_LENGTH + 1];
	char extension[MAXIMUM_FILENAME_LENGTH + 1];
	long file_count;
	long file_index;

	custom_map_total = 0;
	if (!file_reference_create_from_path(&directory, cache_files_map_directory(), TRUE))
		return 0;
	file_count = find_files(0, &directory, NUMBEROF(found_files), found_files);
	for (file_index = 0; file_index < file_count && custom_map_total < CUSTOM_MAP_MAXIMUM_COUNT; file_index++)
	{
		filename[0] = '\0';
		extension[0] = '\0';
		file_reference_get_name(&found_files[file_index], FLAG(_name_filename_bit), filename);
		file_reference_get_name(&found_files[file_index], FLAG(_name_extension_bit), extension);
		if (csstrcasecmp(extension, "map") != 0 || !filename[0])
			continue;
		if (csstrlen(filename) > CUSTOM_MAP_MAXIMUM_NAME_LENGTH)
			continue;
		if (!custom_map_name_is_valid(filename) || custom_map_name_is_retail(filename))
			continue;
		csstrncpy(custom_map_name_storage[custom_map_total], filename,
			CUSTOM_MAP_MAXIMUM_NAME_LENGTH + 1);
		custom_map_total++;
	}
	custom_map_sort();

	if (custom_map_total)
	{
		error(2, "custom map loader: found %d custom map%s in '%s'",
			custom_map_total, custom_map_total == 1 ? "" : "s",
			cache_files_map_directory());
	}

	return custom_map_total;
}

short custom_map_count(void)
{
	return custom_map_total;
}

char const *const *custom_map_names(void)
{
	return (char const *const *)custom_map_name_pointers;
}

long custom_map_definition_string_list_tag(void const *definition)
{
	if (!definition)
		return NONE;

	/* verify_ui_widget_definition_text_label_string_list_offset in ui_widget.c:
	text_label_string_list is the tag_reference at 0xEC of a 'DeLa' */
	return ((struct tag_reference const *)((byte const *)definition + 0xEC))->index;
}

short custom_map_string_index(long list_tag_index, short custom_offset)
{
	struct string_list *list;
	struct custom_map_string_record *record = NULL;
	struct string_list_entry *entries;
	struct string_list_entry *entry;
	short record_index;
	short needed;
	short string_index;
	long new_count;

	if (list_tag_index == NONE || custom_offset < 0 || custom_offset >= custom_map_total)
		return NONE;
	list = unicode_string_list_definition_get(list_tag_index);
	for (record_index = 0; record_index < custom_map_string_record_count; record_index++)
	{
		if (custom_map_string_records[record_index].tag_index == list_tag_index)
		{
			record = &custom_map_string_records[record_index];
			break;
		}
	}
	if (!record)
	{
		if (custom_map_string_record_count >= MAXIMUM_CUSTOM_MAP_STRING_RECORDS)
			return NONE;
		record = &custom_map_string_records[custom_map_string_record_count++];
		record->tag_index = list_tag_index;
		record->retail_count = (short)list->strings.count;
		record->appended = 0;
		record->owned_entries = NULL;
	}
	else if ((short)list->strings.count != record->retail_count + record->appended)
	{
		/* the ui tags were reloaded since the loader appended to them */
		custom_map_release_owned_entries(record);
		record->retail_count = (short)list->strings.count;
		record->appended = 0;
	}

	needed = custom_offset + 1;
	if (record->appended < needed)
	{
		new_count = record->retail_count + needed;
		entries = (struct string_list_entry *)malloc(
			new_count * sizeof(struct string_list_entry));
		if (!entries)
			return NONE;
		csmemcpy(entries, list->strings.address,
			record->retail_count * sizeof(struct string_list_entry));
		/* every appended string is built fresh in the new array: the retail
		   entries were copied from the tag, and the loader's own strings are
		   rebuilt from the scanned names (the old array's strings stay alive
		   until it is released below) */
		for (string_index = 0; string_index < needed; string_index++)
		{
			unsigned long length;

			entry = &entries[record->retail_count + string_index];
			csmemset(entry, 0, sizeof(struct string_list_entry));
			length = ustrlen(custom_map_name_wide[string_index]);
			entry->string.size = 2 * (length + 1);
			entry->string.address = malloc(entry->string.size);
			if (!entry->string.address)
			{
				for (string_index--; string_index >= 0; string_index--)
					free(entries[record->retail_count + string_index].string.address);
				free(entries);
				return NONE;
			}
			csmemcpy(entry->string.address, custom_map_name_wide[string_index],
				2 * length + 2);
		}
		/* the tag's own entries array is the tag's memory, not the loader's */
		custom_map_release_owned_entries(record);
		record->owned_entries = entries;
		list->strings.address = entries;
		list->strings.count = new_count;
		record->appended = needed;
	}

	return record->retail_count + custom_offset;
}

/* ---------- private code */

static boolean custom_map_name_is_retail(char const *name)
{
	long name_index;

	for (name_index = 0; name_index < NUMBEROF(custom_map_retail_map_names); name_index++)
	{
		if (csstrcasecmp(name, custom_map_retail_map_names[name_index]) == 0)
			return TRUE;
	}

	return FALSE;
}

/* the name becomes the last part of the map's path and part of its file
name: letters, digits, '-' and '_' only */
static boolean custom_map_name_is_valid(char const *name)
{
	char const *character;

	for (character = name; *character; character++)
	{
		if ((*character >= 'a' && *character <= 'z') ||
			(*character >= 'A' && *character <= 'Z') ||
			(*character >= '0' && *character <= '9') ||
			*character == '-' || *character == '_')
			continue;
		return FALSE;
	}

	return TRUE;
}

/* free the entries array the loader allocated and the strings it built in
it (the retail entries point into tag memory and are left alone) */
static void custom_map_release_owned_entries(struct custom_map_string_record *record)
{
	struct string_list_entry *entries;
	short string_index;

	if (!record->owned_entries)
		return;

	entries = (struct string_list_entry *)record->owned_entries;
	for (string_index = 0; string_index < record->appended; string_index++)
	{
		if (entries[record->retail_count + string_index].string.address)
			free(entries[record->retail_count + string_index].string.address);
	}
	free(entries);
	record->owned_entries = NULL;
	return;
}

/* the folder's order is the file system's: sort the names without regard
to case, then build the pointers and the wide strings that go with them */
static void custom_map_sort(void)
{
	char entry[CUSTOM_MAP_MAXIMUM_NAME_LENGTH + 1];
	long entry_index;
	long sorted_index;
	long name_index;

	for (entry_index = 1; entry_index < custom_map_total; entry_index++)
	{
		csmemcpy(entry, custom_map_name_storage[entry_index], sizeof(entry));
		for (sorted_index = entry_index;
			sorted_index > 0 &&
				csstrcasecmp(custom_map_name_storage[sorted_index - 1], entry) > 0;
			sorted_index--)
		{
			csmemcpy(custom_map_name_storage[sorted_index],
				custom_map_name_storage[sorted_index - 1], sizeof(entry));
		}
		csmemcpy(custom_map_name_storage[sorted_index], entry, sizeof(entry));
	}
	for (name_index = 0; name_index < custom_map_total; name_index++)
	{
		custom_map_name_pointers[name_index] = custom_map_name_storage[name_index];
		ascii_to_wide(custom_map_name_storage[name_index],
			custom_map_name_wide[name_index],
			sizeof(custom_map_name_wide[name_index]));
	}

	return;
}
