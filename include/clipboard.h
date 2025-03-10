/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2024-2024  The DOSBox Staging Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef DOSBOX_CLIPBOARD_H
#define DOSBOX_CLIPBOARD_H

#include <cstdint>
#include <string>

// Text clipboard support

bool CLIPBOARD_HasText();

void CLIPBOARD_CopyText(const std::string& content);
void CLIPBOARD_CopyText(const std::string& content, const uint16_t code_page);

std::string CLIPBOARD_PasteText();
std::string CLIPBOARD_PasteText(const uint16_t code_page);

#endif // DOSBOX_CLIPBOARD_H
