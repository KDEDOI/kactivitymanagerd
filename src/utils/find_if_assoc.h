/*
 *   Copyright (C) 2012 Ivan Cukic <ivan.cukic(at)kde.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License version 2,
 *   or (at your option) any later version, as published by the Free
 *   Software Foundation
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details
 *
 *   You should have received a copy of the GNU General Public
 *   License along with this program; if not, write to the
 *   Free Software Foundation,3 Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef UTILS_FIND_IF_ASSOC_H
#define UTILS_FIND_IF_ASSOC_H

#include <config-features.h>
#include <utils/nullptr.h>

/********************************************************************
 *  Associative container's find_if (for hash, map, and similar )  *
 ********************************************************************/

namespace kamd {
namespace utils {

namespace details {

    // Iterator Functions

    template <typename Iterator, typename Function>
    Function qt_find_if_assoc(Iterator start, Iterator end, Function f)
    {
        for ( ; start != end; ++ start ) {
            if (f(start.key(), start.value())) break;
        }

        return f;
    }

    template <typename Iterator, typename Function>
    Function stl_find_if_assoc(Iterator start, Iterator end, Function f)
    {
        for ( ; start != end; ++ start ) {
            if (f(start->first, start->second)) break;
        }

        return f;
    }

    // Container functions

    template <typename Container, typename Function>
    Function _find_if_assoc_helper_container(const Container & c, Function f,
            decltype(&Container::constBegin) * )
    {
        return qt_find_if_assoc(c.constBegin(), c.constEnd(), f);
    }

    template <typename Container, typename Function>
    Function _find_if_assoc_helper_container(const Container & c, Function f,
            decltype(&Container::cbegin) * )
    {
        return stl_find_if_assoc(c.cbegin(), c.cend(), f);
    }

} // namespace details

template <typename Container, typename Function>
Function find_if_assoc(const Container & c, Function f)
{
    return details::_find_if_assoc_helper_container
        <Container, Function>(c, f, nullptr);
}

} // namespace utils
} // namespace kamd

#endif // UTILS_FIND_IF_ASSOC_H