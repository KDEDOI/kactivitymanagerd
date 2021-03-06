/*
 *   Copyright (C) 2010 - 2016 by Ivan Cukic <ivan.cukic@kde.org>
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

// Self
#include <kactivities-features.h>
#include "Application.h"

// Qt
#include <QThread>
#include <QDir>
#include <QProcess>
#include <QDBusServiceWatcher>
#include <QDBusConnectionInterface>
#include <QDBusReply>

// KDE
// #include <KCrash>
// #include <KAboutData>
// #include <KCmdLineArgs>
#include <KPluginMetaData>
#include <KPluginLoader>
#include <ksharedconfig.h>
#include <kdbusconnectionpool.h>
#include <kdbusservice.h>

// Boost and utils
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <utils/d_ptr_implementation.h>

// System
#include <signal.h>
#include <stdlib.h>
#include <memory>
#include <functional>

// Local
#include "Activities.h"
#include "Resources.h"
#include "Features.h"
#include "Config.h"
#include "Plugin.h"
#include "DebugApplication.h"
#include "common/dbus/common.h"


namespace {
    QList<QThread *> s_moduleThreads;
}

// Runs a QObject inside a QThread

template <typename T>
T *runInQThread()
{
    T *object = new T();

    class Thread : public QThread {
    public:
        Thread(T *ptr = nullptr)
            : QThread()
            , object(ptr)
        {
        }

        void run() override
        {
            std::unique_ptr<T> o(object);
            exec();
        }

    private:
        T *object;

    } *thread = new Thread(object);

    s_moduleThreads << thread;

    object->moveToThread(thread);
    thread->start();

    return object;
}

class Application::Private {
public:
    Private()
    {
    }

    static inline bool isPluginEnabled(const KConfigGroup &config,
                                const KPluginMetaData& plugin)
    {
        const auto pluginName = plugin.pluginId();
        qCDebug(KAMD_LOG_APPLICATION) << "Plugin Name is " << pluginName << plugin.fileName();

        if (pluginName == "org.kde.ActivityManager.ResourceScoring") {
            // SQLite plugin is necessary for the proper workspace behaviour
            return true;
        } else {
            return config.readEntry(pluginName + "Enabled", plugin.isEnabledByDefault());
        }
    }

    bool loadPlugin(const KPluginMetaData& plugin);

    Resources *resources;
    Activities *activities;
    Features *features;

    QStringList pluginIds;
    QList<Plugin *> plugins;

    static Application *s_instance;
};

Application *Application::Private::s_instance = nullptr;

Application::Application(int &argc, char **argv)
    : QApplication(argc, argv)
{
}

void Application::init()
{
    if (!KDBusConnectionPool::threadConnection().registerService(
            KAMD_DBUS_SERVICE)) {
        QCoreApplication::exit(EXIT_SUCCESS);
    }

    // KAMD is a daemon, if it crashes it is not a problem as
    // long as it restarts properly
    // TODO: Restart on crash
    //       KCrash::setFlags(KCrash::AutoRestart);
    d->resources  = runInQThread<Resources>();
    d->activities = runInQThread<Activities>();
    d->features   = runInQThread<Features>();
    /* d->config */ new Config(this); // this does not need a separate thread

    QMetaObject::invokeMethod(this, "loadPlugins", Qt::QueuedConnection);

    QDBusConnection::sessionBus().registerObject("/ActivityManager", this,
            QDBusConnection::ExportAllSlots);
}

bool Application::Private::loadPlugin(const KPluginMetaData& plugin)
{
    if (!plugin.isValid()) {
        qCWarning(KAMD_LOG_APPLICATION) << "[ FAILED ] plugin offer not valid";
        return false;
    }

    if (pluginIds.contains(plugin.pluginId())) {
        qCDebug(KAMD_LOG_APPLICATION)   << "[   OK   ] already loaded:  " << plugin.pluginId();
        return true;
    }

    KPluginLoader loader(plugin.fileName());
    KPluginFactory* factory = loader.factory();
    if (!factory) {
        qCWarning(KAMD_LOG_APPLICATION) << "[ FAILED ] Could not load KPluginFactory for:"
                << plugin.pluginId() << loader.errorString();
        return false;
    }

    auto pluginInstance = factory->create<Plugin>();

    auto &modules = Module::get();

    if (pluginInstance) {
        bool success = pluginInstance->init(modules);

        if (success) {
            pluginIds << plugin.pluginId();
            plugins << pluginInstance;

            qCDebug(KAMD_LOG_APPLICATION)   << "[   OK   ] loaded:  " << plugin.pluginId();
            return true;
        } else {
            qCWarning(KAMD_LOG_APPLICATION) << "[ FAILED ] init: " << plugin.pluginId() << loader.errorString();
            // TODO: Show a notification for a plugin that failed to load
            delete pluginInstance;
            return false;
        }

    } else {
        qCWarning(KAMD_LOG_APPLICATION) << "[ FAILED ] loading: " << plugin.pluginId() << loader.errorString();
        // TODO: Show a notification for a plugin that failed to load
        return false;
    }
}

void Application::loadPlugins()
{
    using namespace std::placeholders;

    const auto config
        = KSharedConfig::openConfig(QStringLiteral("kactivitymanagerdrc"))
              ->group("Plugins");
    const auto offers = KPluginLoader::findPlugins(QStringLiteral(KAMD_PLUGIN_DIR),
        std::bind(Private::isPluginEnabled, config, _1));
    qCDebug(KAMD_LOG_APPLICATION) << "Found" << offers.size() << "enabled plugins:";

    for (const auto &offer : offers) {
        d->loadPlugin(offer);
    }
}

