// SPDX-License-Identifier: GPL-3.0-or-later
/* Resource identifiers shared by tn34xxw.rc and the Win32 front end.
   Plain C preprocessor only: windres reads this file too. */
#ifndef TN34XX_WIN32_RESOURCE_H
#define TN34XX_WIN32_RESOURCE_H

/* Icons */
#define IDI_APP                 1
#define IDI_PROFILE             2

/* Toolbar image strips (6 glyphs each), picked by DPI */
#define IDB_TOOLBAR16           101
#define IDB_TOOLBAR24           102
#define IDB_TOOLBAR32           103

/* Menu and accelerators */
#define IDR_MAINMENU            200
#define IDR_ACCEL               201
#define IDR_NOTICES             202

/* Commands */
#define ID_FILE_PROFILES        1001
#define ID_FILE_CONNECT         1002
#define ID_FILE_EDIT_PROFILE    1003
#define ID_FILE_SAVE            1004
#define ID_FILE_SAVE_AS         1005
#define ID_FILE_DISCONNECT      1006
#define ID_FILE_EXIT            1007
#define ID_EDIT_KEYMAP          1010
#define ID_EDIT_SETTINGS        1011
#define ID_VIEW_ZOOM_100        1020
#define ID_VIEW_ZOOM_150        1021
#define ID_VIEW_ZOOM_200        1022
#define ID_TERM_ATTN            1030
#define ID_TERM_SYSRQ           1031
#define ID_TERM_RESET           1032
#define ID_TERM_POWER_CYCLE     1033
#define ID_HELP_ABOUT           1040

/* Dialogs */
#define IDD_ROMS                300
#define IDD_SETTINGS            301
#define IDD_PROFILES            302
#define IDD_NAME                303
#define IDD_PROFILE             304
#define IDD_TLS                 305
#define IDD_TRUST               306
#define IDD_KEYMAP              307
#define IDD_KEYMAP_ENTRY        308
#define IDD_ABOUT               309

/* Controls */
#define IDC_STATIC_TEXT         2000
#define IDC_ROM_LIST            2001
#define IDC_ROM_IMPORT          2002
#define IDC_ROM_REMOVE          2003
#define IDC_ROM_RECHECK         2004
#define IDC_ROM_FOLDER          2005
#define IDC_ROM_SUMMARY         2006

#define IDC_SET_SUBTYPE         2020
#define IDC_SET_CRT             2021
#define IDC_SET_SPEED           2022
#define IDC_SET_TLSMIN          2023
#define IDC_SET_ROMS            2024
#define IDC_SET_ASSOC           2025
#define IDC_SET_FOLDER          2026

#define IDC_PROF_LIST           2040
#define IDC_PROF_CONNECT        2041
#define IDC_PROF_NEW            2042
#define IDC_PROF_EDIT           2043
#define IDC_PROF_RENAME         2044
#define IDC_PROF_COPY           2045
#define IDC_PROF_DELETE         2046

#define IDC_NAME_EDIT           2060
#define IDC_NAME_PROMPT         2061

#define IDC_PE_MODEL            2080
#define IDC_PE_HOST1            2081
#define IDC_PE_PORT1            2082
#define IDC_PE_DEV1             2083
#define IDC_PE_HOST2            2084
#define IDC_PE_PORT2            2085
#define IDC_PE_DEV2             2086
#define IDC_PE_TLS              2087
#define IDC_PE_TLS_SETTINGS     2088
#define IDC_PE_TLS_SUMMARY      2089

#define IDC_TLS_SYSTEM          2100
#define IDC_TLS_ANY             2101
#define IDC_TLS_HASH            2102
#define IDC_TLS_HASH_EDIT       2103
#define IDC_TLS_FETCH           2104
#define IDC_TLS_CERT            2105
#define IDC_TLS_MINVER          2106
#define IDC_TLS_FETCH_HOST      2107

#define IDC_TRUST_ICON          2120
#define IDC_TRUST_TEXT          2121
#define IDC_TRUST_DETAILS       2122
#define IDC_TRUST_ONCE          2123
#define IDC_TRUST_ALWAYS        2124

#define IDC_KM_LIST             2140
#define IDC_KM_ADD              2141
#define IDC_KM_EDIT             2142
#define IDC_KM_REMOVE           2143
#define IDC_KM_RESET            2144
#define IDC_KM_BASE             2145

#define IDC_KE_CAPTURE          2160
#define IDC_KE_HOST             2161
#define IDC_KE_SHIFT            2162
#define IDC_KE_CTRL             2163
#define IDC_KE_ALT              2164
#define IDC_KE_ANY              2165
#define IDC_KE_TO               2166
#define IDC_KE_KEYBOARD         2167
#define IDC_KE_TAP              2168

#define IDC_ABOUT_TEXT          2180
#define IDC_ABOUT_LINK          2181
#define IDC_ABOUT_NOTICES       2182

/* Strings */
#define IDS_APP_TITLE           4000
#define IDS_UNTITLED            4001
#define IDS_TB_PROFILES         4010
#define IDS_TB_CONNECT          4011
#define IDS_TB_SAVE             4012
#define IDS_TB_DISCONNECT       4013
#define IDS_TB_SETTINGS         4014
#define IDS_TB_KEYMAP           4015
#define IDS_TB_ATTN             4016
#define IDS_TB_SYSRQ            4017
#define IDS_TB_RESET            4018
#define IDS_NOT_CONNECTED       4030
#define IDS_NO_PROFILE          4031
#define IDS_TLS_OFF             4032
#define IDS_ROMS_MISSING        4040
#define IDS_ASSOC_CONFIRM       4041
#define IDS_ASSOC_DONE          4042
#define IDS_SAVE_CHANGES        4043
#define IDS_DELETE_CONFIRM      4044
#define IDS_RESTART_NEEDED      4045
#define IDS_FILTER_ROMS         4046
#define IDS_FILE_TYPE_NAME      4047

#endif
