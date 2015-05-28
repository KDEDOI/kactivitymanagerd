/*
 *   Copyright (C) 2010, 2011, 2012, 2013, 2014 Ivan Cukic <ivan.cukic(at)kde.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License version 2,
 *   or (at your option) any later version, as published by the Free
 *   Software Foundation
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "manager_p.h"

#include <mutex>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QFutureWatcher>
#include <QFutureWatcherBase>

#include "debug_p.h"
#include "mainthreadexecutor_p.h"

#include "common/dbus/common.h"
#include "utils/dbusfuture_p.h"
#include "utils/continue_with.h"
#include "version.h"

namespace KActivities {

Manager *Manager::s_instance = Q_NULLPTR;

Manager::Manager()
    : QObject()
    , m_watcher(KAMD_DBUS_SERVICE, QDBusConnection::sessionBus())
    , m_activities(new KAMD_DBUS_CLASS_INTERFACE(Activities, Activities, this))
    , m_resources(new KAMD_DBUS_CLASS_INTERFACE(Resources, Resources, this))
    , m_resourcesLinking(new KAMD_DBUS_CLASS_INTERFACE(Resources/Linking, ResourcesLinking, this))
    , m_features(new KAMD_DBUS_CLASS_INTERFACE(Features, Features, this))
    , m_serviceRunning(false)
{
    connect(&m_watcher, &QDBusServiceWatcher::serviceOwnerChanged,
            this, &Manager::serviceOwnerChanged);

    if (isServiceRunning()) {
        serviceOwnerChanged(KAMD_DBUS_SERVICE, QString(), KAMD_DBUS_SERVICE);
    }
}

Manager *Manager::self()
{
    static std::mutex singleton;
    std::lock_guard<std::mutex> singleton_lock(singleton);

    if (!s_instance) {

        runInMainThread([] () {
            // check if the activity manager is already running
            if (!Manager::isServiceRunning()) {

                #if defined(QT_DEBUG)
                QLoggingCategory::setFilterRules(QStringLiteral("org.kde.kactivities.lib.core.debug=true"));

                qCDebug(KAMD_CORELIB) << "Should we start the daemon?";
                if (!QCoreApplication::instance()
                         ->property("org.kde.KActivities.core.disableAutostart")
                         .toBool()) {
                    qCDebug(KAMD_CORELIB) << "Starting the activity manager daemon";
                    QProcess::startDetached(QStringLiteral("kactivitymanagerd"));
                }

                #else
                QProcess::startDetached(QStringLiteral("kactivitymanagerd"));
                #endif
            }

            // creating a new instance of the class
            Manager::s_instance = new Manager();

        });
    }

    return s_instance;
}

bool Manager::isServiceRunning()
{
    return
        (s_instance ? s_instance->m_serviceRunning : true)
        && QDBusConnection::sessionBus().interface()->isServiceRegistered(KAMD_DBUS_SERVICE);
}

void Manager::serviceOwnerChanged(const QString &serviceName, const QString &oldOwner, const QString &newOwner)
{
    Q_UNUSED(oldOwner);

    if (serviceName == KAMD_DBUS_SERVICE) {
        m_serviceRunning = !newOwner.isEmpty();
        emit serviceStatusChanged(m_serviceRunning);

        if (m_serviceRunning) {
            QDBusInterface service(KAMD_DBUS_SERVICE,
                    "/ActivityManager",
                    "org.kde.ActivityManager.Application",
                    QDBusConnection::sessionBus(),
                    Q_NULLPTR);

            kamd::utils::continue_with(
                DBusFuture::asyncCall<QString>(&service, "serviceVersion"),
                [this] (const boost::optional<QString> &serviceVersion) {
                    // Test whether the service is older than the library.
                    // If it is, we need to end this

                    if (!serviceVersion.is_initialized()) {
                        qWarning() << "KActivities: FATAL ERROR: Failed to contact the activity manager daemon";
                        m_serviceRunning = false;
                        return;
                    }

                    auto split = serviceVersion->split('.');
                    QList<int> version;

                    const int requiredVersion[] = {
                            KACTIVITIES_VERSION_MAJOR,
                            KACTIVITIES_VERSION_MINOR,
                            KACTIVITIES_VERSION_RELEASE
                        };

                    std::transform(
                            split.cbegin(), split.cend(),
                            std::back_inserter(version), [] (const QString &component) {
                                return component.toInt();
                            });

                    // if required version is greater than the current version
                    if (std::lexicographical_compare(
                            version.cbegin(), version.cend(),
                            std::begin(requiredVersion), std::end(requiredVersion)
                        )) {
                        QString libraryVersion = QString::number(requiredVersion[0]) + '.'
                                               + QString::number(requiredVersion[1]) + '.'
                                               + QString::number(requiredVersion[2]);

                        qDebug() << "KActivities service version: " << serviceVersion.get();
                        qDebug() << "KActivities library version: " << libraryVersion;
                        qFatal("KActivities: FATAL ERROR: The service is older than the library");

                    }
                });
        }
    }
}

Service::Activities *Manager::activities()
{
    return self()->m_activities;
}

Service::Resources *Manager::resources()
{
    return self()->m_resources;
}

Service::ResourcesLinking *Manager::resourcesLinking()
{
    return self()->m_resourcesLinking;
}

Service::Features *Manager::features()
{
    return self()->m_features;
}

} // namespace KActivities