bool Application::loadPlugin(const QString &pluginId)
{
    auto offers = KPluginLoader::findPluginsById(QStringLiteral(KAMD_PLUGIN_DIR), pluginId);

    if (offers.isEmpty()) {
        qCWarning(KAMD_LOG_APPLICATION) << "[ FAILED ] not found: " << pluginId;
        return false;
    }

    return d->loadPlugin(offers.first());
}

Application::~Application()
{
    qCDebug(KAMD_LOG_APPLICATION) << "Cleaning up...";

    // Waiting for the threads to finish
    for (const auto thread : s_moduleThreads) {
        thread->quit();
        thread->wait();

        delete thread;
    }

    // Deleting plugin objects
    for (const auto plugin : d->plugins) {
        delete plugin;
    }

    Private::s_instance = nullptr;
}

int Application::newInstance()
{
    //We don't want to show the mainWindow()
    return 0;
}

Activities &Application::activities() const
{
    return *d->activities;
}

Resources &Application::resources() const
{
    return *d->resources;
}

// void Application::quit()
// {
//     if (Private::s_instance) {
//         Private::s_instance->exit();
//         delete Private::s_instance;
//     }
// }

void Application::quit()
{
    QApplication::quit();
}

#include "Version.h"
QString Application::serviceVersion() const
{
    return KACTIVITIES_VERSION_STRING;
}

// Leaving object oriented world :)

namespace  {
    template <typename Return>
    Return callOnRunningService(const QString &method)
    {
        static QDBusInterface remote(KAMD_DBUS_SERVICE, "/ActivityManager",
                                     "org.kde.ActivityManager.Application");
        QDBusReply<Return> reply = remote.call(method);

        return (Return)reply;
    }

    QString runningServiceVersion()
    {
        return callOnRunningService<QString>("serviceVersion");
    }

    bool isServiceRunning()
    {
        return QDBusConnection::sessionBus().interface()->isServiceRegistered(
                KAMD_DBUS_SERVICE);
    }
}

int main(int argc, char **argv)
{
    // Disable session management for this process
    qunsetenv("SESSION_MANAGER");

    QGuiApplication::setDesktopSettingsAware(false);

    Application application(argc, argv);
    application.setApplicationName(QStringLiteral("ActivityManager"));
    application.setOrganizationDomain(QStringLiteral("kde.org"));

    // KAboutData about("kactivitymanagerd", nullptr, ki18n("KDE Activity Manager"), "3.0",
    //         ki18n("KDE Activity Management Service"),
    //         KAboutData::License_GPL,
    //         ki18n("(c) 2010, 2011, 2012 Ivan Cukic"), KLocalizedString(),
    //         "http://www.kde.org/");

    // KCmdLineArgs::init(argc, argv, &about);

    const auto arguments = application.arguments();

    if (arguments.size() == 0) {
        QCoreApplication::exit(EXIT_FAILURE);

    } else if (arguments.size() != 1 && (arguments.size() != 2 || arguments[1] == "--help")) {

        QTextStream(stdout)
            << "start\tStarts the service\n"
            << "stop\tStops the server\n"
            << "status\tPrints basic server information\n"
            << "start-daemon\tStarts the service without forking (use with caution)\n"
            << "--help\tThis help message\n";

        QCoreApplication::exit(EXIT_SUCCESS);

    } else if (arguments.size() == 1 || arguments[1] == "start") {

        // Checking whether the service is already running
        if (isServiceRunning()) {
            QTextStream(stdout) << "Already running\n";
            QCoreApplication::exit(EXIT_SUCCESS);
        }

        // Creating the watcher, but not on the wall

        QDBusServiceWatcher watcher(KAMD_DBUS_SERVICE,
                                    QDBusConnection::sessionBus(),
                                    QDBusServiceWatcher::WatchForRegistration);

        QObject::connect(&watcher, &QDBusServiceWatcher::serviceRegistered,
            [] (const QString &service) {
                QTextStream(stdout)
                    << "Service started, version: " << runningServiceVersion()
                    << "\n";

                QCoreApplication::exit(EXIT_SUCCESS);
            });

        // Starting the dameon

        QProcess::startDetached(
                KAMD_FULL_BIN_DIR "/kactivitymanagerd",
                QStringList{"start-daemon"}
            );

        return application.exec();

    } else if (arguments[1] == "stop") {
        if (!isServiceRunning()) {
            QTextStream(stdout) << "Service not running\n";
            QCoreApplication::exit(EXIT_SUCCESS);
        }

        callOnRunningService<void>("quit");

        QTextStream(stdout) << "Service stopped\n";

        return EXIT_SUCCESS;

    } else if (arguments[1] == "status") {

        // Checking whether the service is already running
        if (isServiceRunning()) {
            QTextStream(stdout) << "The service is running, version: "
                                << runningServiceVersion() << "\n";
        } else {
            QTextStream(stdout) << "The service is not running\n";

        }

        return EXIT_SUCCESS;

    } else if (arguments[1] == "start-daemon") {
        // Really starting the activity manager

        KDBusService service(KDBusService::Unique);

        application.init();

        return application.exec();

    } else {
        QTextStream(stdout) << "Unrecognized command: " << arguments[1] << '\n';

        return EXIT_FAILURE;
    }
}

