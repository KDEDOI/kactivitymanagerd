/*
 *   Copyright (C) 2010 - 2015 Ivan Cukic <ivan.cukic(at)kde.org>
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
#include <kactivities-features.h>
#include "Activities.h"
#include "Activities_p.h"

// Qt
#include <QtGlobal>
#include <QDBusConnection>
#include <QMetaObject>
#include <QStandardPaths>
#include <QFile>
#include <QUuid>

// KDE
#include <kdbusconnectionpool.h>
#include <klocalizedstring.h>
#include <kauthorized.h>
#include <kdelibs4migration.h>

// Utils
#include <utils/d_ptr_implementation.h>
#include <utils/range.h>

// Local
#include "Debug.h"
#include "activitiesadaptor.h"
#include "ksmserver/KSMServer.h"
#include "common/dbus/common.h"

// Private
#define ACTIVITY_MANAGER_CONFIG_FILE_NAME QStringLiteral("kactivitymanagerdrc")

Activities::Private::ConfigurationChecker::ConfigurationChecker()
{
    // Checking whether we need to transfer the KActivities/KDE4
    // configuration file to the new location.
    const QString newConfigLocation
        = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
          + '/' + ACTIVITY_MANAGER_CONFIG_FILE_NAME;

    if (QFile(newConfigLocation).exists()) {
        return;
    }

    // Testing for kdehome
    Kdelibs4Migration migration;
    if (!migration.kdeHomeFound()) {
        return;
    }

    QString oldConfigFile(migration.locateLocal("config", "activitymanagerrc"));
    if (!oldConfigFile.isEmpty()) {
        QFile(oldConfigFile).copy(newConfigLocation);
    }
}

Activities::Private::Private(Activities *parent)
    : configChecker()
    , config(QStringLiteral("kactivitymanagerdrc"))
    , q(parent)
{
    // qCDebug(KAMD_ACTIVITIES) << "Using this configuration file:"
    //     << config.name()
    //     << config.locationType()
    //     << QStandardPaths::standardLocations(config.locationType())
    //     ;

    // Reading activities from the config file

    const auto runningActivities
        = mainConfig()
              .readEntry("runningActivities", QStringList())
              .toSet();

    for (const auto &activity: activityNameConfig().keyList()) {
        activities[activity] = runningActivities.contains(activity)
                                      ? Activities::Running
                                      : Activities::Stopped;
    }

    // Is this our first start?
    if (activities.isEmpty()) {
        q->AddActivity(i18n("Default"));
    }
}

void Activities::Private::loadLastActivity()
{
    // This is called from constructor, no need for locking

    // If there are no public activities, try to load the last used activity
    const auto lastUsedActivity
        = mainConfig().readEntry("currentActivity", QString());

    setCurrentActivity(
        (lastUsedActivity.isEmpty() && activities.size() > 0)
            ? activities.keys().at(0)
            : lastUsedActivity);
}

Activities::Private::~Private()
{
    configSync();
}

bool Activities::Private::setCurrentActivity(const QString &activity)
{
    {
        // There is nothing expensive in this block, not a problem to lock
        QWriteLocker lock(&activitiesLock);

        // Should we change the activity at all?
        if (currentActivity == activity) {
            return true;
        }

        // If the activity is empty, this means we are entering a limbo state
        if (activity.isEmpty()) {
            currentActivity.clear();
            emit q->CurrentActivityChanged(currentActivity);
            return true;
        }

        // Does the requested activity exist?
        if (!activities.contains(activity)) {
            return false;
        }
    }

    // Start activity
    q->StartActivity(activity);

    // Saving the current activity, and notifying
    // clients of the change
    currentActivity = activity;

    mainConfig().writeEntry("currentActivity", activity);

    scheduleConfigSync();

    emit q->CurrentActivityChanged(activity);

    return true;
}

QString Activities::Private::addActivity(const QString &name)
{
    QString activity;

    if (name.isEmpty()) {
        Q_ASSERT(!name.isEmpty());
        return activity;
    }

    int activitiesCount = 0;

    {
        QWriteLocker lock(&activitiesLock);

        // Ensuring a new Uuid. The loop should usually end after only
        // one iteration
        while (activity.isEmpty() || activities.contains(activity)) {
            activity = QUuid::createUuid().toString().mid(1, 36);
        }

        // Saves the activity info to the config

        activities[activity] = Invalid;
        activitiesCount = activities.size();
    }

    setActivityState(activity, Running);

    q->SetActivityName(activity, name);

    emit q->ActivityAdded(activity);

    scheduleConfigSync(true);

    if (activitiesCount == 1) {
        q->SetCurrentActivity(activity);
    }

    return activity;
}

void Activities::Private::removeActivity(const QString &activity)
{
    Q_ASSERT(!activity.isEmpty());

    // Sanity checks
    {
        QWriteLocker lock(&activitiesLock);

        if (!activities.contains(activity)) {
            return;
        }

        // Is somebody trying to remove the last activity?
        if (activities.size() == 1) {
            return;
        }
    }

    // If the activity is running, stash it
    q->StopActivity(activity);

    setActivityState(activity, Activities::Invalid);

    bool currentActivityDeleted = false;

    {
        QWriteLocker lock(&activitiesLock);
        // Removing the activity
        activities.remove(activity);

        // If the removed activity was the current one,
        // set another activity as current
        currentActivityDeleted = (currentActivity == activity);
    }

    activityNameConfig().deleteEntry(activity);
    activityDescriptionConfig().deleteEntry(activity);
    activityIconConfig().deleteEntry(activity);

    if (currentActivityDeleted) {
        ensureCurrentActivityIsRunning();
    }

    emit q->ActivityRemoved(activity);

    QMetaObject::invokeMethod(q, "ActivityRemoved", Qt::QueuedConnection,
                              Q_ARG(QString, activity));

    QMetaObject::invokeMethod(this, "configSync", Qt::QueuedConnection);
}

void Activities::Private::scheduleConfigSync(const bool soon)
{
    static const auto shortInterval = 1000;
    static const auto longInterval = 2 * 60 * 1000;

    // If the timer is not running, or has a longer interval than we need,
    // start it
    if ((soon && configSyncTimer.interval() > shortInterval)
            || !configSyncTimer.isActive()) {

        QMetaObject::invokeMethod(
            &configSyncTimer, "start", Qt::QueuedConnection,
            Q_ARG(int, soon ? shortInterval : longInterval));
    }
}

void Activities::Private::configSync()
{
    QMetaObject::invokeMethod(&configSyncTimer, "stop", Qt::QueuedConnection);
    config.sync();
}

void Activities::Private::setActivityState(const QString &activity,
                                           Activities::State state)
{
    bool configNeedsUpdating = false;

    {
        QWriteLocker lock(&activitiesLock);

        Q_ASSERT(activities.contains(activity));

        if (activities.value(activity) == state) {
            return;
        }

        // Treating 'Starting' as 'Running', and 'Stopping' as 'Stopped'
        // as far as the config file is concerned
        configNeedsUpdating = ((activities[activity] & 4) != (state & 4));

        activities[activity] = state;
    }

    switch (state) {
        case Activities::Running:
            emit q->ActivityStarted(activity);
            break;

        case Activities::Stopped:
            emit q->ActivityStopped(activity);
            break;

        default:
            break;
    }

    emit q->ActivityStateChanged(activity, state);

    if (configNeedsUpdating) {
        QReadLocker lock(&activitiesLock);

        mainConfig().writeEntry("runningActivities",
                                activities.keys(Activities::Running)
                                + activities.keys(Activities::Starting));
        scheduleConfigSync();
    }
}

void Activities::Private::ensureCurrentActivityIsRunning()
{
    // If the current activity is not running,
    // make some other activity current

    const auto runningActivities = q->ListActivities(Activities::Running);

    if (!runningActivities.contains(currentActivity) &&
            runningActivities.size() > 0) {
        setCurrentActivity(runningActivities.first());
    }
}

void Activities::Private::activitySessionStateChanged(const QString &activity,
                                                      int status)
{
    QString currentActivity = this->currentActivity;

    {
        QReadLocker lock(&activitiesLock);
        if (!activities.contains(activity)) {
            return;
        }
    }

    switch (status) {
        case KSMServer::Started:
        case KSMServer::FailedToStop:
            setActivityState(activity, Activities::Running);
            break;

        case KSMServer::Stopped:
            setActivityState(activity, Activities::Stopped);

            if (currentActivity == activity) {
                ensureCurrentActivityIsRunning();
            }

            break;
    }

    QMetaObject::invokeMethod(this, "configSync", Qt::QueuedConnection);
}


// Main

Activities::Activities(QObject *parent)
    : Module(QStringLiteral("activities"), parent)
    , d(this)
{
    qCDebug(KAMD_LOG_ACTIVITIES) << "Starting the KDE Activity Manager daemon"
                                 << QDateTime::currentDateTime();

    // Basic initialization ////////////////////////////////////////////////////

    // Initializing D-Bus service

    new ActivitiesAdaptor(this);
    KDBusConnectionPool::threadConnection().registerObject(
        KAMD_DBUS_OBJECT_PATH(Activities), this);

    // Initializing config

    d->connect(&d->configSyncTimer, SIGNAL(timeout()),
               SLOT(configSync()),
               Qt::QueuedConnection);

    d->configSyncTimer.setSingleShot(true);

    d->ksmserver = new KSMServer(this);
    d->connect(d->ksmserver, SIGNAL(activitySessionStateChanged(QString, int)),
               SLOT(activitySessionStateChanged(QString, int)));

    // Loading the last used activity, if possible
    d->loadLastActivity();
}

Activities::~Activities()
{
}

QString Activities::CurrentActivity() const
{
    QReadLocker lock(&d->activitiesLock);
    return d->currentActivity;
}

bool Activities::SetCurrentActivity(const QString &activity)
{
    // Public method can not put us in a limbo state
    if (activity.isEmpty()) {
        return false;
    }

    return d->setCurrentActivity(activity);
}

QString Activities::AddActivity(const QString &name)
{
    if (!KAuthorized::authorize("plasma-desktop/add_activities")) {
        return QString();
    }

    return d->addActivity(name);
}

void Activities::RemoveActivity(const QString &activity)
{
    if (!KAuthorized::authorize("plasma-desktop/add_activities")) {
        return;
    }

    d->removeActivity(activity);
}

QStringList Activities::ListActivities() const
{
    QReadLocker lock(&d->activitiesLock);
    return d->activities.keys();
}

QStringList Activities::ListActivities(int state) const
{
    QReadLocker lock(&d->activitiesLock);
    return d->activities.keys((State)state);
}

QList<ActivityInfo> Activities::ListActivitiesWithInformation() const
{
    using namespace kamd::utils;

    // Mapping activity ids to info

    return as_collection<QList<ActivityInfo>>(
        ListActivities()
            | transformed(&Activities::ActivityInformation, this)
    );
}

ActivityInfo Activities::ActivityInformation(const QString &activity) const
{
    return ActivityInfo {
        activity,
        ActivityName(activity),
        ActivityDescription(activity),
        ActivityIcon(activity),
        ActivityState(activity)
    };
}

#define CREATE_GETTER_AND_SETTER(What)                                         \
    QString Activities::Activity##What(const QString &activity) const          \
    {                                                                          \
        QReadLocker lock(&d->activitiesLock);                                  \
        return d->activities.contains(activity) ? d->activity##What(activity)  \
                                                : QString();                   \
    }                                                                          \
                                                                               \
    void Activities::SetActivity##What(const QString &activity,                \
                                       const QString &value)                   \
    {                                                                          \
        {                                                                      \
            QReadLocker lock(&d->activitiesLock);                              \
            if (value == d->activity##What(activity)                           \
                || !d->activities.contains(activity)) {                        \
                return;                                                        \
            }                                                                  \
        }                                                                      \
                                                                               \
        d->activity##What##Config().writeEntry(activity, value);               \
        d->scheduleConfigSync(true);                                           \
                                                                               \
        emit Activity##What##Changed(activity, value);                         \
        emit ActivityChanged(activity);                                        \
    }

CREATE_GETTER_AND_SETTER(Name)
CREATE_GETTER_AND_SETTER(Description)
CREATE_GETTER_AND_SETTER(Icon)

#undef CREATE_GETTE_AND_SETTERR

// Main

void Activities::StartActivity(const QString &activity)
{
    {
        QReadLocker lock(&d->activitiesLock);
        if (!d->activities.contains(activity)
                || d->activities[activity] != Stopped) {
            return;
        }
    }

    d->setActivityState(activity, Starting);
    d->ksmserver->startActivitySession(activity);
}

void Activities::StopActivity(const QString &activity)
{
    {
        QReadLocker lock(&d->activitiesLock);
        if (!d->activities.contains(activity)
                || d->activities[activity] == Stopped
                || d->activities.size() == 1
                || d->activities.keys(Activities::Running).size() <= 1
                ) {
            return;
        }
    }

    d->setActivityState(activity, Stopping);
    d->ksmserver->stopActivitySession(activity);
}

int Activities::ActivityState(const QString &activity) const
{
    QReadLocker lock(&d->activitiesLock);
    return d->activities.contains(activity) ? d->activities[activity] : Invalid;
}

