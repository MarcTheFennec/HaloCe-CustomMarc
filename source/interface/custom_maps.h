/*
CUSTOM_MAPS.H

The custom map loader (port/linux/game/custom_maps.c): the .map files in the
maps folder that are not retail maps, offered in the menus' level lists
(source/interface/ui_widget_event_handler_functions.c and
source/interface/ui_widget_game_data_input_functions.c).

Include this header from a game unit after cseries.h, as
"interface/custom_maps.h" (it uses boolean). It is only included in the
native builds (HALO_LINUX).
*/

#ifndef __CUSTOM_MAPS_H
#define __CUSTOM_MAPS_H
#pragma once

/* ---------- constants */

enum
{
	/* the maps the menus list before the custom ones */
	CUSTOM_MAP_STOCK_MULTIPLAYER_LEVELS = 13,
	CUSTOM_MAP_STOCK_SINGLE_PLAYER_LEVELS = 10,
	/* the most custom maps the menus offer */
	CUSTOM_MAP_MAXIMUM_COUNT = 64,
	/* the longest map name the loader accepts, without ".map" */
	CUSTOM_MAP_MAXIMUM_NAME_LENGTH = 39,
};

/* ---------- structures */

/* one entry of the single-player level list past the retail ten: the same
layout as struct single_player_level_entry in
source/interface/ui_widget_event_handler_functions.c */
struct custom_map_sp_level_entry
{
	char *map_name;
	boolean available;
	boolean unknown5;
	boolean unknown6;
	boolean unknown7;
};

/* ---------- prototypes/CUSTOM_MAPS.C */

/* rereads the maps folder; returns how many custom maps there are */
short custom_map_scan(void);
short custom_map_count(void);
/* the map names without ".map", sorted without regard to case */
char const *const *custom_map_names(void);

/* the single-player level list past the retail ten: the retail entries
followed by the custom maps. Defined in
ui_widget_event_handler_functions.c so that file can fill it with the
player profile's campaign progress; read by
ui_widget_game_data_input_functions.c when the list draws */
extern struct custom_map_sp_level_entry custom_map_sp_level_data[
	CUSTOM_MAP_STOCK_SINGLE_PLAYER_LEVELS + CUSTOM_MAP_MAXIMUM_COUNT];

/* the string list tag a text box draws its text from
(ui_widget.c's DeLa definition, text_label_string_list at 0xEC), or NONE */
long custom_map_definition_string_list_tag(void const *definition);

/* the string of the custom map custom_offset in a string list tag: the
appended strings follow the tag's own strings, so the retail strings keep
their indices. Appends what the tag still lacks, and returns NONE if the
tag has no strings or the offset names no map (after a rescan that found
fewer maps) */
short custom_map_string_index(long list_tag_index, short custom_offset);

/* ---------- globals */

#endif // __CUSTOM_MAPS_H
