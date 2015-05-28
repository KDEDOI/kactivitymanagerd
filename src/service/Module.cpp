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
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

// Self
#include "Module.h"

// Qt
#include <QHash>
#include <QString>
#include <QObject>

// Utils
#include <utils/d_ptr_implementation.h>

// Local
#include "Debug.h"


class Module::Private {
public:
    static QHash<QString, QObject *> s_modules;
};

QHash<QString, QObject *> Module::Private::s_modules;

Module::Module(const QString &name, QObject *parent)
    : QObject(parent)
    , d()
{
    if (!name.isEmpty()) {
        Private::s_modules[name] = this;
    }
}

Module::~Module()
{
}

QObject *Module::get(const QString &name)
{
    Q_ASSERT(!name.isEmpty());

    if (Private::s_modules.contains(name)) {
        qCDebug(KAMD_LOG_APPLICATION) << "Returning a valid module object for:" << name;
        return Private::s_modules[name];
    }

    qCDebug(KAMD_LOG_APPLICATION) << "The requested module doesn't exist:" << name;
    return Q_NULLPTR;
}

QHash<QString, QObject *> &Module::get()
{
    return Private::s_modules;
}

bool Module::isFeatureEnabled(const QStringList &feature) const
{
    Q_UNUSED(feature);
    return false;
}

bool Module::isFeatureOperational(const QStringList &feature) const
{
    Q_UNUSED(feature);
    return false;
}

void Module::setFeatureEnabled(const QStringList &feature, bool value)
{
    Q_UNUSED(feature);
    Q_UNUSED(value);
}

QStringList Module::listFeatures(const QStringList &feature) const
{
    Q_UNUSED(feature);
    return QStringList();
}

QDBusVariant Module::value(const QStringList &property) const
{
    Q_UNUSED(property);

    return QDBusVariant();
}

void Module::setValue(const QStringList &property, const QDBusVariant &value)
{
    Q_UNUSED(property);
    Q_UNUSED(value);
}

