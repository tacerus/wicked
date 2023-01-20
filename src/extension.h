/*
 *	Extensions aka external commands and bindings.
 *
 *	Copyright (C) 2010-2012 Olaf Kirch <okir@suse.de>
 *	Copyright (C) 2023 SUSE LLC
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *	Authors:
 *		Olaf Kirch
 *		Marius Tomaschewski
 */
#ifndef   NI_WICKED_EXTENSIONS_H
#define   NI_WICKED_EXTENSIONS_H

#include <wicked/types.h>

extern ni_c_binding_t *		ni_c_binding_new(ni_c_binding_t **, const char *,
						const char *, const char *);
extern void *			ni_c_binding_get_address(const ni_c_binding_t *);

extern ni_extension_t *		ni_extension_new(ni_extension_t **, const char *);
extern void			ni_extension_free(ni_extension_t *);
extern ni_extension_t *		ni_extension_by_name(ni_extension_t *head, const char *);

extern void			ni_extension_list_destroy(ni_extension_t **);
extern ni_extension_t *		ni_extension_list_find(ni_extension_t *, const char *);

extern ni_shellcmd_t *		ni_extension_script_new(ni_extension_t *, const char *, const char *);
extern ni_shellcmd_t *		ni_extension_script_find(ni_extension_t *, const char *);

extern const ni_c_binding_t *	ni_extension_find_c_binding(const ni_extension_t *, const char *);

#endif /* NI_WICKED_EXTENSIONS_H */
