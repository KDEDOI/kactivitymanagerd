/*
 *   Copyright (C) 2012 - 2016 by Ivan Cukic <ivan.cukic@kde.org>
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License or (at your option) version 3 or any later version
 *   accepted by the membership of KDE e.V. (or its successor approved
 *   by the membership of KDE e.V.), which shall act as a proxy
 *   defined in Section 14 of version 3 of the license.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UTILS_REMOVE_IF_H
#define UTILS_REMOVE_IF_H

#include <kactivities-features.h>
#include <algorithm>

/********************************************************************
 *  Syntactic sugar for the erase-remove idiom                      *
 ********************************************************************/

namespace kamd {
namespace utils {

template <typename Collection, typename Filter>
__inline void remove_if(Collection &collection, Filter filter)
{
    collection.erase(
        std::remove_if(
            collection.begin(), collection.end(), filter),
        collection.end());
}

} // namespace utils
} // namespace kamd

#endif // UTILS_REMOVE_IF_H
